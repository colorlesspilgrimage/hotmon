# SecAud plan: Wi-Fi hotspot TUI

Apply these steps in order. Do not skip a step. Each step limits the work in the later steps.

## 1. Protect the passphrase in memory and on the screen

1. On the passphrase page, show a mask. Do not show the passphrase characters.
2. On the review page, show only that a passphrase is set or that the network is open. Do not show the passphrase.
3. At the next start, do not show the passphrase.
4. Reject a passphrase that contains `\`, `"`, `#`, or a leading or trailing space. Keep the length rule of 8 to 63 printable ASCII characters.
5. Change the review test so that it does not require the passphrase text on the review screen.

## 2. Protect secret files

1. Create the profile directory with mode `0700`.
2. Write the profile with mode `0600`. Do not leave a world-readable file from `fs::write`.
3. Create `/run/hotmon` with mode `0700` and owner root.
4. Write `hostapd.conf` and the iwd profile with mode `0600`.
5. The dnsmasq file does not need the passphrase. Keep it separate from `hostapd.conf`.
6. Save the profile only after the backend start succeeds. If the start fails, do not leave a new profile that contains a passphrase from this attempt.
7. Add a test that checks these modes when the process can set them.

## 3. Keep the passphrase out of the process list

1. Do not pass the passphrase as an argument of `nmcli` or any other program.
2. Give NetworkManager the secret through a root-only file or through the NetworkManager secret API.
3. Remove the temporary secret file after the call, including after a failure.
4. Add a test that the planned argument lists do not contain the passphrase.

## 4. Replace the firewall rules

Do this step before you enable forwarding.

1. Do not use a forward chain that accepts all remaining packets.
2. Accept forward traffic only from the hotspot interface to the selected upstream interface.
3. Accept return traffic only to the hotspot interface, and only when the state is established or related.
4. Drop every other new forward packet from the hotspot interface.
5. When the upstream interface is `none`, drop forward packets from the hotspot interface. Do not add a masquerade rule.
6. Add an input chain that does not drop traffic from other interfaces.
7. From the hotspot interface, accept only DHCP to the host and DNS to the hotspot address.
8. Drop every other new input packet from the hotspot interface.
9. Change the masquerade rule so that it matches the hotspot interface and the upstream interface. Do not match only the upstream interface.
10. Do not use a base-chain policy that drops traffic from interfaces other than the hotspot interface.
11. If NAT needs forwarding, set forwarding only on the hotspot interface and the upstream interface. Record the previous values.
12. Install these rules before `hostapd` and `dnsmasq` start.
13. Update the tests. The upstream test must not accept a masquerade rule that lacks the hotspot interface.

## 5. Roll back a failed start

1. If a command fails after the rules are installed, delete the hotmon nftables table.
2. Stop only the `hostapd` and `dnsmasq` processes that this start created.
3. Restore any system file that this start replaced.
4. Do not report that the hotspot is active.
5. Do not keep the new secret files after the failure, except a profile that step 2 permits.

## 6. Make stop complete and specific

1. Send `SIGTERM` only when `/proc/<pid>/comm` or the process command is `hostapd` or `dnsmasq`, and only for a process that this program started.
2. If the check fails, do not send the signal.
3. Use this signal path only for the direct backend and for a dnsmasq process that this program started. Do not use it for NetworkManager or iwd.
4. If the direct backend cannot stop `hostapd` or `dnsmasq`, report the failure. Do not say that the hotspot is stopped.
5. Delete the hotmon nftables table on every successful stop.
6. Restore the recorded forwarding values on stop.
7. Remove the pid files after a verified stop.

## 7. Do not keep a replaced system configuration

1. Do not write over `/etc/hostapd/hostapd.conf` unless you first copy it to a root-only backup.
2. On stop, restore that backup and remove the backup only after the restore succeeds.
3. If you did not take a backup, do not modify the system file. Use a private file under `/run/hotmon` instead.
4. On stop of a NetworkManager hotspot, delete the `hotmon` connection or mark it so that it cannot start outside this program. Keep the on-disk profile only as step 2 defines.
5. On stop of an iwd hotspot, remove the iwd profile that this program wrote, or keep it with mode `0600` and do not leave it active.

## 8. Drop privileges for dnsmasq

1. Run `dnsmasq` as a user that is not root.
2. Give that user read access to the dnsmasq file only.
3. Do not give that user read access to `hostapd.conf` or to the profile.

## 9. Require a real second confirmation for capture

1. Keep the warning on the first capture request.
2. Do not use the same key to accept that warning.
3. Start capture only after a different key, for example Enter, and only while the warning is still on the screen.
4. Ignore repeated press events for that sequence.
5. If the hotspot interface changes, show the warning again.

## 10. Warn before an open upstream hotspot

1. When security is `open` and the upstream interface is not `none`, show a clear warning on the review page.
2. State that every device in radio range can use the upstream network.
3. Require an extra confirmation that is not the normal Enter on the first view of that warning.
4. Do not apply the profile before that confirmation.

## Definition of done

- The profile directory mode is `0700`, and the profile file mode is `0600`.
- `hostapd.conf` and the iwd profile are mode `0600` and are not readable by other users.
- No program argument list contains the passphrase.
- The review screen and the status screen do not contain the passphrase.
- A passphrase with `\`, `"`, `#`, or a leading or trailing space is rejected.
- Forward traffic from the hotspot interface can go only to the selected upstream interface. Other new forward traffic from that interface is dropped.
- The masquerade rule matches both the hotspot interface and the upstream interface.
- New input from the hotspot interface is limited to DHCP and DNS. Other new input from that interface is dropped.
- Rules for other interfaces are not given a drop policy by this program.
- A failed start removes the hotmon nftables table and does not leave `hostapd` or `dnsmasq` running.
- Stop does not send a signal to a process that is not the `hostapd` or `dnsmasq` process started by this program.
- Stop does not report success while that `hostapd` or `dnsmasq` process is still running.
- After stop, `/etc/hostapd/hostapd.conf` matches the content from before the start, or the program never changed that file.
- `dnsmasq` does not run as root.
- Capture does not start on a second press of the warning key.
- An open network with an upstream interface needs the extra warning and the extra confirmation.
- `cargo test` passes with these checks.
