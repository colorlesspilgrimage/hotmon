#include "terminal.hpp"

#include "backend_exec.hpp"
#include "capture_socket.hpp"
#include "render.hpp"

#include <clocale>
#include <csignal>
#include <cstring>
#include <exception>
#include <string>

#include <ncurses.h>

namespace hotmon {
namespace {

Terminal* active_terminal = nullptr;

void restore_terminal() {
  if (active_terminal != nullptr) {
    endwin();
  }
}

void on_signal(int) { restore_terminal(); }

Key map_key(wint_t ch, bool function_key) {
  Key key;
  if (function_key) {
    switch (ch) {
      case KEY_LEFT:
        key.code = Key::Code::Left;
        break;
      case KEY_RIGHT:
        key.code = Key::Code::Right;
        break;
      case KEY_UP:
        key.code = Key::Code::Up;
        break;
      case KEY_DOWN:
        key.code = Key::Code::Down;
        break;
      case KEY_ENTER:
        key.code = Key::Code::Enter;
        break;
      case KEY_BACKSPACE:
        key.code = Key::Code::Backspace;
        break;
      case KEY_RESIZE:
        key.code = Key::Code::Other;
        break;
      default:
        key.code = Key::Code::Other;
        break;
    }
    return key;
  }
  if (ch == '\n' || ch == '\r') {
    key.code = Key::Code::Enter;
  } else if (ch == 27) {
    key.code = Key::Code::Esc;
  } else if (ch == '\t') {
    key.code = Key::Code::Tab;
  } else if (ch == 127 || ch == 8) {
    key.code = Key::Code::Backspace;
  } else if (ch == 17) {
    key.code = Key::Code::Char;
    key.ch = U'q';
    key.ctrl = true;
  } else {
    key.code = Key::Code::Char;
    key.ch = static_cast<char32_t>(ch);
  }
  return key;
}

void open_capture(App& app) {
  const auto iface = app.capture.armed_interface();
  if (!iface) {
    app.notice = "Capture is not confirmed.";
    return;
  }
  auto source = LocalCapture::open(*iface);
  if (!source) {
    app.capture_open_failed(source.error());
    return;
  }
  auto pointer = std::make_unique<LocalCapture>(std::move(*source));
  if (auto attached = app.capture.attach(std::move(pointer)); !attached) {
    app.notice = attached.error();
  } else {
    app.notice = "Capture is active.";
  }
}

}  // namespace

Terminal::Terminal() {
  active_terminal = this;
  std::setlocale(LC_ALL, "");
  std::set_terminate([] {
    restore_terminal();
    std::abort();
  });
  ::signal(SIGTERM, on_signal);
  ::signal(SIGINT, on_signal);
  ::signal(SIGHUP, on_signal);
  initscr();
  raw();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  set_escdelay(25);
  timeout(200);
  start_color();
  use_default_colors();
  init_pair(1, COLOR_YELLOW, -1);
}

Terminal::~Terminal() {
  endwin();
  if (active_terminal == this) {
    active_terminal = nullptr;
  }
}

void Terminal::draw(const App& app) {
  const auto lines = render(app, COLS, LINES);
  erase();
  for (size_t row = 0; row < lines.size() && static_cast<int>(row) < LINES; ++row) {
    if (row == 2) {
      attron(COLOR_PAIR(1));
    }
    mvaddstr(static_cast<int>(row), 0, lines[row].c_str());
    if (row == 2) {
      attroff(COLOR_PAIR(1));
    }
  }
  refresh();
}

int Terminal::read_key(App& app, Key& key) {
  wint_t ch = 0;
  const int rc = get_wch(&ch);
  if (rc == ERR) {
    return ERR;
  }
  if (rc == KEY_CODE_YES && ch == KEY_RESIZE) {
    return KEY_RESIZE;
  }
  key = map_key(ch, rc == KEY_CODE_YES);
  (void)app;
  return OK;
}

int run_ui(App& app) {
  SystemRunner runner;
  SystemSignals signals;
  const Paths paths = Paths::system();
  Terminal terminal;
  while (true) {
    terminal.draw(app);
    Key key;
    const int rc = terminal.read_key(app, key);
    if (rc == ERR) {
      app.tick(runner);
      continue;
    }
    if (rc == KEY_RESIZE) {
      continue;
    }
    switch (app.on_key(key)) {
      case Step::Quit:
        return 0;
      case Step::Apply:
        if (auto applied = app.apply_hotspot(runner, signals, paths); !applied) {
          app.notice = applied.error();
        }
        break;
      case Step::StopHotspot:
        if (auto stopped = app.stop_hotspot(runner, signals, paths); !stopped) {
          app.notice = stopped.error();
        }
        break;
      case Step::OpenCapture:
        open_capture(app);
        break;
      case Step::Continue:
        break;
    }
  }
}

}  // namespace hotmon
