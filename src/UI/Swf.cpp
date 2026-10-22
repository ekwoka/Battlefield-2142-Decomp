#include "Swf.h"

namespace dice::hfe::ui {

namespace {

void setError(std::string* error, const char* message)
{
	if (error) {
		*error = message;
	}
}

uint16_t get16le(const uint8_t* p)
{
	return static_cast<uint16_t>(p[0] | p[1] << 8);
}

uint32_t get32le(const uint8_t* p)
{
	return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
		static_cast<uint32_t>(p[3]) << 24;
}

// Most-significant-bit-first reader for the header's RECT.
class BitReader {
public:
	BitReader(const uint8_t* data, size_t size, size_t offset) : m_data(data), m_size(size), m_bit(offset * 8) {}

	bool readUnsigned(int bits, uint32_t& out)
	{
		out = 0;
		for (int i = 0; i < bits; ++i) {
			const size_t byte = m_bit >> 3;
			if (byte >= m_size) {
				return false;
			}
			out = out << 1 | ((m_data[byte] >> (7 - (m_bit & 7))) & 1);
			++m_bit;
		}
		return true;
	}

	bool readSigned(int bits, int32_t& out)
	{
		uint32_t raw = 0;
		if (!readUnsigned(bits, raw)) {
			return false;
		}
		if (bits > 0 && bits < 32 && (raw >> (bits - 1)) & 1) {
			raw |= ~0u << bits; // sign-extend
		}
		out = static_cast<int32_t>(raw);
		return true;
	}

	size_t alignedByte() const { return (m_bit + 7) >> 3; }

private:
	const uint8_t* m_data;
	size_t m_size;
	size_t m_bit;
};

} // namespace

bool readSwfTags(const uint8_t* data, size_t begin, size_t end, std::vector<SwfTag>& out)
{
	size_t at = begin;
	while (at + 2 <= end) {
		SwfTag tag;
		tag.headerOffset = at;
		const uint16_t word = get16le(data + at);
		at += 2;
		tag.code = static_cast<uint16_t>(word >> 6);
		tag.length = word & 0x3f;
		if (tag.length == 0x3f) {
			if (at + 4 > end) {
				return false;
			}
			tag.length = get32le(data + at);
			at += 4;
		}
		if (tag.length > end - at) {
			return false;
		}
		tag.offset = at;
		at += tag.length;
		out.push_back(tag);
		if (tag.code == kSwfEnd) {
			return true;
		}
	}
	// Some writers drop the final End tag; running out exactly is fine.
	return at == end;
}

bool readSpriteTags(const std::vector<uint8_t>& swf, const SwfTag& sprite, std::vector<SwfTag>& out)
{
	if (sprite.code != kSwfDefineSprite || sprite.length < 4 || sprite.offset + sprite.length > swf.size()) {
		return false;
	}
	return readSwfTags(swf.data(), sprite.offset + 4, sprite.offset + sprite.length, out);
}

std::optional<SwfMovie> parseSwf(const std::vector<uint8_t>& swf, std::string* error)
{
	if (swf.size() < 8 || swf[1] != 'W' || swf[2] != 'S') {
		setError(error, "not a SWF");
		return std::nullopt;
	}
	if (swf[0] != 'F') {
		setError(error, "compressed SWF; uncompress it first");
		return std::nullopt;
	}
	SwfMovie movie;
	SwfHeader& h = movie.header;
	h.version = swf[3];
	h.fileLength = get32le(swf.data() + 4);
	if (h.fileLength > swf.size()) {
		setError(error, "SWF shorter than its header says");
		return std::nullopt;
	}
	BitReader bits(swf.data(), h.fileLength, 8);
	uint32_t width = 0;
	if (!bits.readUnsigned(5, width) || !bits.readSigned(static_cast<int>(width), h.xMin) ||
		!bits.readSigned(static_cast<int>(width), h.xMax) || !bits.readSigned(static_cast<int>(width), h.yMin) ||
		!bits.readSigned(static_cast<int>(width), h.yMax)) {
		setError(error, "truncated stage rectangle");
		return std::nullopt;
	}
	size_t at = bits.alignedByte();
	if (at + 4 > h.fileLength) {
		setError(error, "truncated header");
		return std::nullopt;
	}
	h.frameRate = get16le(swf.data() + at);
	h.frameCount = get16le(swf.data() + at + 2);
	h.tagsOffset = at + 4;
	if (!readSwfTags(swf.data(), h.tagsOffset, h.fileLength, movie.tags)) {
		setError(error, "a tag runs past the end of the movie");
		return std::nullopt;
	}
	return movie;
}

const char* swfTagName(uint16_t code)
{
	switch (code) {
	case 0: return "End";
	case 1: return "ShowFrame";
	case 2: return "DefineShape";
	case 4: return "PlaceObject";
	case 5: return "RemoveObject";
	case 6: return "DefineBits";
	case 7: return "DefineButton";
	case 8: return "JPEGTables";
	case 9: return "SetBackgroundColor";
	case 10: return "DefineFont";
	case 11: return "DefineText";
	case 12: return "DoAction";
	case 13: return "DefineFontInfo";
	case 14: return "DefineSound";
	case 15: return "StartSound";
	case 17: return "DefineButtonSound";
	case 18: return "SoundStreamHead";
	case 19: return "SoundStreamBlock";
	case 20: return "DefineBitsLossless";
	case 21: return "DefineBitsJPEG2";
	case 22: return "DefineShape2";
	case 23: return "DefineButtonCxform";
	case 24: return "Protect";
	case 26: return "PlaceObject2";
	case 28: return "RemoveObject2";
	case 32: return "DefineShape3";
	case 33: return "DefineText2";
	case 34: return "DefineButton2";
	case 35: return "DefineBitsJPEG3";
	case 36: return "DefineBitsLossless2";
	case 37: return "DefineEditText";
	case 39: return "DefineSprite";
	case 41: return "ProductInfo";
	case 43: return "FrameLabel";
	case 45: return "SoundStreamHead2";
	case 46: return "DefineMorphShape";
	case 48: return "DefineFont2";
	case 56: return "ExportAssets";
	case 57: return "ImportAssets";
	case 58: return "EnableDebugger";
	case 59: return "DoInitAction";
	case 60: return "DefineVideoStream";
	case 61: return "VideoFrame";
	case 62: return "DefineFontInfo2";
	case 63: return "DebugID";
	case 64: return "EnableDebugger2";
	case 65: return "ScriptLimits";
	case 66: return "SetTabIndex";
	case 69: return "FileAttributes";
	case 70: return "PlaceObject3";
	case 71: return "ImportAssets2";
	case 72: return "DoABCDefine";
	case 73: return "DefineFontAlignZones";
	case 74: return "CSMTextSettings";
	case 75: return "DefineFont3";
	case 76: return "SymbolClass";
	case 77: return "Metadata";
	case 78: return "DefineScalingGrid";
	case 82: return "DoABC";
	case 83: return "DefineShape4";
	case 84: return "DefineMorphShape2";
	case 86: return "DefineSceneAndFrameLabelData";
	case 87: return "DefineBinaryData";
	case 88: return "DefineFontName";
	case 89: return "StartSound2";
	case 90: return "DefineBitsJPEG4";
	case 91: return "DefineFont4";
	default: return "Unknown";
	}
}

} // namespace dice::hfe::ui
