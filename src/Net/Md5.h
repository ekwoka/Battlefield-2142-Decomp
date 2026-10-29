// MD5 (RFC 1321), as GameSpy's CD-key code uses it (0x0071bc70 writes the
// digest as 32 lower-case hex digits).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dice::hfe::net {

std::array<uint8_t, 16> md5(const void* data, size_t size);

// The same, fed in pieces (the engine's MD5 context: init 0x0071b140,
// update 0x0071bac0, final 0x0071bb80).
class Md5 {
public:
	void update(const void* data, size_t size);
	void update(const std::string& text) { update(text.data(), text.size()); }
	std::array<uint8_t, 16> digest() const;

private:
	void block(const uint8_t* p);
	uint32_t m_h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
	uint8_t m_buffer[64] = {};
	size_t m_buffered = 0;
	uint64_t m_length = 0;
};
std::string md5Hex(const std::string& text);

} // namespace dice::hfe::net
