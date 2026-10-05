# PLAN: Ask for authorization before privileged actions

This plan is for the hotmon repo. Read the whole plan before you write code.

## 1. Goal and non-goals

### Feature prompt (exact)

> Prompt the user for their root password or fingerprint authorization to actually enable the hotspot and similar privileged actions.

### Goal

- The user runs `build/hotmon` as a normal user. No `sudo` is needed.
- When the user starts the hotspot, hotmon asks for authorization. The user types the root (admin) password or uses a fingerprint reader.
- The same happens when the user stops the hotspot and when the user starts a packet capture.
- Only the small privileged part runs as root. The ncurses UI never runs as root.
- If hotmon already runs as root (euid 0), it does not ask. It does the work directly, as it does today.
- If the user cancels or fails the authorization, hotmon shows a clear notice and stays in a good state. Nothing is half-applied.

### Non-goals

- No new backend. No change to backend selection, plan contents, or command lines.
- No polkit policy file or install step. The generic `pkexec` action is used.
- No `sudo` fallback. If `pkexec` is missing, hotmon shows an error.
- No long-lived root process. Each privileged action starts one helper and the helper ends when the action ends.
- No password handling inside hotmon. hotmon never reads, stores, or sees the password. The system authentication agent does that.
- No change to the saved profile file format.

## 2. Repo context

- Stack: C++23, CMake 3.20+, ncurses (wide), yyjson, GoogleTest, pkgconf. Linux only.
- Entry point: `src/main.cpp` calls `App::boot()` then `run_ui()` (in `src/terminal.cpp`).
- Install the dependencies with your package manager (Arch: `base-devel cmake ncurses yyjson gtest pkgconf polkit`).
- Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`
- Test: `ctest --test-dir build --output-on-failure` (or `build/hotmon_tests`).
- Run: `build/hotmon`. Today the README says `sudo build/hotmon`.
- CMake picks up every `src/*.cpp` (except `main.cpp`) into the static library `hotmon_core`. It picks up every `tests/*.cpp` into `hotmon_tests`. New files need no CMake edit. Re-run `cmake -S . -B build` once so the glob refreshes.
- Compiler flags: `-Wall -Wextra -Wpedantic -Werror`. Warnings fail the build.
- Conventions: namespace `hotmon`. Fallible functions return `Result<T>` (`std::expected<T, std::string>`, see `src/result.hpp`). Errors use `unexpected_text("...")`. Error text is plain English in full sentences. Two-space indent. Trailing underscore for private members. Tests use GoogleTest with `using namespace hotmon;` in an anonymous namespace. Shared fakes live in `tests/test_support.hpp` (`ScriptedRunner`, `RecordedSignals`, `test_paths`, `sample_profile`).
- Write all docs, comments, and commit messages in ASD-STE100 Simplified Technical English. Use short sentences (20 words or fewer) and active voice.

### Where privilege is needed today

The current code does these privileged steps in the UI process:

1. `src/backend_exec.cpp` `execute_plan(plan, runner, signals, paths)`: writes files to `/run/hotmon`, `/etc/hostapd/hostapd.conf`, `/var/lib/iwd/ap`. Writes `/proc/sys/.../forwarding` (`enable_forwarding`). Runs commands through `Runner` (`nmcli`, `systemctl`, `ip`, `nft`, `hostapd`, `dnsmasq`, `iw`). Roll-back also uses these.
2. `src/app.cpp` `App::stop_hotspot(...)`: runs stop commands, `stop_started` (sends SIGTERM), `run_nft_delete`, `clear_nft_live`, `restore_forwarding`, `restore_hostapd_backup`, removes pid files, `retire_iwd_profile`, removes the NM secret file.
3. `src/capture_socket.cpp` `LocalCapture::open`: opens an `AF_PACKET` raw socket. This needs `CAP_NET_RAW`.
4. `src/terminal.cpp` `open_capture(App&)` calls `LocalCapture::open`. `run_ui` calls `App::apply_hotspot` and `App::stop_hotspot` with `SystemRunner` and `SystemSignals`.

These need NO privilege and stay in the UI process: `probe_system`, `service_active`, `read_system_interfaces`, `iw phy ... info`, `refresh_clients` (`iw dev X station dump`, `ip neigh`), profile load and save, and the capture read loop (the opened socket fd is enough).

## 3. Implementation steps

Design in short: add a `Privileged` interface with three operations (apply, stop, open capture socket). It has two implementations. `DirectPrivilege` does the work in the current process. `HelperPrivilege` runs `pkexec <path to hotmon> --privileged-helper` and talks to it over a Unix socket. `main` picks `DirectPrivilege` when euid is 0, else `HelperPrivilege`. The helper re-plans from the profile, so the caller never sends commands to root.

### Step 1. Export the profile JSON functions

- File `src/profile.hpp`: declare `Result<std::string> profile_to_json(const Profile&);` and `Result<Profile> profile_from_json(std::string_view);`. They already exist in `src/profile.cpp` (namespace `hotmon`, not anonymous). Remove the two forward declarations at the top of `src/profile.cpp` (lines 19 and 20) because the header now has them.
- `profile_from_json` must keep its checks. The helper uses it on untrusted input.

### Step 2. Move the stop sequence into `backend_exec`

- File `src/backend_exec.hpp` and `src/backend_exec.cpp`: add
  `Result<void> teardown_hotspot(BackendKind backend, const Profile& profile, bool private_hostapd, const std::vector<StartedProc>& started, Runner& runner, ProcessControl& signals, const Paths& paths);`
- Move the body of `App::stop_hotspot` from `plan_stop(...)` through `std::filesystem::remove(paths.nm_secret(), error)` into this function. Keep the order and the error texts. It returns `{}` on success.
- Do not move `capture.stop()`, `started.clear()`, `private_hostapd = false`, `finish_stop()`. They stay in `App`.

### Step 3. Add the privilege layer

New files `src/privilege.hpp` and `src/privilege.cpp`.

```
struct StopRequest {            // data the stop action needs
  BackendKind backend;
  Profile profile;
  bool private_hostapd = false;
  std::vector<StartedProc> started;   // used by DirectPrivilege only
};

class Privileged {
 public:
  virtual ~Privileged() = default;
  virtual Result<StartReport> apply(BackendKind backend, const Profile& profile) = 0;
  virtual Result<void> stop(const StopRequest& request) = 0;
  virtual Result<FileDescriptor> open_capture_socket(std::string_view interface) = 0;
};
```

`DirectPrivilege : Privileged`
- Constructor takes `Runner&`, `ProcessControl&`, `Paths`.
- `apply`: `plan_apply(backend, profile, paths)` then `execute_plan(...)`. Return the same errors as `App::apply_hotspot` returned before.
- `stop`: call `teardown_hotspot(...)`.
- `open_capture_socket`: call `LocalCapture::open_fd(interface)` (Step 4).

`HelperPrivilege : Privileged`
- Constructor takes `std::vector<std::string> helper_argv` and a `TerminalHooks` struct with two `std::function<void()>` members, `suspend` and `resume`. Both may be empty.
- A static function `std::vector<std::string> default_helper_argv()` returns `{"pkexec", <realpath of /proc/self/exe>, "--privileged-helper"}`. `pkexec` needs an absolute program path. Read it with `std::filesystem::read_symlink("/proc/self/exe")`.
- Each operation does this:
  1. Build the request (see protocol below).
  2. Call `hooks.suspend()`. In the app this ends ncurses mode so the text agent can use the terminal.
  3. Create a `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)`. Fork. In the child: `dup2(sv[1], 0)` and `dup2(sv[1], 1)`. Keep fd 2 as a pipe to the parent (read the text later). Leave the controlling terminal alone so `pkexec` can prompt. Close other fds the same way `run_capture` does (`close_range(3, ~0U, CLOSE_RANGE_CLOEXEC)`). Then `execvp(helper_argv[0], ...)`. Report exec errors like `run_capture` does with a status pipe. You MAY factor shared code from `src/process.cpp` into a helper there.
  4. Parent: write the request, read the response, `waitpid`, then call `hooks.resume()`. Always call `resume`, also on every error path (use a scope guard).
  5. Map the result (see error mapping below).
- Do not put the passphrase in `argv` or in the environment. It only travels in the socket request.

Wire protocol (new, documented in a comment at the top of `privilege.hpp`):
- Frame: 4-byte big-endian length, then that many bytes of JSON. Reject a frame larger than 1 MiB.
- One request frame from parent, one response frame from helper.
- Request JSON (build with yyjson, the repo already uses it):
  - `{"op":"apply","backend":"<name>","profile":<profile object>}`
  - `{"op":"stop","backend":"<name>","private_hostapd":true|false,"profile":<profile object>}`
  - `{"op":"capture","interface":"wlan0"}`
  - `<name>` is one of `network-manager`, `iwd`, `existing-hostapd`, `direct-hostapd`. Add `std::string_view backend_token(BackendKind)` and `Result<BackendKind> parse_backend_token(std::string_view)`.
  - The profile object is the output of `profile_to_json` embedded as an object (parse it with `profile_from_json` on the helper side; you may send it as a JSON string field `"profile"` to keep this easy).
- Response JSON:
  - `{"ok":true,"started":[{"pid":123,"name":"hostapd"}],"private_hostapd":false}` for apply.
  - `{"ok":true}` for stop.
  - `{"ok":true}` plus the raw socket fd in `SCM_RIGHTS` ancillary data for capture. Send the frame with `sendmsg`. Receive with `recvmsg` and a `MSG_CMSG_CLOEXEC` flag.
  - `{"ok":false,"error":"<text>"}` on any failure.
- Provide testable functions: `std::string encode_request(...)`, `Result<Request> decode_request(std::string_view)`, `std::string encode_response(...)`, `Result<Response> decode_response(std::string_view)`, and framing functions `write_frame(int fd, std::string_view, int pass_fd = -1)` and `read_frame(int fd, int* received_fd = nullptr)`.

Helper side (same files):
- `int run_privileged_helper(Privileged& worker)` reads one request from fd 0, calls the matching `worker` function, writes one response to fd 1, and returns 0. It returns 1 if the request cannot be read or decoded (after it tries to send an error response).
- Safety rules for the helper. These MUST hold:
  - The helper never reads commands, paths, or pids from the request. It re-plans with `plan_apply(backend, profile, Paths::system())`. It always uses `Paths::system()`.
  - `apply` and `stop` first validate the profile with `Profile::check_settings()` (`plan_apply` already does this for apply; do it explicitly for stop).
  - `capture` validates the name with `valid_name()` (`src/iface.hpp`) before it opens the socket.
  - For `stop`, the helper rebuilds the started list from the pid files: use `read_pid_optional(paths.hostapd_pid())` with name `hostapd` and `read_pid_optional(paths.dnsmasq_pid())` with name `dnsmasq`. It ignores any pid from the caller. So `HelperPrivilege::stop` does not send `started`.
  - After a failed read, an unknown `op`, or an extra field of the wrong type, the helper returns an error response and does nothing.
  - The helper process in production is built from `DirectPrivilege` with `SystemRunner`, `SystemSignals`, `Paths::system()`.
- Environment note: `pkexec` already cleans the environment. Do not rely on any variable in the helper.

Error mapping in `HelperPrivilege` (the `pkexec` exit code is the child's status):
- Exec of `pkexec` fails (not installed): `"hotmon needs pkexec to ask for authorization. Install polkit, or run hotmon as root."`
- Exit 126 (user dismissed the dialog): `"Authorization was cancelled. The action was not done."`
- Exit 127 with no response frame: `"Authorization failed. Check the password or the fingerprint and try again."` Append the trimmed text from fd 2 if it is not empty.
- Helper returned `{"ok":false,"error":E}`: return `E` unchanged.
- Helper died or sent a bad frame: `"The privileged helper stopped without a result."` plus the fd 2 text.

### Step 4. Open the capture socket as a plain fd

- File `src/capture_socket.hpp` and `src/capture_socket.cpp`:
  - Add `static Result<FileDescriptor> open_fd(std::string_view interface);`. It holds the current body of `LocalCapture::open` (name check, `socket`, `if_nametoindex`, `bind`) but returns a `FileDescriptor` (from `src/process.hpp`). The socket does not need non-blocking mode, because `try_recv` uses `MSG_DONTWAIT`.
  - Add `static LocalCapture from_fd(FileDescriptor fd);`. It takes ownership.
  - Remove `LocalCapture::open(name)`. No caller needs it after this change. Move any test to `open_fd`. (Clean cutover: no unused code.)

### Step 5. Switch `App` to `Privileged`

- File `src/app.hpp` and `src/app.cpp`:
  - Change `apply_hotspot(Runner&, ProcessControl&, const Paths&)` to `Result<void> apply_hotspot(Privileged& privileged);`
  - Change `stop_hotspot(Runner&, ProcessControl&, const Paths&)` to `Result<void> stop_hotspot(Privileged& privileged);`
  - In `apply_hotspot`: keep the wizard checks and the open-upstream confirmation first. Remove the `plan_apply` and `execute_plan` calls. Call `privileged.apply(backend, *profile)` instead. Keep the same success and failure handling that follows. A failed authorization uses the same failure branch (`fail_apply` when the hotspot was not running, `wizard.set_error` plus notice when it was running). Keep the `Paths` use out of `App`.
  - In `stop_hotspot`: call `privileged.stop(StopRequest{backend, profile, private_hostapd, started})`. On error set the notice and return the error, and keep the hotspot marked running. On success do `started.clear()`, `private_hostapd = false`, `finish_stop()`.
  - A cancelled authorization on stop MUST leave `running == true` and the status view unchanged except for the notice.
  - Do not add capture logic to `App`. It stays in `src/terminal.cpp` (Step 6).
  - Remove the now unused includes from `app.hpp` only if nothing else needs them (`backend_exec.hpp` is still needed for `StartedProc`).

### Step 6. Wire the UI

- File `src/terminal.hpp`: change `int run_ui(App& app);` to `int run_ui(App& app, Privileged& privileged);`. Add a function `TerminalHooks terminal_hooks();` that returns hooks which use ncurses: `suspend` calls `def_prog_mode(); endwin();`. `resume` calls `reset_prog_mode(); clearok(stdscr, TRUE); refresh();`. If `Terminal` is not active (no curses yet) the hooks do nothing.
- File `src/terminal.cpp`:
  - `run_ui` keeps `SystemRunner runner` for `app.tick(runner)` only. Remove `SystemSignals` and `Paths` from it.
  - `case Step::Apply`: `app.apply_hotspot(privileged)`.
  - `case Step::StopHotspot`: `app.stop_hotspot(privileged)`.
  - `open_capture(App&, Privileged&)`: call `privileged.open_capture_socket(*iface)`. On error call `app.capture_open_failed(error)`. On success build `LocalCapture::from_fd(std::move(fd))` and attach as today.
  - Before each privileged call, show a one-line notice such as `"Waiting for authorization..."` and `terminal.draw(app)`. The hooks then leave curses mode, so the prompt of `pkexec` is clear.
- File `src/main.cpp`:
  - After `App::boot()`: if `argc == 2` and `argv[1] == "--privileged-helper"`, do NOT start ncurses. Build `SystemRunner`, `SystemSignals`, `DirectPrivilege worker(runner, signals, Paths::system())`, return `run_privileged_helper(worker)`. If the helper runs with euid != 0, return 1 with the message `"The helper must run as root."` on stderr. Do this check before `App::boot()` so the helper does not read the profile.
  - Any other argument: print `"Usage: hotmon"` to stderr and return 2. (Today hotmon ignores arguments. Keep it simple and strict, since a helper flag now exists.)
  - Normal mode: `if (geteuid() == 0)` build `DirectPrivilege` with `SystemRunner`, `SystemSignals`, `Paths::system()`. Else build `HelperPrivilege(default_helper_argv(), terminal_hooks())`. Pass it to `run_ui`.
  - Put the choice in a function `std::unique_ptr<Privileged> make_privileged(uid_t euid, ...)` in `src/privilege.cpp` so a test can call it with a fake euid.

### Step 7. Docs

- `README.md`: change the Run section to `build/hotmon`. Explain: hotmon asks for the admin password or a fingerprint when it starts or stops the hotspot and when it starts a capture. It needs `pkexec` (package `polkit`) and a polkit agent or a terminal. Running as root (`sudo build/hotmon`) still works and does not ask. Update the "Starting the hotspot needs root" sentence and the "Packet capture" sentence ("Capture needs root or the CAP_NET_RAW capability") to say hotmon asks for authorization. Keep the key table. Add a short "Authorization" section that lists the three actions, the cancel behavior, and the fact that hotmon never sees the password.
- `ROADMAP.md`: add a "Done" item `### 3. Ask for authorization` with two or three short sentences.
- All text in STE.

### Build order check

Steps 1, 2, 4 are independent. Step 3 needs 1, 2, 4. Step 5 needs 3. Step 6 needs 3 and 5. Step 7 last. All of it must compile together, so one agent SHOULD do steps 3, 5, 6 in one change.

## 4. Tests to write

Update existing tests first, then add new ones. All tests MUST run without root, without `pkexec`, and without a display.

### Update existing tests

- `tests/app_test.cpp` and any other test that calls `apply_hotspot(runner, signals, paths)` or `stop_hotspot(...)` (use `grep -rn "apply_hotspot\|stop_hotspot\|LocalCapture::open" tests/`): build a `DirectPrivilege direct(runner, signals, paths)` and call `app.apply_hotspot(direct)` or `app.stop_hotspot(direct)`. Expected command sequences stay identical to today. This proves that the refactor kept behavior.
- `tests/capture_test.cpp`: if a test uses `LocalCapture::open`, switch it to `open_fd` and `from_fd`. A test that opens a raw socket as non-root MAY accept either success or a permission error.
- `tests/backend_test.cpp`: add tests for `teardown_hotspot` that mirror the old stop tests. Same `ScriptedRunner` results give the same calls and the same errors.

### New file `tests/privilege_test.cpp`

Frame and codec:
1. `encode_request` then `decode_request` round-trips an apply request with `sample_profile()`. The decoded profile equals the original (`Profile::operator==`).
2. Same for stop (with `private_hostapd` true and false) and for capture with `"wlan0"`.
3. `decode_request` rejects: empty text, invalid JSON, unknown `op`, unknown backend token, missing `profile`, `interface` with a bad name such as `"../x"` or 16+ characters (the decoder or the helper must reject it; the test checks the whole path through `run_privileged_helper`).
4. `encode_response` and `decode_response` round-trip success (with a started list of two entries), failure with text, and stop success.
5. `write_frame` and `read_frame` over a `socketpair`: a normal frame; an empty frame; a length header bigger than 1 MiB returns an error; a truncated frame (peer closes early) returns an error.
6. `write_frame` with `pass_fd` and `read_frame` with `received_fd`: pass a pipe read end. The received fd reads the data written to the pipe.
7. `backend_token` and `parse_backend_token` round-trip for all four `BackendKind` values.

Helper logic with a fake worker (class `FakePrivileged : Privileged` that records calls and returns canned results):
8. `run_privileged_helper` with an apply request calls `worker.apply` once with the same backend and profile. The response has `ok:true` and the started list.
9. A stop request calls `worker.stop` once. The response is `ok:true`.
10. Worker returns an error: the response is `ok:false` with the same text.
11. A capture request with a valid name calls `worker.open_capture_socket`. The response frame carries an fd.
12. An invalid request (garbage bytes, unknown op) returns 1 and never calls the worker.
13. A profile with an invalid SSID (empty or 33 bytes) or an invalid channel is rejected before the worker is called. Expected: `ok:false`. (For apply the worker is `DirectPrivilege` with a `ScriptedRunner` that records zero calls.)
14. A stop request run through `DirectPrivilege`-based helper ignores any pid in the request. Use a temp dir from `scratch_dir()` and `test_paths()`. Write a pid file. Check that `RecordedSignals` only sees pids from files. (The production helper uses `Paths::system()`. The test uses a testable constructor or function that takes `Paths`.)

`HelperPrivilege` with a fake helper program (no pkexec). Use `helper_argv = {"/bin/sh", "-c", <script>}`:
15. Script `exit 126`: `apply` returns text containing `cancelled`. The `resume` hook was called once. The `suspend` hook was called once, before `resume`.
16. Script `exit 127`: `apply` returns text containing `Authorization failed`.
17. Script that prints a stderr line and exits 127: the error text contains that line.
18. `helper_argv = {"definitely-not-a-program"}`: the error mentions `pkexec` text only when argv[0] is `pkexec`; otherwise it contains `did not start`. (Test the `pkexec` message with `{"pkexec-does-not-exist-xyz"}` only if you make the mapping depend on a flag. Simpler: the not-installed message applies to any exec failure. Then assert the text contains `pkexec`.)
19. Script that reads the request and writes a valid success frame. Build the frame in the test with `encode_response` and `write_frame` through a small helper, or use `printf` of known bytes. `apply` returns `StartReport` with the expected started list.
20. The passphrase is never in the child's argv: assert that `helper_argv` after the call is unchanged and does not contain the sample passphrase. Assert that the encoded request does contain it (it travels in the socket).

Choice:
21. `make_privileged(0, ...)` returns a `DirectPrivilege`. `make_privileged(1000, ...)` returns a `HelperPrivilege`. (Add a virtual `kind()` or use `dynamic_cast` in the test.)

### Test for `main` argument handling

Add `tests/privilege_system_test.cpp` ONLY if it is simple. Otherwise skip it and cover it in the manual script.

### UI tests

- `tests/render_test.cpp` or `tests/app_test.cpp`: after a cancelled stop (fake `Privileged` returns the cancel text), `app.running` is still `true`, `app.status.kind` is `Running`, and `app.notice` contains `cancelled`.
- After a cancelled apply with the hotspot not running: `app.running` is `false`, `app.status.kind` is `Failed`, and the wizard error shows the cancel text. The profile file is NOT written.
- After a cancelled capture open: `capture.phase()` is not `Running` and the notice has the cancel text (use `App::capture_open_failed`).

## 5. Acceptance criteria

- [ ] `cmake --build build -j` finishes with no warning.
- [ ] `ctest --test-dir build --output-on-failure` passes.
- [ ] As a normal user, `build/hotmon` starts and the wizard works. No `sudo` is needed.
- [ ] At the review page, pressing Enter (and `y` when asked) shows an authorization prompt. The prompt takes the admin password. On a machine with a fingerprint reader and polkit set up for it, a fingerprint works.
- [ ] After correct authorization, the hotspot starts and the status view shows it as running.
- [ ] If the user cancels the prompt or enters a wrong password, the user sees a notice that says authorization was cancelled or failed. The hotspot does not start. The wizard stays open with the error. The profile is not saved.
- [ ] Pressing `k` asks for authorization and then stops the hotspot. If the user cancels, the hotspot stays running and the notice says so.
- [ ] Pressing `c` and then Enter asks for authorization and then shows live packets. If the user cancels, capture does not start.
- [ ] After the prompt, the ncurses screen is back and not damaged.
- [ ] The password never appears on screen in hotmon, in the saved profile, or in `ps` output.
- [ ] `sudo build/hotmon` still works and does not ask a second time.
- [ ] If `pkexec` is missing, the user sees a notice that tells them to install polkit or run as root.
- [ ] `build/hotmon --bogus` prints a usage line and exits with a non-zero status.
- [ ] `README.md` and `ROADMAP.md` describe the new behavior.

## 6. Manual check script

Run these steps on a Linux machine with a Wi-Fi device that supports AP mode, `polkit` installed, and a terminal. Replace `wlan0` with your device. Use a real SSID and a passphrase of 8 to 63 characters.

```
# 1. Build and test
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
# Expected: build ends with no warning. All tests pass.

# 2. Check the user is not root
id -u
# Expected: a number other than 0 (for example 1000).

# 3. Check the argument handling
build/hotmon --bogus; echo "exit=$?"
# Expected: a "Usage: hotmon" line on stderr, then exit=2.
build/hotmon --privileged-helper </dev/null; echo "exit=$?"
# Expected: "The helper must run as root." on stderr, then exit=1.

# 4. Start the program as a normal user
build/hotmon
# Expected: the wizard opens. No sudo prompt before this.
```

In the wizard use these values: interface `wlan0`, SSID `HotmonTest`, security `WPA2`, passphrase `correct-horse-battery`, upstream `None` (or your internet interface), the default band and channel, the default address and DHCP. Go to the review page.

```
# 5. Cancel path
# Press Enter on the review page.
# Expected: the screen shows "Waiting for authorization...", then the authorization prompt.
# Cancel the prompt (Ctrl+D at the text prompt, or close the dialog).
# Expected: back in hotmon, the wizard shows "Authorization was cancelled. The action was not done."
# Run in another terminal: ip link show wlan0 ; pgrep hostapd
# Expected: no hostapd process from hotmon. /run/hotmon does not hold hostapd.pid.

# 6. Wrong password path
# Press Enter on the review page again. Type a wrong password three times.
# Expected: hotmon shows "Authorization failed. ..." and stays in the wizard.

# 7. Success path
# Press Enter on the review page. Type the correct admin password (or use the fingerprint).
# Expected: the status view shows HotmonTest as running on wlan0.
# In another terminal:
pgrep -a hostapd
cat /run/hotmon/hostapd.pid
ps aux | grep -c correct-horse-battery
# Expected: hostapd is running (or the NetworkManager/iwd connection is active, based on your backend).
# The passphrase appears only inside the config file, never in a process list (the count is 1, the grep itself).
cat ~/.config/hotmon/profile.json | head -3
# Expected: the profile is saved and owned by your user (ls -l shows -rw-------).

# 8. Capture
# In hotmon press c. Read the warning. Press Enter.
# Expected: authorization prompt, then packet lines appear when a device uses the hotspot.
# Press z. Expected: capture stops.
# Press c, Enter, and cancel the prompt. Expected: notice "Authorization was cancelled...", no capture.

# 9. Stop path
# Press k. Cancel the prompt.
# Expected: the hotspot stays running. The notice says authorization was cancelled.
# Press k again. Authorize.
# Expected: "The hotspot is stopped." In another terminal: pgrep hostapd prints nothing from hotmon.

# 10. Quit
# Press q. Expected: the terminal is restored to normal.

# 11. Root path
sudo build/hotmon
# Start the hotspot through the wizard.
# Expected: no authorization prompt. The hotspot starts. Press k and q to stop and quit.

# 12. Missing pkexec
PATH=/nonexistent:/usr/local/bin build/hotmon
# Start the hotspot through the wizard.
# Expected: notice "hotmon needs pkexec to ask for authorization. Install polkit, or run hotmon as root."
```

If your PATH hides the other tools hotmon needs, the probe can pick a different backend. That does not affect step 12.

## 7. Risks and open assumptions

- Assumption: `pkexec` (polkit) is the right tool. It supports a password and a fingerprint (through `pam_fprintd` with the polkit agent). It is present on Arch and on Omarchy. `sudo` is not used.
- Assumption: one authorization per action (start, stop, capture) is acceptable. Polkit MAY cache authorization if the system policy says so. hotmon does not control this.
- Risk: the generic polkit action `org.freedesktop.policykit.exec` shows the full command path and not a friendly text. A custom `.policy` file would fix this but needs install steps. It is out of scope.
- Risk: when no graphical polkit agent runs, `pkexec` registers a text agent on the controlling terminal. This is why ncurses MUST be suspended around the call (`endwin`, then `reset_prog_mode`). Test this on a bare TTY and in a terminal emulator.
- Risk: after `endwin()` the terminal is in cooked mode. Ctrl+C then sends SIGINT to hotmon and to `pkexec`. The existing handler (`install_signal_handlers`) restores the terminal and ends hotmon. The helper exits on socket end. Accept this behavior.
- Risk: a root helper acts on requests from an unprivileged parent. Mitigations are in Step 3: the helper re-plans from validated data, uses fixed paths, never takes commands or pids from the request, and handles exactly three operations. A local user can already run `pkexec` with their own authorization, so the helper adds no new right. A local attacker who can run the helper through `pkexec` still needs admin authorization.
- Risk: a user who is not an administrator cannot authorize. The notice says authorization failed.
- Risk: `/proc/self/exe` can point to a deleted file if the user rebuilds while hotmon runs. The apply then fails with the `pkexec` error text. Accept this.
- Risk: hotmon is started from a path that is not safe for root (for example a user-writable build directory). Root runs that binary after authorization. This is the normal rule of `pkexec`. Note it in the README: for a system install, put `hotmon` in a root-owned directory.
- Assumption: the backend selected at boot (`select_backend`) is sent to the helper by name. The helper trusts it only as a choice among four known values.
- Assumption: SCM_RIGHTS works through the socketpair on stdin and stdout of the `pkexec` child. `pkexec` runs the target with fds 0, 1, 2 from the caller. Verify this early with the capture manual check. If it fails, fall back to a named Unix socket in a private temp directory created by the parent. Do not switch without testing.
- Open assumption: the new argument handling (`--privileged-helper`, reject others) is allowed. It changes the old behavior of ignoring arguments.
- Open assumption: tests cannot call a real `pkexec`. The fake helper script covers the process and error paths. The real prompt only has manual coverage.
