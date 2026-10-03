use std::path::Path;
use std::process::Command;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct IfaceInfo {
    pub name: String,
    pub wireless: bool,
    pub supports_ap: bool,
}

pub fn valid_name(name: &str) -> bool {
    let mut chars = name.chars();
    let Some(first) = chars.next() else {
        return false;
    };
    if !first.is_ascii_alphabetic() || name.len() > 15 {
        return false;
    }
    chars.all(|ch| ch.is_ascii_alphanumeric() || matches!(ch, '_' | '.' | '-' | ':'))
}

pub fn modes_support_ap(text: &str) -> bool {
    text.lines()
        .any(|line| line.trim() == "* AP" || line.trim() == "AP")
}

pub fn require_ap(info: &IfaceInfo) -> Result<(), String> {
    if !info.wireless {
        return Err(format!("{} is not a wireless interface.", info.name));
    }
    if !info.supports_ap {
        return Err(format!("{} cannot start an access point.", info.name));
    }
    Ok(())
}

pub trait PhyInfo {
    fn supports_ap(&self, phy: &str) -> bool;
}

pub struct SystemPhyInfo;

impl PhyInfo for SystemPhyInfo {
    fn supports_ap(&self, phy: &str) -> bool {
        match Command::new("iw").args(["phy", phy, "info"]).output() {
            Ok(output) if output.status.success() => {
                modes_support_ap(&String::from_utf8_lossy(&output.stdout))
            }
            _ => false,
        }
    }
}

pub fn read_system_interfaces() -> Vec<IfaceInfo> {
    read_interfaces_at(Path::new("/sys/class/net"), &SystemPhyInfo)
}

pub fn read_interfaces_at(root: &Path, phys: &dyn PhyInfo) -> Vec<IfaceInfo> {
    let mut infos = Vec::new();
    let entries = match std::fs::read_dir(root) {
        Ok(entries) => entries,
        Err(_) => return infos,
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        if name == "lo" || !entry.path().is_dir() {
            continue;
        }
        let phy_link = entry.path().join("phy80211");
        let phy = std::fs::read_link(&phy_link).ok().and_then(|path| {
            path.file_name()
                .map(|value| value.to_string_lossy().to_string())
        });
        let wireless = entry.path().join("wireless").exists() || phy.is_some();
        let supports_ap = phy.as_deref().is_some_and(|phy| phys.supports_ap(phy));
        infos.push(IfaceInfo {
            name,
            wireless,
            supports_ap,
        });
    }
    infos.sort_by(|left, right| left.name.cmp(&right.name));
    infos
}

#[cfg(test)]
pub(crate) fn sample_interfaces() -> Vec<IfaceInfo> {
    vec![
        IfaceInfo {
            name: "eth0".to_string(),
            wireless: false,
            supports_ap: false,
        },
        IfaceInfo {
            name: "wlan0".to_string(),
            wireless: true,
            supports_ap: true,
        },
        IfaceInfo {
            name: "wlan1".to_string(),
            wireless: true,
            supports_ap: false,
        },
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::symlink;

    struct MapPhy;

    impl PhyInfo for MapPhy {
        fn supports_ap(&self, phy: &str) -> bool {
            phy == "phy0"
        }
    }

    #[test]
    fn ap_mode_line_is_required() {
        let text = "Supported interface modes:\n\t\t * managed\n\t\t * AP\n\t\t * AP/VLAN\n";
        assert!(modes_support_ap(text));
        assert!(!modes_support_ap("* AP/VLAN\n* managed\n"));
    }

    #[test]
    fn interface_without_ap_support_is_rejected() {
        let wired = IfaceInfo {
            name: "eth0".to_string(),
            wireless: false,
            supports_ap: false,
        };
        assert!(require_ap(&wired).unwrap_err().contains("not a wireless"));
        let station = IfaceInfo {
            name: "wlan1".to_string(),
            wireless: true,
            supports_ap: false,
        };
        assert!(
            require_ap(&station)
                .unwrap_err()
                .contains("cannot start an access point")
        );
        let ap = IfaceInfo {
            name: "wlan0".to_string(),
            wireless: true,
            supports_ap: true,
        };
        assert!(require_ap(&ap).is_ok());
    }

    #[test]
    fn sysfs_read_marks_ap_support() {
        let dir = crate::profile::scratch_dir();
        let wlan0 = dir.join("wlan0");
        let wlan1 = dir.join("wlan1");
        let eth0 = dir.join("eth0");
        std::fs::create_dir_all(&wlan0).unwrap();
        std::fs::create_dir_all(&wlan1).unwrap();
        std::fs::create_dir_all(&eth0).unwrap();
        symlink("phy0", wlan0.join("phy80211")).unwrap();
        std::fs::write(wlan1.join("wireless"), "").unwrap();
        let infos = read_interfaces_at(&dir, &MapPhy);
        assert_eq!(
            infos,
            vec![
                IfaceInfo {
                    name: "eth0".to_string(),
                    wireless: false,
                    supports_ap: false,
                },
                IfaceInfo {
                    name: "wlan0".to_string(),
                    wireless: true,
                    supports_ap: true,
                },
                IfaceInfo {
                    name: "wlan1".to_string(),
                    wireless: true,
                    supports_ap: false,
                },
            ]
        );
        let _ = std::fs::remove_dir_all(&dir);
    }
}
