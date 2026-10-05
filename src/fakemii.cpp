#include "fakemii.hpp"

#include "profile.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
#include <cerrno>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace hotmon {
namespace {

// This is a byte copy of conntest.html from the FakeMii project.
// Some lines have trailing spaces and tabs. Keep them.
constexpr std::string_view CONNTEST_PAGE = R"page(<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">
<html>
    
<head>
		<title>HTML Page</title>
</head>

<body bgcolor="#FFFFFF">
This is test.html page
</body>
    
</html>
)page";

constexpr std::string_view CONNTEST_HOST = "conntest.nintendowifi.net";

constexpr std::string_view NOT_FOUND =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 14\r\n"
    "Connection: close\r\n"
    "\r\n"
    "404 Not Found\n";

constexpr std::string_view INVALID_TARGET = "(invalid request)";

constexpr size_t READ_CHUNK = 4096;

bool equal_ignore_case(std::string_view left, std::string_view right) {
  return left.size() == right.size() && ascii_lower(std::string(left)) == ascii_lower(std::string(right));
}

bool starts_with_ignore_case(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && equal_ignore_case(text.substr(0, prefix.size()), prefix);
}

std::string_view first_line(std::string_view request) {
  std::string_view line = request.substr(0, request.find('\n'));
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  return line;
}

struct RequestLine {
  std::string_view method;
  std::string_view target;
  std::string_view version;
};

bool split_request_line(std::string_view line, RequestLine& out) {
  const size_t first = line.find(' ');
  if (first == std::string_view::npos) {
    return false;
  }
  const size_t second = line.find(' ', first + 1);
  if (second == std::string_view::npos || line.find(' ', second + 1) != std::string_view::npos) {
    return false;
  }
  out.method = line.substr(0, first);
  out.target = line.substr(first + 1, second - first - 1);
  out.version = line.substr(second + 1);
  return !out.method.empty() && !out.target.empty() && !out.version.empty();
}

std::string_view trim_blank(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

std::string_view host_header(std::string_view request) {
  size_t start = request.find('\n');
  while (start != std::string_view::npos && start + 1 < request.size()) {
    ++start;
    const size_t end = request.find('\n', start);
    const std::string_view line =
        request.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (line.empty() || line == "\r") {
      break;
    }
    const size_t colon = line.find(':');
    if (colon != std::string_view::npos && equal_ignore_case(line.substr(0, colon), "host")) {
      return trim_blank(line.substr(colon + 1));
    }
    start = end;
  }
  return {};
}

std::string cut_target(std::string target) {
  if (target.size() > FAKEMII_TARGET_MAX) {
    target.resize(FAKEMII_TARGET_MAX);
  }
  return target;
}

FakeMiiReply not_found(std::string target) {
  return FakeMiiReply{std::string(NOT_FOUND), false, cut_target(std::move(target))};
}

std::string conntest_bytes() {
  std::string bytes = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: " +
                      std::to_string(CONNTEST_PAGE.size()) +
                      "\r\n"
                      "Connection: close\r\n"
                      "Server: BigIP\r\n"
                      "X-Organization: Nintendo\r\n"
                      "\r\n";
  bytes += CONNTEST_PAGE;
  return bytes;
}

bool would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK; }

}

std::string_view fakemii_page() { return CONNTEST_PAGE; }

bool fakemii_request_complete(std::string_view bytes) {
  return bytes.find("\r\n\r\n") != std::string_view::npos;
}

FakeMiiReply fakemii_respond(std::string_view request) {
  RequestLine line;
  if (!split_request_line(first_line(request), line)) {
    return not_found(std::string(INVALID_TARGET));
  }
  constexpr std::string_view scheme = "http://";
  if (!starts_with_ignore_case(line.target, scheme)) {
    // Origin form or another form. FakeMii serves only the absolute-URI form.
    return not_found(std::string(host_header(request)) + std::string(line.target));
  }
  const std::string_view rest = line.target.substr(scheme.size());
  const size_t slash = rest.find('/');
  const std::string_view host = rest.substr(0, slash);
  const std::string_view path = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
  std::string target = std::string(host) + std::string(path);
  const bool host_ok = equal_ignore_case(host, CONNTEST_HOST) ||
                       equal_ignore_case(host, std::string(CONNTEST_HOST) + ":80");
  if (request.size() > FAKEMII_MAX_REQUEST || line.method != "GET" ||
      !line.version.starts_with("HTTP/1.") || !host_ok || path != "/") {
    return not_found(std::move(target));
  }
  return FakeMiiReply{conntest_bytes(), true, cut_target(std::move(target))};
}

FakeMii::~FakeMii() { stop(); }

Result<void> FakeMii::start(std::string_view bind_ip, uint16_t port) {
  stop();
  auto ip = parse_ipv4(bind_ip);
  if (!ip) {
    return unexpected_text(ip.error());
  }
  if (*ip == 0) {
    return unexpected_text("FakeMii does not listen on 0.0.0.0. It needs the hotspot gateway address.");
  }
  const std::string ip_text = format_ipv4(*ip);
  FileDescriptor fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!fd) {
    return unexpected_text("FakeMii could not open a socket. " + errno_text() + ".");
  }
  const int yes = 1;
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
    return unexpected_text("FakeMii could not open a socket. " + errno_text() + ".");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(*ip);
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(fd.get(), 8) != 0) {
    const int err = errno;
    const std::string port_text = std::to_string(port);
    if (err == EADDRINUSE) {
      return unexpected_text("Port " + port_text + " is already in use on " + ip_text + ".");
    }
    if (err == EADDRNOTAVAIL) {
      return unexpected_text("The address " + ip_text + " is not available on this computer.");
    }
    return unexpected_text("FakeMii could not listen on " + ip_text + ":" + port_text + ". " +
                           errno_text(err) + ".");
  }
  sockaddr_in bound{};
  socklen_t size = sizeof(bound);
  if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&bound), &size) != 0) {
    return unexpected_text("FakeMii could not read its port. " + errno_text() + ".");
  }
  listener_ = std::move(fd);
  address_ = format_ipv4(ntohl(bound.sin_addr.s_addr));
  port_ = ntohs(bound.sin_port);
  return {};
}

void FakeMii::stop() {
  clients_.clear();
  listener_ = FileDescriptor();
  address_.clear();
  port_ = 0;
  served_ = 0;
  last_target_.clear();
  conntest_served_ = false;
}

bool FakeMii::running() const { return static_cast<bool>(listener_); }

uint16_t FakeMii::port() const { return port_; }

const std::string& FakeMii::address() const { return address_; }

size_t FakeMii::served() const { return served_; }

const std::string& FakeMii::last_target() const { return last_target_; }

bool FakeMii::conntest_served() const { return conntest_served_; }

void FakeMii::poll(std::chrono::steady_clock::time_point now) {
  if (!running()) {
    return;
  }
  accept_clients(now);
  for (Client& client : clients_) {
    if (!client.replied) {
      read_client(client);
    }
    if (!client.closed && client.replied) {
      write_client(client);
    }
    if (!client.closed && now - client.accepted_at > FAKEMII_TIMEOUT) {
      client.closed = true;
    }
  }
  std::erase_if(clients_, [](const Client& client) { return client.closed; });
}

void FakeMii::accept_clients(std::chrono::steady_clock::time_point now) {
  while (true) {
    FileDescriptor fd(::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
    if (!fd) {
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      return;
    }
    if (clients_.size() >= FAKEMII_MAX_CONNECTIONS) {
      // The local FileDescriptor closes this socket at the end of the iteration.
      continue;
    }
    clients_.push_back(Client{std::move(fd), now, {}, {}, 0, false, false});
  }
}

void FakeMii::read_client(Client& client) {
  std::array<char, READ_CHUNK> buffer{};
  while (client.request.size() <= FAKEMII_MAX_REQUEST) {
    const ssize_t count = ::recv(client.fd.get(), buffer.data(), buffer.size(), 0);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (!would_block(errno)) {
        client.closed = true;
        return;
      }
      break;
    }
    if (count == 0) {
      // The peer closed before a full request. No reply is counted.
      client.closed = true;
      return;
    }
    client.request.append(buffer.data(), static_cast<size_t>(count));
    if (fakemii_request_complete(client.request)) {
      break;
    }
  }
  if (!fakemii_request_complete(client.request) && client.request.size() <= FAKEMII_MAX_REQUEST) {
    return;
  }
  FakeMiiReply reply = fakemii_respond(client.request);
  client.reply = std::move(reply.bytes);
  client.replied = true;
  client.request.clear();
  ++served_;
  last_target_ = std::move(reply.target);
  conntest_served_ = conntest_served_ || reply.conntest;
}

void FakeMii::write_client(Client& client) {
  while (client.sent < client.reply.size()) {
    const ssize_t count = ::send(client.fd.get(), client.reply.data() + client.sent,
                                 client.reply.size() - client.sent, MSG_NOSIGNAL);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (!would_block(errno)) {
        client.closed = true;
      }
      return;
    }
    client.sent += static_cast<size_t>(count);
  }
  // Drain unread bytes after shutdown. Then close() does not send a reset that drops the reply.
  ::shutdown(client.fd.get(), SHUT_WR);
  std::array<char, READ_CHUNK> drop{};
  for (int round = 0; round < 16; ++round) {
    if (::recv(client.fd.get(), drop.data(), drop.size(), 0) <= 0) {
      break;
    }
  }
  client.closed = true;
}

}
