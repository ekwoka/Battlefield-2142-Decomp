// Parses every collision mesh of the game. Needs $BF2142_GAME_DIR.
#include "IO/File/ZipArchive.h"
#include "Physics/CollisionMeshFile.h"
#include "TestMain.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

TEST(CollisionMesh_ParsesGameMeshes)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	int parsed = 0, failed = 0;
	for (const auto& entry : fs::recursive_directory_iterator(fs::path(gameDir) / "mods/bf2142")) {
		if (entry.path().extension() != ".zip") {
			continue;
		}
		auto zip = dice::hfe::io::ZipArchive::open(entry.path().string());
		if (!zip) {
			continue;
		}
		for (const std::string& name : zip->listRecursive("")) {
			if (name.size() < 14 || name.compare(name.size() - 14, 14, ".collisionmesh") != 0) {
				continue;
			}
			std::vector<uint8_t> data;
			zip->read(name, data);
			std::string error;
			if (dice::hfe::physics::parseCollisionMesh(data, &error)) {
				++parsed;
			} else {
				std::fprintf(stderr, "  rejected %s: %s\n", name.c_str(), error.c_str());
				++failed;
			}
		}
	}
	std::fprintf(stderr, "  %d collision meshes parsed, %d rejected\n", parsed, failed);
	CHECK(parsed >= 3762);
	CHECK(failed <= 3); // the shipped data has three malformed files
}

// The FAV's mesh: a material index per vertex (the LOD's +0xd8, the
// mesh's slot 0x44) and the vehicle LOD's radius (slot 0x50: its box's
// farthest corner from the origin, 0x006aeda0).
TEST(CollisionMesh_VertexMaterialsAndRadius)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	auto zip = dice::hfe::io::ZipArchive::open((fs::path(gameDir) / "mods/bf2142/Objects/Vehicles_server.zip").string());
	CHECK(zip != nullptr);
	if (!zip) {
		return;
	}
	std::vector<uint8_t> data;
	zip->read("as_fav/meshes/as_fav.collisionmesh", data);
	const auto mesh = dice::hfe::physics::parseCollisionMesh(data);
	CHECK(mesh.has_value());
	if (!mesh) {
		return;
	}
	const dice::hfe::physics::CollisionLod* lod = nullptr;
	for (const auto& part : mesh->geometries[0].subGeometries) {
		if (!lod) {
			lod = dice::hfe::physics::collisionLodFor(*mesh, part, 1);
		}
	}
	CHECK(lod != nullptr);
	if (!lod) {
		return;
	}
	CHECK(lod->vertexMaterials.size() == lod->vertices.size());
	const float radius = dice::hfe::physics::boxRadius(lod->boundsMin, lod->boundsMax);
	std::fprintf(stderr, "  as_fav: %zu vehicle-LOD vertices, radius %.3f\n", lod->vertices.size(), radius);
	CHECK(radius > 2.3f && radius < 2.7f);
	// A box's farthest corner, not its half diagonal.
	CHECK(std::abs(dice::hfe::physics::boxRadius({0, 0, 0}, {3, 4, 0}) - 5) < 1e-5f);
}

