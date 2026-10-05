# Roadmap

## Planned

## Done

### 1. Automatic network settings in the wizard

The wizard fills the band, the channel, the address range, and the DHCP range.
The user can open advanced setup.
Advanced setup is optional.

### 2. Devices panel and full-terminal UI

The status view lists connected devices and their bandwidth.
The UI fills the terminal and uses solid lines.

### 3. Ask for authorization

hotmon asks for the admin password or a fingerprint before a privileged action.
The actions are start, stop, and packet capture.
hotmon does not read or store the password.

### 4. FakeMii mode

hotmon fakes the Nintendo 3DS connection test on a running hotspot.
Press `f` in the status view to turn it on or off.
A 3DS can then use the hotspot without real internet.
A popup shows the proxy address and the steps on the 3DS.
Reference: https://github.com/Lectem/FakeMii
