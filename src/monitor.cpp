#include <algorithm>
#include "monitor.hpp"

#include "text.hpp"

#include <set>
#include <chrono>
#include <charconv>
#include <limits>

namespace hotmon {
namespace {

constexpr size_t kGraphCapacity = 40;


std::optional<std::string> value_after(std::string_view line, std::string_view label) {
  if (!line.starts_with(label)) {
    return std::nullopt;
  }
  return trim_copy(line.substr(label.size()));
}



uint64_t parse_counter(std::string_view text) {
  const auto parts = split_ws(text);
  if (parts.empty()) {
    return 0;
  }
  std::string_view token = parts[0];
  if (token.starts_with('+')) {
    token.remove_prefix(1);
  }
  uint64_t value = 0;
  const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
  if (token.empty() || error != std::errc() || end != token.data() + token.size()) {
    return 0;
  }
  return value;
}

uint64_t saturating_add(uint64_t left, uint64_t right) {
  return right > std::numeric_limits<uint64_t>::max() - left
             ? std::numeric_limits<uint64_t>::max()
             : left + right;
}

uint64_t multiply_u64(uint64_t left, uint64_t right, uint64_t& high) {
  const uint64_t left_lo = left & 0xffffffffULL;
  const uint64_t left_hi = left >> 32;
  const uint64_t right_lo = right & 0xffffffffULL;
  const uint64_t right_hi = right >> 32;
  const uint64_t low_low = left_lo * right_lo;
  const uint64_t cross = (low_low >> 32) + (left_lo * right_hi & 0xffffffffULL) +
                         (left_hi * right_lo & 0xffffffffULL);
  high = left_hi * right_hi + (left_lo * right_hi >> 32) + (left_hi * right_lo >> 32) + (cross >> 32);
  return (cross << 32) | (low_low & 0xffffffffULL);
}

uint64_t divide_u128(uint64_t high, uint64_t low, uint64_t divisor) {
  if (divisor == 0 || high >= divisor) {
    return std::numeric_limits<uint64_t>::max();
  }
  uint64_t quotient = 0;
  uint64_t remainder = high;
  for (int bit = 63; bit >= 0; --bit) {
    const uint64_t top = remainder >> 63;
    remainder = (remainder << 1) | ((low >> bit) & 1ULL);
    if (top != 0 || remainder >= divisor) {
      remainder -= divisor;
      quotient |= 1ULL << static_cast<unsigned>(bit);
    }
  }
  return quotient;
}

uint64_t bytes_per_second(uint64_t current, uint64_t reference, int64_t elapsed_ms) {
  if (elapsed_ms <= 0 || current < reference) {
    return 0;
  }
  const uint64_t delta = current - reference;
  const auto elapsed = static_cast<uint64_t>(elapsed_ms);
  if (elapsed == 1000) {
    return delta;
  }
  uint64_t high = 0;
  const uint64_t low = multiply_u64(delta, 1000, high);
  return divide_u128(high, low, elapsed);
}

}

Series::Series(size_t capacity) : capacity_(capacity) {}

void Series::observe(uint64_t total) {
  uint64_t delta = 0;
  if (last_ && total >= *last_) {
    delta = total - *last_;
  }
  last_ = total;
  if (capacity_ > 0 && samples_.size() == capacity_) {
    samples_.pop_front();
  }
  samples_.push_back(delta);
}

std::vector<uint64_t> Series::samples() const {
  return {samples_.begin(), samples_.end()};
}

MonitorState::MonitorState() : total_(kGraphCapacity) {}

void MonitorState::update(const std::vector<ClientSnapshot>& clients,
                          std::chrono::steady_clock::time_point now) {
  std::set<std::string> seen;
  uint64_t total_now = 0;
  for (const ClientSnapshot& client : clients) {
    const std::string mac = ascii_lower(client.mac);
    const uint64_t bytes = saturating_add(client.rx_bytes, client.tx_bytes);
    total_now = saturating_add(total_now, bytes);
    seen.insert(mac);
    auto [entry, inserted] = clients_.try_emplace(mac);
    Tracked& tracked = entry->second;
    if (inserted) {
      tracked.traffic.mac = mac;
      tracked.traffic.graph = Series(kGraphCapacity);
      tracked.ref_rx = client.rx_bytes;
      tracked.ref_tx = client.tx_bytes;
      tracked.ref_time = now;
      tracked.has_ref = true;
      tracked.traffic.rx_rate = 0;
      tracked.traffic.tx_rate = 0;
    } else if (tracked.has_ref && now - tracked.ref_time >= std::chrono::seconds(1)) {
      const auto elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - tracked.ref_time).count();
      tracked.traffic.rx_rate = bytes_per_second(client.rx_bytes, tracked.ref_rx, elapsed_ms);
      tracked.traffic.tx_rate = bytes_per_second(client.tx_bytes, tracked.ref_tx, elapsed_ms);
      tracked.ref_rx = client.rx_bytes;
      tracked.ref_tx = client.tx_bytes;
      tracked.ref_time = now;
    }
    tracked.traffic.ip = client.ip;
    tracked.traffic.rx_bytes = client.rx_bytes;
    tracked.traffic.tx_bytes = client.tx_bytes;
    tracked.traffic.graph.observe(bytes);
  }
  for (auto it = clients_.begin(); it != clients_.end();) {
    if (!seen.contains(it->first)) {
      it = clients_.erase(it);
    } else {
      ++it;
    }
  }
  total_.observe(total_now);
}

std::vector<ClientTraffic> MonitorState::clients() const {
  std::vector<ClientTraffic> result;
  result.reserve(clients_.size());
  for (const auto& [mac, tracked] : clients_) {
    (void)mac;
    result.push_back(tracked.traffic);
  }
  return result;
}

uint64_t MonitorState::total_rate(uint64_t ClientTraffic::*field) const {
  uint64_t sum = 0;
  for (const auto& entry : clients_) {
    sum = saturating_add(sum, entry.second.traffic.*field);
  }
  return sum;
}

uint64_t MonitorState::total_rx_rate() const { return total_rate(&ClientTraffic::rx_rate); }

uint64_t MonitorState::total_tx_rate() const { return total_rate(&ClientTraffic::tx_rate); }

std::vector<uint64_t> MonitorState::total_samples() const { return total_.samples(); }

void MonitorState::clear() {
  clients_.clear();
  total_ = Series(kGraphCapacity);
}

std::vector<ClientSnapshot> parse_station_dump(std::string_view text) {
  std::vector<ClientSnapshot> clients;
  std::optional<ClientSnapshot> current;
  auto push_line = [&](std::string_view raw) {
    const std::string trimmed = trim_copy(raw);
    if (trimmed.starts_with("Station ")) {
      if (current) {
        clients.push_back(*current);
      }
      const std::string rest = trimmed.substr(8);
      const auto parts = split_ws(rest);
      current = ClientSnapshot{};
      current->mac = ascii_lower(parts.empty() ? "" : parts[0]);
      return;
    }
    if (!current) {
      return;
    }
    if (auto value = value_after(trimmed, "rx bytes:")) {
      current->rx_bytes = parse_counter(*value);
    } else if (auto tx = value_after(trimmed, "tx bytes:")) {
      current->tx_bytes = parse_counter(*tx);
    }
  };
  for_each_line(text, [&](std::string_view line) {
    push_line(line);
    return true;
  });
  if (current) {
    clients.push_back(*current);
  }
  return clients;
}

std::vector<std::pair<std::string, std::string>> parse_neigh(std::string_view text) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for_each_line(text, [&](std::string_view line) {
    const auto parts = split_ws(line);
    const auto position = std::find(parts.begin(), parts.end(), "lladdr");
    if (position != parts.end() && !parts.empty()) {
      const size_t index = static_cast<size_t>(position - parts.begin());
      if (index + 1 < parts.size()) {
        pairs.emplace_back(parts[0], ascii_lower(parts[index + 1]));
      }
    }
    return true;
  });
  return pairs;
}

std::vector<ClientSnapshot> clients_from_text(std::string_view dump, std::string_view neigh) {
  auto clients = parse_station_dump(dump);
  const auto pairs = parse_neigh(neigh);
  for (ClientSnapshot& client : clients) {
    const auto found = std::find_if(pairs.begin(), pairs.end(), [&](const auto& pair) {
      return pair.second == client.mac;
    });
    if (found != pairs.end()) {
      client.ip = found->first;
    }
  }
  return clients;
}

}
