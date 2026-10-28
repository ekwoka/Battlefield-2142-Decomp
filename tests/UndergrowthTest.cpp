// Undergrowth: the random generator, the shipped files and patch generation.
#include "IO/File/ZipArchive.h"
#include "TestMain.h"
#include "Terrain/TerrainData.h"
#include "Terrain/Undergrowth.h"
#include "IO/File/FileManager.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
namespace terrain = dice::hfe::terrain;

// Two Lehmer steps are one step with the product of the multipliers.
TEST(Undergrowth_RandomIsLehmer)
{
	for (uint32_t seed : {1u, 2u, 12345u, 0x7ffffffeu, 987654321u}) {
		uint32_t state = seed;
		uint64_t expected = seed;
		for (int i = 0; i < 100; ++i) {
			expected = expected * (24112ull * 26143ull) % 0x7fffffffull;
			CHECK(terrain::undergrowthRandom(state) == expected);
		}
	}
	uint32_t zero = 0;
	CHECK(terrain::undergrowthRandom(zero) == 0);
}

TEST(Undergrowth_ParsesShippedLevels)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	int levels = 0, types = 0;
	for (const auto& entry : fs::directory_iterator(fs::path(gameDir) / "mods/bf2142/Levels")) {
		auto zip = dice::hfe::io::ZipArchive::open((entry.path() / "client.zip").string());
		std::vector<uint8_t> data;
		if (!zip || !zip->read("Undergrowth.dat", data)) {
			continue;
		}
		std::string error;
		const auto materials = terrain::parseUndergrowthData(data, &error);
		CHECK(materials.has_value());
		if (!materials) {
			std::fprintf(stderr, "  %s: %s\n", entry.path().filename().string().c_str(), error.c_str());
			continue;
		}
		for (const auto& material : *materials) {
			types += static_cast<int>(material.types.size());
		}
		++levels;
	}
	std::printf("  %d levels, %d undergrowth types\n", levels, types);
	CHECK(levels >= 99);
	CHECK(types > 700);
}

// A flat terrain covered in one material: every grid point keeps an
// instance, the layout repeats for a patch and differs between patches.
TEST(Undergrowth_GeneratesPatches)
{
	terrain::UndergrowthType grass;
	grass.density = 1;
	grass.variation = 0.5f;
	grass.sizeRange = {1, 2};
	grass.vertices = {{{0, 0, 0}, {0, 0}, 0, 0, 0}, {{0, 1, 0}, {0, 32767}, 0, 0, 0.5f}, {{1, 0, 0}, {32767, 0}, 0, 0, 0}};
	grass.indices = {0, 1, 2};
	terrain::UndergrowthMaterial material;
	material.index = 3;
	material.types = {grass};
	terrain::UndergrowthGenerator::Terrain ground;
	ground.mapWidth = ground.mapHeight = 65;
	ground.materialMap.assign(65 * 65, 3);
	ground.sampleSpacing = {1, 1};
	ground.patchesPerSide = 8;
	ground.patchSize = 8;
	ground.height = [](float, float) { return 10.0f; };
	ground.normal = [](float, float) { return std::array<float, 3> {0, 1, 0}; };
	const terrain::UndergrowthGenerator generator({material}, ground);
	// 64 grid points, plus a tenth.
	CHECK(generator.vertexCapacity() == 64 * 3 + 64 * 3 / 10);

	const auto patch = generator.generate(9);
	CHECK(patch.instances == 64);
	CHECK(patch.indices.size() == patch.instances * 3);
	const auto origin = generator.patchOrigin(9);
	CHECK(origin[0] == -24 && origin[1] == -24);
	for (const auto& v : patch.vertices) {
		CHECK(v.position[1] == 10.0f);
		CHECK(v.height >= 0 && v.height <= 2);
	}
	// The bottom vertex of each instance lies in the patch.
	for (size_t i = 0; i < patch.vertices.size(); i += 3) {
		const auto& p = patch.vertices[i].position;
		CHECK(p[0] >= origin[0] && p[0] <= origin[0] + 8 && p[2] >= origin[1] && p[2] <= origin[1] + 8);
	}
	const auto again = generator.generate(9);
	CHECK(again.vertices.size() == patch.vertices.size());
	CHECK(again.vertices[0].position == patch.vertices[0].position);
	const auto other = generator.generate(10);
	CHECK(other.vertices[0].position[0] - origin[0] - 8 != patch.vertices[0].position[0] - origin[0]);

	// No instances where the map holds another material.
	ground.materialMap.assign(65 * 65, 2);
	const terrain::UndergrowthGenerator bare({material}, ground);
	CHECK(bare.generate(9).instances == 0);
}

// terraindata.raw walked to its end (RendDX9 0x100b3760 and its callees):
// the shared LOD index tree, the patches' compiled meshes, the
// surrounding terrains and the culling tree.
TEST(TerrainData_PatchMeshesToTheEnd)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	dice::hfe::io::FileManager files;
	files.setRootPath(dir);
	files.addSearchPath("mods/bf2142");
	files.setMountingAllowed(true);
	files.mountArchive("Levels/Cerbere_Landing/client.zip", "Levels/Cerbere_Landing/");
	files.mountArchive("Levels/Cerbere_Landing/server.zip", "Levels/Cerbere_Landing/");
	std::vector<uint8_t> data;
	if (!files.readFile("Levels/Cerbere_Landing/terraindata.raw", data)) {
		std::fprintf(stderr, "  no terraindata.raw\n");
		return;
	}
	std::string error;
	const auto terrain = dice::hfe::terrain::parseTerrainData(data, &error);
	CHECK(terrain.has_value());
	if (!terrain) {
		return;
	}
	CHECK(terrain->meshes.has_value());
	if (!terrain->meshes) {
		return;
	}
	const auto& m = *terrain->meshes;
	std::fprintf(stderr, "  %zu patches, %zu shared indices, %d surrounding records, %d culling nodes, heights %.1f..%.1f\n",
		m.patches.size(), m.indices.size(), m.surroundingRecords, m.cullingNodes, terrain->lowest, terrain->highest);
	CHECK(m.patches.size() == static_cast<size_t>(terrain->patchesPerSide * terrain->patchesPerSide));
	CHECK(m.nodes.size() == 85 && m.cullingNodes == 1365);
	// The root's LODs: every vertex, every second, every fourth.
	CHECK(m.nodes[0][0][0].count == 98304 && m.nodes[0][1][0].count == 24576 && m.nodes[0][2][0].count == 6144);
	const size_t side = static_cast<size_t>(terrain->patchSize) + 1;
	bool rows = true, heights = true;
	for (const auto& patch : m.patches) {
		for (size_t i = 0; i < patch.vertices.size(); i += 97) {
			const auto& v = patch.vertices[i];
			rows = rows && v.row == i / side && v.column == i % side;
			const float metres = v.height * terrain->primaryWorldScale[1];
			heights = heights && metres >= patch.lowest - 1e-3f && metres <= patch.highest + 1e-3f;
		}
	}
	CHECK(rows && heights);
}
