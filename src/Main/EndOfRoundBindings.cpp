#include "EndOfRoundBindings.h"

#include "Stats/Stats.h"
#include "UI/Player/Natives.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <tuple>

namespace dice::hfe::menu {

namespace avm1 = ui::avm1;
using avm1::Interpreter;
using avm1::Value;
using Args = std::vector<Value>;

namespace {

constexpr const char* kEndOfRound = "dice.bf2.EndOfRound";
constexpr const char* kLogic = "dice.bf2.Logic";
constexpr const char* kServerInfo = "dice.bf2.ServerInfo";
constexpr const char* kClient = "dice.bf2.Client";

// Game tips TEXT_gametip1..105 (Logic.getNextToolTip, 0x1003b290).
constexpr int kGameTips = 105;

Value arg(const Args& args, size_t i) { return i < args.size() ? args[i] : Value(); }

// The game's team 1/2 as the movie's 0/1.
int movieTeamOf(int team) { return team == 2 ? 1 : 0; }

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

std::string spaced(std::string s)
{
	std::replace(s.begin(), s.end(), '_', ' ');
	return s;
}

std::string format(const char* pattern, int a, const std::string& b)
{
	char buffer[160];
	std::snprintf(buffer, sizeof(buffer), pattern, a, b.c_str());
	return buffer;
}

// "%0.2f" (0x1001dcd0).
std::string twoDecimals(float value)
{
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%0.2f", static_cast<double>(value));
	return buffer;
}

} // namespace

// The points each rank starts at: Settings/RankSettings.con
// (rankSettings.setRank, Stats.h). The endOfRound movie asks
// PlayerStats.getNextRankExp (0x1001c3f0) for the next rank's.
int rankExperience(int rank)
{
	return stats::ranks().points(rank);
}

// <map><name> Minsk </name><briefing locid="...">...</briefing><modes>
// <mode type="gpm_cq"><maptype players="16" type="assault"
// locid="GAMEMODE_DESCRIPTION_assault">...</maptype>...
LevelDescription parseLevelDescription(const std::string& xml, const std::string& gameMode, int size)
{
	LevelDescription out;
	std::smatch m;
	if (std::regex_search(xml, m, std::regex(R"re(<name>\s*([^<]*?)\s*</name>)re"))) {
		out.name = m[1];
	}
	if (std::regex_search(xml, m, std::regex(R"re(<briefing[^>]*locid\s*=\s*"([^"]*)")re"))) {
		out.briefingKey = m[1];
	}
	if (std::regex_search(xml, m, std::regex(R"re(<briefing[^>]*>\s*([^<]*?)\s*</briefing>)re"))) {
		out.briefing = m[1];
	}
	const std::regex mode(R"re(<mode[^>]*type\s*=\s*"([^"]*)"[^>]*>([\s\S]*?)</mode>)re");
	const std::regex mapType(R"re(<maptype([^>]*)>)re");
	const std::regex players(R"re(players\s*=\s*"(\d+)")re");
	const std::regex locid(R"re(locid\s*=\s*"([^"]*)")re");
	for (auto it = std::sregex_iterator(xml.begin(), xml.end(), mode); it != std::sregex_iterator(); ++it) {
		out.modes.push_back((*it)[1]);
		if ((*it)[1] != gameMode) {
			continue;
		}
		const std::string body = (*it)[2];
		for (auto t = std::sregex_iterator(body.begin(), body.end(), mapType); t != std::sregex_iterator(); ++t) {
			const std::string attributes = (*t)[1];
			std::smatch a;
			const int players_ = std::regex_search(attributes, a, players) ? std::stoi(a[1]) : 0;
			out.sizes.push_back(players_);
			if ((players_ == size || out.gameModeKey.empty()) && std::regex_search(attributes, a, locid)) {
				out.gameModeKey = a[1];
			}
			// The text between the tags, when there is no locid (the
			// record's +0x3c next to the locid's +0x20, 0x1001e940).
			const size_t open = body.find('>', static_cast<size_t>(t->position(0))) + 1;
			const size_t close = body.find("</maptype>", open);
			if (players_ == size && out.gameModeText.empty() && close != std::string::npos) {
				out.gameModeText = std::regex_replace(body.substr(open, close - open), std::regex(R"re(^\s+|\s+$)re"), "");
			}
		}
	}
	return out;
}

EndOfRoundBindings::EndOfRoundBindings(ui::NativeRegistry& natives) : m_natives(natives)
{
	bindEndOfRound();
	bindLogic();
	bindServer();
	bindLists();
}

void EndOfRoundBindings::setSummary(RoundSummary summary)
{
	m_summary = std::move(summary);
	const RoundSummary& s = m_summary;
	const std::string me = s.localPlayer >= 0 && s.localPlayer < static_cast<int>(s.players.size())
		? s.players[static_cast<size_t>(s.localPlayer)].name : std::string();
	m_debriefing = parseDebriefing(s.endOfRoundText, s.players, me);
}

// --- The debriefing's records (0x0047c7a0)

namespace {

// A record of the text: one player's with one kit, weapon or vehicle
// (0x34 bytes for kits and vehicles, 0x38 for weapons: the player, the
// index, the name, the team, then the numbers).
struct DebriefingRecord {
	DebriefingEntry entry;
	bool local = false;
};

// The share of a minute: the original's 0.016666668.
float perMinute(int count, int seconds)
{
	if (seconds == 0) {
		return 0;
	}
	const float value = static_cast<float>(count) / (static_cast<float>(seconds) * 0.016666668f);
	// Anything above 0 but under 0.01 shows as 0.01 (0x3c23d70a).
	return value > 0 && value < 0.01f ? 0.01f : value;
}

} // namespace

// 0x0047c7a0 reads the text's "\\"-separated key and value pairs. A key is
// <name><index>_<player>: the name runs to the first digit or '_', the
// index is the number there (-1 without), the player the name after the
// first '_', found by name (a name the game doesn't know is skipped). The
// records it keeps, by key:
//   ktw / ks: a kit's seconds (rounded) and score;
//   wk / wa / wtw: a weapon's kills, accuracy and seconds;
//   vtw / vk: a vehicle's seconds and kills;
// each with its count per minute once both are known. Each category is
// then sorted, best first: kits by score, then score per minute, then
// time (0x004732b0); weapons by kills, kills per minute, time
// (0x004733a0); vehicles the same (0x00473490). The player's first record
// is place 0; then every record with the same index counts toward
// "position / players" (+0x51c.. +0x530), and the first teammate's and
// the first opponent's among them (the player's slot 0x3e, its team) are
// places 1 and 2.
Debriefing parseDebriefing(const std::string& text, const std::vector<EndOfRoundPlayer>& players,
	const std::string& localName)
{
	std::vector<std::string> tokens;
	for (size_t at = 0; at <= text.size();) {
		const size_t end = std::min(text.find('\\', at), text.size());
		if (end > at) {
			tokens.push_back(text.substr(at, end - at));
		}
		at = end + 1;
	}
	std::array<std::vector<DebriefingRecord>, 3> records; // kits, weapons, vehicles
	for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
		const std::string& key = tokens[i];
		const std::string& value = tokens[i + 1];
		const size_t stop = key.find_first_of("_0123456789");
		const std::string kind = key.substr(0, stop);
		int index = -1;
		if (stop != std::string::npos && std::isdigit(static_cast<unsigned char>(key[stop]))) {
			index = std::atoi(key.c_str() + stop);
		}
		const size_t underscore = key.find('_');
		if (underscore == std::string::npos) {
			continue;
		}
		const std::string name = key.substr(underscore + 1);
		const auto player = std::find_if(players.begin(), players.end(), [&](const EndOfRoundPlayer& p) { return p.name == name; });
		if (player == players.end()) {
			continue;
		}
		int category = -1;
		if (kind == "ktw" || kind == "ks") {
			category = 0;
		} else if (kind == "wk" || kind == "wa" || kind == "wtw") {
			category = 1;
		} else if (kind == "vtw" || kind == "vk") {
			category = 2;
		}
		if (category < 0) {
			continue;
		}
		std::vector<DebriefingRecord>& list = records[static_cast<size_t>(category)];
		auto record = std::find_if(list.begin(), list.end(),
			[&](const DebriefingRecord& r) { return r.entry.name == name && r.entry.index == index; });
		if (record == list.end()) {
			DebriefingRecord added;
			added.entry.index = index;
			added.entry.name = name;
			added.entry.team = player->team;
			added.local = !localName.empty() && name == localName;
			list.push_back(added);
			record = list.end() - 1;
		}
		DebriefingEntry& e = record->entry;
		if (kind == "ktw" || kind == "wtw" || kind == "vtw") {
			e.seconds = static_cast<int>(std::lround(std::atof(value.c_str())));
		} else if (kind == "wa") {
			e.accuracy = static_cast<float>(std::atof(value.c_str()));
			if (e.accuracy > 0 && e.accuracy < 0.01f) {
				e.accuracy = 0.01f;
			}
		} else {
			e.count = std::atoi(value.c_str());
		}
		e.perMinute = perMinute(e.count, e.seconds);
	}
	Debriefing out;
	for (size_t c = 0; c < records.size(); ++c) {
		std::vector<DebriefingRecord>& list = records[c];
		std::stable_sort(list.begin(), list.end(), [](const DebriefingRecord& a, const DebriefingRecord& b) {
			// Kits by score first, weapons and vehicles by kills (the
			// count in all three), then the count per minute, the time.
			return std::make_tuple(a.entry.count, a.entry.perMinute, a.entry.seconds)
				> std::make_tuple(b.entry.count, b.entry.perMinute, b.entry.seconds);
		});
		DebriefingCategory& category = out.categories[c];
		const auto mine = std::find_if(list.begin(), list.end(), [](const DebriefingRecord& r) { return r.local; });
		if (mine == list.end()) {
			continue;
		}
		category.places[0] = mine->entry;
		for (const DebriefingRecord& r : list) {
			if (r.entry.index != mine->entry.index) {
				continue;
			}
			++category.players;
			if (r.local) {
				category.position = category.players;
			} else if (r.entry.team == mine->entry.team) {
				if (category.places[1].index < 0) {
					category.places[1] = r.entry;
				}
			} else if (category.places[2].index < 0) {
				category.places[2] = r.entry;
			}
		}
	}
	return out;
}

void EndOfRoundBindings::setLoading(float progress)
{
	m_summary.loading = true;
	m_summary.levelLoaded = false;
	m_summary.loadProgress = progress;
}

// The level is up (0x004cbea0, event 8): the loading flag drops and the
// progress tracker is finished and reset (0x006655e0), so the progress
// reads 0 again.
void EndOfRoundBindings::setLevelLoaded()
{
	m_summary.loading = false;
	m_summary.levelLoaded = true;
	m_summary.loadProgress = 0;
}

std::string EndOfRoundBindings::localized(const std::string& key) const
{
	return key.empty() ? std::string() : m_natives.localize(key);
}

std::string EndOfRoundBindings::mapName() const
{
	const RoundSummary& s = m_summary;
	if (!s.mapName.empty()) {
		return s.mapName;
	}
	return s.mapPath.empty() ? std::string("Unknown Map") : spaced(s.mapPath);
}

// 0x1001d9c0: "win" when the winner is the player's team, "loss" when it
// is the other one, else "draw".
std::string EndOfRoundBindings::outcome() const
{
	const RoundSummary& s = m_summary;
	if (s.winningTeam == 0) {
		return "draw";
	}
	return s.winningTeam == s.myTeam ? "win" : "loss";
}

// A team's players by score (the game's order: score, then fewer deaths).
std::vector<const EndOfRoundPlayer*> EndOfRoundBindings::teamPlayers(int team) const
{
	std::vector<const EndOfRoundPlayer*> players;
	for (const EndOfRoundPlayer& p : m_summary.players) {
		if (p.team == team) {
			players.push_back(&p);
		}
	}
	std::stable_sort(players.begin(), players.end(), [](const EndOfRoundPlayer* a, const EndOfRoundPlayer* b) {
		return a->score != b->score ? a->score > b->score : a->deaths < b->deaths;
	});
	return players;
}

// --- dice.bf2.EndOfRound (0x1001fd50)

// 0x1001beb0: HEADER_<mode>; without that text the mod's name of the mode
// (ModInfo, "N/A" when unknown), and then the mode itself.
std::string EndOfRoundBindings::modeName(const std::string& mode) const
{
	const std::string key = "HEADER_" + mode;
	const std::string text = localized(key);
	if (text != key) {
		return text;
	}
	const std::string name = gameModeName ? gameModeName(mode) : std::string("N/A");
	return name == "N/A" || name.empty() ? mode : name;
}

void EndOfRoundBindings::bindEndOfRound()
{
	ui::NativeRegistry& n = m_natives;
	const RoundSummary& s = m_summary;
	const auto bind = [&n](const char* member, std::function<Value()> get) {
		n.setFunction(kEndOfRound, member, [get = std::move(get)](Interpreter&, const Args&) { return get(); });
	};
	// The movie's team 0/1 (the first argument) as the game's 1/2.
	const auto teamArg = [](Interpreter& vm, const Args& args) { return vm.toNumber(arg(args, 0)) == 1 ? 2 : 1; };
	// Those that read the finished round answer nothing without one.
	const auto round = [this](Value v) { return m_summary.validRound ? v : Value(); };

	bind("isValidEOR", [&s] { return Value(s.validRound); });                    // 0x1001bdd0
	bind("isRanked", [&s] { return Value(s.validRound && s.ranked); });          // 0x1001be20
	n.setFunction(kEndOfRound, "getTeamName", [this, teamArg](Interpreter& vm, const Args& args) { // 0x1001d790
		return Value(m_summary.teamNames[static_cast<size_t>(teamArg(vm, args) - 1)]);
	});
	n.setFunction(kEndOfRound, "getPreviousTeamName", [this, teamArg](Interpreter& vm, const Args& args) {
		return Value(m_summary.teamNames[static_cast<size_t>(teamArg(vm, args) - 1)]);
	});
	// 0x1001d910: the winner's index, -1 for none.
	bind("getWinningTeam", [&s, round] { return round(Value(s.winningTeam == 0 ? -1 : movieTeamOf(s.winningTeam))); });
	bind("getMyTeam", [&s, round] { return round(Value(movieTeamOf(s.myTeam))); }); // 0x1001c100

	// The level loading (or next).
	bind("hasMap", [&s] { return Value(!s.mapPath.empty()); }); // 0x1001f9d0
	bind("getMapPath", [&s] { return Value(s.mapPath); });      // 0x1001ce70
	bind("getMapName", [this] { return Value(mapName()); });    // 0x1001cef0
	bind("getMapSize", [&s] { return Value(s.mapSize); });      // 0x1001d030
	bind("getMapMaxPlayers", [&s] { return Value(s.maxPlayers); });
	bind("getMapDescription", [this] { return Value(localized(m_summary.mapDescriptionKey)); });
	bind("getMapGameMode", [&s] { return Value(s.gameMode); }); // 0x1001d210
	// 0x1001beb0: HEADER_<mode>, the mode itself when that has no text.
	bind("getMapGameModeName", [this] { return Value(modeName(m_summary.gameMode)); });
	// 0x1001e610: the .desc's text for the mode and size.
	bind("getMapGameModeDescription", [this] { return Value(localized(m_summary.gameModeDescriptionKey)); });
	bind("getMapFriendlyFire", [&s] { return Value(s.friendlyFire); }); // 0x1001fbd0
	bind("getMapAutoBalance", [&s] { return Value(s.autoBalance); });   // 0x1001fc50
	bind("getMapRoundsPerMap", [&s] { return Value(s.roundsPerMap); }); // 0x1001fcd0

	// The level played.
	bind("getPreviousMapPath", [&s, round] { return round(Value(s.previousMapPath)); }); // 0x1001d2d0
	bind("getPreviousMapName", [&s, round] { return round(Value(spaced(s.previousMapPath))); });
	bind("getPreviousMapSize", [&s, round] { return round(Value(s.previousMapSize)); });
	bind("getPreviousMapMaxPlayers", [&s, round] { return round(Value(s.previousMapSize)); });
	bind("getPreviousMapGameMode", [&s, round] { return round(Value(s.previousGameMode)); });
	bind("getPreviousMapGameModeName", [this, round] {
		return round(Value(modeName(m_summary.previousGameMode)));
	});
	// 0x1001d590: the played level's record (SwiffHost slot 0x180's level,
	// ModInfo slot 0x20): its briefing's locid, else its text, localized;
	// "" without a round.
	bind("getPreviousMapDescription", [this] {
		return Value(m_summary.validRound ? localized(m_summary.previousMapDescriptionKey) : std::string());
	});
	// 0x1001e940: that record's mode ("sp1" read as gpm_sp) and size: the
	// maptype's locid, else its text, localized.
	bind("getPreviousMapGameModeDescription", [this] {
		return Value(m_summary.validRound ? localized(m_summary.previousGameModeDescriptionKey) : std::string());
	});
	// 0x1001cd60: a kit's name by its number, "" past the seven.
	n.setFunction(kEndOfRound, "getKitNameFromId", [](Interpreter& vm, const Args& args) {
		static const char* const kKits[] = {"antitank", "assault", "engineer", "medic", "specops", "support", "sniper"};
		const double id = vm.toNumber(arg(args, 0));
		return Value(id >= 0 && id < 7 ? std::string(kKits[static_cast<int>(id)]) : std::string());
	});

	// 0x1001d9c0 / 0x1001db80: the outcome's pictures.
	bind("getBackgroundImage", [this, round] {
		const RoundSummary& s = m_summary;
		const std::string team = lower(s.teamNames[static_cast<size_t>(s.myTeam == 2 ? 1 : 0)]);
		const std::string mode = s.previousGameMode == "gpm_ti" ? "gpm_ti_" : "";
		return round(Value("images/eor/" + lower(s.previousMapPath) + "/outcome_" + mode + outcome() + "_" + team + ".png"));
	});
	n.setFunction(kEndOfRound, "getHeaderImageOutcome", [this, round](Interpreter& vm, const Args&) {
		const Value name = m_natives.call(vm, "dice.bf2.Locale", "getLanguage", {});
		const std::string language = name.isUndefined() ? "english" : lower(vm.toString(name));
		return round(Value("images/eor/header_outcome_" + outcome() + "_" + language + ".png"));
	});

	// The player's round (0x1001c150 ...).
	bind("getPlayerScore", [&s, round] { return round(Value(s.score)); });
	bind("getPlayerKills", [&s, round] { return round(Value(s.kills)); });
	bind("getPlayerScorePerMinute", [&s, round] { return round(Value(twoDecimals(s.scorePerMinute))); });
	bind("getPlayerDogTagCount", [&s, round] { return round(Value(s.dogTags)); });
	bind("getAwayBonusPoints", [&s, round] { return round(Value(s.awayBonus)); });
	bind("getSquadMemberBonus", [&s, round] { return round(Value(s.squadMemberBonus)); });
	bind("getSquadLeaderBonus", [&s, round] { return round(Value(s.squadLeaderBonus)); });
	bind("getCommanderBonus", [&s, round] { return round(Value(s.commanderBonus)); });
	bind("getNumAwards", [&s, round] { return round(Value(static_cast<double>(s.awards.size()))); }); // 0x1001ca70
	bind("getRoundExp", [&s, round] { return round(Value(s.roundExp)); });                           // 0x1001c630
	bind("getCurrentExp", [&s] { return Value(s.totalExp); });                                       // 0x1001c5c0
	bind("gotRankUp", [&s, round] { return round(Value(s.rankedUp)); });                              // 0x1001c680
	bind("getCurrentRankId", [&s] { return Value(s.rank); });                                        // 0x1001c330
	bind("getCurrentRankString", [this] { return Value(localized("RANK_" + std::to_string(m_summary.rank))); });
	bind("getNextRankString", [this] { return Value(localized("RANK_" + std::to_string(m_summary.rank + 1))); });
	bind("getNextRankExp", [&s] { return Value(rankExperience(s.rank + 1)); }); // 0x1001c3f0
	// 0x1001c4a0: the way from this rank's points to the next's, 0..1.
	bind("getRankProgress", [&s] {
		const int from = rankExperience(s.rank), to = rankExperience(s.rank + 1);
		const double progress = to > from ? static_cast<double>(s.totalExp - from) / (to - from) : 1.0;
		return Value(std::clamp(progress, 0.0, 1.0));
	});

	// Teams (the argument: 0 or 1).
	n.setFunction(kEndOfRound, "getNumVictories", [this, teamArg](Interpreter& vm, const Args& args) { // 0x1001cae0
		return Value(m_summary.victories[static_cast<size_t>(teamArg(vm, args) - 1)]);
	});
	// 0x1001eda0: "N. name" of the team's commander (N: the place in the
	// team), "" without one.
	n.setFunction(kEndOfRound, "getCommander", [this, teamArg](Interpreter& vm, const Args& args) {
		const auto players = teamPlayers(teamArg(vm, args));
		for (size_t i = 0; i < players.size(); ++i) {
			if (players[i]->commander) {
				return Value(format("%d. %s", static_cast<int>(i + 1), players[i]->name));
			}
		}
		return Value("");
	});
	n.setFunction(kEndOfRound, "getCommanderScore", [this, teamArg](Interpreter& vm, const Args& args) { // 0x1001eef0
		for (const EndOfRoundPlayer* p : teamPlayers(teamArg(vm, args))) {
			if (p->commander) {
				return Value(p->score);
			}
		}
		return Value("");
	});
	// 0x1001f020: the commander's medal (0 gold, 1 silver, 2 bronze: the
	// round's three best), -1 for none.
	n.setFunction(kEndOfRound, "getCommanderStar", [this, teamArg](Interpreter& vm, const Args& args) {
		for (const EndOfRoundPlayer* p : teamPlayers(teamArg(vm, args))) {
			if (p->commander) {
				const int place = overallPlace(p);
				return Value(place < 3 ? place : -1);
			}
		}
		return Value(-1);
	});
	// 0x1001df30: a localization key.
	n.setFunction(kEndOfRound, "getCommanderMultiplierString", [this, teamArg](Interpreter& vm, const Args& args) {
		const int team = teamArg(vm, args);
		if (m_summary.winningTeam == team) {
			return Value("HEADER_commanderwin");
		}
		return Value(m_summary.winningTeam == 0 ? "HEADER_commanderdraw" : "HEADER_commanderloss");
	});
	// 0x1001e010: (team, place): "N. name" of the team's place-th best
	// squad (N: its number).
	n.setFunction(kEndOfRound, "getSquadName", [this, teamArg](Interpreter& vm, const Args& args) {
		const auto squads = teamSquads(teamArg(vm, args));
		const double place = vm.toNumber(arg(args, 1));
		if (!(place >= 0 && place < static_cast<double>(squads.size()))) {
			return Value("");
		}
		const SquadScore& squad = squads[static_cast<size_t>(place)];
		return Value(format("%d. %s", squad.number, squadName(squad)));
	});

	// The debriefing (see parseDebriefing): the player's place among those
	// with its best kit, weapon and vehicle, "%d/%d" of the record's
	// +0x51c / +0x520 (0x1001e120), +0x524 / +0x528 (0x1001e390) and
	// +0x52c / +0x530 (0x1001e410); and their pictures.
	static const char* const kPositions[] = {"getKitPosition", "getWeaponPosition", "getVehiclePosition"};
	static const char* const kImages[] = {"getKitImage", "getWeaponImage", "getVehicleImage"};
	static const char* const kHeaders[] = {"getKitHeader", "getWeaponHeader", "getVehicleHeader"};
	for (int c = 0; c < 3; ++c) {
		bind(kPositions[c], [this, c, round] {
			const DebriefingCategory& category = m_debriefing.categories[static_cast<size_t>(c)];
			return round(Value(std::to_string(category.position) + "/" + std::to_string(category.players)));
		});
		bind(kImages[c], [this, c, round] { return round(Value(debriefingImage(c, false, std::string()))); });
		n.setFunction(kEndOfRound, kHeaders[c], [this, c, round](Interpreter& vm, const Args&) {
			// The kit's header is in the game's language (SwiffHost slot 0x3a8).
			const Value name = m_natives.call(vm, "dice.bf2.Locale", "getLanguage", {});
			const std::string language = name.isUndefined() ? "english" : lower(vm.toString(name));
			return round(Value(debriefingImage(c, true, language)));
		});
	}
}

// The debriefing's pictures, by the player's best kit, weapon or vehicle
// (SwiffPlayer's name tables: kits 0x10126f70, weapons 0x10126a30 (32),
// vehicles 0x10126db0 (16); "" for none). <team> is the player's team's
// name (the record's +0x504 through SwiffHost slot 0x184), lower-cased as
// the shipped files are named.
//   kits (0x1001e1a0 / 0x1001e270): images/kits/<team>_<kit>.png and
//     images/kits/<kit>_header_<language>.png;
//   weapons (0x1001f4a0 / 0x1001f640): images/weapons/<prefix><weapon>.png
//     and ..._header_english.png, the prefix "<team>_" for the armies' own
//     weapons (0x1001b8e0: 0..11 and 0x12), Pac's as "as_"; the picture is
//     "" for an index past 31;
//   vehicles (0x1001f790 / 0x1001f8b0): images/vehicles/<prefix><vehicle>.png
//     and ..._header_english.png, the prefix for 0, 1, 2, 4, 5, 6 and 10
//     (0x1001ba60).
// The weapon picture's "images/weapons/Best_buy.png" (an EU player's
// weapon 1 when the record's +0x4fc is set) is left out: nothing in the
// end-of-round text sets that flag.
std::string EndOfRoundBindings::debriefingImage(int category, bool header, const std::string& language) const
{
	static const char* const kKits[] = {"recon", "assault", "engineer", "support"};
	static const char* const kWeapons[] = {"sni", "ar_rifle", "av", "smg", "mg", "handgun", "", "", "", "", "", "",
		"knife", "Unl_Carbine", "unl_adv_sni", "Unl_Har_Rifle", "Unl_Lar_Rifle", "Unl_Av_rifle", "aa", "unl_hmg",
		"unl_shotgun", "Unl_Mine_Motion", "unl_grenade_frag", "Unl_C4", "Unl_Mine_AntiPersonnel", "",
		"Unl_Deployer_Sentrygun", "uav_drone_sentry", "", "", "", "Bp1_Expl_Shotgun"};
	static const char* const kVehicles[] = {"mech", "apc", "tank", "", "tair", "titan", "faav", "", "", "", "aair",
		"eu_pod", "", "", "ifv", "hover_fav"};
	const RoundSummary& s = m_summary;
	const int index = m_debriefing.categories[static_cast<size_t>(category)].places[0].index;
	const std::string team = lower(s.teamNames[static_cast<size_t>(s.myTeam == 2 ? 1 : 0)]);
	const auto named = [index](const char* const* table, int size) {
		return index >= 0 && index < size ? std::string(table[index]) : std::string();
	};
	if (category == 0) {
		const std::string kit = named(kKits, static_cast<int>(std::size(kKits)));
		if (!header) {
			return "images/kits/" + team + "_" + kit + ".png";
		}
		return "images/kits/" + kit + "_header_" + language + ".png";
	}
	std::string prefix;
	if (category == 1) {
		if ((index >= 0 && index <= 11) || index == 0x12) {
			prefix = team + "_";
		}
		if (prefix == "pac_") {
			prefix = "as_";
		}
		if (!header && (index < 0 || index > 31)) {
			return std::string();
		}
		return "images/weapons/" + prefix + named(kWeapons, static_cast<int>(std::size(kWeapons)))
			+ (header ? "_header_english.png" : ".png");
	}
	if (index == 0 || index == 1 || index == 2 || index == 4 || index == 5 || index == 6 || index == 10) {
		prefix = team + "_";
	}
	return "images/vehicles/" + prefix + named(kVehicles, static_cast<int>(std::size(kVehicles)))
		+ (header ? "_header_english.png" : ".png");
}

// --- dice.bf2.Logic and Client: the loading.

void EndOfRoundBindings::bindLogic()
{
	ui::NativeRegistry& n = m_natives;
	const RoundSummary& s = m_summary;
	// BF2142.exe's SwiffHost (vtable 0x00924d50) answers these from its
	// fields: getIsLoading (0x1003c9e0) is its loading flag 0x7b4 (slot
	// 0xe4; set when a level starts loading, 0x004cbb70, cleared when it
	// is up, 0x004cbea0 event 8) or its connecting flag 0xd7c (slot 0x2e0;
	// set while joining a server, 0x0040bd90, cleared at 0x00487020);
	// isFirstTimeLoading (0x1003c970) is 0x7b5 (slot 0xec; cleared by
	// readyToPlay's event 0x10, 0x0040bd90); getNumRoundsLeft (0x1003c3d0)
	// is 0xe4c (slot 0x390): 999 for a game the player starts
	// (0x004d3450), the server's count otherwise (0x00511870).
	// endOfRound.dfm's root shows JOIN GAME when the progress is under
	// 0.95, getIsLoading is false and getNumRoundsLeft is above 0 (or on a
	// first load at progress 1, which the 0.95 test above it never lets
	// through).
	n.setFunction(kLogic, "getIsLoading", [&s](Interpreter&, const Args&) { return Value(s.loading && !s.levelLoaded); });
	n.setFunction(kLogic, "isFirstTimeLoading", [&s](Interpreter&, const Args&) { return Value(s.firstLoad); });
	n.setFunction(kLogic, "getNumRoundsLeft", [&s](Interpreter&, const Args&) { return Value(s.roundsLeft); });
	n.setFunction(kLogic, "isAbortLoadLevel", [](Interpreter&, const Args&) { return Value(false); });
	n.setFunction(kLogic, "isLoadingDemo", [](Interpreter&, const Args&) { return Value(false); });
	n.setFunction(kLogic, "getModVersion", [](Interpreter&, const Args&) { return Value("1.51"); });
	// 0x1003dba0: JOIN GAME (no argument / false) or SPECTATOR (true).
	n.setFunction(kLogic, "readyToPlay", [this](Interpreter&, const Args& args) {
		if (onReadyToPlay) {
			onReadyToPlay(avm1::toBoolean(arg(args, 0)));
		}
		return Value();
	});
	// 0x1003b290: a random tip of the 105, not the last one again.
	n.setFunction(kLogic, "getNextToolTip", [this](Interpreter&, const Args&) {
		m_random = m_random * 1103515245u + 12345u;
		int tip = static_cast<int>((m_random >> 16) % kGameTips) + 1;
		if (tip == m_lastTip) {
			tip = tip % kGameTips + 1;
		}
		m_lastTip = tip;
		return Value(localized("TEXT_gametip" + std::to_string(tip)));
	});
	// 0x1003ca60 / 0x1003bff0: (id, default) and (id, value); the
	// slideshow keeps its page here between runs.
	n.setFunction(kLogic, "getStorageString", [this](Interpreter& vm, const Args& args) {
		const auto it = m_storage.find(vm.toString(arg(args, 0)));
		if (it != m_storage.end()) {
			return Value(it->second);
		}
		return Value(args.size() > 1 ? vm.toString(args[1]) : std::string());
	});
	n.setFunction(kLogic, "setStorageString", [this](Interpreter& vm, const Args& args) {
		m_storage[vm.toString(arg(args, 0))] = vm.toString(arg(args, 1));
		return Value();
	});
	n.setFunction(kLogic, "eraseStorageString", [this](Interpreter& vm, const Args& args) {
		m_storage.erase(vm.toString(arg(args, 0)));
		return Value();
	});
	n.setFunction("dice.bf2.Client", "getLoadProgress", [&s](Interpreter&, const Args&) { // 0x100147b0
		return Value(std::clamp(static_cast<double>(s.loadProgress), 0.0, 1.0));
	});
	// 0x10014810: 1 when the round's end kind (SwiffHost slot 0xb0) is 1,
	// else 0. 0x100148b0: slot 0xb4's seconds. 0x10014910: always 0.
	// 0x10014970: slot 0xb8's file. 0x100149f0: slot 0xd0, a game is up.
	n.setFunction(kClient, "getEORType", [&s](Interpreter&, const Args&) { return Value(s.eorType == 1 ? 1 : 0); });
	n.setFunction(kClient, "getTimeUntilRestart", [&s](Interpreter&, const Args&) {
		return Value(static_cast<double>(s.timeUntilRestart));
	});
	n.setFunction(kClient, "getTotalTimeUntilRestart", [](Interpreter&, const Args&) { return Value(0); });
	n.setFunction(kClient, "getCurrentLoadFile", [&s](Interpreter&, const Args&) { return Value(s.currentLoadFile); });
	n.setFunction(kClient, "hasUserEscapedFromGame", [&s](Interpreter&, const Args&) { return Value(s.gameRunning); });
	// 0x10014610 / 0x10014550: GSPlayerName (slots 0x98 / 0x9c), the
	// argument the default.
	n.setFunction(kClient, "getPlayerName", [this](Interpreter& vm, const Args& args) {
		return Value(m_summary.playerName.empty() ? vm.toString(arg(args, 0)) : m_summary.playerName);
	});
	n.setFunction(kClient, "setPlayerName", [this](Interpreter& vm, const Args& args) {
		m_summary.playerName = vm.toString(arg(args, 0));
		return Value();
	});
	// 0x100146e0 / 0x100145b0: CommandLine.cmd's text ("" when it can't be
	// read; the original then raises "unable to open commandline file"),
	// and the text written there (after host event 5).
	n.setFunction(kClient, "getCommandlineOption", [this](Interpreter&, const Args&) {
		if (commandLineFile.empty()) {
			return Value(m_commandLineOption);
		}
		std::ifstream in(commandLineFile, std::ios::binary);
		std::stringstream text;
		text << in.rdbuf();
		return Value(in ? text.str() : std::string());
	});
	n.setFunction(kClient, "setCommandlineOption", [this](Interpreter& vm, const Args& args) {
		m_commandLineOption = vm.toString(arg(args, 0));
		if (!commandLineFile.empty()) {
			std::ofstream(commandLineFile, std::ios::binary) << m_commandLineOption;
		}
		return Value();
	});
	n.setFunction("dice.bf2.VideoSettings", "isOptimzingShaders", [&s](Interpreter&, const Args&) { // 0x1005d140
		return Value(s.optimizingShaders);
	});
}

// --- dice.bf2.ServerInfo (0x10054030 ...)

void EndOfRoundBindings::bindServer()
{
	ui::NativeRegistry& n = m_natives;
	const RoundSummary& s = m_summary;
	n.setFunction(kServerInfo, "getServerName", [&s](Interpreter&, const Args&) { return Value(s.serverName); });
	n.setFunction(kServerInfo, "getServerIP", [&s](Interpreter&, const Args&) { return Value(s.serverAddress); });
	n.setFunction(kServerInfo, "getWelcomeMessage", [&s](Interpreter&, const Args&) { return Value(s.welcomeMessage); });
	n.setFunction(kServerInfo, "getAllowSpectators", [&s](Interpreter&, const Args&) { return Value(s.allowSpectators); });
	n.setFunction(kServerInfo, "getSponsorLogo", [&s](Interpreter&, const Args&) { return Value(s.sponsorLogo); });
	n.setFunction(kServerInfo, "getCommunityLogo", [&s](Interpreter&, const Args&) { return Value(s.communityLogo); });
}

// --- Lists

void EndOfRoundBindings::bindLists()
{
	ui::NativeRegistry& n = m_natives;
	using List = ui::NativeList;
	const auto fill = [this](Interpreter& vm, List& list, const Args&) {
		if (list.listId == "eorPlayerList") {
			fillPlayers(vm, list);
		} else if (list.listId == "eorEarnedAwards") {
			fillAwards(vm, list);
		} else if (list.listId == "eorSquadScore") {
			fillSquads(vm, list);
		} else if (list.listId == "eorDebriefing") {
			fillDebriefing(vm, list);
		} else {
			const int shown = std::max(1, list.visibleRows(vm));
			list.firstIndex = std::clamp(list.firstIndex, 0, std::max(0, static_cast<int>(list.rows().size()) - shown));
			list.populate(vm);
		}
		return Value(false);
	};
	n.setListMethod("fillList", fill);
	n.setListMethod("refreshList", fill);
}

// --- The lists' rows

std::vector<EndOfRoundBindings::SquadScore> EndOfRoundBindings::teamSquads(int team) const
{
	std::vector<SquadScore> squads;
	for (const EndOfRoundPlayer* p : teamPlayers(team)) {
		if (p->squad <= 0) {
			continue;
		}
		auto it = std::find_if(squads.begin(), squads.end(), [p](const SquadScore& s) { return s.number == p->squad; });
		if (it == squads.end()) {
			SquadScore squad;
			squad.number = p->squad;
			for (const EndOfRoundSquad& named : m_summary.squads) {
				if (named.team == team && named.number == p->squad) {
					squad.squad = &named;
				}
			}
			squads.push_back(squad);
			it = squads.end() - 1;
		}
		if (p->squadLeader) {
			it->members.insert(it->members.begin(), p);
		} else {
			it->members.push_back(p);
		}
		it->score += p->score;
		it->teamwork += p->teamwork;
	}
	std::stable_sort(squads.begin(), squads.end(), [](const SquadScore& a, const SquadScore& b) { return a.score > b.score; });
	return squads;
}

std::string EndOfRoundBindings::squadName(const SquadScore& squad)
{
	if (squad.squad && !squad.squad->name.empty()) {
		return squad.squad->name;
	}
	static const char* const names[] = {"Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel", "India"};
	return squad.number >= 1 && squad.number <= 9 ? names[squad.number - 1] : std::to_string(squad.number);
}

int EndOfRoundBindings::overallPlace(const EndOfRoundPlayer* player) const
{
	int place = 0;
	for (const EndOfRoundPlayer& p : m_summary.players) {
		if (&p != player && (p.score > player->score || (p.score == player->score && p.deaths < player->deaths)
								|| (p.score == player->score && p.deaths == player->deaths && &p < player))) {
			++place;
		}
	}
	return place;
}

namespace {

// A row clip's child, undefined when missing.
Value child(Interpreter& vm, const Value& clip, const std::string& name)
{
	return clip.isObject() ? vm.getMember(clip, name) : Value();
}

void setText(Interpreter& vm, const Value& clip, const std::string& name, const std::string& text)
{
	const Value field = child(vm, clip, name);
	if (field.isObject()) {
		vm.setMember(field, "text", Value(text));
	}
}

void setVisible(Interpreter& vm, const Value& clip, bool visible)
{
	if (clip.isObject()) {
		vm.setMember(clip, "_visible", Value(visible));
	}
}

// The list's row clips "row0", "row1", ... (0x10119780: "row%d").
std::vector<Value> rowClips(Interpreter& vm, ui::NativeList& list)
{
	std::vector<Value> rows;
	const std::string prefix = list.rowPrefix(vm);
	for (int i = 0, n = list.visibleRows(vm); i < n; ++i) {
		rows.push_back(vm.getMember(Value(list.holder), prefix + std::to_string(i)));
	}
	return rows;
}

// The list argument n as a number (0 when missing).
int listArg(const ui::NativeList& list, size_t n)
{
	return n < list.args.size() ? std::atoi(list.args[n].c_str()) : 0;
}

} // namespace

// 0x10066c40: argument 0 the player's team, 1 the other. The commander is
// left out (the page shows them above); the last row shows the player when
// they are further down. Cells: "N. name" (N: the place in the team),
// score, team score, kills, deaths, score per minute; the player's row is
// white with its background on, a buddy has the buddy icon. The three best
// of the round get the page's gold, silver and bronze icons beside their
// score.
void EndOfRoundBindings::fillPlayers(Interpreter& vm, ui::NativeList& list)
{
	const RoundSummary& s = m_summary;
	const int team = listArg(list, 0) == 0 ? s.myTeam : 3 - s.myTeam;
	const auto all = teamPlayers(team);
	std::vector<std::pair<int, const EndOfRoundPlayer*>> shown; // (place, player)
	for (size_t i = 0; i < all.size(); ++i) {
		if (!all[i]->commander) {
			shown.emplace_back(static_cast<int>(i), all[i]);
		}
	}
	const EndOfRoundPlayer* me = s.localPlayer >= 0 && s.localPlayer < static_cast<int>(s.players.size())
		? &s.players[static_cast<size_t>(s.localPlayer)] : nullptr;
	const std::vector<Value> rows = rowClips(vm, list);
	const int count = static_cast<int>(rows.size());
	if (me && count > 0 && static_cast<int>(shown.size()) > count) {
		const auto at = std::find_if(shown.begin(), shown.end(), [me](const auto& e) { return e.second == me; });
		if (at != shown.end() && at - shown.begin() >= count - 1) {
			shown[static_cast<size_t>(count - 1)] = *at;
		}
	}
	// The page's medal icons (the scoreboard holds mcIconGold, ...).
	Value page = Value(list.holder);
	for (int up = 0; up < 6 && page.isObject() && !child(vm, page, "mcIconGold").isObject(); ++up) {
		page = vm.getMember(page, "_parent");
	}
	static const char* const medals[] = {"mcIconGold", "mcIconSilver", "mcIconBronze"};
	for (int i = 0; i < count; ++i) {
		const Value& row = rows[static_cast<size_t>(i)];
		const bool filled = i < static_cast<int>(shown.size());
		const EndOfRoundPlayer* p = filled ? shown[static_cast<size_t>(i)].second : nullptr;
		setText(vm, row, "cell0", p ? format("%d. %s", shown[static_cast<size_t>(i)].first + 1, p->name) : std::string());
		setText(vm, row, "cell1", p ? std::to_string(p->score) : std::string());
		setText(vm, row, "cell2", p ? std::to_string(p->teamwork) : std::string());
		setText(vm, row, "cell3", p ? std::to_string(p->kills) : std::string());
		setText(vm, row, "cell4", p ? std::to_string(p->deaths) : std::string());
		setText(vm, row, "cell5", p ? twoDecimals(p->scorePerMinute) : std::string());
		setVisible(vm, child(vm, row, "bg"), p && p == me);
		setVisible(vm, child(vm, row, "mcIconBuddy"), p && p->buddy);
		if (p == me) {
			for (int c = 0; c < 6; ++c) {
				const Value cell = child(vm, row, "cell" + std::to_string(c));
				if (cell.isObject()) {
					vm.setMember(cell, "textColor", Value(0xffffff));
				}
			}
		}
		const int place = p ? overallPlace(p) : -1;
		if (place >= 0 && place < 3 && page.isObject()) {
			// At the row's mcIconMedal, in the page's coordinates.
			double x = 0, y = 0;
			for (Value c = child(vm, row, "mcIconMedal"); c.isObject() && !(c.object() == page.object());
				 c = vm.getMember(c, "_parent")) {
				x += vm.toNumber(vm.getMember(c, "_x"));
				y += vm.toNumber(vm.getMember(c, "_y"));
			}
			const Value icon = child(vm, page, medals[place]);
			vm.setMember(icon, "_x", Value(x));
			vm.setMember(icon, "_y", Value(y));
		}
	}
}

// 0x10066190 / 0x10066540: four awards a page (the argument: the page);
// rows past the last are hidden. Each row: the picture in mcAward, the
// name (cell0, AWARD_NAME_<id>[_<level>]) and how often (cell1).
void EndOfRoundBindings::fillAwards(Interpreter& vm, ui::NativeList& list)
{
	const auto& awards = m_summary.awards;
	const int first = listArg(list, 0) * 4;
	const std::vector<Value> rows = rowClips(vm, list);
	for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
		const Value& row = rows[static_cast<size_t>(i)];
		const int index = first + i;
		const bool shown = i < 4 && index < static_cast<int>(awards.size());
		setVisible(vm, row, shown);
		if (!shown) {
			continue;
		}
		const EndOfRoundAward& a = awards[static_cast<size_t>(index)];
		const std::string id = a.id < 200 ? std::to_string(a.id) + "_" + std::to_string(a.level) : std::to_string(a.id);
		static const char* const folders[] = {"", "images/awards/badges/eor/badge_eor_", "images/awards/medals/eor/medal_eor_",
			"images/awards/ribbons/eor/ribbons_eor_", "images/awards/pins/eor/pin_eor_"};
		const int kind = a.id / 100;
		setText(vm, row, "cell0", localized("AWARD_NAME_" + id));
		setText(vm, row, "cell1", std::to_string(a.count));
		const Value picture = child(vm, row, "mcAward");
		if (picture.isObject() && kind >= 1 && kind <= 4) {
			vm.callMethod(picture.object(), "loadMovie", {Value(std::string(folders[kind]) + id + ".png")});
		}
	}
}

// 0x100678b0: "<0 the player's team, 1 the other> <place>". Rows: the
// squad leader (HEADER_squadleader), a gap (HEADER_members), the members,
// and on the last the squad's totals (EOR_3_summary). Cells: the label
// (cell0), the name, the score and the team score.
void EndOfRoundBindings::fillSquads(Interpreter& vm, ui::NativeList& list)
{
	const RoundSummary& s = m_summary;
	const int team = listArg(list, 0) == 0 ? s.myTeam : 3 - s.myTeam;
	const auto squads = teamSquads(team);
	const int place = listArg(list, 1);
	const SquadScore* squad = place >= 0 && place < static_cast<int>(squads.size()) ? &squads[static_cast<size_t>(place)] : nullptr;
	const EndOfRoundPlayer* me = s.localPlayer >= 0 && s.localPlayer < static_cast<int>(s.players.size())
		? &s.players[static_cast<size_t>(s.localPlayer)] : nullptr;
	const std::vector<Value> rows = rowClips(vm, list);
	const int last = static_cast<int>(rows.size()) - 1;
	for (int i = 0; i <= last; ++i) {
		const Value& row = rows[static_cast<size_t>(i)];
		std::string label, name, score, teamwork;
		const EndOfRoundPlayer* p = nullptr;
		if (i == 0) {
			label = localized("HEADER_squadleader");
			p = squad && !squad->members.empty() && squad->members.front()->squadLeader ? squad->members.front() : nullptr;
		} else if (i == 1) {
			label = localized("HEADER_members");
		} else if (i == last) {
			label = localized("EOR_3_summary");
			if (squad) {
				score = std::to_string(squad->score);
				teamwork = std::to_string(squad->teamwork);
			}
		} else if (squad) {
			const size_t skip = !squad->members.empty() && squad->members.front()->squadLeader ? 1 : 0;
			const size_t index = static_cast<size_t>(i - 2) + skip;
			p = index < squad->members.size() ? squad->members[index] : nullptr;
		}
		if (p) {
			name = p->name;
			score = std::to_string(p->score);
			teamwork = std::to_string(p->teamwork);
		}
		setText(vm, row, "cell0", squad ? label : std::string());
		setText(vm, row, "cell1", name);
		setText(vm, row, "cell2", score);
		setText(vm, row, "cell3", teamwork);
		setVisible(vm, child(vm, row, "bg"), p && p == me);
	}
}

// 0x10065ad0 (the list made by 0x100658c0 with "<category> <place>":
// 0 kits, 1 weapons, 2 vehicles; place 0 the player's, 1 the best
// teammate's, 2 the best opponent's, parseDebriefing). Four rows of a
// label (cell0) and a value (cell1). The labels are 0x1012c168's: row 0
// the place's header in capitals ("", HEADER_sameteam,
// HEADER_oppositeteam), rows 1-3 the category's three:
//   kits: TEXT_scorewithkit, TEXT_timewithkit, TEXT_scoreperminutewithkit;
//   weapons: TEXT_killswithweapon, TEXT_accuracywithweapon, TEXT_killsperminute;
//   vehicles: TEXT_killswithvehicle, TEXT_timeinvehicle, TEXT_killsperminute.
// The values: the player's name (none at place 0 for kits), the score or
// kills ("%d"), the time (0x100657b0's TEXT_timeformat_hhmm, handed the
// seconds: the record keeps the script's seconds, so the hours read as
// minutes) or for weapons the accuracy ("%d%%" of it × 100, rounded), and
// the count per minute ("%.2f" through GetNumberFormatA in the game's
// locale, 0x10007c60). A place without a record shows the labels alone.
// Place 0's first row is hidden (0x100658c0, slot 0x58). The teammate's
// values are blue (0x1e7dff, 0x10119078) and the opponent's red
// (0xdc2828, 0x1011907c), and so is row 0's label.
void EndOfRoundBindings::fillDebriefing(Interpreter& vm, ui::NativeList& list)
{
	static const char* const kLabels[] = {"", "TEXT_scorewithkit", "TEXT_timewithkit", "TEXT_scoreperminutewithkit",
		"HEADER_sameteam", "TEXT_killswithweapon", "TEXT_accuracywithweapon", "TEXT_killsperminute",
		"HEADER_oppositeteam", "TEXT_killswithvehicle", "TEXT_timeinvehicle", "TEXT_killsperminute"};
	const int category = std::clamp(listArg(list, 0), 0, 2);
	const int place = std::clamp(listArg(list, 1), 0, 2);
	const DebriefingEntry& e = m_debriefing.categories[static_cast<size_t>(category)].places[static_cast<size_t>(place)];
	const auto time = [this](int value) {
		std::string text = localized("TEXT_timeformat_hhmm");
		for (const auto& [marker, number] : {std::pair {"#HH#", value / 60}, std::pair {"#MM#", value % 60}}) {
			if (const size_t at = text.find(marker); at != std::string::npos) {
				char digits[16];
				std::snprintf(digits, sizeof(digits), "%02d", number);
				text.replace(at, 4, digits);
			}
		}
		return text;
	};
	const std::vector<Value> rows = rowClips(vm, list);
	for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
		const Value& row = rows[static_cast<size_t>(r)];
		std::string label;
		if (r == 0) {
			label = place == 0 ? std::string() : localized(kLabels[place * 4]);
			std::transform(label.begin(), label.end(), label.begin(),
				[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		} else if (r < 4) {
			label = localized(kLabels[r + category * 4]);
		}
		std::string value;
		if (e.index >= 0) {
			if (r == 0) {
				value = category == 0 && place == 0 ? std::string() : e.name;
			} else if (r == 1) {
				value = std::to_string(e.count);
			} else if (r == 2) {
				value = category == 1 ? std::to_string(static_cast<int>(std::lround(e.accuracy * 100.0f))) + "%" : time(e.seconds);
			} else if (r == 3) {
				value = twoDecimals(e.perMinute);
			}
		}
		setText(vm, row, "cell0", label);
		setText(vm, row, "cell1", value);
		if (place > 0) {
			const Value colour(place == 1 ? 0x1e7dff : 0xdc2828);
			for (const char* cell : {"cell0", "cell1"}) {
				const Value clip = child(vm, row, cell);
				if (clip.isObject() && (r == 0 || std::string(cell) == "cell1")) {
					vm.setMember(clip, "textColor", colour);
				}
			}
		}
		if (r == 0 && place == 0) {
			setVisible(vm, row, false);
		}
	}
}

} // namespace dice::hfe::menu
