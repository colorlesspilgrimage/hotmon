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
sudo build/hotmon
```

The wizard works without root, so you can look around as a normal user. Starting the hotspot needs root or the `CAP_NET_ADMIN` capability.

## The wizard

You move through these pages: interface, SSID, security, passphrase, upstream, band and channel, address and DHCP, then a review page.

- Security is a pick list: Open, WPA2 or WPA3. The default is WPA2.
- A passphrase must be 8 to 63 printable ASCII characters. Leave it empty for an open network.
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

Capture needs root or the `CAP_NET_RAW` capability. It only reads frames on the hotspot interface and never changes them. Pressing `c` shows a warning, and capture starts only after you press Enter to confirm.

## Backends

hotmon picks the backend itself. You do not choose one. It checks in this order and uses the first match:

1. NetworkManager, if its service is active.
2. iwd, if its service is active.
3. An existing hostapd setup, if hostapd is already configured (`/etc/hostapd/hostapd.conf`).
4. hostapd with dnsmasq and nftables, if nothing else is managing Wi-Fi.

## Saved settings

Settings are stored in `$XDG_CONFIG_HOME/hotmon/profile.json`, or `~/.config/hotmon/profile.json` if that variable is not set. The file includes the passphrase in plain text, so hotmon restricts it to its owner. Do not copy it somewhere public.

## Roadmap

See `ROADMAP.md`.
