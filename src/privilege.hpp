#pragma once

// Wire protocol for the privileged helper.
// A frame is a 4-byte big-endian length, then that many JSON bytes.
// Reject a frame larger than 1 MiB.
// The parent sends one request. The helper sends one response.
// Request ops are apply, stop, and capture.
// The helper does not read commands, paths, or pids from the request.
// A capture response can also carry one file descriptor.

#include "backend_exec.hpp"
#include <sys/types.h>

#include "process.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hotmon {

enum class PrivilegeOp { Apply, Stop, Capture };

struct Request {
  PrivilegeOp op = PrivilegeOp::Apply;
  BackendKind backend = BackendKind::DirectHostapd;
  bool private_hostapd = false;
  Profile profile;
  std::string interface;
};

struct Response {
  bool ok = false;
  std::string error;
  std::optional<StartReport> report;
};

struct StopRequest {
  BackendKind backend = BackendKind::DirectHostapd;
  Profile profile;
  bool private_hostapd = false;
  std::vector<StartedProc> started;
};

struct TerminalHooks {
  std::function<void()> suspend;
  std::function<void()> resume;
};

class Privileged {
 public:
  virtual ~Privileged() = default;
  virtual Result<StartReport> apply(BackendKind backend, const Profile& profile) = 0;
  virtual Result<void> stop(const StopRequest& request) = 0;
  virtual Result<FileDescriptor> open_capture_socket(std::string_view interface) = 0;
};

class DirectPrivilege : public Privileged {
 public:
  DirectPrivilege(Runner& runner, ProcessControl& signals, Paths paths);
  DirectPrivilege(const DirectPrivilege&) = delete;
  DirectPrivilege& operator=(const DirectPrivilege&) = delete;

  Result<StartReport> apply(BackendKind backend, const Profile& profile) override;
  Result<void> stop(const StopRequest& request) override;
  Result<FileDescriptor> open_capture_socket(std::string_view interface) override;

 private:
  Runner& runner_;
  ProcessControl& signals_;
  Paths paths_;
};

class HelperPrivilege : public Privileged {
 public:
  HelperPrivilege(std::vector<std::string> helper_argv, TerminalHooks hooks);
  HelperPrivilege(const HelperPrivilege&) = delete;
  HelperPrivilege& operator=(const HelperPrivilege&) = delete;

  Result<StartReport> apply(BackendKind backend, const Profile& profile) override;
  Result<void> stop(const StopRequest& request) override;
  Result<FileDescriptor> open_capture_socket(std::string_view interface) override;

 private:
  struct Exchange {
    Response response;
    FileDescriptor passed;
  };

  Result<Exchange> exchange(const Request& request);

  std::vector<std::string> helper_argv_;
  TerminalHooks hooks_;
};

std::string_view backend_token(BackendKind kind);
Result<BackendKind> parse_backend_token(std::string_view text);
std::string encode_request(const Request& request);
Result<Request> decode_request(std::string_view text);
std::string encode_response(const Response& response);
Result<Response> decode_response(std::string_view text);
Result<void> write_frame(int fd, std::string_view payload, int pass_fd = -1);
Result<std::string> read_frame(int fd, int* received_fd = nullptr);

std::vector<std::string> default_helper_argv();
std::unique_ptr<Privileged> make_privileged(uid_t euid, Runner& runner, ProcessControl& signals,
                                            const Paths& paths, std::vector<std::string> helper_argv,
                                            TerminalHooks hooks);
int run_privileged_helper(Privileged& worker);
int run_privileged_helper(Privileged& worker, const Paths& paths);

}
