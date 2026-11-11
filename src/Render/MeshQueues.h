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
#include <string>
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

// Whether a material's back faces are culled. RendDX9's state manager
// (0x100238b0) leaves D3DRS_CULLMODE at D3DCULL_CCW for whatever does not set
// it, and a material's settings carry CullMode 3 (CCW) from their
// constructor (0x10040070, "CullMode" at +0x8c) into the draw. The bundled
// mesh's draw (0x100c3880, and its loader 0x100a2100) sets CullMode 1
// (none) only for the blended materials (alpha mode 1, the additive ones
// too); RaShaderBM's and RaShaderSM's techniques name no CullMode of their
// own. So a bundled or skinned mesh's opaque and alpha-tested faces are
// drawn from the front only: a gun's camera inside its turret's box (eu_ifv's
// grenade launchers, the camera 3.5 cm behind the mount's front face) sees
// out through it. (The static meshes are left two-sided here: RaShaderLeaf
// sets CullMode NONE for the leaves, the rest are not sorted out.)
inline bool meshCulled(bool bundledOrSkinned, bool blended)
{
	return bundledOrSkinned && !blended;
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

// Whether an object's template tree is drawn through a child as part of
// the object. An EffectBundle (and its wheel and air-draft kinds) makes
// its children only when it starts (its start, 0x0061a4e0, from
// startOnEffects / startAtCreation) and the effect system draws them: an
// idle bundle (a vehicle's EMP trigger, eu_fav_EMP_trigger with e_EMP_Fav's
// lightning meshes) shows nothing.
inline bool drawnWithObject(const std::string& templateType)
{
	return templateType != "EffectBundle" && templateType != "WheelEffectBundle" && templateType != "AirDraftEffectBundle";
}

} // namespace dice::hfe::render
