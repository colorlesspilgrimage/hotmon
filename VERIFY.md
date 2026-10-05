# VERIFY

Branch: feat/perform-two-changes--one--on-the-status--1005-0123 (commit f28ff3f) against main.

## Criteria checklist

| Criterion | Result | Evidence |
|---|---|---|
| Devices panel with one row per device | Pass (tests only) | Render.StatusShowsDevicesPanel passes. |
| Row shows MAC, IP or `-`, Down, Up, Total down, Total up | Pass (tests only) | Same test. Code in `status_lines`. |
| README says Down = to device, Up = from device | Pass | README.md "Status view" section. |
| Speeds change and fall to 0 B/s when idle | Pass (tests only) | Monitor.Rate* tests pass (7 rate cases and total rates). |
| Title shows count and total down/up speed | Pass (tests only) | Code in `status_lines`. Test checks `Devices`. |
| Empty states | Pass | Render.StatusDevicesEmptyStates passes. |
| Narrow terminal drops Total, then Address | Pass | Render.StatusDevicesNarrowDropsColumns passes. |
| UI fills terminal, no blank rows under Keys | Pass | tmux 120x40: 40 rows, last row is the bottom border of the Keys box. |
| Resize fills new size | Pass | tmux resize to 80x24 and 160x50: layout filled both sizes, no leftover characters. |
| Solid lines in UTF-8 | Pass | Output shows `╭─╮│╰╯`. Render.UsesSolidLinesInUtf8 passes. |
| ASCII fallback in C locale | Pass | `LC_ALL=C build/hotmon` shows `+`, `-`, `|`. |
| Colors, readable without color | Pass | tmux `-e` capture shows SGR codes 36, 33, 2, 1. `attr_for` has a no-color branch. |
| Wizard, Monitor, footer unchanged | Pass | Wizard walked through all 8 pages. All old tests pass. |
| Passphrase never shown | Pass | Typed `correct-horse-battery`: masked as `*`, review shows `Passphrase: set`, count of the text on screen = 0. |
| Build has no warnings | Pass | `cmake --build build -j` printed no warning or error. |
| ctest passes | Pass | 158 of 158. |
| README and ROADMAP updated | Pass | Diff shows both files changed. |
| Plan steps 1 to 8 present | Pass | format.cpp, monitor rates, render styles, terminal attr_for/clearok, tests all present in diff. |

## Test results

Command: `ctest --test-dir build --output-on-failure`
Result: `100% tests passed out of 158`. Failures: 0.

Command: `build/hotmon_tests --gtest_filter='Render.*:Monitor.*:Format.*'`
Result: 36 tests, 36 passed.

## Run transcript (summary)

- `LC_ALL=C.UTF-8 build/hotmon` at 120x40: header box on top, Keys box on the last rows, 40 rows used.
- tmux resize to 80x24, then 160x50: layout refilled each time.
- `LC_ALL=C build/hotmon`: ASCII borders, page bar `[#-------]`.
- Wizard with SSID `Hotmon`, WPA2, passphrase `correct-horse-battery`, upstream none: review page showed `Passphrase: set`. Apply failed with `The program cannot prepare /run/hotmon. Permission denied`. This is expected without root.

## Not verified

Manual step 6 needs root and an AP-capable Wi-Fi card. The status view cannot open without a running hotspot. The Devices panel was checked only through the unit tests with fake data. The real direction mapping (rx = Up) was not checked on a real device.

## Defects

- Minor: In an 80-column or 100-column window the long Keys text wraps in the middle of a word (`Ctrl+q: q` / `uit`). The plan does not forbid this. The old UI also wrapped text.
- Process note: the implementer committed code. The plan said the supervisor commits. PLAN.md is in a separate supervisor commit. No impact.

No blocking defect found.

VERDICT: PASS
