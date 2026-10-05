#include "app.hpp"
#include "privilege.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <fstream>

namespace {

using namespace hotmon;

Key key_char(char ch) { return Key{Key::Code::Char, static_cast<char32_t>(static_cast<unsigned char>(ch)), false}; }
Key key_enter() { return Key{Key::Code::Enter, 0, false}; }
Key key_esc() { return Key{Key::Code::Esc, 0, false}; }

App loaded_app(const std::filesystem::path& dir) {
  return App::from_parts(BackendKind::NetworkManager, sample_interfaces(), dir / "profile.json",
                         sample_profile());
}

Paths local_paths(const std::filesystem::path& dir) {
  return Paths{dir / "run", dir / "hostapd.conf", dir / "iwd", dir / "proc"};
}

Result<void> apply_direct(App& app, Runner& runner, ProcessControl& signals, const Paths& paths) {
  DirectPrivilege direct(runner, signals, paths);
  return app.apply_hotspot(direct);
}

Result<void> stop_direct(App& app, Runner& runner, ProcessControl& signals, const Paths& paths) {
  DirectPrivilege direct(runner, signals, paths);
  return app.stop_hotspot(direct);
}

TEST(App, StartLoadsTheSavedProfileIntoTheWizard) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  EXPECT_TRUE(app.loaded_profile);
  EXPECT_EQ(app.wizard.page, Page::Review);
  EXPECT_EQ(app.wizard.ssid, "Hotmon");
  EXPECT_EQ(app.view, View::Wizard);
  EXPECT_FALSE(app.running);
  std::filesystem::remove_all(dir);
}

TEST(App, ConfirmSavesTheProfileAndShowsStatus) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  const auto paths = local_paths(dir);
  ScriptedRunner runner;
  RecordedSignals signals;
  ASSERT_TRUE(apply_direct(app, runner, signals, paths));
  EXPECT_TRUE(app.running);
  EXPECT_EQ(app.view, View::Status);
  EXPECT_EQ(app.status.kind, HotspotStatus::Kind::Running);
  auto saved = load_profile(app.profile_path);
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->ssid, "Hotmon");
  EXPECT_FALSE(runner.calls.empty());
  std::filesystem::remove_all(dir);
}

TEST(App, CancelDoesNotApplyTheProfile) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  EXPECT_EQ(app.on_key(key_esc()), Step::Continue);
  EXPECT_TRUE(app.wizard.is_cancelled());
  ScriptedRunner runner;
  RecordedSignals signals;
  auto error = apply_direct(app, runner, signals, Paths::system());
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("cancelled"), std::string::npos);
  EXPECT_TRUE(runner.calls.empty());
  EXPECT_FALSE(std::filesystem::exists(app.profile_path));
  std::filesystem::remove_all(dir);
}

TEST(App, RejectedBackendKeepsTheHotspotStopped) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  const auto paths = local_paths(dir);
  auto runner = ScriptedRunner::with_results(
      {unexpected_text("missing"), unexpected_text("the channel is not supported")});
  RecordedSignals signals;
  auto error = apply_direct(app, runner, signals, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("The backend rejected the setting."), std::string::npos);
  EXPECT_FALSE(app.running);
  EXPECT_EQ(app.status.kind, HotspotStatus::Kind::Failed);
  EXPECT_FALSE(std::filesystem::exists(app.profile_path));
  std::filesystem::remove_all(dir);
}

TEST(App, SecondCaptureKeyArmsCaptureAndStopClearsIt) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  const auto paths = local_paths(dir);
  ScriptedRunner runner;
  RecordedSignals signals;
  ASSERT_TRUE(apply_direct(app, runner, signals, paths));
  app.view = View::Monitor;
  EXPECT_EQ(app.on_key(key_char('c')), Step::Continue);
  EXPECT_FALSE(app.capture.is_running());
  EXPECT_NE(app.notice.find("Press Enter"), std::string::npos);
  EXPECT_EQ(app.on_key(key_char('c')), Step::Continue);
  EXPECT_FALSE(app.capture.is_running());
  EXPECT_TRUE(app.capture.is_warned());
  EXPECT_EQ(app.on_key(key_enter()), Step::OpenCapture);
  EXPECT_FALSE(app.capture.is_running());
  app.capture_open_failed("permission denied");
  EXPECT_FALSE(app.capture.is_running());
  EXPECT_NE(app.notice.find("did not start"), std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(App, StopHotspotStopsCaptureAndRecordsTheBackendStop) {
  const auto dir = scratch_dir();
  auto app = App::from_parts(BackendKind::DirectHostapd, sample_interfaces(), dir / "profile.json",
                             sample_profile());
  const auto paths = local_paths(dir);
  ScriptedRunner apply_runner;
  RecordedSignals apply_signals;
  ASSERT_TRUE(apply_direct(app, apply_runner, apply_signals, paths));
  std::filesystem::create_directories(paths.state_dir);
  std::ofstream(paths.hostapd_pid()) << "42\n";
  app.started.push_back(StartedProc{42, "hostapd"});
  app.view = View::Status;
  EXPECT_EQ(app.on_key(key_char('k')), Step::StopHotspot);
  ScriptedRunner runner;
  RecordedSignals signals;
  signals.names.push_back({42, "hostapd"});
  ASSERT_TRUE(stop_direct(app, runner, signals, paths));
  EXPECT_FALSE(app.running);
  EXPECT_FALSE(app.capture.is_running());
  EXPECT_EQ(app.status, HotspotStatus::stopped());
  EXPECT_TRUE(std::any_of(runner.calls.begin(), runner.calls.end(),
                          [](const auto& command) { return command.program == "nft"; }));
  EXPECT_EQ(signals.pids, std::vector<int>({42}));
  EXPECT_FALSE(std::filesystem::exists(paths.hostapd_pid()));
  std::filesystem::remove_all(dir);
}

TEST(App, ClientRefreshUpdatesTheMonitor) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  const auto paths = local_paths(dir);
  ScriptedRunner apply_runner;
  RecordedSignals signals;
  ASSERT_TRUE(apply_direct(app, apply_runner, signals, paths));
  const char* dump = "Station aa:bb:cc:dd:ee:ff (on wlan0)\n\trx bytes:\t10\n\ttx bytes:\t5\n";
  auto runner = ScriptedRunner::with_results({std::string(dump), std::string()});
  app.tick(runner);
  ASSERT_EQ(app.monitor.clients().size(), 1u);
  EXPECT_EQ(app.monitor.clients()[0].rx_bytes, 10u);
  EXPECT_EQ(app.monitor.clients()[0].tx_bytes, 5u);
  std::filesystem::remove_all(dir);
}

TEST(App, OpenUpstreamNeedsADifferentConfirmation) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  app.wizard.security.select_value("open");
  app.wizard.passphrase.clear();
  app.backend = BackendKind::DirectHostapd;
  const auto paths = local_paths(dir);
  EXPECT_TRUE(app.needs_open_warning());
  EXPECT_NE(app.wizard.hint().find("radio range"), std::string::npos);
  EXPECT_EQ(app.on_key(key_enter()), Step::Continue);
  EXPECT_FALSE(app.running);
  EXPECT_NE(app.notice.find("radio range"), std::string::npos);
  ScriptedRunner blocked;
  RecordedSignals signals;
  auto error = apply_direct(app, blocked, signals, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("radio range"), std::string::npos);
  EXPECT_FALSE(app.running);
  EXPECT_EQ(app.on_key(key_enter()), Step::Continue);
  EXPECT_EQ(app.on_key(key_char('y')), Step::Apply);
  ScriptedRunner runner;
  ASSERT_TRUE(apply_direct(app, runner, signals, paths));
  EXPECT_TRUE(app.running);
  auto saved = load_profile(app.profile_path);
  ASSERT_TRUE(saved);
  EXPECT_TRUE(saved->passphrase.empty());
  std::filesystem::remove_all(dir);
}

TEST(App, StopReportsFailureWhileDnsmasqIsRunning) {
  const auto dir = scratch_dir();
  auto app = App::from_parts(BackendKind::DirectHostapd, sample_interfaces(), dir / "profile.json",
                             sample_profile());
  const auto paths = local_paths(dir);
  ScriptedRunner apply_runner;
  RecordedSignals apply_signals;
  ASSERT_TRUE(apply_direct(app, apply_runner, apply_signals, paths));
  app.started.push_back(StartedProc{8, "dnsmasq"});
  RecordedSignals signals;
  signals.names.push_back({8, "dnsmasq"});
  signals.stay_alive = true;
  ScriptedRunner runner;
  auto error = stop_direct(app, runner, signals, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("not stopped"), std::string::npos);
  EXPECT_TRUE(app.running);
  EXPECT_NE(app.status, HotspotStatus::stopped());
  EXPECT_EQ(app.notice.find("The hotspot is stopped."), std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(App, FailedStartKeepsTheOldProfile) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  ASSERT_TRUE(save_profile(app.profile_path, sample_profile()));
  app.wizard.passphrase = "another-secret-value";
  const auto paths = local_paths(dir);
  auto runner = ScriptedRunner::with_results({unexpected_text("missing"), unexpected_text("refused")});
  RecordedSignals signals;
  auto error = apply_direct(app, runner, signals, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("The backend rejected the setting."), std::string::npos);
  EXPECT_FALSE(app.running);
  std::ifstream input(app.profile_path);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("correct-horse"), std::string::npos);
  EXPECT_EQ(text.find("another-secret-value"), std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(App, StopRemovesTheManagerProfileItCreated) {
  const auto dir = scratch_dir();
  const auto paths = local_paths(dir);
  auto app = App::from_parts(BackendKind::NetworkManager, sample_interfaces(), dir / "profile.json",
                             sample_profile());
  ScriptedRunner apply_runner;
  RecordedSignals apply_signals;
  ASSERT_TRUE(apply_direct(app, apply_runner, apply_signals, paths));
  RecordedSignals signals;
  signals.names.push_back({42, "hostapd"});
  app.started.push_back(StartedProc{42, "hostapd"});
  ScriptedRunner runner;
  ASSERT_TRUE(stop_direct(app, runner, signals, paths));
  EXPECT_TRUE(signals.pids.empty());
  EXPECT_TRUE(std::any_of(runner.calls.begin(), runner.calls.end(), [](const auto& command) {
    return command.program == "nmcli" &&
           std::find(command.args.begin(), command.args.end(), "delete") != command.args.end();
  }));
  const auto profile = sample_profile();
  ASSERT_TRUE(save_profile(app.profile_path, profile));
  auto iwd = App::from_parts(BackendKind::Iwd, sample_interfaces(), dir / "other.json", profile);
  const auto iwd_path = paths.iwd_ap_dir / (profile.ssid + ".ap");
  std::filesystem::create_directories(paths.iwd_ap_dir);
  std::ofstream(iwd_path) << "Passphrase=correct-horse\n";
  ScriptedRunner iwd_runner;
  RecordedSignals iwd_signals;
  ASSERT_TRUE(apply_direct(iwd, iwd_runner, iwd_signals, paths));
  ScriptedRunner stop_runner;
  ASSERT_TRUE(stop_direct(iwd, stop_runner, iwd_signals, paths));
  EXPECT_FALSE(std::filesystem::exists(iwd_path));
  EXPECT_EQ(iwd.status, HotspotStatus::stopped());
  std::filesystem::remove_all(dir);
}

TEST(App, EnterDoesNotStartCaptureAfterTheWarningLeaves) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  const auto paths = test_paths(dir);
  ScriptedRunner apply_runner;
  RecordedSignals signals;
  ASSERT_TRUE(apply_direct(app, apply_runner, signals, paths));
  app.view = View::Monitor;
  EXPECT_EQ(app.on_key(key_char('c')), Step::Continue);
  EXPECT_TRUE(app.capture.is_warned());
  auto runner = ScriptedRunner::with_results({unexpected_text("station dump failed")});
  app.tick(runner);
  EXPECT_EQ(app.notice.find("Press Enter"), std::string::npos);
  EXPECT_FALSE(app.capture.is_warned());
  EXPECT_EQ(app.on_key(key_enter()), Step::Continue);
  EXPECT_FALSE(app.capture.is_running());
  std::filesystem::remove_all(dir);
}

TEST(App, NewInterfaceShowsTheCaptureWarningAgain) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  IfaceInfo wlan2;
  wlan2.name = "wlan2";
  wlan2.wireless = true;
  wlan2.supports_ap = true;
  wlan2.supports_5ghz = true;
  wlan2.channels_5 = {36};
  app.interfaces.push_back(wlan2);
  app.facts.interfaces = app.interfaces;
  const auto paths = test_paths(dir);
  ScriptedRunner apply_runner;
  RecordedSignals signals;
  ASSERT_TRUE(apply_direct(app, apply_runner, signals, paths));
  app.view = View::Monitor;
  app.on_key(key_char('c'));
  EXPECT_EQ(app.on_key(key_enter()), Step::OpenCapture);
  app.wizard.ap_interface.add(Choice{"wlan2", "wlan2"});
  app.wizard.ap_interface.select_value("wlan2");
  ScriptedRunner second;
  ASSERT_TRUE(apply_direct(app, second, signals, paths));
  EXPECT_TRUE(app.capture.is_warned());
  EXPECT_FALSE(app.capture.is_running());
  EXPECT_NE(app.notice.find("Press Enter"), std::string::npos);
  ASSERT_TRUE(app.capture.bound_interface());
  EXPECT_EQ(*app.capture.bound_interface(), "wlan2");
  std::filesystem::remove_all(dir);
}

TEST(App, PrivateHostapdStopDoesNotStopTheSystemService) {
  const auto dir = scratch_dir();
  const auto paths = test_paths(dir);
  std::filesystem::create_directories(paths.hostapd_config.parent_path());
  std::ofstream(paths.hostapd_config) << "original-config\n";
  std::filesystem::create_directories(paths.state_dir);
  std::filesystem::create_directories(paths.hostapd_backup());
  auto app = App::from_parts(BackendKind::ExistingHostapd, sample_interfaces(), dir / "profile.json",
                             sample_profile());
  ScriptedRunner apply_runner;
  RecordedSignals apply_signals;
  ASSERT_TRUE(apply_direct(app, apply_runner, apply_signals, paths));
  EXPECT_TRUE(app.private_hostapd);
  std::ifstream kept(paths.hostapd_config);
  std::string kept_text((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
  EXPECT_EQ(kept_text, "original-config\n");
  ScriptedRunner runner;
  RecordedSignals signals;
  ASSERT_TRUE(stop_direct(app, runner, signals, paths));
  EXPECT_FALSE(std::any_of(runner.calls.begin(), runner.calls.end(),
                           [](const auto& command) { return command.program == "systemctl"; }));
  EXPECT_EQ(app.status, HotspotStatus::stopped());
  std::filesystem::remove_all(dir);
}

TEST(App, FailedReapplyKeepsThePrivateHostapdStopPath) {
  const auto dir = scratch_dir();
  const auto paths = test_paths(dir);
  std::filesystem::create_directories(paths.hostapd_config.parent_path());
  std::ofstream(paths.hostapd_config) << "original-config\n";
  std::filesystem::create_directories(paths.state_dir);
  std::filesystem::create_directories(paths.hostapd_backup());
  auto app = App::from_parts(BackendKind::ExistingHostapd, sample_interfaces(), dir / "profile.json",
                             sample_profile());
  ScriptedRunner apply_runner;
  RecordedSignals apply_signals;
  ASSERT_TRUE(apply_direct(app, apply_runner, apply_signals, paths));
  app.started.push_back(StartedProc{55, "hostapd"});
  auto runner = ScriptedRunner::with_results({unexpected_text("link failed")});
  RecordedSignals signals;
  auto error = apply_direct(app, runner, signals, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("The backend rejected the setting."), std::string::npos);
  EXPECT_TRUE(app.running);
  EXPECT_TRUE(app.private_hostapd);
  EXPECT_TRUE(std::any_of(app.started.begin(), app.started.end(),
                          [](const auto& item) { return item.pid == 55; }));
  ScriptedRunner stop_runner;
  RecordedSignals stop_signals;
  stop_signals.names.push_back({55, "hostapd"});
  ASSERT_TRUE(stop_direct(app, stop_runner, stop_signals, paths));
  EXPECT_FALSE(std::any_of(stop_runner.calls.begin(), stop_runner.calls.end(),
                           [](const auto& command) { return command.program == "systemctl"; }));
  EXPECT_EQ(stop_signals.pids, std::vector<int>({55}));
  EXPECT_EQ(app.status, HotspotStatus::stopped());
  std::filesystem::remove_all(dir);
}

class CancelPrivileged : public Privileged {
 public:
  Result<StartReport> apply(BackendKind, const Profile&) override {
    return unexpected_text("Authorization was cancelled. The action was not done.");
  }
  Result<void> stop(const StopRequest&) override {
    return unexpected_text("Authorization was cancelled. The action was not done.");
  }
  Result<FileDescriptor> open_capture_socket(std::string_view) override {
    return unexpected_text("Authorization was cancelled. The action was not done.");
  }
};

TEST(App, CancelledStopKeepsTheHotspotRunning) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  const auto paths = local_paths(dir);
  ScriptedRunner runner;
  RecordedSignals signals;
  ASSERT_TRUE(apply_direct(app, runner, signals, paths));
  EXPECT_EQ(app.status.kind, HotspotStatus::Kind::Running);
  CancelPrivileged cancelled;
  auto error = app.stop_hotspot(cancelled);
  ASSERT_FALSE(error);
  EXPECT_TRUE(app.running);
  EXPECT_EQ(app.status.kind, HotspotStatus::Kind::Running);
  EXPECT_NE(app.notice.find("cancelled"), std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(App, CancelledApplyDoesNotSaveTheProfile) {
  const auto dir = scratch_dir();
  auto app = loaded_app(dir);
  CancelPrivileged cancelled;
  auto error = app.apply_hotspot(cancelled);
  ASSERT_FALSE(error);
  EXPECT_FALSE(app.running);
  EXPECT_EQ(app.status.kind, HotspotStatus::Kind::Failed);
  ASSERT_TRUE(app.wizard.error);
  EXPECT_NE(app.wizard.error->find("cancelled"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(app.profile_path));
  std::filesystem::remove_all(dir);
}

TEST(App, CancelledCaptureDoesNotStart) {
  App app = App::from_parts(BackendKind::DirectHostapd, sample_interfaces(), "unused.json",
                            sample_profile());
  app.capture_open_failed("Authorization was cancelled. The action was not done.");
  EXPECT_NE(app.capture.phase(), CapturePhase::Running);
  EXPECT_NE(app.notice.find("cancelled"), std::string::npos);
}

}
