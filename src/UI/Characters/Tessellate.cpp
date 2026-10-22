#include "Tessellate.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace dice::hfe::ui::swf {

namespace {

constexpr double kPi = 3.14159265358979323846;

// A boundary edge with y0 < y1; `winding` is +1 when the region lies on the
// left of the original direction of travel, -1 otherwise, flipped when the
// edge was stored upside down.
struct Segment {
	double x0, y0, x1, y1;
	int winding;

	double xAt(double y) const
	{
		if (y <= y0) {
			return x0;
		}
		if (y >= y1) {
			return x1;
		}
		return x0 + (x1 - x0) * (y - y0) / (y1 - y0);
	}
};

struct BandEdge {
	double xa, xb;
	int winding;
};

void addSegment(std::vector<Segment>& out, Point a, Point b, int winding)
{
	if (a.y == b.y) {
		return; // horizontal edges bound no band
	}
	if (a.y < b.y) {
		out.push_back({a.x, a.y, b.x, b.y, winding});
	} else {
		out.push_back({b.x, b.y, a.x, a.y, -winding});
	}
}

void pushTriangle(std::vector<Point>& out, double ax, double ay, double bx, double by, double cx, double cy)
{
	const double area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
	if (std::fabs(area) < 1e-9) {
		return;
	}
	out.push_back({static_cast<float>(ax), static_cast<float>(ay)});
	out.push_back({static_cast<float>(bx), static_cast<float>(by)});
	out.push_back({static_cast<float>(cx), static_cast<float>(cy)});
}

// Fills the band between ya and yb, where no two edges cross.
void emitBand(const std::vector<BandEdge>& edges, double ya, double yb, bool nonZero, std::vector<Point>& out)
{
	int winding = 0;
	for (size_t i = 0; i + 1 < edges.size(); ++i) {
		winding = nonZero ? winding + edges[i].winding : winding ^ 1;
		if (winding == 0) {
			continue;
		}
		const BandEdge& l = edges[i];
		const BandEdge& r = edges[i + 1];
		pushTriangle(out, l.xa, ya, r.xa, ya, r.xb, yb);
		pushTriangle(out, l.xa, ya, r.xb, yb, l.xb, yb);
	}
}

void fillBand(const std::vector<const Segment*>& active, double ya, double yb, bool nonZero, std::vector<Point>& out)
{
	std::vector<BandEdge> edges(active.size());
	// Bounded so float noise cannot keep splitting forever.
	for (int guard = 0; guard < 4096; ++guard) {
		for (size_t i = 0; i < active.size(); ++i) {
			edges[i] = {active[i]->xAt(ya), active[i]->xAt(yb), active[i]->winding};
		}
		std::sort(edges.begin(), edges.end(), [](const BandEdge& a, const BandEdge& b) {
			const double ma = a.xa + a.xb, mb = b.xa + b.xb;
			return ma != mb ? ma < mb : a.xa < b.xa;
		});
		// Sorted by midpoint, any crossing shows as an adjacent pair out of
		// order at one end; cut the band at the lowest such crossing.
		const double eps = 1e-6 * std::max(1.0, yb - ya);
		double cut = yb;
		for (size_t i = 0; i + 1 < edges.size(); ++i) {
			const double da = edges[i].xa - edges[i + 1].xa;
			const double db = edges[i].xb - edges[i + 1].xb;
			if ((da > 0 && db < 0) || (da < 0 && db > 0)) {
				const double y = ya + (yb - ya) * da / (da - db);
				if (y > ya + eps && y < yb - eps) {
					cut = std::min(cut, y);
				}
			}
		}
		if (cut >= yb) {
			emitBand(edges, ya, yb, nonZero, out);
			return;
		}
		for (size_t i = 0; i < active.size(); ++i) {
			edges[i] = {active[i]->xAt(ya), active[i]->xAt(cut), active[i]->winding};
		}
		std::sort(edges.begin(), edges.end(), [](const BandEdge& a, const BandEdge& b) {
			const double ma = a.xa + a.xb, mb = b.xa + b.xb;
			return ma != mb ? ma < mb : a.xa < b.xa;
		});
		emitBand(edges, ya, cut, nonZero, out);
		ya = cut;
	}
}

void fillSegments(std::vector<Segment>& segments, bool nonZero, std::vector<Point>& out)
{
	if (segments.size() < 2) {
		return;
	}
	std::vector<double> ys;
	ys.reserve(segments.size() * 2);
	for (const Segment& s : segments) {
		ys.push_back(s.y0);
		ys.push_back(s.y1);
	}
	std::sort(ys.begin(), ys.end());
	ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
	std::sort(segments.begin(), segments.end(), [](const Segment& a, const Segment& b) { return a.y0 < b.y0; });

	std::vector<const Segment*> active;
	size_t next = 0;
	for (size_t k = 0; k + 1 < ys.size(); ++k) {
		const double ya = ys[k], yb = ys[k + 1];
		active.erase(std::remove_if(active.begin(), active.end(), [ya](const Segment* s) { return s->y1 <= ya; }),
			active.end());
		while (next < segments.size() && segments[next].y0 <= ya) {
			active.push_back(&segments[next++]);
		}
		if (active.size() >= 2) {
			fillBand(active, ya, yb, nonZero, out);
		}
	}
}

// The path's outline as points, curves flattened, repeats dropped.
void flattenPath(const ShapePath& path, float tolerance, std::vector<Point>& out)
{
	out.clear();
	out.push_back(path.start);
	for (const ShapeEdge& e : path.edges) {
		if (e.curved) {
			flattenQuadratic(out.back(), e.control, e.anchor, tolerance, out);
		} else {
			out.push_back(e.anchor);
		}
	}
	out.erase(std::unique(out.begin(), out.end(), [](const Point& a, const Point& b) { return a.x == b.x && a.y == b.y; }),
		out.end());
}

struct Vec {
	double x, y;
};

double arcStep(double radius, double tolerance)
{
	if (tolerance >= radius) {
		return kPi / 2;
	}
	return std::max(2.0 * std::acos(1.0 - tolerance / radius), kPi / 32);
}

// A fan around `c` from angle a0 sweeping `sweep` radians.
void pushArc(std::vector<Point>& out, Vec c, double radius, double a0, double sweep, double step)
{
	const int count = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / step)));
	double px = c.x + radius * std::cos(a0), py = c.y + radius * std::sin(a0);
	for (int i = 1; i <= count; ++i) {
		const double a = a0 + sweep * i / count;
		const double qx = c.x + radius * std::cos(a), qy = c.y + radius * std::sin(a);
		pushTriangle(out, c.x, c.y, px, py, qx, qy);
		px = qx;
		py = qy;
	}
}

void strokePolyline(const std::vector<Point>& pts, double width, double tolerance, std::vector<Point>& out)
{
	const double h = width / 2;
	const double step = arcStep(h, tolerance);
	if (pts.size() == 1) {
		pushArc(out, {pts[0].x, pts[0].y}, h, 0, 2 * kPi, step);
		return;
	}
	const size_t n = pts.size();
	const bool closed = n > 2 && pts.front().x == pts.back().x && pts.front().y == pts.back().y;
	std::vector<Vec> dirs(n - 1);
	for (size_t i = 0; i + 1 < n; ++i) {
		const double dx = pts[i + 1].x - pts[i].x, dy = pts[i + 1].y - pts[i].y;
		const double len = std::sqrt(dx * dx + dy * dy);
		dirs[i] = {dx / len, dy / len};
	}
	for (size_t i = 0; i + 1 < n; ++i) {
		const Vec d = dirs[i];
		const double nx = -d.y * h, ny = d.x * h;
		const Point a = pts[i], b = pts[i + 1];
		pushTriangle(out, a.x + nx, a.y + ny, a.x - nx, a.y - ny, b.x - nx, b.y - ny);
		pushTriangle(out, a.x + nx, a.y + ny, b.x - nx, b.y - ny, b.x + nx, b.y + ny);
	}
	const auto join = [&](const Point& p, Vec d0, Vec d1) {
		const double cross = d0.x * d1.y - d0.y * d1.x;
		const double dot = d0.x * d1.x + d0.y * d1.y;
		if (std::fabs(cross) < 1e-9 && dot > 0) {
			return;
		}
		// The gap opens on the side whose normal points away from d1.
		Vec n0 {-d0.y, d0.x}, n1 {-d1.y, d1.x};
		if (n0.x * d1.x + n0.y * d1.y > 0) {
			n0 = {-n0.x, -n0.y};
			n1 = {-n1.x, -n1.y};
		}
		const double a0 = std::atan2(n0.y, n0.x);
		double sweep = std::atan2(n1.y, n1.x) - a0;
		while (sweep > kPi) {
			sweep -= 2 * kPi;
		}
		while (sweep < -kPi) {
			sweep += 2 * kPi;
		}
		pushArc(out, {p.x, p.y}, h, a0, sweep, step);
	};
	for (size_t i = 1; i + 1 < n; ++i) {
		join(pts[i], dirs[i - 1], dirs[i]);
	}
	if (closed) {
		join(pts[0], dirs[n - 2], dirs[0]);
		return;
	}
	// Round caps: half circles from the normal round the back (start) or
	// the front (end).
	const Vec d0 = dirs.front(), d1 = dirs.back();
	pushArc(out, {pts.front().x, pts.front().y}, h, std::atan2(d0.x, -d0.y), kPi, step);
	pushArc(out, {pts.back().x, pts.back().y}, h, std::atan2(d1.x, -d1.y), -kPi, step);
}

} // namespace

size_t ShapeMesh::triangleCount() const
{
	size_t count = 0;
	for (const MeshBatch& b : batches) {
		count += b.triangles.size() / 3;
	}
	return count;
}

void flattenQuadratic(Point p0, Point p1, Point p2, float tolerance, std::vector<Point>& out)
{
	// A chord over parameter step h strays at most |p0 - 2p1 + p2| h^2 / 4.
	const double dx = p0.x - 2.0 * p1.x + p2.x, dy = p0.y - 2.0 * p1.y + p2.y;
	const double dd = std::sqrt(dx * dx + dy * dy);
	const double tol = std::max(1e-3, static_cast<double>(tolerance));
	const int count = std::clamp(static_cast<int>(std::ceil(std::sqrt(dd / (4.0 * tol)))), 1, 1024);
	for (int i = 1; i < count; ++i) {
		const double t = static_cast<double>(i) / count, u = 1.0 - t;
		out.push_back({static_cast<float>(u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x),
			static_cast<float>(u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y)});
	}
	out.push_back(p2);
}

double triangleArea(const std::vector<Point>& triangles)
{
	double area = 0;
	for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
		const Point& a = triangles[i];
		const Point& b = triangles[i + 1];
		const Point& c = triangles[i + 2];
		area += std::fabs((double(b.x) - a.x) * (double(c.y) - a.y) - (double(c.x) - a.x) * (double(b.y) - a.y)) / 2;
	}
	return area;
}

ShapeMesh tessellateShape(const Shape& shape, const TessellateOptions& options)
{
	// (layer, fill = 0 / stroke = 1, style) in draw order.
	std::map<std::tuple<uint32_t, int, uint32_t>, MeshBatch> batches;
	std::map<std::pair<uint32_t, uint32_t>, std::vector<Segment>> fills; // (layer, style)
	std::vector<Point> pts;
	const uint32_t fillCount = static_cast<uint32_t>(shape.fillStyles.size());
	const uint32_t lineCount = static_cast<uint32_t>(shape.lineStyles.size());

	for (const ShapePath& path : shape.paths) {
		const bool hasFill = path.fill0 != path.fill1;
		const bool hasLine = path.line != 0 && path.line <= lineCount;
		if (!hasFill && !hasLine) {
			continue;
		}
		flattenPath(path, options.curveTolerance, pts);
		if (hasFill) {
			// fill0 lies on the left of travel, fill1 on the right.
			for (const auto& [style, winding] : {std::pair {path.fill0, 1}, std::pair {path.fill1, -1}}) {
				if (style == 0 || style > fillCount) {
					continue;
				}
				std::vector<Segment>& segs = fills[{path.layer, style - 1}];
				for (size_t i = 0; i + 1 < pts.size(); ++i) {
					addSegment(segs, pts[i], pts[i + 1], winding);
				}
			}
		}
		if (hasLine) {
			const LineStyle& line = shape.lineStyles[path.line - 1];
			MeshBatch& batch = batches[{path.layer, 1, path.line - 1}];
			batch.kind = MeshBatch::Kind::Stroke;
			batch.style = path.line - 1;
			batch.layer = path.layer;
			batch.hairline = line.width < options.minStrokeWidth;
			const double width = std::max<double>(line.width, options.minStrokeWidth);
			strokePolyline(pts, width, options.curveTolerance, batch.triangles);
		}
	}
	for (auto& [key, segs] : fills) {
		MeshBatch& batch = batches[{key.first, 0, key.second}];
		batch.kind = MeshBatch::Kind::Fill;
		batch.style = key.second;
		batch.layer = key.first;
		fillSegments(segs, options.nonZero, batch.triangles);
	}
	ShapeMesh mesh;
	for (auto& [key, batch] : batches) {
		if (!batch.triangles.empty()) {
			mesh.batches.push_back(std::move(batch));
		}
	}
	return mesh;
}

} // namespace dice::hfe::ui::swf
