#include "app.hpp"
#include "render.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <locale.h>
#include <cstdint>
#include <limits>
#include <chrono>
#include <wchar.h>

namespace {

using namespace hotmon;

std::string joined(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) {
    text += line;
  }
  return text;
}

std::string screen(const App& app) { return joined(render(app, 80, 24)); }

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
  if (setlocale(LC_ALL, "C.UTF-8") == nullptr) {
    GTEST_SKIP() << "C.UTF-8 is not available.";
  }
  auto app = App::from_parts(BackendKind::NetworkManager, sample_interfaces(),
                             "/tmp/hotmon-graph-screen.json", std::nullopt);
  app.view = View::Monitor;
  for (uint64_t total = 0; total < 40; ++total) {
    app.monitor.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, total * total, 0}});
  }
  const auto lines = render(app, 80, 40);
  const std::string graph = sparkline_text(app.monitor.total_samples(), 78);
  bool found = false;
  for (const std::string& line : lines) {
    if (line.find(graph) == std::string::npos) {
      continue;
    }
    EXPECT_TRUE(line.starts_with("│"));
    EXPECT_TRUE(line.ends_with("│"));
    found = true;
  }
  EXPECT_TRUE(found);
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

int column_count(const std::string& text) {
  int columns = 0;
  const char* cursor = text.c_str();
  size_t left = text.size();
  mbstate_t state{};
  while (left > 0) {
    wchar_t code = 0;
    const size_t used = mbrtowc(&code, cursor, left, &state);
    if (used == static_cast<size_t>(-1) || used == static_cast<size_t>(-2) || used == 0) {
      ++columns;
      ++cursor;
      --left;
      state = {};
      continue;
    }
    const int width = wcwidth(code);
    columns += width < 0 ? 1 : width;
    cursor += used;
    left -= used;
  }
  return columns;
}

bool all_spaces(const std::string& text) {
  return text.find_first_not_of(' ') == std::string::npos;
}

App plain_app(const char* path) {
  return App::from_parts(BackendKind::NetworkManager, {}, path, std::nullopt);
}

App running_status(const char* path) {
  auto app = plain_app(path);
  app.view = View::Status;
  app.running = true;
  app.status = HotspotStatus::running("Hotmon", "wlan0", "NetworkManager");
  return app;
}

TEST(Render, StatusShowsDevicesPanel) {
  auto app = running_status("/tmp/hotmon-devices.json");
  const auto t0 = std::chrono::steady_clock::time_point{};
  app.monitor.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", "192.168.42.20", 0, 0}}, t0);
  app.monitor.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", "192.168.42.20", 2000, 6000}},
                     t0 + std::chrono::seconds(2));
  const std::string text = joined(render(app, 100, 30));
  EXPECT_NE(text.find("Devices"), std::string::npos);
  EXPECT_NE(text.find("aa:bb:cc:dd:ee:ff"), std::string::npos);
  EXPECT_NE(text.find("192.168.42.20"), std::string::npos);
  EXPECT_NE(text.find("2.9 KiB/s"), std::string::npos);
  EXPECT_NE(text.find("1000 B/s"), std::string::npos);
  EXPECT_NE(text.find("5.8 KiB"), std::string::npos);
  EXPECT_NE(text.find("1.9 KiB"), std::string::npos);
  EXPECT_EQ(text.find("correct-horse"), std::string::npos);
}

TEST(Render, StatusDevicesEmptyStates) {
  auto app = running_status("/tmp/hotmon-devices-empty.json");
  const auto running = screen(app);
  EXPECT_NE(running.find("No devices are connected."), std::string::npos);
  app.running = false;
  app.status = HotspotStatus::stopped();
  const auto stopped = screen(app);
  EXPECT_NE(stopped.find("The hotspot is stopped."), std::string::npos);
  EXPECT_NE(stopped.find("The hotspot is stopped. No devices to show."), std::string::npos);
}

TEST(Render, StatusDevicesNarrowDropsColumns) {
  auto app = running_status("/tmp/hotmon-devices-narrow.json");
  const auto t0 = std::chrono::steady_clock::time_point{};
  app.monitor.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 0, 0}}, t0);
  app.monitor.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 1000, 3000}},
                     t0 + std::chrono::seconds(1));
  const auto lines = render(app, 50, 24);
  std::string text;
  for (const std::string& line : lines) {
    text += line;
    EXPECT_LE(column_count(line), 50);
  }
  EXPECT_NE(text.find("Down"), std::string::npos);
  EXPECT_NE(text.find("Up"), std::string::npos);
  EXPECT_EQ(text.find("Total"), std::string::npos);
  EXPECT_NE(text.find("-"), std::string::npos);
}

TEST(Render, StatusDevicesOverflowShowsMore) {
  auto app = running_status("/tmp/hotmon-devices-more.json");
  std::vector<ClientSnapshot> clients;
  for (int index = 0; index < 10; ++index) {
    ClientSnapshot client;
    client.mac = "aa:bb:cc:dd:ee:0" + std::to_string(index);
    client.rx_bytes = index == 3 ? std::numeric_limits<uint64_t>::max() : 10;
    client.tx_bytes = 20;
    if (index != 1) {
      client.ip = "192.168.42." + std::to_string(10 + index);
    }
    clients.push_back(client);
  }
  const auto t0 = std::chrono::steady_clock::time_point{};
  app.monitor.update(clients, t0);
  app.monitor.update(clients, t0 + std::chrono::seconds(1));
  const auto lines = render(app, 80, 20);
  EXPECT_EQ(lines.size(), 20u);
  std::string text;
  for (const std::string& line : lines) {
    text += line;
    EXPECT_EQ(column_count(line), 80);
  }
  EXPECT_NE(text.find("more devices."), std::string::npos);
  EXPECT_NE(text.find("-"), std::string::npos);
}

TEST(Render, FillsTheWholeTerminal) {
  setlocale(LC_ALL, "C");
  const int widths[] = {80, 120, 200};
  const int heights[] = {24, 40, 60};
  const View views[] = {View::Status, View::Monitor, View::Wizard};
  for (View view : views) {
    auto app = plain_app("/tmp/hotmon-fill.json");
    app.view = view;
    for (int index = 0; index < 3; ++index) {
      const int width = widths[index];
      const int height = heights[index];
      const auto lines = render(app, width, height);
      ASSERT_EQ(static_cast<int>(lines.size()), height);
      EXPECT_EQ(lines.back().front(), '+');
      EXPECT_EQ(lines.back().back(), '+');
      for (const std::string& line : lines) {
        EXPECT_EQ(column_count(line), width) << line;
        EXPECT_FALSE(all_spaces(line)) << line;
      }
      const auto again = render(app, width + 4, height + 2);
      EXPECT_EQ(static_cast<int>(again.size()), height + 2);
      EXPECT_EQ(column_count(again.front()), width + 4);
    }
  }
}

TEST(Render, UsesSolidLinesInUtf8) {
  if (setlocale(LC_ALL, "C.UTF-8") == nullptr) {
    GTEST_SKIP() << "C.UTF-8 is not available.";
  }
  auto app = plain_app("/tmp/hotmon-solid.json");
  std::string notice;
  for (int index = 0; index < 80; ++index) {
    notice += "\xc3\xa9";
  }
  app.notice = notice;
  const auto lines = render(app, 40, 24);
  std::string text;
  for (const std::string& line : lines) {
    text += line;
    EXPECT_EQ(column_count(line), 40) << line;
    EXPECT_NE(line.front(), '|');
    EXPECT_NE(line.front(), '+');
  }
  EXPECT_NE(text.find("─"), std::string::npos);
  EXPECT_NE(text.find("│"), std::string::npos);
  EXPECT_EQ(text.find("+---"), std::string::npos);
  setlocale(LC_ALL, "C");
}

TEST(Render, FallsBackToAsciiInPlainLocale) {
  setlocale(LC_ALL, "C");
  auto app = plain_app("/tmp/hotmon-ascii.json");
  const auto lines = render(app, 80, 24);
  std::string text;
  bool saw_plus = false;
  bool saw_dash = false;
  for (const std::string& line : lines) {
    text += line;
    if (!line.empty() && (line.front() == '+' || line.front() == '|')) {
      for (unsigned char byte : line) {
        EXPECT_LE(byte, 0x7F);
      }
      if (line.front() == '+') {
        saw_plus = true;
        if (line.find('-') != std::string::npos) {
          saw_dash = true;
        }
      }
    }
  }
  EXPECT_TRUE(saw_plus);
  EXPECT_TRUE(saw_dash);
  EXPECT_NE(text.find("+"), std::string::npos);
}

TEST(Render, TinyTerminalStillRenders) {
  auto app = plain_app("/tmp/hotmon-tiny.json");
  const int widths[] = {1, 2, 10};
  const int heights[] = {1, 2, 5};
  for (int index = 0; index < 3; ++index) {
    const auto lines = render(app, widths[index], heights[index]);
    EXPECT_EQ(static_cast<int>(lines.size()), heights[index]);
  }
}

TEST(Render, StyledRowsMatchTextRows) {
  auto app = plain_app("/tmp/hotmon-styled.json");
  app.view = View::Status;
  const auto styled = render_styled(app, 80, 24);
  const auto plain = render(app, 80, 24);
  ASSERT_EQ(styled.size(), 24u);
  ASSERT_EQ(plain.size(), styled.size());
  for (size_t index = 0; index < styled.size(); ++index) {
    EXPECT_EQ(styled[index].text, plain[index]);
  }
}

}
