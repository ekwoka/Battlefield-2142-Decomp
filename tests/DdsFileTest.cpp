// Parses every texture in the game's archives. Needs $BF2142_GAME_DIR.
#include "IO/File/ZipArchive.h"
#include "Texture/DdsFile.h"
#include "TestMain.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

namespace fs = std::filesystem;
namespace tex = dice::hfe::texture;

TEST(DdsFile_ParsesGameTextures)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	int parsed = 0;
	std::map<std::string, int> failures;
	for (const auto& entry : fs::recursive_directory_iterator(fs::path(gameDir) / "mods/bf2142/Objects")) {
		if (entry.path().extension() != ".zip") {
			continue;
		}
		auto zip = dice::hfe::io::ZipArchive::open(entry.path().string());
		if (!zip) {
			continue;
		}
		for (const std::string& name : zip->listRecursive("")) {
			if (name.size() < 4 || name.compare(name.size() - 4, 4, ".dds") != 0) {
				continue;
			}
			std::vector<uint8_t> data;
			zip->read(name, data);
			std::string error;
			if (tex::parseDds(data, &error)) {
				++parsed;
			} else {
				++failures[error];
			}
		}
	}
	std::fprintf(stderr, "  %d textures parsed\n", parsed);
	for (const auto& [why, count] : failures) {
		std::fprintf(stderr, "  %d failed: %s\n", count, why.c_str());
	}
	CHECK(parsed > 1000);
	CHECK(failures.empty());
}

TEST(DdsFile_ExpandsRgb565)
{
	// A 1x1 R5G6B5 texture holding pure green.
	std::vector<uint8_t> dds(4 + 124 + 2, 0);
	const auto put = [&](size_t at, uint32_t v) {
		for (int i = 0; i < 4; ++i) {
			dds[at + i] = static_cast<uint8_t>(v >> (8 * i));
		}
	};
	put(0, 0x20534444);
	put(4, 124);
	put(4 + 8, 1);           // height
	put(4 + 12, 1);          // width
	put(4 + 72 + 4, 0x40);   // DDPF_RGB
	put(4 + 72 + 12, 16);    // bits
	put(4 + 72 + 16, 0xf800);
	put(4 + 72 + 20, 0x07e0);
	put(4 + 72 + 24, 0x001f);
	dds[128] = 0xe0;
	dds[129] = 0x07;
	const auto texture = tex::parseDds(dds);
	CHECK(texture && texture->format == tex::TextureFormat::RGBA8);
	const auto& rgba = texture->faces[0][0].data;
	CHECK(rgba[0] == 0 && rgba[1] == 255 && rgba[2] == 0 && rgba[3] == 255);
}

// The texture classes' mip skips (RendDX9 0x10017710): top levels dropped
// while the next stays at least 4 texels across, never the last one.
TEST(DdsFile_SkipsTopLevels)
{
	tex::DdsTexture t;
	t.format = tex::TextureFormat::RGBA8;
	t.width = 16;
	t.height = 8;
	t.faces.resize(1);
	for (uint32_t w = 16, h = 8; ; w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
		t.faces[0].push_back({w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, 0)});
		if (w == 1 && h == 1) {
			break;
		}
	}
	tex::DdsTexture once = t;
	CHECK(tex::skipTopLevels(once, 1) == 1 && once.width == 8 && once.height == 4 && once.levelCount() == 4);
	tex::DdsTexture twice = t;
	CHECK(tex::skipTopLevels(twice, 2) == 1); // 4 x 2 is below the minimum
	tex::DdsTexture cube = t;
	cube.cube = true;
	CHECK(tex::skipTopLevels(cube, 2) == 0 && cube.width == 16);
}
