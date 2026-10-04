#include "monitor.hpp"

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

}  // namespace
