// View-frustum culling and the meshes' level of detail by distance (no GPU
// code: shared with the tests).
//
// The renderers cull their drawables against the camera's frustum and pick
// a geometry's LOD by the camera's distance: GeometryTemplate.
// setSubGeometryLodDistance <geometry> <lod> <distance> (RendDX9 0x1007303a)
// sets how far LOD <lod> is drawn, scaled by renderer.global{Static,Bundle,
// Skinned}MeshLodDistanceScale. Spheres come from the vertices the rewrite
// uploads; meshes whose template sets no distances use the defaults below
// (the rewrite's own choice, not the original's).
#pragma once

#include "RenderMath.h"

#include <cmath>
#include <vector>

namespace dice::hfe::render {

struct BoundingSphere {
	Vec3f center;
	float radius = -1; // below 0: unknown (never culled)
};

// The six planes of a view-projection matrix (row vectors, clip depth
// 0..1, as RenderMath): a point p is inside when dot(n, p) + d >= 0 for
// all of them.
struct Frustum {
	float planes[6][4] {};

	static Frustum fromViewProjection(const Mat4& m)
	{
		Frustum f;
		const auto column = [&m](int j, float out[4]) {
			for (int i = 0; i < 4; ++i) {
				out[i] = m.at(i, j);
			}
		};
		float c0[4], c1[4], c2[4], c3[4];
		column(0, c0);
		column(1, c1);
		column(2, c2);
		column(3, c3);
		for (int i = 0; i < 4; ++i) {
			f.planes[0][i] = c3[i] + c0[i]; // left
			f.planes[1][i] = c3[i] - c0[i]; // right
			f.planes[2][i] = c3[i] + c1[i]; // bottom
			f.planes[3][i] = c3[i] - c1[i]; // top
			f.planes[4][i] = c2[i];         // near (z >= 0)
			f.planes[5][i] = c3[i] - c2[i]; // far
		}
		for (auto& plane : f.planes) {
			const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
			if (length > 0) {
				for (float& v : plane) {
					v /= length;
				}
			}
		}
		return f;
	}

	bool containsSphere(const Vec3f& center, float radius) const
	{
		if (radius < 0) {
			return true;
		}
		for (const auto& plane : planes) {
			if (plane[0] * center.x + plane[1] * center.y + plane[2] * center.z + plane[3] < -radius) {
				return false;
			}
		}
		return true;
	}

	// An axis-aligned box: outside when all its corners are behind a plane.
	bool containsBox(const Vec3f& lo, const Vec3f& hi) const
	{
		for (const auto& plane : planes) {
			// The corner farthest along the plane's normal.
			const float x = plane[0] >= 0 ? hi.x : lo.x;
			const float y = plane[1] >= 0 ? hi.y : lo.y;
			const float z = plane[2] >= 0 ? hi.z : lo.z;
			if (plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < 0) {
				return false;
			}
		}
		return true;
	}
};

// The smallest sphere about the points' box centre that holds them.
inline BoundingSphere sphereAround(const std::vector<Vec3f>& points)
{
	BoundingSphere sphere;
	if (points.empty()) {
		return sphere;
	}
	Vec3f lo = points[0], hi = points[0];
	for (const Vec3f& p : points) {
		lo = {std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
		hi = {std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
	}
	sphere.center = (lo + hi) * 0.5f;
	float radius = 0;
	for (const Vec3f& p : points) {
		const Vec3f d = p - sphere.center;
		radius = std::fmax(radius, dot(d, d));
	}
	sphere.radius = std::sqrt(radius);
	return sphere;
}

// A sphere moved by a world matrix (row vectors): the centre transformed,
// the radius scaled by the matrix's largest axis.
inline BoundingSphere transformSphere(const BoundingSphere& s, const Mat4& world)
{
	if (s.radius < 0) {
		return s;
	}
	BoundingSphere out;
	const Vec3f& c = s.center;
	out.center = {c.x * world.at(0, 0) + c.y * world.at(1, 0) + c.z * world.at(2, 0) + world.at(3, 0),
		c.x * world.at(0, 1) + c.y * world.at(1, 1) + c.z * world.at(2, 1) + world.at(3, 1),
		c.x * world.at(0, 2) + c.y * world.at(1, 2) + c.z * world.at(2, 2) + world.at(3, 2)};
	float scale = 0;
	for (int row = 0; row < 3; ++row) {
		const float l = world.at(row, 0) * world.at(row, 0) + world.at(row, 1) * world.at(row, 1) +
			world.at(row, 2) * world.at(row, 2);
		scale = std::fmax(scale, l);
	}
	out.radius = s.radius * std::sqrt(scale);
	return out;
}

// The LOD to draw a mesh with at `distance` from the camera: the first
// whose switch distance is beyond it. `switchDistances[i]` is where LOD i
// gives way to LOD i + 1; `scale` the renderer's LOD distance scale (larger
// keeps detail farther, 0 always LOD 0). LODs past the list keep the last.
inline int lodForDistance(float distance, const std::vector<float>& switchDistances, int lodCount, float scale = 1)
{
	if (lodCount <= 1 || scale <= 0) {
		return 0;
	}
	int lod = 0;
	while (lod + 1 < lodCount && lod < static_cast<int>(switchDistances.size()) &&
		distance > switchDistances[static_cast<size_t>(lod)] * scale) {
		++lod;
	}
	return lod;
}

// Switch distances for a mesh without its own (the rewrite's choice): LOD 1
// from 30 radii, each further LOD at twice the distance.
inline std::vector<float> defaultLodDistances(float radius, int lodCount)
{
	std::vector<float> distances;
	float d = std::fmax(radius, 1.0f) * 30.0f;
	for (int i = 0; i + 1 < lodCount; ++i) {
		distances.push_back(d);
		d *= 2;
	}
	return distances;
}

} // namespace dice::hfe::render
