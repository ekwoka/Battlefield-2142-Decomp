// How RendDX9's Overgrowth object (DAT_1020ded8, vtable 0x101d3790) picks
// and fades a tree's full and "_lod" meshes (no GPU code: shared with the
// tests).
//
// The trees sit in square cells (the Growth base 0x10122130: a grid of
// +0x10 = 10 x 10 cells about the camera) of viewDistance / (10 / 2 - 1)
// metres (0x10120420), fitted so a whole number of them spans the terrain
// (0x1008f510: size = W / floor(W / size), W the smaller of the terrain's
// sides +0x5c / +0x60), their corners on multiples of the size. Per frame:
//
// - the update (slot 7, 0x1008b7a0) makes C = zoom x viewDistance (+0x24) x
//   closeDistPercentage (+0xc8) x the geometry quality's factor (+0x110);
//   each tree's near radius is R = its mesh radius x its scale + C x its
//   type's factor (+0x9c) x viewDistanceScale (+0x28). The type's factor
//   is clamp((the type geometry's bounding radius / 5)^2, 0.3, 1)
//   (0x100917e0). A cell's batch of "_lod" meshes holds its trees outside
//   R (0x1008e790, rebuilt when that changes).
// - the base cell update (0x10120ef0) keeps a cell while the nearest of its
//   four corners (at the terrain's height at its centre, 0x101205d0) is
//   within viewDistance, at alpha 1 up to 70% of it and down to 0 at it
//   (+0x20 = 0.3), and keeps the 3D distance to its centre (+0x1c).
// - the cell draw (0x10121410, through the near pass's +0xd5) multiplies a
//   drawn cell's alpha by clamp(centre distance / 12 (+0x34), f, 1), f =
//   clamp(|camera height - the ground's under it| / 1 (+0x2c), 0.4 (+0x30),
//   1): standing on the ground, the cell about the camera fades to 0.4.
// - the near pass (slot 13, 0x1008da30) draws each tree within R of a cell
//   within (closeDistFade + 1) x C of the camera: with q = d^2 / R^2,
//   w = clamp(sqrt(type factor) x (1 - sqrt q) / closeDistFade, 0, 1); with
//   lodTimer (+0x114) over 0 the tree's own state t (+0x5c) eases instead:
//   t = 0 at w = 1, else up by dt / lodTimer at least to 1 - 2w while
//   w <= 0.5, down by it at most to 2 - 2w above, and w = 1 - t. The
//   "_lod" mesh is drawn at alpha clamp(2(1 - w), 0, 1) while w < 1, the
//   full one at clamp(2w, 0, 1) while w > 0 (0x10089ee0: the mesh's alpha;
//   both doubled while the renderer's 0x1000b500 / +0x50a hold, which the
//   rewrite doesn't have). The cells' fades don't reach these.
#pragma once

#include <algorithm>
#include <cmath>

namespace dice::hfe::render {

struct OvergrowthCells {
	// 0x10120420 then 0x1008f510.
	static float cellSize(float viewDistance, float terrainSide, int cells = 10)
	{
		float size = viewDistance / static_cast<float>(cells / 2 - 1);
		if (terrainSide > 0 && size > 0) {
			const float count = std::floor(terrainSide / size);
			if (count > 0) {
				size = terrainSide / count;
			}
		}
		return size;
	}
	// The base cell update's fade by the nearest corner (0x10120ef0, +0x20).
	// Below 0: the cell isn't kept.
	static float distanceFade(float nearestCorner, float viewDistance, float fadeShare = 0.3f)
	{
		if (viewDistance <= 0) {
			return 1;
		}
		const float share = nearestCorner / viewDistance;
		if (share >= 1) {
			return -1;
		}
		if (fadeShare == 0 || share <= 1 - fadeShare) {
			return 1;
		}
		return std::clamp((1 - share) / fadeShare, 0.0f, 1.0f);
	}
	// The cell draw's near fade (0x10121410: +0x2c 1, +0x30 0.4, +0x34 12).
	static float nearFade(float centreDistance, float cameraAboveGround)
	{
		const float floor = std::clamp(std::fabs(cameraAboveGround) / 1.0f, 0.4f, 1.0f);
		return std::clamp(centreDistance / 12.0f, floor, 1.0f);
	}
};

struct OvergrowthBlend {
	// The type's factor (+0x9c, 0x100917e0).
	static float typeFactor(float geometryRadius)
	{
		const float r = geometryRadius / 5.0f;
		return std::clamp(r * r, 0.3f, 1.0f);
	}
	// w for a tree d from the camera within R (0x1008da30).
	static float weight(float distance, float nearRadius, float typeFactor, float closeDistFade)
	{
		if (nearRadius <= 0) {
			return 0;
		}
		const float q = std::min(distance / nearRadius, 1.0f);
		const float fade = closeDistFade > 0 ? closeDistFade : 1e-6f;
		return std::clamp(std::sqrt(typeFactor) * (1 - q) / fade, 0.0f, 1.0f);
	}
	// lodTimer's easing of the tree's state t (+0x5c); returns the w used.
	static float eased(float w, float& t, float seconds, float lodTimer)
	{
		if (lodTimer <= 0) {
			return w;
		}
		if (w >= 1) {
			t = 0;
		} else {
			const float rate = 1 / lodTimer;
			if (w <= 0.5f) {
				t = std::min(1.0f, std::max(seconds * rate + t, 1 - w * 2));
			} else {
				t = std::max(0.0f, std::min(t - seconds * rate, 2 - w * 2));
			}
		}
		return 1 - t;
	}
	static float fullAlpha(float w) { return w > 0 ? std::clamp(2 * w, 0.0f, 1.0f) : 0.0f; }
	static float lodAlpha(float w) { return w < 1 ? std::clamp(2 * (1 - w), 0.0f, 1.0f) : 0.0f; }
};

} // namespace dice::hfe::render
