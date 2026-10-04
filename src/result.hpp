#pragma once

#include <expected>
#include <string>
#include <utility>

namespace hotmon {

template <typename T>
using Result = std::expected<T, std::string>;

inline auto unexpected_text(std::string text) {
  return std::unexpected(std::move(text));
}

}
