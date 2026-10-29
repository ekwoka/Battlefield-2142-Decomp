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
class ObjectTemplate;
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
	// The view's fog (renderer.fogStartEndAndBase: start, end, base, the
	// fourth), which fades the enemies' tags (0x100f09b0 on the fog's
	// FogRange constants, the fog object's +0xb8, 0x100355c0); an end at
	// or before the start: no fog.
	std::array<float, 4> fog {0, 0, 0, 0};
};

// 0x100f09b0: how much of a tag at distance d shows through the fog,
// FogRange (r0..r3 = 1 / (e - s), -base / (e - s), -s / (e - s), e / (e - s)):
// clamp(max(r3, d r1 + r3) - (d r0 + r2)^3, 0, 1) (the CRT pow's exponent
// taken as calcFog's cube); 1 without fog.
float tagFog(const std::array<float, 4>& startEndAndBase, float distance);

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

// The player's own icon (tag +0x60, 0x100f0310): a squad leader's
// ICObject + 25 + his squad, the commander's ICObject + 25, a soldier
// ICObject + 10 (0x5a), one in a vehicle the vehicle's (vehicleIconId).
int playerIconId(bool squadLeader, int squad, bool commander);
// A vehicle's icon (0x100f0310 through the vehicle root template's 0xc4c4
// interface → the NametagManager's slot 0x74, 0x100ee590): by its
// setVehicleType (slot 0x34, template +0x3d8, 0x1b by default from
// 0x005cd9e0; the VT names of 0x004d.. 0..0x1a): tanks 0x5f, the APC,
// none, armoured cars and tank hunters 0x5d, scout cars 0x5c, aircraft
// 0x61, AA guns 0x64, stationary MGs 0x65, ships and boats 0x60, AT guns
// 0x66, VTAA 0x5e, attack helicopters 0x62, transport ones 0x63, landing
// craft, snipers and infantry 0x5a; any other by its vehicleCategory
// (slot 0x24: VCLand 0x5b, VCSea 0x60, VCAir 0x61, VCHelicopter 0x62)
// unless the vehicle draws its occupant (vehicleHud.usePlayerIcon, the
// HUD data's +0xb8): 0x5a.
int vehicleIconId(int vehicleType, int vehicleCategory, bool usePlayerIcon);
// setVehicleType's name (VTHeavyTank 0 .. VTInfantry 0x1a), 0x1b for none.
int vehicleTypeOf(const std::string& name);
// An order's 3D icon (0x100f4620): ICSquadOrder 0x3d.. for the squad
// leader's, ICCommanderOrder 0x43.. for the commander's, by the order's
// map kind (MapMove 4 .. MapRepair 7: 0x39 / 0x3f + it).
int orderIconId(bool fromCommander, int kind);
// A request's icon (0x100f44d0: kind 0 the orbital bombardment's 0x7a, 1
// the orbital strike's 0x79, 2 the UAV's 0x7b, 3 the supplies' 0x7c; the
// selected ones 0x7e / 0x7d / 0x7f / 0x80).
int requestIconId(int kind, bool selected);
constexpr float kOrderDistance = 1200; // DAT_101f67b4

// What a kit lets its carrier answer (the HUD manager's slot 0xac -> +0x74
// slot 0x38: [0] heal and revive, [1] repair, [2] ammunition); by the
// kit's kitType here (Assault, Engineer, Support).
struct Abilities {
	bool heal = false, repair = false, ammo = false;
};
Abilities kitAbilities(const std::string& kitType);

// The radio calls (the NametagManager's events 0x33..0x37, 0x100f19c0):
// 0x33 medic, 0x34 ammunition, 0x35 repair, 0x36 / 0x37 revive. A call
// from another player within maxCallDistance (150 m) whom the local
// player's kit can answer puts the call's icon on the caller's tag
// (ICMedic 0x2d, ICAmmo 0x2f, ICRepair 0x2e, ICRevive 0x2c) for 12 s (30 s
// for a revive, DAT_101f6740), in the highlight colour, shown off screen
// too; the local player's own call starts a 12 s timer (DAT_10211290 /
// 98 / a0) during which those who can answer it show that icon.
enum CallCode { CallMedic = 0x33, CallAmmo = 0x34, CallRepair = 0x35, CallRevive = 0x36 };
struct CallTimers {
	float medic = 0, ammo = 0, repair = 0; // the local player's own (0x10211290, 0x10211298, 0x102112a0)
	void tick(float seconds);
};
// The icon a player's tag shows for the local player's own call (0x100f0310's
// first branch): -1 none.
int helperIcon(const CallTimers& timers, const Abilities& helper);
// A call's icon on the caller's tag and its time (0x100f19c0's switch); -1
// when the local player can't answer it.
int callIcon(int code, const Abilities& local, float& seconds);
// The rank and veteran icons on the name's line (0x100f0840: tag +0x50
// ICRank + the stats' rank, player slot 0x1a8 +0x20; +0x54 ICVeteran 0x4f
// when the player's slot 0x3ac, +0x402, says so: the online profile's
// "Ent-3" entitlement, 0x0045c8a0 / 0x00513e70, never offline). The
// renderer (0x1016b100) copies them into the tag's text row left of the
// name, each advancing by its width, then 2 pixels, the row centred; in
// white (the squad's colour for a squad member).
struct NameRow {
	int rankIcon = -1, veteranIcon = -1;
};
NameRow nameRow(int rank, bool veteran);

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
	// The level's fog, for the enemies' fade (Settings::fog).
	void setFog(const std::array<float, 4>& startEndAndBase) { m_settings.fog = startEndAndBase; }
	// The icons nametags.con made (the objects' 3D icons draw with them).
	void setIcons(const IconRegistry* icons) { m_icons = icons; }
	// The camera inside a Titan (the objects' showInside / showOutside;
	// RendDX9's object drawer slot 0x40, which the manager's frame reads,
	// 0x100f3490).
	void setInsideTitan(bool inside) { m_insideTitan = inside; }
	// A radio call heard (see CallCode); `local` for the local player's own.
	void radioCall(int caller, int code, bool local);
	// The tag the crosshair selects now (0x100f16c0: the selectable tag
	// nearest the screen's centre within 0.08), its object; 0 none.
	uint32_t selected() const { return m_selectedObject; }
	// The request tag the crosshair selects (the commander's), its id
	// squad x 4 + kind; -1 none.
	int selectedRequest() const { return m_selectedRequest; }
	// The tags shown at all (the manager's +0x465, on from its
	// constructor; slots 0x64 / 0x68, 0x100f5f40 / 0x100f5f50): the HUD's
	// c_GI3dMap flips it (0x00754290). Off, frame draws nothing.
	bool shown() const { return m_shown; }
	void setShown(bool shown) { m_shown = shown; }

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
		int callIcon = -1;  // +0x64
		float callTime = 0; // +0x6c
	};
	// A template's Nametag component (its flags, component +0x30 → the
	// tag's +0xb8: showItem 1, showIcon 2, showName 4, showDistance 8,
	// show3dIcon 0x10, glueToEdge 0x20, teamSpecific 0x200,
	// inverseTeamSpecific 0x400, showIconOnTracker 0x4000, showInside
	// 0x40000, showOutside 0x80000, selectable 0x100000, useTeamColor
	// 0x200000, useAliveArmor 0x1000000; the write-back 0x0063xxxx),
	// read once per template.
	struct TagTemplate {
		bool controlPoint = false;
		bool showItem = false, show3dIcon = false, showDistance = false, glue = false;
		bool teamSpecific = false, inverse = false, tracker = false, showInside = false, showOutside = true;
		bool selectable = false, useTeamColor = false, useAliveArmor = false;
		int icon = -1, selectedIcon = -1; // 3dMapIcon, selected3dMapIcon
		float maxDistance = 1200, maxEnemyDistance = 1200;
		std::array<float, 3> offset {};
		int vehicleIcon = 0x5a; // vehicleIconId of its setVehicleType / vehicleCategory / usePlayerIcon
		bool tagged() const { return (controlPoint && showItem) || (!controlPoint && show3dIcon); }
	};
	const TagTemplate& tagTemplate(const world::ObjectTemplate* t);
	std::map<const world::ObjectTemplate*, TagTemplate> m_templates;
	std::vector<uint32_t> m_tagged; // the objects with tags, looked for once a second
	float m_taggedCheck = 0;
	CallTimers m_calls;
	struct PendingCall {
		int caller, code;
		bool local;
	};
	std::vector<PendingCall> m_pendingCalls;
	uint32_t m_selectedObject = 0;
	int m_selectedRequest = -1;
	bool m_shown = true;
	static constexpr uint32_t kRequestObject = 0x80000000u; // a request's selection (the manager's id - 0x80000000)
	float m_clock = 0; // the manager's +0x10 (the blink)
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
