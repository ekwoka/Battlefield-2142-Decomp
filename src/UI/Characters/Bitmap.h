// Bitmap characters: DefineBits (6) with the movie's JPEGTables (8),
// DefineBitsJPEG2 (21), DefineBitsJPEG3 (35, JPEG plus a zlib alpha plane),
// DefineBitsLossless (20) and DefineBitsLossless2 (36).
//
// Every decoder produces RGBA8 rows top to bottom with PREMULTIPLIED alpha,
// which is how Flash keeps bitmaps: DefineBitsLossless2 already stores its
// colours premultiplied (passed through as stored), JPEG3's colour is
// multiplied by its alpha plane here, and opaque formats have alpha 255.
// Draw with (ONE, ONE_MINUS_SRC_ALPHA), or unpremultiply() first.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dice::hfe::ui::swf {

struct Bitmap {
	uint16_t id = 0;
	uint32_t width = 0, height = 0;
	std::vector<uint8_t> rgba; // width * height * 4, premultiplied
};

// DefineBitsLossless (`version` 1) or DefineBitsLossless2 (2) tag body.
bool decodeBitsLossless(const uint8_t* body, size_t size, int version, Bitmap& out, std::string* error = nullptr);

// DefineBits: `tables` is the JPEGTables tag body (may be empty).
bool decodeDefineBits(const uint8_t* body, size_t size, const std::vector<uint8_t>& tables, Bitmap& out,
	std::string* error = nullptr);

// DefineBitsJPEG2 (no alpha) and DefineBitsJPEG3.
bool decodeDefineBitsJpeg2(const uint8_t* body, size_t size, Bitmap& out, std::string* error = nullptr);
bool decodeDefineBitsJpeg3(const uint8_t* body, size_t size, Bitmap& out, std::string* error = nullptr);

// Decodes a JPEG stream as Flash accepts it: the bytes FF D9 FF D8 (an
// end-of-image right before a start-of-image, left by Flash's writers
// between the tables and the image) are skipped wherever they appear before
// the scan. RGB with alpha 255. PNG data is also accepted (JPEG2/3
// from SWF 8, not used by the menus).
bool decodeJpeg(const uint8_t* data, size_t size, Bitmap& out, std::string* error = nullptr);

// The JPEG with the erroneous header removed; exposed for tests.
std::vector<uint8_t> normalizeJpeg(const uint8_t* data, size_t size);

// Divides colour by alpha in place.
void unpremultiply(Bitmap& bitmap);

} // namespace dice::hfe::ui::swf
