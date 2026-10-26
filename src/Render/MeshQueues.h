// Which of RendDX9's render queues a mesh material goes to, and the order
// the transparent ones are drawn in (no GPU code: shared with the tests).
//
// RendDX9's shader manager (0x1003ed70) keeps queues by name: RSMTTransparency,
// RSMT1Person, RSMT1PersonTransparency, RSMTAdditive, RSMTWater and others;
// a drawable goes to one by FUN_1003e530 (transparent, first person, the
// "delta" light pass: index (delta * 2 | firstPerson) * 2 | transparent,
// 0 being the material's own opaque queue). The bundled mesh (0x100c3880)
// sends a material there by its alpha mode (+0x15c): 1 is blended, to a
// transparency queue, unless the geometry template has hasAdditiveAlpha
// (+0x268), which uses the BMadditive shader in RSMTAdditive (in first
// person RSMT1Person, with the opaque ones); an object fading out (its
// transparency below 1, +0x1e8) sends all its materials to the
// transparency queue. The transparency queues sort (mode 2, +0xc;
// 0x10038120 -> 0x10039380, comparator 0x10038340) by a key, the distance
// from the camera to the object's origin (+0x238 = +0x248, set at the
// start of 0x100c3880), largest first: back to front. The frame
// (0x1002cef0) draws the opaque queues, the water, RSMTTransparency,
// RSMTAdditive and the particles; then, after the depth clear, RSMT1Person,
// the first-person particles and RSMT1PersonTransparency.
#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace dice::hfe::render {

enum class MeshQueue { Opaque, Transparent, Additive };

// A material's queue: `blended` for alpha mode 1, `additive` when its
// geometry also has additive alpha, `fade` its object's transparency.
inline MeshQueue meshQueue(bool blended, bool additive, float fade)
{
	if (blended && additive) {
		return MeshQueue::Additive;
	}
	return blended || fade < 1.0f ? MeshQueue::Transparent : MeshQueue::Opaque;
}

// The transparency queues' key: the camera's distance to the object's
// origin (0x100c3880).
inline float transparencyKey(const float origin[3], const float camera[3])
{
	const float dx = camera[0] - origin[0], dy = camera[1] - origin[1], dz = camera[2] - origin[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Farthest first; equal keys keep their order (one object's materials stay
// in the mesh's order).
template <class T>
void sortBackToFront(std::vector<std::pair<float, T>>& items)
{
	std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
}

// The additive shader's scrolling: frac(GlobalTime * simpleUVTranslation)
// (RaShaderBMadditive.fx).
inline float scrollOffset(float perSecond, float seconds)
{
	const float t = perSecond * seconds;
	return t - std::floor(t);
}

} // namespace dice::hfe::render
