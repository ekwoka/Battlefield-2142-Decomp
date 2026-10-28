#include "TerrainData.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "IO/File/FileManager.h"

#include <algorithm>
#include <cmath>
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

	size_t position() const { return m_pos; }
	size_t size() const { return m_data.size(); }
	bool skip(size_t bytes)
	{
		if (!m_ok || m_data.size() - m_pos < bytes) {
			m_ok = false;
			return false;
		}
		m_pos += bytes;
		return true;
	}
	const uint8_t* at() const { return m_data.data() + m_pos; }

private:
	const std::vector<uint8_t>& m_data;
	size_t m_pos = 0;
	bool m_ok = true;
};

// A grid's vertex count from (x0, z0) to (x1, z1) at `step` (0x100b3c70).
int gridVertices(int x0, int z0, int x1, int z1, int step)
{
	return ((x1 - x0) / step + 1) * ((z1 - z0) / step + 1);
}

std::optional<TerrainMeshes> parseMeshes(Reader& r, const TerrainData& t)
{
	if (t.version < 0x1a || t.patchSize <= 0 || t.patchSize > 256) {
		return std::nullopt;
	}
	TerrainMeshes m;
	const size_t side = static_cast<size_t>(t.patchSize) + 1;
	// The shared LOD index tree (0x100b00a0).
	static const int kStitchOrder[9] = {0, 1, 2, 4, 8, 5, 9, 6, 10};
	m.nodes.resize(85);
	for (size_t n = 0; n < m.nodes.size(); ++n) {
		for (int lod = 0; lod < 3; ++lod) {
			const int ranges = n < 21 && lod > 0 ? 9 : 1;
			for (int i = 0; i < ranges; ++i) {
				IndexRange& range = m.nodes[n][static_cast<size_t>(lod)][static_cast<size_t>(kStitchOrder[i])];
				range.start = r.read<int32_t>();
				range.count = r.read<int32_t>();
			}
		}
	}
	const uint32_t indexCount = r.read<uint32_t>();
	if (!r.ok() || indexCount > (r.size() - r.position()) / 2) {
		return std::nullopt;
	}
	m.indices.resize(indexCount);
	for (uint16_t& index : m.indices) {
		index = r.read<uint16_t>();
	}
	m.baseGrid = t.version > 0x18 && r.read<uint8_t>() != 0;
	const size_t stride = m.baseGrid ? 20 : 24;
	// The patches (0x100aed30 -> 0x1014be90).
	const int patches = t.patchesPerSide * t.patchesPerSide;
	m.patches.resize(static_cast<size_t>(patches));
	for (TerrainPatchMesh& patch : m.patches) {
		patch.x = r.read<int32_t>();
		patch.z = r.read<int32_t>();
		patch.highest = r.read<float>();
		patch.lowest = r.read<float>();
		patch.centre = r.floats<3>();
		patch.radius = r.read<float>();
		if (!r.ok() || (r.size() - r.position()) / stride < side * side) {
			return std::nullopt;
		}
		patch.vertices.resize(side * side);
		for (size_t i = 0; i < patch.vertices.size(); ++i) {
			TerrainVertex& v = patch.vertices[i];
			if (!m.baseGrid) {
				v.edge = r.read<uint8_t>();
				v.row = r.read<uint8_t>();
				v.column = r.read<uint8_t>();
				r.read<uint8_t>();
			} else {
				v.row = static_cast<uint8_t>(i / side);
				v.column = static_cast<uint8_t>(i % side);
			}
			v.height = r.read<float>();
			v.morph = r.floats<3>();
			for (uint8_t& c : v.normal) {
				c = r.read<uint8_t>();
			}
		}
		// The detail materials' index lists (0x1014bc80 -> 0x10178cd0).
		while (r.ok() && r.read<int32_t>() != -1) {
			TerrainPatchMesh::Material material;
			material.material = r.read<int32_t>();
			r.floats<4>();
			const int32_t count = r.read<int32_t>();
			r.read<int32_t>(); // triangles
			r.read<uint32_t>(); // 4
			const int32_t nodes = r.read<int32_t>();
			if (!r.ok() || count < 0 || nodes < 0 || nodes > 1024
				|| !r.skip(static_cast<size_t>(nodes) * 6 * sizeof(int32_t))
				|| static_cast<size_t>(count) > (r.size() - r.position()) / 2) {
				return std::nullopt;
			}
			material.indices.resize(static_cast<size_t>(count));
			for (uint16_t& index : material.indices) {
				index = r.read<uint16_t>();
			}
			patch.materials.push_back(std::move(material));
		}
	}
	// The surrounding terrains (0x100aedb0 -> 0x101506d0): their vertex
	// counts from the meshes RendDX9 generates (0x100b3fe0).
	const int s = static_cast<int>(std::lround(static_cast<float>(t.patchesPerSide * t.patchSize)
		* t.primaryWorldScale[0] / std::max(t.secondaryWorldScale[0], 1e-6f)));
	const int q = s / 4, h = s / 2;
	const int edge = gridVertices(0, 0, s, q, 1) + gridVertices(0, q, s, h, 2) + gridVertices(0, h, s, s, 4);
	const int corner = gridVertices(0, 0, q, q, 1) + gridVertices(q, 0, h, q, 2) + gridVertices(0, q, h, h, 2)
		+ gridVertices(h, 0, s, h, 4) + gridVertices(0, h, s, s, 4);
	while (r.ok()) {
		const int32_t k = r.read<int32_t>();
		if (k == -1) {
			break;
		}
		r.floats<6>();
		const int n = k % 2 == 0 ? corner : edge;
		if (!r.skip(static_cast<size_t>(n) * 16)) {
			return std::nullopt;
		}
		++m.surroundingRecords;
	}
	// The culling quadtree (0x1009da80).
	m.cullingNodes = r.read<int32_t>();
	if (!r.ok() || m.cullingNodes < 0 || !r.skip(static_cast<size_t>(m.cullingNodes) * 44)
		|| r.position() != r.size()) {
		return std::nullopt;
	}
	return m;
}

} // namespace

std::optional<TerrainData> parseTerrainData(const std::vector<uint8_t>& data, std::string* error)
{
	Reader r(data);
	TerrainData t;
	t.version = r.read<uint16_t>();
	const uint16_t flags = r.read<uint16_t>();
	t.primaryWorldScale = r.floats<3>();
	t.secondaryWorldScale = r.floats<3>();
	r.read<int32_t>(); // never set (0xcdcdcdcd)
	t.highest = r.read<float>();
	t.lowest = r.read<float>();
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
	t.meshes = parseMeshes(r, t);
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
