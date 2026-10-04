#include <format>
#include "backend.hpp"

#include "backend_text.hpp"
#include "process.hpp"

#include <cctype>

namespace hotmon {
namespace {

std::string ascii_lower(std::string text) {
  for (char& ch : text) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return text;
}

PlanFile plain_file(std::filesystem::path path, std::string contents, uint32_t mode) {
  PlanFile file;
  file.path = std::move(path);
  file.contents = std::move(contents);
  file.mode = mode;
  file.remove_on_failure = mode == 0600;
  return file;
}

PlanFile temporary_file(PlanFile file) {
  file.temporary = true;
  file.remove_on_failure = true;
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
  const auto secret = paths.nm_secret();
  ApplyPlan plan;
  plan.files.push_back(temporary_file(plain_file(secret, nm_keyfile(profile), 0600)));
  plan.commands.push_back(
      PlannedCommand::make("nmcli", {"connection", "delete", "hotmon"}).optional_command());
  plan.commands.push_back(
      PlannedCommand::make("nmcli", {"connection", "load", secret.string()}));
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

}  // namespace

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
  return Paths{"/run/hotmon", "/etc/hostapd/hostapd.conf", "/var/lib/iwd/ap", "/proc"};
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
std::filesystem::path Paths::nm_secret() const { return state_dir / "hotmon.nmconnection"; }
std::filesystem::path Paths::hostapd_backup() const { return state_dir / "hostapd.conf.bak"; }
std::filesystem::path Paths::created_marker() const { return state_dir / "hostapd.created"; }
std::filesystem::path Paths::forwarding_record() const { return state_dir / "forwarding.restore"; }
std::filesystem::path Paths::nft_live_marker() const { return state_dir / "nft.live"; }

void clear_nft_live(const Paths& paths) {
  std::error_code error;
  std::filesystem::remove(paths.nft_live_marker(), error);
}

PlannedCommand PlannedCommand::make(std::string program, std::vector<std::string> args) {
  return PlannedCommand{std::move(program), std::move(args), false};
}

PlannedCommand PlannedCommand::optional_command() const {
  PlannedCommand copy = *this;
  copy.optional = true;
  return copy;
}

Result<ApplyPlan> plan_apply(BackendKind kind, const Profile& profile, const Paths& paths) {
  if (auto checked = profile.check_settings(); !checked) {
    return unexpected_text("The backend rejected the setting. " + checked.error());
  }
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

}  // namespace hotmon
