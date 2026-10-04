#include "process.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <utility>

namespace hotmon {
namespace {

std::string errno_text(int err) { return std::strerror(err); }

}

FileDescriptor::FileDescriptor(int fd) : fd_(fd) {}

FileDescriptor::~FileDescriptor() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
  other.fd_ = -1;
}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

int FileDescriptor::get() const { return fd_; }

int FileDescriptor::release() {
  const int fd = fd_;
  fd_ = -1;
  return fd;
}

FileDescriptor::operator bool() const { return fd_ >= 0; }

Result<CommandOutput> run_capture(const std::vector<std::string>& argv) {
  if (argv.empty() || argv[0].empty()) {
    return unexpected_text("The program did not start.");
  }

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  int status_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0 || ::pipe(status_pipe) != 0) {
    return unexpected_text(std::string("The program ") + argv[0] + " did not start. " +
                           errno_text(errno));
  }
  FileDescriptor out_read(out_pipe[0]);
  FileDescriptor out_write(out_pipe[1]);
  FileDescriptor err_read(err_pipe[0]);
  FileDescriptor err_write(err_pipe[1]);
  FileDescriptor status_read(status_pipe[0]);
  FileDescriptor status_write(status_pipe[1]);
  ::fcntl(status_write.get(), F_SETFD, FD_CLOEXEC);

  const pid_t child = ::fork();
  if (child < 0) {
    return unexpected_text(std::string("The program ") + argv[0] + " did not start. " +
                           errno_text(errno));
  }
  if (child == 0) {
    ::dup2(out_write.get(), STDOUT_FILENO);
    ::dup2(err_write.get(), STDERR_FILENO);
    out_read = FileDescriptor();
    out_write = FileDescriptor();
    err_read = FileDescriptor();
    err_write = FileDescriptor();
    status_read = FileDescriptor();
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
      args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    ::execvp(args[0], args.data());
    const int err = errno;
    const ssize_t unused = ::write(status_write.get(), &err, sizeof(err));
    (void)unused;
    _exit(127);
  }

  out_write = FileDescriptor();
  err_write = FileDescriptor();
  status_write = FileDescriptor();

  int exec_errno = 0;
  const ssize_t status_count = ::read(status_read.get(), &exec_errno, sizeof(exec_errno));
  if (status_count == static_cast<ssize_t>(sizeof(exec_errno))) {
    int wait_status = 0;
    ::waitpid(child, &wait_status, 0);
    return unexpected_text(std::string("The program ") + argv[0] + " did not start. " +
                           errno_text(exec_errno));
  }

  CommandOutput output;
  pollfd polls[2] = {
      {out_read.get(), POLLIN, 0},
      {err_read.get(), POLLIN, 0},
  };
  bool out_open = true;
  bool err_open = true;
  while (out_open || err_open) {
    if (::poll(polls, 2, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (out_open && (polls[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      std::array<char, 4096> buffer{};
      const ssize_t count = ::read(out_read.get(), buffer.data(), buffer.size());
      if (count > 0) {
        output.out.append(buffer.data(), static_cast<size_t>(count));
      } else if (count == 0 || errno != EINTR) {
        out_open = false;
        polls[0].fd = -1;
      }
    }
    if (err_open && (polls[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      std::array<char, 4096> buffer{};
      const ssize_t count = ::read(err_read.get(), buffer.data(), buffer.size());
      if (count > 0) {
        output.err.append(buffer.data(), static_cast<size_t>(count));
      } else if (count == 0 || errno != EINTR) {
        err_open = false;
        polls[1].fd = -1;
      }
    }
  }

  int wait_status = 0;
  while (::waitpid(child, &wait_status, 0) < 0) {
    if (errno != EINTR) {
      return unexpected_text(std::string("The program ") + argv[0] + " did not start. " +
                             errno_text(errno));
    }
  }
  if (WIFEXITED(wait_status)) {
    output.status = WEXITSTATUS(wait_status);
  } else if (WIFSIGNALED(wait_status)) {
    output.status = 128 + WTERMSIG(wait_status);
  } else {
    output.status = 1;
  }
  return output;
}

}
