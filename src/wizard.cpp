#include "wizard.hpp"

#include <cctype>
#include <tuple>

#include <algorithm>

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

SelectBox security_box(std::string_view selected = "wpa2") {
  SelectBox box({{"Open", "open"}, {"WPA2", "wpa2"}, {"WPA3", "wpa3"}}, 1);
  box.select_value(selected);
  return box;
}

SelectBox band_box() {
  return SelectBox({{"5 GHz", "5"}, {"Increased compatibility", "compat"}}, 0);
}

bool standard_radio(Band band, uint16_t channel) {
  return (band == Band::Band5 && channel == 36) || (band == Band::Band24 && channel == 6);
}

std::string saved_label(Band band, uint16_t channel) {
  return "Saved setting (" + std::string(as_str(band)) + " GHz, channel " +
         std::to_string(channel) + ")";
}

std::vector<int> channels_for(const IfaceInfo& ap, Band band) {
  const auto& listed = band == Band::Band24 ? ap.channels_24 : ap.channels_5;
  if (ap.channels_24.empty() && ap.channels_5.empty()) {
    return band == Band::Band24 ? allowed_channels(Band::Band24) : std::vector<int>{};
  }
  return listed;
}

SelectBox channel_box(const std::vector<int>& channels, uint16_t selected) {
  std::vector<Choice> choices;
  for (int channel : channels) {
    const std::string text = std::to_string(channel);
    choices.push_back(Choice{text, text});
  }
  SelectBox box(std::move(choices), 0);
  box.select_value(std::to_string(selected));
  return box;
}

SelectBox dhcp_box(bool enabled) {
  SelectBox box({{"On", "on"}, {"Off", "off"}}, enabled ? 0 : 1);
  return box;
}

void add_if_missing(SelectBox& box, std::string label, std::string value) {
  if (!box.select_value(value)) {
    box.add(Choice{std::move(label), value});
    box.select_value(value);
  }
}

std::string* typed_target(Page page, size_t field, std::string& ssid, std::string& passphrase,
                          std::string& address, std::string& start, std::string& end) {
  if (page == Page::Ssid) {
    return &ssid;
  }
  if (page == Page::Passphrase) {
    return &passphrase;
  }
  if (page != Page::Advanced) {
    return nullptr;
  }
  if (field == 2) {
    return &address;
  }
  if (field == 4) {
    return &start;
  }
  if (field == 5) {
    return &end;
  }
  return nullptr;
}

size_t char_count(std::string_view text) {
  return static_cast<size_t>(std::count_if(text.begin(), text.end(), [](char ch) {
    return (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
  }));
}

void append_utf8(std::string& text, char32_t ch) {
  if (ch < 0x80) {
    text.push_back(static_cast<char>(ch));
  } else if (ch < 0x800) {
    text.push_back(static_cast<char>(0xC0 | (ch >> 6)));
    text.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
  } else if (ch < 0x10000) {
    text.push_back(static_cast<char>(0xE0 | (ch >> 12)));
    text.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
  } else {
    text.push_back(static_cast<char>(0xF0 | (ch >> 18)));
    text.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
  }
}

}

const char* page_title(Page page) {
  switch (page) {
    case Page::Interface:
      return "Access-point interface";
    case Page::Ssid:
      return "SSID";
    case Page::Security:
      return "Security mode";
    case Page::Passphrase:
      return "Passphrase";
    case Page::Upstream:
      return "Upstream interface";
    case Page::BandChannel:
      return "Band and channel";
    case Page::AddressDhcp:
      return "Address range and DHCP";
    case Page::Advanced:
      return "Advanced setup";
    case Page::Review:
      return "Review";
  }
  return "Review";
}

Wizard Wizard::make(const HostFacts& facts) {
  Wizard wizard;
  std::vector<Choice> ap_choices;
  for (const IfaceInfo& info : ap_candidates(facts.interfaces)) {
    ap_choices.push_back(Choice{info.name, info.name});
  }
  wizard.ap_interface = SelectBox(std::move(ap_choices), 0);
  wizard.security = security_box();
  wizard.band_choice = band_box();
  wizard.rebuild_upstream(facts);
  return wizard;
}

void Wizard::rebuild_upstream(const HostFacts& facts) {
  const std::string previous = upstream.empty() ? "none" : upstream.current().value;
  std::vector<Choice> choices;
  const std::string ap = ap_interface.empty() ? "" : ap_interface.current().value;
  for (const IfaceInfo& info : upstream_candidates(facts.interfaces, ap)) {
    choices.push_back(Choice{info.name, info.name});
  }
  choices.push_back(Choice{"None", "none"});
  const size_t none_index = choices.size() - 1;
  upstream = SelectBox(std::move(choices), none_index);
  if (previous != "none") {
    upstream.select_value(previous);
  }
}

void Wizard::rebuild_band_choice() {
  const std::string previous = band_choice.empty() ? "" : band_choice.current().value;
  std::vector<Choice> choices;
  if (radio_source == NetSource::Saved && !standard_radio(band, channel)) {
    choices.push_back(Choice{saved_label(band, channel), "saved"});
  }
  choices.push_back(Choice{"5 GHz", "5"});
  choices.push_back(Choice{"Increased compatibility", "compat"});
  size_t selected = 0;
  if (radio_source == NetSource::Saved && standard_radio(band, channel)) {
    selected = band == Band::Band5 ? static_cast<size_t>(choices.size() - 2)
                                   : static_cast<size_t>(choices.size() - 1);
  }
  band_choice = SelectBox(std::move(choices), selected);
  if (!previous.empty() && previous != "saved") {
    band_choice.select_value(previous);
  }
}

const IfaceInfo* Wizard::selected_ap(const HostFacts& facts) const {
  if (ap_interface.empty()) {
    return nullptr;
  }
  const std::string name = ap_interface.current().value;
  const auto found = std::find_if(facts.interfaces.begin(), facts.interfaces.end(),
                                  [&](const IfaceInfo& info) { return info.name == name; });
  if (found == facts.interfaces.end()) {
    return nullptr;
  }
  return &*found;
}

void Wizard::apply_radio(const IfaceInfo& ap, bool compatibility) {
  const AutoRadio radio = choose_radio(ap, compatibility);
  band = radio.band;
  channel = radio.channel;
  radio_fell_back = radio.fell_back;
  radio_source = NetSource::Automatic;
  if (radio.fell_back) {
    radio_note = ap.supports_5ghz ? NOTE_NO_CHANNEL_36 : NOTE_NO_5GHZ;
  } else {
    radio_note.clear();
  }
}

Result<void> Wizard::fill_automatic_network(const HostFacts& facts) {
  const std::string ap = ap_interface.empty() ? "" : ap_interface.current().value;
  std::vector<Ipv4Range> used;
  if (facts.local_networks) {
    used = facts.local_networks(ap);
  }
  auto network = choose_network(used);
  if (!network) {
    return unexpected_text(network.error());
  }
  address_cidr = network->address_cidr;
  dhcp_enabled = true;
  dhcp_start = network->dhcp_start;
  dhcp_end = network->dhcp_end;
  network_source = NetSource::Automatic;
  return {};
}

void Wizard::rebuild_advanced(const HostFacts& facts) {
  const IfaceInfo* ap = selected_ap(facts);
  IfaceInfo fallback;
  if (ap == nullptr) {
    fallback.name = ap_interface.empty() ? "" : ap_interface.current().value;
    ap = &fallback;
  }
  std::vector<Choice> bands;
  const bool empty_radio = ap->channels_24.empty() && ap->channels_5.empty();
  if (empty_radio || !ap->channels_24.empty()) {
    bands.push_back(Choice{"2.4 GHz", "2.4"});
  }
  if (!empty_radio && !ap->channels_5.empty()) {
    bands.push_back(Choice{"5 GHz", "5"});
  }
  adv_band = SelectBox(std::move(bands), 0);
  adv_band.select_value(as_str(band));
  const Band selected = adv_band.current().value == "5" ? Band::Band5 : Band::Band24;
  adv_channel = channel_box(channels_for(*ap, selected), channel);
  adv_dhcp = dhcp_box(dhcp_enabled);
  adv_address_cidr = address_cidr;
  adv_dhcp_start = dhcp_start;
  adv_dhcp_end = dhcp_end;
}

Wizard Wizard::from_profile(const Profile& profile, const HostFacts& facts) {
  Wizard wizard = make(facts);
  wizard.page = Page::Review;
  add_if_missing(wizard.ap_interface, profile.ap_interface, profile.ap_interface);
  wizard.security.select_value(as_str(profile.security));
  wizard.ssid = profile.ssid;
  wizard.passphrase = profile.passphrase;
  wizard.band = profile.band;
  wizard.channel = profile.channel;
  wizard.address_cidr = profile.address_cidr;
  wizard.dhcp_enabled = profile.dhcp_enabled;
  wizard.dhcp_start = profile.dhcp_start;
  wizard.dhcp_end = profile.dhcp_end;
  wizard.radio_source = NetSource::Saved;
  wizard.network_source = NetSource::Saved;
  wizard.rebuild_upstream(facts);
  add_if_missing(wizard.upstream, profile.upstream_interface, profile.upstream_interface);
  wizard.rebuild_band_choice();
  return wizard;
}

std::pair<size_t, size_t> Wizard::position() const {
  size_t number = 1;
  switch (page) {
    case Page::Interface:
      number = 1;
      break;
    case Page::Ssid:
      number = 2;
      break;
    case Page::Security:
      number = 3;
      break;
    case Page::Passphrase:
      number = 4;
      break;
    case Page::Upstream:
      number = 5;
      break;
    case Page::BandChannel:
      number = 6;
      break;
    case Page::AddressDhcp:
    case Page::Advanced:
      number = 7;
      break;
    case Page::Review:
      number = 8;
      break;
  }
  return {number, 8};
}

bool Wizard::open_upstream_risk() const {
  return !security.empty() && security.current().value == "open" && !upstream.empty() &&
         upstream.current().value != "none";
}

std::string Wizard::hint() const {
  switch (page) {
    case Page::Interface:
      return "Choose the access-point interface.";
    case Page::Ssid:
      return "Enter the SSID. Use 1 to 32 characters.";
    case Page::Security:
      return "Choose Open, WPA2, or WPA3.";
    case Page::Passphrase:
      return "Enter the passphrase. Use 8 to 63 characters. Leave this empty for open.";
    case Page::Upstream:
      return "Choose the upstream interface, or none.";
    case Page::BandChannel:
      return "Choose 5 GHz or increased compatibility.";
    case Page::AddressDhcp:
      if (network_source == NetSource::Saved) {
        return "Saved values. Press d for automatic values or a for advanced setup.";
      }
      return "The wizard chose these values. Press Enter to accept, or a for advanced setup.";
    case Page::Advanced:
      return "Set the band, the channel, the address range, and DHCP.";
    case Page::Review:
      if (open_upstream_risk()) {
        return std::string(OPEN_UPSTREAM_WARNING) +
               " Press y to apply. Enter does not apply this warning.";
      }
      return "Review the settings. Enter applies the profile.";
  }
  return {};
}

std::vector<FieldLine> Wizard::field_lines() const {
  auto selection = [&](const char* label, const SelectBox& box, bool active) {
    FieldLine line;
    line.label = label;
    line.active = active;
    line.selection = true;
    line.choices = box.choices();
    line.selected = box.index();
    if (!box.empty()) {
      line.value = box.current().label;
    }
    return line;
  };
  auto typed = [&](const char* label, const std::string& value, bool active) {
    return FieldLine{label, value, active, false, {}, 0};
  };
  switch (page) {
    case Page::Interface:
      return {selection("Access-point interface", ap_interface, true)};
    case Page::Ssid:
      return {typed("SSID", ssid, true)};
    case Page::Security:
      return {selection("Security mode", security, true)};
    case Page::Passphrase:
      return {typed("Passphrase", std::string(char_count(passphrase), '*'), true)};
    case Page::Upstream:
      return {selection("Upstream interface", upstream, true)};
    case Page::BandChannel:
      return {selection("Band and channel", band_choice, true)};
    case Page::AddressDhcp:
      return {typed("Address range", address_cidr, false),
              typed("DHCP", dhcp_enabled ? "on" : "off", false),
              typed("DHCP start", dhcp_start, false), typed("DHCP end", dhcp_end, false)};
    case Page::Advanced:
      return {selection("Band", adv_band, field == 0),
              selection("Channel", adv_channel, field == 1),
              typed("Address range", adv_address_cidr, field == 2),
              selection("DHCP", adv_dhcp, field == 3),
              typed("DHCP start", adv_dhcp_start, field == 4),
              typed("DHCP end", adv_dhcp_end, field == 5)};
    case Page::Review:
      return {};
  }
  return {};
}

std::vector<std::string> Wizard::review_lines() const {
  Profile draft;
  draft.ap_interface = ap_interface.empty() ? "" : ap_interface.current().value;
  draft.ssid = ssid;
  draft.security = SecurityMode::Wpa2;
  if (!security.empty()) {
    if (auto mode = parse_security(security.current().value)) {
      draft.security = *mode;
    }
  }
  draft.passphrase = passphrase;
  draft.band = band;
  draft.channel = channel;
  draft.address_cidr = address_cidr;
  draft.dhcp_enabled = dhcp_enabled;
  draft.dhcp_start = dhcp_start;
  draft.dhcp_end = dhcp_end;
  draft.upstream_interface = upstream.empty() ? "none" : upstream.current().value;
  auto lines = draft.review_lines();
  if (radio_fell_back && !radio_note.empty()) {
    const auto channel_line = std::find_if(lines.begin(), lines.end(), [](const std::string& line) {
      return line.starts_with("Channel:");
    });
    if (channel_line != lines.end()) {
      lines.insert(channel_line + 1, radio_note);
    }
  }
  const char* source = "Settings source: automatic";
  if (network_source == NetSource::Advanced) {
    source = "Settings source: set by the user (advanced setup)";
  } else if (network_source == NetSource::Saved) {
    source = "Settings source: saved profile";
  }
  lines.emplace_back(source);
  return lines;
}

void Wizard::push_char(char32_t ch) {
  if (ch < 0x20 || (ch >= 0x7F && ch <= 0x9F) || (ch >= 0xD800 && ch <= 0xDFFF) || ch > 0x10FFFF) {
    return;
  }
  cancelled = false;
  std::string* target = typed_target(page, field, ssid, passphrase, adv_address_cidr,
                                     adv_dhcp_start, adv_dhcp_end);
  if (target == nullptr || char_count(*target) >= 128) {
    return;
  }
  append_utf8(*target, ch);
}

void Wizard::backspace() {
  cancelled = false;
  std::string* target = typed_target(page, field, ssid, passphrase, adv_address_cidr,
                                     adv_dhcp_start, adv_dhcp_end);
  if (target == nullptr) {
    return;
  }
  while (!target->empty()) {
    const auto byte = static_cast<unsigned char>(target->back());
    target->pop_back();
    if ((byte & 0xC0) != 0x80) {
      break;
    }
  }
}

void Wizard::next_field() {
  if (page != Page::Advanced) {
    return;
  }
  field = (field + 1) % 6;
}

void Wizard::move_selection(int delta, const HostFacts& facts) {
  auto move_box = [&](SelectBox& box) {
    if (delta < 0) {
      box.up();
    } else {
      box.down();
    }
  };
  if (page == Page::Interface) {
    move_box(ap_interface);
  } else if (page == Page::Security) {
    move_box(security);
  } else if (page == Page::Upstream) {
    move_box(upstream);
  } else if (page == Page::BandChannel) {
    move_box(band_choice);
  } else if (page == Page::Advanced) {
    if (field == 0) {
      move_box(adv_band);
      const IfaceInfo* ap = selected_ap(facts);
      IfaceInfo fallback;
      const IfaceInfo& info = ap == nullptr ? fallback : *ap;
      const Band selected = adv_band.current().value == "5" ? Band::Band5 : Band::Band24;
      const std::string previous = adv_channel.empty() ? "" : adv_channel.current().value;
      adv_channel = channel_box(channels_for(info, selected), 0);
      adv_channel.select_value(previous);
    } else if (field == 1) {
      move_box(adv_channel);
    } else if (field == 3) {
      move_box(adv_dhcp);
    } else if (delta < 0 && field > 0) {
      --field;
    } else if (delta > 0 && field + 1 < 6) {
      ++field;
    }
  }
}

void Wizard::move_up(const HostFacts& facts) { move_selection(-1, facts); }

void Wizard::move_down(const HostFacts& facts) { move_selection(1, facts); }

Result<void> Wizard::fail(std::string message) {
  error = message;
  return unexpected_text(std::move(message));
}

Result<void> Wizard::validate_current(const HostFacts& facts) const {
  switch (page) {
    case Page::Interface:
      if (ap_interface.empty()) {
        return unexpected_text(NO_AP_INTERFACE);
      }
      return validate_interface(ap_interface.current().value, facts.interfaces);
    case Page::Ssid:
      return validate_ssid(trim_copy(ssid));
    case Page::Security:
      return {};
    case Page::Passphrase: {
      auto mode = parse_security(security.empty() ? "wpa2" : security.current().value);
      if (!mode) {
        return unexpected_text(mode.error());
      }
      return validate_passphrase(*mode, passphrase);
    }
    case Page::Upstream: {
      auto upstream_name = validate_upstream(upstream.empty() ? "none" : upstream.current().value,
                                             ap_interface.empty() ? "" : ap_interface.current().value,
                                             facts.interfaces);
      if (!upstream_name) {
        return unexpected_text(upstream_name.error());
      }
      return {};
    }
    case Page::BandChannel:
    case Page::AddressDhcp:
    case Page::Review:
      return {};
    case Page::Advanced:
      return {};
  }
  return {};
}

Result<void> Wizard::next(const HostFacts& facts) {
  if (cancelled) {
    return fail(CANCELLED);
  }
  if (page == Page::Review) {
    return fail("Use confirm on the review page.");
  }
  if (page == Page::AddressDhcp) {
    page = Page::Review;
    field = 0;
    error.reset();
    return {};
  }
  if (page == Page::Advanced) {
    return confirm_advanced(facts);
  }
  if (auto checked = validate_current(facts); !checked) {
    return fail(checked.error());
  }
  if (page == Page::Ssid) {
    ssid = trim_copy(ssid);
  }
  if (page == Page::Interface) {
    rebuild_upstream(facts);
  }
  if (page == Page::Upstream) {
    page = Page::BandChannel;
    field = 0;
    error.reset();
    rebuild_band_choice();
    return {};
  }
  if (page == Page::BandChannel) {
    if (!band_choice.empty() && band_choice.current().value == "saved") {
      radio_source = NetSource::Saved;
      radio_fell_back = false;
      radio_note.clear();
    } else if (const IfaceInfo* ap = selected_ap(facts); ap != nullptr) {
      apply_radio(*ap, !band_choice.empty() && band_choice.current().value == "compat");
    } else {
      apply_radio(IfaceInfo{}, !band_choice.empty() && band_choice.current().value == "compat");
    }
    if (network_source == NetSource::Automatic) {
      if (auto filled = fill_automatic_network(facts); !filled) {
        return fail(filled.error());
      }
    }
    page = Page::AddressDhcp;
    field = 0;
    error.reset();
    return {};
  }
  if (page == Page::Interface) {
    page = Page::Ssid;
  } else if (page == Page::Ssid) {
    page = Page::Security;
  } else if (page == Page::Security) {
    page = Page::Passphrase;
  } else if (page == Page::Passphrase) {
    page = Page::Upstream;
  }
  field = 0;
  error.reset();
  return {};
}

void Wizard::back() {
  cancelled = false;
  error.reset();
  field = 0;
  switch (page) {
    case Page::Interface:
      return;
    case Page::Ssid:
      page = Page::Interface;
      break;
    case Page::Security:
      page = Page::Ssid;
      break;
    case Page::Passphrase:
      page = Page::Security;
      break;
    case Page::Upstream:
      page = Page::Passphrase;
      break;
    case Page::BandChannel:
      page = Page::Upstream;
      break;
    case Page::AddressDhcp:
      page = Page::BandChannel;
      break;
    case Page::Advanced:
      page = Page::AddressDhcp;
      break;
    case Page::Review:
      page = network_source == NetSource::Advanced ? Page::Advanced : Page::AddressDhcp;
      break;
  }
}

void Wizard::cancel() {
  cancelled = true;
  error.reset();
}

bool Wizard::is_cancelled() const { return cancelled; }

void Wizard::reopen() {
  cancelled = false;
  error.reset();
}

void Wizard::set_error(std::string message) { error = std::move(message); }

Result<void> Wizard::open_advanced(const HostFacts& facts) {
  rebuild_advanced(facts);
  page = Page::Advanced;
  field = 0;
  error.reset();
  return {};
}

Result<void> Wizard::use_automatic(const HostFacts& facts) {
  radio_source = NetSource::Automatic;
  network_source = NetSource::Automatic;
  adv_address_cidr.clear();
  adv_dhcp_start.clear();
  adv_dhcp_end.clear();
  if (const IfaceInfo* ap = selected_ap(facts); ap != nullptr) {
    apply_radio(*ap, false);
  }
  rebuild_band_choice();
  band_choice.select_value("5");
  if (auto filled = fill_automatic_network(facts); !filled) {
    return fail(filled.error());
  }
  page = Page::AddressDhcp;
  return {};
}

Result<void> Wizard::confirm_advanced(const HostFacts& facts) {
  const std::string dhcp = adv_dhcp.empty() || adv_dhcp.current().value == "on" ? "on" : "off";
  auto checked = validate_address_dhcp(adv_address_cidr, dhcp, adv_dhcp_start, adv_dhcp_end);
  if (!checked) {
    return fail(checked.error());
  }
  if (adv_channel.empty()) {
    return fail("The interface has no channel for this band.");
  }
  address_cidr = std::get<0>(*checked);
  dhcp_enabled = std::get<1>(*checked);
  dhcp_start = std::get<2>(*checked);
  dhcp_end = std::get<3>(*checked);
  band = adv_band.current().value == "5" ? Band::Band5 : Band::Band24;
  channel = static_cast<uint16_t>(std::stoi(adv_channel.current().value));
  radio_source = NetSource::Advanced;
  network_source = NetSource::Advanced;
  radio_fell_back = false;
  radio_note.clear();
  (void)facts;
  page = Page::Review;
  field = 0;
  error.reset();
  return {};
}

Result<Profile> Wizard::confirmed_profile(const HostFacts& facts) {
  if (cancelled) {
    error = CANCELLED;
    return unexpected_text(CANCELLED);
  }
  if (page != Page::Review) {
    return fail_profile("Confirm the settings on the review page.");
  }
  if (ap_interface.empty()) {
    return fail_profile(NO_AP_INTERFACE);
  }
  const std::string ap = ap_interface.current().value;
  if (auto checked = validate_interface(ap, facts.interfaces); !checked) {
    return fail_profile(checked.error());
  }
  if (auto ssid_result = validate_ssid(trim_copy(ssid)); !ssid_result) {
    return fail_profile(ssid_result.error());
  }
  auto mode = parse_security(security.empty() ? "" : security.current().value);
  if (!mode) {
    return fail_profile(mode.error());
  }
  if (auto pass = validate_passphrase(*mode, passphrase); !pass) {
    return fail_profile(pass.error());
  }
  auto address = validate_address_dhcp(address_cidr, dhcp_enabled ? "on" : "off", dhcp_start,
                                       dhcp_end);
  if (!address) {
    return fail_profile(address.error());
  }
  auto upstream_name = validate_upstream(upstream.empty() ? "none" : upstream.current().value, ap,
                                         facts.interfaces);
  if (!upstream_name) {
    return fail_profile(upstream_name.error());
  }
  Profile profile;
  profile.ap_interface = ap;
  profile.ssid = trim_copy(ssid);
  profile.security = *mode;
  profile.passphrase = passphrase;
  profile.band = band;
  profile.channel = channel;
  profile.address_cidr = std::get<0>(*address);
  profile.dhcp_enabled = std::get<1>(*address);
  profile.dhcp_start = std::get<2>(*address);
  profile.dhcp_end = std::get<3>(*address);
  profile.upstream_interface = *upstream_name;
  if (auto checked = profile.check_settings(); !checked) {
    return fail_profile(checked.error());
  }
  error.reset();
  return profile;
}

Result<Profile> Wizard::fail_profile(std::string message) {
  error = message;
  return unexpected_text(std::move(message));
}

}
