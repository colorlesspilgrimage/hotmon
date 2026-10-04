# hotmon

hotmon is a text interface for a Wi-Fi hotspot on Linux.
The program configures the hotspot.
The program manages the hotspot.
The program monitors traffic on the hotspot.
The program stores a profile on disk.
The program loads that profile on the next start.

## Build

Install cmake, gtest, yyjson, ncurses, and pkgconf.
Run these commands in the repository:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The program file is `build/hotmon`.
The wizard works without root.
Use root to start the hotspot:

```
sudo build/hotmon
```

## Wizard

The wizard picks the band, the channel, the address range, and the DHCP range.
The default band is 5 GHz on channel 36.
Increased compatibility uses 2.4 GHz on channel 6.
Press `a` on the network page to open advanced setup.
Advanced setup is optional.
The user does not type the security mode, the band, or the channel.

## Privilege requirements

The program needs root, or the CAP_NET_ADMIN capability, to start the hotspot.
The program needs root, or the CAP_NET_ADMIN capability, to stop the hotspot.
Packet capture needs root, or the CAP_NET_RAW capability.
The program captures frames only on the hotspot interface.
The program does not change packet contents.
The program starts capture only after a second confirmation.

## Supported backends

The program detects one backend.
The user does not select the backend.

- The program uses NetworkManager when the NetworkManager service is active.
- The program uses iwd when NetworkManager is not active and the iwd service is active.
- The program uses an existing hostapd setup when NetworkManager and iwd are not active and hostapd is already set up.
- The program uses hostapd, dnsmasq, and nftables when no manager is active.
