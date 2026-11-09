// The 3D name tags' rules (src/Main/Nametags: RendDX9's NametagManager,
// 0x100f0e60, 0x100f0310, 0x100f0be0, 0x100ef210) and their font.
#include "Main/Nametags.h"
#include "Geometry/MeshFile.h"
#include "Game/GameWorld.h"
#include "Main/HudWorld.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "Console/Console.h"
#include "IO/File/FileManager.h"
#include "TestMain.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dice::hfe;

namespace {

float run(const nametags::Settings& s, const nametags::PlayerTagInput& in, nametags::Fade& fade, float seconds)
{
	float alpha = 0;
	for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
		alpha = nametags::playerTagAlpha(s, in, fade, 1 / 30.0f);
	}
	return alpha;
}

} // namespace

// Friends are named to 80 m, fading out by 120 (maxFriendlyDistance);
// aimed at, out to 500 m.
TEST(Nametags_FriendlyRange)
{
	const nametags::Settings s;
	nametags::PlayerTagInput in;
	in.friendly = true;
	nametags::Fade fade;
	in.distance = 50;
	CHECK(std::fabs(run(s, in, fade, 0.1f) - 1) < 1e-4f);
	in.distance = 100;
	fade = {};
	CHECK(std::fabs(run(s, in, fade, 0.1f) - 0.5f) < 1e-3f);
	in.distance = 130;
	fade = {};
	CHECK(run(s, in, fade, 0.1f) == 0);
	// Aimed at: faded in over 0.7 s, full to 333 m.
	in.aimedAt = true;
	in.distance = 300;
	fade = {};
	const float half = run(s, in, fade, 0.35f);
	CHECK(half > 0.4f && half < 0.6f);
	CHECK(std::fabs(run(s, in, fade, 1) - 1) < 1e-4f);
	in.distance = 600;
	CHECK(run(s, in, fade, 0.1f) == 0);
	// Down: gone over 0.6 s, or 14 s for a medic.
	in = {};
	in.friendly = true;
	in.distance = 10;
	in.manDown = true;
	fade = {};
	fade.alpha = 1;
	CHECK(run(s, in, fade, 0.7f) == 0);
	in.localMedic = true;
	fade.alpha = 1;
	CHECK(run(s, in, fade, 7) > 0.4f);
}

// Enemies only while aimed at, after a delay of their distance's share of
// 300 m, and gone past 285 m.
TEST(Nametags_EnemiesWhenAimedAt)
{
	const nametags::Settings s;
	nametags::PlayerTagInput in;
	in.distance = 30;
	nametags::Fade fade;
	CHECK(run(s, in, fade, 2) == 0);
	in.aimedAt = true;
	CHECK(run(s, in, fade, 0.05f) == 0); // the 0.1 s delay
	CHECK(run(s, in, fade, 0.3f) > 0.9f);
	CHECK(nametags::playerTagColour(in) == nametags::Enemy);
	in.manDown = true;
	CHECK(run(s, in, fade, 0.1f) == 0);
	in = {};
	in.aimedAt = true;
	in.distance = 290;
	fade = {};
	CHECK(run(s, in, fade, 5) == 0);
	in.friendly = true;
	in.sameSquad = true;
	CHECK(nametags::playerTagColour(in) == nametags::Squad);
}

TEST(Nametags_ShownDistanceSteps)
{
	CHECK(nametags::shownDistance(-1, 20.4f) == 20);
	CHECK(nametags::shownDistance(20, 21.2f) == 21);
	CHECK(nametags::shownDistance(100, 101.6f) == 100);
	CHECK(nametags::shownDistance(100, 103.4f) == 103);
	CHECK(nametags::shownDistance(300, 304) == 300);
	CHECK(nametags::shownDistance(500, 509) == 500);
	CHECK(nametags::shownDistance(500, 510) == 510);
}

// The projection into the 800 x 600 screen, and glueToEdge's circle.
TEST(Nametags_ProjectionAndEdge)
{
	nametags::Nametags::View view;
	view.fovY = 1;
	view.aspect = 4.0f / 3.0f;
	float x = 0, y = 0;
	CHECK(nametags::Nametags::project(view, {0, 0, 10}, x, y, false, 0.65f));
	CHECK(std::fabs(x - 400) < 1e-3f && std::fabs(y - 300) < 1e-3f);
	CHECK(nametags::Nametags::project(view, {1, 1, 10}, x, y, false, 0.65f));
	CHECK(x > 400 && y < 300);
	CHECK(!nametags::Nametags::project(view, {0, 0, -10}, x, y, false, 0.65f));
	bool clamped = false;
	CHECK(nametags::Nametags::project(view, {0, 0, -10}, x, y, true, 0.65f, &clamped));
	CHECK(clamped && std::fabs(std::hypot((x - 400) / 400, (y - 300) / 300) - 0.65f) < 1e-3f);
}

// The tags' font, Fonts/BF1942.font.
TEST(Nametags_Font)
{
	nametags::TagFont font;
	CHECK(font.parse("Texture = Fonts/BF1942\nTextureWidth = 128\nTextureHeight = 128\nBetweenWidth = 1\nHeight = 11\n"
					 "-------\n  0 0 0\n! 0 0 1\nA 31 12 36\n"));
	CHECK(font.texture == "Fonts/BF1942.dds" && font.height == 11);
	CHECK(font.width("A") == 7);
	CHECK(font.width("A A") == 16);
	std::vector<hud::HudQuad> quads;
	const float white[4] = {1, 1, 1, 1};
	font.write("A A", 0, 0, white, quads);
	CHECK(quads.size() == 2 && quads[1].x0 == 9);
	// The font's DXT1 texture has no alpha (black cells), so its quads must add (source alpha, ONE), not blend over.
	CHECK(quads[0].blend[0] == 7 && quads[0].blend[1] == 2 && quads[1].blend[0] == 7 && quads[1].blend[1] == 2);
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(dir);
	files.addSearchPath("mods/bf2142");
	std::vector<uint8_t> data;
	files.setMountingAllowed(true);
	files.mountArchive("Fonts_client.zip", "Fonts/");
	CHECK(files.readFile("Fonts/BF1942.font", data));
	if (!data.empty()) {
		nametags::TagFont shipped;
		CHECK(shipped.parse(std::string(data.begin(), data.end())));
		CHECK(shipped.present['B'] && shipped.cells['B'].x0 == 37 && shipped.cells['B'].y == 12);
	}
}

// nametags.con's icons (0x10079004, 0x100790e4 → 0x1016b790): ids by name,
// frames along the texture, a bar's fill and base.
TEST(Nametags_IconRegistry)
{
	CHECK(nametags::IconRegistry::idOf("ICObject") == 0x50 && nametags::IconRegistry::idOf("icobjectcore") == 0x96);
	CHECK(nametags::IconRegistry::idOf("ICNothing") == -1);
	// 0x1007168d also takes an id by number, when it is one of the ids.
	CHECK(nametags::IconRegistry::idOf("53") == 0x35 && nametags::IconRegistry::idOf("0") == 0);
	CHECK(nametags::IconRegistry::idOf("1") == -1);
	Console console;
	nametags::IconRegistry icons;
	icons.textureSize = [](const std::string& texture, int& w, int& h) {
		w = texture.find("Objects") != std::string::npos ? 1024 : 64;
		h = 16;
		return true;
	};
	icons.bind(console);
	std::string out;
	console.execute("Nametags.createIcon ICObject", out);
	console.execute("Nametags.setTexture 41 Menu/Nametag/Objects 0 0 16 16", out);
	console.execute("Nametags.createBar ICArmorBar 1 45", out);
	console.execute("Nametags.setTexture 0 Menu/Nametag/bar_filler.dds 0 0 64 8", out);
	console.execute("Nametags.setTexture 1 Menu/Nametag/bar_base.dds 0 0 64 8", out);
	const nametags::IconRegistry::Icon* nine = icons.find(0x50 + 9); // a 3dMapIcon 9: the atlas's tenth frame
	CHECK(nine && nine->texture == "Menu/Nametag/Objects.dds" && std::fabs(nine->u0 - 9 * 16 / 1024.0f) < 1e-6f);
	CHECK(icons.find(0x50 + 41) == nullptr); // past the 41 frames
	// 3dMapIcon 70 (the Titan cores') is ICObjectCore, 0x50 + 70.
	console.execute("Nametags.createIcon ICObjectCore", out);
	console.execute("Nametags.setTexture 0 Menu/Nametag/Titan/core.dds 0 0 32 32", out);
	CHECK(icons.find(0x50 + 70) && icons.find(0x50 + 70)->width == 32);
	CHECK(icons.find(0x35) && icons.find(0x35)->texture == "Menu/Nametag/bar_filler.dds");
	CHECK(icons.find(0x35 | 0x100) && icons.find(0x35 | 0x100)->texture == "Menu/Nametag/bar_base.dds");
}

// The players' icons, the radio calls and the name's row (0x100f0310,
// 0x100f19c0, 0x100f0840, 0x1016b100).
TEST(Nametags_IconsCallsAndRanks)
{
	CHECK(nametags::playerIconId(false, 0, false) == 0x5a);
	CHECK(nametags::playerIconId(true, 3, false) == 0x69 + 3);
	CHECK(nametags::playerIconId(false, 2, true) == 0x69);
	const nametags::Abilities medic = nametags::kitAbilities("Assault"), support = nametags::kitAbilities("Support");
	CHECK(medic.heal && !medic.ammo && support.ammo && nametags::kitAbilities("Engineer").repair);
	float seconds = 0;
	CHECK(nametags::callIcon(nametags::CallMedic, medic, seconds) == 0x2d && seconds == 12);
	CHECK(nametags::callIcon(nametags::CallAmmo, medic, seconds) == -1);
	CHECK(nametags::callIcon(nametags::CallAmmo, support, seconds) == 0x2f);
	CHECK(nametags::callIcon(nametags::CallRevive, medic, seconds) == 0x2c && seconds == 30);
	nametags::CallTimers timers;
	CHECK(nametags::helperIcon(timers, medic) == -1);
	timers.medic = 12;
	CHECK(nametags::helperIcon(timers, medic) == 0x2d && nametags::helperIcon(timers, support) == -1);
	timers.tick(13);
	CHECK(timers.medic == 0 && nametags::helperIcon(timers, medic) == -1);
	CHECK(nametags::nameRow(5, false).rankIcon == 5 && nametags::nameRow(5, false).veteranIcon == -1);
	CHECK(nametags::nameRow(44, true).rankIcon == -1 && nametags::nameRow(0, true).veteranIcon == 0x4f);
}

// A vehicle's own icon (0x100ee590 by setVehicleType / vehicleCategory /
// usePlayerIcon), the orders' and requests' icons (0x100f4620,
// 0x100f44d0) and the fog on enemies (0x100f09b0).
TEST(Nametags_VehicleOrderAndFog)
{
	using nametags::vehicleIconId;
	CHECK(nametags::vehicleTypeOf("VTApc") == 4 && nametags::vehicleTypeOf("VTInfantry") == 0x1a);
	CHECK(nametags::vehicleTypeOf("") == 0x1b);
	CHECK(vehicleIconId(4, 0, false) == 0x5d);   // an APC
	CHECK(vehicleIconId(0, 0, false) == 0x5f);   // a heavy tank
	CHECK(vehicleIconId(0x18, 3, false) == 0x63); // a transport helicopter
	CHECK(vehicleIconId(0x1b, 0, false) == 0x5b); // untyped land vehicle
	CHECK(vehicleIconId(0x1b, 2, false) == 0x61); // untyped aircraft
	CHECK(vehicleIconId(0x1b, 2, true) == 0x5a);  // drawn as its occupant
	CHECK(vehicleIconId(0x19, 0, false) == 0x5a); // VTSniper
	CHECK(nametags::orderIconId(false, 5) == 0x3e && nametags::orderIconId(true, 4) == 0x43);
	CHECK(nametags::orderIconId(false, 1) == 0x3e); // a comm-rose attack as the map's
	CHECK(nametags::requestIconId(0, false) == 0x7a && nametags::requestIconId(1, true) == 0x7d);
	// No fog: 1; Cerbere-like fog 0..140 m: clear near, gone past the end.
	CHECK(nametags::tagFog({0, 0, 0, 0}, 500) == 1);
	CHECK(nametags::tagFog({0, 140, 0, 0}, 10) > 0.99f);
	CHECK(nametags::tagFog({0, 140, 0, 0}, 150) == 0);
	nametags::Settings s;
	s.fog = {0, 140, 0, 0};
	nametags::PlayerTagInput in;
	in.aimedAt = true;
	in.distance = 120;
	nametags::Fade fade;
	const float fogged = run(s, in, fade, 5);
	nametags::Fade clear;
	const float plain = run(nametags::Settings {}, in, clear, 5);
	CHECK(fogged < plain && plain > 0.9f);
}

// A joined game's tags (Nametags::setWorld): the players and the
// commander's requests come from the HUD's world (the server's), not the
// client's round, which has none.
TEST(Nametags_JoinedWorld)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	CHECK(files.mountArchive("Fonts_client.zip", "Fonts"));
	world::ObjectTemplateManager templates;
	templates.registerType(3, "Soldier");
	world::ObjectTemplate* soldierTemplate = templates.create("Soldier", "pac_light_soldier");
	soldierTemplate->settings()["armor.maxHitPoints"] = "100";
	world::ObjectTemplate* kitTemplate = templates.create("Kit", "assault_kit");
	kitTemplate->settings()["kitType"] = "Assault";
	world::ObjectManager objects;
	game::GameWorld round(templates, objects);
	round.startRound();
	const uint32_t mine = objects.create(soldierTemplate, {0, 0, 0})->id();
	const uint32_t friendBody = objects.create(soldierTemplate, {0, 0, 20})->id();
	const uint32_t ordinaryBody = objects.create(soldierTemplate, {2, 0, 15})->id();
	const uint32_t trackerBody = objects.create(soldierTemplate, {0, 0, -10})->id();
	const uint32_t assaultKit = objects.create(kitTemplate, {0, 0, 0})->id();
	// The server's world as the HUD has it.
	struct Joined : host::HudWorld {
		std::vector<PlayerInfo> players;
		std::vector<SquadRequest> requests;
		bool joined() const override { return true; }
		int localPlayer() const override { return 0; }
		bool playerInfo(int player, PlayerInfo& out) const override
		{
			for (const PlayerInfo& p : players) {
				if (p.id == player) {
					out = p;
					return true;
				}
			}
			return false;
		}
		std::vector<PlayerInfo> playerList() const override { return players; }
		Scores scores(int) const override { return {}; }
		float tickets(int) const override { return 0; }
		int commander(int) const override { return 0; }
		std::vector<ControlPointInfo> controlPointList() const override { return {}; }
		std::vector<int> spawnablePoints(int) const override { return {}; }
		int spawnChoice(int) const override { return -1; }
		std::vector<VehicleInfo> vehicleList() const override { return {}; }
		std::vector<int> vehicleOccupants(uint32_t) const override { return {}; }
		float hitPoints(uint32_t) const override { return 100; }
		float objectHealth(uint32_t) const override { return 1; }
		bool squadOrder(int, int, bool, SquadOrder&) const override { return false; }
		std::vector<SquadRequest> squadRequests(int) const override { return requests; }
		const world::Object* soldier() const override { return nullptr; }
		const world::Object* vehicle() const override { return nullptr; }
		float health() const override { return 1; }
		Weapon heldWeapon() const override { return {}; }
		Weapon seatGroupWeapon(int) const override { return {}; }
	} joined;
	game::GameWorld::PlayerInfo me;
	me.id = 0;
	me.team = 1;
	me.alive = true;
	me.commander = true;
	me.soldier = mine;
	me.name = "Cmdr";
	game::GameWorld::PlayerInfo mate = me;
	mate.id = 1;
	mate.commander = false;
	mate.squad = 1;
	mate.squadLeader = true;
	mate.soldier = friendBody;
	mate.position = {0, 0, 20};
	mate.name = "Leader";
	game::GameWorld::PlayerInfo ordinary = mate;
	ordinary.id = 2;
	ordinary.squad = 0;
	ordinary.squadLeader = false;
	ordinary.soldier = ordinaryBody;
	ordinary.position = {2, 0, 15};
	ordinary.kit = assaultKit;
	ordinary.name = "Teammate";
	game::GameWorld::PlayerInfo tracker = ordinary;
	tracker.id = 5;
	tracker.soldier = trackerBody;
	tracker.position = {0, 0, -10};
	tracker.name = "Offscreen Medic";
	game::GameWorld::PlayerInfo commander = mate;
	commander.id = 3;
	commander.commander = true;
	commander.squadLeader = false;
	commander.squad = 0;
	commander.soldier = objects.create(soldierTemplate, {-5, 0, 25})->id();
	commander.position = {-5, 0, 25};
	commander.name = "Commander";
	joined.players = {me, mate, ordinary, commander, tracker};
	joined.requests = {{1, 1, 3, {0, 0, 30}, 0}};
	nametags::IconRegistry icons;
	icons.icons[0x5a] = {"ordinary-player.dds", 0, 0, 1, 1, 16, 16};
	icons.icons[0x6a] = {"squad-leader.dds", 0, 0, 1, 1, 16, 16};
	icons.icons[0x69] = {"commander.dds", 0, 0, 1, 1, 16, 16};
	icons.icons[0x2d] = {"medic-help.dds", 0, 0, 1, 1, 16, 16};
	icons.icons[nametags::requestIconId(3, false)] = {"supplyRequest.dds", 0, 0, 1, 1, 16, 16};
	icons.icons[nametags::requestIconId(3, true)] = {"supplyRequestSelected.dds", 0, 0, 1, 1, 16, 16};
	nametags::Nametags tags(files);
	tags.setIcons(&icons);
	nametags::Nametags::View view;
	view.position = {0, 1.6f, 0};
	const auto hasTexture = [](const std::vector<hud::HudQuad>& quads, const std::string& texture) {
		for (const hud::HudQuad& q : quads) {
			if (q.texture == texture) {
				return true;
			}
		}
		return false;
	};
	// The round alone: no local player, no tags.
	CHECK(tags.frame(round, objects, view, 1 / 30.0f).empty());
	tags.setWorld(&joined);
	std::vector<hud::HudQuad> quads;
	for (int i = 0; i < 30; ++i) {
		quads = tags.frame(round, objects, view, 1 / 30.0f);
	}
	// The friend's name (the font's glyphs) and the squad's request, which
	// the crosshair selects (it is straight ahead).
	CHECK(hasTexture(quads, "Fonts/BF1942.dds"));
	CHECK(hasTexture(quads, "ordinary-player.dds"));
	CHECK(hasTexture(quads, "squad-leader.dds"));
	CHECK(hasTexture(quads, "commander.dds"));
	CHECK(hasTexture(quads, "supplyRequest.dds") || hasTexture(quads, "supplyRequestSelected.dds"));
	CHECK(tags.selectedRequest() == 1 * 4 + 3);
	// This final frame follows multiple tag refreshes. Retail setup initially
	// clears show-icon bit 0x10, then its same-team updater enables it for a
	// friendly player; the native final-frame regression expects that result.
	// The ordinary teammate's feet-based PlayerInfo position is converted to
	// the retail soldier origin, then uses the standing icon offset of -0.6 m.
	// This intentionally places the marker below the independent name anchor.
	const hud::HudQuad* ordinaryIcon = nullptr;
	for (const hud::HudQuad& q : quads) {
		if (q.texture == "ordinary-player.dds") {
			ordinaryIcon = &q;
		}
	}
	CHECK(ordinaryIcon != nullptr);
	if (ordinaryIcon) {
		CHECK(std::fabs((ordinaryIcon->x0 + ordinaryIcon->x1) * 0.5f - 473.2f) < 1.0f);
		CHECK((ordinaryIcon->y0 + ordinaryIcon->y1) * 0.5f > 300.0f);
		float firstNameGlyph = 1e9f;
		for (const hud::HudQuad& q : quads) {
			if (q.texture == "Fonts/BF1942.dds") {
				firstNameGlyph = std::min(firstNameGlyph, q.y0);
			}
		}
		CHECK(firstNameGlyph < (ordinaryIcon->y0 + ordinaryIcon->y1) * 0.5f);
	}
	const auto ordinaryIconY = [&]() {
		float y = -1;
		for (const hud::HudQuad& q : quads) {
			if (q.texture == "ordinary-player.dds") {
				y = (q.y0 + q.y1) * 0.5f;
				break;
			}
		}
		return y;
	};
	const float standingIconY = ordinaryIconY();
	joined.players[2].pose = game::SoldierPose::Crouch;
	quads = tags.frame(round, objects, view, 1 / 30.0f);
	const float crouchedIconY = ordinaryIconY();
	joined.players[2].pose = game::SoldierPose::Prone;
	quads = tags.frame(round, objects, view, 1 / 30.0f);
	const float proneIconY = ordinaryIconY();
	CHECK(standingIconY > 0 && crouchedIconY > standingIconY);
	CHECK(std::fabs(proneIconY - crouchedIconY) < 0.1f);
	joined.players[2].pose = game::SoldierPose::Stand;
	quads = tags.frame(round, objects, view, 1 / 30.0f);
	// With the manager's shown byte false, retail's active radio/help flags
	// keep compatible medic response icons available.
	tags.setShown(false);
	tags.radioCall(0, nametags::CallMedic, true);
	for (int i = 0; i < 3; ++i) {
		quads = tags.frame(round, objects, view, 1 / 30.0f);
	}
	CHECK(hasTexture(quads, "medic-help.dds"));
	CHECK(hasTexture(quads, "Menu/Nametag/loactionArrow.dds"));
	CHECK(!hasTexture(quads, "ordinary-player.dds"));
	CHECK(!hasTexture(quads, "squad-leader.dds"));
	CHECK(!hasTexture(quads, "commander.dds"));
	// The name renderer is independent of the manager's icon shown byte.
	CHECK(hasTexture(quads, "Fonts/BF1942.dds"));
	CHECK(!hasTexture(quads, "supplyRequest.dds") && !hasTexture(quads, "supplyRequestSelected.dds"));
}

// An object tag's refresh (0x100f40d0): below healthActivationLevel (the
// Titans' 60) the secondary icon (slot 0x44(1), secondary3dMapIcon); the
// point raised by the object's radius with useBoundingRadius (1 by
// default, 0x0063b760).
TEST(Nametags_ObjectTagIconAndRadius)
{
	CHECK(nametags::objectTagIcon(49, 53, 60, 61) == 49);
	CHECK(nametags::objectTagIcon(49, 53, 60, 60) == 53);
	CHECK(nametags::objectTagIcon(49, 53, 60, 10) == 53);
	CHECK(nametags::objectTagIcon(49, 53, 100, 10) == 49); // 100: never
	const std::array<float, 3> lifted = nametags::objectTagPoint({1, 2, 3}, {0, -3, 0}, true, 4.5f);
	CHECK(lifted[0] == 1 && lifted[1] == 3.5f && lifted[2] == 3);
	const std::array<float, 3> flat = nametags::objectTagPoint({1, 2, 3}, {0, 0.3f, 0}, false, 4.5f);
	CHECK(std::fabs(flat[1] - 2.3f) < 1e-6f);
}

// Nametag.showSupply / showArmor (RendDX9 0x1016c1e0 at 0x1016c64f): the
// hubs' and MiniMed's storage bar for the team at any level; none for the
// enemy, for an engineer (the armour branch), for showArmor objects, or
// without a SupplyObject.
TEST(Nametags_ObjectSupplyBar)
{
	nametags::ObjectBarInput in;
	in.friendly = true;
	in.showSupply = true;
	in.hasSupply = true;
	in.supply = 0.8f;
	CHECK(nametags::objectTagBar(in) == 0.8f); // above half: still drawn
	in.supply = 0;
	CHECK(nametags::objectTagBar(in) == 0);
	in.friendly = false;
	CHECK(nametags::objectTagBar(in) < 0);
	in.friendly = true;
	in.localRepairs = true;
	CHECK(nametags::objectTagBar(in) < 0);
	in.localRepairs = false;
	in.hasSupply = false;
	CHECK(nametags::objectTagBar(in) < 0);
	in.hasSupply = true;
	in.showSupply = false;
	in.showArmor = true; // the sentry gun
	CHECK(nametags::objectTagBar(in) < 0);
	in.showArmor = false;
	CHECK(nametags::objectTagBar(in) < 0); // neither: not in the pass
}

// A vehicle tag's radius follows the LOD the camera's distance picks
// (0x100c3390 -> 0x100c3260 -> +0x268): LOD 0 within its switch distance
// (plus LOD 0's radius), then the next LODs' boxes; the zoom shortens the
// distance.
TEST(Nametags_RenderRadiusFollowsTheLod)
{
	geom::MeshFile mesh;
	mesh.geometries.resize(2);
	mesh.geometries[1].lods.resize(3);
	mesh.geometries[1].lods[0].bounds = {{0, 0, 0}, {6, 8, 0}}; // radius 5
	mesh.geometries[1].lods[1].bounds = {{0, 0, 0}, {3, 4, 0}}; // 2.5
	mesh.geometries[1].lods[2].bounds = {{0, 0, 0}, {0, 2, 0}}; // 1
	const nametags::MeshLods lods = nametags::meshLods(mesh, {{}, {30, 60}}, 1);
	CHECK(lods.radii.size() == 3 && lods.switchDistances.size() == 2);
	CHECK(nametags::lodRenderRadius(lods, 34, 1) == 5);   // 29 m past the radius
	CHECK(nametags::lodRenderRadius(lods, 36, 1) == 2.5f); // 31 m
	CHECK(nametags::lodRenderRadius(lods, 66, 1) == 1);
	CHECK(nametags::lodRenderRadius(lods, 66, 4) == 5); // zoomed in: 11.5 m
	const nametags::MeshLods halved = nametags::meshLods(mesh, {{}, {30, 60}}, 0.5f);
	CHECK(nametags::lodRenderRadius(halved, 21, 1) == 2.5f); // the low geometry quality
}

// A mesh's render radius (RendDX9 0x100c7000 on the instance 0x100c35c0
// makes): half the diagonal of the shown geometry's LOD 0 box, geometry 1
// when there are two or more (0x100c2b00). The order marker's.
TEST(Nametags_MeshRenderRadius)
{
	geom::MeshFile mesh;
	CHECK(nametags::meshRenderRadius(mesh) == 0);
	mesh.geometries.resize(1);
	mesh.geometries[0].lods.resize(1);
	mesh.geometries[0].lods[0].bounds = {{-1, 0, -2}, {1, 2, 2}};
	CHECK(std::fabs(nametags::meshRenderRadius(mesh) - std::sqrt(24.0f) * 0.5f) < 1e-5f);
	mesh.geometries.resize(2); // a 1p and a 3p geometry: the second shows
	mesh.geometries[1].lods.resize(1);
	mesh.geometries[1].lods[0].bounds = {{0, 0, 0}, {3, 4, 0}};
	CHECK(std::fabs(nametags::meshRenderRadius(mesh) - 2.5f) < 1e-5f);
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(dir);
	files.addSearchPath("mods/bf2142");
	files.setMountingAllowed(true);
	files.mountArchive("Objects/Weapons_client.zip", "Objects/Weapons/");
	std::vector<uint8_t> data;
	CHECK(files.readFile("Objects/Weapons/Handheld/ordermarker/meshes/ordermarker.bundledmesh", data));
	const auto marker = geom::parseMesh(data, geom::MeshKind::Bundled);
	CHECK(marker.has_value());
	if (marker) {
		const float r = nametags::meshRenderRadius(*marker);
		CHECK(std::fabs(r - 0.0815f) < 0.002f); // its two geometries' 0.11 x 0.044 x 0.11 m box
	}
}
