use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use crate::profile::{Band, Profile, SecurityMode};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BackendKind {
    NetworkManager,
    Iwd,
    ExistingHostapd,
    Direct,
}

impl BackendKind {
    pub fn label(self) -> &'static str {
        match self {
            Self::NetworkManager => "NetworkManager",
            Self::Iwd => "iwd",
            Self::ExistingHostapd => "hostapd",
            Self::Direct => "hostapd, dnsmasq, and nftables",
        }
    }
}

pub const DIRECT_TOOLS: [&str; 3] = ["hostapd", "dnsmasq", "nftables"];

pub fn direct_tools() -> &'static [&'static str] {
    &DIRECT_TOOLS
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProbeFacts {
    pub network_manager_active: bool,
    pub iwd_active: bool,
    pub hostapd_setup: bool,
}

pub fn select_backend(facts: &ProbeFacts) -> BackendKind {
    if facts.network_manager_active {
        BackendKind::NetworkManager
    } else if facts.iwd_active {
        BackendKind::Iwd
    } else if facts.hostapd_setup {
        BackendKind::ExistingHostapd
    } else {
        BackendKind::Direct
    }
}

pub fn service_active(unit: &str) -> bool {
    Command::new("systemctl")
        .args(["is-active", "--quiet", unit])
        .status()
        .map(|status| status.success())
        .unwrap_or(false)
}

pub fn probe_system() -> ProbeFacts {
    let network_manager_active = service_active("NetworkManager.service");
    let iwd_active = if network_manager_active {
        false
    } else {
        service_active("iwd.service")
    };
    let hostapd_setup = if network_manager_active || iwd_active {
        false
    } else {
        service_active("hostapd.service") || Path::new("/etc/hostapd/hostapd.conf").is_file()
    };
    ProbeFacts {
        network_manager_active,
        iwd_active,
        hostapd_setup,
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Paths {
    pub state_dir: PathBuf,
    pub hostapd_config: PathBuf,
    pub iwd_ap_dir: PathBuf,
    pub proc_root: PathBuf,
}

impl Paths {
    pub fn system() -> Self {
        Self {
            state_dir: PathBuf::from("/run/hotmon"),
            hostapd_config: PathBuf::from("/etc/hostapd/hostapd.conf"),
            iwd_ap_dir: PathBuf::from("/var/lib/iwd/ap"),
            proc_root: PathBuf::from("/proc"),
        }
    }

    pub fn hostapd_pid(&self) -> PathBuf {
        self.state_dir.join("hostapd.pid")
    }

    pub fn dnsmasq_pid(&self) -> PathBuf {
        self.state_dir.join("dnsmasq.pid")
    }

    pub fn direct_hostapd_conf(&self) -> PathBuf {
        self.state_dir.join("hostapd.conf")
    }

    pub fn dnsmasq_dir(&self) -> PathBuf {
        match self.state_dir.parent() {
            Some(parent) if !parent.as_os_str().is_empty() => parent.join("hotmon-dnsmasq"),
            _ => PathBuf::from("/run/hotmon-dnsmasq"),
        }
    }

    pub fn dnsmasq_conf(&self) -> PathBuf {
        self.dnsmasq_dir().join("dnsmasq.conf")
    }

    pub fn dnsmasq_lease(&self) -> PathBuf {
        self.dnsmasq_dir().join("leases")
    }

    pub fn nft_path(&self) -> PathBuf {
        self.state_dir.join("hotmon.nft")
    }

    pub fn nm_secret(&self) -> PathBuf {
        self.state_dir.join("hotmon.nmconnection")
    }

    pub fn hostapd_backup(&self) -> PathBuf {
        self.state_dir.join("hostapd.conf.bak")
    }

    pub fn created_marker(&self) -> PathBuf {
        self.state_dir.join("hostapd.created")
    }

    pub fn forwarding_record(&self) -> PathBuf {
        self.state_dir.join("forwarding.restore")
    }

    pub fn nft_live_marker(&self) -> PathBuf {
        self.state_dir.join("nft.live")
    }
}

pub fn clear_nft_live(paths: &Paths) {
    let _ = fs::remove_file(paths.nft_live_marker());
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlannedCommand {
    pub program: String,
    pub args: Vec<String>,
    pub optional: bool,
}

impl PlannedCommand {
    pub fn new(
        program: impl Into<String>,
        args: impl IntoIterator<Item = impl Into<String>>,
    ) -> Self {
        Self {
            program: program.into(),
            args: args.into_iter().map(Into::into).collect(),
            optional: false,
        }
    }

    pub fn optional(mut self) -> Self {
        self.optional = true;
        self
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlanFile {
    pub path: PathBuf,
    pub contents: String,
    pub mode: u32,
    pub temporary: bool,
    pub remove_on_failure: bool,
    pub lock_parent: bool,
}

impl PlanFile {
    fn plain(path: PathBuf, contents: String, mode: u32) -> Self {
        Self {
            path,
            contents,
            mode,
            temporary: false,
            remove_on_failure: mode == 0o600,
            lock_parent: false,
        }
    }

    fn temporary(mut self) -> Self {
        self.temporary = true;
        self.remove_on_failure = true;
        self
    }

    fn lock_parent(mut self) -> Self {
        self.lock_parent = true;
        self
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ApplyPlan {
    pub files: Vec<PlanFile>,
    pub commands: Vec<PlannedCommand>,
    pub tools: Vec<&'static str>,
}

pub trait Runner {
    fn run(&mut self, command: &PlannedCommand) -> Result<String, String>;
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StartedProc {
    pub pid: i32,
    pub name: String,
}

#[derive(Debug)]
pub struct StartReport {
    pub started: Vec<StartedProc>,
    pub private_hostapd: bool,
}

pub trait ProcessControl {
    fn describe(&mut self, pid: i32) -> Option<String>;
    fn running(&mut self, pid: i32) -> bool;
    fn terminate(&mut self, pid: i32) -> Result<(), String>;
}

pub struct SystemRunner;

impl Runner for SystemRunner {
    fn run(&mut self, command: &PlannedCommand) -> Result<String, String> {
        let output = Command::new(&command.program)
            .args(&command.args)
            .output()
            .map_err(|err| format!("The program {} did not start. {err}", command.program))?;
        if output.status.success() {
            return Ok(String::from_utf8_lossy(&output.stdout).trim().to_string());
        }
        let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
        let stdout = String::from_utf8_lossy(&output.stdout).trim().to_string();
        let detail = if stderr.is_empty() { stdout } else { stderr };
        if detail.is_empty() {
            Err(format!("exit status {}", output.status))
        } else {
            Err(detail)
        }
    }
}

pub struct SystemSignals;

impl ProcessControl for SystemSignals {
    fn describe(&mut self, pid: i32) -> Option<String> {
        if pid <= 0 {
            return None;
        }
        let comm = fs::read_to_string(format!("/proc/{pid}/comm")).ok();
        let cmdline = fs::read(format!("/proc/{pid}/cmdline")).ok();
        daemon_name(comm.as_deref(), cmdline.as_deref())
    }

    fn running(&mut self, pid: i32) -> bool {
        matches!(self.describe(pid).as_deref(), Some("hostapd" | "dnsmasq"))
    }

    fn terminate(&mut self, pid: i32) -> Result<(), String> {
        if pid <= 0 {
            return Err(format!("The process id {pid} is not valid."));
        }
        match self.describe(pid).as_deref() {
            Some("hostapd" | "dnsmasq") => {}
            _ => {
                return Err(format!("The process {pid} is not hostapd or dnsmasq."));
            }
        }
        let rc = unsafe { libc::kill(pid, libc::SIGTERM) };
        if rc != 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() != Some(libc::ESRCH) {
                return Err(format!("The process {pid} did not stop. {err}"));
            }
        }
        for _ in 0..20 {
            if !self.running(pid) {
                return Ok(());
            }
            std::thread::sleep(Duration::from_millis(25));
        }
        if self.running(pid) {
            Err(format!("The process {pid} is still running."))
        } else {
            Ok(())
        }
    }
}

pub fn daemon_name(comm: Option<&str>, cmdline: Option<&[u8]>) -> Option<String> {
    if let Some(comm) = comm.map(str::trim) {
        if comm == "hostapd" || comm == "dnsmasq" {
            return Some(comm.to_string());
        }
    }
    if let Some(bytes) = cmdline {
        let first = bytes.split(|byte| *byte == 0).next().unwrap_or(&[]);
        let text = String::from_utf8_lossy(first);
        let base = Path::new(text.as_ref())
            .file_name()
            .map(|value| value.to_string_lossy().into_owned())
            .unwrap_or_default();
        if base == "hostapd" || base == "dnsmasq" {
            return Some(base);
        }
    }
    comm.map(str::trim)
        .filter(|name| !name.is_empty())
        .map(str::to_string)
}

#[derive(Clone, Debug, Default)]
#[cfg(test)]
pub struct ScriptedRunner {
    pub calls: Vec<PlannedCommand>,
    pub results: Vec<Result<String, String>>,
    index: usize,
}

#[cfg(test)]
impl ScriptedRunner {
    pub fn with_results(results: Vec<Result<String, String>>) -> Self {
        Self {
            results,
            ..Self::default()
        }
    }
}

#[cfg(test)]
impl Runner for ScriptedRunner {
    fn run(&mut self, command: &PlannedCommand) -> Result<String, String> {
        self.calls.push(command.clone());
        if self.index >= self.results.len() {
            return Ok(String::new());
        }
        let result = self.results[self.index].clone();
        self.index += 1;
        result
    }
}

#[derive(Clone, Debug, Default)]
#[cfg(test)]
pub struct RecordedSignals {
    pub pids: Vec<i32>,
    pub fail: bool,
    pub stay_alive: bool,
    pub names: Vec<(i32, String)>,
}

#[cfg(test)]
impl ProcessControl for RecordedSignals {
    fn describe(&mut self, pid: i32) -> Option<String> {
        self.names
            .iter()
            .find(|(id, _)| *id == pid)
            .map(|(_, name)| name.clone())
    }

    fn running(&mut self, pid: i32) -> bool {
        let Some(name) = self.describe(pid) else {
            return false;
        };
        if name != "hostapd" && name != "dnsmasq" {
            return false;
        }
        if self.pids.contains(&pid) && !self.fail && !self.stay_alive {
            false
        } else {
            true
        }
    }

    fn terminate(&mut self, pid: i32) -> Result<(), String> {
        if pid <= 0 {
            return Err(format!("The process id {pid} is not valid."));
        }
        match self.describe(pid).as_deref() {
            Some("hostapd" | "dnsmasq") => {}
            _ => {
                return Err(format!("The process {pid} is not hostapd or dnsmasq."));
            }
        }
        self.pids.push(pid);
        if self.fail {
            Err("The process did not stop.".to_string())
        } else {
            Ok(())
        }
    }
}

pub fn plan_apply(
    kind: BackendKind,
    profile: &Profile,
    paths: &Paths,
) -> Result<ApplyPlan, String> {
    profile
        .check_settings()
        .map_err(|err| format!("The backend rejected the setting. {err}"))?;
    match kind {
        BackendKind::NetworkManager => Ok(nm_plan(profile, paths)),
        BackendKind::Iwd => Ok(iwd_plan(profile, paths)),
        BackendKind::ExistingHostapd => Ok(hostapd_plan(profile, paths, true)),
        BackendKind::Direct => Ok(hostapd_plan(profile, paths, false)),
    }
}

pub fn plan_stop(kind: BackendKind, profile: &Profile) -> Vec<PlannedCommand> {
    match kind {
        BackendKind::NetworkManager => vec![
            PlannedCommand::new("nmcli", ["connection", "down", "hotmon"]),
            PlannedCommand::new("nmcli", ["connection", "delete", "hotmon"]),
        ],
        BackendKind::Iwd => vec![PlannedCommand::new(
            "iwctl",
            ["ap", profile.ap_interface.as_str(), "stop"],
        )],
        BackendKind::ExistingHostapd => {
            vec![PlannedCommand::new("systemctl", ["stop", "hostapd"])]
        }
        BackendKind::Direct => Vec::new(),
    }
}

pub fn run_stop_commands(
    commands: &[PlannedCommand],
    runner: &mut dyn Runner,
) -> Result<(), String> {
    for command in commands {
        if let Err(message) = runner.run(command) {
            if command.optional || stop_target_missing(&message) {
                continue;
            }
            return Err(format!(
                "The backend rejected the stop request. {} failed: {message}",
                command.program
            ));
        }
    }
    Ok(())
}

fn stop_target_missing(message: &str) -> bool {
    let text = message.to_ascii_lowercase();
    text.contains("unknown connection")
        || text.contains("not found")
        || text.contains("no such")
        || text.contains("does not exist")
        || text.contains("not active")
}

pub fn read_pid(path: &Path) -> Result<i32, String> {
    let text = std::fs::read_to_string(path)
        .map_err(|err| format!("The pid file {} is not readable. {err}", path.display()))?;
    let pid: i32 = text
        .trim()
        .parse()
        .map_err(|_| format!("The pid file {} does not contain a pid.", path.display()))?;
    if pid <= 0 {
        return Err(format!(
            "The pid file {} does not contain a pid.",
            path.display()
        ));
    }
    Ok(pid)
}

pub fn read_pid_optional(path: &Path) -> Option<i32> {
    read_pid(path).ok()
}

struct InstalledFiles {
    temporary: Vec<PathBuf>,
    failure_secrets: Vec<PathBuf>,
    replaced_system: bool,
    created_system: bool,
    redirect: Option<PathBuf>,
    previous_nft: Option<PathBuf>,
    attempt_restore: Option<PathBuf>,
}

pub fn execute_plan(
    plan: &ApplyPlan,
    runner: &mut dyn Runner,
    signals: &mut dyn ProcessControl,
    paths: &Paths,
) -> Result<StartReport, String> {
    let installed = match install_files(plan, paths) {
        Ok(installed) => installed,
        Err((installed, message)) => {
            return fail_start(
                message,
                &installed,
                runner,
                signals,
                paths,
                &[],
                false,
                false,
            );
        }
    };
    let private_hostapd = installed.redirect.is_some();
    let mut started = Vec::new();
    let mut nft_installed = false;
    let mut forwarding_set = false;
    let mut commands = plan.commands.clone();
    if let Some(private) = installed.redirect.clone() {
        redirect_hostapd(&mut commands, paths, &private);
    }
    for command in &commands {
        if command.program == "hotmon-forward" {
            match enable_forwarding(paths, &command.args) {
                Ok(created) => forwarding_set = created,
                Err(message) => {
                    let err = format!(
                        "The backend rejected the setting. hotmon-forward failed: {message}"
                    );
                    forwarding_set = paths.forwarding_record().is_file();
                    return fail_start(
                        err,
                        &installed,
                        runner,
                        signals,
                        paths,
                        &started,
                        nft_installed,
                        forwarding_set,
                    );
                }
            }
            continue;
        }
        match runner.run(command) {
            Ok(_) => {
                if command.program == "nft" && !command.args.iter().any(|arg| arg == "delete") {
                    nft_installed = true;
                    if let Err(message) = write_text(&paths.nft_live_marker(), "1\n", 0o600) {
                        let err = format!(
                            "The backend rejected the setting. nft failed: {message}"
                        );
                        return fail_start(
                            err,
                            &installed,
                            runner,
                            signals,
                            paths,
                            &started,
                            nft_installed,
                            forwarding_set,
                        );
                    }
                }
                note_started(command, &mut started);
            }
            Err(_message) if command.optional => continue,
            Err(message) => {
                let err = format!(
                    "The backend rejected the setting. {} failed: {message}",
                    command.program
                );
                return fail_start(
                    err,
                    &installed,
                    runner,
                    signals,
                    paths,
                    &started,
                    nft_installed,
                    forwarding_set,
                );
            }
        }
    }
    for path in &installed.temporary {
        let _ = fs::remove_file(path);
    }
    if let Some(previous) = &installed.previous_nft {
        let _ = fs::remove_file(previous);
    }
    if let Some(attempt) = &installed.attempt_restore {
        let _ = fs::remove_file(attempt);
    }
    Ok(StartReport {
        started,
        private_hostapd,
    })
}

fn fail_start(
    err: String,
    installed: &InstalledFiles,
    runner: &mut dyn Runner,
    signals: &mut dyn ProcessControl,
    paths: &Paths,
    started: &[StartedProc],
    nft_installed: bool,
    forwarding_set: bool,
) -> Result<StartReport, String> {
    match rollback(
        installed,
        runner,
        signals,
        paths,
        started,
        nft_installed,
        forwarding_set,
    ) {
        Ok(()) => Err(err),
        Err(extra) => Err(format!("{err} {extra}")),
    }
}

fn rollback(
    installed: &InstalledFiles,
    runner: &mut dyn Runner,
    signals: &mut dyn ProcessControl,
    paths: &Paths,
    started: &[StartedProc],
    nft_installed: bool,
    forwarding_set: bool,
) -> Result<(), String> {
    let mut cleanup_error = None;
    if nft_installed {
        if let Some(previous) = &installed.previous_nft {
            let restore = PlannedCommand::new("nft", ["-f", previous.to_string_lossy().as_ref()]);
            if let Err(err) = runner.run(&restore) {
                cleanup_error = Some(format!("The firewall did not stop. {err}"));
            } else if let Err(err) = fs::copy(previous, paths.nft_path()) {
                cleanup_error = Some(format!(
                    "The program cannot restore {}. {err}",
                    paths.nft_path().display()
                ));
            }
            let _ = fs::remove_file(previous);
        } else if let Err(err) = run_nft_delete(runner) {
            cleanup_error = Some(err);
        } else {
            clear_nft_live(paths);
        }
    }
    if let Err(err) = stop_started(signals, started) {
        cleanup_error = Some(err);
    } else {
        let _ = fs::remove_file(paths.hostapd_pid());
        let _ = fs::remove_file(paths.dnsmasq_pid());
    }
    if forwarding_set {
        if let Err(err) = restore_forwarding(paths) {
            cleanup_error = Some(err);
        }
    }
    if let Some(attempt) = &installed.attempt_restore {
        if let Err(err) = fs::copy(attempt, &paths.hostapd_config) {
            cleanup_error = Some(format!(
                "The program cannot restore {}. {err}",
                paths.hostapd_config.display()
            ));
        } else if let Err(err) = fs::remove_file(attempt) {
            cleanup_error = Some(format!(
                "The program cannot remove the backup {}. {err}",
                attempt.display()
            ));
        }
    }
    if installed.replaced_system {
        if let Err(err) = restore_hostapd_backup(paths) {
            cleanup_error = Some(err);
        }
    }
    if installed.created_system {
        let _ = fs::remove_file(&paths.hostapd_config);
        let _ = fs::remove_file(paths.created_marker());
    }
    for path in &installed.failure_secrets {
        if installed.replaced_system && path == &paths.hostapd_config {
            continue;
        }
        let _ = fs::remove_file(path);
    }
    for path in &installed.temporary {
        let _ = fs::remove_file(path);
    }
    match cleanup_error {
        Some(err) => Err(err),
        None => Ok(()),
    }
}

pub fn run_nft_delete(runner: &mut dyn Runner) -> Result<(), String> {
    let command = PlannedCommand::new("nft", ["delete", "table", "inet", "hotmon"]);
    match runner.run(&command) {
        Ok(_) => Ok(()),
        Err(message) if stop_target_missing(&message) => Ok(()),
        Err(message) => Err(format!("The firewall did not stop. {message}")),
    }
}

pub fn stop_started(
    signals: &mut dyn ProcessControl,
    started: &[StartedProc],
) -> Result<(), String> {
    for item in started {
        if item.name != "hostapd" && item.name != "dnsmasq" {
            continue;
        }
        let Some(name) = signals.describe(item.pid) else {
            continue;
        };
        if name != item.name {
            continue;
        }
        signals
            .terminate(item.pid)
            .map_err(|err| format!("The hotspot is not stopped. {err}"))?;
        if signals.running(item.pid) {
            return Err(format!(
                "The hotspot is not stopped. {name} is still running."
            ));
        }
    }
    Ok(())
}

pub fn restore_forwarding(paths: &Paths) -> Result<(), String> {
    let record = paths.forwarding_record();
    if !record.exists() {
        return Ok(());
    }
    let text = fs::read_to_string(&record)
        .map_err(|err| format!("The forwarding record cannot be read. {err}"))?;
    for line in text.lines().filter(|line| !line.is_empty()) {
        let Some((iface, value)) = line.split_once(' ') else {
            return Err("The forwarding record is not valid.".to_string());
        };
        if !crate::iface::valid_name(iface) || (value != "0" && value != "1") {
            return Err("The forwarding record is not valid.".to_string());
        }
        write_sysctl(&forwarding_path(paths, iface), &format!("{value}\n"))?;
    }
    fs::remove_file(&record)
        .map_err(|err| format!("The forwarding record cannot be removed. {err}"))?;
    Ok(())
}

pub fn restore_hostapd_backup(paths: &Paths) -> Result<(), String> {
    let backup = paths.hostapd_backup();
    if backup.is_file() {
        fs::copy(&backup, &paths.hostapd_config).map_err(|err| {
            format!(
                "The program cannot restore {}. {err}",
                paths.hostapd_config.display()
            )
        })?;
        fs::remove_file(&backup).map_err(|err| {
            format!(
                "The program cannot remove the backup {}. {err}",
                backup.display()
            )
        })?;
        return Ok(());
    }
    let marker = paths.created_marker();
    if marker.exists() {
        let _ = fs::remove_file(&paths.hostapd_config);
        let _ = fs::remove_file(&marker);
    }
    Ok(())
}

pub fn retire_iwd_profile(path: &Path) -> Result<(), String> {
    if !path.exists() {
        return Ok(());
    }
    if fs::remove_file(path).is_ok() {
        return Ok(());
    }
    fs::set_permissions(path, fs::Permissions::from_mode(0o600)).map_err(|err| {
        format!(
            "The program cannot protect the iwd profile {}. {err}",
            path.display()
        )
    })
}

fn install_files(
    plan: &ApplyPlan,
    paths: &Paths,
) -> Result<InstalledFiles, (InstalledFiles, String)> {
    let mut installed = InstalledFiles {
        temporary: Vec::new(),
        failure_secrets: Vec::new(),
        replaced_system: false,
        created_system: false,
        redirect: None,
        previous_nft: None,
        attempt_restore: None,
    };
    if let Err(err) = install_files_into(plan, paths, &mut installed) {
        return Err((installed, err));
    }
    Ok(installed)
}

fn install_files_into(
    plan: &ApplyPlan,
    paths: &Paths,
    installed: &mut InstalledFiles,
) -> Result<(), String> {
    secure_state_dir(&paths.state_dir)?;
    for file in &plan.files {
        if file.path == paths.hostapd_config && file.path != paths.direct_hostapd_conf() {
            install_system_hostapd(file, paths, installed)?;
            continue;
        }
        if file.path.parent() == Some(paths.dnsmasq_dir().as_path()) {
            ensure_dir_mode(&paths.dnsmasq_dir(), 0o755)?;
        } else if file.lock_parent {
            if let Some(parent) = file.path.parent() {
                if !parent.as_os_str().is_empty() {
                    ensure_dir_mode(parent, 0o700)?;
                }
            }
        } else if let Some(parent) = file.path.parent() {
            if !parent.as_os_str().is_empty() && !parent.exists() {
                fs::create_dir_all(parent).map_err(|err| {
                    format!("The program cannot prepare {}. {err}", parent.display())
                })?;
            }
        }
        if file.path == paths.nft_path()
            && paths.nft_live_marker().is_file()
            && file.path.is_file()
        {
            let previous = paths.state_dir.join("hotmon.nft.previous");
            copy_private(&file.path, &previous)?;
            installed.previous_nft = Some(previous);
        }
        write_text(&file.path, &file.contents, file.mode)?;
        if file.temporary {
            installed.temporary.push(file.path.clone());
        }
        if file.remove_on_failure {
            installed.failure_secrets.push(file.path.clone());
        }
    }
    if plan
        .files
        .iter()
        .any(|file| file.path == paths.dnsmasq_conf())
    {
        ensure_dir_mode(&paths.dnsmasq_dir(), 0o755)?;
        if !paths.dnsmasq_lease().exists() {
            write_text(&paths.dnsmasq_lease(), "", 0o666)?;
        }
    }
    Ok(())
}

fn install_system_hostapd(
    file: &PlanFile,
    paths: &Paths,
    installed: &mut InstalledFiles,
) -> Result<(), String> {
    if file.path.exists() {
        if paths.hostapd_backup().is_file() {
            let attempt = paths.state_dir.join("hostapd.conf.attempt");
            copy_private(&file.path, &attempt)?;
            installed.attempt_restore = Some(attempt);
            write_text(&file.path, &file.contents, 0o600)?;
        } else {
            match copy_private(&file.path, &paths.hostapd_backup()) {
                Ok(()) => {
                    installed.replaced_system = true;
                    write_text(&file.path, &file.contents, 0o600)?;
                }
                Err(_) => install_private_hostapd(file, paths, installed)?,
            }
        }
        return Ok(());
    }
    install_private_hostapd(file, paths, installed)
}

fn install_private_hostapd(
    file: &PlanFile,
    paths: &Paths,
    installed: &mut InstalledFiles,
) -> Result<(), String> {
    let private = paths.direct_hostapd_conf();
    write_text(&private, &file.contents, 0o600)?;
    installed.redirect = Some(private.clone());
    installed.failure_secrets.push(private);
    Ok(())
}

fn redirect_hostapd(commands: &mut [PlannedCommand], paths: &Paths, private: &Path) {
    let private = private.to_string_lossy().to_string();
    for command in commands.iter_mut() {
        if command.program == "systemctl" {
            *command = PlannedCommand::new(
                "hostapd",
                [
                    "-B".to_string(),
                    "-P".to_string(),
                    paths.hostapd_pid().to_string_lossy().to_string(),
                    private.clone(),
                ],
            );
        }
    }
}

fn note_started(command: &PlannedCommand, started: &mut Vec<StartedProc>) {
    let Some((name, pid_path)) = started_pid(command) else {
        return;
    };
    let Some(pid) = read_pid_optional(&pid_path) else {
        return;
    };
    started.push(StartedProc {
        pid,
        name: name.to_string(),
    });
}

fn started_pid(command: &PlannedCommand) -> Option<(&'static str, PathBuf)> {
    if command.program == "hostapd" {
        let index = command.args.iter().position(|arg| arg == "-P")?;
        let path = command.args.get(index + 1)?;
        return Some(("hostapd", PathBuf::from(path)));
    }
    if command.program == "dnsmasq" {
        let path = command
            .args
            .iter()
            .find_map(|arg| arg.strip_prefix("--pid-file="))?;
        return Some(("dnsmasq", PathBuf::from(path)));
    }
    None
}

pub fn secure_state_dir(path: &Path) -> Result<(), String> {
    ensure_dir_mode(path, 0o700)?;
    match chown_root(path) {
        Ok(()) => Ok(()),
        Err(err) if err.raw_os_error() == Some(libc::EPERM) => Ok(()),
        Err(err) => Err(format!(
            "The program cannot protect {}. {err}",
            path.display()
        )),
    }
}

fn chown_root(path: &Path) -> std::io::Result<()> {
    let text = std::ffi::CString::new(path.as_os_str().as_bytes())
        .map_err(|err| std::io::Error::new(std::io::ErrorKind::InvalidInput, err))?;
    let rc = unsafe { libc::chown(text.as_ptr(), 0, 0) };
    if rc == 0 {
        Ok(())
    } else {
        Err(std::io::Error::last_os_error())
    }
}

fn ensure_dir_mode(path: &Path, mode: u32) -> Result<(), String> {
    fs::create_dir_all(path)
        .map_err(|err| format!("The program cannot prepare {}. {err}", path.display()))?;
    fs::set_permissions(path, fs::Permissions::from_mode(mode))
        .map_err(|err| format!("The program cannot protect {}. {err}", path.display()))
}

fn copy_private(from: &Path, to: &Path) -> Result<(), String> {
    if let Some(parent) = to.parent() {
        ensure_dir_mode(parent, 0o700)?;
    }
    let data = fs::read(from)
        .map_err(|err| format!("The program cannot read {}. {err}", from.display()))?;
    write_bytes(to, &data, 0o600)
}

fn write_text(path: &Path, contents: &str, mode: u32) -> Result<(), String> {
    write_bytes(path, contents.as_bytes(), mode)
}

fn write_bytes(path: &Path, contents: &[u8], mode: u32) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() && !parent.exists() {
            fs::create_dir_all(parent)
                .map_err(|err| format!("The program cannot prepare {}. {err}", parent.display()))?;
        }
    }
    let temporary = match path.file_name() {
        Some(name) => {
            let mut file_name = std::ffi::OsString::from(".");
            file_name.push(name);
            file_name.push(".hotmon-tmp");
            path.with_file_name(file_name)
        }
        None => path.to_path_buf(),
    };
    let write_result = (|| {
        let mut file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(mode)
            .open(&temporary)
            .map_err(|err| format!("The program cannot write {}. {err}", path.display()))?;
        file.write_all(contents)
            .map_err(|err| format!("The program cannot write {}. {err}", path.display()))?;
        file.sync_all()
            .map_err(|err| format!("The program cannot write {}. {err}", path.display()))?;
        fs::set_permissions(&temporary, fs::Permissions::from_mode(mode))
            .map_err(|err| format!("The program cannot protect {}. {err}", path.display()))?;
        fs::rename(&temporary, path)
            .map_err(|err| format!("The program cannot write {}. {err}", path.display()))?;
        Ok(())
    })();
    if write_result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    write_result
}

fn enable_forwarding(paths: &Paths, ifaces: &[String]) -> Result<bool, String> {
    if paths.forwarding_record().is_file() {
        return Ok(false);
    }
    let mut saved = Vec::new();
    for iface in ifaces {
        if !crate::iface::valid_name(iface) || iface == "all" || iface == "default" {
            return Err(format!("Forwarding is not set for the interface {iface}."));
        }
        let path = forwarding_path(paths, iface);
        let previous = if path.exists() {
            fs::read_to_string(&path)
                .map_err(|err| format!("The forwarding control for {iface} cannot be read. {err}"))?
                .trim()
                .to_string()
        } else if paths.proc_root == Path::new("/proc") {
            return Err(format!(
                "The forwarding control for {iface} is not available."
            ));
        } else {
            "0".to_string()
        };
        if previous != "0" && previous != "1" {
            return Err(format!("The forwarding value for {iface} is not valid."));
        }
        saved.push((iface.clone(), previous));
    }
    let record = saved
        .iter()
        .map(|(iface, previous)| format!("{iface} {previous}"))
        .collect::<Vec<_>>()
        .join("\n");
    write_text(&paths.forwarding_record(), &format!("{record}\n"), 0o600)?;
    for (iface, _) in &saved {
        write_sysctl(&forwarding_path(paths, iface), "1\n")?;
    }
    Ok(true)
}

fn forwarding_path(paths: &Paths, iface: &str) -> PathBuf {
    paths
        .proc_root
        .join("sys/net/ipv4/conf")
        .join(iface)
        .join("forwarding")
}

fn write_sysctl(path: &Path, value: &str) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        if !parent.exists() && !path.starts_with("/proc") {
            fs::create_dir_all(parent)
                .map_err(|err| format!("The program cannot prepare {}. {err}", parent.display()))?;
        }
    }
    let mut file = OpenOptions::new()
        .write(true)
        .create(!path.starts_with("/proc"))
        .open(path)
        .map_err(|err| format!("The program cannot write {}. {err}", path.display()))?;
    file.write_all(value.as_bytes())
        .map_err(|err| format!("The program cannot write {}. {err}", path.display()))
}

fn nm_plan(profile: &Profile, paths: &Paths) -> ApplyPlan {
    let secret = paths.nm_secret();
    ApplyPlan {
        files: vec![PlanFile::plain(secret.clone(), nm_keyfile(profile), 0o600).temporary()],
        commands: vec![
            PlannedCommand::new("nmcli", ["connection", "delete", "hotmon"]).optional(),
            PlannedCommand::new(
                "nmcli",
                ["connection", "load", secret.to_string_lossy().as_ref()],
            ),
            PlannedCommand::new("nmcli", ["connection", "up", "hotmon"]),
        ],
        tools: vec!["NetworkManager"],
    }
}

fn nm_keyfile(profile: &Profile) -> String {
    let band = match profile.band {
        Band::Band24 => "bg",
        Band::Band5 => "a",
    };
    let method = if profile.dhcp_enabled {
        "shared"
    } else {
        "manual"
    };
    let address = profile
        .network()
        .and_then(|network| {
            let prefix = network.prefix();
            network.gateway().map(|addr| format!("{addr}/{prefix}"))
        })
        .unwrap_or_else(|_| profile.address_cidr.clone());
    let mut text = format!(
        "[connection]\nid=hotmon\ntype=wifi\ninterface-name={}\nautoconnect=false\n\n[wifi]\nmode=ap\nssid={}\nband={band}\nchannel={}\n\n",
        profile.ap_interface, profile.ssid, profile.channel
    );
    match profile.security {
        SecurityMode::Open => {}
        SecurityMode::Wpa2 => {
            text.push_str(&format!(
                "[wifi-security]\nkey-mgmt=wpa-psk\npsk={}\n\n",
                profile.passphrase
            ));
        }
        SecurityMode::Wpa3 => {
            text.push_str(&format!(
                "[wifi-security]\nkey-mgmt=sae\npsk={}\n\n",
                profile.passphrase
            ));
        }
    }
    text.push_str(&format!(
        "[ipv4]\nmethod={method}\naddress1={address}\n\n[ipv6]\nmethod=disabled\n"
    ));
    text
}

fn iwd_plan(profile: &Profile, paths: &Paths) -> ApplyPlan {
    let path = paths.iwd_ap_dir.join(format!("{}.ap", profile.ssid));
    ApplyPlan {
        files: vec![PlanFile::plain(path, iwd_profile(profile), 0o600).lock_parent()],
        commands: vec![
            PlannedCommand::new(
                "iwctl",
                [
                    "device",
                    profile.ap_interface.as_str(),
                    "set-property",
                    "Mode",
                    "ap",
                ],
            ),
            PlannedCommand::new(
                "iwctl",
                [
                    "ap",
                    profile.ap_interface.as_str(),
                    "start-profile",
                    profile.ssid.as_str(),
                ],
            ),
        ],
        tools: vec!["iwd"],
    }
}

fn hostapd_plan(profile: &Profile, paths: &Paths, existing: bool) -> ApplyPlan {
    let hostapd_conf = if existing {
        paths.hostapd_config.clone()
    } else {
        paths.direct_hostapd_conf()
    };
    let files = vec![
        PlanFile::plain(hostapd_conf.clone(), hostapd_conf_text(profile), 0o600),
        PlanFile::plain(paths.dnsmasq_conf(), dnsmasq_conf_text(profile), 0o644),
        PlanFile::plain(paths.nft_path(), nft_text(profile), 0o644),
    ];
    let gateway = profile
        .network()
        .and_then(|network| {
            let prefix = network.prefix();
            network.gateway().map(|addr| format!("{addr}/{prefix}"))
        })
        .unwrap_or_else(|_| profile.address_cidr.clone());
    let mut commands = vec![
        PlannedCommand::new("ip", ["link", "set", profile.ap_interface.as_str(), "up"]),
        PlannedCommand::new(
            "ip",
            [
                "addr",
                "replace",
                gateway.as_str(),
                "dev",
                profile.ap_interface.as_str(),
            ],
        ),
        PlannedCommand::new(
            "nft",
            ["-f", paths.nft_path().to_str().unwrap_or("hotmon.nft")],
        ),
    ];
    if profile.upstream_interface != "none" {
        commands.push(PlannedCommand::new(
            "hotmon-forward",
            [
                profile.ap_interface.as_str(),
                profile.upstream_interface.as_str(),
            ],
        ));
    }
    if profile.dhcp_enabled {
        commands.push(PlannedCommand::new(
            "dnsmasq",
            [
                format!("--conf-file={}", paths.dnsmasq_conf().display()),
                format!("--pid-file={}", paths.dnsmasq_pid().display()),
                "--user=nobody".to_string(),
                format!("--dhcp-leasefile={}", paths.dnsmasq_lease().display()),
            ],
        ));
    }
    if existing {
        commands.push(PlannedCommand::new("systemctl", ["restart", "hostapd"]));
    } else {
        commands.push(PlannedCommand::new(
            "hostapd",
            [
                "-B",
                "-P",
                paths.hostapd_pid().to_str().unwrap_or("hostapd.pid"),
                hostapd_conf.to_str().unwrap_or("hostapd.conf"),
            ],
        ));
    }
    let tools: Vec<&str> = if existing {
        vec!["hostapd", "dnsmasq", "nftables"]
    } else {
        direct_tools().to_vec()
    };
    ApplyPlan {
        files,
        commands,
        tools,
    }
}

pub fn hostapd_conf_text(profile: &Profile) -> String {
    let hw_mode = match profile.band {
        Band::Band24 => "g",
        Band::Band5 => "a",
    };
    let mut text = format!(
        "interface={}\ndriver=nl80211\nssid={}\nhw_mode={hw_mode}\nchannel={}\nauth_algs=1\nignore_broadcast_ssid=0\n",
        profile.ap_interface, profile.ssid, profile.channel
    );
    match profile.security {
        SecurityMode::Open => {}
        SecurityMode::Wpa2 => {
            text.push_str(&format!(
                "wpa=2\nwpa_passphrase={}\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n",
                profile.passphrase
            ));
        }
        SecurityMode::Wpa3 => {
            text.push_str(&format!(
                "wpa=2\nwpa_passphrase={}\nsae_password={}\nwpa_key_mgmt=SAE\nieee80211w=2\nrsn_pairwise=CCMP\n",
                profile.passphrase, profile.passphrase
            ));
        }
    }
    text
}

pub fn dnsmasq_conf_text(profile: &Profile) -> String {
    let network = profile.network().ok();
    let listen = network
        .and_then(|network| network.gateway().ok())
        .map(|addr| addr.to_string())
        .unwrap_or_else(|| "0.0.0.0".to_string());
    let mut text = format!(
        "interface={}\nbind-interfaces\nexcept-interface=lo\nlisten-address={listen}\n",
        profile.ap_interface
    );
    if profile.dhcp_enabled {
        let netmask = network
            .map(|network| network.netmask().to_string())
            .unwrap_or_else(|| "255.255.255.0".to_string());
        text.push_str(&format!(
            "dhcp-range={},{},{netmask},12h\n",
            profile.dhcp_start, profile.dhcp_end
        ));
    } else {
        text.push_str("port=0\n");
    }
    text
}

pub fn nft_text(profile: &Profile) -> String {
    let ap = &profile.ap_interface;
    let dns = profile
        .network()
        .and_then(|network| network.gateway())
        .map(|addr| {
            format!(
                "    iifname \"{ap}\" ip daddr {addr} udp dport 53 accept\n    iifname \"{ap}\" ip daddr {addr} tcp dport 53 accept\n"
            )
        })
        .unwrap_or_default();
    let input = format!(
        "  chain input {{\n    type filter hook input priority 0; policy accept;\n    iifname \"{ap}\" udp dport 67 accept\n{dns}    iifname \"{ap}\" ct state new drop\n  }}\n"
    );
    if profile.upstream_interface == "none" {
        return format!(
            "add table inet hotmon\ndelete table inet hotmon\ntable inet hotmon {{\n{input}  chain forward {{\n    type filter hook forward priority 0; policy accept;\n    iifname \"{ap}\" drop\n  }}\n}}\n"
        );
    }
    let up = &profile.upstream_interface;
    format!(
        "add table inet hotmon\ndelete table inet hotmon\ntable inet hotmon {{\n{input}  chain forward {{\n    type filter hook forward priority 0; policy accept;\n    iifname \"{ap}\" oifname \"{up}\" accept\n    iifname \"{up}\" oifname \"{ap}\" ct state established,related accept\n    iifname \"{ap}\" drop\n  }}\n  chain postrouting {{\n    type nat hook postrouting priority 100; policy accept;\n    iifname \"{ap}\" oifname \"{up}\" masquerade\n  }}\n}}\n"
    )
}

pub fn iwd_profile(profile: &Profile) -> String {
    let network = profile.network().ok();
    let address = network
        .and_then(|network| network.gateway().ok())
        .map(|addr| addr.to_string())
        .unwrap_or_else(|| "192.168.42.1".to_string());
    let netmask = network
        .map(|network| network.netmask().to_string())
        .unwrap_or_else(|| "255.255.255.0".to_string());
    let mut text = format!("[General]\nChannel={}\n", profile.channel);
    if profile.security != SecurityMode::Open {
        text.push_str(&format!(
            "\n[Security]\nPassphrase={}\n",
            profile.passphrase
        ));
    }
    text.push_str(&format!(
        "\n[IPv4]\nAddress={address}\nGateway={address}\nNetmask={netmask}\n"
    ));
    if profile.dhcp_enabled {
        text.push_str(&format!(
            "IPRange={},{}",
            profile.dhcp_start, profile.dhcp_end
        ));
        text.push('\n');
    }
    text
}

#[cfg(test)]
mod tests {
    use std::os::unix::fs::PermissionsExt;

    use super::*;
    use crate::profile::{sample_profile, scratch_dir};

    fn facts(nm: bool, iwd: bool, hostapd: bool) -> ProbeFacts {
        ProbeFacts {
            network_manager_active: nm,
            iwd_active: iwd,
            hostapd_setup: hostapd,
        }
    }

    #[test]
    fn backend_selection_follows_the_service_order() {
        assert_eq!(
            select_backend(&facts(true, true, true)),
            BackendKind::NetworkManager
        );
        assert_eq!(select_backend(&facts(false, true, true)), BackendKind::Iwd);
        assert_eq!(
            select_backend(&facts(false, false, true)),
            BackendKind::ExistingHostapd
        );
        assert_eq!(
            select_backend(&facts(false, false, false)),
            BackendKind::Direct
        );
        assert_eq!(direct_tools(), ["hostapd", "dnsmasq", "nftables"]);
    }

    #[test]
    fn direct_plan_uses_hostapd_dnsmasq_and_nftables() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let plan = plan_apply(BackendKind::Direct, &sample_profile(), &paths).unwrap();
        assert_eq!(plan.tools, direct_tools());
        assert!(
            plan.commands
                .iter()
                .any(|command| command.program == "hostapd")
        );
        assert!(
            plan.commands
                .iter()
                .any(|command| command.program == "dnsmasq")
        );
        assert!(plan.commands.iter().any(|command| command.program == "nft"));
        let hostapd = plan
            .files
            .iter()
            .find(|file| file.path.ends_with("hostapd.conf"))
            .unwrap();
        assert!(hostapd.contents.contains("ssid=Hotmon"));
        assert!(hostapd.contents.contains("wpa_passphrase=correct-horse"));
        let masq = plan
            .files
            .iter()
            .find_map(|file| {
                file.contents
                    .lines()
                    .find(|line| line.contains("masquerade"))
                    .map(str::to_string)
            })
            .unwrap();
        assert!(masq.contains("wlan0"), "{masq}");
        assert!(masq.contains("eth0"), "{masq}");
        let names: Vec<&str> = plan
            .commands
            .iter()
            .map(|command| command.program.as_str())
            .collect();
        let nft = names.iter().position(|name| *name == "nft").unwrap();
        let forward = names
            .iter()
            .position(|name| *name == "hotmon-forward")
            .unwrap();
        let dnsmasq = names.iter().position(|name| *name == "dnsmasq").unwrap();
        let hostapd_at = names.iter().position(|name| *name == "hostapd").unwrap();
        assert!(nft < forward && forward < dnsmasq && dnsmasq < hostapd_at);
        let dnsmasq_command = plan
            .commands
            .iter()
            .find(|command| command.program == "dnsmasq")
            .unwrap();
        assert!(
            dnsmasq_command
                .args
                .iter()
                .any(|arg| arg == "--user=nobody")
        );
        assert!(dnsmasq_command.args.iter().all(|arg| arg != "--user=root"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn open_hostapd_config_has_no_passphrase() {
        let mut profile = sample_profile();
        profile.security = SecurityMode::Open;
        profile.passphrase.clear();
        profile.dhcp_enabled = false;
        let text = hostapd_conf_text(&profile);
        assert!(!text.contains("wpa_passphrase"));
        assert!(
            nft_text(&{
                let mut isolated = profile.clone();
                isolated.upstream_interface = "none".to_string();
                isolated
            })
            .contains("drop")
        );
    }

    #[test]
    fn networkmanager_plan_uses_nmcli_only() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.clone(),
            hostapd_config: dir.join("h.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let plan = plan_apply(BackendKind::NetworkManager, &sample_profile(), &paths).unwrap();
        assert!(
            plan.commands
                .iter()
                .all(|command| command.program == "nmcli")
        );
        assert!(plan.files[0].contents.contains("ssid=Hotmon"));
        assert!(plan.files[0].contents.contains("key-mgmt=wpa-psk"));
        assert!(plan.files[0].contents.contains("psk=correct-horse"));
        assert!(plan.commands.iter().all(|command| {
            command
                .args
                .iter()
                .all(|arg| !arg.contains("correct-horse"))
        }));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn iwd_plan_writes_the_access_point_profile() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("h.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let plan = plan_apply(BackendKind::Iwd, &sample_profile(), &paths).unwrap();
        assert!(
            plan.commands
                .iter()
                .all(|command| command.program == "iwctl")
        );
        let file = &plan.files[0];
        assert!(file.contents.contains("Channel=6"));
        assert!(file.contents.contains("Passphrase=correct-horse"));
        assert!(
            file.contents
                .contains("IPRange=192.168.42.10,192.168.42.100")
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn backend_rejection_is_a_clear_error() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.clone(),
            hostapd_config: dir.join("h.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let mut profile = sample_profile();
        profile.channel = 2;
        profile.band = crate::profile::Band::Band5;
        let error = plan_apply(BackendKind::Direct, &profile, &paths).unwrap_err();
        assert!(error.contains("The backend rejected the setting."));
        let plan = plan_apply(BackendKind::NetworkManager, &sample_profile(), &paths).unwrap();
        let mut runner = ScriptedRunner {
            results: vec![
                Err("not found".to_string()),
                Err("channel is not supported".to_string()),
            ],
            ..ScriptedRunner::default()
        };
        let error =
            execute_plan(&plan, &mut runner, &mut RecordedSignals::default(), &paths).unwrap_err();
        assert!(error.contains("The backend rejected the setting."));
        assert!(error.contains("channel is not supported"));
        assert_eq!(runner.calls.len(), 2);
        assert!(!paths.nm_secret().exists());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn existing_hostapd_uses_systemctl() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("etc").join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let plan = plan_apply(BackendKind::ExistingHostapd, &sample_profile(), &paths).unwrap();
        assert!(plan.commands.iter().any(|command| {
            command.program == "systemctl" && command.args.contains(&"restart".to_string())
        }));
        assert!(
            plan.files
                .iter()
                .any(|file| file.path == paths.hostapd_config)
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    fn mode_bits(path: &std::path::Path) -> u32 {
        std::fs::metadata(path).unwrap().permissions().mode() & 0o777
    }

    fn assert_no_secret_args(plan: &ApplyPlan, secret: &str) {
        for command in &plan.commands {
            assert!(!command.program.contains(secret), "{}", command.program);
            for arg in &command.args {
                assert!(!arg.contains(secret), "{} {arg}", command.program);
            }
        }
    }

    #[test]
    fn planned_arguments_do_not_contain_the_passphrase() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        for kind in [
            BackendKind::NetworkManager,
            BackendKind::Iwd,
            BackendKind::ExistingHostapd,
            BackendKind::Direct,
        ] {
            let plan = plan_apply(kind, &sample_profile(), &paths).unwrap();
            assert_no_secret_args(&plan, "correct-horse");
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn secret_files_use_private_modes_and_dnsmasq_does_not() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let plan = plan_apply(BackendKind::Direct, &sample_profile(), &paths).unwrap();
        execute_plan(
            &plan,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        assert_eq!(mode_bits(&paths.state_dir), 0o700);
        let hostapd = paths.direct_hostapd_conf();
        assert_eq!(mode_bits(&hostapd), 0o600);
        let dnsmasq = paths.dnsmasq_conf();
        assert_eq!(mode_bits(&dnsmasq) & 0o004, 0o004);
        assert_ne!(dnsmasq, hostapd);
        let dns_text = std::fs::read_to_string(&dnsmasq).unwrap();
        assert!(!dns_text.contains("correct-horse"));
        assert!(!dns_text.contains("wpa_passphrase"));
        let iwd = plan_apply(BackendKind::Iwd, &sample_profile(), &paths).unwrap();
        execute_plan(
            &iwd,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        assert_eq!(mode_bits(&iwd.files[0].path), 0o600);
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }

    #[test]
    fn firewall_limits_hotspot_forward_and_input() {
        let text = nft_text(&sample_profile());
        let masq = text
            .lines()
            .find(|line| line.contains("masquerade"))
            .unwrap();
        assert!(masq.contains("wlan0") && masq.contains("eth0"), "{masq}");
        assert!(text.contains("iifname \"wlan0\" oifname \"eth0\" accept"));
        assert!(
            text.contains("iifname \"eth0\" oifname \"wlan0\" ct state established,related accept")
        );
        assert!(text.contains("iifname \"wlan0\" drop"));
        assert!(text.contains("udp dport 67 accept"));
        assert!(text.contains("ip daddr 192.168.42.1 udp dport 53 accept"));
        assert!(text.contains("ip daddr 192.168.42.1 tcp dport 53 accept"));
        assert!(text.contains("iifname \"wlan0\" ct state new drop"));
        assert!(!text.contains("policy drop"));
        for line in text.lines().filter(|line| line.contains("drop")) {
            assert!(line.contains("wlan0"), "{line}");
        }
        let mut isolated = sample_profile();
        isolated.upstream_interface = "none".to_string();
        let isolated_text = nft_text(&isolated);
        assert!(!isolated_text.contains("masquerade"));
        assert!(isolated_text.contains("iifname \"wlan0\" drop"));
        assert!(!isolated_text.contains("policy drop"));
    }

    #[test]
    fn forwarding_changes_only_the_hotspot_and_upstream() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let all = paths.proc_root.join("sys/net/ipv4/conf/all/forwarding");
        std::fs::create_dir_all(all.parent().unwrap()).unwrap();
        std::fs::write(&all, "0\n").unwrap();
        enable_forwarding(&paths, &["wlan0".to_string(), "eth0".to_string()]).unwrap();
        let ap = std::fs::read_to_string(forwarding_path(&paths, "wlan0")).unwrap();
        let up = std::fs::read_to_string(forwarding_path(&paths, "eth0")).unwrap();
        assert_eq!(ap.trim(), "1");
        assert_eq!(up.trim(), "1");
        assert_eq!(std::fs::read_to_string(&all).unwrap().trim(), "0");
        restore_forwarding(&paths).unwrap();
        assert_eq!(
            std::fs::read_to_string(forwarding_path(&paths, "wlan0"))
                .unwrap()
                .trim(),
            "0"
        );
        assert!(!paths.forwarding_record().exists());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn failed_start_removes_the_firewall_and_the_daemon() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("kept.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        std::fs::write(&paths.hostapd_config, "leave-this\n").unwrap();
        let plan = plan_apply(BackendKind::Direct, &sample_profile(), &paths).unwrap();
        let pid_path = paths.dnsmasq_pid();
        let mut runner = PidOnSuccess {
            inner: ScriptedRunner::with_results(vec![
                Ok(String::new()),
                Ok(String::new()),
                Ok(String::new()),
                Ok(String::new()),
                Err("hostapd failed".to_string()),
            ]),
            pid_path,
            pid: 77,
        };
        let mut signals = RecordedSignals {
            names: vec![(77, "dnsmasq".to_string())],
            ..RecordedSignals::default()
        };
        let error = execute_plan(&plan, &mut runner, &mut signals, &paths).unwrap_err();
        assert!(error.contains("The backend rejected the setting."));
        assert!(runner.inner.calls.iter().any(|command| {
            command.program == "nft" && command.args.iter().any(|arg| arg == "delete")
        }));
        assert_eq!(signals.pids, vec![77]);
        assert!(!signals.running(77));
        assert!(!paths.direct_hostapd_conf().exists());
        assert_eq!(
            std::fs::read_to_string(&paths.hostapd_config).unwrap(),
            "leave-this\n"
        );
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }

    #[test]
    fn stop_does_not_signal_a_foreign_process() {
        let mut foreign = RecordedSignals {
            names: vec![(42, "bash".to_string())],
            ..RecordedSignals::default()
        };
        stop_started(
            &mut foreign,
            &[StartedProc {
                pid: 42,
                name: "hostapd".to_string(),
            }],
        )
        .unwrap();
        assert!(foreign.pids.is_empty());
        let mut other = RecordedSignals {
            names: vec![(99, "hostapd".to_string())],
            ..RecordedSignals::default()
        };
        stop_started(&mut other, &[]).unwrap();
        assert!(other.pids.is_empty());
        let mut alive = RecordedSignals {
            names: vec![(7, "dnsmasq".to_string())],
            stay_alive: true,
            ..RecordedSignals::default()
        };
        let error = stop_started(
            &mut alive,
            &[StartedProc {
                pid: 7,
                name: "dnsmasq".to_string(),
            }],
        )
        .unwrap_err();
        assert!(error.contains("not stopped"));
        assert!(error.contains("still running"));
    }

    #[test]
    fn hostapd_system_file_is_restored_from_the_backup() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("etc").join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        std::fs::create_dir_all(paths.hostapd_config.parent().unwrap()).unwrap();
        std::fs::write(&paths.hostapd_config, "original-config\n").unwrap();
        let plan = plan_apply(BackendKind::ExistingHostapd, &sample_profile(), &paths).unwrap();
        execute_plan(
            &plan,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        let changed = std::fs::read_to_string(&paths.hostapd_config).unwrap();
        assert!(changed.contains("ssid=Hotmon"));
        assert_eq!(mode_bits(&paths.hostapd_backup()), 0o600);
        restore_hostapd_backup(&paths).unwrap();
        assert_eq!(
            std::fs::read_to_string(&paths.hostapd_config).unwrap(),
            "original-config\n"
        );
        assert!(!paths.hostapd_backup().exists());
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }

    #[test]
    fn a_missing_backup_does_not_change_the_system_file() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("etc").join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        std::fs::create_dir_all(paths.hostapd_config.parent().unwrap()).unwrap();
        std::fs::write(&paths.hostapd_config, "original-config\n").unwrap();
        std::fs::create_dir_all(&paths.state_dir).unwrap();
        std::fs::create_dir_all(paths.hostapd_backup()).unwrap();
        let plan = plan_apply(BackendKind::ExistingHostapd, &sample_profile(), &paths).unwrap();
        execute_plan(
            &plan,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        assert_eq!(
            std::fs::read_to_string(&paths.hostapd_config).unwrap(),
            "original-config\n"
        );
        assert!(paths.direct_hostapd_conf().exists());
        assert_eq!(mode_bits(&paths.direct_hostapd_conf()), 0o600);
        let hostapd = plan
            .commands
            .iter()
            .find(|command| command.program == "systemctl")
            .is_some();
        assert!(hostapd);
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }

    #[test]
    fn daemon_name_accepts_only_hostapd_and_dnsmasq() {
        assert_eq!(
            daemon_name(Some("hostapd\n"), None).as_deref(),
            Some("hostapd")
        );
        assert_eq!(daemon_name(Some("nginx\n"), None).as_deref(), Some("nginx"));
        assert_eq!(
            daemon_name(None, Some(b"/usr/sbin/dnsmasq\0--conf-file=/run/x\0")).as_deref(),
            Some("dnsmasq")
        );
        assert_eq!(daemon_name(None, None), None);
    }

    #[test]
    fn nm_secret_file_is_removed_after_the_call() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("h.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let plan = plan_apply(BackendKind::NetworkManager, &sample_profile(), &paths).unwrap();
        let mut runner = ModeCheck {
            saw_private_secret: false,
        };
        execute_plan(&plan, &mut runner, &mut RecordedSignals::default(), &paths).unwrap();
        assert!(runner.saw_private_secret);
        assert!(!paths.nm_secret().exists());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_failed_file_install_removes_the_secret() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("kept.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        std::fs::create_dir_all(&paths.state_dir).unwrap();
        std::fs::create_dir_all(paths.nft_path()).unwrap();
        let plan = plan_apply(BackendKind::Direct, &sample_profile(), &paths).unwrap();
        let error = execute_plan(
            &plan,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap_err();
        assert!(error.contains("cannot write"));
        assert!(!paths.direct_hostapd_conf().exists());
        let dns = paths.dnsmasq_conf();
        if dns.exists() {
            assert!(!std::fs::read_to_string(dns).unwrap().contains("correct-horse"));
        }
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }

    #[test]
    fn a_failed_file_install_restores_the_system_file() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("etc").join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        std::fs::create_dir_all(paths.hostapd_config.parent().unwrap()).unwrap();
        std::fs::write(&paths.hostapd_config, "original-config\n").unwrap();
        std::fs::create_dir_all(&paths.state_dir).unwrap();
        std::fs::create_dir_all(paths.nft_path()).unwrap();
        let plan = plan_apply(BackendKind::ExistingHostapd, &sample_profile(), &paths).unwrap();
        let error = execute_plan(
            &plan,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap_err();
        assert!(error.contains("cannot write"));
        assert_eq!(
            std::fs::read_to_string(&paths.hostapd_config).unwrap(),
            "original-config\n"
        );
        assert!(!paths.hostapd_backup().exists());
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }

    #[test]
    fn a_missing_system_file_uses_the_private_file() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("etc").join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        std::fs::create_dir_all(paths.hostapd_config.parent().unwrap()).unwrap();
        let plan = plan_apply(BackendKind::ExistingHostapd, &sample_profile(), &paths).unwrap();
        let mut runner = ScriptedRunner::default();
        let report = execute_plan(&plan, &mut runner, &mut RecordedSignals::default(), &paths)
            .unwrap();
        assert!(report.private_hostapd);
        assert!(!paths.hostapd_config.exists());
        assert!(paths.direct_hostapd_conf().exists());
        assert!(runner.calls.iter().any(|command| command.program == "hostapd"));
        assert!(!runner
            .calls
            .iter()
            .any(|command| command.program == "systemctl"));
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }

    #[test]
    fn a_failed_forwarding_change_restores_the_first_interface() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let ap = forwarding_path(&paths, "wlan0");
        std::fs::create_dir_all(ap.parent().unwrap()).unwrap();
        std::fs::write(&ap, "0\n").unwrap();
        let upstream = paths.proc_root.join("sys/net/ipv4/conf/eth0");
        std::fs::create_dir_all(upstream.parent().unwrap()).unwrap();
        std::fs::write(&upstream, "not-a-directory\n").unwrap();
        let plan = plan_apply(BackendKind::Direct, &sample_profile(), &paths).unwrap();
        let error = execute_plan(
            &plan,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap_err();
        assert!(error.contains("hotmon-forward"));
        assert_eq!(std::fs::read_to_string(&ap).unwrap().trim(), "0");
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }

    #[test]
    fn a_failed_reapply_keeps_the_live_firewall_and_forwarding_record() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let ap = forwarding_path(&paths, "wlan0");
        let upstream = forwarding_path(&paths, "eth0");
        std::fs::create_dir_all(ap.parent().unwrap()).unwrap();
        std::fs::create_dir_all(upstream.parent().unwrap()).unwrap();
        std::fs::write(&ap, "0\n").unwrap();
        std::fs::write(&upstream, "0\n").unwrap();
        let plan = plan_apply(BackendKind::Direct, &sample_profile(), &paths).unwrap();
        execute_plan(
            &plan,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        let saved = std::fs::read_to_string(paths.forwarding_record()).unwrap();
        assert!(saved.contains("wlan0 0"));
        let mut runner = ScriptedRunner::with_results(vec![
            Ok(String::new()),
            Ok(String::new()),
            Ok(String::new()),
            Err("dnsmasq failed".to_string()),
        ]);
        let error = execute_plan(
            &plan,
            &mut runner,
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap_err();
        assert!(error.contains("dnsmasq"));
        assert_eq!(
            std::fs::read_to_string(paths.forwarding_record()).unwrap(),
            saved
        );
        assert!(!runner.calls.iter().any(|command| {
            command.program == "nft" && command.args.iter().any(|arg| arg == "delete")
        }));
        assert!(runner.calls.iter().any(|command| {
            command.program == "nft"
                && command
                    .args
                    .iter()
                    .any(|arg| arg.ends_with("hotmon.nft.previous"))
        }));
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }

    #[test]
    fn a_failed_start_after_stop_deletes_the_firewall_table() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let plan = plan_apply(BackendKind::Direct, &sample_profile(), &paths).unwrap();
        execute_plan(
            &plan,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        run_nft_delete(&mut ScriptedRunner::default()).unwrap();
        clear_nft_live(&paths);
        let mut runner = ScriptedRunner::with_results(vec![
            Ok(String::new()),
            Ok(String::new()),
            Ok(String::new()),
            Err("dnsmasq failed".to_string()),
        ]);
        let error = execute_plan(
            &plan,
            &mut runner,
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap_err();
        assert!(error.contains("dnsmasq"));
        assert!(runner.calls.iter().any(|command| {
            command.program == "nft" && command.args.iter().any(|arg| arg == "delete")
        }));
        assert!(!runner.calls.iter().any(|command| {
            command
                .args
                .iter()
                .any(|arg| arg.ends_with("hotmon.nft.previous"))
        }));
        assert!(!paths.nft_live_marker().exists());
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }

    #[test]
    fn a_second_failed_reapply_reloads_the_live_rules() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let plan = plan_apply(BackendKind::Direct, &sample_profile(), &paths).unwrap();
        execute_plan(
            &plan,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        let mut changed = sample_profile();
        changed.upstream_interface = "eth1".to_string();
        let next = plan_apply(BackendKind::Direct, &changed, &paths).unwrap();
        let fail = || {
            ScriptedRunner::with_results(vec![
                Ok(String::new()),
                Ok(String::new()),
                Ok(String::new()),
                Err("dnsmasq failed".to_string()),
            ])
        };
        execute_plan(&next, &mut fail(), &mut RecordedSignals::default(), &paths).unwrap_err();
        execute_plan(&next, &mut fail(), &mut RecordedSignals::default(), &paths).unwrap_err();
        let text = std::fs::read_to_string(paths.nft_path()).unwrap();
        assert!(text.contains("oifname \"eth0\""));
        assert!(!text.contains("eth1"));
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }

    #[test]
    fn a_second_apply_keeps_the_original_hostapd_backup() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("etc").join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        std::fs::create_dir_all(paths.hostapd_config.parent().unwrap()).unwrap();
        std::fs::write(&paths.hostapd_config, "original-config\n").unwrap();
        let plan = plan_apply(BackendKind::ExistingHostapd, &sample_profile(), &paths).unwrap();
        execute_plan(
            &plan,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        execute_plan(
            &plan,
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        assert_eq!(
            std::fs::read_to_string(paths.hostapd_backup()).unwrap(),
            "original-config\n"
        );
        restore_hostapd_backup(&paths).unwrap();
        assert_eq!(
            std::fs::read_to_string(&paths.hostapd_config).unwrap(),
            "original-config\n"
        );
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(paths.dnsmasq_dir());
    }
}

#[cfg(test)]
struct PidOnSuccess {
    inner: ScriptedRunner,
    pid_path: std::path::PathBuf,
    pid: i32,
}

#[cfg(test)]
impl Runner for PidOnSuccess {
    fn run(&mut self, command: &PlannedCommand) -> Result<String, String> {
        let result = self.inner.run(command);
        if command.program == "dnsmasq" && result.is_ok() {
            if let Some(parent) = self.pid_path.parent() {
                let _ = std::fs::create_dir_all(parent);
            }
            let _ = std::fs::write(&self.pid_path, format!("{}\n", self.pid));
        }
        result
    }
}

#[cfg(test)]
struct ModeCheck {
    saw_private_secret: bool,
}

#[cfg(test)]
impl Runner for ModeCheck {
    fn run(&mut self, command: &PlannedCommand) -> Result<String, String> {
        if command.args.iter().any(|arg| arg == "load") {
            let path = command.args.last().unwrap();
            let mode = std::fs::metadata(path).unwrap().permissions().mode() & 0o777;
            assert_eq!(mode, 0o600);
            let text = std::fs::read_to_string(path).unwrap();
            assert!(text.contains("psk=correct-horse"));
            assert!(
                command
                    .args
                    .iter()
                    .all(|arg| !arg.contains("correct-horse"))
            );
            self.saw_private_secret = true;
        }
        Ok(String::new())
    }
}
