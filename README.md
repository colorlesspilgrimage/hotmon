# hotmon

A small terminal program for running a Wi-Fi hotspot on Linux. You walk through a short wizard, start the hotspot, and watch who's on it. Written in C++23.

## What it does

You pick the Wi-Fi interface, the network name, security (open, WPA2, or WPA3), a passphrase, and optionally an upstream interface if you want clients to reach the internet. Band, channel, and the address and DHCP ranges are filled in for you. There's an advanced page if you want to change them.

Once it's up, hotmon shows status and live traffic on the hotspot interface, including each connected device with current and total bandwidth in both directions. You can also capture packets on that interface. Settings are saved, so the next run starts from where you left off. Starting, stopping, and capturing all go through whatever your system already has (NetworkManager, iwd, or hostapd), not a stack hotmon installs itself.

## Dependencies

Package names below are Arch's. Other distros call them something similar.

You always need:

- `ncurses` and `yyjson`, which hotmon links against
- `polkit`, for `pkexec`. You also need a polkit agent, or a terminal for the prompt. Skip this if you run hotmon as root.
- `iw`, for interface details and the device list
- `iproute2`, for `ip`
- `systemd`, because hotmon uses `systemctl` to figure out which backend is running

Which extra packages you need depends on the backend hotmon picks. See [Backends](#backends).

| Backend | Packages |
| --- | --- |
| NetworkManager | `networkmanager`, and `dnsmasq` when DHCP is on |
| iwd | `iwd` |
| Existing hostapd setup | `hostapd`, `nftables`, and `dnsmasq` when DHCP is on |
| hostapd, dnsmasq and nftables | `hostapd`, `nftables`, and `dnsmasq` when DHCP is on |

DHCP is on by default, so you'll almost certainly want `dnsmasq`. On Arch, if you're using NetworkManager:

```
sudo pacman -S --needed ncurses yyjson polkit iw iproute2 dnsmasq
```

## Build

You need a C++23 compiler, CMake 3.20 or newer, a wide-character build of ncurses, yyjson, GoogleTest, and pkgconf.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The binary is `build/hotmon`.

## Run

```
build/hotmon
```

Open the wizard as your normal user. You don't need `sudo` just to look at the settings. hotmon asks for your admin password (or a fingerprint) when it actually starts the hotspot, and again when it stops it or starts a capture. That prompt comes from `pkexec` in the `polkit` package, so a polkit agent or a terminal has to be around to show it.

`sudo build/hotmon` still works, and it won't ask again.

## The wizard

The pages, in order: interface, SSID, security, passphrase, upstream, band and channel, address and DHCP, then a review page. An open network skips the passphrase page.

- Security is a list: Open, WPA2, or WPA3. WPA2 is the default.
- A passphrase has to be 8 to 63 printable ASCII characters.
- The default band is 5 GHz on channel 36. If you need something older gear can join, use 2.4 GHz on channel 6.
- On the address page, `a` opens the advanced settings and `d` puts the automatic ones back.
- The upstream page starts on None. If you pick an open network and also share an upstream connection, hotmon warns you and asks you to confirm a second time before it applies anything.

## Keys while the hotspot is running

| Key | Action |
| --- | --- |
| `m` | Traffic monitor |
| `s` or `Esc` | Back to the status view |
| `w` | Reopen the wizard |
| `c` | Start a packet capture (asks first) |
| `z` | Stop the capture |
| `k` | Stop the hotspot |
| `q` | Quit |

## Status view

Connected devices show up as a table: Device, Address, Down, Up, Total down, Total up. Down is traffic sent to the device; up is traffic coming from it. The panel title shows how many devices are connected and the combined speeds.

The UI uses the whole terminal. With a UTF-8 locale you get real box-drawing characters. Otherwise it falls back to `+`, `-`, and `|`.

## Packet capture

Press `c` and you'll get a warning. Capture starts after you press Enter, and only after authorization. It only reads frames on the hotspot interface. It doesn't rewrite them.

## Authorization

Starting the hotspot, stopping it, and starting a capture all ask first. Cancel and nothing happens. At the text prompt, Ctrl+C cancels the prompt without quitting hotmon, and the notice says authorization was cancelled. If the check itself fails, the notice says authorization failed.

hotmon never sees the password. The system authentication agent handles that. It starts `pkexec` from a known system path (like `/usr/bin`) instead of searching `PATH`, and `pkexec` re-runs that same hotmon binary as root.

That's worth knowing: if your user can rewrite the binary, anything running as you can rewrite it too, and then it runs as root the next time you authorize. For daily use, put hotmon somewhere only root can change, such as `/usr/local/bin`.

## Backends

You don't pick a backend. hotmon checks in this order and uses the first one that fits:

1. NetworkManager, if that service is active.
2. iwd, if that service is active.
3. An existing hostapd setup, if `/etc/hostapd/hostapd.conf` is already there.
4. hostapd with dnsmasq and nftables, if nothing else is managing Wi-Fi.

With NetworkManager and DHCP on, NetworkManager runs dnsmasq itself to hand out addresses, so the `dnsmasq` package has to be installed. hotmon checks for it before it asks you to authorize.

## Saved settings

Settings live in `$XDG_CONFIG_HOME/hotmon/profile.json`, or `~/.config/hotmon/profile.json` if that variable isn't set. The passphrase is stored in plain text, and the file is restricted to its owner. Don't copy it somewhere public.

## Roadmap

See `ROADMAP.md`.
