# PLAN: FakeMii mode for hotmon

## 1. Goal and non-goals

### Feature prompt (exact)

Add a FakeMii mode to hotmon: a toggleable fake Nintendo connection-test server for a running hotspot, with a hotkey and an instruction popup. Full design is in the task text.

### Goal

hotmon gets a FakeMii mode. FakeMii fakes the Nintendo 3DS connection test. A 3DS can then use a hotspot that has no internet.

How the 3DS works: the user sets the 3DS to use an HTTP proxy at `<gateway-ip>:3000`. The 3DS sends `GET http://conntest.nintendowifi.net/ HTTP/1.1`. FakeMii answers with a small HTML page and the headers `Server: BigIP` and `X-Organization: Nintendo`.

The reference is the abandoned Node.js tool in `/home/samh/Work/FakeMii` (files `FakeMii.js`, `conntest.html`). hotmon does not use Node. hotmon does not read that directory at run time.

Required behavior:

1. New module `src/fakemii.hpp` and `src/fakemii.cpp`. It is a native C++ HTTP server.
   - A pure function turns raw request bytes into response bytes.
   - A request for `http://conntest.nintendowifi.net/` (absolute-URI form) returns the conntest page with the two headers.
   - EVERYTHING else returns 404. The original has a bug: `indexOf("launcher")` is truthy when the text is absent, so nearly every request got the launcher page. Do NOT copy this.
   - The page body is embedded in the binary. No file reads at run time.
   - Do NOT include `launcher.html` or any exploit or launcher payload. Never.
2. Sockets are non-blocking. They are polled from the existing UI tick (`App::tick`, called when `timeout(200)` expires in `run_ui`). No thread.
   - Bind to the hotspot gateway IP only (from `Profile::network()` then `Ipv4Network::gateway()`). Port 3000. Never `0.0.0.0`.
   - Limits: request size cap, per-connection timeout, connection cap. Close after each reply.
   - Never forward anything. FakeMii is not an open proxy.
   - It runs unprivileged (port above 1024). No extra `pkexec` prompt.
3. Hotkey `f` toggles the service in the status view, only while the hotspot runs.
   - Stop the service when the hotspot stops, when the wizard re-applies settings, and on quit.
   - A bind failure (for example port in use) shows a clear notice.
   - The state is not saved in the profile. It lasts one session.
4. A popup is drawn with the existing box helpers in `src/render.cpp` while the service is on. It shows: SSID, proxy address, the 3DS steps, live evidence (requests served, last requested host and path, `conntest served`), the upstream-None note, and the firewall note. On short terminals it hides. The Keys box always stays on screen. The Keys box gets `f: FakeMii`.
5. Firewall: `nft_text` in `src/backend_text.cpp` always adds an accept rule for TCP port 3000 to the gateway. No second privileged action.
6. Tests and README section (see sections 4 and 7).

### Non-goals

- No launcher page. No exploit payload. No other HTML page.
- No forwarding. No CONNECT tunnel. No real proxy behavior.
- No threads. No `epoll` loop of its own. No new dependency.
- No saving of the FakeMii state in the profile. No change to `Profile` JSON.
- No new privileged operation. No change to `src/privilege.cpp`.
- No claim that other consoles work. The text says "3DS" only.
- No configurable port. The port is the constant 3000.
- Agents cannot test with a real 3DS. This plan says so in section 6.

## 2. Repo context

- Repo: `/home/samh/Work/hotmon`. Branch: `feat/add-a-fakemii-mode-to-hotmon--a-toggleab-1005-1656`. Main branch: `main`.
- Stack: C++23, CMake 3.20+, ncurses (wide), yyjson, GoogleTest. Linux only.
- Entry point: `src/main.cpp` builds `App::boot()` then calls `run_ui(app, privileged)` in `src/terminal.cpp`.
- Build and test:
  ```
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
  ctest --test-dir build --output-on-failure
  ```
  Direct test run: `build/hotmon_tests --gtest_filter='FakeMii*'`.
- `CMakeLists.txt` uses `file(GLOB ... CONFIGURE_DEPENDS)` for `src/*.cpp` and `tests/*.cpp`. New files need NO CMake edit. Re-run the cmake configure command once after adding files.
- Warnings are errors: `-Wall -Wextra -Wpedantic -Werror`. Code must be warning-free.
- Run command: `build/hotmon`. It needs a real wireless card and admin rights for the hotspot. Agents cannot run it.
- Conventions (match them):
  - Namespace `hotmon`. Header guard `#pragma once`. 2-space indent. Include the module header first in its `.cpp`.
  - Errors use `Result<T>` (`std::expected<T, std::string>`) and `unexpected_text("...")` from `src/result.hpp`.
  - User-facing and code text uses ASD-STE100: short sentences, active voice. Example notice: `"The hotspot is not active."`
  - File descriptors use `FileDescriptor` from `src/process.hpp` (move-only, closes on destruction).
  - Raw sockets exist in `src/capture_socket.cpp`. Copy its include style.
- Key files to read before work:
  - `src/app.hpp`, `src/app.cpp`: `App` struct. `App::on_run_key` handles keys. `App::tick(Runner&)` runs each UI tick and calls `capture.poll()`. `App::finish_stop()` and `App::fail_apply()` reset state. `App::apply_hotspot()` calls `privileged.apply(...)`.
  - `src/render.cpp`: helpers `append_box`, `box_chars`, `box_top`, `box_row`, `wrap_text`, `printable_text`, `printable`, `footer_text`, `status_lines`, `footer_box`, `render_styled`. `render_styled` sets `stretch = height >= 12`. `status_lines` hides the Devices panel when `device_rows < 3`. `render_styled` cuts the body so the Keys box stays on the last rows when `stretch` is true. A final pass applies `printable_text` to every row.
  - `src/backend_text.cpp`: `nft_text`. The `input` chain accepts udp 67, accepts DNS to the gateway only when a gateway is known, then `ct state new drop` on the AP interface.
  - `src/profile.hpp`: `Profile::network()` returns `Result<Ipv4Network>`. `Ipv4Network::gateway()` returns `Result<std::string>` (first host address, for example `192.168.42.1`). `parse_ipv4`, `format_ipv4`.
  - `src/terminal.cpp`: `run_ui` loop. `Step::Quit` returns 0.
  - Tests: `tests/app_test.cpp` (helpers `loaded_app`, `key_char`, `apply_direct`, `stop_direct`), `tests/render_test.cpp` (helpers `running_status`, `plain_app`, `screen`, `joined`, `has_row_with`, `column_count`), `tests/backend_test.cpp` (`FirewallLimitsHotspotForwardAndInput`), `tests/test_support.hpp` (`sample_profile()` has address `192.168.42.0/24`, ssid `Hotmon`, upstream `eth0`, interface `wlan0`).
- Used keys today: `q c m s w k z`, `Enter`, `Esc`, `Ctrl+q`. Key `f` is free.
- `App` is move-only (it holds `CaptureControl` with a `unique_ptr`). A new `FakeMii` member must be move-constructible. `App::from_parts` returns `App` by value.

## 3. Implementation steps

Do the steps in this order.

### Step 1. `src/fakemii.hpp` and `src/fakemii.cpp`

Constants (in `fakemii.hpp`):

```
inline constexpr uint16_t FAKEMII_PORT = 3000;
inline constexpr size_t FAKEMII_MAX_REQUEST = 8192;      // bytes
inline constexpr size_t FAKEMII_MAX_CONNECTIONS = 8;
inline constexpr std::chrono::seconds FAKEMII_TIMEOUT{5};  // per connection, from accept
inline constexpr size_t FAKEMII_TARGET_MAX = 80;         // stored bytes of host/path
```

Pure handler API (no sockets, no clock, no globals):

```
struct FakeMiiReply {
  std::string bytes;      // full HTTP response, ready to send
  bool conntest = false;  // true only for the conntest page
  std::string target;     // "host/path" for display, at most FAKEMII_TARGET_MAX bytes, raw (NOT sanitized)
};
bool fakemii_request_complete(std::string_view bytes);   // true when "\r\n\r\n" is present
FakeMiiReply fakemii_respond(std::string_view request);  // never throws, accepts any bytes
```

Handler rules:

1. Parse only the request line (first line up to `\r\n`, or up to `\n`). Split on single spaces into method, target, version.
2. `conntest` is true only when ALL of these hold: method is exactly `GET`; version starts with `HTTP/1.`; target is the absolute-URI form `http://` + host + `/`; host equals `conntest.nintendowifi.net` (compare case-insensitively, an optional `:80` suffix is allowed); the path is exactly `/` (no query, no extra text).
3. In that case return `200 OK` with exactly these bytes (headers in this order, CRLF line ends, then a blank line, then the body):
   ```
   HTTP/1.1 200 OK\r\n
   Content-Type: text/html\r\n
   Content-Length: <body size>\r\n
   Connection: close\r\n
   Server: BigIP\r\n
   X-Organization: Nintendo\r\n
   \r\n
   <body>
   ```
   (no line breaks between header lines in the real bytes except `\r\n`).
4. EVERYTHING else returns this exact response, including origin-form `GET /`, `GET /launcher`, `POST`, `CONNECT host:443`, other hosts, empty input, binary garbage, and a request over `FAKEMII_MAX_REQUEST` bytes:
   ```
   HTTP/1.1 404 Not Found\r\n
   Content-Type: text/plain\r\n
   Content-Length: 14\r\n
   Connection: close\r\n
   \r\n
   404 Not Found\n
   ```
   The 404 response has NO `Server` and NO `X-Organization` header. The handler never makes a network call.
5. Body: embed the text of `/home/samh/Work/FakeMii/conntest.html` as a `constexpr std::string_view` (raw string literal) in `fakemii.cpp`. Copy it byte for byte (XHTML doctype, title `HTML Page`, body text `This is test.html page`). Do not read the file at run time. Do not add `launcher.html` content.
6. `target` for display: for the absolute-URI form, `host` + `path` (for example `conntest.nintendowifi.net/`). For other well-formed request lines, use the `Host:` header value if present, then the path (origin form), else the path alone. Cut at `FAKEMII_TARGET_MAX` bytes. For an unreadable request line, use `(invalid request)`. Do not sanitize here. `render.cpp` sanitizes.

Server class:

```
class FakeMii {
 public:
  Result<void> start(std::string_view bind_ip, uint16_t port);  // port 0 = ephemeral, for tests
  void stop();                       // close listener and all clients; reset counters; idempotent
  bool running() const;
  uint16_t port() const;             // bound port (real port after port 0)
  const std::string& address() const;// bound IP text
  void poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  size_t served() const;             // replies sent, 200 and 404
  const std::string& last_target() const;  // raw, empty before the first reply
  bool conntest_served() const;      // true after one 200 conntest reply, until stop()
  // move-only; destructor calls stop()
};
```

`start` rules:
- Parse `bind_ip` with `parse_ipv4` (from `profile.hpp`). Reject an unparsable address. Reject `0.0.0.0` with an error. NEVER bind `INADDR_ANY`.
- `start` while running: call `stop()` first (rebind).
- `socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)`, `SO_REUSEADDR`, `bind` to that address only, `listen(fd, 8)`.
- On failure, close the socket and return a clear error. Map errno: `EADDRINUSE` gives `"Port 3000 is already in use on 192.168.42.1."`; `EADDRNOTAVAIL` gives `"The address 192.168.42.1 is not available on this computer."`; `EACCES` and others give `"FakeMii could not listen on 192.168.42.1:3000. <strerror text>."` Use the real port number in the text.
- Keep the object not running after a failure.

`poll` rules (one call must do all steps, so a request needs only one tick):
1. If not running, return.
2. Loop `accept4(listener, ..., SOCK_NONBLOCK | SOCK_CLOEXEC)` until `EAGAIN`. If `FAKEMII_MAX_CONNECTIONS` clients are open, close the new socket at once.
3. For each client: `recv` until `EAGAIN` (non-blocking). Append to its buffer. If `fakemii_request_complete` is true, or the buffer exceeds `FAKEMII_MAX_REQUEST`, call `fakemii_respond` (pass the whole buffer; the oversize case must give 404), queue `reply.bytes` for writing, update `served`, `last_target`, `conntest_served`, and stop reading from this client.
4. For each client with queued bytes: `send(..., MSG_NOSIGNAL)`. Keep the unsent rest for the next poll. When all bytes are sent, `shutdown` and close (close after each reply).
5. Close a client when `recv` returns 0 (peer closed before a full request, no reply is counted) or an error, or when `now - accepted_at > FAKEMII_TIMEOUT`.
6. One request per connection. Ignore any extra bytes after the first request.

No `connect`, no `socket` other than the listener and accepted clients. No outbound traffic of any kind.

### Step 2. `src/app.hpp` and `src/app.cpp`

- Include `fakemii.hpp` in `app.hpp`. Add to `struct App`:
  - `FakeMii fakemii;`
  - `uint16_t fakemii_port = FAKEMII_PORT;` (a test seam: tests set it to 0 for an ephemeral port).
- Add private `Step toggle_fakemii();` and `void stop_fakemii();` (or inline calls to `fakemii.stop()`).
- `on_run_key`: after `capture.dismiss_warning();`, add: `if (key.code == Key::Code::Char && key.ch == U'f' && status_view) { return toggle_fakemii(); }`. In the monitor view, `f` does nothing.
- `toggle_fakemii()`:
  - If `fakemii.running()`: `fakemii.stop()`; `notice = "FakeMii is off."`; return `Step::Continue`.
  - If `!running || !active`: `notice = "The hotspot is not active. Start the hotspot to use FakeMii."`; return.
  - Get `active->network()` then `gateway()`. On error: `notice = "FakeMii needs a gateway address. " + error`; return.
  - Call `fakemii.start(*gateway, fakemii_port)`. On error: `notice = error` (the clear message from step 1). On success: `notice = "FakeMii is on. Proxy: <ip>:<port>."`.
  - Never write to the profile. Never call `Privileged`.
- `App::tick`: call `fakemii.poll()` at the start of the function (before the `running` block). It is a no-op when off.
- Stop FakeMii in all these places:
  - `finish_stop()` (hotspot stop; also the `!active` path of `stop_hotspot`). Set the notice after the stop as today.
  - `fail_apply()`.
  - `apply_hotspot()`: call `fakemii.stop()` right before `privileged.apply(backend, *profile)` (the gateway may change). After a successful apply, if FakeMii was on before, make the notice `"The hotspot is active. FakeMii is off. Press f to turn it on again."` (keep the capture warning notice logic working: the capture warning text has priority if `interface_changed`). Do not stop FakeMii when apply is rejected before this point (cancelled wizard, open-upstream warning, missing tools).
  - Quit: in `run_ui` (`src/terminal.cpp`) call `app.fakemii.stop()` in the `Step::Quit` case before `return 0`. The `FakeMii` destructor also closes sockets.
- The wizard view must not react to `f` (text fields use it). Only `on_run_key` with `status_view == true` handles it.

### Step 3. `src/backend_text.cpp` — `nft_text`

- In the `if (auto gateway = network->gateway())` block, append to the `dns` string one more rule:
  `    iifname "<ap>" ip daddr <gateway> tcp dport 3000 accept\n`
  Use `FAKEMII_PORT` from `fakemii.hpp` in the `std::format` call (not a literal), or a literal 3000 with a comment. Prefer the constant.
- The rule goes before `iifname "<ap>" ct state new drop`. It is ALWAYS present when a gateway is known, for both upstream modes. Rename the local variable `dns` to `allow` (or similar) if it helps clarity.
- Do not change anything else in `nft_text`. No second privileged operation.
- NetworkManager and iwd backends do not manage the firewall. Step 5 and the README say a host firewall may block port 3000.

### Step 4. `src/render.cpp`

Add the popup as a box in the status view, built with `append_box`.

- New file-local function `fakemii_popup(const App& app, int width, const BoxChars& box)` returning `std::vector<StyledRow>`. Title: `FakeMii (3DS)`. Use `append_box(lines, box, title, rows, width, true, 0)` (wrap on). Rows (style in brackets):
  1. `FakeMii is on. SSID: <app.active->ssid>` [Good] (use `app.status.ssid` if `active` is empty).
  2. `Proxy: <app.fakemii.address()>:<app.fakemii.port()>` [Accent]
  3. `Requests served: <N>` [Plain]
  4. `Last request: <last_target>` or `Last request: none yet` [Plain]. Sanitize the target with `printable_text` before use (the final pass also sanitizes).
  5. `conntest served` [Good], only when `app.fakemii.conntest_served()`.
  6. A blank row.
  7. `On the 3DS:` [Title], then the steps as separate rows [Plain]:
     `1. Internet Settings > Connection Settings > pick this connection > Change Settings.`
     `2. Proxy Settings: Yes > Detailed Setup.`
     `3. Proxy server <ip>, Port 3000 > save.`
     `4. Test Connection.`
  8. A blank row.
  9. `FakeMii mainly helps when the hotspot has no upstream (upstream None). With an upstream connection, the console reaches the real test server anyway.` [Dim]
  10. For `app.backend != BackendKind::DirectHostapd` only: `hotmon does not manage the firewall for this backend. A host firewall (such as ufw) may block port 3000.` [Dim]
  Use the real port value in rows 2 and 3 of the steps text (`Port <port>`).
- Show rule, inside `status_lines`:
  - The popup shows only when `app.fakemii.running()` AND `stretch` is true AND the popup fits: `status box lines + popup lines <= body_rows`. The Devices panel then uses what remains and keeps its own rule (`device_rows < 3` hides it).
  - If FakeMii is on but the popup does not fit (height below 12, or too few rows), hide the popup. Add one row to the Status box: `FakeMii is on: <ip>:<port>. Make the terminal taller to see the steps.` [Good]. Compute the Status box twice if needed (first without the hint to test the fit).
  - The Keys box stays on screen at heights of 12 and more (the existing cut logic in `render_styled`). Do not change that logic.
- `footer_text`, status view: add `f: FakeMii` after `m: monitor`. New text: `m: monitor  c: capture  z: stop capture  k: stop hotspot  f: FakeMii  w: wizard  q: quit`. Monitor and wizard footers do not change. Update any existing test that matches the old status footer text (search `tests/render_test.cpp` for `FooterMatchesThePageKind` and `FooterWrapsAtWordBoundaries`).
- Also show `f: FakeMii` only in the status footer. Do not show it when the hotspot is stopped? Keep it simple: it shows in the status footer always (matches the existing `c`, `z` behavior). A press while stopped gives the notice from Step 2.

### Step 5. Docs

- `README.md`: add a `## FakeMii` section and the `f` key row. See section 7 of this plan for the exact text rules.
- `ROADMAP.md`: move the `### FakeMii integration` entry from `## Planned` to `## Done` as `### 4. FakeMii mode` with a one-paragraph description (toggle with `f`, 3DS only, popup with steps). Keep the style of the other Done entries. Keep the reference link line. If `## Planned` becomes empty, keep the heading with no entries.

## 4. Tests to write

All tests use GoogleTest, `namespace { using namespace hotmon; ... }`, like the existing files. Run with `build/hotmon_tests`.

### `tests/fakemii_test.cpp` (new): handler tests (no sockets)

Test suite name `FakeMii`.

| Test | Input | Expected |
| --- | --- | --- |
| `ConntestAbsoluteUriGetsThePage` | `GET http://conntest.nintendowifi.net/ HTTP/1.1\r\nHost: conntest.nintendowifi.net\r\n\r\n` | `reply.conntest == true`; starts with `HTTP/1.1 200 OK\r\n`; contains `Server: BigIP\r\n` and `X-Organization: Nintendo\r\n`; body (after `\r\n\r\n`) equals the embedded page; `Content-Length` equals the body size; contains `Connection: close\r\n`; `target == "conntest.nintendowifi.net/"` |
| `ConntestHeadersAreExact` | same | the full header block equals the exact string from step 1 rule 3 |
| `ConntestHostIsCaseInsensitive` | `GET http://CONNTEST.NintendoWiFi.net/ HTTP/1.0\r\n\r\n` | conntest true |
| `OriginFormGetsNotFound` | `GET / HTTP/1.1\r\nHost: conntest.nintendowifi.net\r\n\r\n` | 404, conntest false, no `BigIP`, no `Nintendo`; target is `conntest.nintendowifi.net/` |
| `OtherPathsGetNotFound` | `GET http://conntest.nintendowifi.net/launcher`, `GET http://conntest.nintendowifi.net/x`, `GET http://example.com/`, `GET http://conntest.nintendowifi.net/?a=1`, `GET http://conntest.nintendowifi.net.evil.com/` | each is exactly the 404 bytes from step 1 rule 4 (this pins the `indexOf` bug as fixed) |
| `OtherMethodsGetNotFound` | `POST http://conntest.nintendowifi.net/ HTTP/1.1`, `CONNECT example.com:443 HTTP/1.1`, `HEAD ...` | 404 |
| `GarbageGetsNotFound` | empty string, `\x00\xff\xfe`, `GET`, `\r\n\r\n`, a long line with no spaces | 404 with target `(invalid request)` where the request line is unreadable; no crash |
| `OversizedRequestGetsNotFound` | valid conntest line followed by a header of 9000 bytes | 404 (never 200) |
| `RequestCompleteNeedsBlankLine` | `GET / HTTP/1.1\r\n` then with `\r\n` added | false then true |
| `TargetIsCutAtTheLimit` | a path of 500 bytes | `target.size() <= FAKEMII_TARGET_MAX` |
| `TargetKeepsControlBytesRaw` | path containing `\x1b[2J` | target still contains the byte (render sanitizes) |
| `PageHasNoLauncherContent` | embedded body | does not contain `launcher` (case-insensitive) or `<script` |

### `tests/fakemii_test.cpp`: socket tests (loopback, ephemeral port)

Helpers: a blocking client `connect` to `127.0.0.1:<port>`; a loop that calls `server.poll()` every 10 ms until the client has data or 2 s pass; a function that reads until EOF.

| Test | Steps | Expected |
| --- | --- | --- |
| `ServesConntestOverLoopback` | `start("127.0.0.1", 0)`; client sends the conntest request; poll | client reads the full 200 reply, then EOF; `served()==1`; `conntest_served()`; `last_target()` is `conntest.nintendowifi.net/` |
| `ServesNotFoundOverLoopback` | client sends `GET / HTTP/1.1\r\n\r\n` | 404 reply; `served()==1`; `conntest_served()` false |
| `ClosesAfterEachReply` | after the reply, client `recv` returns 0 | pass |
| `RequestSplitAcrossPackets` | send half, poll, send the rest, poll | one 200 reply |
| `OversizedRequestIsRejected` | send 20000 bytes with no blank line | 404 reply or close; no 200; server still serves the next client |
| `SlowClientTimesOut` | connect, send nothing; call `poll(now + FAKEMII_TIMEOUT + 1s)` | server closes: client `recv` returns 0; `served()==0` |
| `ConnectionCapDropsExtraClients` | open `FAKEMII_MAX_CONNECTIONS + 2` idle clients; poll | at most `FAKEMII_MAX_CONNECTIONS` stay open; extra clients get EOF; a later valid request still works after idle ones time out |
| `StopClosesThePort` | `stop()`; try `connect` | connection refused; `running()` false; counters reset |
| `StartTwiceRebinds` | `start` twice | second works, `running()` true |
| `BindFailsWhenPortIsInUse` | start server A on port 0; start server B on A's port at `127.0.0.1` | B returns an error that says the port is in use; B is not running |
| `BindFailsOnAddressNotOnThisComputer` | `start("203.0.113.1", 0)` | error text mentions `203.0.113.1`; not running |
| `RefusesWildcardAndGarbageAddress` | `start("0.0.0.0", 0)`, `start("", 0)`, `start("nope", 0)` | all return errors; not running |
| `BindsOnlyTheGivenAddress` | `start("127.0.0.1", 0)`; read the listener address with `getsockname` (expose through `address()` plus an `ss`-free check: assert `address() == "127.0.0.1"`) | address is exactly `127.0.0.1`, never `0.0.0.0` |
| `DoesNotForward` | request `GET http://127.0.0.1:<other-port>/ HTTP/1.1` where a second listening test socket waits | reply is 404; the second socket gets no connection (`accept` would block; use non-blocking and expect `EAGAIN`) |
| `PollWhenStoppedIsSafe` | `poll()` on a never-started object | no crash |

Add one disabled smoke test (used by the manual check in section 6):

```
TEST(FakeMiiSmoke, DISABLED_ServeOnLoopbackFor30Seconds)
```

It starts the server on `127.0.0.1` port 38080, prints `listening 127.0.0.1:38080` (flush stdout), calls `poll()` every 20 ms for 30 s, then stops. It is skipped in normal runs.

### `tests/backend_test.cpp` (extend `FirewallLimitsHotspotForwardAndInput` or add a new test next to it)

- `nft_text(sample_profile())` contains `iifname "wlan0" ip daddr 192.168.42.1 tcp dport 3000 accept`.
- The same rule exists with upstream `none`.
- The rule appears BEFORE `iifname "wlan0" ct state new drop` (compare `find` positions).
- The rule is not present when no gateway is known. Build a profile with an invalid `address_cidr` (for example `""`) and expect no `dport 3000` text. First read `Profile::network()` to confirm that such a profile gives an error.
- The existing checks stay green (DNS rules, `ct state new drop`, no `policy drop`).
- A profile with a different range (for example `10.5.0.0/24`) gives `ip daddr 10.5.0.1 tcp dport 3000`.

### `tests/app_test.cpp` (extend)

Use a `loopback_app()` helper: `loaded_app(dir)`, then apply with `apply_direct` and a `ScriptedRunner`/`RecordedSignals` as in `ConfirmSavesTheProfileAndShowsStatus`, then set `app.active->address_cidr = "127.0.0.0/8"` (gateway `127.0.0.1`; prefix 8 is accepted by `Ipv4Network::parse`) and `app.fakemii_port = 0`.

| Test | Expected |
| --- | --- |
| `FakeMiiKeyTogglesTheServer` | `on_key('f')` gives `Step::Continue`, `fakemii.running()` true, notice contains `FakeMii is on`; second `f` stops it, notice `FakeMii is off.` |
| `FakeMiiKeyNeedsARunningHotspot` | in a stopped app with `view = Status`, `f` leaves `fakemii.running()` false; notice says the hotspot is not active |
| `FakeMiiKeyIsIgnoredInTheMonitorAndWizardViews` | view Monitor and view Wizard: `f` does not start the server and the wizard fields do not change |
| `FakeMiiStopsWhenTheHotspotStops` | start FakeMii, call `stop_direct`; `fakemii.running()` false; client connect to the old port is refused |
| `FakeMiiStopsWhenTheWizardAppliesAgain` | start FakeMii, then `apply_direct` again (wizard re-apply); `fakemii.running()` false; notice mentions that FakeMii is off |
| `FakeMiiStaysOnWhenApplyIsRejectedEarly` | cancel the wizard, call `apply_hotspot` (rejected before `privileged.apply`); FakeMii stays on |
| `FakeMiiBindFailureShowsANotice` | keep the sample address `192.168.42.0/24` (not on this machine), `f`: `running()` false, notice mentions `192.168.42.1`; AND a second case: occupy the port with another `FakeMii` on `127.0.0.1`, set `app.fakemii_port` to that port, `f` gives a notice that says the port is in use |
| `FakeMiiIsNotSavedInTheProfile` | start FakeMii; the profile file content is byte-identical to the content before (no new JSON key) |
| `FakeMiiTickServesRequests` | start, connect a client to the port, send the conntest request, call `app.tick(runner)` in a loop with a `ScriptedRunner` that returns errors for `iw`/`ip` (or a stub); `fakemii.served() == 1` |
| `FakeMiiNeedsNoPrivilege` | a counting `Privileged` stub (or `ScriptedRunner.calls`) shows no extra call after the `f` toggle |

### `tests/render_test.cpp` (extend)

Use `running_status(...)` then set `app.active` (profile with a known SSID such as `Hotmon`), `app.fakemii` started on `127.0.0.1` port 0 (the popup prints the real address and port, so read them from `app.fakemii`).

| Test | Expected |
| --- | --- |
| `FakeMiiPopupShowsTheInstructions` | at 100x40 the screen contains `FakeMii`, the SSID, `<ip>:<port>`, `Internet Settings`, `Connection Settings`, `Change Settings`, `Proxy Settings`, `Detailed Setup`, `Test Connection`, `upstream None`, and `Requests served: 0`; every row has the exact width (`column_count`) |
| `FakeMiiPopupShowsLiveEvidence` | feed one conntest request to the server (socket helper or a test hook) so `served()==1`; screen contains `Requests served: 1`, `conntest.nintendowifi.net/`, and `conntest served`. Before any request, `conntest served` is absent |
| `FakeMiiPopupSanitizesTheLastRequest` | the last target holds `\x1b[2J\r\b` bytes (send a request with that path); no row of `render(...)` contains a byte below 0x20 or 0x7F; run in the `C` and `C.UTF-8` locales like `ControlCharactersDoNotReachTheScreen` |
| `FakeMiiPopupIsHiddenWhenFakeMiiIsOff` | with the server stopped the screen has no `Proxy:` row and no `FakeMii (3DS)` |
| `FakeMiiPopupHidesOnShortTerminals` | at 80x12 and 80x13 the popup box is absent; the Keys box is on the last rows (last row starts and ends with `+` in the `C` locale; `has_row_with(lines, "q: quit")`); a short hint with the proxy address is shown |
| `FakeMiiPopupAppearsWhenTheTerminalIsTallEnough` | at 80x40 the popup is present; at every height from 12 to 60 the output has exactly `height` rows and the Keys box stays on the last rows (loop test) |
| `FakeMiiNoteForUnmanagedFirewall` | with `app.backend = NetworkManager` the screen contains `may block port 3000`; with `DirectHostapd` it does not |
| `FooterShowsTheFakeMiiKey` | the status footer contains `f: FakeMii`; the monitor footer does not |
| `FakeMiiPopupKeepsTheDevicesPanelRule` | tall terminal with popup and devices: the Devices panel hides when fewer than 3 rows remain, as before; no overflow past the Keys box |

Update any existing footer test that compares the old status footer string.

### Regression

Run the full suite. The existing tests (`FillsTheWholeTerminal`, `KeysBoxStaysOnTheLastRows`, `TinyTerminalStillRenders`) must pass without a change to their expectations (except the footer text).

## 5. Acceptance criteria

- [ ] With the hotspot running, pressing `f` in the status view shows a FakeMii popup and the notice `FakeMii is on. Proxy: <gateway>:3000.`
- [ ] Pressing `f` again hides the popup. The notice says `FakeMii is off.`
- [ ] Pressing `f` with no running hotspot does nothing except a notice. Pressing `f` in the monitor view or wizard does nothing.
- [ ] The Keys box lists `f: FakeMii` in the status view.
- [ ] The popup shows the SSID, the proxy address `<gateway-ip>:3000`, and the 3DS steps (Internet Settings > Connection Settings > pick the connection > Change Settings > Proxy Settings: Yes > Detailed Setup > Proxy server `<ip>`, Port 3000 > save > Test Connection).
- [ ] The popup shows `Requests served: N` and `Last request: <host/path>`. Control characters in the request never reach the screen.
- [ ] After the 3DS (or `curl`) fetches `http://conntest.nintendowifi.net/` through the proxy, the popup shows `conntest served`.
- [ ] The popup says that FakeMii mainly helps when the hotspot has no upstream (upstream None), and that with an upstream connection the console reaches the real test server anyway.
- [ ] The text says "3DS" only. It makes no claim about other consoles.
- [ ] For the NetworkManager and iwd backends, the popup says that a host firewall (such as ufw) may block port 3000.
- [ ] On terminals of 12 or 13 rows (and any size where the popup does not fit) the popup hides, a one-line hint shows, and the Keys box stays on the last rows.
- [ ] `curl -x http://<gateway>:3000 http://conntest.nintendowifi.net/` returns status 200, the headers `Server: BigIP` and `X-Organization: Nintendo`, and the conntest HTML.
- [ ] Any other request through the proxy (other host, other path, `GET /`, `CONNECT`) returns 404. FakeMii never contacts another host.
- [ ] The server listens on the gateway IP only. It is not reachable on `0.0.0.0`, `127.0.0.1` (for a real hotspot), or the upstream interface address.
- [ ] A request with more than 8192 bytes, a slow client (over 5 s), and more than 8 connections do not stop the server or the UI.
- [ ] A bind failure (port in use, address not available) shows a clear notice. FakeMii stays off. hotmon keeps running.
- [ ] FakeMii stops when the hotspot stops (`k`), when the wizard applies new settings, and on quit. The port closes at once.
- [ ] hotmon does not ask for a password when FakeMii is toggled.
- [ ] FakeMii state is not in the saved profile file.
- [ ] The generated nft rules contain `iifname "<ap>" ip daddr <gateway> tcp dport 3000 accept` before the `ct state new drop` rule, for both upstream modes.
- [ ] The README has a FakeMii section. `ROADMAP.md` shows FakeMii as done.
- [ ] `ctest --test-dir build --output-on-failure` passes with no warning (`-Werror`).
- [ ] The repo contains no `launcher` page or exploit payload.

## 6. Manual check script

### A. Agents (no hotspot, no 3DS): loopback smoke test

Run from `/home/samh/Work/hotmon`.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

Start the disabled smoke server in the background (it runs 30 s):

```
build/hotmon_tests --gtest_also_run_disabled_tests --gtest_filter='FakeMiiSmoke.*' &
sleep 1
```

Expected: the output has the line `listening 127.0.0.1:38080`.

Valid conntest request:

```
curl -s -i -x http://127.0.0.1:38080 http://conntest.nintendowifi.net/
```

Expected: first line `HTTP/1.1 200 OK`. Headers include `Server: BigIP`, `X-Organization: Nintendo`, `Content-Type: text/html`, `Connection: close`. Body contains `This is test.html page`.

Other requests (all must give 404):

```
curl -s -i -x http://127.0.0.1:38080 http://conntest.nintendowifi.net/launcher
curl -s -i -x http://127.0.0.1:38080 http://example.com/
curl -s -i http://127.0.0.1:38080/
curl -s -i -X POST -x http://127.0.0.1:38080 http://conntest.nintendowifi.net/
printf 'garbage\r\n\r\n' | nc -q1 127.0.0.1 38080
head -c 20000 /dev/zero | tr '\0' 'a' | nc -q1 127.0.0.1 38080
```

Expected: each prints `HTTP/1.1 404 Not Found` and the body `404 Not Found`. None has `BigIP`. (If `nc` is missing, skip the two `nc` lines and say so in the report.)

Check the bind address:

```
sleep 1; ss -ltn 'sport = :38080'
```

Expected: the local address is `127.0.0.1:38080`, never `0.0.0.0:38080` or `*:38080`.

After the 30 s end: `curl -s -m 2 -x http://127.0.0.1:38080 http://conntest.nintendowifi.net/` fails with a connection refused error (exit code 7).

Terminal render check without a hotspot: the render tests in section 4 cover the popup at 80x12, 80x13, 80x24, and 100x40. Agents must not claim a real 3DS test.

### B. Agents cannot run these: real hotspot and real 3DS

A real 3DS test cannot be run by agents. They have no 3DS, no Wi-Fi card access, and no root rights. The user runs this check.

1. Run `build/hotmon`. Create a hotspot with the wizard. Use upstream `none` (no internet) for the main test. Example values: SSID `Hotmon`, WPA2, passphrase `correct-horse-battery`, address `192.168.42.0/24`, DHCP on.
2. In the status view press `f`. Expected: the popup shows `Proxy: 192.168.42.1:3000` and the 3DS steps. No password prompt. The notice says `FakeMii is on.`
3. From a phone or laptop on the hotspot: `curl -x http://192.168.42.1:3000 http://conntest.nintendowifi.net/`. Expected: HTTP 200 with the headers above. The popup shows `Requests served: 1` and `conntest served`.
4. On the 3DS: Internet Settings > Connection Settings > pick `Hotmon` > Change Settings > Proxy Settings: Yes > Detailed Setup > Proxy server `192.168.42.1`, Port `3000` > save > Test Connection. Expected: the test passes and the popup shows `conntest served`.
5. Press `f`. The popup hides. `ss -ltn 'sport = :3000'` shows no listener.
6. Press `f`, then `k`. The hotspot stops and the port 3000 listener is gone.
7. Press `f`, press `w`, apply the wizard again. FakeMii turns off. Press `f` to turn it on again.
8. With the NetworkManager backend and ufw active: if the 3DS cannot reach the proxy, run `sudo ufw allow in on <ap-interface> to any port 3000 proto tcp`. This step checks the firewall note. The plan does not claim that ufw blocks the port.
9. Bind failure: run `python3 -m http.server 3000 --bind 192.168.42.1` in another terminal, then press `f`. Expected: a notice that port 3000 is in use. Stop the Python server, press `f`, and FakeMii starts.
10. Direct hostapd backend: run `sudo nft list table inet hotmon`. Expected: the input chain has `iifname "wlan0" ip daddr 192.168.42.1 tcp dport 3000 accept` before `ct state new drop`.

## 7. Risks and open assumptions

### README rules (step 5 detail)

The user just rewrote `README.md` in a plain, lowercase, short voice. Match it. Change only what is needed:

- Add one row to the keybinds table, after `m`: `` | `f` | toggle FakeMii (status view, hotspot running) | ``. Keep the table style.
- Add a `## FakeMii` section after `## keybinds`. Use the same lowercase heading style. Content, in short lines:
  - What it is: a fake Nintendo 3DS connection test server for the hotspot, so a 3DS can use a network with no internet. It listens on `<hotspot gateway ip>:3000`, answers only `http://conntest.nintendowifi.net/`, and returns 404 for everything else. It never forwards traffic.
  - Hotkey: `f` in the status view while the hotspot runs. It stops when the hotspot stops, when the wizard applies again, and when hotmon quits. It is not saved in the profile.
  - 3DS steps: Internet Settings > Connection Settings > pick the connection > Change Settings > Proxy Settings: Yes > Detailed Setup > proxy server is the gateway IP, port 3000 > save > Test Connection.
  - Upstream note: it mainly helps when the upstream is `none`. With an upstream connection the console reaches the real test server anyway.
  - Firewall note: with the direct hostapd backend, hotmon opens TCP port 3000 to the gateway in its own nftables table. With NetworkManager or iwd, hotmon does not manage the firewall. A host firewall (such as ufw) may block port 3000.
  - Say "3DS" only. Say nothing about other consoles.
  - Credit: the idea comes from FakeMii (https://github.com/Lectem/FakeMii), as `ROADMAP.md` already links it.
- Do not rewrite other README parts.

### Risks and assumptions

- **No real 3DS test.** The behavior with a real 3DS is unverified. The request format (`GET http://conntest.nintendowifi.net/ HTTP/1.1`) comes from the original `FakeMii.js`. [INFERENCE] A 3DS may send `Connection: keep-alive`. The original server sent `connection: keep-alive` in its reply. This plan closes after each reply (design requirement). If the 3DS test fails, the first thing to try is a keep-alive reply. That is a design change and needs user approval.
- **Handler edge choice.** The design says "everything else returns 404". This plan also uses 404 for garbage and oversized requests (not 400 or 431). It keeps the handler simple and matches the design text. Change it only if the user asks.
- **Optional `:80` in the host.** The plan accepts `conntest.nintendowifi.net:80`. This is a small widening of the exact URL match. It is safe because the host is still exact. Remove it if the user wants byte-exact matching.
- **Tick latency.** The service is polled from `App::tick`. `tick` runs when `get_wch` times out (every 200 ms with no key). Constant key presses can delay replies. A busy `show_wait` (privileged apply) also pauses the loop, but FakeMii is stopped then. Reply latency is up to about 200 ms. This is fine for a connection test. A thread is not needed.
- **Bind address may not exist yet.** For NetworkManager and iwd, the gateway IP comes from the saved profile (`Ipv4Network::gateway()`), which can differ from the real interface address (iwd uses a default address in `iwd_profile`; read `src/backend_text.cpp` lines near `iwd_profile`). If the address is not on the computer, `bind` fails with `EADDRNOTAVAIL` and the user sees the notice. This is the defined behavior (clear notice, no fallback to `0.0.0.0`).
- **Test seam.** `App::fakemii_port` exists only so tests can use an ephemeral port. The popup and notices always print the real bound port. In production it is always 3000.
- **Tests that need loopback sockets.** They need `127.0.0.1` in the sandbox. This is normal on Linux. The test for `203.0.113.1` expects `EADDRNOTAVAIL` (TEST-NET-3 range). If a sandbox has an unusual setup, the test could differ. Check the error text only for the address, not for the errno code.
- **Port 3000 in use during tests.** Tests never bind port 3000. They use port 0 (or 38080 for the disabled smoke test only).
- **Disabled smoke test is test-only code.** It is the only way to run the server without a hotspot, because hotmon has no standalone mode and the design does not add one.
- **Popup height.** At 80 columns the popup needs roughly 20 rows including borders. It shows only when the whole box fits after the Status box. On typical 24-row terminals it may not fit, so the user sees the hint line first. The README and the hint tell the user to enlarge the terminal. If the user wants the popup to take priority over the Status box, that is a layout change to decide later. Tests assert the real behavior, so check the row count while implementing (print `render(app, 80, 24)` once) and adjust the text, not the rule.
- **Firewall.** `nft_text` is used only by the direct hostapd backend. The new rule needs a gateway, like the DNS rule. A previously applied hotspot keeps the old nft table until the user applies again. The user must re-apply the wizard once after updating hotmon for the port 3000 rule to exist. Mention this in the README FakeMii section in one line.
- **Security.** The server accepts untrusted bytes from Wi-Fi clients. It parses only the first line, caps all buffers, and has no file or network access. Never log raw bytes to a file. The UI shows sanitized text only.
