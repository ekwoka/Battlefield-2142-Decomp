#include "TerrainData.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "IO/File/FileManager.h"

#include <cstdint>
#include <cstring>

namespace dice::hfe::terrain {

namespace {

class Reader {
public:
	explicit Reader(const std::vector<uint8_t>& data) : m_data(data) {}
	bool ok() const { return m_ok; }

	template <class T>
	T read()
	{
		T value {};
		if (!m_ok || m_data.size() - m_pos < sizeof(T)) {
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

	// Text up to '\n' (0x1019fa40).
	std::string line()
	{
		std::string text;
		while (m_ok) {
			const char c = read<char>();
			if (!m_ok || c == '\n') {
				break;
			}
			text.push_back(c);
		}
		return text;
	}

private:
	const std::vector<uint8_t>& m_data;
	size_t m_pos = 0;
	bool m_ok = true;
};

} // namespace

std::optional<TerrainData> parseTerrainData(const std::vector<uint8_t>& data, std::string* error)
{
	Reader r(data);
	TerrainData t;
	t.version = r.read<uint16_t>();
	const uint16_t flags = r.read<uint16_t>();
	t.primaryWorldScale = r.floats<3>();
	t.secondaryWorldScale = r.floats<3>();
	r.read<int32_t>();
	r.read<int32_t>();
	r.read<int32_t>();
	t.patchSize = r.read<int32_t>();
	r.read<uint8_t>();
	t.patchesPerSide = r.read<int32_t>();
	t.colormapSize = r.read<int32_t>();
	t.lowDetailmapSize = r.read<int32_t>();
	t.colormapBaseName = r.line();
	t.detailmapBaseName = r.line();
	t.lowDetailmapBaseName = r.line();
	t.lightmapBaseName = r.line();
	t.farSideTiling = r.floats<2>();
	t.farTopTilingHigh = r.read<float>();
	t.farTopTilingLow = (flags != 0 && t.version >= 0x17) ? r.read<float>() : t.farTopTilingHigh;
	t.farYOffset = r.read<float>();
	t.sunColor = r.floats<3>();
	t.giColor = r.floats<3>();
	t.waterColor = r.floats<3>();
	const int32_t layerCount = r.read<int32_t>();
	if (!r.ok() || layerCount < 0 || layerCount > 64 || t.patchesPerSide <= 0 || t.patchesPerSide > 64) {
		if (error) {
			*error = "malformed terrain header";
		}
		return std::nullopt;
	}
	t.layers.resize(static_cast<size_t>(layerCount));
	for (DetailLayer& layer : t.layers) {
		layer.texture = r.line();
		layer.sideProjection = r.read<uint8_t>() != 0;
		layer.sideTiling = r.floats<2>();
		layer.tiling = r.read<float>();
		r.read<int32_t>();
		r.read<uint8_t>();
	}
	if (!r.ok()) {
		if (error) {
			*error = "truncated terrain header";
		}
		return std::nullopt;
	}
	return t;
}

void registerTerrainDataConsoleMethods(Console& console, const io::FileManager& files,
	std::optional<TerrainData>& terrain)
{
	const uint32_t access = ConsoleAccess::Anyone;
	bindMethod(console, "terrain.create", access, [](std::string) {});
	bindMethod(console, "terrain.load", access, [&files, &terrain](std::string path) {
		std::vector<uint8_t> data;
		terrain.reset();
		if (files.readFile(path, data)) {
			terrain = parseTerrainData(data);
		}
	});
}

} // namespace dice::hfe::terrain
