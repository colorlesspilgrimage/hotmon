# VERIFY

Feature: ask for authorization before privileged actions.
Branch: feat/prompt-the-user-for-their-root-password--1005-0157.

## Test results

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`: 0 lines with "warning".
- `ctest --test-dir build --output-on-failure`: 193 of 193 passed. 0 failed.
- `id -u` printed `1000`. The user is not root.

## Criteria checklist

- [x] Build has no warning. Evidence: build output has 0 warnings.
- [x] ctest passes. Evidence: 193 passed, 0 failed.
- [x] Normal user starts hotmon with no sudo. Evidence: the wizard opened in a pty as uid 1000.
- [~] Enter on the review page shows an authorization prompt. Evidence: the code calls `pkexec` through
  `HelperPrivilege`. I used a fake `pkexec`. I did not see a real prompt.
- [ ] Correct authorization starts the hotspot. NOT RUN. It needs a real password and changes the Wi-Fi link.
  Unit tests cover the fake-helper success frame (test 19).
- [x] Cancel: notice shows and the wizard stays open. Evidence: the transcript below.
  The profile was not saved (no file in the test HOME).
- [ ] Wrong password path (real pkexec). NOT RUN. The code maps exit 127 to "Authorization failed." The unit tests cover it.
- [x] Stop with cancel keeps the hotspot running. Evidence: test `App.CancelledStopKeepsTheHotspotRunning`.
- [x] Capture cancel does not start capture. Evidence: test `App.CancelledCaptureDoesNotStart`.
  The live capture path through `SCM_RIGHTS` and a real `pkexec` was NOT run.
- [~] Ncurses screen returns after the prompt. Evidence: the screen showed the wizard after the fake cancel.
  A real text prompt was not tested.
- [x] Passphrase not in argv. The passphrase travels only in the socket request. Unit test 20 covers it.
  `ps` check with a real hotspot was NOT run.
- [ ] `sudo build/hotmon` does not ask. NOT RUN. `make_privileged(0, ...)` returns `DirectPrivilege` (unit test 21).
- [x] Missing pkexec shows the install notice. Evidence: the transcript below.
- [x] `build/hotmon --bogus` prints usage and exits 2. Evidence: below.
- [x] `--privileged-helper` as non-root prints the message and exits 1. Evidence: below.
- [x] README.md and ROADMAP.md describe the new behavior. Evidence: diff shows the Run section, the Authorization
  section, and ROADMAP item 3.

## Plan step check

- Step 1: `profile_to_json` and `profile_from_json` are in `profile.hpp`. Done.
- Step 2: `teardown_hotspot` is in `backend_exec`. Backend tests exist. Done.
- Step 3: `privilege.hpp` and `privilege.cpp` exist. 22 tests in `tests/privilege_test.cpp`. Done.
- Step 4: `open_fd` and `from_fd` exist. `LocalCapture::open` is removed. Done.
- Step 5: `App::apply_hotspot(Privileged&)` and `stop_hotspot(Privileged&)`. Done.
  A cancelled stop leaves `running` true. `capture.stop()` runs only after a successful stop.
  The plan said `capture.stop()` stays in App. This order is a reasonable choice.
- Step 6: `run_ui`, `terminal_hooks`, and `main` are wired. "Waiting for authorization..." is shown. Done.
- Step 7: docs updated. Done.
- Optional `tests/privilege_system_test.cpp`: skipped. The plan allows that.

## Run transcript

```
$ build/hotmon --bogus; echo exit=$?
Usage: hotmon
exit=2
$ build/hotmon --privileged-helper </dev/null; echo exit=$?
The helper must run as root.
exit=1
```

Cancel path. I ran hotmon in a pty with a fake `pkexec` script (`echo ...; exit 126`) first in PATH.
I sent the wizard keys: Enter, `HotmonTest`, Enter, Enter, `correct-horse-battery`, Enter x5 to reach review, Enter to apply.
Last screen text:

```
hotmon  Wizard  NetworkManager
Review the settings. Enter applies the profile.
SSID: HotmonTest
...
Authorization was cancelled. The action was not done.
```

The wizard stayed open. A second Enter gave the same notice. The profile file was not created.

Missing pkexec. I ran hotmon in a pty with a PATH that holds symlinks to iw, ip, nmcli, systemctl, nft, hostapd,
and dnsmasq but not pkexec. The same keys gave:

```
hotmon needs pkexec to ask for authorization. Install polkit, or run hotmon as root.
```

## Defects

- No defect found in the code I checked.
- Coverage gap: I did not run the real `pkexec` prompt, the real hotspot start or stop, the real capture,
  or `sudo build/hotmon`. These steps need a password and change the Wi-Fi state of this machine.
  They have unit-test coverage with fake helpers only. The `SCM_RIGHTS` assumption through real `pkexec`
  (PLAN risk section) is unverified.

VERDICT: PASS
