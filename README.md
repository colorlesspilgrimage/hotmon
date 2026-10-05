# hotmon

hotmon is a terminal program for running a Wi-Fi hotspot on Linux. You set the hotspot up in a short wizard, start it, and watch the traffic on it. It is written in C++23.

## What it does

- Walks you through the settings: which Wi-Fi interface to use, the network name (SSID), the security mode, the passphrase, and an optional upstream interface for internet access.
- Picks the band, channel, address range and DHCP range for you. You can change them on an advanced page if you want to.
- Starts and stops the hotspot using whatever your system already has.
- Shows status and live traffic counters for the hotspot interface.
- Lists connected devices on the status view with current and total bandwidth, up and down.
- Can capture packets on the hotspot interface.
- Saves your settings so the next run starts from them.

## Dependencies

Install these before you start hotmon. The package names are the Arch Linux ones. Other distributions use similar names.

Always needed:

- `ncurses` and `yyjson`, the libraries hotmon links against.
- `polkit`, for `pkexec`. You also need a polkit agent or a terminal for the prompt. Not needed if you start hotmon as root.
- `iw`, for interface details and the connected-devices list.
- `iproute2`, for `ip`.
- `systemd`, for `systemctl`, which hotmon uses to find the backend.

Needed for the backend hotmon picks (see [Backends](#backends)):

| Backend | Packages |
| --- | --- |
| NetworkManager | `networkmanager`, and `dnsmasq` when DHCP is on |
| iwd | `iwd` |
| Existing hostapd setup | `hostapd`, `nftables`, and `dnsmasq` when DHCP is on |
| hostapd, dnsmasq and nftables | `hostapd`, `nftables`, and `dnsmasq` when DHCP is on |

DHCP is on by default, so plan on installing `dnsmasq`. On Arch with NetworkManager:

```
sudo pacman -S --needed ncurses yyjson polkit iw iproute2 dnsmasq
```

## Build

You need a C++23 compiler, CMake 3.20 or newer, ncurses (wide-character build), yyjson, GoogleTest and pkgconf.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The program ends up at `build/hotmon`.

## Run

```
build/hotmon
```

Start hotmon as a normal user. No `sudo` is needed to open the wizard.
hotmon asks for the admin password or a fingerprint when it starts the hotspot.
It asks again when it stops the hotspot and when it starts a capture.
hotmon needs `pkexec` from the `polkit` package.
A polkit agent or a terminal must be available.
`sudo build/hotmon` still works and does not ask again.

## The wizard

You move through these pages: interface, SSID, security, passphrase, upstream, band and channel, address and DHCP, then a review page. The passphrase page is skipped for an open network.

- Security is a pick list: Open, WPA2 or WPA3. The default is WPA2.
- A passphrase must be 8 to 63 printable ASCII characters.
- The default band is 5 GHz on channel 36. The compatibility choice is 2.4 GHz on channel 6.
- On the address page, press `a` to open the advanced settings. Press `d` to go back to the automatic ones.
- The upstream page starts on None. If you choose an open network and also share an upstream connection, hotmon warns you and asks for a second confirmation before it applies anything.

## Keys while the hotspot is running

| Key | Action |
| --- | --- |
| `m` | Show the traffic monitor |
| `s` or `Esc` | Go back to the status view |
| `w` | Reopen the wizard |
| `c` | Start a packet capture (asks you to confirm first) |
| `z` | Stop the capture |
| `k` | Stop the hotspot |
| `q` | Quit |


## Status view

The status view lists each connected device.
The columns are Device, Address, Down, Up, Total down, and Total up.
Down is data sent to the device.
Up is data received from the device.
The panel title shows the device count and the total speeds.
The UI uses the whole terminal.
A UTF-8 locale uses Unicode lines.
Other locales use plain `+`, `-`, and `|` lines.

## Packet capture

hotmon asks for authorization before capture starts.
Capture only reads frames on the hotspot interface. It never changes those frames.
Press `c` to see a warning. Capture starts after you press Enter.

## Authorization

hotmon asks before it starts the hotspot.
hotmon asks before it stops the hotspot.
hotmon asks before it starts a capture.
If you cancel, hotmon does not apply the action.
At the text prompt, you can push Ctrl+C to cancel. hotmon continues to run.
The notice says that authorization was cancelled.
If the check fails, the notice says that authorization failed.
hotmon never reads, stores, or sees the password.
The system authentication agent does that work.
hotmon starts `pkexec` from a system directory, such as `/usr/bin`.
It does not search `PATH` for `pkexec`.
`pkexec` runs the same hotmon file as root.
If your user can change that file, a program that runs as your user can change it too.
Then it runs as root after you authorize.
For daily use, install hotmon in a directory that only root can change, such as `/usr/local/bin`.

## Backends

hotmon picks the backend itself. You do not choose one. It checks in this order and uses the first match:

1. NetworkManager, if its service is active.
2. iwd, if its service is active.
3. An existing hostapd setup, if hostapd is already configured (`/etc/hostapd/hostapd.conf`).
4. hostapd with dnsmasq and nftables, if nothing else is managing Wi-Fi.

With NetworkManager and DHCP on, NetworkManager runs dnsmasq itself to hand out addresses, so the `dnsmasq` package must be installed. hotmon checks for it before it asks for authorization.

## Saved settings

Settings are stored in `$XDG_CONFIG_HOME/hotmon/profile.json`, or `~/.config/hotmon/profile.json` if that variable is not set. The file includes the passphrase in plain text, so hotmon restricts it to its owner. Do not copy it somewhere public.

## Roadmap

See `ROADMAP.md`.
