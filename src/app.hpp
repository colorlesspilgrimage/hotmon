#pragma once

#include "backend.hpp"
#include "privilege.hpp"
#include "capture.hpp"
#include "iface.hpp"
#include "monitor.hpp"
#include "profile.hpp"
#include "wizard.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace hotmon {

enum class View { Wizard, Status, Monitor };

struct HotspotStatus {
  enum class Kind { Stopped, Running, Failed };
  Kind kind = Kind::Stopped;
  std::string ssid;
  std::string interface;
  std::string backend;
  std::string message;

  static HotspotStatus stopped();
  static HotspotStatus running(std::string ssid, std::string interface, std::string backend);
  static HotspotStatus failed(std::string message);
  bool operator==(const HotspotStatus&) const = default;
};

enum class Step { Continue, Quit, Apply, StopHotspot, OpenCapture };

struct Key {
  enum class Code { Char, Enter, Esc, Left, Right, Up, Down, Tab, Backspace, Other };
  Code code = Code::Other;
  char32_t ch = 0;
  bool ctrl = false;
};

struct App {
  View view = View::Wizard;
  Wizard wizard;
  HotspotStatus status = HotspotStatus::stopped();
  MonitorState monitor;
  BackendKind backend = BackendKind::DirectHostapd;
  CaptureControl capture;
  std::vector<IfaceInfo> interfaces;
  HostFacts facts;
  std::string notice;
  std::filesystem::path profile_path;
  bool running = false;
  std::optional<Profile> active;
  bool loaded_profile = false;
  std::vector<StartedProc> started;
  bool private_hostapd = false;
  bool open_extra = false;
  bool open_upstream_ok = false;

  static Result<App> boot();
  static App from_parts(BackendKind backend, std::vector<IfaceInfo> interfaces,
                        std::filesystem::path profile_path, std::optional<Profile> loaded);
  bool needs_open_warning() const;
  Step on_key(const Key& key);
  Result<void> apply_hotspot(Privileged& privileged);
  Result<void> stop_hotspot(Privileged& privileged);
  Result<void> refresh_clients(Runner& runner);
  void tick(Runner& runner);
  void capture_open_failed(std::string message);
  Result<std::string> hotspot_iface() const;

 private:
  void replace_notice(std::string message);
  void finish_stop();
  void fail_apply(std::string message);
  Step on_wizard_key(const Key& key);
  Step review_enter();
  Step review_confirm_key();
  Step on_run_key(const Key& key, bool status_view);
  Step request_capture();
  Step accept_capture();
};

}
