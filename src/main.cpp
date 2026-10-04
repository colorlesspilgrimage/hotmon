#include "app.hpp"
#include "terminal.hpp"

#include <cstdio>

int main() {
  auto app = hotmon::App::boot();
  if (!app) {
    std::fprintf(stderr, "%s\n", app.error().c_str());
    return 1;
  }
  return hotmon::run_ui(*app);
}
