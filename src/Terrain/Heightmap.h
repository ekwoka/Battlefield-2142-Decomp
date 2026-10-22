// Terrain heights ("heightmapcluster.*" and "heightmap.*" in Heightdata.con).
//
// A level has a primary heightmap (e.g. 1025x1025 samples, 2 m apart) at
// cluster position 0/0, centred on the world origin, and lower-resolution
// secondary heightmaps around it at positions -1..1 for the distant
// scenery. Samples are unsigned 8- or 16-bit integers in a .raw file, row
// by row; height = sample * scale.y.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
namespace io {
class FileManager;
}
} // namespace dice::hfe

namespace dice::hfe::terrain {

class Heightmap {
public:
	Heightmap(int clusterX, int clusterY) : m_clusterX(clusterX), m_clusterY(clusterY) {}

	int clusterX() const { return m_clusterX; }
	int clusterY() const { return m_clusterY; }

	void setSize(int width, int height);
	int width() const { return m_width; }
	int height() const { return m_height; }
	void setScale(const std::array<float, 3>& scale) { m_scale = scale; }
	const std::array<float, 3>& scale() const { return m_scale; }
	void setBitResolution(int bits) { m_bits = bits; }
	bool load(const io::FileManager& files, const std::string& path);
	bool loaded() const { return !m_samples.empty(); }

	// Height in metres of sample (x, y).
	float sample(int x, int y) const;
	// World-space extent along x and z.
	float worldWidth() const { return (m_width - 1) * m_scale[0]; }
	float worldDepth() const { return (m_height - 1) * m_scale[2]; }
	// World position of sample (0, 0) along x and z.
	float originX() const;
	float originZ() const;
	// Height at a world position, bilinearly filtered; 0 outside.
	float heightAt(float worldX, float worldZ) const;

private:
	int m_clusterX;
	int m_clusterY;
	int m_width = 0;
	int m_height = 0;
	std::array<float, 3> m_scale {1, 1, 1};
	int m_bits = 16;
	std::vector<uint16_t> m_samples;
};

class HeightmapCluster {
public:
	void setClusterSize(int size) { m_clusterSize = size; }
	void setHeightmapSize(int size) { m_heightmapSize = size; }
	int heightmapSize() const { return m_heightmapSize; }

	Heightmap* addHeightmap(int x, int y);
	// "heightmapcluster.setSeaWaterLevel": the sea's height; none if unset.
	void setSeaWaterLevel(float level) { m_seaWaterLevel = level; m_hasSea = true; }
	bool hasSea() const { return m_hasSea; }
	float seaWaterLevel() const { return m_seaWaterLevel; }
	Heightmap* active() const { return m_active; }
	const Heightmap* primary() const;
	const std::vector<std::unique_ptr<Heightmap>>& heightmaps() const { return m_heightmaps; }

private:
	int m_clusterSize = 1;
	float m_seaWaterLevel = 0;
	bool m_hasSea = false;
	int m_heightmapSize = 0;
	std::vector<std::unique_ptr<Heightmap>> m_heightmaps;
	Heightmap* m_active = nullptr;
};

void registerTerrainConsoleMethods(Console& console, HeightmapCluster& cluster, const io::FileManager& files);

} // namespace dice::hfe::terrain
