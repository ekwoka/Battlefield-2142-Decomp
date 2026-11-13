// The objects' distance fade: BF2142.exe's object manager fades each
// object's render object in and out by its distance from the camera as it
// walks the visible set (0x006ff110, the manager's slots 0x40 / 0x4c
// through 0x007003c0 / 0x007003f0), sized by the template's
// cullRadiusScale.
//
// The walk (0x006ff110; skipped, everything kept, while the walk's filter
// carries flag 0x1000000) builds a record per frame from the render view
// (the Renderer's slot 0x54, RendDX9 0x1000d0c0: the view at renderer
// +0xf8, constructed by 0x100671d0, vtable 0x101c6610):
//   p0 = S x m^2   S the view's zoom (slot 0x2c, 0x10066bf0: +0x1c, set by
//                  0x10066b30 to tan(first fov / 2) / tan(fov / 2)), m the
//                  manager's +0x148;
//   p1 = F         the view distance (slot 0x40, 0x10066ca0: +0x30, 1000
//                  from the constructor; BF2142.exe 0x00434610 sets it
//                  each frame through slot 0x3c to GSViewDistance x the
//                  VideoSettings view distance scale: GSViewDistance is
//                  GameLogic.maximumLevelViewDistance, rounded and clamped
//                  to 20..3000 by 0x004cc870, 500 when unset);
//   p2 = 10 S, the camera's position, the frame's seconds;
// and for each root object (0x006fb280) p3 = (+0x140)^2 x p2, or
// (+0x144)^2 x p2 for a Soldier (class 0x9493) or PlayerControlObject
// (0xc4c2), and the radius R = cullRadiusScale (template slot 0x70, +0x58,
// 1 by default) x the object's radius (slot 0x8c, 0x006f7ec0). The
// object's children take a second record (p0 / 4 (*0x00a07f04), p3 = 0.5
// (*0x00a07f08) x (+0x140)^2 x p2) when their own R is under 0.8
// (*0x00a07f00) x the root's.
//
// The manager's scales are 4 / 5 / 100 from its constructor (0x00702de0);
// RendDX9's frame (0x1002cef0) sets them around the scene's draw from the
// renderer's +0x6a8 / +0x6ac / +0x6b0 (the manager's setters, vtable
// 0x0099aba8 slots 0xc0 / 0xc8 / 0xb8) and restores them after; those are
// the geometry quality's (0x10010040): low 6 / 6 / 40, medium 7 / 8 / 60,
// high 8 / 10 / 80 (4 / 5 / 100 from the renderer's constructor 0x1002aba0;
// +0x6b0 is also renderer.minCullDistance, slots 0x84 / 0x88).
//
// The fade (0x006fb320, on the render object's +0x2c; 0x006fa520 hands the
// renderer +0x2c x the game's own fade +0x28 through slot 0x104): with d^2
// = max(|camera - origin|^2 - R^2, 0) and A = max(pi R^2 p3, p0):
//   - gone (0) past (R + p1)^2 or (1 + 0.2) A (*0x00a07f10);
//   - whole (1) within (1 - 0.2) A;
//   - between 0.8 A and A, one not whole eases in: max(1 - (d^2 - 0.8 A)
//     / (0.2 d^2), fade + 4 s) (*0x00a07f0c), at most 1;
//   - between A and 1.2 A, one not gone eases out: min(1 - (d^2 - A) /
//     (0.2 d^2), fade - 4 s), at least 0.
// An object whose fade is 0 or less isn't drawn and its children aren't
// walked (0x006fe3b0).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace dice::hfe::render {

// The manager's +0x140 / +0x144 / +0x148 for the scene's draw: the
// geometry quality's (1 low .. 3 high; 0x10010040).
struct ObjectCullScales {
	float area = 8;        // +0x140 (renderer +0x6a8)
	float actorArea = 10;  // +0x144 (+0x6ac): soldiers and vehicles
	float minimum = 80;    // +0x148 (+0x6b0)
};
inline ObjectCullScales objectCullScales(int geometryQuality)
{
	switch (std::clamp(geometryQuality, 1, 3)) {
	case 1:
		return {6, 6, 40};
	case 2:
		return {7, 8, 60};
	default:
		return {8, 10, 80};
	}
}

// GSViewDistance as 0x004cc870 stores it (rounded, 20..3000), times the
// view distance scale: the view's +0x30.
inline float viewFarDistance(float levelViewDistance, float viewDistanceScale)
{
	const float stored = static_cast<float>(static_cast<int>(std::clamp(levelViewDistance, 20.0f, 3000.0f) + 0.5f));
	return stored * viewDistanceScale;
}

// A skinned mesh's radius (RendDX9 0x101a7b10, the geometry record's +0x1c,
// which its LOD pick 0x101616c0 reads): the box's farthest corner from its
// centre, the half diagonal.
inline float boxHalfDiagonal(const std::array<float, 3>& min, const std::array<float, 3>& max)
{
	const float x = max[0] - min[0], y = max[1] - min[1], z = max[2] - min[2];
	return 0.5f * std::sqrt(x * x + y * y + z * z);
}

// A soldier's radius. The object's radius (0x006f7ec0) is its collision
// component's when it has one (+0x4c, 0x006dedb0: the first LOD's +0xc0,
// the box corner farthest from the origin), and every soldier template
// names the built-in Soldier_CollisionMesh (0x006e4880: x and z +-0.4,
// y -1..0.8 about the physics origin). Its UAVTarget child has no radius,
// so it doesn't widen it (kit weapons assumed at the origin; unchecked).
inline float soldierCullRadius()
{
	return std::sqrt(0.4f * 0.4f + 1.0f + 0.4f * 0.4f);
}

// The view distance scale a frame uses. While the commander's overhead
// view is on, BF2142.exe 0x00779680 sets the VideoSettings' scale to 1
// (DAT_00a0c208 set, DAT_00a0c204 1.0: the Renderer's slot 0x80, RendDX9
// 0x10006260, its video settings at +0x3e8, slot 0x28 0x1000f0c0 →
// +0x108, clamped 0.5..1), keeping the player's (slot 0x2c, 0x100113c0,
// in the minimap's +0xdb4) and putting it back as the view goes off. So
// the view's far distance (+0x30) and the fog (0x10028020) are full there.
inline float frameViewDistanceScale(float settingsScale, bool overheadView)
{
	return overheadView ? 1.0f : settingsScale;
}

// One root object's record (0x006ff110 + 0x006fb280).
struct CullRecord {
	float minArea = 0;  // p0
	float farDistance = 0; // p1
	float areaScale = 0; // p3
	float seconds = 0;
};
inline CullRecord cullRecord(const ObjectCullScales& scales, float zoom, float farDistance, bool actor, float seconds)
{
	const float p2 = 10 * zoom;
	const float scale = actor ? scales.actorArea : scales.area;
	return {zoom * scales.minimum * scales.minimum, farDistance, scale * scale * p2, seconds};
}

// 0x006fb320: the fade after this frame, from the last one (`fade`), the
// cull radius R and |camera - origin|^2.
inline float stepDistanceFade(float fade, float radius, float centreDistanceSquared, const CullRecord& r)
{
	constexpr float kBand = 0.2f; // *0x00a07f10
	constexpr float kRate = 4.0f; // *0x00a07f0c
	const float r2 = radius * radius;
	const float d2 = std::max(centreDistanceSquared - r2, 0.0f);
	const float area = std::max(r2 * 3.1415927f * r.areaScale, r.minArea);
	if ((radius + r.farDistance) * (radius + r.farDistance) < d2 || (kBand + 1) * area < d2) {
		return 0;
	}
	const float whole = (1 - kBand) * area;
	if (d2 <= whole) {
		return 1;
	}
	if (d2 <= area) {
		if (fade < 1) {
			const float target = 1 - (d2 - whole) / (kBand * d2);
			return std::min(std::max(target, fade + kRate * r.seconds), 1.0f);
		}
		return fade;
	}
	if (fade > 0) {
		const float target = 1 - (d2 - area) / (kBand * d2);
		return std::max(std::min(target, fade - kRate * r.seconds), 0.0f);
	}
	return fade;
}

} // namespace dice::hfe::render
