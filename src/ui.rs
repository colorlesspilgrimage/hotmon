use std::io;
use std::time::Duration;

use crossterm::event::{self, Event, KeyEventKind};
use ratatui::layout::{Constraint, Layout};
use ratatui::style::{Color, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Paragraph, Sparkline, Wrap};
use ratatui::{DefaultTerminal, Frame};

use crate::app::{App, HotspotStatus, Step, View};
use crate::backend::{self, SystemRunner, SystemSignals};
use crate::capture_lib::LocalCapture;

pub fn run(terminal: &mut DefaultTerminal, app: &mut App) -> io::Result<()> {
    let mut runner = SystemRunner;
    let mut signals = SystemSignals;
    let paths = backend::Paths::system();
    loop {
        terminal.draw(|frame| draw(frame, app))?;
        if event::poll(Duration::from_millis(200))? {
            let event = event::read()?;
            if let Event::Key(key) = event {
                if key.kind != KeyEventKind::Press {
                    continue;
                }
                match app.on_key(key.code, key.modifiers) {
                    Step::Quit => break,
                    Step::Apply => {
                        if let Err(err) = app.apply_hotspot(&mut runner, &mut signals, &paths) {
                            app.notice = err;
                        }
                    }
                    Step::StopHotspot => {
                        if let Err(err) = app.stop_hotspot(&mut runner, &mut signals, &paths) {
                            app.notice = err;
                        }
                    }
                    Step::OpenCapture => open_capture(app),
                    Step::Continue => {}
                }
            }
        } else {
            app.tick(&mut runner);
        }
    }
    Ok(())
}

fn open_capture(app: &mut App) {
    let Some(iface) = app.capture.armed_interface().map(str::to_string) else {
        app.notice = "Capture is not confirmed.".to_string();
        return;
    };
    match LocalCapture::open(&iface) {
        Ok(source) => {
            if let Err(err) = app.capture.attach(Box::new(source)) {
                app.notice = err;
            } else {
                app.notice = "Capture is active.".to_string();
            }
        }
        Err(err) => app.capture_open_failed(err.to_string()),
    }
}

fn draw(frame: &mut Frame, app: &App) {
    let area = frame.area();
    let chunks = Layout::vertical([
        Constraint::Length(4),
        Constraint::Fill(1),
        Constraint::Length(3),
    ])
    .split(area);
    frame.render_widget(header(app), chunks[0]);
    match app.view {
        View::Wizard => frame.render_widget(wizard_body(app), chunks[1]),
        View::Status => frame.render_widget(status_body(app), chunks[1]),
        View::Monitor => draw_monitor(frame, app, chunks[1]),
    }
    frame.render_widget(footer(app), chunks[2]);
}

fn header(app: &App) -> Paragraph<'_> {
    let view = match app.view {
        View::Wizard => "Wizard",
        View::Status => "Status",
        View::Monitor => "Monitor",
    };
    let notice = if app.notice.is_empty() {
        app.wizard.error.clone().unwrap_or_default()
    } else {
        app.notice.clone()
    };
    let text = vec![
        Line::from(format!("hotmon  {view}  {}", app.backend.label())),
        Line::from(Span::styled(notice, Style::default().fg(Color::Yellow))),
    ];
    Paragraph::new(text).block(Block::bordered().title("hotmon"))
}

fn wizard_body(app: &App) -> Paragraph<'_> {
    let (page, total) = app.wizard.position();
    let mut lines = vec![
        Line::from(format!(
            "Page {page} of {total}: {}",
            app.wizard.page.title()
        )),
        Line::from(app.wizard.hint()),
        Line::from(""),
    ];
    if app.wizard.page == crate::wizard::Page::Review {
        if let Ok(profile) = preview_profile(app) {
            for line in profile.review_lines() {
                lines.push(Line::from(line));
            }
        } else {
            for field in app.wizard.field_lines() {
                lines.push(field_line(&field));
            }
        }
    } else {
        for field in app.wizard.field_lines() {
            lines.push(field_line(&field));
        }
    }
    Paragraph::new(lines)
        .block(Block::bordered().title(app.wizard.page.title()))
        .wrap(Wrap { trim: false })
}

fn field_line(field: &crate::wizard::FieldLine) -> Line<'static> {
    let mark = if field.active { ">" } else { " " };
    Line::from(format!("{mark} {}: {}", field.label, field.value))
}

fn preview_profile(app: &App) -> Result<crate::profile::Profile, String> {
    let mut wizard = app.wizard.clone();
    wizard.confirmed_profile(&app.interfaces)
}

fn status_body(app: &App) -> Paragraph<'_> {
    let state = match &app.status {
        HotspotStatus::Stopped => "The hotspot is stopped.".to_string(),
        HotspotStatus::Running {
            ssid,
            interface,
            backend,
        } => format!("The hotspot {ssid} is active on {interface}. Backend: {backend}."),
        HotspotStatus::Failed { message } => message.clone(),
    };
    let mut lines = vec![Line::from(state)];
    if let Some(profile) = &app.active {
        lines.push(Line::from(format!("SSID: {}", profile.ssid)));
        lines.push(Line::from(format!("Interface: {}", profile.ap_interface)));
    } else if app.loaded_profile {
        lines.push(Line::from(
            "A saved profile is loaded. It is not applied yet.",
        ));
    }
    if app.capture.is_running() {
        lines.push(Line::from("Capture is active."));
        for line in app.capture.recent_lines(5) {
            lines.push(Line::from(line.to_string()));
        }
    }
    Paragraph::new(lines)
        .block(Block::bordered().title("Status"))
        .wrap(Wrap { trim: false })
}

fn draw_monitor(frame: &mut Frame, app: &App, area: ratatui::layout::Rect) {
    let clients = app.monitor.clients();
    let shown = clients.len().min(4);
    let mut constraints = vec![Constraint::Length(4), Constraint::Length(6)];
    for _ in 0..shown {
        constraints.push(Constraint::Length(4));
    }
    constraints.push(Constraint::Fill(1));
    let chunks = Layout::vertical(constraints).split(area);
    let total = app.monitor.total_samples();
    let total_line = Sparkline::default()
        .block(Block::bordered().title("Total traffic"))
        .data(&total);
    frame.render_widget(total_line, chunks[0]);
    let mut packet_lines = vec![Line::from(if app.capture.is_running() {
        "Capture is active."
    } else {
        "Capture is stopped."
    })];
    if app.capture.recent_lines(1).is_empty() {
        packet_lines.push(Line::from("No packets."));
    } else {
        for line in app.capture.recent_lines(4) {
            packet_lines.push(Line::from(line.to_string()));
        }
    }
    frame.render_widget(
        Paragraph::new(packet_lines).block(Block::bordered().title("Packets")),
        chunks[1],
    );
    for (index, client) in clients.iter().take(shown).enumerate() {
        let samples = client.graph.samples();
        let title = format!(
            "{} rx {} tx {}",
            client.mac, client.rx_bytes, client.tx_bytes
        );
        let spark = Sparkline::default()
            .block(Block::bordered().title(title))
            .data(&samples);
        frame.render_widget(spark, chunks[index + 2]);
    }
    if clients.is_empty() {
        frame.render_widget(
            Paragraph::new("No devices are connected.").block(Block::bordered().title("Devices")),
            chunks[chunks.len() - 1],
        );
    } else if clients.len() > shown {
        frame.render_widget(
            Paragraph::new(format!("{} more devices.", clients.len() - shown)),
            chunks[chunks.len() - 1],
        );
    }
}

fn footer(app: &App) -> Paragraph<'static> {
    let text = match app.view {
        View::Wizard
            if app.wizard.page == crate::wizard::Page::Review && app.needs_open_warning() =>
        {
            "y: apply open hotspot  Enter does not apply  Left: previous page  Esc: cancel  Ctrl+q: quit"
        }
        View::Wizard if app.wizard.page == crate::wizard::Page::Review => {
            "Enter: apply  Left: previous page  Esc: cancel  Ctrl+q: quit"
        }
        View::Wizard => {
            "Enter: next page  Left: previous page  Esc: cancel  Tab: next field  Ctrl+q: quit"
        }
        View::Status => {
            "m: monitor  c: capture  z: stop capture  k: stop hotspot  w: wizard  q: quit"
        }
        View::Monitor => "s: status  c: capture  z: stop capture  k: stop hotspot  q: quit",
    };
    Paragraph::new(text).block(Block::bordered().title("Keys"))
}

#[cfg(test)]
mod tests {
    use std::path::PathBuf;

    use ratatui::Terminal;
    use ratatui::backend::TestBackend;

    use super::*;
    use crate::backend::BackendKind;

    fn buffer_text(app: &App) -> String {
        let backend = TestBackend::new(80, 24);
        let mut terminal = Terminal::new(backend).expect("test terminal");
        terminal.draw(|frame| draw(frame, app)).expect("draw");
        terminal
            .backend()
            .buffer()
            .content()
            .iter()
            .map(|cell| cell.symbol())
            .collect()
    }

    #[test]
    fn rejection_message_is_visible_in_the_header() {
        let mut app = App::from_parts(
            BackendKind::NetworkManager,
            Vec::new(),
            PathBuf::from("/tmp/hotmon-header-test.json"),
            None,
        );
        app.wizard.error = Some("The interface nope is not available.".to_string());
        let text = buffer_text(&app);
        assert!(
            text.contains("The interface nope is not available."),
            "header clipped the rejection message: {text}"
        );
    }

    #[test]
    fn review_and_status_do_not_show_the_passphrase() {
        let mut app = App::from_parts(
            BackendKind::NetworkManager,
            crate::iface::sample_interfaces(),
            PathBuf::from("/tmp/hotmon-secret-screen.json"),
            Some(crate::profile::sample_profile()),
        );
        let review = buffer_text(&app);
        assert!(review.contains("Passphrase: set"));
        assert!(!review.contains("correct-horse"), "{review}");
        app.wizard.page = crate::wizard::Page::Passphrase;
        let masked = buffer_text(&app);
        assert!(!masked.contains("correct-horse"), "{masked}");
        assert!(masked.contains("*************"));
        app.wizard.security = "open".to_string();
        app.wizard.passphrase.clear();
        app.wizard.upstream_interface = "eth0".to_string();
        app.wizard.page = crate::wizard::Page::Review;
        let warning = buffer_text(&app);
        assert!(warning.contains("radio range"), "{warning}");
        assert!(!warning.contains("correct-horse"));
        app.view = View::Status;
        app.active = Some(crate::profile::sample_profile());
        app.status = HotspotStatus::Running {
            ssid: "Hotmon".to_string(),
            interface: "wlan0".to_string(),
            backend: "NetworkManager".to_string(),
        };
        let status = buffer_text(&app);
        assert!(!status.contains("correct-horse"), "{status}");
        assert!(status.contains("Hotmon"));
    }
}
