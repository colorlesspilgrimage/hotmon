#include "profile.hpp"
#include "test_support.hpp"

#include <yyjson.h>

#include <gtest/gtest.h>

#include <fstream>

namespace {

using namespace hotmon;

std::filesystem::path fixture_path() {
  return std::filesystem::path(HOTMON_SOURCE_DIR) / "tests/data/rust_profile.json";
}

TEST(ProfileCompat, LoadsTheRustFixture) {
  auto loaded = load_profile(fixture_path());
  ASSERT_TRUE(loaded) << loaded.error();
  EXPECT_EQ(*loaded, sample_profile());
}

TEST(ProfileCompat, SaveKeepsKeyOrderAndTypes) {
  const auto dir = scratch_dir();
  const auto path = dir / "profile.json";
  ASSERT_TRUE(save_profile(path, sample_profile()));
  std::ifstream input(path);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
  ASSERT_NE(doc, nullptr);
  yyjson_val* root = yyjson_doc_get_root(doc);
  std::vector<std::string> keys;
  yyjson_obj_iter iter;
  yyjson_obj_iter_init(root, &iter);
  yyjson_val* key = nullptr;
  while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
    keys.emplace_back(yyjson_get_str(key));
  }
  EXPECT_EQ(keys, (std::vector<std::string>{"ap_interface", "ssid", "security", "passphrase", "band",
                                            "channel", "address_cidr", "dhcp_enabled", "dhcp_start",
                                            "dhcp_end", "upstream_interface"}));
  yyjson_val* band = yyjson_obj_get(root, "band");
  yyjson_val* channel = yyjson_obj_get(root, "channel");
  EXPECT_STREQ(yyjson_get_str(band), "2.4");
  EXPECT_TRUE(yyjson_is_num(channel));
  EXPECT_EQ(yyjson_get_int(channel), 6);
  yyjson_doc_free(doc);
  std::filesystem::remove_all(dir);
}

TEST(ProfileCompat, StrictReadRules) {
  const auto dir = scratch_dir();
  auto write = [&](const std::string& body) {
    const auto path = dir / "profile.json";
    std::ofstream(path) << body;
    return path;
  };
  const std::string base =
      R"({"ap_interface":"wlan0","ssid":"Hotmon","security":"wpa2","passphrase":"correct-horse","band":"2.4","channel":6,"address_cidr":"192.168.42.0/24","dhcp_enabled":true,"dhcp_start":"192.168.42.10","dhcp_end":"192.168.42.100","upstream_interface":"eth0"})";
  EXPECT_FALSE(load_profile(write(R"({"ssid":"Hotmon"})")));
  auto wrong_channel = base;
  const auto channel_at = wrong_channel.find("\"channel\":6");
  ASSERT_NE(channel_at, std::string::npos);
  wrong_channel.replace(channel_at, std::string("\"channel\":6").size(), "\"channel\":\"6\"");
  EXPECT_FALSE(load_profile(write(wrong_channel)));
  auto bad_security = base;
  const auto security = bad_security.find("\"security\":\"wpa2\"");
  bad_security.replace(security, std::string("\"security\":\"wpa2\"").size(), "\"security\":\"WPA2\"");
  EXPECT_FALSE(load_profile(write(bad_security)));
  auto bad_band = base;
  const auto band = bad_band.find("\"band\":\"2.4\"");
  bad_band.replace(band, std::string("\"band\":\"2.4\"").size(), "\"band\":\"6\"");
  EXPECT_FALSE(load_profile(write(bad_band)));
  auto extra = base;
  extra.insert(extra.size() - 1, ",\"extra\":true");
  auto loaded = load_profile(write(extra));
  ASSERT_TRUE(loaded) << loaded.error();
  EXPECT_EQ(loaded->ssid, "Hotmon");
  std::filesystem::remove_all(dir);
}

TEST(ProfileCompat, EscapedNulIsNotCutOff) {
  const auto dir = scratch_dir();
  const auto path = dir / "profile.json";
  std::ofstream(path)
      << R"({"ap_interface":"wlan0","ssid":"Hot\u0000mon","security":"wpa2","passphrase":"correct-horse","band":"2.4","channel":6,"address_cidr":"192.168.42.0/24","dhcp_enabled":true,"dhcp_start":"192.168.42.10","dhcp_end":"192.168.42.100","upstream_interface":"eth0"})";
  auto loaded = load_profile(path);
  ASSERT_TRUE(loaded) << loaded.error();
  EXPECT_EQ(loaded->ssid, std::string("Hot\0mon", 7));
  EXPECT_FALSE(loaded->check_settings());
  std::ofstream(path)
      << R"({"ap_interface":"wlan0","ssid":"Hotmon","security":"wpa2\u0000x","passphrase":"correct-horse","band":"2.4","channel":6,"address_cidr":"192.168.42.0/24","dhcp_enabled":true,"dhcp_start":"192.168.42.10","dhcp_end":"192.168.42.100","upstream_interface":"eth0"})";
  EXPECT_FALSE(load_profile(path));
  std::filesystem::remove_all(dir);
}

}
