#include "render.hpp"

#include <algorithm>
#include <langinfo.h>
#include <locale.h>
#include <wchar.h>

namespace hotmon {
namespace {

bool utf8_locale() {
  const char* codeset = nl_langinfo(CODESET);
  return codeset != nullptr && std::string(codeset) == "UTF-8";
}

// Return the byte length of the UTF-8 sequence at index. Set columns to its screen width.
size_t next_char(const std::string& text, size_t index, int& columns) {
  const auto byte = static_cast<unsigned char>(text[index]);
  size_t length = 1;
  char32_t code = byte;
  if (byte >= 0xF0) {
    length = 4;
    code = byte & 0x07;
  } else if (byte >= 0xE0) {
    length = 3;
    code = byte & 0x0F;
  } else if (byte >= 0xC0) {
    length = 2;
    code = byte & 0x1F;
  }
  size_t used = 1;
  while (used < length && index + used < text.size() &&
         (static_cast<unsigned char>(text[index + used]) & 0xC0) == 0x80) {
    code = (code << 6) | (static_cast<unsigned char>(text[index + used]) & 0x3F);
    ++used;
  }
  const int width = used == length ? ::wcwidth(static_cast<wchar_t>(code)) : -1;
  columns = width < 0 ? 1 : width;
  return used;
}

// Cut the text at a character boundary so that it uses at most width columns.
// Set used to the columns of the result.
size_t cut_columns(const std::string& text, size_t start, int width, int& used) {
  size_t index = start;
  used = 0;
  while (index < text.size()) {
    int columns = 0;
    const size_t length = next_char(text, index, columns);
    if (used + columns > width) {
      break;
    }
    used += columns;
    index += length;
  }
  return index;
}

std::string pad(std::string text, int width) {
  if (width < 0) {
    width = 0;
  }
  int used = 0;
  text.resize(cut_columns(text, 0, width, used));
  text.append(static_cast<size_t>(width - used), ' ');
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
    int used = 0;
    size_t end = cut_columns(text, start, width, used);
    if (end == start) {
      int columns = 0;
      end = start + next_char(text, start, columns);
    }
    lines.push_back(text.substr(start, end - start));
    start = end;
  }
  if (lines.empty()) {
    lines.emplace_back();
  }
  return lines;
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
  lines.push_back(box_top("hotmon", width));
  for (const std::string& part :
       wrap_text("hotmon  " + std::string(view) + "  " + label(app.backend), std::max(width - 2, 0))) {
    lines.push_back(box_row(part, width));
  }
  for (const std::string& part : wrap_text(notice, std::max(width - 2, 0))) {
    lines.push_back(box_row(part, width));
  }
  lines.push_back(box_bottom(width));
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
  lines.push_back(box_top(page_title(app.wizard.page), width));
  for (const std::string& line : body) {
    for (const std::string& part : wrap_text(line, std::max(width - 2, 0))) {
      lines.push_back(box_row(part, width));
    }
  }
  lines.push_back(box_bottom(width));
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
  lines.push_back(box_top("Status", width));
  for (const std::string& line : body) {
    for (const std::string& part : wrap_text(line, std::max(width - 2, 0))) {
      lines.push_back(box_row(part, width));
    }
  }
  lines.push_back(box_bottom(width));
  return lines;
}

std::vector<std::string> monitor_lines(const App& app, int width) {
  std::vector<std::string> lines;
  lines.push_back(box_top("Total traffic", width));
  lines.push_back(box_row(sparkline_text(app.monitor.total_samples(), std::max(width - 2, 1)), width));
  lines.push_back(box_bottom(width));
  lines.push_back(box_top("Packets", width));
  lines.push_back(box_row(app.capture.is_running() ? "Capture is active." : "Capture is stopped.", width));
  const auto packets = app.capture.recent_lines(4);
  if (packets.empty()) {
    lines.push_back(box_row("No packets.", width));
  } else {
    for (const std::string& line : packets) {
      lines.push_back(box_row(line, width));
    }
  }
  lines.push_back(box_bottom(width));
  const auto clients = app.monitor.clients();
  const size_t shown = std::min<size_t>(clients.size(), 4);
  for (size_t index = 0; index < shown; ++index) {
    const auto& client = clients[index];
    lines.push_back(box_top(client.mac + " rx " + std::to_string(client.rx_bytes) + " tx " +
                                std::to_string(client.tx_bytes),
                            width));
    lines.push_back(box_row(sparkline_text(client.graph.samples(), std::max(width - 2, 1)), width));
    lines.push_back(box_bottom(width));
  }
  if (clients.empty()) {
    lines.push_back(box_top("Devices", width));
    lines.push_back(box_row("No devices are connected.", width));
    lines.push_back(box_bottom(width));
  } else if (clients.size() > shown) {
    lines.push_back(std::to_string(clients.size() - shown) + " more devices.");
  }
  return lines;
}

}

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

}
