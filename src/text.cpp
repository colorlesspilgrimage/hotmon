#include "text.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>

namespace hotmon {

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

std::string ascii_lower(std::string text) {
  for (char& ch : text) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return text;
}

std::string errno_text(int err) { return std::strerror(err); }

std::string errno_text() { return errno_text(errno); }

uint32_t prefix_mask(uint8_t prefix) {
  if (prefix == 0) {
    return 0;
  }
  if (prefix >= 32) {
    return 0xFFFFFFFFU;
  }
  return 0xFFFFFFFFU << (32 - prefix);
}

std::vector<std::string> split_ws(std::string_view text) {
  std::vector<std::string> parts;
  size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
      ++index;
    }
    if (index >= text.size()) {
      break;
    }
    const size_t start = index;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) == 0) {
      ++index;
    }
    parts.emplace_back(text.substr(start, index - start));
  }
  return parts;
}

}
