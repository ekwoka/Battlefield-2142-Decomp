// DefineFont2 (48): embedded outline fonts, or device fonts that only name
// a system font.
//
// Glyph outlines are shapes on the EM square: 1024 units, y down, the
// baseline at 0 (so ascenders are negative). Each glyph fills style 1; the
// Shape here carries one white solid fill so tessellateShape() works on it
// directly. The code table maps glyph index -> character code (UCS-2 when
// wideCodes, else ANSI or Shift-JIS bytes). The layout block (advances,
// bounds, kerning, ascent/descent/leading) is in EM units too, and is
// present only when hasLayout is set; static text needs it, edit fields
// can do without.
#pragma once

#include "UI/Characters/Shape.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dice::hfe::ui::swf {

struct FontGlyph {
	uint16_t code = 0;
	int16_t advance = 0; // hasLayout only
	Rect bounds; // hasLayout only
	Shape shape;
};

struct KerningPair {
	uint16_t left = 0, right = 0; // character codes
	int16_t adjustment = 0;
};

struct Font {
	uint16_t id = 0;
	std::string name; // trailing NULs removed
	uint8_t languageCode = 0;
	bool hasLayout = false;
	bool shiftJis = false;
	bool smallText = false; // aligned to pixels for small sizes
	bool ansi = false;
	bool wideOffsets = false;
	bool wideCodes = false;
	bool italic = false;
	bool bold = false;
	uint16_t ascent = 0, descent = 0;
	int16_t leading = 0;
	std::vector<FontGlyph> glyphs;
	std::vector<KerningPair> kerning;

	// No outlines: the host substitutes a system font called `name`.
	bool isDeviceFont() const { return glyphs.empty(); }
	// Index of the glyph for `code`, or -1.
	int glyphIndex(uint16_t code) const;
	int16_t kerningAdjustment(uint16_t left, uint16_t right) const;
};

bool parseDefineFont2(const uint8_t* body, size_t size, Font& out, std::string* error = nullptr);

} // namespace dice::hfe::ui::swf
