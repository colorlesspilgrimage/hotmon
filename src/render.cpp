#include "render.hpp"

#include <algorithm>
#include <langinfo.h>
#include <locale.h>

namespace hotmon {
namespace {

bool utf8_locale() {
  const char* codeset = nl_langinfo(CODESET);
  return codeset != nullptr && std::string(codeset) == "UTF-8";
}

std::string pad(std::string text, int width) {
  if (width < 0) {
    width = 0;
  }
  if (static_cast<int>(text.size()) > width) {
    text.resize(static_cast<size_t>(width));
  }
  text.append(static_cast<size_t>(width) - text.size(), ' ');
  return text;
}

std::string box_top(const std::string& title, int width) {
  std::string line(static_cast<size_t>(std::max(width, 2)), '-');
  line.front() = '+';
  line.back() = '+';
  if (!title.empty() && width > 4) {
    const std::string label = " " + title + " ";
    const size_t count = std::min(label.size(), static_cast<size_t>(width - 2));
    line.replace(1, count, label.substr(0, count));
  }
  return line;
}

std::string box_bottom(int width) { return box_top("", width); }

std::string box_row(const std::string& text, int width) {
  const int inner = std::max(width - 2, 0);
  return "|" + pad(text, inner) + "|";
}

std::vector<std::string> wrap_text(const std::string& text, int width) {
  if (width <= 0) {
    return {text};
  }
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    lines.push_back(text.substr(start, static_cast<size_t>(width)));
    start += static_cast<size_t>(width);
  }
  if (lines.empty()) {
    lines.emplace_back();
  }
  return lines;
}

void append_box(std::vector<std::string>& lines, const std::string& title,
                const std::vector<std::string>& rows, int width, bool wrap) {
  lines.push_back(box_top(title, width));
  if (wrap) {
    const int inner = std::max(width - 2, 0);
    for (const std::string& line : rows) {
      for (const std::string& part : wrap_text(line, inner)) {
        lines.push_back(box_row(part, width));
      }
    }
  } else {
    for (const std::string& row : rows) {
      lines.push_back(box_row(row, width));
    }
  }
  lines.push_back(box_bottom(width));
}

std::string footer_text(const App& app) {
  if (app.view == View::Status) {
    return "m: monitor  c: capture  z: stop capture  k: stop hotspot  w: wizard  q: quit";
  }
  if (app.view == View::Monitor) {
    return "s: status  c: capture  z: stop capture  k: stop hotspot  q: quit";
  }
  if (app.wizard.page == Page::Review && app.needs_open_warning()) {
    return "y: apply open hotspot  Enter does not apply  Left: previous page  Esc: cancel  Ctrl+q: quit";
  }
  if (app.wizard.page == Page::Review) {
    return "Enter: apply  Left: previous page  Esc: cancel  Ctrl+q: quit";
  }
  if (app.wizard.page == Page::AddressDhcp) {
    return "Enter: accept  a: advanced setup  d: automatic values  Left: previous page  Esc: cancel  Ctrl+q: quit";
  }
  if (app.wizard.page == Page::Ssid || app.wizard.page == Page::Passphrase ||
      app.wizard.page == Page::Advanced) {
    return "Enter: next page  Left: previous page  Esc: cancel  Tab: next field  Ctrl+q: quit";
  }
  return "Up/Down: choose  Enter: next page  Left: previous page  Esc: cancel  Ctrl+q: quit";
}

std::vector<std::string> header_lines(const App& app, int width) {
  const char* view = "Wizard";
  if (app.view == View::Status) {
    view = "Status";
  } else if (app.view == View::Monitor) {
    view = "Monitor";
  }
  const std::string notice = app.notice.empty() && app.wizard.error ? *app.wizard.error : app.notice;
  std::vector<std::string> lines;
  append_box(lines, "hotmon",
             {"hotmon  " + std::string(view) + "  " + label(app.backend), notice}, width, true);
  return lines;
}

std::vector<std::string> wizard_lines(const App& app, int width) {
  const auto [page, total] = app.wizard.position();
  std::vector<std::string> body;
  body.push_back("Page " + std::to_string(page) + " of " + std::to_string(total) + ": " +
                 page_title(app.wizard.page));
  body.push_back(app.wizard.hint());
  body.emplace_back();
  if (app.wizard.page == Page::Review) {
    for (const std::string& line : app.wizard.review_lines()) {
      body.push_back(line);
    }
  } else {
    for (const FieldLine& field : app.wizard.field_lines()) {
      if (field.selection) {
        body.push_back(std::string(field.active ? "> " : "  ") + field.label);
        for (size_t index = 0; index < field.choices.size(); ++index) {
          body.push_back(std::string(index == field.selected ? "(*) " : "( ) ") +
                         field.choices[index].label);
        }
      } else {
        body.push_back(std::string(field.active ? "> " : "  ") + field.label + ": " + field.value);
      }
    }
  }
  std::vector<std::string> lines;
  append_box(lines, page_title(app.wizard.page), body, width, true);
  return lines;
}

std::vector<std::string> status_lines(const App& app, int width) {
  std::vector<std::string> body;
  if (app.status.kind == HotspotStatus::Kind::Running) {
    body.push_back("The hotspot " + app.status.ssid + " is active on " + app.status.interface +
                   ". Backend: " + app.status.backend + ".");
  } else if (app.status.kind == HotspotStatus::Kind::Failed) {
    body.push_back(app.status.message);
  } else {
    body.emplace_back("The hotspot is stopped.");
  }
  if (app.active) {
    body.push_back("SSID: " + app.active->ssid);
    body.push_back("Interface: " + app.active->ap_interface);
  } else if (app.loaded_profile) {
    body.emplace_back("A saved profile is loaded. It is not applied yet.");
  }
  if (app.capture.is_running()) {
    body.emplace_back("Capture is active.");
    for (const std::string& line : app.capture.recent_lines(5)) {
      body.push_back(line);
    }
  }
  std::vector<std::string> lines;
  append_box(lines, "Status", body, width, true);
  return lines;
}

std::vector<std::string> monitor_lines(const App& app, int width) {
  std::vector<std::string> lines;
  append_box(lines, "Total traffic",
             {sparkline_text(app.monitor.total_samples(), std::max(width - 2, 1))}, width, false);
  std::vector<std::string> packets = {
      app.capture.is_running() ? "Capture is active." : "Capture is stopped."};
  const auto recent = app.capture.recent_lines(4);
  if (recent.empty()) {
    packets.emplace_back("No packets.");
  } else {
    packets.insert(packets.end(), recent.begin(), recent.end());
  }
  append_box(lines, "Packets", packets, width, false);
  const auto clients = app.monitor.clients();
  const size_t shown = std::min<size_t>(clients.size(), 4);
  for (size_t index = 0; index < shown; ++index) {
    const auto& client = clients[index];
    append_box(lines,
               client.mac + " rx " + std::to_string(client.rx_bytes) + " tx " +
                   std::to_string(client.tx_bytes),
               {sparkline_text(client.graph.samples(), std::max(width - 2, 1))}, width, false);
  }
  if (clients.empty()) {
    append_box(lines, "Devices", {"No devices are connected."}, width, false);
  } else if (clients.size() > shown) {
    lines.push_back(std::to_string(clients.size() - shown) + " more devices.");
  }
  return lines;
}

}  // namespace

std::string sparkline_text(const std::vector<uint64_t>& samples, int width) {
  static const char* bars[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
  const bool unicode = utf8_locale();
  const int count = std::max(width, 0);
  std::vector<uint64_t> shown;
  if (count == 0) {
    return {};
  }
  if (static_cast<int>(samples.size()) > count) {
    shown.assign(samples.end() - count, samples.end());
  } else {
    shown = samples;
  }
  const uint64_t max = shown.empty() ? 0 : *std::max_element(shown.begin(), shown.end());
  std::string text;
  for (uint64_t value : shown) {
    size_t index = 0;
    if (max != 0) {
      index = static_cast<size_t>(value * 7 / max);
    }
    if (unicode) {
      text += bars[index];
    } else {
      text += index == 0 ? "_" : "#";
    }
  }
  return text;
}

std::vector<std::string> render(const App& app, int width, int height) {
  if (width < 2) {
    width = 2;
  }
  if (height < 1) {
    height = 1;
  }
  std::vector<std::string> lines = header_lines(app, width);
  const std::vector<std::string> body = app.view == View::Wizard    ? wizard_lines(app, width)
                                        : app.view == View::Monitor ? monitor_lines(app, width)
                                                                    : status_lines(app, width);
  lines.insert(lines.end(), body.begin(), body.end());
  lines.push_back(box_top("Keys", width));
  for (const std::string& part : wrap_text(footer_text(app), std::max(width - 2, 0))) {
    lines.push_back(box_row(part, width));
  }
  lines.push_back(box_bottom(width));
  if (static_cast<int>(lines.size()) < height) {
    lines.resize(static_cast<size_t>(height), std::string(static_cast<size_t>(width), ' '));
  } else if (static_cast<int>(lines.size()) > height) {
    lines.resize(static_cast<size_t>(height));
  }
  return lines;
}

}  // namespace hotmon
