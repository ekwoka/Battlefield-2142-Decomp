// A bundled mesh in the dynamic shadow map: RendDX9's
// BundledMeshDrawShadowMap (0x100c67e0).
#pragma once

#include <algorithm>

namespace dice::hfe::render {

// The light manager's shadow LOD (its +0xbc, slot 0x228 getter 0x1014a3e0):
// 0 from its constructor (0x100aafe0). Its setter (slot 0x224, 0x1014a3d0)
// has no caller in RendDX9 and the EXE, which only registers and drops its
// lights with the manager (0x0064a770, 0x0064a7f0), and no shadowManager.*
// console method reaches it, so it stays 0.
constexpr int kLightManagerShadowLod = 0;

// The LOD a bundled mesh casts with: its own, raised to at least the light
// manager's and capped at the cast geometry's last. Swapping a cockpit's
// geometry 0 for its exterior 1 (0x100c6871 → setGeometry 0x100c3260)
// resets the mesh's LOD to 0, so a swapped mesh starts from 0, not from the
// LOD its shown geometry had.
inline int bundledShadowLod(int shownLod, bool geometrySwapped, int lodCount)
{
	const int lod = std::max(geometrySwapped ? 0 : shownLod, kLightManagerShadowLod);
	return std::max(0, std::min(lod, lodCount - 1));
}

// Only its opaque materials cast: alpha mode 1 (blended or additive) and 2
// (alpha tested) are skipped (the material's +0x15c, 0x100c67e0), whatever
// shadowManager.allowAlphaShadows says.
inline bool bundledMaterialCastsShadow(int alphaMode)
{
	return alphaMode != 1 && alphaMode != 2;
}

} // namespace dice::hfe::render
