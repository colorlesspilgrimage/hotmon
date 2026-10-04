#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hotmon {

std::string trim_copy(std::string_view text);
std::string ascii_lower(std::string text);
std::string errno_text();
std::string errno_text(int err);
uint32_t prefix_mask(uint8_t prefix);
std::vector<std::string> split_ws(std::string_view text);

// Visit each line. Stop when visit returns false.
template <typename Visit>
void for_each_line(std::string_view text, Visit&& visit) {
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    const std::string_view line =
        text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (!visit(line)) {
      return;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
}

}
