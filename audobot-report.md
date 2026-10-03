# Audobot report

The audit uses branch sbuild/plan-wifi-hotspot-tui/audobot.
The commit is 475ba5c.
The audit reads PLAN.md.
The audit sends bad input to the backend.
The audit does not change the program code.
The audit does not start a hotspot.
A crash is a panic or an abort.
The unit tests are 42 tests.
All 42 tests pass.

## Crashes

- No crash occurs.

## Successes

- The function select_backend follows the service order for five fact sets.
- The function service_active returns false for an empty unit name.
- The function service_active returns false for a very long unit name.
- The process does not stop when systemctl rejects that long name.
- The function plan_apply makes a plan for each supported backend with a valid profile.
- The function plan_apply rejects an empty SSID.
- The function plan_apply rejects the SSID values "." and "..".
- The function plan_apply rejects an SSID that contains a slash.
- The function plan_apply rejects an SSID of 33 characters.
- The function plan_apply rejects an SSID that contains a newline or a nul.
- The function plan_apply accepts an SSID of 32 characters.
- The function plan_apply accepts an SSID that contains spaces.
- The function plan_apply accepts an SSID that starts with a hyphen.
- The function plan_apply accepts an SSID that contains a quote.
- The function plan_apply rejects a short passphrase and a passphrase of 64 characters.
- The function plan_apply rejects a passphrase that is not printable ASCII.
- The function plan_apply rejects a passphrase on an open network.
- The function plan_apply accepts an open network with an empty passphrase.
- The function plan_apply rejects channel 0, channel 14, and channel 65535 on the 2.4 GHz band.
- The function plan_apply rejects channel 36 on the 2.4 GHz band.
- The function plan_apply rejects channel 6 on the 5 GHz band.
- The function plan_apply accepts channel 36 on the 5 GHz band.
- The function plan_apply rejects a bad address range.
- The bad ranges include text, a host address, an empty value, and a bad prefix.
- The function plan_apply rejects a DHCP range outside the network.
- The function plan_apply rejects a reversed DHCP range.
- The function plan_apply rejects a DHCP range that contains the gateway.
- The function plan_apply accepts a bad DHCP range when DHCP is off.
- The function plan_apply rejects an empty interface name.
- The function plan_apply rejects an interface name with a space, a digit first, or 16 characters.
- The function plan_apply accepts an interface name of 15 characters.
- The function plan_apply rejects an upstream name that is the same as the access-point name.
- The function plan_apply accepts the upstream name none.
- The function plan_apply accepts the upstream name None.
- The nft text for None contains masquerade.
- The function plan_apply rejects an upstream name that contains a quote or a space.
- The text helpers do not stop on quotes, newlines, or a text of 100000 characters.
- The function plan_stop returns commands for a bad interface name.
- The profile loader rejects empty, truncated, binary, and wrong JSON.
- The profile loader rejects a bad security name and a channel value above 65535.
- A missing profile file gives an empty result.
- The save function rejects a directory path.
- The save function rejects a parent path that is a file.
- The save function stores a valid profile and loads that profile.
- The function read_pid rejects an empty file, 0, a negative value, text, and a missing file.
- The function read_pid accepts the text +42 and the text 00042.
- The function terminate rejects pid 0, pid -1, and the minimum pid value.
- The stop list returns an error for pid 0.
- The function execute_plan returns a clear error when the runner rejects a command.
- The function execute_plan writes direct-backend files in a temp directory.
- The function execute_plan returns an error when the parent path is a file.
- The runner returns an error for a nul in the program name.
- The runner returns an error for a nul in an argument.
- The runner returns an error for an empty program name.
- The runner returns an error when the command false exits.
- The runner returns an error when the program does not exist.
- A profile file with 4000 nested arrays returns an error.
- Short frames, bad IP headers, and 50 odd frames do not stop the summary function.
- Capture rejects an empty interface name and a different interface name.
- Capture returns an error when the frame source fails.
- A raw socket does not open for an empty name, a space, or a nul.
- A missing interface name returns the error "Operation not permitted".
- The wizard rejects bad characters and a cancelled confirm.
- Bad keys in the application state do not stop the process.
- The text interface accepts bad keys in a normal terminal.
- The text interface accepts bad keys in a terminal of 1 by 1.
- The text interface accepts bad keys in a terminal of 0 by 0.
- The text interface accepts a block of non-text bytes.
- The text interface returns exit code 1 when the input is not a terminal.
- That exit is the error "No such device or address".
- The process does not panic.
