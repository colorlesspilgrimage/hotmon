#include "test_support.hpp"

#include <algorithm>
#include <iterator>

#include <atomic>
#include <fstream>
#include <unistd.h>

namespace hotmon {

Profile sample_profile() {
  Profile profile;
  profile.ap_interface = "wlan0";
  profile.ssid = "Hotmon";
  profile.security = SecurityMode::Wpa2;
  profile.passphrase = "correct-horse";
  profile.band = Band::Band24;
  profile.channel = 6;
  profile.address_cidr = "192.168.42.0/24";
  profile.dhcp_enabled = true;
  profile.dhcp_start = "192.168.42.10";
  profile.dhcp_end = "192.168.42.100";
  profile.upstream_interface = "eth0";
  return profile;
}

std::vector<IfaceInfo> sample_interfaces() {
  IfaceInfo eth0;
  eth0.name = "eth0";
  IfaceInfo wlan0;
  wlan0.name = "wlan0";
  wlan0.wireless = true;
  wlan0.supports_ap = true;
  wlan0.supports_5ghz = true;
  wlan0.channels_24 = {1, 6, 11};
  wlan0.channels_5 = {36, 40, 44};
  IfaceInfo wlan1;
  wlan1.name = "wlan1";
  wlan1.wireless = true;
  wlan1.channels_24 = {1, 6, 11};
  return {eth0, wlan0, wlan1};
}

std::filesystem::path scratch_dir() {
  static std::atomic<uint64_t> number{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("hotmon-" + std::to_string(::getpid()) + "-" +
                    std::to_string(number.fetch_add(1)));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

Paths test_paths(const std::filesystem::path& dir) {
  return Paths{dir / "run", dir / "etc" / "hostapd.conf", dir / "iwd", dir / "proc"};
}

ScriptedRunner ScriptedRunner::with_results(std::vector<Result<std::string>> values) {
  ScriptedRunner runner;
  runner.results = std::move(values);
  return runner;
}

Result<std::string> ScriptedRunner::run(const PlannedCommand& command) {
  calls.push_back(command);
  if (index >= results.size()) {
    return std::string();
  }
  return results[index++];
}

std::optional<std::string> RecordedSignals::describe(int pid) {
  for (const auto& [id, name] : names) {
    if (id == pid) {
      return name;
    }
  }
  return std::nullopt;
}

bool RecordedSignals::running(int pid) {
  const auto name = describe(pid);
  if (!name || (*name != "hostapd" && *name != "dnsmasq")) {
    return false;
  }
  if (std::find(pids.begin(), pids.end(), pid) != pids.end() && !fail && !stay_alive) {
    return false;
  }
  return true;
}

Result<void> RecordedSignals::terminate(int pid) {
  if (pid <= 0) {
    return unexpected_text("The process id " + std::to_string(pid) + " is not valid.");
  }
  const auto name = describe(pid);
  if (!name || (*name != "hostapd" && *name != "dnsmasq")) {
    return unexpected_text("The process " + std::to_string(pid) + " is not hostapd or dnsmasq.");
  }
  pids.push_back(pid);
  if (fail) {
    return unexpected_text("The process did not stop.");
  }
  return {};
}

Result<std::string> PidOnSuccess::run(const PlannedCommand& command) {
  auto result = inner.run(command);
  if (command.program == "dnsmasq" && result) {
    if (pid_path.has_parent_path()) {
      std::filesystem::create_directories(pid_path.parent_path());
    }
    std::ofstream output(pid_path);
    output << pid << "\n";
  }
  return result;
}

Result<std::string> ModeCheck::run(const PlannedCommand& command) {
  if (std::find(command.args.begin(), command.args.end(), "load") != command.args.end()) {
    const auto& path = command.args.back();
    const auto mode = std::filesystem::status(path).permissions();
    const auto bits = static_cast<unsigned>(mode) & 0777;
    if (bits != 0600) {
      return unexpected_text("secret mode is not 0600");
    }
    std::ifstream input(path);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.find("psk=correct-horse") == std::string::npos) {
      return unexpected_text("secret text is missing");
    }
    for (const auto& arg : command.args) {
      if (arg.find("correct-horse") != std::string::npos) {
        return unexpected_text("secret is in an argument");
      }
    }
    saw_private_secret = true;
  }
  return std::string();
}

Result<std::optional<std::vector<uint8_t>>> FakeSource::try_recv() {
  ++reads;
  if (frames.empty()) {
    return std::optional<std::vector<uint8_t>>();
  }
  auto frame = std::move(frames.front());
  frames.erase(frames.begin());
  return std::optional<std::vector<uint8_t>>(std::move(frame));
}

}  // namespace hotmon
