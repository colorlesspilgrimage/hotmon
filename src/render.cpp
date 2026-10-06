#include "render.hpp"

#include "format.hpp"

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

struct BoxChars {
  std::string tl;
  std::string tr;
  std::string bl;
  std::string br;
  std::string h;
  std::string v;
};

BoxChars box_chars() {
  if (utf8_locale()) {
    return {"╭", "╮", "╰", "╯", "─", "│"};
  }
  return {"+", "+", "+", "+", "-", "|"};
}

// Set printable to false for a control character. A control character uses one column.
size_t next_char(const std::string& text, size_t index, int& columns, bool& printable) {
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
  const bool control = used == length && (code < 0x20 || (code >= 0x7F && code < 0xA0));
  const int width = used == length && !control ? ::wcwidth(static_cast<wchar_t>(code)) : -1;
  printable = !control;
  columns = width < 0 ? 1 : width;
  return used;
}

size_t next_char(const std::string& text, size_t index, int& columns) {
  bool printable = true;
  return next_char(text, index, columns, printable);
}

// Replace each control character with '?'.
// The terminal then cannot get cursor movement or escape bytes from the text.
std::string printable_text(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  size_t index = 0;
  while (index < text.size()) {
    int columns = 0;
    bool printable = true;
    const size_t length = next_char(text, index, columns, printable);
    if (printable) {
      out.append(text, index, length);
    } else {
      out.push_back('?');
    }
    index += length;
  }
  return out;
}

// Cut the text at a character boundary so that it uses at most width columns.
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

std::string align_cell(const std::string& text, int width, bool right) {
  if (!right) {
    return pad(text, width);
  }
  int used = 0;
  const size_t end = cut_columns(text, 0, width, used);
  const int spaces = std::max(width - used, 0);
  return std::string(static_cast<size_t>(spaces), ' ') + text.substr(0, end);
}

std::string repeat_unit(const std::string& unit, int count) {
  std::string out;
  if (count <= 0 || unit.empty()) {
    return out;
  }
  out.reserve(unit.size() * static_cast<size_t>(count));
  for (int index = 0; index < count; ++index) {
    out += unit;
  }
  return out;
}

// Replace control characters with spaces. A control character moves the
// terminal cursor, so it breaks the box layout.
std::string printable(std::string text) {
  for (size_t index = 0; index < text.size(); ++index) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x20 || byte == 0x7F) {
      text[index] = ' ';
    } else if (byte == 0xC2 && index + 1 < text.size() &&
               static_cast<unsigned char>(text[index + 1]) >= 0x80 &&
               static_cast<unsigned char>(text[index + 1]) <= 0x9F) {
      text.replace(index, 2, " ");
    }
  }
  return text;
}

std::string box_top(const BoxChars& box, const std::string& title, int width) {
  width = std::max(width, 2);
  const int inner = width - 2;
  if (title.empty() || inner < 4) {
    return box.tl + repeat_unit(box.h, inner) + box.tr;
  }
  const std::string clean = printable(title);
  const int title_budget = inner - 3;
  int used = 0;
  const size_t end = cut_columns(clean, 0, title_budget, used);
  const int bars = inner - 3 - used;
  return box.tl + box.h + " " + clean.substr(0, end) + " " + repeat_unit(box.h, bars) + box.tr;
}

std::string box_bottom(const BoxChars& box, int width) {
  width = std::max(width, 2);
  return box.bl + repeat_unit(box.h, width - 2) + box.br;
}

std::string box_row(const BoxChars& box, const std::string& text, int width) {
  const int inner = std::max(width - 2, 0);
  return box.v + pad(printable(text), inner) + box.v;
}

// Wrap the text at spaces. Prefer a double space, because it separates the key items.
// Cut a word only when it is wider than the line.
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
    size_t next = end;
    if (end < text.size()) {
      size_t space = text.rfind("  ", end);
      if (space == std::string::npos || space <= start) {
        space = text[end] == ' ' ? end : text.rfind(' ', end - 1);
      }
      if (space != std::string::npos && space > start) {
        end = space;
        next = space;
      }
    }
    lines.push_back(text.substr(start, end - start));
    start = next;
    while (start < text.size() && text[start] == ' ') {
      ++start;
    }
  }
  if (lines.empty()) {
    lines.emplace_back();
  }
  return lines;
}

void append_box(std::vector<StyledRow>& lines, const BoxChars& box, const std::string& title,
                const std::vector<StyledRow>& rows, int width, bool wrap, int min_rows) {
  std::vector<StyledRow> inner;
  if (wrap) {
    const int inner_width = std::max(width - 2, 0);
    for (const StyledRow& row : rows) {
      for (const std::string& part : wrap_text(row.text, inner_width)) {
        inner.push_back(StyledRow{part, row.style});
      }
    }
  } else {
    inner = rows;
  }
  const int content = static_cast<int>(inner.size());
  const int target = std::max(min_rows, content + 2);
  const int blanks = std::max(target - 2 - content, 0);
  lines.push_back(StyledRow{box_top(box, title, width), Style::Border});
  for (const StyledRow& row : inner) {
    lines.push_back(StyledRow{box_row(box, row.text, width), row.style});
  }
  for (int index = 0; index < blanks; ++index) {
    lines.push_back(StyledRow{box_row(box, "", width), Style::Plain});
  }
  lines.push_back(StyledRow{box_bottom(box, width), Style::Border});
}

std::string footer_text(const App& app) {
  if (app.view == View::Status) {
    return "m: monitor  c: capture  z: stop capture  k: stop hotspot  f: FakeMii  w: wizard  q: quit";
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

Style run_style(const App& app) {
  if (app.status.kind == HotspotStatus::Kind::Running) {
    return Style::Good;
  }
  if (app.status.kind == HotspotStatus::Kind::Failed) {
    return Style::Warn;
  }
  return Style::Dim;
}

std::vector<StyledRow> header_lines(const App& app, int width, const BoxChars& box) {
  const char* view = "Wizard";
  if (app.view == View::Status) {
    view = "Status";
  } else if (app.view == View::Monitor) {
    view = "Monitor";
  }
  const std::string notice = app.notice.empty() && app.wizard.error ? *app.wizard.error : app.notice;
  std::vector<StyledRow> lines;
  append_box(lines, box, "hotmon",
             {StyledRow{"hotmon  " + std::string(view) + "  " + label(app.backend), run_style(app)},
              StyledRow{notice, Style::Warn}},
             width, true, 0);
  return lines;
}

std::string page_progress(size_t page, size_t total) {
  std::string text = "Page " + std::to_string(page) + " of " + std::to_string(total) + "  ";
  if (utf8_locale()) {
    for (size_t index = 1; index <= total; ++index) {
      text += index <= page ? "●" : "○";
    }
    return text;
  }
  text += "[";
  text.append(page, '#');
  if (total >= page) {
    text.append(total - page, '-');
  }
  text += "]";
  return text;
}

std::vector<StyledRow> wizard_lines(const App& app, int width, const BoxChars& box, int min_rows) {
  const auto [page, total] = app.wizard.position();
  std::vector<StyledRow> body;
  body.push_back(StyledRow{page_progress(page, total), Style::Accent});
  body.push_back(StyledRow{app.wizard.hint(), Style::Dim});
  body.emplace_back();
  if (app.wizard.page == Page::Review) {
    for (const std::string& line : app.wizard.review_lines()) {
      body.push_back(StyledRow{line, Style::Plain});
    }
  } else {
    for (const FieldLine& field : app.wizard.field_lines()) {
      const Style style = field.active ? Style::Accent : Style::Plain;
      if (field.selection) {
        body.push_back(StyledRow{std::string(field.active ? "> " : "  ") + field.label, style});
        for (size_t index = 0; index < field.choices.size(); ++index) {
          body.push_back(StyledRow{std::string(index == field.selected ? "(*) " : "( ) ") +
                                       field.choices[index].label,
                                   Style::Plain});
        }
      } else {
        body.push_back(StyledRow{std::string(field.active ? "> " : "  ") + field.label + ": " + field.value,
                                 style});
      }
    }
  }
  std::vector<StyledRow> lines;
  append_box(lines, box, page_title(app.wizard.page), body, width, true, min_rows);
  return lines;
}

struct TableColumn {
  std::string header;
  bool right = false;
};

void shrink_columns(std::vector<int>& widths, const std::vector<int>& floors, int& over) {
  const int count = static_cast<int>(widths.size());
  for (int index = count - 1; index >= 0 && over > 0; --index) {
    const int room = widths[static_cast<size_t>(index)] - floors[static_cast<size_t>(index)];
    const int cut = std::min(std::max(room, 0), over);
    widths[static_cast<size_t>(index)] -= cut;
    over -= cut;
  }
}

std::vector<std::string> table_rows(const std::vector<TableColumn>& columns,
                                    const std::vector<std::vector<std::string>>& rows, int width) {
  const int count = static_cast<int>(columns.size());
  if (count <= 0 || width <= 0) {
    return {};
  }
  std::vector<int> preferred(static_cast<size_t>(count), 1);
  std::vector<int> minimum(static_cast<size_t>(count), 1);
  for (int index = 0; index < count; ++index) {
    int header_used = 0;
    cut_columns(columns[static_cast<size_t>(index)].header, 0, 64, header_used);
    minimum[static_cast<size_t>(index)] = std::max(header_used, 1);
    preferred[static_cast<size_t>(index)] = minimum[static_cast<size_t>(index)];
    for (const auto& row : rows) {
      if (index >= static_cast<int>(row.size())) {
        continue;
      }
      int used = 0;
      cut_columns(row[static_cast<size_t>(index)], 0, 64, used);
      preferred[static_cast<size_t>(index)] = std::max(preferred[static_cast<size_t>(index)], used);
    }
  }
  const int gaps = count - 1;
  int sum = gaps;
  for (int width_now : preferred) {
    sum += width_now;
  }
  if (sum < width) {
    int extra = width - sum;
    int index = 0;
    while (extra > 0) {
      preferred[static_cast<size_t>(index % count)] += 1;
      ++index;
      --extra;
    }
  } else if (sum > width) {
    int over = sum - width;
    shrink_columns(preferred, minimum, over);
    if (over > 0) {
      shrink_columns(preferred, std::vector<int>(preferred.size(), 1), over);
    }
  }
  auto emit = [&](const std::vector<std::string>& cells) {
    std::string line;
    for (int index = 0; index < count; ++index) {
      if (index > 0) {
        line += ' ';
      }
      const std::string cell = index < static_cast<int>(cells.size()) ? cells[static_cast<size_t>(index)] : "";
      line += align_cell(cell, preferred[static_cast<size_t>(index)], columns[static_cast<size_t>(index)].right);
    }
    return pad(line, width);
  };
  std::vector<std::string> headers;
  headers.reserve(static_cast<size_t>(count));
  for (const TableColumn& column : columns) {
    headers.push_back(column.header);
  }
  std::vector<std::string> out;
  out.push_back(emit(headers));
  for (const auto& row : rows) {
    out.push_back(emit(row));
  }
  return out;
}

std::string more_devices_text(size_t count) {
  return std::to_string(count) + " more devices.";
}

void append_table_rows(std::vector<StyledRow>& content, const std::vector<std::string>& table,
                       int row_limit) {
  if (table.empty() || row_limit <= 0) {
    return;
  }
  content.push_back(StyledRow{table.front(), Style::Title});
  const int last = std::min(row_limit, static_cast<int>(table.size()));
  for (int index = 1; index < last; ++index) {
    content.push_back(StyledRow{table[static_cast<size_t>(index)], Style::Plain});
  }
}

std::string devices_title(const App& app) {
  const std::string count = std::to_string(app.monitor.clients().size());
  const std::string down = format_rate(app.monitor.total_tx_rate());
  const std::string up = format_rate(app.monitor.total_rx_rate());
  const char* down_word = utf8_locale() ? "↓" : "Down";
  const char* up_word = utf8_locale() ? "↑" : "Up";
  return "Devices (" + count + ")  " + down_word + " " + down + "  " + up_word + " " + up;
}

std::vector<std::string> device_table(const std::vector<ClientTraffic>& clients, int inner) {
  std::vector<TableColumn> columns = {
      {"Device", false},
      {"Down", true},
      {"Up", true},
  };
  const bool show_address = inner >= 42;
  const bool show_totals = inner >= 60;
  if (show_address) {
    columns.insert(columns.begin() + 1, TableColumn{"Address", false});
  }
  if (show_totals) {
    columns.push_back(TableColumn{"Total down", true});
    columns.push_back(TableColumn{"Total up", true});
  }
  std::vector<std::vector<std::string>> rows;
  rows.reserve(clients.size());
  for (const ClientTraffic& client : clients) {
    std::vector<std::string> cells;
    cells.push_back(client.mac);
    if (show_address) {
      cells.push_back(client.ip ? *client.ip : "-");
    }
    cells.push_back(format_rate(client.tx_rate));
    cells.push_back(format_rate(client.rx_rate));
    if (show_totals) {
      cells.push_back(format_bytes(client.tx_bytes));
      cells.push_back(format_bytes(client.rx_bytes));
    }
    rows.push_back(std::move(cells));
  }
  return table_rows(columns, rows, inner);
}

std::vector<StyledRow> devices_panel(const App& app, int width, const BoxChars& box, int min_rows) {
  const auto clients = app.monitor.clients();
  std::vector<StyledRow> content;
  const int slots = min_rows >= 2 ? min_rows - 2 : 100000;
  if (!app.running) {
    content.push_back(StyledRow{"The hotspot is stopped. No devices to show.", Style::Dim});
  } else if (clients.empty()) {
    content.push_back(StyledRow{"No devices are connected.", Style::Dim});
  } else {
    const auto table = device_table(clients, std::max(width - 2, 0));
    if (static_cast<int>(table.size()) <= slots) {
      append_table_rows(content, table, static_cast<int>(table.size()));
    } else if (slots <= 1) {
      content.push_back(StyledRow{more_devices_text(clients.size()), Style::Dim});
    } else {
      const int shown = std::max(slots - 2, 0);
      append_table_rows(content, table, shown + 1);
      const int hidden = static_cast<int>(clients.size()) - shown;
      content.push_back(StyledRow{more_devices_text(static_cast<size_t>(hidden)), Style::Dim});
    }
  }
  std::vector<StyledRow> lines;
  append_box(lines, box, devices_title(app), content, width, false, min_rows);
  return lines;
}

std::vector<StyledRow> fakemii_popup(const App& app, int width, const BoxChars& box) {
  const std::string ssid = app.active ? app.active->ssid : app.status.ssid;
  const std::string port = std::to_string(app.fakemii.port());
  const std::string& target = app.fakemii.last_target();
  std::vector<StyledRow> rows;
  rows.push_back(StyledRow{"FakeMii is on. SSID: " + ssid, Style::Good});
  rows.push_back(StyledRow{"Proxy: " + app.fakemii.endpoint(), Style::Accent});
  rows.push_back(StyledRow{"Requests served: " + std::to_string(app.fakemii.served()), Style::Plain});
  rows.push_back(StyledRow{"Last request: " + (target.empty() ? std::string("none yet") : printable_text(target)),
                           Style::Plain});
  if (app.fakemii.conntest_served()) {
    rows.push_back(StyledRow{"conntest served", Style::Good});
  }
  rows.emplace_back();
  rows.push_back(StyledRow{"On the 3DS:", Style::Title});
  rows.push_back(StyledRow{
      "1. Internet Settings > Connection Settings > pick this connection > Change Settings.",
      Style::Plain});
  rows.push_back(StyledRow{"2. Proxy Settings: Yes > Detailed Setup.", Style::Plain});
  rows.push_back(
      StyledRow{"3. Proxy server " + app.fakemii.address() + ", Port " + port + " > save.", Style::Plain});
  rows.push_back(StyledRow{"4. Test Connection.", Style::Plain});
  rows.emplace_back();
  rows.push_back(StyledRow{"FakeMii mainly helps when the hotspot has no upstream (upstream None). With an "
                           "upstream connection, the console reaches the real test server anyway.",
                           Style::Dim});
  if (app.backend != BackendKind::DirectHostapd) {
    // The firewall note names the fixed port. The firewall rule uses the same constant.
    rows.push_back(StyledRow{"hotmon opens port " + std::to_string(FAKEMII_PORT) +
                                 " in ufw. It manages no other firewall for this backend. Another host "
                                 "firewall may block the port.",
                             Style::Dim});
  }
  std::vector<StyledRow> lines;
  append_box(lines, box, "FakeMii (3DS)", rows, width, true, 0);
  return lines;
}

std::vector<StyledRow> status_lines(const App& app, int width, const BoxChars& box, int body_rows,
                                    bool stretch) {
  std::vector<StyledRow> body;
  if (app.status.kind == HotspotStatus::Kind::Running) {
    body.push_back(StyledRow{"State: Running", Style::Good});
    body.push_back(StyledRow{"The hotspot " + app.status.ssid + " is active on " + app.status.interface +
                                 ". Backend: " + app.status.backend + ".",
                             Style::Plain});
  } else if (app.status.kind == HotspotStatus::Kind::Failed) {
    body.push_back(StyledRow{"State: Failed", Style::Warn});
    body.push_back(StyledRow{app.status.message, Style::Warn});
  } else {
    body.push_back(StyledRow{"State: Stopped", Style::Dim});
    body.emplace_back(StyledRow{"The hotspot is stopped.", Style::Plain});
  }
  if (app.active) {
    body.push_back(StyledRow{"SSID: " + app.active->ssid, Style::Plain});
    body.push_back(StyledRow{"Interface: " + app.active->ap_interface, Style::Plain});
  } else if (app.loaded_profile) {
    body.emplace_back(StyledRow{"A saved profile is loaded. It is not applied yet.", Style::Dim});
  }
  if (app.capture.is_running()) {
    body.emplace_back(StyledRow{"Capture is active.", Style::Good});
    for (const std::string& line : app.capture.recent_lines(5)) {
      body.push_back(StyledRow{line, Style::Plain});
    }
  }
  std::vector<StyledRow> lines;
  append_box(lines, box, "Status", body, width, true, 0);
  std::vector<StyledRow> popup;
  if (app.fakemii.running()) {
    if (stretch) {
      popup = fakemii_popup(app, width, box);
    }
    if (static_cast<int>(lines.size() + popup.size()) > body_rows || !stretch) {
      // Show the hint as the first row of the status box, so a cut keeps it.
      popup.clear();
      body.insert(body.begin(),
                  StyledRow{"FakeMii is on: " + app.fakemii.endpoint() +
                                ". Make the terminal taller to see the steps.",
                            Style::Good});
      lines.clear();
      append_box(lines, box, "Status", body, width, true, 0);
    }
  }
  if (!stretch) {
    const auto devices = devices_panel(app, width, box, 0);
    lines.insert(lines.end(), devices.begin(), devices.end());
    return lines;
  }
  const int device_rows = body_rows - static_cast<int>(lines.size() + popup.size());
  if (device_rows < 3) {
    // The devices panel has no space. Stretch the status box so the keys box stays at the bottom.
    lines.clear();
    append_box(lines, box, "Status", body, width, true, body_rows - static_cast<int>(popup.size()));
    lines.insert(lines.end(), popup.begin(), popup.end());
    return lines;
  }
  lines.insert(lines.end(), popup.begin(), popup.end());
  const auto devices = devices_panel(app, width, box, device_rows);
  lines.insert(lines.end(), devices.begin(), devices.end());
  return lines;
}

std::vector<StyledRow> monitor_lines(const App& app, int width, const BoxChars& box, int body_rows,
                                     bool stretch) {
  std::vector<StyledRow> total;
  append_box(total, box, "Total traffic",
             {StyledRow{sparkline_text(app.monitor.total_samples(), std::max(width - 2, 1)), Style::Plain}},
             width, false, 0);
  const Style capture_style = app.capture.is_running() ? Style::Good : Style::Dim;
  std::vector<StyledRow> packet_rows_text = {
      StyledRow{app.capture.is_running() ? "Capture is active." : "Capture is stopped.", capture_style}};
  const auto recent = app.capture.recent_lines(4);
  if (recent.empty()) {
    packet_rows_text.emplace_back(StyledRow{"No packets.", Style::Dim});
  } else {
    for (const std::string& line : recent) {
      packet_rows_text.push_back(StyledRow{line, Style::Plain});
    }
  }
  const auto clients = app.monitor.clients();
  size_t shown = std::min<size_t>(clients.size(), 4);
  if (stretch) {
    // Show only the device boxes that fit, so the keys box stays on screen.
    const int room = body_rows - static_cast<int>(total.size() + packet_rows_text.size() + 2);
    while (shown > 0 && static_cast<int>(shown) * 3 + (clients.size() > shown ? 1 : 0) > room) {
      --shown;
    }
  }
  std::vector<StyledRow> devices;
  for (size_t index = 0; index < shown; ++index) {
    const auto& client = clients[index];
    const std::string title = client.mac + " down " + format_bytes(client.tx_bytes) + " up " +
                              format_bytes(client.rx_bytes);
    append_box(devices, box, title,
               {StyledRow{sparkline_text(client.graph.samples(), std::max(width - 2, 1)), Style::Plain}},
               width, false, 0);
  }
  if (clients.empty()) {
    append_box(devices, box, "Devices", {StyledRow{"No devices are connected.", Style::Dim}}, width,
               false, 0);
  } else if (clients.size() > shown) {
    devices.push_back(StyledRow{pad(more_devices_text(clients.size() - shown), width), Style::Dim});
  }
  const int fixed = static_cast<int>(total.size() + devices.size());
  int packet_rows = 0;
  if (stretch) {
    packet_rows = std::max(body_rows - fixed, 0);
  }
  std::vector<StyledRow> packets;
  append_box(packets, box, "Packets", packet_rows_text, width, false, packet_rows);
  std::vector<StyledRow> lines = total;
  lines.insert(lines.end(), packets.begin(), packets.end());
  lines.insert(lines.end(), devices.begin(), devices.end());
  return lines;
}

std::vector<StyledRow> footer_box(const App& app, int width, const BoxChars& box) {
  std::vector<StyledRow> lines;
  append_box(lines, box, "Keys", {StyledRow{footer_text(app), Style::Dim}}, width, true, 0);
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

std::vector<StyledRow> render_styled(const App& app, int width, int height) {
  width = std::max(width, 2);
  height = std::max(height, 1);
  const BoxChars box = box_chars();
  const bool stretch = height >= 12;
  const std::vector<StyledRow> header = header_lines(app, width, box);
  const std::vector<StyledRow> footer = footer_box(app, width, box);
  const int body_budget =
      std::max(height - static_cast<int>(header.size()) - static_cast<int>(footer.size()), 0);
  std::vector<StyledRow> body;
  if (app.view == View::Wizard) {
    body = wizard_lines(app, width, box, stretch ? body_budget : 0);
  } else if (app.view == View::Monitor) {
    body = monitor_lines(app, width, box, body_budget, stretch);
  } else {
    body = status_lines(app, width, box, body_budget, stretch);
  }
  if (stretch && static_cast<int>(body.size()) > body_budget) {
    // Cut the body so the keys box stays on the last rows. Close the cut box with a bottom border.
    body.resize(static_cast<size_t>(body_budget));
    if (!body.empty()) {
      body.back() = StyledRow{box_bottom(box, width), Style::Border};
    }
  }
  std::vector<StyledRow> lines;
  lines.reserve(static_cast<size_t>(std::max(height, 1)));
  lines.insert(lines.end(), header.begin(), header.end());
  lines.insert(lines.end(), body.begin(), body.end());
  lines.insert(lines.end(), footer.begin(), footer.end());
  if (static_cast<int>(lines.size()) < height) {
    const StyledRow blank{std::string(static_cast<size_t>(width), ' '), Style::Plain};
    lines.resize(static_cast<size_t>(height), blank);
  } else if (static_cast<int>(lines.size()) > height) {
    lines.resize(static_cast<size_t>(height));
  }
  for (StyledRow& row : lines) {
    row.text = pad(printable_text(row.text), width);
  }
  return lines;
}

std::vector<std::string> render(const App& app, int width, int height) {
  const auto styled = render_styled(app, width, height);
  std::vector<std::string> lines;
  lines.reserve(styled.size());
  for (const StyledRow& row : styled) {
    lines.push_back(row.text);
  }
  return lines;
}

}
