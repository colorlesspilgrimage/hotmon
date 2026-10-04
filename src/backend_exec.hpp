#pragma once

#include "backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace hotmon {

class ProcessControl {
 public:
  virtual ~ProcessControl() = default;
  virtual std::optional<std::string> describe(int pid) = 0;
  virtual bool running(int pid) = 0;
  virtual Result<void> terminate(int pid) = 0;
};

struct StartedProc {
  int pid = 0;
  std::string name;
};

struct StartReport {
  std::vector<StartedProc> started;
  bool private_hostapd = false;
};

class SystemRunner : public Runner {
 public:
  Result<std::string> run(const PlannedCommand& command) override;
};

class SystemSignals : public ProcessControl {
 public:
  std::optional<std::string> describe(int pid) override;
  bool running(int pid) override;
  Result<void> terminate(int pid) override;
};

Result<StartReport> execute_plan(const ApplyPlan& plan, Runner& runner, ProcessControl& signals,
                                 const Paths& paths);
Result<void> run_nft_delete(Runner& runner);
Result<void> stop_started(ProcessControl& signals, const std::vector<StartedProc>& started);
Result<void> restore_forwarding(const Paths& paths);
Result<void> restore_hostapd_backup(const Paths& paths);
Result<void> retire_iwd_profile(const std::filesystem::path& path);
Result<void> secure_state_dir(const std::filesystem::path& path);
Result<int> read_pid(const std::filesystem::path& path);
std::optional<int> read_pid_optional(const std::filesystem::path& path);
std::optional<std::string> daemon_name(std::optional<std::string> comm,
                                       std::optional<std::string> cmdline);
std::filesystem::path forwarding_path(const Paths& paths, std::string_view iface);
Result<bool> enable_forwarding(const Paths& paths, const std::vector<std::string>& ifaces);
bool stop_target_missing(std::string_view message);

}
