#pragma once

#include "iface.hpp"
#include "profile.hpp"
#include "result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hotmon {

struct Ipv4Range {
  uint32_t base = 0;
  uint8_t prefix = 0;
};

struct AutoNetwork {
  std::string address_cidr;
  std::string dhcp_start;
  std::string dhcp_end;
};

struct AutoRadio {
  Band band = Band::Band5;
  uint16_t channel = 36;
  bool fell_back = false;
};

bool overlaps(Ipv4Range left, Ipv4Range right);
std::vector<Ipv4Range> read_local_networks(std::string_view exclude_iface);
std::vector<Ipv4Range> parse_proc_net_route(std::string_view text);
Result<AutoNetwork> choose_network(const std::vector<Ipv4Range>& used);
AutoRadio choose_radio(const IfaceInfo& ap, bool increased_compatibility);
std::vector<Ipv4Range> candidate_networks();
uint32_t ipv4_host(int a, int b, int c, int d);

}  // namespace hotmon
