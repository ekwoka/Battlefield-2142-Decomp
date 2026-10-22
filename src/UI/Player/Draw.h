// Turning SWF characters into the renderer's menu batches (MenuDraw.h):
// shapes and morph shapes from their tessellated meshes, text fields from
// their fonts' glyph outlines. Fills become textures:
//   solid    a white texel tinted per vertex
//   gradient a small image of the gradient (256 x 1 linear, 128 x 128
//            radial), sampled in the gradient's ±16384-twip square
//   bitmap   the bitmap (straight alpha), sampled through the inverse of
//            its fill matrix (bitmap pixels per shape twip)
// and the colour transform goes into each vertex's multiply and add.
// Meshes and images are made once per character and kept.
#pragma once

#include "UI/Characters/CharacterDictionary.h"
#include "UI/Characters/Tessellate.h"
#include "UI/MenuDraw.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::ui {

swf::Matrix multiply(const swf::Matrix& outer, const swf::Matrix& inner); // inner first
swf::Matrix inverse(const swf::Matrix& m);
swf::ColorTransform combine(const swf::ColorTransform& outer, const swf::ColorTransform& inner);
// Cuts the triangles of batches[from...] to the stage rectangle
// (pixels), interpolating texture coordinates and colours; batches left
// empty are removed. How masks are applied (as their bounding boxes).
void clipBatches(std::vector<MenuBatch>& batches, size_t from, float x0, float y0, float x1, float y1);
// Is `local` inside one of the mesh's triangles?
bool hitMesh(const swf::ShapeMesh& mesh, swf::Point local);

class Drawer {
public:
	explicit Drawer(const swf::CharacterDictionary& dictionary);

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

	// How a field lays `text` out: lines as ranges of code points, sizes in twips.
	struct TextLine {
		size_t from = 0, to = 0;
		float width = 0;
	};
	struct TextLayout {
		const swf::Font* font = nullptr;
		float scale = 0; // twips per EM unit
		std::vector<uint32_t> codes;
		std::vector<TextLine> lines;
		float left = 0, width = 0; // where lines go
		float ascent = 0, lineHeight = 0;
		float textWidth = 0, textHeight = 0;
	};
	TextLayout layoutText(const swf::EditText& field, const std::string& text) const;

	// The tessellated shape (made once), or null.
	const swf::ShapeMesh* shapeMesh(uint16_t id);
	// Is `local` (shape twips) inside the filled or stroked area of shape `id`?
	bool hitShape(uint16_t id, swf::Point local);

	// The font a field draws with: its own, or for device fonts (no
	// outlines) the movie's first font that has them.
	const swf::Font* outlineFont(uint16_t fontId) const;

private:
	static float glyphAdvance(const swf::Font& font, float scale, uint32_t code);
	void drawMesh(const swf::Shape& shape, const swf::ShapeMesh& mesh, uint64_t cacheKey, const swf::Matrix& world,
		const swf::ColorTransform& colour, std::vector<MenuBatch>& out);
	std::shared_ptr<const MenuImage> bitmapImage(uint16_t id);
	std::shared_ptr<const MenuImage> gradientImage(uint64_t key, const swf::FillStyle& style);
	const swf::ShapeMesh& glyphMesh(const swf::Font& font, size_t glyph);

	const swf::CharacterDictionary& m_dictionary;
	std::shared_ptr<MenuImage> m_white;
	std::map<uint16_t, swf::ShapeMesh> m_meshes;
	std::map<uint16_t, std::shared_ptr<const MenuImage>> m_bitmaps;
	std::map<uint64_t, std::shared_ptr<const MenuImage>> m_gradients;
	std::map<std::pair<uint16_t, size_t>, swf::ShapeMesh> m_glyphs;
};

} // namespace dice::hfe::ui
