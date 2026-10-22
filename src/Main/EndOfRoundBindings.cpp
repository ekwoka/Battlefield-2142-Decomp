#include "EndOfRoundBindings.h"

#include "Stats/Stats.h"
#include "UI/Player/Natives.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <regex>

namespace dice::hfe::menu {

namespace avm1 = ui::avm1;
using avm1::Interpreter;
using avm1::Value;
using Args = std::vector<Value>;

namespace {

constexpr const char* kEndOfRound = "dice.bf2.EndOfRound";
constexpr const char* kLogic = "dice.bf2.Logic";
constexpr const char* kServerInfo = "dice.bf2.ServerInfo";

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
	const std::regex mode(R"re(<mode[^>]*type\s*=\s*"([^"]*)"[^>]*>([\s\S]*?)</mode>)re");
	const std::regex mapType(R"re(<maptype([^>]*)>)re");
	const std::regex players(R"re(players\s*=\s*"(\d+)")re");
	const std::regex locid(R"re(locid\s*=\s*"([^"]*)")re");
	for (auto it = std::sregex_iterator(xml.begin(), xml.end(), mode); it != std::sregex_iterator(); ++it) {
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

void EndOfRoundBindings::setSummary(RoundSummary summary) { m_summary = std::move(summary); }

void EndOfRoundBindings::setLoading(float progress)
{
	m_summary.loading = true;
	m_summary.levelLoaded = false;
	m_summary.loadProgress = progress;
}

void EndOfRoundBindings::setLevelLoaded()
{
	m_summary.loading = false;
	m_summary.levelLoaded = true;
	m_summary.loadProgress = 1;
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
	bind("getMapGameModeName", [this] {
		const std::string key = "HEADER_" + m_summary.gameMode;
		const std::string text = localized(key);
		return Value(text == key ? m_summary.gameMode : text);
	});
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
		return round(Value(localized("HEADER_" + m_summary.previousGameMode)));
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

	// The debriefing's best kit, weapon and vehicle: no records yet.
	for (const char* member : {"getKitPosition", "getWeaponPosition", "getVehiclePosition"}) {
		bind(member, [round] { return round(Value("-/-")); });
	}
	for (const char* member : {"getKitImage", "getWeaponImage", "getVehicleImage", "getKitHeader", "getWeaponHeader",
			 "getVehicleHeader"}) {
		bind(member, [round] { return round(Value("")); });
	}
}

// --- dice.bf2.Logic and Client: the loading.

void EndOfRoundBindings::bindLogic()
{
	ui::NativeRegistry& n = m_natives;
	const RoundSummary& s = m_summary;
	// A loaded level: not loading, no progress and a round to play (the
	// movie's condition for JOIN GAME; the game's own values provisional).
	n.setFunction(kLogic, "getIsLoading", [&s](Interpreter&, const Args&) { return Value(s.loading && !s.levelLoaded); });
	n.setFunction(kLogic, "isFirstTimeLoading", [&s](Interpreter&, const Args&) { return Value(s.firstLoad); });
	n.setFunction(kLogic, "getNumRoundsLeft", [&s](Interpreter&, const Args&) {
		return Value(s.levelLoaded ? std::max(1, s.roundsLeft) : s.roundsLeft);
	});
	n.setFunction(kLogic, "isAbortLoadLevel", [](Interpreter&, const Args&) { return Value(false); });
	n.setFunction(kLogic, "isLoadingDemo", [](Interpreter&, const Args&) { return Value(false); });
	n.setFunction(kLogic, "getModVersion", [](Interpreter&, const Args&) { return Value("1.51"); });
	n.setFunction(kLogic, "enableTab", [](Interpreter&, const Args&) { return Value(); });
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
		return Value(s.levelLoaded ? 0.0 : std::clamp(static_cast<double>(s.loadProgress), 0.0, 1.0));
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

// The best players by kit, weapon and vehicle: the game kept per-kit and
// per-weapon records for the stats server; there are none yet, so the rows
// stay empty.
void EndOfRoundBindings::fillDebriefing(Interpreter& vm, ui::NativeList& list)
{
	for (const Value& row : rowClips(vm, list)) {
		for (int c = 0; c < 4; ++c) {
			setText(vm, row, "cell" + std::to_string(c), std::string());
		}
	}
}

} // namespace dice::hfe::menu
