// The Titans' interior and exterior parts, as RendDX9's mesh renderers
// cull them (GPU-free, for the tests).
//
// The object drawer (DAT_1020ef1c) knows whether the camera is inside a
// Titan: its slot 0x40 (0x100b9db0) is +0x90 (set each frame from the
// game's slot 0x110, the Titan the camera is in, 0x100b6xxx) or +0x91
// (ObjectDrawer.isInsideTitanOverride); slot 0x48 is +0x92
// (ObjectDrawer.showAllTitanParts).
//
//  - Static meshes (the StaticMeshRenderer's draw 0x100bdb00): unless all
//    parts are shown, a template marked isTitanInterior (+0xc9, slot 0xd4)
//    is drawn only from inside, through the interior path 0x100bd0a0, and
//    from inside nothing else is.
//  - Bundled meshes (0x100c5030): from inside, a template marked
//    isTitanExterior (+0xc8, slot 0xcc) isn't drawn, nor anything farther
//    than renderer.indoorBundledMeshCullDistance (+0x59c), and the rest goes
//    through the interior path 0x100c3880.
//  - Skinned meshes (0x10162070): from inside, nothing farther than
//    renderer.indoorSkinnedMeshCullDistance (+0x598).
//
// The hull's exterior parts (static, isTitanExterior with a
// titanExteriorPartIndex 1..4, +0x2bc) share their LODs in pairs so the
// pieces meet: each records the LOD its distance picks (0x100baf40, the
// renderer's +0x9c..+0xa8 by part), then draws at the smaller of its pair's
// last frame (0x100baf90: parts 1 and 2 at +0xac, 3 and 4 at +0xb0, both
// remade once a frame by 0x100bb470), at most its LOD count.
#pragma once

#include <algorithm>

namespace dice::hfe::render {

enum class TitanDraw {
	Skip,     // not drawn this frame
	Normal,   // drawn as usual
	Interior, // drawn by the interior path (the Titan's hemi map)
};

struct TitanView {
	bool cameraInside = false; // ObjectDrawer slot 0x40 (with the override)
	bool showAll = false;      // ObjectDrawer.showAllTitanParts
};

// A static mesh (0x100bdb00).
inline TitanDraw titanStaticDraw(const TitanView& view, bool interior)
{
	if (view.showAll) {
		return TitanDraw::Normal;
	}
	if (!view.cameraInside) {
		return interior ? TitanDraw::Skip : TitanDraw::Normal;
	}
	return interior ? TitanDraw::Interior : TitanDraw::Skip;
}

// A bundled mesh (0x100c5030) at `distance` metres from the camera.
inline TitanDraw titanBundledDraw(const TitanView& view, bool exterior, float distance, float indoorCullDistance)
{
	if (!view.cameraInside) {
		return TitanDraw::Normal;
	}
	if (exterior || indoorCullDistance < distance) {
		return TitanDraw::Skip;
	}
	return TitanDraw::Interior;
}

// A skinned mesh (0x10162070).
inline TitanDraw titanSkinnedDraw(const TitanView& view, float distance, float indoorCullDistance)
{
	if (!view.cameraInside) {
		return TitanDraw::Normal;
	}
	return indoorCullDistance < distance ? TitanDraw::Skip : TitanDraw::Interior;
}

// The exterior parts' LOD pairs (the StaticMeshRenderer's +0x9c..+0xb0, 0
// at first).
class TitanExteriorLods {
public:
	// A part's LOD this frame: records the one its distance picked and
	// returns its pair's (from the last frame), at most `lodCount`
	// (0x100bdb00 caps it at the count, not the count less one: the caller
	// keeps it in range).
	int resolve(int part, int distanceLod, int lodCount)
	{
		if (part >= 1 && part <= 4) {
			m_recorded[part - 1] = distanceLod;
		}
		const int paired = part < 3 ? m_pairs[0] : m_pairs[1];
		return std::min(paired, lodCount);
	}
	// Once a frame, after the draws (0x100bb470).
	void endFrame()
	{
		m_pairs[0] = std::min(m_recorded[0], m_recorded[1]);
		m_pairs[1] = std::min(m_recorded[2], m_recorded[3]);
	}

private:
	int m_recorded[4] = {0, 0, 0, 0};
	int m_pairs[2] = {0, 0};
};

} // namespace dice::hfe::render
