#include "Heightmap.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "IO/File/FileManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dice::hfe::terrain {

void Heightmap::setSize(int width, int height)
{
	m_width = std::max(width, 0);
	m_height = std::max(height, 0);
	m_samples.clear();
}

bool Heightmap::load(const io::FileManager& files, const std::string& path)
{
	std::vector<uint8_t> data;
	const size_t count = static_cast<size_t>(m_width) * m_height;
	const size_t bytesPerSample = m_bits == 8 ? 1 : 2;
	if (count == 0 || !files.readFile(path, data) || data.size() < count * bytesPerSample) {
		return false;
	}
	m_samples.resize(count);
	if (bytesPerSample == 1) {
		std::copy(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(count), m_samples.begin());
	} else {
		std::memcpy(m_samples.data(), data.data(), count * 2); // little-endian
	}
	return true;
}

float Heightmap::sample(int x, int y) const
{
	x = std::clamp(x, 0, m_width - 1);
	y = std::clamp(y, 0, m_height - 1);
	return m_samples[static_cast<size_t>(y) * m_width + x] * m_scale[1];
}

// The primary heightmap is centred on the origin; secondary ones sit next
// to it, each covering as much ground as the primary.
float Heightmap::originX() const
{
	return -worldWidth() / 2 + m_clusterX * worldWidth();
}

float Heightmap::originZ() const
{
	return -worldDepth() / 2 + m_clusterY * worldDepth();
}

float Heightmap::heightAt(float worldX, float worldZ) const
{
	if (!loaded()) {
		return 0;
	}
	const float fx = (worldX - originX()) / m_scale[0];
	const float fy = (worldZ - originZ()) / m_scale[2];
	if (fx < 0 || fy < 0 || fx > m_width - 1 || fy > m_height - 1) {
		return 0;
	}
	const int x = static_cast<int>(fx);
	const int y = static_cast<int>(fy);
	const float tx = fx - x;
	const float ty = fy - y;
	const float top = sample(x, y) * (1 - tx) + sample(x + 1, y) * tx;
	const float bottom = sample(x, y + 1) * (1 - tx) + sample(x + 1, y + 1) * tx;
	return top * (1 - ty) + bottom * ty;
}

float Heightmap::heightAndNormal(float worldX, float worldZ, std::array<float, 3>& normal) const
{
	normal = {0, 1, 0};
	if (!loaded()) {
		return 0;
	}
	const float gx = (worldX - originX()) / m_scale[0];
	const float gz = (worldZ - originZ()) / m_scale[2];
	if (gx < 0 || gz < 0 || gx > m_width - 1 || gz > m_height - 1) {
		return 0;
	}
	const int x = static_cast<int>(std::floor(gx));
	const int z = static_cast<int>(std::floor(gz));
	const float fx = gx - x, fz = gz - z;
	const float h00 = sample(x, z), h10 = sample(x + 1, z), h01 = sample(x, z + 1);
	// The slopes per metre along x and z (the original scales both by x's).
	const float k = 1 / m_scale[0];
	float height;
	if (fx + fz <= 1) {
		height = h00 + fx * (h10 - h00) + fz * (h01 - h00);
		normal = {-(h10 - h00) * k, 1, -(h01 - h00) * k};
	} else {
		const float h11 = sample(x + 1, z + 1);
		height = h11 + (1 - fx) * (h01 - h11) + (1 - fz) * (h10 - h11);
		normal = {(h01 - h11) * k, 1, (h10 - h11) * k};
	}
	const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
	normal = {normal[0] / length, normal[1] / length, normal[2] / length};
	return height;
}

bool Heightmap::raycast(const std::array<float, 3>& from, const std::array<float, 3>& to,
	float& fraction, std::array<float, 3>& normal, std::array<float, 3>* point) const
{
	if (!loaded() || m_width < 2 || m_height < 2 || m_scale[0] <= 0 || m_scale[2] <= 0) return false;
	using V = std::array<float, 3>;
	const auto sub = [](const V& a, const V& b) -> V { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; };
	const auto dot = [](const V& a, const V& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
	const auto cross = [](const V& a, const V& b) -> V { return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; };
	const V direction = sub(to, from);
	// Limit traversal to the rectangle occupied by this heightmap.
	float enter = 0, leave = 1;
	const auto clip = [&](float p, float d, float low, float high) {
		if (std::fabs(d) < 1e-8f) return p >= low && p <= high;
		float a = (low - p) / d, b = (high - p) / d;
		if (a > b) std::swap(a, b);
		enter = std::max(enter, a); leave = std::min(leave, b);
		return enter <= leave;
	};
	if (!clip(from[0], direction[0], originX(), originX() + worldWidth())
		|| !clip(from[2], direction[2], originZ(), originZ() + worldDepth())) return false;
	const float gx = (from[0] + direction[0]*enter - originX()) / m_scale[0];
	const float gz = (from[2] + direction[2]*enter - originZ()) / m_scale[2];
	int x = std::clamp(static_cast<int>(std::floor(gx)), 0, m_width - 2);
	int z = std::clamp(static_cast<int>(std::floor(gz)), 0, m_height - 2);
	const int sx = (direction[0] > 0) - (direction[0] < 0);
	const int sz = (direction[2] > 0) - (direction[2] < 0);
	const float inf = std::numeric_limits<float>::infinity();
	const float stepx = sx ? m_scale[0] / std::fabs(direction[0]) : inf;
	const float stepz = sz ? m_scale[2] / std::fabs(direction[2]) : inf;
	float nextx = sx ? (originX() + (x + (sx > 0)) * m_scale[0] - from[0]) / direction[0] : inf;
	float nextz = sz ? (originZ() + (z + (sz > 0)) * m_scale[2] - from[2]) / direction[2] : inf;
	float best = 2, cellEnter = enter;
	V bestNormal {}, bestPoint {};
	const auto triangle = [&](const V& a, const V& b, const V& c) {
		const V e1 = sub(b, a), e2 = sub(c, a), h = cross(direction, e2);
		V n = cross(e1, e2);
		if (n[1] < 0) for (float& component : n) component = -component;
		if (n[1] <= 0 || dot(direction, n) >= 0) return;
		const float length = std::sqrt(dot(n, n));
		if (length == 0) return;
		for (float& component : n) component /= length;
		// Retail snaps an already-submerged starting point to this triangle
		// when its motion continues into the ground (0x006d21fe).
		const V q0 {from[0] + direction[0]*cellEnter, from[1] + direction[1]*cellEnter,
			from[2] + direction[2]*cellEnter};
		const auto side = [](const V& p, const V& q, const V& r) {
			return (q[0]-p[0])*(r[2]-p[2]) - (q[2]-p[2])*(r[0]-p[0]);
		};
		const float ab = side(a,b,q0), bc = side(b,c,q0), ca = side(c,a,q0);
		const bool inside = (ab >= -1e-6f && bc >= -1e-6f && ca >= -1e-6f)
			|| (ab <= 1e-6f && bc <= 1e-6f && ca <= 1e-6f);
		const float surface = a[1] - (n[0]*(q0[0]-a[0]) + n[2]*(q0[2]-a[2])) / n[1];
		if (inside && q0[1] < surface && cellEnter < best) {
			best = cellEnter; bestNormal = n; bestPoint = {q0[0], surface, q0[2]};
			return;
		}
		const float determinant = dot(e1, h);
		if (std::fabs(determinant) < 1e-8f) return;
		const float inv = 1.0f / determinant;
		const V v = sub(from, a);
		const float u = dot(v, h) * inv;
		if (u < -1e-6f || u > 1.000001f) return;
		const V q = cross(v, e1);
		const float w = dot(direction, q) * inv;
		if (w < -1e-6f || u+w > 1.000001f) return;
		const float t = dot(e2, q) * inv;
		if (t < 0 || t > 1 || t >= best) return;
		best = t; bestNormal = n;
		bestPoint = {from[0] + direction[0]*t, from[1] + direction[1]*t, from[2] + direction[2]*t};
	};
	while (x >= 0 && x < m_width-1 && z >= 0 && z < m_height-1) {
		const float x0 = originX() + x*m_scale[0], z0 = originZ() + z*m_scale[2];
		const V a {x0, sample(x,z), z0}, b {x0+m_scale[0], sample(x+1,z), z0};
		const V c {x0, sample(x,z+1), z0+m_scale[2]}, d {x0+m_scale[0], sample(x+1,z+1), z0+m_scale[2]};
		triangle(a, b, c); triangle(d, c, b);
		const float next = std::min(nextx, nextz);
		if (best <= next + 1e-6f || next > leave || (sx == 0 && sz == 0)) break;
		cellEnter = next;
		if (nextx <= nextz) { x += sx; nextx += stepx; }
		if (nextz <= next + 1e-6f) { z += sz; nextz += stepz; }
	}
	if (best > 1) return false;
	fraction = best; normal = bestNormal;
	if (point) *point = bestPoint;
	return true;
}

bool Heightmap::loadMaterials(const io::FileManager& files, const std::string& path)
{
	m_materials.clear();
	std::vector<uint8_t> data;
	const size_t count = static_cast<size_t>(m_width) * m_height * m_materialScale * m_materialScale;
	if (count == 0 || !files.readFile(path, data) || data.size() < count) {
		return false;
	}
	data.resize(count);
	m_materials = std::move(data);
	return true;
}

uint32_t Heightmap::material(int x, int y) const
{
	if (m_materials.empty()) {
		return 0;
	}
	// Unsigned, as the engine compares: negative is outside too.
	const auto ux = static_cast<uint32_t>(x + m_width / 2);
	const auto uy = static_cast<uint32_t>(y + m_height / 2);
	if (ux >= static_cast<uint32_t>(m_width) || uy >= static_cast<uint32_t>(m_height)) {
		return 0;
	}
	const size_t s = static_cast<size_t>(m_materialScale);
	return m_materials[(static_cast<size_t>(m_width) * uy * s + ux) * s];
}

uint32_t Heightmap::materialAt(float worldX, float worldZ) const
{
	// The secondary heightmaps sit a whole primary's extent away (originX).
	const float x = worldX - m_clusterX * worldWidth();
	const float z = worldZ - m_clusterY * worldDepth();
	return material(static_cast<int>(std::floor(x / m_scale[0])), static_cast<int>(std::floor(z / m_scale[2])));
}

bool Heightmap::covers(float worldX, float worldZ) const
{
	const float fx = worldX - originX(), fz = worldZ - originZ();
	return m_width > 0 && fx >= 0 && fz >= 0 && fx <= worldWidth() && fz <= worldDepth();
}

uint32_t HeightmapCluster::materialAt(float worldX, float worldZ) const
{
	if (const Heightmap* first = primary(); first && first->covers(worldX, worldZ)) {
		return first->materialAt(worldX, worldZ);
	}
	for (const auto& heightmap : m_heightmaps) {
		if (heightmap->covers(worldX, worldZ)) {
			return heightmap->materialAt(worldX, worldZ);
		}
	}
	return 0;
}

Heightmap* HeightmapCluster::addHeightmap(int x, int y)
{
	m_heightmaps.push_back(std::make_unique<Heightmap>(x, y));
	m_active = m_heightmaps.back().get();
	return m_active;
}

const Heightmap* HeightmapCluster::primary() const
{
	for (const auto& heightmap : m_heightmaps) {
		if (heightmap->clusterX() == 0 && heightmap->clusterY() == 0) {
			return heightmap.get();
		}
	}
	return nullptr;
}

void registerTerrainConsoleMethods(Console& console, HeightmapCluster& cluster, const io::FileManager& files)
{
	const uint32_t access = ConsoleAccess::Anyone;
	bindMethod(console, "heightmapcluster.create", access, [](std::string) {});
	bindMethod(console, "heightmapcluster.setClusterSize", access, [&cluster](int size) { cluster.setClusterSize(size); });
	bindMethod(console, "heightmapcluster.setHeightmapSize", access,
		[&cluster](int size) { cluster.setHeightmapSize(size); });
	bindMethod(console, "heightmapcluster.setSeaWaterLevel", access,
		[&cluster](float level) { cluster.setSeaWaterLevel(level); });
	bindMethod(console, "heightmapcluster.addHeightmap", access,
		[&cluster](std::string, int x, int y) { cluster.addHeightmap(x, y); });

	// These apply to the heightmap added last.
	bindMethod(console, "heightmap.setSize", access, [&cluster](int width, int height) {
		if (Heightmap* heightmap = cluster.active()) {
			heightmap->setSize(width, height);
		}
	});
	bindMethod(console, "heightmap.setScale", access, [&cluster](ConsoleVec3 scale) {
		if (Heightmap* heightmap = cluster.active()) {
			heightmap->setScale({scale.x, scale.y, scale.z});
		}
	});
	bindMethod(console, "heightmap.setBitResolution", access, [&cluster](int bits) {
		if (Heightmap* heightmap = cluster.active()) {
			heightmap->setBitResolution(bits);
		}
	});
	bindMethod(console, "heightmap.loadHeightData", access, [&cluster, &files](std::string path) {
		if (Heightmap* heightmap = cluster.active()) {
			heightmap->load(files, path);
		}
	});
	bindMethod(console, "heightmap.setMaterialScale", access, [&cluster](int scale) {
		if (Heightmap* heightmap = cluster.active()) {
			heightmap->setMaterialScale(scale);
		}
	});
	bindMethod(console, "heightmap.loadMaterialData", access, [&cluster, &files](std::string path) {
		if (Heightmap* heightmap = cluster.active()) {
			heightmap->loadMaterials(files, path);
		}
	});
}

} // namespace dice::hfe::terrain
