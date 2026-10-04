#pragma once

#include "app.hpp"

namespace hotmon {

class Terminal {
 public:
  Terminal();
  ~Terminal();
  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;

  void draw(const App& app);
  int read_key(App& app, Key& key);
};

int run_ui(App& app);

}
