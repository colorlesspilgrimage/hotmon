# VERIFY: FakeMii mode

Branch: `feat/add-a-fakemii-mode-to-hotmon--a-toggleab-1005-1656`. Base: `main`.

## Test results

Commands:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

- Build: no warnings, no errors (`-Werror`).
- ctest: `100% tests passed out of 259`. 0 failed.
- 1 test did not run: `FakeMiiSmoke.ServeOnLoopbackFor30Seconds` (disabled by design).

## Criteria checklist

| Criterion | Evidence | Result |
| --- | --- | --- |
| `f` toggles the popup and the notices | `toggle_fakemii` in `src/app.cpp`; tests `FakeMiiKeyTogglesTheServer` pass | Pass |
| `f` with no hotspot: notice only; ignored in monitor and wizard | code `status_view` guard; tests pass | Pass |
| Keys box has `f: FakeMii` | `FooterShowsTheFakeMiiKey` passes | Pass |
| Popup shows SSID, proxy, 3DS steps, evidence, notes | render tests pass | Pass |
| Control bytes do not reach the screen | `FakeMiiPopupSanitizesTheLastRequest` passes | Pass |
| Short terminal: popup hides, hint shows, Keys box stays | render tests pass (loop 12 to 60) | Pass |
| Firewall note for non-hostapd backends | `FakeMiiNoteForUnmanagedFirewall` passes | Pass |
| curl through proxy gets 200 with headers and page | see transcript | Pass |
| All other requests get 404 | see transcript | Pass |
| Bind to the gateway only; refuse 0.0.0.0 | `src/fakemii.cpp` line 183; `ss` shows `127.0.0.1:38080` | Pass |
| Size cap, timeout, connection cap | socket tests pass; 20000-byte request gave 404 | Pass |
| Bind failure shows a notice | `FakeMiiBindFailureShowsANotice` passes | Pass |
| Stops on hotspot stop, re-apply, quit | diff: `finish_stop`, `fail_apply`, `apply_hotspot`, `run_ui` quit | Pass |
| No privilege; `src/privilege.cpp` unchanged | diff is empty; `FakeMiiNeedsNoPrivilege` passes | Pass |
| Not saved in the profile | `FakeMiiIsNotSavedInTheProfile` passes | Pass |
| nft rule before `ct state new drop`, both modes | backend tests pass; diff of `nft_text` | Pass |
| README section and ROADMAP entry | `README.md` has `## FakeMii`; ROADMAP diff present | Pass |
| No launcher page or payload | search for "launcher" in src, README, ROADMAP: no hit. Only the test checks its absence | Pass |
| No `connect` call, no forwarding | search of `src/fakemii.cpp`: no outbound call | Pass |

## Run transcript

Smoke server: `build/hotmon_tests --gtest_also_run_disabled_tests --gtest_filter='FakeMiiSmoke.*'`
(started with `setsid nohup`). Log line: `listening 127.0.0.1:38080`.

`ss -ltn 'sport = :38080'`:
`LISTEN 0 8 127.0.0.1:38080 0.0.0.0:*` (the listen address is 127.0.0.1).

`curl -s -i -x http://127.0.0.1:38080 http://conntest.nintendowifi.net/`:
`HTTP/1.1 200 OK`, `Content-Type: text/html`, `Content-Length: 246`, `Connection: close`,
`Server: BigIP`, `X-Organization: Nintendo`. Body is the XHTML page with `This is test.html page`.

Each of these gave `HTTP/1.1 404 Not Found`, `Content-Length: 14`, body `404 Not Found`, no `BigIP`:
- `.../launcher` through the proxy
- `http://example.com/` through the proxy
- `curl http://127.0.0.1:38080/`
- `-X POST` through the proxy
- `curl -p` CONNECT to `https://example.com/`
- raw socket `garbage\r\n\r\n`
- raw socket 20000 bytes of `a` (`nc` is not installed; Python socket used)

After 30 s: `curl -s -m 2 -x ... ` gave exit code 7 (connection refused).

Note: the first start of the smoke server in a plain background job did not stay up. A retry with `setsid nohup` worked. This is an environment detail, not a code defect.

## Not tested

- A real hotspot and a real 3DS (PLAN section 6B). Agents cannot run them.
- Interactive TUI run of `build/hotmon` (needs a wireless card and admin rights). The render and key paths are covered by unit tests only.

## Defects found

None.

VERDICT: PASS
