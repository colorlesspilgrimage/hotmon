#pragma once

#include "profile.hpp"
#include "result.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hotmon {

enum class BackendKind { NetworkManager, Iwd, ExistingHostapd, DirectHostapd };

const char* label(BackendKind kind);
inline constexpr const char* DIRECT_TOOLS[] = {"hostapd", "dnsmasq", "nftables"};

// Looks in the folders where NetworkManager looks for its helper programs.
bool system_program_installed(std::string_view name);
// NetworkManager runs dnsmasq itself for a shared (DHCP) hotspot and fails late without it.
Result<void> check_backend_tools(BackendKind kind, const Profile& profile,
                                 const std::function<bool(std::string_view)>& installed);

struct ProbeFacts {
  bool network_manager_active = false;
  bool iwd_active = false;
  bool hostapd_setup = false;
};

BackendKind select_backend(const ProbeFacts& facts);
bool service_active(std::string_view unit);
ProbeFacts probe_system();

struct Paths {
  std::filesystem::path state_dir;
  std::filesystem::path hostapd_config;
  std::filesystem::path iwd_ap_dir;
  std::filesystem::path proc_root;
  // NetworkManager loads keyfiles only from its own folders.
  std::filesystem::path nm_connection_dir;
  // NetworkManager's dnsmasq for shared connections reads extra settings only from here.
  std::filesystem::path nm_dnsmasq_dir;

  static Paths system();
  std::filesystem::path hostapd_pid() const;
  std::filesystem::path dnsmasq_pid() const;
  std::filesystem::path direct_hostapd_conf() const;
  std::filesystem::path dnsmasq_dir() const;
  std::filesystem::path dnsmasq_conf() const;
  std::filesystem::path dnsmasq_lease() const;
  std::filesystem::path nft_path() const;
  std::filesystem::path nm_secret() const;
  std::filesystem::path nm_dnsmasq_conf() const;
  std::filesystem::path hostapd_backup() const;
  std::filesystem::path created_marker() const;
  std::filesystem::path forwarding_record() const;
  std::filesystem::path nft_live_marker() const;
  std::filesystem::path ufw_record() const;
};

void clear_nft_live(const Paths& paths);

struct PlannedCommand {
  std::string program;
  std::vector<std::string> args;
  bool optional = false;
  // Some tools exit 0 after a failure. Text here on stderr marks the command as failed.
  std::string stderr_failure;

  static PlannedCommand make(std::string program, std::vector<std::string> args);
  PlannedCommand optional_command() const;
  PlannedCommand failing_on_stderr(std::string marker) const;
  bool operator==(const PlannedCommand&) const = default;
};

struct PlanFile {
  std::filesystem::path path;
  std::string contents;
  uint32_t mode = 0644;
  bool remove_on_failure = false;
  bool lock_parent = false;
};

struct ApplyPlan {
  std::vector<PlanFile> files;
  std::vector<PlannedCommand> commands;
  std::vector<std::string> tools;
};

Result<ApplyPlan> plan_apply(BackendKind kind, const Profile& profile, const Paths& paths);
std::vector<PlannedCommand> plan_stop(BackendKind kind, const Profile& profile);

class Runner {
 public:
  virtual ~Runner() = default;
  virtual Result<std::string> run(const PlannedCommand& command) = 0;
};

Result<void> run_stop_commands(const std::vector<PlannedCommand>& commands, Runner& runner);

}
