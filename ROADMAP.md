# Roadmap

## Planned

### FakeMii integration

Fake the Nintendo 3DS connection test on the hotspot so a 3DS can join without real internet (for FTP, netloader, Input Redirection, and similar local tools).
Reference: https://github.com/Lectem/FakeMii

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
