#include "terminal.hpp"

#include <gtest/gtest.h>

#include <csignal>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace hotmon;

int signal_after_handlers(int sig) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    install_signal_handlers();
    ::raise(sig);
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  return WIFSIGNALED(status) ? WTERMSIG(status) : -1;
}

TEST(Terminal, SignalsEndTheProgram) {
  EXPECT_EQ(signal_after_handlers(SIGTERM), SIGTERM);
  EXPECT_EQ(signal_after_handlers(SIGHUP), SIGHUP);
  EXPECT_EQ(signal_after_handlers(SIGINT), SIGINT);
}

}  // namespace
