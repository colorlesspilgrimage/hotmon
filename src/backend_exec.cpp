#include "backend_exec.hpp"

#include "iface.hpp"
#include "process.hpp"
#include "text.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iterator>
#include <fstream>
#include <thread>

namespace hotmon {


mode_t mode_of(uint32_t mode) { return static_cast<mode_t>(mode); }

Result<void> ensure_dir_mode(const std::filesystem::path& path, uint32_t mode) {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error) {
    return unexpected_text("The program cannot prepare " + path.string() + ". " + error.message());
  }
  std::filesystem::permissions(path, static_cast<std::filesystem::perms>(mode),
                               std::filesystem::perm_options::replace, error);
  if (error) {
    return unexpected_text("The program cannot protect " + path.string() + ". " + error.message());
  }
  return {};
}

Result<void> write_bytes(const std::filesystem::path& path, std::string_view contents,
                         uint32_t mode) {
  if (const auto parent = path.parent_path(); !parent.empty() && !std::filesystem::exists(parent)) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      return unexpected_text("The program cannot prepare " + parent.string() + ". " +
                             error.message());
    }
  }
  std::filesystem::path temporary = path;
  if (path.has_filename()) {
    temporary = path.parent_path() / ("." + path.filename().string() + ".hotmon-tmp");
  }
  auto fail = [&](const std::string& message) {
    std::error_code error;
    std::filesystem::remove(temporary, error);
    return unexpected_text(message);
  };
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode_of(mode));
  if (fd < 0) {
    return fail("The program cannot write " + path.string() + ". " + errno_text());
  }
  size_t written = 0;
  while (written < contents.size()) {
    const ssize_t count = ::write(fd, contents.data() + written, contents.size() - written);
    if (count < 0) {
      const std::string message = errno_text();
      ::close(fd);
      return fail("The program cannot write " + path.string() + ". " + message);
    }
    written += static_cast<size_t>(count);
  }
  if (::fsync(fd) != 0 || ::fchmod(fd, mode_of(mode)) != 0) {
    const std::string message = errno_text();
    ::close(fd);
    return fail("The program cannot write " + path.string() + ". " + message);
  }
  ::close(fd);
  std::error_code error;
  std::filesystem::permissions(temporary, static_cast<std::filesystem::perms>(mode),
                               std::filesystem::perm_options::replace, error);
  if (error) {
    return fail("The program cannot protect " + path.string() + ". " + error.message());
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    return fail("The program cannot write " + path.string() + ". " + error.message());
  }
  return {};
}

Result<void> write_text(const std::filesystem::path& path, std::string_view contents,
                        uint32_t mode) {
  return write_bytes(path, contents, mode);
}

Result<void> copy_private(const std::filesystem::path& from, const std::filesystem::path& to) {
  if (const auto parent = to.parent_path(); !parent.empty()) {
    if (auto ready = ensure_dir_mode(parent, 0700); !ready) {
      return ready;
    }
  }
  std::error_code error;
  const auto data = std::filesystem::file_size(from, error);
  (void)data;
  std::ifstream input(from, std::ios::binary);
  if (!input) {
    return unexpected_text("The program cannot read " + from.string() + ". " + errno_text());
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return write_bytes(to, buffer.str(), 0600);
}

Result<void> write_sysctl(const std::filesystem::path& path, std::string_view value) {
  if (const auto parent = path.parent_path(); !parent.empty() && !std::filesystem::exists(parent) &&
                                              !path.string().starts_with("/proc")) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      return unexpected_text("The program cannot prepare " + parent.string() + ". " +
                             error.message());
    }
  }
  const int flags = O_WRONLY | (path.string().starts_with("/proc") ? 0 : O_CREAT);
  const int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    return unexpected_text("The program cannot write " + path.string() + ". " + errno_text());
  }
  const ssize_t count = ::write(fd, value.data(), value.size());
  const int err = errno;
  ::close(fd);
  if (count < 0) {
    return unexpected_text("The program cannot write " + path.string() + ". " + errno_text(err));
  }
  return {};
}

struct InstalledFiles {
  std::vector<std::filesystem::path> temporary;
  std::vector<std::filesystem::path> failure_secrets;
  bool replaced_system = false;
  bool created_system = false;
  std::optional<std::filesystem::path> redirect;
  std::optional<std::filesystem::path> previous_nft;
  std::optional<std::filesystem::path> attempt_restore;
};

Result<void> install_system_hostapd(const PlanFile& file, const Paths& paths,
                                    InstalledFiles& installed);
Result<void> install_private_hostapd(const PlanFile& file, const Paths& paths,
                                     InstalledFiles& installed);

Result<void> install_private_hostapd(const PlanFile& file, const Paths& paths,
                                     InstalledFiles& installed) {
  const auto private_path = paths.direct_hostapd_conf();
  if (auto written = write_text(private_path, file.contents, 0600); !written) {
    return written;
  }
  installed.redirect = private_path;
  installed.failure_secrets.push_back(private_path);
  return {};
}

Result<void> install_system_hostapd(const PlanFile& file, const Paths& paths,
                                    InstalledFiles& installed) {
  if (std::filesystem::exists(file.path)) {
    if (std::filesystem::is_regular_file(paths.hostapd_backup())) {
      const auto attempt = paths.state_dir / "hostapd.conf.attempt";
      if (auto copied = copy_private(file.path, attempt); !copied) {
        return copied;
      }
      installed.attempt_restore = attempt;
      return write_text(file.path, file.contents, 0600);
    }
    if (auto copied = copy_private(file.path, paths.hostapd_backup()); copied) {
      installed.replaced_system = true;
      return write_text(file.path, file.contents, 0600);
    }
    return install_private_hostapd(file, paths, installed);
  }
  return install_private_hostapd(file, paths, installed);
}

Result<void> install_files_into(const ApplyPlan& plan, const Paths& paths,
                                InstalledFiles& installed) {
  if (auto secured = secure_state_dir(paths.state_dir); !secured) {
    return secured;
  }
  for (const PlanFile& file : plan.files) {
    if (file.path == paths.hostapd_config && file.path != paths.direct_hostapd_conf()) {
      if (auto installed_file = install_system_hostapd(file, paths, installed); !installed_file) {
        return installed_file;
      }
      continue;
    }
    if (file.path.parent_path() == paths.dnsmasq_dir()) {
      if (auto ready = ensure_dir_mode(paths.dnsmasq_dir(), 0755); !ready) {
        return ready;
      }
    } else if (file.lock_parent) {
      if (const auto parent = file.path.parent_path(); !parent.empty()) {
        if (auto ready = ensure_dir_mode(parent, 0700); !ready) {
          return ready;
        }
      }
    } else if (const auto parent = file.path.parent_path();
               !parent.empty() && !std::filesystem::exists(parent)) {
      std::error_code error;
      std::filesystem::create_directories(parent, error);
      if (error) {
        return unexpected_text("The program cannot prepare " + parent.string() + ". " +
                               error.message());
      }
    }
    if (file.path == paths.nft_path() && std::filesystem::is_regular_file(paths.nft_live_marker()) &&
        std::filesystem::is_regular_file(file.path)) {
      const auto previous = paths.state_dir / "hotmon.nft.previous";
      if (auto copied = copy_private(file.path, previous); !copied) {
        return copied;
      }
      installed.previous_nft = previous;
    }
    if (auto written = write_text(file.path, file.contents, file.mode); !written) {
      return written;
    }
    if (file.temporary) {
      installed.temporary.push_back(file.path);
    }
    if (file.remove_on_failure) {
      installed.failure_secrets.push_back(file.path);
    }
  }
  if (std::any_of(plan.files.begin(), plan.files.end(), [&](const PlanFile& file) {
        return file.path == paths.dnsmasq_conf();
      })) {
    if (auto ready = ensure_dir_mode(paths.dnsmasq_dir(), 0755); !ready) {
      return ready;
    }
    if (!std::filesystem::exists(paths.dnsmasq_lease())) {
      // dnsmasq opens the lease file as root before it drops to nobody. Other users must not write it.
      if (auto lease = write_text(paths.dnsmasq_lease(), "", 0644); !lease) {
        return lease;
      }
    } else if (std::filesystem::is_regular_file(
                   std::filesystem::symlink_status(paths.dnsmasq_lease()))) {
      std::error_code error;
      std::filesystem::permissions(paths.dnsmasq_lease(), std::filesystem::perms(0644),
                                   std::filesystem::perm_options::replace, error);
    }
  }
  return {};
}

void redirect_hostapd(std::vector<PlannedCommand>& commands, const Paths& paths,
                      const std::filesystem::path& private_path) {
  for (PlannedCommand& command : commands) {
    if (command.program == "systemctl") {
      command = PlannedCommand::make(
          "hostapd", {"-B", "-P", paths.hostapd_pid().string(), private_path.string()});
    }
  }
}

std::optional<std::pair<std::string, std::filesystem::path>> started_pid(
    const PlannedCommand& command) {
  if (command.program == "hostapd") {
    const auto index = std::find(command.args.begin(), command.args.end(), "-P");
    if (index == command.args.end() || index + 1 == command.args.end()) {
      return std::nullopt;
    }
    return std::pair{"hostapd", std::filesystem::path(*(index + 1))};
  }
  if (command.program == "dnsmasq") {
    for (const std::string& arg : command.args) {
      if (arg.starts_with("--pid-file=")) {
        return std::pair{"dnsmasq", std::filesystem::path(arg.substr(11))};
      }
    }
  }
  return std::nullopt;
}

void note_started(const PlannedCommand& command, std::vector<StartedProc>& started) {
  const auto info = started_pid(command);
  if (!info) {
    return;
  }
  const auto pid = read_pid_optional(info->second);
  if (!pid) {
    return;
  }
  started.push_back(StartedProc{*pid, info->first});
}

Result<void> rollback(const InstalledFiles& installed, Runner& runner, ProcessControl& signals,
                      const Paths& paths, const std::vector<StartedProc>& started,
                      bool nft_installed, bool forwarding_set) {
  std::optional<std::string> cleanup_error;
  if (installed.previous_nft) {
    if (nft_installed) {
      if (auto restored = runner.run(PlannedCommand::make(
              "nft", {"-f", installed.previous_nft->string()}));
          !restored) {
        cleanup_error = "The firewall did not stop. " + restored.error();
      }
    }
    std::error_code error;
    std::filesystem::copy_file(*installed.previous_nft, paths.nft_path(),
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      cleanup_error = "The program cannot restore " + paths.nft_path().string() + ". " +
                      error.message();
    }
    std::filesystem::remove(*installed.previous_nft, error);
  } else if (nft_installed) {
    if (auto deleted = run_nft_delete(runner); !deleted) {
      cleanup_error = deleted.error();
    } else {
      clear_nft_live(paths);
    }
  }
  if (auto stopped = stop_started(signals, started); !stopped) {
    cleanup_error = stopped.error();
  } else {
    std::error_code error;
    std::filesystem::remove(paths.hostapd_pid(), error);
    std::filesystem::remove(paths.dnsmasq_pid(), error);
  }
  if (forwarding_set) {
    if (auto restored = restore_forwarding(paths); !restored) {
      cleanup_error = restored.error();
    }
  }
  if (installed.attempt_restore) {
    std::error_code error;
    std::filesystem::copy_file(*installed.attempt_restore, paths.hostapd_config,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      cleanup_error = "The program cannot restore " + paths.hostapd_config.string() + ". " +
                      error.message();
    } else if (std::filesystem::remove(*installed.attempt_restore, error); error) {
      cleanup_error = "The program cannot remove the backup " + installed.attempt_restore->string() +
                      ". " + error.message();
    }
  }
  if (installed.replaced_system) {
    if (auto restored = restore_hostapd_backup(paths); !restored) {
      cleanup_error = restored.error();
    }
  }
  if (installed.created_system) {
    std::error_code error;
    std::filesystem::remove(paths.hostapd_config, error);
    std::filesystem::remove(paths.created_marker(), error);
  }
  for (const auto& path : installed.failure_secrets) {
    if (installed.replaced_system && path == paths.hostapd_config) {
      continue;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
  }
  for (const auto& path : installed.temporary) {
    std::error_code error;
    std::filesystem::remove(path, error);
  }
  if (cleanup_error) {
    return unexpected_text(*cleanup_error);
  }
  return {};
}

Result<StartReport> fail_start(std::string err, const InstalledFiles& installed, Runner& runner,
                               ProcessControl& signals, const Paths& paths,
                               const std::vector<StartedProc>& started, bool nft_installed,
                               bool forwarding_set) {
  if (auto cleaned = rollback(installed, runner, signals, paths, started, nft_installed,
                              forwarding_set);
      !cleaned) {
    return unexpected_text(err + " " + cleaned.error());
  }
  return unexpected_text(std::move(err));
}


Result<std::string> SystemRunner::run(const PlannedCommand& command) {
  std::vector<std::string> argv;
  argv.push_back(command.program);
  argv.insert(argv.end(), command.args.begin(), command.args.end());
  auto output = run_capture(argv);
  if (!output) {
    return unexpected_text("The program " + command.program + " did not start. " + output.error());
  }
  if (output->status == 0) {
    return trim_copy(output->out);
  }
  std::string detail = trim_copy(output->err);
  if (detail.empty()) {
    detail = trim_copy(output->out);
  }
  if (detail.empty()) {
    return unexpected_text("exit status exit status: " + std::to_string(output->status));
  }
  return unexpected_text(detail);
}

std::optional<std::string> daemon_name(std::optional<std::string> comm,
                                       std::optional<std::string> cmdline) {
  if (comm) {
    const std::string name = trim_copy(*comm);
    if (name == "hostapd" || name == "dnsmasq") {
      return name;
    }
  }
  if (cmdline) {
    const std::string first = cmdline->substr(0, cmdline->find('\0'));
    const std::filesystem::path path(first);
    const std::string base = path.filename().string();
    if (base == "hostapd" || base == "dnsmasq") {
      return base;
    }
  }
  if (comm) {
    const std::string name = trim_copy(*comm);
    if (!name.empty()) {
      return name;
    }
  }
  return std::nullopt;
}

std::optional<std::string> SystemSignals::describe(int pid) {
  if (pid <= 0) {
    return std::nullopt;
  }
  std::optional<std::string> comm;
  std::ifstream comm_file("/proc/" + std::to_string(pid) + "/comm");
  if (comm_file) {
    std::string text;
    std::getline(comm_file, text);
    comm = text;
  }
  std::optional<std::string> cmdline;
  std::ifstream cmd_file("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
  if (cmd_file) {
    std::ostringstream buffer;
    buffer << cmd_file.rdbuf();
    cmdline = buffer.str();
  }
  return daemon_name(comm, cmdline);
}

bool SystemSignals::running(int pid) {
  const auto name = describe(pid);
  return name && (*name == "hostapd" || *name == "dnsmasq");
}

Result<void> SystemSignals::terminate(int pid) {
  if (pid <= 0) {
    return unexpected_text("The process id " + std::to_string(pid) + " is not valid.");
  }
  const auto name = describe(pid);
  if (!name || (*name != "hostapd" && *name != "dnsmasq")) {
    return unexpected_text("The process " + std::to_string(pid) + " is not hostapd or dnsmasq.");
  }
  if (::kill(pid, SIGTERM) != 0 && errno != ESRCH) {
    return unexpected_text("The process " + std::to_string(pid) + " did not stop. " + errno_text());
  }
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!running(pid)) {
      return {};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (running(pid)) {
    return unexpected_text("The process " + std::to_string(pid) + " is still running.");
  }
  return {};
}

Result<int> read_pid(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return unexpected_text("The pid file " + path.string() + " is not readable. " + errno_text());
  }
  const std::string text = trim_copy(std::string((std::istreambuf_iterator<char>(input)),
                                                 std::istreambuf_iterator<char>()));
  std::string_view digits = text;
  if (digits.starts_with('+')) {
    digits.remove_prefix(1);
  }
  int pid = 0;
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), pid);
  if (digits.empty() || digits.starts_with('-') || error != std::errc() ||
      end != digits.data() + digits.size() || pid <= 0) {
    return unexpected_text("The pid file " + path.string() + " does not contain a pid.");
  }
  return pid;
}

std::optional<int> read_pid_optional(const std::filesystem::path& path) {
  auto pid = read_pid(path);
  if (!pid) {
    return std::nullopt;
  }
  return *pid;
}

Result<void> secure_state_dir(const std::filesystem::path& path) {
  if (auto ready = ensure_dir_mode(path, 0700); !ready) {
    return ready;
  }
  if (::chown(path.c_str(), 0, 0) != 0 && errno != EPERM) {
    return unexpected_text("The program cannot protect " + path.string() + ". " + errno_text());
  }
  return {};
}

Result<void> restore_forwarding(const Paths& paths) {
  const auto record = paths.forwarding_record();
  if (!std::filesystem::exists(record)) {
    return {};
  }
  std::ifstream input(record);
  if (!input) {
    return unexpected_text("The forwarding record cannot be read. " + errno_text());
  }
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  std::optional<std::string> failed;
  for_each_line(text, [&](std::string_view raw) {
    const std::string line(raw);
    if (line.empty()) {
      return true;
    }
    const size_t space = line.find(' ');
    if (space == std::string::npos) {
      failed = "The forwarding record is not valid.";
      return false;
    }
    const std::string iface = line.substr(0, space);
    const std::string value = line.substr(space + 1);
    if (!valid_name(iface) || (value != "0" && value != "1")) {
      failed = "The forwarding record is not valid.";
      return false;
    }
    if (auto written = write_sysctl(forwarding_path(paths, iface), value + "\n"); !written) {
      failed = written.error();
      return false;
    }
    return true;
  });
  if (failed) {
    return unexpected_text(*failed);
  }
  std::error_code error;
  std::filesystem::remove(record, error);
  if (error) {
    return unexpected_text("The forwarding record cannot be removed. " + error.message());
  }
  return {};
}

Result<void> restore_hostapd_backup(const Paths& paths) {
  const auto backup = paths.hostapd_backup();
  if (std::filesystem::is_regular_file(backup)) {
    std::error_code error;
    std::filesystem::copy_file(backup, paths.hostapd_config,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      return unexpected_text("The program cannot restore " + paths.hostapd_config.string() + ". " +
                             error.message());
    }
    std::filesystem::remove(backup, error);
    if (error) {
      return unexpected_text("The program cannot remove the backup " + backup.string() + ". " +
                             error.message());
    }
    return {};
  }
  if (std::filesystem::exists(paths.created_marker())) {
    std::error_code error;
    std::filesystem::remove(paths.hostapd_config, error);
    std::filesystem::remove(paths.created_marker(), error);
  }
  return {};
}

Result<void> retire_iwd_profile(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return {};
  }
  std::error_code error;
  if (std::filesystem::remove(path, error)) {
    return {};
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, error);
  if (error) {
    return unexpected_text("The program cannot protect the iwd profile " + path.string() + ". " +
                           error.message());
  }
  return {};
}

std::filesystem::path forwarding_path(const Paths& paths, std::string_view iface) {
  return paths.proc_root / "sys/net/ipv4/conf" / std::string(iface) / "forwarding";
}

Result<bool> enable_forwarding(const Paths& paths, const std::vector<std::string>& ifaces) {
  if (std::filesystem::is_regular_file(paths.forwarding_record())) {
    return false;
  }
  std::vector<std::pair<std::string, std::string>> saved;
  for (const std::string& iface : ifaces) {
    if (!valid_name(iface) || iface == "all" || iface == "default") {
      return unexpected_text("Forwarding is not set for the interface " + iface + ".");
    }
    const auto path = forwarding_path(paths, iface);
    std::string previous = "0";
    if (std::filesystem::exists(path)) {
      std::ifstream input(path);
      if (!input) {
        return unexpected_text("The forwarding control for " + iface + " cannot be read. " +
                               errno_text());
      }
      std::getline(input, previous);
      previous = trim_copy(previous);
    } else if (paths.proc_root == "/proc") {
      return unexpected_text("The forwarding control for " + iface + " is not available.");
    }
    if (previous != "0" && previous != "1") {
      return unexpected_text("The forwarding value for " + iface + " is not valid.");
    }
    saved.emplace_back(iface, previous);
  }
  std::string record;
  for (size_t index = 0; index < saved.size(); ++index) {
    if (index != 0) {
      record.push_back('\n');
    }
    record += saved[index].first + " " + saved[index].second;
  }
  if (auto written = write_text(paths.forwarding_record(), record + "\n", 0600); !written) {
    return unexpected_text(written.error());
  }
  for (const auto& [iface, previous] : saved) {
    (void)previous;
    if (auto written = write_sysctl(forwarding_path(paths, iface), "1\n"); !written) {
      return unexpected_text(written.error());
    }
  }
  return true;
}

Result<void> run_nft_delete(Runner& runner) {
  const auto command = PlannedCommand::make("nft", {"delete", "table", "inet", "hotmon"});
  if (auto result = runner.run(command); !result) {
    if (stop_target_missing(result.error())) {
      return {};
    }
    return unexpected_text("The firewall did not stop. " + result.error());
  }
  return {};
}

Result<void> stop_started(ProcessControl& signals, const std::vector<StartedProc>& started) {
  for (const StartedProc& item : started) {
    if (item.name != "hostapd" && item.name != "dnsmasq") {
      continue;
    }
    const auto name = signals.describe(item.pid);
    if (!name || *name != item.name) {
      continue;
    }
    if (auto stopped = signals.terminate(item.pid); !stopped) {
      return unexpected_text("The hotspot is not stopped. " + stopped.error());
    }
    if (signals.running(item.pid)) {
      return unexpected_text("The hotspot is not stopped. " + *name + " is still running.");
    }
  }
  return {};
}

Result<StartReport> execute_plan(const ApplyPlan& plan, Runner& runner, ProcessControl& signals,
                                 const Paths& paths) {
  InstalledFiles installed;
  if (auto files = install_files_into(plan, paths, installed); !files) {
    return fail_start(files.error(), installed, runner, signals, paths, {}, false, false);
  }
  const bool private_hostapd = installed.redirect.has_value();
  std::vector<StartedProc> started;
  bool nft_installed = false;
  bool forwarding_set = false;
  auto commands = plan.commands;
  if (installed.redirect) {
    redirect_hostapd(commands, paths, *installed.redirect);
  }
  for (const PlannedCommand& command : commands) {
    if (command.program == "hotmon-forward") {
      auto enabled = enable_forwarding(paths, command.args);
      if (!enabled) {
        forwarding_set = std::filesystem::is_regular_file(paths.forwarding_record());
        return fail_start("The backend rejected the setting. hotmon-forward failed: " +
                              enabled.error(),
                          installed, runner, signals, paths, started, nft_installed, forwarding_set);
      }
      forwarding_set = *enabled;
      continue;
    }
    auto result = runner.run(command);
    if (!result && command.optional) {
      continue;
    }
    if (!result) {
      return fail_start("The backend rejected the setting. " + command.program + " failed: " +
                            result.error(),
                        installed, runner, signals, paths, started, nft_installed, forwarding_set);
    }
    if (command.program == "nft" &&
        std::find(command.args.begin(), command.args.end(), "delete") == command.args.end()) {
      nft_installed = true;
      if (auto marker = write_text(paths.nft_live_marker(), "1\n", 0600); !marker) {
        return fail_start("The backend rejected the setting. nft failed: " + marker.error(),
                          installed, runner, signals, paths, started, nft_installed, forwarding_set);
      }
    }
    note_started(command, started);
  }
  for (const auto& path : installed.temporary) {
    std::error_code error;
    std::filesystem::remove(path, error);
  }
  if (installed.previous_nft) {
    std::error_code error;
    std::filesystem::remove(*installed.previous_nft, error);
  }
  if (installed.attempt_restore) {
    std::error_code error;
    std::filesystem::remove(*installed.attempt_restore, error);
  }
  return StartReport{std::move(started), private_hostapd};
}

Result<void> teardown_hotspot(BackendKind backend, const Profile& profile, bool private_hostapd,
                              const std::vector<StartedProc>& started, Runner& runner,
                              ProcessControl& signals, const Paths& paths) {
  auto commands = plan_stop(backend, profile);
  if (private_hostapd) {
    std::erase_if(commands, [](const PlannedCommand& command) {
      return command.program == "systemctl";
    });
  }
  if (auto stopped = run_stop_commands(commands, runner); !stopped) {
    return unexpected_text(stopped.error());
  }
  if (backend != BackendKind::NetworkManager && backend != BackendKind::Iwd) {
    if (auto stopped = stop_started(signals, started); !stopped) {
      return unexpected_text(stopped.error());
    }
  }
  if (auto deleted = run_nft_delete(runner); !deleted) {
    return unexpected_text(deleted.error());
  }
  clear_nft_live(paths);
  if (auto restored = restore_forwarding(paths); !restored) {
    return unexpected_text(restored.error());
  }
  if (auto restored = restore_hostapd_backup(paths); !restored) {
    return unexpected_text(restored.error());
  }
  std::error_code error;
  std::filesystem::remove(paths.hostapd_pid(), error);
  std::filesystem::remove(paths.dnsmasq_pid(), error);
  const auto iwd_profile = paths.iwd_ap_dir / (profile.ssid + ".ap");
  if (auto retired = retire_iwd_profile(iwd_profile); !retired) {
    return unexpected_text(retired.error());
  }
  std::filesystem::remove(paths.nm_secret(), error);
  return {};
}

}
