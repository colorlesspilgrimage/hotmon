#include "capture.hpp"

#include <format>
#include <sstream>

namespace hotmon {
namespace {

PacketSummary make_summary(size_t length, std::string source, std::string destination,
                           std::string protocol) {
  return PacketSummary{length, std::move(source), std::move(destination), std::move(protocol)};
}

PacketSummary unknown_summary(size_t length, const char* protocol) {
  return make_summary(length, "-", "-", protocol);
}

std::string format_mac(std::span<const uint8_t> bytes) {
  std::string text;
  for (size_t index = 0; index < bytes.size(); ++index) {
    if (index != 0) {
      text.push_back(':');
    }
    text += std::format("{:02x}", bytes[index]);
  }
  return text;
}

std::string port_text(const char* name, std::span<const uint8_t> ip, size_t ihl) {
  if (ip.size() < ihl + 4) {
    return name;
  }
  const uint16_t src = static_cast<uint16_t>((ip[ihl] << 8) | ip[ihl + 1]);
  const uint16_t dst = static_cast<uint16_t>((ip[ihl + 2] << 8) | ip[ihl + 3]);
  return std::format("{} {}->{}", name, src, dst);
}

PacketSummary summarize_ipv4(std::span<const uint8_t> frame, size_t header) {
  if (frame.size() < header + 20) {
    return unknown_summary(frame.size(), "truncated");
  }
  const auto ip = frame.subspan(header);
  const size_t ihl = static_cast<size_t>(ip[0] & 0x0F) * 4;
  if (ihl < 20 || ip.size() < ihl) {
    return unknown_summary(frame.size(), "truncated");
  }
  const uint8_t protocol = ip[9];
  const std::string source = std::format("{}.{}.{}.{}", ip[12], ip[13], ip[14], ip[15]);
  const std::string destination = std::format("{}.{}.{}.{}", ip[16], ip[17], ip[18], ip[19]);
  std::string proto;
  switch (protocol) {
    case 1:
      proto = "ICMP";
      break;
    case 6:
      proto = port_text("TCP", ip, ihl);
      break;
    case 17:
      proto = port_text("UDP", ip, ihl);
      break;
    default:
      proto = std::format("ip {}", protocol);
      break;
  }
  return make_summary(frame.size(), source, destination, proto);
}

}

std::string PacketSummary::text() const {
  return std::format("{} B {} -> {} {}", length, source, destination, protocol);
}

CapturePhase CaptureControl::phase() const { return phase_; }

bool CaptureControl::is_running() const { return phase_ == CapturePhase::Running; }

bool CaptureControl::is_warned() const { return phase_ == CapturePhase::Warned; }

std::optional<std::string> CaptureControl::bound_interface() const {
  if (phase_ == CapturePhase::Idle || iface_.empty()) {
    return std::nullopt;
  }
  return iface_;
}

std::optional<std::string> CaptureControl::armed_interface() const {
  if (phase_ == CapturePhase::Armed) {
    return iface_;
  }
  return std::nullopt;
}

std::vector<std::string> CaptureControl::lines() const {
  return {lines_.begin(), lines_.end()};
}

std::vector<std::string> CaptureControl::recent_lines(size_t count) const {
  std::vector<std::string> result;
  for (auto it = lines_.rbegin(); it != lines_.rend() && result.size() < count; ++it) {
    result.push_back(*it);
  }
  return result;
}

Result<ConfirmResult> CaptureControl::warn(std::string_view hotspot_iface) {
  auto iface = capture_interface(hotspot_iface, hotspot_iface);
  if (!iface) {
    return unexpected_text(iface.error());
  }
  if (phase_ == CapturePhase::Armed || phase_ == CapturePhase::Running) {
    return ConfirmResult{ConfirmKind::Already, {}};
  }
  if (phase_ == CapturePhase::Warned && iface_ != *iface) {
    iface_ = *iface;
    return ConfirmResult{ConfirmKind::Warning, {}};
  }
  iface_ = *iface;
  phase_ = CapturePhase::Warned;
  return ConfirmResult{ConfirmKind::Warning, {}};
}

Result<ConfirmResult> CaptureControl::accept(std::string_view hotspot_iface) {
  auto iface = capture_interface(hotspot_iface, hotspot_iface);
  if (!iface) {
    return unexpected_text(iface.error());
  }
  if (phase_ != CapturePhase::Warned) {
    return ConfirmResult{ConfirmKind::Ignored, {}};
  }
  if (iface_ != *iface) {
    iface_ = *iface;
    phase_ = CapturePhase::Warned;
    return ConfirmResult{ConfirmKind::Warning, {}};
  }
  phase_ = CapturePhase::Armed;
  return ConfirmResult{ConfirmKind::Open, iface_};
}

void CaptureControl::dismiss_warning() {
  if (phase_ == CapturePhase::Warned) {
    phase_ = CapturePhase::Idle;
    iface_.clear();
  }
}

Result<void> CaptureControl::attach(std::unique_ptr<FrameSource> source) {
  if (phase_ != CapturePhase::Armed) {
    return unexpected_text("Capture is not confirmed.");
  }
  source_ = std::move(source);
  phase_ = CapturePhase::Running;
  return {};
}

std::string CaptureControl::fail_open(std::string message) {
  stop();
  return "Capture did not start. " + message;
}

void CaptureControl::stop() {
  phase_ = CapturePhase::Idle;
  iface_.clear();
  source_.reset();
}

Result<void> CaptureControl::poll() {
  if (phase_ != CapturePhase::Running || source_ == nullptr) {
    return {};
  }
  while (true) {
    auto frame = source_->try_recv();
    if (!frame) {
      stop();
      return unexpected_text("Capture stopped. " + frame.error());
    }
    if (!frame->has_value()) {
      break;
    }
    lines_.push_back(summarize(**frame).text());
    while (lines_.size() > 50) {
      lines_.pop_front();
    }
  }
  return {};
}

Result<std::string> capture_interface(std::string_view requested, std::string_view hotspot) {
  if (requested.empty() || hotspot.empty()) {
    return unexpected_text("The hotspot interface is not set.");
  }
  if (requested != hotspot) {
    return unexpected_text("Capture is allowed only on the hotspot interface.");
  }
  return std::string(hotspot);
}

PacketSummary summarize(std::span<const uint8_t> frame) {
  if (frame.size() < 14) {
    return unknown_summary(frame.size(), "short");
  }
  uint16_t ethertype = static_cast<uint16_t>((frame[12] << 8) | frame[13]);
  size_t header = 14;
  if (ethertype == 0x8100 && frame.size() >= 18) {
    ethertype = static_cast<uint16_t>((frame[16] << 8) | frame[17]);
    header = 18;
  }
  const std::string src_mac = format_mac(frame.subspan(6, 6));
  const std::string dst_mac = format_mac(frame.subspan(0, 6));
  switch (ethertype) {
    case 0x0800:
      return summarize_ipv4(frame, header);
    case 0x0806:
      return make_summary(frame.size(), src_mac, dst_mac, "ARP");
    case 0x86DD:
      return make_summary(frame.size(), src_mac, dst_mac, "IPv6");
    default:
      return make_summary(frame.size(), src_mac, dst_mac, std::format("eth {:04x}", ethertype));
  }
}

}
