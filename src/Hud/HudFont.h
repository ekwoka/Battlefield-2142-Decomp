// HUD bitmap fonts (.dif with a .dds of the same name): a text header
//   header / 2 / <name> / <texture width> / <texture height> / <line height>
// then "glyphs" and a count, one tab-separated line per glyph
//   <code> <a> <b> <c> <top> <x0> <y0> <x1> <y1>
// (a, b, c: the space before, the width of and the space after the glyph,
// in pixels; top: its offset below the top of the line; x0..y1: its
// rectangle in the texture), and "kerning" pairs <first> <second> <amount>.
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace dice::hfe::hud {

struct Glyph {
	float before = 0, width = 0, after = 0;
	int top = 0;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	float advance() const { return before + width + after; }
};

struct HudFont {
	std::string name;
	int textureWidth = 0;
	int textureHeight = 0;
	float lineHeight = 0;
	std::map<uint32_t, Glyph> glyphs;
	std::map<std::pair<uint32_t, uint32_t>, float> kerning;

	const Glyph* glyph(uint32_t code) const;
	float kern(uint32_t first, uint32_t second) const;
	float width(const std::string& text) const;

	// Flat tables for codes under 256 (text is looked up a glyph at a
	// time, every frame): a copy of their glyphs, and which of them start
	// a kerning pair. parseHudFont makes them; call again after changing
	// `glyphs` or `kerning` (they are skipped while the counts differ).
	void buildIndex();

private:
	std::array<Glyph, 256> m_ascii {};
	std::bitset<256> m_asciiPresent;
	std::bitset<256> m_kernFirst;
	size_t m_indexedGlyphs = static_cast<size_t>(-1);
	size_t m_indexedKerning = static_cast<size_t>(-1);
};

std::optional<HudFont> parseHudFont(const std::string& text);

// The next character of HUD text from `at` (moved past it): a UTF-8
// sequence's code point (the strings come from the UTF-16 strings.csv,
// and the fonts' glyphs are by Unicode code point: the Chinese fonts'
// 1,447 run to 0x9xxx), or a lone byte as itself (Latin-1 names, the
// lists' font switch "\xa7").
uint32_t nextCode(const std::string& text, size_t& at);

// The game's language (BF2142.exe 0x00405360: the registry's Language,
// "English" by default, kept as the GSLanguage setting): the column of
// strings.csv and the fonts' folder (Fonts/<language>/, its 800 x 600
// fonts in Fonts/<language>/800/), English when that has none. One for
// the process, as the setting is.
const std::string& language();
void setLanguage(const std::string& name);

} // namespace dice::hfe::hud
