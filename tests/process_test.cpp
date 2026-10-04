#include "process.hpp"

#include <gtest/gtest.h>

namespace {

using namespace hotmon;

TEST(Process, TrueSucceedsAndFalseReportsStatus) {
  auto ok = run_capture({"true"});
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok->status, 0);
  auto bad = run_capture({"false"});
  ASSERT_TRUE(bad);
  EXPECT_NE(bad->status, 0);
}

TEST(Process, EchoDoesNotSplitTheArgument) {
  auto output = run_capture({"echo", "a b"});
  ASSERT_TRUE(output);
  EXPECT_EQ(output->status, 0);
  EXPECT_EQ(output->out, "a b\n");
}

TEST(Process, MissingProgramReturnsAnError) {
  EXPECT_FALSE(run_capture({"definitely-not-a-program"}));
}

}  // namespace
