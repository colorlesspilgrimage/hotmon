#include "monitor.hpp"

#include <limits>
#include <chrono>

#include <gtest/gtest.h>

namespace {

using namespace hotmon;

TEST(Monitor, StationDumpReadsByteCounters) {
  const char* text =
      "Station AA:BB:CC:DD:EE:FF (on wlan0)\n"
      "\tinactive time:\t10 ms\n"
      "\trx bytes:\t1000\n"
      "\trx packets:\t8\n"
      "\ttx bytes:\t2500\n"
      "Station 11:22:33:44:55:66 (on wlan0)\n"
      "\trx bytes:\t40\n"
      "\ttx bytes:\t50\n";
  const auto clients = parse_station_dump(text);
  ASSERT_EQ(clients.size(), 2u);
  EXPECT_EQ(clients[0].mac, "aa:bb:cc:dd:ee:ff");
  EXPECT_EQ(clients[0].rx_bytes, 1000u);
  EXPECT_EQ(clients[0].tx_bytes, 2500u);
  EXPECT_EQ(clients[1].rx_bytes, 40u);
  EXPECT_EQ(clients[1].tx_bytes, 50u);
}

TEST(Monitor, NeighAddsTheClientAddress) {
  const char* dump = "Station aa:bb:cc:dd:ee:ff (on wlan0)\n\trx bytes:\t1\n\ttx bytes:\t2\n";
  const char* neigh = "192.168.42.20 lladdr aa:bb:cc:dd:ee:ff REACHABLE\n";
  const auto clients = clients_from_text(dump, neigh);
  ASSERT_FALSE(clients.empty());
  ASSERT_TRUE(clients[0].ip);
  EXPECT_EQ(*clients[0].ip, "192.168.42.20");
}

TEST(Monitor, GraphChangesWhenTheByteCounterChanges) {
  Series series(8);
  series.observe(100);
  series.observe(140);
  series.observe(140);
  EXPECT_EQ(series.samples(), (std::vector<uint64_t>{0, 40, 0}));
  series.observe(180);
  EXPECT_EQ(series.samples().back(), 40u);
}

TEST(Monitor, TotalGraphAddsEachClient) {
  MonitorState monitor;
  monitor.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 0, 0},
                  ClientSnapshot{"11:22:33:44:55:66", std::nullopt, 0, 0}});
  monitor.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 10, 0},
                  ClientSnapshot{"11:22:33:44:55:66", std::nullopt, 0, 5}});
  EXPECT_EQ(monitor.total_samples(), (std::vector<uint64_t>{0, 15}));
  const auto clients = monitor.clients();
  ASSERT_EQ(clients.size(), 2u);
  EXPECT_EQ(clients[0].graph.samples(), (std::vector<uint64_t>{0, 5}));
  EXPECT_EQ(clients[1].graph.samples(), (std::vector<uint64_t>{0, 10}));
}

TEST(Monitor, BadCountersReadAsZero) {
  const char* text =
      "Station aa:bb:cc:dd:ee:ff (on wlan0)\n"
      "\trx bytes:\t-1\n"
      "\ttx bytes:\t12abc\n"
      "Station 11:22:33:44:55:66 (on wlan0)\n"
      "\trx bytes:\t99999999999999999999999\n"
      "\ttx bytes:\t+7\n";
  const auto clients = parse_station_dump(text);
  ASSERT_EQ(clients.size(), 2u);
  EXPECT_EQ(clients[0].rx_bytes, 0u);
  EXPECT_EQ(clients[0].tx_bytes, 0u);
  EXPECT_EQ(clients[1].rx_bytes, 0u);
  EXPECT_EQ(clients[1].tx_bytes, 7u);
}

TEST(Monitor, HugeCountersDoNotWrapTheTotal) {
  const uint64_t max = std::numeric_limits<uint64_t>::max();
  MonitorState state;
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 10, 0}});
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, max, max},
                ClientSnapshot{"11:22:33:44:55:66", std::nullopt, max, 0}});
  EXPECT_EQ(state.total_samples(), (std::vector<uint64_t>{0, max - 10}));
  EXPECT_EQ(state.clients()[1].graph.samples(), (std::vector<uint64_t>{0, max - 10}));
}

TEST(Monitor, RateIsZeroOnFirstSample) {
  MonitorState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 100, 200}}, t0);
  const auto clients = state.clients();
  ASSERT_EQ(clients.size(), 1u);
  EXPECT_EQ(clients[0].rx_rate, 0u);
  EXPECT_EQ(clients[0].tx_rate, 0u);
}

TEST(Monitor, RateUsesElapsedTime) {
  MonitorState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 0, 0}}, t0);
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 2000, 6000}},
               t0 + std::chrono::seconds(2));
  const auto clients = state.clients();
  ASSERT_EQ(clients.size(), 1u);
  EXPECT_EQ(clients[0].rx_rate, 1000u);
  EXPECT_EQ(clients[0].tx_rate, 3000u);
}

TEST(Monitor, RateKeepsOldValueUnderOneSecond) {
  MonitorState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 0, 0}}, t0);
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 2000, 4000}},
               t0 + std::chrono::seconds(2));
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 9000, 9000}},
               t0 + std::chrono::seconds(2) + std::chrono::milliseconds(200));
  const auto clients = state.clients();
  ASSERT_EQ(clients.size(), 1u);
  EXPECT_EQ(clients[0].rx_rate, 1000u);
  EXPECT_EQ(clients[0].tx_rate, 2000u);
}

TEST(Monitor, RateIsZeroWhenCounterResets) {
  MonitorState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 5000, 8000}}, t0);
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 100, 50}},
               t0 + std::chrono::seconds(1));
  const auto clients = state.clients();
  ASSERT_EQ(clients.size(), 1u);
  EXPECT_EQ(clients[0].rx_rate, 0u);
  EXPECT_EQ(clients[0].tx_rate, 0u);
}

TEST(Monitor, RateIsZeroForIdleDevice) {
  MonitorState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 100, 200}}, t0);
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 100, 200}},
               t0 + std::chrono::seconds(1));
  const auto clients = state.clients();
  ASSERT_EQ(clients.size(), 1u);
  EXPECT_EQ(clients[0].rx_rate, 0u);
  EXPECT_EQ(clients[0].tx_rate, 0u);
}

TEST(Monitor, ReturningDeviceStartsAtZero) {
  MonitorState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 0, 0}}, t0);
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 5000, 5000}},
               t0 + std::chrono::seconds(2));
  state.update({}, t0 + std::chrono::seconds(3));
  EXPECT_TRUE(state.clients().empty());
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 9000, 9000}},
               t0 + std::chrono::seconds(4));
  const auto clients = state.clients();
  ASSERT_EQ(clients.size(), 1u);
  EXPECT_EQ(clients[0].rx_rate, 0u);
  EXPECT_EQ(clients[0].tx_rate, 0u);
}

TEST(Monitor, HugeDeltaDoesNotOverflow) {
  MonitorState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  const uint64_t max = std::numeric_limits<uint64_t>::max();
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 0, 0}}, t0);
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, max, max}},
               t0 + std::chrono::seconds(1));
  const auto clients = state.clients();
  ASSERT_EQ(clients.size(), 1u);
  EXPECT_EQ(clients[0].rx_rate, max);
  EXPECT_EQ(clients[0].tx_rate, max);
}

TEST(Monitor, TotalRatesSumTheDevices) {
  MonitorState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 0, 0},
                ClientSnapshot{"11:22:33:44:55:66", std::nullopt, 0, 0}},
               t0);
  state.update({ClientSnapshot{"aa:bb:cc:dd:ee:ff", std::nullopt, 1000, 2000},
                ClientSnapshot{"11:22:33:44:55:66", std::nullopt, 3000, 4000}},
               t0 + std::chrono::seconds(1));
  EXPECT_EQ(state.total_rx_rate(), 4000u);
  EXPECT_EQ(state.total_tx_rate(), 6000u);
}

}
