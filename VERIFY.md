# Verification report

Feature branch: `feat/i-want-to-rework-this-entire-project-int-1004-0055`. Commit `a1073e0`.
I checked the code against `PLAN.md`. I ran the build, the tests, and the real program.

## Test results

Command: `rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j && ctest --test-dir build --output-on-failure`

- Build: no warning and no error. Targets `hotmon_core`, `hotmon`, `hotmon_tests` built. Flags `-Wall -Wextra -Wpedantic -Werror` are in `CMakeLists.txt`.
- ctest: `100% tests passed out of 116`. Passed: 116. Failed: 0.
- Ported test counts match Rust: backend 26, app 15, monitor 4, profile 13 (Rust 12). Capture, iface, and render counts are equal or higher.

## Criteria checklist

- [x] Build with no warning. Evidence: build output above.
- [x] ctest passes, count 116 is at least 99. Evidence: ctest output.
- [x] `build/hotmon` shows page 1 and Ctrl+q quits. Evidence: tmux run, shell printed `exit=0`.
- [x] No `.rs`, `Cargo.toml`, `Cargo.lock`. Evidence: `git ls-files | grep -E '\.rs$|Cargo'` printed nothing, `exit=1`.
- [x] Security page is a selection box, WPA2 selected, typing `x` changes nothing. Evidence: transcript.
- [x] AP page lists only AP-capable interfaces. Evidence: only `wlp1s0` was listed. Not `lo` or `tailscale0`.
- [x] Upstream page lists other interfaces and None. Evidence: `tailscale0`, `None`. The AP interface is absent.
- [x] Band page offers `5 GHz` (selected) and `Increased compatibility`. Evidence: transcript.
- [x] Default gives 5 GHz channel 36. Evidence: Review `Band: 5`, `Channel: 36`.
- [x] Increased compatibility gives 2.4 GHz channel 6. Evidence: Review `Band: 2.4`, `Channel: 6`.
- [x] No-5-GHz fallback with review note. Evidence: unit tests in `wizard_test.cpp` and `netauto_test.cpp`. This machine has a 5 GHz radio, so I did not see it live.
- [x] Network page shows range and DHCP, no typing. Evidence: transcript.
- [x] No overlap with local networks. Evidence: `netauto_test.cpp` cases pass. Not seen live.
- [x] DHCP range inside network, not on gateway. Evidence: `netauto_test.cpp` case passes.
- [x] Advanced opens with `a`. Evidence: transcript.
- [x] Advanced has selection boxes and typed fields. Evidence: transcript.
- [x] Advanced channel box lists only radio channels. Evidence: 5 GHz box showed 36, 40, 44, 48, 149 to 165.
- [x] Bad input errors keep the page. Evidence: host address error, and gateway error, page stayed.
- [x] Review shows band, channel, range, DHCP, and source. Evidence: transcript.
- [x] Review hides the passphrase. Evidence: `Passphrase: set`. The text `correct-horse` was never on screen.
- [x] Profile path and modes `0700`/`0600`. Evidence: unit test passes. I did not save a profile live (needs root, step 6).
- [x] Rust profile loads. Evidence: manual step 1 shows stored values. `profile_compat_test` passes.
- [x] Apply, stop, status, monitor, capture, keys. Evidence: 15 app tests pass. Live run is not done (needs root and AP card).
- [x] Open hotspot with upstream needs Enter then `y`. Evidence: live run showed the warning and `y: apply open hotspot`.
- [x] Capture needs second confirmation. Evidence: capture tests pass.
- [x] README has Build and Wizard sections. ROADMAP has `No planned items.` and a Done list. Evidence: file content.
- [x] Cutover search for `cargo|ratatui|crossterm|serde`: only `PLAN.md` and old report files match. This is allowed.
- [x] Terminal setup in `src/terminal.cpp`: `setlocale`, `raw`, `set_escdelay(25)`, `timeout(200)`, `set_terminate`, signal handlers.

## Run transcript (tmux, 100x40, scratch `XDG_CONFIG_HOME`)

Manual step 1 (Rust-format profile, `channel` 11):

```
|hotmon  Wizard  NetworkManager
|The saved profile is loaded.
|Page 8 of 8: Review
|Passphrase: set
|Band: 2.4
|Channel: 11
|Address range: 10.9.8.0/24
|DHCP: on 10.9.8.20-10.9.8.30
|Upstream interface: none
|Settings source: saved profile
```
After Ctrl+q the shell printed `exit=0`. The `wlan0` interface does not exist here, but the Review page showed no error.

Manual step 2 (default path). Keys: Down, Up, Enter, `Cafe Guest`, Enter, `x`, Enter, `correct-horse`, Enter x4.
- Page 1 listed `(*) wlp1s0`.
- Page 3 showed `( ) Open`, `(*) WPA2`, `( ) WPA3`. After `x` it was the same.
- Page 4 showed `> Passphrase: *************`.
- Page 5 showed `( ) tailscale0`, `(*) None`.
- Page 7 showed `Address range: 192.168.42.0/24`, `DHCP: on`, `DHCP start: 192.168.42.10`, `DHCP end: 192.168.42.100`.
- Review showed `Band: 5`, `Channel: 36`, `Settings source: automatic`.
- Esc gave `The wizard is cancelled. The settings were not applied.`

Manual step 3: Down on page 6 gave `Band: 2.4`, `Channel: 6`, `Settings source: automatic`.

Manual step 4: `a` on page 7 opened `Page 7 of 8: Advanced setup`. I typed `192.168.42.5/24` and pressed Enter.
- Error: `The address range must be a network address, for example 192.168.42.0/24.` The page stayed.
- Start `10.20.30.1` gave `The DHCP range must not include the hotspot address 10.20.30.1.` The page stayed.
- Start `10.20.30.50` and end `10.20.30.90` gave Review with `Address range: 10.20.30.0/24`, `DHCP: on 10.20.30.50-10.20.30.90`, `Settings source: set by the user (advanced setup)`.
- Note: the fields are prefilled. My first typing appended to the prefill. The error text was correct for that input.

Manual step 5 (open security, upstream `tailscale0`, Enter on Review):
```
|Every device in radio range can use the upstream network. Press y to apply this open hotspot.
|y: apply open hotspot  Enter does not apply  Left: previous page  Esc: cancel  Ctrl+q: quit
```
Pressing Enter on Review of an open hotspot with no upstream tried to apply and failed with `The program cannot prepare /run/hotmon. Permission denied`. This is correct without root.

Manual step 9: `git ls-files | grep -E '\.rs$|Cargo'; echo "exit=$?"` printed `exit=1`.

Not run: manual steps 6, 7, and 8. They need root and an AP-capable card. I did not save a profile, so I did not check the live directory mode.

## Defects found

None that block the plan. Minor notes:
- Footer text on page 7 wraps at 100 columns. This is cosmetic.
- No live check of the no-5-GHz fallback or of root-only paths. Unit tests cover them.

VERDICT: PASS
