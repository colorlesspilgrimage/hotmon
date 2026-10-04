#include "netauto.hpp"

#include <gtest/gtest.h>

namespace {

using namespace hotmon;

IfaceInfo radio(bool five, std::vector<int> channels) {
  IfaceInfo info;
  info.name = "wlan0";
  info.wireless = true;
  info.supports_ap = true;
  info.supports_5ghz = five;
  info.channels_5 = std::move(channels);
  info.channels_24 = {1, 6, 11};
  return info;
}

TEST(Netauto, ChooseRadioUsesTheDefaultAndFallback) {
  auto full = choose_radio(radio(true, {36, 40}), false);
  EXPECT_EQ(full.band, Band::Band5);
  EXPECT_EQ(full.channel, 36);
  EXPECT_FALSE(full.fell_back);
  auto compat = choose_radio(radio(true, {36}), true);
  EXPECT_EQ(compat.band, Band::Band24);
  EXPECT_EQ(compat.channel, 6);
  EXPECT_FALSE(compat.fell_back);
  auto none = choose_radio(radio(false, {}), false);
  EXPECT_EQ(none.band, Band::Band24);
  EXPECT_EQ(none.channel, 6);
  EXPECT_TRUE(none.fell_back);
  auto missing = choose_radio(radio(true, {40, 44}), false);
  EXPECT_EQ(missing.band, Band::Band24);
  EXPECT_EQ(missing.channel, 6);
  EXPECT_TRUE(missing.fell_back);
}

TEST(Netauto, ChooseNetworkSkipsUsedRanges) {
  auto free = choose_network({});
  ASSERT_TRUE(free);
  EXPECT_EQ(free->address_cidr, "192.168.42.0/24");
  EXPECT_EQ(free->dhcp_start, "192.168.42.10");
  EXPECT_EQ(free->dhcp_end, "192.168.42.100");
  auto next = choose_network({Ipv4Range{ipv4_host(192, 168, 42, 0), 24}});
  ASSERT_TRUE(next);
  EXPECT_EQ(next->address_cidr, "192.168.43.0/24");
  auto wide = choose_network({Ipv4Range{ipv4_host(192, 168, 0, 0), 16}});
  ASSERT_TRUE(wide);
  EXPECT_EQ(wide->address_cidr.rfind("10.42.", 0), 0u);
  auto private_range = choose_network({Ipv4Range{ipv4_host(10, 0, 0, 0), 8},
                                       Ipv4Range{ipv4_host(192, 168, 0, 0), 16}});
  ASSERT_TRUE(private_range);
  EXPECT_EQ(private_range->address_cidr, "172.16.0.0/24");
  auto blocked = choose_network(candidate_networks());
  ASSERT_FALSE(blocked);
  EXPECT_EQ(blocked.error(), "No private address range is free. Use advanced setup.");
}

TEST(Netauto, OverlapsIsSymmetricAndASmallRangeBlocks) {
  const Ipv4Range candidate{ipv4_host(192, 168, 42, 0), 24};
  const Ipv4Range inside{ipv4_host(192, 168, 42, 4), 30};
  EXPECT_TRUE(overlaps(candidate, inside));
  EXPECT_TRUE(overlaps(inside, candidate));
  EXPECT_EQ(overlaps(candidate, inside), overlaps(inside, candidate));
  const Ipv4Range other{ipv4_host(10, 1, 1, 0), 24};
  EXPECT_FALSE(overlaps(candidate, other));
}

TEST(Netauto, ParseProcNetRouteSkipsTheDefaultRoute) {
  const char* text =
      "Iface Destination Gateway Flags RefCnt Use Metric Mask\n"
      "eth0 00000000 0101A8C0 0003 0 0 0 00000000 0 0 0\n"
      "wlan0 0000A8C0 00000000 0001 0 0 600 00FFFFFF 0 0 0\n";
  const auto ranges = parse_proc_net_route(text);
  ASSERT_EQ(ranges.size(), 1u);
  EXPECT_EQ(ranges[0].base, ipv4_host(192, 168, 0, 0));
  EXPECT_EQ(ranges[0].prefix, 24);
}

TEST(Netauto, ParseProcNetRouteSkipsTheAccessPointAndLoopback) {
  const char* text =
      "Iface Destination Gateway Flags RefCnt Use Metric Mask\n"
      "wlan0 002AA8C0 00000000 0001 0 0 0 00FFFFFF 0 0 0\n"
      "lo 0000007F 00000000 0001 0 0 0 000000FF 0 0 0\n"
      "eth0 0000000A 00000000 0001 0 0 0 000000FF 0 0 0\n";
  const auto ranges = parse_proc_net_route(text, "wlan0");
  ASSERT_EQ(ranges.size(), 1u);
  EXPECT_EQ(ranges[0].base, ipv4_host(10, 0, 0, 0));
  EXPECT_EQ(ranges[0].prefix, 8);
  auto chosen = choose_network(ranges);
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->address_cidr, "192.168.42.0/24");
}

TEST(Netauto, DhcpRangeNeverContainsTheGateway) {
  for (const auto& candidate : candidate_networks()) {
    auto chosen = choose_network({});
    ASSERT_TRUE(chosen);
    auto network = Ipv4Network::parse(chosen->address_cidr);
    ASSERT_TRUE(network);
    auto gateway = network->gateway();
    ASSERT_TRUE(gateway);
    EXPECT_NE(*gateway, chosen->dhcp_start);
    EXPECT_NE(*gateway, chosen->dhcp_end);
    auto start = parse_ipv4(chosen->dhcp_start);
    auto end = parse_ipv4(chosen->dhcp_end);
    auto gate = parse_ipv4(*gateway);
    ASSERT_TRUE(start && end && gate);
    EXPECT_TRUE(*start > *gate || *end < *gate);
    EXPECT_TRUE(network->usable(*start));
    EXPECT_TRUE(network->usable(*end));
    (void)candidate;
    break;
  }
  for (const auto& candidate : candidate_networks()) {
    std::vector<Ipv4Range> used;
    for (const auto& other : candidate_networks()) {
      if (other.base != candidate.base) {
        used.push_back(other);
      }
    }
    auto chosen = choose_network(used);
    ASSERT_TRUE(chosen) << format_ipv4(candidate.base);
    EXPECT_EQ(chosen->address_cidr, format_ipv4(candidate.base) + "/24");
    auto network = Ipv4Network::parse(chosen->address_cidr);
    ASSERT_TRUE(network);
    auto gateway = parse_ipv4(*network->gateway());
    auto start = parse_ipv4(chosen->dhcp_start);
    auto end = parse_ipv4(chosen->dhcp_end);
    ASSERT_TRUE(gateway && start && end);
    EXPECT_LT(*start, *end);
    EXPECT_TRUE(*start > *gateway || *end < *gateway);
    EXPECT_TRUE(network->usable(*start));
    EXPECT_TRUE(network->usable(*end));
  }
}

}
