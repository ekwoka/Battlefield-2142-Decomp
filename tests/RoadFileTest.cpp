// Parses every compiled road of the shipped levels and every road template's
// settings. Needs $BF2142_GAME_DIR.
#include "Geometry/RoadFile.h"
#include "IO/File/ZipArchive.h"
#include "TestMain.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
namespace geom = dice::hfe::geom;

namespace {

bool endsWith(const std::string& text, const char* suffix)
{
	const size_t n = std::char_traits<char>::length(suffix);
	return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

} // namespace

TEST(RoadFile_ParsesShippedRoads)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	int roads = 0, failed = 0, settings = 0;
	for (const auto& entry : fs::recursive_directory_iterator(fs::path(gameDir) / "mods/bf2142")) {
		if (entry.path().extension() != ".zip") {
			continue;
		}
		auto zip = dice::hfe::io::ZipArchive::open(entry.path().string());
		if (!zip) {
			continue;
		}
		for (const std::string& name : zip->listRecursive("")) {
			std::vector<uint8_t> data;
			std::string error;
			if (endsWith(name, "_compiled.mesh")) {
				zip->read(name, data);
				if (geom::parseCompiledRoad(data, &error)) {
					++roads;
				} else {
					++failed;
				}
			} else if (endsWith(name, "_compiled.dat")) {
				zip->read(name, data);
				const auto parsed = geom::parseRoadSettings(data, &error);
				CHECK(parsed.has_value());
				if (parsed) {
					// (A few unfinished templates have no textures.)
					CHECK(parsed->fadeStart < parsed->fadeEnd);
					++settings;
				}
			}
		}
	}
	std::printf("  %d roads, %d unsupported, %d road templates\n", roads, failed, settings);
	CHECK(roads > 4000);
	// Versions 0 and corrupt files in two BF2-era levels.
	CHECK(failed <= 9);
	CHECK(settings > 50);
}
