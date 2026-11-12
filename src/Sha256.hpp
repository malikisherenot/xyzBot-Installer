#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace xyzinst {
std::string sha256Hex(std::vector<uint8_t> const& data);
}
