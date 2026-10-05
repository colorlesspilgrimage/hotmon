#include "format.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using namespace hotmon;

TEST(Format, BytesSmall) {
  EXPECT_EQ(format_bytes(0), "0 B");
  EXPECT_EQ(format_bytes(1023), "1023 B");
}

TEST(Format, BytesUnits) {
  EXPECT_EQ(format_bytes(1024), "1.0 KiB");
  EXPECT_EQ(format_bytes(1536), "1.5 KiB");
  EXPECT_EQ(format_bytes(5242880), "5.0 MiB");
  EXPECT_EQ(format_bytes(1073741824), "1.0 GiB");
}

TEST(Format, BytesMax) {
  const std::string text = format_bytes(std::numeric_limits<uint64_t>::max());
  EXPECT_NE(text.find(" TiB"), std::string::npos);
  EXPECT_EQ(text.size() >= 4 && text.ends_with(" TiB"), true);
}

TEST(Format, RateAddsSuffix) {
  EXPECT_EQ(format_rate(2048), "2.0 KiB/s");
  EXPECT_EQ(format_rate(0), "0 B/s");
}

}
