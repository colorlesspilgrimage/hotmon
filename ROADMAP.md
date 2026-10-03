# Roadmap

Planned work for hotmon. Each item is not in the program yet.

## 1. Automatic network settings in the wizard

The wizard asks the user to type the band, the channel, the address range, and the DHCP range. The default path fills those values. A closed list of choices is a selection box. Free text stays typed. That free text is the SSID, the passphrase, and, in advanced setup, the address range and the DHCP start and end.

### Band and channel

The wizard page "Band and channel" asks for a band (`2.4` or `5`) and a channel number.

- The default is 5 GHz on channel 36. The user does not type a channel.
- The user can request increased compatibility. That choice uses 2.4 GHz on channel 6.
- When the access-point interface has no 5 GHz radio, the wizard uses 2.4 GHz on channel 6 and says so on the review page.
- Advanced setup can select a specific band and a channel from the channels that band allows. The user does not type the band or the channel. A choice the interface cannot use is not in the box.
- The review page shows the band and the channel before apply.
- Save and apply the same profile fields the wizard already uses.

### Address range and DHCP

The wizard page "Address range and DHCP" asks for four values:

- Address range
- DHCP on or off
- DHCP start
- DHCP end

The default path fills those values. The user does not type an address range or a DHCP range. Typed values stay available as optional advanced setup.

### Default path

- Choose a private address range that does not overlap the upstream network or another local network.
- Turn DHCP on.
- Choose a DHCP range inside that network. The range does not include the gateway.
- Show the chosen values on the review page.
- Save and apply the same profile fields the wizard already uses.

### Advanced setup

- The user opens advanced setup from the network page. The page is optional.
- DHCP on or off is a selection box. The user does not type `on` or `off`.
- The user can set the address range and the DHCP start and end.
- A bad range, a host address, an empty value, a bad prefix, or a DHCP range that contains the gateway is still rejected.
- The review page shows that the user set these values.

### Selection boxes

A value that already has a fixed set of choices is a selection box. The user moves through the choices and confirms one. The user does not type the value.

- Security mode lists Open, WPA2, and WPA3. The page opens with WPA2 selected.
- Increased compatibility is a choice on the band page: 5 GHz, or increased compatibility. The page opens with 5 GHz selected.
- Advanced band lists 2.4 GHz and 5 GHz.
- Advanced channel lists the channels allowed for the selected band.
- DHCP lists on and off.
- The access-point interface lists the interfaces that can start an access point.
- The upstream interface lists the other interfaces and None.

The SSID and the passphrase stay typed. The advanced address range, DHCP start, and DHCP end stay typed.

### Done when

- A new hotspot can be created without a typed band, channel, address range, or DHCP range.
- The default band is 5 GHz on channel 36.
- Increased compatibility uses 2.4 GHz on channel 6.
- Advanced setup is available and is not required.
- A saved profile still loads its stored band, channel, address range, and DHCP values.
- The review page shows the band, the channel, the address range, and the DHCP range before apply.
- Security, DHCP, band, channel, the access-point interface, and the upstream interface are selection boxes.
- The user cannot type a security mode.
