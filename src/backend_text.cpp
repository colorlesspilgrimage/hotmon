#include "backend_text.hpp"

#include "fakemii.hpp"

#include <format>

namespace hotmon {

std::string nm_keyfile(const Profile& profile) {
  const char* band = profile.band == Band::Band24 ? "bg" : "a";
  const char* method = profile.dhcp_enabled ? "shared" : "manual";
  std::string address = profile.address_cidr;
  if (auto network = profile.network()) {
    if (auto gateway = network->gateway()) {
      address = *gateway + "/" + std::to_string(network->prefix());
    }
  }
  std::string text = std::format(
      "[connection]\nid=hotmon\ntype=wifi\ninterface-name={}\nautoconnect=false\n\n[wifi]\nmode=ap\n"
      "ssid={}\nband={}\nchannel={}\n\n",
      profile.ap_interface, profile.ssid, band, profile.channel);
  if (profile.security == SecurityMode::Wpa2) {
    text += std::format("[wifi-security]\nkey-mgmt=wpa-psk\npsk={}\n\n", profile.passphrase);
  } else if (profile.security == SecurityMode::Wpa3) {
    text += std::format("[wifi-security]\nkey-mgmt=sae\npsk={}\n\n", profile.passphrase);
  }
  text += std::format("[ipv4]\nmethod={}\naddress1={}\n\n[ipv6]\nmethod=disabled\n", method, address);
  return text;
}

std::string hostapd_conf_text(const Profile& profile) {
  const char* hw_mode = profile.band == Band::Band24 ? "g" : "a";
  std::string text = std::format(
      "interface={}\ndriver=nl80211\nssid={}\nhw_mode={}\nchannel={}\nauth_algs=1\n"
      "ignore_broadcast_ssid=0\n",
      profile.ap_interface, profile.ssid, hw_mode, profile.channel);
  if (profile.security == SecurityMode::Wpa2) {
    text += std::format("wpa=2\nwpa_passphrase={}\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n",
                        profile.passphrase);
  } else if (profile.security == SecurityMode::Wpa3) {
    text += std::format(
        "wpa=2\nwpa_passphrase={}\nsae_password={}\nwpa_key_mgmt=SAE\nieee80211w=2\n"
        "rsn_pairwise=CCMP\n",
        profile.passphrase, profile.passphrase);
  }
  return text;
}

std::string dnsmasq_conf_text(const Profile& profile) {
  std::string listen = "0.0.0.0";
  std::string netmask = "255.255.255.0";
  if (auto network = profile.network()) {
    if (auto gateway = network->gateway()) {
      listen = *gateway;
    }
    netmask = network->netmask();
  }
  std::string text = std::format(
      "interface={}\nbind-interfaces\nexcept-interface=lo\nlisten-address={}\n",
      profile.ap_interface, listen);
  if (profile.dhcp_enabled) {
    text += std::format("dhcp-range={},{},{},12h\n", profile.dhcp_start, profile.dhcp_end, netmask);
  } else {
    text += "port=0\n";
  }
  return text;
}

std::string nft_text(const Profile& profile) {
  const std::string& ap = profile.ap_interface;
  std::string allow;
  if (auto network = profile.network()) {
    if (auto gateway = network->gateway()) {
      allow = std::format(
          "    iifname \"{}\" ip daddr {} udp dport 53 accept\n    iifname \"{}\" ip daddr {} tcp "
          "dport 53 accept\n    iifname \"{}\" ip daddr {} tcp dport {} accept\n",
          ap, *gateway, ap, *gateway, ap, *gateway, FAKEMII_PORT);
    }
  }
  const std::string input = std::format(
      "  chain input {{\n    type filter hook input priority 0; policy accept;\n    iifname \"{}\" "
      "udp dport 67 accept\n{}    iifname \"{}\" ct state new drop\n  }}\n",
      ap, allow, ap);
  if (profile.upstream_interface == "none") {
    return std::format(
        "add table inet hotmon\ndelete table inet hotmon\ntable inet hotmon {{\n{}  chain forward "
        "{{\n    type filter hook forward priority 0; policy accept;\n    iifname \"{}\" drop\n  "
        "}}\n}}\n",
        input, ap);
  }
  const std::string& up = profile.upstream_interface;
  return std::format(
      "add table inet hotmon\ndelete table inet hotmon\ntable inet hotmon {{\n{}  chain forward "
      "{{\n    type filter hook forward priority 0; policy accept;\n    iifname \"{}\" oifname "
      "\"{}\" accept\n    iifname \"{}\" oifname \"{}\" ct state established,related accept\n    "
      "iifname \"{}\" drop\n  }}\n  chain postrouting {{\n    type nat hook postrouting priority "
      "100; policy accept;\n    iifname \"{}\" oifname \"{}\" masquerade\n  }}\n}}\n",
      input, ap, up, up, ap, ap, ap, up);
}

std::string iwd_profile(const Profile& profile) {
  std::string address = "192.168.42.1";
  std::string netmask = "255.255.255.0";
  if (auto network = profile.network()) {
    if (auto gateway = network->gateway()) {
      address = *gateway;
    }
    netmask = network->netmask();
  }
  std::string text = std::format("[General]\nChannel={}\n", profile.channel);
  if (profile.security != SecurityMode::Open) {
    text += std::format("\n[Security]\nPassphrase={}\n", profile.passphrase);
  }
  text += std::format("\n[IPv4]\nAddress={}\nGateway={}\nNetmask={}\n", address, address, netmask);
  if (profile.dhcp_enabled) {
    text += std::format("IPRange={},{}\n", profile.dhcp_start, profile.dhcp_end);
  }
  return text;
}

}
