use std::collections::VecDeque;

pub const CAPTURE_WARNING: &str = "Packet capture reads frames on the hotspot interface only. The program does not change packet contents. Press Enter to start capture.";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CapturePhase {
    Idle,
    Warned,
    Armed,
    Running,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ConfirmResult {
    Warning,
    Open { iface: String },
    Already,
    Ignored,
}

pub trait FrameSource {
    fn try_recv(&mut self) -> std::io::Result<Option<Vec<u8>>>;
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PacketSummary {
    pub length: usize,
    pub source: String,
    pub destination: String,
    pub protocol: String,
}

impl PacketSummary {
    pub fn text(&self) -> String {
        format!(
            "{} B {} -> {} {}",
            self.length, self.source, self.destination, self.protocol
        )
    }
}

pub struct CaptureControl {
    phase: CapturePhase,
    iface: String,
    lines: VecDeque<String>,
    source: Option<Box<dyn FrameSource>>,
}

impl CaptureControl {
    pub fn new() -> Self {
        Self {
            phase: CapturePhase::Idle,
            iface: String::new(),
            lines: VecDeque::new(),
            source: None,
        }
    }

    #[cfg(test)]
    pub fn phase(&self) -> CapturePhase {
        self.phase
    }

    pub fn is_running(&self) -> bool {
        self.phase == CapturePhase::Running
    }

    pub fn armed_interface(&self) -> Option<&str> {
        if self.phase == CapturePhase::Armed {
            Some(self.iface.as_str())
        } else {
            None
        }
    }

    #[cfg(test)]
    pub fn lines(&self) -> impl Iterator<Item = &str> {
        self.lines.iter().map(String::as_str)
    }

    pub fn recent_lines(&self, count: usize) -> Vec<String> {
        self.lines.iter().rev().take(count).cloned().collect()
    }

    pub fn is_warned(&self) -> bool {
        self.phase == CapturePhase::Warned
    }

    pub fn warn(&mut self, hotspot_iface: &str) -> Result<ConfirmResult, String> {
        let iface = capture_interface(hotspot_iface, hotspot_iface)?;
        if matches!(self.phase, CapturePhase::Armed | CapturePhase::Running) {
            return Ok(ConfirmResult::Already);
        }
        if self.phase == CapturePhase::Warned && self.iface != iface {
            self.iface = iface.to_string();
            return Ok(ConfirmResult::Warning);
        }
        self.iface = iface.to_string();
        self.phase = CapturePhase::Warned;
        Ok(ConfirmResult::Warning)
    }

    pub fn accept(&mut self, hotspot_iface: &str) -> Result<ConfirmResult, String> {
        let iface = capture_interface(hotspot_iface, hotspot_iface)?;
        if self.phase != CapturePhase::Warned {
            return Ok(ConfirmResult::Ignored);
        }
        if self.iface != iface {
            self.iface = iface.to_string();
            self.phase = CapturePhase::Warned;
            return Ok(ConfirmResult::Warning);
        }
        self.phase = CapturePhase::Armed;
        Ok(ConfirmResult::Open {
            iface: self.iface.clone(),
        })
    }

    pub fn dismiss_warning(&mut self) {
        if self.phase == CapturePhase::Warned {
            self.phase = CapturePhase::Idle;
            self.iface.clear();
        }
    }

    pub fn attach(&mut self, source: Box<dyn FrameSource>) -> Result<(), String> {
        if self.phase != CapturePhase::Armed {
            return Err("Capture is not confirmed.".to_string());
        }
        self.source = Some(source);
        self.phase = CapturePhase::Running;
        Ok(())
    }

    pub fn fail_open(&mut self, message: String) -> String {
        self.stop();
        format!("Capture did not start. {message}")
    }

    pub fn stop(&mut self) {
        self.phase = CapturePhase::Idle;
        self.iface.clear();
        self.source = None;
    }

    pub fn poll(&mut self) -> Result<(), String> {
        if self.phase != CapturePhase::Running {
            return Ok(());
        }
        let Some(source) = self.source.as_mut() else {
            return Ok(());
        };
        loop {
            match source.try_recv() {
                Ok(None) => break,
                Ok(Some(frame)) => {
                    let summary = summarize(&frame);
                    self.lines.push_back(summary.text());
                    while self.lines.len() > 50 {
                        self.lines.pop_front();
                    }
                }
                Err(err) => {
                    self.stop();
                    return Err(format!("Capture stopped. {err}"));
                }
            }
        }
        Ok(())
    }
}

impl Default for CaptureControl {
    fn default() -> Self {
        Self::new()
    }
}

pub fn capture_interface<'a>(requested: &'a str, hotspot: &'a str) -> Result<&'a str, String> {
    if requested.is_empty() || hotspot.is_empty() {
        return Err("The hotspot interface is not set.".to_string());
    }
    if requested != hotspot {
        return Err("Capture is allowed only on the hotspot interface.".to_string());
    }
    Ok(hotspot)
}

pub fn summarize(frame: &[u8]) -> PacketSummary {
    if frame.len() < 14 {
        return PacketSummary {
            length: frame.len(),
            source: "-".to_string(),
            destination: "-".to_string(),
            protocol: "short".to_string(),
        };
    }
    let mut ethertype = u16::from_be_bytes([frame[12], frame[13]]);
    let mut header = 14;
    if ethertype == 0x8100 && frame.len() >= 18 {
        ethertype = u16::from_be_bytes([frame[16], frame[17]]);
        header = 18;
    }
    let src_mac = format_mac(&frame[6..12]);
    let dst_mac = format_mac(&frame[0..6]);
    match ethertype {
        0x0800 => summarize_ipv4(frame, header, frame.len()),
        0x0806 => PacketSummary {
            length: frame.len(),
            source: src_mac,
            destination: dst_mac,
            protocol: "ARP".to_string(),
        },
        0x86dd => PacketSummary {
            length: frame.len(),
            source: src_mac,
            destination: dst_mac,
            protocol: "IPv6".to_string(),
        },
        other => PacketSummary {
            length: frame.len(),
            source: src_mac,
            destination: dst_mac,
            protocol: format!("eth {other:04x}"),
        },
    }
}

fn summarize_ipv4(frame: &[u8], header: usize, length: usize) -> PacketSummary {
    if frame.len() < header + 20 {
        return PacketSummary {
            length,
            source: "-".to_string(),
            destination: "-".to_string(),
            protocol: "truncated".to_string(),
        };
    }
    let ip = &frame[header..];
    let ihl = (ip[0] & 0x0f) as usize * 4;
    if ihl < 20 || ip.len() < ihl {
        return PacketSummary {
            length,
            source: "-".to_string(),
            destination: "-".to_string(),
            protocol: "truncated".to_string(),
        };
    }
    let protocol = ip[9];
    let source = std::net::Ipv4Addr::new(ip[12], ip[13], ip[14], ip[15]).to_string();
    let destination = std::net::Ipv4Addr::new(ip[16], ip[17], ip[18], ip[19]).to_string();
    let proto = match protocol {
        1 => "ICMP".to_string(),
        6 => port_text("TCP", ip, ihl),
        17 => port_text("UDP", ip, ihl),
        other => format!("ip {other}"),
    };
    PacketSummary {
        length,
        source,
        destination,
        protocol: proto,
    }
}

fn port_text(name: &str, ip: &[u8], ihl: usize) -> String {
    if ip.len() < ihl + 4 {
        return name.to_string();
    }
    let src = u16::from_be_bytes([ip[ihl], ip[ihl + 1]]);
    let dst = u16::from_be_bytes([ip[ihl + 2], ip[ihl + 3]]);
    format!("{name} {src}->{dst}")
}

fn format_mac(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect::<Vec<_>>()
        .join(":")
}

#[cfg(test)]
mod tests {
    use super::*;

    struct FakeSource {
        frames: Vec<Vec<u8>>,
        reads: usize,
    }

    impl FrameSource for FakeSource {
        fn try_recv(&mut self) -> std::io::Result<Option<Vec<u8>>> {
            self.reads += 1;
            if self.frames.is_empty() {
                Ok(None)
            } else {
                Ok(Some(self.frames.remove(0)))
            }
        }
    }

    fn udp_frame() -> Vec<u8> {
        let mut frame = vec![0_u8; 14 + 20 + 8];
        frame[0..6].copy_from_slice(&[0xff, 0xff, 0xff, 0xff, 0xff, 0xff]);
        frame[6..12].copy_from_slice(&[0x02, 0x00, 0x00, 0x00, 0x00, 0x01]);
        frame[12] = 0x08;
        frame[13] = 0x00;
        frame[14] = 0x45;
        frame[16] = 0x00;
        frame[17] = 28;
        frame[22] = 64;
        frame[23] = 17;
        frame[26] = 192;
        frame[27] = 168;
        frame[28] = 42;
        frame[29] = 10;
        frame[30] = 192;
        frame[31] = 168;
        frame[32] = 42;
        frame[33] = 1;
        frame[34] = 0x30;
        frame[35] = 0x39;
        frame[36] = 0x00;
        frame[37] = 53;
        frame
    }

    #[test]
    fn capture_does_not_start_before_the_second_confirmation() {
        let mut capture = CaptureControl::new();
        let first = capture.warn("wlan0").unwrap();
        assert_eq!(first, ConfirmResult::Warning);
        assert_eq!(capture.phase(), CapturePhase::Warned);
        assert!(!capture.is_running());
        let repeated = capture.warn("wlan0").unwrap();
        assert_eq!(repeated, ConfirmResult::Warning);
        assert_eq!(capture.phase(), CapturePhase::Warned);
        let source = FakeSource {
            frames: vec![udp_frame()],
            reads: 0,
        };
        capture.poll().unwrap();
        assert_eq!(source.reads, 0);
        let _ = source;
        let second = capture.accept("wlan0").unwrap();
        assert!(matches!(second, ConfirmResult::Open { iface } if iface == "wlan0"));
        assert!(!capture.is_running());
        capture
            .attach(Box::new(FakeSource {
                frames: vec![udp_frame()],
                reads: 0,
            }))
            .unwrap();
        assert!(capture.is_running());
        capture.poll().unwrap();
        let text = capture.lines().next().unwrap().to_string();
        assert!(text.contains("192.168.42.10"));
        assert!(text.contains("192.168.42.1"));
        assert!(text.contains("UDP"));
    }

    #[test]
    fn summary_does_not_change_the_frame() {
        let mut frame = udp_frame();
        let before = frame.clone();
        let text = summarize(&frame).text();
        frame.reverse();
        frame.reverse();
        assert_eq!(frame, before);
        assert!(text.contains("UDP 12345->53"));
        assert!(capture_interface("eth0", "wlan0").is_err());
        assert_eq!(capture_interface("wlan0", "wlan0").unwrap(), "wlan0");
    }

    #[test]
    fn stop_ends_capture() {
        let mut capture = CaptureControl::new();
        capture.warn("wlan0").unwrap();
        capture.accept("wlan0").unwrap();
        capture
            .attach(Box::new(FakeSource {
                frames: Vec::new(),
                reads: 0,
            }))
            .unwrap();
        capture.stop();
        assert_eq!(capture.phase(), CapturePhase::Idle);
        assert!(!capture.is_running());
    }

    #[test]
    fn attach_before_the_second_confirmation_fails() {
        let mut capture = CaptureControl::new();
        capture.warn("wlan0").unwrap();
        let error = capture
            .attach(Box::new(FakeSource {
                frames: Vec::new(),
                reads: 0,
            }))
            .unwrap_err();
        assert!(error.contains("not confirmed"));
    }

    #[test]
    fn the_warning_key_does_not_start_capture() {
        let mut capture = CaptureControl::new();
        assert_eq!(capture.accept("wlan0").unwrap(), ConfirmResult::Ignored);
        capture.warn("wlan0").unwrap();
        capture.warn("wlan0").unwrap();
        assert_eq!(capture.phase(), CapturePhase::Warned);
        assert!(capture.armed_interface().is_none());
        let changed = capture.accept("wlan1").unwrap();
        assert_eq!(changed, ConfirmResult::Warning);
        assert_eq!(capture.phase(), CapturePhase::Warned);
        assert!(capture.armed_interface().is_none());
        let open = capture.accept("wlan1").unwrap();
        assert!(matches!(open, ConfirmResult::Open { iface } if iface == "wlan1"));
    }
}
