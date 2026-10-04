#include "backend.hpp"
#include "backend_exec.hpp"
#include "backend_text.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <fstream>

namespace {

using namespace hotmon;

ProbeFacts facts(bool nm, bool iwd, bool hostapd) {
  return ProbeFacts{nm, iwd, hostapd};
}

unsigned mode_bits(const std::filesystem::path& path) {
  return static_cast<unsigned>(std::filesystem::status(path).permissions()) & 0777;
}

void assert_no_secret_args(const ApplyPlan& plan, const std::string& secret) {
  for (const auto& command : plan.commands) {
    EXPECT_EQ(command.program.find(secret), std::string::npos);
    for (const auto& arg : command.args) {
      EXPECT_EQ(arg.find(secret), std::string::npos) << command.program << " " << arg;
    }
  }
}

Paths run_paths(const std::filesystem::path& dir) {
  return Paths{dir / "run", dir / "hostapd.conf", dir / "iwd", dir / "proc"};
}

TEST(Backend, SelectionFollowsTheServiceOrder) {
  EXPECT_EQ(select_backend(facts(true, true, true)), BackendKind::NetworkManager);
  EXPECT_EQ(select_backend(facts(false, true, true)), BackendKind::Iwd);
  EXPECT_EQ(select_backend(facts(false, false, true)), BackendKind::ExistingHostapd);
  EXPECT_EQ(select_backend(facts(false, false, false)), BackendKind::DirectHostapd);
  EXPECT_STREQ(DIRECT_TOOLS[0], "hostapd");
  EXPECT_STREQ(DIRECT_TOOLS[1], "dnsmasq");
  EXPECT_STREQ(DIRECT_TOOLS[2], "nftables");
}

TEST(Backend, DirectPlanUsesHostapdDnsmasqAndNftables) {
  const auto dir = scratch_dir();
  const auto paths = run_paths(dir);
  auto plan = plan_apply(BackendKind::DirectHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->tools, (std::vector<std::string>{"hostapd", "dnsmasq", "nftables"}));
  EXPECT_TRUE(std::any_of(plan->commands.begin(), plan->commands.end(),
                          [](const auto& command) { return command.program == "hostapd"; }));
  EXPECT_TRUE(std::any_of(plan->commands.begin(), plan->commands.end(),
                          [](const auto& command) { return command.program == "dnsmasq"; }));
  EXPECT_TRUE(std::any_of(plan->commands.begin(), plan->commands.end(),
                          [](const auto& command) { return command.program == "nft"; }));
  const auto hostapd = std::find_if(plan->files.begin(), plan->files.end(), [](const auto& file) {
    return file.path.filename() == "hostapd.conf";
  });
  ASSERT_NE(hostapd, plan->files.end());
  EXPECT_NE(hostapd->contents.find("ssid=Hotmon"), std::string::npos);
  EXPECT_NE(hostapd->contents.find("wpa_passphrase=correct-horse"), std::string::npos);
  std::string masq;
  for (const auto& file : plan->files) {
    for (size_t start = 0; start < file.contents.size();) {
      const size_t end = file.contents.find('\n', start);
      const auto line = file.contents.substr(start, end == std::string::npos ? end : end - start);
      if (line.find("masquerade") != std::string::npos) {
        masq = line;
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
  }
  EXPECT_NE(masq.find("wlan0"), std::string::npos);
  EXPECT_NE(masq.find("eth0"), std::string::npos);
  std::vector<std::string> names;
  for (const auto& command : plan->commands) {
    names.push_back(command.program);
  }
  const auto nft = std::find(names.begin(), names.end(), "nft") - names.begin();
  const auto forward = std::find(names.begin(), names.end(), "hotmon-forward") - names.begin();
  const auto dnsmasq = std::find(names.begin(), names.end(), "dnsmasq") - names.begin();
  const auto hostapd_at = std::find(names.begin(), names.end(), "hostapd") - names.begin();
  EXPECT_LT(nft, forward);
  EXPECT_LT(forward, dnsmasq);
  EXPECT_LT(dnsmasq, hostapd_at);
  const auto dnsmasq_command = std::find_if(plan->commands.begin(), plan->commands.end(),
                                            [](const auto& command) { return command.program == "dnsmasq"; });
  ASSERT_NE(dnsmasq_command, plan->commands.end());
  EXPECT_NE(std::find(dnsmasq_command->args.begin(), dnsmasq_command->args.end(), "--user=nobody"),
            dnsmasq_command->args.end());
  EXPECT_EQ(std::find(dnsmasq_command->args.begin(), dnsmasq_command->args.end(), "--user=root"),
            dnsmasq_command->args.end());
  std::filesystem::remove_all(dir);
}

TEST(Backend, OpenHostapdConfigHasNoPassphrase) {
  auto profile = sample_profile();
  profile.security = SecurityMode::Open;
  profile.passphrase.clear();
  profile.dhcp_enabled = false;
  const auto text = hostapd_conf_text(profile);
  EXPECT_EQ(text.find("wpa_passphrase"), std::string::npos);
  profile.upstream_interface = "none";
  EXPECT_NE(nft_text(profile).find("drop"), std::string::npos);
}

TEST(Backend, NetworkManagerPlanUsesNmcliOnly) {
  const auto dir = scratch_dir();
  Paths paths{dir, dir / "h.conf", dir / "iwd", dir / "proc"};
  auto plan = plan_apply(BackendKind::NetworkManager, sample_profile(), paths);
  ASSERT_TRUE(plan);
  EXPECT_TRUE(std::all_of(plan->commands.begin(), plan->commands.end(),
                          [](const auto& command) { return command.program == "nmcli"; }));
  EXPECT_NE(plan->files[0].contents.find("ssid=Hotmon"), std::string::npos);
  EXPECT_NE(plan->files[0].contents.find("key-mgmt=wpa-psk"), std::string::npos);
  EXPECT_NE(plan->files[0].contents.find("psk=correct-horse"), std::string::npos);
  for (const auto& command : plan->commands) {
    for (const auto& arg : command.args) {
      EXPECT_EQ(arg.find("correct-horse"), std::string::npos);
    }
  }
  std::filesystem::remove_all(dir);
}

TEST(Backend, IwdPlanWritesTheAccessPointProfile) {
  const auto dir = scratch_dir();
  const auto paths = run_paths(dir);
  auto plan = plan_apply(BackendKind::Iwd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  EXPECT_TRUE(std::all_of(plan->commands.begin(), plan->commands.end(),
                          [](const auto& command) { return command.program == "iwctl"; }));
  EXPECT_NE(plan->files[0].contents.find("Channel=6"), std::string::npos);
  EXPECT_NE(plan->files[0].contents.find("Passphrase=correct-horse"), std::string::npos);
  EXPECT_NE(plan->files[0].contents.find("IPRange=192.168.42.10,192.168.42.100"), std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(Backend, RejectionIsAClearError) {
  const auto dir = scratch_dir();
  Paths paths{dir, dir / "h.conf", dir / "iwd", dir / "proc"};
  auto profile = sample_profile();
  profile.channel = 2;
  profile.band = Band::Band5;
  auto error = plan_apply(BackendKind::DirectHostapd, profile, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("The backend rejected the setting."), std::string::npos);
  auto plan = plan_apply(BackendKind::NetworkManager, sample_profile(), paths);
  ASSERT_TRUE(plan);
  auto runner = ScriptedRunner::with_results(
      {unexpected_text("not found"), unexpected_text("channel is not supported")});
  RecordedSignals signals;
  auto failed = execute_plan(*plan, runner, signals, paths);
  ASSERT_FALSE(failed);
  EXPECT_NE(failed.error().find("The backend rejected the setting."), std::string::npos);
  EXPECT_NE(failed.error().find("channel is not supported"), std::string::npos);
  EXPECT_EQ(runner.calls.size(), 2u);
  EXPECT_FALSE(std::filesystem::exists(paths.nm_secret()));
  std::filesystem::remove_all(dir);
}

TEST(Backend, ExistingHostapdUsesSystemctl) {
  const auto dir = scratch_dir();
  Paths paths{dir / "run", dir / "etc" / "hostapd.conf", dir / "iwd", dir / "proc"};
  auto plan = plan_apply(BackendKind::ExistingHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  EXPECT_TRUE(std::any_of(plan->commands.begin(), plan->commands.end(), [](const auto& command) {
    return command.program == "systemctl" &&
           std::find(command.args.begin(), command.args.end(), "restart") != command.args.end();
  }));
  EXPECT_TRUE(std::any_of(plan->files.begin(), plan->files.end(),
                          [&](const auto& file) { return file.path == paths.hostapd_config; }));
  std::filesystem::remove_all(dir);
}

TEST(Backend, PlannedArgumentsDoNotContainThePassphrase) {
  const auto dir = scratch_dir();
  const auto paths = run_paths(dir);
  for (auto kind : {BackendKind::NetworkManager, BackendKind::Iwd, BackendKind::ExistingHostapd,
                    BackendKind::DirectHostapd}) {
    auto plan = plan_apply(kind, sample_profile(), paths);
    ASSERT_TRUE(plan);
    assert_no_secret_args(*plan, "correct-horse");
  }
  std::filesystem::remove_all(dir);
}

TEST(Backend, SecretFilesUsePrivateModes) {
  const auto dir = scratch_dir();
  const auto paths = run_paths(dir);
  auto plan = plan_apply(BackendKind::DirectHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner runner;
  RecordedSignals signals;
  ASSERT_TRUE(execute_plan(*plan, runner, signals, paths));
  EXPECT_EQ(mode_bits(paths.state_dir), 0700u);
  EXPECT_EQ(mode_bits(paths.direct_hostapd_conf()), 0600u);
  EXPECT_EQ(mode_bits(paths.dnsmasq_conf()) & 0004u, 0004u);
  EXPECT_NE(paths.dnsmasq_conf(), paths.direct_hostapd_conf());
  std::ifstream dns(paths.dnsmasq_conf());
  std::string dns_text((std::istreambuf_iterator<char>(dns)), std::istreambuf_iterator<char>());
  EXPECT_EQ(dns_text.find("correct-horse"), std::string::npos);
  EXPECT_EQ(dns_text.find("wpa_passphrase"), std::string::npos);
  auto iwd = plan_apply(BackendKind::Iwd, sample_profile(), paths);
  ASSERT_TRUE(iwd);
  ASSERT_TRUE(execute_plan(*iwd, runner, signals, paths));
  EXPECT_EQ(mode_bits(iwd->files[0].path), 0600u);
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, FirewallLimitsHotspotForwardAndInput) {
  const auto text = nft_text(sample_profile());
  std::string masq;
  for (size_t start = 0; start < text.size();) {
    const size_t end = text.find('\n', start);
    const auto line = text.substr(start, end == std::string::npos ? end : end - start);
    if (line.find("masquerade") != std::string::npos) {
      masq = line;
    }
    if (line.find("drop") != std::string::npos) {
      EXPECT_NE(line.find("wlan0"), std::string::npos) << line;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  EXPECT_NE(masq.find("wlan0"), std::string::npos);
  EXPECT_NE(masq.find("eth0"), std::string::npos);
  EXPECT_NE(text.find("iifname \"wlan0\" oifname \"eth0\" accept"), std::string::npos);
  EXPECT_NE(text.find("iifname \"eth0\" oifname \"wlan0\" ct state established,related accept"),
            std::string::npos);
  EXPECT_NE(text.find("iifname \"wlan0\" drop"), std::string::npos);
  EXPECT_NE(text.find("udp dport 67 accept"), std::string::npos);
  EXPECT_NE(text.find("ip daddr 192.168.42.1 udp dport 53 accept"), std::string::npos);
  EXPECT_NE(text.find("ip daddr 192.168.42.1 tcp dport 53 accept"), std::string::npos);
  EXPECT_NE(text.find("iifname \"wlan0\" ct state new drop"), std::string::npos);
  EXPECT_EQ(text.find("policy drop"), std::string::npos);
  auto isolated = sample_profile();
  isolated.upstream_interface = "none";
  const auto isolated_text = nft_text(isolated);
  EXPECT_EQ(isolated_text.find("masquerade"), std::string::npos);
  EXPECT_NE(isolated_text.find("iifname \"wlan0\" drop"), std::string::npos);
  EXPECT_EQ(isolated_text.find("policy drop"), std::string::npos);
}

TEST(Backend, ForwardingChangesOnlyTheHotspotAndUpstream) {
  const auto dir = scratch_dir();
  const auto paths = run_paths(dir);
  const auto all = paths.proc_root / "sys/net/ipv4/conf/all/forwarding";
  std::filesystem::create_directories(all.parent_path());
  std::ofstream(all) << "0\n";
  ASSERT_TRUE(enable_forwarding(paths, {"wlan0", "eth0"}));
  std::ifstream ap(forwarding_path(paths, "wlan0"));
  std::ifstream up(forwarding_path(paths, "eth0"));
  std::string ap_text;
  std::string up_text;
  std::getline(ap, ap_text);
  std::getline(up, up_text);
  EXPECT_EQ(ap_text, "1");
  EXPECT_EQ(up_text, "1");
  std::ifstream all_file(all);
  std::string all_text;
  std::getline(all_file, all_text);
  EXPECT_EQ(all_text, "0");
  ASSERT_TRUE(restore_forwarding(paths));
  std::ifstream restored(forwarding_path(paths, "wlan0"));
  std::string restored_text;
  std::getline(restored, restored_text);
  EXPECT_EQ(restored_text, "0");
  EXPECT_FALSE(std::filesystem::exists(paths.forwarding_record()));
  std::filesystem::remove_all(dir);
}

TEST(Backend, FailedStartRemovesTheFirewallAndTheDaemon) {
  const auto dir = scratch_dir();
  Paths paths{dir / "run", dir / "kept.conf", dir / "iwd", dir / "proc"};
  std::filesystem::create_directories(dir);
  std::ofstream(paths.hostapd_config) << "leave-this\n";
  auto plan = plan_apply(BackendKind::DirectHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  PidOnSuccess runner;
  runner.inner = ScriptedRunner::with_results(
      {std::string(), std::string(), std::string(), std::string(), unexpected_text("hostapd failed")});
  runner.pid_path = paths.dnsmasq_pid();
  runner.pid = 77;
  RecordedSignals signals;
  signals.names.push_back({77, "dnsmasq"});
  auto error = execute_plan(*plan, runner, signals, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("The backend rejected the setting."), std::string::npos);
  EXPECT_TRUE(std::any_of(runner.inner.calls.begin(), runner.inner.calls.end(), [](const auto& command) {
    return command.program == "nft" &&
           std::find(command.args.begin(), command.args.end(), "delete") != command.args.end();
  }));
  EXPECT_EQ(signals.pids, std::vector<int>({77}));
  EXPECT_FALSE(signals.running(77));
  EXPECT_FALSE(std::filesystem::exists(paths.direct_hostapd_conf()));
  std::ifstream kept(paths.hostapd_config);
  std::string kept_text((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
  EXPECT_EQ(kept_text, "leave-this\n");
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, StopDoesNotSignalAForeignProcess) {
  RecordedSignals foreign;
  foreign.names.push_back({42, "bash"});
  ASSERT_TRUE(stop_started(foreign, {StartedProc{42, "hostapd"}}));
  EXPECT_TRUE(foreign.pids.empty());
  RecordedSignals other;
  other.names.push_back({99, "hostapd"});
  ASSERT_TRUE(stop_started(other, {}));
  EXPECT_TRUE(other.pids.empty());
  RecordedSignals alive;
  alive.names.push_back({7, "dnsmasq"});
  alive.stay_alive = true;
  auto error = stop_started(alive, {StartedProc{7, "dnsmasq"}});
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("not stopped"), std::string::npos);
  EXPECT_NE(error.error().find("still running"), std::string::npos);
}

TEST(Backend, HostapdSystemFileIsRestoredFromTheBackup) {
  const auto dir = scratch_dir();
  Paths paths{dir / "run", dir / "etc" / "hostapd.conf", dir / "iwd", dir / "proc"};
  std::filesystem::create_directories(paths.hostapd_config.parent_path());
  std::ofstream(paths.hostapd_config) << "original-config\n";
  auto plan = plan_apply(BackendKind::ExistingHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner runner;
  RecordedSignals signals;
  ASSERT_TRUE(execute_plan(*plan, runner, signals, paths));
  std::ifstream changed(paths.hostapd_config);
  std::string changed_text((std::istreambuf_iterator<char>(changed)), std::istreambuf_iterator<char>());
  EXPECT_NE(changed_text.find("ssid=Hotmon"), std::string::npos);
  EXPECT_EQ(mode_bits(paths.hostapd_backup()), 0600u);
  ASSERT_TRUE(restore_hostapd_backup(paths));
  std::ifstream restored(paths.hostapd_config);
  std::string restored_text((std::istreambuf_iterator<char>(restored)), std::istreambuf_iterator<char>());
  EXPECT_EQ(restored_text, "original-config\n");
  EXPECT_FALSE(std::filesystem::exists(paths.hostapd_backup()));
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, MissingBackupDoesNotChangeTheSystemFile) {
  const auto dir = scratch_dir();
  Paths paths{dir / "run", dir / "etc" / "hostapd.conf", dir / "iwd", dir / "proc"};
  std::filesystem::create_directories(paths.hostapd_config.parent_path());
  std::ofstream(paths.hostapd_config) << "original-config\n";
  std::filesystem::create_directories(paths.state_dir);
  std::filesystem::create_directories(paths.hostapd_backup());
  auto plan = plan_apply(BackendKind::ExistingHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner runner;
  RecordedSignals signals;
  ASSERT_TRUE(execute_plan(*plan, runner, signals, paths));
  std::ifstream kept(paths.hostapd_config);
  std::string kept_text((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
  EXPECT_EQ(kept_text, "original-config\n");
  EXPECT_TRUE(std::filesystem::exists(paths.direct_hostapd_conf()));
  EXPECT_EQ(mode_bits(paths.direct_hostapd_conf()), 0600u);
  EXPECT_TRUE(std::any_of(plan->commands.begin(), plan->commands.end(),
                          [](const auto& command) { return command.program == "systemctl"; }));
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, DaemonNameAcceptsOnlyHostapdAndDnsmasq) {
  EXPECT_EQ(daemon_name(std::string("hostapd\n"), std::nullopt), "hostapd");
  EXPECT_EQ(daemon_name(std::string("nginx\n"), std::nullopt), "nginx");
  EXPECT_EQ(daemon_name(std::nullopt, std::string("/usr/sbin/dnsmasq\0--conf-file=/run/x", 32)),
            "dnsmasq");
  EXPECT_FALSE(daemon_name(std::nullopt, std::nullopt));
}

TEST(Backend, NmSecretFileIsRemovedAfterTheCall) {
  const auto dir = scratch_dir();
  Paths paths{dir / "run", dir / "h.conf", dir / "iwd", dir / "proc"};
  auto plan = plan_apply(BackendKind::NetworkManager, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ModeCheck runner;
  RecordedSignals signals;
  ASSERT_TRUE(execute_plan(*plan, runner, signals, paths));
  EXPECT_TRUE(runner.saw_private_secret);
  EXPECT_FALSE(std::filesystem::exists(paths.nm_secret()));
  std::filesystem::remove_all(dir);
}

TEST(Backend, FailedFileInstallRemovesTheSecret) {
  const auto dir = scratch_dir();
  Paths paths{dir / "run", dir / "kept.conf", dir / "iwd", dir / "proc"};
  std::filesystem::create_directories(paths.state_dir);
  std::filesystem::create_directories(paths.nft_path());
  auto plan = plan_apply(BackendKind::DirectHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner runner;
  RecordedSignals signals;
  auto error = execute_plan(*plan, runner, signals, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("cannot write"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(paths.direct_hostapd_conf()));
  if (std::filesystem::exists(paths.dnsmasq_conf())) {
    std::ifstream dns(paths.dnsmasq_conf());
    std::string text((std::istreambuf_iterator<char>(dns)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("correct-horse"), std::string::npos);
  }
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, FailedFileInstallRestoresTheSystemFile) {
  const auto dir = scratch_dir();
  Paths paths{dir / "run", dir / "etc" / "hostapd.conf", dir / "iwd", dir / "proc"};
  std::filesystem::create_directories(paths.hostapd_config.parent_path());
  std::ofstream(paths.hostapd_config) << "original-config\n";
  std::filesystem::create_directories(paths.state_dir);
  std::filesystem::create_directories(paths.nft_path());
  auto plan = plan_apply(BackendKind::ExistingHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner runner;
  RecordedSignals signals;
  auto error = execute_plan(*plan, runner, signals, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("cannot write"), std::string::npos);
  std::ifstream kept(paths.hostapd_config);
  std::string text((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
  EXPECT_EQ(text, "original-config\n");
  EXPECT_FALSE(std::filesystem::exists(paths.hostapd_backup()));
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, MissingSystemFileUsesThePrivateFile) {
  const auto dir = scratch_dir();
  Paths paths{dir / "run", dir / "etc" / "hostapd.conf", dir / "iwd", dir / "proc"};
  std::filesystem::create_directories(paths.hostapd_config.parent_path());
  auto plan = plan_apply(BackendKind::ExistingHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner runner;
  RecordedSignals signals;
  auto report = execute_plan(*plan, runner, signals, paths);
  ASSERT_TRUE(report);
  EXPECT_TRUE(report->private_hostapd);
  EXPECT_FALSE(std::filesystem::exists(paths.hostapd_config));
  EXPECT_TRUE(std::filesystem::exists(paths.direct_hostapd_conf()));
  EXPECT_TRUE(std::any_of(runner.calls.begin(), runner.calls.end(),
                          [](const auto& command) { return command.program == "hostapd"; }));
  EXPECT_FALSE(std::any_of(runner.calls.begin(), runner.calls.end(),
                           [](const auto& command) { return command.program == "systemctl"; }));
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, FailedForwardingChangeRestoresTheFirstInterface) {
  const auto dir = scratch_dir();
  const auto paths = run_paths(dir);
  const auto ap = forwarding_path(paths, "wlan0");
  std::filesystem::create_directories(ap.parent_path());
  std::ofstream(ap) << "0\n";
  const auto upstream = paths.proc_root / "sys/net/ipv4/conf/eth0";
  std::filesystem::create_directories(upstream.parent_path());
  std::ofstream(upstream) << "not-a-directory\n";
  auto plan = plan_apply(BackendKind::DirectHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner runner;
  RecordedSignals signals;
  auto error = execute_plan(*plan, runner, signals, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("hotmon-forward"), std::string::npos);
  std::ifstream restored(ap);
  std::string text;
  std::getline(restored, text);
  EXPECT_EQ(text, "0");
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, FailedReapplyKeepsTheLiveFirewall) {
  const auto dir = scratch_dir();
  const auto paths = run_paths(dir);
  const auto ap = forwarding_path(paths, "wlan0");
  const auto upstream = forwarding_path(paths, "eth0");
  std::filesystem::create_directories(ap.parent_path());
  std::filesystem::create_directories(upstream.parent_path());
  std::ofstream(ap) << "0\n";
  std::ofstream(upstream) << "0\n";
  auto plan = plan_apply(BackendKind::DirectHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner first;
  RecordedSignals signals;
  ASSERT_TRUE(execute_plan(*plan, first, signals, paths));
  std::ifstream saved_file(paths.forwarding_record());
  std::string saved((std::istreambuf_iterator<char>(saved_file)), std::istreambuf_iterator<char>());
  EXPECT_NE(saved.find("wlan0 0"), std::string::npos);
  auto runner = ScriptedRunner::with_results(
      {std::string(), std::string(), std::string(), unexpected_text("dnsmasq failed")});
  auto error = execute_plan(*plan, runner, signals, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("dnsmasq"), std::string::npos);
  std::ifstream record(paths.forwarding_record());
  std::string record_text((std::istreambuf_iterator<char>(record)), std::istreambuf_iterator<char>());
  EXPECT_EQ(record_text, saved);
  EXPECT_FALSE(std::any_of(runner.calls.begin(), runner.calls.end(), [](const auto& command) {
    return command.program == "nft" &&
           std::find(command.args.begin(), command.args.end(), "delete") != command.args.end();
  }));
  EXPECT_TRUE(std::any_of(runner.calls.begin(), runner.calls.end(), [](const auto& command) {
    return command.program == "nft" && std::any_of(command.args.begin(), command.args.end(),
                                                   [](const auto& arg) {
                                                     return arg.ends_with("hotmon.nft.previous");
                                                   });
  }));
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, FailedStartAfterStopDeletesTheFirewallTable) {
  const auto dir = scratch_dir();
  const auto paths = run_paths(dir);
  auto plan = plan_apply(BackendKind::DirectHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner first;
  RecordedSignals signals;
  ASSERT_TRUE(execute_plan(*plan, first, signals, paths));
  ScriptedRunner deleter;
  ASSERT_TRUE(run_nft_delete(deleter));
  clear_nft_live(paths);
  auto runner = ScriptedRunner::with_results(
      {std::string(), std::string(), std::string(), unexpected_text("dnsmasq failed")});
  auto error = execute_plan(*plan, runner, signals, paths);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("dnsmasq"), std::string::npos);
  EXPECT_TRUE(std::any_of(runner.calls.begin(), runner.calls.end(), [](const auto& command) {
    return command.program == "nft" &&
           std::find(command.args.begin(), command.args.end(), "delete") != command.args.end();
  }));
  EXPECT_FALSE(std::any_of(runner.calls.begin(), runner.calls.end(), [](const auto& command) {
    return std::any_of(command.args.begin(), command.args.end(),
                       [](const auto& arg) { return arg.ends_with("hotmon.nft.previous"); });
  }));
  EXPECT_FALSE(std::filesystem::exists(paths.nft_live_marker()));
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, SecondFailedReapplyReloadsTheLiveRules) {
  const auto dir = scratch_dir();
  const auto paths = run_paths(dir);
  auto plan = plan_apply(BackendKind::DirectHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner first;
  RecordedSignals signals;
  ASSERT_TRUE(execute_plan(*plan, first, signals, paths));
  auto changed = sample_profile();
  changed.upstream_interface = "eth1";
  auto next = plan_apply(BackendKind::DirectHostapd, changed, paths);
  ASSERT_TRUE(next);
  auto fail = [] {
    return ScriptedRunner::with_results(
        {std::string(), std::string(), std::string(), unexpected_text("dnsmasq failed")});
  };
  auto first_fail = fail();
  auto second_fail = fail();
  EXPECT_FALSE(execute_plan(*next, first_fail, signals, paths));
  EXPECT_FALSE(execute_plan(*next, second_fail, signals, paths));
  std::ifstream input(paths.nft_path());
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("oifname \"eth0\""), std::string::npos);
  EXPECT_EQ(text.find("eth1"), std::string::npos);
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, FailedReapplyBeforeNftKeepsTheLiveRulesFile) {
  const auto dir = scratch_dir();
  const auto paths = run_paths(dir);
  auto plan = plan_apply(BackendKind::DirectHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner first;
  RecordedSignals signals;
  ASSERT_TRUE(execute_plan(*plan, first, signals, paths));
  auto changed = sample_profile();
  changed.upstream_interface = "eth1";
  auto next = plan_apply(BackendKind::DirectHostapd, changed, paths);
  ASSERT_TRUE(next);
  auto early = ScriptedRunner::with_results({unexpected_text("ip failed")});
  EXPECT_FALSE(execute_plan(*next, early, signals, paths));
  std::ifstream early_file(paths.nft_path());
  std::string early_text((std::istreambuf_iterator<char>(early_file)), std::istreambuf_iterator<char>());
  EXPECT_NE(early_text.find("oifname \"eth0\""), std::string::npos);
  EXPECT_EQ(early_text.find("eth1"), std::string::npos);
  auto later = ScriptedRunner::with_results(
      {std::string(), std::string(), std::string(), unexpected_text("dnsmasq failed")});
  EXPECT_FALSE(execute_plan(*next, later, signals, paths));
  std::ifstream input(paths.nft_path());
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("oifname \"eth0\""), std::string::npos);
  EXPECT_EQ(text.find("eth1"), std::string::npos);
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, SecondApplyKeepsTheOriginalHostapdBackup) {
  const auto dir = scratch_dir();
  Paths paths{dir / "run", dir / "etc" / "hostapd.conf", dir / "iwd", dir / "proc"};
  std::filesystem::create_directories(paths.hostapd_config.parent_path());
  std::ofstream(paths.hostapd_config) << "original-config\n";
  auto plan = plan_apply(BackendKind::ExistingHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner first;
  ScriptedRunner second;
  RecordedSignals signals;
  ASSERT_TRUE(execute_plan(*plan, first, signals, paths));
  ASSERT_TRUE(execute_plan(*plan, second, signals, paths));
  std::ifstream backup(paths.hostapd_backup());
  std::string backup_text((std::istreambuf_iterator<char>(backup)), std::istreambuf_iterator<char>());
  EXPECT_EQ(backup_text, "original-config\n");
  ASSERT_TRUE(restore_hostapd_backup(paths));
  std::ifstream restored(paths.hostapd_config);
  std::string restored_text((std::istreambuf_iterator<char>(restored)), std::istreambuf_iterator<char>());
  EXPECT_EQ(restored_text, "original-config\n");
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(paths.dnsmasq_dir());
}

TEST(Backend, PidFileMustHoldOnlyOnePid) {
  const auto dir = scratch_dir();
  const auto path = dir / "test.pid";
  auto pid_from = [&](const std::string& text) {
    std::ofstream(path, std::ios::trunc) << text;
    return read_pid(path);
  };
  ASSERT_TRUE(pid_from("1234\n"));
  EXPECT_EQ(*pid_from("1234\n"), 1234);
  EXPECT_EQ(*pid_from("+77"), 77);
  EXPECT_FALSE(pid_from("123abc\n"));
  EXPECT_FALSE(pid_from("123\n456\n"));
  EXPECT_FALSE(pid_from("-5\n"));
  EXPECT_FALSE(pid_from("0\n"));
  EXPECT_FALSE(pid_from("99999999999\n"));
  EXPECT_FALSE(pid_from("\n"));
  std::filesystem::remove_all(dir);
}

}  // namespace
