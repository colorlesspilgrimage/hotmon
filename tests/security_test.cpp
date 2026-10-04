// Security regression tests. Each test shows one flaw that the security review found.

#include "backend.hpp"
#include "backend_exec.hpp"
#include "process.hpp"
#include "profile.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <iterator>

namespace {

using namespace hotmon;

std::string read_all(const std::filesystem::path& path) {
  std::ifstream input(path);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

unsigned mode_bits(const std::filesystem::path& path) {
  return static_cast<unsigned>(std::filesystem::status(path).permissions()) & 0777;
}

// S1: A malformed UTF-8 lead byte must not hide a control byte or a slash.
TEST(Security, SsidRejectsMalformedUtf8) {
  EXPECT_FALSE(validate_ssid("a\xE2\x82"));
  EXPECT_FALSE(validate_ssid("ab\xC5\ninterface=evil0"));
  EXPECT_FALSE(validate_ssid("a\xC5/..\xC5/..\xC5/escape"));
  EXPECT_FALSE(validate_ssid("a\x80z"));
  EXPECT_FALSE(validate_ssid("a\xED\xA0\x80z"));
  EXPECT_FALSE(validate_ssid("a\xF4\x90\x80\x80z"));
  EXPECT_TRUE(validate_ssid("Caf\xC3\xA9"));
  EXPECT_TRUE(validate_ssid("net \xE2\x82\xAC \xF0\x9F\x93\xB6"));
}

// S1: The iwd backend uses the SSID as a file name. A hidden slash must not escape the iwd folder.
TEST(Security, IwdPlanRejectsAPathInTheSsid) {
  const auto dir = scratch_dir();
  Profile profile = sample_profile();
  profile.ssid = "a\xC5/..\xC5/..\xC5/escape";
  EXPECT_FALSE(plan_apply(BackendKind::Iwd, profile, test_paths(dir)));
  profile.ssid = "ab\xC5\ninterface=evil0";
  EXPECT_FALSE(plan_apply(BackendKind::DirectHostapd, profile, test_paths(dir)));
  std::filesystem::remove_all(dir);
}

// S2: Save must not write through a symbolic link at the profile path.
TEST(Security, SaveProfileDoesNotFollowAFileLink) {
  const auto dir = scratch_dir();
  const auto victim = dir / "victim.txt";
  { std::ofstream(victim) << "keep"; }
  std::filesystem::permissions(victim, std::filesystem::perms(0644));
  std::filesystem::create_directories(dir / "hotmon");
  std::filesystem::create_symlink(victim, dir / "hotmon" / "profile.json");
  EXPECT_FALSE(save_profile(dir / "hotmon" / "profile.json", sample_profile()));
  EXPECT_EQ(read_all(victim), "keep");
  EXPECT_EQ(mode_bits(victim), 0644u);
  std::filesystem::remove_all(dir);
}

// S2: Save must not change the mode of a folder through a symbolic link.
TEST(Security, SaveProfileDoesNotFollowAFolderLink) {
  const auto dir = scratch_dir();
  const auto victim_dir = dir / "victim";
  std::filesystem::create_directories(victim_dir);
  std::filesystem::permissions(victim_dir, std::filesystem::perms(0755));
  std::filesystem::create_symlink(victim_dir, dir / "hotmon");
  EXPECT_FALSE(save_profile(dir / "hotmon" / "profile.json", sample_profile()));
  EXPECT_EQ(mode_bits(victim_dir), 0755u);
  EXPECT_FALSE(std::filesystem::exists(victim_dir / "profile.json"));
  std::filesystem::remove_all(dir);
}

// S2: A normal save still works and uses private modes.
TEST(Security, SaveProfileStillWritesPrivateFiles) {
  const auto dir = scratch_dir();
  const auto path = dir / "cfg" / "hotmon" / "profile.json";
  ASSERT_TRUE(save_profile(path, sample_profile()));
  ASSERT_TRUE(save_profile(path, sample_profile()));
  EXPECT_EQ(mode_bits(path.parent_path()), 0700u);
  EXPECT_EQ(mode_bits(path), 0600u);
  auto loaded = load_profile(path);
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->ssid, sample_profile().ssid);
  std::filesystem::remove_all(dir);
}

// S3: A child program must not get open descriptors of hotmon, for example the capture socket.
TEST(Security, ChildDoesNotInheritOpenDescriptors) {
  const int fd = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(fd, 0);
  const int high = ::dup2(fd, 200);
  ASSERT_EQ(high, 200);
  ::close(fd);
  auto output = run_capture({"ls", "/proc/self/fd"});
  ::close(high);
  ASSERT_TRUE(output);
  EXPECT_EQ(output->status, 0);
  EXPECT_EQ(output->out.find("200\n"), std::string::npos) << output->out;
}

// S4: Other local users must not write the dnsmasq lease file.
TEST(Security, DnsmasqLeaseFileIsNotWorldWritable) {
  const auto dir = scratch_dir();
  const Paths paths{dir / "run", dir / "hostapd.conf", dir / "iwd", dir / "proc"};
  auto plan = plan_apply(BackendKind::DirectHostapd, sample_profile(), paths);
  ASSERT_TRUE(plan);
  ScriptedRunner runner;
  RecordedSignals signals;
  ASSERT_TRUE(execute_plan(*plan, runner, signals, paths));
  ASSERT_TRUE(std::filesystem::exists(paths.dnsmasq_lease()));
  EXPECT_EQ(mode_bits(paths.dnsmasq_lease()) & 0002u, 0u);
  std::filesystem::remove_all(dir);
}

}
