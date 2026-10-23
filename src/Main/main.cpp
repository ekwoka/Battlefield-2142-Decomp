// bf2142: the native executable.
//
// For now this boots the engine as far as the rewrite goes: it sets up the
// virtual file system for the mod, runs the start-up scripts in the order
// the original does (FUN_00406420 and FUN_0040d270 in 1.51), and reports
// which console methods the scripts called that aren't implemented yet.
//
// usage: bf2142 [--game <install dir>] [--api <console method csv>] [--verbose]
//               [+modPath mods/bf2142] [+dedicated 1]
//               [+level Camp_Gibraltar [+gameMode gpm_cq] [+mapSize 32]]
//               [--view | --screenshot out.png] [--camera x/y/z/yaw/pitch]
//               [--nosound] [--play-sound <name>]
//               [--play [team]] [--drive <vehicle template>]
// --view opens a window showing the level (needs a build with SDL3);
// --screenshot renders one frame to a PNG instead.
// The install directory defaults to $BF2142_GAME_DIR. The API tables are
// symbols/*.console.csv (one --api each): methods listed there but not
// implemented are registered as stubs with the original's argument counts.
#include "Animation/AnimationConsole.h"
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameLogic.h"
#include "Hud/Hud.h"
#include "Input/ControlMap.h"
#include "Net/Fesl.h"
#include "Net/GameClient.h"
#include "Net/GameConnection.h"
#include "Net/GhostLayout.h"
#include "Net/Networkables.h"
#include "Net/RemoteInput.h"
#include "Net/GameEvents.h"
#include "Net/ServerBrowser.h"
#include "AI/NavMesh.h"
#include "Console/ConsoleBinding.h"
#include "Game/Bots.h"
#include "UI/Player/Player.h"
#include "IO/Localization/Localization.h"
#include "MenuBindings.h"
#include "EndOfRoundBindings.h"
#include "Game/GameWorld.h"
#include "Stats/Stats.h"
#include "Game/ScriptBridge.h"
#if BF2142_HAS_PYTHON
#include "Python/ScriptHost.h"
#endif
#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
#include "IO/File/FileManager.h"
#include "IO/File/ZipArchive.h"
#include "Geometry/GeometryTemplateManager.h"
#include "Scene/LightSettings.h"
#include "Sound/Game/SoundConsole.h"
#include "Sound/Game/SoundManager.h"
#include "Sound/Game/VfsSoundFileSystem.h"
#include "Terrain/Heightmap.h"
#include "Terrain/TerrainData.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/TemplateFiles.h"
#include "World/WorldConsole.h"
#include "Voip/VoiceChat.h"
#include "Voip/VoipNatives.h"

#if BF2142_HAS_RENDERER
#include "Render/LevelRenderer.h"
#include "Viewer.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <cmath>
#include <filesystem>
#include <optional>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace dice::hfe;

struct Options {
	std::string gameDir;
	std::vector<std::string> apiTables;
	std::string modPath = "mods/bf2142";
	std::string level;           // e.g. "Camp_Gibraltar"
	std::string gameMode = "gpm_cq";
	int mapSize = 32;
	bool dedicated = false;
	bool verbose = false;
	bool view = false;
	std::string screenshot;
	std::string camera;
	bool sound = false;          // create the audio engine (on with --view)
	std::string playSound;       // play a named sound after start-up
	std::vector<std::string> spawns; // "<template>@x/y/z[/yaw]" objects to add for viewing
	std::string animation;       // .baf played by skinned objects in the viewer
	int playTeam = 0;            // --play: control a soldier of this team (1 PAC, 2 EU)
	std::string drive;           // --drive <vehicle>: put one before the player and drive it
	int bots = 0;                // --bots N: AI players, alternately PAC and EU
	int simulate = 0;            // --simulate N: run N seconds headless, then show the scores
	bool servers = false;        // --servers: list the servers OpenSpy's master server knows
	std::string connect;         // --connect host[:port]: play on a game server
	std::string probe;           // --probe host[:port]: shake hands and show what the server sends
	uint32_t profileId = 0xffffffff; // the soldier's profile from --login
	bool fesl = false;           // --fesl: greet the account service (no login)
	bool scoreboard = false;     // --scoreboard: show it (as Tab does)
	bool spawnScreen = false;    // --spawn-screen: die first, to show the spawn screen
	bool unlockAll = false;      // --unlock-all: every kit unlock (no stats server to ask)
	std::string menu;            // --menu [mainMenu|endOfRound]: the Flash menu over everything
	float menuSeconds = 10;      // --menu-seconds: how long a screenshot lets the menu play first
	std::vector<std::string> console; // --console "<command>": run once the round has started
	bool offline = false;        // --offline: the menu finds no account service (offline mode)
	std::string inputScript;     // --input-script "<s>:<command> ...": scripted input in the window (Viewer.h)
	std::string menuScript;      // --menu-script "wait:12 click:x,y type:text ...": drive the menu headless
	std::string login;           // --login name:password[@soldier]: log in to the account service
	std::string dumpTemplate;    // --dump-template: print a template's tree and settings
	// Set by a game started from the menu (server settings).
	float botSkill = -1;         // 0..1, -1 for the AI scripts' own
	int ticketRatio = 100;
	int timeLimit = 0;
	int scoreLimit = 0;
	int maxPlayers = 64;
};

bool parseOptions(int argc, char** argv, Options& options)
{
	if (const char* dir = std::getenv("BF2142_GAME_DIR")) {
		options.gameDir = dir;
	}
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		const bool hasValue = i + 1 < argc;
		if (arg == "--game" && hasValue) {
			options.gameDir = argv[++i];
		} else if (arg == "--api" && hasValue) {
			options.apiTables.push_back(argv[++i]);
		} else if (arg == "--view") {
			options.view = true;
			options.sound = true;
		} else if (arg == "--nosound") {
			options.sound = false;
		} else if (arg == "--play-sound" && hasValue) {
			options.playSound = argv[++i];
			options.sound = true;
		} else if (arg == "--screenshot" && hasValue) {
			options.screenshot = argv[++i];
		} else if (arg == "--camera" && hasValue) {
			options.camera = argv[++i];
		} else if (arg == "--connect" && hasValue) {
			options.connect = argv[++i];
			options.view = options.screenshot.empty();
		} else if (arg == "--probe" && hasValue) {
			options.probe = argv[++i];
		} else if (arg == "--servers") {
			options.servers = true;
		} else if (arg == "--simulate" && hasValue) {
			options.simulate = std::atoi(argv[++i]);
		} else if (arg == "--bots" && hasValue) {
			options.bots = std::atoi(argv[++i]);
		} else if (arg == "--menu") {
			options.menu = hasValue && argv[i + 1][0] != '-' && argv[i + 1][0] != '+' ? argv[++i] : "mainMenu";
			options.view = options.screenshot.empty();
		} else if (arg == "--console" && hasValue) {
			options.console.push_back(argv[++i]);
		} else if (arg == "--offline") {
			options.offline = true;
		} else if (arg == "--input-script" && hasValue) {
			options.inputScript = argv[++i];
		} else if (arg == "--menu-script" && hasValue) {
			options.menuScript = argv[++i];
			if (options.menu.empty()) {
				options.menu = "mainMenu";
			}
		} else if (arg == "--menu-seconds" && hasValue) {
			options.menuSeconds = static_cast<float>(std::atof(argv[++i]));
		} else if (arg == "--unlock-all") {
			options.unlockAll = true;
		} else if (arg == "--spawn-screen") {
			options.spawnScreen = true;
		} else if (arg == "--scoreboard") {
			options.scoreboard = true;
		} else if (arg == "--fesl") {
			options.fesl = true;
		} else if (arg == "--login" && hasValue) {
			options.login = argv[++i];
		} else if (arg == "--drive" && hasValue) {
			options.drive = argv[++i];
			if (options.playTeam == 0) {
				options.playTeam = 2;
			}
		} else if (arg == "--spawn" && hasValue) {
			options.spawns.push_back(argv[++i]);
		} else if (arg == "--play") {
			options.playTeam = 2;
			if (hasValue && (std::string(argv[i + 1]) == "1" || std::string(argv[i + 1]) == "2")) {
				options.playTeam = std::atoi(argv[++i]);
			}
			options.view = true;
		} else if (arg == "--dump-template" && hasValue) {
			options.dumpTemplate = argv[++i];
		} else if (arg == "--animation" && hasValue) {
			options.animation = argv[++i];
		} else if (arg == "--verbose") {
			options.verbose = true;
		} else if (arg == "+modPath" && hasValue) {
			options.modPath = argv[++i];
		} else if (arg == "+level" && hasValue) {
			options.level = argv[++i];
		} else if (arg == "+gameMode" && hasValue) {
			options.gameMode = argv[++i];
		} else if (arg == "+mapSize" && hasValue) {
			options.mapSize = std::atoi(argv[++i]);
		} else if (arg == "+dedicated" && hasValue) {
			options.dedicated = std::atoi(argv[++i]) != 0;
		} else if (arg[0] == '+' && hasValue) {
			++i; // other "+setting value" options are not used yet
		} else {
			std::fprintf(stderr, "unknown option %s\n", arg.c_str());
			return false;
		}
	}
	if (options.gameDir.empty()) {
		std::fprintf(stderr, "usage: bf2142 --game <BF2142 install dir> [--api symbols/BF2142.exe.console.csv]\n");
		return false;
	}
	return true;
}

// Unimplemented methods: calls are counted per method.
class StubMethods {
public:
	// Reads object,method,kind,access,min,max,... rows.
	int load(Console& console, const std::string& path)
	{
		std::ifstream in(path);
		std::string line;
		std::getline(in, line); // header
		int count = 0;
		while (std::getline(in, line)) {
			std::vector<std::string> cols;
			std::stringstream row(line);
			for (std::string col; std::getline(row, col, ',');) {
				cols.push_back(col);
			}
			if (cols.size() < 6 || cols[0] == "?") {
				continue;
			}
			const std::string name = cols[0] + "." + cols[1];
			if (console.hasMethod(name)) {
				continue; // implemented
			}
			// Properties, and one-argument methods (setters such as "team" on
			// ControlPoint templates), keep their values on the template.
			const bool setter = cols[2] == "method" && cols[4] == "1" && cols[5] == "1";
			if (cols[0] == "ObjectTemplate" && (cols[2] == "property" || setter)) {
				m_untypedTemplateProperties.push_back(cols[1]);
				continue; // kept on the template, see registerUntypedTemplateProperties
			}
			ConsoleMethod method;
			method.name = name;
			method.kind = cols[2] == "property" ? ConsoleMethodKind::ReadWriteProperty
				: cols[2] == "readonly"         ? ConsoleMethodKind::ReadOnlyProperty
				: cols[2] == "writeonly"        ? ConsoleMethodKind::WriteOnlyProperty
												: ConsoleMethodKind::Method;
			method.access = ConsoleAccess::Anyone;
			method.minArgs = std::atoi(cols[4].c_str());
			method.maxArgs = std::atoi(cols[5].c_str());
			method.invoke = [this, name](const std::vector<std::string>&) {
				++m_calls[name];
				return std::string();
			};
			method.stub = true;
			console.registerMethod(std::move(method));
			++count;
		}
		return count;
	}

	const std::vector<std::string>& untypedTemplateProperties() const { return m_untypedTemplateProperties; }

	void report(size_t top) const
	{
		std::vector<std::pair<std::string, int>> calls(m_calls.begin(), m_calls.end());
		std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		long total = 0;
		for (const auto& [name, count] : calls) {
			total += count;
		}
		std::printf("%ld calls to %zu unimplemented methods; most used:\n", total, calls.size());
		for (size_t i = 0; i < calls.size() && i < top; ++i) {
			std::printf("  %7d %s\n", calls[i].second, calls[i].first.c_str());
		}
	}

private:
	std::map<std::string, int> m_calls;
	std::vector<std::string> m_untypedTemplateProperties;
};

// What the HUD shows of the game (the HUD variables the engine sets each
// frame; the names are the HUD scripts').
// The scoreboard's lists (data 2 friendly and 1 enemy players, 10 and 9
// the teams' totals): name, score, teamwork, kills, deaths, ping, players
// by score. The columns sit under the header icons
// (Ingame/Scoreboard/scoreboard_icons.tga at x 233, 143 wide: score,
// teamwork, kills, deaths, ping); the list's own drawing (RendDX9.dll) is
// not decompiled, so the layout is read off the scripts.
void setupScoreboard(hud::HudManager& hud)
{
	const std::vector<hud::HudManager::ListColumn> columns {
		{8, 200, 0}, {214, 30, 2}, {245, 30, 2}, {278, 30, 2}, {308, 30, 2}, {342, 30, 2}};
	for (int data : {1, 2, 9, 10}) {
		hud.setListColumns(data, columns);
	}
}

void updateTeams(hud::HudManager& hud, const game::GameWorld& round, const game::GameLogic& logic)
{
	const int friendly = round.playerTeam() == 1 ? 1 : 2;
	// The teams' names and flags (0x0079a5f0: HUD_TEXT_MENU_SPAWN_ARMY_MEC
	// and _USMC for PAC and EU, flags by the team's name).
	for (int side = 0; side < 2; ++side) {
		const int team = side == 0 ? friendly : 3 - friendly;
		const std::string prefix = side == 0 ? "Friendly" : "Enemy";
		hud.setString(prefix + "TeamNameString", team == 1 ? "HUD_TEXT_MENU_SPAWN_ARMY_MEC" : "HUD_TEXT_MENU_SPAWN_ARMY_USMC");
		hud.setString(prefix + "FlagIconPathString", "Ingame/Flags/Icons/Hud/Score/" + logic.team(team).name + "/scoreBoard_Flag.tga");
		hud.setString(prefix + "TeamWinsString", "0");
	}
}

void updateScoreboard(hud::HudManager& hud, const game::GameWorld& round, const std::function<int(int)>& scoreOf)
{
	const int friendly = round.playerTeam() == 1 ? 1 : 2;
	for (int side = 0; side < 2; ++side) {
		const int team = side == 0 ? friendly : 3 - friendly;
		std::vector<game::GameWorld::PlayerInfo> players;
		for (const game::GameWorld::PlayerInfo& player : round.playerList()) {
			if (player.team == team) {
				players.push_back(player);
			}
		}
		const auto score = [&scoreOf](const game::GameWorld::PlayerInfo& p) {
			return scoreOf ? scoreOf(p.id) : p.kills * 2 - p.teamKills * 4;
		};
		std::stable_sort(players.begin(), players.end(), [&](const auto& a, const auto& b) { return score(a) > score(b); });
		std::vector<std::vector<std::string>> rows;
		int totalScore = 0, totalKills = 0, totalDeaths = 0;
		for (const game::GameWorld::PlayerInfo& p : players) {
			rows.push_back({p.name, std::to_string(score(p)), "0", std::to_string(p.kills), std::to_string(p.deaths),
				p.ai ? "BOT" : "0"});
			totalScore += score(p);
			totalKills += p.kills;
			totalDeaths += p.deaths;
		}
		hud.setList(side == 0 ? 2 : 1, std::move(rows));
		hud.setList(side == 0 ? 10 : 9, {{"", std::to_string(totalScore), "0", std::to_string(totalKills),
											   std::to_string(totalDeaths), ""}});
	}
}

// Map markers that pick squad spawns (control points use their ids).
constexpr int kMarkerLeader = 100000;
constexpr int kMarkerBeacon = 100001;

// The map: the minimap while playing, the big map with the spawn choices
// while dead (the spawn screen). Markers: control points by owner (with
// the spawn icons on the spawn screen, clickable), the team's soldiers,
// and the player's heading arrow.
void updateMap(hud::HudManager& hud, game::GameWorld& round, const game::GameLogic& logic, bool commanderScreen)
{
	game::GameWorld::PlayerInfo me;
	const bool known = round.playerInfo(round.localPlayer(), me);
	const bool spawning = known && !me.alive;
	const bool big = spawning || commanderScreen;
	hud.set("MapShow", known ? 1 : 0);
	hud.set("MapMinSize", big ? 0 : 1);
	hud.set("MapFullSize", big ? 1 : 0);
	hud.set("MapFullSizeAndSpawnShow", spawning ? 1 : 0);
	hud.set("SpawnShow", spawning ? 1 : 0);
	if (!known) {
		return;
	}
	const auto folder = [&logic](int team) { return team == 0 ? std::string("Neutral") : logic.team(team).name; };
	std::vector<hud::HudManager::MapMarker> markers;
	const std::vector<int> spawnable = round.spawnablePoints(me.team);
	const int chosen = round.spawnChoice(me.id);
	for (const auto& point : round.controlPointList()) {
		hud::HudManager::MapMarker flag {"Ingame/Flags/Icons/Minimap/" + folder(point.team) + "/miniMap_CP.tga",
			point.position.x, point.position.z, 32};
		markers.push_back(flag);
		if (spawning && std::find(spawnable.begin(), spawnable.end(), point.id) != spawnable.end()) {
			hud::HudManager::MapMarker spawn {std::string("Ingame/Minimap/Icons/") +
					(point.id == chosen ? "spawn_Selected.dds" : "spawn_UnSelected.dds"),
				point.position.x, point.position.z, 32};
			spawn.id = point.id;
			markers.push_back(spawn);
		}
	}
	// The squad: its leader (to spawn on) and the leader's beacon.
	if (spawning && me.squad != 0) {
		for (const auto& player : round.playerList()) {
			if (player.alive && player.squadLeader && player.team == me.team && player.squad == me.squad && player.id != me.id
				&& !player.vehicle) {
				hud::HudManager::MapMarker leader {"Ingame/Player/Icons/Minimap/SquadLeaders/mini_Squad_" + std::to_string(me.squad) +
						"_small.dds",
					player.position.x, player.position.z, 32};
				leader.id = kMarkerLeader;
				markers.push_back(leader);
			}
		}
		if (const world::Object* beacon = round.objects().find(round.squadBeacon(me.team, me.squad))) {
			hud::HudManager::MapMarker spawn {std::string("Ingame/Minimap/Icons/") +
					(chosen == game::GameWorld::kSpawnOnBeacon ? "spawn_SquadSelected.dds" : "spawn_SquadUnSelected.dds"),
				beacon->position().x, beacon->position().z, 32};
			spawn.id = kMarkerBeacon;
			markers.push_back(spawn);
		}
	}
	for (const auto& player : round.playerList()) {
		if (!player.alive || player.team != me.team || player.id == me.id) {
			continue;
		}
		// Players: 16 px, tinted 0.7 grey (table 0x00b3be30).
		hud::HudManager::MapMarker mate {"Ingame/Player/Icons/Minimap/mini_Soldier.dds", player.position.x, player.position.z, 16};
		mate.color[0] = mate.color[1] = mate.color[2] = 0.7f;
		markers.push_back(mate);
	}
	// The commanders' work: the UAV's ring, strikes on their way, supply
	// crates, and the squad's order.
	physics::Vec3 uav;
	float uavRadius = 0;
	if (round.uavArea(me.team, uav, uavRadius)) {
		markers.push_back({"Ingame/Minimap/Icons/icon_UAV.tga", uav.x, uav.z, 24});
	}
	for (const auto& strike : round.strikes()) {
		if (strike.team == me.team) {
			markers.push_back({strike.ability == game::GameWorld::CommanderAbility::OrbitalStrike
					? "Ingame/Commander/tools/orbit_strike.tga"
					: "Ingame/Commander/tools/force_strike.tga",
				strike.target.x, strike.target.z, 24});
		}
	}
	for (const uint32_t crate : round.supplyCrates()) {
		if (const world::Object* object = round.objects().find(crate)) {
			markers.push_back({"Ingame/Minimap/Icons/icon_supplies.tga", object->position().x, object->position().z, 20});
		}
	}
	if (const auto* order = me.squad ? round.squadOrder(me.team, me.squad) : nullptr) {
		using Order = game::GameWorld::OrderType;
		const bool attack = order->type == Order::Attack || order->type == Order::MapAttack;
		const bool defend = order->type == Order::Defend || order->type == Order::MapDefend;
		const char* icon = attack ? "orderAttackCommander.tga" : defend ? "orderDefendCommander.tga" : "orderMoveCommander.tga";
		markers.push_back({std::string("Ingame/Minimap/Icons/") + icon, order->position.x, order->position.z, 24});
	}
	// Enemies a recon drone's pulse revealed to the team, in red.
	// The satellite scan's enemies, shown to the commander only (0x0057cfb0).
	std::vector<int> revealed = round.revealedPlayers(me.team);
	for (const int id : round.scannedPlayers(me.id)) {
		if (std::find(revealed.begin(), revealed.end(), id) == revealed.end()) {
			revealed.push_back(id);
		}
	}
	for (const int id : revealed) {
		game::GameWorld::PlayerInfo enemy;
		if (round.playerInfo(id, enemy) && enemy.alive) {
			hud::HudManager::MapMarker spotted {"Ingame/Player/Icons/Minimap/mini_Soldier.dds", enemy.position.x,
				enemy.position.z, 16};
			spotted.color[1] = 0.3f;
			spotted.color[2] = 0.3f;
			markers.push_back(spotted);
		}
	}
	hud.setMapInVehicle(me.alive && me.vehicle != 0);
	if (me.alive) {
		// The player's own icon and its heading arrow, 32 px (0x00792980).
		markers.push_back({"Ingame/Player/Icons/Minimap/mini_LocalArrow2.tga", me.position.x, me.position.z, 32, me.yaw});
		hud::HudManager::MapMarker arrow {"Ingame/Player/Icons/Minimap/mini_headingArrow.dds", me.position.x, me.position.z, 32,
			me.yaw};
		markers.push_back(arrow);
		hud.setMapFocus(me.position.x, me.position.z);
	}
	hud.setMapMarkers(std::move(markers));
}

// The commander's tools on the HUD (HudElementsCommander*.con): whether
// the screen shows, which tools have their asset, its armour, and the
// recharge bars.
void updateCommander(hud::HudManager& hud, const game::GameWorld& round, bool screen)
{
	game::GameWorld::PlayerInfo me;
	const bool commander = round.playerInfo(round.localPlayer(), me) && me.commander;
	for (const char* show : {"CommanderShow", "CommanderMenuShow", "CommanderInterfaceShow"}) {
		hud.set(show, commander && screen ? 1 : 0);
	}
	if (!commander) {
		return;
	}
	using Ability = game::GameWorld::CommanderAbility;
	struct Tool {
		Ability ability;
		const char* name;      // CommanderNo<name>Show, <name>Armor
		const char* reloading; // <reloading>Reloading, <reloading>TimeUntilReloaded, <reloading>ShowFullStatus
	};
	for (const Tool& tool : {Tool {Ability::OrbitalStrike, "OrbitalStrike", "OrbitalStrike"},
			 Tool {Ability::Artillery, "Artillery", "Artillery"}, Tool {Ability::Uav, "UAV", "MinimapUAV"},
			 Tool {Ability::Satellite, "Satellite", "MinimapSatellite"}, Tool {Ability::Supplies, "Supply", "Supply"}}) {
		const auto state = round.abilityState(me.team, tool.ability);
		hud.set(std::string("CommanderNo") + tool.name + "Show", state.available ? 0 : 1);
		hud.set(std::string(tool.name) + "Armor", state.armor);
		hud.set(std::string(tool.reloading) + "Reloading", state.ready ? 0 : 1);
		hud.set(std::string(tool.reloading) + "TimeUntilReloaded", state.reloaded);
		hud.set(std::string(tool.reloading) + "ShowFullStatus", state.ready ? 1 : 0);
	}
}

void updateHud(hud::HudManager& hud, game::GameWorld& round, const game::GameLogic& logic, bool scoreboard = false,
	const std::function<int(int)>& scoreOf = {}, bool commanderScreen = false)
{
	updateMap(hud, round, logic, commanderScreen);
	updateCommander(hud, round, commanderScreen);
	updateTeams(hud, round, logic);
	hud.set("ScoreboardShow", scoreboard ? 1 : 0);
	hud.set("ToggleScore", 1);
	if (scoreboard) {
		updateScoreboard(hud, round, scoreOf);
	}
	const world::Object* soldier = round.player();
	const bool alive = soldier != nullptr;
	const bool inVehicle = round.playerVehicle() != nullptr;
	// The in-game HUD stays up while dead (the spawn screen is part of it);
	// the soldier's own items have their own show variables.
	const bool connected = round.localPlayer() >= 0;
	hud.set("ShowIngameHud", connected ? 1 : 0);
	hud.set("ShowSoldierHud", connected ? 1 : 0);
	hud.set("ShowVehicleHud", inVehicle ? 1 : 0);
	hud.set("InVehicle", inVehicle ? 1 : 0);
	hud.set("MenuBackgroundAlpha", 1);
	hud.set("CrosshairIconShow", alive && !inVehicle ? 1 : 0);
	hud.set("HitIndicatorIconShow", 0);
	// In a vehicle: its icon and armour, and a gunner's gun (its HUD by
	// the firearm's weaponHud.guiIndex, the turret's direction).
	hud.set("VehicleIconShow", inVehicle ? 1 : 0);
	hud.set("PrimaryHeatShow", 0);
	hud.set("TurretIconShow", 0);
	if (const world::Object* vehicle = round.playerVehicle()) {
		const auto& settings = vehicle->objectTemplate()->settings();
		const auto icon = settings.find("vehicleHud.vehicleIcon");
		hud.setString("VehicleIconPathString", icon != settings.end() ? icon->second : std::string());
		const float left = round.hitPoints(vehicle->id());
		const auto most = settings.find("armor.maxHitPoints");
		const float max = most != settings.end() ? static_cast<float>(std::atof(most->second.c_str())) : 0;
		hud.set("VehicleArmor", max > 0 ? std::clamp(left / max, 0.0f, 1.0f) : 1.0f);
		hud.setString("VehicleArmorString", std::to_string(static_cast<int>(std::max(left, 0.0f))));
		game::GameWorld::PlayerInfo me;
		float yaw = 0, pitch = 0;
		if (round.playerInfo(round.localPlayer(), me) && round.gunAim(vehicle->id(), me.seat, yaw, pitch)) {
			const auto seats = round.vehicleSeats(vehicle->id());
			const world::ObjectTemplate* gun = round.templates().find(seats[static_cast<size_t>(me.seat)].weapon);
			const auto gui = gun ? gun->settings().find("weaponHud.guiIndex") : settings.end();
			hud.set("GuiIndex", gun && gui != gun->settings().end() ? static_cast<float>(std::atoi(gui->second.c_str())) : 0);
			hud.set("CrosshairIconShow", 1);
			hud.set("TurretIconShow", 1);
			hud.set("VehicleTurretAngle", yaw);
			if (const game::FireArm* firearm = round.gun(vehicle->id(), me.seat); firearm && firearm->settings().heatAddWhenFire > 0) {
				hud.set("PrimaryHeatShow", 1);
				hud.set("PrimaryHeatValue", firearm->heat());
				hud.set("PrimaryHeatBlink", firearm->overheated() ? 1 : 0);
			}
		}
	}
	// Compass headings, degrees clockwise from north (+z): the vehicle's
	// and, for the minimap's ring, the soldier's.
	const auto heading = [](float yaw) { return yaw - 360.0f * std::floor(yaw / 360.0f); };
	if (const world::Object* vehicle = round.playerVehicle()) {
		hud.set("VehicleAngle", heading(vehicle->rotation().x));
	}
	if (soldier) {
		hud.set("MinimapDelayedMapAngle", heading(soldier->rotation().x));
	}
	hud.set("PlayerHealthShow", alive ? 1 : 0);
	hud.set("PlayerHealth", 1);
	hud.setString("PlayerHealthString", "100");
	const game::FireArm* weapon = round.playerWeapon();
	hud.set("PrimaryAmmoShow", weapon && !inVehicle ? 1 : 0);
	hud.set("UsingChildHud", weapon && !inVehicle ? 1 : 0);
	hud.set("PrimaryAmmoBarShow", 1);
	if (weapon) {
		const int magSize = std::max(weapon->settings().magSize, 1);
		hud.set("PrimaryAmmo", static_cast<float>(weapon->roundsInMag()) / static_cast<float>(magSize));
		hud.setString("PrimaryAmmoString", std::to_string(weapon->roundsInMag()));
		// -1: magazines without end (ammo.nrOfMags -1).
		hud.setString("PrimaryClipString", weapon->magsLeft() < 0 ? std::string() : std::to_string(weapon->magsLeft()));
	}
	const int team = round.playerTeam() == 1 ? 1 : 2, enemy = 3 - team;
	hud.setString("FriendlyTicketsString", std::to_string(static_cast<int>(logic.team(team).tickets)));
	hud.setString("EnemyTicketsString", std::to_string(static_cast<int>(logic.team(enemy).tickets)));
	if (soldier) {
		if (const world::Object* rifle = round.objects().find(round.playerWeaponObject())) {
			const auto& settings = rifle->objectTemplate()->settings();
			const auto gui = settings.find("weaponHud.guiIndex");
			hud.set("GuiIndex", gui != settings.end() ? static_cast<float>(std::atoi(gui->second.c_str())) : 0);
			// The crosshair's parts (0x007be770): the spread in pixels is
			// the deviation (degrees) times weaponHud.deviationFactor
			// (default 10); the parts are 20 long, about the 800 x 600
			// centre. The deviation stays at its minimum (not modelled yet).
			const auto number = [&settings](const char* key, float otherwise) {
				const auto it = settings.find(key);
				return it != settings.end() ? static_cast<float>(std::atof(it->second.c_str())) : otherwise;
			};
			const float spread = std::ceil(number("deviation.minDev", 0) * number("weaponHud.deviationFactor", 10));
			constexpr float kLength = 20;
			hud.set("CrossRightPos", 400 + spread);
			hud.set("CrossLeftPos", 399 - kLength - spread);
			hud.set("CrossUpPos", 300 - kLength - spread);
			hud.set("CrossDownPos", 301 + spread);
		}
	}
}

// The levels the menu offers: every Levels/<level>/server.zip's
// GameModes/<mode>/<size>/ with gameplay objects.
std::vector<dice::hfe::menu::MenuMap> scanMenuMaps(const Options& options)
{
	namespace fs = std::filesystem;
	std::vector<dice::hfe::menu::MenuMap> maps;
	std::error_code error;
	for (const auto& entry : fs::directory_iterator(fs::path(options.gameDir) / options.modPath / "Levels", error)) {
		const auto zip = io::ZipArchive::open((entry.path() / "server.zip").string());
		if (!zip) {
			continue;
		}
		for (const std::string& name : zip->listRecursive("gamemodes")) {
			// gamemodes/<mode>/<size>/gameplayobjects.con
			const size_t mode = name.find('/') + 1, size = name.find('/', mode) + 1, file = name.find('/', size) + 1;
			if (mode == 0 || size == 0 || file == 0 || name.compare(file, std::string::npos, "gameplayobjects.con") != 0) {
				continue;
			}
			maps.push_back({entry.path().filename().string(), name.substr(mode, size - mode - 1),
				std::atoi(name.substr(size, file - size - 1).c_str())});
		}
	}
	return maps;
}

// The end-of-round movie's account of the round just over: the outcome,
// the scoreboard (scripts' scores) and the local player's totals. Ranks,
// awards and bonuses stay a recruit's (no stats server).
dice::hfe::menu::RoundSummary roundSummary(const Options& options, const game::GameWorld& round,
	const game::GameLogic& logic, const std::function<int(int)>& scoreOf, const std::string& serverName)
{
	dice::hfe::menu::RoundSummary summary;
	summary.validRound = true;
	summary.mapPath = summary.previousMapPath = options.level;
	summary.gameMode = summary.previousGameMode = options.gameMode;
	summary.mapSize = summary.previousMapSize = options.mapSize;
	summary.maxPlayers = options.maxPlayers;
	summary.serverName = serverName;
	summary.roundsLeft = 999; // a local game's (0x004d3450)
	for (int team = 1; team <= 2; ++team) {
		if (!logic.team(team).name.empty()) {
			summary.teamNames[static_cast<size_t>(team - 1)] = logic.team(team).name;
		}
	}
	summary.winningTeam = logic.winner();
	if (summary.winningTeam == 1 || summary.winningTeam == 2) {
		summary.victories[static_cast<size_t>(summary.winningTeam - 1)] = 1;
	}
	const float minutes = std::max(logic.elapsed / 60.0f, 1.0f / 60.0f);
	summary.roundSeconds = logic.elapsed;
	for (const auto& player : round.playerList()) {
		dice::hfe::menu::EndOfRoundPlayer row;
		row.name = player.name;
		row.team = player.team;
		row.score = scoreOf ? scoreOf(player.id) : player.kills * 2;
		row.kills = player.kills;
		row.deaths = player.deaths;
		row.scorePerMinute = static_cast<float>(row.score) / minutes;
		row.squad = player.squad;
		row.squadLeader = player.squadLeader;
		if (player.id == round.localPlayer()) {
			summary.localPlayer = static_cast<int>(summary.players.size());
			summary.myTeam = player.team == 1 ? 1 : 2;
			summary.score = row.score;
			summary.kills = row.kills;
			summary.deaths = row.deaths;
			summary.scorePerMinute = row.scorePerMinute;
			summary.roundExp = std::max(0, row.score);
			summary.totalExp = summary.roundExp;
		}
		summary.players.push_back(row);
	}
	return summary;
}

// dice.bf2.Sound.playSound/stopSound: the menu sounds Menu/MenuSounds.con
// added (sound.addMenuSound), by name.
void bindMenuSounds(ui::Player& movie, sound::SoundManager& sounds)
{
	movie.natives().setFunction("dice.bf2.Sound", "playSound", [&sounds](ui::avm1::Interpreter& vm, const std::vector<ui::avm1::Value>& args) {
		if (!args.empty()) {
			sounds.playSound(vm.toString(args[0]));
		}
		return ui::avm1::Value();
	});
	movie.natives().setFunction("dice.bf2.Sound", "stopSound", [&sounds](ui::avm1::Interpreter& vm, const std::vector<ui::avm1::Value>& args) {
		if (!args.empty()) {
			sounds.stopSound(vm.toString(args[0]));
		}
		return ui::avm1::Value();
	});
}

// endOfRound.dfm's files: "$/Levels/..." and "/Levels/..." (the map's
// pictures) from the game's root, the rest beside the movie.
std::function<bool(const std::string&, std::vector<uint8_t>&)> endOfRoundReader(const io::FileManager& files)
{
	return [&files](const std::string& requested, std::vector<uint8_t>& out) {
		const std::string path = requested.rfind('$', 0) == 0 ? requested.substr(1) : requested;
		return path.rfind('/', 0) == 0 ? files.readFile(path.substr(1), out)
									   : files.readFile("Menu/External/FlashMenu/" + path, out);
	};
}

// Plays the menu headless: "wait:<seconds>", "click:<x>,<y>" (stage
// pixels), "type:<text>", separated by spaces; stops early once `done`.
void runMenuScript(ui::Player& menu, const std::string& script, const std::function<bool()>& done)
{
	std::istringstream commands(script);
	for (std::string command; commands >> command && !done();) {
		const size_t colon = command.find(':');
		const std::string op = command.substr(0, colon);
		const std::string arg = colon == std::string::npos ? std::string() : command.substr(colon + 1);
		if (op == "wait") {
			for (float t = 0; t < std::atof(arg.c_str()) && !done(); t += 1.0f / 30) {
				menu.advance(1.0f / 30);
			}
		} else if (op == "click") {
			const float x = static_cast<float>(std::atof(arg.c_str()));
			const float y = static_cast<float>(std::atof(arg.substr(arg.find(',') + 1).c_str()));
			menu.mouseMove(x, y);
			menu.advance(1.0f / 30);
			menu.mouseDown();
			menu.advance(1.0f / 30);
			menu.mouseUp();
		} else if (op == "type") {
			for (const unsigned char c : arg) {
				menu.keyDown(static_cast<uint32_t>(std::toupper(c)), c);
				menu.keyUp(static_cast<uint32_t>(std::toupper(c)));
			}
		} else {
			std::fprintf(stderr, "menu script: unknown command %s\n", command.c_str());
		}
	}
}

// Where rebound keys are kept: Controls.con's format, next to the
// menu's settings.
std::string controlsFile()
{
	const std::string settings = dice::hfe::menu::MenuBindings::defaultSettingsFile();
	return settings.empty() ? std::string() : (std::filesystem::path(settings).parent_path() / "controls.con").string();
}

// A Flash key code (Key.getCode) as the control maps name the key
// ("IDKey_Q"; "" for keys they do not name).
std::string controlForFlashKey(int key)
{
	if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
		return std::string("IDKey_") + static_cast<char>(key);
	}
	if (key >= 112 && key <= 123) {
		return "IDKey_F" + std::to_string(key - 111);
	}
	static const std::pair<int, const char*> named[] = {{32, "Space"}, {16, "LeftShift"}, {17, "LeftCtrl"}, {18, "LeftAlt"},
		{9, "Tab"}, {13, "Enter"}, {8, "Backspace"}, {46, "Delete"}, {45, "Insert"}, {36, "Home"}, {35, "End"},
		{33, "PageUp"}, {34, "PageDown"}, {38, "ArrowUp"}, {40, "ArrowDown"}, {37, "ArrowLeft"}, {39, "ArrowRight"},
		{20, "Capital"}, {192, "Grave"}};
	for (const auto& [code, name] : named) {
		if (key == code) {
			return std::string("IDKey_") + name;
		}
	}
	return std::string();
}

#if BF2142_HAS_RENDERER
// The viewer's mouse and keys to the Flash menu.
std::function<void(const ViewerOptions::MenuInput&)> menuInput(ui::Player& menu, std::function<void(int flashKey)> onKey = {})
{
	return [&menu, onKey](const ViewerOptions::MenuInput& input) {
		using Kind = ViewerOptions::MenuInput::Kind;
		switch (input.kind) {
		case Kind::Move:
			menu.mouseMove(input.x * menu.stageWidth(), input.y * menu.stageHeight());
			break;
		case Kind::Down:
			menu.mouseMove(input.x * menu.stageWidth(), input.y * menu.stageHeight());
			menu.mouseDown();
			break;
		case Kind::Up:
			menu.mouseMove(input.x * menu.stageWidth(), input.y * menu.stageHeight());
			menu.mouseUp();
			break;
		case Kind::Wheel:
			menu.mouseWheel(input.key);
			break;
		case Kind::Key:
			if (onKey && input.key) {
				onKey(input.key);
			}
			if (input.key) {
				// Keys that type something come again as Text.
				const bool control = input.key < 32 || (input.key >= 33 && input.key <= 46 && input.key != 32);
				menu.keyDown(input.key, control ? static_cast<uint32_t>(input.key == 13 ? 13 : input.key == 8 ? 8 : 0) : 0);
			}
			break;
		case Kind::KeyUp:
			if (input.key) {
				menu.keyUp(input.key);
			}
			break;
		case Kind::Text:
			for (const unsigned char c : input.text) {
				if (c >= 32 && c < 127) {
					menu.keyDown(0, c);
					menu.keyUp(0);
				}
			}
			break;
		}
	};
}
#endif

// What carries over from one map to the next: the menu (its login and
// pages stay as they were) and its playlist.
struct Session {
	input::ControlMaps controls;
	ui::Player menu;
	std::unique_ptr<dice::hfe::menu::MenuBindings> menuBindings;
	bool menuQuit = false;
	std::optional<dice::hfe::menu::GameSetup> startedGame;
	bool disconnect = false; // Logic.disconnectGame: back to the menu
	std::vector<dice::hfe::menu::MenuMap> playlist;
	size_t current = 0;
	int roundsPerMap = 1;
	int roundsOnMap = 0;
	bool nextMap = false; // the round ended: the playlist goes on
	// A game on a server (--connect, or the menu's join): the connection
	// and the replicated world, for as long as it lasts.
	std::unique_ptr<net::GameClient> client;
	bool joinRequested = false; // the menu joined a server
	bool levelChange = false;   // the server changed maps: load its new level
	// Voice chat (Voip/VoiceChat.h): the menu's settings and mic test, the
	// joined server's voice; updated on its own thread.
	std::unique_ptr<dice::hfe::voip::VoiceChat> voice;
#if BF2142_HAS_RENDERER
	std::unique_ptr<Window> window; // opened by the first view, kept for the next maps
#endif
};

int runGame(Options& options, Session& session);

} // namespace

int main(int argc, char** argv)
{
	Options options;
	if (!parseOptions(argc, argv, options)) {
		return 2;
	}
	// The account service: a greeting, or a login whose soldier's profile
	// id goes to the game server.
	uint32_t profileId = 0xffffffff;
	net::FeslClient fesl;
	if (options.fesl || !options.login.empty()) {
		std::string error;
		if (!fesl.connect(net::kFeslHost, net::kFeslPort, 10, &error)) {
			std::printf("%s:%u: %s\n", net::kFeslHost, net::kFeslPort, error.c_str());
			return 1;
		}
		if (options.login.empty()) {
			const auto hello = fesl.hello();
			if (!hello) {
				std::printf("%s: no greeting\n", net::kFeslHost);
				return 1;
			}
			std::printf("%s greeted us:\n", net::kFeslHost);
			for (const auto& [key, value] : hello->fields) {
				std::printf("  %s = %s\n", key.c_str(), net::feslUnquote(value).c_str());
			}
			fesl.goodbye();
			if (options.connect.empty() && options.probe.empty()) {
				return 0;
			}
		} else {
			const size_t colon = options.login.find(':');
			const size_t at = options.login.rfind('@');
			const std::string name = options.login.substr(0, colon);
			const std::string password = colon == std::string::npos
				? std::string()
				: options.login.substr(colon + 1, (at == std::string::npos || at < colon ? options.login.size() : at) - colon - 1);
			const std::string soldier = at == std::string::npos || at < colon ? std::string() : options.login.substr(at + 1);
			const auto session = net::feslLogin(fesl, name, password, soldier, &error);
			if (!session) {
				std::printf("login failed: %s\n", error.c_str());
				return 1;
			}
			std::printf("logged in as %s (user %ld); soldiers:", session->account.displayName.c_str(), session->account.userId);
			for (const std::string& s : session->account.soldiers) {
				std::printf(" %s", s.c_str());
			}
			std::printf("\nplaying as %s (profile %ld)\n", session->soldier.c_str(), session->soldierProfileId);
			profileId = static_cast<uint32_t>(session->soldierProfileId);
			if (options.connect.empty() && options.probe.empty()) {
				fesl.goodbye();
				return 0;
			}
		}
	}
	options.profileId = profileId;
	if (!options.probe.empty()) {
		const size_t colon = options.probe.rfind(':');
		const std::string host = options.probe.substr(0, colon);
		const uint16_t port = colon == std::string::npos ? net::kGamePort
														  : static_cast<uint16_t>(std::atoi(options.probe.c_str() + colon + 1));
		net::GameConnection connection;
		const net::ConnectResult result = connection.connect(host, port, {}, 6000);
		switch (result.outcome) {
		case net::ConnectResult::Outcome::Accepted: {
			std::printf("%s:%u accepted us as connection %u (server clock %u ms)\n", host.c_str(), port,
				result.connection, result.serverClock);
			// Stay connected for a while (answering pings), showing the
			// game data the server sends.
			size_t received = 0;
			uint32_t eventSequence = 0;
			connection.pump(options.verbose ? 30000 : 3000, [&](const net::ConnectedPacket& packet) {
				++received;
				// The payload after its 16-bit length.
				if (packet.payload.size() > 2) {
					const std::vector<uint8_t> payload(packet.payload.begin() + 2, packet.payload.end());
					const net::EventBlock block = net::readEventBlock(payload);
					std::printf("  data #%u: event block %u (%zu events)\n", packet.sequence, block.sequence, block.events.size());
					for (const net::ReceivedEvent& event : block.events) {
						std::printf("  event %u%s%s\n", event.type, event.challenge.empty() ? "" : ": CD-key challenge ",
							event.challenge.c_str());
						if (event.type == net::kDataTransfer) {
							if (event.transferStart) {
								std::printf("    transfer %u of %u bytes\n", event.transferType, event.transferSize);
							} else {
								std::string shown;
								for (char c : event.chunk) {
									shown += c >= 32 && c < 127 ? c : '.';
								}
								std::printf("    %zu bytes: %s\n", event.chunk.size(), shown.c_str());
							}
						}
						if (event.type == net::kCdKeyChallenge) {
							// Answer with a key (this tool has none of its own: a fixed test key).
							const uint32_t random = static_cast<uint32_t>(std::rand()) << 16 ^ static_cast<uint32_t>(std::rand());
							const std::string response = net::cdKeyResponse("XXXX-XXXX-XXXX-XXXX", event.challenge, random);
							connection.sendData(net::cdKeyResponsePayload(eventSequence++, response,
								static_cast<uint32_t>(std::rand()), profileId));
							std::printf("  answered the challenge\n");
							connection.sendData(net::connectionTypePayload(eventSequence++, 4));
						}
					}
					if (!block.complete) {
						std::printf("  (event %u and the rest not decoded yet; input block %d)\n",
							block.events.empty() ? 0 : block.events.back().type, block.inputPresent);
					}
				}
				if (options.verbose && false) {
					std::printf("  data #%u, %zu bytes:", packet.sequence, packet.payload.size());
					for (size_t b = 0; b < packet.payload.size() && b < 40; ++b) {
						std::printf(" %02x", packet.payload[b]);
					}
					std::printf("%s\n", packet.payload.size() > 40 ? " ..." : "");
				}
			});
			if (!connection.connected()) {
				std::printf("the server disconnected us (reason %u)\n", connection.disconnectReason());
			} else {
				std::printf("still connected after %zu data packets\n", received);
			}
			connection.disconnect();
			return 0;
		}
		case net::ConnectResult::Outcome::Refused:
			std::printf("%s:%u refused: %s (%u)%s%s\n", host.c_str(), port, net::refusalReason(result.reason), result.reason,
				result.message.empty() ? "" : ": ", result.message.c_str());
			return 1;
		case net::ConnectResult::Outcome::NoAnswer:
			std::printf("%s:%u did not answer\n", host.c_str(), port);
			return 1;
		case net::ConnectResult::Outcome::Error:
			std::printf("%s:%u: %s\n", host.c_str(), port, result.message.empty() ? "unexpected reply" : result.message.c_str());
			return 1;
		}
	}
	if (options.servers) {
		// The server browser's query (fields as the game's browser asks).
		const std::vector<std::string> fields {"hostname", "gamever", "mapname", "gametype", "gamevariant", "numplayers",
			"maxplayers", "password", "hostport"};
		const std::string host = net::masterServerHost(net::kGameName);
		std::string error;
		const auto list = net::fetchServerList(host, "", fields, 15, &error);
		if (!list) {
			std::fprintf(stderr, "%s: %s\n", host.c_str(), error.c_str());
			return 1;
		}
		std::printf("%s: %zu servers (we are %s)\n", host.c_str(), list->servers.size(), list->publicIp.c_str());
		for (const net::ServerEntry& server : list->servers) {
			const auto field = [&server](const char* name) {
				const auto it = server.fields.find(name);
				return it == server.fields.end() ? std::string() : it->second;
			};
			std::printf("  %-15s %5s  %-40s %-24s %s/%s  %s\n", server.ip.c_str(), field("hostport").c_str(),
				field("hostname").c_str(), field("mapname").c_str(), field("numplayers").c_str(), field("maxplayers").c_str(),
				field("gametype").c_str());
		}
		return 0;
	}

	// Map after map: a playlist started from the menu goes on until the
	// window is closed.
	Session session;
	for (;;) {
		const int result = runGame(options, session);
#if BF2142_HAS_RENDERER
		if (session.window && session.window->closed()) {
			return result;
		}
#endif
		if (session.disconnect) {
			// The menu again, on its own (without one, the run is over).
			session.disconnect = false;
			if (session.voice) {
				session.voice->leave();
			}
			session.client.reset();
			session.joinRequested = false;
			options.connect.clear();
			if (options.menu.empty()) {
				return result;
			}
			session.startedGame.reset();
			session.playlist.clear();
			options.level.clear();
			continue;
		}
		if (session.levelChange && session.client) {
			// The server's next level, on the same connection.
			session.levelChange = false;
			const auto& level = session.client->world().level();
			std::string directory = level.levelDirectory;
			while (!directory.empty() && (directory.back() == '/' || directory.back() == '\\')) {
				directory.pop_back();
			}
			const std::string wanted = directory.substr(directory.find_last_of("/\\") + 1);
			options.level = wanted;
			for (const dice::hfe::menu::MenuMap& installed : scanMenuMaps(options)) {
				if (installed.level.size() == wanted.size() &&
					std::equal(wanted.begin(), wanted.end(), installed.level.begin(), [](char x, char y) {
						return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
					})) {
					options.level = installed.level;
				}
			}
			if (!level.gameMode.empty()) {
				options.gameMode = level.gameMode;
			}
			continue;
		}
		if (!session.nextMap || session.playlist.empty()) {
			return result;
		}
		session.nextMap = false;
		if (++session.roundsOnMap >= session.roundsPerMap) {
			session.roundsOnMap = 0;
			session.current = (session.current + 1) % session.playlist.size();
		}
		const dice::hfe::menu::MenuMap& map = session.playlist[session.current];
		options.level = map.level;
		options.gameMode = map.gameMode;
		options.mapSize = map.size;
		std::printf("next map: %s (%s, %d)\n", options.level.c_str(), options.gameMode.c_str(), options.mapSize);
	}
}

namespace {

// One level's round (after the menu, when it runs first). Every manager
// lives here, so the next map starts from nothing.
int runGame(Options& options, Session& session)
{
	io::FileManager files;
	files.setRootPath(options.gameDir);
	files.addSearchPath(options.modPath);

	Console console(&files);
	std::map<std::string, int> errors;
	console.setScriptOutput(true, true);
	console.setPrintHandler([&](const std::string& text) {
		if (text.compare(0, 7, "Error: ") == 0) {
			// "Error: <message> (<file>:<line>: <text>)": count by message.
			++errors[text.substr(7, text.find(" (") - 7)];
		}
		if (options.verbose || text.compare(0, 7, "Error: ") != 0) {
			std::printf("%s\n", text.c_str());
		}
	});

	bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
		[&files](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);

	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.setCurrentFileProvider([&console] { return console.currentFile(); });
	world::registerWorldConsoleMethods(console, templates, objects);
	geom::GeometryTemplateManager geometries;
	geom::registerGeometryConsoleMethods(console, geometries);
	physics::CollisionTemplateManager collisionTemplates;
	physics::registerCollisionConsoleMethods(console, collisionTemplates);
	physics::MaterialManager materials;
	physics::registerMaterialConsoleMethods(console, materials);
	terrain::HeightmapCluster heightmaps;
	terrain::registerTerrainConsoleMethods(console, heightmaps, files);
	std::optional<terrain::TerrainData> terrainData;
	terrain::registerTerrainDataConsoleMethods(console, files, terrainData);
	scene::LightSettings lights;
	scene::registerLightConsoleMethods(console, lights);
	scene::SkySettings sky;
	scene::registerSkyConsoleMethods(console, sky);
	scene::WaterSettings water;
	scene::registerWaterConsoleMethods(console, water);

	// Sound, as 0x007fcd50: the engine from getSoundEngine() reading files
	// through the virtual file system, wrapped by the game's sound manager.
	sound::VfsSoundFileSystem soundFiles(files);
	sound::ISoundEngine* soundEngine = options.sound ? sound::getSoundEngine() : nullptr;
	if (soundEngine) {
		soundEngine->setFileSystem(&soundFiles);
		if (soundEngine->initialize(2, "0", 32) != 0) {
			std::printf("sound: engine initialisation failed, continuing without sound\n");
			sound::shutdownSoundEngine();
			soundEngine = nullptr;
		}
	}
	sound::SoundManager sounds(soundEngine, templates);
	sounds.registerTemplateType();
	sound::registerSoundConsoleMethods(console, sounds);

	// Engine constants: symbols/<module>.constants.csv next to each API table.
	for (const std::string& table : options.apiTables) {
		const size_t suffix = table.rfind(".console.csv");
		if (suffix == std::string::npos) {
			continue;
		}
		std::ifstream in(table.substr(0, suffix) + ".constants.csv");
		std::string line;
		std::getline(in, line); // header
		while (std::getline(in, line)) {
			const size_t comma = line.find(',');
			if (comma != std::string::npos) {
				console.registerConstant(line.substr(0, comma), line.substr(comma + 1));
			}
		}
	}
	// The HUD, built by Menu/HUD/HudSetup/HudSetupMain.con.
	hud::HudManager hudManager([&files](const std::string& path, std::string& text) {
		std::vector<uint8_t> data;
		if (!files.readFile(path, data)) {
			return false;
		}
		text.assign(data.begin(), data.end());
		return true;
	});
	hud::registerHudConsoleMethods(console, hudManager);
	// The player's controls (Settings/Controls.con builds the maps once;
	// the controls page's changes stay for the next maps).
	input::ControlMaps& controls = session.controls;
	input::registerControlMapConsoleMethods(console, controls);
	// Localized text (0x00405360: Localization/strings.csv in the
	// profile's language, English here; strings_patch.csv over it).
	io::Localization localization;
	for (const char* file : {"Localization/strings.csv", "Localization/strings_patch.csv"}) {
		std::vector<uint8_t> data;
		if (files.readFile(file, data)) {
			localization.load(data, "English");
		}
	}
	hudManager.setLocalizer([&localization](const std::string& id) { return localization.text(id); });
	setupScoreboard(hudManager);

	game::GameLogic gameLogic;
	game::registerGameLogicConsoleMethods(console, gameLogic);
	gameLogic.mapSize = options.mapSize;

	// The bots' settings (AI/AIBehaviours.ai, AI/AIDefault.ai).
	game::BotSettings botSettings;
	game::registerBotConsoleMethods(console, botSettings);
	// Ranks (rankSettings.setRank; Settings/RankSettings.con, 0x0043ac80).
	stats::registerRankConsoleMethods(console);
	// The kits' unlock properties the unlock ids come from (also without --api).
	world::registerUntypedTemplateProperties(console, templates, {"unlockIndex", "unlockLevel", "unlockBranch"});

	anim::AnimationSystems animationSystems(
		[&files](const std::string& path, std::vector<uint8_t>& data) { return files.readFile(path, data); });
	anim::registerAnimationConsoleMethods(console, animationSystems);

	StubMethods stubs;
	for (const std::string& table : options.apiTables) {
		std::printf("%d console methods stubbed from %s\n", stubs.load(console, table), table.c_str());
	}
	world::registerUntypedTemplateProperties(console, templates, stubs.untypedTemplateProperties());

	// Start-up scripts, in the original's order.
	const auto run = [&](const char* script, std::vector<std::string> args = {}) {
		std::string output;
		const bool ok = console.run(script, args, output);
		std::printf("run %-24s %s\n", script, ok ? "ok" : output.c_str());
	};
	run("ServerArchives.con");
	if (!options.dedicated) {
		run("ClientArchives.con");
	}
	files.setMountingAllowed(false);
	std::printf("%zu archives mounted\n", files.mountedArchiveCount());
	if (!options.dedicated) {
		run("Settings/Sound.con");
		run("Menu/MenuSounds.con");
	}
	run("Init.con");
	run("Settings/RankSettings.con");
	run("Menu/HUD/HudSetup/HudSetupMain.con");
	if (!options.dedicated) {
		// The player profile's options (the crosshair colour among them).
		hudManager.set("CrossHairColorAlpha", 1);
		run("Settings/Profiles/Default/GeneralOptions.con");
		if (controls.maps().empty()) {
			// The keys as the controls page left them, else the game's.
			std::ifstream saved(controlsFile());
			if (saved) {
				std::string output;
				for (std::string line; std::getline(saved, line);) {
					console.execute(line, output, ConsoleAccess::Anyone);
				}
				std::printf("controls from %s\n", controlsFile().c_str());
			} else {
				run("Settings/Controls.con");
			}
		}
	}
	sounds.reloadSoundFiles();

	// The Flash menu (SwiffPlayer): the movie from Menu_server.zip, its
	// images from Menu_client.zip, its text through the localization.
	ui::Player& menu = session.menu;
	bool& menuQuit = session.menuQuit;
	std::optional<dice::hfe::menu::GameSetup>& startedGame = session.startedGame;
	// Over the game: from the start with --menu and +level, else on Esc.
	bool showMenu = !startedGame;
	if (!options.menu.empty()) {
		menu.setFileReader([&files, &options](const std::string& path, std::vector<uint8_t>& out) {
			// "/Levels/..." (map pictures) from the game's root, the rest
			// beside the movie.
			const bool found = path.rfind('/', 0) == 0 ? files.readFile(path.substr(1), out)
													  : files.readFile("Menu/External/FlashMenu/" + path, out);
			if (options.verbose || !found) {
				std::printf("menu reads %s: %s\n", path.c_str(), found ? "ok" : "missing");
			}
			return found;
		});
		menu.natives().setLocalizer([&localization](const std::string& key) { return localization.text(key); });
		bindMenuSounds(menu, sounds);
	}
	// Voice chat's settings from the audio page (AudioSettings.Voip*; unset
	// ones keep their value).
	const auto applyVoiceSettings = [&session] {
		if (!session.voice || !session.menuBindings) {
			return;
		}
		const dice::hfe::menu::MenuBindings& settings = *session.menuBindings;
		dice::hfe::voip::VoiceSettings voice = session.voice->settings();
		voice.enabled = settings.setting("AudioSettings.VoipEnabled", voice.enabled) != 0;
		voice.playbackVolume = static_cast<float>(settings.setting("AudioSettings.VoipPlaybackVolume", voice.playbackVolume));
		voice.captureVolume = static_cast<float>(settings.setting("AudioSettings.VoipCaptureVolume", voice.captureVolume));
		voice.captureThreshold = static_cast<float>(settings.setting("AudioSettings.VoipCaptureThreshold", voice.captureThreshold));
		voice.boost = settings.setting("AudioSettings.VoipBoostEnabled", voice.boost) != 0;
		voice.pushToTalk = settings.setting("AudioSettings.VoipUsePushToTalk", voice.pushToTalk) != 0;
		session.voice->setSettings(voice);
	};
	// The options pages' applied values, now and whenever applied again:
	// the volumes and the window's size. AudioSettings' apply (0x00496830)
	// sets the master volume to 1, then the sound manager's groups (slot
	// 45, 0x007f9a80, clamps to 0..1): effectsVolume (+0xf8, console
	// property 0x00411bf4) to groups 0, 4 (first-person weapon sounds,
	// 0x005db760) and 3 (other menu sounds); musicVolume (+0xfc,
	// 0x00411cfb) to group 1 (the music and interface menu sounds,
	// 0x007fb2f0); helpVoiceVolume (+0x100, the page's VoiceOverVolume) to
	// group 2 (Sound/VoiceMessages.con's voice messages, 0x004d8f00). The
	// VOIP values go to Bf2VoipClient directly, not through groups.
	const auto applySettings = [&] {
		if (!session.menuBindings) {
			return;
		}
		applyVoiceSettings();
		const dice::hfe::menu::MenuBindings& settings = *session.menuBindings;
		if (!controls.maps().empty() && !controlsFile().empty()) {
			std::ofstream(controlsFile()) << controls.script();
		}
		const auto effects = static_cast<float>(settings.setting("AudioSettings.EffectsVolume", 1));
		sounds.setMasterVolume(1);
		for (const int group : {0, 4, 3}) {
			sounds.setVolumeForGroup(group, effects);
		}
		sounds.setVolumeForGroup(1, static_cast<float>(settings.setting("AudioSettings.MusicVolume", 1)));
		sounds.setVolumeForGroup(2, static_cast<float>(settings.setting("AudioSettings.VoiceOverVolume", 1)));
#if BF2142_HAS_RENDERER
		int width = 0, height = 0;
		const std::string resolution = settings.text("VideoSettings.Resolution");
		if (session.window && std::sscanf(resolution.c_str(), "%dx%d", &width, &height) == 2 && width > 0 && height > 0) {
			session.window->setSize(width, height);
		}
#endif
	};
	if (session.menuBindings) {
		session.menuBindings->setControls(&controls);
	}
	if (session.menuBindings) {
		session.menuBindings->onSettingsApplied = applySettings;
		applySettings();
	}
	if (!options.menu.empty() && !session.menuBindings) {
		std::vector<uint8_t> dfm;
		std::string error;
		const std::string movie = "Menu/External/FlashMenu/" + options.menu + ".dfm";
		// What the menu calls in the game (MenuBindings.h).
		dice::hfe::menu::MenuServices services = dice::hfe::menu::MenuServices::openSpy();
		if (options.offline) {
			services.checkOnline = [] { return false; };
			services.loginAccount = [](const std::string&, const std::string&) {
				dice::hfe::menu::AccountReply reply;
				reply.reached = false;
				return reply;
			};
			services.fetchServers = [](std::string* error) {
				*error = "offline";
				return std::optional<net::ServerList>();
			};
		}
		session.menuBindings = std::make_unique<dice::hfe::menu::MenuBindings>(menu.natives(), services);
		dice::hfe::menu::MenuBindings* menuBindings = session.menuBindings.get();
		// The voice natives (dice.bf2.Sound.voip*) in place of MenuBindings' stand-ins.
		if (!session.voice) {
			session.voice = std::make_unique<dice::hfe::voip::VoiceChat>();
			session.voice->runInBackground(true);
		}
		dice::hfe::voip::bindVoipNatives(menu.natives(), *session.voice);
		applyVoiceSettings();
		menuBindings->setProfileFile(dice::hfe::menu::MenuBindings::defaultProfileFile());
		menuBindings->setStatsFile(dice::hfe::menu::MenuBindings::defaultStatsFile());
		menuBindings->onQuit = [&session]() { session.menuQuit = true; };
		menuBindings->onLog = [](const std::string& line) { std::printf("menu: %s\n", line.c_str()); };
		menuBindings->setMaps(scanMenuMaps(options));
		menuBindings->onStartGame = [&session](const dice::hfe::menu::GameSetup& setup) { session.startedGame = setup; };
		menuBindings->onDisconnect = [&session] { session.disconnect = true; };
		menuBindings->onJoin = [&session, &options](const std::string& host, uint16_t port) {
			options.connect = host + ":" + std::to_string(port);
			session.joinRequested = true;
		};
		if (!files.readFile(movie, dfm) || !menu.loadDfm(dfm, &error)) {
			std::fprintf(stderr, "cannot load %s: %s\n", movie.c_str(), error.c_str());
		} else {
			std::printf("menu %s: %.0f fps, %.0f x %.0f\n", options.menu.c_str(), menu.frameRate(), menu.stageWidth(),
				menu.stageHeight());
			if (!options.screenshot.empty()) {
				for (float t = 0; t < options.menuSeconds; t += 0.5f) {
					menu.advance(0.5);
				}
				for (const std::string& error : menu.scriptErrors()) {
					std::printf("  script error: %s\n", error.c_str());
				}
				if (options.verbose) {
					const ui::MenuFrame frame = menu.frame();
					for (const auto& batch : frame.batches) {
						if (batch.image) {
							const ui::MenuVertex& v = batch.vertices.front();
							std::printf("  image %ux%u, %zu vertices, repeat %d, first at %.0f,%.0f mult %.2f %.2f %.2f %.2f add %.2f %.2f %.2f %.2f\n",
								batch.image->width, batch.image->height, batch.vertices.size(), batch.repeat, v.x, v.y, v.mult[0], v.mult[1],
								v.mult[2], v.mult[3], v.add[0], v.add[1], v.add[2], v.add[3]);
						}
					}
				}
			}
		}
	}
	// The menu before any level: it runs alone until it quits or starts a
	// game (Multiplay.createServer), which then loads like +level.
	if (menu.loaded() && options.level.empty() && (!options.menuScript.empty() || (options.screenshot.empty() && options.view))) {
		const auto over = [&] { return menuQuit || startedGame.has_value() || session.joinRequested; };
		if (!options.menuScript.empty()) {
			runMenuScript(menu, options.menuScript, over);
#if BF2142_HAS_RENDERER
			if (!options.screenshot.empty() && !over()) {
				// The menu where the script left it, rendered off screen.
				render::LevelSources sources {files, templates, objects, geometries, heightmaps, nullptr, lights, sky, water, "", ""};
				sources.menu = [&menu](float) { return menu.frame(); };
				ViewerOptions viewer;
				viewer.screenshot = options.screenshot;
				return runViewer(sources, viewer);
			}
#endif
		} else {
#if BF2142_HAS_RENDERER
			render::LevelSources sources {files, templates, objects, geometries, heightmaps, nullptr, lights, sky, water, "", ""};
			sources.menu = [&menu](float seconds) {
				menu.advance(seconds);
				return menu.frame();
			};
			ViewerOptions viewer;
			viewer.onMenuInput = menuInput(menu, [&session](int key) {
				session.menuBindings->setLastInput(controlForFlashKey(key));
			});
			viewer.wantsQuit = over;
			viewer.onTicks = [&sounds](int ticks) { sounds.update(ticks); };
			if (!session.window) {
				session.window = Window::open(viewer.width, viewer.height);
				if (session.window) {
					session.window->setScript(options.inputScript);
				}
			}
			if (!session.window) {
				return 1;
			}
			session.menuBindings->setDisplayModes(session.window->displayModes());
			applySettings();
			runViewer(sources, viewer, session.window.get());
			if (session.window->closed()) {
				return 0;
			}
#endif
		}
		if (session.joinRequested) {
			// The level the browser listed for the server (the connection
			// below may name it otherwise).
			const dice::hfe::menu::MenuMap& map = session.menuBindings->joinedMap();
			options.level = map.level;
			if (!map.gameMode.empty()) {
				options.gameMode = map.gameMode;
			}
			if (map.size > 0) {
				options.mapSize = map.size;
			}
		} else if (!startedGame) {
			std::printf("the menu started no game\n");
			return 0;
		}
	}
	// A game the menu has just started (later maps come from the playlist).
	if (startedGame && options.level.empty() && !session.joinRequested) {
		session.playlist = startedGame->maps;
		session.current = 0;
		session.roundsPerMap = startedGame->roundsPerMap;
		const dice::hfe::menu::MenuMap& map = startedGame->maps.front();
		options.level = map.level;
		options.gameMode = map.gameMode;
		options.mapSize = map.size;
		options.maxPlayers = startedGame->maxPlayers;
		// The bots fill the server up to their number, the player included.
		options.bots = std::max(0, startedGame->bots - (startedGame->botsIncludeHumans ? 1 : 0));
		options.botSkill = startedGame->botSkill;
		options.ticketRatio = startedGame->ticketRatio;
		options.timeLimit = startedGame->timeLimit;
		options.scoreLimit = startedGame->scoreLimit;
		options.playTeam = options.playTeam ? options.playTeam : 2;
		showMenu = false;
		gameLogic.mapSize = options.mapSize;
		std::printf("starting %s (%s, %d) with %d bots\n", options.level.c_str(), options.gameMode.c_str(), options.mapSize,
			options.bots);
	}
	// A game on a server: connect, answer the challenge, and wait for the
	// server's level block (data block 2); that level loads here as for a
	// local game, and the server's objects come in as ghosts.
	if (!options.connect.empty() && !session.client) {
		const size_t colon = options.connect.rfind(':');
		const std::string host = options.connect.substr(0, colon);
		const uint16_t port = colon == std::string::npos ? net::kGamePort
														  : static_cast<uint16_t>(std::atoi(options.connect.c_str() + colon + 1));
		session.client = std::make_unique<net::GameClient>();
		if (!session.voice) {
			// Voice chat for a game joined without the menu.
			session.voice = std::make_unique<dice::hfe::voip::VoiceChat>();
			session.voice->runInBackground(true);
		}
		net::GameClientOptions clientOptions;
		clientOptions.profileId = options.profileId;
		net::GameClient& client = *session.client;
		client.connect(host, port, clientOptions);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		// Until the level block (data block 2: the level to load; a map
		// list without one is a fallback).
		while (std::chrono::steady_clock::now() < deadline && !client.world().hasLevel()) {
			if (!client.update() && client.state() != net::GameClient::State::Connecting &&
				client.state() != net::GameClient::State::Connected) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		const auto maps = client.world().mapList();
		if (client.state() == net::GameClient::State::Refused) {
			std::printf("%s:%u refused: %s\n", host.c_str(), port, net::refusalReason(client.connectResult().reason));
		}
		if (client.state() != net::GameClient::State::Connected ||
			(maps.empty() && options.level.empty() && !client.world().hasLevel())) {
			std::printf("%s:%u: no game (%s)\n", host.c_str(), port,
				client.state() == net::GameClient::State::Connected ? "no map list" : "not connected");
			session.client.reset();
			if (session.joinRequested) {
				session.joinRequested = false;
				session.disconnect = true; // back to the menu
				options.connect.clear();
				return 0;
			}
			return 1;
		}
		std::printf("%s:%u: %s, %zu maps\n", host.c_str(), port, client.world().serverName().c_str(), maps.size());
		// The server's level: its block, else the browser's map, else the
		// map list's current entry.
		std::string wanted = options.level;
		if (client.world().hasLevel()) {
			const auto& level = client.world().level();
			std::string directory = level.levelDirectory;
			while (!directory.empty() && (directory.back() == '/' || directory.back() == '\\')) {
				directory.pop_back();
			}
			wanted = directory.substr(directory.find_last_of("/\\") + 1);
			if (!level.gameMode.empty()) {
				options.gameMode = level.gameMode;
			}
		}
		const int index = client.world().currentMapIndex();
		const size_t current = index < 0 || maps.empty() ? 0 : std::min<size_t>(static_cast<size_t>(index), maps.size() - 1);
		if (!client.world().hasLevel() && wanted.empty() && !maps.empty()) {
			wanted = maps[current].name;
			options.gameMode = maps[current].gameMode;
		}
		if (!maps.empty()) {
			if (maps[current].size > 0) {
				options.mapSize = static_cast<int>(maps[current].size);
			}
		}
		if (!wanted.empty()) {
			options.level.clear();
			// The level's folder as installed (the server names it in lower case).
			for (const dice::hfe::menu::MenuMap& installed : scanMenuMaps(options)) {
				const std::string& a = installed.level;
				const std::string& b = wanted;
				if (a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
						return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
					})) {
					options.level = installed.level;
				}
			}
			if (options.level.empty()) {
				options.level = wanted;
			}
		}
		options.bots = 0;     // the server's players
		options.playTeam = 0; // our soldier is the server's
		gameLogic.mapSize = options.mapSize;
	}
	if (options.bots > 0) {
		run("AI/AIBehaviours.ai");
		run("AI/AIDefault.ai");
		if (options.botSkill >= 0) {
			botSettings.skill = options.botSkill;
		}
		std::printf("bots: skill %.2f, Fire weight %.1f\n", botSettings.skill, botSettings.modifier("Fire"));
	}
	templates.setLoading(true);
	// The loading screen (endOfRound.dfm while a level loads) when the
	// window is up already: from the menu, or between maps.
	ui::Player loadingMovie;
	dice::hfe::menu::EndOfRoundBindings loadingBindings(loadingMovie.natives());
#if BF2142_HAS_RENDERER
	std::optional<render::LevelSources> loadingSources;
	std::unique_ptr<Window::MenuScreen> loadingScreen;
	if (session.window && !options.level.empty()) {
		loadingMovie.setFileReader(endOfRoundReader(files));
		loadingMovie.natives().setLocalizer([&localization](const std::string& key) { return localization.text(key); });
		dice::hfe::menu::RoundSummary summary;
		summary.loading = true;
		summary.firstLoad = session.current == 0 && session.roundsOnMap == 0;
		summary.mapPath = options.level;
		summary.gameMode = options.gameMode;
		summary.mapSize = options.mapSize;
		summary.maxPlayers = options.maxPlayers;
		summary.roundsLeft = 999; // a local game's (0x004d3450)
		std::string desc;
		if (files.readText("Levels/" + options.level + "/Info/" + options.level + ".desc", desc)) {
			const dice::hfe::menu::LevelDescription level =
				dice::hfe::menu::parseLevelDescription(desc, options.gameMode, options.mapSize);
			summary.mapName = level.name;
			summary.mapDescriptionKey = level.briefingKey;
			summary.gameModeDescriptionKey = level.gameModeKey;
		}
		loadingBindings.setSummary(summary);
		std::vector<uint8_t> dfm;
		if (files.readFile("Menu/External/FlashMenu/endOfRound.dfm", dfm) && loadingMovie.loadDfm(dfm)) {
			loadingSources.emplace(render::LevelSources {files, templates, objects, geometries, heightmaps, nullptr, lights, sky,
				water, "", ""});
			loadingSources->menu = [&loadingMovie](float) {
				loadingMovie.advance(1.0f / 30.0f);
				return loadingMovie.frame();
			};
			loadingScreen = std::make_unique<Window::MenuScreen>(*session.window, *loadingSources);
		}
	}
#endif
	// Shows how far loading got (a few frames of the loading screen).
	const auto loadingStep = [&](float progress) {
		loadingBindings.setLoading(progress);
#if BF2142_HAS_RENDERER
		for (int i = 0; loadingScreen && i < 3; ++i) {
			loadingScreen->frame();
		}
#endif
	};
	loadingStep(0);
	// Level loading, after FUN_004d3450: the level's archives are mounted at
	// its directory, then materials, static objects and the game mode's
	// gameplay objects are created.
	if (!options.level.empty()) {
		const std::string levelPath = "Levels/" + options.level + "/";
		files.setMountingAllowed(true);
		files.mountArchive(levelPath + "server.zip", levelPath);
		if (!options.dedicated) {
			files.mountArchive(levelPath + "client.zip", levelPath);
		}
		files.setMountingAllowed(false);
		for (const std::string& script : {levelPath + "ServerArchives.con", levelPath + "ClientArchives.con"}) {
			if (files.exists(script)) {
				run(script.c_str());
			}
		}
		loadingStep(0.1f);
		std::printf("%zu template scripts run from Objects/\n",
			world::loadObjectTemplates(console, files, "Objects/", options.bots > 0));
		loadingStep(0.5f);
		run("Common/Material/MaterialManagerSettings.con");
		run((levelPath + "Init.con").c_str());
		loadingStep(0.6f);
		run((levelPath + "StaticObjects.con").c_str());
		loadingStep(0.8f);
		// v_arg1 is "host" on the server (a local game hosts) and "client"
		// otherwise; spawners and control points only exist on the host
		// (on a server's game they come as its ghosts).
		run((levelPath + "GameModes/" + options.gameMode + "/" + std::to_string(options.mapSize) +
				"/GamePlayObjects.con").c_str(),
			{session.client ? "client" : "host"});
		if (options.bots > 0) {
			// The level's AI settings (ai/AI.ai: ai.init and the pathfinding
			// maps' slope and step limits), then the bots' strategies and the
			// level's strategic areas.
			if (files.exists(levelPath + "ai/AI.ai")) {
				run((levelPath + "ai/AI.ai").c_str());
			}
			run("AI/AIDefaultStrategies.ai");
			run("AI/AIDefaultStrategiesAdd.ai");
			const std::string areas = levelPath + "GameModes/" + options.gameMode + "/" + std::to_string(options.mapSize) +
				"/ai/StrategicAreas.ai";
			if (files.exists(areas)) {
				run(areas.c_str());
			}
		}
	}

	templates.setLoading(false);
	sounds.reloadSoundFiles();
	loadingStep(0.9f);
#if BF2142_HAS_RENDERER
	loadingScreen.reset(); // the level's renderer takes the window
#endif

	if (!options.playSound.empty() && soundEngine) {
		// Plays for two seconds and reports what the engine did.
		sounds.playSound(options.playSound);
		int maxPlaying = 0;
		for (int tick = 0; tick < 60; ++tick) {
			sounds.update(1);
			maxPlaying = std::max(maxPlaying, soundEngine->statistics()->playingSamples);
			std::this_thread::sleep_for(std::chrono::milliseconds(33));
		}
		const sound::SoundStatistics* stats = soundEngine->statistics();
		std::printf("sound: played \"%s\": at most %d samples playing, %d files loaded (%s)\n",
			options.playSound.c_str(), maxPlaying, stats->loadedFiles,
			stats->providerDescription ? stats->providerDescription : "?");
	}
	std::printf("%zu object templates, %zu objects, %zu geometry templates\n", templates.count(),
		objects.objects().size(), geometries.count());

	stubs.report(25);
	std::vector<std::pair<std::string, int>> sorted(errors.begin(), errors.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
	if (!options.dumpTemplate.empty()) {
		std::function<void(const world::ObjectTemplate&, const std::string&, int)> dump;
		dump = [&](const world::ObjectTemplate& t, const std::string& indent, int depth) {
			std::printf("%s%s %s (id %u)\n", indent.c_str(), t.typeName().c_str(), t.name().c_str(), t.id());
			for (const auto& [key, value] : t.settings()) {
				std::printf("%s  .%s = %s\n", indent.c_str(), key.c_str(), value.c_str());
			}
			for (const world::ChildTemplate& child : t.children()) {
				std::printf("%s  @ %g/%g/%g rot %g/%g/%g\n", indent.c_str(), child.position.x, child.position.y,
					child.position.z, child.rotation.x, child.rotation.y, child.rotation.z);
				if (const world::ObjectTemplate* c = templates.find(child.name); c && depth < 6) {
					dump(*c, indent + "    ", depth + 1);
				} else {
					std::printf("%s    (%s)\n", indent.c_str(), child.name.c_str());
				}
			}
		};
		if (const world::ObjectTemplate* t = templates.find(options.dumpTemplate)) {
			dump(*t, "", 0);
		}
		return 0;
	}
	std::printf("%zu materials, %zu material cells, %zu collision mesh templates\n", materials.materialCount(),
		materials.cellCount(), collisionTemplates.count());
	std::printf("%zu kinds of script errors:\n", sorted.size());
	for (size_t i = 0; i < sorted.size() && i < 15; ++i) {
		std::printf("  %7d %s\n", sorted[i].second, sorted[i].first.c_str());
	}

	if (options.view || !options.screenshot.empty() || options.simulate > 0) {
		// Start the round: spawners take their control points' teams and
		// put their vehicles out on the first tick.
		game::GameWorld round(templates, objects);
		// The bots see as far as the level's fog lets them (AIDLL 0x10056b20).
		botSettings.levelFog = lights.fogStartEndAndBase;
		game::BotManager bots(round, botSettings);
		round.unlockAllByDefault = options.unlockAll;
		if (heightmaps.hasSea()) {
			round.setWaterLevel(heightmaps.seaWaterLevel());
		}
		// The spawn screen's kit buttons: the kit (by the team's kit index,
		// gameLogic.setKit) of the local player's next spawn.
		int localKit = 1;
		bindMethod(console, "spawnManager.setPlayerKit", ConsoleAccess::Anyone, [&](int index) {
			const game::Team& team = gameLogic.team(round.playerTeam() == 1 ? 1 : 2);
			if (index >= 0 && static_cast<size_t>(index) < team.kits.size() && round.localPlayer() >= 0) {
				localKit = index;
				round.setKit(round.localPlayer(), team.kits[static_cast<size_t>(index)].name,
					team.kits[static_cast<size_t>(index)].soldier);
			}
		});
		// The commander (Commander.con's buttons and rose): applying and
		// resigning, a tool or an order armed by its button and used where
		// the map is clicked next, the satellite scan at once.
		using Ability = game::GameWorld::CommanderAbility;
		bool commanderScreen = false;
		bool commanderKeyWas = false; // Caps Lock as of the last input
		std::optional<Ability> armedTool;
		std::optional<game::GameWorld::OrderType> armedOrder;
		const auto myTeam = [&round] { return round.playerTeam() == 1 ? 1 : 2; };
		bindMethod(console, "Commander.commanderApply", ConsoleAccess::Anyone, [&] { round.applyForCommander(round.localPlayer()); });
		bindMethod(console, "Commander.commanderResign", ConsoleAccess::Anyone, [&] { round.resignCommander(round.localPlayer()); });
		const auto arm = [&](Ability tool) {
			armedTool = armedTool == tool ? std::nullopt : std::optional<Ability>(tool);
			armedOrder.reset();
		};
		for (const auto& [command, tool] : std::vector<std::pair<std::string, Ability>> {
				 {"Commander.toggleOrbitalStrikeState", Ability::OrbitalStrike}, {"Commander.orbitalStrike", Ability::OrbitalStrike},
				 {"Commander.toggleArtilleryState", Ability::Artillery}, {"Commander.artillery", Ability::Artillery},
				 {"Commander.toggleUAVState", Ability::Uav}, {"Commander.uav", Ability::Uav},
				 {"Commander.toggleSupplyState", Ability::Supplies}, {"Commander.supplies", Ability::Supplies}}) {
			const Ability armed = tool;
			bindMethod(console, command, ConsoleAccess::Anyone, [&arm, armed] { arm(armed); });
		}
		bindMethod(console, "Commander.satellite", ConsoleAccess::Anyone,
			[&] { round.commanderAction(myTeam(), Ability::Satellite, 0, 0); });
		// sendOrder <kind> <add>: the order kind (0..3 move, attack, defend,
		// repair from the issue-order menu, 4..7 the same placed on the map,
		// 10 or more drops it), sent where the map is clicked next. Both the
		// squad leader's and the commander's run the same handler
		// (0x00770889 → 0x007d6ab0). The commander's goes to the squad whose
		// leader is nearest the click (the screen's squad selection is not
		// drawn); the squad leader's to his own squad.
		bool leaderOrder = false;
		const auto armOrder = [&](int kind, bool leader) {
			const game::GameWorld::OrderType type = game::GameWorld::orderTypeFromKind(kind);
			armedOrder = type == game::GameWorld::OrderType::None ? std::nullopt : std::optional(type);
			leaderOrder = leader;
			armedTool.reset();
		};
		bindMethod(console, "Commander.sendOrder", ConsoleAccess::Anyone, [&armOrder](int kind, int) { armOrder(kind, false); });
		bindMethod(console, "SquadLeader.sendOrder", ConsoleAccess::Anyone, [&armOrder](int kind, int) { armOrder(kind, true); });
		bindMethod(console, "Commander.issueOrder", ConsoleAccess::Anyone, [&armOrder] { armOrder(4, false); });
		// The offer of command after applying (0x004c8c20 accepts it).
		bindMethod(console, "Commander.accept", ConsoleAccess::Anyone,
			[&](int accept) { round.answerCommanderOffer(round.localPlayer(), accept != 0); });
		bindMethod(console, "Commander.commRoseAccept", ConsoleAccess::Anyone,
			[&](int accept) { round.acceptOrder(round.localPlayer(), accept != 0); });
		// A click on the map with a tool or order armed: used there.
		const auto commanderClick = [&](float x, float y) {
			float worldX = 0, worldZ = 0;
			if ((!armedTool && !armedOrder) || !hudManager.mapWorldAt(x, y, worldX, worldZ)) {
				return false;
			}
			if (armedTool) {
				round.commanderAction(myTeam(), *armedTool, worldX, worldZ);
			} else {
				if (leaderOrder) {
					round.issueSquadLeaderOrder(round.localPlayer(), *armedOrder, worldX, worldZ);
				} else {
					int squad = 0;
					float best = 1e30f;
					for (const auto& player : round.playerList()) {
						const float dx = player.position.x - worldX, dz = player.position.z - worldZ;
						if (player.team == myTeam() && player.squadLeader && player.squad && dx * dx + dz * dz < best) {
							best = dx * dx + dz * dz;
							squad = player.squad;
						}
					}
					if (squad) {
						round.issueOrder(myTeam(), squad, *armedOrder, worldX, worldZ, round.localPlayer());
					}
				}
			}
			armedTool.reset();
			armedOrder.reset();
			return true;
		};
		// The bots' navigation mesh (coop and single-player levels have one).
		std::optional<ai::NavMesh> navMesh, vehicleNavMesh;
		if (options.bots > 0) {
			std::vector<uint8_t> qtr, clb;
			const std::string base = "Levels/" + options.level + "/AIPathFinding/Infantry";
			if (files.readFile(base + ".qtr", qtr) && files.readFile(base + ".clb", clb)) {
				std::string error;
				navMesh = ai::parseNavMesh(qtr, clb, &error);
				std::printf("navigation mesh: %s\n", navMesh ? "loaded" : error.c_str());
			} else {
				std::printf("navigation mesh: none for this level\n");
			}
			bots.setNavMesh(navMesh ? &*navMesh : nullptr);
			const std::string vehicleBase = "Levels/" + options.level + "/AIPathFinding/Vehicle";
			if (files.readFile(vehicleBase + ".qtr", qtr) && files.readFile(vehicleBase + ".clb", clb)) {
				vehicleNavMesh = ai::parseNavMesh(qtr, clb);
			}
			bots.setVehicleNavMesh(vehicleNavMesh ? &*vehicleNavMesh : nullptr);
		}
		// Animation systems are built by their scripts when a soldier or
		// weapon first needs one, after the global value holders.
		bool valueHoldersLoaded = false;
		round.setAnimationSystems(&animationSystems, [&](const std::string& path) {
			std::string output;
			if (!valueHoldersLoaded) {
				valueHoldersLoaded = true;
				console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, output);
			}
			return animationSystems.get(path, [&] { console.run(path, {}, output); });
		});
		// The HUD's map: the level's ingameMap.dds over the primary terrain.
		if (const terrain::Heightmap* ground = heightmaps.primary(); ground && ground->loaded()) {
			hudManager.setMap("Levels/" + options.level + "/Hud/Minimap/ingameMap.dds", ground->originX(), ground->originZ(),
				ground->worldWidth(), ground->worldDepth());
		}
		if (const terrain::Heightmap* ground = heightmaps.primary(); ground && ground->loaded()) {
			physics::Ground surface;
			surface.height = [ground](float x, float z) { return ground->heightAt(x, z); };
			// The terrain's material under a point (heightmap.loadMaterialData).
			surface.material = [&heightmaps](float x, float z) { return heightmaps.materialAt(x, z); };
			surface.heightAndNormal = [ground](float x, float z, physics::Vec3& normal) {
				const float step = ground->scale()[0];
				const float dx = ground->heightAt(x + step, z) - ground->heightAt(x - step, z);
				const float dz = ground->heightAt(x, z + step) - ground->heightAt(x, z - step);
				const float length = std::sqrt(dx * dx + 4 * step * step + dz * dz);
				normal = {-dx / length, 2 * step / length, -dz / length};
				return ground->heightAt(x, z);
			};
			round.enablePhysics(files, collisionTemplates, std::move(surface), &materials);
		}
		round.startRound();
		gameLogic.startRound();
		std::function<int(int)> scoreOf; // the scripts' scores, by player
		bool scoreboardHeld = options.scoreboard;
		// The end-of-round movie (endOfRound.dfm, its own player: its lists
		// are not the main menu's), shown once the round is over until the
		// next map.
		ui::Player endOfRound;
		dice::hfe::menu::EndOfRoundBindings endOfRoundBindings(endOfRound.natives());
		bool endOfRoundShown = false;
		endOfRound.setFileReader(endOfRoundReader(files));
		endOfRound.natives().setLocalizer([&localization](const std::string& key) { return localization.text(key); });
		bindMenuSounds(endOfRound, sounds);
		// On a server: its ghosts become objects of the level (by their
		// templates), moved as their updates come; our input goes up.
		if (session.client) {
			// This level's templates name the server's ghosts and lay out
			// their state groups (GhostLayout.h).
			session.client->world().setTemplateResolver([&templates](uint32_t id) {
				const world::ObjectTemplate* t = templates.findById(id);
				return t ? t->name() : std::string();
			});
			session.client->world().setGhostTemplates([&templates](uint32_t id) {
				const world::ObjectTemplate* t = templates.findById(id);
				return t ? net::ghostTemplateFrom(templates, *t) : net::GhostTemplateNode {};
			});
		}
		std::map<uint16_t, uint32_t> ghostObjects; // ghost -> object
		std::map<uint16_t, uint32_t> ragdollUpdates; // ghost -> the ragdoll update last applied
		struct TimedCommand {
			float at;
			std::string command;
		};
		std::vector<TimedCommand> timedConsole;
		const auto runTimedConsole = [&] {
			for (auto it = timedConsole.begin(); it != timedConsole.end();) {
				if (gameLogic.elapsed < it->at) {
					++it;
					continue;
				}
				std::string output;
				console.execute(it->command, output, ConsoleAccess::Anyone);
				std::printf("console at %.0f s: %s%s%s\n", gameLogic.elapsed, it->command.c_str(), output.empty() ? "" : " -> ",
					output.c_str());
				it = timedConsole.erase(it);
			}
		};
		const auto remoteStep = [&] {
			runTimedConsole();
			if (!session.client) {
				return;
			}
			net::GameClient& client = *session.client;
			client.update();
			std::set<uint16_t> seen;
			for (const net::RemoteObject& ghost : client.world().objectList()) {
				seen.insert(ghost.ghost);
				if (ghost.part > 0 || !ghost.hasPosition) {
					continue; // a seat of a vehicle ghost (drawn with its vehicle), or not placed yet
				}
				const world::Vec3 at {ghost.position.x, ghost.position.y, ghost.position.z};
				const world::Vec3 turned {ghost.rotation.x, ghost.rotation.y, ghost.rotation.z};
				if (const auto it = ghostObjects.find(ghost.ghost); it != ghostObjects.end()) {
					// A dead soldier's body follows the server's networked
					// ragdoll particles; GameWorld places it meanwhile.
					if (ghost.ragdoll) {
						if (ragdollUpdates[ghost.ghost] != ghost.ragdollUpdates) {
							ragdollUpdates[ghost.ghost] = ghost.ragdollUpdates;
							std::vector<physics::Vec3> particles;
							for (const net::Vec3f& p : ghost.ragdollParticles) {
								particles.push_back({p.x, p.y, p.z});
							}
							round.driveRemoteRagDoll(it->second, particles);
						}
					} else if (round.remoteRagDoll(it->second)) {
						round.removeRemoteRagDoll(it->second); // revived
						ragdollUpdates.erase(ghost.ghost);
					}
					if (world::Object* object = round.remoteRagDoll(it->second) ? nullptr : objects.find(it->second)) {
						object->setPosition(at);
						object->setRotation(turned);
					}
				} else if (world::ObjectTemplate* t = ghost.templateName.empty() ? templates.findById(ghost.templateId)
																			   : templates.find(ghost.templateName)) {
					if (world::Object* object = objects.create(t, at, turned)) {
						object->setTeam(ghost.team);
						ghostObjects[ghost.ghost] = object->id();
						if (options.verbose) {
							std::printf("ghost %u: %s\n", ghost.ghost, t->name().c_str());
						}
					}
				}
			}
			for (auto it = ghostObjects.begin(); it != ghostObjects.end();) {
				if (!seen.count(it->first)) {
					round.removeRemoteRagDoll(it->second);
					ragdollUpdates.erase(it->first);
					objects.remove(it->second);
					it = ghostObjects.erase(it);
				} else {
					++it;
				}
			}
			for (const net::ChatLine& line : client.world().takeChat()) {
				const net::RemotePlayer* from = client.world().player(line.player);
				std::printf("chat %s: %s\n", from ? from->name.c_str() : "server", line.text.c_str());
			}
			// Voice chat: the server's voice settings (data block 0), its
			// VoipSession and mute events (not used by the world), our squad.
			if (session.voice) {
				if (const auto block = client.world().dataBlocks().find(0); block != client.world().dataBlocks().end()) {
					session.voice->setServerSettingsBlock(block->second, options.connect.substr(0, options.connect.rfind(':')));
				}
				for (const net::GameEvent& event : client.world().takeUnhandled()) {
					session.voice->handleEvent(event, client.world().localPlayer());
				}
				if (const net::RemotePlayer* me = client.world().player(client.world().localPlayer())) {
					session.voice->setRole(me->commander, me->squadLeader, me->squad);
				}
			}
			// A new level block: the server changed maps (the connection stays).
			for (const auto& level : client.world().takeLevelChanges()) {
				if (client.world().levelsReceived() > 1) {
					std::printf("the server loads %s\n", level.mapName.c_str());
					session.levelChange = true;
				}
			}
			if (client.state() != net::GameClient::State::Connected) {
				std::printf("the server ended the connection (reason %u)\n", client.disconnectReason());
				if (session.voice) {
					session.voice->leave();
				}
				session.client.reset();
				session.disconnect = true;
			}
		};
#if BF2142_HAS_PYTHON
		// The game mode's Python script runs the round (python/bf2 and
		// mods/<mod>/python/game/gamemodes/<mode>.py).
		game::ServerSettings serverSettings;
		serverSettings.gameMode = options.gameMode;
		serverSettings.mapName = options.level;
		serverSettings.ticketRatio = options.ticketRatio;
		serverSettings.timeLimit = options.timeLimit;
		serverSettings.scoreLimit = options.scoreLimit;
		serverSettings.maxPlayers = options.maxPlayers;
		game::ScriptBridge bridge(round, gameLogic, serverSettings, options.modPath);
		scoreOf = [&bridge](int player) { return static_cast<int>(bridge.playerScore(player, "score").asInt()); };
		script::ScriptHost scripts(bridge,
			{options.gameDir + "/python", options.gameDir + "/" + options.modPath + "/python"},
			[&options](const std::string& line) {
				if (options.verbose || line.find("Error") != std::string::npos || line.find("Traceback") != std::string::npos
					|| line.compare(0, 7, "stderr:") == 0) {
					std::printf("python: %s\n", line.c_str());
				}
			});
		const auto raise = [&scripts](const std::string& event, const game::ScriptArgs& args) { scripts.raise(event, args); };
		round.setEventSink(raise);
		gameLogic.setEventSink(raise);
		gameLogic.onEndGame = [&](int winner, int victoryType) {
			std::printf("round over: team %d won (victory type %d)\n", winner, victoryType);
			scripts.setGameStatus(game::GameStatus::EndGame);
		};
		if (session.client) {
			std::printf("on %s: the server runs the round\n", session.client->world().serverName().c_str());
		} else if (scripts.ok() && scripts.startGameMode(options.gameMode)) {
			scripts.setGameStatus(game::GameStatus::PreGame);
			scripts.setGameStatus(game::GameStatus::Playing);
			std::printf("game mode %s started: tickets %d / %d\n", options.gameMode.c_str(),
				static_cast<int>(gameLogic.team(1).tickets), static_cast<int>(gameLogic.team(2).tickets));
		} else {
			std::fprintf(stderr, "game mode %s did not start\n", options.gameMode.c_str());
		}
		const auto step = [&] {
			remoteStep();
			bots.update(1.0f / 30.0f);
			round.tick(1.0f / 30.0f);
			gameLogic.tick(1.0f / 30.0f);
			scripts.update();
		};
#else
		const auto step = [&] {
			remoteStep();
			bots.update(1.0f / 30.0f);
			round.tick(1.0f / 30.0f);
			gameLogic.tick(1.0f / 30.0f);
		};
#endif
		// After the round, sv.endDelay (15 s) before a playlist's next map.
		constexpr float kEndDelay = 15;
		float sinceEnd = 0;
		const auto nextMapDue = [&] { return !session.playlist.empty() && sinceEnd >= kEndDelay; };
		const auto showEndOfRound = [&] {
			if (endOfRoundShown || options.dedicated) {
				return;
			}
			endOfRoundShown = true;
			std::vector<uint8_t> dfm;
			std::string error;
			if (!files.readFile("Menu/External/FlashMenu/endOfRound.dfm", dfm) || !endOfRound.loadDfm(dfm, &error)) {
				std::fprintf(stderr, "cannot load endOfRound.dfm: %s\n", error.c_str());
				return;
			}
			dice::hfe::menu::RoundSummary summary = roundSummary(options, round, gameLogic, scoreOf, "BF2142 server");
			if (session.menuBindings) {
				session.menuBindings->recordRound(summary); // the soldier's stats and rank
			}
			endOfRoundBindings.setSummary(summary);
			// JOIN GAME: on to the next map without waiting out sv.endDelay.
			endOfRoundBindings.onReadyToPlay = [&](bool) { sinceEnd = std::max(sinceEnd, kEndDelay); };
			showMenu = false;
		};
		const size_t before = objects.objects().size();
		// For a screenshot, let the spawned vehicles settle for 3 seconds.
		const int ticks = options.screenshot.empty() ? 1 : 90;
		for (int i = 0; i < ticks; ++i) {
			step();
		}
		std::printf("round started: %zu control points, %zu spawners, %zu objects spawned, %zu with physics\n",
			round.controlPoints().size(), round.spawners().size(), objects.objects().size() - before,
			round.physicsBodies());
		if (options.bots > 0 && !bots.enabled()) {
			// As the original: a level without AI data (no AI/AI.ai, no
			// navigation mesh) has no bots.
			std::printf("no bots on this level (it has no AI navigation data)\n");
		}
		for (int i = 0; i < (bots.enabled() ? options.bots : 0); ++i) {
			const int team = i % 2 == 0 ? (options.playTeam == 1 ? 2 : 1) : (options.playTeam == 1 ? 1 : 2);
			const int bot = round.addPlayer("Bot" + std::to_string(i + 1), team, true);
			// Bots take the team's kits in turn.
			const auto& kits = gameLogic.team(team).kits;
			if (!kits.empty()) {
				const game::Kit& kit = kits[static_cast<size_t>(i / 2) % kits.size()];
				round.setKit(bot, kit.name, kit.soldier);
			}
			round.joinSquad(bot, round.openSquad(team));
			if (!round.spawnSoldier(bot)) {
				std::fprintf(stderr, "no spawn point for bot %d (team %d)\n", i + 1, team);
			}
		}
		if (options.playTeam != 0) {
			// The soldier's chosen unlocks (--unlock-all gives everything).
			if (session.menuBindings) {
				round.localPlayerUnlocks = stats::containersFor(stats::unlockContainers(templates), session.menuBindings->soldierUnlocks());
			}
			if (const world::Object* player = round.spawnPlayer(options.playTeam)) {
				round.joinSquad(round.localPlayer(), round.openSquad(options.playTeam));
				std::printf("playing %s at %.1f/%.1f/%.1f\n", player->objectTemplate()->name().c_str(),
					player->position().x, player->position().y, player->position().z);
				if (!options.drive.empty()) {
					// A vehicle 2.5 m ahead of the soldier: enter it and drive.
					world::ObjectTemplate* vehicle = templates.find(options.drive);
					const float yaw = player->rotation().x * 3.14159265f / 180.0f;
					const float x = player->position().x + std::sin(yaw) * 2.5f, z = player->position().z + std::cos(yaw) * 2.5f;
					const terrain::Heightmap* terrainHeights = heightmaps.primary();
					const float y = terrainHeights && terrainHeights->loaded() ? terrainHeights->heightAt(x, z) : player->position().y;
					if (!vehicle || !objects.create(vehicle, {x, y + 1.5f, z}, {player->rotation().x, 0, 0})) {
						std::fprintf(stderr, "cannot spawn %s\n", options.drive.c_str());
					} else {
						// Let it land (briefly: on a slope it would roll out of reach).
						for (int i = 0; i < 15; ++i) {
							step();
						}
						// Stand at its left side (it may have been pushed out of a tight spot).
						if (const world::Object* car = [&]() -> const world::Object* {
								for (const auto& o : objects.objects()) {
									if (o->objectTemplate() == vehicle) {
										return o.get();
									}
								}
								return nullptr;
							}()) {
							const float carYaw = car->rotation().x * 3.14159265f / 180.0f;
							const physics::Vec3 side {car->position().x - std::cos(carYaw) * 1.8f, car->position().y,
								car->position().z + std::sin(carYaw) * 1.8f};
							round.setSoldierPosition(round.playerObject(),
								{side.x, std::max(side.y - 0.5f, terrainHeights ? terrainHeights->heightAt(side.x, side.z) : side.y), side.z});
						}
						game::SoldierInput use;
						use.use = true;
						round.setPlayerInput(use);
						step();
						const world::Object* car = round.playerVehicle();
						std::printf("drive: %s\n", car ? "entered" : "could not enter");
						const auto report = [&](const char* what) {
							const world::Object* v = round.playerVehicle();
							const game::Vehicle* state = round.playerVehicleState();
							if (v && state) {
								std::printf("  %s: at %.1f/%.1f/%.1f yaw %.0f, gear %d, rpm %.2f\n", what, v->position().x,
									v->position().y, v->position().z, v->rotation().x, state->gear(), state->rpm());
							}
						};
						report("parked");
						const auto drive = [&](float throttle, float steer, int ticks) {
							game::SoldierInput input;
							input.throttle = throttle;
							input.strafe = steer;
							for (int i = 0; i < ticks; ++i) {
								round.setPlayerInput(input);
								step();
							}
						};
						drive(1, 0, 90);
						report("after 3 s of throttle");
						drive(-1, 0, 30);
						report("after 1 s braking");
						drive(0.5f, 1, 45);
						report("after 1.5 s turning right");
						// The gunner's seat (F2): turn the gun a quarter round and fire.
						game::SoldierInput gunner;
						gunner.switchSeat = 2;
						round.setPlayerInput(gunner);
						step();
						game::GameWorld::PlayerInfo me;
						round.playerInfo(round.localPlayer(), me);
						const size_t impactsBefore = round.impacts().size();
						for (int i = 0; i < 30; ++i) {
							game::SoldierInput aim;
							aim.turn = i < 15 ? 6.0f : 0.0f;
							aim.aimPitch = 5;
							aim.fire = i >= 15;
							round.setPlayerInput(aim);
							step();
						}
						float yaw = 0, pitch = 0;
						const bool armed = round.gunAim(me.vehicle, me.seat, yaw, pitch);
						std::printf("  seat %d of %zu%s; gun at yaw %.0f pitch %.0f; %d rounds fired, %zu hit something\n", me.seat + 1,
							round.vehicleSeats(me.vehicle).size(), armed ? " (a gunner)" : "", yaw, pitch,
							round.playerRounds(false, false).empty() ? 0 : round.playerRounds(false, false)[0].count,
							round.impacts().size() - impactsBefore);
						// Critical damage: below armor.criticalDamage it burns down, and
						// the soldier aboard dies with it.
						const uint32_t carId = me.vehicle;
						round.setHitPoints(carId, 100);
						int burnt = 0;
						for (; burnt < 30 * 20 && !objects.find(carId)->destroyed(); ++burnt) {
							round.setPlayerInput({});
							step();
						}
						round.playerInfo(round.localPlayer(), me);
						std::printf("  at 100 hit points it burnt out after %.1f s; the gunner is %s\n", burnt / 30.0f,
							me.alive ? "alive" : "dead");
					}
				} else if (!options.screenshot.empty()) {
					// Run forward for two seconds to show the soldier moving.
					game::SoldierInput forward;
					forward.throttle = 1;
					for (int i = 0; i < 60; ++i) {
						round.setPlayerInput(forward);
						step();
					}
					const world::Object* moved = round.player();
					std::printf("after 2 s: %.1f/%.1f/%.1f\n", moved->position().x, moved->position().y,
						moved->position().z);
					const auto printAnimations = [&] {
						std::printf("  animations:");
						for (const std::string& name : round.playingAnimations(moved->id())) {
							std::printf(" %s", name.c_str());
						}
						std::printf("\n");
					};
					printAnimations();
					// And fire a burst slightly down.
					game::SoldierInput shoot;
					shoot.fire = true;
					shoot.aimPitch = -3;
					for (int i = 0; i < 30; ++i) {
						round.setPlayerInput(shoot);
						step();
					}
					std::printf("fired: %d rounds left in the magazine, %zu impacts, %zu effect emitters\n",
						round.playerWeapon() ? round.playerWeapon()->roundsInMag() : -1, round.impacts().size(),
						round.effects().emitters());
					printAnimations();
					if (!round.impacts().empty()) {
						const auto& last = round.impacts().back();
						std::printf("  last impact at %.1f/%.1f/%.1f on object %u (material %u), damage %.1f\n",
							last.point.x, last.point.y, last.point.z, last.object, last.material, last.damage);
					}
				}
			} else {
				std::fprintf(stderr, "no spawn point for team %d\n", options.playTeam);
			}
		}
		// --console "<command>" runs now, "<seconds>:<command>" once the round
		// is that old.
		for (const std::string& command : options.console) {
			const size_t colon = command.find(':');
			if (colon != std::string::npos && colon > 0 &&
				command.find_first_not_of("0123456789.", 0) == colon) {
				timedConsole.push_back({static_cast<float>(std::atof(command.c_str())), command.substr(colon + 1)});
				continue;
			}
			std::string output;
			console.execute(command, output, ConsoleAccess::Anyone);
			std::printf("console: %s%s%s\n", command.c_str(), output.empty() ? "" : " -> ", output.c_str());
		}
		if (options.simulate > 0) {
			for (int i = 0; i < options.simulate * 30; ++i) {
				step();
				if (gameLogic.ended()) {
					sinceEnd += 1.0f / 30.0f;
					showEndOfRound();
					if (endOfRound.loaded()) {
						endOfRound.advance(1.0f / 30.0f);
					}
				}
				if (nextMapDue()) {
					session.nextMap = true;
					std::printf("round over %.0f s ago: the next map\n", sinceEnd);
					break;
				}
			}
			if (endOfRound.loaded()) {
				std::printf("end of round shown: %zu script errors\n", endOfRound.scriptErrors().size());
				for (const std::string& error : endOfRound.scriptErrors()) {
					std::printf("  end of round script error: %s\n", error.c_str());
				}
			}
			std::printf("after %d s: tickets PAC %.0f, EU %.0f; control points:", options.simulate,
				gameLogic.team(1).tickets, gameLogic.team(2).tickets);
			for (const auto& point : round.controlPointList()) {
				std::printf(" %d:%s", point.id, point.team == 1 ? "PAC" : point.team == 2 ? "EU" : "-");
			}
			std::printf("\n");
			for (const auto& player : round.playerList()) {
				const auto fired = round.playerRounds(player.id, false, false);
				const auto hit = round.playerRounds(player.id, true, false);
				std::printf("  %-8s %s %-6s kills %d deaths %d, %d rounds fired, %d hit%s%s\n", player.name.c_str(),
					player.team == 1 ? "PAC" : "EU ", player.alive ? "alive" : "dead", player.kills, player.deaths,
					fired.empty() ? 0 : fired[0].count, hit.empty() ? 0 : hit[0].count, player.ai ? "  " : "",
					player.ai ? bots.behaviour(player.id).c_str() : "");
			}
			if (session.nextMap || (!options.view && options.screenshot.empty())) {
				return 0;
			}
		}
		for (const std::string& spawn : options.spawns) {
			const size_t at = spawn.find('@');
			world::ObjectTemplate* spawned = templates.find(spawn.substr(0, at));
			float x = 0, y = 0, z = 0, yaw = 0;
			if (!spawned || at == std::string::npos
				|| std::sscanf(spawn.c_str() + at + 1, "%f/%f/%f/%f", &x, &y, &z, &yaw) < 3) {
				std::fprintf(stderr, "cannot spawn %s\n", spawn.c_str());
				continue;
			}
			objects.create(spawned, {x, y, z}, {yaw, 0, 0});
		}
#if BF2142_HAS_RENDERER
		render::LevelSources sources {files, templates, objects, geometries, heightmaps,
			terrainData ? &*terrainData : nullptr, lights, sky, water,
			"Levels/" + options.level + "/", options.animation};
		sources.animationFor = [&round](uint32_t object) { return round.animationFor(object); };
		sources.effects = &round.effects();
		sources.overlay = [&](float seconds) {
			for (int kit = 0; kit < 8; ++kit) {
				hudManager.set("PlayerKitIcon" + std::to_string(kit) + "SelectShow", kit == localKit ? 1 : 0);
			}
			updateHud(hudManager, round, gameLogic, scoreboardHeld, scoreOf, commanderScreen);
			return hudManager.frame(seconds);
		};
		// A game started from the menu shows it on Esc (Logic.isGameRunning).
		if (session.menuBindings) {
			session.menuBindings->setGameRunning(startedGame.has_value());
		}
		{
			sources.menu = [&menu, &options, &showMenu, &endOfRound](float seconds) {
				ui::Player* shown = showMenu && menu.loaded() ? &menu : endOfRound.loaded() ? &endOfRound : nullptr;
				if (!shown) {
					return ui::MenuFrame();
				}
				if (options.screenshot.empty()) {
					shown->advance(seconds);
				}
				return shown->frame();
			};
			if (menu.loaded() && options.playTeam == 0) {
				sources.overlay = nullptr; // no soldier: no HUD under the menu
			}
		}
		sources.skeletonPose = [&round](uint32_t object, const anim::Skeleton& skeleton, std::vector<anim::Matrix>& world) {
			return round.skeletonPose(object, skeleton, world);
		};
		sources.partTransforms = [&round](uint32_t object, std::vector<anim::Matrix>& parts) {
			return round.partTransforms(object, parts);
		};
		sources.partPose = [&round](uint32_t object, const std::string& part, world::Vec3& offset, world::Vec3& rotation) {
			return round.partPose(object, part, offset, rotation);
		};
		ViewerOptions viewer;
		viewer.screenshot = options.screenshot;
		viewer.camera = options.camera;
		{
			// Input to the menu while it is up, else to the end-of-round movie.
			const auto toMenu = menuInput(menu, [&session](int key) {
				if (session.menuBindings) {
					session.menuBindings->setLastInput(controlForFlashKey(key));
				}
			});
			const auto toEndOfRound = menuInput(endOfRound);
			viewer.onMenuInput = [toMenu, toEndOfRound, &showMenu, &menu](const ViewerOptions::MenuInput& input) {
				(showMenu && menu.loaded() ? toMenu : toEndOfRound)(input);
			};
			if (menu.loaded() && !startedGame) {
				viewer.menuActive = [&showMenu] { return showMenu; }; // --menu over a level: always up
			} else {
				viewer.menuActive = [&showMenu, &menu, &endOfRound] { return (showMenu && menu.loaded()) || endOfRound.loaded(); };
			}
			if (startedGame) {
				viewer.onEscape = [&showMenu] { showMenu = !showMenu; };
			}
		}
		if (session.menuBindings) {
			// ControlSettings: 0.5 is the game's default sensitivity.
			const dice::hfe::menu::MenuBindings* settings = session.menuBindings.get();
			viewer.mouseScale = [settings] {
				return static_cast<float>(settings->setting("ControlSettings.MouseSensitivity", 0.5) / 0.5);
			};
			viewer.invertMouse = [settings] { return settings->setting("ControlSettings.InvertMouse", 0) != 0; };
		}
		if (!controls.maps().empty()) {
			viewer.controls = &controls;
		}
		// Ragdolls' level of detail goes by the camera (GameWorld::setCamera).
		viewer.onCamera = [&round](const float position[3], const float forward[3]) {
			round.setCamera({position[0], position[1], position[2]}, {forward[0], forward[1], forward[2]});
		};
		viewer.wantsQuit = [&] {
			session.nextMap = nextMapDue();
			return menuQuit || session.nextMap || session.disconnect || session.levelChange ||
				(session.window && session.window->closed());
		};
		if (session.client) {
			// Our soldier is the server's: the camera follows the control
			// object, the keys go up as moves, a click on a spawn point asks
			// for it (with the first kit).
			viewer.playerView = [&session](ViewerOptions::PlayerView& view) {
				if (!session.client || !session.client->world().hasControlObject()) {
					return false;
				}
				const net::Vec3f& p = session.client->world().controlObject().position;
				view = {p.x, p.y, p.z, 0};
				return true;
			};
			viewer.onPlayerInput = [&session, &scoreboardHeld, &options, lastAimPitch = 0.0f](
										 const ViewerOptions::PlayerInput& in) mutable {
				scoreboardHeld = options.scoreboard || in.scoreboard;
				if (!session.client) {
					return;
				}
				if (session.voice) {
					session.voice->setKeys(in.voipPushToTalk, in.voipLeaderChannel);
				}
				game::SoldierInput input;
				input.throttle = in.throttle;
				input.strafe = in.strafe;
				input.sprint = in.sprint;
				input.turn = in.turnDegrees;
				input.aimPitch = in.aimPitchDegrees;
				input.fire = in.fire;
				input.altFire = in.altFire;
				input.use = in.use;
				session.client->setInput(net::playerInputFrom(input, lastAimPitch));
				lastAimPitch = input.aimPitch;
			};
			viewer.wantsPointer = [&session] { return session.client && !session.client->world().hasControlObject(); };
			viewer.onClick = [&session, &hudManager](float x, float y) {
				const int point = hudManager.mapMarkerAt(x, y);
				if (session.client && point >= 0) {
					session.client->selectKit(0);
					session.client->selectSpawnGroup(point);
				}
			};
		} else if (round.player()) {
			viewer.onPlayerInput = [&round, &scoreboardHeld, &options, &showMenu, &startedGame, &endOfRound, &commanderScreen,
										&commanderKeyWas, wasAlive = false, spawnedProne = false](
										const ViewerOptions::PlayerInput& in) mutable {
				scoreboardHeld = options.scoreboard || in.scoreboard;
				// Caps Lock opens and closes the commander's screen.
				if (in.commanderScreen && !commanderKeyWas) {
					game::GameWorld::PlayerInfo me;
					commanderScreen = !commanderScreen && round.playerInfo(round.localPlayer(), me) && me.commander;
				}
				commanderKeyWas = in.commanderScreen;
				if ((showMenu && startedGame) || endOfRound.loaded()) {
					round.setPlayerInput({}); // the menu has the keys
					return;
				}
				game::SoldierInput input;
				input.throttle = in.throttle;
				input.strafe = in.strafe;
				input.sprint = in.sprint;
				input.turn = in.turnDegrees;
				input.aimPitch = in.aimPitchDegrees;
				input.fire = in.fire;
				input.altFire = in.altFire;
				input.use = in.use;
				input.switchSeat = in.seat;
				input.selectItem = in.item;
				// Stance: crouch or lie while their keys are held. A spawn where
				// only lying fits (the squad leader's scatter, 0x006c3340) starts
				// prone until a stance key is pressed.
				game::GameWorld::PlayerInfo me;
				const bool alive = round.playerInfo(round.localPlayer(), me) && me.alive;
				if (alive && !wasAlive) {
					spawnedProne = me.spawnedProne;
				}
				wasAlive = alive;
				if (in.crouch || in.lie) {
					spawnedProne = false;
				}
				input.pose = in.lie ? game::SoldierPose::Prone
					: in.crouch     ? game::SoldierPose::Crouch
					: spawnedProne  ? game::SoldierPose::Prone
									: game::SoldierPose::Stand;
				round.setPlayerInput(input);
			};
			// The spawn screen: a pointer, and clicking a spawn icon picks it.
			viewer.wantsPointer = [&round, &showMenu, &startedGame, &endOfRound, &commanderScreen] {
				game::GameWorld::PlayerInfo me;
				return (showMenu && startedGame) || endOfRound.loaded() || commanderScreen || (round.playerInfo(round.localPlayer(), me) && !me.alive);
			};
			viewer.onClick = [&round, &hudManager, &commanderClick](float x, float y) {
				if (commanderClick(x, y)) {
					return;
				}
				const int point = hudManager.mapMarkerAt(x, y);
				if (point == kMarkerLeader || point == kMarkerBeacon) {
					round.setSpawnChoice(round.localPlayer(),
						point == kMarkerLeader ? game::GameWorld::kSpawnOnLeader : game::GameWorld::kSpawnOnBeacon);
				} else if (point >= 0) {
					round.setSpawnChoice(round.localPlayer(), point);
				} else {
					hudManager.click(x, y);
				}
			};
			viewer.playerView = [&round](ViewerOptions::PlayerView& view) {
				const world::Object* player = round.player();
				if (!player) {
					return false;
				}
				// The camera turns with the look (the head turns before the body, 0x0058a6d0).
				game::GameWorld::PlayerInfo look;
				const float lookYaw = round.playerInfo(round.localPlayer(), look) ? look.yaw : player->rotation().x;
				view = {player->position().x, player->position().y, player->position().z, lookYaw};
				if (const world::Object* vehicle = round.playerVehicle()) {
					// A gunner looks along the gun.
					game::GameWorld::PlayerInfo me;
					float gunYaw = 0, gunPitch = 0;
					round.playerInfo(round.localPlayer(), me);
					round.gunAim(vehicle->id(), me.seat, gunYaw, gunPitch);
					view = {vehicle->position().x, vehicle->position().y, vehicle->position().z, vehicle->rotation().x + gunYaw};
					view.driving = true;
					// Its category (VCNoVehicleCategory: the root vehicle's).
					static const char* kCategories[] = {"VCLand", "VCSea", "VCAir", "VCHelicopter"};
					const auto& settings = vehicle->objectTemplate()->settings();
					const auto category = settings.find("vehicleCategory");
					for (int i = 0; i < 4 && category != settings.end(); ++i) {
						if (category->second == kCategories[i]) {
							view.vehicleCategory = i;
						}
					}
				}
				return true;
			};
			if (!options.screenshot.empty() && options.camera.empty()) {
				// Frame the soldier (or the vehicle) from behind (along a gunner's gun).
				const world::Object* player = round.playerVehicle() ? round.playerVehicle() : round.player();
				game::GameWorld::PlayerInfo me;
				float gunYaw = 0, gunPitch = 0;
				round.playerInfo(round.localPlayer(), me);
				round.gunAim(me.vehicle, me.seat, gunYaw, gunPitch);
				const float yawDegrees = (me.vehicle ? player->rotation().x : me.yaw) + gunYaw;
				const float yaw = yawDegrees * 3.14159265f / 180.0f;
				char camera[128];
				std::snprintf(camera, sizeof(camera), "%f/%f/%f/%f/-12", player->position().x - std::sin(yaw) * 5,
					player->position().y + 2.5f, player->position().z - std::cos(yaw) * 5, yawDegrees);
				viewer.camera = camera;
			}
		}
		viewer.onTicks = [&](int ticks) {
			for (int i = 0; i < ticks; ++i) {
				step();
				// The game's sounds this tick (ragdoll impacts), where they happen.
				for (const auto& sound : round.takeSounds()) {
					sounds.playSoundAt(sound.sound, sound.position.x, sound.position.y, sound.position.z);
				}
				if (gameLogic.ended()) {
					sinceEnd += 1.0f / 30.0f;
					showEndOfRound();
				}
			}
			sounds.update(ticks);
		};
		if (options.spawnScreen && round.player()) {
			// Die (and pick the first point to spawn at) to show the spawn screen.
			const std::vector<int> points = round.spawnablePoints(round.playerTeam());
			round.setSpawnChoice(round.localPlayer(), points.empty() ? -1 : points.front());
			round.setHitPoints(round.playerObject(), 0);
		}
		if (!options.screenshot.empty()) {
			return runViewer(sources, viewer);
		}
		if (!session.window) {
			session.window = Window::open(viewer.width, viewer.height);
			if (session.window) {
				session.window->setScript(options.inputScript);
			}
		}
		return session.window ? runViewer(sources, viewer, session.window.get()) : 1;
#else
		std::fprintf(stderr, "built without the renderer (SDL3 not found)\n");
		return 1;
#endif
	}
	return 0;
}

} // namespace
