#include "format.hpp"

namespace hotmon {
namespace {

std::string with_tenth(uint64_t whole, uint64_t tenths, const char* unit) {
  return std::to_string(whole) + "." + std::to_string(tenths) + " " + unit;
}

}

std::string format_bytes(uint64_t bytes) {
  if (bytes < 1024) {
    return std::to_string(bytes) + " B";
  }
  uint64_t scale = 1024;
  int unit = 0;
  while (unit < 3 && bytes >= scale * 1024) {
    scale *= 1024;
    ++unit;
  }
  const uint64_t whole = bytes / scale;
  const uint64_t tenths = (bytes % scale) * 10 / scale;
  static const char* names[] = {"KiB", "MiB", "GiB", "TiB"};
  return with_tenth(whole, tenths, names[unit]);
}

std::string format_rate(uint64_t bytes_per_second) {
  return format_bytes(bytes_per_second) + "/s";
}

}
