#include "Draw.h"

#include "Hud/HudFont.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dice::hfe::ui {

namespace {

constexpr float kTwips = 20.0f;
constexpr float kGradientHalf = 16384.0f; // gradients span -16384..16384 twips before their matrix

// The colour of a gradient at a ratio (0..255) from its stops.
swf::Rgba gradientAt(const std::vector<swf::GradientStop>& stops, float ratio)
{
	if (stops.empty()) {
		return {};
	}
	if (ratio <= stops.front().ratio) {
		return stops.front().color;
	}
	for (size_t i = 1; i < stops.size(); ++i) {
		if (ratio <= stops[i].ratio) {
			const swf::GradientStop& a = stops[i - 1];
			const swf::GradientStop& b = stops[i];
			const float t = b.ratio > a.ratio ? (ratio - a.ratio) / static_cast<float>(b.ratio - a.ratio) : 0.0f;
			const auto mix = [t](uint8_t x, uint8_t y) {
				return static_cast<uint8_t>(std::lround(x + (static_cast<float>(y) - x) * t));
			};
			return {mix(a.color.r, b.color.r), mix(a.color.g, b.color.g), mix(a.color.b, b.color.b), mix(a.color.a, b.color.a)};
		}
	}
	return stops.back().color;
}

// UTF-8 to code points; HTML markup dropped and entities decoded when asked.
std::vector<uint32_t> codePoints(const std::string& text, bool html)
{
	std::string plain;
	if (html) {
		for (size_t i = 0; i < text.size(); ++i) {
			if (text[i] == '<') {
				const size_t close = text.find('>', i);
				const std::string tag = text.substr(i, close == std::string::npos ? std::string::npos : close - i + 1);
				if (tag.rfind("<br", 0) == 0 || tag.rfind("</p", 0) == 0 || tag.rfind("<BR", 0) == 0 || tag.rfind("</P", 0) == 0) {
					plain += '\n';
				}
				i = close == std::string::npos ? text.size() : close;
			} else if (text[i] == '&') {
				static const std::pair<const char*, char> entities[] = {
					{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&nbsp;", ' '}};
				bool matched = false;
				for (const auto& [name, c] : entities) {
					if (text.compare(i, std::strlen(name), name) == 0) {
						plain += c;
						i += std::strlen(name) - 1;
						matched = true;
						break;
					}
				}
				if (!matched) {
					plain += '&';
				}
			} else {
				plain += text[i];
			}
		}
	} else {
		plain = text;
	}
	std::vector<uint32_t> codes;
	for (size_t i = 0; i < plain.size();) {
		const auto c = static_cast<unsigned char>(plain[i]);
		uint32_t code = c;
		size_t length = 1;
		if (c >= 0xf0 && i + 3 < plain.size()) {
			code = (c & 0x07u) << 18 | (plain[i + 1] & 0x3fu) << 12 | (plain[i + 2] & 0x3fu) << 6 | (plain[i + 3] & 0x3fu);
			length = 4;
		} else if (c >= 0xe0 && i + 2 < plain.size()) {
			code = (c & 0x0fu) << 12 | (plain[i + 1] & 0x3fu) << 6 | (plain[i + 2] & 0x3fu);
			length = 3;
		} else if (c >= 0xc0 && i + 1 < plain.size()) {
			code = (c & 0x1fu) << 6 | (plain[i + 1] & 0x3fu);
			length = 2;
		}
		codes.push_back(code);
		i += length;
	}
	return codes;
}

void pushVertex(std::vector<MenuVertex>& out, const swf::Matrix& world, swf::Point p, float u, float v, const float mult[4],
	const float add[4])
{
	const swf::Point at = world.apply(p);
	MenuVertex vertex {at.x / kTwips, at.y / kTwips, u, v, {mult[0], mult[1], mult[2], mult[3]}, {add[0], add[1], add[2], add[3]}};
	out.push_back(vertex);
}

} // namespace

swf::Matrix multiply(const swf::Matrix& outer, const swf::Matrix& inner)
{
	swf::Matrix m;
	m.a = outer.a * inner.a + outer.c * inner.b;
	m.b = outer.b * inner.a + outer.d * inner.b;
	m.c = outer.a * inner.c + outer.c * inner.d;
	m.d = outer.b * inner.c + outer.d * inner.d;
	m.tx = outer.a * inner.tx + outer.c * inner.ty + outer.tx;
	m.ty = outer.b * inner.tx + outer.d * inner.ty + outer.ty;
	return m;
}

swf::Matrix inverse(const swf::Matrix& m)
{
	const float det = m.a * m.d - m.b * m.c;
	if (std::fabs(det) < 1e-12f) {
		return {0, 0, 0, 0, 0, 0};
	}
	swf::Matrix r;
	r.a = m.d / det;
	r.b = -m.b / det;
	r.c = -m.c / det;
	r.d = m.a / det;
	r.tx = -(r.a * m.tx + r.c * m.ty);
	r.ty = -(r.b * m.tx + r.d * m.ty);
	return r;
}

swf::ColorTransform combine(const swf::ColorTransform& outer, const swf::ColorTransform& inner)
{
	// outer(inner(c)) = (c * mi + ai) * mo + ao
	swf::ColorTransform c;
	for (int i = 0; i < 4; ++i) {
		c.mult[i] = inner.mult[i] * outer.mult[i];
		c.add[i] = static_cast<int16_t>(std::clamp(inner.add[i] * outer.mult[i] + outer.add[i], -32768.0f, 32767.0f));
	}
	return c;
}

Drawer::Drawer(const swf::CharacterDictionary& dictionary) : m_dictionary(dictionary), m_white(std::make_shared<MenuImage>())
{
	m_white->width = m_white->height = 1;
	m_white->rgba = {255, 255, 255, 255};
}

std::shared_ptr<const MenuImage> Drawer::bitmapImage(uint16_t id)
{
	auto& slot = m_bitmaps[id];
	if (!slot) {
		const swf::Bitmap* bitmap = m_dictionary.bitmap(id);
		if (!bitmap) {
			return nullptr;
		}
		swf::Bitmap straight = *bitmap;
		swf::unpremultiply(straight);
		auto image = std::make_shared<MenuImage>();
		image->width = straight.width;
		image->height = straight.height;
		image->rgba = std::move(straight.rgba);
		slot = image;
	}
	return slot;
}

std::shared_ptr<const MenuImage> Drawer::gradientImage(uint64_t key, const swf::FillStyle& style)
{
	auto& slot = m_gradients[key];
	if (slot) {
		return slot;
	}
	auto image = std::make_shared<MenuImage>();
	const bool linear = style.type == swf::FillStyle::Type::LinearGradient;
	image->width = linear ? 256 : 128;
	image->height = linear ? 1 : 128;
	image->rgba.resize(static_cast<size_t>(image->width) * image->height * 4);
	for (uint32_t y = 0; y < image->height; ++y) {
		for (uint32_t x = 0; x < image->width; ++x) {
			float ratio;
			if (linear) {
				ratio = static_cast<float>(x); // 256 texels: one per ratio step
			} else {
				const float dx = (x + 0.5f) / image->width * 2 - 1, dy = (y + 0.5f) / image->height * 2 - 1;
				ratio = std::min(1.0f, std::sqrt(dx * dx + dy * dy)) * 255.0f;
			}
			const swf::Rgba c = gradientAt(style.stops, ratio);
			uint8_t* p = &image->rgba[(static_cast<size_t>(y) * image->width + x) * 4];
			p[0] = c.r;
			p[1] = c.g;
			p[2] = c.b;
			p[3] = c.a;
		}
	}
	slot = image;
	return slot;
}

void Drawer::drawMesh(const swf::Shape& shape, const swf::ShapeMesh& mesh, uint64_t cacheKey, const swf::Matrix& world,
	const swf::ColorTransform& colour, std::vector<MenuBatch>& out)
{
	const float add[4] = {colour.add[0] / 255.0f, colour.add[1] / 255.0f, colour.add[2] / 255.0f, colour.add[3] / 255.0f};
	for (const swf::MeshBatch& part : mesh.batches) {
		MenuBatch batch;
		float mult[4] = {colour.mult[0], colour.mult[1], colour.mult[2], colour.mult[3]};
		swf::Matrix toTexture {0, 0, 0, 0, 0.5f, 0.5f}; // shape twips to texture coordinates (solid: the middle)
		if (part.kind == swf::MeshBatch::Kind::Stroke) {
			if (part.style >= shape.lineStyles.size()) {
				continue;
			}
			const swf::Rgba c = shape.lineStyles[part.style].color;
			mult[0] *= c.r / 255.0f;
			mult[1] *= c.g / 255.0f;
			mult[2] *= c.b / 255.0f;
			mult[3] *= c.a / 255.0f;
			batch.image = m_white;
		} else {
			if (part.style >= shape.fillStyles.size()) {
				continue;
			}
			const swf::FillStyle& fill = shape.fillStyles[part.style];
			switch (fill.type) {
			case swf::FillStyle::Type::Solid:
				mult[0] *= fill.color.r / 255.0f;
				mult[1] *= fill.color.g / 255.0f;
				mult[2] *= fill.color.b / 255.0f;
				mult[3] *= fill.color.a / 255.0f;
				batch.image = m_white;
				break;
			case swf::FillStyle::Type::LinearGradient:
			case swf::FillStyle::Type::RadialGradient:
			case swf::FillStyle::Type::FocalGradient: {
				batch.image = gradientImage(cacheKey << 16 | part.style, fill);
				// Gradient space (-16384..16384) to 0..1.
				const swf::Matrix toGradient = inverse(fill.matrix);
				const float s = 1.0f / (2 * kGradientHalf);
				toTexture = multiply({s, 0, 0, s, 0.5f, 0.5f}, toGradient);
				break;
			}
			case swf::FillStyle::Type::Bitmap: {
				batch.image = bitmapImage(fill.bitmapId);
				if (!batch.image) {
					continue; // a stale id: nothing (as Flash)
				}
				// The fill matrix takes bitmap pixels to shape twips.
				const swf::Matrix toPixels = inverse(fill.matrix);
				toTexture = multiply({1.0f / batch.image->width, 0, 0, 1.0f / batch.image->height, 0, 0}, toPixels);
				batch.repeat = fill.repeat;
				break;
			}
			}
		}
		batch.vertices.reserve(part.triangles.size());
		for (const swf::Point& p : part.triangles) {
			const swf::Point uv = toTexture.apply(p);
			pushVertex(batch.vertices, world, p, uv.x, uv.y, mult, add);
		}
		out.push_back(std::move(batch));
	}
}

void Drawer::shape(uint16_t id, const swf::Matrix& world, const swf::ColorTransform& colour, std::vector<MenuBatch>& out)
{
	const swf::Shape* shape = m_dictionary.shape(id);
	const swf::ShapeMesh* mesh = shapeMesh(id);
	if (shape && mesh) {
		drawMesh(*shape, *mesh, id, world, colour, out);
	}
}

void Drawer::morphShape(uint16_t id, float ratio, const swf::Matrix& world, const swf::ColorTransform& colour,
	std::vector<MenuBatch>& out)
{
	const swf::MorphShape* morph = m_dictionary.morphShape(id);
	if (!morph) {
		return;
	}
	// Tessellated anew each time (morphs are few and small); gradients keyed by the ratio step.
	const swf::Shape shape = swf::interpolateMorphShape(*morph, std::clamp(ratio, 0.0f, 1.0f));
	const uint64_t step = static_cast<uint64_t>(std::lround(std::clamp(ratio, 0.0f, 1.0f) * 64));
	drawMesh(shape, swf::tessellateShape(shape), (uint64_t {1} << 40) | (uint64_t {id} << 8) | step, world, colour, out);
}

Drawer::~Drawer() = default;

// SwiffPlayer.dll names fonts once the movie is read (0x100b9000): each
// exported font's name maps to its export name, and every font that is not
// exported takes the export name of the exported font with its name (the
// menus' device fonts "Helvetica" and "Helvetica Condensed" become
// DynamicText and StandardText). An edit_text_character asks for its font
// id's name when made (0x100c1700, the definition's slot 0x3c, 0x100b67a0).
std::string Drawer::engineFontName(uint16_t fontId) const
{
	if (!m_fontNamesBuilt) {
		m_fontNamesBuilt = true;
		std::map<std::string, std::string> byFontName;
		for (const auto& [name, id] : m_dictionary.exports()) {
			if (const swf::Font* font = m_dictionary.font(id)) {
				m_fontNames[id] = name;
				byFontName[font->name] = name;
			}
		}
		for (const auto& [id, character] : m_dictionary.characters()) {
			const auto* font = std::get_if<swf::Font>(&character);
			if (font && !m_fontNames.count(id)) {
				const auto it = byFontName.find(font->name);
				if (it != byFontName.end()) {
					m_fontNames[id] = it->second;
				}
			}
		}
	}
	const auto it = m_fontNames.find(fontId);
	return it == m_fontNames.end() ? std::string() : it->second;
}

// The text object's font (RendDX9.dll 0x101172f0: FontSystem's
// "<name>_<size>"; 0x10185e90 finds its .dif in "Fonts/<language>/<res>/"
// then "Fonts/<res>/"). <res> is "800" at an 800-pixel-wide window and
// empty from 1024 on, where the font is scaled by width / 1024; the menu
// stage is 1024 wide, so the fonts here are the 1024 ones at their own
// size, one font pixel a stage pixel. The language is the menu's (English).
const hud::HudFont* Drawer::engineFont(const std::string& name, int pixels) const
{
	if (name.empty()) {
		return nullptr;
	}
	const std::string key = name + "_" + std::to_string(pixels);
	auto [it, added] = m_engineFonts.try_emplace(key);
	if (added && m_fontReader) {
		for (const std::string& path : {"Fonts/English/" + key + ".dif", "Fonts/" + key + ".dif"}) {
			std::string text;
			if (m_fontReader(path, text)) {
				if (auto font = hud::parseHudFont(text)) {
					it->second = std::make_unique<hud::HudFont>(std::move(*font));
					it->second->name = path.substr(0, path.size() - 4); // the .dds beside it
				}
				break;
			}
		}
	}
	return it->second && !it->second->glyphs.empty() ? it->second.get() : nullptr;
}

namespace {

// RendDX9.dll 0x10183240: the boxes of a line's characters. A character
// the font lacks repeats the previous box.
Drawer::TextLine layoutLine(const hud::HudFont& font, std::vector<uint32_t> codes)
{
	Drawer::TextLine line;
	Drawer::GlyphBox box;
	float pen = 0;
	for (size_t i = 0; i < codes.size(); ++i) {
		const uint32_t code = codes[i] == 0xa0 ? ' ' : codes[i];
		if (const hud::Glyph* g = font.glyph(code)) {
			const float start = pen;
			if (i > 0) {
				pen += font.kern(codes[i - 1], codes[i]);
			}
			const float x = std::floor(g->before + pen + 0.5f);
			box.x = x;
			box.left = std::min(std::floor(start), x);
			pen = g->width + g->after + x;
			box.right = std::ceil(pen);
			box.top = std::floor(static_cast<float>(g->top));
		}
		line.boxes.push_back(box);
	}
	line.codes = std::move(codes);
	// The position after the last character (0x10182e60 at the length).
	line.end = line.boxes.empty() ? 0.0f : line.boxes.back().right;
	return line;
}

} // namespace

Drawer::TextLayout Drawer::layoutText(const swf::EditText& field, const std::string& text, float scaleX, float scaleY) const
{
	TextLayout layout;
	layout.lineStep = static_cast<float>(field.leading) + field.fontHeight;
	// FUN_100bf280 makes text objects at round(height * 0.05) pixels.
	layout.font = engineFont(engineFontName(field.fontId), static_cast<int>(std::nearbyint(field.fontHeight * 0.05f)));
	if (!layout.font) {
		return layout; // gameswf's own glyph layout (0x100c06a0) runs instead; edit text never draws it
	}
	const hud::HudFont& font = *layout.font;
	std::vector<uint32_t> codes = codePoints(text, field.html);
	codes.push_back(0); // the loop reads the terminating NUL as a character too
	const float width = static_cast<float>(field.bounds.xMax - field.bounds.xMin) * 0.05f * scaleX;
	const float height = static_cast<float>(field.bounds.yMax - field.bounds.yMin) * scaleY; // twips
	const float wrapAt = width - field.rightMargin * 0.05f - 2.0f;
	std::vector<TextLine>& objects = layout.lines;
	// FUN_100bf280: a line's text object (made on first use) gets the text
	// up to its first NUL; a password field shows as many '*'.
	const auto setLine = [&](int index, const std::vector<uint32_t>& line) {
		std::vector<uint32_t> shown;
		for (const uint32_t c : line) {
			if (c == 0) {
				break;
			}
			shown.push_back(field.password ? '*' : c);
		}
		if (index >= static_cast<int>(objects.size())) {
			objects.resize(static_cast<size_t>(index) + 1);
		}
		objects[static_cast<size_t>(index)] = layoutLine(font, std::move(shown));
	};
	std::vector<uint32_t> current;
	int index = 0;
	float y = 0; // twips, the line's top
	int breakAt = -1;
	uint32_t previous = 0;
	bool cut = false;
	for (const uint32_t c : codes) {
		if (height < y && !field.autoSize) {
			--index;
			cut = c != 0; // the last line is kept even when empty
			break;
		}
		const auto nextLine = [&] {
			++index;
			y += layout.lineStep;
		};
		if (c == '\n' || c == '\r' || (previous == '|' && c == 'L')) {
			if (!(c == '\n' && previous == '\r')) {
				setLine(index, current);
				current.clear();
				breakAt = -1;
				nextLine();
			}
		} else if (c != '|') {
			if (c == ' ' || c == 0x2022) {
				breakAt = static_cast<int>(current.size());
			}
			current.push_back(c);
			setLine(index, current);
			if (field.wordWrap && wrapAt <= objects[static_cast<size_t>(index)].end) {
				if (breakAt < 0) {
					current.pop_back();
					setLine(index, current);
					current.assign(1, c);
				} else {
					std::vector<uint32_t> rest(current.begin() + breakAt + 1, current.end());
					current.resize(static_cast<size_t>(breakAt));
					setLine(index, current);
					current = std::move(rest);
					breakAt = -1;
				}
				nextLine();
			}
		}
		previous = c;
	}
	if (!cut && current.empty()) {
		--index;
	}
	const int count = static_cast<int>(objects.size());
	if (count <= index && 0 < index) {
		index = count - 1;
	}
	objects.resize(static_cast<size_t>(std::clamp(index + 1, 0, count)));
	for (const TextLine& line : objects) {
		layout.textWidth = std::max(layout.textWidth, line.end);
	}
	// The field's textHeight (0x100bd740's argument): the font's own
	// leading (EM units, unscaled) plus one line, or with autoSize down to
	// the last line.
	const swf::Font* swfFont = m_dictionary.font(field.fontId);
	const float fontLeading = swfFont && swfFont->hasLayout ? static_cast<float>(swfFont->leading) : 0.0f;
	const float textHeight = field.autoSize ? fontLeading + field.fontHeight + y : fontLeading + field.leading + field.fontHeight;
	layout.textHeight = static_cast<int>(std::nearbyint(textHeight * 0.05f));
	return layout;
}

void Drawer::editText(uint16_t id, const std::string& text, const swf::Matrix& world, const swf::ColorTransform& colour,
	std::vector<MenuBatch>& out)
{
	if (const swf::EditText* field = m_dictionary.editText(id)) {
		editText(*field, text, world, colour, out);
	}
}

namespace {

// Where the display puts line `i` (0x100c01a0): the field's origin (its
// world translation, not its bounds) plus the indent and left margin
// (not below 0) and 40 twips across, 30 twips plus the line steps down,
// unscaled; RendDX9.dll's drawText (0x101190b0) rounds that to pixels
// (round(v + 0.5)) and its text object adds the alignment offset
// (0x10183570: right, the field's width less the text, not below 0;
// centre, half of it) and floors (0x10182430).
struct LinePlace {
	float x, y;
};

LinePlace placeLine(const swf::EditText& field, const Drawer::TextLayout& layout, size_t i, const swf::Matrix& world,
	float scrollY)
{
	const float indent = std::max(0.0f, static_cast<float>(static_cast<int16_t>(field.indent)) + field.leftMargin);
	const float x = std::nearbyint((world.tx + indent + 40.0f) * 0.05f + 0.5f);
	const float y = std::nearbyint((world.ty + 30.0f + layout.lineStep * static_cast<float>(i) - scrollY) * 0.05f + 0.5f);
	const float width = std::nearbyint(static_cast<float>(field.bounds.xMax - field.bounds.xMin) * 0.05f * world.a + 0.5f);
	const float end = layout.lines[i].end;
	float offset = 0;
	if (field.align == swf::EditText::Align::Right) {
		offset = std::max(0.0f, width - end);
	} else if (field.align == swf::EditText::Align::Center) {
		offset = (width - end) * 0.5f;
	}
	return {std::floor(x + offset + 0.5f), std::floor(y)};
}

} // namespace

// Each line's glyphs at their boxes, unscaled and unrotated (the display
// passes the world matrix with its scale set to 1), in the field's text
// colour under the colour transform. Texels are sampled 0.01 in, as
// RendDX9.dll does for unscaled fonts (0x10181f10); its half-pixel shift
// is Direct3D 9's and has no counterpart here.
void Drawer::editText(const swf::EditText& field, const std::string& text, const swf::Matrix& world,
	const swf::ColorTransform& colour, std::vector<MenuBatch>& out, float scrollY)
{
	const TextLayout layout = layoutText(field, text, world.a, world.d);
	if (!layout.font) {
		return;
	}
	const hud::HudFont& font = *layout.font;
	const swf::Rgba c = field.textColor;
	const float mult[4] = {colour.mult[0] * c.r / 255.0f, colour.mult[1] * c.g / 255.0f, colour.mult[2] * c.b / 255.0f,
		colour.mult[3] * c.a / 255.0f};
	const float add[4] = {colour.add[0] / 255.0f, colour.add[1] / 255.0f, colour.add[2] / 255.0f, colour.add[3] / 255.0f};
	const float tw = static_cast<float>(std::max(font.textureWidth, 1));
	const float th = static_cast<float>(std::max(font.textureHeight, 1));
	MenuBatch batch;
	batch.texture = font.name + ".dds";
	for (size_t i = 0; i < layout.lines.size(); ++i) {
		const TextLine& line = layout.lines[i];
		const LinePlace at = placeLine(field, layout, i, world, scrollY);
		for (size_t k = 0; k < line.codes.size(); ++k) {
			const hud::Glyph* g = font.glyph(line.codes[k] == 0xa0 ? ' ' : line.codes[k]);
			if (!g || g->x1 <= g->x0 || g->y1 <= g->y0) {
				continue;
			}
			const float x0 = at.x + line.boxes[k].x, y0 = at.y + line.boxes[k].top;
			const float x1 = x0 + static_cast<float>(g->x1 - g->x0), y1 = y0 + static_cast<float>(g->y1 - g->y0);
			const float u0 = (static_cast<float>(g->x0) + 0.01f) / tw, v0 = (static_cast<float>(g->y0) + 0.01f) / th;
			const float u1 = (static_cast<float>(g->x1) + 0.01f) / tw, v1 = (static_cast<float>(g->y1) + 0.01f) / th;
			const MenuVertex corners[4] = {{x0, y0, u0, v0, {mult[0], mult[1], mult[2], mult[3]}, {add[0], add[1], add[2], add[3]}},
				{x1, y0, u1, v0, {mult[0], mult[1], mult[2], mult[3]}, {add[0], add[1], add[2], add[3]}},
				{x1, y1, u1, v1, {mult[0], mult[1], mult[2], mult[3]}, {add[0], add[1], add[2], add[3]}},
				{x0, y1, u0, v1, {mult[0], mult[1], mult[2], mult[3]}, {add[0], add[1], add[2], add[3]}}};
			for (int v : {0, 1, 2, 0, 2, 3}) {
				batch.vertices.push_back(corners[v]);
			}
		}
	}
	if (!batch.vertices.empty()) {
		out.push_back(std::move(batch));
	}
}

void Drawer::textCaret(const swf::EditText& field, const std::string& text, const swf::Matrix& world,
	const swf::ColorTransform& colour, std::vector<MenuBatch>& out)
{
	const TextLayout layout = layoutText(field, text, world.a, world.d);
	if (!layout.font || layout.lines.empty()) {
		return;
	}
	const size_t last = layout.lines.size() - 1;
	const LinePlace at = placeLine(field, layout, last, world, 0);
	const float x = at.x + layout.lines[last].end;
	const float height = std::nearbyint(field.fontHeight * 0.05f);
	const swf::Rect r {static_cast<int32_t>(x * kTwips), static_cast<int32_t>((x + 1) * kTwips), static_cast<int32_t>(at.y * kTwips),
		static_cast<int32_t>((at.y + height) * kTwips)};
	rectangle(r, field.textColor, {}, colour, out);
}

void Drawer::rectangle(const swf::Rect& rect, swf::Rgba fill, const swf::Matrix& world, const swf::ColorTransform& colour,
	std::vector<MenuBatch>& out)
{
	const float mult[4] = {colour.mult[0] * fill.r / 255.0f, colour.mult[1] * fill.g / 255.0f, colour.mult[2] * fill.b / 255.0f,
		colour.mult[3] * fill.a / 255.0f};
	const float add[4] = {colour.add[0] / 255.0f, colour.add[1] / 255.0f, colour.add[2] / 255.0f, colour.add[3] / 255.0f};
	MenuBatch batch;
	batch.image = m_white;
	const swf::Point corners[4] = {{static_cast<float>(rect.xMin), static_cast<float>(rect.yMin)},
		{static_cast<float>(rect.xMax), static_cast<float>(rect.yMin)}, {static_cast<float>(rect.xMax), static_cast<float>(rect.yMax)},
		{static_cast<float>(rect.xMin), static_cast<float>(rect.yMax)}};
	for (int i : {0, 1, 2, 0, 2, 3}) {
		pushVertex(batch.vertices, world, corners[i], 0.5f, 0.5f, mult, add);
	}
	out.push_back(std::move(batch));
}

void Drawer::image(const std::shared_ptr<const MenuImage>& image, const swf::Matrix& world, const swf::ColorTransform& colour,
	std::vector<MenuBatch>& out)
{
	if (!image || image->width == 0 || image->height == 0) {
		return;
	}
	const float mult[4] = {colour.mult[0], colour.mult[1], colour.mult[2], colour.mult[3]};
	const float add[4] = {colour.add[0] / 255.0f, colour.add[1] / 255.0f, colour.add[2] / 255.0f, colour.add[3] / 255.0f};
	MenuBatch batch;
	batch.image = image;
	const float w = image->width * kTwips, h = image->height * kTwips;
	const swf::Point corners[4] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
	const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	for (int i : {0, 1, 2, 0, 2, 3}) {
		pushVertex(batch.vertices, world, corners[i], uv[i][0], uv[i][1], mult, add);
	}
	out.push_back(std::move(batch));
}

void Drawer::dynamicShape(const swf::Shape& shape, const swf::ShapeMesh& mesh, uint64_t cacheKey, const swf::Matrix& world,
	const swf::ColorTransform& colour, std::vector<MenuBatch>& out)
{
	drawMesh(shape, mesh, (uint64_t {2} << 40) | cacheKey, world, colour, out);
}

const swf::ShapeMesh* Drawer::shapeMesh(uint16_t id)
{
	const swf::Shape* shape = m_dictionary.shape(id);
	if (!shape) {
		return nullptr;
	}
	auto it = m_meshes.find(id);
	if (it == m_meshes.end()) {
		it = m_meshes.emplace(id, swf::tessellateShape(*shape)).first;
	}
	return &it->second;
}

namespace {

bool insideTriangle(swf::Point p, swf::Point a, swf::Point b, swf::Point c)
{
	const auto side = [](swf::Point p, swf::Point a, swf::Point b) {
		return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
	};
	const float d1 = side(p, a, b), d2 = side(p, b, c), d3 = side(p, c, a);
	const bool negative = d1 < 0 || d2 < 0 || d3 < 0;
	const bool positive = d1 > 0 || d2 > 0 || d3 > 0;
	return !(negative && positive);
}

} // namespace

bool hitMesh(const swf::ShapeMesh& mesh, swf::Point local)
{
	for (const swf::MeshBatch& part : mesh.batches) {
		for (size_t i = 0; i + 2 < part.triangles.size(); i += 3) {
			if (insideTriangle(local, part.triangles[i], part.triangles[i + 1], part.triangles[i + 2])) {
				return true;
			}
		}
	}
	return false;
}

namespace {

MenuVertex lerpVertex(const MenuVertex& a, const MenuVertex& b, float t)
{
	MenuVertex v;
	v.x = a.x + (b.x - a.x) * t;
	v.y = a.y + (b.y - a.y) * t;
	v.u = a.u + (b.u - a.u) * t;
	v.v = a.v + (b.v - a.v) * t;
	for (int i = 0; i < 4; ++i) {
		v.mult[i] = a.mult[i] + (b.mult[i] - a.mult[i]) * t;
		v.add[i] = a.add[i] + (b.add[i] - a.add[i]) * t;
	}
	return v;
}

// One Sutherland-Hodgman step: keeps the part where side(v) >= 0.
template <typename Side>
void clipPolygon(std::vector<MenuVertex>& polygon, Side side)
{
	std::vector<MenuVertex> out;
	for (size_t i = 0; i < polygon.size(); ++i) {
		const MenuVertex& a = polygon[i];
		const MenuVertex& b = polygon[(i + 1) % polygon.size()];
		const float da = side(a), db = side(b);
		if (da >= 0) {
			out.push_back(a);
		}
		if ((da >= 0) != (db >= 0)) {
			out.push_back(lerpVertex(a, b, da / (da - db)));
		}
	}
	polygon = std::move(out);
}

} // namespace

void clipBatches(std::vector<MenuBatch>& batches, size_t from, float x0, float y0, float x1, float y1)
{
	for (size_t b = from; b < batches.size(); ++b) {
		std::vector<MenuVertex>& vertices = batches[b].vertices;
		std::vector<MenuVertex> kept;
		kept.reserve(vertices.size());
		for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
			const MenuVertex* t = &vertices[i];
			const float minX = std::min({t[0].x, t[1].x, t[2].x}), maxX = std::max({t[0].x, t[1].x, t[2].x});
			const float minY = std::min({t[0].y, t[1].y, t[2].y}), maxY = std::max({t[0].y, t[1].y, t[2].y});
			if (maxX <= x0 || minX >= x1 || maxY <= y0 || minY >= y1) {
				continue; // outside
			}
			if (minX >= x0 && maxX <= x1 && minY >= y0 && maxY <= y1) {
				kept.insert(kept.end(), t, t + 3); // inside
				continue;
			}
			std::vector<MenuVertex> polygon(t, t + 3);
			clipPolygon(polygon, [x0](const MenuVertex& v) { return v.x - x0; });
			clipPolygon(polygon, [x1](const MenuVertex& v) { return x1 - v.x; });
			clipPolygon(polygon, [y0](const MenuVertex& v) { return v.y - y0; });
			clipPolygon(polygon, [y1](const MenuVertex& v) { return y1 - v.y; });
			for (size_t k = 1; k + 1 < polygon.size(); ++k) {
				kept.push_back(polygon[0]);
				kept.push_back(polygon[k]);
				kept.push_back(polygon[k + 1]);
			}
		}
		vertices = std::move(kept);
	}
	batches.erase(std::remove_if(batches.begin() + static_cast<std::ptrdiff_t>(from), batches.end(),
					  [](const MenuBatch& batch) { return batch.vertices.empty(); }),
		batches.end());
}

bool Drawer::hitShape(uint16_t id, swf::Point local)
{
	const swf::Shape* shape = m_dictionary.shape(id);
	if (!shape || local.x < shape->bounds.xMin || local.x > shape->bounds.xMax || local.y < shape->bounds.yMin ||
		local.y > shape->bounds.yMax) {
		return false;
	}
	const swf::ShapeMesh* mesh = shapeMesh(id);
	return mesh && hitMesh(*mesh, local);
}

} // namespace dice::hfe::ui
