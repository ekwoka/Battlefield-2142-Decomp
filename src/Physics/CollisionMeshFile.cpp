#include "CollisionMeshFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dice::hfe::physics {

namespace {

class Reader {
public:
	explicit Reader(const std::vector<uint8_t>& data) : m_data(data) {}
	bool ok() const { return m_ok; }
	bool atEnd() const { return m_pos == m_data.size(); }
	size_t remaining() const { return m_data.size() - m_pos; }

	template <class T>
	T read()
	{
		T value {};
		bytes(&value, sizeof(T));
		return value;
	}

	void bytes(void* out, size_t size)
	{
		if (!m_ok || remaining() < size) {
			m_ok = false;
			return;
		}
		std::memcpy(out, m_data.data() + m_pos, size);
		m_pos += size;
	}

	// A count of items of `itemSize` bytes that must fit in the file.
	uint32_t count(size_t itemSize)
	{
		const uint32_t n = read<uint32_t>();
		if (itemSize > 0 && n > remaining() / itemSize) {
			m_ok = false;
			return 0;
		}
		return n;
	}

	template <class T>
	void array(std::vector<T>& out, uint32_t n)
	{
		out.resize(n);
		if (n > 0) {
			bytes(out.data(), sizeof(T) * n);
		}
	}

private:
	const std::vector<uint8_t>& m_data;
	size_t m_pos = 0;
	bool m_ok = true;
};

} // namespace

const CollisionLod* collisionLodFor(const CollisionMeshFile& file, const CollisionSubGeometry& part, uint32_t type)
{
	if (file.version < 9) {
		return type < part.lods.size() ? &part.lods[type] : nullptr;
	}
	for (const CollisionLod& lod : part.lods) {
		if (lod.collisionType == type) {
			return &lod;
		}
	}
	return nullptr;
}

const CollisionSubGeometry* collisionPart(const CollisionMeshFile& file, size_t geometry, size_t state)
{
	if (geometry >= file.geometries.size() || state >= file.geometries[geometry].subGeometries.size()) {
		return nullptr;
	}
	return &file.geometries[geometry].subGeometries[state];
}

float boxRadius(const std::array<float, 3>& low, const std::array<float, 3>& high)
{
	float farthest = 0;
	for (int i = 0; i < 8; ++i) {
		const float x = (i & 1) ? high[0] : low[0], y = (i & 2) ? high[1] : low[1], z = (i & 4) ? high[2] : low[2];
		farthest = std::max(farthest, x * x + y * y + z * z);
	}
	return std::sqrt(farthest);
}

std::optional<CollisionMeshFile> parseCollisionMesh(const std::vector<uint8_t>& data, std::string* error)
{
	Reader r(data);
	CollisionMeshFile file;
	r.read<uint32_t>();
	file.version = r.read<uint32_t>();
	if (file.version < 8 || file.version > 10) {
		if (error) {
			*error = "unsupported version";
		}
		return std::nullopt;
	}
	file.geometries.resize(r.count(4));
	for (CollisionGeometry& geometry : file.geometries) {
		geometry.subGeometries.resize(r.count(4));
		for (CollisionSubGeometry& sub : geometry.subGeometries) {
			sub.lods.resize(r.count(8));
			for (CollisionLod& lod : sub.lods) {
				if (file.version >= 9) {
					lod.collisionType = r.read<uint32_t>();
				}
				const uint32_t faces = r.count(8);
				lod.faces.resize(faces);
				for (CollisionFace& face : lod.faces) {
					for (uint16_t& v : face.vertices) {
						v = r.read<uint16_t>();
					}
					face.material = r.read<uint16_t>();
				}
				r.array(lod.vertices, r.count(12));
				r.array(lod.vertexMaterials, static_cast<uint32_t>(lod.vertices.size()));
				r.bytes(lod.boundsMin.data(), 12);
				r.bytes(lod.boundsMax.data(), 12);
				r.read<uint8_t>();
				std::array<float, 6> bounds2;
				r.bytes(bounds2.data(), sizeof(bounds2));
				r.array(lod.bspNodes, r.count(16));
				r.array(lod.bspFaceLists, r.count(2));
				if (file.version >= 10) {
					r.array(lod.bspExtra, r.count(4));
				}
				for (const CollisionFace& face : lod.faces) {
					for (uint16_t v : face.vertices) {
						if (v >= lod.vertices.size()) {
							if (error) {
								*error = "face vertex out of range";
							}
							return std::nullopt;
						}
					}
				}
			}
		}
	}
	if (!r.ok() || !r.atEnd()) {
		if (error) {
			*error = r.ok() ? "unexpected data at the end" : "truncated";
		}
		return std::nullopt;
	}
	// What the template's loader (0x006d8740, its loop over 0x006e4880) keeps.
	// A trailing run of geometries without a LOD is dropped (unless it
	// starts at geometry 0), and when no geometry's sub-geometry 0 has a
	// LOD (0x006de510), every geometry loses its first sub-geometry
	// (0x006e5820): the shipped vehicles' files lead each geometry with an
	// empty one, so their normal shape becomes sub-geometry 0 and the wreck
	// sub-geometry 1, the states a mesh instance's +0x14 picks.
	const auto hasLod = [](const CollisionGeometry& g) {
		return std::any_of(g.subGeometries.begin(), g.subGeometries.end(),
			[](const CollisionSubGeometry& sub) { return !sub.lods.empty(); });
	};
	size_t keep = file.geometries.size();
	while (keep > 0 && !hasLod(file.geometries[keep - 1])) {
		--keep;
	}
	if (keep > 0) {
		file.geometries.resize(keep);
	}
	const bool firstHasLod = std::any_of(file.geometries.begin(), file.geometries.end(),
		[](const CollisionGeometry& g) { return !g.subGeometries.empty() && !g.subGeometries[0].lods.empty(); });
	if (!firstHasLod) {
		for (CollisionGeometry& g : file.geometries) {
			if (!g.subGeometries.empty()) {
				g.subGeometries.erase(g.subGeometries.begin());
			}
		}
	}
	return file;
}

} // namespace dice::hfe::physics
