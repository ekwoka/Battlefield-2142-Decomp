#include "Draw.h"

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

size_t utf8Length(const std::string& s)
{
	size_t n = 0;
	for (const char c : s) {
		n += (static_cast<unsigned char>(c) & 0xc0) != 0x80;
	}
	return n;
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

const swf::Font* Drawer::outlineFont(uint16_t fontId) const
{
	const swf::Font* font = m_dictionary.font(fontId);
	if (font && !font->isDeviceFont()) {
		return font;
	}
	for (const auto& [id, character] : m_dictionary.characters()) {
		if (const auto* candidate = std::get_if<swf::Font>(&character); candidate && !candidate->isDeviceFont()) {
			return candidate;
		}
	}
	return nullptr;
}

const swf::ShapeMesh& Drawer::glyphMesh(const swf::Font& font, size_t glyph)
{
	auto it = m_glyphs.find({font.id, glyph});
	if (it == m_glyphs.end()) {
		swf::TessellateOptions options;
		options.curveTolerance = 4; // EM units (1024 a glyph)
		it = m_glyphs.emplace(std::make_pair(font.id, glyph), swf::tessellateShape(font.glyphs[glyph].shape, options)).first;
	}
	return it->second;
}

// Layout (provisional, simpler than Flash's): lines break at newlines and,
// with word wrap, at the last space that fits; each line is aligned in the
// field's width less its margins; the first baseline is two pixels plus
// the ascent below the top.
Drawer::TextLayout Drawer::layoutText(const swf::EditText& field, const std::string& text) const
{
	TextLayout layout;
	layout.font = outlineFont(field.fontId);
	const swf::Font* font = layout.font;
	if (!font) {
		return layout;
	}
	layout.scale = field.fontHeight / 1024.0f; // twips per EM unit
	const float scale = layout.scale;
	layout.codes = codePoints(field.password ? std::string(utf8Length(text), '*') : text, field.html);
	const std::vector<uint32_t>& codes = layout.codes;
	layout.left = field.bounds.xMin + 40.0f + field.leftMargin + field.indent;
	layout.width = static_cast<float>(field.bounds.xMax - field.bounds.xMin) - 80.0f - field.leftMargin - field.rightMargin;
	const auto advance = [&](uint32_t code) { return glyphAdvance(*font, scale, code); };
	size_t start = 0;
	float lineWidth = 0;
	size_t lastSpace = std::string::npos;
	const auto endLine = [&](size_t from, size_t to) {
		TextLine line {from, to, 0};
		for (size_t i = from; i < to; ++i) {
			line.width += advance(codes[i]);
		}
		layout.lines.push_back(line);
	};
	for (size_t i = 0; i < codes.size(); ++i) {
		if (codes[i] == '\n' || codes[i] == '\r') {
			endLine(start, i);
			start = i + 1;
			lineWidth = 0;
			lastSpace = std::string::npos;
			continue;
		}
		lineWidth += advance(codes[i]);
		if (codes[i] == ' ') {
			lastSpace = i;
		}
		if (field.wordWrap && lineWidth > layout.width && lastSpace != std::string::npos && lastSpace > start) {
			endLine(start, lastSpace);
			start = lastSpace + 1;
			lastSpace = std::string::npos;
			lineWidth = 0;
			for (size_t j = start; j <= i; ++j) {
				lineWidth += advance(codes[j]);
			}
		}
	}
	endLine(start, codes.size());
	layout.lineHeight = (font->ascent + font->descent) * scale + field.leading;
	layout.ascent = font->ascent * scale;
	for (const TextLine& line : layout.lines) {
		layout.textWidth = std::max(layout.textWidth, line.width);
	}
	layout.textHeight = layout.lineHeight * static_cast<float>(layout.lines.size());
	return layout;
}

float Drawer::glyphAdvance(const swf::Font& font, float scale, uint32_t code)
{
	const int glyph = font.glyphIndex(static_cast<uint16_t>(code));
	if (glyph < 0) {
		return code == ' ' ? 256 * scale : 0.0f;
	}
	const swf::FontGlyph& g = font.glyphs[static_cast<size_t>(glyph)];
	return (font.hasLayout ? g.advance : static_cast<float>(g.shape.bounds.xMax)) * scale;
}

void Drawer::editText(uint16_t id, const std::string& text, const swf::Matrix& world, const swf::ColorTransform& colour,
	std::vector<MenuBatch>& out)
{
	if (const swf::EditText* field = m_dictionary.editText(id)) {
		editText(*field, text, world, colour, out);
	}
}

void Drawer::editText(const swf::EditText& field, const std::string& text, const swf::Matrix& world,
	const swf::ColorTransform& colour, std::vector<MenuBatch>& out, float scrollY)
{
	if (text.empty()) {
		return;
	}
	const TextLayout layout = layoutText(field, text);
	const swf::Font* font = layout.font;
	if (!font) {
		return;
	}
	const swf::Rgba c = field.textColor;
	const float mult[4] = {colour.mult[0] * c.r / 255.0f, colour.mult[1] * c.g / 255.0f, colour.mult[2] * c.b / 255.0f,
		colour.mult[3] * c.a / 255.0f};
	const float add[4] = {colour.add[0] / 255.0f, colour.add[1] / 255.0f, colour.add[2] / 255.0f, colour.add[3] / 255.0f};
	MenuBatch batch;
	batch.image = m_white;
	float baseline = field.bounds.yMin + 40.0f + layout.ascent - scrollY;
	for (const TextLine& line : layout.lines) {
		if (baseline - layout.ascent >= field.bounds.yMin - 1 || field.autoSize) {
			float pen = layout.left;
			if (field.align == swf::EditText::Align::Right) {
				pen += layout.width - line.width;
			} else if (field.align == swf::EditText::Align::Center) {
				pen += (layout.width - line.width) / 2;
			}
			for (size_t i = line.from; i < line.to; ++i) {
				const uint32_t code = layout.codes[i];
				const int glyph = font->glyphIndex(static_cast<uint16_t>(code));
				if (glyph >= 0) {
					const swf::ShapeMesh& mesh = glyphMesh(*font, static_cast<size_t>(glyph));
					const swf::Matrix place {layout.scale, 0, 0, layout.scale, pen, baseline};
					const swf::Matrix toStage = multiply(world, place);
					for (const swf::MeshBatch& part : mesh.batches) {
						if (part.kind != swf::MeshBatch::Kind::Fill) {
							continue;
						}
						for (const swf::Point& p : part.triangles) {
							pushVertex(batch.vertices, toStage, p, 0.5f, 0.5f, mult, add);
						}
					}
				}
				pen += glyphAdvance(*font, layout.scale, code);
			}
		}
		baseline += layout.lineHeight;
		if (baseline - layout.ascent > field.bounds.yMax && !field.autoSize) {
			break; // past the field
		}
	}
	if (!batch.vertices.empty()) {
		out.push_back(std::move(batch));
	}
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
