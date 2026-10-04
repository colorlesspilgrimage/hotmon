#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

namespace hotmon {

struct ClientSnapshot {
  std::string mac;
  std::optional<std::string> ip;
  uint64_t rx_bytes = 0;
  uint64_t tx_bytes = 0;
};

class Series {
 public:
  explicit Series(size_t capacity = 40);
  void observe(uint64_t total);
  std::vector<uint64_t> samples() const;

 private:
  std::deque<uint64_t> samples_;
  std::optional<uint64_t> last_;
  size_t capacity_ = 40;
};

struct ClientTraffic {
  std::string mac;
  std::optional<std::string> ip;
  uint64_t rx_bytes = 0;
  uint64_t tx_bytes = 0;
  Series graph{40};
};

class MonitorState {
 public:
  MonitorState();
  void update(const std::vector<ClientSnapshot>& clients);
  std::vector<ClientTraffic> clients() const;
  std::vector<uint64_t> total_samples() const;
  void clear();

 private:
  std::map<std::string, ClientTraffic> clients_;
  Series total_;
};

std::vector<ClientSnapshot> parse_station_dump(std::string_view text);
std::vector<std::pair<std::string, std::string>> parse_neigh(std::string_view text);
std::vector<ClientSnapshot> clients_from_text(std::string_view dump, std::string_view neigh);

}
