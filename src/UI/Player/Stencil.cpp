#include "Stencil.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::ui {

namespace {

using Point = Stencil::Point;

constexpr float kEpsilon = 1e-4f; // square pixels: smaller pieces are slivers of cut edges

float cross(Point o, Point a, Point b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

float signedArea(const std::vector<Point>& p)
{
	float a = 0;
	for (size_t i = 0; i < p.size(); ++i) {
		const Point& u = p[i];
		const Point& v = p[(i + 1) % p.size()];
		a += u.x * v.y - v.x * u.y;
	}
	return a * 0.5f;
}

Point positionOf(const Point& p) { return p; }
Point positionOf(const MenuVertex& v) { return {v.x, v.y}; }

Point lerp(const Point& a, const Point& b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

MenuVertex lerp(const MenuVertex& a, const MenuVertex& b, float t)
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

// Keeps the part of `polygon` on the side of the line a -> b where
// side * cross >= 0 (Sutherland-Hodgman, one edge).
template <typename V>
void clipHalf(std::vector<V>& polygon, Point a, Point b, float side)
{
	if (polygon.empty()) {
		return;
	}
	std::vector<V> out;
	out.reserve(polygon.size() + 2);
	for (size_t i = 0; i < polygon.size(); ++i) {
		const V& p = polygon[i];
		const V& q = polygon[(i + 1) % polygon.size()];
		const float dp = side * cross(a, b, positionOf(p));
		const float dq = side * cross(a, b, positionOf(q));
		if (dp >= 0) {
			out.push_back(p);
		}
		if ((dp >= 0) != (dq >= 0)) {
			out.push_back(lerp(p, q, dp / (dp - dq)));
		}
	}
	polygon = std::move(out);
}

// `polygon` cut to the inside of the convex `convex`.
template <typename V>
void clipConvex(std::vector<V>& polygon, const std::vector<Point>& convex)
{
	const float side = signedArea(convex) >= 0 ? 1.0f : -1.0f;
	for (size_t i = 0; i < convex.size() && !polygon.empty(); ++i) {
		clipHalf(polygon, convex[i], convex[(i + 1) % convex.size()], side);
	}
}

// a \ b for convex a and b, as disjoint convex pieces: piece i lies outside
// edge i of b and inside its edges before i.
std::vector<std::vector<Point>> subtract(const std::vector<Point>& a, const std::vector<Point>& b)
{
	std::vector<std::vector<Point>> pieces;
	const float side = signedArea(b) >= 0 ? 1.0f : -1.0f;
	std::vector<Point> inside = a;
	for (size_t i = 0; i < b.size() && !inside.empty(); ++i) {
		const Point p = b[i], q = b[(i + 1) % b.size()];
		std::vector<Point> outside = inside;
		clipHalf(outside, p, q, -side);
		if (outside.size() >= 3 && std::fabs(signedArea(outside)) > kEpsilon) {
			pieces.push_back(std::move(outside));
		}
		clipHalf(inside, p, q, side);
	}
	return pieces;
}

struct Box {
	float x0, y0, x1, y1;
};

template <typename V>
Box boxOf(const V* v, size_t n)
{
	Box b {positionOf(v[0]).x, positionOf(v[0]).y, positionOf(v[0]).x, positionOf(v[0]).y};
	for (size_t i = 1; i < n; ++i) {
		const Point p = positionOf(v[i]);
		b.x0 = std::min(b.x0, p.x);
		b.y0 = std::min(b.y0, p.y);
		b.x1 = std::max(b.x1, p.x);
		b.y1 = std::max(b.y1, p.y);
	}
	return b;
}

bool overlap(const Box& a, const Box& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }

} // namespace

void Stencil::reset()
{
	m_mode = Mode::Off;
	m_level = 0;
	m_done = 0;
	m_pieces.clear();
}

void Stencil::beginSubmit(std::vector<MenuBatch>& out)
{
	flush(out);
	if (m_level == 0) {
		m_pieces.clear(); // Clear(D3DCLEAR_STENCIL, 0) at the first level
	}
	++m_level;
	m_mode = Mode::Submit;
}

void Stencil::endSubmit(std::vector<MenuBatch>& out)
{
	flush(out);
	m_mode = Mode::Test;
}

void Stencil::disable(std::vector<MenuBatch>& out)
{
	flush(out);
	m_mode = Mode::Off;
	--m_level;
}

// One triangle of a mask: where it covers value v the stencil becomes v + 1
// if the level is greater than v (INCR), else the level (REPLACE).
void Stencil::submitTriangle(const Point t[3])
{
	std::vector<Point> triangle(t, t + 3);
	if (std::fabs(signedArea(triangle)) <= kEpsilon) {
		return;
	}
	const Box tb = boxOf(t, 3);
	std::vector<Piece> next;
	next.reserve(m_pieces.size() + 4);
	std::vector<std::vector<Point>> uncovered {triangle};
	for (Piece& piece : m_pieces) {
		if (!overlap(tb, boxOf(piece.polygon.data(), piece.polygon.size()))) {
			next.push_back(std::move(piece));
			continue;
		}
		std::vector<Point> covered = piece.polygon;
		clipConvex(covered, triangle);
		if (covered.size() < 3 || std::fabs(signedArea(covered)) <= kEpsilon) {
			next.push_back(std::move(piece));
			continue;
		}
		for (std::vector<Point>& rest : subtract(piece.polygon, triangle)) {
			next.push_back({std::move(rest), piece.value});
		}
		const int value = m_level > piece.value ? piece.value + 1 : m_level;
		std::vector<std::vector<Point>> remaining;
		for (const std::vector<Point>& u : uncovered) {
			for (std::vector<Point>& r : subtract(u, piece.polygon)) {
				remaining.push_back(std::move(r));
			}
		}
		uncovered = std::move(remaining);
		next.push_back({std::move(covered), value});
	}
	for (std::vector<Point>& u : uncovered) {
		next.push_back({std::move(u), 1}); // 0 -> 1: the level is at least 1
	}
	m_pieces = std::move(next);
}

void Stencil::flush(std::vector<MenuBatch>& out)
{
	if (m_done > out.size()) {
		m_done = out.size();
	}
	if (m_mode == Mode::Submit) {
		// Colour writes off: the mask only marks the stencil.
		for (size_t b = m_done; b < out.size(); ++b) {
			const std::vector<MenuVertex>& v = out[b].vertices;
			for (size_t i = 0; i + 2 < v.size(); i += 3) {
				const Point t[3] = {{v[i].x, v[i].y}, {v[i + 1].x, v[i + 1].y}, {v[i + 2].x, v[i + 2].y}};
				submitTriangle(t);
			}
		}
		out.resize(m_done);
	} else if (m_mode == Mode::Test) {
		std::vector<const Piece*> shown;
		std::vector<Box> boxes;
		for (const Piece& piece : m_pieces) {
			if (piece.value == m_level) {
				shown.push_back(&piece);
				boxes.push_back(boxOf(piece.polygon.data(), piece.polygon.size()));
			}
		}
		for (size_t b = m_done; b < out.size(); ++b) {
			std::vector<MenuVertex>& vertices = out[b].vertices;
			std::vector<MenuVertex> kept;
			for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
				const MenuVertex* t = &vertices[i];
				const Box tb = boxOf(t, 3);
				for (size_t k = 0; k < shown.size(); ++k) {
					if (!overlap(tb, boxes[k])) {
						continue;
					}
					std::vector<MenuVertex> polygon(t, t + 3);
					clipConvex(polygon, shown[k]->polygon);
					for (size_t j = 1; j + 1 < polygon.size(); ++j) {
						kept.push_back(polygon[0]);
						kept.push_back(polygon[j]);
						kept.push_back(polygon[j + 1]);
					}
				}
			}
			vertices = std::move(kept);
		}
		out.erase(std::remove_if(out.begin() + static_cast<std::ptrdiff_t>(m_done), out.end(),
					  [](const MenuBatch& batch) { return batch.vertices.empty(); }),
			out.end());
	}
	m_done = out.size();
}

} // namespace dice::hfe::ui
