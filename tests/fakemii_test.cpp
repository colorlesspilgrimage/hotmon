#include "fakemii.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace hotmon;

const std::string CONNTEST_REQUEST =
    "GET http://conntest.nintendowifi.net/ HTTP/1.1\r\nHost: conntest.nintendowifi.net\r\n\r\n";

const std::string NOT_FOUND_BYTES =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 14\r\n"
    "Connection: close\r\n"
    "\r\n"
    "404 Not Found\n";

std::string body_of(const std::string& reply) {
  const size_t end = reply.find("\r\n\r\n");
  return end == std::string::npos ? std::string() : reply.substr(end + 4);
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return text;
}

// Wait for EOF on the client. Return false if data or nothing arrives.
bool gets_eof(int fd, int timeout_ms = 2000) {
  pollfd entry{fd, POLLIN, 0};
  if (::poll(&entry, 1, timeout_ms) <= 0) {
    return false;
  }
  char byte = 0;
  return ::recv(fd, &byte, 1, MSG_DONTWAIT) == 0;
}

// ---- Handler tests. They use no socket. ----

TEST(FakeMii, ConntestAbsoluteUriGetsThePage) {
  const auto reply = fakemii_respond(CONNTEST_REQUEST);
  EXPECT_TRUE(reply.conntest);
  EXPECT_TRUE(reply.bytes.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_NE(reply.bytes.find("Server: BigIP\r\n"), std::string::npos);
  EXPECT_NE(reply.bytes.find("X-Organization: Nintendo\r\n"), std::string::npos);
  EXPECT_NE(reply.bytes.find("Connection: close\r\n"), std::string::npos);
  const std::string body = body_of(reply.bytes);
  EXPECT_EQ(body, std::string(fakemii_page()));
  EXPECT_NE(reply.bytes.find("Content-Length: " + std::to_string(body.size()) + "\r\n"),
            std::string::npos);
  EXPECT_EQ(reply.target, "conntest.nintendowifi.net/");
}

TEST(FakeMii, ConntestHeadersAreExact) {
  const auto reply = fakemii_respond(CONNTEST_REQUEST);
  const std::string expected = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/html\r\n"
                               "Content-Length: " +
                               std::to_string(fakemii_page().size()) +
                               "\r\n"
                               "Connection: close\r\n"
                               "Server: BigIP\r\n"
                               "X-Organization: Nintendo\r\n"
                               "\r\n";
  EXPECT_EQ(reply.bytes.substr(0, reply.bytes.find("\r\n\r\n") + 4), expected);
  EXPECT_EQ(reply.bytes, expected + std::string(fakemii_page()));
}

TEST(FakeMii, ConntestHostIsCaseInsensitive) {
  EXPECT_TRUE(fakemii_respond("GET http://CONNTEST.NintendoWiFi.net/ HTTP/1.0\r\n\r\n").conntest);
  EXPECT_TRUE(fakemii_respond("GET http://conntest.nintendowifi.net:80/ HTTP/1.1\r\n\r\n").conntest);
}

TEST(FakeMii, OriginFormGetsNotFound) {
  const auto reply = fakemii_respond("GET / HTTP/1.1\r\nHost: conntest.nintendowifi.net\r\n\r\n");
  EXPECT_EQ(reply.bytes, NOT_FOUND_BYTES);
  EXPECT_FALSE(reply.conntest);
  EXPECT_EQ(reply.bytes.find("BigIP"), std::string::npos);
  EXPECT_EQ(reply.bytes.find("Nintendo"), std::string::npos);
  EXPECT_EQ(reply.target, "conntest.nintendowifi.net/");
}

TEST(FakeMii, OtherPathsGetNotFound) {
  const char* targets[] = {
      "http://conntest.nintendowifi.net/launcher", "http://conntest.nintendowifi.net/x",
      "http://example.com/",                       "http://conntest.nintendowifi.net/?a=1",
      "http://conntest.nintendowifi.net.evil.com/", "http://conntest.nintendowifi.net",
      "http://conntest.nintendowifi.net:8080/",    "http://user@conntest.nintendowifi.net/"};
  for (const char* target : targets) {
    const auto reply = fakemii_respond(std::string("GET ") + target + " HTTP/1.1\r\n\r\n");
    EXPECT_EQ(reply.bytes, NOT_FOUND_BYTES) << target;
    EXPECT_FALSE(reply.conntest) << target;
  }
}

TEST(FakeMii, OtherMethodsGetNotFound) {
  const char* requests[] = {
      "POST http://conntest.nintendowifi.net/ HTTP/1.1\r\n\r\n",
      "CONNECT example.com:443 HTTP/1.1\r\n\r\n",
      "HEAD http://conntest.nintendowifi.net/ HTTP/1.1\r\n\r\n",
      "get http://conntest.nintendowifi.net/ HTTP/1.1\r\n\r\n",
      "GET http://conntest.nintendowifi.net/ HTTP/2.0\r\n\r\n",
      "GET  http://conntest.nintendowifi.net/ HTTP/1.1\r\n\r\n"};
  for (const char* request : requests) {
    const auto reply = fakemii_respond(request);
    EXPECT_EQ(reply.bytes, NOT_FOUND_BYTES) << request;
    EXPECT_FALSE(reply.conntest) << request;
  }
  EXPECT_EQ(fakemii_respond("CONNECT example.com:443 HTTP/1.1\r\n\r\n").target, "example.com:443");
}

TEST(FakeMii, GarbageGetsNotFound) {
  const std::string inputs[] = {std::string(), std::string("\x00\xff\xfe", 3), "GET", "\r\n\r\n",
                                std::string(5000, 'a')};
  for (const std::string& input : inputs) {
    const auto reply = fakemii_respond(input);
    EXPECT_EQ(reply.bytes, NOT_FOUND_BYTES);
    EXPECT_FALSE(reply.conntest);
    EXPECT_EQ(reply.target, "(invalid request)");
  }
}

TEST(FakeMii, OversizedRequestGetsNotFound) {
  const std::string request = "GET http://conntest.nintendowifi.net/ HTTP/1.1\r\nX-Pad: " +
                              std::string(9000, 'a') + "\r\n\r\n";
  const auto reply = fakemii_respond(request);
  EXPECT_EQ(reply.bytes, NOT_FOUND_BYTES);
  EXPECT_FALSE(reply.conntest);
}

TEST(FakeMii, RequestCompleteNeedsBlankLine) {
  EXPECT_FALSE(fakemii_request_complete("GET / HTTP/1.1\r\n"));
  EXPECT_TRUE(fakemii_request_complete("GET / HTTP/1.1\r\n\r\n"));
}

TEST(FakeMii, TargetIsCutAtTheLimit) {
  const auto reply = fakemii_respond("GET /" + std::string(500, 'p') + " HTTP/1.1\r\n\r\n");
  EXPECT_LE(reply.target.size(), FAKEMII_TARGET_MAX);
  EXPECT_EQ(reply.target.size(), FAKEMII_TARGET_MAX);
  const auto absolute =
      fakemii_respond("GET http://conntest.nintendowifi.net/" + std::string(500, 'p') + " HTTP/1.1\r\n\r\n");
  EXPECT_LE(absolute.target.size(), FAKEMII_TARGET_MAX);
}

TEST(FakeMii, TargetKeepsControlBytesRaw) {
  const auto reply = fakemii_respond("GET /a\x1b[2Jb HTTP/1.1\r\n\r\n");
  EXPECT_NE(reply.target.find('\x1b'), std::string::npos);
  EXPECT_EQ(reply.bytes, NOT_FOUND_BYTES);
}

TEST(FakeMii, PageHasNoLauncherContent) {
  const std::string page = lower(std::string(fakemii_page()));
  EXPECT_EQ(page.find("launcher"), std::string::npos);
  EXPECT_EQ(page.find("<script"), std::string::npos);
  EXPECT_NE(page.find("this is test.html page"), std::string::npos);
  EXPECT_NE(page.find("<title>html page</title>"), std::string::npos);
}

// ---- Socket tests. They use loopback and a free port. ----

TEST(FakeMii, ServesConntestOverLoopback) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  ASSERT_NE(server.port(), 0);
  const std::string reply = fakemii_exchange(server, CONNTEST_REQUEST);
  EXPECT_EQ(reply, fakemii_respond(CONNTEST_REQUEST).bytes);
  EXPECT_EQ(server.served(), 1u);
  EXPECT_TRUE(server.conntest_served());
  EXPECT_EQ(server.last_target(), "conntest.nintendowifi.net/");
}

TEST(FakeMii, ServesNotFoundOverLoopback) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  EXPECT_EQ(fakemii_exchange(server, "GET / HTTP/1.1\r\n\r\n"), NOT_FOUND_BYTES);
  EXPECT_EQ(server.served(), 1u);
  EXPECT_FALSE(server.conntest_served());
}

TEST(FakeMii, ClosesAfterEachReply) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  const int fd = loopback_connect(server.port());
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::send(fd, CONNTEST_REQUEST.data(), CONNTEST_REQUEST.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(CONNTEST_REQUEST.size()));
  std::string reply;
  char buffer[4096];
  while (reply.size() < fakemii_respond(CONNTEST_REQUEST).bytes.size() &&
         poll_until_readable(server, fd)) {
    const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
    ASSERT_GT(count, 0);
    reply.append(buffer, static_cast<size_t>(count));
  }
  EXPECT_TRUE(reply.starts_with("HTTP/1.1 200 OK\r\n"));
  ASSERT_TRUE(poll_until_readable(server, fd));
  EXPECT_EQ(::recv(fd, buffer, sizeof(buffer), 0), 0);
  ::close(fd);
}

TEST(FakeMii, RequestSplitAcrossPackets) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  const int fd = loopback_connect(server.port());
  ASSERT_GE(fd, 0);
  const size_t half = CONNTEST_REQUEST.size() / 2;
  ASSERT_EQ(::send(fd, CONNTEST_REQUEST.data(), half, MSG_NOSIGNAL), static_cast<ssize_t>(half));
  for (int round = 0; round < 5; ++round) {
    server.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(server.served(), 0u);
  const size_t rest = CONNTEST_REQUEST.size() - half;
  ASSERT_EQ(::send(fd, CONNTEST_REQUEST.data() + half, rest, MSG_NOSIGNAL), static_cast<ssize_t>(rest));
  const std::string reply = read_until_eof(server, fd);
  ::close(fd);
  EXPECT_EQ(reply, fakemii_respond(CONNTEST_REQUEST).bytes);
  EXPECT_EQ(server.served(), 1u);
  EXPECT_TRUE(server.conntest_served());
}

TEST(FakeMii, OversizedRequestIsRejected) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  const std::string reply = fakemii_exchange(server, std::string(20000, 'a'));
  EXPECT_EQ(reply.find("200 OK"), std::string::npos);
  EXPECT_TRUE(reply.empty() || reply == NOT_FOUND_BYTES) << reply;
  const std::string oversized = "GET http://conntest.nintendowifi.net/ HTTP/1.1\r\nX-Pad: " +
                                std::string(20000, 'a') + "\r\n\r\n";
  EXPECT_EQ(fakemii_exchange(server, oversized).find("200 OK"), std::string::npos);
  EXPECT_FALSE(server.conntest_served());
  EXPECT_EQ(fakemii_exchange(server, CONNTEST_REQUEST), fakemii_respond(CONNTEST_REQUEST).bytes);
  EXPECT_TRUE(server.conntest_served());
}

// The request ends below the size cap. Extra bytes follow in the same read.
// The server must ignore the extra bytes and serve the page.
TEST(FakeMii, ExtraBytesAfterTheRequestAreIgnored) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  std::string request = "GET http://conntest.nintendowifi.net/ HTTP/1.1\r\nX-Pad: ";
  request += std::string(8100 - request.size(), 'a') + "\r\n\r\n";
  ASSERT_LE(request.size(), FAKEMII_MAX_REQUEST);
  const int fd = loopback_connect(server.port());
  ASSERT_GE(fd, 0);
  const size_t head = 8000;
  ASSERT_EQ(::send(fd, request.data(), head, MSG_NOSIGNAL), static_cast<ssize_t>(head));
  for (int round = 0; round < 5; ++round) {
    server.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(server.served(), 0u);
  const std::string rest = request.substr(head) + std::string(9000, 'b');
  ASSERT_EQ(::send(fd, rest.data(), rest.size(), MSG_NOSIGNAL), static_cast<ssize_t>(rest.size()));
  const std::string reply = read_until_eof(server, fd);
  ::close(fd);
  EXPECT_EQ(reply, fakemii_respond(request).bytes);
  EXPECT_TRUE(server.conntest_served());
}

TEST(FakeMii, SlowClientTimesOut) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  const int fd = loopback_connect(server.port());
  ASSERT_GE(fd, 0);
  server.poll();
  EXPECT_FALSE(gets_eof(fd, 50));
  server.poll(std::chrono::steady_clock::now() + FAKEMII_TIMEOUT + std::chrono::seconds(1));
  EXPECT_TRUE(gets_eof(fd));
  EXPECT_EQ(server.served(), 0u);
  ::close(fd);
}

TEST(FakeMii, ConnectionCapDropsExtraClients) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  std::vector<int> clients;
  for (size_t index = 0; index < FAKEMII_MAX_CONNECTIONS + 2; ++index) {
    const int fd = loopback_connect(server.port());
    ASSERT_GE(fd, 0);
    clients.push_back(fd);
    server.poll();
  }
  size_t closed = 0;
  for (int fd : clients) {
    closed += gets_eof(fd, 100) ? 1 : 0;
  }
  EXPECT_EQ(closed, 2u);
  EXPECT_LE(clients.size() - closed, FAKEMII_MAX_CONNECTIONS);
  server.poll(std::chrono::steady_clock::now() + FAKEMII_TIMEOUT + std::chrono::seconds(1));
  EXPECT_EQ(fakemii_exchange(server, CONNTEST_REQUEST), fakemii_respond(CONNTEST_REQUEST).bytes);
  for (int fd : clients) {
    ::close(fd);
  }
}

TEST(FakeMii, StopClosesThePort) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  const uint16_t port = server.port();
  ASSERT_FALSE(fakemii_exchange(server, CONNTEST_REQUEST).empty());
  EXPECT_EQ(server.served(), 1u);
  server.stop();
  EXPECT_FALSE(server.running());
  EXPECT_EQ(server.served(), 0u);
  EXPECT_TRUE(server.last_target().empty());
  EXPECT_FALSE(server.conntest_served());
  EXPECT_EQ(loopback_connect(port), -1);
  server.stop();
  EXPECT_FALSE(server.running());
}

TEST(FakeMii, StartTwiceRebinds) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  EXPECT_TRUE(server.running());
  EXPECT_EQ(fakemii_exchange(server, CONNTEST_REQUEST), fakemii_respond(CONNTEST_REQUEST).bytes);
}

TEST(FakeMii, BindFailsWhenPortIsInUse) {
  FakeMii first;
  ASSERT_TRUE(first.start("127.0.0.1", 0));
  FakeMii second;
  auto result = second.start("127.0.0.1", first.port());
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("already in use"), std::string::npos) << result.error();
  EXPECT_NE(result.error().find(std::to_string(first.port())), std::string::npos);
  EXPECT_FALSE(second.running());
}

TEST(FakeMii, BindFailsOnAddressNotOnThisComputer) {
  FakeMii server;
  auto result = server.start("203.0.113.1", 0);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("203.0.113.1"), std::string::npos) << result.error();
  EXPECT_FALSE(server.running());
}

TEST(FakeMii, RefusesWildcardAndGarbageAddress) {
  for (const char* address : {"0.0.0.0", "", "nope", "256.1.1.1"}) {
    FakeMii server;
    EXPECT_FALSE(server.start(address, 0)) << address;
    EXPECT_FALSE(server.running()) << address;
  }
}

TEST(FakeMii, BindsOnlyTheGivenAddress) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  EXPECT_EQ(server.address(), "127.0.0.1");
  // Another loopback address must not reach the server. A wildcard bind would accept it.
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in other{};
  other.sin_family = AF_INET;
  other.sin_port = htons(server.port());
  other.sin_addr.s_addr = htonl(0x7F000002);
  EXPECT_NE(::connect(fd, reinterpret_cast<sockaddr*>(&other), sizeof(other)), 0);
  ::close(fd);
}

TEST(FakeMii, DoesNotForward) {
  const int target = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  ASSERT_GE(target, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::bind(target, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::listen(target, 4), 0);
  socklen_t size = sizeof(address);
  ASSERT_EQ(::getsockname(target, reinterpret_cast<sockaddr*>(&address), &size), 0);
  const std::string port = std::to_string(ntohs(address.sin_port));
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 0));
  const std::string request =
      "GET http://127.0.0.1:" + port + "/ HTTP/1.1\r\nHost: 127.0.0.1:" + port + "\r\n\r\n";
  EXPECT_EQ(fakemii_exchange(server, request), NOT_FOUND_BYTES);
  EXPECT_EQ(::accept4(target, nullptr, nullptr, SOCK_NONBLOCK), -1);
  EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
  ::close(target);
}

TEST(FakeMii, PollWhenStoppedIsSafe) {
  FakeMii server;
  server.poll();
  EXPECT_FALSE(server.running());
  EXPECT_EQ(server.served(), 0u);
}

TEST(FakeMii, MovedServerKeepsServing) {
  FakeMii first;
  ASSERT_TRUE(first.start("127.0.0.1", 0));
  FakeMii second(std::move(first));
  EXPECT_TRUE(second.running());
  EXPECT_EQ(fakemii_exchange(second, CONNTEST_REQUEST), fakemii_respond(CONNTEST_REQUEST).bytes);
}

// The manual check in PLAN.md uses this test.
TEST(FakeMiiSmoke, DISABLED_ServeOnLoopbackFor30Seconds) {
  FakeMii server;
  ASSERT_TRUE(server.start("127.0.0.1", 38080));
  std::printf("listening 127.0.0.1:38080\n");
  std::fflush(stdout);
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < end) {
    server.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  server.stop();
}

}
