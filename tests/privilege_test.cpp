#include "privilege.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>

namespace {

using namespace hotmon;

class FdSwap {
 public:
  FdSwap(int target, int source) : target_(target), saved_(::dup(target)) { ::dup2(source, target); }
  ~FdSwap() {
    if (saved_ >= 0) {
      ::dup2(saved_, target_);
      ::close(saved_);
    }
  }
  FdSwap(const FdSwap&) = delete;
  FdSwap& operator=(const FdSwap&) = delete;

 private:
  int target_ = -1;
  int saved_ = -1;
};

struct HelperRun {
  int code = -1;
  Result<std::string> frame = unexpected_text("The helper was not run.");
  int fd = -1;
};

HelperRun run_worker(Privileged& worker, std::string_view request, const Paths* paths) {
  int sockets[2] = {-1, -1};
  HelperRun run;
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
    run.frame = unexpected_text("The test socket did not open.");
    return run;
  }
  FileDescriptor parent(sockets[0]);
  FileDescriptor child(sockets[1]);
  if (auto wrote = write_frame(parent.get(), request); !wrote) {
    run.frame = unexpected_text(wrote.error());
    return run;
  }
  ::shutdown(parent.get(), SHUT_WR);
  {
    FdSwap input(STDIN_FILENO, child.get());
    FdSwap output(STDOUT_FILENO, child.get());
    child = FileDescriptor();
    if (paths == nullptr) {
      run.code = run_privileged_helper(worker);
    } else {
      run.code = run_privileged_helper(worker, *paths);
    }
  }
  run.frame = read_frame(parent.get(), &run.fd);
  return run;
}

class FakePrivileged : public Privileged {
 public:
  int apply_calls = 0;
  int stop_calls = 0;
  int capture_calls = 0;
  BackendKind seen_backend = BackendKind::DirectHostapd;
  Profile seen_profile;
  StopRequest seen_stop;
  Result<StartReport> apply_result = StartReport{};
  Result<void> stop_result = {};
  Result<FileDescriptor> capture_result = unexpected_text("No socket.");

  Result<StartReport> apply(BackendKind backend, const Profile& profile) override {
    ++apply_calls;
    seen_backend = backend;
    seen_profile = profile;
    return apply_result;
  }

  Result<void> stop(const StopRequest& request) override {
    ++stop_calls;
    seen_stop = request;
    return stop_result;
  }

  Result<FileDescriptor> open_capture_socket(std::string_view) override {
    ++capture_calls;
    return std::move(capture_result);
  }
};

Request apply_request(const Profile& profile) {
  Request request;
  request.op = PrivilegeOp::Apply;
  request.backend = BackendKind::DirectHostapd;
  request.profile = profile;
  return request;
}

TEST(Privilege, ApplyRequestRoundTrips) {
  const Profile profile = sample_profile();
  auto decoded = decode_request(encode_request(apply_request(profile)));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->op, PrivilegeOp::Apply);
  EXPECT_EQ(decoded->backend, BackendKind::DirectHostapd);
  EXPECT_EQ(decoded->profile, profile);
}

TEST(Privilege, StopAndCaptureRequestsRoundTrip) {
  Request stop;
  stop.op = PrivilegeOp::Stop;
  stop.backend = BackendKind::Iwd;
  stop.private_hostapd = true;
  stop.profile = sample_profile();
  auto decoded = decode_request(encode_request(stop));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->op, PrivilegeOp::Stop);
  EXPECT_TRUE(decoded->private_hostapd);
  EXPECT_EQ(decoded->profile, stop.profile);
  stop.private_hostapd = false;
  decoded = decode_request(encode_request(stop));
  ASSERT_TRUE(decoded);
  EXPECT_FALSE(decoded->private_hostapd);

  Request capture;
  capture.op = PrivilegeOp::Capture;
  capture.interface = "wlan0";
  decoded = decode_request(encode_request(capture));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->op, PrivilegeOp::Capture);
  EXPECT_EQ(decoded->interface, "wlan0");
}

TEST(Privilege, DecodeRequestRejectsBadText) {
  EXPECT_FALSE(decode_request(""));
  EXPECT_FALSE(decode_request("{"));
  EXPECT_FALSE(decode_request("{\"op\":\"reboot\"}"));
  EXPECT_FALSE(decode_request("{\"op\":\"apply\",\"backend\":\"wifi\",\"profile\":{}}"));
  EXPECT_FALSE(decode_request("{\"op\":\"apply\",\"backend\":\"iwd\"}"));
  EXPECT_FALSE(decode_request("{\"op\":\"capture\",\"interface\":\"../x\"}"));
  EXPECT_FALSE(decode_request("{\"op\":\"capture\",\"interface\":\"wlan0wlan0wlan01\"}"));
  EXPECT_FALSE(decode_request("{\"op\":1}"));
}

TEST(Privilege, BadInterfaceDoesNotCallTheWorker) {
  FakePrivileged worker;
  auto run = run_worker(worker, "{\"op\":\"capture\",\"interface\":\"../x\"}", nullptr);
  EXPECT_EQ(run.code, 1);
  EXPECT_EQ(worker.capture_calls, 0);
  ASSERT_TRUE(run.frame);
  auto response = decode_response(*run.frame);
  ASSERT_TRUE(response);
  EXPECT_FALSE(response->ok);
}

TEST(Privilege, ResponseRoundTrips) {
  Response success;
  success.ok = true;
  StartReport report;
  report.started = {StartedProc{11, "hostapd"}, StartedProc{12, "dnsmasq"}};
  report.private_hostapd = true;
  success.report = report;
  auto decoded = decode_response(encode_response(success));
  ASSERT_TRUE(decoded);
  ASSERT_TRUE(decoded->ok);
  ASSERT_TRUE(decoded->report);
  ASSERT_EQ(decoded->report->started.size(), 2u);
  EXPECT_EQ(decoded->report->started[0].pid, 11);
  EXPECT_EQ(decoded->report->started[1].name, "dnsmasq");
  EXPECT_TRUE(decoded->report->private_hostapd);

  Response failure;
  failure.ok = false;
  failure.error = "The backend rejected the setting.";
  decoded = decode_response(encode_response(failure));
  ASSERT_TRUE(decoded);
  EXPECT_FALSE(decoded->ok);
  EXPECT_EQ(decoded->error, failure.error);

  Response stop;
  stop.ok = true;
  decoded = decode_response(encode_response(stop));
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(decoded->ok);
  EXPECT_FALSE(decoded->report);
}

TEST(Privilege, FramesRoundTripOverASocket) {
  int sockets[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  FileDescriptor left(sockets[0]);
  FileDescriptor right(sockets[1]);
  ASSERT_TRUE(write_frame(left.get(), "hello"));
  auto frame = read_frame(right.get());
  ASSERT_TRUE(frame);
  EXPECT_EQ(*frame, "hello");
  ASSERT_TRUE(write_frame(left.get(), ""));
  frame = read_frame(right.get());
  ASSERT_TRUE(frame);
  EXPECT_TRUE(frame->empty());
  const uint32_t huge = htonl(1024 * 1024 + 1);
  ASSERT_EQ(::write(left.get(), &huge, sizeof(huge)), static_cast<ssize_t>(sizeof(huge)));
  frame = read_frame(right.get());
  EXPECT_FALSE(frame);
  int other[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, other), 0);
  FileDescriptor peer(other[0]);
  FileDescriptor writer(other[1]);
  const uint32_t short_length = htonl(8);
  ASSERT_EQ(::write(writer.get(), &short_length, sizeof(short_length)),
            static_cast<ssize_t>(sizeof(short_length)));
  ASSERT_EQ(::write(writer.get(), "ab", 2), 2);
  writer = FileDescriptor();
  EXPECT_FALSE(read_frame(peer.get()));
}

TEST(Privilege, FrameCanPassAFileDescriptor) {
  int sockets[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  int pipe_fd[2] = {-1, -1};
  ASSERT_EQ(::pipe(pipe_fd), 0);
  ASSERT_EQ(::write(pipe_fd[1], "abc", 3), 3);
  ::close(pipe_fd[1]);
  ASSERT_TRUE(write_frame(sockets[0], "fd", pipe_fd[0]));
  ::close(pipe_fd[0]);
  int received = -1;
  auto frame = read_frame(sockets[1], &received);
  ASSERT_TRUE(frame);
  EXPECT_EQ(*frame, "fd");
  ASSERT_GE(received, 0);
  char buffer[4] = {};
  ASSERT_EQ(::read(received, buffer, sizeof(buffer)), 3);
  EXPECT_EQ(std::string(buffer, 3), "abc");
  ::close(received);
  ::close(sockets[0]);
  ::close(sockets[1]);
}

TEST(Privilege, BackendTokensRoundTrip) {
  for (auto kind : {BackendKind::NetworkManager, BackendKind::Iwd, BackendKind::ExistingHostapd,
                    BackendKind::DirectHostapd}) {
    auto parsed = parse_backend_token(backend_token(kind));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, kind);
  }
  EXPECT_FALSE(parse_backend_token("wifi"));
}

TEST(Privilege, HelperApplyCallsTheWorker) {
  FakePrivileged worker;
  StartReport report;
  report.started = {StartedProc{7, "hostapd"}};
  worker.apply_result = report;
  auto run = run_worker(worker, encode_request(apply_request(sample_profile())), nullptr);
  EXPECT_EQ(run.code, 0);
  EXPECT_EQ(worker.apply_calls, 1);
  EXPECT_EQ(worker.seen_backend, BackendKind::DirectHostapd);
  EXPECT_EQ(worker.seen_profile, sample_profile());
  ASSERT_TRUE(run.frame);
  auto response = decode_response(*run.frame);
  ASSERT_TRUE(response);
  EXPECT_TRUE(response->ok);
  ASSERT_TRUE(response->report);
  ASSERT_EQ(response->report->started.size(), 1u);
  EXPECT_EQ(response->report->started[0].pid, 7);
}

TEST(Privilege, HelperStopCallsTheWorker) {
  FakePrivileged worker;
  Request request;
  request.op = PrivilegeOp::Stop;
  request.backend = BackendKind::NetworkManager;
  request.profile = sample_profile();
  auto run = run_worker(worker, encode_request(request), nullptr);
  EXPECT_EQ(run.code, 0);
  EXPECT_EQ(worker.stop_calls, 1);
  EXPECT_EQ(worker.seen_stop.backend, BackendKind::NetworkManager);
  ASSERT_TRUE(run.frame);
  auto response = decode_response(*run.frame);
  ASSERT_TRUE(response);
  EXPECT_TRUE(response->ok);
}

TEST(Privilege, HelperReturnsTheWorkerError) {
  FakePrivileged worker;
  worker.apply_result = unexpected_text("The backend rejected the setting.");
  auto run = run_worker(worker, encode_request(apply_request(sample_profile())), nullptr);
  EXPECT_EQ(run.code, 0);
  ASSERT_TRUE(run.frame);
  auto response = decode_response(*run.frame);
  ASSERT_TRUE(response);
  EXPECT_FALSE(response->ok);
  EXPECT_EQ(response->error, "The backend rejected the setting.");
}

TEST(Privilege, HelperCapturePassesAFileDescriptor) {
  int pipe_fd[2] = {-1, -1};
  ASSERT_EQ(::pipe(pipe_fd), 0);
  ASSERT_EQ(::write(pipe_fd[1], "pkt", 3), 3);
  ::close(pipe_fd[1]);
  FakePrivileged worker;
  worker.capture_result = FileDescriptor(pipe_fd[0]);
  Request request;
  request.op = PrivilegeOp::Capture;
  request.interface = "wlan0";
  auto run = run_worker(worker, encode_request(request), nullptr);
  EXPECT_EQ(run.code, 0);
  EXPECT_EQ(worker.capture_calls, 1);
  ASSERT_GE(run.fd, 0);
  char buffer[4] = {};
  ASSERT_EQ(::read(run.fd, buffer, sizeof(buffer)), 3);
  EXPECT_EQ(std::string(buffer, 3), "pkt");
  ::close(run.fd);
}

TEST(Privilege, InvalidRequestDoesNotCallTheWorker) {
  FakePrivileged worker;
  int sockets[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  FileDescriptor parent(sockets[0]);
  FileDescriptor child(sockets[1]);
  ASSERT_EQ(::write(parent.get(), "nope", 4), 4);
  ::shutdown(parent.get(), SHUT_WR);
  int code = -1;
  {
    FdSwap input(STDIN_FILENO, child.get());
    FdSwap output(STDOUT_FILENO, child.get());
    child = FileDescriptor();
    code = run_privileged_helper(worker);
  }
  EXPECT_EQ(code, 1);
  EXPECT_EQ(worker.apply_calls, 0);
  EXPECT_EQ(worker.stop_calls, 0);
  EXPECT_EQ(worker.capture_calls, 0);

  auto unknown = run_worker(worker, "{\"op\":\"reboot\"}", nullptr);
  EXPECT_EQ(unknown.code, 1);
  EXPECT_EQ(worker.apply_calls, 0);
}

TEST(Privilege, InvalidProfileDoesNotRunCommands) {
  const auto dir = scratch_dir();
  const auto paths = test_paths(dir);
  ScriptedRunner runner;
  RecordedSignals signals;
  DirectPrivilege worker(runner, signals, paths);
  Profile profile = sample_profile();
  profile.ssid.clear();
  auto empty = run_worker(worker, encode_request(apply_request(profile)), &paths);
  EXPECT_EQ(runner.calls.size(), 0u);
  ASSERT_TRUE(empty.frame);
  auto response = decode_response(*empty.frame);
  ASSERT_TRUE(response);
  EXPECT_FALSE(response->ok);

  profile = sample_profile();
  profile.ssid = std::string(33, 'a');
  auto long_name = run_worker(worker, encode_request(apply_request(profile)), &paths);
  EXPECT_EQ(runner.calls.size(), 0u);
  ASSERT_TRUE(long_name.frame);
  response = decode_response(*long_name.frame);
  ASSERT_TRUE(response);
  EXPECT_FALSE(response->ok);

  profile = sample_profile();
  profile.channel = 0;
  auto channel = run_worker(worker, encode_request(apply_request(profile)), &paths);
  EXPECT_EQ(runner.calls.size(), 0u);
  ASSERT_TRUE(channel.frame);
  response = decode_response(*channel.frame);
  ASSERT_TRUE(response);
  EXPECT_FALSE(response->ok);
  std::filesystem::remove_all(dir);
}

TEST(Privilege, HelperStopUsesPidFilesNotRequestPids) {
  const auto dir = scratch_dir();
  const auto paths = test_paths(dir);
  std::filesystem::create_directories(paths.state_dir);
  std::ofstream(paths.hostapd_pid()) << "42\n";
  std::ofstream(paths.dnsmasq_pid()) << "77\n";
  ScriptedRunner runner;
  RecordedSignals signals;
  signals.names.push_back({42, "hostapd"});
  signals.names.push_back({77, "dnsmasq"});
  signals.names.push_back({999, "hostapd"});
  DirectPrivilege worker(runner, signals, paths);
  std::string json = encode_request([&] {
    Request request;
    request.op = PrivilegeOp::Stop;
    request.backend = BackendKind::DirectHostapd;
    request.profile = sample_profile();
    return request;
  }());
  ASSERT_FALSE(json.empty());
  json.insert(1, "\"pid\":999,\"started\":[{\"pid\":999,\"name\":\"hostapd\"}],");
  auto run = run_worker(worker, json, &paths);
  EXPECT_EQ(run.code, 0);
  EXPECT_EQ(signals.pids, std::vector<int>({42, 77}));
  std::filesystem::remove_all(dir);
}

TEST(Privilege, Exit126CancelsAndResumes) {
  int suspends = 0;
  int resumes = 0;
  int order = 0;
  int suspend_at = 0;
  int resume_at = 0;
  TerminalHooks hooks;
  hooks.suspend = [&] {
    ++suspends;
    suspend_at = ++order;
  };
  hooks.resume = [&] {
    ++resumes;
    resume_at = ++order;
  };
  HelperPrivilege helper({"/bin/sh", "-c", "exit 126"}, hooks);
  auto error = helper.apply(BackendKind::DirectHostapd, sample_profile());
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("cancelled"), std::string::npos);
  EXPECT_EQ(suspends, 1);
  EXPECT_EQ(resumes, 1);
  EXPECT_LT(suspend_at, resume_at);
}

TEST(Privilege, Exit127ReportsAuthorizationFailure) {
  HelperPrivilege helper({"/bin/sh", "-c", "exit 127"}, {});
  auto error = helper.apply(BackendKind::DirectHostapd, sample_profile());
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("Authorization failed"), std::string::npos);
}

TEST(Privilege, Exit127KeepsTheStderrLine) {
  HelperPrivilege helper({"/bin/sh", "-c", "echo agent-refused >&2; exit 127"}, {});
  auto error = helper.apply(BackendKind::DirectHostapd, sample_profile());
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("agent-refused"), std::string::npos);
}

TEST(Privilege, MissingProgramMentionsPkexec) {
  HelperPrivilege helper({"definitely-not-a-program"}, {});
  auto error = helper.apply(BackendKind::DirectHostapd, sample_profile());
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("pkexec"), std::string::npos);
}

TEST(Privilege, HelperScriptCanReturnAStartedList) {
  const auto dir = scratch_dir();
  const auto frame_path = dir / "frame.bin";
  Response success;
  success.ok = true;
  StartReport report;
  report.started = {StartedProc{21, "hostapd"}, StartedProc{22, "dnsmasq"}};
  success.report = report;
  const int fd = ::open(frame_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(write_frame(fd, encode_response(success)));
  ::close(fd);
  const std::string script = "cat >/dev/null; cat " + frame_path.string();
  HelperPrivilege helper({"/bin/sh", "-c", script}, {});
  auto result = helper.apply(BackendKind::DirectHostapd, sample_profile());
  ASSERT_TRUE(result);
  ASSERT_EQ(result->started.size(), 2u);
  EXPECT_EQ(result->started[0].pid, 21);
  EXPECT_EQ(result->started[1].name, "dnsmasq");
  std::filesystem::remove_all(dir);
}

TEST(Privilege, PassphraseStaysOutOfTheHelperArgv) {
  const auto dir = scratch_dir();
  const auto saved = dir / "request.bin";
  const std::string script = "cat > " + saved.string();
  std::vector<std::string> argv = {"/bin/sh", "-c", script};
  HelperPrivilege helper(argv, {});
  const Profile profile = sample_profile();
  (void)helper.apply(BackendKind::DirectHostapd, profile);
  EXPECT_EQ(argv, (std::vector<std::string>{"/bin/sh", "-c", script}));
  for (const auto& arg : argv) {
    EXPECT_EQ(arg.find(profile.passphrase), std::string::npos);
  }
  const std::string encoded = encode_request(apply_request(profile));
  EXPECT_NE(encoded.find(profile.passphrase), std::string::npos);
  std::ifstream input(saved);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find(profile.passphrase), std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(Privilege, MakePrivilegedChoosesByEuid) {
  ScriptedRunner runner;
  RecordedSignals signals;
  const Paths paths = Paths::system();
  auto root = make_privileged(0, runner, signals, paths, {"/bin/true"}, {});
  EXPECT_NE(dynamic_cast<DirectPrivilege*>(root.get()), nullptr);
  auto user = make_privileged(1000, runner, signals, paths, {"/bin/true"}, {});
  EXPECT_NE(dynamic_cast<HelperPrivilege*>(user.get()), nullptr);
  EXPECT_EQ(dynamic_cast<DirectPrivilege*>(user.get()), nullptr);
}

// Security: a program named pkexec in a user PATH directory must not get the admin password.
TEST(Privilege, DefaultHelperArgvDoesNotSearchPath) {
  const auto dir = scratch_dir();
  const auto fake = dir / "pkexec";
  {
    std::ofstream out(fake);
    out << "#!/bin/sh\nexit 0\n";
  }
  std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
  const char* old_path = std::getenv("PATH");
  const std::string saved = old_path != nullptr ? old_path : "";
  ::setenv("PATH", dir.c_str(), 1);
  const auto argv = default_helper_argv();
  ::setenv("PATH", saved.c_str(), 1);
  ASSERT_EQ(argv.size(), 3u);
  ASSERT_FALSE(argv[0].empty());
  EXPECT_EQ(argv[0].front(), '/');
  EXPECT_FALSE(argv[0].starts_with(dir.string()));
  EXPECT_EQ(argv[2], "--privileged-helper");
  std::filesystem::remove_all(dir);
}

}
