#include <algorithm>
#include "monitor.hpp"

#include "text.hpp"

#include <set>

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
  const std::string token(text.substr(0, text.find_first_of(" \t")));
  if (token.empty()) {
    return 0;
  }
  try {
    return std::stoull(token);
  } catch (const std::exception&) {
    return 0;
  }
}


}  // namespace

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

void MonitorState::update(const std::vector<ClientSnapshot>& clients) {
  std::set<std::string> seen;
  uint64_t total_now = 0;
  for (const ClientSnapshot& client : clients) {
    const std::string mac = ascii_lower(client.mac);
    const uint64_t bytes = client.rx_bytes + client.tx_bytes;
    total_now += bytes;
    seen.insert(mac);
    auto [entry, inserted] = clients_.try_emplace(mac);
    if (inserted) {
      entry->second.mac = mac;
      entry->second.graph = Series(kGraphCapacity);
    }
    entry->second.ip = client.ip;
    entry->second.rx_bytes = client.rx_bytes;
    entry->second.tx_bytes = client.tx_bytes;
    entry->second.graph.observe(bytes);
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
  for (const auto& [mac, client] : clients_) {
    (void)mac;
    result.push_back(client);
  }
  return result;
}

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

}  // namespace hotmon
