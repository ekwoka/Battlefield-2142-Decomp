// The commander's overhead view: what the renderer is told of it and the
// projection it is drawn with (header-only, for the tests).
#pragma once

#include "RenderMath.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::render {

// The commander's overhead view (BF2142.exe's OverheadCamera, class
// 0x948c): the team's camera over the commander's point, which the
// player's view takes while the commander screen's map is at its zoom 2
// (0x00779680). The camera (0x005f8170) sits over (x, z), kept 80 m inside
// the terrain's edges, at the mean of the ground's height there and 2 m
// off along both axes (or the sea's when higher) plus the template's
// `height`, looking straight down with north up; the world pass is drawn
// from it with `fovY` over `rect` (the HUD's map node, 800 x 600: the view
// shows through it as the map fades out), the rest of the screen going on
// with the same view.
struct OverheadView {
	float x = 0, z = 0;
	float height = 260;  // MultiPlayerOverheadCamera.tweak's height (+0x2a4)
	float fovY = 0.3f;   // its fOV
	float rect[4] = {100, 0, 700, 600}; // x0, y0, x1, y1
};

// The overhead view's projection: `fovY` over the view's rectangle of the
// 800 x 600 screen (the HUD stretched over a `width` x `height` target),
// the screen around it going on with the same view (an off-centre
// perspective; Direct3D's row-vector convention, depth 0..1).
inline Mat4 overheadProjection(const OverheadView& view, float width, float height, float nearPlane, float farPlane)
{
	// The rectangle in normalised device coordinates and its shape in pixels.
	const float nx0 = view.rect[0] / 400 - 1, nx1 = view.rect[2] / 400 - 1;
	const float ny0 = 1 - view.rect[1] / 300, ny1 = 1 - view.rect[3] / 300;
	const float rectWidth = std::max((view.rect[2] - view.rect[0]) / 800 * width, 1.0f);
	const float rectHeight = std::max((view.rect[3] - view.rect[1]) / 600 * height, 1.0f);
	const float t = std::tan(view.fovY / 2);
	const float aspect = rectWidth / rectHeight;
	Mat4 r;
	r.m = {(nx1 - nx0) / (2 * t * aspect), 0, 0, 0, 0, (ny0 - ny1) / (2 * t), 0, 0, (nx0 + nx1) / 2, (ny0 + ny1) / 2,
		farPlane / (farPlane - nearPlane), 1, 0, 0, -nearPlane * farPlane / (farPlane - nearPlane), 0};
	return r;
}

// The ground width the view spans across its rectangle's height at its
// height over the ground (0x0077c4b0: 2 tan(fov / 2) times the height).
inline float overheadGroundWidth(float fovY, float height)
{
	return 2 * std::tan(fovY / 2) * height;
}

} // namespace dice::hfe::render
