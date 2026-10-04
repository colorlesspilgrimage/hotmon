#pragma once

#include "result.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hotmon {

inline constexpr const char* CAPTURE_WARNING =
    "Packet capture reads frames on the hotspot interface only. The program does not change "
    "packet contents. Press Enter to start capture.";

enum class CapturePhase { Idle, Warned, Armed, Running };

enum class ConfirmKind { Warning, Open, Already, Ignored };

struct ConfirmResult {
  ConfirmKind kind = ConfirmKind::Ignored;
  std::string iface;

  bool operator==(const ConfirmResult&) const = default;
};

class FrameSource {
 public:
  virtual ~FrameSource() = default;
  virtual Result<std::optional<std::vector<uint8_t>>> try_recv() = 0;
};

struct PacketSummary {
  size_t length = 0;
  std::string source;
  std::string destination;
  std::string protocol;

  std::string text() const;
};

class CaptureControl {
 public:
  CapturePhase phase() const;
  bool is_running() const;
  bool is_warned() const;
  std::optional<std::string> bound_interface() const;
  std::optional<std::string> armed_interface() const;
  std::vector<std::string> lines() const;
  std::vector<std::string> recent_lines(size_t count) const;

  Result<ConfirmResult> warn(std::string_view hotspot_iface);
  Result<ConfirmResult> accept(std::string_view hotspot_iface);
  void dismiss_warning();
  Result<void> attach(std::unique_ptr<FrameSource> source);
  std::string fail_open(std::string message);
  void stop();
  Result<void> poll();

 private:
  CapturePhase phase_ = CapturePhase::Idle;
  std::string iface_;
  std::deque<std::string> lines_;
  std::unique_ptr<FrameSource> source_;
};

Result<std::string> capture_interface(std::string_view requested, std::string_view hotspot);
PacketSummary summarize(std::span<const uint8_t> frame);

}
