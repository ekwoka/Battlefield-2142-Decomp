// Vector shapes: DefineShape (2), DefineShape2 (22), DefineShape3 (32),
// DefineMorphShape (46), and the glyph outlines of DefineFont2.
//
// A shape is a list of edges (straight or quadratic) drawn by a pen. Style
// change records move the pen and pick three styles for the edges that
// follow: fill0 (the fill on the left of the direction of travel), fill1 (on
// the right) and a line style. Indices are 1-based into the current style
// arrays, 0 meaning none. DefineShape2/3 may bring a fresh set of styles in
// the middle ("new styles"); everything after that is a new layer drawn on
// top of the earlier ones. Here the arrays are merged and the indices made
// global, with each path remembering its layer.
//
// DefineShape and DefineShape2 colours are RGB, DefineShape3 and morph
// shapes RGBA. Gradients map the square (-16384,-16384)-(16384,16384)
// through their matrix; bitmap fills map one bitmap pixel to one twip unit
// through theirs (so a 1:1 bitmap has a scale of 20).
#pragma once

#include "UI/Characters/BitReader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dice::hfe::ui::swf {

struct GradientStop {
	uint8_t ratio = 0; // 0..255 along the gradient
	Rgba color;
};

struct FillStyle {
	enum class Type : uint8_t { Solid, LinearGradient, RadialGradient, FocalGradient, Bitmap };

	Type type = Type::Solid;
	uint8_t rawType = 0; // the file's byte (0x00, 0x10, 0x12, 0x13, 0x40..0x43)
	Rgba color; // solid fills
	Matrix matrix; // gradient or bitmap matrix
	std::vector<GradientStop> stops;
	uint8_t spreadMode = 0; // 0 pad, 1 reflect, 2 repeat (SWF 8; 0 in SWF 7)
	uint8_t interpolationMode = 0; // 0 normal RGB, 1 linear RGB (SWF 8)
	float focalPoint = 0; // focal gradients only
	uint16_t bitmapId = 0; // not a bitmap (0xffff, or a stale id: mainMenu has some) -> draw nothing
	bool repeat = true; // 0x40/0x42 repeat, 0x41/0x43 clip (edge pixels stretch)
	bool smooth = true; // 0x40/0x41 bilinear, 0x42/0x43 nearest
};

struct LineStyle {
	uint16_t width = 0; // twips; 0 is a hairline
	Rgba color;
};

struct ShapeEdge {
	Point control; // straight edges: the midpoint (eases morphing)
	Point anchor; // end point; the start is the previous edge's anchor
	bool curved = false;
};

// Edges drawn without lifting the pen, all with the same styles.
struct ShapePath {
	uint32_t fill0 = 0, fill1 = 0, line = 0; // 1-based into the shape's arrays, 0 = none
	uint32_t layer = 0;
	Point start;
	std::vector<ShapeEdge> edges;
};

struct Shape {
	uint16_t id = 0;
	Rect bounds;
	std::vector<FillStyle> fillStyles;
	std::vector<LineStyle> lineStyles;
	std::vector<ShapePath> paths;
	uint32_t layerCount = 1;
};

// Parses a DefineShape/2/3 tag body (`tagCode` is 2, 22 or 32).
bool parseDefineShape(const uint8_t* body, size_t size, uint16_t tagCode, Shape& out, std::string* error = nullptr);

// Parses a SHAPE record (glyphs: fill/line bit counts then records, no style
// arrays) at the reader's position. The paths use fill style 1, which the
// caller supplies.
bool parseGlyphShape(BitReader& in, Shape& out);

struct MorphGradientStop {
	uint8_t startRatio = 0, endRatio = 0;
	Rgba startColor, endColor;
};

struct MorphFillStyle {
	FillStyle::Type type = FillStyle::Type::Solid;
	uint8_t rawType = 0;
	Rgba startColor, endColor;
	Matrix startMatrix, endMatrix;
	std::vector<MorphGradientStop> stops;
	uint16_t bitmapId = 0;
	bool repeat = true;
	bool smooth = true;
};

struct MorphLineStyle {
	uint16_t startWidth = 0, endWidth = 0;
	Rgba startColor, endColor;
};

// One edge of a morph shape's start or end outline with its absolute start
// point (the end outline only moves the pen; its styles come from the start).
struct MorphEdge {
	Point from;
	ShapeEdge edge;
};

struct MorphShape {
	uint16_t id = 0;
	Rect startBounds, endBounds;
	std::vector<MorphFillStyle> fillStyles;
	std::vector<MorphLineStyle> lineStyles;
	std::vector<ShapePath> startPaths; // with styles
	std::vector<MorphEdge> endEdges; // as many as startPaths have edges
};

bool parseDefineMorphShape(const uint8_t* body, size_t size, MorphShape& out, std::string* error = nullptr);

// The shape at `ratio` (0 = start, 1 = end; PlaceObject2's ratio / 65535).
// Straight edges facing a curve become curves with a midpoint control.
Shape interpolateMorphShape(const MorphShape& morph, float ratio);

} // namespace dice::hfe::ui::swf
