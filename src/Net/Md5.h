// MD5 (RFC 1321), as GameSpy's CD-key code uses it (0x0071bc70 writes the
// digest as 32 lower-case hex digits).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dice::hfe::net {

std::array<uint8_t, 16> md5(const void* data, size_t size);
std::string md5Hex(const std::string& text);

} // namespace dice::hfe::net
