#include "capture.hpp"
#include "capture_socket.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <memory>
#include <gtest/gtest.h>

namespace {

using namespace hotmon;

std::vector<uint8_t> udp_frame() {
  std::vector<uint8_t> frame(14 + 20 + 8);
  for (int index = 0; index < 6; ++index) {
    frame[static_cast<size_t>(index)] = 0xFF;
  }
  frame[6] = 0x02;
  frame[11] = 0x01;
  frame[12] = 0x08;
  frame[14] = 0x45;
  frame[17] = 28;
  frame[22] = 64;
  frame[23] = 17;
  frame[26] = 192;
  frame[27] = 168;
  frame[28] = 42;
  frame[29] = 10;
  frame[30] = 192;
  frame[31] = 168;
  frame[32] = 42;
  frame[33] = 1;
  frame[34] = 0x30;
  frame[35] = 0x39;
  frame[37] = 53;
  return frame;
}

TEST(Capture, DoesNotStartBeforeTheSecondConfirmation) {
  CaptureControl capture;
  auto first = capture.warn("wlan0");
  ASSERT_TRUE(first);
  EXPECT_EQ(first->kind, ConfirmKind::Warning);
  EXPECT_EQ(capture.phase(), CapturePhase::Warned);
  EXPECT_FALSE(capture.is_running());
  auto repeated = capture.warn("wlan0");
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->kind, ConfirmKind::Warning);
  FakeSource unused;
  unused.frames.push_back(udp_frame());
  ASSERT_TRUE(capture.poll());
  EXPECT_EQ(unused.reads, 0);
  auto second = capture.accept("wlan0");
  ASSERT_TRUE(second);
  EXPECT_EQ(second->kind, ConfirmKind::Open);
  EXPECT_EQ(second->iface, "wlan0");
  EXPECT_FALSE(capture.is_running());
  auto source = std::make_unique<FakeSource>();
  source->frames.push_back(udp_frame());
  ASSERT_TRUE(capture.attach(std::move(source)));
  EXPECT_TRUE(capture.is_running());
  ASSERT_TRUE(capture.poll());
  const auto lines = capture.lines();
  ASSERT_FALSE(lines.empty());
  EXPECT_NE(lines[0].find("192.168.42.10"), std::string::npos);
  EXPECT_NE(lines[0].find("192.168.42.1"), std::string::npos);
  EXPECT_NE(lines[0].find("UDP"), std::string::npos);
}

TEST(Capture, SummaryDoesNotChangeTheFrame) {
  auto frame = udp_frame();
  const auto before = frame;
  const auto text = summarize(frame).text();
  std::reverse(frame.begin(), frame.end());
  std::reverse(frame.begin(), frame.end());
  EXPECT_EQ(frame, before);
  EXPECT_NE(text.find("UDP 12345->53"), std::string::npos);
  EXPECT_FALSE(capture_interface("eth0", "wlan0"));
  auto same = capture_interface("wlan0", "wlan0");
  ASSERT_TRUE(same);
  EXPECT_EQ(*same, "wlan0");
}

TEST(Capture, StopEndsCapture) {
  CaptureControl capture;
  ASSERT_TRUE(capture.warn("wlan0"));
  ASSERT_TRUE(capture.accept("wlan0"));
  ASSERT_TRUE(capture.attach(std::make_unique<FakeSource>()));
  capture.stop();
  EXPECT_EQ(capture.phase(), CapturePhase::Idle);
  EXPECT_FALSE(capture.is_running());
}

TEST(Capture, AttachBeforeTheSecondConfirmationFails) {
  CaptureControl capture;
  ASSERT_TRUE(capture.warn("wlan0"));
  auto error = capture.attach(std::make_unique<FakeSource>());
  ASSERT_FALSE(error);
  EXPECT_NE(error.error().find("not confirmed"), std::string::npos);
}

TEST(Capture, WarningKeyDoesNotStartCapture) {
  CaptureControl capture;
  auto ignored = capture.accept("wlan0");
  ASSERT_TRUE(ignored);
  EXPECT_EQ(ignored->kind, ConfirmKind::Ignored);
  ASSERT_TRUE(capture.warn("wlan0"));
  ASSERT_TRUE(capture.warn("wlan0"));
  EXPECT_EQ(capture.phase(), CapturePhase::Warned);
  EXPECT_FALSE(capture.armed_interface());
  auto changed = capture.accept("wlan1");
  ASSERT_TRUE(changed);
  EXPECT_EQ(changed->kind, ConfirmKind::Warning);
  EXPECT_EQ(capture.phase(), CapturePhase::Warned);
  EXPECT_FALSE(capture.armed_interface());
  auto open = capture.accept("wlan1");
  ASSERT_TRUE(open);
  EXPECT_EQ(open->kind, ConfirmKind::Open);
  EXPECT_EQ(open->iface, "wlan1");
}

TEST(Capture, SummariesCoverCommonFrames) {
  std::vector<uint8_t> arp(14, 0);
  arp[12] = 0x08;
  arp[13] = 0x06;
  EXPECT_NE(summarize(arp).text().find("ARP"), std::string::npos);
  auto tcp = udp_frame();
  tcp[23] = 6;
  EXPECT_NE(summarize(tcp).text().find("TCP"), std::string::npos);
  EXPECT_NE(summarize(udp_frame()).text().find("UDP"), std::string::npos);
  EXPECT_NE(summarize(std::vector<uint8_t>{1, 2, 3}).text().find("short"), std::string::npos);
}

TEST(Capture, InvalidInterfaceDoesNotOpenASocket) {
  auto empty = LocalCapture::open("");
  ASSERT_FALSE(empty);
  EXPECT_NE(empty.error().find("The capture interface name is not valid."), std::string::npos);
  auto bad = LocalCapture::open("not a name");
  ASSERT_FALSE(bad);
  EXPECT_NE(bad.error().find("The capture interface name is not valid."), std::string::npos);
}

}  // namespace
