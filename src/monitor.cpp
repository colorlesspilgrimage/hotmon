#include <algorithm>
#include "monitor.hpp"

#include <cctype>
#include <set>
#include <charconv>
#include <limits>

namespace hotmon {
namespace {

constexpr size_t kGraphCapacity = 40;

std::string trim_copy(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string ascii_lower(std::string text) {
  for (char& ch : text) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return text;
}

std::optional<std::string> value_after(std::string_view line, std::string_view label) {
  if (!line.starts_with(label)) {
    return std::nullopt;
  }
  return trim_copy(line.substr(label.size()));
}

std::vector<std::string> split_ws(std::string_view text) {
  std::vector<std::string> parts;
  size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
      ++index;
    }
    if (index >= text.size()) {
      break;
    }
    const size_t start = index;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) == 0) {
      ++index;
    }
    parts.emplace_back(text.substr(start, index - start));
  }
  return parts;
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
    const uint64_t bytes = saturating_add(client.rx_bytes, client.tx_bytes);
    total_now = saturating_add(total_now, bytes);
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
  size_t start = 0;
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
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    push_line(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  if (current) {
    clients.push_back(*current);
  }
  return clients;
}

std::vector<std::pair<std::string, std::string>> parse_neigh(std::string_view text) {
  std::vector<std::pair<std::string, std::string>> pairs;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    const std::string_view line =
        text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    const auto parts = split_ws(line);
    const auto position = std::find(parts.begin(), parts.end(), "lladdr");
    if (position != parts.end() && !parts.empty()) {
      const size_t index = static_cast<size_t>(position - parts.begin());
      if (index + 1 < parts.size()) {
        pairs.emplace_back(parts[0], ascii_lower(parts[index + 1]));
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
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
