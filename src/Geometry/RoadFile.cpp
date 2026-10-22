#include "RoadFile.h"

#include <cstring>

namespace dice::hfe::geom {

namespace {

class Reader {
public:
	explicit Reader(const std::vector<uint8_t>& data) : m_data(data) {}

	bool ok() const { return m_ok; }
	size_t remaining() const { return m_data.size() - m_pos; }

	template <class T>
	T read()
	{
		T value {};
		if (sizeof(T) > remaining()) {
			m_ok = false;
			return value;
		}
		std::memcpy(&value, m_data.data() + m_pos, sizeof(T));
		m_pos += sizeof(T);
		return value;
	}

	template <size_t N>
	std::array<float, N> floats()
	{
		std::array<float, N> values {};
		for (float& v : values) {
			v = read<float>();
		}
		return values;
	}

	// A count that must fit in the rest of the file at `size` bytes per item.
	uint32_t count(size_t size)
	{
		const uint32_t n = read<uint32_t>();
		if (n > remaining() / size) {
			m_ok = false;
			return 0;
		}
		return n;
	}

	// Text up to a newline.
	std::string line()
	{
		std::string text;
		while (m_pos < m_data.size() && m_data[m_pos] != '\n') {
			text += static_cast<char>(m_data[m_pos++]);
		}
		if (m_pos >= m_data.size()) {
			m_ok = false;
		}
		++m_pos;
		return text;
	}

private:
	const std::vector<uint8_t>& m_data;
	size_t m_pos = 0;
	bool m_ok = true;
};

bool fail(std::string* error, const char* message)
{
	if (error) {
		*error = message;
	}
	return false;
}

} // namespace

std::optional<CompiledRoad> parseCompiledRoad(const std::vector<uint8_t>& data, std::string* error)
{
	Reader r(data);
	CompiledRoad road;
	road.version = r.read<uint32_t>();
	if (road.version < 0x10002 || road.version > 0x10004) {
		fail(error, "unsupported version");
		return std::nullopt;
	}
	road.position = r.floats<3>();
	road.radius = r.read<float>();
	road.boundsMin = r.floats<3>();
	road.boundsMax = r.floats<3>();
	r.read<uint32_t>();
	const bool withNormals = road.version == 0x10002;
	const uint32_t vertexCount = r.count(withNormals ? 56 : 32);
	road.vertices.resize(vertexCount);
	for (RoadVertex& v : road.vertices) {
		v.position = r.floats<3>();
		if (withNormals) {
			r.floats<3>();
		}
		v.texCoord0 = r.floats<2>();
		v.texCoord1 = r.floats<2>();
		if (withNormals) {
			r.floats<4>();
		} else {
			v.alpha = r.read<float>();
		}
	}
	const uint32_t indexCount = r.count(2);
	road.indices.resize(indexCount);
	for (uint16_t& index : road.indices) {
		index = r.read<uint16_t>();
	}
	if (road.version == 0x10004) {
		const uint32_t extra = r.count(24);
		for (uint32_t i = 0; i < extra * 6; ++i) {
			r.read<uint32_t>();
		}
	}
	if (!r.ok() || r.remaining() != 0) {
		fail(error, r.ok() ? "trailing data" : "truncated");
		return std::nullopt;
	}
	if (indexCount % 3 != 0) {
		fail(error, "index count is not a triangle list");
		return std::nullopt;
	}
	for (uint16_t index : road.indices) {
		if (index >= vertexCount) {
			fail(error, "index out of range");
			return std::nullopt;
		}
	}
	return road;
}

std::optional<RoadSettings> parseRoadSettings(const std::vector<uint8_t>& data, std::string* error)
{
	Reader r(data);
	RoadSettings settings;
	settings.texture0 = r.line();
	settings.texture1 = r.line();
	settings.blendFactor = r.read<float>();
	settings.fadeStart = r.read<float>();
	settings.fadeEnd = r.read<float>();
	settings.priority = r.read<uint32_t>();
	if (!r.ok()) {
		fail(error, "truncated");
		return std::nullopt;
	}
	return settings;
}

} // namespace dice::hfe::geom
