# Plan: Wi-Fi hotspot TUI

## Feature definition

hotmon is a Rust program with a text user interface. The interface uses ratatui. The user creates a Wi-Fi hotspot with the program. The user configures a Wi-Fi hotspot with the program. The user monitors traffic on a Wi-Fi hotspot with the program.

A wizard collects the hotspot settings. The wizard shows one group of settings on each page. The user confirms the settings. The program applies the settings after that confirmation.

The program detects the network backend on the machine. The program uses the detected backend to configure the hotspot. The user does not select the backend by hand.

The monitor lists each device on the hotspot. The monitor shows a real-time traffic graph for each device. The graph updates while the hotspot is active.

The user can capture packets on the hotspot. The program starts capture only after an explicit user command. The program shows a warning before capture starts. The program captures traffic only on the local hotspot interface. The program shows packet summaries. The program does not change packet contents.

## Implementation steps

1. Create a Rust binary crate with the name hotmon.
2. Add the ratatui dependency.
3. Add the crossterm dependency.
4. Add the application state for the wizard, the status view, and the monitor view.
5. Detect NetworkManager when that service is active.
6. Detect iwd when NetworkManager is not active.
7. Detect an existing hostapd setup when the other backends are not active.
8. Select a direct hostapd, dnsmasq, and nftables path when no manager is active.
9. Read the wireless interfaces from the system.
10. Reject an interface that cannot start an access point.
11. Add a wizard page for the access-point interface.
12. Add a wizard page for the SSID.
13. Add a wizard page for the security mode.
14. Add a wizard page for the passphrase.
15. Add a wizard page for the band and the channel.
16. Add a wizard page for the address range and DHCP.
17. Add a wizard page for the upstream interface.
18. Validate the input on each page before the next page opens.
19. Show a review page with all selected settings.
20. Save the profile on disk after the user confirms the review page.
21. Apply the profile through the detected backend.
22. Show the hotspot status after a successful start.
23. Read the client list from the active backend.
24. Read the byte counters for each client.
25. Draw a real-time graph for each client in the monitor.
26. Draw a total traffic graph for the hotspot.
27. Require a second confirmation before packet capture starts.
28. Capture packets on the hotspot interface with a local capture library.
29. Show a short summary for each captured packet.
30. Stop the capture when the user stops the capture.
31. Stop the hotspot when the user stops the hotspot.
32. Load a saved profile when the program starts again.
33. Write the privilege requirements in the project readme.
34. Write the supported backends in the project readme.

## Definition of done

- The command `cargo build` completes with no errors.
- The program opens a text interface in a terminal.
- The wizard collects every required hotspot setting.
- The user can move back to a previous wizard page.
- The user can cancel the wizard before the program applies the settings.
- The program detects the network backend with no backend flag.
- The program starts a hotspot on one supported backend.
- The program keeps the profile after the user exits.
- The program loads the saved profile on the next start.
- The monitor shows each connected device.
- The monitor shows a live graph for each device.
- The graph changes when the traffic changes.
- Packet capture does not start before the user confirms it.
- The monitor shows packet summaries during capture.
- The user can stop packet capture.
- The user can stop the hotspot from the interface.
- The program reports a clear error when the backend rejects a setting.
- The program does not change packet contents.
