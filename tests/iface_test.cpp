#include "iface.hpp"
#include "test_support.hpp"
#include <fstream>
#include <gtest/gtest.h>

namespace {

using namespace hotmon;

class MapPhy : public PhyInfo {
 public:
  PhyCaps caps(std::string_view phy) const override {
    PhyCaps result;
    if (phy == "phy0") {
      result.supports_ap = true;
      result.channels_24 = {1, 6, 11};
      result.channels_5 = {36};
    }
    return result;
  }
};

TEST(Iface, ApModeLineIsRequired) {
  const char* text = "Supported interface modes:\n\t\t * managed\n\t\t * AP\n\t\t * AP/VLAN\n";
  EXPECT_TRUE(modes_support_ap(text));
  EXPECT_FALSE(modes_support_ap("* AP/VLAN\n* managed\n"));
}

TEST(Iface, InterfaceWithoutApSupportIsRejected) {
  IfaceInfo wired;
  wired.name = "eth0";
  auto wired_error = require_ap(wired);
  ASSERT_FALSE(wired_error);
  EXPECT_NE(wired_error.error().find("not a wireless"), std::string::npos);
  IfaceInfo station;
  station.name = "wlan1";
  station.wireless = true;
  auto station_error = require_ap(station);
  ASSERT_FALSE(station_error);
  EXPECT_NE(station_error.error().find("cannot start an access point"), std::string::npos);
  IfaceInfo ap;
  ap.name = "wlan0";
  ap.wireless = true;
  ap.supports_ap = true;
  EXPECT_TRUE(require_ap(ap));
}

TEST(Iface, SysfsReadMarksApSupport) {
  const auto dir = scratch_dir();
  std::filesystem::create_directories(dir / "wlan0");
  std::filesystem::create_directories(dir / "wlan1");
  std::filesystem::create_directories(dir / "eth0");
  std::filesystem::create_symlink("phy0", dir / "wlan0" / "phy80211");
  std::ofstream(dir / "wlan1" / "wireless") << "";
  const auto infos = read_interfaces_at(dir, MapPhy{});
  ASSERT_EQ(infos.size(), 3u);
  EXPECT_EQ(infos[0].name, "eth0");
  EXPECT_FALSE(infos[0].wireless);
  EXPECT_FALSE(infos[0].supports_ap);
  EXPECT_EQ(infos[1].name, "wlan0");
  EXPECT_TRUE(infos[1].wireless);
  EXPECT_TRUE(infos[1].supports_ap);
  EXPECT_TRUE(infos[1].supports_5ghz);
  EXPECT_EQ(infos[1].channels_5, std::vector<int>({36}));
  EXPECT_EQ(infos[2].name, "wlan1");
  EXPECT_TRUE(infos[2].wireless);
  EXPECT_FALSE(infos[2].supports_ap);
  std::filesystem::remove_all(dir);
}

TEST(Iface, PhyInfoSkipsDisabledChannels) {
  const char* text =
      "Supported interface modes:\n * AP\nFrequencies:\n"
      " * 2412.0 MHz [1] (20.0 dBm)\n"
      " * 2417.0 MHz [2] (20.0 dBm) (no IR)\n"
      " * 2437.0 MHz [6] (disabled)\n"
      " * 2484.0 MHz [14] (20.0 dBm)\n"
      " * 5180.0 MHz [36] (22.0 dBm)\n"
      " * 5200.0 MHz [40] (no IR)\n"
      " * 5260.0 MHz [52] (20.0 dBm) (radar detection)\n";
  const auto caps = parse_phy_info(text);
  EXPECT_TRUE(caps.supports_ap);
  EXPECT_EQ(caps.channels_24, std::vector<int>({1}));
  EXPECT_EQ(caps.channels_5, (std::vector<int>{36, 52}));
}

TEST(Iface, CandidatesSplitApAndUpstream) {
  const auto interfaces = sample_interfaces();
  const auto ap = ap_candidates(interfaces);
  ASSERT_EQ(ap.size(), 1u);
  EXPECT_EQ(ap[0].name, "wlan0");
  const auto upstream = upstream_candidates(interfaces, "wlan0");
  ASSERT_EQ(upstream.size(), 2u);
  EXPECT_EQ(upstream[0].name, "eth0");
  EXPECT_EQ(upstream[1].name, "wlan1");
}

}
