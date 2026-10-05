# PLAN: Devices panel and UI polish for hotmon

## 1. Goal and non-goals

### Feature prompt (exact)

> Perform two changes. One: on the status page, add a panel that shows connected devices and their bandwidth usage, up and down. Two: beautify the UI. Fill the full terminal when it opens, use solid lines instead of dashes, and increase the visual appeal. Use external libraries if needed.

### Goal

- Change one: the status view shows a "Devices" panel. It lists each connected device. For each device it shows the current upload speed, the current download speed, and the total bytes in each direction.
- Change two: the UI fills the whole terminal. Box borders are solid lines, not `-` and `|`. The UI has colors and clear visual structure.

### Non-goals

- No change to the wizard logic, the backends, packet capture, or the profile file.
- No new keys. The existing keys keep their meaning.
- No new runtime dependency unless a step below says so. The plan uses ncurses only (see section 7, risk 1).
- No mouse support. No new views. The Monitor view stays. Only its border style and layout change.
- No device host names. hotmon cannot read them with the current commands.

## 2. Repo context

- Stack: C++23, CMake 3.20 or newer, ncurses (wide build), yyjson, GoogleTest, pkgconf. Warnings are errors (`-Wall -Wextra -Wpedantic -Werror`).
- Repo: `/home/samh/Work/hotmon`. Work on branch `feat/perform-two-changes--one--on-the-status--1005-0123`. Do not commit PLAN.md. The supervisor commits.
- Entry point: `src/main.cpp` calls `App::boot()` then `run_ui()` in `src/terminal.cpp`.
- Install and build (all `src/*.cpp` except `main.cpp` are globbed into the `hotmon_core` library; all `tests/*.cpp` are globbed into `hotmon_tests`. New files need no CMake edit, but re-run the configure step):
  ```
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
  ```
- Test command: `ctest --test-dir build --output-on-failure`
- Run command: `sudo build/hotmon`. The wizard also works without root.
- Conventions:
  - Namespace `hotmon`. Headers use `#pragma once`. Errors use `Result<T>` from `src/result.hpp`.
  - Comments and docs are in ASD-STE100 Simplified Technical English: short active sentences.
  - Tests use GoogleTest, one `TEST(Suite, Name)` per case, in `tests/<module>_test.cpp`. Shared helpers are in `tests/test_support.hpp`.
- Key files today:
  - `src/render.cpp` and `src/render.hpp`: `render(app, width, height)` returns `std::vector<std::string>`, one string per screen row. It draws boxes with `+`, `-`, `|` (`box_top`, `box_bottom`, `box_row`, `append_box`). It builds `header_lines`, `status_lines`, `monitor_lines`, `wizard_lines`, and the footer. It pads or cuts the result to `height`. It uses `pad`, `cut_columns`, `next_char`, `wrap_text` to handle UTF-8 column widths. `sparkline_text` makes graphs.
  - `src/terminal.cpp`: `Terminal::draw` calls `render(app, COLS, LINES)` and prints each row. Only row 2 gets color pair 1 (yellow). `Terminal` ctor sets up ncurses (`start_color`, `use_default_colors`). The main loop in `run_ui` redraws every key and every 200 ms (`timeout(200)`), and calls `app.tick(runner)` on timeout.
  - `src/monitor.hpp` and `src/monitor.cpp`: `ClientSnapshot` (mac, ip, rx_bytes, tx_bytes), `Series`, `ClientTraffic`, `MonitorState::update/clients/total_samples/clear`, `parse_station_dump`, `parse_neigh`, `clients_from_text`.
  - `src/app.cpp`: `App::refresh_clients` runs `iw dev <iface> station dump` and `ip neigh show dev <iface>`, then `monitor.update(...)`. `App::tick` calls it when the hotspot runs.
  - `src/text.hpp`: small string helpers.
- Data meaning: in `iw station dump`, `rx bytes` is data received from the device. That is the device **upload**. `tx bytes` is data sent to the device. That is the device **download**.
- Current layout of every view: header box (4 rows), body box(es), footer "Keys" box. The rows below the footer are blank. So the UI does not fill the terminal.
- Existing render tests (`tests/render_test.cpp`) check the text with `find`, and one test checks exact row text with `|` borders (`MonitorGraphShowsEverySampleInUtf8`, `WrapKeepsMultibyteCharactersWhole`). These two tests need updates.

## 3. Implementation steps

Do the steps in this order. Build after steps 2, 5, and 8.

### Step 1. Number formatting helpers

- New files `src/format.hpp` and `src/format.cpp` (namespace `hotmon`).
- `std::string format_bytes(uint64_t bytes);` Binary units, one decimal for KiB and above. Examples: `0` -> `"0 B"`, `1023` -> `"1023 B"`, `1536` -> `"1.5 KiB"`, `5242880` -> `"5.0 MiB"`. Units: B, KiB, MiB, GiB, TiB. The largest unit is TiB. No overflow for `UINT64_MAX`.
- `std::string format_rate(uint64_t bytes_per_second);` Same as `format_bytes` plus `"/s"`. Example: `2048` -> `"2.0 KiB/s"`.
- Use integer math only (no locale-dependent float printing). Show one decimal by using tenths computed with integers.

### Step 2. Bandwidth rates in the monitor state

Edit `src/monitor.hpp` and `src/monitor.cpp`.

- Add to `ClientTraffic`: `uint64_t rx_rate = 0;` (bytes per second from the device, upload) and `uint64_t tx_rate = 0;` (bytes per second to the device, download). Add private rate bookkeeping: the reference counters and the reference time point.
- Change `MonitorState::update` to `void update(const std::vector<ClientSnapshot>& clients, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());`. The default argument keeps existing callers and tests valid. Tests pass explicit times.
- Rate rule, per client:
  - First time a device appears: store the counters and the time. Rates are 0.
  - Each later call: compute `elapsed = now - reference_time`. If `elapsed` is less than 1 second, keep the old rates and keep the reference. This smooths the 200 ms polling.
  - If `elapsed` is 1 second or more: `rate = (current - reference) * 1000 / elapsed_ms` (use 64-bit math without overflow; guard multiplication, for example use `unsigned __int128` or divide first when the delta is large). If the current counter is smaller than the reference (counter reset), the rate is 0. Then move the reference to the current counters and time.
  - A device that leaves is erased (existing behavior). A device that returns starts again with rates 0.
- Add `uint64_t MonitorState::total_rx_rate() const` and `total_tx_rate() const`: saturating sums over all clients. The panel title uses them.
- `clear()` resets everything (existing behavior; keep).
- Do not change `Series`. The existing graph keeps working. `refresh_clients` in `src/app.cpp` needs no change because of the default argument.

### Step 3. Status-view devices panel (content)

Edit `src/render.cpp` (`status_lines`).

- After the existing "Status" box, add a "Devices" box. It shows one table row per device from `app.monitor.clients()`.
- Title: `Devices (N)  ↓ <total down rate>  ↑ <total up rate>` in UTF-8 locales. In non-UTF-8 locales write `Down` and `Up` instead of the arrows. Use `format_rate`.
- Header row: `Device`, `Address`, `Down`, `Up`, `Total down`, `Total up`. Column meaning:
  - `Device`: the MAC address.
  - `Address`: the IP address from `ClientTraffic::ip`, or `-` when unknown.
  - `Down`: `format_rate(tx_rate)`. `Up`: `format_rate(rx_rate)`.
  - `Total down`: `format_bytes(tx_bytes)`. `Total up`: `format_bytes(rx_bytes)`.
- Build the table with a helper `table_rows(columns, width)` in the anonymous namespace. Column widths come from the inner width. Right-align the four number columns. Pad and cut with the existing `pad` (it handles UTF-8 widths). On narrow widths (inner width below 60) drop the two `Total` columns, then the `Address` column. The `Down` and `Up` columns always stay.
- Empty states:
  - Hotspot running and no device: `No devices are connected.`
  - Hotspot not running (`app.running` false): `The hotspot is stopped. No devices to show.`
- If the devices do not fit in the rows that remain on screen, show as many as fit and end with `N more devices.` (see step 5 for the row budget).
- The panel shows only the MAC, IP and byte counts. It never shows the passphrase.
- Keep the Monitor view. Its per-device box titles (`mac rx ... tx ...`) change to use `format_bytes` and the words `down` and `up`. Use the same direction meaning as above.

### Step 4. Solid lines and a style model

Edit `src/render.hpp` and `src/render.cpp`.

- Add a style tag per row. In `render.hpp` add:
  ```
  enum class Style { Plain, Border, Title, Accent, Good, Warn, Dim };
  struct StyledRow { std::string text; Style style = Style::Plain; };
  std::vector<StyledRow> render_styled(const App& app, int width, int height);
  ```
  Keep `std::vector<std::string> render(...)` as a wrapper that returns only `text`. Existing tests use it.
- Border characters:
  - In a UTF-8 locale (the existing `utf8_locale()` check): use `┌ ─ ┐ │ └ ┘` for plain boxes. Rounded corners `╭ ╮ ╰ ╯` are allowed and are the recommended choice for a modern look. Pick one set and use it everywhere.
  - In a non-UTF-8 locale: fall back to `+`, `-`, `|` (the terminal may be a bare console). This is the only place dashes remain.
  - Put the border characters in one struct, for example `BoxChars`, chosen once per call. Change `box_top`, `box_bottom`, `box_row`, `append_box`. The title text in the top border stays (for example `╭─ Status ───────╮`).
- Column math: box-drawing characters have width 1. `cut_columns` and `pad` already use `wcwidth`. Do not use `std::string::size()` for widths. Fix `box_top`: it currently cuts the title by bytes (`label.size()`). Replace it with a column-based cut so a UTF-8 title (for example the arrows) cannot break the border.
- Row styles: Border for box edges, Title for the header row text, Accent for the page title, Good for "active" lines, Warn for warnings and notices, Dim for the footer keys. Choose a sensible tag per box. A row has one style only. Do not try to color inside a row.
- Per-row coloring replaces the hard-coded `row == 2` yellow in `Terminal::draw`. Make sure the old yellow behavior has an equal replacement: the notice line stays visibly highlighted (Warn).

### Step 5. Fill the full terminal

Edit `src/render.cpp` (`render_styled`, `append_box`, view builders).

- Row budget: `render_styled(app, width, height)` knows `height`. The header (3 rows: top, one text line, bottom; the notice goes in the same box and may wrap) and the footer box have a known height. The body gets `height - header - footer` rows.
- Add a `min_rows` argument to `append_box` (or a helper `stretch_box`). The main body box (Status view: the Devices box; Wizard view: the page box; Monitor view: the Packets box) gets blank inner rows so the box reaches the bottom of the body area. The footer box then sits on the last rows of the terminal. No blank rows remain below the footer.
- Do the same for width: every box already uses the full `width`. Keep that. Add a one-column margin only if the result looks better; do not leave unused columns on the right.
- Keep the existing guards: `width` below 2 becomes 2, `height` below 1 becomes 1, and the result is cut to `height` rows. For very small terminals (height below 12) drop the stretch and keep today's compact layout so key content stays visible.
- Terminal setup in `src/terminal.cpp`: ncurses already starts in the full terminal (`initscr` uses all of `LINES` x `COLS`). Make three changes so that it also looks full:
  1. Keep `Terminal::draw` using the current `COLS` and `LINES` on every call. Handle `KEY_RESIZE` by calling `endwin(); refresh();` is NOT needed with ncurses; just redraw. Confirm that `run_ui` redraws after `KEY_RESIZE` (it does, by `continue`). Add `clearok(stdscr, TRUE)` after a resize to clear leftover characters.
  2. Draw the full-width rows with a background so empty cells are not left unpainted: call `bkgd` with the default color pair, and print every row padded to `COLS` (render already pads to full width).
  3. Use the alternate screen (ncurses does this by default via `initscr` when the terminal supports `smcup`). Do not turn it off.

### Step 6. Colors in the terminal

Edit `src/terminal.cpp` (`Terminal::Terminal`, `Terminal::draw`).

- Call `render_styled`. For each row, set the ncurses attribute from `Style` with a small function `attr_for(Style)`.
- Color pairs (use `-1` for the default background so the user theme stays). Suggested pairs: Border = cyan, Title = bold white on default, Accent = bold cyan, Good = green, Warn = yellow (the old pair 1), Dim = dim/`A_DIM` default. Add `A_BOLD` for Title and Accent.
- If `has_colors()` is false, use attributes only (`A_BOLD`, `A_DIM`, `A_REVERSE` for Title). Do not fail.
- Use `attrset` (not many `attron`/`attroff` pairs) so a style never leaks to the next row.
- Print with `mvaddstr` (UTF-8, wide build). Keep `std::setlocale(LC_ALL, "")` before `initscr` (already there).

### Step 7. Polish the other views

Edit `src/render.cpp`.

- Wizard view: put the page progress as a small bar in the box, for example `Page 3 of 8  ●●●○○○○○` (UTF-8) or `Page 3 of 8  [###-----]` (non-UTF-8). Mark the active field line with the Accent style. Keep the exact strings that tests rely on: `(*) WPA2`, `( ) Open`, `( ) WPA3`, `Address range: ...`, `advanced setup`, `Up/Down: choose`, `Tab: next field`, `Enter: apply`, `Passphrase: set`, `*************` (masked passphrase), `radio range`.
- Header: keep `hotmon  <View>  <backend label>` text. Add the Good style when the hotspot runs, Warn when it failed, Dim when it is stopped.
- Footer: keep the exact footer strings in `footer_text`. Only style them.
- Status box: add the line `State: Running`, `State: Stopped` or `State: Failed` with Good, Dim or Warn style. Keep the existing lines `The hotspot <ssid> is active on <iface>. Backend: <backend>.`, `SSID:`, `Interface:`.

### Step 8. Docs

- Edit `README.md`: in "What it does" add a bullet: the status view lists connected devices with current and total bandwidth, up and down. Add a short "Status view" section that names the columns and says: Down is data sent to the device, Up is data received from the device. Say that the UI uses the whole terminal and Unicode lines in a UTF-8 locale, and plain `+-|` lines otherwise.
- Edit `ROADMAP.md`: add a `### 2.` entry under "Done" in the same style as entry 1.
- Write in ASD-STE100 style.

## 4. Tests to write

Run all with `ctest --test-dir build --output-on-failure`. Tests must not need root, network, or a real Wi-Fi card. Use fixed `steady_clock` time points for rate tests.

### `tests/format_test.cpp` (new)

- `Format.BytesSmall`: `0` -> `"0 B"`, `1023` -> `"1023 B"`.
- `Format.BytesUnits`: `1024` -> `"1.0 KiB"`, `1536` -> `"1.5 KiB"`, `5242880` -> `"5.0 MiB"`, `1073741824` -> `"1.0 GiB"`.
- `Format.BytesMax`: `UINT64_MAX` gives a string that ends in `" TiB"` and does not crash.
- `Format.RateAddsSuffix`: `2048` -> `"2.0 KiB/s"`, `0` -> `"0 B/s"`.

### `tests/monitor_test.cpp` (add cases)

- `Monitor.RateIsZeroOnFirstSample`: one `update` at t0 -> `rx_rate == 0`, `tx_rate == 0`.
- `Monitor.RateUsesElapsedTime`: t0 counters rx 0 tx 0; at t0+2s counters rx 2000 tx 6000 -> `rx_rate == 1000`, `tx_rate == 3000`.
- `Monitor.RateKeepsOldValueUnderOneSecond`: after a rate is set, an update 200 ms later with larger counters keeps the old rates.
- `Monitor.RateIsZeroWhenCounterResets`: counters go down between two updates 1 s apart -> both rates 0, no wrap to a huge number.
- `Monitor.RateIsZeroForIdleDevice`: same counters 1 s apart -> rates 0.
- `Monitor.ReturningDeviceStartsAtZero`: device removed in one update, added again -> rates 0.
- `Monitor.HugeDeltaDoesNotOverflow`: counters 0 and `UINT64_MAX` 1 s apart -> rate equals `UINT64_MAX` or a very large value; no crash, no wrap to a small value.
- `Monitor.TotalRatesSumTheDevices`: two devices with known rates -> `total_rx_rate()` and `total_tx_rate()` equal the sums.
- Keep all existing monitor tests unchanged. They must still pass.

### `tests/render_test.cpp` (add and update)

- Update `MonitorGraphShowsEverySampleInUtf8`: the row index and the border characters change. Find the graph row by searching for the `sparkline_text` result, and assert the row starts with the new vertical border character (`│`) and ends with it. Do not hard-code `lines[5]`.
- Update `WrapKeepsMultibyteCharactersWhole`: the test runs in the "C" locale, so it still sees `|` borders. Keep it; if the layout change breaks it, fix the expectations only. The width check (each row has 12 columns) must stay.
- `Render.StatusShowsDevicesPanel`: running app, `app.monitor.update` called twice with fixed times, so one device has `rx_rate` 1000 and `tx_rate` 3000, IP `192.168.42.20`, MAC `aa:bb:cc:dd:ee:ff`. Render at 100x30. The text contains `Devices`, the MAC, the IP, `2.9 KiB/s` (3000 B/s down), `1000 B/s` (up), and the total bytes.
- `Render.StatusDevicesEmptyStates`: running with no client -> `No devices are connected.`; stopped -> `The hotspot is stopped.` and the devices text for the stopped case.
- `Render.StatusDevicesNarrowDropsColumns`: width 50 -> the Down and Up columns remain, the `Total` columns are absent. No row is wider than 50 columns.
- `Render.StatusDevicesOverflowShowsMore`: 10 devices, height 20 -> the output has `more devices.` and has exactly 20 rows.
- `Render.FillsTheWholeTerminal`: for Status, Monitor and Wizard views at sizes 80x24, 120x40 and 200x60: `render` returns exactly `height` rows; every row has exactly `width` columns (count UTF-8 columns); the last row is a bottom border (the footer box ends on the last row); no row is blank (all spaces) except inside a box.
- `Render.UsesSolidLinesInUtf8`: with `setlocale(LC_ALL, "C.UTF-8")`, the screen contains `─` and `│` and does not contain `+---` or `|` as a border character. Restore the locale to `C` at the end of the test.
- `Render.FallsBackToAsciiInPlainLocale`: locale `C` -> the screen contains `+` and `-` borders and no byte above 0x7F in the border rows.
- `Render.TinyTerminalStillRenders`: sizes 1x1, 2x2, 10x5 return `height` rows and do not crash.
- `Render.StyledRowsMatchTextRows`: `render_styled` and `render` give the same text, and `render_styled` returns `height` entries.
- Keep existing assertions on `(*) WPA2`, `Passphrase: set`, and the others. They must still pass.

### `tests/terminal_test.cpp` (check)

- Read the file. If it tests drawing helpers that change, update it. Add a test for `attr_for` only if the function is exposed in `terminal.hpp`. Do not expose it only for a test; skip the test if not exposed.

### Edge cases to cover across the tests

- UTF-8 title or notice text that crosses the border column limit.
- A device with no IP.
- A device with very large counters.
- Zero devices, one device, more devices than rows.
- Terminal size changes between two `render` calls (the output always matches the new size).

## 5. Acceptance criteria

- [ ] When the hotspot runs, the status view shows a "Devices" panel with one row per connected device.
- [ ] Each device row shows the MAC, the IP (or `-`), the current down speed, the current up speed, the total down bytes, and the total up bytes.
- [ ] Down means data sent to the device. Up means data received from the device. The README says this.
- [ ] The speeds change when a device uses the network, and fall to `0 B/s` when it is idle.
- [ ] The panel title shows the device count and the total down and up speed.
- [ ] With no device, the panel says `No devices are connected.`. With the hotspot stopped, the panel says that the hotspot is stopped.
- [ ] On a narrow terminal, the panel drops the Total columns first, then the Address column. It never wraps the table or breaks a border.
- [ ] When the program opens, the UI fills the whole terminal in width and height. No blank rows remain below the Keys box.
- [ ] After the user resizes the terminal, the UI fills the new size at the next redraw. No leftover characters remain.
- [ ] In a UTF-8 terminal, all boxes use solid line characters. No `-`, `+` or `|` shows as a border.
- [ ] In a non-UTF-8 terminal, the UI still works and uses `+`, `-`, `|` borders.
- [ ] The UI uses colors: borders, titles, active state, warnings, and the key footer are visibly different. The UI is still readable with no color support.
- [ ] The wizard, the Monitor view, and the footer key text still work as before. All keys in the README table still work.
- [ ] The passphrase never shows on any screen.
- [ ] `cmake --build build -j` has no warnings and no errors.
- [ ] `ctest --test-dir build --output-on-failure` passes, including the new tests.
- [ ] `README.md` and `ROADMAP.md` describe the new behavior.

## 6. Manual check script

Run from `/home/samh/Work/hotmon`. A real device on the hotspot is not needed for steps 1 to 5. Step 6 needs root and a Wi-Fi card that supports AP mode.

1. Build and test:
   ```
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j
   ctest --test-dir build --output-on-failure
   ```
   Expected: the build ends with no warnings. `ctest` reports `100% tests passed`.
2. Full-terminal check. Open a terminal sized about 120x40. Run:
   ```
   build/hotmon
   ```
   Expected: the wizard opens. The header box is on the first rows. The "Keys" box is on the last rows of the terminal. No blank rows remain under it. Borders are solid lines (`╭─╮│╰╯` or similar). Colors show.
3. Resize check. While the program runs, make the terminal window bigger and smaller. Expected: the layout fills the new size and no old characters remain.
4. ASCII fallback check:
   ```
   LC_ALL=C build/hotmon
   ```
   Expected: the program runs. Borders use `+`, `-`, `|`. All text is readable.
5. Wizard flow check (as a normal user). Press `Enter` through the pages. Use SSID `Hotmon`, security WPA2, passphrase `correct-horse-battery`, upstream `None`. Expected: the review page shows `Passphrase: set` and never shows the passphrase. Press `Ctrl+q` to quit.
6. Devices panel check (root, AP-capable card):
   ```
   sudo build/hotmon
   ```
   Apply the hotspot from the review page (`Enter`). Expected: the status view opens. The state line says Running. The "Devices" panel says `No devices are connected.`. Connect a phone to the `Hotmon` network. Within a few seconds, a row appears with the phone MAC, its IP (for example `192.168.42.20`), and counters. Start a video or a speed test on the phone. Expected: the `Down` value rises (for example `2.4 MiB/s`) and the `Up` value shows the upload speed. Stop the traffic. Expected: both speeds fall to `0 B/s` within a few seconds. The `Total` columns keep growing only while traffic flows. Press `m` to open the Monitor view: the graphs show activity and the borders are solid. Press `s` to go back. Press `k` to stop the hotspot. Expected: the panel says that the hotspot is stopped. Press `q` to quit.
7. Fake-data check without root (optional). Run the new render tests only:
   ```
   build/hotmon_tests --gtest_filter='Render.*:Monitor.*:Format.*'
   ```
   Expected: all listed tests pass.

## 7. Risks and open assumptions

1. External libraries. The prompt allows them but does not require them. The plan uses ncurses only. It has color, bold, and UTF-8 box characters. A TUI library (for example FTXUI) would need a rewrite of `render.cpp` and `terminal.cpp` and would break the tests that read text rows. If the reviewer wants a library, treat that as a new decision. Do not add one silently.
2. "Fill the full terminal when it opens" means the layout uses all rows and columns. The program cannot make the terminal window larger. The plan assumes this meaning.
3. Direction meaning. `iw` reports `rx bytes` from the device's point of view of the access point. The plan maps rx to Up and tx to Down. Check this on a real device in manual step 6. If it is reversed in practice, swap the two columns in `status_lines` and in the README.
4. Rate accuracy. `iw station dump` counters come from the driver. Some drivers count only data frames, some include headers. The rate is a good estimate, not exact. The 1-second window smooths the 200 ms polling. The first second after a device joins shows 0.
5. Polling cost. `refresh_clients` runs `iw` and `ip` every 200 ms in the existing code. The plan does not change this. If it is a problem, a later change can poll every 1 second.
6. Locale. Box characters depend on `nl_langinfo(CODESET)` being `UTF-8` after `setlocale(LC_ALL, "")`. Tests run in the `C` locale by default and set `C.UTF-8` where needed. If `C.UTF-8` does not exist on the test host, the UTF-8 tests fail. Skip those tests with `GTEST_SKIP()` when `setlocale` returns `nullptr`.
7. Wide characters. East Asian wide characters in a notice use 2 columns. The existing `cut_columns` handles this. New code must use it and never use `std::string::size()` for width.
8. Existing exact-row tests (`MonitorGraphShowsEverySampleInUtf8`, `WrapKeepsMultibyteCharactersWhole`) depend on the layout. Step 4 and step 5 change the layout. The implementer must update these two tests and must not weaken other assertions.
9. Very small terminals (height below 12) use the compact layout, so content is not hidden by padding. Some bottom content may still be cut. This is the same as today.
10. No host names. The panel shows MAC and IP only. A name column would need DHCP lease parsing, which is out of scope.
