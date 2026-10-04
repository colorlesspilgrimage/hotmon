#pragma once

#include "profile.hpp"

#include <string>

namespace hotmon {

std::string nm_keyfile(const Profile& profile);
std::string iwd_profile(const Profile& profile);
std::string hostapd_conf_text(const Profile& profile);
std::string dnsmasq_conf_text(const Profile& profile);
std::string nft_text(const Profile& profile);

}
