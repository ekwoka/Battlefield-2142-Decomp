// HUD bitmap fonts (.dif with a .dds of the same name): a text header
//   header / 2 / <name> / <texture width> / <texture height> / <line height>
// then "glyphs" and a count, one tab-separated line per glyph
//   <code> <a> <b> <c> <top> <x0> <y0> <x1> <y1>
// (a, b, c: the space before, the width of and the space after the glyph,
// in pixels; top: its offset below the top of the line; x0..y1: its
// rectangle in the texture), and "kerning" pairs <first> <second> <amount>.
#pragma once

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
};

std::optional<HudFont> parseHudFont(const std::string& text);

} // namespace dice::hfe::hud
