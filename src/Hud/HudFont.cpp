#include "HudFont.h"

#include <cstdlib>
#include <sstream>
#include <vector>

namespace dice::hfe::hud {

namespace {

std::vector<std::string> fields(const std::string& line)
{
	std::vector<std::string> out;
	std::string field;
	std::istringstream in(line);
	while (std::getline(in, field, '\t')) {
		if (!field.empty() && field.back() == '\r') {
			field.pop_back();
		}
		if (!field.empty()) {
			out.push_back(field);
		}
	}
	return out;
}

} // namespace

const Glyph* HudFont::glyph(uint32_t code) const
{
	const auto it = glyphs.find(code);
	return it == glyphs.end() ? nullptr : &it->second;
}

float HudFont::kern(uint32_t first, uint32_t second) const
{
	const auto it = kerning.find({first, second});
	return it == kerning.end() ? 0.0f : it->second;
}

float HudFont::width(const std::string& text) const
{
	float x = 0;
	uint32_t previous = 0;
	for (unsigned char c : text) {
		if (const Glyph* g = glyph(c)) {
			x += kern(previous, c) + g->advance();
		}
		previous = c;
	}
	return x;
}

std::optional<HudFont> parseHudFont(const std::string& text)
{
	std::vector<std::vector<std::string>> lines;
	std::istringstream in(text);
	for (std::string line; std::getline(in, line);) {
		lines.push_back(fields(line));
	}
	if (lines.size() < 8 || lines[0].empty() || lines[0][0] != "header") {
		return std::nullopt;
	}
	HudFont font;
	const auto first = [&](size_t i) { return lines[i].empty() ? std::string() : lines[i][0]; };
	font.name = first(2);
	font.textureWidth = std::atoi(first(3).c_str());
	font.textureHeight = std::atoi(first(4).c_str());
	font.lineHeight = static_cast<float>(std::atof(first(5).c_str()));
	bool kerning = false;
	for (size_t i = 8; i < lines.size(); ++i) {
		const auto& f = lines[i];
		if (f.size() == 1 && f[0] == "kerning") {
			kerning = true;
			continue;
		}
		if (!kerning && f.size() >= 9) {
			Glyph g;
			g.before = static_cast<float>(std::atof(f[1].c_str()));
			g.width = static_cast<float>(std::atof(f[2].c_str()));
			g.after = static_cast<float>(std::atof(f[3].c_str()));
			g.top = std::atoi(f[4].c_str());
			g.x0 = std::atoi(f[5].c_str());
			g.y0 = std::atoi(f[6].c_str());
			g.x1 = std::atoi(f[7].c_str());
			g.y1 = std::atoi(f[8].c_str());
			font.glyphs[static_cast<uint32_t>(std::atoi(f[0].c_str()))] = g;
		} else if (f.size() == 3) {
			font.kerning[{static_cast<uint32_t>(std::atoi(f[0].c_str())), static_cast<uint32_t>(std::atoi(f[1].c_str()))}] =
				static_cast<float>(std::atof(f[2].c_str()));
		}
	}
	return font;
}

} // namespace dice::hfe::hud
