#include "BitStream.h"

#include <algorithm>
#include <cstring>

namespace dice::hfe::net {

void BitWriter::write(uint32_t value, unsigned bits)
{
	for (unsigned i = 0; i < bits; ++i, ++m_bits) {
		if (m_bits / 8 >= m_data.size()) {
			m_data.push_back(0);
		}
		if (value >> i & 1u) {
			m_data[m_bits / 8] = static_cast<uint8_t>(m_data[m_bits / 8] | 1u << (m_bits % 8));
		}
	}
}

void BitWriter::writeBytes(const void* data, size_t bytes)
{
	const auto* p = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < bytes; ++i) {
		write(p[i], 8);
	}
}

void BitWriter::writeText(const std::string& text, size_t bytes)
{
	std::string field = text.substr(0, bytes);
	field.resize(bytes, '\0');
	writeBytes(field.data(), bytes);
}

void BitWriter::writeSigned(int32_t value, unsigned bits)
{
	write(value < 0 ? 1 : 0, 1);
	write(static_cast<uint32_t>(value < 0 ? -static_cast<int64_t>(value) : value), bits - 1);
}

void BitWriter::writeFloat(float value)
{
	uint32_t raw;
	std::memcpy(&raw, &value, 4);
	write(raw, 32);
}

void BitWriter::append(const BitWriter& other)
{
	for (size_t i = 0; i < other.m_bits; ++i) {
		write(other.m_data[i / 8] >> (i % 8) & 1u, 1);
	}
}

bool BitReader::read(uint32_t& value, unsigned bits)
{
	value = 0;
	if (bits == 0 || m_at + bits > m_size) {
		m_bad = m_bad || bits != 0;
		return false;
	}
	for (unsigned i = 0; i < bits; ++i, ++m_at) {
		if (m_data[m_at / 8] >> (m_at % 8) & 1u) {
			value |= 1u << i;
		}
	}
	return true;
}

int32_t BitReader::readSigned(unsigned bits)
{
	const bool negative = read(1) == 1;
	const auto magnitude = static_cast<int32_t>(read(bits - 1));
	return negative ? -magnitude : magnitude;
}

float BitReader::readFloat()
{
	const uint32_t raw = read(32);
	float value;
	std::memcpy(&value, &raw, 4);
	return value;
}

bool BitReader::seek(size_t bit)
{
	if (bit > m_size) {
		m_bad = true;
		return false;
	}
	m_at = bit;
	return true;
}

std::string BitReader::readText(size_t bytes)
{
	std::string text;
	for (size_t i = 0; i < bytes; ++i) {
		const char c = static_cast<char>(read(8));
		text.push_back(c);
	}
	text.resize(std::min(text.find('\0'), text.size()));
	return text;
}

} // namespace dice::hfe::net
