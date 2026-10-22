// The bit streams game packets are made of (BF2142.exe: set data
// 0x00687490, read 0x006871b0, write 0x00687290). Bits are packed from the
// lowest bit of each byte up, so a value of several bytes is little-endian
// and fields need not start on byte boundaries.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dice::hfe::net {

class BitWriter {
public:
	void write(uint32_t value, unsigned bits);
	void writeBytes(const void* data, size_t bytes); // whole bytes at the current bit
	// A fixed-size text field (`bytes` long, zero padded, cut if longer).
	void writeText(const std::string& text, size_t bytes);
	// A sign bit and `bits - 1` bits of magnitude (0x0050a410).
	void writeSigned(int32_t value, unsigned bits);
	void writeFloat(float value); // the raw 32 bits
	// Appends another stream's bits.
	void append(const BitWriter& other);
	const std::vector<uint8_t>& data() const { return m_data; }
	size_t bits() const { return m_bits; }

private:
	std::vector<uint8_t> m_data;
	size_t m_bits = 0;
};

class BitReader {
public:
	BitReader(const uint8_t* data, size_t bytes) : m_data(data), m_size(bytes * 8) {}
	// False (and the stream marked bad) past the end, as 0x006871b0.
	bool read(uint32_t& value, unsigned bits);
	uint32_t read(unsigned bits)
	{
		uint32_t v = 0;
		read(v, bits);
		return v;
	}
	std::string readText(size_t bytes);
	// A sign bit and `bits - 1` bits of magnitude (0x0050a460).
	int32_t readSigned(unsigned bits);
	float readFloat();
	bool bad() const { return m_bad; }
	size_t remaining() const { return m_size - m_at; }
	size_t position() const { return m_at; } // in bits
	// Moves to a bit position (0x0057e820 rewinds this way); false past the end.
	bool seek(size_t bit);

private:
	const uint8_t* m_data;
	size_t m_size;
	size_t m_at = 0;
	bool m_bad = false;
};

} // namespace dice::hfe::net
