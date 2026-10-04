#include "app.hpp"
#include "render.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <locale.h>

namespace {

using namespace hotmon;

std::string screen(const App& app) {
  std::string text;
  for (const auto& line : render(app, 80, 24)) {
    text += line;
  }
  return text;
}

Key none_key() { return {}; }

TEST(Render, RejectionMessageIsVisibleInTheHeader) {
  auto app = App::from_parts(BackendKind::NetworkManager, {}, "/tmp/hotmon-header-test.json",
                             std::nullopt);
  app.wizard.error = "The interface nope is not available.";
  EXPECT_NE(screen(app).find("The interface nope is not available."), std::string::npos);
}

TEST(Render, ReviewAndStatusDoNotShowThePassphrase) {
  auto app = App::from_parts(BackendKind::NetworkManager, sample_interfaces(),
                             "/tmp/hotmon-secret-screen.json", sample_profile());
  const auto review = screen(app);
  EXPECT_NE(review.find("Passphrase: set"), std::string::npos);
  EXPECT_EQ(review.find("correct-horse"), std::string::npos);
  app.wizard.page = Page::Passphrase;
  const auto masked = screen(app);
  EXPECT_EQ(masked.find("correct-horse"), std::string::npos);
  EXPECT_NE(masked.find("*************"), std::string::npos);
  app.wizard.security.select_value("open");
  app.wizard.passphrase.clear();
  app.wizard.upstream.select_value("eth0");
  app.wizard.page = Page::Review;
  const auto warning = screen(app);
  EXPECT_NE(warning.find("radio range"), std::string::npos);
  EXPECT_EQ(warning.find("correct-horse"), std::string::npos);
  app.view = View::Status;
  app.active = sample_profile();
  app.status = HotspotStatus::running("Hotmon", "wlan0", "NetworkManager");
  const auto status = screen(app);
  EXPECT_EQ(status.find("correct-horse"), std::string::npos);
  EXPECT_NE(status.find("Hotmon"), std::string::npos);
  (void)none_key;
}

TEST(Render, SecurityPageShowsTheSelectionMarker) {
  auto app = App::from_parts(BackendKind::NetworkManager, sample_interfaces(), "/tmp/hotmon-ui.json",
                             std::nullopt);
  app.wizard.page = Page::Security;
  const auto text = screen(app);
  EXPECT_NE(text.find("(*) WPA2"), std::string::npos);
  EXPECT_NE(text.find("( ) Open"), std::string::npos);
  EXPECT_NE(text.find("( ) WPA3"), std::string::npos);
}

TEST(Render, NetworkPageShowsTheChosenRange) {
  auto app = App::from_parts(BackendKind::NetworkManager, sample_interfaces(), "/tmp/hotmon-ui.json",
                             std::nullopt);
  app.wizard.page = Page::AddressDhcp;
  app.wizard.address_cidr = "192.168.42.0/24";
  app.wizard.dhcp_enabled = true;
  app.wizard.dhcp_start = "192.168.42.10";
  app.wizard.dhcp_end = "192.168.42.100";
  const auto text = screen(app);
  EXPECT_NE(text.find("Address range: 192.168.42.0/24"), std::string::npos);
  EXPECT_NE(text.find("advanced setup"), std::string::npos);
}

TEST(Render, ReviewPageShowsTheRequiredLines) {
  auto app = App::from_parts(BackendKind::NetworkManager, sample_interfaces(), "/tmp/hotmon-ui.json",
                             sample_profile());
  const auto text = screen(app);
  EXPECT_NE(text.find("Band:"), std::string::npos);
  EXPECT_NE(text.find("Channel:"), std::string::npos);
  EXPECT_NE(text.find("Address range:"), std::string::npos);
  EXPECT_NE(text.find("DHCP:"), std::string::npos);
}

TEST(Render, FooterMatchesThePageKind) {
  auto app = App::from_parts(BackendKind::NetworkManager, sample_interfaces(), "/tmp/hotmon-ui.json",
                             std::nullopt);
  app.wizard.page = Page::Security;
  EXPECT_NE(screen(app).find("Up/Down: choose"), std::string::npos);
  app.wizard.page = Page::Ssid;
  EXPECT_NE(screen(app).find("Tab: next field"), std::string::npos);
  app.wizard.page = Page::AddressDhcp;
  EXPECT_NE(screen(app).find("a: advanced setup"), std::string::npos);
  app.wizard.page = Page::Review;
  EXPECT_NE(screen(app).find("Enter: apply"), std::string::npos);
}

TEST(Render, SparklineUsesBlockCharacters) {
  setlocale(LC_ALL, "C.UTF-8");
  const auto text = sparkline_text({0, 5}, 2);
  EXPECT_NE(text.find("▁"), std::string::npos);
  EXPECT_NE(text.find("█"), std::string::npos);
}

}  // namespace
