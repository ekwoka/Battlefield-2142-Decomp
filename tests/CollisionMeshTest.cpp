// Parses every collision mesh of the game. Needs $BF2142_GAME_DIR.
#include "IO/File/ZipArchive.h"
#include "Physics/CollisionMeshFile.h"
#include "TestMain.h"

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
