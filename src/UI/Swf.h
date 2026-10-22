// A minimal reader for uncompressed ("FWS") SWF movies: the header and the
// tag list, enough to inspect the menus decoded from .dfm files (see Dfm.h).
// The player itself was gameswf inside SwiffPlayer.dll; this only walks the
// container.
//
// Layout (all little-endian): "FWS", version byte, u32 file length, the
// stage RECT (5-bit field width then xMin, xMax, yMin, yMax in twips,
// 1/20 pixel, bit-packed and byte-aligned at the end), u16 frame rate in
// 8.8 fixed point, u16 frame count, then tags. Each tag starts with a u16
// "code << 6 | length"; a length of 0x3f means a u32 length follows. The
// list ends with tag 0 (End). DefineSprite (39) holds its own tag list
// after a u16 sprite id and u16 frame count.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::ui {

struct SwfTag {
	uint16_t code = 0;
	uint32_t length = 0;    // body bytes
	size_t offset = 0;      // body offset in the movie
	size_t headerOffset = 0; // where the tag's code/length word starts
};

struct SwfHeader {
	uint8_t version = 0;
	uint32_t fileLength = 0;
	int32_t xMin = 0, xMax = 0, yMin = 0, yMax = 0; // twips
	uint16_t frameRate = 0; // 8.8 fixed point
	uint16_t frameCount = 0;
	size_t tagsOffset = 0;

	int32_t widthTwips() const { return xMax - xMin; }
	int32_t heightTwips() const { return yMax - yMin; }
	double framesPerSecond() const { return frameRate / 256.0; }
};

struct SwfMovie {
	SwfHeader header;
	std::vector<SwfTag> tags; // top level, End tag included
};

// Parses an "FWS" movie (use uncompressSwf from Dfm.h for "CWS").
std::optional<SwfMovie> parseSwf(const std::vector<uint8_t>& swf, std::string* error = nullptr);

// Reads tags from [begin, end) of `data`, stopping after an End tag or at
// `end`. Returns false if a tag runs past `end`.
bool readSwfTags(const uint8_t* data, size_t begin, size_t end, std::vector<SwfTag>& out);

// The tags inside a DefineSprite tag (its body's list after id and frame
// count).
bool readSpriteTags(const std::vector<uint8_t>& swf, const SwfTag& sprite, std::vector<SwfTag>& out);

// The tag's name from the SWF specification ("DefineShape2"), or
// "Unknown" for codes it does not define.
const char* swfTagName(uint16_t code);

// Well-known tag codes.
enum SwfTagCode : uint16_t {
	kSwfEnd = 0,
	kSwfShowFrame = 1,
	kSwfDefineShape = 2,
	kSwfPlaceObject = 4,
	kSwfDoAction = 12,
	kSwfDefineSprite = 39,
	kSwfFileAttributes = 69,
	kSwfDoInitAction = 59,
	kSwfDoAbc = 82,
	kSwfDoAbcDefine = 72,
};

} // namespace dice::hfe::ui
