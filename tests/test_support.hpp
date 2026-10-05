#pragma once

#include "backend.hpp"
#include "backend_exec.hpp"
#include "capture.hpp"
#include "fakemii.hpp"
#include "iface.hpp"
#include "profile.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace hotmon {

Profile sample_profile();
std::vector<IfaceInfo> sample_interfaces();
std::filesystem::path scratch_dir();
Paths test_paths(const std::filesystem::path& dir);

class ScriptedRunner : public Runner {
 public:
  std::vector<PlannedCommand> calls;
  std::vector<Result<std::string>> results;
  size_t index = 0;

  static ScriptedRunner with_results(std::vector<Result<std::string>> results);
  Result<std::string> run(const PlannedCommand& command) override;
};

class RecordedSignals : public ProcessControl {
 public:
  std::vector<int> pids;
  bool fail = false;
  bool stay_alive = false;
  std::vector<std::pair<int, std::string>> names;

  std::optional<std::string> describe(int pid) override;
  bool running(int pid) override;
  Result<void> terminate(int pid) override;
};

class PidOnSuccess : public Runner {
 public:
  ScriptedRunner inner;
  std::filesystem::path pid_path;
  int pid = 0;
  Result<std::string> run(const PlannedCommand& command) override;
};

class ModeCheck : public Runner {
 public:
  bool saw_private_secret = false;
  Result<std::string> run(const PlannedCommand& command) override;
};

class FakeSource : public FrameSource {
 public:
  std::vector<std::vector<uint8_t>> frames;
  int reads = 0;
  Result<std::optional<std::vector<uint8_t>>> try_recv() override;
};

// Connect a blocking TCP client to 127.0.0.1. Return -1 on failure.
int loopback_connect(uint16_t port);
// Poll the server every 10 ms until the client fd has data or EOF, or 2 s pass.
bool poll_until_readable(FakeMii& server, int fd);
// Read until EOF or an error. Poll the server between reads.
std::string read_until_eof(FakeMii& server, int fd);
// Send one request through a new loopback client and return the full reply.
std::string fakemii_exchange(FakeMii& server, std::string_view request);

}
