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
| `f` | toggle FakeMii (status view, hotspot running) |
| `s`, `Esc` | back to status |
| `w` | wizard again |
| `c` | start a capture (asks first) |
| `z` | stop the capture |
| `k` | stop the hotspot |
| `q` | quit |

## FakeMii

fake Nintendo 3DS connection test server for the hotspot, so a 3DS can use a network with no internet.

- listens on `<hotspot gateway ip>:3000` only.
- answers only `http://conntest.nintendowifi.net/`. everything else gets 404.
- never forwards traffic. it is not a real proxy.
- press `f` in the status view while the hotspot runs. press `f` again to stop it.
- stops when the hotspot stops, when the wizard applies again, and when hotmon quits.
- not saved in the profile.
- the popup needs a tall terminal. on a short terminal you get a one-line hint.

on the 3DS: Internet Settings > Connection Settings > pick the connection > Change Settings > Proxy Settings: Yes > Detailed Setup > proxy server is the gateway IP, port 3000 > save > Test Connection.

it mainly helps when the upstream is `none`. with an upstream connection the console reaches the real test server anyway.

firewall: with the direct hostapd backend, hotmon opens TCP port 3000 to the gateway in its own nftables table. the rule accepts only a socket that listens on one address, as FakeMii does. other services on `0.0.0.0` port 3000 stay closed to hotspot clients. apply the wizard once after an update to get this rule. with NetworkManager or iwd, hotmon does not manage the firewall. a host firewall (such as ufw) may block port 3000.

idea from FakeMii (https://github.com/Lectem/FakeMii).



## backend selection


1. NetworkManager, then
2. iwd, then
3. an existing hostapd config, if `/etc/hostapd/hostapd.conf` then,
4. hostapd, dnsmasq, and nftables, if nothing else is managing wireless.

## settings

all configs are written in $XDG_CONFIG_HOME/hotmon.
