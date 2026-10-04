#pragma once

#include "app.hpp"

#include <string>
#include <vector>

namespace hotmon {

std::vector<std::string> render(const App& app, int width, int height);
std::string sparkline_text(const std::vector<uint64_t>& samples, int width);

}  // namespace hotmon
