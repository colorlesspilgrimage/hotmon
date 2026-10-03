use std::path::{Path, PathBuf};
use std::process::Command;

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
}

impl Paths {
    pub fn system() -> Self {
        Self {
            state_dir: PathBuf::from("/run/hotmon"),
            hostapd_config: PathBuf::from("/etc/hostapd/hostapd.conf"),
            iwd_ap_dir: PathBuf::from("/var/lib/iwd/ap"),
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

    pub fn dnsmasq_conf(&self) -> PathBuf {
        self.state_dir.join("dnsmasq.conf")
    }

    pub fn nft_path(&self) -> PathBuf {
        self.state_dir.join("hotmon.nft")
    }
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

pub trait ProcessControl {
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

fn require_pid(pid: i32) -> Result<(), String> {
    if pid <= 0 {
        Err(format!("The process id {pid} is not valid."))
    } else {
        Ok(())
    }
}

impl ProcessControl for SystemSignals {
    fn terminate(&mut self, pid: i32) -> Result<(), String> {
        require_pid(pid)?;
        let rc = unsafe { libc::kill(pid, libc::SIGTERM) };
        if rc == 0 {
            Ok(())
        } else {
            Err(format!(
                "The process {pid} did not stop. {}",
                std::io::Error::last_os_error()
            ))
        }
    }
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
}

#[cfg(test)]
impl ProcessControl for RecordedSignals {
    fn terminate(&mut self, pid: i32) -> Result<(), String> {
        require_pid(pid)?;
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
        BackendKind::NetworkManager => Ok(nm_plan(profile)),
        BackendKind::Iwd => Ok(iwd_plan(profile, paths)),
        BackendKind::ExistingHostapd => Ok(hostapd_plan(profile, paths, true)),
        BackendKind::Direct => Ok(hostapd_plan(profile, paths, false)),
    }
}

pub fn plan_stop(kind: BackendKind, profile: &Profile) -> Vec<PlannedCommand> {
    match kind {
        BackendKind::NetworkManager => vec![PlannedCommand::new(
            "nmcli",
            ["connection", "down", "hotmon"],
        )],
        BackendKind::Iwd => vec![PlannedCommand::new(
            "iwctl",
            ["ap", profile.ap_interface.as_str(), "stop"],
        )],
        BackendKind::ExistingHostapd => vec![
            PlannedCommand::new("systemctl", ["stop", "hostapd"]),
            delete_hotmon_table(),
        ],
        BackendKind::Direct => vec![delete_hotmon_table()],
    }
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

pub fn execute_plan(plan: &ApplyPlan, runner: &mut dyn Runner) -> Result<(), String> {
    for file in &plan.files {
        crate::profile::prepare_and_write(&file.path, &file.contents)?;
    }
    for command in &plan.commands {
        if let Err(message) = runner.run(command) {
            if command.optional {
                continue;
            }
            return Err(format!(
                "The backend rejected the setting. {} failed: {message}",
                command.program
            ));
        }
    }
    Ok(())
}

pub fn terminate_pids(signals: &mut dyn ProcessControl, pids: &[i32]) -> Result<(), String> {
    for pid in pids {
        signals
            .terminate(*pid)
            .map_err(|err| format!("The backend rejected the stop request. {err}"))?;
    }
    Ok(())
}

fn delete_hotmon_table() -> PlannedCommand {
    PlannedCommand::new("nft", ["delete", "table", "inet", "hotmon"]).optional()
}

fn push_wifi_security(add: &mut Vec<String>, profile: &Profile) {
    let mgmt = match profile.security {
        SecurityMode::Open => return,
        SecurityMode::Wpa2 => "wpa-psk",
        SecurityMode::Wpa3 => "sae",
    };
    add.extend([
        "wifi-sec.key-mgmt".to_string(),
        mgmt.to_string(),
        "wifi-sec.psk".to_string(),
        profile.passphrase.clone(),
    ]);
}

fn append_hostapd_security(text: &mut String, profile: &Profile) {
    let (key_mgmt, sae) = match profile.security {
        SecurityMode::Open => return,
        SecurityMode::Wpa2 => ("WPA-PSK", false),
        SecurityMode::Wpa3 => ("SAE", true),
    };
    text.push_str(&format!("wpa=2\nwpa_passphrase={}\n", profile.passphrase));
    if sae {
        text.push_str(&format!("sae_password={}\n", profile.passphrase));
    }
    text.push_str(&format!("wpa_key_mgmt={key_mgmt}\n"));
    if sae {
        text.push_str("ieee80211w=2\n");
    }
    text.push_str("rsn_pairwise=CCMP\n");
}

fn nm_plan(profile: &Profile) -> ApplyPlan {
    let band = match profile.band {
        Band::Band24 => "bg",
        Band::Band5 => "a",
    };
    let method = if profile.dhcp_enabled {
        "shared"
    } else {
        "manual"
    };
    let gateway = profile.gateway_cidr();
    let mut add = vec![
        "connection".to_string(),
        "add".to_string(),
        "type".to_string(),
        "wifi".to_string(),
        "ifname".to_string(),
        profile.ap_interface.clone(),
        "con-name".to_string(),
        "hotmon".to_string(),
        "autoconnect".to_string(),
        "no".to_string(),
        "ssid".to_string(),
        profile.ssid.clone(),
        "802-11-wireless.mode".to_string(),
        "ap".to_string(),
        "802-11-wireless.band".to_string(),
        band.to_string(),
        "802-11-wireless.channel".to_string(),
        profile.channel.to_string(),
        "ipv4.method".to_string(),
        method.to_string(),
        "ipv4.addresses".to_string(),
        gateway,
    ];
    push_wifi_security(&mut add, profile);
    ApplyPlan {
        files: Vec::new(),
        commands: vec![
            PlannedCommand::new("nmcli", ["connection", "delete", "hotmon"]).optional(),
            PlannedCommand::new("nmcli", add),
            PlannedCommand::new("nmcli", ["connection", "up", "hotmon"]),
        ],
        tools: vec!["NetworkManager"],
    }
}

fn iwd_plan(profile: &Profile, paths: &Paths) -> ApplyPlan {
    let path = paths.iwd_ap_dir.join(format!("{}.ap", profile.ssid));
    ApplyPlan {
        files: vec![PlanFile {
            path,
            contents: iwd_profile(profile),
        }],
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
        PlanFile {
            path: hostapd_conf.clone(),
            contents: hostapd_conf_text(profile),
        },
        PlanFile {
            path: paths.dnsmasq_conf(),
            contents: dnsmasq_conf_text(profile),
        },
        PlanFile {
            path: paths.nft_path(),
            contents: nft_text(profile),
        },
    ];
    let gateway = profile.gateway_cidr();
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
    if profile.dhcp_enabled {
        commands.push(PlannedCommand::new(
            "dnsmasq",
            [
                format!("--conf-file={}", paths.dnsmasq_conf().display()),
                format!("--pid-file={}", paths.dnsmasq_pid().display()),
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
    let tools = direct_tools().to_vec();
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
    append_hostapd_security(&mut text, profile);
    text
}

pub fn dnsmasq_conf_text(profile: &Profile) -> String {
    let listen = profile.gateway_text("0.0.0.0");
    let mut text = format!(
        "interface={}\nbind-interfaces\nexcept-interface=lo\nlisten-address={listen}\n",
        profile.ap_interface
    );
    if profile.dhcp_enabled {
        let netmask = profile.netmask_text("255.255.255.0");
        text.push_str(&format!(
            "dhcp-range={},{},{netmask},12h\n",
            profile.dhcp_start, profile.dhcp_end
        ));
    } else {
        text.push_str("port=0\n");
    }
    text
}

fn nft_table(body: &str) -> String {
    format!("add table inet hotmon\ndelete table inet hotmon\ntable inet hotmon {{\n{body}}}\n")
}

pub fn nft_text(profile: &Profile) -> String {
    if profile.upstream_interface == "none" {
        return nft_table(&format!(
            "  chain forward {{\n    type filter hook forward priority 0; policy accept;\n    iifname \"{}\" drop\n  }}\n",
            profile.ap_interface
        ));
    }
    nft_table(&format!(
        "  chain postrouting {{\n    type nat hook postrouting priority 100; policy accept;\n    oifname \"{}\" masquerade\n  }}\n  chain forward {{\n    type filter hook forward priority 0; policy accept;\n    iifname \"{}\" oifname \"{}\" accept\n    iifname \"{}\" oifname \"{}\" ct state established,related accept\n  }}\n",
        profile.upstream_interface,
        profile.ap_interface,
        profile.upstream_interface,
        profile.upstream_interface,
        profile.ap_interface
    ))
}

pub fn iwd_profile(profile: &Profile) -> String {
    let address = profile.gateway_text("192.168.42.1");
    let netmask = profile.netmask_text("255.255.255.0");
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
        assert!(
            plan.files
                .iter()
                .any(|file| file.contents.contains("masquerade") && file.contents.contains("eth0"))
        );
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
        };
        let plan = plan_apply(BackendKind::NetworkManager, &sample_profile(), &paths).unwrap();
        assert!(
            plan.commands
                .iter()
                .all(|command| command.program == "nmcli")
        );
        let add = plan
            .commands
            .iter()
            .find(|command| command.args.contains(&"add".to_string()))
            .unwrap();
        assert!(add.args.iter().any(|arg| arg == "Hotmon"));
        assert!(add.args.iter().any(|arg| arg == "wpa-psk"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn iwd_plan_writes_the_access_point_profile() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("h.conf"),
            iwd_ap_dir: dir.join("iwd"),
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
        let error = execute_plan(&plan, &mut runner).unwrap_err();
        assert!(error.contains("The backend rejected the setting."));
        assert!(error.contains("channel is not supported"));
        assert_eq!(runner.calls.len(), 2);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn existing_hostapd_uses_systemctl() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("etc").join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
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
}
