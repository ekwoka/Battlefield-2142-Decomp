#include "TgaFile.h"

namespace dice::hfe::texture {

std::optional<DdsTexture> parseTga(const std::vector<uint8_t>& data, std::string* error)
{
	const auto fail = [error](const char* why) -> std::optional<DdsTexture> {
		if (error) {
			*error = why;
		}
		return std::nullopt;
	};
	if (data.size() < 18) {
		return fail("truncated header");
	}
	const uint8_t idLength = data[0];
	const uint8_t colourMapType = data[1];
	const uint8_t type = data[2];
	const uint32_t width = data[12] | (data[13] << 8);
	const uint32_t height = data[14] | (data[15] << 8);
	const uint8_t bits = data[16];
	const uint8_t descriptor = data[17];
	const bool rle = type == 10 || type == 11;
	const bool grey = type == 3 || type == 11;
	if (colourMapType != 0 || !(type == 2 || type == 3 || type == 10 || type == 11)) {
		return fail("unsupported image type");
	}
	if ((grey && bits != 8) || (!grey && bits != 24 && bits != 32) || width == 0 || height == 0) {
		return fail("unsupported pixel format");
	}
	const size_t bytesPerPixel = bits / 8;
	const size_t pixels = static_cast<size_t>(width) * height;
	std::vector<uint8_t> rgba(pixels * 4);
	size_t at = 18 + idLength;
	const auto readPixel = [&](uint8_t* out) {
		if (at + bytesPerPixel > data.size()) {
			return false;
		}
		if (grey) {
			out[0] = out[1] = out[2] = data[at];
			out[3] = 255;
		} else {
			out[0] = data[at + 2]; // stored as BGR(A)
			out[1] = data[at + 1];
			out[2] = data[at];
			out[3] = bytesPerPixel == 4 ? data[at + 3] : 255;
		}
		at += bytesPerPixel;
		return true;
	};
	size_t i = 0;
	while (i < pixels) {
		if (!rle) {
			if (!readPixel(&rgba[i * 4])) {
				return fail("truncated pixels");
			}
			++i;
			continue;
		}
		if (at >= data.size()) {
			return fail("truncated packets");
		}
		const uint8_t packet = data[at++];
		const size_t count = (packet & 0x7f) + 1u;
		if (packet & 0x80) {
			uint8_t pixel[4];
			if (!readPixel(pixel)) {
				return fail("truncated packets");
			}
			for (size_t k = 0; k < count && i < pixels; ++k, ++i) {
				std::copy(pixel, pixel + 4, &rgba[i * 4]);
			}
		} else {
			for (size_t k = 0; k < count && i < pixels; ++k, ++i) {
				if (!readPixel(&rgba[i * 4])) {
					return fail("truncated packets");
				}
			}
		}
	}
	// Rows are stored bottom-up unless the descriptor says top-down.
	if (!(descriptor & 0x20)) {
		const size_t row = static_cast<size_t>(width) * 4;
		for (uint32_t y = 0; y < height / 2; ++y) {
			std::swap_ranges(rgba.begin() + static_cast<std::ptrdiff_t>(y * row),
				rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
				rgba.begin() + static_cast<std::ptrdiff_t>((height - 1 - y) * row));
		}
	}
	DdsTexture texture;
	texture.format = TextureFormat::RGBA8;
	texture.width = width;
	texture.height = height;
	texture.faces.resize(1);
	texture.faces[0].push_back({width, height, std::move(rgba)});
	return texture;
}

} // namespace dice::hfe::texture
