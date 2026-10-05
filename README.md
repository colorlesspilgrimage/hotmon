# hotmon

Terminal UI for a Linux Wi-Fi hotspot. Set the name, the password, and whether clients get internet, then see who is connected and how much traffic they're moving. It does not ship its own access-point stack. It drives NetworkManager, iwd, or hostapd, whichever is already there.

C++23, ncurses.

## Dependencies

One set to compile it, another for the tools it shells out to.

### Build

A C++23 compiler, CMake 3.20 or newer, pkg-config, wide-character ncurses, yyjson, and GoogleTest. GoogleTest is only for `ctest`.

| | packages |
| --- | --- |
| Arch | `base-devel cmake pkgconf ncurses yyjson gtest` |
| Debian, Ubuntu | `g++ cmake pkgconf libncurses-dev libyyjson-dev libgtest-dev` |
| Fedora | `gcc-c++ cmake pkgconf-pkg-config ncurses-devel yyjson-devel gtest-devel` |
| openSUSE | `gcc-c++ cmake pkgconf-pkg-config ncurses-devel yyjson-devel gtest` |

`libyyjson-dev` is in Debian 13 and Ubuntu 25.04 and later. It is not in Debian 12 or Ubuntu 24.04. On openSUSE, `yyjson-devel` is in Tumbleweed, not in Leap. Fedora has had it for a while. If your repos don't have it, build [yyjson](https://github.com/ibireme/yyjson) and install that.

### Runtime

| | Arch | Debian / Ubuntu | Fedora | openSUSE |
| --- | --- | --- | --- | --- |
| `pkexec` | `polkit` | `pkexec` | `polkit` | `pkexec` |
| `iw` | `iw` | `iw` | `iw` | `iw` |
| `ip` | `iproute2` | `iproute2` | `iproute` | `iproute2` |
| NetworkManager | `networkmanager` | `network-manager` | `NetworkManager` | `NetworkManager` |
| iwd | `iwd` | `iwd` | `iwd` | `iwd` |
| hostapd | `hostapd` | `hostapd` | `hostapd` | `hostapd` |
| nftables | `nftables` | `nftables` | `nftables` | `nftables` |
| dnsmasq | `dnsmasq` | `dnsmasq` | `dnsmasq` | `dnsmasq` |

You don't install the whole table. `iw`, `ip`, and `pkexec` are for hotmon. The rest are backends, and you only need the one it will use. See [Which backend](#which-backend). DHCP is on unless you turn it off, so install `dnsmasq` too. `systemctl` comes with systemd.

`pkexec` is what asks for your password or fingerprint. A polkit agent (the admin dialog your desktop already uses) shows a GUI. On a bare tty it asks in the terminal. Running hotmon as root skips the prompt. On older Ubuntu, before `pkexec` was split out, that binary came from `policykit-1`.

Arch, assuming NetworkManager:

```
sudo pacman -S --needed base-devel cmake pkgconf ncurses yyjson gtest polkit iw iproute2 dnsmasq
```

Debian, or Ubuntu 25.04 and newer:

```
sudo apt install g++ cmake pkgconf libncurses-dev libyyjson-dev libgtest-dev pkexec iw iproute2 dnsmasq
```

Fedora:

```
sudo dnf install gcc-c++ cmake pkgconf-pkg-config ncurses-devel yyjson-devel gtest-devel polkit iw iproute dnsmasq
```

openSUSE Tumbleweed:

```
sudo zypper install gcc-c++ cmake pkgconf-pkg-config ncurses-devel yyjson-devel gtest pkexec iw iproute2 dnsmasq
```

Those are build deps plus the usual runtime deps. They do not install NetworkManager. Add `networkmanager`, `network-manager`, or `NetworkManager` if you don't already have it, or skip it and use iwd or hostapd.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The binary is `build/hotmon`.

## Running it

```
build/hotmon
```

No sudo to open the wizard. It asks when it starts the hotspot, stops it, or starts a capture.

Wizard order: interface, SSID, security, passphrase, upstream, band and channel, addresses, review. Open networks skip the passphrase. Security is Open, WPA2, or WPA3, default WPA2. A passphrase is 8 to 63 printable ASCII characters. The radio defaults to 5 GHz channel 36; the compatibility choice is 2.4 GHz channel 6. On the address page, `a` opens the manual settings and `d` puts the automatic ones back.

Upstream starts on None. An open network that also shares your upstream connection gets a warning and a second confirmation before anything is applied.

Once the hotspot is up:

| key | |
| --- | --- |
| `m` | traffic |
| `s`, `Esc` | back to status |
| `w` | wizard again |
| `c` | start a capture (asks first) |
| `z` | stop the capture |
| `k` | stop the hotspot |
| `q` | quit |

Status is a table of clients: device, address, down, up, total down, total up. Down is traffic toward the client. The title line has the client count and the combined rate. It uses the whole terminal. A UTF-8 locale gets box drawing; otherwise `+`, `-`, and `|`.

Capture only reads frames on the hotspot interface. `c` shows a warning, Enter starts it.

## Which backend

You don't pick. First match:

1. NetworkManager, if that service is active.
2. iwd, if that service is active.
3. An existing hostapd config, if `/etc/hostapd/hostapd.conf` is already there.
4. hostapd, dnsmasq, and nftables, if nothing else is managing Wi-Fi.

With NetworkManager and DHCP on, NetworkManager runs dnsmasq itself, but the `dnsmasq` package still has to be installed. hotmon checks for it before asking you to authorize.

## Authorization

Start, stop, and capture each ask. Cancel, or hit Ctrl+C at the text prompt, and hotmon does not apply that action. It stays running. The notice says authorization was cancelled, or that it failed if the check itself failed.

hotmon never sees the password. It execs `pkexec` from a fixed system directory such as `/usr/bin`, not from `PATH`, and `pkexec` re-runs that same hotmon binary as root. If your user can overwrite the binary, so can anything else running as you, and the next approved prompt is root. For anything you leave installed, put it somewhere only root can write, like `/usr/local/bin`.

## Settings

`$XDG_CONFIG_HOME/hotmon/profile.json`, or `~/.config/hotmon/profile.json`. The passphrase is plain text in that file. Permissions are owner-only. Don't copy it into a public dotfiles repo.

`ROADMAP.md` is the rest.
