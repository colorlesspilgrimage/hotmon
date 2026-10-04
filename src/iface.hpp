#pragma once

#include "result.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hotmon {

struct IfaceInfo {
  std::string name;
  bool wireless = false;
  bool supports_ap = false;
  bool supports_5ghz = false;
  std::vector<int> channels_24;
  std::vector<int> channels_5;

  bool operator==(const IfaceInfo&) const = default;
};

struct PhyCaps {
  bool supports_ap = false;
  std::vector<int> channels_24;
  std::vector<int> channels_5;
};

class PhyInfo {
 public:
  virtual ~PhyInfo() = default;
  virtual PhyCaps caps(std::string_view phy) const = 0;
};

class SystemPhyInfo : public PhyInfo {
 public:
  PhyCaps caps(std::string_view phy) const override;
};

bool valid_name(std::string_view name);
bool modes_support_ap(std::string_view text);
Result<void> require_ap(const IfaceInfo& info);
PhyCaps parse_phy_info(std::string_view text);
std::vector<IfaceInfo> read_interfaces_at(const std::filesystem::path& root, const PhyInfo& phys);
std::vector<IfaceInfo> read_system_interfaces();
std::vector<IfaceInfo> ap_candidates(const std::vector<IfaceInfo>& interfaces);
std::vector<IfaceInfo> upstream_candidates(const std::vector<IfaceInfo>& interfaces,
                                           std::string_view ap);

}
