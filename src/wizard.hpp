#pragma once

#include "iface.hpp"
#include "netauto.hpp"
#include "profile.hpp"
#include "select_box.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hotmon {

inline constexpr const char* CANCELLED =
    "The wizard is cancelled. The settings were not applied.";
inline constexpr const char* NO_AP_INTERFACE = "No interface can start an access point.";
inline constexpr const char* NOTE_NO_5GHZ =
    "This interface has no 5 GHz radio. The wizard uses 2.4 GHz on channel 6.";
inline constexpr const char* NOTE_NO_CHANNEL_36 =
    "This interface cannot use 5 GHz channel 36. The wizard uses 2.4 GHz on channel 6.";

enum class Page {
  Interface,
  Ssid,
  Security,
  Passphrase,
  Upstream,
  BandChannel,
  AddressDhcp,
  Advanced,
  Review
};

enum class NetSource { Automatic, Advanced, Saved };

struct HostFacts {
  std::vector<IfaceInfo> interfaces;
  std::function<std::vector<Ipv4Range>(std::string_view)> local_networks;
};

struct FieldLine {
  std::string label;
  std::string value;
  bool active = false;
  bool selection = false;
  std::vector<Choice> choices;
  size_t selected = 0;
};

const char* page_title(Page page);

struct Wizard {
  Page page = Page::Interface;
  size_t field = 0;
  bool cancelled = false;
  std::optional<std::string> error;
  SelectBox ap_interface;
  SelectBox security;
  SelectBox upstream;
  SelectBox band_choice;
  std::string ssid;
  std::string passphrase;
  Band band = Band::Band5;
  uint16_t channel = 36;
  bool radio_fell_back = false;
  std::string radio_note;
  std::string address_cidr;
  bool dhcp_enabled = true;
  void rebuild_upstream(const HostFacts& facts);
  void rebuild_band_choice();
  const IfaceInfo* selected_ap(const HostFacts& facts) const;
  void apply_radio(const IfaceInfo& ap, bool compatibility);
  Result<void> fill_automatic_network(const HostFacts& facts);
  void rebuild_advanced(const HostFacts& facts);
  void move_selection(int delta, const HostFacts& facts);
  Result<void> fail(std::string message);
  Result<Profile> fail_profile(std::string message);
  Result<void> validate_current(const HostFacts& facts) const;
  Result<void> confirm_advanced(const HostFacts& facts);
  std::string dhcp_start;
  std::string dhcp_end;
  NetSource radio_source = NetSource::Automatic;
  NetSource network_source = NetSource::Automatic;
  SelectBox adv_band;
  SelectBox adv_channel;
  SelectBox adv_dhcp;
  std::string adv_address_cidr;
  std::string adv_dhcp_start;
  std::string adv_dhcp_end;

  static Wizard make(const HostFacts& facts);
  static Wizard from_profile(const Profile& profile, const HostFacts& facts);

  std::pair<size_t, size_t> position() const;
  bool open_upstream_risk() const;
  std::string hint() const;
  std::vector<FieldLine> field_lines() const;
  std::vector<std::string> review_lines() const;
  void push_char(char32_t ch);
  void backspace();
  void next_field();
  void move_up(const HostFacts& facts);
  void move_down(const HostFacts& facts);
  Result<void> next(const HostFacts& facts);
  void back();
  void cancel();
  bool is_cancelled() const;
  void reopen();
  void set_error(std::string message);
  Result<void> open_advanced(const HostFacts& facts);
  Result<void> use_automatic(const HostFacts& facts);
  Result<Profile> confirmed_profile(const HostFacts& facts);
};

}
