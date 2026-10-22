#include "Font.h"

namespace dice::hfe::ui::swf {

namespace {

void setError(std::string* error, const char* message)
{
	if (error) {
		*error = message;
	}
}

} // namespace

int Font::glyphIndex(uint16_t code) const
{
	for (size_t i = 0; i < glyphs.size(); ++i) {
		if (glyphs[i].code == code) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int16_t Font::kerningAdjustment(uint16_t left, uint16_t right) const
{
	for (const KerningPair& k : kerning) {
		if (k.left == left && k.right == right) {
			return k.adjustment;
		}
	}
	return 0;
}

bool parseDefineFont2(const uint8_t* body, size_t size, Font& out, std::string* error)
{
	BitReader in(body, size);
	out = Font {};
	out.id = in.u16();
	const uint8_t flags = in.u8();
	out.hasLayout = flags & 0x80;
	out.shiftJis = flags & 0x40;
	out.smallText = flags & 0x20;
	out.ansi = flags & 0x10;
	out.wideOffsets = flags & 0x08;
	out.wideCodes = flags & 0x04;
	out.italic = flags & 0x02;
	out.bold = flags & 0x01;
	out.languageCode = in.u8();
	out.name = in.bytes(in.u8());
	while (!out.name.empty() && out.name.back() == '\0') {
		out.name.pop_back();
	}
	const uint16_t count = in.u16();
	if (!in.ok()) {
		setError(error, "truncated font header");
		return false;
	}
	// Offsets count from the start of the offset table, which also holds
	// the code table's offset after the glyph entries.
	const size_t tableAt = in.position();
	std::vector<uint32_t> offsets(count);
	for (uint16_t i = 0; i < count; ++i) {
		offsets[i] = out.wideOffsets ? in.u32() : in.u16();
	}
	// Device fonts (no glyphs) may or may not carry the code table offset,
	// depending on the tool; skip it when the bytes left say it is there.
	uint32_t codeTableOffset = 0;
	const size_t offsetSize = out.wideOffsets ? 4 : 2;
	const size_t emptyLayoutSize = out.hasLayout ? 8 : 0; // ascent, descent, leading, no kerning
	if (count > 0) {
		codeTableOffset = out.wideOffsets ? in.u32() : in.u16();
	} else if (in.remaining() == offsetSize + emptyLayoutSize) {
		in.skip(offsetSize);
	}
	if (!in.ok()) {
		setError(error, "truncated font offset table");
		return false;
	}
	out.glyphs.resize(count);
	for (uint16_t i = 0; i < count; ++i) {
		const size_t at = tableAt + offsets[i];
		if (at >= size) {
			setError(error, "glyph offset past the tag");
			return false;
		}
		BitReader glyph(body, size);
		glyph.seek(at);
		Shape& shape = out.glyphs[i].shape;
		if (!parseGlyphShape(glyph, shape)) {
			setError(error, "bad glyph shape");
			return false;
		}
		shape.id = out.id;
		shape.fillStyles.resize(1); // white solid
	}
	if (count > 0) {
		in.seek(tableAt + codeTableOffset);
	}
	for (uint16_t i = 0; i < count; ++i) {
		out.glyphs[i].code = out.wideCodes ? in.u16() : in.u8();
	}
	if (out.hasLayout) {
		out.ascent = in.u16();
		out.descent = in.u16();
		out.leading = in.s16();
		for (uint16_t i = 0; i < count; ++i) {
			out.glyphs[i].advance = in.s16();
		}
		for (uint16_t i = 0; i < count; ++i) {
			out.glyphs[i].bounds = in.rect();
			out.glyphs[i].shape.bounds = out.glyphs[i].bounds;
		}
		const uint16_t kerningCount = in.u16();
		for (uint16_t i = 0; i < kerningCount && in.ok(); ++i) {
			KerningPair k;
			k.left = out.wideCodes ? in.u16() : in.u8();
			k.right = out.wideCodes ? in.u16() : in.u8();
			k.adjustment = in.s16();
			out.kerning.push_back(k);
		}
	}
	if (!in.ok()) {
		setError(error, "truncated font tables");
		return false;
	}
	return true;
}

} // namespace dice::hfe::ui::swf
