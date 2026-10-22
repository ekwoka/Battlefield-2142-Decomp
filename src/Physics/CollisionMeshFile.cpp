#include "CollisionMeshFile.h"

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
				std::vector<uint16_t> vertexIds;
				r.array(vertexIds, static_cast<uint32_t>(lod.vertices.size()));
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
	return file;
}

} // namespace dice::hfe::physics
