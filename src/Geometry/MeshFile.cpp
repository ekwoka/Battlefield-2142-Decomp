#include "MeshFile.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace dice::hfe::geom {

namespace {

class Reader {
public:
	explicit Reader(const std::vector<uint8_t>& data) : m_data(data) {}

	bool ok() const { return m_ok; }
	size_t position() const { return m_pos; }
	size_t remaining() const { return m_data.size() - m_pos; }

	template <class T>
	T read()
	{
		T value {};
		if (!need(sizeof(T))) {
			return value;
		}
		std::memcpy(&value, m_data.data() + m_pos, sizeof(T));
		m_pos += sizeof(T);
		return value;
	}

	// A count that must fit in the rest of the file at `minBytes` per item.
	uint32_t count(size_t minBytes)
	{
		const int32_t n = read<int32_t>();
		if (n < 0 || (minBytes > 0 && static_cast<size_t>(n) > remaining() / minBytes)) {
			m_ok = false;
			return 0;
		}
		return static_cast<uint32_t>(n);
	}

	void bytes(void* out, size_t size)
	{
		if (need(size)) {
			std::memcpy(out, m_data.data() + m_pos, size);
			m_pos += size;
		}
	}

	void skip(size_t size)
	{
		if (need(size)) {
			m_pos += size;
		}
	}

	std::string string()
	{
		const uint32_t length = count(1);
		std::string text;
		if (need(length)) {
			text.assign(reinterpret_cast<const char*>(m_data.data() + m_pos), length);
			m_pos += length;
		}
		return text;
	}

	void floats(float* out, size_t n) { bytes(out, n * sizeof(float)); }

private:
	bool need(size_t size)
	{
		if (!m_ok || remaining() < size) {
			m_ok = false;
			return false;
		}
		return true;
	}

	const std::vector<uint8_t>& m_data;
	size_t m_pos = 0;
	bool m_ok = true;
};

} // namespace

std::optional<MeshKind> meshKindFromExtension(const std::string& path)
{
	const size_t dot = path.rfind('.');
	if (dot == std::string::npos) {
		return std::nullopt;
	}
	std::string extension = path.substr(dot + 1);
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (extension == "staticmesh") {
		return MeshKind::Static;
	}
	if (extension == "bundledmesh") {
		return MeshKind::Bundled;
	}
	if (extension == "skinnedmesh") {
		return MeshKind::Skinned;
	}
	return std::nullopt;
}

const VertexElement* MeshFile::element(VertexUsage usage, uint8_t usageIndex) const
{
	for (const VertexElement& e : vertexElements) {
		if (e.usage == usage && e.usageIndex == usageIndex) {
			return &e;
		}
	}
	return nullptr;
}

bool MeshFile::readFloats(uint32_t vertex, const VertexElement& e, float* out, int count) const
{
	const int available = static_cast<int>(e.type) + 1; // Float1..Float4
	if (e.type == VertexType::D3DColor || vertex >= vertexCount || count > available) {
		return false;
	}
	const size_t offset = static_cast<size_t>(vertex) * vertexStride + e.offset;
	if (offset + sizeof(float) * count > vertices.size()) {
		return false;
	}
	std::memcpy(out, vertices.data() + offset, sizeof(float) * count);
	return true;
}

std::optional<MeshFile> parseMesh(const std::vector<uint8_t>& data, MeshKind kind, std::string* error)
{
	const auto fail = [&](const char* why) -> std::optional<MeshFile> {
		if (error) {
			*error = why;
		}
		return std::nullopt;
	};

	Reader r(data);
	MeshFile mesh;
	mesh.kind = kind;
	r.read<int32_t>();
	mesh.version = static_cast<uint32_t>(r.read<int32_t>());
	r.skip(3 * sizeof(int32_t) + 1);
	const bool skinned = kind == MeshKind::Skinned;

	const uint32_t geometryCount = r.count(4);
	std::vector<uint32_t> lodCounts(geometryCount);
	for (uint32_t& lods : lodCounts) {
		lods = r.count(0);
	}

	const uint32_t elementCount = r.count(8);
	for (uint32_t i = 0; i < elementCount; ++i) {
		const uint16_t stream = r.read<uint16_t>();
		const uint16_t offset = r.read<uint16_t>();
		const uint16_t type = r.read<uint16_t>();
		const uint16_t usage = r.read<uint16_t>();
		if (stream == 0xff) {
			continue; // D3DDECL_END
		}
		mesh.vertexElements.push_back({offset, static_cast<VertexType>(type), static_cast<VertexUsage>(usage & 0xff),
			static_cast<uint8_t>(usage >> 8)});
	}

	r.read<int32_t>(); // bytes per vertex element unit, always 4
	mesh.vertexStride = r.count(0);
	mesh.vertexCount = r.count(std::max<uint32_t>(mesh.vertexStride, 1));
	mesh.vertices.resize(static_cast<size_t>(mesh.vertexStride) * mesh.vertexCount);
	r.bytes(mesh.vertices.data(), mesh.vertices.size());

	const uint32_t indexCount = r.count(2);
	mesh.indices.resize(indexCount);
	r.bytes(mesh.indices.data(), mesh.indices.size() * sizeof(uint16_t));
	if (!skinned) {
		r.read<int32_t>();
	}
	if (!r.ok()) {
		return fail("truncated vertex or index data");
	}

	mesh.geometries.resize(geometryCount);
	for (uint32_t g = 0; g < geometryCount; ++g) {
		mesh.geometries[g].lods.resize(lodCounts[g]);
		for (MeshLod& lod : mesh.geometries[g].lods) {
			r.floats(lod.bounds.min.data(), 3);
			r.floats(lod.bounds.max.data(), 3);
			if (mesh.version <= 6) {
				r.skip(3 * sizeof(float)); // pivot
			}
			if (skinned) {
				lod.rigs.resize(r.count(4));
				for (auto& rig : lod.rigs) {
					rig.resize(r.count(68));
					for (Bone& bone : rig) {
						bone.id = r.read<int32_t>();
						r.floats(bone.matrix.data(), 16);
					}
				}
			} else if (kind == MeshKind::Bundled) {
				lod.partCount = r.count(0);
			} else {
				lod.nodes.resize(r.count(64));
				for (auto& node : lod.nodes) {
					r.floats(node.data(), 16);
				}
			}
		}
	}

	for (MeshGeometry& geometry : mesh.geometries) {
		for (MeshLod& lod : geometry.lods) {
			lod.materials.resize(r.count(8));
			for (MeshMaterial& material : lod.materials) {
				if (!skinned) {
					material.alphaMode = static_cast<uint32_t>(r.read<int32_t>());
				}
				material.effect = r.string();
				material.technique = r.string();
				material.maps.resize(r.count(4));
				for (std::string& map : material.maps) {
					map = r.string();
				}
				material.vertexStart = static_cast<uint32_t>(r.read<int32_t>());
				material.indexStart = static_cast<uint32_t>(r.read<int32_t>());
				material.indexCount = static_cast<uint32_t>(r.read<int32_t>());
				material.vertexCount = static_cast<uint32_t>(r.read<int32_t>());
				r.skip(2 * sizeof(int32_t));
				if (mesh.version == 11 && !skinned) {
					r.skip(6 * sizeof(float)); // material bounds
				}
				if (static_cast<uint64_t>(material.indexStart) + material.indexCount > mesh.indices.size() ||
					static_cast<uint64_t>(material.vertexStart) + material.vertexCount > mesh.vertexCount) {
					return fail("material range outside the mesh");
				}
			}
		}
	}
	if (!r.ok()) {
		return fail("truncated LOD or material data");
	}
	if (r.remaining() != 0) {
		return fail("unexpected data after the materials");
	}
	return mesh;
}

} // namespace dice::hfe::geom
