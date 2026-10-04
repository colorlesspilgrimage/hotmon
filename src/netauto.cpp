#include <algorithm>
#include <cctype>
#include "netauto.hpp"

#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <fstream>
#include <sstream>

namespace hotmon {
namespace {

uint32_t prefix_mask(uint8_t prefix) {
  if (prefix == 0) {
    return 0;
  }
  if (prefix >= 32) {
    return 0xFFFFFFFFU;
  }
  return 0xFFFFFFFFU << (32 - prefix);
}

uint8_t prefix_of(uint32_t mask) {
  uint8_t bits = 0;
  while (mask != 0) {
    bits = static_cast<uint8_t>(bits + (mask >> 31));
    mask <<= 1;
  }
  return bits;
}

bool contains(Ipv4Range outer, Ipv4Range inner) {
  if (outer.prefix > inner.prefix) {
    return false;
  }
  const uint32_t mask = prefix_mask(outer.prefix);
  return (outer.base & mask) == (inner.base & mask);
}

uint32_t parse_hex(std::string_view text) {
  uint32_t value = 0;
  for (char ch : text) {
    value <<= 4;
    if (ch >= '0' && ch <= '9') {
      value |= static_cast<uint32_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      value |= static_cast<uint32_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      value |= static_cast<uint32_t>(ch - 'A' + 10);
    }
  }
  return value;
}

Ipv4Range make_range(int a, int b, int c) {
  return Ipv4Range{ipv4_host(a, b, c, 0), 24};
}

}

uint32_t ipv4_host(int a, int b, int c, int d) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
         (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
}

bool overlaps(Ipv4Range left, Ipv4Range right) {
  return contains(left, right) || contains(right, left);
}

std::vector<Ipv4Range> candidate_networks() {
  std::vector<Ipv4Range> ranges;
  ranges.push_back(make_range(192, 168, 42));
  for (int third = 43; third <= 255; ++third) {
    ranges.push_back(make_range(192, 168, third));
  }
  for (int third = 0; third <= 41; ++third) {
    ranges.push_back(make_range(192, 168, third));
  }
  for (int third = 0; third <= 255; ++third) {
    ranges.push_back(make_range(10, 42, third));
  }
  for (int second = 16; second <= 31; ++second) {
    ranges.push_back(make_range(172, second, 0));
  }
  return ranges;
}

std::vector<Ipv4Range> parse_proc_net_route(std::string_view text) {
  std::vector<Ipv4Range> ranges;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    const std::string_view line =
        text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    std::vector<std::string> parts;
    size_t index = 0;
    while (index < line.size()) {
      while (index < line.size() &&
             std::isspace(static_cast<unsigned char>(line[index])) != 0) {
        ++index;
      }
      if (index >= line.size()) {
        break;
      }
      const size_t token = index;
      while (index < line.size() &&
             std::isspace(static_cast<unsigned char>(line[index])) == 0) {
        ++index;
      }
      parts.emplace_back(line.substr(token, index - token));
    }
    if (parts.size() >= 8 && parts[0] != "Iface") {
      const uint32_t mask = ntohl(parse_hex(parts[7]));
      if (mask != 0) {
        const uint32_t destination = ntohl(parse_hex(parts[1]));
        ranges.push_back(Ipv4Range{destination & mask, prefix_of(mask)});
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return ranges;
}

std::vector<Ipv4Range> read_local_networks(std::string_view exclude_iface) {
  std::vector<Ipv4Range> ranges;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) == 0) {
    for (ifaddrs* item = list; item != nullptr; item = item->ifa_next) {
      if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET ||
          item->ifa_netmask == nullptr || item->ifa_name == nullptr) {
        continue;
      }
      const std::string name = item->ifa_name;
      if (name == "lo" || name == exclude_iface) {
        continue;
      }
      const uint32_t address = ntohl(reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr.s_addr);
      const uint32_t mask = ntohl(reinterpret_cast<sockaddr_in*>(item->ifa_netmask)->sin_addr.s_addr);
      ranges.push_back(Ipv4Range{address & mask, prefix_of(mask)});
    }
    freeifaddrs(list);
  }
  std::ifstream route("/proc/net/route");
  std::ostringstream buffer;
  buffer << route.rdbuf();
  for (const Ipv4Range& item : parse_proc_net_route(buffer.str())) {
    ranges.push_back(item);
  }
  return ranges;
}

Result<AutoNetwork> choose_network(const std::vector<Ipv4Range>& used) {
  for (const Ipv4Range& candidate : candidate_networks()) {
    const bool blocked = std::any_of(used.begin(), used.end(), [&](const Ipv4Range& item) {
      return overlaps(candidate, item);
    });
    if (blocked) {
      continue;
    }
    const uint32_t base = candidate.base;
    AutoNetwork network;
    network.address_cidr = format_ipv4(base) + "/24";
    network.dhcp_start = format_ipv4(base + 10);
    network.dhcp_end = format_ipv4(base + 100);
    return network;
  }
  return unexpected_text("No private address range is free. Use advanced setup.");
}

AutoRadio choose_radio(const IfaceInfo& ap, bool increased_compatibility) {
  if (increased_compatibility) {
    return AutoRadio{Band::Band24, 6, false};
  }
  const bool has_36 = std::find(ap.channels_5.begin(), ap.channels_5.end(), 36) != ap.channels_5.end();
  if (ap.supports_5ghz && has_36) {
    return AutoRadio{Band::Band5, 36, false};
  }
  return AutoRadio{Band::Band24, 6, true};
}

}
