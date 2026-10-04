#pragma once

#include "iface.hpp"
#include "result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace hotmon {

enum class SecurityMode { Open, Wpa2, Wpa3 };

enum class Band { Band24, Band5 };

const char* as_str(SecurityMode mode);
const char* as_str(Band band);

class Ipv4Network {
 public:
  static Result<Ipv4Network> parse(std::string_view text);
  uint8_t prefix() const;
  Result<std::string> gateway() const;
  std::string netmask() const;
  bool usable(uint32_t ip) const;
  Result<uint32_t> contains_text(std::string_view text) const;
  uint32_t address() const;

 private:
  Ipv4Network(uint32_t address, uint8_t prefix);

  uint32_t address_ = 0;
  uint8_t prefix_ = 0;
};

struct Profile {
  std::string ap_interface;
  std::string ssid;
  SecurityMode security = SecurityMode::Wpa2;
  std::string passphrase;
  Band band = Band::Band24;
  uint16_t channel = 0;
  std::string address_cidr;
  bool dhcp_enabled = false;
  std::string dhcp_start;
  std::string dhcp_end;
  std::string upstream_interface;

  bool operator==(const Profile&) const = default;

  Result<void> check_settings() const;
  Result<Ipv4Network> network() const;
  std::vector<std::string> review_lines() const;
};

Result<uint32_t> parse_ipv4(std::string_view text);
std::string format_ipv4(uint32_t address);
Result<SecurityMode> parse_security(std::string_view text);
Result<Band> parse_band(std::string_view text);
Result<void> validate_ssid(std::string_view ssid);
Result<void> validate_passphrase(SecurityMode mode, std::string_view passphrase);
bool channel_allowed(Band band, uint16_t channel);
std::vector<int> allowed_channels(Band band);
Result<uint16_t> parse_channel(Band band, std::string_view text);
Result<bool> parse_dhcp_flag(std::string_view text);
Result<void> validate_interface(std::string_view name, const std::vector<IfaceInfo>& interfaces);
Result<std::string> validate_upstream(std::string_view name, std::string_view ap_interface,
                                      const std::vector<IfaceInfo>& interfaces);
Result<std::tuple<std::string, bool, std::string, std::string>> validate_address_dhcp(
    std::string_view cidr, std::string_view dhcp_text, std::string_view start,
    std::string_view end);

inline constexpr const char* OPEN_UPSTREAM_WARNING =
    "Every device in radio range can use the upstream network.";

std::filesystem::path profile_path_from(std::optional<std::string> xdg,
                                        std::optional<std::string> home);
std::filesystem::path default_profile_path();
Result<void> save_profile(const std::filesystem::path& path, const Profile& profile);
Result<Profile> load_profile(const std::filesystem::path& path);
Result<std::optional<Profile>> load_optional(const std::filesystem::path& path);

}
