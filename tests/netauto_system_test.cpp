#include "netauto.hpp"

#include <gtest/gtest.h>

namespace {

TEST(NetautoSystem, ReadLocalNetworksDoesNotThrow) {
  EXPECT_NO_THROW((void)hotmon::read_local_networks(""));
}

}
