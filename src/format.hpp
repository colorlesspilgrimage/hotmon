#pragma once

#include <cstdint>
#include <string>

namespace hotmon {

std::string format_bytes(uint64_t bytes);
std::string format_rate(uint64_t bytes_per_second);

}
