// The end-of-round screen's natives (Main/EndOfRoundBindings.h): the values
// the movie reads from a RoundSummary, and (with $BF2142_GAME_DIR) the
// shipped endOfRound.dfm as a loading screen and after a round.
#include "IO/File/ZipArchive.h"
#include "IO/Localization/Localization.h"
#include "Main/EndOfRoundBindings.h"
#include "Hud/HudFont.h"
#include "TestMain.h"
#include "UI/Player/Player.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

using namespace dice::hfe;
using namespace dice::hfe::ui;

namespace {

avm1::Value call(avm1::Interpreter& vm, NativeRegistry& natives, const char* object, const char* member,
	std::vector<avm1::Value> args = {})
{
	return natives.call(vm, object, member, args);
}

menu::RoundSummary finishedRound()
{
	menu::RoundSummary r;
	r.mapPath = "Minsk";
	r.gameMode = "gpm_cq";
	r.mapSize = 32;
	r.mapDescriptionKey = "LOADINGSCREEN_MAPDESCRIPTION_Minsk";
	r.gameModeDescriptionKey = "GAMEMODE_DESCRIPTION_assault";
	r.validRound = true;
	r.previousMapPath = "Minsk";
	r.myTeam = 2;
	r.winningTeam = 2;
	r.victories = {0, 1};
	r.score = 57;
	r.kills = 12;
	r.dogTags = 2;
	r.scorePerMinute = 3.5f;
	r.roundExp = 57;
	r.totalExp = 100;
	r.rank = 2; // 80..120 points
	for (int k = 0; k < 12; ++k) {
		menu::EndOfRoundPlayer p;
		p.name = k == 0 ? "Ghost" : "Bot" + std::to_string(k);
		p.team = k % 2 ? 1 : 2;
		p.score = 60 - k * 3;
		p.kills = 12 - k / 2;
		p.deaths = k % 5;
		p.teamwork = k % 7;
		p.scorePerMinute = p.score / 15.0f;
		p.commander = k == 2;
		p.squad = k == 2 ? 0 : 1 + (k / 2) % 3;
		p.squadLeader = k == 0 || k == 1 || k == 4 || k == 5;
		r.players.push_back(p);
	}
	r.localPlayer = 0;
	r.squads = {{2, 1, "Alpha"}};
	r.awards = {{200, 0, 1}, {100, 1, 1}};
	r.endOfRoundText = "\\ktw1_Ghost\\300\\ks1_Ghost\\20\\ktw1_Bot4\\120\\ks1_Bot4\\30\\ktw1_Bot1\\60\\ks1_Bot1\\25"
		"\\wk0_Ghost\\5\\wa0_Ghost\\0.5\\wtw0_Ghost\\120\\vtw2_Ghost\\60\\vk2_Ghost\\2\\vtw2_Bot3\\30\\vk2_Bot3\\3";
	return r;
}

} // namespace

TEST(EndOfRoundBindings_Natives)
{
	NativeRegistry natives;
	natives.setLocalizer([](const std::string& key) {
		if (key == "HEADER_gpm_cq") {
			return std::string("CONQUEST");
		}
		return key == "RANK_2" ? std::string("Private 1st class") : key;
	});
	menu::EndOfRoundBindings bindings(natives);
	avm1::Interpreter vm;
	const auto number = [&](const char* object, const char* member, std::vector<avm1::Value> args = {}) {
		return vm.toNumber(call(vm, natives, object, member, std::move(args)));
	};
	const auto text = [&](const char* object, const char* member, std::vector<avm1::Value> args = {}) {
		return vm.toString(call(vm, natives, object, member, std::move(args)));
	};
	const char* eor = "dice.bf2.EndOfRound";

	// No round yet: the loading screen's values only.
	menu::RoundSummary loading;
	loading.mapPath = "Camp_Gibraltar";
	loading.gameMode = "gpm_cq";
	bindings.setSummary(loading);
	bindings.setLoading(0.5f);
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Logic", "getIsLoading")));
	CHECK(number("dice.bf2.Client", "getLoadProgress") == 0.5);
	CHECK(!avm1::toBoolean(call(vm, natives, eor, "isValidEOR")));
	CHECK(call(vm, natives, eor, "getPlayerScore").isUndefined());
	CHECK(text(eor, "getMapName") == "Camp Gibraltar");
	CHECK(text(eor, "getMapPath") == "Camp_Gibraltar");
	CHECK(text(eor, "getMapGameModeName") == "CONQUEST");
	CHECK(avm1::toBoolean(call(vm, natives, eor, "hasMap")));
	CHECK(text("dice.bf2.Logic", "getNextToolTip").rfind("TEXT_gametip", 0) == 0);
	// The movie's JOIN GAME condition once the level is up.
	bindings.setLevelLoaded();
	CHECK(!avm1::toBoolean(call(vm, natives, "dice.bf2.Logic", "getIsLoading")));
	CHECK(number("dice.bf2.Client", "getLoadProgress") < 0.95);
	CHECK(number("dice.bf2.Logic", "getNumRoundsLeft") > 0);

	// A finished round.
	bindings.setSummary(finishedRound());
	CHECK(avm1::toBoolean(call(vm, natives, eor, "isValidEOR")));
	CHECK(number(eor, "getMyTeam") == 1);
	CHECK(number(eor, "getWinningTeam") == 1);
	CHECK(text(eor, "getTeamName", {avm1::Value(1)}) == "EU");
	CHECK(text(eor, "getBackgroundImage") == "images/eor/minsk/outcome_win_eu.png");
	CHECK(text(eor, "getHeaderImageOutcome") == "images/eor/header_outcome_win_english.png");
	CHECK(number(eor, "getPlayerScore") == 57);
	CHECK(text(eor, "getPlayerScorePerMinute") == "3.50");
	// Through GetNumberFormatA in the game's language (0x10007c60 with
	// 0x10007730's LCID): German's decimal comma.
	hud::setLanguage("German");
	CHECK(text(eor, "getPlayerScorePerMinute") == "3,50");
	hud::setLanguage("English");
	CHECK(number(eor, "getNumVictories", {avm1::Value(1)}) == 1);
	CHECK(number(eor, "getNumAwards") == 2);
	CHECK(text(eor, "getCurrentRankString") == "Private 1st class");
	CHECK(number(eor, "getNextRankExp") == 120);
	CHECK(number(eor, "getRankProgress") == 0.5);
	// Team 1 (EU): the commander is Bot2, second in the team and third of
	// the round (bronze).
	CHECK(text(eor, "getCommander", {avm1::Value(1)}) == "2. Bot2");
	CHECK(number(eor, "getCommanderScore", {avm1::Value(1)}) == 54);
	CHECK(number(eor, "getCommanderStar", {avm1::Value(1)}) == 2);
	CHECK(text(eor, "getCommanderMultiplierString", {avm1::Value(1)}) == "HEADER_commanderwin");
	CHECK(text(eor, "getCommanderMultiplierString", {avm1::Value(0)}) == "HEADER_commanderloss");
	CHECK(text(eor, "getCommander", {avm1::Value(0)}).empty());
	// Squads by score: EU's squad 1 (Ghost, Bot6: 102) before squad 3.
	CHECK(text(eor, "getSquadName", {avm1::Value(1), avm1::Value(0)}) == "1. Alpha");
	CHECK(text(eor, "getSquadName", {avm1::Value(1), avm1::Value(1)}) == "3. Charlie");
	CHECK(text(eor, "getSquadName", {avm1::Value(1), avm1::Value(5)}).empty());

	// JOIN GAME and SPECTATOR.
	int joined = 0, spectated = 0;
	bindings.onReadyToPlay = [&](bool spectator) { ++(spectator ? spectated : joined); };
	call(vm, natives, "dice.bf2.Logic", "readyToPlay");
	call(vm, natives, "dice.bf2.Logic", "readyToPlay", {avm1::Value(true)});
	CHECK(joined == 1 && spectated == 1);

	// Storage with defaults.
	CHECK(text("dice.bf2.Logic", "getStorageString", {avm1::Value("EOR_lastpage"), avm1::Value("0")}) == "0");
	call(vm, natives, "dice.bf2.Logic", "setStorageString", {avm1::Value("EOR_lastpage"), avm1::Value("2")});
	CHECK(text("dice.bf2.Logic", "getStorageString", {avm1::Value("EOR_lastpage"), avm1::Value("0")}) == "2");
}

TEST(EndOfRoundBindings_LevelDescription)
{
	const std::string desc = "<map>\n\t<name> Minsk </name>\n"
		"\t<briefing locid=\"LOADINGSCREEN_MAPDESCRIPTION_Minsk\">text</briefing>\n"
		"\t<modes>\n\t\t<mode type=\"gpm_cq\">\n"
		"\t\t\t<maptype players=\"16\" type=\"assault\" locid=\"GAMEMODE_DESCRIPTION_assault\">x</maptype>\n"
		"\t\t\t<maptype players=\"32\" type=\"headon\" locid=\"GAMEMODE_DESCRIPTION_headon\">x</maptype>\n"
		"\t\t</mode>\n\t\t<mode type=\"gpm_ti\">\n"
		"\t\t\t<maptype players=\"48\" type=\"titan\" locid=\"GAMEMODE_DESCRIPTION_titan\">x</maptype>\n"
		"\t\t</mode>\n\t</modes>\n</map>\n";
	const menu::LevelDescription cq = menu::parseLevelDescription(desc, "gpm_cq", 32);
	CHECK(cq.name == "Minsk");
	CHECK(cq.briefingKey == "LOADINGSCREEN_MAPDESCRIPTION_Minsk");
	CHECK(cq.gameModeKey == "GAMEMODE_DESCRIPTION_headon");
	CHECK(cq.sizes.size() == 2 && cq.sizes[0] == 16 && cq.sizes[1] == 32);
	const menu::LevelDescription ti = menu::parseLevelDescription(desc, "gpm_ti", 48);
	CHECK(ti.gameModeKey == "GAMEMODE_DESCRIPTION_titan");
	CHECK(menu::rankExperience(0) == 0 && menu::rankExperience(1) == 40 && menu::rankExperience(3) == 120);
}

namespace {

// endOfRound.dfm with its images: "images/..." from Menu_client.zip, "$/..."
// and "/..." from the mod's folder (the levels' map pictures), "/Fonts/..."
// (the text fields' engine fonts) from Fonts_client.zip.
struct EorMovie {
	Player player;
	std::shared_ptr<io::ZipArchive> client;
	std::shared_ptr<io::ZipArchive> fonts;
	io::Localization strings;
	std::string mod;
	std::vector<std::string> missing;

	bool load(const std::string& gameDir)
	{
		mod = gameDir + "/mods/bf2142/";
		const auto server = io::ZipArchive::open(mod + "Menu_server.zip");
		client = io::ZipArchive::open(mod + "Menu_client.zip");
		fonts = io::ZipArchive::open(mod + "Fonts_client.zip");
		std::vector<uint8_t> dfm;
		if (!server || !client || !server->read("External/FlashMenu/endOfRound.dfm", dfm)) {
			return false;
		}
		for (const char* file : {"strings.csv", "strings_patch.csv"}) {
			std::ifstream in(mod + "Localization/" + file, std::ios::binary);
			const std::vector<uint8_t> csv((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			if (!csv.empty()) {
				strings.load(csv);
			}
		}
		player.setFileReader([this](const std::string& url, std::vector<uint8_t>& out) {
			std::string path = url[0] == '$' ? url.substr(1) : url;
			bool found = false;
			if (path.rfind("/Fonts/", 0) == 0) {
				// A font the game lacks (no Fonts/English/DynamicText_11.dif)
				// is looked for again in Fonts/, as RendDX9.dll does.
				return fonts && fonts->read(path.substr(7), out);
			}
			if (path[0] == '/') {
				std::ifstream in(mod + path.substr(1), std::ios::binary);
				out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
				found = !out.empty();
			} else {
				found = client->read("External/FlashMenu/" + path, out);
			}
			if (!found) {
				missing.push_back(url);
			}
			return found;
		});
		player.natives().setLocalizer([this](const std::string& key) { return strings.text(key); });
		return player.loadDfm(dfm);
	}
	void run(double seconds)
	{
		for (int i = 0; i < static_cast<int>(seconds * 30); ++i) {
			player.tick();
		}
	}
	bool click(const std::string& target)
	{
		for (const auto& c : player.clickables()) {
			if (c.target.size() >= target.size() && c.target.compare(c.target.size() - target.size(), target.size(), target) == 0) {
				player.mouseMove(c.x + c.width / 2, c.y + c.height / 2);
				player.tick();
				player.mouseDown();
				player.tick();
				player.mouseUp();
				return true;
			}
		}
		return false;
	}
	std::string text(const std::string& path) { return player.interpreter()->toString(player.evaluate(path)); }
	std::string label(const std::string& clip) { return text(clip + ".m_mcLabel.labelTextField.text"); }
	bool clean()
	{
		for (const std::string& e : player.scriptErrors()) {
			std::fprintf(stderr, "endOfRound script error: %s\n", e.c_str());
		}
		for (const std::string& m : missing) {
			std::fprintf(stderr, "endOfRound missing file: %s\n", m.c_str());
		}
		return player.scriptErrors().empty();
	}
};

} // namespace

// The loading screen: the map's name, picture and description, the bar,
// then JOIN GAME once the level is up.
TEST(EndOfRoundBindings_LoadingScreen)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	EorMovie movie;
	menu::EndOfRoundBindings bindings(movie.player.natives());
	menu::RoundSummary r;
	r.mapPath = "Minsk";
	r.gameMode = "gpm_cq";
	r.mapSize = 32;
	r.mapDescriptionKey = "LOADINGSCREEN_MAPDESCRIPTION_Minsk";
	r.gameModeDescriptionKey = "GAMEMODE_DESCRIPTION_assault";
	r.serverName = "Local server";
	bindings.setSummary(r);
	bindings.setLoading(0.3f);
	int joined = 0;
	bindings.onReadyToPlay = [&joined](bool) { ++joined; };
	CHECK(movie.load(gameDir));
	movie.run(3);
	CHECK(movie.label("EOR_holder.mc_mapName") == "MINSK");
	CHECK(movie.label("EOR_holder.mc_mapGameMode") == movie.strings.text("HEADER_gpm_cq"));
	CHECK(movie.text("EOR_holder.mc_map.m_mcForImageUrl") == "$/Levels/Minsk/Info/gpm_cq_32_menuMap.png");
	CHECK(!movie.label("EOR_holder.mc_mapDescription").empty());
	CHECK(!avm1::toBoolean(movie.player.evaluate("EOR_holder.mcJoin._visible")));
	CHECK(movie.missing.empty());

	bindings.setLoading(0.97f); // "verifying"
	movie.run(1);
	bindings.setLevelLoaded();
	movie.run(1);
	CHECK(avm1::toBoolean(movie.player.evaluate("_root.bIsReady")));
	CHECK(movie.click("EOR_holder.mcJoin"));
	movie.run(0.5);
	CHECK(joined == 1);
	CHECK(movie.clean());
}

// After a round: the outcome page with the awards, the scoreboard.
TEST(EndOfRoundBindings_FinishedRound)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	EorMovie movie;
	menu::EndOfRoundBindings bindings(movie.player.natives());
	menu::RoundSummary r = finishedRound();
	r.roundsLeft = 1; // the next round on this map: ready at once
	bindings.setSummary(r);
	CHECK(movie.load(gameDir));
	movie.run(3);
	const std::string outcome = "EOR_holder.mcHolderAll.";
	CHECK(movie.label(outcome + "holder_upper.mcKills") == "12");
	CHECK(movie.label(outcome + "holder_upper.mcScore") == "57");
	const std::string awards = outcome + "holder_middle.mcList.eorEarnedAwards.holder_list.";
	CHECK(movie.text(awards + "row0.cell0.text") == movie.strings.text("AWARD_NAME_200"));
	CHECK(movie.text(awards + "row1.cell0.text") == movie.strings.text("AWARD_NAME_100_1"));
	CHECK(!avm1::toBoolean(movie.player.evaluate(awards + "row2._visible")));

	CHECK(movie.click("EOR_nav.mcNav.EOR_nav_scoreboard"));
	movie.run(2);
	CHECK(movie.label("EOR_holder.leftHeaderTitle") == movie.strings.text("HEADER_euwon"));
	CHECK(movie.label("EOR_holder.mcCommanderLeft") == "2. Bot2");
	std::string left;
	for (const char* name : {"instance64", "instance63", "instance65"}) {
		const std::string row = std::string("EOR_holder.") + name + ".holder_list.row0.";
		if (movie.text(row + "cell0.text") == "1. Ghost") {
			left = row;
		}
	}
	CHECK(!left.empty());
	if (!left.empty()) {
		CHECK(movie.text(left + "cell1.text") == "60");
		CHECK(movie.text(left + "cell5.text") == "4.00");
	}

	CHECK(movie.click("EOR_nav.mcNav.EOR_nav_squad"));
	movie.run(2);
	CHECK(movie.label("EOR_holder.mcSquadLeft1") == "1. ALPHA"); // upper-cased by the label
	// The debriefing: the player's best kit, weapon and vehicle and the
	// best teammate's and opponent's with the same one.
	CHECK(movie.click("EOR_nav.mcNav.EOR_nav_debriefing"));
	movie.run(2);
	CHECK(movie.text("EOR_holder.eorDebriefing00.holder_list.row1.cell1.text") == "20");
	CHECK(movie.text("EOR_holder.eorDebriefing00.holder_list.row3.cell1.text") == "4.00");
	CHECK(movie.text("EOR_holder.eorDebriefing01.holder_list.row0.cell1.text") == "Bot4");
	CHECK(movie.text("EOR_holder.eorDebriefing02.holder_list.row0.cell1.text") == "Bot1");
	CHECK(movie.text("EOR_holder.eorDebriefing10.holder_list.row2.cell1.text") == "50%");
	CHECK(movie.text("EOR_holder.eorDebriefing22.holder_list.row0.cell1.text") == "Bot3");
	CHECK(movie.text("EOR_holder.eorDebriefing22.holder_list.row1.cell0.text") == movie.strings.text("TEXT_killswithvehicle"));
	CHECK(movie.clean());
}

// The debriefing (0x0047c7a0's records from the scripts' end-of-round
// text, the natives 0x1001e120.. and the rows 0x10065ad0).
TEST(EndOfRoundBindings_Debriefing)
{
	std::vector<menu::EndOfRoundPlayer> players(4);
	players[0].name = "Me";
	players[0].team = 2;
	players[1].name = "Mate";
	players[1].team = 2;
	players[2].name = "Foe";
	players[2].team = 1;
	players[3].name = "Foe Two";
	players[3].team = 1;
	// bf2/stats/endofround.py's text (its order: ktw, ks; wk, wa, wtw; vtw, vk).
	const std::string text = "\\tt_Me\\600\\ktw1_Me\\300\\ks1_Me\\10\\ktw3_Me\\300\\ks3_Me\\20"
		"\\ktw3_Mate\\120\\ks3_Mate\\30\\ktw3_Foe\\600\\ks3_Foe\\5\\ktw3_Foe Two\\60\\ks3_Foe Two\\25"
		"\\ktw3_Stranger\\60\\ks3_Stranger\\99"
		"\\wk4_Me\\6\\wa4_Me\\0.25\\wtw4_Me\\180\\wk4_Foe\\9\\wa4_Foe\\0.004\\wtw4_Foe\\60"
		"\\vtw2_Mate\\90\\vk2_Mate\\1\\tp0\\Mate";
	const menu::Debriefing d = menu::parseDebriefing(text, players, "Me");
	// Kits by score: Stranger (unknown, dropped), Mate 30, Foe Two 25, Me 20, Foe 5.
	const menu::DebriefingCategory& kits = d.categories[0];
	CHECK(kits.places[0].index == 3 && kits.places[0].count == 20 && kits.places[0].seconds == 300);
	CHECK(std::abs(kits.places[0].perMinute - 4.0f) < 1e-4f);
	CHECK(kits.places[1].name == "Mate" && kits.places[1].count == 30);
	CHECK(kits.places[2].name == "Foe Two"); // the best opponent with the same kit
	CHECK(kits.position == 3 && kits.players == 4);
	const menu::DebriefingCategory& weapons = d.categories[1];
	CHECK(weapons.places[0].index == 4 && weapons.places[0].count == 6 && weapons.places[0].seconds == 180);
	CHECK(std::abs(weapons.places[0].accuracy - 0.25f) < 1e-6f && std::abs(weapons.places[0].perMinute - 2.0f) < 1e-4f);
	CHECK(weapons.places[1].index == -1 && weapons.places[2].name == "Foe");
	CHECK(std::abs(weapons.places[2].accuracy - 0.01f) < 1e-6f); // under 0.01: 0.01
	CHECK(weapons.position == 2 && weapons.players == 2);
	// No vehicle of the player's: nothing, 0/0.
	CHECK(d.categories[2].places[0].index == -1 && d.categories[2].players == 0);

	// The natives and the rows.
	NativeRegistry natives;
	natives.setLocalizer([](const std::string& key) { return key == "TEXT_timeformat_hhmm" ? std::string("#HH#:#MM#") : key; });
	menu::EndOfRoundBindings bindings(natives);
	menu::RoundSummary r;
	r.validRound = true;
	r.players = players;
	r.localPlayer = 0;
	r.myTeam = 2;
	r.endOfRoundText = text;
	bindings.setSummary(r);
	avm1::Interpreter vm;
	natives.install(vm);
	CHECK(vm.toString(call(vm, natives, "dice.bf2.EndOfRound", "getKitPosition")) == "3/4");
	CHECK(vm.toString(call(vm, natives, "dice.bf2.EndOfRound", "getVehiclePosition")) == "0/0");
	CHECK(vm.toString(call(vm, natives, "dice.bf2.EndOfRound", "getKitImage")) == "images/kits/eu_support.png");
	CHECK(vm.toString(call(vm, natives, "dice.bf2.EndOfRound", "getKitHeader")) == "images/kits/support_header_english.png");
	CHECK(vm.toString(call(vm, natives, "dice.bf2.EndOfRound", "getWeaponImage")) == "images/weapons/eu_mg.png");
	CHECK(vm.toString(call(vm, natives, "dice.bf2.EndOfRound", "getWeaponHeader"))
		== "images/weapons/eu_mg_header_english.png");
	const avm1::Value bf2 = vm.getMember(vm.getMember(avm1::Value(vm.global()), "dice"), "bf2");
	const auto rowsOf = [&](const char* args) {
		auto holder = vm.newObject();
		std::vector<avm1::ObjectRef> cells;
		for (int i = 0; i < 4; ++i) {
			auto row = vm.newObject();
			for (const char* cell : {"cell0", "cell1"}) {
				cells.push_back(vm.newObject());
				vm.setMember(avm1::Value(row), cell, avm1::Value(cells.back()));
			}
			vm.setMember(avm1::Value(holder), "row" + std::to_string(i), avm1::Value(row));
		}
		const avm1::Value list = vm.constructWith(vm.getMember(bf2, "List"),
			{avm1::Value(holder), avm1::Value("eorDebriefing"), avm1::Value(args)});
		vm.callMember(list, "fillList", {avm1::Value(true)});
		std::vector<std::string> texts;
		for (const auto& cell : cells) {
			texts.push_back(vm.toString(vm.getMember(avm1::Value(cell), "text")));
		}
		return texts;
	};
	const auto mine = rowsOf("0 0");
	CHECK(mine.size() == 8);
	if (mine.size() == 8) {
		CHECK(mine[0].empty() && mine[1].empty()); // the player's kit: no header, no name
		CHECK(mine[2] == "TEXT_scorewithkit" && mine[3] == "20");
		CHECK(mine[4] == "TEXT_timewithkit" && mine[5] == "05:00"); // the seconds as #HH#:#MM#
		CHECK(mine[6] == "TEXT_scoreperminutewithkit" && mine[7] == "4.00");
	}
	const auto foe = rowsOf("1 2");
	if (foe.size() == 8) {
		CHECK(foe[0] == "HEADER_OPPOSITETEAM" && foe[1] == "Foe");
		CHECK(foe[2] == "TEXT_killswithweapon" && foe[3] == "9" && foe[5] == "1%" && foe[7] == "9.00");
	}
	const auto none = rowsOf("2 1");
	if (none.size() == 8) {
		CHECK(none[0] == "HEADER_SAMETEAM" && none[1].empty() && none[2] == "TEXT_killswithvehicle" && none[3].empty());
	}
}
