#include "Md5.h"

#include <algorithm>
#include <cstring>

namespace dice::hfe::net {

namespace {

constexpr uint32_t kShift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5,
	9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10,
	15, 21, 6, 10, 15, 21};

constexpr uint32_t kSine[64] = {0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
	0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6,
	0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681,
	0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085,
	0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
	0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82,
	0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

uint32_t rotate(uint32_t x, uint32_t c) { return (x << c) | (x >> (32 - c)); }

} // namespace

void Md5::block(const uint8_t* p)
{
	uint32_t w[16];
	for (int i = 0; i < 16; ++i) {
		w[i] = static_cast<uint32_t>(p[i * 4]) | static_cast<uint32_t>(p[i * 4 + 1]) << 8
			| static_cast<uint32_t>(p[i * 4 + 2]) << 16 | static_cast<uint32_t>(p[i * 4 + 3]) << 24;
	}
	uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3];
	for (uint32_t i = 0; i < 64; ++i) {
		uint32_t f, g;
		if (i < 16) {
			f = (b & c) | (~b & d);
			g = i;
		} else if (i < 32) {
			f = (d & b) | (~d & c);
			g = (5 * i + 1) % 16;
		} else if (i < 48) {
			f = b ^ c ^ d;
			g = (3 * i + 5) % 16;
		} else {
			f = c ^ (b | ~d);
			g = (7 * i) % 16;
		}
		const uint32_t next = d;
		d = c;
		c = b;
		b = b + rotate(a + f + kSine[i] + w[g], kShift[i]);
		a = next;
	}
	m_h[0] += a;
	m_h[1] += b;
	m_h[2] += c;
	m_h[3] += d;
}

void Md5::update(const void* data, size_t size)
{
	const uint8_t* p = static_cast<const uint8_t*>(data);
	m_length += size;
	while (size > 0) {
		const size_t take = std::min(size, sizeof(m_buffer) - m_buffered);
		std::memcpy(m_buffer + m_buffered, p, take);
		m_buffered += take;
		p += take;
		size -= take;
		if (m_buffered == sizeof(m_buffer)) {
			block(m_buffer);
			m_buffered = 0;
		}
	}
}

std::array<uint8_t, 16> Md5::digest() const
{
	Md5 copy = *this;
	const uint64_t bits = static_cast<uint64_t>(m_length) * 8;
	const uint8_t pad = 0x80;
	copy.update(&pad, 1);
	const uint8_t zero = 0;
	while (copy.m_buffered != 56) {
		copy.update(&zero, 1);
	}
	uint8_t length[8];
	for (int i = 0; i < 8; ++i) {
		length[i] = static_cast<uint8_t>(bits >> (8 * i));
	}
	copy.update(length, 8);
	std::array<uint8_t, 16> out {};
	for (int i = 0; i < 16; ++i) {
		out[static_cast<size_t>(i)] = static_cast<uint8_t>(copy.m_h[i / 4] >> (8 * (i % 4)));
	}
	return out;
}

std::array<uint8_t, 16> md5(const void* data, size_t size)
{
	Md5 context;
	context.update(data, size);
	return context.digest();
}

std::string md5Hex(const std::string& text)
{
	static const char* hex = "0123456789abcdef";
	std::string out;
	for (uint8_t b : md5(text.data(), text.size())) {
		out += hex[b >> 4];
		out += hex[b & 15];
	}
	return out;
}

} // namespace dice::hfe::net
