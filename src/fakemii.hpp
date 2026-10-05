#pragma once

#include "process.hpp"
#include "result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hotmon {

inline constexpr uint16_t FAKEMII_PORT = 3000;
inline constexpr size_t FAKEMII_MAX_REQUEST = 8192;
inline constexpr size_t FAKEMII_MAX_CONNECTIONS = 8;
inline constexpr std::chrono::seconds FAKEMII_TIMEOUT{5};  // per connection, from accept
inline constexpr size_t FAKEMII_TARGET_MAX = 80;         // stored bytes of host/path

// Join an IPv4 address and a port. Example: 192.168.42.1:3000.
inline std::string endpoint_text(std::string_view ip, uint16_t port) {
  return std::string(ip) + ":" + std::to_string(port);
}

struct FakeMiiReply {
  std::string bytes;      // full HTTP response, ready to send
  bool conntest = false;
  std::string target;     // "host/path" for display, raw (not sanitized)
};

// The binary contains this page. The program reads no file for it.
std::string_view fakemii_page();
// True when the request has the blank line that ends the headers.
bool fakemii_request_complete(std::string_view bytes);
// It accepts any bytes and makes no network call.
FakeMiiReply fakemii_respond(std::string_view request);

// A small HTTP server that fakes the Nintendo 3DS connection test.
// It never forwards a request. The UI tick calls poll(). It uses no thread.
class FakeMii {
 public:
  FakeMii() = default;
  ~FakeMii();
  FakeMii(const FakeMii&) = delete;
  FakeMii& operator=(const FakeMii&) = delete;
  FakeMii(FakeMii&& other) noexcept = default;
  FakeMii& operator=(FakeMii&& other) noexcept = default;

  // Port 0 gives a free port. Tests use it.
  Result<void> start(std::string_view bind_ip, uint16_t port);
  void stop();
  bool running() const;
  uint16_t port() const;
  const std::string& address() const;
  std::string endpoint() const { return endpoint_text(address_, port_); }
  void poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  size_t served() const;
  const std::string& last_target() const;
  bool conntest_served() const;

 private:
  struct Client {
    FileDescriptor fd;
    std::chrono::steady_clock::time_point accepted_at;
    std::string request;
    std::string reply;
    size_t sent = 0;
    bool replied = false;
    bool closed = false;
  };

  void accept_clients(std::chrono::steady_clock::time_point now);
  void read_client(Client& client);
  void write_client(Client& client);

  FileDescriptor listener_;
  std::vector<Client> clients_;
  std::string address_;
  uint16_t port_ = 0;
  size_t served_ = 0;
  std::string last_target_;
  bool conntest_served_ = false;
};

}
