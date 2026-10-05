#include "app.hpp"
#include "privilege.hpp"
#include "terminal.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--privileged-helper") == 0) {
    if (::geteuid() != 0) {
      std::fprintf(stderr, "The helper must run as root.\n");
      return 1;
    }
    hotmon::SystemRunner runner;
    hotmon::SystemSignals signals;
    hotmon::DirectPrivilege worker(runner, signals, hotmon::Paths::system());
    return hotmon::run_privileged_helper(worker);
  }
  if (argc != 1) {
    std::fprintf(stderr, "Usage: hotmon\n");
    return 2;
  }
  auto app = hotmon::App::boot();
  if (!app) {
    std::fprintf(stderr, "%s\n", app.error().c_str());
    return 1;
  }
  hotmon::SystemRunner runner;
  hotmon::SystemSignals signals;
  auto privileged = hotmon::make_privileged(::geteuid(), runner, signals, hotmon::Paths::system(),
                                             hotmon::default_helper_argv(), hotmon::terminal_hooks());
  return hotmon::run_ui(*app, *privileged);
}
