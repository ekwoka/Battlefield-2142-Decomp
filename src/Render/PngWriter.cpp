#include "PngWriter.h"

#include <zlib.h>

#include <cstdio>

namespace dice::hfe::render {

namespace {

void putU32(std::vector<uint8_t>& out, uint32_t v)
{
	out.insert(out.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)});
}

void chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data)
{
	putU32(out, static_cast<uint32_t>(data.size()));
	const size_t start = out.size();
	out.insert(out.end(), type, type + 4);
	out.insert(out.end(), data.begin(), data.end());
	putU32(out, static_cast<uint32_t>(crc32(0, out.data() + start, static_cast<uInt>(out.size() - start))));
}

} // namespace

bool writePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba)
{
	if (rgba.size() < static_cast<size_t>(width) * height * 4) {
		return false;
	}
	// Each row is prefixed with filter type 0 (none).
	std::vector<uint8_t> raw;
	raw.reserve((static_cast<size_t>(width) * 4 + 1) * height);
	for (uint32_t y = 0; y < height; ++y) {
		raw.push_back(0);
		const auto row = rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * width * 4);
		raw.insert(raw.end(), row, row + static_cast<std::ptrdiff_t>(width) * 4);
	}
	uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
	std::vector<uint8_t> compressed(compressedSize);
	if (compress2(compressed.data(), &compressedSize, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
		return false;
	}
	compressed.resize(compressedSize);

	std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	std::vector<uint8_t> header;
	putU32(header, width);
	putU32(header, height);
	header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, deflate, no filter, no interlace
	chunk(png, "IHDR", header);
	chunk(png, "IDAT", compressed);
	chunk(png, "IEND", {});

	FILE* file = std::fopen(path.c_str(), "wb");
	if (!file) {
		return false;
	}
	const bool ok = std::fwrite(png.data(), 1, png.size(), file) == png.size();
	std::fclose(file);
	return ok;
}

} // namespace dice::hfe::render
