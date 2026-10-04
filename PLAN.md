# Plan: Port hotmon from Rust to C++

This plan replaces the old plan. The old plan was for a previous feature.
Read this file from top to bottom before you write code.
This file has all the context you need.

## 1. Goal and non-goals

### Feature prompt (exact text)

> I want to rework this entire project into C++. Layout a port plan, taking into account the changes listed to be made in ROADMAP.md.

### Goal

- Replace the whole Rust program with a C++ program. The program name stays `hotmon`.
- Keep every current behavior of the Rust program. The Rust code is the specification.
- Add the change in `ROADMAP.md` item 1 ("Automatic network settings in the wizard") to the C++ program.
- Keep the profile file format. A profile saved by the Rust program must load in the C++ program.
- Remove the Rust code, `Cargo.toml`, and `Cargo.lock` after the port passes all tests.

### Roadmap summary (full text is in `ROADMAP.md`)

- The default path fills band, channel, address range, and DHCP range. The user types none of them.
- Default band is 5 GHz on channel 36.
- "Increased compatibility" uses 2.4 GHz on channel 6.
- An access-point interface with no 5 GHz radio uses 2.4 GHz on channel 6. The review page says so.
- Optional "advanced setup" lets the user choose band, channel, address range, DHCP on or off, DHCP start, and DHCP end.
- Selection boxes replace typed input for: security mode, band choice, advanced band, advanced channel, DHCP on or off, access-point interface, upstream interface.
- SSID, passphrase, advanced address range, advanced DHCP start, and advanced DHCP end stay typed.
- The review page shows band, channel, address range, and DHCP range before apply.
- A saved profile still loads its stored band, channel, address range, and DHCP values.
- The saved and applied profile fields do not change.

### Non-goals

- Do not add new features beyond the Rust program and `ROADMAP.md` item 1.
- Do not change the hotspot backends (NetworkManager, iwd, existing hostapd, direct hostapd + dnsmasq + nftables).
- Do not change the profile JSON field names or values.
- Do not add a graphical interface, a daemon, a config option for the backend, or IPv6.
- Do not add network access at build time. The build uses only system packages.
- Do not keep the Rust code as a second build. This is a clean cutover.
- Do not rewrite or delete the old report files (`*-report.md`, `PLAN.secaud.md`).

## 2. Repo context

### The Rust program today (the source of truth for the port)

hotmon is a text user interface (TUI) for a Wi-Fi hotspot on Linux. It has a wizard, a status view, a monitor view, and packet capture.

Rust sources (about 5,900 lines, 99 unit tests) in `src/`:

| Rust file | Lines | Tests | Role |
|---|---|---|---|
| `main.rs` | 25 | 0 | Starts the app. Restores the terminal on panic. |
| `profile.rs` | 620 | 12 | `Profile`, `SecurityMode`, `Band`, `Ipv4Network`, validators, JSON save and load, review lines. |
| `iface.rs` | 187 | 3 | `IfaceInfo`, name check, reads `/sys/class/net`, asks `iw phy <phy> info` for AP mode. |
| `backend.rs` | 2317 | 26 | Backend detection, apply plans, plan executor with rollback, stop logic, config text builders. |
| `capture.rs` | 410 | 5 | `CaptureControl` state machine, packet summaries, `FrameSource` trait. |
| `capture_lib.rs` | 97 | 1 | `AF_PACKET` raw socket (`LocalCapture`). |
| `monitor.rs` | 276 | 4 | Parses `iw station dump` and `ip neigh`. Keeps traffic series for each client. |
| `wizard.rs` | 552 | 8 | Wizard pages and typed fields. This file changes the most. |
| `app.rs` | 1034 | 15 | `App` state, key handling, apply and stop, client refresh. |
| `ui.rs` | 326 | 2 | Event loop and drawing with ratatui. |

Rust dependencies: `crossterm`, `ratatui` (terminal), `serde`, `serde_json` (profile JSON), `libc`.

Git history: the Rust sources stay in the work tree until the last step. If you need the old code later, run `git show main:src/<file>.rs`.

### Environment (observed on the build machine)

- OS: Arch-based Linux (omarchy). Compilers: `g++` 16.2 and `clang++`. Both support C++23.
- Tools: `cmake` 4.4 (installed), `make`, `pkg-config`. `ninja` is not installed. Use the default CMake generator.
- Libraries found: `ncursesw` (pkg-config `ncursesw`, header `/usr/include/ncurses.h`), `yyjson` (pkg-config `yyjson`, CMake package `yyjson`), `GTest` (CMake package `GTest`, pkg-config `gtest`), `fmt`.
- `nlohmann_json` is NOT installed. Do not use it.
- Runtime tools used by the program: `systemctl`, `nmcli`, `iw`, `ip`, `hostapd`, `dnsmasq`, `nft`. The tests must not call them. `nmcli` and `iw` exist on this machine. The others may not.

### Chosen C++ stack

| Concern | Choice | Reason |
|---|---|---|
| Language standard | C++23 (`-std=c++23`) | `std::expected` replaces Rust `Result<T, String>`. |
| Build | CMake (minimum 3.20), out-of-tree in `build/` | Standard. CMake is installed. |
| Terminal UI | ncurses (wide-character, `ncursesw`) | Installed. No network fetch needed. |
| JSON | `yyjson` through `find_package(yyjson)` or `pkg_check_modules` | Installed. Keeps key order. |
| Tests | GoogleTest (`find_package(GTest REQUIRED)`) with `gtest_discover_tests` | Installed. |
| System calls | POSIX: `fork`/`execvp`/`pipe`/`waitpid`, `open`/`fchmod`/`chown`, `socket(AF_PACKET)`, `getifaddrs` | Replaces `std::process::Command` and `libc`. |

Error type: define `using Result<T> = std::expected<T, std::string>;` in `src/result.hpp`. Error text stays the same as the Rust text.

### Install, build, test, run

```
sudo pacman -S --needed cmake gtest yyjson ncurses pkgconf   # packages (already present on this machine)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure      # test command
build/hotmon                                    # run command (the wizard works without root)
sudo build/hotmon                               # run with root to start the hotspot
```

Compiler flags: `-Wall -Wextra -Wpedantic -Werror` for project targets.

### Conventions

- Write all documents, comments, and commit messages in ASD-STE100 (Simplified Technical English). Use short sentences (20 words or fewer). Use active voice.
- User-visible text stays exactly as in the Rust code unless this plan names a change. The tests compare these strings.
- No shell. Run external programs with `fork` + `execvp` and an argument vector. Never build a command line string.
- Never put the passphrase in a command argument or in the review text. Never log it.
- Files that hold secrets use mode `0600`. Their directory uses mode `0700`. Copy the exact modes and rollback order from `backend.rs`.
- Keep the "plan then execute" design. Builders return data (`ApplyPlan`). The executor runs it through the `Runner` and `ProcessControl` interfaces. Tests replace both with fakes.
- One header and one source file per module. Namespace: `hotmon`.

## 3. Implementation steps

Do the steps in order. After each step, the project must build and the tests of that step must pass.
For each ported module, translate every Rust `#[test]` into a GoogleTest case with the same intent and the same asserted text. Section 4 lists the files.

### Step 1. Project skeleton and build system

1. Create `CMakeLists.txt` in the repo root:
   - `cmake_minimum_required(VERSION 3.20)`, `project(hotmon LANGUAGES CXX)`, C++23, no compiler extensions.
   - `find_package(Curses REQUIRED)` with `CURSES_NEED_WIDE ON`. If this fails, use `pkg_check_modules(NCURSESW REQUIRED IMPORTED_TARGET ncursesw)`.
   - `pkg_check_modules(YYJSON REQUIRED IMPORTED_TARGET yyjson)`.
   - `find_package(GTest REQUIRED)`, `enable_testing()`, `include(GoogleTest)`.
   - Static library `hotmon_core` from all `src/*.cpp` except `main.cpp`. Link yyjson. Link ncurses only in the `ui` and `terminal` sources (the core library may link it too).
   - Executable `hotmon` from `src/main.cpp` linked to `hotmon_core`.
   - Executable `hotmon_tests` from `tests/*.cpp` linked to `hotmon_core` and `GTest::gtest_main`. Call `gtest_discover_tests(hotmon_tests)`.
2. Update `.gitignore`: keep `/target` until step 12, and add `/build`.
3. Create `src/result.hpp` with `Result<T>` and a helper `unexpected_text(std::string)`.
4. Create `src/main.cpp` with an empty `main` that returns 0. Create `tests/smoke_test.cpp` with one passing test. Build and run `ctest`.

Do not delete the Rust files yet. The directory `src/` holds both languages until step 12. CMake globs only `*.cpp`.

### Step 2. `iface` module (from `iface.rs`)

Files: `src/iface.hpp`, `src/iface.cpp`.

- `struct IfaceInfo { std::string name; bool wireless; bool supports_ap; bool supports_5ghz; std::vector<int> channels_24; std::vector<int> channels_5; }`.
  - The last three fields are new (roadmap). They describe what the radio can use.
  - `channels_24` and `channels_5` list the channels the radio allows now. They come from `iw phy <phy> info`.
- `bool valid_name(std::string_view)`: same rules (first char ASCII letter, at most 15 characters, rest `[A-Za-z0-9_.:-]`).
- `bool modes_support_ap(std::string_view text)`: a trimmed line equal to `* AP` or `AP`.
- `Result<void> require_ap(const IfaceInfo&)`: same two error messages.
- New parser `PhyCaps parse_phy_info(std::string_view text)` where `PhyCaps { bool supports_ap; std::vector<int> channels_24; std::vector<int> channels_5; }`.
  - It reads the `Frequencies:` blocks of `iw phy info`. A line looks like `* 5180.0 MHz [36] (22.0 dBm)` or `* 2412 MHz [1] (20.0 dBm)`.
  - Skip a channel when the line contains `(disabled)` or `no IR`. Keep `radar detection` channels (see risk R3).
  - A channel number 1 to 14 on a 24xx MHz frequency goes to `channels_24`. A frequency from 5000 to 5895 MHz goes to `channels_5`.
  - Keep only channels that `channel_allowed()` in `profile` accepts for the band (the program supports 1 to 13 and the listed 5 GHz channels). Sort and remove duplicates.
- Interface `class PhyInfo { virtual PhyCaps caps(std::string_view phy) const = 0; }`. This replaces the Rust `supports_ap(phy)` method.
- `class SystemPhyInfo`: runs `iw phy <phy> info` through a small helper `run_capture({"iw","phy",phy,"info"})` (see Step 5 for the helper). A failed run gives empty caps.
- `std::vector<IfaceInfo> read_interfaces_at(const std::filesystem::path& root, const PhyInfo&)` and `read_system_interfaces()`: same logic as Rust. Skip `lo` and non-directories. `wireless` is true when `wireless/` exists or `phy80211` links to a phy. Sort by name. Set `supports_5ghz = !channels_5.empty()`.
- Add `std::vector<IfaceInfo> ap_candidates(const std::vector<IfaceInfo>&)`: wireless and `supports_ap`. Add `std::vector<IfaceInfo> upstream_candidates(const std::vector<IfaceInfo>&, std::string_view ap)`: every interface except `ap`. The UI uses these lists for the selection boxes.

### Step 3. `profile` module (from `profile.rs`)

Files: `src/profile.hpp`, `src/profile.cpp`.

- `enum class SecurityMode { Open, Wpa2, Wpa3 }` with `as_str()` returning `open`, `wpa2`, `wpa3`.
- `enum class Band { Band24, Band5 }` with `as_str()` returning `2.4`, `5`.
- `struct Profile` with these fields in this order: `ap_interface`, `ssid`, `security`, `passphrase`, `band`, `channel` (`uint16_t`), `address_cidr`, `dhcp_enabled`, `dhcp_start`, `dhcp_end`, `upstream_interface`. Define `operator==`.
- `class Ipv4Network`: `parse`, `prefix`, `gateway`, `netmask`, `usable`, `contains_text`. Same rules and same messages. Prefix must be 8 to 30. The address must be a network address. Use `uint32_t` host-order math and `inet_pton`/`inet_ntop` (or a small own parser that accepts only four decimal parts). Reject forms that `inet_pton` rejects. Rust `Ipv4Addr::parse` rejects leading zeros such as `010.0.0.1`. Match this.
- Free functions with the same behavior and text: `parse_ipv4`, `parse_security`, `parse_band`, `validate_ssid`, `validate_passphrase`, `channel_allowed`, `parse_channel`, `parse_dhcp_flag`, `validate_interface`, `validate_upstream`, `validate_address_dhcp`.
  - `validate_ssid` counts bytes (limit 32). It rejects `.` and `..`, any `/`, and any control character. Decode UTF-8 to find Unicode control characters (U+0000 to U+001F and U+007F to U+009F).
  - `validate_passphrase` counts characters (8 to 63) but needs printable ASCII, so byte count equals character count when it passes.
  - `OPEN_UPSTREAM_WARNING` stays `"Every device in radio range can use the upstream network."`.
- `Result<void> Profile::check_settings() const`, `Result<Ipv4Network> Profile::network() const`, `std::vector<std::string> Profile::review_lines() const`. Keep the existing lines. The roadmap needs more review lines. Step 8 adds them in the wizard, not here. Leave `review_lines()` as the pure profile summary.
- Paths and files: `profile_path_from(optional<string> xdg, optional<string> home)` and `default_profile_path()`. Same order: non-empty `XDG_CONFIG_HOME`, then `HOME/.config`, then relative `.config`.
- `save_profile(path, profile)`: create the parent directory, set it to `0700`, write the JSON text plus a newline with mode `0600` (open with `O_WRONLY|O_CREAT|O_TRUNC` and mode, then `fchmod`).
- `load_profile(path)` and `load_optional(path)`: same messages (`The program cannot read ...`, `The profile file is not valid. ...`).
- JSON with `yyjson`:
  - Write an object with keys in field order. `security` is a lowercase string. `band` is the string `"2.4"` or `"5"`. `channel` is a number. `dhcp_enabled` is a boolean. Use the pretty flag. The indent width does not have to match serde (serde used 2 spaces).
  - Read strictly like serde: every field must exist with the right type. `security` must be `open`, `wpa2`, or `wpa3`. `band` must be `"2.4"` or `"5"`. `channel` must be an integer from 0 to 65535. Ignore unknown keys. Any failure gives the error `The profile file is not valid. <detail>`.
- Test helpers go in `tests/test_support.hpp` (see Section 4): `sample_profile()`, `scratch_dir()`, `sample_interfaces()`.

### Step 4. `monitor` module (from `monitor.rs`)

Files: `src/monitor.hpp`, `src/monitor.cpp`.

- `ClientSnapshot { mac; optional<string> ip; uint64_t rx_bytes; uint64_t tx_bytes; }`.
- `Series` (capacity 40 samples, `observe(total)` stores the delta, a decrease gives 0, the first sample gives 0, `samples()` returns a vector). Use `std::deque<uint64_t>`.
- `MonitorState`: `update`, `clients()`, `total_samples()`, `clear()`. Use `std::map` and `std::set` where Rust used `BTreeMap` and `BTreeSet`, so the order stays sorted.
- `parse_station_dump`, `parse_neigh`, `clients_from_text`, with the same parsing rules. Read `monitor.rs` lines 117 to 200 for the exact rules (label search, counter parse).

### Step 5. `backend` module (from `backend.rs`)

This is the largest module. Split it into these files. All text output of the builders must stay byte-identical to the Rust builders for the same profile (except `nm_keyfile` and friends as noted in Step 8).

| File | Content |
|---|---|
| `src/process.hpp/.cpp` | `run_capture(argv)`: fork, exec, collect stdout and stderr, return exit status and text. No shell. Used by `SystemRunner`, `SystemPhyInfo`, `service_active`, monitor refresh. |
| `src/backend.hpp/.cpp` | `BackendKind`, `label()`, `ProbeFacts`, `select_backend`, `service_active`, `probe_system`, `Paths`, `PlannedCommand`, `PlanFile`, `ApplyPlan`, `plan_apply`, `plan_stop`, `run_stop_commands`. |
| `src/backend_text.hpp/.cpp` | `nm_keyfile`, `iwd_profile`, `hostapd_conf_text`, `dnsmasq_conf_text`, `nft_text`. |
| `src/backend_exec.hpp/.cpp` | `Runner`, `ProcessControl`, `SystemRunner`, `SystemSignals`, `StartedProc`, `StartReport`, `execute_plan`, rollback, `stop_started`, `restore_forwarding`, `restore_hostapd_backup`, `retire_iwd_profile`, file install helpers, `secure_state_dir`, `enable_forwarding`, `daemon_name`, pid file readers. |

Rules for the port:

- `BackendKind { NetworkManager, Iwd, ExistingHostapd, DirectHostapd }`. Read `backend.rs` lines 12 to 53 for labels and the selection order. NetworkManager first, then iwd, then existing hostapd, then direct tools. `DIRECT_TOOLS` is `hostapd`, `dnsmasq`, `nftables`.
- `Paths` keeps the same fields (`state_dir`, `hostapd_config`, `iwd_ap_dir`, `proc_root`) and the same derived paths. System values: `/run/hotmon`, `/etc/hostapd/hostapd.conf`, `/var/lib/iwd/ap`, `/proc`.
- `Runner` and `ProcessControl` become abstract classes with virtual methods: `Result<std::string> run(const PlannedCommand&)`, `optional<string> describe(int pid)`, `bool running(int pid)`, `Result<void> terminate(int pid)`.
- `SystemSignals`: read `/proc/<pid>/comm` and `/proc/<pid>/cmdline`. `daemon_name` accepts only `hostapd` and `dnsmasq`. `terminate` sends `SIGTERM` with `kill`. Copy the wait and retry logic from `backend.rs` lines 282 to 312.
- `execute_plan` keeps the exact order: install files, enable forwarding, run commands, record started processes, roll back on failure. Port `fail_start`, `rollback`, `InstalledFiles`, `install_system_hostapd`, `install_private_hostapd`, `redirect_hostapd`, `note_started`, `started_pid` function by function. The 26 Rust tests describe the rollback rules. Port all of them.
- Use RAII (a small `FileDescriptor` class) for file descriptors. Use `std::filesystem` for copy, remove, and permissions. Use `::chown(path, 0, 0)` for `secure_state_dir` and keep the fallback in `backend.rs` lines 971 to 992.
- Test doubles (`ScriptedRunner`, `RecordedSignals`, `PidOnSuccess`, `ModeCheck`) go in `tests/test_support.hpp`, not in `src/`.

### Step 6. `capture` modules (from `capture.rs`, `capture_lib.rs`)

Files: `src/capture.hpp/.cpp`, `src/capture_socket.hpp/.cpp`.

- `CapturePhase { Idle, Warned, Armed, Running }`, `ConfirmResult { Warning, Open{iface}, Already, Ignored }` (use a struct with a kind enum and a string).
- `class FrameSource { virtual Result<std::optional<std::vector<uint8_t>>> try_recv() = 0; }`. The error value is a message string.
- `CaptureControl`: same state machine and methods (`warn`, `accept`, `dismiss_warning`, `attach`, `fail_open`, `stop`, `poll`, `recent_lines`, `bound_interface`, `armed_interface`). Keep the line buffer limit. Read `capture.rs` lines 56 to 200.
- `capture_interface`, `summarize`, `summarize_ipv4`, `port_text`, `format_mac`, `PacketSummary::text()` with the same output (`"<len> B <src> -> <dst> <proto>"`). The summary function must never modify the frame (take `std::span<const uint8_t>`).
- `CAPTURE_WARNING` text stays exact.
- `LocalCapture` (in `capture_socket`): `AF_PACKET`, `SOCK_RAW`, `htons(ETH_P_ALL)`, `if_nametoindex`, `bind` with `sockaddr_ll`, non-blocking `recv` with `MSG_DONTWAIT`, `EAGAIN` gives "no frame". Check `valid_name` first and fail with `Invalid input` text `The capture interface name is not valid.` before any socket call. Use a 65535-byte buffer allocated once per object, not per call.

### Step 7. Network auto-selection module (new, from the roadmap)

Files: `src/netauto.hpp`, `src/netauto.cpp`. This module has pure functions. It does not call the system, except in the two `read_*` functions.

```
struct Ipv4Range { uint32_t base; uint8_t prefix; };          // any prefix 0..32, not validated for the hotspot rules
bool overlaps(Ipv4Range a, Ipv4Range b);                      // true when one range contains the other
std::vector<Ipv4Range> read_local_networks(std::string_view exclude_iface);  // getifaddrs (AF_INET) + /proc/net/route
std::vector<Ipv4Range> parse_proc_net_route(std::string_view text);           // pure, for tests; skips prefix 0 (default route)
struct AutoNetwork { std::string address_cidr; std::string dhcp_start; std::string dhcp_end; };
Result<AutoNetwork> choose_network(const std::vector<Ipv4Range>& used);
struct AutoRadio { Band band; uint16_t channel; bool fell_back; };
AutoRadio choose_radio(const IfaceInfo& ap, bool increased_compatibility);
```

Rules:

- `read_local_networks(exclude_iface)`: take every IPv4 address with its netmask from `getifaddrs`, and every non-default route from `/proc/net/route`. Skip the interface named `exclude_iface` (the access-point interface) and `lo`. A route line has hex fields in host byte order on little-endian: destination, gateway, mask. Convert with `ntohl`. Skip lines with mask 0.
- `choose_network(used)`: test these candidate `/24` networks in order and return the first one that overlaps none of `used`:
  1. `192.168.42.0/24` (the current default),
  2. `192.168.43.0/24` to `192.168.255.0/24`,
  3. `192.168.0.0/24` to `192.168.41.0/24`,
  4. `10.42.0.0/24` to `10.42.255.0/24` (third octet 0 to 255),
  5. `172.16.0.0/24` to `172.31.0.0/24` (second octet 16 to 31, third octet 0).
  - If nothing fits, return the error `No private address range is free. Use advanced setup.`
  - For the chosen network `N`, `address_cidr = "<N>/24"`, `dhcp_start = N+10`, `dhcp_end = N+100`. The gateway is `N+1`. The range never includes the gateway.
- `choose_radio(ap, compat)`:
  - If `compat` is true, return `{Band24, 6, false}`.
  - Else if `ap.supports_5ghz` and channel 36 is in `ap.channels_5`, return `{Band5, 36, false}`.
  - Else return `{Band24, 6, true}` (`fell_back = true`). The review page shows the text `This interface has no 5 GHz radio. The wizard uses 2.4 GHz on channel 6.` In this case the wizard shows that text. If the radio has 5 GHz but not channel 36, use the text `This interface cannot use 5 GHz channel 36. The wizard uses 2.4 GHz on channel 6.`
  - The fallback to 2.4 GHz on channel 6 does not check `channels_24`. Channel 6 is always the default for 2.4 GHz.

### Step 8. `wizard` module (rewrite, from `wizard.rs` plus the roadmap)

Files: `src/wizard.hpp`, `src/wizard.cpp`, `src/select_box.hpp`, `src/select_box.cpp`.

#### 8.1 Selection box

```
struct Choice { std::string label; std::string value; };
class SelectBox {
 public:
  SelectBox(std::vector<Choice> choices, size_t selected = 0);
  void up();            // moves up, stops at the first choice (no wrap)
  void down();          // moves down, stops at the last choice (no wrap)
  const Choice& current() const;
  bool select_value(std::string_view value);   // returns false when absent
  const std::vector<Choice>& choices() const;
  size_t index() const;
};
```

The user moves with Up and Down. The user confirms with Enter (this is the same Enter that goes to the next page). A selection box accepts no typed text. Typed characters for a selection field are ignored.

#### 8.2 Pages and order

The Rust order was: Interface, Ssid, Security, Passphrase, BandChannel, AddressDhcp, Upstream, Review.
The new order is below. `Upstream` moves before the radio and network pages. The automatic address choice needs the upstream choice to avoid an overlap with the upstream network.

| # | Page enum | Title | Fields |
|---|---|---|---|
| 1 | `Interface` | "Access-point interface" | selection box: `ap_candidates` |
| 2 | `Ssid` | "SSID" | typed text |
| 3 | `Security` | "Security mode" | selection box: Open, WPA2, WPA3. Opens with WPA2 selected. |
| 4 | `Passphrase` | "Passphrase" | typed text, masked with `*` |
| 5 | `Upstream` | "Upstream interface" | selection box: `upstream_candidates` then `None` |
| 6 | `BandChannel` | "Band and channel" | selection box: `5 GHz`, `Increased compatibility`. Opens with 5 GHz selected. |
| 7 | `AddressDhcp` | "Address range and DHCP" | shows the chosen values (read only). Key `a` opens advanced setup. Key `d` returns to automatic values. |
| 8 | `Advanced` | "Advanced setup" | six fields, see 8.4. Only reachable with `a` from page 7. |
| 9 | `Review` | "Review" | no fields |

- `ORDER` for "Page N of M" is pages 1 to 7 plus `Review`. The position text shows `Review` as the last page. `Advanced` shows as `Page 7 of 8: Advanced setup` (same number as the network page).
- Enter on page 7 goes to `Review`. Enter on `Advanced` validates and goes to `Review`. Left on `Advanced` goes to page 7. Left on `Review` goes to the last page the user came from (`Advanced` if the user set advanced values, else page 7).
- Empty candidate lists: Page 1 with no candidate shows the error `No interface can start an access point.` and Enter stays on the page. Page 5 always has `None`.
- Keep: Esc cancels, Left goes back, Tab goes to the next typed field, Backspace edits typed fields, `y` confirms on Review, 128 characters limit for typed fields.
- Add keys: Up and Down move in a selection box (and move the active field up or down on `Advanced`, see 8.4).

#### 8.3 Wizard state

Replace the free-text strings for the selection fields by `SelectBox` members and typed `std::string` members for the typed fields.

```
enum class NetSource { Automatic, Advanced, Saved };
struct Wizard {
  Page page; size_t field; bool cancelled; optional<string> error;
  SelectBox ap_interface, security, upstream, band_choice;
  std::string ssid, passphrase;
  // resolved radio and network values
  Band band; uint16_t channel; bool radio_fell_back; std::string radio_note;
  std::string address_cidr; bool dhcp_enabled; std::string dhcp_start, dhcp_end;
  NetSource radio_source, network_source;
  // advanced page state
  SelectBox adv_band, adv_channel, adv_dhcp;
  std::string adv_address_cidr, adv_dhcp_start, adv_dhcp_end;
};
```

Rules:

- `Wizard::next(const HostFacts&)` needs facts about the host. Define `struct HostFacts { std::vector<IfaceInfo> interfaces; std::vector<Ipv4Range> local_networks; }`. `App` holds one `HostFacts` and passes it. For the local networks, read `read_local_networks(ap_interface)` when the wizard enters page 7, not at start-up. Tests inject `local_networks` through a function pointer or a `std::function` member in `HostFacts` so tests do not read the real system. Use `std::function<std::vector<Ipv4Range>(std::string_view ap)> local_networks`.
- Entering page 6: rebuild the `band_choice` box. Re-run `choose_radio(ap, compat=false)`. If `radio_source` is `Saved` or `Advanced`, keep the stored values (see "Saved profile" below).
- Confirming page 6: set `band` and `channel` from `choose_radio(ap, compat)` where `compat` is true for the choice `Increased compatibility`. Set `radio_source = Automatic`. Save `radio_fell_back` and `radio_note`.
- Entering page 7 with `network_source == Automatic`: compute `choose_network(local_networks(ap))`. Set `address_cidr`, `dhcp_enabled = true`, `dhcp_start`, `dhcp_end`. If `choose_network` fails, show the error and stay on page 6 with the error text.
- Page 7 shows four read-only lines: `Address range: <cidr>`, `DHCP: on`, `DHCP start: <ip>`, `DHCP end: <ip>`. The hint says: `The wizard chose these values. Press Enter to accept, or a for advanced setup.`
- Key `d` on page 7 (and on `Advanced`) sets `network_source = Automatic` and `radio_source = Automatic`, recomputes the values, and clears the advanced typed values.

#### 8.4 Advanced setup page

Fields in this order, `field` index 0 to 5:

| # | Label | Kind | Content |
|---|---|---|---|
| 0 | Band | selection box | `2.4 GHz` and `5 GHz`. Show a band only when the interface lists at least one channel for it. |
| 1 | Channel | selection box | the channels the interface allows for the selected band (`channels_24` or `channels_5`). A channel the interface cannot use is not in the box. The box rebuilds when the band changes. |
| 2 | Address range | typed | prefilled with the current value |
| 3 | DHCP | selection box | `On`, `Off` |
| 4 | DHCP start | typed | prefilled |
| 5 | DHCP end | typed | prefilled |

- Tab moves to the next field. Up and Down move inside a selection field. Characters go only to typed fields.
- If `channels_24` and `channels_5` are both empty (for example, `iw` failed), the Band and Channel boxes list the full allowed lists from `profile.cpp` for 2.4 GHz only (channels 1 to 13), and the review note says nothing special. See risk R2.
- Enter validates with `validate_address_dhcp(address, dhcp, start, end)`. It gives the same error messages as today: bad range, host address, empty value, bad prefix, and a DHCP range that contains the gateway all stay rejected. Convert the DHCP box value to `"on"` or `"off"` for this call. If DHCP is off, clear start and end.
- A successful Enter sets `network_source = Advanced` and `radio_source = Advanced`, and stores `band`, `channel`, and the address values. Then it goes to `Review`.

#### 8.5 Saved profile

- `Wizard::from_profile(const Profile&, const HostFacts&)` starts on `Review`. It selects the stored interface, security, and upstream in the boxes (add the stored interface to a box when the box lacks it, so the review stays valid). It stores `band`, `channel`, `address_cidr`, `dhcp_*` from the profile. It sets `radio_source = Saved` and `network_source = Saved`.
- With source `Saved`, the wizard keeps the stored values until the user changes them. Page 6 shows an extra first choice only when the saved pair is not (5, 36) and not (2.4, 6): `Saved setting (<band> GHz, channel <n>)`. The box opens on that choice.
- Page 7 with source `Saved` shows the stored values and the hint `Saved values. Press d for automatic values or a for advanced setup.`
- When the saved pair is (5, 36) or (2.4, 6), page 6 opens on the matching choice and `radio_source` stays `Saved` until the user confirms. A confirmed choice sets `Automatic`.

#### 8.6 Review page

`Wizard::review_lines()` gives all lines. It starts with `Profile::review_lines()` for the existing lines. It then adds:

- After the `Channel:` line, when `radio_fell_back`, add the note from `radio_note`.
- A line `Settings source: automatic`, `Settings source: set by the user (advanced setup)`, or `Settings source: saved profile` (use `network_source`).
- The `Address range:` and `DHCP:` lines already show `address_cidr` and `on <start>-<end>`.

The review page must show: band, channel, address range, DHCP range, and any radio note, before apply. The review page never shows the passphrase.

#### 8.7 Confirm

- `Wizard::confirmed_profile(const HostFacts&)` builds a `Profile` with the same fields as before and runs `Profile::check_settings()`. It runs `validate_interface` and `validate_upstream` against `facts.interfaces`. Upstream value is `none` (lowercase) for the `None` choice.
- `Wizard::hint()` and `Wizard::field_lines()` return text for each page. For selection fields, `FieldLine` carries the choices and the selected index, so `ui` can draw a list with a marker `(*)` for the selected choice and `( )` for the others.
- `Wizard::open_upstream_risk()` is true when security is Open and upstream is not `none`. This is the same rule as today.

### Step 9. `app` module (from `app.rs`)

Files: `src/app.hpp`, `src/app.cpp`.

- Replace crossterm key types with `struct Key { enum class Code { Char, Enter, Esc, Left, Right, Up, Down, Tab, Backspace, Other }; Code code; char32_t ch; bool ctrl; }`.
- Port `View`, `HotspotStatus` (use `std::variant` or a struct with a kind), `Step`, `App`. Keep all fields and all methods: `boot`, `from_parts`, `on_key`, `apply_hotspot`, `stop_hotspot`, `refresh_clients`, `tick`, `capture_open_failed`, `hotspot_iface`, and the private helpers.
- `App::boot()`: `select_backend(probe_system())`, `read_system_interfaces()`, `default_profile_path()`, `load_optional`. If the profile fails to load, `boot` returns the error. `main` prints it to stderr and exits with code 1. Keep this.
- Update `on_wizard_key` for the new keys: Up and Down go to the wizard. `a` on page 7 opens `Advanced`. `d` on page 7 and `Advanced` resets to automatic. Character keys reach only typed fields. `y` on `Review` keeps its meaning. Keep the open-upstream two-step confirmation (Enter shows the warning, then `y` applies).
- `a` and `d` count as typed text on typed fields of `Advanced` (the user may type them in an address). Rule: on `Advanced` the keys `a` and `d` are text. Only page 7 treats `a` and `d` as commands. To reset on `Advanced`, the user goes Left to page 7 and presses `d`.
- Keep `Ctrl+q` as quit in every view. Keep `q`, `c`, `Enter`, `m`, `s`, `w`, `k`, `z` in the status and monitor views.

### Step 10. Terminal UI (from `ui.rs`, `main.rs`)

Files: `src/render.hpp/.cpp` (pure text rendering), `src/terminal.hpp/.cpp` (ncurses), `src/main.cpp`.

Design for testability: `render(const App&, int width, int height)` returns a `std::vector<std::string>` of text lines for the whole screen. The ncurses code only draws those lines. Tests check the lines. This replaces the ratatui `TestBackend`.

- `render.cpp`: port `header`, `wizard_body`, `status_body`, `draw_monitor`, `footer`, `capture_lines`, `traffic_graph`, `preview_profile`. Draw boxes with ASCII or Unicode line characters. Keep titles and texts.
  - The header shows `hotmon  <View>  <backend label>` and the notice (or the wizard error when the notice is empty).
  - Wizard body: `Page N of M: <title>`, the hint, a blank line, then the fields. A selection field shows each choice on its own line with `(*)` or `( )`, and a `>` mark on the active field. A typed field shows `> Label: value`. Long lines wrap.
  - Review page: the lines from `Wizard::review_lines()`.
  - Footer keys per view (update the wizard footers): selection page `Up/Down: choose  Enter: next page  Left: previous page  Esc: cancel  Ctrl+q: quit`. Typed page: the old footer text. Network page: `Enter: accept  a: advanced setup  d: automatic values  Left: previous page  Esc: cancel  Ctrl+q: quit`.
  - Sparkline: map each sample to one of `▁▂▃▄▅▆▇█` using `value * 7 / max` (max of the shown samples, 0 gives `▁`). Show the last `width` samples.
- `terminal.cpp`: RAII class `Terminal`. Constructor calls `setlocale(LC_ALL, "")`, `initscr`, `raw`, `noecho`, `keypad(stdscr, TRUE)`, `curs_set(0)`, `set_escdelay(25)`, `timeout(200)`. Destructor calls `endwin`. `raw()` makes Ctrl+q reach the program as code 17 (not flow control). Install `std::set_terminate` and signal handlers (`SIGTERM`, `SIGINT`, `SIGHUP`) that call `endwin()` first. This replaces the Rust panic hook.
  - Read keys with `get_wch`. Map `KEY_LEFT`, `KEY_RIGHT`, `KEY_UP`, `KEY_DOWN`, `KEY_ENTER` and `'\n'` and `'\r'`, `27` (Esc), `'\t'`, `KEY_BACKSPACE` and `127` and `8`, `17` (Ctrl+q), `KEY_RESIZE` (redraw). A timeout (`ERR`) calls `app.tick(runner)`, as in the Rust loop.
  - Draw with `mvaddstr` and `addnwstr` (wide characters). Use yellow for the notice line.
- `main.cpp`: port the loop in `ui::run`: draw, read key, `app.on_key`, match `Step` (`Quit`, `Apply`, `StopHotspot`, `OpenCapture`, `Continue`), call `SystemRunner`, `SystemSignals`, `Paths::system()`. `open_capture` creates `LocalCapture`, attaches it, and calls `capture_open_failed` on error (see `ui.rs` lines 50 to 66). `App::boot` runs before ncurses starts, so a boot error prints to stderr normally.

### Step 11. Port all tests and add the new tests

Port the 99 Rust tests and add the new tests of Section 4. Run `ctest`. Fix every failure in the C++ code. Do not weaken a test.

### Step 12. Cutover and cleanup

1. Delete `src/*.rs`, `Cargo.toml`, and `Cargo.lock`. Remove `/target` from `.gitignore`.
2. Update `README.md`: add a "Build" section (the commands in Section 2), a "Wizard" section that says the wizard picks band, channel, address range, and DHCP range by default, and a short note about advanced setup. Keep the existing "Privilege requirements" and "Supported backends" text. Write in ASD-STE100.
3. Update `ROADMAP.md`: item 1 is now in the program. Replace the item with the sentence `No planned items.` under the heading `Roadmap`, or move item 1 to a short "Done" list. Keep the file valid Markdown.
4. Search the repo for leftover mentions of `cargo`, `ratatui`, `crossterm`, `serde`: `grep -rn -i -E "cargo|ratatui|crossterm|serde" --include='*' . --exclude-dir=.git --exclude-dir=build`. Fix mentions in `README.md`. Old report files may keep their history.
5. Do a clean build: `rm -rf build && cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`.

## 4. Tests to write

Test framework: GoogleTest. Run all with `ctest --test-dir build --output-on-failure`.
Test files in `tests/`. No test may call `iw`, `nmcli`, `systemctl`, `hostapd`, `dnsmasq`, or `nft`. No test may need root. No test may write outside a temporary directory. Use `scratch_dir()` (a unique directory under `std::filesystem::temp_directory_path()`, removed at the end).

`tests/test_support.hpp`/`.cpp`: `sample_profile()` (wlan0, SSID `Hotmon`, WPA2, passphrase `correct-horse`, band 2.4, channel 6, `192.168.42.0/24`, DHCP on `192.168.42.10` to `192.168.42.100`, upstream `eth0`), `sample_interfaces()` (eth0 wired, wlan0 wireless + AP + 5 GHz + channels, wlan1 wireless no AP), `scratch_dir()`, `ScriptedRunner`, `RecordedSignals`, `PidOnSuccess`, `ModeCheck`, `FakeSource`.

### 4.1 Ported tests (same intent as the Rust tests)

| New file | Port from | Cases |
|---|---|---|
| `tests/profile_test.cpp` | `profile.rs` (12 tests) | SSID rejects empty and slash; passphrase rules (open, short, wpa3, spaces, backslash, quote, hash, outer spaces); channel matches band; DHCP range inside network and not on gateway; profile round trip; missing profile gives none; corrupt profile gives `not valid`; path uses XDG then HOME; review lists each setting and hides passphrase; open review warning only with an upstream; directory mode `0700` and file mode `0600`; bad channel fails `check_settings`. |
| `tests/iface_test.cpp` | `iface.rs` (3) | `* AP` line needed (not `* AP/VLAN`); `require_ap` messages; sysfs read with a fake `PhyInfo` and symlinks. |
| `tests/monitor_test.cpp` | `monitor.rs` (4) | station dump counters; neigh adds the IP; graph changes with counters; totals add clients. |
| `tests/backend_test.cpp` | `backend.rs` (26) | Every Rust test: backend order; direct plan uses hostapd, dnsmasq, nftables; open hostapd config has no passphrase; NM plan uses `nmcli` only; iwd plan writes the AP file; backend rejection error; existing hostapd uses `systemctl`; no secret in arguments; secret modes; firewall limits forward and input; forwarding touches only hotspot and upstream; failed start removes firewall and daemon; stop does not signal a foreign process; hostapd file restored from backup; missing backup leaves the file; `daemon_name`; NM secret removed after the call; failed install removes the secret and restores the system file; missing system file uses a private file; failed forwarding restores the first interface; failed reapply keeps the live firewall; failed start after stop deletes the table; second failed reapply reloads rules; failed reapply before nft keeps the rules file; second apply keeps the first backup. |
| `tests/capture_test.cpp` | `capture.rs`, `capture_lib.rs` (6) | No capture before the second confirmation; summary does not change the frame; stop ends capture; attach before confirmation fails; the warning key does not start capture; invalid interface name does not open a socket (`LocalCapture::open("")` and `open("not a name")` both return the invalid-name error). Add summary cases for an ARP frame, an IPv4 TCP frame, an IPv4 UDP frame, and a short frame. |
| `tests/app_test.cpp` | `app.rs` (15) | All 15 Rust cases (see names in `src/app.rs` lines 511 to 1000): saved profile loads into the wizard; confirm saves profile and shows status; cancel does not apply; rejected backend keeps hotspot stopped; second capture key arms capture and `z` clears; stop stops capture and records the backend stop; client refresh updates monitor; open upstream needs a different confirmation; stop fails while dnsmasq runs; failed start keeps the old profile; stop removes the manager profile it created; Enter does not start capture after the warning leaves; a new interface shows the capture warning again; private hostapd stop does not stop the system service; failed reapply keeps the private hostapd stop path. Update these tests to drive the new wizard through selection boxes. |
| `tests/render_test.cpp` | `ui.rs` (2) | A rejection message shows in the header. Review and status never show the passphrase. |

### 4.2 New tests for the roadmap

`tests/select_box_test.cpp`:
- Down moves one choice. Up moves back. Up at the first choice stays. Down at the last choice stays.
- `select_value("wpa2")` selects it. An unknown value returns false and keeps the index.
- Typed characters do not change a selection box (test through `Wizard::push_char` on the Security page).

`tests/netauto_test.cpp`:
- `choose_radio`: AP with 5 GHz and channel 36 gives (5, 36, no fallback). Compatibility gives (2.4, 6, no fallback). AP with no 5 GHz gives (2.4, 6, fallback). AP with 5 GHz but no channel 36 gives (2.4, 6, fallback).
- `choose_network` with no used networks gives `192.168.42.0/24`, start `.10`, end `.100`.
- With `192.168.42.0/24` used (upstream), the result is `192.168.43.0/24`.
- With `192.168.0.0/16` used, the result does not start with `192.168.`. It starts with `10.42.` (first candidate in group 4).
- With `10.0.0.0/8` and `192.168.0.0/16` used, the result is in `172.16.0.0/24`.
- With every candidate used, the result is the error `No private address range is free. Use advanced setup.`
- A used network with a prefix shorter than 24 that contains a candidate blocks it. A used `/30` inside a candidate also blocks it.
- `overlaps` is symmetric. `parse_proc_net_route` skips the default route (mask 0) and decodes a sample line (`wlan0 0000A8C0 00000000 0001 0 0 600 00FFFFFF ...` is `192.168.0.0/24`).
- The DHCP range never contains the gateway (`N+1`) and always lies inside the network, for every candidate.

`tests/wizard_test.cpp` (replaces and extends the 8 Rust wizard tests):
- Start state: Security box opens on WPA2. Band box opens on 5 GHz.
- Default path: pick AP interface `wlan0`, type SSID `Cafe Guest`, keep WPA2, type passphrase `correct-horse`, choose upstream `eth0`, keep 5 GHz, accept the network page. `confirmed_profile` gives band 5, channel 36, `192.168.42.0/24`, DHCP on `192.168.42.10` to `192.168.42.100`. The user typed no band, channel, address, or DHCP value.
- Compatibility: choosing `Increased compatibility` gives band 2.4, channel 6.
- No 5 GHz radio: with an AP interface that has `supports_5ghz = false`, the profile has band 2.4, channel 6, and the review lines contain the fallback note.
- Network overlap: with a fake `local_networks` that returns `192.168.42.0/24`, the chosen range is `192.168.43.0/24`.
- AP interface box lists only `wlan0` (not `eth0`, not `wlan1`). The upstream box lists `eth0`, `wlan1`, and `None`, and does not list the AP interface.
- Empty AP candidates: Enter stays on page 1 with `No interface can start an access point.`.
- Security box lists Open, WPA2, WPA3 in this order. No typed text changes it.
- Open security with an empty passphrase passes. Open with a non-empty passphrase fails with `An open network does not use a passphrase.`.
- Passphrase page masks the value. It rejects backslash, double quote, hash, and outer spaces.
- Advanced setup: from page 7 `a` opens `Advanced`. Band box lists only bands with channels. Selecting 5 GHz lists exactly the `channels_5` of the interface. Selecting 2.4 GHz lists exactly `channels_24`. A channel not in the radio list is absent.
- Advanced valid input: address `10.20.30.0/24`, DHCP on, start `10.20.30.50`, end `10.20.30.90` gives a profile with these values. The review shows `Settings source: set by the user (advanced setup)`.
- Advanced bad input, each rejected with the matching message: `192.168.42.5/24` (host address), empty address, `192.168.42.0` (no prefix), `192.168.42.0/31` (bad prefix), `not-an-address`, DHCP range containing the gateway (`192.168.42.1` to `192.168.42.20`), DHCP start after end, DHCP start outside the network, empty DHCP start with DHCP on.
- Advanced with DHCP off: start and end are cleared and the profile has `dhcp_enabled = false`.
- `d` resets to automatic values and the source returns to `automatic`.
- Saved profile (band 2.4, channel 11, `10.9.8.0/24`, DHCP on `10.9.8.20` to `10.9.8.30`): `from_profile` starts on Review. `confirmed_profile` returns the same stored values. The review shows `Settings source: saved profile`. The band box shows the `Saved setting (2.4 GHz, channel 11)` choice first.
- Saved profile with (5, 36): no extra `Saved setting` choice.
- Going back and forward keeps selection values and typed values (port of `valid_pages_advance_and_back_keeps_the_value`).
- Cancel blocks confirm. `confirmed_profile` outside the Review page fails with `Confirm the settings on the review page.`.
- Review page shows band, channel, address range, and DHCP range before apply.
- Typed field limit: 128 characters.
- The Review page with open security and an upstream shows `OPEN_UPSTREAM_WARNING`.

`tests/render_test.cpp` (new cases):
- Wizard Security page text contains `(*) WPA2`, `( ) Open`, `( ) WPA3`.
- Network page text contains `Address range: 192.168.42.0/24` and the hint for advanced setup.
- Review page text contains `Band:`, `Channel:`, `Address range:`, `DHCP:`.
- Footer text matches the page kind.
- Sparkline renders `▁` for zero and `█` for the maximum sample.

`tests/profile_compat_test.cpp`:
- The fixture file `tests/data/rust_profile.json` is the exact output of the old Rust `serde_json::to_string_pretty` for `sample_profile()`:

```
{
  "ap_interface": "wlan0",
  "ssid": "Hotmon",
  "security": "wpa2",
  "passphrase": "correct-horse",
  "band": "2.4",
  "channel": 6,
  "address_cidr": "192.168.42.0/24",
  "dhcp_enabled": true,
  "dhcp_start": "192.168.42.10",
  "dhcp_end": "192.168.42.100",
  "upstream_interface": "eth0"
}
```

- Load this file and compare with `sample_profile()`.
- Save `sample_profile()`, then parse the result with `yyjson` in the test. Check key order, the `band` string `"2.4"`, and the `channel` number.
- A file with a missing field, a wrong type (`"channel": "6"`), `"security": "WPA2"`, `"band": "6"`, or an unknown extra key: missing field, wrong type, bad security, and bad band fail. The extra key loads.

`tests/process_test.cpp` and `tests/netauto_system_test.cpp` (small):
- `run_capture({"true"})` succeeds. `run_capture({"false"})` reports a non-zero status. `run_capture({"echo","a b"})` returns `a b\n` and shows that no shell splits the argument. `run_capture({"definitely-not-a-program"})` returns an error.
- `read_local_networks("")` returns a vector without throwing (no content check).

## 5. Acceptance criteria

Each item is a behavior a user can see.

- [ ] `cmake -S . -B build && cmake --build build -j` succeeds with no warning (flags `-Wall -Wextra -Wpedantic -Werror`).
- [ ] `ctest --test-dir build --output-on-failure` passes all tests. The count is at least 99 (ported) plus the new tests.
- [ ] `build/hotmon` starts in a terminal and shows the wizard on page 1. `Ctrl+q` quits and restores the terminal.
- [ ] The repo has no `.rs` file, no `Cargo.toml`, and no `Cargo.lock`.
- [ ] The Security page is a selection box with Open, WPA2, WPA3. WPA2 is selected at start. The user cannot type a security mode.
- [ ] The access-point interface page lists only interfaces that can start an access point.
- [ ] The upstream page lists the other interfaces and None.
- [ ] The band page offers `5 GHz` (selected at start) and `Increased compatibility`. The user does not type a band or a channel.
- [ ] The default path gives 5 GHz on channel 36.
- [ ] `Increased compatibility` gives 2.4 GHz on channel 6.
- [ ] An interface with no 5 GHz radio gets 2.4 GHz on channel 6 and the review page says so.
- [ ] The network page shows the chosen address range and DHCP range. The user types nothing.
- [ ] The chosen address range does not overlap the upstream network or another local network.
- [ ] The DHCP range is inside the chosen network and does not include the gateway.
- [ ] Advanced setup is optional. The user opens it with `a` on the network page.
- [ ] Advanced setup has selection boxes for band, channel, and DHCP, and typed fields for address range, DHCP start, and DHCP end.
- [ ] The advanced channel box lists only channels the interface allows for the chosen band.
- [ ] A bad range, a host address, an empty value, a bad prefix, or a DHCP range that contains the gateway shows an error and does not leave the advanced page.
- [ ] The review page shows the band, the channel, the address range, the DHCP range, and whether the values are automatic, user set, or saved.
- [ ] The review page never shows the passphrase.
- [ ] The profile file stays at `$XDG_CONFIG_HOME/hotmon/profile.json` (or `~/.config/hotmon/profile.json`) with directory mode `0700` and file mode `0600`.
- [ ] A profile saved by the Rust program loads. The review page shows its stored band, channel, address range, and DHCP values.
- [ ] The apply, stop, status, monitor, and capture behaviors and key bindings (`m`, `s`, `c`, `z`, `k`, `w`, `q`, `Enter`, `Esc`) work as before.
- [ ] An open hotspot with an upstream interface still needs the two-step warning (Enter, then `y`).
- [ ] Capture starts only after the second confirmation and only on the hotspot interface.
- [ ] `README.md` explains the build, and `ROADMAP.md` no longer lists item 1 as planned.

## 6. Manual check script

Use a scratch config directory so the real profile stays safe. These commands run without root. Steps 6 and 7 need root and a Wi-Fi card that can start an access point. Skip them on a machine without one.

```
cd /home/samh/Work/hotmon
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```
Expected: the build ends without error. `ctest` prints `100% tests passed`.

1. Old profile compatibility. Create a profile in the old format:
```
export XDG_CONFIG_HOME=$(mktemp -d)
mkdir -p "$XDG_CONFIG_HOME/hotmon"
cat > "$XDG_CONFIG_HOME/hotmon/profile.json" <<'EOF'
{
  "ap_interface": "wlan0",
  "ssid": "Cafe Guest",
  "security": "wpa2",
  "passphrase": "correct-horse",
  "band": "2.4",
  "channel": 11,
  "address_cidr": "10.9.8.0/24",
  "dhcp_enabled": true,
  "dhcp_start": "10.9.8.20",
  "dhcp_end": "10.9.8.30",
  "upstream_interface": "none"
}
EOF
chmod 600 "$XDG_CONFIG_HOME/hotmon/profile.json"
build/hotmon
```
Expected: the header says `The saved profile is loaded.` The wizard opens on the Review page. The page shows `Band: 2.4`, `Channel: 11`, `Address range: 10.9.8.0/24`, `DHCP: on 10.9.8.20-10.9.8.30`, `Settings source: saved profile`, and `Passphrase: set`. The text `correct-horse` does not appear. Press `Ctrl+q`. The shell prompt returns with a normal terminal.
(If the machine has no `wlan0`, the Review page can show an error. That is correct. Use `ip link` to see your wireless interface name and edit `ap_interface`.)

2. Default path from a clean config:
```
export XDG_CONFIG_HOME=$(mktemp -d)
build/hotmon
```
Expected page 1 "Access-point interface": a list of wireless interfaces that can start an access point. Press `Down` and `Up` to move. Press `Enter`.
- Page 2 "SSID": type `Cafe Guest`. Press `Enter`.
- Page 3 "Security mode": the list shows `( ) Open`, `(*) WPA2`, `( ) WPA3`. Type `x`: nothing changes. Press `Enter`.
- Page 4 "Passphrase": type `correct-horse`. The screen shows `*************`. Press `Enter`.
- Page 5 "Upstream interface": choose `None` or your wired interface. Press `Enter`.
- Page 6 "Band and channel": `(*) 5 GHz` and `( ) Increased compatibility`. Press `Enter`.
- Page 7 "Address range and DHCP": the page shows `Address range: 192.168.42.0/24` (or `192.168.43.0/24` when your upstream uses `192.168.42.0/24`), `DHCP: on`, `DHCP start: ...10`, `DHCP end: ...100`. Press `Enter`.
- Page "Review": the page shows `Band: 5`, `Channel: 36`, the address range, the DHCP range, `Settings source: automatic`, and no passphrase. If your card has no 5 GHz radio, it shows `Band: 2.4`, `Channel: 6`, and the fallback note.
Press `Esc`. Expected: header says `The wizard is cancelled. The settings were not applied.` Press `Ctrl+q`.

3. Increased compatibility: repeat step 2, but on page 6 press `Down` then `Enter`. Expected: Review shows `Band: 2.4` and `Channel: 6`.

4. Advanced setup: repeat step 2 up to page 7. Press `a`. Expected: page "Advanced setup" with Band, Channel, Address range, DHCP, DHCP start, DHCP end. Press `Tab` twice to the address field. Clear it with `Backspace`. Type `192.168.42.5/24`. Press `Enter`. Expected: an error that the address range must be a network address, and the page stays. Fix the value to `10.20.30.0/24`. Tab to DHCP start. Type `10.20.30.1` for start, `10.20.30.20` for end. Press `Enter`. Expected: an error that the DHCP range must not include the hotspot address `10.20.30.1`. Change start to `10.20.30.50` and end to `10.20.30.90`. Press `Enter`. Expected: Review shows `Address range: 10.20.30.0/24`, `DHCP: on 10.20.30.50-10.20.30.90`, and `Settings source: set by the user (advanced setup)`.

5. Selection box check for open security: on page 3 choose `Open` (Up once, Enter). Page 4: leave the passphrase empty, press Enter. On page 5 choose an upstream interface. On Review press `Enter`. Expected: the warning `Every device in radio range can use the upstream network.` and the text to press `y`. (Do not press `y` unless you want to start an open hotspot.)

6. Apply (root, real card). From step 2 with a WPA2 hotspot, press `Enter` on Review.
```
sudo XDG_CONFIG_HOME="$XDG_CONFIG_HOME" build/hotmon
```
Expected: the status view shows the SSID, the interface, and the backend name. The profile file `$XDG_CONFIG_HOME/hotmon/profile.json` exists with mode `600`. Check with `stat -c '%a' "$XDG_CONFIG_HOME/hotmon/profile.json"` (expected `600`). A phone sees the network `Cafe Guest`.

7. Monitor and capture (root, after step 6). Press `m`. Expected: the monitor lists connected devices with a graph. Press `c`. Expected: the capture warning. Press `Enter`. Expected: packet summaries such as `66 B aa:bb:cc:dd:ee:ff -> 11:22:33:44:55:66 TCP`. Press `z` to stop capture. Press `k` to stop the hotspot. Press `q`.

8. Profile permissions check (no root):
```
stat -c '%a' "$XDG_CONFIG_HOME/hotmon"
```
Expected: `700` (after a save in step 6).

9. Cutover check:
```
git ls-files | grep -E '\.rs$|Cargo' ; echo "exit=$?"
```
Expected: no file listed and `exit=1`.

## 7. Risks and open assumptions

Assumptions (the planner chose these because the prompt and `ROADMAP.md` do not say):

- A1. C++ standard is C++23 (for `std::expected`). Both installed compilers support it.
- A2. The terminal library is ncurses (installed). Other options were FTXUI (not installed, needs network) and a custom ANSI renderer.
- A3. JSON uses `yyjson` (installed). `nlohmann_json` is not installed. Output indent can differ from serde. Loading stays strict like serde.
- A4. The roadmap names "the network page" for the advanced entry. The plan maps it to the page "Address range and DHCP" (page 7). One "Advanced setup" page holds band, channel, address, DHCP flag, DHCP start, and DHCP end. The roadmap lists the advanced band and channel together with the advanced address values.
- A5. The upstream page moves before the band and network pages. The automatic address needs the upstream choice. This changes the page order of the old wizard.
- A6. The upstream box opens on `None` (the safe choice). The roadmap does not name a default.
- A7. The Passphrase page stays in the wizard for Open security. It accepts only an empty value. This keeps today's behavior.
- A8. The default network is `192.168.42.0/24` (the current example). If it overlaps a local network, the plan tries the other candidate networks in the order of Step 7. DHCP range is host `.10` to `.100`.
- A9. The access-point interface's own addresses are ignored when the wizard checks for overlap. This keeps the choice stable when the hotspot already runs.
- A10. A channel is "allowed" for an interface when `iw phy <phy> info` lists it and does not mark it `disabled` or `no IR`, and the program accepts it for the band (Rust `channel_allowed`).
- A11. The 5 GHz default needs channel 36 in the interface list. If channel 36 is not allowed, the wizard falls back to 2.4 GHz on channel 6 and says so.

Risks:

- R1. `iw phy info` text can differ between `iw` versions. Mitigation: `parse_phy_info` is a pure function with fixture tests. Add a fixture from the real `iw phy phy0 info` output of this machine if a Wi-Fi card is present.
- R2. If `iw` is missing or fails, `supports_ap` is false for every interface (today's behavior). The wizard then lists no AP candidates. The user sees the message `No interface can start an access point.` This is the same outcome as the Rust program (the Rust program rejects the interface).
- R3. DFS (`radar detection`) channels are kept in the advanced list. Some hostapd setups need extra time on these channels. The default channel 36 is not a DFS channel.
- R4. The port is large (about 5,900 Rust lines). The backend rollback logic has many edge cases. Mitigation: port function by function, port all 26 backend tests first, and compare builder output with the Rust output for `sample_profile()`. Do not "simplify" rollback.
- R5. ncurses and Unicode: the Sparkline characters need a UTF-8 locale. If `setlocale` gives a non-UTF-8 locale, fall back to `_` and `#` characters. Check with `nl_langinfo(CODESET)`.
- R6. `Ctrl+q` can be caught by terminal flow control. `raw()` mode avoids this. Check it in the manual script.
- R7. Rust `Ipv4Addr` parsing is stricter than `inet_pton` on some inputs. Add tests for `01.2.3.4`, `1.2.3`, `1.2.3.4.5`, and `256.1.1.1`. All must fail.
- R8. Root-only paths (`/run/hotmon`, `/etc/hostapd`, `/proc/sys/net/ipv4/conf`) are not covered by unit tests, except through the `Paths` override. Manual steps 6 and 7 cover them on a real machine.
- R9. The roadmap does not say how a loaded profile should interact with the selection boxes. Plan section 8.5 defines it (source `Saved`, an extra "Saved setting" choice). Review this rule if the user wants a different behavior.
- R10. The old order of wizard pages changes (A5). Users who know the Rust wizard see the upstream page earlier.
