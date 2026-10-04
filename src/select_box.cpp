#include "select_box.hpp"

namespace hotmon {

SelectBox::SelectBox(std::vector<Choice> choices, size_t selected) : choices_(std::move(choices)) {
  if (choices_.empty()) {
    index_ = 0;
  } else if (selected >= choices_.size()) {
    index_ = choices_.size() - 1;
  } else {
    index_ = selected;
  }
}

void SelectBox::up() {
  if (index_ > 0) {
    --index_;
  }
}

void SelectBox::down() {
  if (!choices_.empty() && index_ + 1 < choices_.size()) {
    ++index_;
  }
}

const Choice& SelectBox::current() const {
  static const Choice empty;
  if (choices_.empty()) {
    return empty;
  }
  return choices_[index_];
}

bool SelectBox::select_value(std::string_view value) {
  for (size_t index = 0; index < choices_.size(); ++index) {
    if (choices_[index].value == value) {
      index_ = index;
      return true;
    }
  }
  return false;
}

const std::vector<Choice>& SelectBox::choices() const { return choices_; }

size_t SelectBox::index() const { return index_; }

bool SelectBox::empty() const { return choices_.empty(); }

void SelectBox::add(Choice choice) { choices_.push_back(std::move(choice)); }

}
