#pragma once

#include "app.hpp"

#include <string>
#include <vector>

namespace hotmon {

enum class Style { Plain, Border, Title, Accent, Good, Warn, Dim };

struct StyledRow {
  std::string text;
  Style style = Style::Plain;
};

std::vector<std::string> render(const App& app, int width, int height);
std::vector<StyledRow> render_styled(const App& app, int width, int height);
std::string sparkline_text(const std::vector<uint64_t>& samples, int width);

}
