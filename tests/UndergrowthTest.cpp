// Undergrowth: the random generator, the shipped files and patch generation.
#include "IO/File/ZipArchive.h"
#include "TestMain.h"
#include "Render/TerrainLod.h"
#include "Terrain/TerrainData.h"
#include "Terrain/Undergrowth.h"
#include "IO/File/FileManager.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
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

// The main terrain's LOD cells (Render/TerrainLod.h) drawn from the compiled
// meshes leave no cracks: wherever two visible cells meet, each one's edge
// vertices lie on the other's edge as drawn (the geomorphs and the stitched
// edges' adders), and no cell pops: every vertex of a LOD l cell is fully
// morphed under LOD l - 1's limits, so it looked the same just before.
TEST(TerrainLod_NoCracksOrPops)
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
		return;
	}
	const auto t = terrain::parseTerrainData(data);
	CHECK(t && t->meshes);
	if (!t || !t->meshes) {
		return;
	}
	namespace render = dice::hfe::render;
	const auto& m = *t->meshes;
	const int quads = t->patchSize, side = quads + 1, patches = t->patchesPerSide;
	const float metres = t->primaryWorldScale[0], yScale = t->primaryWorldScale[1];
	const float origin = -0.5f * quads * patches * metres;
	const dice::hfe::scene::RendererSettings settings; // 64 m cells: 32 quads, nodes 5-20
	const int cellQuads = static_cast<int>(settings.baseCellSize / metres);
	CHECK(cellQuads == 32);
	const int perPatch = quads / cellQuads, perSide = patches * perPatch;
	// The level-2 node at each corner of a patch.
	std::vector<int> nodeAt(static_cast<size_t>(perPatch * perPatch), -1);
	for (int n = 5; n < 21; ++n) {
		const auto& r = m.nodes[static_cast<size_t>(n)][0][0];
		int x0 = quads, z0 = quads;
		for (int i = r.start; i < r.start + r.count; ++i) {
			x0 = std::min(x0, m.indices[static_cast<size_t>(i)] % side);
			z0 = std::min(z0, m.indices[static_cast<size_t>(i)] / side);
		}
		nodeAt[static_cast<size_t>((z0 / cellQuads) * perPatch + x0 / cellQuads)] = n;
	}
	struct Camera {
		float x, z, height;
	};
	int cracks = 0, pops = 0, shared = 0;
	for (const Camera& camera : {Camera {0, 0, 2}, Camera {130, -70, 12}, Camera {-300, 260, 1.5f}, Camera {37, 411, 40},
			 Camera {450, -450, 0.5f}, Camera {-100, -480, 80}, Camera {200, 100, 5}, Camera {-17, 33, 160}}) {
		std::vector<render::TerrainLodCell> cells(static_cast<size_t>(perSide * perSide));
		for (int cz = 0; cz < perSide; ++cz) {
			for (int cx = 0; cx < perSide; ++cx) {
				auto& cell = cells[static_cast<size_t>(cz * perSide + cx)];
				cell.visible = true;
				cell.boundsMin = {origin + cx * cellQuads * metres, 0, origin + cz * cellQuads * metres};
				cell.boundsMax = {cell.boundsMin.x + cellQuads * metres, 0, cell.boundsMin.z + cellQuads * metres};
			}
		}
		const float lodSwitch = render::terrainLodSwitch(1, settings);
		CHECK(lodSwitch == 75);
		render::pickTerrainLods(cells, perSide, camera.x, camera.z, camera.height, lodSwitch, settings.lod2Start);
		// Each cell's drawn vertices on its four edges, by global grid point:
		// the height it draws them at.
		std::vector<std::map<std::pair<int, int>, float>> edges(cells.size());
		for (int cz = 0; cz < perSide; ++cz) {
			for (int cx = 0; cx < perSide; ++cx) {
				const size_t c = static_cast<size_t>(cz * perSide + cx);
				const auto& cell = cells[c];
				const auto& patch = m.patches[static_cast<size_t>((cz / perPatch) * patches + cx / perPatch)];
				const int node = nodeAt[static_cast<size_t>((cz % perPatch) * perPatch + cx % perPatch)];
				auto range = m.nodes[static_cast<size_t>(node)][static_cast<size_t>(cell.lod)][static_cast<size_t>(cell.mask)];
				CHECK(range.count > 0);
				const auto u = render::terrainMorph(cell.lod, true, lodSwitch, settings, camera.x, camera.z, camera.height);
				const auto previous = cell.lod > 0 ? render::terrainMorph(cell.lod - 1, true, lodSwitch, settings,
															camera.x, camera.z, camera.height)
												   : u;
				const int x0 = (cx % perPatch) * cellQuads, z0 = (cz % perPatch) * cellQuads;
				for (int i = range.start; i < range.start + range.count; ++i) {
					const int index = m.indices[static_cast<size_t>(i)];
					const int lx = index % side, lz = index / side;
					const auto& v = patch.vertices[static_cast<size_t>(index)];
					const float delta[3] = {v.morph[0] * yScale, v.morph[1] * yScale, v.morph[2] * yScale};
					const float x = origin + ((cx / perPatch) * quads + lx) * metres;
					const float z = origin + ((cz / perPatch) * quads + lz) * metres;
					const float h = render::terrainMorphedHeight(u, x, v.height * yScale, z, delta, v.edge);
					if (cell.lod > 0) {
						// Fully morphed under the finer LOD's limits: it looked
						// like this LOD's surface just before the switch.
						const float dx = camera.x - x, dz = camera.z - z;
						const float d2 = dx * dx + camera.height * camera.height + dz * dz;
						pops += d2 * previous.limits[0] - previous.limits[1] < 1.0f;
					}
					if (lx == x0 || lx == x0 + cellQuads || lz == z0 || lz == z0 + cellQuads) {
						edges[c][{(cx / perPatch) * quads + lx, (cz / perPatch) * quads + lz}] = h;
					}
				}
			}
		}
		// Where two cells meet, each one's edge vertex lies on the other's
		// edge: equal to the other's height there, or between its two
		// neighbours on that line.
		const auto heightOn = [&](const std::map<std::pair<int, int>, float>& e, int gx, int gz, bool alongX, float& h) {
			auto exact = e.find({gx, gz});
			if (exact != e.end()) {
				h = exact->second;
				return true;
			}
			int a = -1, b = -1;
			float ha = 0, hb = 0;
			for (const auto& [p, value] : e) {
				const int along = alongX ? p.first : p.second, across = alongX ? p.second : p.first;
				const int at = alongX ? gx : gz, line = alongX ? gz : gx;
				if (across != line) {
					continue;
				}
				if (along < at && along > a) {
					a = along;
					ha = value;
				}
				if (along > at && (b < 0 || along < b)) {
					b = along;
					hb = value;
				}
			}
			if (a < 0 || b < 0) {
				return false;
			}
			const int at = alongX ? gx : gz;
			h = ha + (hb - ha) * static_cast<float>(at - a) / static_cast<float>(b - a);
			return true;
		};
		for (int cz = 0; cz < perSide; ++cz) {
			for (int cx = 0; cx < perSide; ++cx) {
				const size_t c = static_cast<size_t>(cz * perSide + cx);
				for (const auto [nx, nz] : {std::pair {cx + 1, cz}, std::pair {cx, cz + 1}}) {
					if (nx >= perSide || nz >= perSide) {
						continue;
					}
					const size_t o = static_cast<size_t>(nz * perSide + nx);
					const bool alongX = nz != cz; // the shared line runs along x
					const int line = alongX ? (cz + 1) * cellQuads : (cx + 1) * cellQuads;
					for (const auto* pair : {&edges[c], &edges[o]}) {
						const auto& other = pair == &edges[c] ? edges[o] : edges[c];
						for (const auto& [p, h] : *pair) {
							if ((alongX ? p.second : p.first) != line) {
								continue;
							}
							float there = 0;
							if (heightOn(other, p.first, p.second, alongX, there)) {
								++shared;
								if (std::fabs(there - h) > 2e-3f) {
									++cracks;
									std::fprintf(stderr, "  crack at %d,%d: %.4f vs %.4f (cells %d,%d lod %d mask %d / %d,%d lod %d mask %d)\n",
										p.first, p.second, h, there, cx, cz, cells[c].lod, cells[c].mask, nx, nz,
										cells[o].lod, cells[o].mask);
								}
							}
						}
					}
				}
			}
		}
	}
	std::fprintf(stderr, "  %d shared edge vertices, %d cracks, %d pops\n", shared, cracks, pops);
	CHECK(shared > 1000 && cracks == 0 && pops == 0);
}
