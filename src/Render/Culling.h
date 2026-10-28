// View-frustum culling and the meshes' level of detail by distance (no GPU
// code: shared with the tests).
//
// The renderers cull their drawables against the camera's frustum and pick
// a geometry's LOD by the camera's distance: GeometryTemplate.
// setSubGeometryLodDistance <geometry> <lod> <distance> (RendDX9 0x1007303a)
// sets how far LOD <lod> is drawn, scaled by renderer.global{Static,Bundle,
// Skinned}MeshLodDistanceScale. Spheres come from the vertices the rewrite
// uploads; distances a template leaves unset are RendDX9's defaults
// (lodSwitchDistances).
#pragma once

#include "RenderMath.h"

#include <algorithm>
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

// The LOD to draw a mesh with at `distance` (lodDistance): one past the
// last switch distance that, times `scale`, is below it (RendDX9 walks the
// list from its end, 0x100bdb00). `switchDistances[i]` is where LOD i gives
// way to LOD i + 1; LODs past the list keep the last.
inline int lodForDistance(float distance, const std::vector<float>& switchDistances, int lodCount, float scale = 1)
{
	const int entries = std::min(static_cast<int>(switchDistances.size()), lodCount - 1);
	for (int i = entries - 1; i >= 0; --i) {
		if (switchDistances[static_cast<size_t>(i)] * scale < distance) {
			return i + 1;
		}
	}
	return 0;
}

// A geometry's switch distances as RendDX9 fills them when the mesh loads
// (static meshes 0x100d4d30, bundled and skinned 0x100a31d0): entry i is
// the template's setSubGeometryLodDistance for LOD i if it set one, else a
// running distance that starts at 50 m and grows by 50 m for each entry
// left to it and by the set distance for each set one (the editor's writer
// 0x100d3320 leaves out exactly the values 50, 100, 150 ...).
inline std::vector<float> lodSwitchDistances(const std::vector<float>& set, int lodCount)
{
	std::vector<float> distances;
	float running = 50.0f;
	for (int i = 0; i + 1 < lodCount; ++i) {
		if (i < static_cast<int>(set.size())) {
			distances.push_back(set[static_cast<size_t>(i)]);
			running += set[static_cast<size_t>(i)];
		} else {
			distances.push_back(running);
			running += 50.0f;
		}
	}
	return distances;
}

// The distance a mesh's LOD is picked by (0x100bdb00 static, 0x100c3390
// bundled, 0x101616c0 skinned): the camera's distance over the view's zoom
// (RenderView slot 0x30, tan(first fov / 2) / tan(fov / 2) inverted) less
// the mesh's radius. Its LOD is the last whose switch distance times
// renderer.global*MeshLodDistanceScale times the geometry quality
// (renderer +0x974: 0.5 / 0.8 / 1 by the video setting, 1 here) is below it.
inline float lodDistance(float cameraDistance, float zoom, float radius)
{
	return cameraDistance / std::fmax(zoom, 1e-3f) - std::fmax(radius, 0.0f);
}

} // namespace dice::hfe::render
