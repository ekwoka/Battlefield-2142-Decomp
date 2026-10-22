#include "Undergrowth.h"

#include "Game/Random.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace dice::hfe::terrain {

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

	// A count that must fit in the rest of the data at `size` bytes per item.
	uint32_t count(size_t size)
	{
		const uint32_t n = read<uint32_t>();
		if (n > remaining() / size) {
			m_ok = false;
			return 0;
		}
		return n;
	}

	std::string string()
	{
		const uint32_t length = count(1);
		std::string text(reinterpret_cast<const char*>(m_data.data() + m_pos), length);
		m_pos += length;
		return text;
	}

private:
	const std::vector<uint8_t>& m_data;
	size_t m_pos = 0;
	bool m_ok = true;
};

std::array<float, 3> cross(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

// x87 conversions round to nearest, ties to even.
int roundToInt(float value) { return static_cast<int>(std::nearbyint(value)); }

} // namespace

UndergrowthConfig parseUndergrowthConfig(const std::string& text)
{
	UndergrowthConfig config;
	std::istringstream lines(text);
	std::string line;
	int depth = 0;
	while (std::getline(lines, line)) {
		std::istringstream words(line);
		std::string key;
		if (!(words >> key)) {
			continue;
		}
		if (key == "{") {
			++depth;
			continue;
		}
		if (key == "}") {
			--depth;
			continue;
		}
		if (depth > 0) {
			continue;
		}
		float value = 0;
		if (!(words >> value)) {
			continue;
		}
		if (key == "ViewDistance") {
			config.viewDistance = value;
		} else if (key == "ViewDistanceFadeScale") {
			config.viewDistanceFadeScale = value;
		} else if (key == "ViewDistanceHeightScale") {
			config.viewDistanceHeightScale = value;
		} else if (key == "ViewDistanceHeight2Scale") {
			config.viewDistanceHeight2Scale = value;
		} else if (key == "ViewDistanceStreamingScale") {
			config.viewDistanceStreamingScale = value;
		} else if (key == "PatchSubdivide") {
			config.patchSubdivide = static_cast<int>(value);
		} else if (key == "SwayScale") {
			config.swayScale = value;
		} else if (key == "LightingScale") {
			config.lightingScale = value;
		} else if (key == "AlphaRef") {
			config.alphaRef = value;
		}
	}
	return config;
}

std::optional<std::vector<UndergrowthMaterial>> parseUndergrowthData(const std::vector<uint8_t>& data,
	std::string* error)
{
	Reader r(data);
	const uint32_t version = r.read<uint32_t>();
	if (version < 1 || version > 2) {
		if (error) {
			*error = "unsupported version";
		}
		return std::nullopt;
	}
	std::vector<UndergrowthMaterial> materials(r.count(16));
	for (UndergrowthMaterial& material : materials) {
		material.name = r.string();
		material.index = r.read<uint32_t>();
		material.generalHeight = r.read<float>();
		material.types.resize(r.count(64));
		for (UndergrowthType& type : material.types) {
			type.name = r.string();
			type.mesh = r.string();
			type.texture = r.string();
			for (float* pair : {type.crossSize.data(), type.sizeRange.data(), type.normalYRange.data()}) {
				pair[0] = r.read<float>();
				pair[1] = r.read<float>();
			}
			type.density = std::clamp(r.read<float>(), 0.01f, 10.0f);
			type.variation = std::clamp(r.read<float>(), -1.0f, 1.0f);
			type.terrainColorScale = r.read<float>();
			if (version > 1) {
				type.swayScale = r.read<float>();
			}
			type.skew = r.read<uint8_t>() != 0;
			type.vertices.resize(r.count(24));
			for (UndergrowthVertex& v : type.vertices) {
				for (float& p : v.position) {
					p = r.read<float>();
				}
				v.texCoord[0] = r.read<uint16_t>();
				v.texCoord[1] = r.read<uint16_t>();
				v.flags = r.read<uint8_t>();
				v.terrainColorMix = r.read<uint8_t>();
				r.read<uint16_t>();
				v.sway = r.read<float>();
			}
			type.indices.resize(r.count(2));
			for (uint16_t& index : type.indices) {
				index = r.read<uint16_t>();
			}
			type.extra.resize(r.count(4));
			for (uint32_t& value : type.extra) {
				value = r.read<uint32_t>();
			}
		}
	}
	if (!r.ok() || r.remaining() != 0) {
		if (error) {
			*error = r.ok() ? "trailing data" : "truncated";
		}
		return std::nullopt;
	}
	for (const UndergrowthMaterial& material : materials) {
		for (const UndergrowthType& type : material.types) {
			for (uint16_t index : type.indices) {
				if (index >= type.vertices.size()) {
					if (error) {
						*error = "index out of range";
					}
					return std::nullopt;
				}
			}
		}
	}
	return materials;
}

uint32_t undergrowthRandom(uint32_t& state)
{
	return game::lehmerNext(state);
}

UndergrowthGenerator::UndergrowthGenerator(std::vector<UndergrowthMaterial> materials, Terrain terrain)
	: m_materials(std::move(materials)), m_terrain(std::move(terrain))
{
	const float area = m_terrain.patchSize * m_terrain.patchSize;
	for (const UndergrowthMaterial& material : m_materials) {
		size_t vertices = 0, indices = 0;
		for (const UndergrowthType& type : material.types) {
			const size_t instances = static_cast<size_t>(roundToInt(type.density * area));
			vertices += instances * type.vertices.size();
			indices += instances * type.indices.size();
		}
		m_vertexCapacity = std::max(m_vertexCapacity, vertices);
		m_indexCapacity = std::max(m_indexCapacity, indices);
	}
	m_vertexCapacity += m_vertexCapacity / 10;
	m_indexCapacity += m_indexCapacity / 10;
}

std::array<float, 2> UndergrowthGenerator::patchOrigin(int patch) const
{
	const int n = m_terrain.patchesPerSide;
	return {static_cast<float>(patch % n - n / 2) * m_terrain.patchSize,
		static_cast<float>(patch / n - n / 2) * m_terrain.patchSize};
}

// 0x1010aa70: samples are centred on the origin; out of range gives 0.
void UndergrowthGenerator::mapSample(float x, float z, int& sx, int& sz) const
{
	sx = static_cast<int>(std::floor(x * (1.0f / m_terrain.sampleSpacing[0]))) + m_terrain.mapWidth / 2;
	sz = static_cast<int>(std::floor(z * (1.0f / m_terrain.sampleSpacing[1]))) + m_terrain.mapHeight / 2;
	if (sx > m_terrain.mapWidth || sx < 0) {
		sx = 0;
	}
	if (sz > m_terrain.mapHeight || sz < 0) {
		sz = 0;
	}
}

uint8_t UndergrowthGenerator::materialAt(float x, float z) const
{
	int sx = 0, sz = 0;
	mapSample(x, z, sx, sz);
	if (sx >= m_terrain.mapWidth || sz >= m_terrain.mapHeight || m_terrain.materialMap.empty()) {
		return 0;
	}
	return m_terrain.materialMap[static_cast<size_t>(sz) * m_terrain.mapWidth + sx];
}

std::array<float, 16> UndergrowthGenerator::materialPresence(int patch) const
{
	const auto origin = patchOrigin(patch);
	int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
	mapSample(origin[0], origin[1], x0, z0);
	mapSample(origin[0] + m_terrain.patchSize, origin[1] + m_terrain.patchSize, x1, z1);
	std::array<uint32_t, 256> counts {};
	for (int z = z0; z <= z1; ++z) {
		for (int x = x0; x <= x1; ++x) {
			if (x < m_terrain.mapWidth && z < m_terrain.mapHeight && !m_terrain.materialMap.empty()) {
				++counts[m_terrain.materialMap[static_cast<size_t>(z) * m_terrain.mapWidth + x]];
			}
		}
	}
	const float total = static_cast<float>((z1 - z0 + 1) * (x1 - x0 + 1));
	std::array<float, 16> presence {};
	for (size_t i = 0; i < presence.size(); ++i) {
		presence[i] = counts[i] / total;
	}
	return presence;
}

UndergrowthPatch UndergrowthGenerator::generate(int patch) const
{
	UndergrowthPatch result;
	const float size = m_terrain.patchSize;
	const auto origin = patchOrigin(patch);
	const float minX = origin[0], minZ = origin[1];
	const float maxX = minX + size, maxZ = minZ + size;
	const std::array<float, 16> presence = materialPresence(patch);
	uint32_t random = static_cast<uint32_t>(patch) & 0x7fffffff;
	constexpr float kDegrees = 3.14159265358979f / 180.0f;

	for (const UndergrowthMaterial& material : m_materials) {
		if (material.index >= presence.size() || presence[material.index] == 0) {
			continue;
		}
		for (const UndergrowthType& type : material.types) {
			if (type.vertices.empty() || type.density == 0) {
				continue;
			}
			const float spread = type.variation / std::sqrt(type.density);
			const int cells = roundToInt(std::sqrt(type.density) * size);
			for (int row = 0; row < cells; ++row) {
				const float fz = static_cast<float>(row) / cells;
				for (int column = 0; column < cells; ++column) {
					const float fx = static_cast<float>(column) / cells;
					const uint32_t r = undergrowthRandom(random);
					float x, z;
					if (type.variation >= 0) {
						// A jittered grid point, wrapped into the patch.
						x = fx * size + ((r & 0xff) * 0.0078125f - 1.0f) * spread + minX;
						z = fz * size + (((r >> 8) & 0xff) * 0.0078125f - 1.0f) * spread + minZ;
						if (x > maxX) {
							x -= size;
						} else if (x < minX) {
							x += size;
						}
						if (z > maxZ) {
							z -= size;
						} else if (z < minZ) {
							z += size;
						}
					} else {
						x = (r & 0xff) * (size * 0.00390625f) + minX;
						z = ((r >> 8) & 0xff) * (size * 0.00390625f) + minZ;
					}
					if (materialAt(x, z) != material.index) {
						continue;
					}
					if (result.vertices.size() + type.vertices.size() > m_vertexCapacity ||
						result.indices.size() + type.indices.size() > m_indexCapacity) {
						continue;
					}
					const std::array<float, 3> up = m_terrain.normal(x, z);
					if (up[1] < type.normalYRange[0] || up[1] > type.normalYRange[1]) {
						continue;
					}
					const float ground = m_terrain.height(x, z);
					const float scale =
						((r >> 16) & 0x3f) * (type.sizeRange[1] - type.sizeRange[0]) * 0.015625f + type.sizeRange[0];
					// One of 32 yaw steps, then tilted onto the terrain normal.
					const float yaw = ((r >> 26) & 0x1f) * 11.25f * kDegrees;
					const std::array<float, 3> zAxis {std::sin(yaw), 0, std::cos(yaw)};
					const std::array<float, 3> xAxis = cross(up, zAxis);
					const std::array<float, 3> tiltedZ = cross(xAxis, up);
					const std::array<float, 3> rows[3] = {xAxis, up, tiltedZ};
					const uint8_t swayIndex = static_cast<uint8_t>((r >> 22) & 0xf);
					// Bytes are converted with _ftol (truncating) and keep the low 8 bits.
					const uint8_t swayBase = static_cast<uint8_t>(static_cast<int>(scale * type.crossSize[1] * 85.0f));

					const size_t first = result.vertices.size();
					for (const UndergrowthVertex& v : type.vertices) {
						std::array<float, 3> offset {};
						for (int axis = 0; axis < 3; ++axis) {
							offset[axis] = (rows[0][axis] * v.position[0] + rows[1][axis] * v.position[1] +
											   rows[2][axis] * v.position[2]) *
								scale;
						}
						UndergrowthPatchVertex out;
						out.position = {x + offset[0], ground, z + offset[2]};
						out.height = offset[1];
						out.texCoord = {v.texCoord[0] / 32767.0f, v.texCoord[1] / 32767.0f};
						out.swayIndex = swayIndex;
						out.sway = static_cast<uint8_t>(static_cast<int>(swayBase * v.sway));
						out.terrainColorMix = v.terrainColorMix;
						out.type = &type;
						result.vertices.push_back(out);
					}
					if (type.skew && type.vertices.size() >= 8) {
						// Vertices 2, 3, 6, 7 move above 1, 0, 5, 4.
						const float lift = scale * type.crossSize[1];
						for (const auto& [to, from] : {std::pair {2, 1}, {3, 0}, {6, 5}, {7, 4}}) {
							UndergrowthPatchVertex& target = result.vertices[first + to];
							const UndergrowthPatchVertex& source = result.vertices[first + from];
							target.position = source.position;
							target.height = source.height + lift;
						}
					}
					for (uint16_t index : type.indices) {
						result.indices.push_back(static_cast<uint32_t>(first + index));
					}
					++result.instances;
				}
			}
		}
	}
	return result;
}

} // namespace dice::hfe::terrain
