#include <unistd.h>

#include <format>
#include "backend.hpp"

#include "backend_text.hpp"
#include "process.hpp"
#include "text.hpp"

namespace hotmon {
namespace {


PlanFile plain_file(std::filesystem::path path, std::string contents, uint32_t mode) {
  PlanFile file;
  file.path = std::move(path);
  file.contents = std::move(contents);
  file.mode = mode;
  file.remove_on_failure = mode == 0600;
  return file;
}

PlanFile lock_parent(PlanFile file) {
  file.lock_parent = true;
  return file;
}

std::string gateway_cidr(const Profile& profile) {
  if (auto network = profile.network()) {
    if (auto gateway = network->gateway()) {
      return *gateway + "/" + std::to_string(network->prefix());
    }
  }
  return profile.address_cidr;
}

ApplyPlan nm_plan(const Profile& profile, const Paths& paths) {
  const auto keyfile = paths.nm_secret();
  ApplyPlan plan;
  // The keyfile stays while the hotspot runs. `nmcli connection delete` removes it at stop.
  // A delete before the load would remove the new file, because the old profile used this path.
  plan.files.push_back(plain_file(keyfile, nm_keyfile(profile), 0600));
  // Written every time, so a reapply with an upstream drops the FakeMii name. Removed at stop.
  PlanFile dns = plain_file(paths.nm_dnsmasq_conf(), fakemii_dns_text(profile), 0644);
  dns.remove_on_failure = true;
  plan.files.push_back(std::move(dns));
  plan.commands.push_back(PlannedCommand::make("nmcli", {"connection", "load", keyfile.string()})
                              .failing_on_stderr("Could not load file"));
  plan.commands.push_back(PlannedCommand::make("nmcli", {"connection", "up", "hotmon"}));
  plan.tools.push_back("NetworkManager");
  return plan;
}

ApplyPlan iwd_plan(const Profile& profile, const Paths& paths) {
  const auto path = paths.iwd_ap_dir / (profile.ssid + ".ap");
  ApplyPlan plan;
  plan.files.push_back(lock_parent(plain_file(path, iwd_profile(profile), 0600)));
  plan.commands.push_back(PlannedCommand::make(
      "iwctl", {"device", profile.ap_interface, "set-property", "Mode", "ap"}));
  plan.commands.push_back(PlannedCommand::make(
      "iwctl", {"ap", profile.ap_interface, "start-profile", profile.ssid}));
  plan.tools.push_back("iwd");
  return plan;
}

ApplyPlan hostapd_plan(const Profile& profile, const Paths& paths, bool existing) {
  const auto hostapd_conf = existing ? paths.hostapd_config : paths.direct_hostapd_conf();
  ApplyPlan plan;
  plan.files.push_back(plain_file(hostapd_conf, hostapd_conf_text(profile), 0600));
  plan.files.push_back(plain_file(paths.dnsmasq_conf(), dnsmasq_conf_text(profile), 0644));
  plan.files.push_back(plain_file(paths.nft_path(), nft_text(profile), 0644));
  const std::string gateway = gateway_cidr(profile);
  plan.commands.push_back(
      PlannedCommand::make("ip", {"link", "set", profile.ap_interface, "up"}));
  plan.commands.push_back(
      PlannedCommand::make("ip", {"addr", "replace", gateway, "dev", profile.ap_interface}));
  plan.commands.push_back(PlannedCommand::make("nft", {"-f", paths.nft_path().string()}));
  if (profile.upstream_interface != "none") {
    plan.commands.push_back(PlannedCommand::make(
        "hotmon-forward", {profile.ap_interface, profile.upstream_interface}));
  }
  if (profile.dhcp_enabled) {
    plan.commands.push_back(PlannedCommand::make(
        "dnsmasq", {std::format("--conf-file={}", paths.dnsmasq_conf().string()),
                    std::format("--pid-file={}", paths.dnsmasq_pid().string()), "--user=nobody",
                    std::format("--dhcp-leasefile={}", paths.dnsmasq_lease().string())}));
  }
  if (existing) {
    plan.commands.push_back(PlannedCommand::make("systemctl", {"restart", "hostapd"}));
  } else {
    plan.commands.push_back(PlannedCommand::make(
        "hostapd",
        {"-B", "-P", paths.hostapd_pid().string(), hostapd_conf.string()}));
  }
  plan.tools = existing ? std::vector<std::string>{"hostapd", "dnsmasq", "nftables"}
                        : std::vector<std::string>{DIRECT_TOOLS[0], DIRECT_TOOLS[1], DIRECT_TOOLS[2]};
  return plan;
}

ApplyPlan backend_plan(BackendKind kind, const Profile& profile, const Paths& paths) {
  switch (kind) {
    case BackendKind::NetworkManager:
      return nm_plan(profile, paths);
    case BackendKind::Iwd:
      return iwd_plan(profile, paths);
    case BackendKind::ExistingHostapd:
      return hostapd_plan(profile, paths, true);
    case BackendKind::DirectHostapd:
      return hostapd_plan(profile, paths, false);
  }
  return hostapd_plan(profile, paths, false);
}

}

const char* label(BackendKind kind) {
  switch (kind) {
    case BackendKind::NetworkManager:
      return "NetworkManager";
    case BackendKind::Iwd:
      return "iwd";
    case BackendKind::ExistingHostapd:
      return "hostapd";
    case BackendKind::DirectHostapd:
      return "hostapd, dnsmasq, and nftables";
  }
  return "hostapd, dnsmasq, and nftables";
}

BackendKind select_backend(const ProbeFacts& facts) {
  if (facts.network_manager_active) {
    return BackendKind::NetworkManager;
  }
  if (facts.iwd_active) {
    return BackendKind::Iwd;
  }
  if (facts.hostapd_setup) {
    return BackendKind::ExistingHostapd;
  }
  return BackendKind::DirectHostapd;
}

bool system_program_installed(std::string_view name) {
  for (const char* dir : {"/usr/local/sbin", "/usr/sbin", "/usr/local/bin", "/usr/bin", "/sbin", "/bin"}) {
    const auto path = std::filesystem::path(dir) / std::string(name);
    if (::access(path.c_str(), X_OK) == 0) {
      return true;
    }
  }
  return false;
}

Result<void> check_backend_tools(BackendKind kind, const Profile& profile,
                                 const std::function<bool(std::string_view)>& installed) {
  if (kind == BackendKind::NetworkManager && profile.dhcp_enabled && !installed("dnsmasq")) {
    return unexpected_text(
        "NetworkManager needs dnsmasq to give addresses to devices. Install the dnsmasq package, "
        "or turn DHCP off in advanced setup.");
  }
  return {};
}

bool service_active(std::string_view unit) {
  const auto result = run_capture({"systemctl", "is-active", "--quiet", std::string(unit)});
  return result && result->status == 0;
}

ProbeFacts probe_system() {
  ProbeFacts facts;
  facts.network_manager_active = service_active("NetworkManager.service");
  facts.iwd_active = facts.network_manager_active ? false : service_active("iwd.service");
  if (!facts.network_manager_active && !facts.iwd_active) {
    facts.hostapd_setup = service_active("hostapd.service") ||
                          std::filesystem::is_regular_file("/etc/hostapd/hostapd.conf");
  }
  return facts;
}

Paths Paths::system() {
  return Paths{"/run/hotmon", "/etc/hostapd/hostapd.conf", "/var/lib/iwd/ap", "/proc",
               "/run/NetworkManager/system-connections", "/etc/NetworkManager/dnsmasq-shared.d"};
}

std::filesystem::path Paths::hostapd_pid() const { return state_dir / "hostapd.pid"; }
std::filesystem::path Paths::dnsmasq_pid() const { return state_dir / "dnsmasq.pid"; }
std::filesystem::path Paths::direct_hostapd_conf() const { return state_dir / "hostapd.conf"; }

std::filesystem::path Paths::dnsmasq_dir() const {
  const auto parent = state_dir.parent_path();
  if (!parent.empty()) {
    return parent / "hotmon-dnsmasq";
  }
  return "/run/hotmon-dnsmasq";
}

std::filesystem::path Paths::dnsmasq_conf() const { return dnsmasq_dir() / "dnsmasq.conf"; }
std::filesystem::path Paths::dnsmasq_lease() const { return dnsmasq_dir() / "leases"; }
std::filesystem::path Paths::nft_path() const { return state_dir / "hotmon.nft"; }
std::filesystem::path Paths::nm_secret() const { return nm_connection_dir / "hotmon.nmconnection"; }
std::filesystem::path Paths::nm_dnsmasq_conf() const { return nm_dnsmasq_dir / "hotmon.conf"; }
std::filesystem::path Paths::hostapd_backup() const { return state_dir / "hostapd.conf.bak"; }
std::filesystem::path Paths::created_marker() const { return state_dir / "hostapd.created"; }
std::filesystem::path Paths::forwarding_record() const { return state_dir / "forwarding.restore"; }
std::filesystem::path Paths::nft_live_marker() const { return state_dir / "nft.live"; }
std::filesystem::path Paths::ufw_record() const { return state_dir / "ufw.holes"; }

void clear_nft_live(const Paths& paths) {
  std::error_code error;
  std::filesystem::remove(paths.nft_live_marker(), error);
}

PlannedCommand PlannedCommand::make(std::string program, std::vector<std::string> args) {
  return PlannedCommand{std::move(program), std::move(args), false, {}};
}

PlannedCommand PlannedCommand::optional_command() const {
  PlannedCommand copy = *this;
  copy.optional = true;
  return copy;
}

PlannedCommand PlannedCommand::failing_on_stderr(std::string marker) const {
  PlannedCommand copy = *this;
  copy.stderr_failure = std::move(marker);
  return copy;
}

Result<ApplyPlan> plan_apply(BackendKind kind, const Profile& profile, const Paths& paths) {
  if (auto checked = profile.check_settings(); !checked) {
    return unexpected_text("The backend rejected the setting. " + checked.error());
  }
  ApplyPlan plan = backend_plan(kind, profile, paths);
  // Keep this last. execute_plan does not remove the ufw rules when a later command fails.
  if (auto network = profile.network()) {
    if (auto gateway = network->gateway()) {
      plan.commands.push_back(PlannedCommand::make("hotmon-ufw", {profile.ap_interface, *gateway}));
    }
  }
  return plan;
}

std::vector<PlannedCommand> plan_stop(BackendKind kind, const Profile& profile) {
  switch (kind) {
    case BackendKind::NetworkManager:
      return {PlannedCommand::make("nmcli", {"connection", "down", "hotmon"}),
              PlannedCommand::make("nmcli", {"connection", "delete", "hotmon"})};
    case BackendKind::Iwd:
      return {PlannedCommand::make("iwctl", {"ap", profile.ap_interface, "stop"})};
    case BackendKind::ExistingHostapd:
      return {PlannedCommand::make("systemctl", {"stop", "hostapd"})};
    case BackendKind::DirectHostapd:
      return {};
  }
  return {};
}

bool stop_target_missing(std::string_view message) {
  const std::string text = ascii_lower(std::string(message));
  return text.find("unknown connection") != std::string::npos ||
         text.find("not found") != std::string::npos || text.find("no such") != std::string::npos ||
         text.find("does not exist") != std::string::npos ||
         text.find("not active") != std::string::npos;
}

Result<void> run_stop_commands(const std::vector<PlannedCommand>& commands, Runner& runner) {
  for (const PlannedCommand& command : commands) {
    if (auto result = runner.run(command); !result) {
      if (command.optional || stop_target_missing(result.error())) {
        continue;
      }
      return unexpected_text("The backend rejected the stop request. " + command.program +
                             " failed: " + result.error());
    }
  }
  return {};
}

}
