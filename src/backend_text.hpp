#pragma once

#include "profile.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace hotmon {

std::string nm_keyfile(const Profile& profile);
std::string iwd_profile(const Profile& profile);
std::string hostapd_conf_text(const Profile& profile);
std::string dnsmasq_conf_text(const Profile& profile);
// With no upstream, the hotspot DNS answers the 3DS connection-test name with the gateway.
// A failed lookup can end the 3DS test before the 3DS contacts the FakeMii proxy.
std::string fakemii_dns_text(const Profile& profile);
std::string nft_text(const Profile& profile);

// ufw drops DHCP, DNS, and FakeMii from hotspot clients. An accept in another nftables table
// does not help, so hotmon inserts these iptables rules into ufw's own chain.
inline constexpr const char* UFW_INPUT_CHAIN = "ufw-user-input";
// Rule specs without the chain. "-m socket" skips sockets on 0.0.0.0, as "socket wildcard 0" does.
std::vector<std::vector<std::string>> ufw_hole_rules(std::string_view ap, std::string_view gateway);

}
