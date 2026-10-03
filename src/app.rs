use std::path::PathBuf;

use crossterm::event::{KeyCode, KeyModifiers};

use crate::backend::{
    self, BackendKind, Paths, PlannedCommand, ProcessControl, Runner, StartedProc, probe_system,
    select_backend,
};
use crate::capture::{self, CaptureControl, ConfirmResult};
use crate::iface::{self, IfaceInfo};
use crate::monitor::{self, MonitorState};
use crate::profile::{self, Profile};
use crate::wizard::{Page, Wizard};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum View {
    Wizard,
    Status,
    Monitor,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum HotspotStatus {
    Stopped,
    Running {
        ssid: String,
        interface: String,
        backend: String,
    },
    Failed {
        message: String,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    Continue,
    Quit,
    Apply,
    StopHotspot,
    OpenCapture,
}

pub struct App {
    pub view: View,
    pub wizard: Wizard,
    pub status: HotspotStatus,
    pub monitor: MonitorState,
    pub backend: BackendKind,
    pub capture: CaptureControl,
    pub interfaces: Vec<IfaceInfo>,
    pub notice: String,
    pub profile_path: PathBuf,
    pub running: bool,
    pub active: Option<Profile>,
    pub loaded_profile: bool,
    pub started: Vec<StartedProc>,
    pub open_extra: bool,
    pub open_upstream_ok: bool,
}

impl App {
    pub fn boot() -> Result<Self, String> {
        let backend = select_backend(&probe_system());
        let interfaces = iface::read_system_interfaces();
        let profile_path = profile::default_profile_path();
        let loaded = profile::load_optional(&profile_path)?;
        Ok(Self::from_parts(backend, interfaces, profile_path, loaded))
    }

    pub fn from_parts(
        backend: BackendKind,
        interfaces: Vec<IfaceInfo>,
        profile_path: PathBuf,
        loaded: Option<Profile>,
    ) -> Self {
        let loaded_profile = loaded.is_some();
        let wizard = match &loaded {
            Some(profile) => Wizard::from_profile(profile),
            None => Wizard::new(),
        };
        let notice = if loaded_profile {
            "The saved profile is loaded.".to_string()
        } else {
            String::new()
        };
        Self {
            view: View::Wizard,
            wizard,
            status: HotspotStatus::Stopped,
            monitor: MonitorState::new(),
            backend,
            capture: CaptureControl::new(),
            interfaces,
            notice,
            profile_path,
            running: false,
            active: None,
            loaded_profile,
            started: Vec::new(),
            open_extra: false,
            open_upstream_ok: false,
        }
    }

    pub fn needs_open_warning(&self) -> bool {
        self.wizard.open_upstream_risk()
    }

    pub fn on_key(&mut self, code: KeyCode, modifiers: KeyModifiers) -> Step {
        if modifiers.contains(KeyModifiers::CONTROL)
            && matches!(code, KeyCode::Char('q') | KeyCode::Char('Q'))
        {
            return Step::Quit;
        }
        match self.view {
            View::Wizard => self.on_wizard_key(code),
            View::Status => self.on_run_key(code, true),
            View::Monitor => self.on_run_key(code, false),
        }
    }

    pub fn apply_hotspot(
        &mut self,
        runner: &mut dyn Runner,
        signals: &mut dyn ProcessControl,
        paths: &Paths,
    ) -> Result<(), String> {
        if self.wizard.is_cancelled() {
            let message = "The wizard is cancelled. The settings were not applied.".to_string();
            self.notice = message.clone();
            return Err(message);
        }
        let profile = match self.wizard.confirmed_profile(&self.interfaces) {
            Ok(profile) => profile,
            Err(err) => {
                self.notice = err.clone();
                return Err(err);
            }
        };
        if profile.security == profile::SecurityMode::Open
            && profile.upstream_interface != "none"
            && !self.open_upstream_ok
        {
            let message = format!(
                "{} Confirm this warning before you apply the profile.",
                profile::OPEN_UPSTREAM_WARNING
            );
            self.notice = message.clone();
            self.open_upstream_ok = false;
            return Err(message);
        }
        let plan = match backend::plan_apply(self.backend, &profile, paths) {
            Ok(plan) => plan,
            Err(err) => {
                self.fail_apply(err.clone());
                return Err(err);
            }
        };
        let started = match backend::execute_plan(&plan, runner, signals, paths) {
            Ok(started) => started,
            Err(err) => {
                self.started.clear();
                self.fail_apply(err.clone());
                return Err(err);
            }
        };
        self.started = started;
        self.status = HotspotStatus::Running {
            ssid: profile.ssid.clone(),
            interface: profile.ap_interface.clone(),
            backend: self.backend.label().to_string(),
        };
        self.active = Some(profile.clone());
        self.running = true;
        self.view = View::Status;
        if let Err(err) = profile::save_profile(&self.profile_path, &profile) {
            self.notice = err.clone();
            return Err(err);
        }
        self.notice = "The hotspot is active.".to_string();
        Ok(())
    }

    pub fn stop_hotspot(
        &mut self,
        runner: &mut dyn Runner,
        signals: &mut dyn ProcessControl,
        paths: &Paths,
    ) -> Result<(), String> {
        self.capture.stop();
        let Some(profile) = self.active.clone() else {
            self.finish_stop();
            return Ok(());
        };
        if let Err(err) =
            backend::run_stop_commands(&backend::plan_stop(self.backend, &profile), runner)
        {
            self.notice = err.clone();
            return Err(err);
        }
        if !matches!(self.backend, BackendKind::NetworkManager | BackendKind::Iwd) {
            if let Err(err) = backend::stop_started(signals, &self.started) {
                self.notice = err.clone();
                return Err(err);
            }
        }
        if let Err(err) = backend::run_nft_delete(runner) {
            self.notice = err.clone();
            return Err(err);
        }
        if let Err(err) = backend::restore_forwarding(paths) {
            self.notice = err.clone();
            return Err(err);
        }
        if let Err(err) = backend::restore_hostapd_backup(paths) {
            self.notice = err.clone();
            return Err(err);
        }
        let _ = std::fs::remove_file(paths.hostapd_pid());
        let _ = std::fs::remove_file(paths.dnsmasq_pid());
        let iwd_profile = paths.iwd_ap_dir.join(format!("{}.ap", profile.ssid));
        if let Err(err) = backend::retire_iwd_profile(&iwd_profile) {
            self.notice = err.clone();
            return Err(err);
        }
        let _ = std::fs::remove_file(paths.nm_secret());
        self.started.clear();
        self.finish_stop();
        Ok(())
    }

    pub fn refresh_clients(&mut self, runner: &mut dyn Runner) -> Result<(), String> {
        if !self.running {
            return Ok(());
        }
        let Some(profile) = &self.active else {
            return Ok(());
        };
        let iface = profile.ap_interface.clone();
        let dump = runner
            .run(&PlannedCommand::new(
                "iw",
                ["dev", iface.as_str(), "station", "dump"],
            ))
            .map_err(|err| format!("The client list is not available. {err}"))?;
        let neigh = runner
            .run(&PlannedCommand::new(
                "ip",
                ["neigh", "show", "dev", iface.as_str()],
            ))
            .unwrap_or_default();
        let clients = monitor::clients_from_text(&dump, &neigh);
        self.monitor.update(&clients);
        Ok(())
    }

    pub fn tick(&mut self, runner: &mut dyn Runner) {
        if self.running {
            if let Err(err) = self.refresh_clients(runner) {
                self.notice = err;
            }
        }
        if let Err(err) = self.capture.poll() {
            self.notice = err;
        }
    }

    pub fn capture_open_failed(&mut self, message: String) {
        self.notice = self.capture.fail_open(message);
    }

    fn finish_stop(&mut self) {
        self.running = false;
        self.monitor.clear();
        self.status = HotspotStatus::Stopped;
        self.notice = "The hotspot is stopped.".to_string();
    }

    fn fail_apply(&mut self, message: String) {
        self.wizard.set_error(message.clone());
        self.status = HotspotStatus::Failed {
            message: message.clone(),
        };
        self.notice = message;
        self.running = false;
    }

    fn on_wizard_key(&mut self, code: KeyCode) -> Step {
        match code {
            KeyCode::Enter => {
                if self.wizard.page == Page::Review {
                    self.review_enter()
                } else {
                    self.open_extra = false;
                    self.open_upstream_ok = false;
                    if let Err(err) = self.wizard.next(&self.interfaces) {
                        self.notice = err;
                    } else {
                        self.notice.clear();
                    }
                    Step::Continue
                }
            }
            KeyCode::Esc => {
                self.open_extra = false;
                self.open_upstream_ok = false;
                self.wizard.cancel();
                self.view = View::Status;
                self.notice = "The wizard is cancelled. The settings were not applied.".to_string();
                Step::Continue
            }
            KeyCode::Left => {
                self.open_extra = false;
                self.open_upstream_ok = false;
                self.wizard.back();
                Step::Continue
            }
            KeyCode::Backspace => {
                self.wizard.backspace();
                Step::Continue
            }
            KeyCode::Tab => {
                self.wizard.next_field();
                Step::Continue
            }
            KeyCode::Char('y') if self.wizard.page == Page::Review => self.review_confirm_key(),
            KeyCode::Char(ch) => {
                self.wizard.push_char(ch);
                Step::Continue
            }
            _ => Step::Continue,
        }
    }

    fn review_enter(&mut self) -> Step {
        if self.needs_open_warning() {
            self.open_extra = true;
            self.open_upstream_ok = false;
            self.notice = format!(
                "{} Press y to apply this open hotspot.",
                profile::OPEN_UPSTREAM_WARNING
            );
            return Step::Continue;
        }
        Step::Apply
    }

    fn review_confirm_key(&mut self) -> Step {
        if self.needs_open_warning() && self.open_extra {
            self.open_upstream_ok = true;
            return Step::Apply;
        }
        if self.needs_open_warning() {
            self.notice = format!(
                "{} Press Enter to read this warning, then press y.",
                profile::OPEN_UPSTREAM_WARNING
            );
            return Step::Continue;
        }
        Step::Continue
    }

    fn on_run_key(&mut self, code: KeyCode, status_view: bool) -> Step {
        match code {
            KeyCode::Char('q') => Step::Quit,
            KeyCode::Char('c') => self.request_capture(),
            KeyCode::Enter => self.accept_capture(),
            other => {
                self.capture.dismiss_warning();
                match other {
                    KeyCode::Char('m') if status_view => {
                        self.view = View::Monitor;
                        Step::Continue
                    }
                    KeyCode::Char('s') if !status_view => {
                        self.view = View::Status;
                        Step::Continue
                    }
                    KeyCode::Esc if !status_view => {
                        self.view = View::Status;
                        Step::Continue
                    }
                    KeyCode::Char('w') if status_view => {
                        self.wizard.reopen();
                        self.view = View::Wizard;
                        Step::Continue
                    }
                    KeyCode::Char('k') => Step::StopHotspot,
                    KeyCode::Char('z') => {
                        self.capture.stop();
                        self.notice = "Capture is stopped.".to_string();
                        Step::Continue
                    }
                    _ => Step::Continue,
                }
            }
        }
    }

    fn request_capture(&mut self) -> Step {
        let iface = match self.hotspot_iface() {
            Ok(iface) => iface.to_string(),
            Err(err) => {
                self.notice = err;
                return Step::Continue;
            }
        };
        match self.capture.warn(&iface) {
            Ok(ConfirmResult::Warning) => {
                self.notice = capture::CAPTURE_WARNING.to_string();
                Step::Continue
            }
            Ok(ConfirmResult::Already) => {
                self.notice = "Capture is already active.".to_string();
                Step::Continue
            }
            Ok(ConfirmResult::Open { .. } | ConfirmResult::Ignored) => Step::Continue,
            Err(err) => {
                self.notice = err;
                Step::Continue
            }
        }
    }

    fn accept_capture(&mut self) -> Step {
        let iface = match self.hotspot_iface() {
            Ok(iface) => iface.to_string(),
            Err(_) => return Step::Continue,
        };
        match self.capture.accept(&iface) {
            Ok(ConfirmResult::Open { .. }) => Step::OpenCapture,
            Ok(ConfirmResult::Warning) => {
                self.notice = capture::CAPTURE_WARNING.to_string();
                Step::Continue
            }
            Ok(ConfirmResult::Already | ConfirmResult::Ignored) => Step::Continue,
            Err(err) => {
                self.notice = err;
                Step::Continue
            }
        }
    }

    fn hotspot_iface(&self) -> Result<&str, String> {
        if !self.running {
            return Err("The hotspot is not active.".to_string());
        }
        self.active
            .as_ref()
            .map(|profile| profile.ap_interface.as_str())
            .ok_or_else(|| "The hotspot interface is not set.".to_string())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::{RecordedSignals, ScriptedRunner};
    use crate::iface::sample_interfaces;
    use crate::profile::{sample_profile, scratch_dir};
    use std::fs;
    use std::path::Path;

    fn loaded_app(dir: &Path) -> App {
        App::from_parts(
            BackendKind::NetworkManager,
            sample_interfaces(),
            dir.join("profile.json"),
            Some(sample_profile()),
        )
    }

    #[test]
    fn start_loads_the_saved_profile_into_the_wizard() {
        let dir = scratch_dir();
        let app = loaded_app(&dir);
        assert!(app.loaded_profile);
        assert_eq!(app.wizard.page, Page::Review);
        assert_eq!(app.wizard.ssid, "Hotmon");
        assert_eq!(app.view, View::Wizard);
        assert!(!app.running);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn confirm_saves_the_profile_and_shows_status() {
        let dir = scratch_dir();
        let mut app = loaded_app(&dir);
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let mut runner = ScriptedRunner::default();
        app.apply_hotspot(&mut runner, &mut RecordedSignals::default(), &paths)
            .unwrap();
        assert!(app.running);
        assert_eq!(app.view, View::Status);
        assert!(matches!(app.status, HotspotStatus::Running { .. }));
        let saved = profile::load_profile(&app.profile_path).unwrap();
        assert_eq!(saved.ssid, "Hotmon");
        assert!(!runner.calls.is_empty());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn cancel_does_not_apply_the_profile() {
        let dir = scratch_dir();
        let mut app = loaded_app(&dir);
        let step = app.on_key(KeyCode::Esc, KeyModifiers::NONE);
        assert_eq!(step, Step::Continue);
        assert!(app.wizard.is_cancelled());
        let mut runner = ScriptedRunner::default();
        let paths = Paths::system();
        let error = app
            .apply_hotspot(&mut runner, &mut RecordedSignals::default(), &paths)
            .unwrap_err();
        assert!(error.contains("cancelled"));
        assert!(runner.calls.is_empty());
        assert!(!app.profile_path.exists());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn rejected_backend_keeps_the_hotspot_stopped() {
        let dir = scratch_dir();
        let mut app = loaded_app(&dir);
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let mut runner = ScriptedRunner::with_results(vec![
            Err("missing".to_string()),
            Err("the channel is not supported".to_string()),
        ]);
        let error = app
            .apply_hotspot(&mut runner, &mut RecordedSignals::default(), &paths)
            .unwrap_err();
        assert!(error.contains("The backend rejected the setting."));
        assert!(!app.running);
        assert!(matches!(app.status, HotspotStatus::Failed { .. }));
        assert!(!app.profile_path.exists());
        assert!(!matches!(app.status, HotspotStatus::Running { .. }));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn second_capture_key_arms_capture_and_stop_clears_it() {
        let dir = scratch_dir();
        let mut app = loaded_app(&dir);
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        app.apply_hotspot(
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        app.view = View::Monitor;
        let first = app.on_key(KeyCode::Char('c'), KeyModifiers::NONE);
        assert_eq!(first, Step::Continue);
        assert!(!app.capture.is_running());
        assert!(app.notice.contains("Press Enter"));
        let second = app.on_key(KeyCode::Char('c'), KeyModifiers::NONE);
        assert_eq!(second, Step::Continue);
        assert!(!app.capture.is_running());
        assert!(app.capture.is_warned());
        let third = app.on_key(KeyCode::Enter, KeyModifiers::NONE);
        assert_eq!(third, Step::OpenCapture);
        assert!(!app.capture.is_running());
        app.capture_open_failed("permission denied".to_string());
        assert!(!app.capture.is_running());
        assert!(app.notice.contains("did not start"));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn stop_hotspot_stops_capture_and_records_the_backend_stop() {
        let dir = scratch_dir();
        let mut app = App::from_parts(
            BackendKind::Direct,
            sample_interfaces(),
            dir.join("profile.json"),
            Some(sample_profile()),
        );
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        app.apply_hotspot(
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        fs::create_dir_all(&paths.state_dir).unwrap();
        fs::write(paths.hostapd_pid(), "42\n").unwrap();
        app.started.push(StartedProc {
            pid: 42,
            name: "hostapd".to_string(),
        });
        app.view = View::Status;
        assert_eq!(
            app.on_key(KeyCode::Char('k'), KeyModifiers::NONE),
            Step::StopHotspot
        );
        let mut runner = ScriptedRunner::default();
        let mut signals = RecordedSignals {
            names: vec![(42, "hostapd".to_string())],
            ..RecordedSignals::default()
        };
        app.stop_hotspot(&mut runner, &mut signals, &paths).unwrap();
        assert!(!app.running);
        assert!(!app.capture.is_running());
        assert_eq!(app.status, HotspotStatus::Stopped);
        assert!(runner.calls.iter().any(|command| command.program == "nft"));
        assert_eq!(signals.pids, vec![42]);
        assert!(!paths.hostapd_pid().exists());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn client_refresh_updates_the_monitor() {
        let dir = scratch_dir();
        let mut app = loaded_app(&dir);
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        app.apply_hotspot(
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        let dump = "Station aa:bb:cc:dd:ee:ff (on wlan0)\n\trx bytes:\t10\n\ttx bytes:\t5\n";
        let mut runner =
            ScriptedRunner::with_results(vec![Ok(dump.to_string()), Ok(String::new())]);
        app.tick(&mut runner);
        assert_eq!(app.monitor.clients().len(), 1);
        assert_eq!(app.monitor.clients()[0].rx_bytes, 10);
        assert_eq!(app.monitor.clients()[0].tx_bytes, 5);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn open_upstream_needs_a_different_confirmation() {
        let dir = scratch_dir();
        let mut app = loaded_app(&dir);
        app.wizard.security = "open".to_string();
        app.wizard.passphrase.clear();
        app.backend = BackendKind::Direct;
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        assert!(app.needs_open_warning());
        assert!(app.wizard.hint().contains("radio range"));
        assert_eq!(
            app.on_key(KeyCode::Enter, KeyModifiers::NONE),
            Step::Continue
        );
        assert!(!app.running);
        assert!(app.notice.contains("radio range"));
        let error = app
            .apply_hotspot(
                &mut ScriptedRunner::default(),
                &mut RecordedSignals::default(),
                &paths,
            )
            .unwrap_err();
        assert!(error.contains("radio range"));
        assert!(!app.running);
        assert_eq!(
            app.on_key(KeyCode::Enter, KeyModifiers::NONE),
            Step::Continue
        );
        assert_eq!(
            app.on_key(KeyCode::Char('y'), KeyModifiers::NONE),
            Step::Apply
        );
        app.apply_hotspot(
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        assert!(app.running);
        let saved = profile::load_profile(&app.profile_path).unwrap();
        assert!(saved.passphrase.is_empty());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn stop_reports_failure_while_dnsmasq_is_running() {
        let dir = scratch_dir();
        let mut app = App::from_parts(
            BackendKind::Direct,
            sample_interfaces(),
            dir.join("profile.json"),
            Some(sample_profile()),
        );
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        app.apply_hotspot(
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        app.started.push(StartedProc {
            pid: 8,
            name: "dnsmasq".to_string(),
        });
        let mut signals = RecordedSignals {
            names: vec![(8, "dnsmasq".to_string())],
            stay_alive: true,
            ..RecordedSignals::default()
        };
        let error = app
            .stop_hotspot(&mut ScriptedRunner::default(), &mut signals, &paths)
            .unwrap_err();
        assert!(error.contains("not stopped"));
        assert!(app.running);
        assert_ne!(app.status, HotspotStatus::Stopped);
        assert!(!app.notice.contains("The hotspot is stopped."));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_failed_start_keeps_the_old_profile() {
        let dir = scratch_dir();
        let mut app = loaded_app(&dir);
        profile::save_profile(&app.profile_path, &sample_profile()).unwrap();
        app.wizard.passphrase = "another-secret-value".to_string();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let mut runner = ScriptedRunner::with_results(vec![
            Err("missing".to_string()),
            Err("refused".to_string()),
        ]);
        let error = app
            .apply_hotspot(&mut runner, &mut RecordedSignals::default(), &paths)
            .unwrap_err();
        assert!(error.contains("The backend rejected the setting."));
        assert!(!app.running);
        let text = fs::read_to_string(&app.profile_path).unwrap();
        assert!(text.contains("correct-horse"));
        assert!(!text.contains("another-secret-value"));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn stop_removes_the_manager_profile_it_created() {
        let dir = scratch_dir();
        let paths = Paths {
            state_dir: dir.join("run"),
            hostapd_config: dir.join("hostapd.conf"),
            iwd_ap_dir: dir.join("iwd"),
            proc_root: dir.join("proc"),
        };
        let mut app = App::from_parts(
            BackendKind::NetworkManager,
            sample_interfaces(),
            dir.join("profile.json"),
            Some(sample_profile()),
        );
        app.apply_hotspot(
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        let mut signals = RecordedSignals {
            names: vec![(42, "hostapd".to_string())],
            ..RecordedSignals::default()
        };
        app.started.push(StartedProc {
            pid: 42,
            name: "hostapd".to_string(),
        });
        let mut runner = ScriptedRunner::default();
        app.stop_hotspot(&mut runner, &mut signals, &paths).unwrap();
        assert!(signals.pids.is_empty());
        assert!(runner.calls.iter().any(|command| {
            command.program == "nmcli" && command.args.iter().any(|arg| arg == "delete")
        }));
        let profile = sample_profile();
        profile::save_profile(&app.profile_path, &profile).unwrap();
        let mut iwd = App::from_parts(
            BackendKind::Iwd,
            sample_interfaces(),
            dir.join("other.json"),
            Some(profile.clone()),
        );
        let iwd_path = paths.iwd_ap_dir.join(format!("{}.ap", profile.ssid));
        fs::create_dir_all(&paths.iwd_ap_dir).unwrap();
        fs::write(&iwd_path, "Passphrase=correct-horse\n").unwrap();
        iwd.apply_hotspot(
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        iwd.stop_hotspot(
            &mut ScriptedRunner::default(),
            &mut RecordedSignals::default(),
            &paths,
        )
        .unwrap();
        assert!(!iwd_path.exists());
        assert_eq!(iwd.status, HotspotStatus::Stopped);
        let _ = fs::remove_dir_all(&dir);
    }
}
