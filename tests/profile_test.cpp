#include "profile.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <fstream>

namespace {

using namespace hotmon;

TEST(Profile, SsidRejectsEmptyAndSlash) {
  EXPECT_FALSE(validate_ssid(""));
  EXPECT_FALSE(validate_ssid("cafe/guest"));
  EXPECT_TRUE(validate_ssid("Cafe Guest"));
}

TEST(Profile, PassphraseRules) {
  EXPECT_TRUE(validate_passphrase(SecurityMode::Open, ""));
  EXPECT_FALSE(validate_passphrase(SecurityMode::Open, "secret"));
  EXPECT_FALSE(validate_passphrase(SecurityMode::Wpa2, "short"));
  EXPECT_TRUE(validate_passphrase(SecurityMode::Wpa3, "correct-horse"));
  EXPECT_TRUE(validate_passphrase(SecurityMode::Wpa2, "correct horse"));
  EXPECT_FALSE(validate_passphrase(SecurityMode::Wpa2, "correct-horse\\"));
  EXPECT_FALSE(validate_passphrase(SecurityMode::Wpa2, "correct\"horse"));
  EXPECT_FALSE(validate_passphrase(SecurityMode::Wpa2, "correct#horse"));
  EXPECT_FALSE(validate_passphrase(SecurityMode::Wpa2, " correct-horse"));
  EXPECT_FALSE(validate_passphrase(SecurityMode::Wpa2, "correct-horse "));
}

TEST(Profile, ChannelMustMatchTheBand) {
  EXPECT_TRUE(parse_channel(Band::Band24, "6"));
  EXPECT_FALSE(parse_channel(Band::Band24, "36"));
  EXPECT_TRUE(parse_channel(Band::Band5, "36"));
  EXPECT_FALSE(parse_channel(Band::Band5, "6"));
}

TEST(Profile, DhcpRangeMustStayInsideTheNetwork) {
  EXPECT_TRUE(validate_address_dhcp("192.168.42.0/24", "on", "192.168.42.10", "192.168.42.20"));
  EXPECT_FALSE(validate_address_dhcp("192.168.42.0/24", "on", "10.0.0.10", "10.0.0.20"));
  EXPECT_FALSE(validate_address_dhcp("192.168.42.0/24", "on", "192.168.42.1", "192.168.42.20"));
}

TEST(Profile, ProfileRoundTrip) {
  const auto dir = scratch_dir();
  const auto path = dir / "profile.json";
  const auto profile = sample_profile();
  ASSERT_TRUE(save_profile(path, profile));
  auto loaded = load_profile(path);
  ASSERT_TRUE(loaded);
  EXPECT_EQ(*loaded, profile);
  std::filesystem::remove_all(dir);
}

TEST(Profile, MissingProfileIsEmpty) {
  const auto dir = scratch_dir();
  auto loaded = load_optional(dir / "missing.json");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->has_value());
  std::filesystem::remove_all(dir);
}

TEST(Profile, CorruptProfileReturnsAnError) {
  const auto dir = scratch_dir();
  const auto path = dir / "profile.json";
  std::ofstream(path) << "{";
  auto error = load_profile(path);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("not valid"), std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(Profile, DirectoryProfileReportsTheReadError) {
  const auto dir = scratch_dir();
  const auto path = dir / "profile.json";
  std::filesystem::create_directories(path);
  auto error = load_optional(path);
  ASSERT_FALSE(error);
  EXPECT_EQ(error.error(), "The program cannot read " + path.string() + ". Is a directory");
  std::filesystem::remove_all(dir);
}

TEST(Profile, ProfilePathUsesXdgThenHome) {
  EXPECT_EQ(profile_path_from("/cfg", "/home/sam"),
            std::filesystem::path("/cfg/hotmon/profile.json"));
  EXPECT_EQ(profile_path_from("", "/home/sam"),
            std::filesystem::path("/home/sam/.config/hotmon/profile.json"));
}

TEST(Profile, ReviewListsEachSetting) {
  const auto lines = sample_profile().review_lines();
  std::string text;
  for (const auto& line : lines) {
    text += line + "\n";
  }
  for (const char* item : {"wlan0", "Hotmon", "wpa2", "Passphrase: set", "2.4", "6",
                           "192.168.42.0/24", "192.168.42.10", "eth0"}) {
    EXPECT_NE(text.find(item), std::string::npos) << item;
  }
  EXPECT_EQ(text.find("correct-horse"), std::string::npos);
}

TEST(Profile, ReviewForAnOpenNetworkHidesThePassphrase) {
  auto profile = sample_profile();
  profile.security = SecurityMode::Open;
  profile.passphrase.clear();
  profile.upstream_interface = "none";
  std::string lines;
  for (const auto& line : profile.review_lines()) {
    lines += line + "\n";
  }
  EXPECT_NE(lines.find("the network is open"), std::string::npos);
  EXPECT_EQ(lines.find(OPEN_UPSTREAM_WARNING), std::string::npos);
  profile.upstream_interface = "eth0";
  lines.clear();
  for (const auto& line : profile.review_lines()) {
    lines += line + "\n";
  }
  EXPECT_NE(lines.find(OPEN_UPSTREAM_WARNING), std::string::npos);
  EXPECT_EQ(lines.find("correct-horse"), std::string::npos);
}

TEST(Profile, ProfileDirectoryAndFileArePrivate) {
  const auto dir = scratch_dir();
  const auto path = dir / "hotmon" / "profile.json";
  ASSERT_TRUE(save_profile(path, sample_profile()));
  const auto dir_mode = static_cast<unsigned>(std::filesystem::status(path.parent_path()).permissions()) & 0777;
  const auto file_mode = static_cast<unsigned>(std::filesystem::status(path).permissions()) & 0777;
  EXPECT_EQ(dir_mode, 0700u);
  EXPECT_EQ(file_mode, 0600u);
  std::filesystem::remove_all(dir);
}

TEST(Profile, BadChannelFailsTheSettingCheck) {
  auto profile = sample_profile();
  profile.channel = 99;
  EXPECT_FALSE(profile.check_settings());
}

TEST(Profile, Ipv4RejectsRustForms) {
  EXPECT_FALSE(parse_ipv4("01.2.3.4"));
  EXPECT_FALSE(parse_ipv4("1.2.3"));
  EXPECT_FALSE(parse_ipv4("1.2.3.4.5"));
  EXPECT_FALSE(parse_ipv4("256.1.1.1"));
}

}
