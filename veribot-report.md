# Veribot report

Commit: 14ffc42
Worktree: /home/samh/Work/sbuild-plan-wifi-hotspot-tui-veribot
Result: pass

The checks use `cargo run` in a terminal of 80 columns and 24 rows. The program is hotmon. The screen text is the source of each result.

## Checks

`cargo build` completes with no errors.

The program opens a text interface. The first page is "Access-point interface". The header shows "Wizard" and "NetworkManager". The user does not select the backend.

The wizard has eight pages. Each page has one group of settings. Enter on a valid page opens the next page. Left opens the previous page. The values stay on the page.

The program rejects the inputs below. The page does not change. The header shows the message.

| Step | Input | Result on the screen |
| --- | --- | --- |
| Interface, empty field | Enter | "The interface  is not available." |
| Interface | nope, then Enter | "The interface nope is not available." |
| SSID, empty field | Enter | "The SSID must contain 1 to 32 characters." |
| Security mode | wep, then Enter | "The security mode must be open, wpa2, or wpa3." |
| Passphrase | short, then Enter | "The passphrase must contain 8 to 63 characters." |
| Band 2.4, channel | 36, then Enter | "The channel 36 is not valid for the 2.4 GHz band." |
| Address range | 192.168.42.1/24, then Enter | "The address range must be a network address, for example 192.168.42.0/24." |
| Upstream interface | wlp1s0, then Enter | "The upstream interface must be different from the access-point interface." |

A second entry opens the next page. The accepted values are wlp1s0, Hotmon, wpa2, correct-horse, band 2.4, channel 6, 192.168.42.0/24, DHCP on, 192.168.42.10 to 192.168.42.50, and upstream none.

The review page shows all of those settings. The footer says "Enter: apply". Enter is not pressed. The program does not apply the profile. The program does not create a profile file.

Esc cancels the wizard. The header shows "The wizard is cancelled. The settings were not applied." The status view shows "The hotspot is stopped."

The monitor shows "Capture is stopped.", "No packets.", and "No devices are connected." The key c is pressed while the hotspot is stopped. The header shows "The hotspot is not active." Capture does not start.

Ctrl+q stops the program. The interface wlp1s0 stays up. The SSID stays "Hood House". The type stays managed.

## Issues

No issues.

## Observations

1. Apply is skipped. The only wireless interface is wlp1s0. That interface carries the live network. Enter on the review page would apply the hotspot, so Enter is not pressed. These checks are not done: start the hotspot, save the profile, load the profile, show a connected device, show a live graph, start packet capture, and stop a running hotspot. The skip is a safety stop. It is not a product failure.

2. On the wizard pages, the key line is cut at 80 columns. The line ends with "Ctrl+q: q". The word "quit" is not complete. Ctrl+q still stops the program. The review page key line is complete.

3. An empty interface name shows "The interface  is not available." The message has a blank name. The message does not say that the name is empty. The page does not change, and a valid name is accepted on the next try.
