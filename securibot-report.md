# Security report: Wi-Fi hotspot TUI

This report gives weaknesses only. It does not give fixes.

The plan is `PLAN.md`. The feature diff is the change from `ee669d6` to `475ba5c` on branch `plan/wifi-hotspot-tui`.

## W1 — The passphrase is exposed

The plan stores the profile on disk and shows every setting on the review page. The passphrase is one of those settings.

`save_profile` in `src/profile.rs` writes `profile.json` with `fs::write`. That call does not set the file mode. `create_dir_all` does not set the directory mode. With a normal umask, the file is readable by other local users. The file contains the passphrase in clear text.

`execute_plan` in `src/backend.rs` writes these files with the same call:

- `hostapd.conf` under `/run/hotmon` or `/etc/hostapd/hostapd.conf`
- the iwd file `/var/lib/iwd/ap/<SSID>.ap`

Those files contain `wpa_passphrase`, `sae_password`, or `Passphrase`. `/run` is searchable by every local user. A mode `0644` file in `/run/hotmon` leaks the passphrase.

`nm_plan` puts the passphrase in the argument list of `nmcli` (`wifi-sec.psk`). Every local user can read that argument list from the process table while `nmcli` runs.

`review_lines` prints `Passphrase:` and the full passphrase. `Wizard::from_profile` opens the review page. At the next start, the saved passphrase is on the screen.

`validate_passphrase` allows `\`. The iwd text writes the passphrase as a raw key-file value. iwd treats `\` as an escape. The access point can use a different passphrase from the passphrase that the user entered. The same value is also written into the hostapd line with no encoding.

The profile is saved before the backend start succeeds. A failed start still leaves the passphrase on disk.

## W2 — The firewall does not limit the hotspot

`nft_text` in `src/backend.rs` builds the direct rules and the existing-hostapd rules.

The forward chain uses `policy accept`. The accept rules do not limit traffic. A packet that does not match a drop rule continues.

When an upstream interface is set:

- There is no drop rule for other traffic from the hotspot interface.
- If forwarding is already enabled, clients can reach other interfaces, not only the selected upstream interface.
- The masquerade rule matches every packet that leaves the upstream interface (`oifname` only). It does not match the hotspot interface. The rule can change the source address of traffic that is not from the hotspot.

When the upstream interface is `none`, the forward chain drops packets from the hotspot interface. It does not drop packets to the host.

There is no input chain. Clients on the hotspot can reach services on the host.

`execute_plan` does not remove the nftables table when a later command fails. `fail_apply` marks the hotspot stopped and does not remove the rules. A failed start can leave the masquerade rule in place.

The plan does not say that these rules must be removed on failure.

## W3 — Stop can kill the wrong process or leave the hotspot active

`stop_hotspot` in `src/app.rs` reads `/run/hotmon/hostapd.pid` and `/run/hotmon/dnsmasq.pid` for every backend. `terminate` sends `SIGTERM` to that process id. The code does not check that the process is `hostapd` or `dnsmasq`. A reused process id can belong to another process. The program runs with root or `CAP_NET_ADMIN`, so the signal is privileged.

For the direct backend, `plan_stop` only deletes the nftables table. The table delete is optional. If the pid files are missing, stop still reports success. `hostapd` and `dnsmasq` can keep running after the status says that the hotspot is stopped. The firewall rules are then gone, and the access point can stay up.

The direct plan starts `dnsmasq` as the same user as `hotmon`. That user is root during a normal privileged start. The DHCP and DNS server is not dropped to an unprivileged user.

## W4 — A system hostapd file is replaced and not restored

When the backend is an existing hostapd setup, `hostapd_plan` writes the hotmon configuration over `/etc/hostapd/hostapd.conf`. That file is outside `/run`. It survives stop and reboot.

`plan_stop` for that backend runs `systemctl stop hostapd` and deletes the nftables table. It does not restore the previous `hostapd.conf`. A later start of the hostapd service can bring the hotmon access point back, including an open network, outside this program.

Stop also leaves the NetworkManager connection `hotmon` and the iwd profile on disk. Those objects contain the passphrase or an open network, and another tool can start them.

## W5 — Packet capture starts on the same key

The plan requires a second confirmation before capture. `request_capture` uses the key `c` for the warning and for the start. The warning text says to confirm again. A second press of `c` is enough.

On a typical terminal, a repeated press is another press event. `ui.rs` ignores the key kind `Repeat`, but a repeated press can still arrive as `Press`. One hold of `c` can pass both steps. Capture then reads frames on the hotspot interface.

## W6 — An open hotspot can share the upstream network

The security page accepts `open`. The upstream page accepts a real interface. The review page shows the two values and does not give a separate warning. Enter applies the profile.

An open SSID with an upstream interface lets every device in radio range use that upstream path. NAT makes the shared use more serious. The plan does not require a distinct confirmation for this pair of settings.
