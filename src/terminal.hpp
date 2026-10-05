#pragma once

#include "app.hpp"
#include "privilege.hpp"
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

int run_ui(App& app, Privileged& privileged);
TerminalHooks terminal_hooks();
// SIGTERM, SIGINT, and SIGHUP restore the terminal and then end the program.
void install_signal_handlers();

}
