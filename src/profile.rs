use std::fs;
use std::io::Write;
use std::net::Ipv4Addr;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::iface::{self, IfaceInfo};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum SecurityMode {
    Open,
    Wpa2,
    Wpa3,
}

impl SecurityMode {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Open => "open",
            Self::Wpa2 => "wpa2",
            Self::Wpa3 => "wpa3",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Band {
    #[serde(rename = "2.4")]
    Band24,
    #[serde(rename = "5")]
    Band5,
}

impl Band {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Band24 => "2.4",
            Self::Band5 => "5",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Profile {
    pub ap_interface: String,
    pub ssid: String,
    pub security: SecurityMode,
    pub passphrase: String,
    pub band: Band,
    pub channel: u16,
    pub address_cidr: String,
    pub dhcp_enabled: bool,
    pub dhcp_start: String,
    pub dhcp_end: String,
    pub upstream_interface: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ipv4Network {
    address: u32,
    prefix: u8,
}

impl Ipv4Network {
    pub fn parse(text: &str) -> Result<Self, String> {
        let (addr_text, prefix_text) = text.split_once('/').ok_or_else(|| {
            "The address range needs a prefix. Use the form 192.168.42.0/24.".to_string()
        })?;
        let address = parse_ipv4(addr_text)?;
        let prefix: u8 = prefix_text
            .parse()
            .map_err(|_| "The prefix is not a number.".to_string())?;
        if !(8..=30).contains(&prefix) {
            return Err("The prefix must be from 8 to 30.".to_string());
        }
        let network = address & prefix_mask(prefix);
        if network != address {
            return Err(
                "The address range must be a network address, for example 192.168.42.0/24."
                    .to_string(),
            );
        }
        Ok(Self { address, prefix })
    }

    pub fn prefix(self) -> u8 {
        self.prefix
    }

    pub fn gateway(self) -> Result<Ipv4Addr, String> {
        let gateway = self.address.saturating_add(1);
        if !self.usable(gateway) {
            return Err("The address range has no gateway address.".to_string());
        }
        Ok(Ipv4Addr::from(gateway))
    }

    pub fn netmask(self) -> Ipv4Addr {
        Ipv4Addr::from(prefix_mask(self.prefix))
    }

    pub fn usable(self, ip: u32) -> bool {
        let mask = prefix_mask(self.prefix);
        let broadcast = self.address | !mask;
        ip & mask == self.address && ip != self.address && ip != broadcast
    }

    pub fn contains_text(self, text: &str) -> Result<u32, String> {
        let ip = parse_ipv4(text)?;
        if !self.usable(ip) {
            return Err(format!("The address {text} is outside the address range."));
        }
        Ok(ip)
    }
}

pub fn parse_ipv4(text: &str) -> Result<u32, String> {
    let addr: Ipv4Addr = text
        .parse()
        .map_err(|_| format!("The address {text} is not a valid IPv4 address."))?;
    Ok(u32::from(addr))
}

fn prefix_mask(prefix: u8) -> u32 {
    if prefix == 0 {
        0
    } else {
        u32::MAX << (32 - prefix)
    }
}

pub fn parse_security(text: &str) -> Result<SecurityMode, String> {
    match text.trim().to_ascii_lowercase().as_str() {
        "open" => Ok(SecurityMode::Open),
        "wpa2" => Ok(SecurityMode::Wpa2),
        "wpa3" => Ok(SecurityMode::Wpa3),
        _ => Err("The security mode must be open, wpa2, or wpa3.".to_string()),
    }
}

pub fn parse_band(text: &str) -> Result<Band, String> {
    match text.trim().to_ascii_lowercase().as_str() {
        "2.4" | "2.4ghz" => Ok(Band::Band24),
        "5" | "5ghz" => Ok(Band::Band5),
        _ => Err("The band must be 2.4 or 5.".to_string()),
    }
}

pub fn validate_ssid(ssid: &str) -> Result<(), String> {
    if ssid.is_empty() || ssid == "." || ssid == ".." {
        return Err("The SSID must contain 1 to 32 characters.".to_string());
    }
    if ssid.len() > 32 {
        return Err("The SSID must contain 1 to 32 characters.".to_string());
    }
    if ssid.chars().any(|ch| ch.is_control() || ch == '/') {
        return Err("The SSID contains a character that is not allowed.".to_string());
    }
    Ok(())
}

pub const OPEN_UPSTREAM_WARNING: &str = "Every device in radio range can use the upstream network.";

pub fn validate_passphrase(mode: SecurityMode, passphrase: &str) -> Result<(), String> {
    match mode {
        SecurityMode::Open => {
            if passphrase.is_empty() {
                Ok(())
            } else {
                Err("An open network does not use a passphrase.".to_string())
            }
        }
        SecurityMode::Wpa2 | SecurityMode::Wpa3 => {
            let length = passphrase.chars().count();
            if !(8..=63).contains(&length) {
                return Err("The passphrase must contain 8 to 63 characters.".to_string());
            }
            if !passphrase.bytes().all(|byte| (0x20..=0x7e).contains(&byte)) {
                return Err("The passphrase must use printable ASCII characters.".to_string());
            }
            if passphrase.starts_with(' ') || passphrase.ends_with(' ') {
                return Err("The passphrase must not start or end with a space.".to_string());
            }
            if passphrase
                .bytes()
                .any(|byte| matches!(byte, b'\\' | b'"' | b'#'))
            {
                return Err(
                    "The passphrase must not contain a backslash, a double quote, or a hash."
                        .to_string(),
                );
            }
            Ok(())
        }
    }
}

pub fn channel_allowed(band: Band, channel: u16) -> bool {
    match band {
        Band::Band24 => (1..=13).contains(&channel),
        Band::Band5 => matches!(
            channel,
            36 | 40
                | 44
                | 48
                | 52
                | 56
                | 60
                | 64
                | 100
                | 104
                | 108
                | 112
                | 116
                | 120
                | 124
                | 128
                | 132
                | 136
                | 140
                | 144
                | 149
                | 153
                | 157
                | 161
                | 165
        ),
    }
}

pub fn parse_channel(band: Band, text: &str) -> Result<u16, String> {
    let channel: u16 = text
        .trim()
        .parse()
        .map_err(|_| "The channel is not a number.".to_string())?;
    if !channel_allowed(band, channel) {
        return Err(format!(
            "The channel {channel} is not valid for the {} GHz band.",
            band.as_str()
        ));
    }
    Ok(channel)
}

pub fn parse_dhcp_flag(text: &str) -> Result<bool, String> {
    match text.trim().to_ascii_lowercase().as_str() {
        "on" | "yes" | "true" => Ok(true),
        "off" | "no" | "false" => Ok(false),
        _ => Err("DHCP must be on or off.".to_string()),
    }
}

pub fn validate_interface(name: &str, interfaces: &[IfaceInfo]) -> Result<(), String> {
    let Some(info) = interfaces.iter().find(|info| info.name == name) else {
        return Err(format!("The interface {name} is not available."));
    };
    iface::require_ap(info)
}

pub fn validate_upstream(
    name: &str,
    ap_interface: &str,
    interfaces: &[IfaceInfo],
) -> Result<String, String> {
    if name.eq_ignore_ascii_case("none") {
        return Ok("none".to_string());
    }
    if !iface::valid_name(name) {
        return Err("The upstream interface name is not valid.".to_string());
    }
    if name == ap_interface {
        return Err(
            "The upstream interface must be different from the access-point interface.".to_string(),
        );
    }
    if !interfaces.iter().any(|info| info.name == name) {
        return Err(format!("The interface {name} is not available."));
    }
    Ok(name.to_string())
}

pub fn validate_address_dhcp(
    cidr: &str,
    dhcp_text: &str,
    start: &str,
    end: &str,
) -> Result<(String, bool, String, String), String> {
    let network = Ipv4Network::parse(cidr.trim())?;
    let dhcp_enabled = parse_dhcp_flag(dhcp_text)?;
    let gateway = u32::from(network.gateway()?);
    if !dhcp_enabled {
        return Ok((cidr.trim().to_string(), false, String::new(), String::new()));
    }
    if start.trim().is_empty() || end.trim().is_empty() {
        return Err("Enter the DHCP start address and the DHCP end address.".to_string());
    }
    let start_ip = network
        .contains_text(start.trim())
        .map_err(|_| format!("The DHCP start address {start} is outside the address range."))?;
    let end_ip = network
        .contains_text(end.trim())
        .map_err(|_| format!("The DHCP end address {end} is outside the address range."))?;
    if start_ip > end_ip {
        return Err("The DHCP start address must not be after the DHCP end address.".to_string());
    }
    if start_ip == gateway || end_ip == gateway || (start_ip <= gateway && gateway <= end_ip) {
        return Err(format!(
            "The DHCP range must not include the hotspot address {}.",
            Ipv4Addr::from(gateway)
        ));
    }
    Ok((
        cidr.trim().to_string(),
        true,
        start.trim().to_string(),
        end.trim().to_string(),
    ))
}

impl Profile {
    pub fn check_settings(&self) -> Result<(), String> {
        if !iface::valid_name(&self.ap_interface) {
            return Err("The access-point interface name is not valid.".to_string());
        }
        validate_ssid(&self.ssid)?;
        validate_passphrase(self.security, &self.passphrase)?;
        if !channel_allowed(self.band, self.channel) {
            return Err(format!(
                "The channel {} is not valid for the {} GHz band.",
                self.channel,
                self.band.as_str()
            ));
        }
        validate_address_dhcp(
            &self.address_cidr,
            if self.dhcp_enabled { "on" } else { "off" },
            &self.dhcp_start,
            &self.dhcp_end,
        )?;
        if self.upstream_interface.eq_ignore_ascii_case("none") {
            return Ok(());
        }
        if !iface::valid_name(&self.upstream_interface) {
            return Err("The upstream interface name is not valid.".to_string());
        }
        if self.upstream_interface == self.ap_interface {
            return Err(
                "The upstream interface must be different from the access-point interface."
                    .to_string(),
            );
        }
        Ok(())
    }

    pub fn network(&self) -> Result<Ipv4Network, String> {
        Ipv4Network::parse(&self.address_cidr)
    }

    pub fn review_lines(&self) -> Vec<String> {
        let dhcp = if self.dhcp_enabled {
            format!("on {}-{}", self.dhcp_start, self.dhcp_end)
        } else {
            "off".to_string()
        };
        let mut lines = vec![
            format!("Access-point interface: {}", self.ap_interface),
            format!("SSID: {}", self.ssid),
            format!("Security: {}", self.security.as_str()),
            match self.security {
                SecurityMode::Open => "Passphrase: the network is open".to_string(),
                _ => "Passphrase: set".to_string(),
            },
            format!("Band: {}", self.band.as_str()),
            format!("Channel: {}", self.channel),
            format!("Address range: {}", self.address_cidr),
            format!("DHCP: {dhcp}"),
            format!("Upstream interface: {}", self.upstream_interface),
        ];
        if self.security == SecurityMode::Open && self.upstream_interface != "none" {
            lines.push(OPEN_UPSTREAM_WARNING.to_string());
        }
        lines
    }
}

pub fn profile_path_from(xdg: Option<&str>, home: Option<&str>) -> PathBuf {
    if let Some(xdg) = xdg.filter(|value| !value.is_empty()) {
        return PathBuf::from(xdg).join("hotmon").join("profile.json");
    }
    if let Some(home) = home.filter(|value| !value.is_empty()) {
        return PathBuf::from(home)
            .join(".config")
            .join("hotmon")
            .join("profile.json");
    }
    PathBuf::from(".config").join("hotmon").join("profile.json")
}

pub fn default_profile_path() -> PathBuf {
    let xdg = std::env::var("XDG_CONFIG_HOME").ok();
    let home = std::env::var("HOME").ok();
    profile_path_from(xdg.as_deref(), home.as_deref())
}

pub fn save_profile(path: &Path, profile: &Profile) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)
                .map_err(|err| format!("The program cannot prepare {}. {err}", parent.display()))?;
            fs::set_permissions(parent, fs::Permissions::from_mode(0o700))
                .map_err(|err| format!("The program cannot protect {}. {err}", parent.display()))?;
        }
    }
    let text = serde_json::to_string_pretty(profile)
        .map_err(|err| format!("The profile cannot be encoded. {err}"))?;
    write_mode(path, &(text + "\n"), 0o600)
}

fn write_mode(path: &Path, contents: &str, mode: u32) -> Result<(), String> {
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(mode)
        .open(path)
        .map_err(|err| format!("The program cannot write {}. {err}", path.display()))?;
    file.write_all(contents.as_bytes())
        .map_err(|err| format!("The program cannot write {}. {err}", path.display()))?;
    fs::set_permissions(path, fs::Permissions::from_mode(mode))
        .map_err(|err| format!("The program cannot protect {}. {err}", path.display()))
}

pub fn load_profile(path: &Path) -> Result<Profile, String> {
    let text = fs::read_to_string(path)
        .map_err(|err| format!("The program cannot read {}. {err}", path.display()))?;
    serde_json::from_str(&text).map_err(|err| format!("The profile file is not valid. {err}"))
}

pub fn load_optional(path: &Path) -> Result<Option<Profile>, String> {
    if !path.exists() {
        return Ok(None);
    }
    load_profile(path).map(Some)
}

#[cfg(test)]
pub(crate) fn sample_profile() -> Profile {
    Profile {
        ap_interface: "wlan0".to_string(),
        ssid: "Hotmon".to_string(),
        security: SecurityMode::Wpa2,
        passphrase: "correct-horse".to_string(),
        band: Band::Band24,
        channel: 6,
        address_cidr: "192.168.42.0/24".to_string(),
        dhcp_enabled: true,
        dhcp_start: "192.168.42.10".to_string(),
        dhcp_end: "192.168.42.100".to_string(),
        upstream_interface: "eth0".to_string(),
    }
}

#[cfg(test)]
pub(crate) fn scratch_dir() -> PathBuf {
    use std::sync::atomic::{AtomicU64, Ordering};
    static N: AtomicU64 = AtomicU64::new(0);
    let dir = std::env::temp_dir().join(format!(
        "hotmon-{}-{}",
        std::process::id(),
        N.fetch_add(1, Ordering::Relaxed)
    ));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ssid_rejects_empty_and_slash() {
        assert!(validate_ssid("").is_err());
        assert!(validate_ssid("cafe/guest").is_err());
        assert!(validate_ssid("Cafe Guest").is_ok());
    }

    #[test]
    fn passphrase_rules() {
        assert!(validate_passphrase(SecurityMode::Open, "").is_ok());
        assert!(validate_passphrase(SecurityMode::Open, "secret").is_err());
        assert!(validate_passphrase(SecurityMode::Wpa2, "short").is_err());
        assert!(validate_passphrase(SecurityMode::Wpa3, "correct-horse").is_ok());
        assert!(validate_passphrase(SecurityMode::Wpa2, "correct horse").is_ok());
        assert!(validate_passphrase(SecurityMode::Wpa2, "correct-horse\\").is_err());
        assert!(validate_passphrase(SecurityMode::Wpa2, "correct\"horse").is_err());
        assert!(validate_passphrase(SecurityMode::Wpa2, "correct#horse").is_err());
        assert!(validate_passphrase(SecurityMode::Wpa2, " correct-horse").is_err());
        assert!(validate_passphrase(SecurityMode::Wpa2, "correct-horse ").is_err());
    }

    #[test]
    fn channel_must_match_the_band() {
        assert!(parse_channel(Band::Band24, "6").is_ok());
        assert!(parse_channel(Band::Band24, "36").is_err());
        assert!(parse_channel(Band::Band5, "36").is_ok());
        assert!(parse_channel(Band::Band5, "6").is_err());
    }

    #[test]
    fn dhcp_range_must_stay_inside_the_network() {
        let ok = validate_address_dhcp("192.168.42.0/24", "on", "192.168.42.10", "192.168.42.20");
        assert!(ok.is_ok());
        let bad = validate_address_dhcp("192.168.42.0/24", "on", "10.0.0.10", "10.0.0.20");
        assert!(bad.is_err());
        let gateway =
            validate_address_dhcp("192.168.42.0/24", "on", "192.168.42.1", "192.168.42.20");
        assert!(gateway.is_err());
    }

    #[test]
    fn profile_round_trip() {
        let dir = scratch_dir();
        let path = dir.join("profile.json");
        let profile = sample_profile();
        save_profile(&path, &profile).unwrap();
        let loaded = load_profile(&path).unwrap();
        assert_eq!(loaded, profile);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn missing_profile_is_empty() {
        let dir = scratch_dir();
        let loaded = load_optional(&dir.join("missing.json")).unwrap();
        assert!(loaded.is_none());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn corrupt_profile_returns_an_error() {
        let dir = scratch_dir();
        let path = dir.join("profile.json");
        fs::write(&path, "{").unwrap();
        let error = load_profile(&path).unwrap_err();
        assert!(error.contains("not valid"));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn profile_path_uses_xdg_then_home() {
        let xdg = profile_path_from(Some("/cfg"), Some("/home/sam"));
        assert_eq!(xdg, PathBuf::from("/cfg/hotmon/profile.json"));
        let home = profile_path_from(Some(""), Some("/home/sam"));
        assert_eq!(home, PathBuf::from("/home/sam/.config/hotmon/profile.json"));
    }

    #[test]
    fn review_lists_each_setting() {
        let lines = sample_profile().review_lines().join("\n");
        for text in [
            "wlan0",
            "Hotmon",
            "wpa2",
            "Passphrase: set",
            "2.4",
            "6",
            "192.168.42.0/24",
            "192.168.42.10",
            "eth0",
        ] {
            assert!(lines.contains(text), "{text}");
        }
        assert!(
            !lines.contains("correct-horse"),
            "the review screen shows the passphrase"
        );
    }

    #[test]
    fn review_for_an_open_network_hides_the_passphrase() {
        let mut profile = sample_profile();
        profile.security = SecurityMode::Open;
        profile.passphrase.clear();
        profile.upstream_interface = "none".to_string();
        let lines = profile.review_lines().join("\n");
        assert!(lines.contains("the network is open"));
        assert!(!lines.contains(OPEN_UPSTREAM_WARNING));
        profile.upstream_interface = "eth0".to_string();
        let warned = profile.review_lines().join("\n");
        assert!(warned.contains(OPEN_UPSTREAM_WARNING));
        assert!(!warned.contains("correct-horse"));
    }

    #[test]
    fn profile_directory_and_file_are_private() {
        let dir = scratch_dir();
        let path = dir.join("hotmon").join("profile.json");
        save_profile(&path, &sample_profile()).unwrap();
        let dir_mode = fs::metadata(path.parent().unwrap())
            .unwrap()
            .permissions()
            .mode()
            & 0o777;
        let file_mode = fs::metadata(&path).unwrap().permissions().mode() & 0o777;
        assert_eq!(dir_mode, 0o700);
        assert_eq!(file_mode, 0o600);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn bad_channel_fails_the_setting_check() {
        let mut profile = sample_profile();
        profile.channel = 99;
        assert!(profile.check_settings().is_err());
    }
}
