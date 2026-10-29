// Turning SWF characters into the renderer's menu batches (MenuDraw.h):
// shapes and morph shapes from their tessellated meshes, text fields with
// the engine's bitmap fonts (see layoutText). Fills become textures:
//   solid    a white texel tinted per vertex
//   gradient a small image of the gradient (256 x 1 linear, 128 x 128
//            radial), sampled in the gradient's ±16384-twip square
//   bitmap   the bitmap (straight alpha), sampled through the inverse of
//            its fill matrix (bitmap pixels per shape twip)
// and the colour transform goes into each vertex's multiply and add.
// Meshes and images are made once per character and kept.
#pragma once

#include "Stencil.h"
#include "UI/Characters/CharacterDictionary.h"
#include "UI/Characters/Tessellate.h"
#include "UI/MenuDraw.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dice::hfe::hud {
struct HudFont;
}

namespace dice::hfe::ui {

swf::Matrix multiply(const swf::Matrix& outer, const swf::Matrix& inner); // inner first
swf::Matrix inverse(const swf::Matrix& m);
swf::ColorTransform combine(const swf::ColorTransform& outer, const swf::ColorTransform& inner);
// Cuts the triangles of batches[from...] to the stage rectangle
// (pixels), interpolating texture coordinates and colours; batches left
// empty are removed.
void clipBatches(std::vector<MenuBatch>& batches, size_t from, float x0, float y0, float x1, float y1);
// Is `local` inside one of the mesh's triangles?
bool hitMesh(const swf::ShapeMesh& mesh, swf::Point local);

class Drawer {
public:
	explicit Drawer(const swf::CharacterDictionary& dictionary);
	~Drawer();

	// `world` takes the character's twips to stage twips (the batches come
	// out in stage pixels).
	void shape(uint16_t id, const swf::Matrix& world, const swf::ColorTransform& colour, std::vector<MenuBatch>& out);
	void morphShape(uint16_t id, float ratio, const swf::Matrix& world, const swf::ColorTransform& colour,
		std::vector<MenuBatch>& out);
	// A text field showing `text` (UTF-8; markup stripped when it is HTML).
	void editText(uint16_t id, const std::string& text, const swf::Matrix& world, const swf::ColorTransform& colour,
		std::vector<MenuBatch>& out);
	// The same for a field whose properties scripts changed (colour, size,
	// bounds...). `scrollY` (twips) moves the text up.
	void editText(const swf::EditText& field, const std::string& text, const swf::Matrix& world,
		const swf::ColorTransform& colour, std::vector<MenuBatch>& out, float scrollY = 0);
	// An input field's caret: one pixel wide, the font's size high, after
	// the last line's text.
	void textCaret(const swf::EditText& field, const std::string& text, const swf::Matrix& world,
		const swf::ColorTransform& colour, std::vector<MenuBatch>& out);
	// A solid rectangle (text field borders and backgrounds), in twips.
	void rectangle(const swf::Rect& rect, swf::Rgba fill, const swf::Matrix& world, const swf::ColorTransform& colour,
		std::vector<MenuBatch>& out);
	// A loaded image, one pixel per 20 twips, its top left at the origin.
	void image(const std::shared_ptr<const MenuImage>& image, const swf::Matrix& world, const swf::ColorTransform& colour,
		std::vector<MenuBatch>& out);
	// A shape that is not in the dictionary (the drawing API). `cacheKey`
	// identifies its gradients.
	void dynamicShape(const swf::Shape& shape, const swf::ShapeMesh& mesh, uint64_t cacheKey, const swf::Matrix& world,
		const swf::ColorTransform& colour, std::vector<MenuBatch>& out);

	// Text fields draw with the engine's fonts, as SwiffPlayer.dll did: a
	// field whose font has the name of an exported font (fonts 1-4 export
	// as StandardTextBold, StandardText, DynamicText and Titles) gets one
	// RendDX9.dll text object per line, in the bitmap font
	// "Fonts/<language>/<export name>_<round(height / 20)>.dif" (else
	// "Fonts/..."). A field without one draws no text. How a field lays its
	// text out (0x100bfd00):
	//   - '\r', '\n' (not after '\r') and "|L" end a line; '|' is dropped;
	//   - with word wrap, once a line's text reaches the field's width
	//     (times the world x scale) less its right margin and 2 pixels, the
	//     line ends at its last space or bullet (the space dropped), or
	//     before the character that did not fit;
	//   - unless the field sizes itself, lines that start below its height
	//     are dropped;
	//   - lines are leading + font height twips apart.
	// A line's characters sit where RendDX9.dll puts them (0x10183240):
	// the pen moves by the kerning, the glyph goes at
	// floor(pen + before + 0.5) and the pen moves on by width + after.
	struct GlyphBox {
		float left = 0, right = 0; // the character's extent (positions and widths)
		float x = 0, top = 0;      // where its glyph goes
	};
	struct TextLine {
		std::vector<uint32_t> codes; // as shown (passwords as '*')
		std::vector<GlyphBox> boxes;
		float end = 0; // after the last character, pixels (0 when empty)
	};
	struct TextLayout {
		const hud::HudFont* font = nullptr; // null: the field draws no text
		std::vector<TextLine> lines;
		float lineStep = 0;   // twips from one line to the next
		int textHeight = 666; // the field's textHeight, pixels (666 until laid out: 0x100c1700)
		float textWidth = 0;  // the widest line, pixels
	};
	// `scaleX` and `scaleY` are the field's world scale (the world matrix's a and d).
	// Layouts are kept by everything they depend on (the text, the field's
	// font, size, bounds, margins, flags and the scale), so a field drawn
	// again unchanged is not laid out again; the reference lasts until the
	// next call.
	const TextLayout& layoutText(const swf::EditText& field, const std::string& text, float scaleX = 1, float scaleY = 1) const;
	// The exported font name a field's font draws with, or empty.
	std::string engineFontName(uint16_t fontId) const;
	// Reads the engine's fonts (.dif text, by game path, e.g. "Fonts/x.dif").
	using FontReader = std::function<bool(const std::string& path, std::string& text)>;
	void setFontReader(FontReader reader)
	{
		m_fontReader = std::move(reader);
		m_layouts.clear();
	}
	const hud::HudFont* engineFont(const std::string& name, int pixels) const;
	// One line of an engine font laid out as RendDX9 does (0x10183240):
	// its width, and its glyphs with the top left at (x, y), stage pixels.
	float lineWidth(const hud::HudFont& font, const std::vector<uint32_t>& codes) const;
	void engineLine(const hud::HudFont& font, const std::vector<uint32_t>& codes, float x, float y, swf::Rgba colour,
		std::vector<MenuBatch>& out) const;

	// The mask state of the frame being drawn (Stencil.h).
	Stencil& stencil() { return m_stencil; }

	// The tessellated shape (made once), or null.
	const swf::ShapeMesh* shapeMesh(uint16_t id);
	// Is `local` (shape twips) inside the filled or stroked area of shape `id`?
	bool hitShape(uint16_t id, swf::Point local);

private:
	void drawMesh(const swf::Shape& shape, const swf::ShapeMesh& mesh, uint64_t cacheKey, const swf::Matrix& world,
		const swf::ColorTransform& colour, std::vector<MenuBatch>& out);
	std::shared_ptr<const MenuImage> bitmapImage(uint16_t id);
	std::shared_ptr<const MenuImage> gradientImage(uint64_t key, const swf::FillStyle& style);
	TextLayout makeLayout(const swf::EditText& field, const std::string& text, float scaleX, float scaleY) const;

	struct LayoutKey {
		std::string text;
		uint16_t fontId = 0, fontHeight = 0, rightMargin = 0;
		int16_t leading = 0;
		swf::Rect bounds;
		uint8_t flags = 0; // html, password, wordWrap, autoSize
		float scaleX = 1, scaleY = 1;
		bool operator==(const LayoutKey& o) const;
	};
	struct LayoutKeyHash {
		size_t operator()(const LayoutKey& k) const;
	};

	const swf::CharacterDictionary& m_dictionary;
	std::shared_ptr<MenuImage> m_white;
	std::map<uint16_t, swf::ShapeMesh> m_meshes;
	std::map<uint16_t, std::shared_ptr<const MenuImage>> m_bitmaps;
	std::map<uint64_t, std::shared_ptr<const MenuImage>> m_gradients;
	FontReader m_fontReader;
	mutable bool m_fontNamesBuilt = false;
	mutable std::map<uint16_t, std::string> m_fontNames;                     // font id -> export name
	mutable std::map<std::string, std::unique_ptr<hud::HudFont>> m_engineFonts; // "name_size" (null: missing)
	mutable std::unordered_map<LayoutKey, TextLayout, LayoutKeyHash> m_layouts;
	Stencil m_stencil;
};

} // namespace dice::hfe::ui
