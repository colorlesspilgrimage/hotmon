use crate::iface::IfaceInfo;
use crate::profile::{
    Profile, parse_band, parse_channel, parse_security, validate_address_dhcp, validate_interface,
    validate_passphrase, validate_ssid, validate_upstream,
};

pub const CANCELLED: &str = "The wizard is cancelled. The settings were not applied.";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Page {
    Interface,
    Ssid,
    Security,
    Passphrase,
    BandChannel,
    AddressDhcp,
    Upstream,
    Review,
}

const ORDER: [Page; 8] = [
    Page::Interface,
    Page::Ssid,
    Page::Security,
    Page::Passphrase,
    Page::BandChannel,
    Page::AddressDhcp,
    Page::Upstream,
    Page::Review,
];

impl Page {
    fn index(self) -> usize {
        ORDER.iter().position(|page| *page == self).unwrap_or(0)
    }

    pub fn title(self) -> &'static str {
        match self {
            Self::Interface => "Access-point interface",
            Self::Ssid => "SSID",
            Self::Security => "Security mode",
            Self::Passphrase => "Passphrase",
            Self::BandChannel => "Band and channel",
            Self::AddressDhcp => "Address range and DHCP",
            Self::Upstream => "Upstream interface",
            Self::Review => "Review",
        }
    }

    fn field_count(self) -> usize {
        match self {
            Self::BandChannel => 2,
            Self::AddressDhcp => 4,
            Self::Review => 0,
            _ => 1,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FieldLine {
    pub label: &'static str,
    pub value: String,
    pub active: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Wizard {
    pub page: Page,
    pub field: usize,
    pub cancelled: bool,
    pub error: Option<String>,
    pub ap_interface: String,
    pub ssid: String,
    pub security: String,
    pub passphrase: String,
    pub band: String,
    pub channel: String,
    pub address_cidr: String,
    pub dhcp: String,
    pub dhcp_start: String,
    pub dhcp_end: String,
    pub upstream_interface: String,
}

impl Wizard {
    pub fn new() -> Self {
        Self {
            page: Page::Interface,
            field: 0,
            cancelled: false,
            error: None,
            ap_interface: String::new(),
            ssid: String::new(),
            security: String::new(),
            passphrase: String::new(),
            band: String::new(),
            channel: String::new(),
            address_cidr: String::new(),
            dhcp: String::new(),
            dhcp_start: String::new(),
            dhcp_end: String::new(),
            upstream_interface: String::new(),
        }
    }

    pub fn from_profile(profile: &Profile) -> Self {
        Self {
            page: Page::Review,
            field: 0,
            cancelled: false,
            error: None,
            ap_interface: profile.ap_interface.clone(),
            ssid: profile.ssid.clone(),
            security: profile.security.as_str().to_string(),
            passphrase: profile.passphrase.clone(),
            band: profile.band.as_str().to_string(),
            channel: profile.channel.to_string(),
            address_cidr: profile.address_cidr.clone(),
            dhcp: if profile.dhcp_enabled { "on" } else { "off" }.to_string(),
            dhcp_start: profile.dhcp_start.clone(),
            dhcp_end: profile.dhcp_end.clone(),
            upstream_interface: profile.upstream_interface.clone(),
        }
    }

    pub fn position(&self) -> (usize, usize) {
        (self.page.index() + 1, ORDER.len())
    }

    pub fn open_upstream_risk(&self) -> bool {
        self.security == "open"
            && !self.upstream_interface.is_empty()
            && self.upstream_interface != "none"
    }

    pub fn hint(&self) -> String {
        match self.page {
            Page::Interface => "Enter the access-point interface.".to_string(),
            Page::Ssid => "Enter the SSID. Use 1 to 32 characters.".to_string(),
            Page::Security => "Enter open, wpa2, or wpa3.".to_string(),
            Page::Passphrase => {
                "Enter the passphrase. Use 8 to 63 characters. Leave this empty for open."
                    .to_string()
            }
            Page::BandChannel => "Enter the band (2.4 or 5) and the channel.".to_string(),
            Page::AddressDhcp => {
                "Enter the address range, DHCP on or off, and the DHCP range.".to_string()
            }
            Page::Upstream => "Enter the upstream interface, or none.".to_string(),
            Page::Review if self.open_upstream_risk() => format!(
                "{} Press y to apply. Enter does not apply this warning.",
                crate::profile::OPEN_UPSTREAM_WARNING
            ),
            Page::Review => "Review the settings. Enter applies the profile.".to_string(),
        }
    }

    pub fn field_lines(&self) -> Vec<FieldLine> {
        let masked;
        let pairs: Vec<(&str, &str)> = match self.page {
            Page::Interface => vec![("Access-point interface", &self.ap_interface)],
            Page::Ssid => vec![("SSID", &self.ssid)],
            Page::Security => vec![("Security mode", &self.security)],
            Page::Passphrase => {
                masked = "*".repeat(self.passphrase.chars().count());
                vec![("Passphrase", masked.as_str())]
            }
            Page::BandChannel => vec![("Band", &self.band), ("Channel", &self.channel)],
            Page::AddressDhcp => vec![
                ("Address range", &self.address_cidr),
                ("DHCP", &self.dhcp),
                ("DHCP start", &self.dhcp_start),
                ("DHCP end", &self.dhcp_end),
            ],
            Page::Upstream => vec![("Upstream interface", &self.upstream_interface)],
            Page::Review => vec![],
        };
        pairs
            .into_iter()
            .enumerate()
            .map(|(index, (label, value))| FieldLine {
                label,
                value: value.to_string(),
                active: index == self.field,
            })
            .collect()
    }

    pub fn push_char(&mut self, ch: char) {
        if ch.is_control() {
            return;
        }
        self.cancelled = false;
        let Some(field) = self.active_field_mut() else {
            return;
        };
        if field.chars().count() >= 128 {
            return;
        }
        field.push(ch);
    }

    pub fn backspace(&mut self) {
        self.cancelled = false;
        if let Some(field) = self.active_field_mut() {
            field.pop();
        }
    }

    pub fn next_field(&mut self) {
        let count = self.page.field_count();
        if count == 0 {
            return;
        }
        self.field = (self.field + 1) % count;
    }

    pub fn next(&mut self, interfaces: &[IfaceInfo]) -> Result<(), String> {
        self.ensure_open()?;
        if let Err(message) = self.validate_current(interfaces) {
            return self.fail(message);
        }
        self.normalize_current();
        let index = self.page.index();
        if index + 1 >= ORDER.len() {
            return self.fail("Use confirm on the review page.".to_string());
        }
        self.page = ORDER[index + 1];
        self.field = 0;
        self.error = None;
        Ok(())
    }

    pub fn back(&mut self) {
        self.cancelled = false;
        let index = self.page.index();
        if index == 0 {
            return;
        }
        self.page = ORDER[index - 1];
        self.field = 0;
        self.error = None;
    }

    pub fn cancel(&mut self) {
        self.cancelled = true;
        self.error = None;
    }

    pub fn is_cancelled(&self) -> bool {
        self.cancelled
    }

    pub fn reopen(&mut self) {
        self.cancelled = false;
        self.error = None;
    }

    pub fn set_error(&mut self, message: String) {
        self.error = Some(message);
    }

    pub fn confirmed_profile(&mut self, interfaces: &[IfaceInfo]) -> Result<Profile, String> {
        self.ensure_open()?;
        if self.page != Page::Review {
            return self.fail("Confirm the settings on the review page.".to_string());
        }
        match self.build_profile(interfaces) {
            Ok(profile) => {
                self.error = None;
                Ok(profile)
            }
            Err(message) => self.fail(message),
        }
    }

    fn ensure_open(&mut self) -> Result<(), String> {
        if self.cancelled {
            self.fail(CANCELLED.to_string())
        } else {
            Ok(())
        }
    }

    fn fail<T>(&mut self, message: String) -> Result<T, String> {
        self.error = Some(message.clone());
        Err(message)
    }

    fn validate_current(&self, interfaces: &[IfaceInfo]) -> Result<(), String> {
        match self.page {
            Page::Interface => validate_interface(self.ap_interface.trim(), interfaces),
            Page::Ssid => validate_ssid(self.ssid.trim()),
            Page::Security => parse_security(&self.security).map(|_| ()),
            Page::Passphrase => {
                let mode = parse_security(&self.security)?;
                validate_passphrase(mode, &self.passphrase)
            }
            Page::BandChannel => {
                let band = parse_band(&self.band)?;
                parse_channel(band, &self.channel).map(|_| ())
            }
            Page::AddressDhcp => validate_address_dhcp(
                &self.address_cidr,
                &self.dhcp,
                &self.dhcp_start,
                &self.dhcp_end,
            )
            .map(|_| ()),
            Page::Upstream => validate_upstream(
                self.upstream_interface.trim(),
                self.ap_interface.trim(),
                interfaces,
            )
            .map(|_| ()),
            Page::Review => Ok(()),
        }
    }

    fn normalize_current(&mut self) {
        match self.page {
            Page::Interface => self.ap_interface = self.ap_interface.trim().to_string(),
            Page::Ssid => self.ssid = self.ssid.trim().to_string(),
            Page::Security => {
                if let Ok(mode) = parse_security(&self.security) {
                    self.security = mode.as_str().to_string();
                }
            }
            Page::BandChannel => {
                if let Ok(band) = parse_band(&self.band) {
                    self.band = band.as_str().to_string();
                }
                self.channel = self.channel.trim().to_string();
            }
            Page::AddressDhcp => {
                if let Ok((cidr, enabled, start, end)) = validate_address_dhcp(
                    &self.address_cidr,
                    &self.dhcp,
                    &self.dhcp_start,
                    &self.dhcp_end,
                ) {
                    self.address_cidr = cidr;
                    self.dhcp = if enabled { "on" } else { "off" }.to_string();
                    self.dhcp_start = start;
                    self.dhcp_end = end;
                }
            }
            Page::Upstream => {
                self.upstream_interface =
                    if self.upstream_interface.trim().eq_ignore_ascii_case("none") {
                        "none".to_string()
                    } else {
                        self.upstream_interface.trim().to_string()
                    };
            }
            _ => {}
        }
    }

    fn build_profile(&self, interfaces: &[IfaceInfo]) -> Result<Profile, String> {
        validate_interface(self.ap_interface.trim(), interfaces)?;
        validate_ssid(self.ssid.trim())?;
        let security = parse_security(&self.security)?;
        validate_passphrase(security, &self.passphrase)?;
        let band = parse_band(&self.band)?;
        let channel = parse_channel(band, &self.channel)?;
        let (address_cidr, dhcp_enabled, dhcp_start, dhcp_end) = validate_address_dhcp(
            &self.address_cidr,
            &self.dhcp,
            &self.dhcp_start,
            &self.dhcp_end,
        )?;
        let upstream = validate_upstream(
            self.upstream_interface.trim(),
            self.ap_interface.trim(),
            interfaces,
        )?;
        let profile = Profile {
            ap_interface: self.ap_interface.trim().to_string(),
            ssid: self.ssid.trim().to_string(),
            security,
            passphrase: self.passphrase.clone(),
            band,
            channel,
            address_cidr,
            dhcp_enabled,
            dhcp_start,
            dhcp_end,
            upstream_interface: upstream,
        };
        profile.check_settings()?;
        Ok(profile)
    }

    fn active_field_mut(&mut self) -> Option<&mut String> {
        match self.page {
            Page::Interface => Some(&mut self.ap_interface),
            Page::Ssid => Some(&mut self.ssid),
            Page::Security => Some(&mut self.security),
            Page::Passphrase => Some(&mut self.passphrase),
            Page::BandChannel if self.field == 0 => Some(&mut self.band),
            Page::BandChannel => Some(&mut self.channel),
            Page::AddressDhcp => match self.field {
                0 => Some(&mut self.address_cidr),
                1 => Some(&mut self.dhcp),
                2 => Some(&mut self.dhcp_start),
                _ => Some(&mut self.dhcp_end),
            },
            Page::Upstream => Some(&mut self.upstream_interface),
            Page::Review => None,
        }
    }
}

impl Default for Wizard {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::iface::sample_interfaces;
    use crate::profile::sample_profile;

    fn fill_valid(wizard: &mut Wizard) {
        let profile = sample_profile();
        let loaded = Wizard::from_profile(&profile);
        wizard.ap_interface = loaded.ap_interface;
        wizard.ssid = loaded.ssid;
        wizard.security = loaded.security;
        wizard.passphrase = loaded.passphrase;
        wizard.band = loaded.band;
        wizard.channel = loaded.channel;
        wizard.address_cidr = loaded.address_cidr;
        wizard.dhcp = loaded.dhcp;
        wizard.dhcp_start = loaded.dhcp_start;
        wizard.dhcp_end = loaded.dhcp_end;
        wizard.upstream_interface = loaded.upstream_interface;
    }

    #[test]
    fn invalid_input_blocks_the_next_page() {
        let mut wizard = Wizard::new();
        wizard.ap_interface = "wlan1".to_string();
        let error = wizard.next(&sample_interfaces()).unwrap_err();
        assert!(error.contains("cannot start an access point"));
        assert_eq!(wizard.page, Page::Interface);
    }

    #[test]
    fn valid_pages_advance_and_back_keeps_the_value() {
        let mut wizard = Wizard::new();
        fill_valid(&mut wizard);
        wizard.page = Page::Interface;
        wizard.next(&sample_interfaces()).unwrap();
        assert_eq!(wizard.page, Page::Ssid);
        wizard.ssid = "Cafe".to_string();
        wizard.next(&sample_interfaces()).unwrap();
        assert_eq!(wizard.page, Page::Security);
        wizard.back();
        assert_eq!(wizard.page, Page::Ssid);
        assert_eq!(wizard.ssid, "Cafe");
    }

    #[test]
    fn each_page_rejects_bad_input() {
        let mut wizard = Wizard::new();
        fill_valid(&mut wizard);
        wizard.page = Page::Ssid;
        wizard.ssid.clear();
        assert!(wizard.next(&sample_interfaces()).is_err());
        wizard.ssid = "Cafe".to_string();
        wizard.page = Page::Security;
        wizard.security = "wep".to_string();
        assert!(wizard.next(&sample_interfaces()).is_err());
        wizard.security = "wpa2".to_string();
        wizard.page = Page::Passphrase;
        wizard.passphrase = "short".to_string();
        assert!(wizard.next(&sample_interfaces()).is_err());
        wizard.passphrase = "correct-horse".to_string();
        wizard.page = Page::BandChannel;
        wizard.channel = "36".to_string();
        assert!(wizard.next(&sample_interfaces()).is_err());
        wizard.channel = "6".to_string();
        wizard.page = Page::AddressDhcp;
        wizard.dhcp_start = "10.1.1.1".to_string();
        assert!(wizard.next(&sample_interfaces()).is_err());
        wizard.dhcp_start = "192.168.42.10".to_string();
        wizard.page = Page::Upstream;
        wizard.upstream_interface = "wlan0".to_string();
        assert!(wizard.next(&sample_interfaces()).is_err());
    }

    #[test]
    fn open_security_allows_an_empty_passphrase() {
        let mut wizard = Wizard::new();
        fill_valid(&mut wizard);
        wizard.page = Page::Security;
        wizard.security = "open".to_string();
        wizard.next(&sample_interfaces()).unwrap();
        wizard.passphrase.clear();
        wizard.next(&sample_interfaces()).unwrap();
        assert_eq!(wizard.page, Page::BandChannel);
    }

    #[test]
    fn cancel_blocks_confirm() {
        let mut wizard = Wizard::from_profile(&sample_profile());
        wizard.cancel();
        let error = wizard.confirmed_profile(&sample_interfaces()).unwrap_err();
        assert!(error.contains("cancelled"));
    }

    #[test]
    fn review_confirm_returns_the_profile() {
        let mut wizard = Wizard::from_profile(&sample_profile());
        let profile = wizard.confirmed_profile(&sample_interfaces()).unwrap();
        assert_eq!(profile, sample_profile());
        assert_eq!(wizard.page, Page::Review);
    }

    #[test]
    fn passphrase_page_masks_the_value() {
        let mut wizard = Wizard::from_profile(&sample_profile());
        wizard.page = Page::Passphrase;
        let lines = wizard.field_lines();
        assert_eq!(lines[0].value, "*".repeat("correct-horse".chars().count()));
        assert!(!lines[0].value.contains("correct-horse"));
        assert!(!lines[0].value.contains('c'));
    }

    #[test]
    fn passphrase_page_rejects_shell_characters_and_outer_spaces() {
        let mut wizard = Wizard::new();
        fill_valid(&mut wizard);
        wizard.page = Page::Passphrase;
        for bad in [
            "correct#horse",
            "correct\"horse",
            "correct\\horse",
            " correct-horse",
            "correct-horse ",
        ] {
            wizard.passphrase = bad.to_string();
            assert!(wizard.next(&sample_interfaces()).is_err(), "{bad}");
            assert_eq!(wizard.page, Page::Passphrase);
        }
    }
}
