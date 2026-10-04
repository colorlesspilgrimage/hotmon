#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hotmon {

struct Choice {
  std::string label;
  std::string value;
};

class SelectBox {
 public:
  SelectBox() = default;
  explicit SelectBox(std::vector<Choice> choices, size_t selected = 0);

  void up();
  void down();
  const Choice& current() const;
  bool select_value(std::string_view value);
  const std::vector<Choice>& choices() const;
  size_t index() const;
  bool empty() const;
  void add(Choice choice);

 private:
  std::vector<Choice> choices_;
  size_t index_ = 0;
};

}  // namespace hotmon
