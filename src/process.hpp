#pragma once

#include "result.hpp"

#include <string>
#include <vector>

namespace hotmon {

class FileDescriptor {
 public:
  FileDescriptor() = default;
  explicit FileDescriptor(int fd);
  ~FileDescriptor();

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept;
  FileDescriptor& operator=(FileDescriptor&& other) noexcept;

  int get() const;
  int release();
  explicit operator bool() const;

 private:
  int fd_ = -1;
};

struct CommandOutput {
  int status = 0;
  std::string out;
  std::string err;
};

Result<CommandOutput> run_capture(const std::vector<std::string>& argv);

}  // namespace hotmon
