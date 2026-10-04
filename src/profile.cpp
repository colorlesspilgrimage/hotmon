#include <algorithm>
#include <cctype>
#include "profile.hpp"

#include <yyjson.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <utility>

namespace hotmon {

Result<std::string> profile_to_json(const Profile& profile);
Result<Profile> profile_from_json(std::string_view text);
namespace {

std::string errno_text() { return std::strerror(errno); }

std::string trim_copy(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
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

uint32_t prefix_mask(uint8_t prefix) {
  if (prefix == 0) {
    return 0;
  }
  if (prefix >= 32) {
    return 0xFFFFFFFFU;
  }
  return 0xFFFFFFFFU << (32 - prefix);
}

bool is_unicode_control(char32_t code) {
  return code <= 0x1F || (code >= 0x7F && code <= 0x9F);
}

// Decode strict UTF-8. Reject malformed bytes, because they can hide a newline or a slash.
bool has_disallowed_ssid_char(std::string_view ssid) {
  for (size_t index = 0; index < ssid.size();) {
    const unsigned char byte = static_cast<unsigned char>(ssid[index]);
    char32_t code = 0;
    size_t width = 1;
    char32_t minimum = 0;
    if (byte < 0x80) {
      code = byte;
    } else if ((byte & 0xE0) == 0xC0) {
      code = byte & 0x1F;
      width = 2;
      minimum = 0x80;
    } else if ((byte & 0xF0) == 0xE0) {
      code = byte & 0x0F;
      width = 3;
      minimum = 0x800;
    } else if ((byte & 0xF8) == 0xF0) {
      code = byte & 0x07;
      width = 4;
      minimum = 0x10000;
    } else {
      return true;
    }
    if (index + width > ssid.size()) {
      return true;
    }
    for (size_t next = 1; next < width; ++next) {
      const unsigned char part = static_cast<unsigned char>(ssid[index + next]);
      if ((part & 0xC0) != 0x80) {
        return true;
      }
      code = (code << 6) | (part & 0x3F);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
      return true;
    }
    if (is_unicode_control(code) || code == '/') {
      return true;
    }
    index += width;
  }
  return false;
}

// Write a file in an open folder. The program often runs as root, so it does not follow links.
// It does not truncate a file that has a second hard link.
Result<void> write_mode(int dir_fd, const std::filesystem::path& path, std::string_view contents,
                        mode_t mode) {
  const std::string name = path.filename().string();
  const int fd = ::openat(dir_fd, name.c_str(),
                          O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, mode);
  if (fd < 0) {
    return unexpected_text("The program cannot write " + path.string() + ". " + errno_text());
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
    ::close(fd);
    return unexpected_text("The program cannot write " + path.string() +
                           ". The file is not a private regular file.");
  }
  if (::fchmod(fd, mode) != 0 || ::ftruncate(fd, 0) != 0) {
    const std::string message = errno_text();
    ::close(fd);
    return unexpected_text("The program cannot protect " + path.string() + ". " + message);
  }
  size_t written = 0;
  while (written < contents.size()) {
    const ssize_t count = ::write(fd, contents.data() + written, contents.size() - written);
    if (count < 0) {
      const std::string message = errno_text();
      ::close(fd);
      return unexpected_text("The program cannot write " + path.string() + ". " + message);
    }
    written += static_cast<size_t>(count);
  }
  ::close(fd);
  return {};
}

std::string json_error(std::string detail) {
  return "The profile file is not valid. " + std::move(detail);
}

std::string string_field(yyjson_val* root, const char* key, std::string& error) {
  yyjson_val* value = yyjson_obj_get(root, key);
  if (value == nullptr) {
    error = json_error(std::string("missing field ") + key);
    return {};
  }
  if (!yyjson_is_str(value)) {
    error = json_error(std::string("field ") + key + " has the wrong type");
    return {};
  }
  return std::string(yyjson_get_str(value), yyjson_get_len(value));
}

}  // namespace

const char* as_str(SecurityMode mode) {
  switch (mode) {
    case SecurityMode::Open:
      return "open";
    case SecurityMode::Wpa2:
      return "wpa2";
    case SecurityMode::Wpa3:
      return "wpa3";
  }
  return "open";
}

const char* as_str(Band band) {
  switch (band) {
    case Band::Band24:
      return "2.4";
    case Band::Band5:
      return "5";
  }
  return "2.4";
}

Ipv4Network::Ipv4Network(uint32_t address, uint8_t prefix) : address_(address), prefix_(prefix) {}

Result<Ipv4Network> Ipv4Network::parse(std::string_view text) {
  const size_t slash = text.find('/');
  if (slash == std::string_view::npos) {
    return unexpected_text("The address range needs a prefix. Use the form 192.168.42.0/24.");
  }
  auto address = parse_ipv4(text.substr(0, slash));
  if (!address) {
    return unexpected_text(address.error());
  }
  const std::string prefix_text(text.substr(slash + 1));
  if (prefix_text.empty() ||
      std::find_if(prefix_text.begin(), prefix_text.end(), [](char ch) {
        return std::isdigit(static_cast<unsigned char>(ch)) == 0;
      }) != prefix_text.end()) {
    return unexpected_text("The prefix is not a number.");
  }
  int prefix = 0;
  try {
    prefix = std::stoi(prefix_text);
  } catch (const std::exception&) {
    return unexpected_text("The prefix is not a number.");
  }
  if (prefix < 8 || prefix > 30) {
    return unexpected_text("The prefix must be from 8 to 30.");
  }
  const auto bits = static_cast<uint8_t>(prefix);
  const uint32_t network = *address & prefix_mask(bits);
  if (network != *address) {
    return unexpected_text(
        "The address range must be a network address, for example 192.168.42.0/24.");
  }
  return Ipv4Network(*address, bits);
}

uint8_t Ipv4Network::prefix() const { return prefix_; }

uint32_t Ipv4Network::address() const { return address_; }

Result<std::string> Ipv4Network::gateway() const {
  const uint32_t gateway = address_ == 0xFFFFFFFFU ? address_ : address_ + 1;
  if (!usable(gateway)) {
    return unexpected_text("The address range has no gateway address.");
  }
  return format_ipv4(gateway);
}

std::string Ipv4Network::netmask() const { return format_ipv4(prefix_mask(prefix_)); }

bool Ipv4Network::usable(uint32_t ip) const {
  const uint32_t mask = prefix_mask(prefix_);
  const uint32_t broadcast = address_ | ~mask;
  return (ip & mask) == address_ && ip != address_ && ip != broadcast;
}

Result<uint32_t> Ipv4Network::contains_text(std::string_view text) const {
  auto ip = parse_ipv4(text);
  if (!ip) {
    return unexpected_text(ip.error());
  }
  if (!usable(*ip)) {
    return unexpected_text("The address " + std::string(text) + " is outside the address range.");
  }
  return *ip;
}

Result<uint32_t> parse_ipv4(std::string_view text) {
  uint32_t value = 0;
  size_t index = 0;
  for (int part = 0; part < 4; ++part) {
    if (index >= text.size() || std::isdigit(static_cast<unsigned char>(text[index])) == 0) {
      return unexpected_text("The address " + std::string(text) + " is not a valid IPv4 address.");
    }
    if (text[index] == '0' && index + 1 < text.size() &&
        std::isdigit(static_cast<unsigned char>(text[index + 1])) != 0) {
      return unexpected_text("The address " + std::string(text) + " is not a valid IPv4 address.");
    }
    int number = 0;
    const size_t start = index;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
      number = number * 10 + (text[index] - '0');
      if (number > 255 || index - start > 2) {
        return unexpected_text("The address " + std::string(text) +
                               " is not a valid IPv4 address.");
      }
      ++index;
    }
    value = (value << 8) | static_cast<uint32_t>(number);
    if (part < 3) {
      if (index >= text.size() || text[index] != '.') {
        return unexpected_text("The address " + std::string(text) +
                               " is not a valid IPv4 address.");
      }
      ++index;
    }
  }
  if (index != text.size()) {
    return unexpected_text("The address " + std::string(text) + " is not a valid IPv4 address.");
  }
  return value;
}

std::string format_ipv4(uint32_t address) {
  return std::to_string((address >> 24) & 0xFF) + "." + std::to_string((address >> 16) & 0xFF) +
         "." + std::to_string((address >> 8) & 0xFF) + "." + std::to_string(address & 0xFF);
}

Result<SecurityMode> parse_security(std::string_view text) {
  const std::string value = ascii_lower(trim_copy(text));
  if (value == "open") {
    return SecurityMode::Open;
  }
  if (value == "wpa2") {
    return SecurityMode::Wpa2;
  }
  if (value == "wpa3") {
    return SecurityMode::Wpa3;
  }
  return unexpected_text("The security mode must be open, wpa2, or wpa3.");
}

Result<Band> parse_band(std::string_view text) {
  const std::string value = ascii_lower(trim_copy(text));
  if (value == "2.4" || value == "2.4ghz") {
    return Band::Band24;
  }
  if (value == "5" || value == "5ghz") {
    return Band::Band5;
  }
  return unexpected_text("The band must be 2.4 or 5.");
}

Result<void> validate_ssid(std::string_view ssid) {
  if (ssid.empty() || ssid == "." || ssid == ".." || ssid.size() > 32) {
    return unexpected_text("The SSID must contain 1 to 32 characters.");
  }
  if (has_disallowed_ssid_char(ssid)) {
    return unexpected_text("The SSID contains a character that is not allowed.");
  }
  return {};
}

Result<void> validate_passphrase(SecurityMode mode, std::string_view passphrase) {
  if (mode == SecurityMode::Open) {
    if (passphrase.empty()) {
      return {};
    }
    return unexpected_text("An open network does not use a passphrase.");
  }
  const size_t length = passphrase.size();
  size_t chars = 0;
  for (size_t index = 0; index < passphrase.size();) {
    ++chars;
    const unsigned char byte = static_cast<unsigned char>(passphrase[index]);
    if (byte < 0x80) {
      ++index;
    } else if ((byte & 0xE0) == 0xC0) {
      index += 2;
    } else if ((byte & 0xF0) == 0xE0) {
      index += 3;
    } else {
      index += 4;
    }
  }
  if (chars < 8 || chars > 63) {
    return unexpected_text("The passphrase must contain 8 to 63 characters.");
  }
  if (!std::all_of(passphrase.begin(), passphrase.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte >= 0x20 && byte <= 0x7E;
      })) {
    return unexpected_text("The passphrase must use printable ASCII characters.");
  }
  (void)length;
  if (passphrase.front() == ' ' || passphrase.back() == ' ') {
    return unexpected_text("The passphrase must not start or end with a space.");
  }
  if (passphrase.find('\\') != std::string_view::npos ||
      passphrase.find('"') != std::string_view::npos ||
      passphrase.find('#') != std::string_view::npos) {
    return unexpected_text(
        "The passphrase must not contain a backslash, a double quote, or a hash.");
  }
  return {};
}

bool channel_allowed(Band band, uint16_t channel) {
  if (band == Band::Band24) {
    return channel >= 1 && channel <= 13;
  }
  switch (channel) {
    case 36:
    case 40:
    case 44:
    case 48:
    case 52:
    case 56:
    case 60:
    case 64:
    case 100:
    case 104:
    case 108:
    case 112:
    case 116:
    case 120:
    case 124:
    case 128:
    case 132:
    case 136:
    case 140:
    case 144:
    case 149:
    case 153:
    case 157:
    case 161:
    case 165:
      return true;
    default:
      return false;
  }
}

std::vector<int> allowed_channels(Band band) {
  std::vector<int> channels;
  if (band == Band::Band24) {
    for (int channel = 1; channel <= 13; ++channel) {
      channels.push_back(channel);
    }
    return channels;
  }
  for (int channel : {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132,
                      136, 140, 144, 149, 153, 157, 161, 165}) {
    channels.push_back(channel);
  }
  return channels;
}

Result<uint16_t> parse_channel(Band band, std::string_view text) {
  const std::string value = trim_copy(text);
  if (value.empty() || std::find_if(value.begin(), value.end(), [](char ch) {
                         return std::isdigit(static_cast<unsigned char>(ch)) == 0;
                       }) != value.end()) {
    return unexpected_text("The channel is not a number.");
  }
  unsigned long channel = 0;
  try {
    channel = std::stoul(value);
  } catch (const std::exception&) {
    return unexpected_text("The channel is not a number.");
  }
  if (channel > 65535 || !channel_allowed(band, static_cast<uint16_t>(channel))) {
    return unexpected_text("The channel " + std::to_string(channel) +
                           " is not valid for the " + as_str(band) + " GHz band.");
  }
  return static_cast<uint16_t>(channel);
}

Result<bool> parse_dhcp_flag(std::string_view text) {
  const std::string value = ascii_lower(trim_copy(text));
  if (value == "on" || value == "yes" || value == "true") {
    return true;
  }
  if (value == "off" || value == "no" || value == "false") {
    return false;
  }
  return unexpected_text("DHCP must be on or off.");
}

Result<void> validate_interface(std::string_view name, const std::vector<IfaceInfo>& interfaces) {
  const auto found = std::find_if(interfaces.begin(), interfaces.end(), [&](const IfaceInfo& info) {
    return info.name == name;
  });
  if (found == interfaces.end()) {
    return unexpected_text("The interface " + std::string(name) + " is not available.");
  }
  return require_ap(*found);
}

Result<std::string> validate_upstream(std::string_view name, std::string_view ap_interface,
                                      const std::vector<IfaceInfo>& interfaces) {
  if (ascii_lower(std::string(name)) == "none") {
    return std::string("none");
  }
  if (!valid_name(name)) {
    return unexpected_text("The upstream interface name is not valid.");
  }
  if (name == ap_interface) {
    return unexpected_text(
        "The upstream interface must be different from the access-point interface.");
  }
  const bool found = std::any_of(interfaces.begin(), interfaces.end(), [&](const IfaceInfo& info) {
    return info.name == name;
  });
  if (!found) {
    return unexpected_text("The interface " + std::string(name) + " is not available.");
  }
  return std::string(name);
}

Result<std::tuple<std::string, bool, std::string, std::string>> validate_address_dhcp(
    std::string_view cidr, std::string_view dhcp_text, std::string_view start,
    std::string_view end) {
  auto network = Ipv4Network::parse(trim_copy(cidr));
  if (!network) {
    return unexpected_text(network.error());
  }
  auto dhcp_enabled = parse_dhcp_flag(dhcp_text);
  if (!dhcp_enabled) {
    return unexpected_text(dhcp_enabled.error());
  }
  auto gateway = network->gateway();
  if (!gateway) {
    return unexpected_text(gateway.error());
  }
  auto gateway_ip = parse_ipv4(*gateway);
  if (!gateway_ip) {
    return unexpected_text(gateway_ip.error());
  }
  if (!*dhcp_enabled) {
    return std::tuple{trim_copy(cidr), false, std::string(), std::string()};
  }
  if (trim_copy(start).empty() || trim_copy(end).empty()) {
    return unexpected_text("Enter the DHCP start address and the DHCP end address.");
  }
  auto start_ip = network->contains_text(trim_copy(start));
  if (!start_ip) {
    return unexpected_text("The DHCP start address " + std::string(start) +
                           " is outside the address range.");
  }
  auto end_ip = network->contains_text(trim_copy(end));
  if (!end_ip) {
    return unexpected_text("The DHCP end address " + std::string(end) +
                           " is outside the address range.");
  }
  if (*start_ip > *end_ip) {
    return unexpected_text("The DHCP start address must not be after the DHCP end address.");
  }
  if (*start_ip == *gateway_ip || *end_ip == *gateway_ip ||
      (*start_ip <= *gateway_ip && *gateway_ip <= *end_ip)) {
    return unexpected_text("The DHCP range must not include the hotspot address " + *gateway +
                           ".");
  }
  return std::tuple{trim_copy(cidr), true, trim_copy(start), trim_copy(end)};
}

Result<void> Profile::check_settings() const {
  if (!valid_name(ap_interface)) {
    return unexpected_text("The access-point interface name is not valid.");
  }
  if (auto ssid_result = validate_ssid(ssid); !ssid_result) {
    return ssid_result;
  }
  if (auto pass_result = validate_passphrase(security, passphrase); !pass_result) {
    return pass_result;
  }
  if (!channel_allowed(band, channel)) {
    return unexpected_text("The channel " + std::to_string(channel) + " is not valid for the " +
                           as_str(band) + " GHz band.");
  }
  if (auto dhcp = validate_address_dhcp(address_cidr, dhcp_enabled ? "on" : "off", dhcp_start,
                                        dhcp_end);
      !dhcp) {
    return unexpected_text(dhcp.error());
  }
  if (ascii_lower(upstream_interface) == "none") {
    return {};
  }
  if (!valid_name(upstream_interface)) {
    return unexpected_text("The upstream interface name is not valid.");
  }
  if (upstream_interface == ap_interface) {
    return unexpected_text(
        "The upstream interface must be different from the access-point interface.");
  }
  return {};
}

Result<Ipv4Network> Profile::network() const { return Ipv4Network::parse(address_cidr); }

std::vector<std::string> Profile::review_lines() const {
  const std::string dhcp = dhcp_enabled ? "on " + dhcp_start + "-" + dhcp_end : "off";
  std::vector<std::string> lines = {
      "Access-point interface: " + ap_interface,
      "SSID: " + ssid,
      "Security: " + std::string(as_str(security)),
      security == SecurityMode::Open ? "Passphrase: the network is open" : "Passphrase: set",
      "Band: " + std::string(as_str(band)),
      "Channel: " + std::to_string(channel),
      "Address range: " + address_cidr,
      "DHCP: " + dhcp,
      "Upstream interface: " + upstream_interface,
  };
  if (security == SecurityMode::Open && upstream_interface != "none") {
    lines.emplace_back(OPEN_UPSTREAM_WARNING);
  }
  return lines;
}

std::filesystem::path profile_path_from(std::optional<std::string> xdg,
                                        std::optional<std::string> home) {
  if (xdg && !xdg->empty()) {
    return std::filesystem::path(*xdg) / "hotmon" / "profile.json";
  }
  if (home && !home->empty()) {
    return std::filesystem::path(*home) / ".config" / "hotmon" / "profile.json";
  }
  return std::filesystem::path(".config") / "hotmon" / "profile.json";
}

std::filesystem::path default_profile_path() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  const char* home = std::getenv("HOME");
  return profile_path_from(xdg == nullptr ? std::nullopt : std::optional<std::string>(xdg),
                           home == nullptr ? std::nullopt : std::optional<std::string>(home));
}

Result<void> save_profile(const std::filesystem::path& path, const Profile& profile) {
  auto text = profile_to_json(profile);
  if (!text) {
    return unexpected_text(text.error());
  }
  const std::filesystem::path parent = path.parent_path();
  if (parent.empty()) {
    return write_mode(AT_FDCWD, path, *text + "\n", 0600);
  }
  std::error_code error;
  std::filesystem::create_directories(parent, error);
  if (error) {
    return unexpected_text("The program cannot prepare " + parent.string() + ". " +
                           error.message());
  }
  // Do not follow a link at the last folder. A link can point to a system folder.
  const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dir_fd < 0) {
    return unexpected_text("The program cannot protect " + parent.string() + ". " + errno_text());
  }
  if (::fchmod(dir_fd, 0700) != 0) {
    const std::string message = errno_text();
    ::close(dir_fd);
    return unexpected_text("The program cannot protect " + parent.string() + ". " + message);
  }
  auto written = write_mode(dir_fd, path, *text + "\n", 0600);
  ::close(dir_fd);
  return written;
}

Result<std::string> profile_to_json(const Profile& profile) {
  yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
  if (doc == nullptr) {
    return unexpected_text("The profile cannot be encoded. out of memory");
  }
  yyjson_mut_val* root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_str(doc, root, "ap_interface", profile.ap_interface.c_str());
  yyjson_mut_obj_add_str(doc, root, "ssid", profile.ssid.c_str());
  yyjson_mut_obj_add_str(doc, root, "security", as_str(profile.security));
  yyjson_mut_obj_add_str(doc, root, "passphrase", profile.passphrase.c_str());
  yyjson_mut_obj_add_str(doc, root, "band", as_str(profile.band));
  yyjson_mut_obj_add_uint(doc, root, "channel", profile.channel);
  yyjson_mut_obj_add_str(doc, root, "address_cidr", profile.address_cidr.c_str());
  yyjson_mut_obj_add_bool(doc, root, "dhcp_enabled", profile.dhcp_enabled);
  yyjson_mut_obj_add_str(doc, root, "dhcp_start", profile.dhcp_start.c_str());
  yyjson_mut_obj_add_str(doc, root, "dhcp_end", profile.dhcp_end.c_str());
  yyjson_mut_obj_add_str(doc, root, "upstream_interface", profile.upstream_interface.c_str());
  size_t length = 0;
  char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &length);
  yyjson_mut_doc_free(doc);
  if (json == nullptr) {
    return unexpected_text("The profile cannot be encoded. write failed");
  }
  std::string text(json, length);
  std::free(json);
  return text;
}

Result<Profile> profile_from_json(std::string_view text) {
  yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
  if (doc == nullptr) {
    return unexpected_text(json_error("the text is not JSON"));
  }
  yyjson_val* root = yyjson_doc_get_root(doc);
  if (root == nullptr || !yyjson_is_obj(root)) {
    yyjson_doc_free(doc);
    return unexpected_text(json_error("the root is not an object"));
  }
  std::string error;
  std::string ap = string_field(root, "ap_interface", error);
  std::string ssid = string_field(root, "ssid", error);
  const std::string security = string_field(root, "security", error);
  std::string passphrase = string_field(root, "passphrase", error);
  const std::string band = string_field(root, "band", error);
  std::string address = string_field(root, "address_cidr", error);
  std::string dhcp_start = string_field(root, "dhcp_start", error);
  std::string dhcp_end = string_field(root, "dhcp_end", error);
  std::string upstream = string_field(root, "upstream_interface", error);
  yyjson_val* channel = yyjson_obj_get(root, "channel");
  yyjson_val* dhcp = yyjson_obj_get(root, "dhcp_enabled");
  if (!error.empty()) {
    yyjson_doc_free(doc);
    return unexpected_text(error);
  }
  if (channel == nullptr) {
    yyjson_doc_free(doc);
    return unexpected_text(json_error("missing field channel"));
  }
  if (!yyjson_is_int(channel) && !yyjson_is_uint(channel)) {
    yyjson_doc_free(doc);
    return unexpected_text(json_error("field channel has the wrong type"));
  }
  const int64_t channel_number = yyjson_get_sint(channel);
  if (channel_number < 0 || channel_number > 65535) {
    yyjson_doc_free(doc);
    return unexpected_text(json_error("field channel is out of range"));
  }
  if (dhcp == nullptr) {
    yyjson_doc_free(doc);
    return unexpected_text(json_error("missing field dhcp_enabled"));
  }
  if (!yyjson_is_bool(dhcp)) {
    yyjson_doc_free(doc);
    return unexpected_text(json_error("field dhcp_enabled has the wrong type"));
  }
  Profile profile;
  profile.ap_interface = std::move(ap);
  profile.ssid = std::move(ssid);
  if (security == "open") {
    profile.security = SecurityMode::Open;
  } else if (security == "wpa2") {
    profile.security = SecurityMode::Wpa2;
  } else if (security == "wpa3") {
    profile.security = SecurityMode::Wpa3;
  } else {
    yyjson_doc_free(doc);
    return unexpected_text(json_error("field security is not valid"));
  }
  profile.passphrase = std::move(passphrase);
  if (band == "2.4") {
    profile.band = Band::Band24;
  } else if (band == "5") {
    profile.band = Band::Band5;
  } else {
    yyjson_doc_free(doc);
    return unexpected_text(json_error("field band is not valid"));
  }
  profile.channel = static_cast<uint16_t>(channel_number);
  profile.address_cidr = std::move(address);
  profile.dhcp_enabled = yyjson_get_bool(dhcp);
  profile.dhcp_start = std::move(dhcp_start);
  profile.dhcp_end = std::move(dhcp_end);
  profile.upstream_interface = std::move(upstream);
  yyjson_doc_free(doc);
  return profile;
}

Result<Profile> load_profile(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return unexpected_text("The program cannot read " + path.string() + ". " + errno_text());
  }
  std::string text;
  char chunk[4096];
  while (true) {
    const ssize_t count = ::read(fd, chunk, sizeof(chunk));
    if (count == 0) {
      break;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      const std::string message = errno_text();
      ::close(fd);
      return unexpected_text("The program cannot read " + path.string() + ". " + message);
    }
    text.append(chunk, static_cast<size_t>(count));
  }
  ::close(fd);
  return profile_from_json(text);
}

Result<std::optional<Profile>> load_optional(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return std::optional<Profile>();
  }
  auto profile = load_profile(path);
  if (!profile) {
    return unexpected_text(profile.error());
  }
  return std::optional<Profile>(std::move(*profile));
}

}  // namespace hotmon
