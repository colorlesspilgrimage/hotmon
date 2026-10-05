# hotmon

terminal UI for creating and managing wireless hotspots, monitoring devices on network, and capturing packets.

## dependencies
### build

- your C++ compiler of choice
- CMake 
- pkg-config
- ncurses
- yyjson
- gtest []

per distro packages below.

| | packages |
| --- | --- |
| Arch | `base-devel cmake pkgconf ncurses yyjson gtest` |
| Debian, Ubuntu | `g++ cmake pkgconf libncurses-dev libyyjson-dev libgtest-dev` |
| Fedora | `gcc-c++ cmake pkgconf-pkg-config ncurses-devel yyjson-devel gtest-devel` |
| openSUSE | `gcc-c++ cmake pkgconf-pkg-config ncurses-devel yyjson-devel gtest` |

### runtime

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


## build

```
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release``
`cmake --build build -j``

to run tests:

`ctest --test-dir build --output-on-failure`
```


## keybinds

| key | |
| --- | --- |
| `m` | traffic |
| `s`, `Esc` | back to status |
| `w` | wizard again |
| `c` | start a capture (asks first) |
| `z` | stop the capture |
| `k` | stop the hotspot |
| `q` | quit |


## backend selection


1. NetworkManager, then
2. iwd, then
3. an existing hostapd config, if `/etc/hostapd/hostapd.conf` then,
4. hostapd, dnsmasq, and nftables, if nothing else is managing wireless.

## settings

all configs are written in $XDG_CONFIG_HOME/hotmon.
