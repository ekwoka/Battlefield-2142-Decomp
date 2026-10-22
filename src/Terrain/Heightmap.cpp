#include "Heightmap.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "IO/File/FileManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
}

} // namespace dice::hfe::terrain
