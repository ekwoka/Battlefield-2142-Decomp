#include "Shape.h"

#include <algorithm>

namespace dice::hfe::ui::swf {

namespace {

void setError(std::string* error, const char* message)
{
	if (error) {
		*error = message;
	}
}

FillStyle::Type typeOf(uint8_t raw, bool& known)
{
	known = true;
	switch (raw) {
	case 0x00: return FillStyle::Type::Solid;
	case 0x10: return FillStyle::Type::LinearGradient;
	case 0x12: return FillStyle::Type::RadialGradient;
	case 0x13: return FillStyle::Type::FocalGradient;
	case 0x40:
	case 0x41:
	case 0x42:
	case 0x43: return FillStyle::Type::Bitmap;
	default: known = false; return FillStyle::Type::Solid;
	}
}

// FILLSTYLEARRAY / LINESTYLEARRAY counts: a byte, 0xff escaping to a u16
// from DefineShape2 on.
uint32_t styleCount(BitReader& in, int version)
{
	uint32_t count = in.u8();
	if (count == 0xff && version >= 2) {
		count = in.u16();
	}
	return count;
}

bool readFillStyle(BitReader& in, int version, FillStyle& f)
{
	f.rawType = in.u8();
	bool known = false;
	f.type = typeOf(f.rawType, known);
	if (!known) {
		return false;
	}
	switch (f.type) {
	case FillStyle::Type::Solid:
		f.color = version >= 3 ? in.rgba() : in.rgb();
		break;
	case FillStyle::Type::LinearGradient:
	case FillStyle::Type::RadialGradient:
	case FillStyle::Type::FocalGradient: {
		f.matrix = in.matrix();
		const uint8_t header = in.u8();
		f.spreadMode = header >> 6;
		f.interpolationMode = (header >> 4) & 3;
		const int count = header & 15;
		for (int i = 0; i < count; ++i) {
			GradientStop stop;
			stop.ratio = in.u8();
			stop.color = version >= 3 ? in.rgba() : in.rgb();
			f.stops.push_back(stop);
		}
		if (f.type == FillStyle::Type::FocalGradient) {
			f.focalPoint = in.f8();
		}
		break;
	}
	case FillStyle::Type::Bitmap:
		f.bitmapId = in.u16();
		f.matrix = in.matrix();
		f.repeat = (f.rawType & 1) == 0;
		f.smooth = (f.rawType & 2) == 0;
		break;
	}
	return in.ok();
}

bool readStyles(BitReader& in, int version, Shape& shape)
{
	const uint32_t fills = styleCount(in, version);
	for (uint32_t i = 0; i < fills; ++i) {
		FillStyle f;
		if (!readFillStyle(in, version, f)) {
			return false;
		}
		shape.fillStyles.push_back(std::move(f));
	}
	const uint32_t lines = styleCount(in, version);
	for (uint32_t i = 0; i < lines; ++i) {
		LineStyle l;
		l.width = in.u16();
		l.color = version >= 3 ? in.rgba() : in.rgb();
		shape.lineStyles.push_back(l);
	}
	return in.ok();
}

Point toPoint(int32_t x, int32_t y)
{
	return {static_cast<float>(x), static_cast<float>(y)};
}

// Reads SHAPERECORDs up to the end record. With `styles` set, new-style
// records append to it (and need `version` >= 2); `paths` gets the edges
// grouped by style, `edges` (if set) every edge with its start point.
bool readRecords(BitReader& in, int version, Shape* styles, std::vector<ShapePath>& paths, std::vector<MorphEdge>* edges,
	uint32_t& layerCount)
{
	in.align();
	int fillBits = static_cast<int>(in.ub(4));
	int lineBits = static_cast<int>(in.ub(4));
	// The current style group: its first global index minus one, and size.
	uint32_t fillBase = 0, lineBase = 0;
	uint32_t fillCount = styles ? static_cast<uint32_t>(styles->fillStyles.size()) : 0xffffffffu;
	uint32_t lineCount = styles ? static_cast<uint32_t>(styles->lineStyles.size()) : 0xffffffffu;
	int32_t x = 0, y = 0;
	ShapePath current;
	uint32_t layer = 0;
	const auto flush = [&]() {
		if (!current.edges.empty()) {
			paths.push_back(current);
		}
		current.edges.clear();
		current.start = toPoint(x, y);
	};
	const auto globalIndex = [](uint32_t local, uint32_t base, uint32_t count) -> uint32_t {
		return local == 0 || local > count ? 0 : base + local;
	};

	while (in.ok()) {
		if (in.ub(1) == 0) {
			const uint32_t flags = in.ub(5);
			if (flags == 0) {
				flush();
				layerCount = layer + 1;
				return in.ok();
			}
			const bool newStyles = flags & 0x10;
			const bool hasLine = flags & 0x08;
			const bool hasFill1 = flags & 0x04;
			const bool hasFill0 = flags & 0x02;
			const bool hasMove = flags & 0x01;
			flush();
			if (hasMove) {
				const int n = static_cast<int>(in.ub(5));
				x = in.sb(n);
				y = in.sb(n);
			}
			const uint32_t fill0 = hasFill0 ? in.ub(fillBits) : 0;
			const uint32_t fill1 = hasFill1 ? in.ub(fillBits) : 0;
			const uint32_t line = hasLine ? in.ub(lineBits) : 0;
			if (newStyles) {
				if (!styles || version < 2) {
					return false;
				}
				fillBase = static_cast<uint32_t>(styles->fillStyles.size());
				lineBase = static_cast<uint32_t>(styles->lineStyles.size());
				if (!readStyles(in, version, *styles)) {
					return false;
				}
				fillCount = static_cast<uint32_t>(styles->fillStyles.size()) - fillBase;
				lineCount = static_cast<uint32_t>(styles->lineStyles.size()) - lineBase;
				fillBits = static_cast<int>(in.ub(4));
				lineBits = static_cast<int>(in.ub(4));
				current.fill0 = current.fill1 = current.line = 0;
				++layer;
			}
			if (hasFill0) {
				current.fill0 = globalIndex(fill0, fillBase, fillCount);
			}
			if (hasFill1) {
				current.fill1 = globalIndex(fill1, fillBase, fillCount);
			}
			if (hasLine) {
				current.line = globalIndex(line, lineBase, lineCount);
			}
			current.layer = layer;
			current.start = toPoint(x, y);
			continue;
		}
		const Point from = toPoint(x, y);
		ShapeEdge edge;
		if (in.ub(1)) { // straight
			const int n = static_cast<int>(in.ub(4)) + 2;
			if (in.ub(1)) { // general line
				x += in.sb(n);
				y += in.sb(n);
			} else if (in.ub(1)) { // vertical
				y += in.sb(n);
			} else {
				x += in.sb(n);
			}
			edge.anchor = toPoint(x, y);
			edge.control = {(from.x + edge.anchor.x) * 0.5f, (from.y + edge.anchor.y) * 0.5f};
		} else {
			const int n = static_cast<int>(in.ub(4)) + 2;
			const int32_t cx = x + in.sb(n);
			const int32_t cy = y + in.sb(n);
			x = cx + in.sb(n);
			y = cy + in.sb(n);
			edge.control = toPoint(cx, cy);
			edge.anchor = toPoint(x, y);
			edge.curved = true;
		}
		current.edges.push_back(edge);
		if (edges) {
			edges->push_back({from, edge});
		}
	}
	return false;
}

float lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

uint8_t lerp8(uint8_t a, uint8_t b, float t)
{
	const float v = lerp(a, b, t) + 0.5f;
	return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
}

Rgba lerpColor(const Rgba& a, const Rgba& b, float t)
{
	return {lerp8(a.r, b.r, t), lerp8(a.g, b.g, t), lerp8(a.b, b.b, t), lerp8(a.a, b.a, t)};
}

Matrix lerpMatrix(const Matrix& m, const Matrix& n, float t)
{
	Matrix r;
	r.a = lerp(m.a, n.a, t);
	r.b = lerp(m.b, n.b, t);
	r.c = lerp(m.c, n.c, t);
	r.d = lerp(m.d, n.d, t);
	r.tx = lerp(m.tx, n.tx, t);
	r.ty = lerp(m.ty, n.ty, t);
	return r;
}

Point lerpPoint(Point p, Point q, float t)
{
	return {lerp(p.x, q.x, t), lerp(p.y, q.y, t)};
}

} // namespace

bool parseDefineShape(const uint8_t* body, size_t size, uint16_t tagCode, Shape& out, std::string* error)
{
	const int version = tagCode == 2 ? 1 : tagCode == 22 ? 2 : tagCode == 32 ? 3 : 0;
	if (version == 0) {
		setError(error, "not a DefineShape tag");
		return false;
	}
	BitReader in(body, size);
	out = Shape {};
	out.id = in.u16();
	out.bounds = in.rect();
	if (!readStyles(in, version, out)) {
		setError(error, "bad shape styles");
		return false;
	}
	if (!readRecords(in, version, &out, out.paths, nullptr, out.layerCount)) {
		setError(error, "bad shape records");
		return false;
	}
	return true;
}

bool parseGlyphShape(BitReader& in, Shape& out)
{
	out.paths.clear();
	// Glyphs index fill style 1 without declaring it; pretend it exists.
	Shape styles;
	styles.fillStyles.resize(1);
	return readRecords(in, 1, &styles, out.paths, nullptr, out.layerCount);
}

bool parseDefineMorphShape(const uint8_t* body, size_t size, MorphShape& out, std::string* error)
{
	BitReader in(body, size);
	out = MorphShape {};
	out.id = in.u16();
	out.startBounds = in.rect();
	out.endBounds = in.rect();
	const uint32_t endOffset = in.u32(); // from here to the end edges
	const size_t endEdgesAt = in.position() + endOffset;
	const uint32_t fills = styleCount(in, 2);
	for (uint32_t i = 0; i < fills && in.ok(); ++i) {
		MorphFillStyle f;
		f.rawType = in.u8();
		bool known = false;
		f.type = typeOf(f.rawType, known);
		if (!known || f.type == FillStyle::Type::FocalGradient) {
			setError(error, "bad morph fill style");
			return false;
		}
		if (f.type == FillStyle::Type::Solid) {
			f.startColor = in.rgba();
			f.endColor = in.rgba();
		} else if (f.type == FillStyle::Type::Bitmap) {
			f.bitmapId = in.u16();
			f.startMatrix = in.matrix();
			f.endMatrix = in.matrix();
			f.repeat = (f.rawType & 1) == 0;
			f.smooth = (f.rawType & 2) == 0;
		} else {
			f.startMatrix = in.matrix();
			f.endMatrix = in.matrix();
			const int count = in.u8() & 15;
			for (int s = 0; s < count; ++s) {
				MorphGradientStop stop;
				stop.startRatio = in.u8();
				stop.startColor = in.rgba();
				stop.endRatio = in.u8();
				stop.endColor = in.rgba();
				f.stops.push_back(stop);
			}
		}
		out.fillStyles.push_back(std::move(f));
	}
	const uint32_t lines = styleCount(in, 2);
	for (uint32_t i = 0; i < lines && in.ok(); ++i) {
		MorphLineStyle l;
		l.startWidth = in.u16();
		l.endWidth = in.u16();
		l.startColor = in.rgba();
		l.endColor = in.rgba();
		out.lineStyles.push_back(l);
	}
	if (!in.ok()) {
		setError(error, "truncated morph styles");
		return false;
	}
	// Index checks use the morph style counts.
	Shape styles;
	styles.fillStyles.resize(out.fillStyles.size());
	styles.lineStyles.resize(out.lineStyles.size());
	uint32_t layers = 1;
	if (!readRecords(in, 3, &styles, out.startPaths, nullptr, layers)) {
		setError(error, "bad morph start edges");
		return false;
	}
	if (endEdgesAt > size) {
		setError(error, "morph end edges past the tag");
		return false;
	}
	in.seek(endEdgesAt);
	std::vector<ShapePath> endPaths;
	if (!readRecords(in, 3, &styles, endPaths, &out.endEdges, layers)) {
		setError(error, "bad morph end edges");
		return false;
	}
	size_t startEdges = 0;
	for (const ShapePath& p : out.startPaths) {
		startEdges += p.edges.size();
	}
	if (startEdges != out.endEdges.size()) {
		setError(error, "morph start and end edge counts differ");
		return false;
	}
	return true;
}

Shape interpolateMorphShape(const MorphShape& morph, float ratio)
{
	const float t = std::clamp(ratio, 0.0f, 1.0f);
	Shape shape;
	shape.id = morph.id;
	shape.bounds.xMin = static_cast<int32_t>(lerp(float(morph.startBounds.xMin), float(morph.endBounds.xMin), t));
	shape.bounds.xMax = static_cast<int32_t>(lerp(float(morph.startBounds.xMax), float(morph.endBounds.xMax), t));
	shape.bounds.yMin = static_cast<int32_t>(lerp(float(morph.startBounds.yMin), float(morph.endBounds.yMin), t));
	shape.bounds.yMax = static_cast<int32_t>(lerp(float(morph.startBounds.yMax), float(morph.endBounds.yMax), t));
	for (const MorphFillStyle& m : morph.fillStyles) {
		FillStyle f;
		f.type = m.type;
		f.rawType = m.rawType;
		f.color = lerpColor(m.startColor, m.endColor, t);
		f.matrix = lerpMatrix(m.startMatrix, m.endMatrix, t);
		for (const MorphGradientStop& s : m.stops) {
			f.stops.push_back({lerp8(s.startRatio, s.endRatio, t), lerpColor(s.startColor, s.endColor, t)});
		}
		f.bitmapId = m.bitmapId;
		f.repeat = m.repeat;
		f.smooth = m.smooth;
		shape.fillStyles.push_back(std::move(f));
	}
	for (const MorphLineStyle& m : morph.lineStyles) {
		LineStyle l;
		l.width = static_cast<uint16_t>(lerp(m.startWidth, m.endWidth, t) + 0.5f);
		l.color = lerpColor(m.startColor, m.endColor, t);
		shape.lineStyles.push_back(l);
	}
	size_t next = 0;
	for (const ShapePath& start : morph.startPaths) {
		ShapePath path;
		path.fill0 = start.fill0;
		path.fill1 = start.fill1;
		path.line = start.line;
		Point from = start.start;
		for (size_t i = 0; i < start.edges.size(); ++i, ++next) {
			const ShapeEdge& s = start.edges[i];
			const MorphEdge& e = morph.endEdges[next];
			const Point at = lerpPoint(from, e.from, t);
			// The end outline may lift the pen where the start does not.
			if (path.edges.empty()) {
				path.start = at;
			} else {
				const Point last = path.edges.back().anchor;
				if (last.x != at.x || last.y != at.y) {
					shape.paths.push_back(path);
					path.edges.clear();
					path.start = at;
				}
			}
			ShapeEdge edge;
			edge.curved = s.curved || e.edge.curved;
			edge.control = lerpPoint(s.control, e.edge.control, t);
			edge.anchor = lerpPoint(s.anchor, e.edge.anchor, t);
			path.edges.push_back(edge);
			from = s.anchor;
		}
		if (!path.edges.empty()) {
			shape.paths.push_back(std::move(path));
		}
	}
	return shape;
}

} // namespace dice::hfe::ui::swf
