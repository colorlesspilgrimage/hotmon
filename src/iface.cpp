#include "iface.hpp"

#include "process.hpp"
#include "profile.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace hotmon {
namespace {

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

bool parse_channel_line(std::string_view line, double& frequency, int& channel) {
  const size_t mhz = line.find("MHz");
  const size_t open = line.find('[');
  const size_t close = line.find(']');
  if (mhz == std::string_view::npos || open == std::string_view::npos ||
      close == std::string_view::npos || open >= close || mhz > open) {
    return false;
  }
  const std::string freq_text = trim_copy(line.substr(0, mhz));
  const size_t number = freq_text.find_first_of("0123456789");
  if (number == std::string::npos) {
    return false;
  }
  try {
    frequency = std::stod(freq_text.substr(number));
    channel = std::stoi(std::string(line.substr(open + 1, close - open - 1)));
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

void add_unique(std::vector<int>& values, int channel) {
  if (std::find(values.begin(), values.end(), channel) == values.end()) {
    values.push_back(channel);
  }
}

}

bool valid_name(std::string_view name) {
  if (name.empty() || name.size() > 15) {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(name[0]);
  if (std::isalpha(first) == 0 || first > 127) {
    return false;
  }
  for (size_t index = 1; index < name.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(name[index]);
    if (std::isalnum(ch) != 0) {
      continue;
    }
    if (ch != '_' && ch != '.' && ch != '-' && ch != ':') {
      return false;
    }
  }
  return true;
}

bool modes_support_ap(std::string_view text) {
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    const std::string line = trim_copy(text.substr(start, end == std::string_view::npos
                                                              ? std::string_view::npos
                                                              : end - start));
    if (line == "* AP" || line == "AP") {
      return true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

Result<void> require_ap(const IfaceInfo& info) {
  if (!info.wireless) {
    return unexpected_text(info.name + " is not a wireless interface.");
  }
  if (!info.supports_ap) {
    return unexpected_text(info.name + " cannot start an access point.");
  }
  return {};
}

PhyCaps parse_phy_info(std::string_view text) {
  PhyCaps caps;
  caps.supports_ap = modes_support_ap(text);
  bool in_frequencies = false;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    const std::string_view raw =
        text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    const std::string line = trim_copy(raw);
    if (line.find("Frequencies:") != std::string::npos) {
      in_frequencies = true;
    } else if (in_frequencies) {
      if (!line.empty() && line.back() == ':' && line.find("MHz") == std::string::npos) {
        in_frequencies = false;
      } else if (line.find("(disabled)") == std::string::npos &&
                 line.find("no IR") == std::string::npos) {
        double frequency = 0;
        int channel = 0;
        if (parse_channel_line(line, frequency, channel)) {
          if (frequency >= 2400.0 && frequency < 2500.0 && channel >= 1 && channel <= 14 &&
              channel_allowed(Band::Band24, static_cast<uint16_t>(channel))) {
            add_unique(caps.channels_24, channel);
          }
          if (frequency >= 5000.0 && frequency <= 5895.0 &&
              channel_allowed(Band::Band5, static_cast<uint16_t>(channel))) {
            add_unique(caps.channels_5, channel);
          }
        }
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  std::sort(caps.channels_24.begin(), caps.channels_24.end());
  std::sort(caps.channels_5.begin(), caps.channels_5.end());
  return caps;
}

PhyCaps SystemPhyInfo::caps(std::string_view phy) const {
  const auto result = run_capture({"iw", "phy", std::string(phy), "info"});
  if (!result || result->status != 0) {
    return {};
  }
  return parse_phy_info(result->out);
}

std::vector<IfaceInfo> read_interfaces_at(const std::filesystem::path& root, const PhyInfo& phys) {
  std::vector<IfaceInfo> infos;
  std::error_code error;
  if (!std::filesystem::is_directory(root, error)) {
    return infos;
  }
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(root, error)) {
    if (error) {
      break;
    }
    const std::string name = entry.path().filename().string();
    std::error_code entry_error;
    if (name == "lo" || !entry.is_directory(entry_error)) {
      continue;
    }
    const std::filesystem::path phy_link = entry.path() / "phy80211";
    std::string phy;
    std::error_code link_error;
    if (std::filesystem::is_symlink(phy_link, link_error)) {
      const std::filesystem::path target = std::filesystem::read_symlink(phy_link, link_error);
      if (!link_error) {
        phy = target.filename().string();
      }
    }
    const bool wireless =
        std::filesystem::exists(entry.path() / "wireless", entry_error) || !phy.empty();
    PhyCaps caps;
    if (!phy.empty()) {
      caps = phys.caps(phy);
    }
    IfaceInfo info;
    info.name = name;
    info.wireless = wireless;
    info.supports_ap = caps.supports_ap;
    info.channels_24 = caps.channels_24;
    info.channels_5 = caps.channels_5;
    info.supports_5ghz = !info.channels_5.empty();
    infos.push_back(std::move(info));
  }
  std::sort(infos.begin(), infos.end(),
            [](const IfaceInfo& left, const IfaceInfo& right) { return left.name < right.name; });
  return infos;
}

std::vector<IfaceInfo> read_system_interfaces() {
  return read_interfaces_at("/sys/class/net", SystemPhyInfo{});
}

std::vector<IfaceInfo> ap_candidates(const std::vector<IfaceInfo>& interfaces) {
  std::vector<IfaceInfo> result;
  for (const IfaceInfo& info : interfaces) {
    if (info.wireless && info.supports_ap) {
      result.push_back(info);
    }
  }
  return result;
}

std::vector<IfaceInfo> upstream_candidates(const std::vector<IfaceInfo>& interfaces,
                                           std::string_view ap) {
  std::vector<IfaceInfo> result;
  for (const IfaceInfo& info : interfaces) {
    if (info.name != ap) {
      result.push_back(info);
    }
  }
  return result;
}

}
