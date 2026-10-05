#include "privilege.hpp"

#include "capture_socket.hpp"
#include "iface.hpp"
#include "text.hpp"

#include <yyjson.h>

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <utility>

namespace hotmon {
namespace {

constexpr uint32_t kMaxFrameBytes = 1024 * 1024;

struct YyDoc {
  yyjson_doc* doc = nullptr;
  ~YyDoc() {
    if (doc != nullptr) {
      yyjson_doc_free(doc);
    }
  }
  YyDoc() = default;
  YyDoc(const YyDoc&) = delete;
  YyDoc& operator=(const YyDoc&) = delete;
};

struct YyMut {
  yyjson_mut_doc* doc = nullptr;
  ~YyMut() {
    if (doc != nullptr) {
      yyjson_mut_doc_free(doc);
    }
  }
  YyMut() = default;
  YyMut(const YyMut&) = delete;
  YyMut& operator=(const YyMut&) = delete;
};

std::string mut_text(yyjson_mut_doc* doc) {
  size_t length = 0;
  char* json = yyjson_mut_write(doc, 0, &length);
  if (json == nullptr) {
    return {};
  }
  std::string text(json, length);
  std::free(json);
  return text;
}

Result<void> write_all(int fd, const char* data, size_t size) {
  size_t done = 0;
  while (done < size) {
    ssize_t count = ::send(fd, data + done, size - done, MSG_NOSIGNAL);
    if (count < 0 && errno == ENOTSOCK) {
      count = ::write(fd, data + done, size - done);
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return unexpected_text("The frame write failed. " + errno_text());
    }
    if (count == 0) {
      return unexpected_text("The frame write stopped early.");
    }
    done += static_cast<size_t>(count);
  }
  return {};
}

Result<void> read_some(int fd, char* data, size_t size, size_t& done, int* received_fd, bool& want_fd) {
  if (size == 0) {
    return {};
  }
  if (want_fd) {
    want_fd = false;
    alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))];
    std::memset(control, 0, sizeof(control));
    iovec io{};
    io.iov_base = data + done;
    io.iov_len = size - done;
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t count = ::recvmsg(fd, &message, MSG_CMSG_CLOEXEC);
    if (count < 0) {
      if (errno == EINTR) {
        want_fd = true;
        return {};
      }
      return unexpected_text("The frame read failed. " + errno_text());
    }
    if (count == 0) {
      return unexpected_text("The frame ended early.");
    }
    if ((message.msg_flags & MSG_CTRUNC) != 0) {
      return unexpected_text("The frame control data was truncated.");
    }
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
        continue;
      }
      const auto bytes = static_cast<int>(header->cmsg_len - CMSG_LEN(0));
      const int total = bytes / static_cast<int>(sizeof(int));
      const auto* fds = reinterpret_cast<const int*>(CMSG_DATA(header));
      for (int index = 0; index < total; ++index) {
        if (received_fd != nullptr && *received_fd < 0) {
          *received_fd = fds[index];
        } else {
          ::close(fds[index]);
        }
      }
    }
    done += static_cast<size_t>(count);
    return {};
  }
  const ssize_t count = ::read(fd, data + done, size - done);
  if (count < 0) {
    if (errno == EINTR) {
      return {};
    }
    return unexpected_text("The frame read failed. " + errno_text());
  }
  if (count == 0) {
    return unexpected_text("The frame ended early.");
  }
  done += static_cast<size_t>(count);
  return {};
}

Result<void> read_exact(int fd, char* data, size_t size, int* received_fd) {
  size_t done = 0;
  bool want_fd = received_fd != nullptr;
  while (done < size) {
    if (auto step = read_some(fd, data, size, done, received_fd, want_fd); !step) {
      return step;
    }
  }
  return {};
}

void close_fd(int* fd) {
  if (fd != nullptr && *fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}

std::string with_note(std::string base, const std::string& note) {
  if (note.empty()) {
    return base;
  }
  base.push_back(' ');
  base += note;
  return base;
}

Response failed_response(std::string text) {
  Response response;
  response.ok = false;
  response.error = std::move(text);
  return response;
}

int write_response(const Response& response, int pass_fd) {
  const std::string json = encode_response(response);
  if (json.empty()) {
    return 1;
  }
  if (auto wrote = write_frame(STDOUT_FILENO, json, pass_fd); !wrote) {
    return 1;
  }
  return 0;
}

std::vector<StartedProc> pids_from_files(const Paths& paths) {
  std::vector<StartedProc> started;
  if (const auto pid = read_pid_optional(paths.hostapd_pid())) {
    started.push_back(StartedProc{*pid, "hostapd"});
  }
  if (const auto pid = read_pid_optional(paths.dnsmasq_pid())) {
    started.push_back(StartedProc{*pid, "dnsmasq"});
  }
  return started;
}

[[noreturn]] void exec_helper(char** args, int sock, int err_write, int status_write) {
  if (::dup2(sock, STDIN_FILENO) < 0 || ::dup2(sock, STDOUT_FILENO) < 0 ||
      ::dup2(err_write, STDERR_FILENO) < 0) {
    const int err = errno;
    const ssize_t unused = ::write(status_write, &err, sizeof(err));
    (void)unused;
    _exit(127);
  }
  if (sock > STDERR_FILENO) {
    ::close(sock);
  }
  if (err_write > STDERR_FILENO) {
    ::close(err_write);
  }
  if (::close_range(3, ~0U, CLOSE_RANGE_CLOEXEC) != 0) {
    const long limit = ::sysconf(_SC_OPEN_MAX);
    const int last = limit > 0 ? static_cast<int>(limit) : 1024;
    for (int fd = 3; fd < last; ++fd) {
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
  }
  ::execvp(args[0], args);
  const int err = errno;
  const ssize_t unused = ::write(status_write, &err, sizeof(err));
  (void)unused;
  _exit(127);
}

std::string read_all(int fd) {
  std::string text;
  char buffer[512];
  while (true) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (count == 0) {
      break;
    }
    text.append(buffer, static_cast<size_t>(count));
  }
  return text;
}

bool add_profile(yyjson_mut_doc* doc, yyjson_mut_val* root, const Profile& profile) {
  auto text = profile_to_json(profile);
  if (!text) {
    return false;
  }
  YyDoc parsed;
  parsed.doc = yyjson_read(text->data(), text->size(), 0);
  if (parsed.doc == nullptr) {
    return false;
  }
  yyjson_mut_val* copy = yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed.doc));
  if (copy == nullptr) {
    return false;
  }
  return yyjson_mut_obj_add_val(doc, root, "profile", copy);
}

Result<void> field_type_error(yyjson_val* root, const char* key, bool (*ok)(yyjson_val*)) {
  yyjson_val* value = yyjson_obj_get(root, key);
  if (value != nullptr && !ok(value)) {
    return unexpected_text(std::string("The request ") + key + " has the wrong type.");
  }
  return {};
}

}  // namespace

std::string_view backend_token(BackendKind kind) {
  switch (kind) {
    case BackendKind::NetworkManager:
      return "network-manager";
    case BackendKind::Iwd:
      return "iwd";
    case BackendKind::ExistingHostapd:
      return "existing-hostapd";
    case BackendKind::DirectHostapd:
      return "direct-hostapd";
  }
  return "direct-hostapd";
}

Result<BackendKind> parse_backend_token(std::string_view text) {
  if (text == "network-manager") {
    return BackendKind::NetworkManager;
  }
  if (text == "iwd") {
    return BackendKind::Iwd;
  }
  if (text == "existing-hostapd") {
    return BackendKind::ExistingHostapd;
  }
  if (text == "direct-hostapd") {
    return BackendKind::DirectHostapd;
  }
  return unexpected_text("The request backend is not valid.");
}

std::string encode_request(const Request& request) {
  YyMut holder;
  holder.doc = yyjson_mut_doc_new(nullptr);
  if (holder.doc == nullptr) {
    return {};
  }
  yyjson_mut_val* root = yyjson_mut_obj(holder.doc);
  yyjson_mut_doc_set_root(holder.doc, root);
  const char* op = "apply";
  if (request.op == PrivilegeOp::Stop) {
    op = "stop";
  } else if (request.op == PrivilegeOp::Capture) {
    op = "capture";
  }
  yyjson_mut_obj_add_strcpy(holder.doc, root, "op", op);
  if (request.op == PrivilegeOp::Capture) {
    yyjson_mut_obj_add_strcpy(holder.doc, root, "interface", request.interface.c_str());
    return mut_text(holder.doc);
  }
  yyjson_mut_obj_add_strcpy(holder.doc, root, "backend", std::string(backend_token(request.backend)).c_str());
  if (request.op == PrivilegeOp::Stop) {
    yyjson_mut_obj_add_bool(holder.doc, root, "private_hostapd", request.private_hostapd);
  }
  if (!add_profile(holder.doc, root, request.profile)) {
    return {};
  }
  return mut_text(holder.doc);
}

Result<Request> decode_request(std::string_view text) {
  if (text.empty()) {
    return unexpected_text("The request is empty.");
  }
  YyDoc holder;
  holder.doc = yyjson_read(text.data(), text.size(), 0);
  if (holder.doc == nullptr) {
    return unexpected_text("The request is not JSON.");
  }
  yyjson_val* root = yyjson_doc_get_root(holder.doc);
  if (root == nullptr || !yyjson_is_obj(root)) {
    return unexpected_text("The request root is not an object.");
  }
  if (auto bad = field_type_error(root, "op", yyjson_is_str); !bad) {
    return unexpected_text(bad.error());
  }
  if (auto bad = field_type_error(root, "backend", yyjson_is_str); !bad) {
    return unexpected_text(bad.error());
  }
  if (auto bad = field_type_error(root, "profile", yyjson_is_obj); !bad) {
    return unexpected_text(bad.error());
  }
  if (auto bad = field_type_error(root, "private_hostapd", yyjson_is_bool); !bad) {
    return unexpected_text(bad.error());
  }
  if (auto bad = field_type_error(root, "interface", yyjson_is_str); !bad) {
    return unexpected_text(bad.error());
  }
  yyjson_val* op = yyjson_obj_get(root, "op");
  if (op == nullptr) {
    return unexpected_text("The request op is missing.");
  }
  const std::string_view op_text(yyjson_get_str(op));
  Request request;
  if (op_text == "apply") {
    request.op = PrivilegeOp::Apply;
  } else if (op_text == "stop") {
    request.op = PrivilegeOp::Stop;
  } else if (op_text == "capture") {
    request.op = PrivilegeOp::Capture;
  } else {
    return unexpected_text("The request op is not valid.");
  }
  if (request.op == PrivilegeOp::Capture) {
    yyjson_val* iface = yyjson_obj_get(root, "interface");
    if (iface == nullptr) {
      return unexpected_text("The request interface is missing.");
    }
    request.interface = yyjson_get_str(iface);
    if (!valid_name(request.interface)) {
      return unexpected_text("The request interface is not valid.");
    }
    return request;
  }
  yyjson_val* backend = yyjson_obj_get(root, "backend");
  if (backend == nullptr) {
    return unexpected_text("The request backend is missing.");
  }
  auto kind = parse_backend_token(yyjson_get_str(backend));
  if (!kind) {
    return unexpected_text(kind.error());
  }
  request.backend = *kind;
  yyjson_val* flag = yyjson_obj_get(root, "private_hostapd");
  if (flag != nullptr) {
    request.private_hostapd = yyjson_get_bool(flag);
  }
  yyjson_val* profile = yyjson_obj_get(root, "profile");
  if (profile == nullptr) {
    return unexpected_text("The request profile is missing.");
  }
  size_t length = 0;
  char* json = yyjson_val_write(profile, 0, &length);
  if (json == nullptr) {
    return unexpected_text("The request profile is not valid.");
  }
  auto parsed = profile_from_json(std::string_view(json, length));
  std::free(json);
  if (!parsed) {
    return unexpected_text(parsed.error());
  }
  request.profile = std::move(*parsed);
  return request;
}

std::string encode_response(const Response& response) {
  YyMut holder;
  holder.doc = yyjson_mut_doc_new(nullptr);
  if (holder.doc == nullptr) {
    return {};
  }
  yyjson_mut_val* root = yyjson_mut_obj(holder.doc);
  yyjson_mut_doc_set_root(holder.doc, root);
  yyjson_mut_obj_add_bool(holder.doc, root, "ok", response.ok);
  if (!response.ok) {
    yyjson_mut_obj_add_strcpy(holder.doc, root, "error", response.error.c_str());
    return mut_text(holder.doc);
  }
  if (response.report) {
    yyjson_mut_val* started = yyjson_mut_arr(holder.doc);
    for (const StartedProc& item : response.report->started) {
      yyjson_mut_val* entry = yyjson_mut_obj(holder.doc);
      yyjson_mut_obj_add_int(holder.doc, entry, "pid", item.pid);
      yyjson_mut_obj_add_strcpy(holder.doc, entry, "name", item.name.c_str());
      yyjson_mut_arr_add_val(started, entry);
    }
    yyjson_mut_obj_add_val(holder.doc, root, "started", started);
    yyjson_mut_obj_add_bool(holder.doc, root, "private_hostapd", response.report->private_hostapd);
  }
  return mut_text(holder.doc);
}

Result<Response> decode_response(std::string_view text) {
  if (text.empty()) {
    return unexpected_text("The response is empty.");
  }
  YyDoc holder;
  holder.doc = yyjson_read(text.data(), text.size(), 0);
  if (holder.doc == nullptr) {
    return unexpected_text("The response is not JSON.");
  }
  yyjson_val* root = yyjson_doc_get_root(holder.doc);
  if (root == nullptr || !yyjson_is_obj(root)) {
    return unexpected_text("The response root is not an object.");
  }
  yyjson_val* ok = yyjson_obj_get(root, "ok");
  if (ok == nullptr || !yyjson_is_bool(ok)) {
    return unexpected_text("The response ok field is not valid.");
  }
  Response response;
  response.ok = yyjson_get_bool(ok);
  if (!response.ok) {
    yyjson_val* error = yyjson_obj_get(root, "error");
    if (error == nullptr || !yyjson_is_str(error)) {
      return unexpected_text("The response error field is not valid.");
    }
    response.error = yyjson_get_str(error);
    return response;
  }
  yyjson_val* started = yyjson_obj_get(root, "started");
  if (started == nullptr) {
    return response;
  }
  if (!yyjson_is_arr(started)) {
    return unexpected_text("The response started field has the wrong type.");
  }
  StartReport report;
  yyjson_val* flag = yyjson_obj_get(root, "private_hostapd");
  if (flag != nullptr) {
    if (!yyjson_is_bool(flag)) {
      return unexpected_text("The response private_hostapd field has the wrong type.");
    }
    report.private_hostapd = yyjson_get_bool(flag);
  }
  size_t index = 0;
  size_t max = 0;
  yyjson_val* item = nullptr;
  yyjson_arr_foreach(started, index, max, item) {
    if (!yyjson_is_obj(item)) {
      return unexpected_text("The response started entry has the wrong type.");
    }
    yyjson_val* pid = yyjson_obj_get(item, "pid");
    yyjson_val* name = yyjson_obj_get(item, "name");
    if (pid == nullptr || (!yyjson_is_int(pid) && !yyjson_is_uint(pid))) {
      return unexpected_text("The response pid field has the wrong type.");
    }
    if (name == nullptr || !yyjson_is_str(name)) {
      return unexpected_text("The response name field has the wrong type.");
    }
    const int64_t value = yyjson_is_uint(pid) ? static_cast<int64_t>(yyjson_get_uint(pid))
                                              : yyjson_get_sint(pid);
    if (value <= 0 || value > INT_MAX) {
      return unexpected_text("The response pid field is not valid.");
    }
    report.started.push_back(StartedProc{static_cast<int>(value), yyjson_get_str(name)});
  }
  response.report = std::move(report);
  return response;
}

Result<void> write_frame(int fd, std::string_view payload, int pass_fd) {
  if (payload.size() > kMaxFrameBytes) {
    return unexpected_text("The frame is too large.");
  }
  const uint32_t length = htonl(static_cast<uint32_t>(payload.size()));
  std::string bytes(sizeof(length) + payload.size(), '\0');
  std::memcpy(bytes.data(), &length, sizeof(length));
  if (!payload.empty()) {
    std::memcpy(bytes.data() + sizeof(length), payload.data(), payload.size());
  }
  if (pass_fd < 0) {
    return write_all(fd, bytes.data(), bytes.size());
  }
  alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))];
  std::memset(control, 0, sizeof(control));
  iovec io{};
  io.iov_base = bytes.data();
  io.iov_len = bytes.size();
  msghdr message{};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(header), &pass_fd, sizeof(pass_fd));
  size_t done = 0;
  bool sent_fd = false;
  while (done < bytes.size()) {
    ssize_t count = 0;
    if (!sent_fd) {
      io.iov_base = bytes.data() + done;
      io.iov_len = bytes.size() - done;
      count = ::sendmsg(fd, &message, MSG_NOSIGNAL);
      if (count > 0) {
        sent_fd = true;
        message.msg_control = nullptr;
        message.msg_controllen = 0;
      }
    } else {
      count = ::send(fd, bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return unexpected_text("The frame write failed. " + errno_text());
    }
    if (count == 0) {
      return unexpected_text("The frame write stopped early.");
    }
    done += static_cast<size_t>(count);
  }
  return {};
}

Result<std::string> read_frame(int fd, int* received_fd) {
  if (received_fd != nullptr) {
    *received_fd = -1;
  }
  char header[4];
  if (auto got = read_exact(fd, header, sizeof(header), received_fd); !got) {
    close_fd(received_fd);
    return unexpected_text(got.error());
  }
  uint32_t length = 0;
  std::memcpy(&length, header, sizeof(length));
  length = ntohl(length);
  if (length > kMaxFrameBytes) {
    close_fd(received_fd);
    return unexpected_text("The frame is too large.");
  }
  std::string payload(length, '\0');
  if (length > 0) {
    if (auto got = read_exact(fd, payload.data(), length, nullptr); !got) {
      close_fd(received_fd);
      return unexpected_text(got.error());
    }
  }
  return payload;
}

DirectPrivilege::DirectPrivilege(Runner& runner, ProcessControl& signals, Paths paths)
    : runner_(runner), signals_(signals), paths_(std::move(paths)) {}

Result<StartReport> DirectPrivilege::apply(BackendKind backend, const Profile& profile) {
  auto plan = plan_apply(backend, profile, paths_);
  if (!plan) {
    return unexpected_text(plan.error());
  }
  return execute_plan(*plan, runner_, signals_, paths_);
}

Result<void> DirectPrivilege::stop(const StopRequest& request) {
  return teardown_hotspot(request.backend, request.profile, request.private_hostapd, request.started,
                          runner_, signals_, paths_);
}

Result<FileDescriptor> DirectPrivilege::open_capture_socket(std::string_view interface) {
  return LocalCapture::open_fd(interface);
}

HelperPrivilege::HelperPrivilege(std::vector<std::string> helper_argv, TerminalHooks hooks)
    : helper_argv_(std::move(helper_argv)), hooks_(std::move(hooks)) {}

Result<HelperPrivilege::Exchange> HelperPrivilege::exchange(const Request& request) {
  if (hooks_.suspend) {
    hooks_.suspend();
  }
  struct ResumeGuard {
    std::function<void()>* resume = nullptr;
    ~ResumeGuard() {
      if (resume != nullptr && *resume) {
        (*resume)();
      }
    }
  } guard{&hooks_.resume};

  const std::string missing =
      "hotmon needs pkexec to ask for authorization. Install polkit, or run hotmon as root.";
  if (helper_argv_.empty() || helper_argv_[0].empty()) {
    return unexpected_text(missing);
  }
  const std::string json = encode_request(request);
  if (json.empty()) {
    return unexpected_text("The request cannot be encoded.");
  }

  int sockets[2] = {-1, -1};
  int errors[2] = {-1, -1};
  int status_pipe[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0 || ::pipe(errors) != 0 ||
      ::pipe(status_pipe) != 0) {
    const std::string message = errno_text();
    for (int fd : {sockets[0], sockets[1], errors[0], errors[1], status_pipe[0], status_pipe[1]}) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
    return unexpected_text("The privileged helper did not start. " + message);
  }
  FileDescriptor parent_sock(sockets[0]);
  FileDescriptor child_sock(sockets[1]);
  FileDescriptor err_read(errors[0]);
  FileDescriptor err_write(errors[1]);
  FileDescriptor status_read(status_pipe[0]);
  FileDescriptor status_write(status_pipe[1]);
  ::fcntl(status_write.get(), F_SETFD, FD_CLOEXEC);

  std::vector<char*> args;
  args.reserve(helper_argv_.size() + 1);
  for (const std::string& arg : helper_argv_) {
    args.push_back(const_cast<char*>(arg.c_str()));
  }
  args.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) {
    return unexpected_text("The privileged helper did not start. " + errno_text());
  }
  if (child == 0) {
    exec_helper(args.data(), child_sock.get(), err_write.get(), status_write.get());
  }
  child_sock = FileDescriptor();
  err_write = FileDescriptor();
  status_write = FileDescriptor();

  int exec_errno = 0;
  ssize_t status_count = 0;
  while (true) {
    status_count = ::read(status_read.get(), &exec_errno, sizeof(exec_errno));
    if (status_count < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  (void)exec_errno;
  const bool exec_failed = status_count > 0;
  bool got_frame = false;
  Response response;
  int passed = -1;
  if (!exec_failed) {
    if (auto wrote = write_frame(parent_sock.get(), json); wrote) {
      ::shutdown(parent_sock.get(), SHUT_WR);
      auto frame = read_frame(parent_sock.get(), &passed);
      if (frame) {
        auto decoded = decode_response(*frame);
        if (decoded) {
          got_frame = true;
          response = std::move(*decoded);
        }
      }
    }
  }
  int wait_status = 0;
  while (::waitpid(child, &wait_status, 0) < 0) {
    if (errno != EINTR) {
      break;
    }
  }
  const std::string note = trim_copy(read_all(err_read.get()));
  if (exec_failed) {
    close_fd(&passed);
    return unexpected_text(missing);
  }
  if (got_frame) {
    if (!response.ok) {
      close_fd(&passed);
      return unexpected_text(response.error);
    }
    Exchange exchange;
    exchange.response = std::move(response);
    if (passed >= 0) {
      exchange.passed = FileDescriptor(passed);
    }
    return exchange;
  }
  close_fd(&passed);
  if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 126) {
    return unexpected_text("Authorization was cancelled. The action was not done.");
  }
  if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 127) {
    return unexpected_text(with_note(
        "Authorization failed. Check the password or the fingerprint and try again.", note));
  }
  return unexpected_text(with_note("The privileged helper stopped without a result.", note));
}

Result<StartReport> HelperPrivilege::apply(BackendKind backend, const Profile& profile) {
  Request request;
  request.op = PrivilegeOp::Apply;
  request.backend = backend;
  request.profile = profile;
  auto result = exchange(request);
  if (!result) {
    return unexpected_text(result.error());
  }
  if (result->response.report) {
    return *result->response.report;
  }
  return StartReport{};
}

Result<void> HelperPrivilege::stop(const StopRequest& request) {
  Request encoded;
  encoded.op = PrivilegeOp::Stop;
  encoded.backend = request.backend;
  encoded.profile = request.profile;
  encoded.private_hostapd = request.private_hostapd;
  auto result = exchange(encoded);
  if (!result) {
    return unexpected_text(result.error());
  }
  return {};
}

Result<FileDescriptor> HelperPrivilege::open_capture_socket(std::string_view interface) {
  Request request;
  request.op = PrivilegeOp::Capture;
  request.interface = std::string(interface);
  auto result = exchange(request);
  if (!result) {
    return unexpected_text(result.error());
  }
  if (!result->passed) {
    return unexpected_text("The privileged helper stopped without a result.");
  }
  return std::move(result->passed);
}

std::vector<std::string> default_helper_argv() {
  // Do not search PATH for pkexec.
  // A program with that name in a user directory can show a false prompt and keep the password.
  std::string pkexec = "/usr/bin/pkexec";
  for (const char* candidate : {"/usr/bin/pkexec", "/bin/pkexec", "/run/wrappers/bin/pkexec"}) {
    if (::access(candidate, X_OK) == 0) {
      pkexec = candidate;
      break;
    }
  }
  std::error_code error;
  const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
  if (error) {
    return {pkexec, "/proc/self/exe", "--privileged-helper"};
  }
  return {pkexec, path.string(), "--privileged-helper"};
}

std::unique_ptr<Privileged> make_privileged(uid_t euid, Runner& runner, ProcessControl& signals,
                                            const Paths& paths, std::vector<std::string> helper_argv,
                                            TerminalHooks hooks) {
  if (euid == 0) {
    return std::make_unique<DirectPrivilege>(runner, signals, paths);
  }
  return std::make_unique<HelperPrivilege>(std::move(helper_argv), std::move(hooks));
}

int run_privileged_helper(Privileged& worker) { return run_privileged_helper(worker, Paths::system()); }

int run_privileged_helper(Privileged& worker, const Paths& paths) {
  int extra = -1;
  auto frame = read_frame(STDIN_FILENO, &extra);
  close_fd(&extra);
  if (!frame) {
    (void)write_response(failed_response(frame.error()), -1);
    return 1;
  }
  auto request = decode_request(*frame);
  if (!request) {
    (void)write_response(failed_response(request.error()), -1);
    return 1;
  }
  if (request->op == PrivilegeOp::Apply || request->op == PrivilegeOp::Stop) {
    if (auto checked = request->profile.check_settings(); !checked) {
      (void)write_response(failed_response(checked.error()), -1);
      return 0;
    }
  }
  if (request->op == PrivilegeOp::Apply) {
    auto report = worker.apply(request->backend, request->profile);
    if (!report) {
      (void)write_response(failed_response(report.error()), -1);
      return 0;
    }
    Response response;
    response.ok = true;
    response.report = std::move(*report);
    return write_response(response, -1);
  }
  if (request->op == PrivilegeOp::Stop) {
    StopRequest stop;
    stop.backend = request->backend;
    stop.profile = request->profile;
    stop.private_hostapd = request->private_hostapd;
    stop.started = pids_from_files(paths);
    auto stopped = worker.stop(stop);
    if (!stopped) {
      (void)write_response(failed_response(stopped.error()), -1);
      return 0;
    }
    Response response;
    response.ok = true;
    return write_response(response, -1);
  }
  if (!valid_name(request->interface)) {
    (void)write_response(failed_response("The capture interface name is not valid."), -1);
    return 0;
  }
  auto socket_fd = worker.open_capture_socket(request->interface);
  if (!socket_fd) {
    (void)write_response(failed_response(socket_fd.error()), -1);
    return 0;
  }
  Response response;
  response.ok = true;
  const int code = write_response(response, socket_fd->get());
  return code;
}

}
