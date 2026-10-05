#include "privilege.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <thread>

namespace {

using namespace hotmon;

size_t open_fd_count() {
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    (void)entry;
    ++count;
  }
  return count;
}

std::string frame_bytes(std::string_view payload) {
  const uint32_t length = htonl(static_cast<uint32_t>(payload.size()));
  std::string bytes(sizeof(length), '\0');
  std::memcpy(bytes.data(), &length, sizeof(length));
  bytes.append(payload);
  return bytes;
}

// Write the frame to a file. A fake helper script then sends the file to the parent.
std::filesystem::path frame_file(const std::filesystem::path& dir, std::string_view payload) {
  const auto path = dir / "frame.bin";
  std::ofstream(path, std::ios::binary) << frame_bytes(payload);
  return path;
}

Result<StartReport> apply_with_script(const std::string& script) {
  HelperPrivilege helper({"/bin/sh", "-c", script}, {});
  return helper.apply(BackendKind::DirectHostapd, sample_profile());
}

// Ctrl+C at the text prompt of pkexec sends SIGINT to the foreground process group.
// hotmon must not stop. The helper must keep the default action for SIGINT.
TEST(PrivilegeAudit, SigintAtThePromptDoesNotEndHotmon) {
  struct sigaction before {};
  ASSERT_EQ(::sigaction(SIGINT, nullptr, &before), 0);
  auto result = apply_with_script("kill -INT $PPID; sleep 0.2; exit 126");
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("cancelled"), std::string::npos);
  struct sigaction after {};
  ASSERT_EQ(::sigaction(SIGINT, nullptr, &after), 0);
  EXPECT_EQ(after.sa_handler, before.sa_handler);
}

// The helper keeps the default action for SIGINT. If it ignored SIGINT, the script would exit 3.
TEST(PrivilegeAudit, SigintStopsTheHelperAndReportsACancel) {
  auto result = apply_with_script("kill -INT $$; exit 3");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), "Authorization was cancelled. The action was not done.");
}

TEST(PrivilegeAudit, SigquitAtThePromptDoesNotEndHotmon) {
  auto result = apply_with_script("kill -QUIT $PPID; sleep 0.2; exit 126");
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("cancelled"), std::string::npos);
}

// A frame with four descriptors does not fit the control buffer. No descriptor may leak.
TEST(PrivilegeAudit, TruncatedControlDataDoesNotLeakDescriptors) {
  int sockets[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  FileDescriptor sender(sockets[0]);
  FileDescriptor receiver(sockets[1]);
  int pipe_fds[2] = {-1, -1};
  ASSERT_EQ(::pipe(pipe_fds), 0);
  FileDescriptor first(pipe_fds[0]);
  FileDescriptor second(pipe_fds[1]);
  const size_t before = open_fd_count();

  std::string bytes = frame_bytes("{\"ok\":true}");
  alignas(cmsghdr) unsigned char control[CMSG_SPACE(4 * sizeof(int))];
  std::memset(control, 0, sizeof(control));
  iovec io{bytes.data(), bytes.size()};
  msghdr message{};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(4 * sizeof(int));
  const int fds[4] = {first.get(), second.get(), first.get(), second.get()};
  std::memcpy(CMSG_DATA(header), fds, sizeof(fds));
  ASSERT_EQ(::sendmsg(sender.get(), &message, 0), static_cast<ssize_t>(bytes.size()));

  int received = -1;
  auto frame = read_frame(receiver.get(), &received);
  EXPECT_FALSE(frame);
  EXPECT_EQ(received, -1);
  EXPECT_EQ(open_fd_count(), before);
}

TEST(PrivilegeAudit, RequestFieldsWithTheWrongTypeAreRejected) {
  const std::string profile = *profile_to_json(sample_profile());
  const std::string head = "{\"op\":\"stop\",\"backend\":\"iwd\",\"profile\":" + profile;
  EXPECT_FALSE(decode_request(head + ",\"private_hostapd\":1}"));
  EXPECT_FALSE(decode_request(head + ",\"private_hostapd\":\"true\"}"));
  EXPECT_FALSE(decode_request("{\"op\":\"stop\",\"backend\":\"iwd\",\"profile\":\"" + profile + "\"}"));
  EXPECT_FALSE(decode_request("{\"op\":7}"));
  EXPECT_FALSE(decode_request("[\"apply\"]"));
  EXPECT_FALSE(decode_request("null"));
  EXPECT_FALSE(decode_request("{\"op\":\"capture\"}"));
  EXPECT_FALSE(decode_request("{\"op\":\"capture\",\"interface\":\"\"}"));
  EXPECT_FALSE(decode_request("{\"op\":\"capture\",\"interface\":\"wl an0\"}"));
  EXPECT_FALSE(decode_request("{\"op\":\"capture\",\"interface\":\"wlan0/../x\"}"));
  EXPECT_FALSE(decode_request("{\"op\":\"capture\",\"interface\":\"\\u00e9th0\"}"));
  EXPECT_FALSE(decode_request("{\"op\":\"APPLY\",\"backend\":\"iwd\",\"profile\":" + profile + "}"));
  EXPECT_FALSE(decode_request("{\"op\":\"apply\",\"backend\":\"iwd \",\"profile\":" + profile + "}"));
  EXPECT_FALSE(decode_request(std::string("{\"op\":\"apply\"}\0garbage", 22)));
  EXPECT_FALSE(decode_request(std::string(5000, '[')));
}

TEST(PrivilegeAudit, ResponseFieldsWithTheWrongTypeAreRejected) {
  EXPECT_FALSE(decode_response("{\"ok\":\"true\"}"));
  EXPECT_FALSE(decode_response("{\"ok\":1}"));
  EXPECT_FALSE(decode_response("{\"ok\":false}"));
  EXPECT_FALSE(decode_response("{\"ok\":false,\"error\":3}"));
  EXPECT_FALSE(decode_response("{\"ok\":true,\"started\":{}}"));
  EXPECT_FALSE(decode_response("{\"ok\":true,\"started\":[{\"pid\":1.5,\"name\":\"x\"}]}"));
  EXPECT_FALSE(decode_response("{\"ok\":true,\"started\":[{\"pid\":0,\"name\":\"x\"}]}"));
  EXPECT_FALSE(decode_response("{\"ok\":true,\"started\":[{\"pid\":-4,\"name\":\"x\"}]}"));
  EXPECT_FALSE(decode_response("{\"ok\":true,\"started\":[{\"pid\":4294967297,\"name\":\"x\"}]}"));
  EXPECT_FALSE(decode_response("{\"ok\":true,\"started\":[{\"pid\":18446744073709551615,\"name\":\"x\"}]}"));
  EXPECT_FALSE(decode_response("{\"ok\":true,\"started\":[{\"pid\":5}]}"));
  EXPECT_FALSE(decode_response("{\"ok\":true,\"started\":[7]}"));
  EXPECT_FALSE(decode_response("{\"ok\":true,\"started\":[],\"private_hostapd\":\"no\"}"));
}

TEST(PrivilegeAudit, UnicodeErrorTextRoundTrips) {
  Response failure;
  failure.ok = false;
  failure.error = "Fehler: Gerät \xe2\x80\x9cwlan0\xe2\x80\x9d \"quoted\" \\ end";
  auto decoded = decode_response(encode_response(failure));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->error, failure.error);
}

TEST(PrivilegeAudit, FrameAtTheLimitIsAcceptedAndOneMoreByteIsRejected) {
  const std::string limit(1024 * 1024, 'a');
  EXPECT_FALSE(write_frame(-1, limit + "a"));
  int sockets[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  FileDescriptor a(sockets[0]);
  FileDescriptor b(sockets[1]);
  std::string got;
  std::thread reader([&] {
    auto frame = read_frame(b.get());
    if (frame) {
      got = *frame;
    }
  });
  EXPECT_TRUE(write_frame(a.get(), limit));
  reader.join();
  EXPECT_EQ(got.size(), limit.size());
}

TEST(PrivilegeAudit, HelperGarbageAndOversizeFramesGiveTheGenericError) {
  const auto dir = scratch_dir();
  for (const std::string& script :
       {std::string("cat >/dev/null; printf 'xx'"),
        std::string("cat >/dev/null; printf '\\377\\377\\377\\377abc'"),
        std::string("cat >/dev/null; printf '\\0\\0\\0\\0'"),
        "cat >/dev/null; cat " + frame_file(dir, "{\"ok\":tru").string(),
        std::string("cat >/dev/null; exit 0"), std::string("kill -KILL $$")}) {
    auto result = apply_with_script(script);
    ASSERT_FALSE(result) << script;
    EXPECT_NE(result.error().find("stopped without a result"), std::string::npos) << script;
  }
  std::filesystem::remove_all(dir);
}

TEST(PrivilegeAudit, CaptureSuccessWithoutDescriptorIsAnError) {
  const auto dir = scratch_dir();
  HelperPrivilege helper({"/bin/sh", "-c", "cat >/dev/null; cat " + frame_file(dir, "{\"ok\":true}").string()},
                         {});
  auto result = helper.open_capture_socket("wlan0");
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("stopped without a result"), std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(PrivilegeAudit, HelperErrorIsReturnedUnchanged) {
  const auto dir = scratch_dir();
  Response failure;
  failure.ok = false;
  failure.error = "The hostapd program did not start.";
  const auto path = frame_file(dir, encode_response(failure));
  auto result = apply_with_script("cat >/dev/null; cat " + path.string() + "; exit 3");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), failure.error);
  std::filesystem::remove_all(dir);
}

TEST(PrivilegeAudit, RepeatedCallsDoNotLeakDescriptors) {
  (void)apply_with_script("exit 126");
  const size_t before = open_fd_count();
  for (int round = 0; round < 20; ++round) {
    (void)apply_with_script("exit 126");
    (void)apply_with_script("cat >/dev/null; exit 127");
  }
  HelperPrivilege missing({"definitely-not-a-program"}, {});
  for (int round = 0; round < 20; ++round) {
    (void)missing.stop(StopRequest{BackendKind::Iwd, sample_profile(), false, {}});
  }
  EXPECT_EQ(open_fd_count(), before);
}

TEST(PrivilegeAudit, EmptyArgvGivesThePkexecMessage) {
  int resumes = 0;
  TerminalHooks hooks;
  hooks.resume = [&] { ++resumes; };
  HelperPrivilege empty({}, hooks);
  auto result = empty.open_capture_socket("wlan0");
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("pkexec"), std::string::npos);
  EXPECT_EQ(resumes, 1);
  HelperPrivilege blank({""}, {});
  EXPECT_FALSE(blank.apply(BackendKind::Iwd, sample_profile()));
}

TEST(PrivilegeAudit, HelperRejectsAPassedDescriptorFromTheCaller) {
  int sockets[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  FileDescriptor parent(sockets[0]);
  FileDescriptor child(sockets[1]);
  int pipe_fds[2] = {-1, -1};
  ASSERT_EQ(::pipe(pipe_fds), 0);
  FileDescriptor read_end(pipe_fds[0]);
  FileDescriptor write_end(pipe_fds[1]);
  ASSERT_TRUE(write_frame(parent.get(), "{\"op\":\"reboot\"}", read_end.get()));
  ::shutdown(parent.get(), SHUT_WR);
  const size_t before = open_fd_count();
  class Never : public Privileged {
   public:
    int calls = 0;
    Result<StartReport> apply(BackendKind, const Profile&) override {
      ++calls;
      return StartReport{};
    }
    Result<void> stop(const StopRequest&) override {
      ++calls;
      return {};
    }
    Result<FileDescriptor> open_capture_socket(std::string_view) override {
      ++calls;
      return unexpected_text("No.");
    }
  } worker;
  const int saved_in = ::dup(STDIN_FILENO);
  const int saved_out = ::dup(STDOUT_FILENO);
  ::dup2(child.get(), STDIN_FILENO);
  ::dup2(child.get(), STDOUT_FILENO);
  const int code = run_privileged_helper(worker);
  ::dup2(saved_in, STDIN_FILENO);
  ::dup2(saved_out, STDOUT_FILENO);
  ::close(saved_in);
  ::close(saved_out);
  EXPECT_EQ(code, 1);
  EXPECT_EQ(worker.calls, 0);
  EXPECT_EQ(open_fd_count(), before);
}

}
