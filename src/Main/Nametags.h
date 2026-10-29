// The 3D name tags and object icons: the names over friendly soldiers and
// vehicles, the medic's health bars, the enemy's name when aimed at, the
// control points' icons with their distances.
//
// In the original these are RendDX9.dll's NametagManager (the class
// dice.hfe.geom.NametagManager, vtable 0x101d6c80; its frame 0x100f3490)
// with its NametagRenderer (the bars 0x1016c1e0, the icon passes from
// 0x10168890, Shaders/Nametag), fed by BF2142.exe's game objects (the
// manager looked up by 0x0043ab00; object tags added through its slot 0x30,
// 0x100f40d0, from the templates' Nametag component, 0x0063b760). The
// rewrite builds them as HUD quads in the 800 x 600 screen, under the HUD.
// See docs/engine.md "Name tags".
#pragma once

#include "Hud/Hud.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
namespace io {
class FileManager;
}
namespace world {
class ObjectManager;
}
namespace game {
class GameWorld;
}
} // namespace dice::hfe

namespace dice::hfe::nametags {

// nametags.* (RendDX9's .data defaults; nametags.con sets none of them).
struct Settings {
	float maxFriendlyDistance = 120;        // 0x101f6768
	float maxCallDistance = 150;            // 0x101f673c
	float vehicleNametagOffset = 0.45f;     // 0x101f676c
	float vehicleNametagOffsetFactor = 0.35f; // 0x101f6770
	float deathFadeOutTime = 0.6f;          // 0x101f6778
	float manDownFadeOutTime = 14;          // 0x101f6774
	float playerCullFar = 300;              // getCullDistance(TTPlayer).y, 0x101f67a8
	float aimedFriendlyDistance = 500;      // an aimed-at friendly's range
	float aimCone = 0.99999f;               // DAT_101f6744, 0x100ef4c0
	float circleRadius = 0.65f;             // GSNameTagCircleRadius (glueToEdge)
};

// A tag's fade (tag +0xa0 the timer, +0xa8 the alpha).
struct Fade {
	float alpha = 0;
	float timer = 0;
	// 0x100eefd0: waits `delay` while still faded out, then alpha += dt / time.
	void in(float seconds, float delay, float time);
	// 0x100ef070: alpha -= dt / time; the timer starts over.
	void out(float seconds, float time);
};

// The colour indices (tag +0x5c): ColorArray's rows in Shaders/Nametag.
enum Colour { Neutral = 0, Enemy = 1, Team = 2, Squad = 3, Highlight = 4 };

// One player's tag this frame, as 0x100f0e60 / 0x100f0310 decide it.
struct PlayerTagInput {
	bool friendly = false;
	bool sameSquad = false;
	bool commander = false;
	bool alive = true;
	bool manDown = false;
	bool sameVehicle = false; // in the local player's vehicle: no tag
	bool aimedAt = false;
	bool localMedic = false;  // the local kit revives (manDownFadeOutTime)
	float distance = 0;       // horizontal, metres
};
// Advances the fade and returns the tag's alpha (0: not drawn).
float playerTagAlpha(const Settings& settings, const PlayerTagInput& in, Fade& fade, float seconds);
// The colour index (0x100f0310's end).
Colour playerTagColour(const PlayerTagInput& in);
// The rounded distance shown (0x100f0be0 / 0x100ef120): kept until it
// changes by more than 0 / 2 / 5 / 9 m below 25 / 250 / 400 m / beyond.
int shownDistance(int shown, float distance);

// The BF1942.font bitmap font the tags are written in ("Fonts/BF1942.font":
// a header, then "<char> <x0> <y> <x1>" cells of Height rows).
struct TagFont {
	std::string texture; // "Fonts/BF1942.dds"
	int textureWidth = 128, textureHeight = 128;
	int height = 11;
	int between = 1;
	struct Cell {
		int x0 = 0, y = 0, x1 = -1;
	};
	std::array<Cell, 256> cells {};
	std::array<bool, 256> present {};
	bool parse(const std::string& text);
	float width(const std::string& text) const;
	// Quads for the text from (x, y), its top left, in `colour`.
	void write(const std::string& text, float x, float y, const float colour[4], std::vector<hud::HudQuad>& out) const;
};

// The tags' icons and bars (Menu/Nametag/nametags.con, run by the HUD
// manager's set-up 0x0075cfa0 after HudSetupMain.con): nametags.createIcon
// <NametagItemIDs> (0x10079004) picks an id by its name (the enum of
// 0x10070e1a: ICRank 0 (44 ranks), ICRevive 0x2c, ICMedic, ICRepair,
// ICAmmo, the vehicle ones 0x30-0x33, the bars 0x34-0x36, ICSquadOrder
// 0x3d and ICCommanderOrder 0x43 (6 frames each), ICTarget 0x4e,
// ICVeteran 0x4f, ICObject 0x50 (41 frames), the request orders
// 0x79-0x80, the Titans 0x81-0x88, the silos 0x8c-0x91, 0x92-0x94 the
// selected backgrounds, ICObjectCore 0x96, the consoles 0x97-0x9a, the
// networkables 0xa0-0xa5) and nametags.setTexture <frames> <texture> x y w
// h (0x100790e4 → the manager 0x100ee9a0 → 0x1016b790) its texture: frame
// k of the id + k is the w x h texels at x + k w, y (one frame for 0);
// nametags.createBar <id> <from> <to> starts a bar, its fill (texture 0)
// and base (1) set after.
struct IconRegistry {
	struct Icon {
		std::string texture; // with ".dds" when it had no extension
		float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
		float width = 16, height = 16;
	};
	std::map<int, Icon> icons;
	int current = -1;
	bool bar = false;
	std::map<int, std::pair<int, int>> barSpan; // createBar's texels
	// The textures' sizes (texels), to turn the frames into UVs.
	std::function<bool(const std::string& texture, int& width, int& height)> textureSize;
	static int idOf(const std::string& name); // -1 unknown
	void bind(Console& console);
	void setTexture(int frames, const std::string& texture, int x, int y, int w, int h);
	const Icon* find(int id) const
	{
		const auto it = icons.find(id);
		return it == icons.end() ? nullptr : &it->second;
	}
};

class Nametags {
public:
	explicit Nametags(const io::FileManager& files);

	struct View {
		std::array<float, 3> position {};
		float yaw = 0, pitch = 0; // radians: yaw 0 along +z, pitch up positive
		float fovY = 1;
		float aspect = 4.0f / 3.0f;
	};
	// This frame's tags (back to front of nothing: drawn under the HUD).
	std::vector<hud::HudQuad> frame(game::GameWorld& world, const world::ObjectManager& objects,
		const View& view, float seconds);

	const Settings& settings() const { return m_settings; }
	// The icons nametags.con made (the objects' 3D icons draw with them).
	void setIcons(const IconRegistry* icons) { m_icons = icons; }
	// The camera inside a Titan (the objects' showInside / showOutside).
	void setInsideTitan(bool inside) { m_insideTitan = inside; }

	// A point in the 800 x 600 screen; false behind the camera. With
	// `glue`, a point off the circle of radius circleRadius (normalised
	// device units) or behind is put on it and `clamped` set (glueToEdge).
	static bool project(const View& view, const std::array<float, 3>& point, float& x, float& y, bool glue,
		float circleRadius, bool* clamped = nullptr);

private:
	struct Tag {
		Fade fade;
		int distance = -1;
		float aimCheck = 0; // until the next look (0.25 s)
		bool aimed = false;
	};
	Settings m_settings;
	const IconRegistry* m_icons = nullptr;
	bool m_insideTitan = false;
	TagFont m_font;
	bool m_fontLoaded = false;
	std::map<int, Tag> m_players; // by player id
	float m_crosshairCheck = 0;   // the crosshair ray every 0.5 s (0x100ef5b0)
	uint32_t m_crosshairTarget = 0;
};

} // namespace dice::hfe::nametags
