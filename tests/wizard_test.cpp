#include "wizard.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

namespace {

using namespace hotmon;

HostFacts facts_with(std::vector<IfaceInfo> interfaces,
                     std::function<std::vector<Ipv4Range>(std::string_view)> networks = {}) {
  HostFacts facts;
  facts.interfaces = std::move(interfaces);
  facts.local_networks = networks ? std::move(networks)
                                  : [](std::string_view) { return std::vector<Ipv4Range>{}; };
  return facts;
}

void type_text(Wizard& wizard, const std::string& text) {
  for (char ch : text) {
    wizard.push_char(static_cast<char32_t>(ch));
  }
}

std::string join(const std::vector<std::string>& lines) {
  std::string text;
  for (const auto& line : lines) {
    text += line + "\n";
  }
  return text;
}

TEST(Wizard, StartStateOpensOnWpa2AndFiveGhz) {
  auto wizard = Wizard::make(facts_with(sample_interfaces()));
  EXPECT_EQ(wizard.security.current().value, "wpa2");
  EXPECT_EQ(wizard.band_choice.current().value, "5");
}

TEST(Wizard, DefaultPathFillsRadioAndNetwork) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::make(facts);
  ASSERT_TRUE(wizard.next(facts));
  type_text(wizard, "Cafe Guest");
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  type_text(wizard, "correct-horse");
  ASSERT_TRUE(wizard.next(facts));
  wizard.upstream.select_value("eth0");
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  EXPECT_EQ(wizard.page, Page::Review);
  auto profile = wizard.confirmed_profile(facts);
  ASSERT_TRUE(profile);
  EXPECT_EQ(profile->band, Band::Band5);
  EXPECT_EQ(profile->channel, 36);
  EXPECT_EQ(profile->address_cidr, "192.168.42.0/24");
  EXPECT_TRUE(profile->dhcp_enabled);
  EXPECT_EQ(profile->dhcp_start, "192.168.42.10");
  EXPECT_EQ(profile->dhcp_end, "192.168.42.100");
  EXPECT_EQ(profile->ssid, "Cafe Guest");
}

TEST(Wizard, CompatibilityUsesChannel6) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::make(facts);
  ASSERT_TRUE(wizard.next(facts));
  type_text(wizard, "Cafe Guest");
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  type_text(wizard, "correct-horse");
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  wizard.band_choice.down();
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  auto profile = wizard.confirmed_profile(facts);
  ASSERT_TRUE(profile);
  EXPECT_EQ(profile->band, Band::Band24);
  EXPECT_EQ(profile->channel, 6);
}

TEST(Wizard, NoFiveGhzRadioShowsTheFallbackNote) {
  auto interfaces = sample_interfaces();
  interfaces[1].supports_5ghz = false;
  interfaces[1].channels_5.clear();
  auto facts = facts_with(interfaces);
  auto wizard = Wizard::make(facts);
  ASSERT_TRUE(wizard.next(facts));
  type_text(wizard, "Cafe");
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  type_text(wizard, "correct-horse");
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  auto profile = wizard.confirmed_profile(facts);
  ASSERT_TRUE(profile);
  EXPECT_EQ(profile->band, Band::Band24);
  EXPECT_EQ(profile->channel, 6);
  EXPECT_NE(join(wizard.review_lines()).find(NOTE_NO_5GHZ), std::string::npos);
}

TEST(Wizard, NetworkOverlapSkipsTheUsedRange) {
  auto facts = facts_with(sample_interfaces(), [](std::string_view) {
    return std::vector<Ipv4Range>{Ipv4Range{ipv4_host(192, 168, 42, 0), 24}};
  });
  auto wizard = Wizard::make(facts);
  ASSERT_TRUE(wizard.next(facts));
  type_text(wizard, "Cafe");
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  type_text(wizard, "correct-horse");
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  EXPECT_EQ(wizard.address_cidr, "192.168.43.0/24");
}

TEST(Wizard, BoxesListOnlyValidInterfaces) {
  auto wizard = Wizard::make(facts_with(sample_interfaces()));
  ASSERT_EQ(wizard.ap_interface.choices().size(), 1u);
  EXPECT_EQ(wizard.ap_interface.choices()[0].value, "wlan0");
  std::vector<std::string> upstream;
  for (const auto& choice : wizard.upstream.choices()) {
    upstream.push_back(choice.value);
  }
  EXPECT_NE(std::find(upstream.begin(), upstream.end(), "eth0"), upstream.end());
  EXPECT_NE(std::find(upstream.begin(), upstream.end(), "wlan1"), upstream.end());
  EXPECT_NE(std::find(upstream.begin(), upstream.end(), "none"), upstream.end());
  EXPECT_EQ(std::find(upstream.begin(), upstream.end(), "wlan0"), upstream.end());
}

TEST(Wizard, EmptyApCandidatesStayOnPageOne) {
  auto facts = facts_with({IfaceInfo{"eth0", false, false, false, {}, {}}});
  auto wizard = Wizard::make(facts);
  auto error = wizard.next(facts);
  ASSERT_FALSE(error);
  EXPECT_EQ(error.error(), NO_AP_INTERFACE);
  EXPECT_EQ(wizard.page, Page::Interface);
}

TEST(Wizard, SecurityBoxIgnoresTypedText) {
  auto wizard = Wizard::make(facts_with(sample_interfaces()));
  wizard.page = Page::Security;
  const auto labels = wizard.security.choices();
  ASSERT_EQ(labels.size(), 3u);
  EXPECT_EQ(labels[0].label, "Open");
  EXPECT_EQ(labels[1].label, "WPA2");
  EXPECT_EQ(labels[2].label, "WPA3");
  wizard.push_char(U'x');
  EXPECT_EQ(wizard.security.current().value, "wpa2");
}

TEST(Wizard, OpenSecurityAllowsOnlyAnEmptyPassphrase) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::make(facts);
  wizard.page = Page::Security;
  wizard.security.select_value("open");
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  EXPECT_EQ(wizard.page, Page::Upstream);
  wizard.page = Page::Passphrase;
  wizard.passphrase = "secret";
  auto error = wizard.next(facts);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("An open network does not use a passphrase."), std::string::npos);
}

TEST(Wizard, PassphrasePageMasksAndRejectsShellCharacters) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::from_profile(sample_profile(), facts);
  wizard.page = Page::Passphrase;
  const auto lines = wizard.field_lines();
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines[0].value, std::string(13, '*'));
  EXPECT_EQ(lines[0].value.find("correct-horse"), std::string::npos);
  for (const char* bad : {"correct#horse", "correct\"horse", "correct\\horse", " correct-horse",
                          "correct-horse "}) {
    wizard.passphrase = bad;
    EXPECT_FALSE(wizard.next(facts)) << bad;
    EXPECT_EQ(wizard.page, Page::Passphrase);
  }
}

TEST(Wizard, AdvancedSetupUsesInterfaceChannels) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::make(facts);
  wizard.page = Page::AddressDhcp;
  wizard.address_cidr = "192.168.42.0/24";
  wizard.dhcp_start = "192.168.42.10";
  wizard.dhcp_end = "192.168.42.100";
  ASSERT_TRUE(wizard.open_advanced(facts));
  EXPECT_EQ(wizard.page, Page::Advanced);
  wizard.adv_band.select_value("5");
  wizard.move_down(facts);
  wizard.move_up(facts);
  wizard.adv_band.select_value("5");
  const IfaceInfo* ap = nullptr;
  for (const auto& info : facts.interfaces) {
    if (info.name == "wlan0") {
      ap = &info;
    }
  }
  ASSERT_NE(ap, nullptr);
  wizard.rebuild_advanced(facts);
  wizard.adv_band.select_value("5");
  std::vector<int> channels;
  for (const auto& choice : wizard.adv_channel.choices()) {
    if (wizard.adv_band.current().value == "5") {
      channels.push_back(std::stoi(choice.value));
    }
  }
  wizard.adv_band.select_value("2.4");
  auto after = Wizard::make(facts);
  after.page = Page::AddressDhcp;
  after.band = Band::Band5;
  after.channel = 36;
  after.address_cidr = "192.168.42.0/24";
  after.dhcp_enabled = true;
  after.dhcp_start = "192.168.42.10";
  after.dhcp_end = "192.168.42.100";
  ASSERT_TRUE(after.open_advanced(facts));
  after.adv_band.select_value("5");
  after.move_up(facts);
  after.move_down(facts);
  std::vector<int> five;
  for (const auto& choice : after.adv_channel.choices()) {
    five.push_back(std::stoi(choice.value));
  }
  EXPECT_EQ(five, ap->channels_5);
  after.adv_band.select_value("2.4");
  after.move_down(facts);
  after.move_up(facts);
  std::vector<int> low;
  for (const auto& choice : after.adv_channel.choices()) {
    low.push_back(std::stoi(choice.value));
  }
  EXPECT_EQ(low, ap->channels_24);
}

TEST(Wizard, AdvancedValidInputSetsTheSource) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::make(facts);
  ASSERT_TRUE(wizard.next(facts));
  type_text(wizard, "Cafe");
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  type_text(wizard, "correct-horse");
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.next(facts));
  ASSERT_TRUE(wizard.open_advanced(facts));
  wizard.adv_address_cidr = "10.20.30.0/24";
  wizard.adv_dhcp.select_value("on");
  wizard.adv_dhcp_start = "10.20.30.50";
  wizard.adv_dhcp_end = "10.20.30.90";
  ASSERT_TRUE(wizard.next(facts));
  auto profile = wizard.confirmed_profile(facts);
  ASSERT_TRUE(profile);
  EXPECT_EQ(profile->address_cidr, "10.20.30.0/24");
  EXPECT_EQ(profile->dhcp_start, "10.20.30.50");
  EXPECT_EQ(profile->dhcp_end, "10.20.30.90");
  EXPECT_NE(join(wizard.review_lines()).find("Settings source: set by the user (advanced setup)"),
            std::string::npos);
}

TEST(Wizard, AdvancedBadInputStaysOnThePage) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::make(facts);
  wizard.page = Page::Advanced;
  wizard.adv_band = SelectBox({{"2.4 GHz", "2.4"}}, 0);
  wizard.adv_channel = SelectBox({{"6", "6"}}, 0);
  wizard.adv_dhcp = SelectBox({{"On", "on"}, {"Off", "off"}}, 0);
  struct Case {
    const char* address;
    const char* start;
    const char* end;
    const char* text;
  };
  const Case cases[] = {
      {"192.168.42.5/24", "192.168.42.10", "192.168.42.20", "network address"},
      {"", "192.168.42.10", "192.168.42.20", "prefix"},
      {"192.168.42.0", "192.168.42.10", "192.168.42.20", "prefix"},
      {"192.168.42.0/31", "192.168.42.10", "192.168.42.20", "8 to 30"},
      {"not-an-address", "192.168.42.10", "192.168.42.20", "prefix"},
      {"192.168.42.0/24", "192.168.42.1", "192.168.42.20", "hotspot address"},
      {"192.168.42.0/24", "192.168.42.20", "192.168.42.10", "must not be after"},
      {"192.168.42.0/24", "10.0.0.10", "10.0.0.20", "outside"},
      {"192.168.42.0/24", "", "192.168.42.20", "Enter the DHCP start"},
  };
  for (const auto& item : cases) {
    wizard.page = Page::Advanced;
    wizard.adv_address_cidr = item.address;
    wizard.adv_dhcp_start = item.start;
    wizard.adv_dhcp_end = item.end;
    auto error = wizard.next(facts);
    ASSERT_FALSE(error) << item.address;
    EXPECT_NE(error.error().find(item.text), std::string::npos) << error.error();
    EXPECT_EQ(wizard.page, Page::Advanced);
  }
}

TEST(Wizard, AdvancedDhcpOffClearsTheRange) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::make(facts);
  wizard.ssid = "Cafe";
  wizard.passphrase = "correct-horse";
  wizard.page = Page::Advanced;
  wizard.adv_band = SelectBox({{"2.4 GHz", "2.4"}}, 0);
  wizard.adv_channel = SelectBox({{"6", "6"}}, 0);
  wizard.adv_dhcp = SelectBox({{"On", "on"}, {"Off", "off"}}, 1);
  wizard.adv_address_cidr = "10.20.30.0/24";
  wizard.adv_dhcp_start = "10.20.30.50";
  wizard.adv_dhcp_end = "10.20.30.90";
  ASSERT_TRUE(wizard.next(facts));
  auto profile = wizard.confirmed_profile(facts);
  ASSERT_TRUE(profile) << profile.error();
  EXPECT_FALSE(profile->dhcp_enabled);
  EXPECT_TRUE(profile->dhcp_start.empty());
  EXPECT_TRUE(profile->dhcp_end.empty());
}

TEST(Wizard, AutomaticResetRestoresTheSource) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::make(facts);
  wizard.network_source = NetSource::Advanced;
  wizard.address_cidr = "10.9.8.0/24";
  ASSERT_TRUE(wizard.use_automatic(facts));
  EXPECT_EQ(wizard.network_source, NetSource::Automatic);
  EXPECT_EQ(wizard.address_cidr, "192.168.42.0/24");
  EXPECT_NE(join(wizard.review_lines()).find("Settings source: automatic"), std::string::npos);
}

TEST(Wizard, SavedProfileKeepsStoredValues) {
  auto profile = sample_profile();
  profile.band = Band::Band24;
  profile.channel = 11;
  profile.address_cidr = "10.9.8.0/24";
  profile.dhcp_start = "10.9.8.20";
  profile.dhcp_end = "10.9.8.30";
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::from_profile(profile, facts);
  EXPECT_EQ(wizard.page, Page::Review);
  auto confirmed = wizard.confirmed_profile(facts);
  ASSERT_TRUE(confirmed);
  EXPECT_EQ(*confirmed, profile);
  EXPECT_NE(join(wizard.review_lines()).find("Settings source: saved profile"), std::string::npos);
  EXPECT_EQ(wizard.band_choice.choices().front().label, "Saved setting (2.4 GHz, channel 11)");
}

TEST(Wizard, SavedFiveGhzHasNoExtraChoice) {
  auto profile = sample_profile();
  profile.band = Band::Band5;
  profile.channel = 36;
  auto wizard = Wizard::from_profile(profile, facts_with(sample_interfaces()));
  for (const auto& choice : wizard.band_choice.choices()) {
    EXPECT_EQ(choice.label.find("Saved setting"), std::string::npos);
  }
}

TEST(Wizard, BackAndForwardKeepsValues) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::make(facts);
  ASSERT_TRUE(wizard.next(facts));
  type_text(wizard, "Cafe");
  ASSERT_TRUE(wizard.next(facts));
  EXPECT_EQ(wizard.page, Page::Security);
  wizard.back();
  EXPECT_EQ(wizard.page, Page::Ssid);
  EXPECT_EQ(wizard.ssid, "Cafe");
  wizard.security.select_value("wpa3");
  ASSERT_TRUE(wizard.next(facts));
  wizard.back();
  EXPECT_EQ(wizard.security.current().value, "wpa3");
}

TEST(Wizard, CancelBlocksConfirm) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::from_profile(sample_profile(), facts);
  wizard.cancel();
  auto error = wizard.confirmed_profile(facts);
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("cancelled"), std::string::npos);
}

TEST(Wizard, ConfirmOutsideReviewFails) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::make(facts);
  auto error = wizard.confirmed_profile(facts);
  ASSERT_FALSE(error);
  EXPECT_EQ(error.error(), "Confirm the settings on the review page.");
}

TEST(Wizard, ReviewShowsTheChosenValues) {
  auto facts = facts_with(sample_interfaces());
  auto wizard = Wizard::from_profile(sample_profile(), facts);
  const auto text = join(wizard.review_lines());
  EXPECT_NE(text.find("Band:"), std::string::npos);
  EXPECT_NE(text.find("Channel:"), std::string::npos);
  EXPECT_NE(text.find("Address range:"), std::string::npos);
  EXPECT_NE(text.find("DHCP:"), std::string::npos);
  EXPECT_EQ(text.find("correct-horse"), std::string::npos);
}

TEST(Wizard, TypedFieldLimitIs128) {
  auto wizard = Wizard::make(facts_with(sample_interfaces()));
  wizard.page = Page::Ssid;
  for (int index = 0; index < 140; ++index) {
    wizard.push_char(U'a');
  }
  EXPECT_EQ(wizard.ssid.size(), 128u);
}

TEST(Wizard, OpenUpstreamReviewShowsTheWarning) {
  auto facts = facts_with(sample_interfaces());
  auto profile = sample_profile();
  profile.security = SecurityMode::Open;
  profile.passphrase.clear();
  auto wizard = Wizard::from_profile(profile, facts);
  EXPECT_NE(join(wizard.review_lines()).find(OPEN_UPSTREAM_WARNING), std::string::npos);
}

TEST(Wizard, UpstreamBoxOpensOnNone) {
  auto wizard = Wizard::make(facts_with(sample_interfaces()));
  ASSERT_GT(wizard.upstream.choices().size(), 1u);
  EXPECT_EQ(wizard.upstream.current().value, "none");
}

TEST(Wizard, TypedFieldsAcceptUnicodeText) {
  auto wizard = Wizard::make(facts_with(sample_interfaces()));
  wizard.page = Page::Ssid;
  wizard.push_char(U'C');
  wizard.push_char(U'a');
  wizard.push_char(U'f');
  wizard.push_char(U'\u00e9');
  wizard.push_char(U'\U0001F600');
  EXPECT_EQ(wizard.ssid, "Caf\xc3\xa9\xf0\x9f\x98\x80");
  EXPECT_TRUE(validate_ssid(wizard.ssid));
}

TEST(Wizard, TypedFieldsRejectControlAndInvalidCodePoints) {
  auto wizard = Wizard::make(facts_with(sample_interfaces()));
  wizard.page = Page::Ssid;
  wizard.push_char(U'\u0085');
  wizard.push_char(static_cast<char32_t>(0xD800));
  wizard.push_char(static_cast<char32_t>(0x110000));
  EXPECT_EQ(wizard.ssid, "");
}

TEST(Wizard, BackspaceRemovesOneWholeCharacter) {
  auto wizard = Wizard::make(facts_with(sample_interfaces()));
  wizard.page = Page::Ssid;
  wizard.push_char(U'a');
  wizard.push_char(U'\u00e9');
  wizard.backspace();
  EXPECT_EQ(wizard.ssid, "a");
}

TEST(Wizard, TypedFieldLimitCountsCharacters) {
  auto wizard = Wizard::make(facts_with(sample_interfaces()));
  wizard.page = Page::Ssid;
  for (int index = 0; index < 140; ++index) {
    wizard.push_char(U'\u00e9');
  }
  EXPECT_EQ(wizard.ssid.size(), 256u);
}

}  // namespace
