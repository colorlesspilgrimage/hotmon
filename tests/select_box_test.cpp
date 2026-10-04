#include "select_box.hpp"
#include "wizard.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

namespace {

using namespace hotmon;

TEST(SelectBox, MovesAndStopsAtTheEnds) {
  SelectBox box({{"Open", "open"}, {"WPA2", "wpa2"}, {"WPA3", "wpa3"}}, 0);
  box.down();
  EXPECT_EQ(box.current().value, "wpa2");
  box.up();
  EXPECT_EQ(box.current().value, "open");
  box.up();
  EXPECT_EQ(box.index(), 0u);
  box.select_value("wpa3");
  box.down();
  EXPECT_EQ(box.current().value, "wpa3");
}

TEST(SelectBox, SelectValueKeepsTheIndexWhenAbsent) {
  SelectBox box({{"Open", "open"}, {"WPA2", "wpa2"}}, 1);
  EXPECT_TRUE(box.select_value("wpa2"));
  EXPECT_EQ(box.index(), 1u);
  EXPECT_FALSE(box.select_value("wep"));
  EXPECT_EQ(box.index(), 1u);
}

TEST(SelectBox, TypedCharactersDoNotChangeTheSecurityBox) {
  auto wizard = Wizard::make(HostFacts{sample_interfaces(), [](std::string_view) {
                                return std::vector<Ipv4Range>{};
                              }});
  wizard.page = Page::Security;
  wizard.push_char(U'x');
  EXPECT_EQ(wizard.security.current().value, "wpa2");
}

}  // namespace
