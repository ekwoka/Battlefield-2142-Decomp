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

// setNodeLogicShowVariable's value is an int (0x0076e090 names "int",
// 0x0076e1ca reads it with operator>>(int&)): "1.5" is 1.
TEST(Hud_LogicValueIsAnInt)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {
			 "hudBuilder.createPictureNode BottomLeftStatic Left 0 0 10 10",
			 "hudBuilder.setPictureNodeTexture Ingame/a.tga",
			 "hudBuilder.setNodeLogicShowVariable NOT IsSpectator 1.5",
		 }) {
		console.execute(line, out);
	}
	CHECK(!manager.frame(0).empty());
	manager.set("IsSpectator", 1);
	CHECK(manager.frame(0).empty());
}

// The corner roots (the "Ingame" MemeFile's transforms): a picture at 0, 0
// under BottomLeftStatic and BottomRightStatic draws at -1, 563 and 401, 563.
TEST(Hud_CornerRootsFromTheMemeFile)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {
			 "hudBuilder.createPictureNode BottomLeftStatic Left 0 0 10 10",
			 "hudBuilder.setPictureNodeTexture Ingame/a.tga",
			 "hudBuilder.createPictureNode BottomRightStatic Right 0 0 10 10",
			 "hudBuilder.setPictureNodeTexture Ingame/b.tga",
		 }) {
		console.execute(line, out);
	}
	// (Drawn in both passes.)
	const auto quads = manager.frame(0);
	const auto at = [&](const char* texture, float x, float y) {
		return std::any_of(quads.begin(), quads.end(), [&](const hud::HudQuad& q) {
			return q.texture == std::string("Menu/HUD/Texture/Ingame/") + texture && q.x0 == x && q.y0 == y;
		});
	};
	CHECK(at("a.tga", -1, 563));
	CHECK(at("b.tga", 401, 563));
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
	// Ids ignore case and the first of two keeps its text (0x0065b2c0,
	// 0x006956f0): the login page asks for BUTTON_CREATENEWACCOUNT.
	const std::u16string cased = u"StringID\tEnglish\r\nBUTTON_createnewaccount\tCREATE NEW ACCOUNT\r\n"
								 u"BUTTON_CREATENEWACCOUNT\tSECOND\r\n";
	std::vector<uint8_t> casedData;
	for (char16_t c : cased) {
		casedData.push_back(static_cast<uint8_t>(c));
		casedData.push_back(static_cast<uint8_t>(c >> 8));
	}
	io::Localization folded;
	CHECK(folded.load(casedData, "English") && folded.size() == 1);
	CHECK(folded.text("BUTTON_CREATENEWACCOUNT") == "CREATE NEW ACCOUNT");
	CHECK(folded.text("button_CreateNewAccount") == "CREATE NEW ACCOUNT");
	CHECK(english.text("hud_text_menu_score_players") == "PLAYERS" && english.text("Missing_Id") == "Missing_Id");

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

// setBarNodeBorder and setCompassNodeBorder share one executor
// (0x0076aebf -> 0x007b0780: the active node's slot 0x100), which bars and
// compasses both answer with 0x007e9270 (+0x74..+0x80): either command
// sets the same border on either node. A compass's strip period is its
// width less the right border; a bar fills inside the border.
TEST(Hud_BarAndCompassShareTheBorder)
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
			 "hudBuilder.setBarNodeBorder 0 0 0 60",
			 "hudBuilder.createBarNode Global Health 3 100 500 200 10",
			 "hudBuilder.setBarNodeTexture 1 Ingame/full.tga",
			 "hudBuilder.setBarNodeValueVariable PlayerHealth",
			 "hudBuilder.setCompassNodeBorder 0 0 20 80",
		 }) {
		console.execute(line, out);
	}
	manager.set("VehicleAngle", 90);
	manager.set("PlayerHealth", 1);
	const auto quads = manager.frame(0);
	bool compass = false, bar = false;
	for (const auto& q : quads) {
		if (q.texture.find("compass.tga") != std::string::npos && q.x0 == 100) {
			compass = true;
			CHECK(std::abs(q.u0 - 75 / 360.0f) < 1e-5f); // floor(90 / 360 x (360 - 60))
		}
		if (q.texture.find("full.tga") != std::string::npos) {
			bar = true;
			CHECK(q.x0 == 120 && q.x1 == 220); // 20 off the left, 80 off the right
		}
	}
	CHECK(compass && bar);
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
	// While playing, the minimap's texture eases to MenuMapAlpha, the
	// profile's minimap transparency (0x0049bad0; 170 / 255 by default,
	// 0x0049bdf0), and its icons take MenuMapIconAlpha (0x00780940).
	CHECK(!quads.empty() && std::abs(quads[0].color[3] - 170.0f / 255.0f) < 1e-3f);
	manager.set("MenuMapAlpha", 0.25f);
	manager.set("MenuMapIconAlpha", 0.5f);
	manager.setMapFocus(256, 256);
	for (int i = 0; i < 20; ++i) {
		quads = manager.frame(1);
	}
	CHECK(quads.size() == 2 && std::abs(quads[0].color[3] - 0.25f) < 1e-3f && std::abs(quads[1].color[3] - 0.5f) < 1e-3f);
	manager.set("MapFullSize", 1);
	for (int i = 0; i < 20; ++i) {
		quads = manager.frame(1);
	}
	CHECK(quads.size() == 2 && quads[0].color[3] == 1 && quads[1].color[3] == 1); // the big map: full
	manager.set("MapFullSize", 0);
	manager.set("MenuMapAlpha", 170.0f / 255.0f);
	manager.set("MenuMapIconAlpha", 1);
	manager.setMapFocus(-500, -500);
	for (int i = 0; i < 20; ++i) {
		quads = manager.frame(1);
	}
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

// The commander screen's map (setCommanderPos/Size) and its overhead view:
// the map's texture fades out (MenuMapAlpha, +0x7b0 eased) and the map is
// zoomed so the node's height spans the camera's ground width about it
// (0x0077c4b0), the marks staying on what it sees.
TEST(Hud_CommanderMapOverhead)
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
			 "hudBuilder.setCommanderPos -300/-300",
			 "hudBuilder.setCommanderSize 600/600",
			 "hudBuilder.setNodeLogicShowVariable NOT AffectedByEMPShow 1",
		 }) {
		console.execute(line, out);
	}
	manager.setMap("Levels/x/Hud/Minimap/ingameMap.dds", -512, -512, 1024, 1024);
	manager.setMapMarkers({{"mark.dds", 10, 0, 16}});
	manager.set("MapFullSize", 1);
	manager.set("CommanderShow", 1);
	auto quads = manager.frame(0);
	CHECK(!quads.empty() && quads[0].x0 == 100 && quads[0].y0 == 0 && quads[0].x1 == 700 && quads[0].y1 == 600);
	// Its own zoom index: two steps to the overhead view.
	CHECK(manager.commanderMapZoom() == 0);
	manager.cycleMapZoom();
	manager.cycleMapZoom();
	CHECK(manager.commanderMapZoom() == 2 && manager.mapZoom() == 1);
	// 60 m across the node's 600 pixels, over the origin: the mark 10 m
	// east is 100 pixels right of the middle; the map fades to nothing.
	manager.setOverhead(true, 0, 0, 60);
	for (int i = 0; i < 200; ++i) {
		quads = manager.frame(1.0f / 60);
	}
	CHECK(quads.size() == 1);
	if (quads.size() == 1) {
		CHECK(std::abs((quads[0].x0 + quads[0].x1) / 2 - 500) < 1 && std::abs((quads[0].y0 + quads[0].y1) / 2 - 300) < 1);
	}
	float rect[4];
	CHECK(manager.overheadRect(rect) && rect[0] == 100 && rect[3] == 600);
	// Off again: the map comes back.
	manager.setOverhead(false, 0, 0, 60);
	CHECK(!manager.overheadRect(rect));
	for (int i = 0; i < 200; ++i) {
		quads = manager.frame(1.0f / 60);
	}
	CHECK(quads.size() == 2 && quads[0].color[3] == 1);
}

// A picture without a texture is a fill in its colour (the commander
// screen's black borders); setPictureNodeCenterPoint's point (about the
// screen's centre) is the rotation's pivot.
TEST(Hud_PictureFillAndPivot)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {
			 "hudBuilder.createSplitNode Global Items",
			 "hudBuilder.setNodeLogicShowVariable NOT AffectedByEMPShow 1",
			 "hudBuilder.createPictureNode Items Border 0 0 100 600",
			 "hudBuilder.setNodeColor 0 0 0 1",
			 "hudBuilder.createPictureNode Items Bar 304 394 193 13",
			 "hudBuilder.setPictureNodeTexture Ingame/bar.tga",
			 "hudBuilder.setPictureNodeRotateVariable Banking",
			 "hudBuilder.setPictureNodeCenterPoint 0 0",
			 "hudBuilder.createPictureNode Items Plain 10 10 20 20",
			 "hudBuilder.setPictureNodeTexture Ingame/plain.tga",
			 "hudBuilder.addNodeBlendEffect 7 2",
		 }) {
		console.execute(line, out);
	}
	manager.set("Banking", 30);
	const auto quads = manager.frame(0);
	CHECK(quads.size() == 3);
	if (quads.size() == 3) {
		CHECK(quads[0].texture == "Menu/Console/white.dds" && quads[0].x1 == 100 && quads[0].color[0] == 0 &&
			quads[0].color[3] == 1);
		CHECK(quads[1].rotation == 30 && quads[1].pivotSet && quads[1].pivot[0] == 400 && quads[1].pivot[1] == 300);
		CHECK(!quads[2].pivotSet && quads[2].blend[0] == 7 && quads[2].blend[1] == 2);
	}
}

// The text node styles (setTextNodeStyle → 0x007af920): 0 centred lines
// (0x007dab10), 1 right-aligned (0x007ebf00), 2 left-aligned with word
// wrap inside the node (0x007ec480).
TEST(Hud_TextNodeStyles)
{
	// A font of one glyph 'A' (10 wide) and the space (5 wide), 12 high.
	const std::string font = "header\r\n2\r\ntest\r\n64\r\n64\r\n12.000000\r\nglyphs\r\n2\r\n"
							 "65\t0.000000\t10.000000\t0.000000\t0\t0\t0\t10\t10\r\n"
							 "32\t0.000000\t5.000000\t0.000000\t0\t0\t0\t0\t0\r\n";
	Console console;
	hud::HudManager manager([&font](const std::string& path, std::string& out) {
		if (path.find("t.dif") == std::string::npos) {
			return false;
		}
		out = font;
		return true;
	});
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {"hudBuilder.createSplitNode Global G", "hudBuilder.createTextNode G Centre 100 0 50 12",
			 "hudBuilder.setTextNodeStyle Fonts/t.dif 0", "hudBuilder.setTextNodeString AA",
			 "hudBuilder.createTextNode G Right 100 20 50 12", "hudBuilder.setTextNodeStyle Fonts/t.dif 1",
			 "hudBuilder.setTextNodeString AA", "hudBuilder.createTextNode G Wrap 100 40 30 30",
			 "hudBuilder.setTextNodeStyle Fonts/t.dif 2", "hudBuilder.setTextNodeString \"AA AA A\""}) {
		console.execute(line, out);
	}
	const auto quads = manager.frame(0);
	std::vector<std::pair<float, float>> at;
	for (const auto& q : quads) {
		at.push_back({q.x0, q.y0});
	}
	// Both passes draw these (no AffectedByEMPShow logic): 9 glyphs each.
	CHECK(at.size() == 18);
	if (at.size() == 18) {
		CHECK(at[0].first == 115 && at[1].first == 125); // centred: (50 - 20) / 2
		CHECK(at[2].first == 130 && at[3].first == 140); // right: 150 - 20
		// "AA AA" is 45 > 30: one word a line; the third starts at 64,
		// above the node's bottom (70): still drawn.
		CHECK(at[4] == std::make_pair(100.0f, 40.0f) && at[5] == std::make_pair(110.0f, 40.0f));
		CHECK(at[6] == std::make_pair(100.0f, 52.0f) && at[8] == std::make_pair(100.0f, 64.0f));
	}
}

// A vehicle's miniMapIconOffset (0x0077dd70 → 0x0077c610): the icon drawn
// that many pixels from its point, turned about the point.
// The minimap turns with the player (0x0078f680): the heading (0x00778ea0)
// eased into the map's turn (+0x80c), the texture and the icons turned
// about the node's centre (0x0077cba0, 0x0077c610) so the view's way is
// up; a static map (MapStatic) or a dead player at the minimap back to
// north; the compass's delayed angle (+0x800) follows.
TEST(Hud_MinimapTurnsWithThePlayer)
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
	manager.setMap("Levels/x/Hud/Minimap/ingameMap.dds", -512, -512, 1024, 1024);
	manager.setMapFocus(0, 0);
	// A mark 100 m east of the player, pointing north.
	manager.setMapMarkers({{"mark.dds", 100, 0, 16, 0}});
	manager.set("MapFullSize", 0);
	const float east = 3.1415927f / 2;
	for (int i = 0; i < 120; ++i) {
		manager.setMapHeading(east);
		manager.tickMapTurn(1.0f / 60, true);
	}
	CHECK(std::abs(manager.mapTurn() - east) < 1e-4f);
	CHECK(std::abs(manager.mapDelayedAngle() - east) < 0.01f);
	std::vector<hud::HudQuad> quads;
	for (int i = 0; i < 20; ++i) {
		quads = manager.frame(1);
	}
	const float cx = 645 + 73, cy = 9 + 73;
	CHECK(quads.size() == 2);
	if (quads.size() == 2) {
		// The map turned a quarter counter-clockwise about the node's centre.
		CHECK(std::abs(quads[0].rotation + 90) < 0.01f && quads[0].pivotSet && quads[0].pivot[0] == cx && quads[0].pivot[1] == cy);
		CHECK(quads[0].clipped);
		// East is up: the mark straight above the centre, pointing left
		// (north on the turned map).
		const float mx = (quads[1].x0 + quads[1].x1) / 2, my = (quads[1].y0 + quads[1].y1) / 2;
		CHECK(std::abs(mx - cx) <= 1 && my < cy - 40);
		CHECK(std::abs(quads[1].rotation + 90) < 0.01f);
	}
	// A static map: north up again.
	manager.set("MapStatic", 1);
	manager.setMapHeading(east);
	manager.tickMapTurn(1.0f / 60, true);
	CHECK(manager.mapTurn() == 0 && manager.mapDelayedAngle() == 0);
	quads = manager.frame(1);
	CHECK(quads.size() == 2 && quads[0].rotation == 0);
	if (quads.size() == 2) {
		CHECK(std::abs((quads[1].x0 + quads[1].x1) / 2 - cx) > 40);
	}
	// Turning again, then dead at the minimap: back to 0.
	manager.set("MapStatic", 0);
	for (int i = 0; i < 120; ++i) {
		manager.setMapHeading(east);
		manager.tickMapTurn(1.0f / 60, true);
	}
	CHECK(std::abs(manager.mapTurn() - east) < 1e-4f);
	manager.setMapHeading(0);
	manager.tickMapTurn(1.0f / 60, false);
	CHECK(manager.mapTurn() == 0);
}

TEST(Hud_MapMarkerOffset)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::string out;
	for (const char* line : {
			 "hudBuilder.createMapNode Global Minimap",
			 "hudBuilder.setMaxiPos -300/-300",
			 "hudBuilder.setMaxiSize 600/600",
			 "hudBuilder.setMiniPos 245/-291",
			 "hudBuilder.setMiniSize 146/146",
			 "hudBuilder.setNodeLogicShowVariable NOT AffectedByEMPShow 1",
		 }) {
		console.execute(line, out);
	}
	manager.setMap("Levels/x/Hud/Minimap/ingameMap.dds", -512, -512, 1024, 1024);
	hud::HudManager::MapMarker plain {"mark.dds", 0, 0, 16, 90};
	hud::HudManager::MapMarker moved = plain;
	moved.offsetX = 0;
	moved.offsetY = 8;
	manager.setMapMarkers({plain, moved});
	manager.set("MapFullSize", 1);
	std::vector<hud::HudQuad> marks;
	for (const hud::HudQuad& q : manager.frame(0)) {
		if (q.texture.find("mark.dds") != std::string::npos) {
			marks.push_back(q);
		}
	}
	CHECK(marks.size() == 2);
	if (marks.size() == 2) {
		const hud::HudQuad& a = marks[0];
		const hud::HudQuad& b = marks[1];
		CHECK(!a.pivotSet && b.pivotSet);
		CHECK(b.x0 == a.x0 && b.y0 == a.y0 + 8);
		// The pivot is the point (the plain icon's centre).
		CHECK(b.pivot[0] == (a.x0 + a.x1) / 2 && b.pivot[1] == (a.y0 + a.y1) / 2);
		CHECK(b.rotation == 90);
	}
}

// The commander's mini-map tab (Bf2MiniMenuMapNode, 0x007e8a90): the
// level's map at zoom 0 and half alpha filling the node, the players as
// 2-pixel dots (4 for the squad's), the zoom square at the map's focus
// shrunk by the commander map's zoom (120 / clamp(zoom / 2, 1, 100)), and
// a click's world point (0x007e7d00).
TEST(Hud_CommanderMiniMapTab)
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
			 "hudBuilder.createPictureNode Global CommanderMapTabSelected 651 0 146 188",
			 "hudBuilder.createMiniMapNode CommanderMapTabSelected CommanderMiniMap 6 0 133 163",
		 }) {
		console.execute(line, out);
	}
	manager.setMap("Levels/x/Hud/Minimap/ingameMap.dds", -512, -512, 1024, 1024);
	manager.set("MapFullSize", 1);
	hud::HudManager::MiniMapDot mate, squad;
	mate.x = 0;
	mate.z = 0;
	squad.x = 256;
	squad.z = 256;
	squad.squad = true;
	manager.setMiniMapDots({mate, squad}, true);
	manager.setMapFocus(-256, -256);
	std::vector<hud::HudQuad> quads;
	for (int i = 0; i < 3; ++i) {
		quads = manager.frame(1);
	}
	// The node at 657, 0, 133 x 163: the map over it at half alpha.
	const auto mini = std::find_if(quads.begin(), quads.end(), [](const hud::HudQuad& q) {
		return q.texture.find("ingameMap") != std::string::npos && q.x0 == 657;
	});
	CHECK(mini != quads.end());
	if (mini != quads.end()) {
		CHECK(mini->y0 == 0 && mini->x1 == 657 + 133 && mini->y1 == 163 && std::abs(mini->color[3] - 0.5f) < 1e-5f);
	}
	std::vector<hud::HudQuad> dots;
	for (const hud::HudQuad& q : quads) {
		if (q.texture == "Menu/Console/white.dds" && q.x1 - q.x0 <= 4) { // not the tab's own fill
			dots.push_back(q);
		}
	}
	// (The test's nodes have no AffectedByEMPShow logic: both passes draw them.)
	CHECK(dots.size() == 4);
	dots.resize(std::min<size_t>(dots.size(), 2));
	if (dots.size() == 2) {
		CHECK(dots[0].x1 - dots[0].x0 == 2 && dots[1].x1 - dots[1].x0 == 4);
		// The middle of the node; three quarters across, a quarter down.
		CHECK(std::abs(dots[0].x0 + 1 - (657 + 66.5f)) <= 1 && std::abs(dots[0].y0 + 1 - 81.5f) <= 1);
		CHECK(std::abs(dots[1].x0 + 2 - (657 + 133 * 0.75f)) <= 1 && std::abs(dots[1].y0 + 2 - 163 * 0.25f) <= 1);
	}
	// The big map at zoom level 0 (zoom 1): a 120-pixel square.
	const auto square = std::find_if(quads.begin(), quads.end(),
		[](const hud::HudQuad& q) { return q.texture.find("micromap_zoomSquare") != std::string::npos; });
	CHECK(square != quads.end());
	if (square != quads.end()) {
		CHECK(square->x1 - square->x0 <= 120 && square->x1 > square->x0);
	}
	// A click a quarter across and down: the level's north-west quarter's middle.
	float x = 0, z = 0;
	CHECK(manager.miniMapWorldAt(657 + 133 * 0.25f, 163 * 0.25f, x, z));
	CHECK(std::abs(x + 256) < 1 && std::abs(z - 256) < 1);
	CHECK(!manager.miniMapWorldAt(100, 300, x, z));
}

// Non-English text (coverage-7 P8; BF2142.exe 0x00405360): the strings in
// the game's language and its own fonts (Fonts/<language>/), whose glyphs
// are by Unicode code point; HUD text is read as UTF-8 (hud::nextCode), so
// every character of the German, Russian and Chinese strings is a glyph of
// the language's hudFontLocalBold_9.
TEST(Hud_NonEnglishTextHasGlyphs)
{
	std::string s = "a\xc3\xa9\xe4\xb8\xad\xa7" "1";
	size_t at = 0;
	std::vector<uint32_t> codes;
	while (at < s.size()) {
		codes.push_back(hud::nextCode(s, at));
	}
	CHECK((codes == std::vector<uint32_t> {'a', 0xe9, 0x4e2d, 0xa7, '1'}));
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
	console.run("ClientArchives.con", {}, out);
	std::vector<uint8_t> csv;
	CHECK(files.readFile("Localization/strings.csv", csv));
	for (const char* language : {"German", "Russian", "Chinese_Simplified", "Polish"}) {
		io::Localization strings;
		CHECK(strings.load(csv, language));
		hud::setLanguage(language);
		size_t checked = 0, missing = 0;
		for (const char* id : {"HUD_TEXT_MENU_SCORE_PLAYERNAME", "HUD_TEXT_COMMANDER_MINIMAP", "HUD_HUD_KICKVOTE_IN_PROGRESS",
				 "HUD_TEXT_MENU_SQUAD_MANAGE_KICKVOTE", "HUD_TEXT_MENU_SCORE_ROUNDSWON"}) {
			hud::HudManager manager([&files](const std::string& path, std::string& text) {
				std::vector<uint8_t> data;
				if (!files.readFile(path, data)) {
					return false;
				}
				text.assign(data.begin(), data.end());
				return true;
			});
			manager.setLocalizer([&strings](const std::string& key) { return strings.text(key); });
			Console builder;
			hud::registerHudConsoleMethods(builder, manager);
			builder.execute("hudBuilder.createTextNode Global T 0 0 800 20", out);
			builder.execute("hudBuilder.setTextNodeStyle Fonts/hudFontLocalBold_9.dif 0", out);
			builder.execute(std::string("hudBuilder.setTextNodeString ") + id, out);
			const std::string text = strings.text(id);
			const auto quads = manager.frame(0);
			size_t visible = 0;
			for (size_t at2 = 0; at2 < text.size();) {
				const uint32_t c = hud::nextCode(text, at2);
				visible += c != ' ' ? 1 : 0;
				++checked;
			}
			// Both passes draw the test's node (no AffectedByEMPShow logic).
			const size_t drawn = quads.size() / 2;
			missing += visible > drawn ? visible - drawn : 0;
		}
		std::fprintf(stderr, "  %s: %zu characters, %zu without a glyph\n", language, checked, missing);
		CHECK(checked > 20 && missing == 0);
	}
	hud::setLanguage("English");
}
