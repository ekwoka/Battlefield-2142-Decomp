// The HUD: fonts, and the game's HUD scripts building the node tree that
// variables then show, fill and label (the scripts need $BF2142_GAME_DIR).
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Hud/Hud.h"
#include "IO/File/FileManager.h"
#include "IO/Localization/Localization.h"
#include "TestMain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace dice::hfe;

TEST(Hud_ParsesFonts)
{
	const auto font = hud::parseHudFont("header\r\n2\r\ntest\r\n128\r\n64\r\n13.000000\r\nglyphs\r\n2\r\n"
										"65\t0.125000\t4.250000\t0.025000\t2\t44\t16\t49\t25\r\n"
										"86\t0.000000\t4.000000\t0.000000\t2\t50\t16\t55\t25\r\n"
										"kerning\r\n65\t86\t-0.375000\r\n");
	CHECK(font.has_value());
	if (!font) {
		return;
	}
	CHECK(font->textureWidth == 128 && font->textureHeight == 64 && font->lineHeight == 13);
	CHECK(font->glyph('A') && font->glyph('A')->top == 2 && font->glyph('A')->x1 == 49);
	CHECK(std::abs(font->width("AV") - (4.4f + 4.0f - 0.375f)) < 1e-4f);
}

TEST(Hud_ShowLogicAndBars)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {
			 "hudBuilder.createSplitNode Global GlobalHud",
			 "hudBuilder.setNodeShowVariable ShowIngameHud",
			 "hudBuilder.setNodeLogicShowVariable NOT AffectedByEMPShow 1",
			 "hudBuilder.createBarNode GlobalHud Health 3 100 500 200 10",
			 "hudBuilder.setBarNodeTexture 0 Ingame/empty.tga",
			 "hudBuilder.setBarNodeTexture 1 Ingame/full.tga",
			 "hudBuilder.setBarNodeValueVariable PlayerHealth",
			 "hudBuilder.setNodeLogicShowVariable NOT IsSpectator 1",
		 }) {
		console.execute(line, out);
	}
	CHECK(manager.frame(0).empty()); // ShowIngameHud is 0
	manager.set("ShowIngameHud", 1);
	manager.set("PlayerHealth", 0.25f);
	const auto quads = manager.frame(0);
	CHECK(quads.size() == 2);
	if (quads.size() == 2) {
		CHECK(quads[1].texture == "Menu/HUD/Texture/Ingame/full.tga");
		CHECK(quads[1].x0 == 100 && quads[1].x1 == 150 && quads[1].u1 == 0.25f);
	}
	manager.set("IsSpectator", 1);
	CHECK(manager.frame(0).empty());
}

TEST(Hud_PassesAndOrTerms)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {
			 // Drawn in the second pass only, for either weapon.
			 "hudBuilder.createSplitNode Global WeaponHuds",
			 "hudBuilder.setNodeShowVariable CrosshairIconShow",
			 "hudBuilder.setNodeLogicShowVariable AND AffectedByEMPShow 1",
			 "hudBuilder.createPictureNode WeaponHuds Cross 0 298 19 5",
			 "hudBuilder.setPictureNodeTexture cross.tga",
			 "hudBuilder.setNodeLogicShowVariable EQUAL GuiIndex 83",
			 "hudBuilder.setNodeLogicShowVariable OR GuiIndex 84",
			 "hudBuilder.setNodePosVariable 0 CrossLeftPos",
			 // In the first pass only, and only with the map open.
			 "hudBuilder.createPictureNode Global Spawn 10 10 16 16",
			 "hudBuilder.setPictureNodeTexture spawn.tga",
			 "hudBuilder.setNodeShowVariable MapShow",
			 "hudBuilder.setNodeLogicShowVariable NOT AffectedByEMPShow 1",
			 "hudBuilder.setNodeLogicShowVariable OR UpdateAllEnabled 1",
			 "hudBuilder.setNodeLogicShowVariable NOT IsSpectator 1",
		 }) {
		console.execute(line, out);
	}
	manager.set("CrosshairIconShow", 1);
	manager.set("GuiIndex", 84);
	manager.set("CrossLeftPos", 375);
	auto quads = manager.frame(0);
	CHECK(quads.size() == 1);
	if (quads.size() == 1) {
		CHECK(quads[0].texture == "Menu/HUD/Texture/cross.tga" && quads[0].x0 == 375 && quads[0].y0 == 298);
	}
	manager.set("GuiIndex", 12);
	CHECK(manager.frame(0).empty());
	manager.set("MapShow", 1);
	quads = manager.frame(0);
	CHECK(quads.size() == 1 && quads[0].texture == "Menu/HUD/Texture/spawn.tga");
	CHECK(manager.number("AffectedByEMPShow") == 0);

	// The profile's colour, as GeneralOptions.con writes it.
	console.execute("game.setCrossHairColor 256.000000 128.000000 0.000000", out);
	CHECK(manager.number("CrossHairColorRed") == 1 && std::abs(manager.number("CrossHairColorGreen") - 128 / 255.0f) < 1e-6f);
	CHECK(manager.number("CrossHairColorBlue") == 0 && manager.number("CrossHairColorAlpha") == 1);
}

TEST(Hud_GameHudScripts)
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
	std::fprintf(stderr, "  %zu HUD nodes\n", manager.nodeCount());
	CHECK(manager.nodeCount() > 2000);
	for (const char* name : {"ShowIngameHud", "ShowSoldierHud", "PlayerHealthShow", "PrimaryAmmoShow", "UsingChildHud",
			 "MenuBackgroundAlpha"}) {
		manager.set(name, 1);
	}
	manager.set("PlayerHealth", 0.8f);
	manager.setString("PlayerHealthString", "80");
	const auto quads = manager.frame(1 / 30.0f);
	const bool healthBar = std::any_of(quads.begin(), quads.end(),
		[](const hud::HudQuad& q) { return q.texture.find("healthBaseGradient") != std::string::npos; });
	const bool text = std::any_of(quads.begin(), quads.end(),
		[](const hud::HudQuad& q) { return q.texture.find("Fonts/") == 0; });
	std::fprintf(stderr, "  %zu quads (health bar %d, text %d)\n", quads.size(), healthBar, text);
	CHECK(healthBar);
	CHECK(text);
}

TEST(Hud_LocalizationAndLists)
{
	// strings.csv: UTF-16LE with a byte-order mark, tab-separated, a column per language.
	const std::u16string text = u"StringID\tEnglish\tGerman\r\nHUD_TEXT_MENU_SCORE_PLAYERS\tPLAYERS\tSPIELER\r\n"
								u"QUOTED\t\"a \u00e9 b\"\tx\r\n";
	std::vector<uint8_t> data {0xff, 0xfe};
	for (char16_t c : text) {
		data.push_back(static_cast<uint8_t>(c));
		data.push_back(static_cast<uint8_t>(c >> 8));
	}
	io::Localization english;
	CHECK(english.load(data, "English"));
	CHECK(english.size() == 2 && english.text("HUD_TEXT_MENU_SCORE_PLAYERS") == "PLAYERS");
	CHECK(english.text("QUOTED") == "a \xc3\xa9 b" && english.text("MISSING") == "MISSING");
	io::Localization german;
	CHECK(german.load(data, "German") && german.text("HUD_TEXT_MENU_SCORE_PLAYERS") == "SPIELER");
	CHECK(!io::Localization().load(data, "Klingon"));

	// A list node draws its background (header band first, beneath earlier
	// siblings) and its data source's rows at the columns.
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {
			 "hudBuilder.createSplitNode Global Board",
			 "hudBuilder.setNodeLogicShowVariable NOT AffectedByEMPShow 1",
			 "hudBuilder.createPictureNode Board Header 10 10 50 10",
			 "hudBuilder.setPictureNodeTexture header.tga",
			 "hudBuilder.createListNode Board Scores 10 10 200 100 19 1",
			 "hudBuilder.setListNodeData 2",
			 "hudBuilder.setListNodeBorder 20 22 3 8",
			 "hudBuilder.setListNodeBackgroundColor 0.5 0.5 0.5 1",
		 }) {
		console.execute(line, out);
	}
	manager.setListColumns(2, {{8, 100, 0}, {150, 30, 2}});
	manager.setList(2, {{"", ""}, {"", ""}});
	const auto quads = manager.frame(0);
	CHECK(quads.size() == 3);
	if (quads.size() == 3) {
		CHECK(quads[0].texture == "Menu/Console/white.dds" && quads[0].y0 == 10 && quads[0].y1 == 30);
		CHECK(quads[1].texture == "Menu/HUD/Texture/header.tga");
		CHECK(quads[2].texture == "Menu/Console/white.dds" && quads[2].y0 == 30 && quads[2].y1 == 110);
	}
}

TEST(Hud_CompassScrolls)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {
			 "hudBuilder.createCompassNode Global Compass 3 100 10 90 10 1 0",
			 "hudBuilder.setCompassNodeTexture 1 compass.tga",
			 "hudBuilder.setCompassNodeTextureSize 360 10",
			 "hudBuilder.setCompassNodeValueVariable VehicleAngle",
			 "hudBuilder.setNodeLogicShowVariable NOT AffectedByEMPShow 1",
		 }) {
		console.execute(line, out);
	}
	// 0x007e9560: the strip slides back by floor(value / 360 * 360): facing
	// east its pixels 90..180 show.
	manager.set("VehicleAngle", 90);
	auto quads = manager.frame(0);
	CHECK(quads.size() == 1);
	if (quads.size() == 1) {
		CHECK(quads[0].x0 == 100 && quads[0].x1 == 190);
		CHECK(std::abs(quads[0].u0 - 0.25f) < 1e-5f && std::abs(quads[0].u1 - 0.5f) < 1e-5f);
	}
	// 300 degrees is -60: the strip starts 60 pixels in and a copy a period
	// to the left fills the gap.
	manager.set("VehicleAngle", 300);
	quads = manager.frame(0);
	CHECK(quads.size() == 2);
	if (quads.size() == 2) {
		CHECK(quads[0].x0 == 160 && quads[0].x1 == 190 && quads[0].u0 == 0);
		CHECK(std::abs(quads[0].u1 - 30 / 360.0f) < 1e-5f);
		CHECK(quads[1].x0 == 100 && quads[1].x1 == 160 && std::abs(quads[1].u0 - 300 / 360.0f) < 1e-5f && quads[1].u1 == 1);
	}
}

TEST(Hud_CompassSnaps)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {
			 "hudBuilder.createCompassNode Global Arrow 0 316 246 8 107 0 1",
			 "hudBuilder.setCompassNodeTexture 1 arrow.tga",
			 "hudBuilder.setCompassNodeTextureSize 8 107",
			 "hudBuilder.setCompassNodeSnapTexture 1 high.tga",
			 "hudBuilder.setCompassNodeValueVariable PlayerForwardMovement",
			 "hudBuilder.setNodeLogicShowVariable NOT AffectedByEMPShow 1",
		 }) {
		console.execute(line, out);
	}
	// Snapping past the high end (the node's height less the snap offset,
	// 107 - 60): c stops at 47 and the snap texture shows.
	console.execute("hudBuilder.setCompassNodeSnapOffset 0 60 0 0", out);
	manager.set("PlayerForwardMovement", 200);
	auto quads = manager.frame(0);
	CHECK(quads.size() == 1 && quads[0].texture == "Menu/HUD/Texture/high.tga");
	if (!quads.empty()) {
		CHECK(quads[0].y0 == 246 && quads[0].y1 == 246 - 47 + 107 && std::abs(quads[0].v0 - 47 / 107.0f) < 1e-5f);
	}
	// Within: slid up by floor(10 / 360 * 107) = 2 pixels.
	manager.set("PlayerForwardMovement", 10);
	quads = manager.frame(0);
	CHECK(quads.size() == 1 && quads[0].texture == "Menu/HUD/Texture/arrow.tga" && quads[0].y0 == 246 && quads[0].y1 == 351);
	if (!quads.empty()) {
		CHECK(std::abs(quads[0].v0 - 2 / 107.0f) < 1e-5f);
	}
}

TEST(Hud_MapAndMarkers)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {
			 "hudBuilder.createMapNode Global Minimap",
			 "hudBuilder.setMaxiPos -122/-271",
			 "hudBuilder.setMaxiSize 512/512",
			 "hudBuilder.setMiniPos 245/-291",
			 "hudBuilder.setMiniSize 146/146",
			 "hudBuilder.setNodeLogicShowVariable NOT AffectedByEMPShow 1",
		 }) {
		console.execute(line, out);
	}
	// A 1024 m level centred on the origin; a clickable spawn point at its
	// north-east quarter.
	manager.setMap("Levels/x/Hud/Minimap/ingameMap.dds", -512, -512, 1024, 1024);
	hud::HudManager::MapMarker spawn {"spawn.dds", 256, 256, 32};
	spawn.id = 7;
	manager.setMapMarkers({spawn});
	manager.set("MapFullSize", 1);
	auto quads = manager.frame(0);
	CHECK(quads.size() == 2);
	if (quads.size() == 2) {
		CHECK(quads[0].x0 == 278 && quads[0].y0 == 29 && quads[0].x1 == 790 && quads[0].u1 == 1);
		// 3/4 across, 1/4 down the big map, 32 pixels.
		CHECK(quads[1].x0 == 278 + 384 - 16 && quads[1].x1 == 278 + 384 + 16);
		CHECK(quads[1].y0 == 29 + 128 - 16 && quads[1].y1 == 29 + 128 + 16);
	}
	CHECK(manager.mapMarkerAt(278 + 384, 29 + 128) == 7 && manager.mapMarkerAt(300, 400) == -1);
	// The minimap eases to its zoom (level 1.5 on foot: 2.3^1.5) round the
	// focus, kept covering the node: in the level's south-west corner the
	// map's corner sits at the node's.
	manager.set("MapFullSize", 0);
	manager.setMapFocus(-500, -500);
	for (int i = 0; i < 20; ++i) {
		quads = manager.frame(1);
	}
	CHECK(!quads.empty() && quads[0].x0 == 645 && quads[0].y0 == 9 && quads[0].x1 == 791 && quads[0].y1 == 155);
	if (!quads.empty()) {
		const float span = 1 / std::pow(2.3f, 1.5f);
		CHECK(std::abs(quads[0].u0) < 1e-5f && std::abs(quads[0].v1 - 1) < 1e-5f && std::abs(quads[0].u1 - span) < 1e-4f);
	}
	CHECK(quads.size() == 1); // the spawn point is outside the window
	// A combat area of the level's middle half: the big map shows it
	// (widened by 20 of 512 pixels a side) filling the node.
	manager.setMapArea(-256, -256, 256, 256);
	manager.set("MapFullSize", 1);
	for (int i = 0; i < 20; ++i) {
		quads = manager.frame(1);
	}
	CHECK(!quads.empty());
	if (!quads.empty()) {
		const float u0 = (128 - 20) / 512.0f, u1 = (384 + 20) / 512.0f;
		CHECK(std::abs(quads[0].u0 - u0) < 1e-4f && std::abs(quads[0].u1 - u1) < 1e-4f);
		CHECK(std::abs(quads[0].v0 - u0) < 1e-4f && std::abs(quads[0].v1 - u1) < 1e-4f);
	}
}


TEST(Hud_ListBordersSelectionScrollbar)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {
			 "hudBuilder.createListNode Global Scores 10 10 200 100 19 1",
			 "hudBuilder.setNodeLogicShowVariable NOT AffectedByEMPShow 1",
			 "hudBuilder.setListNodeData 2",
			 "hudBuilder.setListNodeBorder 20 22 3 8",
			 "hudBuilder.setListNodeBorderColor 0 0 1 1",
			 "hudBuilder.setListNodeSelectColor 1 0 0 1",
			 "hudBuilder.setListNodeScrollbar 10 3",
			 "hudBuilder.setListNodeScrollbarColor 0 1 0 1",
			 "hudBuilder.setListNodeScrollbarBackgroundColor 0 1 1 1",
		 }) {
		console.execute(line, out);
	}
	manager.setListColumns(2, {{8, 100, 0}});
	// Rows from y 30, 19 apart, while one fits above 110 - 22: rows at 30,
	// 49 and 68 (87 + 19 > 88), of five: a scroll bar.
	manager.setList(2, {{""}, {""}, {""}, {""}, {""}});
	manager.setListSelection(2, 1);
	const auto quads = manager.frame(0);
	// The top band, then the right, bottom and left bands (0x007de7d0),
	// the selection, the bar's track and thumb.
	CHECK(quads.size() == 7);
	if (quads.size() == 7) {
		CHECK(quads[0].y0 == 10 && quads[0].y1 == 30 && quads[0].x1 == 210);
		CHECK(quads[1].x0 == 202 && quads[1].y0 == 30 && quads[1].y1 == 110);
		CHECK(quads[2].y0 == 88 && quads[2].x1 == 202);
		CHECK(quads[3].x1 == 13 && quads[3].y0 == 30);
		CHECK(quads[4].color[0] == 1 && quads[4].x0 == 13 && quads[4].x1 == 202 && quads[4].y0 == 49 && quads[4].y1 == 68);
		CHECK(quads[5].x0 == 197 && quads[5].x1 == 207 && quads[5].y0 == 30 && quads[5].y1 == 88);
		// The thumb: 3 of 5 of the track less two rows, (58 - 38) * 0.6.
		CHECK(quads[6].y0 == 30 && std::abs(quads[6].y1 - 42) < 1e-4f);
	}
}
