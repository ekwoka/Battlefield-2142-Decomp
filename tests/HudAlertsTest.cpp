// The HUD nodes the commander's and squad leader's map menus, the lock-on
// weapons and the slide-in effects need (transform lists, object markers,
// move and blend effects, mouse areas, alternate and show commands, the
// pointer and minimap settings), and the alerts: the vehicles' missile
// and mine warnings and the help pop-ups (src/Main/HudAlerts).
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Hud/Hud.h"
#include "IO/File/FileManager.h"
#include "Main/HudAlerts.h"
#include "TestMain.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"
#include "World/Object.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

void run(Console& console, std::initializer_list<const char*> lines)
{
	std::string out;
	for (const char* line : lines) {
		console.execute(line, out);
	}
}

const hud::HudQuad* quadWith(const std::vector<hud::HudQuad>& quads, const std::string& texture, size_t nth = 0)
{
	for (const hud::HudQuad& q : quads) {
		if (q.texture.find(texture) != std::string::npos && nth-- == 0) {
			return &q;
		}
	}
	return nullptr;
}

} // namespace

TEST(HudNodes_TransformListStacksAtItsVariables)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	run(console, {
					 "hudBuilder.createSplitNode Global Menu",
					 "hudBuilder.createButtonNode Menu RowA 370 20 197 22",
					 "hudBuilder.setButtonNodeTexture 1 a.tga",
					 "hudBuilder.createPictureNode Menu Gap 370 20 197 8",
					 "hudBuilder.setPictureNodeTexture gap.tga",
					 "hudBuilder.createButtonNode Menu RowB 370 20 197 22",
					 "hudBuilder.setButtonNodeTexture 1 b.tga",
					 "hudBuilder.createTextNode Menu Label 440 25 197 22",
					 "hudBuilder.setNodeOffset 0 1",
					 // parent name x y w h spacing (the last argument)
					 "hudBuilder.createTransformListNode Menu Rows 285 35 500 495 0",
					 "hudBuilder.setTranformListNodePosVariable 0 OrderMenuPosX",
					 "hudBuilder.setTranformListNodePosVariable 1 OrderMenuPosY",
					 "hudBuilder.addTransformListNode RowA",
					 "hudBuilder.addTransformListNode Gap",
					 "hudBuilder.addTransformListNode RowB",
					 "hudBuilder.createTransformListNode Menu Labels 285 35 500 495 7",
					 "hudBuilder.setTranformListNodePosVariable 0 OrderMenuPosX",
					 "hudBuilder.setTranformListNodePosVariable 1 OrderMenuPosY",
					 "hudBuilder.setTranformListNodeOffset 0 -2",
					 "hudBuilder.addTransformListNode Label",
				 });
	// Before the game sets the variables the list stays where it was made.
	auto quads = manager.frame(0);
	const hud::HudQuad* a = quadWith(quads, "a.tga");
	CHECK(a && a->x0 == 285 && a->y0 == 35);
	manager.set("OrderMenuPosX", 300.7f); // the variables are ints
	manager.set("OrderMenuPosY", 100);
	quads = manager.frame(0);
	a = quadWith(quads, "a.tga");
	const hud::HudQuad* gap = quadWith(quads, "gap.tga");
	const hud::HudQuad* b = quadWith(quads, "b.tga");
	CHECK(a && gap && b);
	if (a && gap && b) {
		CHECK(a->x0 == 300 && a->y0 == 100 && a->x1 == 497);
		CHECK(gap->y0 == 122);        // under the first row (22 high, no spacing)
		CHECK(b->x0 == 300 && b->y0 == 130); // under the 8 high gap
	}
	// The block is kept on the screen: 52 rows' worth would leave the bottom.
	manager.set("OrderMenuPosX", 700);
	manager.set("OrderMenuPosY", 590);
	quads = manager.frame(0);
	a = quadWith(quads, "a.tga");
	b = quadWith(quads, "b.tga");
	CHECK(a && a->x0 == 800 - 197 && a->y0 == 600 - 52);
	CHECK(b && b->y1 == 600);
}

TEST(HudNodes_MoveBlendAndBlinkEffects)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	run(console, {
					 "hudBuilder.createSplitNode Global Slide",
					 "hudBuilder.setNodeShowVariable SlideShow",
					 "hudBuilder.setNodeInTime 1",
					 "hudBuilder.setNodeOutTime 1",
					 "hudBuilder.addNodeMoveShowEffect -1.57 30",
					 "hudBuilder.createPictureNode Slide Panel 100 200 50 50",
					 "hudBuilder.setPictureNodeTexture panel.tga",
					 "hudBuilder.createSplitNode Global Additive",
					 "hudBuilder.addNodeBlendEffect 7 2",
					 "hudBuilder.createPictureNode Additive Glow 0 0 10 10",
					 "hudBuilder.setPictureNodeTexture glow.tga",
					 "hudBuilder.createPictureNode Global Blinker 0 0 10 10",
					 "hudBuilder.setPictureNodeTexture blink.tga",
					 "hudBuilder.setNodeShowVariable BlinkShow",
					 "hudBuilder.setNodeTimesBlink 2 0",
					 "hudBuilder.setNodeIntervalBlink 0.4",
				 });
	auto quads = manager.frame(0); // hidden at the start
	CHECK(!quadWith(quads, "panel.tga"));
	const hud::HudQuad* glow = quadWith(quads, "glow.tga");
	CHECK(glow && glow->blend[0] == 7 && glow->blend[1] == 2);
	manager.set("SlideShow", 1);
	quads = manager.frame(0.5f);
	const hud::HudQuad* panel = quadWith(quads, "panel.tga");
	// Half way in: 15 px of the 30 still to go, upwards (-pi/2), fully opaque
	// (no alpha effect).
	CHECK(panel && std::abs(panel->y0 - (200 - 15)) < 0.1f && std::abs(panel->x0 - 100) < 0.1f);
	CHECK(panel && panel->color[3] == 1);
	CHECK(panel && panel->blend[0] == 0);
	quads = manager.frame(0.6f);
	panel = quadWith(quads, "panel.tga");
	CHECK(panel && panel->y0 == 200);
	// Blinking: on, off, on, off, then on for good.
	manager.set("BlinkShow", 1);
	std::vector<bool> seen;
	for (int i = 0; i < 6; ++i) {
		seen.push_back(quadWith(manager.frame(i == 0 ? 0.0f : 0.5f), "blink.tga") != nullptr);
	}
	CHECK((seen == std::vector<bool> {true, false, true, false, true, true}));
}

TEST(HudNodes_ButtonsMouseOverAndShowCommands)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::vector<std::string> ran;
	manager.setCommandRunner([&ran](const std::string& line) { ran.push_back(line); });
	run(console, {
					 "hudBuilder.createButtonNode Global Row 100 100 197 22",
					 "hudBuilder.setButtonNodeTexture 1 row.tga",
					 "hudBuilder.setButtonNodeMouseArea 0 0 120 15",
					 "hudBuilder.setButtonNodeConCmd \"Commander.sendOrder 6 1\" 0",
					 "hudBuilder.setButtonNodeAltConCmd \"CommanderMenu.deselect\" 0",
					 "hudBuilder.createMouseOverNode Global Drop 300 300 60 40",
					 "hudBuilder.setMouseOverNodeConCmd \"kitInterface.selectDroppedItem 1\" 3",
					 "hudBuilder.createPictureNode Global Pod 0 0 10 10",
					 "hudBuilder.setPictureNodeTexture pod.tga",
					 "hudBuilder.setNodeShowVariable PodShow",
					 "hudBuilder.setNodeShowConCmd \"sound.playSound POD_power_up\" 1",
					 "hudBuilder.setNodeShowConCmd \"sound.playSound POD_power_down\" 0",
				 });
	manager.frame(0);
	CHECK(!manager.click(150, 118)); // inside the node, outside its mouse area
	CHECK(manager.click(110, 105));
	CHECK(manager.click(110, 105, true));
	CHECK((ran == std::vector<std::string> {"Commander.sendOrder 6 1", "CommanderMenu.deselect"}));
	ran.clear();
	manager.click(320, 320); // a release over the drop target
	CHECK((ran == std::vector<std::string> {"kitInterface.selectDroppedItem 1"}));
	ran.clear();
	manager.set("PodShow", 1);
	manager.frame(0);
	manager.frame(0);
	manager.set("PodShow", 0);
	manager.frame(0);
	CHECK((ran == std::vector<std::string> {"sound.playSound POD_power_up", "sound.playSound POD_power_down"}));
}

TEST(HudNodes_ObjectMarkersAndLabels)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	run(console, {
					 "hudBuilder.createSplitNode Global Targets",
					 "hudBuilder.setNodeLogicShowVariable NOT AffectedByEMPShow 1", // drawn in one pass
					 "hudBuilder.createTextNode Targets Label0 100 100 40 10",
					 "hudBuilder.createTextNode Targets Label1 100 120 40 10",
					 "hudBuilder.createObjectMarkerNode Targets Markers 120 190 564 224",
					 "hudBuilder.setObjectMarkerNodeTexture 0 friendly.tga",
					 "hudBuilder.setObjectMarkerNodeTexture 1 locking.tga",
					 "hudBuilder.setObjectMarkerNodeTexture 2 locked.tga",
					 "hudBuilder.setObjectMarkerNodeTextureSize 0 32 32",
					 "hudBuilder.setObjectMarkerNodeTextureSize 1 32 32",
					 "hudBuilder.setObjectMarkerNodeTextureSize 2 64 64",
					 "hudBuilder.setObjectMarkerNodeObjects 2",
					 "hudBuilder.setObjectMarkerNodeLockOnType 1",
					 "hudBuilder.setObjectMarkerNodeWeapon 1",
					 "hudBuilder.setObjectMarkerNodeLockText 1 Label",
					 "hudBuilder.setObjectMarkerNodeLockTextOffset -20 11",
					 "hudBuilder.addObjectMarkerNodeLockTextNode Label0",
					 "hudBuilder.addObjectMarkerNodeLockTextNode Label1",
					 // The label text is the "<name>TextNode" of the exe's pair.
					 "hudBuilder.setActiveObject Targets Label0TextNode",
					 "hudBuilder.setTextNodeStringVariable Label0String",
				 });
	hud::HudManager::MarkerQuery asked;
	manager.setMarkerSource([&asked](const hud::HudManager::MarkerQuery& query) {
		asked = query;
		return std::vector<hud::HudManager::MarkerTarget> {{400, 300, 2, 120.2f}, {200, 250, 0, 80}, {500, 260, 0, 90}};
	});
	const auto quads = manager.frame(0);
	CHECK(asked.lockOnType == 1 && asked.weapon == 1 && asked.maxObjects == 2);
	CHECK(asked.x0 == 120 && asked.y0 == 190 && asked.x1 == 684 && asked.y1 == 414);
	const hud::HudQuad* locked = quadWith(quads, "locked.tga");
	const hud::HudQuad* candidate = quadWith(quads, "friendly.tga");
	CHECK(locked && locked->x0 == 368 && locked->y0 == 268 && locked->x1 == 432);
	CHECK(candidate && candidate->x0 == 184 && candidate->y0 == 234);
	CHECK(!quadWith(quads, "friendly.tga", 1)); // at most two
	CHECK(manager.string("Label0String") == "121" && manager.number("Label0Show") == 1);
	CHECK(manager.string("Label1String") == "80" && manager.number("Label2Show") == 0);
	// The first label beside the locked target: its point + half its size + the offset.
	manager.setMarkerSource({});
	manager.frame(0);
	CHECK(manager.find("Label0") && manager.find("Label0") == manager.find("label0"));
}

TEST(HudNodes_PointerAndMinimapSettings)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	run(console, {
					 "hudManager.setMouseTextureArtillery Ingame/GeneralIcons/crosshair_placeAirStrike.tga",
					 "hudManager.setMouseTextureFull Ingame/GeneralIcons/pointerMinimap.tga",
					 "hudManager.setPointerSize 25 25",
					 "hudManager.setCommPos 150 150",
					 "hudManager.setSpottedMenuSize 83 130",
					 "MiniMap.setZoom",
					 "Minimap.setPaintKit 2",
					 "Minimap.setPaintAllVehicles",
				 });
	using Pointer = hud::HudManager::Pointer;
	hud::HudQuad q;
	CHECK(!manager.pointerQuad(Pointer::Artillery, q)); // no pointer yet
	manager.setPointer(400, 300);
	CHECK(manager.pointerQuad(Pointer::Artillery, q));
	CHECK(q.texture == "Menu/HUD/Texture/Ingame/GeneralIcons/crosshair_placeAirStrike.tga" && q.x0 == 387.5f && q.x1 == 412.5f);
	CHECK(!manager.pointerQuad(Pointer::Uav, q));
	CHECK(manager.setting("HudManager.setCommPos")[0] == 150 && manager.setting("HudManager.setSpottedMenuSize")[1] == 130);
	CHECK(manager.mapZoom() == 2); // 1 at the start, one step
	std::string out;
	console.execute("Minimap.setZoom", out);
	CHECK(manager.mapZoom() == 0);
	CHECK(!manager.paintsKit(2) && manager.paintsKit(1));
	CHECK(!manager.paintsVehicle(3));
	console.execute("Minimap.setPaintAllKits", out); // not all on: all on
	CHECK(manager.paintsKit(2));
}

TEST(HudAlerts_MissileAndMineWarnings)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "PlayerControlObject");
	world::registerWorldConsoleMethods(console, templates, objects);
	menu::registerWarningHudMethods(console, templates);
	run(console, {
					 "ObjectTemplate.create PlayerControlObject tank",
					 "ObjectTemplate.createComponent WarningHud",
					 "ObjectTemplate.WarningHud.setWarningType TTMagnet 1",
					 "ObjectTemplate.WarningHud.setFirstWarningSound TTMagnet HUD_MotionMine_Warning",
					 "ObjectTemplate.WarningHud.setSecondWarningSound TTMagnet HUD_MotionMine_Chase",
					 "ObjectTemplate.WarningHud.setWarningIcon TTMagnet Ingame\\GeneralIcons\\Action_Icons\\large_mineicon.tga",
					 "ObjectTemplate.WarningHud.setWarningType TTLaser 1",
					 "ObjectTemplate.WarningHud.setWarningIcon TTLaser laser.tga",
					 "ObjectTemplate.WarningHud.setWarningType TTHeat 0",
				 });
	const world::ObjectTemplate* tank = templates.find("tank");
	CHECK(tank != nullptr);
	if (!tank) {
		return;
	}
	CHECK(menu::targetTypeIndex("TTMagnet") == 3 && menu::targetTypeIndex("ttunique") == 4 && menu::targetTypeIndex("x") == -1);
	const menu::Warning magnet = menu::warningFor(*tank, 3);
	CHECK(magnet.warns && magnet.firstSound == "HUD_MotionMine_Warning");
	hud::HudManager hudManager(nullptr);
	menu::WarningHud warnings;
	std::vector<std::string> sounds;
	warnings.setSoundHook([&sounds](const std::string& sound, bool start) { sounds.push_back((start ? "+" : "-") + sound); });
	warnings.update(hudManager, tank, {{1, 2, true}}); // a heat seeker: the tank doesn't warn of those
	CHECK(hudManager.number("WarningIconShow") == 0 && sounds.empty());
	warnings.update(hudManager, tank, {{1, 3, false}}); // a mine locked on
	CHECK(hudManager.number("WarningIconShow") == 1);
	CHECK(hudManager.string("WarningIconPath") == "Ingame\\GeneralIcons\\Action_Icons\\large_mineicon.tga");
	warnings.update(hudManager, tank, {{1, 3, true}}); // chasing: the second tone
	warnings.update(hudManager, tank, {});              // gone
	CHECK(hudManager.number("WarningIconShow") == 0);
	CHECK((sounds == std::vector<std::string> {"+HUD_MotionMine_Warning", "-HUD_MotionMine_Warning", "+HUD_MotionMine_Chase",
					 "-HUD_MotionMine_Chase"}));
	// TTUnique warns when TTLaser does.
	warnings.update(hudManager, tank, {{7, 4, false}});
	CHECK(hudManager.number("WarningIconShow") == 1 && hudManager.string("WarningIconPath").empty());
	warnings.update(hudManager, nullptr, {{7, 4, false}}); // on foot: nothing
	CHECK(hudManager.number("WarningIconShow") == 0);
}

TEST(HudAlerts_HelpPopups)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "PlayerControlObject");
	world::registerWorldConsoleMethods(console, templates, objects);
	world::registerUntypedTemplateProperties(console, templates,
		{"helpHud.helpStringKey", "helpHud.helpSoundKey", "helpHud.lowArmorHelpStringKey", "helpHud.closeToDistance",
			"helpHud.closeToHelpStringKeyEnemy", "armor.criticalDamage"});
	menu::HelpPopups help;
	help.bind(console);
	run(console, {
					 "ObjectTemplate.create PlayerControlObject fav",
					 "ObjectTemplate.HelpHud.helpStringKey \"HUD_HELP_VEHICLE_FAV_speedboost\"",
					 "ObjectTemplate.HelpHud.helpSoundKey \"HUD_HELP_VEHICLE_FAV_speedboost_vo\"",
					 "ObjectTemplate.HelpHud.lowArmorHelpStringKey \"HUD_HELP_LOWHEALTH\"",
					 "ObjectTemplate.armor.criticalDamage 20",
					 "ObjectTemplate.create PlayerControlObject artillery",
					 "ObjectTemplate.HelpHud.closeToDistance 5",
					 "ObjectTemplate.HelpHud.closeToHelpStringKeyEnemy \"HUD_HELP_WORLD_PLAYER_ARTILLERY_enemy\"",
				 });
	const world::ObjectTemplate* fav = templates.find("fav");
	const world::ObjectTemplate* artillery = templates.find("artillery");
	CHECK(fav && artillery);
	if (!fav || !artillery) {
		return;
	}
	hud::HudManager hudManager(nullptr);
	std::vector<std::string> voices;
	help.setVoiceHook([&voices](const std::string& key) { voices.push_back(key); });
	help.setLocalizer([](const std::string& key) { return "<" + key + ">"; });
	menu::HelpPopups::Context context;
	context.vehicle = fav;
	context.vehicleHitPoints = 100;
	help.update(hudManager, context, 0.1f);
	CHECK(hudManager.number("IngameHelpShow") == 1);
	CHECK(hudManager.string("IngameHelpString") == "<HUD_HELP_VEHICLE_FAV_speedboost>");
	CHECK((voices == std::vector<std::string> {"HUD_HELP_VEHICLE_FAV_speedboost_vo"}));
	help.update(hudManager, context, menu::HelpPopups::kShowSeconds);
	CHECK(hudManager.number("IngameHelpShow") == 0); // the timer ran out
	// Out and in again: shown once a round.
	help.update(hudManager, {}, 0.1f);
	help.update(hudManager, context, 0.1f);
	CHECK(hudManager.number("IngameHelpShow") == 0);
	// The armour falls below 0.3 of its maximum (0x00646cb0 case 0x3a).
	context.vehicleHitPoints = 35;
	context.vehicleShare = 0.35f;
	help.update(hudManager, context, 0.1f);
	CHECK(hudManager.string("IngameHelpString") != "<HUD_HELP_LOWHEALTH>");
	context.vehicleHitPoints = 15;
	context.vehicleShare = 0.15f;
	help.update(hudManager, context, 0.1f);
	CHECK(hudManager.string("IngameHelpString") == "<HUD_HELP_LOWHEALTH>");
	// Near an enemy's artillery, on foot.
	context = {};
	context.nearby.push_back({artillery, 7, 2});
	help.update(hudManager, context, 0.1f);
	CHECK(hudManager.string("IngameHelpString") == "<HUD_HELP_LOWHEALTH>"); // 7 m: too far
	context.nearby.front().distance = 4;
	help.update(hudManager, context, 0.1f);
	CHECK(hudManager.string("IngameHelpString") == "<HUD_HELP_WORLD_PLAYER_ARTILLERY_enemy>");
	// Shown keys go on the played list, and played keys don't show again
	// next round (GeneralSettings.setPlayedVOHelp).
	std::vector<std::string> played;
	help.onPlayed = [&played](const std::string& key) { played.push_back(key); };
	CHECK(help.played("HUD_HELP_WORLD_PLAYER_ARTILLERY_enemy"));
	std::string out;
	console.execute("GeneralSettings.setPlayedVOHelp \"HUD_HELP_OLD\"", out);
	help.reset();
	CHECK(!help.show(hudManager, "HUD_HELP_OLD"));
	CHECK(help.show(hudManager, "HUD_HELP_NEW"));
	CHECK((played == std::vector<std::string> {"HUD_HELP_NEW"}));
	// GeneralSettings.setHelpPopups 0: none.
	console.execute("GeneralSettings.setHelpPopups 0", out);
	help.reset();
	CHECK(!help.show(hudManager, "HUD_HELP_ANYTHING"));
}

// The weapon's alt and low-ammunition keys (0x00646cb0 cases 0x50, 0x3b)
// and the help manager's own messages (0x00645d40, ticked by 0x00649d80).
TEST(HudAlerts_AltLowAmmoAndTutorial)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "GenericFireArm");
	world::registerWorldConsoleMethods(console, templates, objects);
	world::registerUntypedTemplateProperties(console, templates,
		{"helpHud.altHelpStringKey", "helpHud.altHelpSoundKey", "helpHud.lowAmmoHelpStringKey", "helpHud.lowAmmoHelpSoundKey"});
	menu::HelpPopups help;
	help.bind(console);
	run(console, {
					 "ObjectTemplate.create GenericFireArm rifle",
					 "ObjectTemplate.HelpHud.altHelpStringKey \"HUD_HELP_RIFLE_zoom\"",
					 "ObjectTemplate.HelpHud.lowAmmoHelpStringKey \"HUD_HELP_RIFLE_lowAmmo\"",
					 "ObjectTemplate.HelpHud.lowAmmoHelpSoundKey \"rifle_low_vo\"",
				 });
	const world::ObjectTemplate* rifle = templates.find("rifle");
	CHECK(rifle);
	if (!rifle) {
		return;
	}
	hud::HudManager hudManager(nullptr);
	std::vector<std::string> voices;
	help.setVoiceHook([&voices](const std::string& key) { voices.push_back(key); });
	menu::HelpPopups::Context context;
	context.weapon = rifle;
	help.update(hudManager, context, 0.1f);
	CHECK(hudManager.number("IngameHelpShow") == 0); // no helpStringKey
	context.zoomed = true;
	help.update(hudManager, context, 0.1f);
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_RIFLE_zoom");
	context.ammo.push_back({rifle, 0.5f});
	help.update(hudManager, context, 0.1f);
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_RIFLE_zoom");
	context.ammo.front().share = 0.2f;
	help.update(hudManager, context, 0.1f);
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_RIFLE_lowAmmo");
	CHECK(!voices.empty() && voices.back() == "rifle_low_vo");

	// The tutorial: nothing until the round plays; then the first message
	// that holds (on foot, no menu: move the mouse to look), shown 9 s with
	// no voice; the next only 10 s later.
	menu::HelpPopups tutor;
	std::vector<std::string> said;
	tutor.setVoiceHook([&said](const std::string& key) { said.push_back(key); });
	menu::HelpPopups::Context round;
	round.onFoot = round.alive = true;
	CHECK(!tutor.tutorial(hudManager, round, 0.1f));
	round.playing = true;
	round.roundTime = 1;
	CHECK(tutor.tutorial(hudManager, round, 0.1f));
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_PLAYER_CONTROLS_moveMouseToLook");
	CHECK(said.empty());
	CHECK(!tutor.tutorial(hudManager, round, 5));
	tutor.update(hudManager, {}, menu::HelpPopups::kShowSeconds);
	CHECK(hudManager.number("IngameHelpShow") == 0);
	CHECK(tutor.tutorial(hudManager, round, 6)); // 11 s on: moving needs 3 s of the round (it is 1): firing
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_PLAYER_CONTROLS_fireLeftMouse");
	CHECK((said == std::vector<std::string> {"HUD_HELP_PLAYER_CONTROLS_fireLeftMouse"}));
	// Played keys are done at the start (0x006467f0).
	Console settings;
	menu::HelpPopups veteran;
	veteran.bind(settings);
	std::string out;
	settings.execute("GeneralSettings.setPlayedVOHelp \"HUD_HELP_PLAYER_CONTROLS_moveMouseToLook\"", out);
	CHECK(veteran.tutorial(hudManager, round, 0.1f));
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_PLAYER_CONTROLS_fireLeftMouse");
}

// The game's commander menu (HudElementsCommanderMenuConquest.con): its
// rows stacked at the right click, under the marker.
TEST(HudNodes_GameCommanderMapMenu)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	Console console(&files);
	bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
		[&files](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
	std::string out;
	console.run("ServerArchives.con", {}, out);
	console.run("ClientArchives.con", {}, out);
	hud::HudManager manager([&files](const std::string& path, std::string& text) {
		std::vector<uint8_t> data;
		if (!files.readFile(path, data)) {
			return false;
		}
		text.assign(data.begin(), data.end());
		return true;
	});
	hud::registerHudConsoleMethods(console, manager);
	console.setScriptOutput(false, false);
	console.run("Menu/HUD/HudSetup/HudSetupMain.con", {}, out);
	for (const char* name : {"ShowIngameHud", "ShowSoldierHud", "CommanderShow", "CommanderMenuShow", "MapShow", "MapFullSize"}) {
		manager.set(name, 1);
	}
	manager.set("MinimapRMBMenuState", 7); // nothing under the click
	manager.set("OrderMenuPosX", 300);
	manager.set("OrderMenuPosY", 200);
	manager.frame(0);
	const auto quads = manager.frame(1);
	std::vector<const hud::HudQuad*> rows;
	for (const hud::HudQuad& q : quads) {
		if (q.texture.find("Commander/rmb_menu_Bg") != std::string::npos) {
			rows.push_back(&q);
		}
	}
	std::fprintf(stderr, "  %zu menu rows\n", rows.size());
	CHECK(rows.size() >= 4);
	// All at the click's x, stacked downwards from its y.
	float top = 1e9f;
	std::vector<float> ys;
	for (const hud::HudQuad* row : rows) {
		CHECK(row->x0 == 300);
		top = std::min(top, row->y0);
		ys.push_back(row->y0);
	}
	std::sort(ys.begin(), ys.end());
	CHECK(top == 200);
	CHECK(std::adjacent_find(ys.begin(), ys.end()) == ys.end());
	const hud::HudQuad* marker = quadWith(quads, "crosshair_comrose");
	CHECK(marker && marker->x0 == 280 && marker->y0 == 180); // -20 -20 from the click
}

// The help manager's menu-state, ladder and vehicle messages (0x00646cb0
// cases 0x0a, 0x19, 0x1d / 0x2d, 0x43, 0x47) and the profile's play time.
TEST(HudAlerts_TutorialMenuStatesAndReach)
{
	hud::HudManager hudManager(nullptr);
	menu::HelpPopups tutor;
	menu::HelpPopups::Context c;
	c.playing = true;
	c.spawnScreen = true;
	c.menuState = 0x13; // the members half, not leading a squad
	CHECK(tutor.tutorial(hudManager, c, 0.1f));
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_SPAWNSCREEN_SQUADTAB_createSquad");
	// A ladder in reach on foot with a menu up (the movement ones need none).
	menu::HelpPopups ladder;
	menu::HelpPopups::Context l;
	l.playing = l.onFoot = l.alive = l.ladderNear = true;
	l.menuOpen = true;
	CHECK(ladder.tutorial(hudManager, l, 0.1f));
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_GAMEPLAY_climbLadder");
	l.ladderNear = false;
	l.vehicleNear = true;
	ladder.update(hudManager, {}, menu::HelpPopups::kShowSeconds);
	CHECK(ladder.tutorial(hudManager, l, 10.5f));
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_VEHICLE_GENERAL_CONTROLS_enterVehicle");
	// The squad leader's page (0x12): case 0x1d shows 0x2d's text.
	menu::HelpPopups leader;
	menu::HelpPopups::Context s;
	s.playing = true;
	s.menuState = 0x12;
	s.squad = 1;
	s.squadLeader = true;
	s.menuOpen = true;
	CHECK(leader.tutorial(hudManager, s, 0.1f));
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_SPAWNSCREEN_SQUADTAB_applyCommander");
	leader.update(hudManager, {}, menu::HelpPopups::kShowSeconds);
	CHECK(leader.tutorial(hudManager, s, 10.5f));
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_SQUAD_LEADER_mainMapRightMouseMenu");
	// Past its play-time limit a message is done from the start (0x006467f0).
	menu::HelpPopups old;
	menu::HelpPopups::Context o;
	o.playing = o.onFoot = o.alive = true;
	o.roundTime = 1;
	o.playTime = 1e6f;
	CHECK(old.tutorial(hudManager, o, 0.1f));
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_PLAYER_CONTROLS_moveMouseToLook");
	old.update(hudManager, {}, menu::HelpPopups::kShowSeconds);
	CHECK(old.tutorial(hudManager, o, 10.5f));
	CHECK(hudManager.string("IngameHelpString") != "HUD_HELP_PLAYER_CONTROLS_jump");
}

// Event-only messages wait for the HUD's slot 0x128 (0x00751540).
TEST(HudAlerts_TutorialEvents)
{
	hud::HudManager hudManager(nullptr);
	menu::HelpPopups tutor;
	menu::HelpPopups::Context c;
	c.playing = true;
	c.menuOpen = true; // none of the tick's own hold
	CHECK(!tutor.tutorial(hudManager, c, 0.1f));
	tutor.enableTutorial(0x33);
	CHECK(tutor.tutorial(hudManager, c, 0.1f));
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_GAMEPLAY_captureFlag");
	tutor.update(hudManager, {}, menu::HelpPopups::kShowSeconds);
	tutor.enableTutorial(0x18);
	tutor.disableTutorial(0x18);
	CHECK(!tutor.tutorial(hudManager, c, 10.5f));
	c.commander = true;
	tutor.enableTutorial(0x18);
	CHECK(tutor.tutorial(hudManager, c, 0.1f));
	CHECK(hudManager.string("IngameHelpString") == "HUD_HELP_COMMANDER_commanderApply");
}

// The other raisers' messages: 0x39 bombardmentDamaged on the commander
// screen (0x007a9500), 0x16 UAVplaced never shown (no case in 0x00646cb0),
// 0x41 useUnlock on the spawn screen's kits (menu state 1), 0x17 / 0x22
// placement states for the event-only callForTarget's neighbours.
TEST(HudAlerts_TutorialEventCallers)
{
	hud::HudManager hudManager(nullptr);
	menu::HelpPopups tutor;
	menu::HelpPopups::Context c;
	c.playing = true;
	c.menuOpen = true;
	c.menuState = 0x10;
	tutor.enableTutorial(0x16);
	tutor.enableTutorial(0x39);
	CHECK(!tutor.tutorial(hudManager, c, 0.1f)); // not on the commander screen
	c.commanderScreen = true;
	// The commander screen's own messages come first; then the damaged
	// asset's, never UAVplaced.
	std::set<std::string> shown;
	for (int i = 0; i < 40; ++i) {
		if (tutor.tutorial(hudManager, c, 10.5f)) {
			shown.insert(hudManager.string("IngameHelpString"));
			tutor.update(hudManager, {}, menu::HelpPopups::kShowSeconds);
		}
	}
	CHECK(shown.count("HUD_HELP_COMMANDER_bombardmentDamaged") == 1);
	CHECK(shown.count("HUD_HELP_COMMANDER_UAVplaced") == 0);
	menu::HelpPopups kits;
	menu::HelpPopups::Context k;
	k.playing = true;
	k.spawnScreen = true;
	k.menuState = 0x13;
	kits.enableTutorial(0x41);
	kits.disableTutorial(0x3f);
	kits.disableTutorial(0x40);
	kits.disableTutorial(0x42);
	bool useUnlock = false;
	for (int i = 0; i < 3 && !useUnlock; ++i) {
		k.menuState = i == 2 ? 1 : 0x13;
		if (kits.tutorial(hudManager, k, 10.5f)) {
			useUnlock = hudManager.string("IngameHelpString") == "HUD_HELP_SPAWNSCREEN_KITTAB_useUnlock";
			CHECK(!useUnlock || k.menuState == 1);
			kits.update(hudManager, {}, menu::HelpPopups::kShowSeconds);
		}
	}
	CHECK(useUnlock);
}
