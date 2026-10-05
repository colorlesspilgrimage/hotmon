#include "capture_socket.hpp"

#include "iface.hpp"
#include "text.hpp"

#include <cerrno>

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hotmon {

LocalCapture::LocalCapture(int fd) : fd_(fd) {}

LocalCapture::~LocalCapture() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

LocalCapture::LocalCapture(LocalCapture&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

LocalCapture& LocalCapture::operator=(LocalCapture&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

Result<FileDescriptor> LocalCapture::open_fd(std::string_view interface) {
  if (!valid_name(interface)) {
    return unexpected_text("The capture interface name is not valid.");
  }
  const int raw = ::socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
  if (raw < 0) {
    return unexpected_text(errno_text());
  }
  FileDescriptor fd(raw);
  const std::string name(interface);
  const unsigned int index = if_nametoindex(name.c_str());
  if (index == 0) {
    return unexpected_text(errno_text());
  }
  sockaddr_ll address{};
  address.sll_family = AF_PACKET;
  address.sll_protocol = htons(ETH_P_ALL);
  address.sll_ifindex = static_cast<int>(index);
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    return unexpected_text(errno_text());
  }
  return fd;
}

LocalCapture LocalCapture::from_fd(FileDescriptor fd) { return LocalCapture(fd.release()); }

Result<std::optional<std::vector<uint8_t>>> LocalCapture::try_recv() {
  const ssize_t size = ::recv(fd_, buffer_.data(), buffer_.size(), MSG_DONTWAIT);
  if (size < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return std::optional<std::vector<uint8_t>>();
    }
    return unexpected_text(errno_text());
  }
  return std::optional<std::vector<uint8_t>>(
      std::vector<uint8_t>(buffer_.begin(), buffer_.begin() + size));
}

}
