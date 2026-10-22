// The end-of-round screen's natives (Main/EndOfRoundBindings.h): the values
// the movie reads from a RoundSummary, and (with $BF2142_GAME_DIR) the
// shipped endOfRound.dfm as a loading screen and after a round.
#include "IO/File/ZipArchive.h"
#include "IO/Localization/Localization.h"
#include "Main/EndOfRoundBindings.h"
#include "TestMain.h"
#include "UI/Player/Player.h"

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
// and "/..." from the mod's folder (the levels' map pictures).
struct EorMovie {
	Player player;
	std::shared_ptr<io::ZipArchive> client;
	io::Localization strings;
	std::string mod;
	std::vector<std::string> missing;

	bool load(const std::string& gameDir)
	{
		mod = gameDir + "/mods/bf2142/";
		const auto server = io::ZipArchive::open(mod + "Menu_server.zip");
		client = io::ZipArchive::open(mod + "Menu_client.zip");
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
	CHECK(movie.clean());
}
