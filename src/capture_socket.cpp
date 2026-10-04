#include "capture_socket.hpp"

#include "iface.hpp"

#include <cerrno>
#include <cstring>

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

Result<LocalCapture> LocalCapture::open(std::string_view interface) {
  if (!valid_name(interface)) {
    return unexpected_text("The capture interface name is not valid.");
  }
  const int fd = ::socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
  if (fd < 0) {
    return unexpected_text(std::strerror(errno));
  }
  const std::string name(interface);
  const unsigned int index = if_nametoindex(name.c_str());
  if (index == 0) {
    const std::string message = std::strerror(errno);
    ::close(fd);
    return unexpected_text(message);
  }
  sockaddr_ll address{};
  address.sll_family = AF_PACKET;
  address.sll_protocol = htons(ETH_P_ALL);
  address.sll_ifindex = static_cast<int>(index);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string message = std::strerror(errno);
    ::close(fd);
    return unexpected_text(message);
  }
  return LocalCapture(fd);
}

Result<std::optional<std::vector<uint8_t>>> LocalCapture::try_recv() {
  const ssize_t size = ::recv(fd_, buffer_.data(), buffer_.size(), MSG_DONTWAIT);
  if (size < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return std::optional<std::vector<uint8_t>>();
    }
    return unexpected_text(std::strerror(errno));
  }
  return std::optional<std::vector<uint8_t>>(
      std::vector<uint8_t>(buffer_.begin(), buffer_.begin() + size));
}

}  // namespace hotmon
