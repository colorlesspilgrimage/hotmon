#pragma once

#include "capture.hpp"

#include <array>
#include <string>

namespace hotmon {

class LocalCapture : public FrameSource {
 public:
  static Result<LocalCapture> open(std::string_view interface);
  Result<std::optional<std::vector<uint8_t>>> try_recv() override;

  LocalCapture(const LocalCapture&) = delete;
  LocalCapture& operator=(const LocalCapture&) = delete;
  LocalCapture(LocalCapture&& other) noexcept;
  LocalCapture& operator=(LocalCapture&& other) noexcept;
  ~LocalCapture() override;

 private:
  explicit LocalCapture(int fd);

  int fd_ = -1;
  std::array<uint8_t, 65535> buffer_{};
};

}  // namespace hotmon
