// SWF's basic records and the bit reader that decodes them.
//
// Byte fields are little-endian. Bit fields (UB, SB, FB) are packed
// most-significant bit first and start where the previous bit field ended;
// a byte read (or align()) skips to the next whole byte. RECT, MATRIX and
// CXFORM are bit-packed and end byte-aligned. Coordinates are twips
// (1/20 pixel). The reader never runs past its buffer: an overrun returns
// zeros and clears ok().
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dice::hfe::ui::swf {

struct Rgba {
	uint8_t r = 0, g = 0, b = 0, a = 255;

	bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
	bool operator!=(const Rgba& o) const { return !(*this == o); }
};

struct Rect {
	int32_t xMin = 0, xMax = 0, yMin = 0, yMax = 0; // twips
};

struct Point {
	float x = 0, y = 0;
};

// x' = a*x + c*y + tx, y' = b*x + d*y + ty (the file's ScaleX, RotateSkew0,
// RotateSkew1, ScaleY, TranslateX, TranslateY).
struct Matrix {
	float a = 1, b = 0, c = 0, d = 1;
	float tx = 0, ty = 0; // twips

	Point apply(Point p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
};

// Each channel becomes clamp(channel * mult + add). The multipliers are
// the file's 8.8 fixed-point terms divided by 256.
struct ColorTransform {
	float mult[4] = {1, 1, 1, 1}; // r, g, b, a
	int16_t add[4] = {0, 0, 0, 0};
};

class BitReader {
public:
	BitReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

	bool ok() const { return m_ok; }
	// Byte position (rounded up if mid-byte).
	size_t position() const { return (m_bit + 7) / 8; }
	size_t size() const { return m_size; }
	size_t remaining() const { return position() < m_size ? m_size - position() : 0; }
	void seek(size_t byte) { m_bit = byte * 8; }
	void align() { m_bit = (m_bit + 7) & ~size_t(7); }

	uint32_t ub(int bits)
	{
		if (bits <= 0) {
			return 0;
		}
		if (bits > 32 || m_bit + static_cast<size_t>(bits) > m_size * 8) {
			m_ok = false;
			m_bit = m_size * 8;
			return 0;
		}
		uint32_t v = 0;
		for (int i = 0; i < bits; ++i, ++m_bit) {
			v = v << 1 | ((m_data[m_bit >> 3] >> (7 - (m_bit & 7))) & 1);
		}
		return v;
	}

	int32_t sb(int bits)
	{
		const uint32_t v = ub(bits);
		if (bits <= 0 || bits >= 32) {
			return static_cast<int32_t>(v);
		}
		const uint32_t sign = 1u << (bits - 1);
		return static_cast<int32_t>((v ^ sign) - sign);
	}

	// 16.16 fixed point.
	float fb(int bits) { return static_cast<float>(sb(bits)) / 65536.0f; }

	uint8_t u8()
	{
		align();
		return static_cast<uint8_t>(ub(8));
	}
	uint16_t u16()
	{
		const uint32_t lo = u8();
		return static_cast<uint16_t>(lo | static_cast<uint32_t>(u8()) << 8);
	}
	int16_t s16() { return static_cast<int16_t>(u16()); }
	uint32_t u32()
	{
		const uint32_t lo = u16();
		return lo | static_cast<uint32_t>(u16()) << 16;
	}
	// 8.8 fixed point.
	float f8() { return static_cast<float>(s16()) / 256.0f; }

	Rgba rgb()
	{
		Rgba c;
		c.r = u8();
		c.g = u8();
		c.b = u8();
		return c;
	}
	Rgba rgba()
	{
		Rgba c = rgb();
		c.a = u8();
		return c;
	}

	Rect rect()
	{
		align();
		Rect r;
		const int n = static_cast<int>(ub(5));
		r.xMin = sb(n);
		r.xMax = sb(n);
		r.yMin = sb(n);
		r.yMax = sb(n);
		align();
		return r;
	}

	Matrix matrix()
	{
		align();
		Matrix m;
		if (ub(1)) {
			const int n = static_cast<int>(ub(5));
			m.a = fb(n);
			m.d = fb(n);
		}
		if (ub(1)) {
			const int n = static_cast<int>(ub(5));
			m.b = fb(n);
			m.c = fb(n);
		}
		const int n = static_cast<int>(ub(5));
		m.tx = static_cast<float>(sb(n));
		m.ty = static_cast<float>(sb(n));
		align();
		return m;
	}

	// CXFORM (alpha = false) or CXFORMWITHALPHA.
	ColorTransform cxform(bool alpha)
	{
		align();
		ColorTransform t;
		const bool hasAdd = ub(1) != 0;
		const bool hasMult = ub(1) != 0;
		const int n = static_cast<int>(ub(4));
		const int channels = alpha ? 4 : 3;
		if (hasMult) {
			for (int i = 0; i < channels; ++i) {
				t.mult[i] = static_cast<float>(sb(n)) / 256.0f;
			}
		}
		if (hasAdd) {
			for (int i = 0; i < channels; ++i) {
				t.add[i] = static_cast<int16_t>(sb(n));
			}
		}
		align();
		return t;
	}

	// A NUL-terminated string (UTF-8 from SWF 6 on), terminator consumed.
	std::string string()
	{
		align();
		std::string s;
		while (true) {
			if (position() >= m_size) {
				m_ok = false;
				break;
			}
			const char c = static_cast<char>(m_data[position()]);
			m_bit += 8;
			if (c == 0) {
				break;
			}
			s.push_back(c);
		}
		return s;
	}

	// `count` raw bytes as a string (the font name's UI8 array).
	std::string bytes(size_t count)
	{
		align();
		if (count > remaining()) {
			m_ok = false;
			count = remaining();
		}
		std::string s(reinterpret_cast<const char*>(m_data + position()), count);
		m_bit += count * 8;
		return s;
	}

	void skip(size_t count)
	{
		align();
		if (count > remaining()) {
			m_ok = false;
			count = remaining();
		}
		m_bit += count * 8;
	}

private:
	const uint8_t* m_data;
	size_t m_size;
	size_t m_bit = 0;
	bool m_ok = true;
};

} // namespace dice::hfe::ui::swf
