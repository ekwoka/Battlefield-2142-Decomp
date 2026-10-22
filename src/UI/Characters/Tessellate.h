// Turns a Shape into triangles a renderer can draw.
//
// Fills: every fill style's region is bounded by the edges that have it on
// exactly one side (fill0 xor fill1; an edge with the style on both sides
// is interior). Those edges, curves flattened, are cut into horizontal bands
// at every vertex and every crossing, and each band is filled between edge
// pairs by the even-odd rule, giving two triangles per trapezoid. Even-odd
// over the per-style boundary is how Flash's fill0/fill1 ownership resolves
// for well-formed shapes (holes and islands included) and needs no closed
// loops, so the odd broken outline still draws something sensible.
//
// Strokes: each path with a line style becomes quads along its segments
// with round joins and round caps (SWF 7 has no other kind), as triangles.
// Widths below `minStrokeWidth` (one pixel; 0 means hairline) are widened.
//
// Draw order: layer by layer (new-style groups), fills before strokes in a
// layer, then by style index.
#pragma once

#include "UI/Characters/Shape.h"

#include <cstdint>
#include <vector>

namespace dice::hfe::ui::swf {

struct TessellateOptions {
	float curveTolerance = 2.0f; // largest distance of a flattened curve from the true one, in shape units
	float minStrokeWidth = 20.0f; // in shape units (twips: one pixel)
	bool nonZero = false; // non-zero winding instead of even-odd (glyphs from some tools)
};

struct MeshBatch {
	enum class Kind : uint8_t { Fill, Stroke };

	Kind kind = Kind::Fill;
	uint32_t style = 0; // 0-based into Shape::fillStyles or Shape::lineStyles
	uint32_t layer = 0;
	bool hairline = false; // strokes: the file's width was under minStrokeWidth
	std::vector<Point> triangles; // three per triangle, shape units
};

struct ShapeMesh {
	std::vector<MeshBatch> batches;

	size_t triangleCount() const;
};

ShapeMesh tessellateShape(const Shape& shape, const TessellateOptions& options = {});

// Appends the curve from `p0` through control `p1` to `p2` as points after
// p0 (p2 last), each segment within `tolerance` of the curve.
void flattenQuadratic(Point p0, Point p1, Point p2, float tolerance, std::vector<Point>& out);

// Sum of the triangles' (unsigned) areas.
double triangleArea(const std::vector<Point>& triangles);

} // namespace dice::hfe::ui::swf
