// DirectDraw Surface (.dds) textures, the format of all BF2142 textures.
//
// The game uses DXT1/3/5 (BC1/2/3), 16-bit RGB (565, 1555, 4444), 32-bit
// BGRA/BGRX, and DXT1 cube maps. Block-compressed data is kept as is (modern
// GPUs sample BC1-3 directly); every other format is expanded to RGBA8.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::texture {

enum class TextureFormat {
	BC1, // DXT1
	BC2, // DXT3
	BC3, // DXT5
	RGBA8,
};

struct TextureLevel {
	uint32_t width;
	uint32_t height;
	std::vector<uint8_t> data;
	uint32_t depth = 1; // slices of a volume texture's level
};

struct DdsTexture {
	TextureFormat format = TextureFormat::RGBA8;
	uint32_t width = 0;
	uint32_t height = 0;
	bool cube = false;
	uint32_t depth = 1; // above 1: a volume texture (one face, its levels halving the depth too)
	// Faces (6 for cube maps, in +X -X +Y -Y +Z -Z order), each with its mip levels.
	std::vector<std::vector<TextureLevel>> faces;

	uint32_t levelCount() const { return faces.empty() ? 0 : static_cast<uint32_t>(faces[0].size()); }
};

std::optional<DdsTexture> parseDds(const std::vector<uint8_t>& data, std::string* error = nullptr);

// Bytes of one mip level.
size_t levelSize(TextureFormat format, uint32_t width, uint32_t height);

// Drops up to `levels` top mip levels of a 2D texture as RendDX9's texture
// manager does for its texture classes' skips (0x10017710): one at a time
// while more than one level is left and the next is at least `minimum`
// (the manager's +0x1c, 4) texels on both sides. Cube and volume textures
// keep theirs. Returns the levels dropped.
int skipTopLevels(DdsTexture& texture, int levels, uint32_t minimum = 4);

} // namespace dice::hfe::texture
