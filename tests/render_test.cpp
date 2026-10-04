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

TEST(Render, MonitorGraphShowsEverySampleInUtf8) {
  setlocale(LC_ALL, "C.UTF-8");
  auto app = App::from_parts(BackendKind::NetworkManager, sample_interfaces(),
                             "/tmp/hotmon-graph-screen.json", std::nullopt);
  app.view = View::Monitor;
  for (uint64_t total = 0; total < 40; ++total) {
    app.monitor.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, total * total, 0}});
  }
  const auto lines = render(app, 80, 40);
  const std::string graph = sparkline_text(app.monitor.total_samples(), 78);
  EXPECT_EQ(lines[5], "|" + graph + std::string(78 - 40, ' ') + "|");
  setlocale(LC_ALL, "C");
}

TEST(Render, WrapKeepsMultibyteCharactersWhole) {
  auto app = App::from_parts(BackendKind::NetworkManager, {}, "/tmp/hotmon-wrap-test.json",
                             std::nullopt);
  std::string notice;
  for (int index = 0; index < 30; ++index) {
    notice += "\xc3\xa9";
  }
  app.notice = notice;
  const auto lines = render(app, 12, 24);
  size_t notice_rows = 0;
  for (const std::string& line : lines) {
    size_t columns = 0;
    for (char ch : line) {
      columns += (static_cast<unsigned char>(ch) & 0xC0) != 0x80 ? 1 : 0;
    }
    EXPECT_EQ(columns, 12u) << line;
    if (line.find('\xc3') != std::string::npos) {
      EXPECT_EQ(line, "|" + std::string(notice.data(), 20) + "|");
      ++notice_rows;
    }
  }
  EXPECT_EQ(notice_rows, 3u);
}

}  // namespace
