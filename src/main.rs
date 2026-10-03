mod app;
mod backend;
mod capture;
mod capture_lib;
mod iface;
mod monitor;
mod profile;
mod ui;
mod wizard;

fn main() -> std::io::Result<()> {
    let previous = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        ratatui::restore();
        previous(info);
    }));
    let mut app = app::App::boot().map_err(|err| {
        eprintln!("{err}");
        std::io::Error::other(err)
    })?;
    let mut terminal = ratatui::try_init()?;
    let result = ui::run(&mut terminal, &mut app);
    ratatui::restore();
    result
}
