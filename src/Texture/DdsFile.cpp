#include "DdsFile.h"

#include <algorithm>
#include <cstring>

namespace dice::hfe::texture {

namespace {

constexpr uint32_t kMagic = 0x20534444; // "DDS "
constexpr uint32_t kHeaderSize = 124;
constexpr uint32_t kMipMapCount = 0x20000;   // DDSD_MIPMAPCOUNT
constexpr uint32_t kFourCC = 0x4;            // DDPF_FOURCC
constexpr uint32_t kAlphaPixels = 0x1;       // DDPF_ALPHAPIXELS
constexpr uint32_t kCubeMap = 0x200;         // DDSCAPS2_CUBEMAP
constexpr uint32_t kVolume = 0x200000;       // DDSCAPS2_VOLUME

uint32_t u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

constexpr uint32_t fourCC(char a, char b, char c, char d)
{
	return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

// Scales a masked channel to 0..255 (1 if the mask is empty, for alpha).
struct Channel {
	uint32_t mask = 0;
	int shift = 0;
	uint32_t max = 0;

	explicit Channel(uint32_t m) : mask(m)
	{
		if (mask) {
			while (!((mask >> shift) & 1)) {
				++shift;
			}
			max = mask >> shift;
		}
	}
	uint8_t get(uint32_t pixel, uint8_t fallback) const
	{
		return mask ? static_cast<uint8_t>(((pixel & mask) >> shift) * 255 / max) : fallback;
	}
};

} // namespace

size_t levelSize(TextureFormat format, uint32_t width, uint32_t height)
{
	const size_t blocks = static_cast<size_t>(std::max(1u, (width + 3) / 4)) * std::max(1u, (height + 3) / 4);
	switch (format) {
	case TextureFormat::BC1:
		return blocks * 8;
	case TextureFormat::BC2:
	case TextureFormat::BC3:
		return blocks * 16;
	case TextureFormat::RGBA8:
		return static_cast<size_t>(width) * height * 4;
	}
	return 0;
}

std::optional<DdsTexture> parseDds(const std::vector<uint8_t>& data, std::string* error)
{
	const auto fail = [&](const char* why) -> std::optional<DdsTexture> {
		if (error) {
			*error = why;
		}
		return std::nullopt;
	};
	if (data.size() < 4 + kHeaderSize || u32(data.data()) != kMagic || u32(data.data() + 4) != kHeaderSize) {
		return fail("not a DDS file");
	}
	const uint8_t* h = data.data() + 4;
	const uint32_t flags = u32(h + 4);
	DdsTexture texture;
	texture.height = u32(h + 8);
	texture.width = u32(h + 12);
	const uint32_t mipCount = (flags & kMipMapCount) ? std::max(1u, u32(h + 24)) : 1;
	const uint8_t* pf = h + 72; // DDS_PIXELFORMAT
	const uint32_t pfFlags = u32(pf + 4);
	const uint32_t pfFourCC = u32(pf + 8);
	const uint32_t bitCount = u32(pf + 12);
	const Channel red(u32(pf + 16)), green(u32(pf + 20)), blue(u32(pf + 24));
	const Channel alpha((pfFlags & kAlphaPixels) ? u32(pf + 28) : 0);
	const uint32_t caps2 = u32(h + 108);
	texture.cube = (caps2 & kCubeMap) != 0;
	if (caps2 & kVolume) {
		texture.depth = std::max(1u, u32(h + 20)); // dwDepth
		if (texture.cube || texture.depth > 2048) {
			return fail("bad volume texture");
		}
	}

	uint32_t sourceBytesPerPixel = 0;
	if (pfFlags & kFourCC) {
		switch (pfFourCC) {
		case fourCC('D', 'X', 'T', '1'):
			texture.format = TextureFormat::BC1;
			break;
		case fourCC('D', 'X', 'T', '3'):
			texture.format = TextureFormat::BC2;
			break;
		case fourCC('D', 'X', 'T', '5'):
			texture.format = TextureFormat::BC3;
			break;
		default:
			return fail("unsupported compressed format");
		}
	} else if (bitCount == 16 || bitCount == 32) {
		texture.format = TextureFormat::RGBA8;
		sourceBytesPerPixel = bitCount / 8;
	} else {
		return fail("unsupported pixel format");
	}
	if (texture.width == 0 || texture.height == 0 || texture.width > 16384 || texture.height > 16384) {
		return fail("bad dimensions");
	}

	size_t offset = 4 + kHeaderSize;
	texture.faces.resize(texture.cube ? 6 : 1);
	for (auto& face : texture.faces) {
		uint32_t width = texture.width;
		uint32_t height = texture.height;
		uint32_t depth = texture.depth;
		for (uint32_t level = 0; level < mipCount; ++level) {
			TextureLevel out {width, height, {}, depth};
			if (sourceBytesPerPixel == 0) {
				const size_t size = levelSize(texture.format, width, height) * depth;
				if (offset + size > data.size()) {
					return fail("truncated");
				}
				out.data.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
					data.begin() + static_cast<std::ptrdiff_t>(offset + size));
				offset += size;
			} else {
				const size_t pixels = static_cast<size_t>(width) * height * depth;
				if (offset + pixels * sourceBytesPerPixel > data.size()) {
					return fail("truncated");
				}
				out.data.resize(pixels * 4);
				for (size_t i = 0; i < pixels; ++i) {
					const uint8_t* p = data.data() + offset + i * sourceBytesPerPixel;
					const uint32_t pixel = sourceBytesPerPixel == 2 ? uint32_t(p[0] | p[1] << 8) : u32(p);
					out.data[i * 4 + 0] = red.get(pixel, 0);
					out.data[i * 4 + 1] = green.get(pixel, 0);
					out.data[i * 4 + 2] = blue.get(pixel, 0);
					out.data[i * 4 + 3] = alpha.get(pixel, 255);
				}
				offset += pixels * sourceBytesPerPixel;
			}
			face.push_back(std::move(out));
			width = std::max(1u, width / 2);
			height = std::max(1u, height / 2);
			depth = std::max(1u, depth / 2);
		}
	}
	return texture;
}

} // namespace dice::hfe::texture
