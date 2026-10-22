// Parses every mesh in the game's object archives. Needs $BF2142_GAME_DIR.
#include "Geometry/MeshFile.h"
#include "IO/File/ZipArchive.h"
#include "TestMain.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
namespace geom = dice::hfe::geom;

TEST(MeshFile_ParsesAllGameMeshes)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	int parsed = 0;
	int failed = 0;
	int badIndices = 0;
	for (const auto& entry : fs::directory_iterator(fs::path(gameDir) / "mods/bf2142/Objects")) {
		if (entry.path().extension() != ".zip") {
			continue;
		}
		auto zip = dice::hfe::io::ZipArchive::open(entry.path().string());
		CHECK(zip != nullptr);
		if (!zip) {
			continue;
		}
		for (const std::string& name : zip->listRecursive("")) {
			const auto kind = geom::meshKindFromExtension(name);
			if (!kind) {
				continue;
			}
			std::vector<uint8_t> data;
			CHECK(zip->read(name, data));
			std::string error;
			const auto mesh = geom::parseMesh(data, *kind, &error);
			if (!mesh) {
				std::fprintf(stderr, "  %s: %s\n", name.c_str(), error.c_str());
				++failed;
				continue;
			}
			++parsed;
			// Indices are relative to the material's first vertex.
			for (const auto& geometry : mesh->geometries) {
				for (const auto& lod : geometry.lods) {
					for (const auto& material : lod.materials) {
						for (uint32_t i = 0; i < material.indexCount; ++i) {
							if (mesh->indices[material.indexStart + i] >= material.vertexCount) {
								++badIndices;
								break;
							}
						}
					}
				}
			}
		}
	}
	std::fprintf(stderr, "  %d meshes parsed\n", parsed);
	CHECK(parsed > 1600);
	CHECK(failed == 0);
	CHECK(badIndices == 0);
}
