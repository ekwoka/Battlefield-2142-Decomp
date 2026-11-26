#pragma once

#include "Hud/Hud.h"

#include <cmath>

namespace dice::hfe::hud {

// enhanced.hudWidescreen (not retail): the 4:3 HUD on a wider window. The HUD
// pass squeezes x about the screen's centre by xScale, the side pieces move out
// by sideShift (left by -sideShift), and the full-screen gunner frames and
// filters (authored 0 0 1024 1024) span frameX..frameX + frameWidth.
//
// Remaster's hudfix fixes these at 16:9: fAspectComp = 12/16 in its
// PortedMenuShader.fx, its HudRotation.con's 120 moves of the sides, and its
// Menu_server_hudfix.zip's frames re-cut to -134 0 1367 1024. This derives
// them from the window instead, matching those numbers at 16:9:
//   - xScale = (4/3) / aspect, so the HUD keeps its 4:3 shape;
//   - sideShift = 360 (1 / xScale - 1): 0 at 4:3, 120 at 16:9, the same
//     share (nine tenths) of the room the squeeze opens at each side;
//   - the frames keep their authored on-screen 0..1024, one unit past each
//     side, as Remaster's -134..1233 does (-133.3..1232 exact).
// A window no wider than 4:3 keeps the HUD as authored.
struct WidescreenLayout {
	float xScale = 1.0f;
	int sideShift = 0;
	int frameX = 0;
	int frameWidth = 1024;
};

inline WidescreenLayout widescreenLayout(int width, int height)
{
	WidescreenLayout layout;
	if (width <= 0 || height <= 0 || 3LL * width <= 4LL * height) {
		return layout;
	}
	const double w = width, h = height;
	layout.xScale = static_cast<float>(4.0 * h / (3.0 * w));
	// 1 / xScale = 3w / 4h; the products below are exact for whole ratios.
	layout.sideShift = static_cast<int>(std::lround((1080.0 * w - 1440.0 * h) / (4.0 * h)));
	const double left = 400.0 - 1200.0 * w / (4.0 * h);  // 400 - 400 / xScale
	const double right = 400.0 + 1872.0 * w / (4.0 * h); // 400 + (1024 - 400) / xScale
	layout.frameX = static_cast<int>(std::ceil(left)) - 1;
	layout.frameWidth = static_cast<int>(std::floor(right)) + 1 - layout.frameX;
	return layout;
}

// The nodes it moves: the sides HudRotation.con moves, and the frames the
// hudfix re-cuts. The commander screen stays centred.
inline constexpr const char* kWidescreenLeftNodes[] = {
	"BottomLeftStaticHud", "SquadInfo", "SquadOrderInfo", "IngameChat", "IngameChatInput", "PlayerNames"};
inline constexpr const char* kWidescreenRightNodes[] = {"BottomRightStaticHud", "MinSizeAlpha", "Minimap", "ItemSelectionHud"};
inline constexpr const char* kWidescreenFrameNodes[] = {"IFVGunnerHudFrame", "IFVRocketHudFrame", "LightHoverHeatHudFilter",
	"EuGunshipCoFilter", "EUGunshipHeatHudFilter", "GunshipCoFilter", "GunshipHeatHudFilter", "TankEuGunnerHudFrame",
	"TankGunnerHudFrame", "GunEuFrame", "GunPacFrame"};

// Moves the HUD's nodes from the layout last applied (`applied`, the authored
// HUD at first) to `layout`, so a resized window re-lays it out.
inline void applyWidescreenLayout(HudManager& manager, WidescreenLayout& applied, const WidescreenLayout& layout)
{
	const int dx = layout.sideShift - applied.sideShift;
	if (dx != 0) {
		for (const char* name : kWidescreenLeftNodes) {
			manager.shiftNode(name, -dx);
		}
		for (const char* name : kWidescreenRightNodes) {
			manager.shiftNode(name, dx);
		}
	}
	if (layout.frameX != applied.frameX || layout.frameWidth != applied.frameWidth) {
		for (const char* name : kWidescreenFrameNodes) {
			manager.spanNode(name, layout.frameX, layout.frameWidth);
		}
	}
	applied = layout;
}

} // namespace dice::hfe::hud
