#include "app.hpp"

#include "netauto.hpp"

namespace hotmon {

HotspotStatus HotspotStatus::stopped() { return {}; }

HotspotStatus HotspotStatus::running(std::string ssid, std::string interface, std::string backend) {
  HotspotStatus status;
  status.kind = Kind::Running;
  status.ssid = std::move(ssid);
  status.interface = std::move(interface);
  status.backend = std::move(backend);
  return status;
}

HotspotStatus HotspotStatus::failed(std::string message) {
  HotspotStatus status;
  status.kind = Kind::Failed;
  status.message = std::move(message);
  return status;
}

Result<App> App::boot() {
  const BackendKind backend = select_backend(probe_system());
  auto interfaces = read_system_interfaces();
  const auto profile_path = default_profile_path();
  auto loaded = load_optional(profile_path);
  if (!loaded) {
    return unexpected_text(loaded.error());
  }
  return from_parts(backend, std::move(interfaces), profile_path, std::move(*loaded));
}

App App::from_parts(BackendKind backend, std::vector<IfaceInfo> interfaces,
                    std::filesystem::path profile_path, std::optional<Profile> loaded) {
  App app;
  app.backend = backend;
  app.interfaces = std::move(interfaces);
  app.facts.interfaces = app.interfaces;
  app.facts.local_networks = [](std::string_view ap) { return read_local_networks(ap); };
  app.profile_path = std::move(profile_path);
  app.loaded_profile = loaded.has_value();
  app.wizard = loaded ? Wizard::from_profile(*loaded, app.facts) : Wizard::make(app.facts);
  app.notice = app.loaded_profile ? "The saved profile is loaded." : "";
  return app;
}

bool App::needs_open_warning() const { return wizard.open_upstream_risk(); }

Step App::on_key(const Key& key) {
  if (key.ctrl && key.code == Key::Code::Char && (key.ch == U'q' || key.ch == U'Q')) {
    return Step::Quit;
  }
  switch (view) {
    case View::Wizard:
      return on_wizard_key(key);
    case View::Status:
      return on_run_key(key, true);
    case View::Monitor:
      return on_run_key(key, false);
  }
  return Step::Continue;
}

Result<void> App::apply_hotspot(Privileged& privileged) {
  if (wizard.is_cancelled()) {
    notice = "The wizard is cancelled. The settings were not applied.";
    return unexpected_text(notice);
  }
  auto profile = wizard.confirmed_profile(facts);
  if (!profile) {
    notice = profile.error();
    return unexpected_text(profile.error());
  }
  if (profile->security == SecurityMode::Open && profile->upstream_interface != "none" &&
      !open_upstream_ok) {
    notice = std::string(OPEN_UPSTREAM_WARNING) +
             " Confirm this warning before you apply the profile.";
    open_upstream_ok = false;
    return unexpected_text(notice);
  }
  if (auto tools = check_backend_tools(backend, *profile, program_installed); !tools) {
    wizard.set_error(tools.error());
    notice = tools.error();
    return unexpected_text(tools.error());
  }
  const bool fakemii_was_on = fakemii.running();
  // The gateway can change. Stop FakeMii before the new settings apply.
  fakemii.stop();
  auto report = privileged.apply(backend, *profile);
  if (!report) {
    if (running) {
      wizard.set_error(report.error());
      notice = report.error();
    } else {
      started.clear();
      private_hostapd = false;
      fail_apply(report.error());
    }
    return unexpected_text(report.error());
  }
  started = std::move(report->started);
  private_hostapd = report->private_hostapd;
  status = HotspotStatus::running(profile->ssid, profile->ap_interface, label(backend));
  const bool interface_changed =
      capture.bound_interface().has_value() && *capture.bound_interface() != profile->ap_interface;
  active = *profile;
  running = true;
  view = View::Status;
  if (interface_changed) {
    capture.stop();
    (void)capture.warn(profile->ap_interface);
  }
  if (auto saved = save_profile(profile_path, *profile); !saved) {
    notice = saved.error();
    return unexpected_text(saved.error());
  }
  if (interface_changed) {
    notice = CAPTURE_WARNING;
  } else if (fakemii_was_on) {
    notice = "The hotspot is active. FakeMii is off. Press f to turn it on again.";
  } else {
    notice = "The hotspot is active.";
  }
  return {};
}

Result<void> App::stop_hotspot(Privileged& privileged) {
  if (!active) {
    capture.stop();
    finish_stop();
    return {};
  }
  const Profile profile = *active;
  auto stopped = privileged.stop(StopRequest{backend, profile, private_hostapd, started});
  if (!stopped) {
    notice = stopped.error();
    return unexpected_text(stopped.error());
  }
  capture.stop();
  started.clear();
  private_hostapd = false;
  finish_stop();
  return {};
}

Result<void> App::refresh_clients(Runner& runner) {
  if (!running || !active) {
    return {};
  }
  const std::string iface = active->ap_interface;
  auto dump = runner.run(PlannedCommand::make("iw", {"dev", iface, "station", "dump"}));
  if (!dump) {
    return unexpected_text("The client list is not available. " + dump.error());
  }
  auto neigh = runner.run(PlannedCommand::make("ip", {"neigh", "show", "dev", iface}));
  const std::string neigh_text = neigh ? *neigh : "";
  monitor.update(clients_from_text(*dump, neigh_text));
  return {};
}

void App::tick(Runner& runner) {
  fakemii.poll();
  if (running) {
    if (auto refreshed = refresh_clients(runner); !refreshed) {
      replace_notice(refreshed.error());
    }
  }
  if (auto polled = capture.poll(); !polled) {
    replace_notice(polled.error());
  }
}

void App::replace_notice(std::string message) {
  if (capture.is_warned() && message != CAPTURE_WARNING) {
    capture.dismiss_warning();
  }
  notice = std::move(message);
}

void App::capture_open_failed(std::string message) { notice = capture.fail_open(std::move(message)); }

void App::finish_stop() {
  fakemii.stop();
  running = false;
  monitor.clear();
  status = HotspotStatus::stopped();
  notice = "The hotspot is stopped.";
}

void App::fail_apply(std::string message) {
  fakemii.stop();
  wizard.set_error(message);
  status = HotspotStatus::failed(message);
  notice = std::move(message);
  running = false;
}

Step App::on_wizard_key(const Key& key) {
  if (key.code == Key::Code::Enter) {
    if (wizard.page == Page::Review) {
      return review_enter();
    }
    open_extra = false;
    open_upstream_ok = false;
    if (auto advanced = wizard.next(facts); !advanced) {
      notice = advanced.error();
    } else {
      notice.clear();
    }
    return Step::Continue;
  }
  if (key.code == Key::Code::Esc) {
    open_extra = false;
    open_upstream_ok = false;
    wizard.cancel();
    view = View::Status;
    notice = "The wizard is cancelled. The settings were not applied.";
    return Step::Continue;
  }
  if (key.code == Key::Code::Left) {
    open_extra = false;
    open_upstream_ok = false;
    wizard.back();
    return Step::Continue;
  }
  if (key.code == Key::Code::Up) {
    wizard.move_up(facts);
    return Step::Continue;
  }
  if (key.code == Key::Code::Down) {
    wizard.move_down(facts);
    return Step::Continue;
  }
  if (key.code == Key::Code::Backspace) {
    wizard.backspace();
    return Step::Continue;
  }
  if (key.code == Key::Code::Tab) {
    wizard.next_field();
    return Step::Continue;
  }
  if (key.code == Key::Code::Char && key.ch == U'y' && wizard.page == Page::Review) {
    return review_confirm_key();
  }
  if (key.code == Key::Code::Char && wizard.page == Page::AddressDhcp && key.ch == U'a') {
    (void)wizard.open_advanced(facts);
    return Step::Continue;
  }
  if (key.code == Key::Code::Char && wizard.page == Page::AddressDhcp && key.ch == U'd') {
    if (auto reset = wizard.use_automatic(facts); !reset) {
      notice = reset.error();
    }
    return Step::Continue;
  }
  if (key.code == Key::Code::Char) {
    wizard.push_char(key.ch);
  }
  return Step::Continue;
}

Step App::review_enter() {
  if (needs_open_warning()) {
    open_extra = true;
    open_upstream_ok = false;
    notice = std::string(OPEN_UPSTREAM_WARNING) + " Press y to apply this open hotspot.";
    return Step::Continue;
  }
  return Step::Apply;
}

Step App::review_confirm_key() {
  if (needs_open_warning() && open_extra) {
    open_upstream_ok = true;
    return Step::Apply;
  }
  if (needs_open_warning()) {
    notice = std::string(OPEN_UPSTREAM_WARNING) + " Press Enter to read this warning, then press y.";
    return Step::Continue;
  }
  return Step::Continue;
}

Step App::on_run_key(const Key& key, bool status_view) {
  if (key.code == Key::Code::Char && key.ch == U'q') {
    return Step::Quit;
  }
  if (key.code == Key::Code::Char && key.ch == U'c') {
    return request_capture();
  }
  if (key.code == Key::Code::Enter) {
    return accept_capture();
  }
  capture.dismiss_warning();
  if (key.code == Key::Code::Char && key.ch == U'f' && status_view) {
    return toggle_fakemii();
  }
  if (key.code == Key::Code::Char && key.ch == U'm' && status_view) {
    view = View::Monitor;
    return Step::Continue;
  }
  if (key.code == Key::Code::Char && key.ch == U's' && !status_view) {
    view = View::Status;
    return Step::Continue;
  }
  if (key.code == Key::Code::Esc && !status_view) {
    view = View::Status;
    return Step::Continue;
  }
  if (key.code == Key::Code::Char && key.ch == U'w' && status_view) {
    wizard.reopen();
    view = View::Wizard;
    return Step::Continue;
  }
  if (key.code == Key::Code::Char && key.ch == U'k') {
    return Step::StopHotspot;
  }
  if (key.code == Key::Code::Char && key.ch == U'z') {
    capture.stop();
    notice = "Capture is stopped.";
    return Step::Continue;
  }
  return Step::Continue;
}

Step App::toggle_fakemii() {
  if (fakemii.running()) {
    fakemii.stop();
    notice = "FakeMii is off.";
    return Step::Continue;
  }
  if (!running || !active) {
    notice = "The hotspot is not active. Start the hotspot to use FakeMii.";
    return Step::Continue;
  }
  auto network = active->network();
  if (!network) {
    notice = "FakeMii needs a gateway address. " + network.error();
    return Step::Continue;
  }
  auto gateway = network->gateway();
  if (!gateway) {
    notice = "FakeMii needs a gateway address. " + gateway.error();
    return Step::Continue;
  }
  if (auto started = fakemii.start(*gateway, fakemii_port); !started) {
    notice = started.error();
    return Step::Continue;
  }
  notice = "FakeMii is on. Proxy: " + fakemii.address() + ":" + std::to_string(fakemii.port()) + ".";
  return Step::Continue;
}

Step App::request_capture() {
  auto iface = hotspot_iface();
  if (!iface) {
    notice = iface.error();
    return Step::Continue;
  }
  auto result = capture.warn(*iface);
  if (!result) {
    notice = result.error();
    return Step::Continue;
  }
  if (result->kind == ConfirmKind::Warning) {
    notice = CAPTURE_WARNING;
  } else if (result->kind == ConfirmKind::Already) {
    notice = "Capture is already active.";
  }
  return Step::Continue;
}

Step App::accept_capture() {
  if (notice != CAPTURE_WARNING) {
    capture.dismiss_warning();
    return Step::Continue;
  }
  auto iface = hotspot_iface();
  if (!iface) {
    return Step::Continue;
  }
  auto result = capture.accept(*iface);
  if (!result) {
    notice = result.error();
    return Step::Continue;
  }
  if (result->kind == ConfirmKind::Open) {
    return Step::OpenCapture;
  }
  if (result->kind == ConfirmKind::Warning) {
    notice = CAPTURE_WARNING;
  }
  return Step::Continue;
}

Result<std::string> App::hotspot_iface() const {
  if (!running) {
    return unexpected_text("The hotspot is not active.");
  }
  if (!active) {
    return unexpected_text("The hotspot interface is not set.");
  }
  return active->ap_interface;
}

}
