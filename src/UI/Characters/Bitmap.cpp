#include "Bitmap.h"

#include "UI/Characters/BitReader.h"

#include <zlib.h>

// stb_image (public domain / MIT, vendored unchanged) does the JPEG and
// PNG decoding; it is kept private to this file.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wtype-limits"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "UI/Characters/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstring>

namespace dice::hfe::ui::swf {

namespace {

void setError(std::string* error, const char* message)
{
	if (error) {
		*error = message;
	}
}

// Inflates exactly `expected` bytes (trailing data is ignored).
bool inflateExactly(const uint8_t* data, size_t size, size_t expected, std::vector<uint8_t>& out)
{
	out.assign(expected, 0);
	z_stream z {};
	if (inflateInit(&z) != Z_OK) {
		return false;
	}
	z.next_in = const_cast<Bytef*>(data);
	z.avail_in = static_cast<uInt>(size);
	z.next_out = out.data();
	z.avail_out = static_cast<uInt>(expected);
	int result = Z_OK;
	while (z.avail_out > 0 && result == Z_OK) {
		result = inflate(&z, Z_NO_FLUSH);
	}
	inflateEnd(&z);
	return z.avail_out == 0;
}

uint8_t premultiply(uint8_t c, uint8_t a)
{
	return static_cast<uint8_t>((c * a + 127) / 255);
}

bool isPng(const uint8_t* data, size_t size)
{
	return size >= 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0;
}

} // namespace

bool decodeBitsLossless(const uint8_t* body, size_t size, int version, Bitmap& out, std::string* error)
{
	BitReader in(body, size);
	out = Bitmap {};
	out.id = in.u16();
	const uint8_t format = in.u8();
	out.width = in.u16();
	out.height = in.u16();
	const uint32_t colors = format == 3 ? in.u8() + 1u : 0;
	if (!in.ok()) {
		setError(error, "truncated lossless bitmap");
		return false;
	}
	const size_t w = out.width, h = out.height;
	size_t expected = 0, rowBytes = 0;
	const size_t entry = version >= 2 ? 4 : 3;
	switch (format) {
	case 3:
		rowBytes = (w + 3) & ~size_t(3);
		expected = colors * entry + rowBytes * h;
		break;
	case 4:
		if (version >= 2) {
			setError(error, "15-bit format in DefineBitsLossless2");
			return false;
		}
		rowBytes = (w * 2 + 3) & ~size_t(3);
		expected = rowBytes * h;
		break;
	case 5:
		rowBytes = w * 4;
		expected = rowBytes * h;
		break;
	default:
		setError(error, "unknown lossless bitmap format");
		return false;
	}
	std::vector<uint8_t> data;
	if (!inflateExactly(body + in.position(), in.remaining(), expected, data)) {
		setError(error, "lossless bitmap data does not inflate");
		return false;
	}
	out.rgba.resize(w * h * 4);
	uint8_t* dst = out.rgba.data();
	if (format == 3) {
		const uint8_t* table = data.data();
		const uint8_t* pixels = table + colors * entry;
		for (size_t y = 0; y < h; ++y) {
			for (size_t x = 0; x < w; ++x, dst += 4) {
				const uint32_t index = pixels[y * rowBytes + x];
				if (index >= colors) {
					std::memset(dst, 0, 4);
					continue;
				}
				const uint8_t* c = table + index * entry;
				dst[0] = c[0];
				dst[1] = c[1];
				dst[2] = c[2];
				dst[3] = entry == 4 ? c[3] : 255;
			}
		}
	} else if (format == 4) {
		// PIX15: big-endian, a pad bit then 5 bits each of red, green, blue.
		for (size_t y = 0; y < h; ++y) {
			for (size_t x = 0; x < w; ++x, dst += 4) {
				const uint8_t* p = data.data() + y * rowBytes + x * 2;
				const uint32_t v = static_cast<uint32_t>(p[0]) << 8 | p[1];
				const auto expand = [](uint32_t c) { return static_cast<uint8_t>(c << 3 | c >> 2); };
				dst[0] = expand((v >> 10) & 31);
				dst[1] = expand((v >> 5) & 31);
				dst[2] = expand(v & 31);
				dst[3] = 255;
			}
		}
	} else {
		// ARGB (Lossless2) or a pad byte then RGB (Lossless).
		const uint8_t* p = data.data();
		for (size_t i = 0; i < w * h; ++i, p += 4, dst += 4) {
			dst[0] = p[1];
			dst[1] = p[2];
			dst[2] = p[3];
			dst[3] = version >= 2 ? p[0] : 255;
		}
	}
	return true;
}

std::vector<uint8_t> normalizeJpeg(const uint8_t* data, size_t size)
{
	std::vector<uint8_t> out {0xff, 0xd8};
	size_t at = 0;
	while (at + 1 < size) {
		if (data[at] != 0xff) {
			break; // not a marker: hand the rest over untouched
		}
		const uint8_t marker = data[at + 1];
		if (marker == 0xff) {
			++at; // fill byte
			continue;
		}
		if (marker == 0xd8 || marker == 0xd9) {
			at += 2; // the one SOI is already out; EOIs before the scan go
			continue;
		}
		if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
			out.insert(out.end(), data + at, data + at + 2);
			at += 2;
			continue;
		}
		if (marker == 0xda) {
			break; // the scan and everything after it
		}
		if (at + 4 > size) {
			break;
		}
		const size_t length = 2 + (static_cast<size_t>(data[at + 2]) << 8 | data[at + 3]);
		if (at + length > size) {
			break;
		}
		out.insert(out.end(), data + at, data + at + length);
		at += length;
	}
	out.insert(out.end(), data + at, data + size);
	return out;
}

bool decodeJpeg(const uint8_t* data, size_t size, Bitmap& out, std::string* error)
{
	std::vector<uint8_t> stream;
	if (isPng(data, size)) {
		stream.assign(data, data + size);
	} else {
		stream = normalizeJpeg(data, size);
	}
	int w = 0, h = 0, channels = 0;
	stbi_uc* pixels = stbi_load_from_memory(stream.data(), static_cast<int>(stream.size()), &w, &h, &channels, 4);
	if (!pixels) {
		setError(error, stbi_failure_reason());
		return false;
	}
	out.width = static_cast<uint32_t>(w);
	out.height = static_cast<uint32_t>(h);
	out.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
	stbi_image_free(pixels);
	if (isPng(data, size)) {
		// PNG alpha is straight; keep the premultiplied promise.
		for (size_t i = 0; i < out.rgba.size(); i += 4) {
			for (int c = 0; c < 3; ++c) {
				out.rgba[i + c] = premultiply(out.rgba[i + c], out.rgba[i + 3]);
			}
		}
	}
	return true;
}

bool decodeDefineBits(const uint8_t* body, size_t size, const std::vector<uint8_t>& tables, Bitmap& out, std::string* error)
{
	if (size < 2) {
		setError(error, "truncated DefineBits");
		return false;
	}
	const uint16_t id = static_cast<uint16_t>(body[0] | body[1] << 8);
	std::vector<uint8_t> stream(tables);
	stream.insert(stream.end(), body + 2, body + size);
	out = Bitmap {};
	if (!decodeJpeg(stream.data(), stream.size(), out, error)) {
		return false;
	}
	out.id = id;
	return true;
}

bool decodeDefineBitsJpeg2(const uint8_t* body, size_t size, Bitmap& out, std::string* error)
{
	return decodeDefineBits(body, size, {}, out, error);
}

bool decodeDefineBitsJpeg3(const uint8_t* body, size_t size, Bitmap& out, std::string* error)
{
	if (size < 6) {
		setError(error, "truncated DefineBitsJPEG3");
		return false;
	}
	const uint16_t id = static_cast<uint16_t>(body[0] | body[1] << 8);
	const uint32_t alphaOffset = static_cast<uint32_t>(body[2]) | static_cast<uint32_t>(body[3]) << 8 |
		static_cast<uint32_t>(body[4]) << 16 | static_cast<uint32_t>(body[5]) << 24;
	if (alphaOffset > size - 6) {
		setError(error, "JPEG3 alpha offset past the tag");
		return false;
	}
	out = Bitmap {};
	if (!decodeJpeg(body + 6, alphaOffset, out, error)) {
		return false;
	}
	out.id = id;
	const uint8_t* alpha = body + 6 + alphaOffset;
	const size_t alphaSize = size - 6 - alphaOffset;
	if (alphaSize == 0 || isPng(body + 6, alphaOffset)) {
		return true; // opaque
	}
	std::vector<uint8_t> plane;
	if (!inflateExactly(alpha, alphaSize, static_cast<size_t>(out.width) * out.height, plane)) {
		setError(error, "JPEG3 alpha plane does not inflate");
		return false;
	}
	for (size_t i = 0; i < plane.size(); ++i) {
		uint8_t* p = out.rgba.data() + i * 4;
		const uint8_t a = plane[i];
		p[0] = premultiply(p[0], a);
		p[1] = premultiply(p[1], a);
		p[2] = premultiply(p[2], a);
		p[3] = a;
	}
	return true;
}

void unpremultiply(Bitmap& bitmap)
{
	for (size_t i = 0; i + 3 < bitmap.rgba.size(); i += 4) {
		uint8_t* p = bitmap.rgba.data() + i;
		const uint32_t a = p[3];
		if (a == 0 || a == 255) {
			continue;
		}
		for (int c = 0; c < 3; ++c) {
			p[c] = static_cast<uint8_t>(std::min<uint32_t>(255, (p[c] * 255u + a / 2) / a));
		}
	}
}

} // namespace dice::hfe::ui::swf
