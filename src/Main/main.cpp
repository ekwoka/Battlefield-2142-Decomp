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
//               [--nosound] [--sound] [--play-sound <name>]
//               [--play [team]] [--drive <vehicle template>]
// --view opens a window showing the level (needs a build with SDL3);
// --screenshot renders one frame to a PNG instead.
// The install directory defaults to $BF2142_GAME_DIR. The API tables are
// symbols/*.console.csv (one --api each): methods listed there but not
// implemented are registered as stubs with the original's argument counts.
#include "Animation/AnimationConsole.h"
#include "DemoCamera.h"
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/CombatAreas.h"
#include "Game/GameLogic.h"
#include "Hud/Hud.h"
#include "Input/ControlMap.h"
#include "Input/GameControllers.h"
#include "Net/Fesl.h"
#include "Net/OnlineConfig.h"
#include "Platform/Platform.h"
#include "Net/OnlineSession.h"
#include "Net/OpenSpyEmulator.h"
#include "Net/Demo.h"
#include "Net/GameClient.h"
#include "Net/GameConnection.h"
#include "Net/GhostLayout.h"
#include "Net/Networkables.h"
#include "Net/RemoteInput.h"
#include "Net/GameEvents.h"
#include "Net/ServerBrowser.h"
#include "Net/ServerQuery.h"
#include "AI/NavMesh.h"
#include "Console/ConsoleBinding.h"
#include "Game/Bots.h"
#include "UI/Player/Player.h"
#include "IO/Localization/Localization.h"
#include "HostedGame.h"
#include "HudWorld.h"
#include "ServerAdmin.h"
#include "ServerScriptBridge.h"
#include "Net/RemoteConsole.h"
#if BF2142_HAS_PYTHON
#include "AdminScript.h"
#endif
#include "Prediction.h"
#include "RemoteGuns.h"
#include "Movie/MovieDecoder.h"
#include "Movie/MovieQueue.h"
#include "Movie/MenuVideo.h"
#include "HudAlerts.h"
#include "HudCombatArea.h"
#include "HudMenus.h"
#include "DeploymentLifecycle.h"
#include "DeploymentTeam.h"
#include "TitanShotTrace.h"
#include "HudMenusChat.h"
#include "HudMenusComm.h"
#include "HudMenusKits.h"
#include "HudMenusVehicles.h"
#include "MapPlayerMarker.h"
#include "MapOrderMarker.h"
#include "MenuBindings.h"
#include "MenuGameState.h"
#include "SettingsObjects.h"
#include "PlayerCamera.h"
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
#include "Scene/EffectSettings.h"
#include "Scene/EnhancedSettings.h"
#include "Scene/LightSettings.h"
#include "Scene/OvergrowthSettings.h"
#include "Scene/PostProductionSettings.h"
#include "Sound/Game/SoundConsole.h"
#include "Sound/Game/GameSounds.h"
#include "Sound/Game/SoundManager.h"
#include "Sound/Game/VoiceMessages.h"
#include "Sound/Game/VfsSoundFileSystem.h"
#include "Sound/SoundEngine/EnhancedAudio.h"
#if BF2142_HAS_STEAM_AUDIO
#include "Sound/Enhanced/SteamAudioOcclusion.h"
#endif
#include "GameAudio.h"
#include "Terrain/Heightmap.h"
#include "Terrain/TerrainData.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/TemplateFiles.h"
#include "World/WorldConsole.h"
#include "Voip/VoiceChat.h"
#include "Voip/VoipNatives.h"

#include "Nametags.h"
#if BF2142_HAS_RENDERER
#include "Render/LevelRenderer.h"
#include "Viewer.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <ctime>
#include <chrono>
#include <csignal>
#include <functional>
#include <cmath>
#include <filesystem>
#include <optional>
#include <thread>
#include <tuple>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
	bool viewAsked = false; // --view given: keep the window even with --simulate
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
	bool enhanced = false;       // --enhanced: start with enhanced.enabled 1 (Scene/EnhancedSettings.h)
	std::string language = "English"; // --language: the registry's Language (0x00405360), strings.csv's column
	bool servers = false;        // --servers: list the servers OpenSpy's master server knows
	std::string connect;         // --connect host[:port] (the original's +joinServer): play on a game server
	std::string joinServer;      // +joinServer <address>: with the menu, its command-line join (GSJoinAddress)
	std::string joinPassword;    // +password <text>: that server's (GSPassword)
	bool hostServer = false;     // +hostServer 1: with the menu, its command-line host (GSIsServer)
	std::string demo;            // --demo <file.bf2142demo>: play a recorded game back (Net/Demo.h)
	std::string demoCamera;      // --demo-camera <file.bf2142cam>: the demo's camera from a file (demo.cameramode playback)
	std::string demoRecordCamera; // --demo-record-camera <file.bf2142cam>: record the demo's camera (demo.cameramode record)
	bool host = false;           // --host: serve the local game to other clients (HostedGame.h)
	bool playTeamGiven = false;  // --play 1|2: on a server, ask for that team (PostRemote 6/7)
	bool walk = false;           // --walk: on a server without a window, walk and turn (the prediction's check)
	bool spectate = false;       // --spectate: on a server, join as a spectator (the menu's SPECTATOR)
	// --remote-actions "<s>:<op> ...": on a server without a window, the
	// inputs from that second of the round on (crouch, prone, stand, fire,
	// hold (stop firing), reload, forward, stop, turn:<deg a tick>,
	// face:<player name> (turn towards that soldier), still (no turning),
	// zoom (the alternate fire's press), pitch:<deg> (the aim, up positive)).
	std::string remoteActions;
	std::string maps;            // --maps level[:mode[:size]],...: a playlist for --host (map changes on the same connections)
	uint16_t hostPort = 17567;   // +port N (the original's "specifies the network port to be used")
	uint16_t queryPort = 29900;  // --query-port N: the LAN query responder's (0: none)
	bool hostPortGiven = false, queryPortGiven = false, timeLimitGiven = false; // given on the command line (over sv.*)
	std::string configFile;      // +config <file>: a server's settings (default <mod>/Settings/ServerSettings.con)
	std::string mapListFile;     // +mapList <file>: a server's map list (default <mod>/Settings/maplist.con)
	std::string overlayPath;     // +overlayPath <dir/>: the original's GSOverlayPath (its Settings/, admin/default.cfg)
	std::string rcon;            // --rcon host[:port]: talk to a server's remote console (admin/default.py)
	std::string rconPassword;    // --rcon-password <password> (admin/default.cfg's)
	std::vector<std::string> rconCommands; // --rcon-command "<console command>" (repeatable; "exec" is added)
	bool movies = true;          // --no-movies: no start-up movies (Init.con's bf2Engine.playMovie)
	std::string movie;           // --movie <file>: play that movie (Movies/<name>.bik under the mod)
	float movieSeconds = 1;      // --movie-seconds: with --screenshot, the movie's frame at that time
	std::string lanAddress = "255.255.255.255"; // --lan <address>: where the LOCAL page's search probes
	std::string playerName;      // --name N: our name on a server (ChangePlayerName)
	std::string probe;           // --probe host[:port]: shake hands and show what the server sends
	// Network diagnostics for a joined game (Net/NetDiagnostics.h):
	std::string netLog;          // --net-log <file>: every datagram in and out, hex and decoded, and the problems
	bool netStrict = false;      // --net-strict: each problem on stderr as found, a summary at the end
	std::string netCapture;      // --net-capture <file>: the datagrams, for --net-replay
	std::string netReplay;       // --net-replay <file>: play a capture into the client (offline, nothing sent)
	uint32_t profileId = 0xffffffff; // the soldier's profile from --login
	bool fesl = false;           // --fesl: greet the account service (no login)
	bool scoreboard = false;     // --scoreboard: show it (as Tab does)
	bool spawnScreen = false;    // --spawn-screen: begin at deployment before creating a soldier
	bool commanderScreen = false; // --commander-screen: take command and open its screen
	bool unlockAll = false;      // --unlock-all: every kit unlock (no stats server to ask)
	std::string menu;            // --menu [mainMenu|endOfRound]: the Flash menu over everything
	float menuSeconds = 10;      // --menu-seconds: how long a screenshot lets the menu play first
	std::vector<std::string> console; // --console "<command>": run once the round has started
	std::vector<std::string> enhancedSettings; // +enhanced.<setting> <value>: set at startup (Scene/EnhancedSettings.h)
	bool offline = false;        // --offline: the menu finds no account service (offline mode)
	// --hud-click x/y[/right|/move] (repeatable): a click (or a pointer
	// move) on the HUD's 800 x 600 screen before the screenshot.
	std::vector<std::string> hudClicks;
	std::string inputScript;     // --input-script "<s>:<command> ...": scripted input in the window (Viewer.h)
	std::string actions;         // --actions "<command> ...": the soldier's inputs before a screenshot (off screen)
	std::string menuScript;      // --menu-script "wait:12 click:x,y type:text ...": drive the menu headless
	std::string login;           // --login [name:password[@soldier]]: log in (FESL, then GameSpy Presence); bare: the configured credentials
	bool online = false;         // --online: the live services (OpenSpy); without it only this machine (Net/OnlineConfig.h)
	bool openspyEmulator = false; // --openspy-emulator: run the local stand-ins of OpenSpy's services (Net/OpenSpyEmulator.h)
	std::vector<std::string> emulatorAccounts; // --emulator-account name:password@soldier[,soldier]
	std::vector<std::string> emulatorCdKeys;   // --emulator-cdkey KEY: the CD-key service takes only these
	std::vector<std::string> emulatorServers;  // --emulator-list host:gameport[:queryport]: what the master lists
	bool requireLogin = false;   // --require-login: a host checks logins as a retail server (HostSetup::login)
	// The login's results for the game connection (Net/OnlineSession.h).
	std::string onlineSoldier, onlineTicket;
	uint32_t onlineNameChecksum = 0;
	std::string dumpTemplate;    // --dump-template: print a template's tree and settings
	std::string cameraMode = "first"; // --camera-mode first|third: the player's initial/debug view
	bool zoom = false;           // --zoom: a screenshot's soldier zooms in after its burst
	int seat = 0;                // --seat N: with --drive, stay aboard in seat N (1 the driver)
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
			options.viewAsked = true;
			options.sound = true;
		} else if (arg == "--nosound") {
			options.sound = false;
		} else if (arg == "--sound") {
			options.sound = true; // the audio engine without a window (with --simulate or --screenshot)
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
		} else if (arg == "--demo" && hasValue) {
			// A demo plays as a joined game whose payloads come from the
			// file (the client pump with no socket, 0x004b52e0).
			options.demo = argv[++i];
			options.connect = "demo";
			options.view = options.screenshot.empty();
		} else if (arg == "--demo-camera" && hasValue) {
			// The original's demo options file: demo.cameramode playback
			// with demo.camerafile (0x0043a3a0, 0x00517490 -> 0x00516830).
			options.demoCamera = argv[++i];
		} else if (arg == "--demo-record-camera" && hasValue) {
			// demo.cameramode record (0x00517490 -> 0x005171f0).
			options.demoRecordCamera = argv[++i];
		} else if (arg == "--probe" && hasValue) {
			options.probe = argv[++i];
		} else if (arg == "--net-log" && hasValue) {
			options.netLog = argv[++i];
		} else if (arg == "--net-strict") {
			options.netStrict = true;
		} else if (arg == "--net-capture" && hasValue) {
			options.netCapture = argv[++i];
		} else if (arg == "--net-replay" && hasValue) {
			// A capture plays as a joined game whose datagrams come from the
			// file (the connect reply on); nothing is sent.
			options.netReplay = argv[++i];
			options.connect = "replay";
			options.view = options.screenshot.empty();
		} else if (arg == "--servers") {
			options.servers = true;
		} else if (arg == "--simulate" && hasValue) {
			options.simulate = std::atoi(argv[++i]);
		} else if (arg == "--language" && hasValue) {
			options.language = argv[++i]; // German, Russian, Chinese_Simplified, ... (strings.csv's header)
		} else if (arg == "--bots" && hasValue) {
			options.bots = std::atoi(argv[++i]);
		} else if (arg == "--menu") {
			options.menu = hasValue && argv[i + 1][0] != '-' && argv[i + 1][0] != '+' ? argv[++i] : "mainMenu";
			options.view = options.screenshot.empty();
		} else if (arg == "--console" && hasValue) {
			options.console.push_back(argv[++i]);
		} else if (arg == "--enhanced") {
			options.enhanced = true;
		} else if (arg == "--offline") {
			options.offline = true;
		} else if (arg == "--hud-click" && hasValue) {
			options.hudClicks.push_back(argv[++i]);
		} else if (arg == "--input-script" && hasValue) {
			options.inputScript = argv[++i];
		} else if (arg == "--actions" && hasValue) {
			options.actions = argv[++i];
		} else if (arg == "--remote-actions" && hasValue) {
			options.remoteActions = argv[++i];
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
		} else if (arg == "--commander-screen") {
			options.commanderScreen = true;
		} else if (arg == "--scoreboard") {
			options.scoreboard = true;
		} else if (arg == "--fesl") {
			options.fesl = true;
		} else if (arg == "--login") {
			options.login = hasValue && argv[i + 1][0] != '-' && argv[i + 1][0] != '+' ? argv[++i] : "config";
		} else if (arg == "--online") {
			options.online = true;
		} else if (arg == "--openspy-emulator") {
			options.openspyEmulator = true;
		} else if (arg == "--emulator-account" && hasValue) {
			options.emulatorAccounts.push_back(argv[++i]);
		} else if (arg == "--emulator-cdkey" && hasValue) {
			options.emulatorCdKeys.push_back(argv[++i]);
		} else if (arg == "--emulator-list" && hasValue) {
			options.emulatorServers.push_back(argv[++i]);
		} else if (arg == "--require-login") {
			options.requireLogin = true;
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
				options.playTeamGiven = true;
			}
			options.view = true;
		} else if (arg == "--seat" && hasValue) {
			options.seat = std::atoi(argv[++i]);
		} else if (arg == "--zoom") {
			options.zoom = true;
		} else if (arg == "--camera-mode" && hasValue) {
			options.cameraMode = argv[++i];
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
		} else if (arg == "+joinServer" && hasValue) {
			options.connect = options.joinServer = argv[++i];
		} else if (arg == "+password" && hasValue) {
			options.joinPassword = argv[++i];
		} else if (arg == "+hostServer" && hasValue) {
			options.hostServer = std::atoi(argv[++i]) != 0;
		} else if (arg == "+port" && hasValue) {
			options.hostPort = static_cast<uint16_t>(std::atoi(argv[++i]));
			options.hostPortGiven = true;
		} else if (arg == "+config" && hasValue) {
			options.configFile = argv[++i];
		} else if (arg == "+mapList" && hasValue) {
			options.mapListFile = argv[++i];
		} else if (arg == "+overlayPath" && hasValue) {
			options.overlayPath = argv[++i];
			if (!options.overlayPath.empty() && options.overlayPath.back() != '/' && options.overlayPath.back() != '\\') {
				options.overlayPath += '/';
			}
		} else if (arg == "--dedicated") {
			options.dedicated = true;
		} else if (arg == "--rcon" && hasValue) {
			options.rcon = argv[++i];
		} else if (arg == "--rcon-password" && hasValue) {
			options.rconPassword = argv[++i];
		} else if (arg == "--rcon-command" && hasValue) {
			options.rconCommands.push_back(argv[++i]);
		} else if (arg == "--no-movies") {
			options.movies = false;
		} else if (arg == "--movie" && hasValue) {
			options.movie = argv[++i];
		} else if (arg == "--movie-seconds" && hasValue) {
			options.movieSeconds = static_cast<float>(std::atof(argv[++i]));
		} else if (arg == "--host") {
			options.host = true;
		} else if (arg == "--walk") {
			options.walk = true;
		} else if (arg == "--spectate") {
			options.spectate = true;
		} else if (arg == "--maps" && hasValue) {
			options.maps = argv[++i];
		} else if (arg == "--time-limit" && hasValue) {
			options.timeLimit = std::atoi(argv[++i]);
			options.timeLimitGiven = true;
		} else if (arg == "--query-port" && hasValue) {
			options.queryPort = static_cast<uint16_t>(std::atoi(argv[++i]));
			options.queryPortGiven = true;
		} else if (arg == "--lan" && hasValue) {
			options.lanAddress = argv[++i];
		} else if (arg == "--name" && hasValue) {
			options.playerName = argv[++i];
		} else if (arg == "+dedicated" && hasValue) {
			options.dedicated = std::atoi(argv[++i]) != 0;
		} else if (arg.rfind("+enhanced.", 0) == 0 && hasValue) {
			options.enhancedSettings.push_back(arg.substr(1) + " " + argv[++i]);
		} else if (arg[0] == '+' && hasValue) {
			++i; // other "+setting value" options are not used yet
		} else {
			std::fprintf(stderr, "unknown option %s\n", arg.c_str());
			return false;
		}
	}
	if (options.simulate > 0 && options.screenshot.empty() && !options.viewAsked) {
		// A game run for a while without a picture stays headless, whatever
		// else asked for the viewer (--play, --connect, --drive): only --view
		// opens a window with it.
		options.view = false;
	}
	if (options.gameDir.empty() && options.rcon.empty()) {
		std::fprintf(stderr, "usage: bf2142 --game <BF2142 install dir> [--api symbols/BF2142.exe.console.csv]\n");
		return false;
	}
	// Without --api, the engine's console tables from symbols/ (here, else
	// the source tree's): the game's scripts call methods the rewrite only
	// answers through them, and a level goes wrong without (no control
	// points, a team losing at once).
	if (options.apiTables.empty()) {
		std::vector<std::filesystem::path> roots {"."};
#ifdef BF2142_SOURCE_DIR
		roots.emplace_back(BF2142_SOURCE_DIR);
#endif
		for (const std::filesystem::path& root : roots) {
			const std::filesystem::path exe = root / "symbols" / "BF2142.exe.console.csv";
			const std::filesystem::path rend = root / "symbols" / "RendDX9_ori.dll.console.csv";
			std::error_code error;
			if (std::filesystem::exists(exe, error) && std::filesystem::exists(rend, error)) {
				options.apiTables = {exe.string(), rend.string()};
				break;
			}
		}
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
std::function<std::string(const std::string&)> g_localize; // strings.csv

constexpr float tabKickColumn = 279; // the KICKVOTE label's 285..373 less the list's 10, the 12-pixel box centred near its start

void setupScoreboard(hud::HudManager& hud)
{
	const std::vector<hud::HudManager::ListColumn> columns {
		{8, 200, 0}, {214, 30, 2}, {245, 30, 2}, {278, 30, 2}, {308, 30, 2}, {342, 30, 2}};
	for (int data : {9, 10}) {
		hud.setListColumns(data, columns);
	}
	// The players' lists add the voice box (column 6, a click toggles the
	// player's mute: 0x007bc590 type 7; where the exe draws it isn't known,
	// so it sits at the name's end), the kit icon (column 7, 15 pixels
	// square as the row filler 0x007bb540 draws it, from the HUD manager's
	// slot 0x64, 0x0074e940: menu::scoreboardIcon; before the voice box)
	// and the Manage tab's kick-vote button (column 8, 0x007bc590 types
	// 10 / 11, under the tab's HUD_TEXT_MENU_SQUAD_MANAGE_KICKVOTE label at
	// 285 / 675, the lists at 10 / 402), and keep the player's id in cell 9.
	auto players = columns;
	players[0].width = 160; // the name, clear of the icons
	players.push_back({196, 0, 0, 12});
	players.push_back({176, 0, 0, 15, {0, 0, 0, 1}}); // black (0x00b3bec0)
	players.push_back({tabKickColumn, 0, 0, 12});
	for (int data : {1, 2}) {
		hud.setListColumns(data, players);
	}	// The "Players" tab first (scoreboard.setToggleShow picks the others).
	hud.set("ToggleScore", 1);
}

void updateTeams(hud::HudManager& hud, const host::HudWorld& world, const game::GameLogic& logic)
{
	const int friendly = world.playerTeam() == 1 ? 1 : 2;
	for (int team = 1; team <= 2; ++team) {
		const std::string prefix = "Team" + std::to_string(team);
		hud.set(prefix + "Selected", world.playerTeam() == team ? 1 : 0);
		hud.setString(prefix + "NameString", team == 1 ? "HUD_TEXT_MENU_SPAWN_ARMY_MEC" : "HUD_TEXT_MENU_SPAWN_ARMY_USMC");
		hud.setString(prefix + "FlagIconPathString", "Ingame/Flags/Icons/Hud/Score/" + logic.team(team).name + "/scoreBoard_Flag.tga");
	}
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

// A player's voice box on the scoreboard (column 6: the voice client's
// mute and talk state; "" none), set while a joined game has voice.
std::function<std::string(int player)> g_scoreboardVoice;

void updateScoreboard(hud::HudManager& hud, const host::HudWorld& world, const world::ObjectManager& objects)
{
	const int friendly = world.playerTeam() == 1 ? 1 : 2;
	game::GameWorld::PlayerInfo me;
	world.playerInfo(world.localPlayer(), me);
	const std::vector<game::GameWorld::PlayerInfo> everyone = world.playerList();
	for (int side = 0; side < 2; ++side) {
		const int team = side == 0 ? friendly : 3 - friendly;
		std::vector<game::GameWorld::PlayerInfo> players;
		for (const game::GameWorld::PlayerInfo& player : everyone) {
			if (player.team == team) {
				players.push_back(player);
			}
		}
		const auto score = [&world](const game::GameWorld::PlayerInfo& p) { return world.scores(p.id).score; };
		std::stable_sort(players.begin(), players.end(), [&](const auto& a, const auto& b) { return score(a) > score(b); });
		std::vector<std::vector<std::string>> rows;
		std::vector<std::array<float, 4>> colors, backgrounds;
		int totalScore = 0, totalTeamwork = 0, totalKills = 0, totalDeaths = 0;
		for (const game::GameWorld::PlayerInfo& p : players) {
			// The cells in the list's font 1 (0x007bb540: "\xa71%s\xa70" 0x009b4f1c,
			// "\xa71%d\xa70" 0x009b6854), then the voice box and the id.
			const auto f1 = [](const std::string& text) { return "\xa7" "1" + text + "\xa7" "0"; };
			const host::HudWorld::Scores n = world.scores(p.id);
			// The Manage tab (ToggleManage): the score columns give way to
			// its VOIP, buddy and kick-vote boxes; the kick box for the
			// others' rows.
			const bool manage = hud.number("ToggleManage") != 0;
			const auto cell = [&](const std::string& text) { return manage ? std::string() : f1(text); };
			rows.push_back({f1(p.name), cell(std::to_string(n.score)), cell(std::to_string(n.teamwork)), cell(std::to_string(p.kills)),
				cell(std::to_string(p.deaths)), cell(p.ai ? "BOT" : std::to_string(std::max(n.ping, 0))),
				g_scoreboardVoice && !p.ai && p.id != world.localPlayer() ? g_scoreboardVoice(p.id) : std::string(),
				dice::hfe::menu::scoreboardIcon(p, me.id == world.localPlayer() ? me.team : 0,
					p.kit && objects.find(p.kit) ? objects.find(p.kit)->objectTemplate() : nullptr),
				manage && p.id != world.localPlayer() ? "Ingame/Scoreboard/Icons/kickbutton_up.tga" : std::string(),
				std::to_string(p.id)});
			totalTeamwork += n.teamwork;
			// The row's colours (0x007bb540; see menu::scoreboardStyle).
			dice::hfe::menu::ScoreboardRow look;
			look.alive = p.alive;
			look.friendlyList = side == 0;
			look.local = p.id == world.localPlayer();
			look.sameSquad = me.squad > 0 && p.squad == me.squad;
			look.localCommands = me.commander;
			const dice::hfe::menu::ScoreboardStyle style = dice::hfe::menu::scoreboardStyle(look, static_cast<int>(colors.size()));
			colors.push_back(style.text);
			backgrounds.push_back(style.background);
			totalScore += score(p);
			totalKills += p.kills;
			totalDeaths += p.deaths;
		}
		if (hud.number("ToggleSquads") != 0) {
			// The "Squads" tab: grouped under the squads' header rows
			// (menu::scoreboardBySquad, 0x007baed0 / 0x007ba570).
			std::vector<dice::hfe::menu::ScoreboardMember> members;
			for (size_t i = 0; i < players.size(); ++i) {
				const auto& p = players[i];
				const host::HudWorld::Scores n = world.scores(p.id);
				members.push_back({static_cast<int>(i), p.squad, p.commander, n.score, n.teamwork, p.kills, p.deaths, std::max(n.ping, 0)});
			}
			std::vector<std::vector<std::string>> grouped;
			std::vector<std::array<float, 4>> groupedColors, groupedBackgrounds;
			const auto text = [](const std::string& id) { return g_localize ? g_localize(id) : id; };
			const auto f1 = [](const std::string& t) { return "\xa7" "1" + t + "\xa7" "0"; };
			int memberRow = 0;
			for (const auto& line : dice::hfe::menu::scoreboardBySquad(members)) {
				if (line.member >= 0) {
					grouped.push_back(rows[static_cast<size_t>(line.member)]);
					groupedColors.push_back(colors[static_cast<size_t>(line.member)]);
					const bool local = players[static_cast<size_t>(line.member)].id == world.localPlayer();
					groupedBackgrounds.push_back(local ? std::array<float, 4> {0.392157f, 0.392157f, 0.392157f, 0.35f}
						: memberRow % 2 != 0           ? std::array<float, 4> {0.65098f, 0.65098f, 0.65098f, 0.8f}
													   : std::array<float, 4> {0, 0, 0, 0});
					++memberRow;
					continue;
				}
				const std::string name = line.squad < 0 ? text("HUD_HUD_MENU_COMMANDER_HEADING")
					: line.squad == 0                   ? text("HUD_TEXT_MENU_SPAWN_SQUAD_UNASSIGNED")
														: std::to_string(line.squad) + "  " + dice::hfe::menu::squadName(line.squad);
				if (line.totals) {
					grouped.push_back({f1(name), f1(std::to_string(line.score)), f1(std::to_string(line.teamwork)),
						f1(std::to_string(line.kills)), f1(std::to_string(line.deaths)), f1(std::to_string(line.ping))});
				} else {
					grouped.push_back({f1(name), "", "", "", "", ""});
				}
				const bool mine = side == 0 && line.squad > 0 && line.squad == me.squad;
				groupedColors.push_back(mine ? std::array<float, 4> {0.1412f, 0.502f, 0.2588f, 1} : std::array<float, 4> {0.5f, 0.5f, 0.5f, 1});
				groupedBackgrounds.push_back({0.8431f, 0.8471f, 0.8471f, 0.9f});
			}
			rows = std::move(grouped);
			colors = std::move(groupedColors);
			backgrounds = std::move(groupedBackgrounds);
		}
		hud.setList(side == 0 ? 2 : 1, std::move(rows));
		hud.setListRowColors(side == 0 ? 2 : 1, std::move(colors));
		hud.setListRowBackgrounds(side == 0 ? 2 : 1, std::move(backgrounds));
		hud.setList(side == 0 ? 10 : 9, {{"", std::to_string(totalScore), std::to_string(totalTeamwork),
											   std::to_string(totalKills), std::to_string(totalDeaths), ""}});
	}
}

// Map markers that pick squad spawns (control points use their ids).
constexpr int kMarkerLeader = 100000;
constexpr int kMarkerBeacon = 100001;
// Mobile spawn points (GameWorld::spawnVehicles): kMarkerVehicle + their
// index in g_spawnVehicles, the list the map shows now; g_spawnVehicle the
// one picked.
constexpr int kMarkerVehicle = 200000;
std::vector<uint32_t> g_spawnVehicles;
uint32_t g_spawnVehicle = 0;
// A Titan marker selects that Titan's moving interior SpawnPoint children.
constexpr int kMarkerTitan = 300000;
std::vector<uint32_t> g_spawnTitans;

// The map: the minimap while playing, the big map with the spawn choices
// while dead (the spawn screen). Markers: control points by owner (with
// the spawn icons on the spawn screen, clickable), the team's soldiers,
// and the player's heading arrow.
// Where the commander's zoom buttons put the commander's view (0x007d8e00:
// the asset's position); the commander screen's map centres on it.
std::optional<std::pair<float, float>> g_commanderView;
// The demo being played (its speed, fov and camera), null otherwise.
std::shared_ptr<dice::hfe::host::DemoPlayback> g_demo;
// The modifiers as of the last input (the commander's squad list clicks).
bool g_ctrlHeld = false, g_shiftHeld = false;
// The team's overhead camera (MultiPlayerOverheadCamera: fOV, height), the
// commander screen's view at its map zoom 2 (render::OverheadView).
float g_overheadFov = 0.3f, g_overheadHeight = 260.0f;
// The map's right-click menu open (the HUD manager's menu states: 0x14
// MapMenuShow, 0x15 SquadLeaderMenuShow, 0x16 CommanderMenuShow; 0 none).
int g_mapMenu = 0;
// The weapon, item selection and vehicle parts of the HUD
// (HudMenusVehicles.h) and the sound the weapon HUD starts (its overheat).
dice::hfe::menu::WeaponHud g_weaponHud;
dice::hfe::menu::AbilityBar g_abilityBar;
dice::hfe::menu::ItemSelection g_itemSelection;
dice::hfe::menu::StrategicObjects g_strategicObjects;
dice::hfe::menu::VehicleHud g_vehicleHud;
std::function<void(const std::string&)> g_hudSound;
int g_inHandSlot = -1;
// A joined game's weapon key pressed this frame (0 none) and held.
int g_remoteItemKey = 0;
// The tactical rose opened by the commander or a squad's player
// (0x00754290 → the squad panel's +0x1e4, SELECTED_ALL_SQUADS).
bool g_allSquads = false;
bool g_remoteItemHeld = false;
// A weapon key pressed on foot (0 none), until the weapon in hand changes
// to it: the key's pick (0x004d2a50 -> 0x0079c190) opens the list.
int g_footItemKey = 0;
// The HUD's input pass over the item list (0x00755d50, ItemListInput),
// which the input handlers run on the frame's input before the soldier
// gets it; updateHud keeps what it needs.
dice::hfe::menu::ItemListInput g_itemList;
bool g_weaponZoomed = false;
// The seat camera's view mode (game::camera::Mode; 1 Inside on foot): the
// runtime VehicleHud's camera test (0x00657660).
int g_cameraMode = 1;
// The HUD's weapon flag (+0x334: 1 from the HUD's constructor 0x007a24c0,
// the soldier's slot 0xa4 taken on foot by 0x007a0f80, kept in a seat):
// menu::soldierWeaponFlag.
bool g_hudWeaponFlag = true;
#if BF2142_HAS_RENDERER
void itemListInput(ViewerOptions::PlayerInput& in)
{
	const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	const std::string sound = g_itemList.apply(g_itemSelection, in.fire, in.item, in.itemStep, now);
	if (!sound.empty() && g_hudSound) {
		g_hudSound(sound);
	}
}
#endif
float g_hudTime = -1;
// What the HUD shows now (HudWorld.h): the local round's, and a joined
// game's while one runs (the console's HUD methods ask it).
host::HudWorld* g_hudWorld = nullptr;
host::RemoteHudWorld* g_remoteHud = nullptr;
// A joined game's soldier deviation now (the client's own weapon feel,
// -1 none), for the crosshair.
float g_remoteDeviation = -1;
// The name tags' icons (Menu/Nametag/nametags.con).
nametags::IconRegistry g_tagIcons;
// The name tags of the game running now (the radio calls reach them).
nametags::Nametags* g_nametags = nullptr;
// The big map (the HUD's menu state 2, MapShow at full size): c_GIMapSize
// toggles it while alive (0x00754290 → 0x0074fd90(2)).
bool g_bigMap = false;
float g_pointerX = 0, g_pointerY = 0; // the last HUD pointer position (800 x 600)
bool g_pointerWarp = false; // the system cursor still has to be put on g_pointerX/Y
// Enter opens deployment without killing the soldier; Redeploy is explicit.
bool g_deploymentOpen = false;
bool g_initialDeploymentPending = false;
// Dead: Enter hid the spawn screen; the respawn timer runs on (0x00754d74, 0x004bfe40).
bool g_spawnScreenHidden = false;
// The local round's overhead camera (the commander's, HudMenus.h): its
// point is g_commanderView's.
dice::hfe::menu::OverheadCamera g_overheadCamera;

// The template of the object a player controls in a vehicle (player slot
// 0x4c: the seat's PlayerControlObject, the vehicle itself for the
// driver); null on foot or when the vehicle isn't known here.
const world::ObjectTemplate* controlledTemplate(game::GameWorld& round, const game::GameWorld::PlayerInfo& player)
{
	const world::Object* vehicle = player.vehicle ? round.objects().find(player.vehicle) : nullptr;
	if (!vehicle || !vehicle->objectTemplate()) {
		return nullptr;
	}
	if (player.seat > 0) {
		const auto seats = round.vehicleSeats(player.vehicle);
		if (static_cast<size_t>(player.seat) < seats.size()) {
			if (const world::ObjectTemplate* seat = round.templates().find(seats[static_cast<size_t>(player.seat)].name)) {
				return seat;
			}
		}
	}
	return vehicle->objectTemplate();
}

void updateMap(hud::HudManager& hud, const host::HudWorld& world, game::GameWorld& round, const game::GameLogic& logic,
	bool commanderScreen, std::optional<float> viewHeading)
{
	game::GameWorld::PlayerInfo me;
	const bool known = world.playerInfo(world.localPlayer(), me);
	const bool spawning = known && dice::main::deploymentScreenShown(me.alive, g_deploymentOpen, g_spawnScreenHidden);
	const bool big = spawning || commanderScreen || g_bigMap;
	hud.set("MapShow", known ? 1 : 0);
	hud.set("MapMinSize", big ? 0 : 1);
	hud.set("MapFullSize", big ? 1 : 0);
	hud.set("MapFullSizeAndSpawnShow", spawning ? 1 : 0);
	hud.set("MapMenuShow", g_mapMenu == 0x14 ? 1 : 0);
	hud.set("SquadLeaderMenuShow", g_mapMenu == 0x15 ? 1 : 0);
	hud.set("SpawnShow", spawning ? 1 : 0);
	hud.set("SuicideButtonShow", known && me.alive && g_deploymentOpen ? 1 : 0);
	if (!known) {
		return;
	}
	const auto folder = [&logic](int team) { return team == 0 ? std::string("Neutral") : logic.team(team).name; };
	std::vector<hud::HudManager::MapMarker> markers;
	const std::vector<int> spawnable = world.spawnablePoints(me.team);
	const int chosen = world.spawnChoice(me.id);
	for (const auto& point : world.controlPointList()) {
		hud::HudManager::MapMarker flag {"Ingame/Flags/Icons/Minimap/" + folder(point.team) + // 0x0077dbf0: a point that can't change hands is a base
				(point.capturable ? "/miniMap_CP.tga" : "/miniMap_CPBase.tga"),
			// 0x0077dc72 / 0x0077dd04 construct 32x32 on both maps.
			point.position.x, point.position.z, 32};
		markers.push_back(flag);
		if (spawning && std::find(spawnable.begin(), spawnable.end(), point.id) != spawnable.end()) {
			// 0x0078e9b0: the group's slot 0x28 picks the active texture (+0x9f4,
			// +0xa04) or the Inactive one (+0x9f0, +0xa00).
			hud::HudManager::MapMarker spawn {std::string("Ingame/Minimap/Icons/") +
					(point.id == chosen ? "spawn_Selected" : "spawn_UnSelected") +
					(world.spawnGroupActive(point.id, me.id) ? ".dds" : "Inactive.dds"),
				point.position.x, point.position.z, hud::HudManager::kSpawnMarkerSize};
			spawn.id = point.id;
			spawn.label = point.name.empty() ? "CP " + std::to_string(point.id) : point.name;
			markers.push_back(spawn);
		}
	}
	// Mobile spawn points: the team's vehicles that carry a SpawnPoint
	// (APCs, the AT and IFV spawn vehicles). The original's spawn map
	// (0x0078e830) draws every group the spawn manager offers the team
	// with the same spawn icons (+0x9f0/+0x9f4 unselected, +0xa00/+0xa04
	// selected, Inactive/active by the group's slot 0x28; the selected
	// group at +0xa48), 16 HUD units, at the group's position; a vehicle's group
	// moves with it.
	g_spawnVehicles.clear();
	if (spawning) {
		g_spawnVehicles = round.spawnVehicles(me.team);
		for (size_t i = 0; i < g_spawnVehicles.size(); ++i) {
			const world::Object* vehicle = round.objects().find(g_spawnVehicles[i]);
			if (!vehicle) {
				continue;
			}
			const bool selected = chosen == game::GameWorld::kSpawnInVehicle && g_spawnVehicle == g_spawnVehicles[i];
			hud::HudManager::MapMarker spawn {std::string("Ingame/Minimap/Icons/") +
					(selected ? "spawn_Selected.dds" : "spawn_UnSelected.dds"),
				vehicle->position().x, vehicle->position().z, hud::HudManager::kSpawnMarkerSize};
			spawn.id = kMarkerVehicle + static_cast<int>(i);
			if (vehicle->objectTemplate()) {
				const auto& settings = vehicle->objectTemplate()->settings();
				const auto label = settings.find("vehicleHud.hudName");
				spawn.label = label == settings.end() ? vehicle->objectTemplate()->name() : label->second;
				if (spawn.label.size() >= 2 && spawn.label.front() == '"' && spawn.label.back() == '"') {
					spawn.label = spawn.label.substr(1, spawn.label.size() - 2);
				}
			}
			markers.push_back(spawn);
		}
	}
	// Titan interior points belong to their spawner's CP group. The local
	// Titan selector narrows that mixed group to this Titan's own points.
	g_spawnTitans.clear();
	if (spawning && !world.joined() && round.gameMode() == "gpm_ti") {
		const std::vector<int> available = world.spawnablePoints(me.team);
		const std::vector<uint32_t> titanSpawns = round.titanSpawnPoints(me.team);
		for (const game::GameWorld::TitanInfo& titan : round.titans()) {
			if (titan.team != me.team || titan.wreck || titan.controlPoint < 0
				|| std::find(available.begin(), available.end(), titan.controlPoint) == available.end()
				|| titanSpawns.empty()) {
				continue;
			}
			// The vehicle pass draws the authored miniMapIcon (mini_titan),
			// including its size, offset and heading. The Titan status-panel
			// artwork is not a map icon. Add only the normal spawn selector here.
			const bool selected = chosen == game::GameWorld::kSpawnOnTitan && round.spawnTitan(me.id) == titan.object;
			hud::HudManager::MapMarker spawn {std::string("Ingame/Minimap/Icons/") +
					(selected ? "spawn_Selected.dds" : "spawn_UnSelected.dds"),
				titan.position.x, titan.position.z, hud::HudManager::kSpawnMarkerSize};
			spawn.label = titan.team == 1 ? "PAC Titan" : "EU Titan";
			spawn.id = kMarkerTitan + static_cast<int>(g_spawnTitans.size());
			g_spawnTitans.push_back(titan.object);
			markers.push_back(spawn);
		}
	}
	// The squad leader's beacon (the leader, to spawn on, is drawn with the team below).
	if (spawning && me.squad != 0) {
		if (const world::Object* beacon = round.objects().find(round.squadBeacon(me.team, me.squad))) {
			hud::HudManager::MapMarker spawn {std::string("Ingame/Minimap/Icons/") +
					(chosen == game::GameWorld::kSpawnOnBeacon ? "spawn_SquadSelected.dds" : "spawn_SquadUnSelected.dds"),
				beacon->position().x, beacon->position().z, hud::HudManager::kSpawnMarkerSize};
			spawn.id = kMarkerBeacon;
			spawn.label = "HUD_TEXT_MENU_SPAWN_PLAYER_SL";
			markers.push_back(spawn);
		}
	}
	// The vehicles (Bf2MenuMapNode 0x00793b60; see HudMenusVehicles.h): by
	// their relation to the player, filtered by Minimap.setPaintVehicle's
	// vehicleType bits, coloured by their relation, turned by the vehicle.
	for (const auto& vehicle : world.vehicleList()) {
		const world::Object* object = round.objects().find(vehicle.object);
		if (!object || !object->objectTemplate()) {
			continue;
		}
		const world::ObjectTemplate& t = *object->objectTemplate();
		const dice::hfe::menu::MapIcons& icons = dice::hfe::menu::mapIcons(t);
		if (icons.info != 0 || !hud.paintsVehicle(icons.vehicleType)) {
			continue; // the reload icons (the silos) are drawn below
		}
		// zoomOnMinimap (the Titans): not on the map at zoom 2 while the
		// player's overhead view shows the Titan itself (0x0079429c).
		if (dice::hfe::menu::hiddenOnZoomedMap(icons, commanderScreen ? hud.commanderMapZoom() : hud.mapZoom(),
				commanderScreen && hud.commanderMapZoom() == 2)) {
			continue;
		}
		int occupantTeam = 0;
		bool occupied = false;
		for (const int occupant : world.vehicleOccupants(vehicle.object)) {
			game::GameWorld::PlayerInfo who;
			if (occupant >= 0 && world.playerInfo(occupant, who)) {
				occupied = true;
				occupantTeam = who.team;
				break;
			}
		}
		const bool mine = me.alive && me.vehicle == vehicle.object;
		const auto relation = dice::hfe::menu::vehicleRelation(occupied, mine, occupantTeam, object->team(), me.team);
		dice::hfe::menu::VehicleMarker look;
		if (!dice::hfe::menu::vehicleMarker(t, relation, mine, false, look) || (look.asPlayer && occupied)) {
			continue;
		}
		// vehicleHud.useVehicleCameraRotation (0x00794f03): turned by its
		// camera's yaw bundle, a seat's look, instead of its body.
		float iconYaw = vehicle.yaw;
		if (const std::string bundle = dice::hfe::menu::cameraRotationBundle(t, round.templates()); !bundle.empty()
			&& !std::isnan(vehicle.cameraYaw)) {
			iconYaw = vehicle.cameraYaw; // a sentry drone's gun (GameWorld::mapObjects)
		} else if (!bundle.empty()) {
			const auto seats = round.vehicleSeats(vehicle.object);
			for (size_t i = 0; i < seats.size(); ++i) {
				float turn = 0;
				if (seats[i].yawBundle == bundle && round.seatYaw(vehicle.object, static_cast<int>(i), turn)) {
					iconYaw += turn;
					break;
				}
			}
		}
		hud::HudManager::MapMarker icon {look.texture, vehicle.position.x, vehicle.position.z, look.width, iconYaw};
		icon.height = look.height;
		icon.offsetX = look.offsetX;
		icon.offsetY = look.offsetY;
		const auto color = dice::hfe::menu::mapRelationColor(relation);
		std::copy(color.begin(), color.end(), icon.color);
		markers.push_back(icon);
	}
	// The Titan silos (miniMapInfo "all"; HudMenusVehicles.h MapIcons): the
	// background (0x0077df60) under the reload ring wiped over the time
	// still to go (0x0077dee0, 0x0078a990), in the vehicles' tint.
	const auto silos = round.missileSilos();
	for (size_t i = 0; i < silos.size(); ++i) {
		const game::GameWorld::SiloInfo& silo = silos[i];
		const world::Object* object = round.objects().find(silo.object);
		if (!object || !object->objectTemplate()) {
			continue;
		}
		const dice::hfe::menu::MapIcons& icons = dice::hfe::menu::mapIcons(*object->objectTemplate());
		const auto relation = dice::hfe::menu::vehicleRelation(false, false, 0, silo.team, me.team);
		if (icons.info == 0 || !hud.paintsVehicle(icons.vehicleType)
			|| (relation == dice::hfe::menu::MapRelation::Enemy && !icons.showOnEnemy && !icons.always)) {
			continue;
		}
		const int frame = silo.number; // the numbered frames _01.._05 (0x006574d0)
		const float tint = relation == dice::hfe::menu::MapRelation::Neutral ? 1.0f : 0.7f;
		const auto layer = [&](const std::string& texture, float sweep) {
			hud::HudManager::MapMarker m {dice::hfe::menu::iconFrame(texture, frame), silo.position.x, silo.position.z,
				icons.size[0]};
			m.height = icons.size[1];
			m.sweep = sweep;
			m.color[0] = m.color[1] = m.color[2] = tint;
			markers.push_back(m);
		};
		if (icons.info == 2 && !icons.background.empty()) {
			layer(icons.background, -1);
		}
		const float sweep = dice::hfe::menu::reloadSweep(silo.reloadLeft, silo.reloadTime, relation);
		if (!icons.reload.empty() && sweep > 0) {
			layer(icons.reload, sweep);
		}
	}
	for (const auto& player : world.playerList()) {
		if (!player.alive || player.team != me.team || player.id == me.id) {
			continue;
		}
		// Squad members are green; the rest of the team is blue.
		hud::HudManager::MapMarker mate = dice::hfe::menu::playerMapMarker(player);
		const auto color = dice::hfe::menu::mapRelationColor(dice::hfe::menu::MapRelation::Friendly,
			me.squad != 0 && player.squad == me.squad);
		std::copy(color.begin(), color.end(), mate.color);
		// 0x0077d9f0, with paint kit 7 on (0x00792f16): a squad leader (slot
		// 0x150) is his squad's 16x16 icon and the commander (0x128) the 32x32
		// mini_commander2, both upright, the 32x32 heading arrow turned
		// beside them in the same colour (0x00793b04).
		if ((player.squadLeader || player.commander) && hud.paintsKit(7)) {
			hud::HudManager::MapMarker arrow {"Ingame/Player/Icons/Minimap/mini_headingArrow.dds", mate.x, mate.z, 32, mate.yaw};
			std::copy(color.begin(), color.end(), arrow.color);
			mate.texture = player.squadLeader
				? "Ingame/Player/Icons/Minimap/SquadLeaders/mini_Squad_" + std::to_string(player.squad) + "_small.dds"
				: "Ingame/Player/Icons/Minimap/mini_commander2.dds";
			mate.size = player.squadLeader ? 16 : 32;
			mate.yaw = 0;
			if (spawning && player.squadLeader && me.squad != 0 && player.squad == me.squad && !player.vehicle) {
				mate.id = kMarkerLeader;
			}
			markers.push_back(mate);
			markers.push_back(arrow);
			continue;
		}
		markers.push_back(mate);
	}
	// The commander screen's mini-map tab (HudManager::drawMiniMap): the
	// team's players as dots coloured by 0x007e7580 (the local player's
	// squad (0.4, 1, 0), its leader white, the team (0.254, 0.552, 0.89)),
	// the local player's mark the commander map's zoom square.
	std::vector<hud::HudManager::MiniMapDot> dots;
	for (const auto& player : world.playerList()) {
		if (!player.alive || player.team != me.team || player.id == me.id) {
			continue;
		}
		hud::HudManager::MiniMapDot dot;
		dot.x = player.position.x;
		dot.z = player.position.z;
		dot.squad = me.squad != 0 && player.squad == me.squad;
		const float squad[4] = {0.4f, 1, 0, 1}, team[4] = {0.254f, 0.552f, 0.89f, 1}, white[4] = {1, 1, 1, 1};
		const float* color = !dot.squad ? team : player.squadLeader ? white : squad;
		std::copy(color, color + 4, dot.color);
		dots.push_back(dot);
	}
	hud.setMiniMapDots(std::move(dots), commanderScreen);
	// The commanders' work: the UAV's ring, strikes on their way, supply
	// crates, and the squad's order.
	physics::Vec3 uav;
	float uavRadius = 0;
	if (round.uavArea(me.team, uav, uavRadius)) {
		markers.push_back({"Ingame/Minimap/Icons/icon_UAV.tga", uav.x, uav.z, 16});
	}
	for (const auto& strike : round.strikes()) {
		if (strike.team == me.team) {
			markers.push_back({strike.ability == game::GameWorld::CommanderAbility::OrbitalStrike
					? "Ingame/Commander/tools/orbit_strike.tga"
					: "Ingame/Commander/tools/force_strike.tga",
				strike.target.x, strike.target.z, 19});
		}
	}
	for (const uint32_t crate : round.supplyCrates()) {
		if (const world::Object* object = round.objects().find(crate)) {
			markers.push_back({"Ingame/Minimap/Icons/icon_supplies.tga", object->position().x, object->position().z, 15});
		}
	}
	std::vector<int> orderSquads;
	if (commanderScreen) {
		// The commander map shows the orders for every squad, including the
		// commander's own order when it has no squad membership.
		for (const auto& player : world.playerList()) {
			if (player.team == me.team && player.squad > 0 &&
				std::find(orderSquads.begin(), orderSquads.end(), player.squad) == orderSquads.end()) {
				orderSquads.push_back(player.squad);
			}
		}
	} else if (me.squad > 0) {
		orderSquads.push_back(me.squad);
	}
	for (const int squad : orderSquads) {
		const game::GameWorld::SquadOrder* orders[] = {
			round.squadLeaderOrder(me.team, squad), round.squadOrder(me.team, squad)};
		for (const game::GameWorld::SquadOrder* order : orders) {
			if (order) {
				if (auto marker = main::mapOrderMarker(static_cast<int>(order->type), order->fromCommander,
						order->position.x, order->position.z)) {
					markers.push_back(std::move(*marker));
				}
			}
		}
	}
	// Enemies a recon drone's pulse revealed to the team, in red.
	// The satellite scan's enemies, shown to the commander only (0x0057cfb0).
	std::vector<int> revealed = world.revealedPlayers(me.team);
	for (const int id : round.scannedPlayers(me.id)) {
		if (std::find(revealed.begin(), revealed.end(), id) == revealed.end()) {
			revealed.push_back(id);
		}
	}
	// The radar rings of the team's sonars, radar grenades and recon
	// drones (GameWorld::radarSources; 0x0078f060: icon_uav_circle, a full
	// ring turned by the sweep with radialRadar, else one growing to the
	// radius over the sweep's turn and fading as 1 - sweep / 2 pi), and
	// the enemies the rings pass (radarContacts) with the reported ones.
	{
		const float sweep = round.radarSweep();
		for (const game::GameWorld::RadarSource& source : round.radarSources(me.team)) {
			hud::HudManager::MapMarker ring {"Ingame/Minimap/icon_uav_circle.tga", source.position.x, source.position.z, 1};
			if (source.radial) {
				ring.worldSize = 2 * source.radius;
				ring.yaw = sweep * 57.29578f;
			} else {
				ring.worldSize = std::max(2 * source.radius * sweep / 6.2831853f, 0.01f);
				ring.color[3] = 1 - sweep / 6.2831853f;
			}
			markers.push_back(ring);
		}
	}
	for (const game::GameWorld::RadarContact& contact : round.radarContacts(me.team)) {
		if (contact.player >= 0 && std::find(revealed.begin(), revealed.end(), contact.player) == revealed.end()) {
			revealed.push_back(contact.player);
		}
	}
	// The squad radar's enemies (their kits' spotted icons, a decoy's place
	// for one on foot near it) and the vehicles the reports spotted, in red
	// (menu::radarMarkers).
	for (const dice::hfe::menu::RadarMarker& m : dice::hfe::menu::radarMarkers(world, round, revealed)) {
		hud::HudManager::MapMarker spotted {m.texture, m.x, m.z, m.size, m.yaw};
		spotted.color[1] = 0.3f;
		spotted.color[2] = 0.3f;
		markers.push_back(spotted);
	}
	hud.setMapInVehicle(me.alive && me.vehicle != 0);
	if (me.alive) {
		// The player's own icon and its heading arrow, 32 px (0x00792980).
		markers.push_back({"Ingame/Player/Icons/Minimap/mini_LocalArrow2.tga", me.position.x, me.position.z, 32, viewHeading.value_or(me.yaw)});
		hud::HudManager::MapMarker arrow {"Ingame/Player/Icons/Minimap/mini_headingArrow.dds", me.position.x, me.position.z, 32,
			viewHeading.value_or(me.yaw)};
		markers.push_back(arrow);
		hud.setMapFocus(me.position.x, me.position.z);
	}
	if (commanderScreen && g_commanderView) {
		hud.setMapFocus(g_commanderView->first, g_commanderView->second);
	}
	// The overhead view over the commander's point (else his own), its
	// ground width 2 tan(fov / 2) height (0x0077c4b0).
	const bool overhead = commanderScreen && hud.commanderMapZoom() == 2;
	float viewX = g_commanderView ? g_commanderView->first : me.position.x;
	float viewZ = g_commanderView ? g_commanderView->second : me.position.z;
	// Joined: the team's overhead camera where the server put it (its
	// ghost, moved by our CommanderCam), as 0x00779680 takes it.
	if (overhead && world.joined()) {
		world.overheadCamera(me.team, viewX, viewZ);
	}
	hud.setOverhead(overhead, viewX, viewZ, 2 * std::tan(g_overheadFov / 2) * g_overheadHeight);
	hud.set("MinimapZoomDisplay", static_cast<float>(commanderScreen ? hud.commanderMapZoom() : hud.mapZoom()));
	hud.setMapMarkers(std::move(markers));
}

// The commander's tools on the HUD (HudElementsCommander*.con): whether
// the screen shows, which tools have their asset, its armour, and the
// recharge bars.
void updateCommander(hud::HudManager& hud, const host::HudWorld& world, const game::GameWorld& round, bool screen)
{
	game::GameWorld::PlayerInfo me;
	const bool commander = world.playerInfo(world.localPlayer(), me) && me.commander;
	// (CommanderInterfaceShow is the commander's tactical rose, menu state
	// 9: the comm roses show it.)
	hud.set("CommanderShow", commander && screen ? 1 : 0);
	// The right-click menu (state 0x16), not the screen.
	hud.set("CommanderMenuShow", commander && screen && g_mapMenu == 0x16 ? 1 : 0);
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

// The spawn screen's kit buttons show the upgrades the player has for each
// kit (Kit%iAbilityIcon%iShow / PathString, seven a kit, the HUD manager's
// variables of 0x0044f1a0): the kit's unlocked ItemContainers' items with
// a weaponHud.specialAbilityIcon (unl_scope_zoom, unl_stabilizer, ...), in
// the kit's order.
void updateKitAbilities(hud::HudManager& hud, const game::GameWorld& round, const game::GameLogic& logic,
	const world::ObjectTemplateManager& templates)
{
	const int team = round.playerTeam() == 1 ? 1 : 2;
	const auto& kits = logic.team(team).kits;
	const auto& unlocked = round.localPlayerUnlocks;
	for (size_t kit = 0; kit < 5; ++kit) {
		std::vector<std::string> icons;
		const world::ObjectTemplate* kitTemplate = kit < kits.size() ? templates.find(kits[kit].name) : nullptr;
		for (size_t c = 0; kitTemplate && c < kitTemplate->children().size(); ++c) {
			const std::string& name = kitTemplate->children()[c].name;
			const world::ObjectTemplate* container = templates.find(name);
			if (!container || (!round.unlockAllByDefault && std::find(unlocked.begin(), unlocked.end(), name) == unlocked.end())) {
				continue;
			}
			for (const auto& item : container->children()) {
				const world::ObjectTemplate* itemTemplate = templates.find(item.name);
				const auto icon = itemTemplate ? itemTemplate->settings().find("weaponHud.specialAbilityIcon") : decltype(itemTemplate->settings().end()) {};
				if (itemTemplate && icon != itemTemplate->settings().end() && !icon->second.empty()
					&& std::find(icons.begin(), icons.end(), icon->second) == icons.end()) {
					icons.push_back(icon->second);
				}
			}
		}
		for (size_t i = 0; i < 7; ++i) {
			const std::string prefix = "Kit" + std::to_string(kit) + "AbilityIcon" + std::to_string(i);
			hud.set(prefix + "Show", i < icons.size() ? 1 : 0);
			hud.setString(prefix + "PathString", i < icons.size() ? icons[i] : std::string());
		}
	}
}

// The HUD's variables from the game it shows (HudWorld.h: the local round
// or a joined game's server; `round` the level's objects either way).
// The seat's second gun's block (0x007c2d50; see updateHud): its own
// WeaponHud, so neither block reads its template's fields again each frame.
dice::hfe::menu::WeaponHud g_secondaryWeaponHud;

void updateSecondaryWeapon(hud::HudManager& hud, const host::HudWorld& world, const world::Object* vehicle,
	bool commanderScreen, float dt)
{
	const host::HudWorld::Weapon second = vehicle && !commanderScreen ? world.seatGroupWeapon(1) : host::HudWorld::Weapon {};
	if (!second.weapon || (!second.gun && second.rounds < 0)) {
		hud.set("SecondaryAmmoShow", 0);
		return;
	}
	const game::FireArmSettings gun = second.gun ? second.gun->settings() : game::FireArmSettings::fromTemplate(*second.weapon);
	dice::hfe::menu::WeaponHudInput in;
	in.weapon = second.weapon;
	in.seconds = dt;
	in.magSize = gun.magSize;
	in.nrOfMags = gun.nrOfMags;
	in.rounds = second.rounds;
	in.mags = second.mags >= 0 ? second.mags : 0;
	if (const game::FireArm* arm = second.gun) {
		in.reload = arm->reloading() && gun.reloadTime > 0 ? 1 - arm->reloadLeft() / gun.reloadTime : 0;
		in.heat = gun.heatAddWhenFire > 0 ? arm->heat() : 0;
		in.overheated = arm->overheated();
	}
	const std::string sound = g_secondaryWeaponHud.update(hud, in, true);
	if (!sound.empty() && g_hudSound) {
		g_hudSound(sound);
	}
}

void updateHud(hud::HudManager& hud, const host::HudWorld& world, game::GameWorld& round, const game::GameLogic& logic,
	bool scoreboard = false, bool commanderScreen = false, std::optional<float> viewHeading = {})
{
	updateMap(hud, world, round, logic, commanderScreen, viewHeading);
	updateCommander(hud, world, round, commanderScreen);
	updateTeams(hud, world, logic);
	hud.set("ScoreboardShow", scoreboard ? 1 : 0);
	if (scoreboard) {
		updateScoreboard(hud, world, round.objects());
	}
	const world::Object* soldier = world.soldier();
	const bool alive = soldier != nullptr;
	const world::Object* const playerVehicle = world.vehicle();
	const bool inVehicle = playerVehicle != nullptr;
	// The in-game HUD stays up while dead (the spawn screen is part of it);
	// the soldier's own items have their own show variables.
	const bool connected = world.localPlayer() >= 0;
	hud.set("ShowIngameHud", connected ? 1 : 0);
	hud.set("ShowSoldierHud", connected ? 1 : 0);
	hud.set("ShowVehicleHud", inVehicle ? 1 : 0);
	hud.set("InVehicle", inVehicle ? 1 : 0);
	game::GameWorld::ProjectileCamera missileView;
	const bool ridingMissile = !world.joined() && round.projectileCamera(world.localPlayer(), missileView);
	// The authored gunship indicator is a compass strip showing estimated range progress.
	hud.set("BombFuelAngle", ridingMissile ? dice::hfe::menu::missileFuelAngle(1.0f - 0.5f * (missileView.rangeProgress + 1.0f - missileView.lifeRemaining)) : 0.0f);
	hud.set("MenuBackgroundAlpha", 1);
	// The crosshair (CrosshairIconShow, the weapon HUD's +0xec, 0x007be6e0)
	// needs CrosshairActive (+0xed), which the HUD's menu state sets
	// (0x007c1bf0): on with no menu while alive, off for the spawn screen
	// (1), the scoreboard and the roses (2..6, 8, 9, 0xc) and the
	// commander screen and its states (0x10, 0x12.., 0x17.., 0x21, 0x22).
	const bool crosshairActive = !scoreboard && !commanderScreen && !g_bigMap && !g_deploymentOpen;
	hud.set("CrosshairIconShow", alive && !inVehicle && crosshairActive ? 1 : 0);
	// The weapon HUD's hit indicator (0x007be6e0): the local player's alpha
	// (Player +0x2a4, 1 after a hit, fading 3 a second) while the crosshair
	// is active; shown when it is not 0. The vehicle HUD shares the variables.
	const float hitAlpha = connected && crosshairActive ? round.hitIndicator(round.localPlayer()) : 0.0f;
	hud.set("HitIndicatorIconAlpha", hitAlpha);
	hud.set("HitIndicatorIconShow", hitAlpha != 0 ? 1 : 0);
	// In a vehicle: its icon and armour, and a gunner's gun (its HUD by
	// the firearm's weaponHud.guiIndex, the turret's direction).
	hud.set("VehicleIconShow", inVehicle ? 1 : 0);
	hud.set("PrimaryHeatShow", 0);
	hud.set("TurretIconShow", 0);
	// The frame's time (the item selection's timer).
	const float now = round.roundTime();
	const float dt = g_hudTime < 0 || now < g_hudTime ? 0.0f : now - g_hudTime;
	g_hudTime = now;
	// The gun in hand (the soldier's or the seat's) for the weapon HUD.
	const game::FireArm* heldGun = nullptr;
	const world::ObjectTemplate* heldGunTemplate = nullptr;
	const host::HudWorld::Weapon held = world.heldWeapon();
	bool childHud = false; // the seat's vehicleHud.useChildHud
	if (const world::Object* vehicle = playerVehicle) {
		const auto& settings = vehicle->objectTemplate()->settings();
		const auto icon = settings.find("vehicleHud.vehicleIcon");
		hud.setString("VehicleIconPathString", icon != settings.end() ? icon->second : std::string());
		const float left = world.hitPoints(vehicle->id());
		const auto most = settings.find("armor.maxHitPoints");
		const float max = most != settings.end() ? static_cast<float>(std::atof(most->second.c_str())) : 0;
		// The vehicle's own HUD (0x007a3180 → 0x007a2e60, 0x007a18a0,
		// 0x007a2b50): name, armour, speed, altitude, the seat dots.
		dice::hfe::menu::VehicleHudInput look;
		look.vehicle = vehicle->objectTemplate();
		look.armor = max > 0 ? left / max : 1.0f;
		{
			// Matrix rows' y (Mat3::yawPitchRoll's inverse): forward.y = -sin(pitch),
			// right.y = sin(roll) cos(pitch).
			const float rad = 3.14159265f / 180.0f, pitch = vehicle->rotation().y * rad;
			look.forwardY = -std::sin(pitch);
			look.rightY = std::sin(vehicle->rotation().z * rad) * std::cos(pitch);
		}
		// The walkers' step (0x007a0940): the vehicle's own animation system's
		// walk cycle (a joined client's: its ghost's, animateRemoteVehicle).
		look.walking = round.walkCycle(vehicle->id(), look.walkLength, look.walkTime);
		// vehicleHud.useChildArmor (0x007a18a0 -> 0x005c6460): the
		// controlled seat's own armour or its first armoured child's: a
		// Titan gun hub shows its gun's (the Titan's part of that name),
		// not the Titan's.
		if (game::GameWorld::PlayerInfo seated; world.playerInfo(world.localPlayer(), seated)) {
			if (const world::ObjectTemplate* armour =
					dice::hfe::menu::childArmorTemplate(controlledTemplate(round, seated), round.templates())) {
				if (armour != vehicle->objectTemplate()) {
					for (const auto& part : round.titanParts(seated.team)) {
						if (part.maxHitPoints > 0 && part.name.size() == armour->name().size() &&
							std::equal(part.name.begin(), part.name.end(), armour->name().begin(),
								[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) {
							look.armor = part.hitPoints / part.maxHitPoints;
							break;
						}
					}
				}
			}
		}
		look.seconds = dt;
		// The control object's sprint (slots 0x108 / 0x100: the boost's
		// stamina): the root PCO's for its driver, a seat's own (never
		// configured: 1) for the others; a joined game's vehicles carry
		// none to the client, 1.
		if (game::GameWorld::PlayerInfo driver; !world.joined() && world.playerInfo(world.localPlayer(), driver) && driver.seat == 0) {
			round.vehicleStamina(vehicle->id(), look.stamina, look.sprinting);
		}
		for (const auto& info : world.vehicleList()) {
			if (info.object == vehicle->id()) {
				const physics::Vec3& v = info.velocity;
				look.speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
				look.climb = v.y;
			}
		}
		float sea = 0;
		look.altitude = vehicle->position().y - (round.waterLevel(sea) ? sea : 0.0f);
		game::GameWorld::PlayerInfo me;
		const bool known = world.playerInfo(world.localPlayer(), me);
		const auto seats = round.vehicleSeats(vehicle->id());
		const auto occupants = world.vehicleOccupants(vehicle->id());
		for (size_t i = 0; i < seats.size(); ++i) {
			dice::hfe::menu::SeatDot dot;
			const world::ObjectTemplate* part = round.templates().find(seats[i].name);
			const std::string pos = dice::hfe::menu::hudSetting(part, "vehicleHud.vehicleIconPos");
			if (!pos.empty()) {
				dot.x = static_cast<float>(std::atof(pos.c_str()));
				const size_t slash = pos.find('/');
				dot.y = slash == std::string::npos ? dot.x : static_cast<float>(std::atof(pos.c_str() + slash + 1));
			}
			const int occupant = i < occupants.size() ? occupants[i] : -1;
			game::GameWorld::PlayerInfo who;
			const bool occupied = occupant >= 0 && world.playerInfo(occupant, who);
			dot.state = dice::hfe::menu::seatDotState(occupied, occupied && who.id == me.id, occupied ? who.squad : 0,
				known && me.team == who.team ? me.squad : 0, occupied && who.team == me.team);
			if (occupied) {
				dot.occupant = who.name;
				dot.relation = who.team != me.team ? 1 : who.squad != 0 && who.squad == me.squad ? 3 : 2;
			}
			look.seats.push_back(dot);
		}
		// The controlled object (the seat's PlayerControlObject, the vehicle
		// for the driver) and the other seats' yaw bundles (0x007a2b50).
		look.seat = known && me.seat > 0 && static_cast<size_t>(me.seat) < seats.size()
			? round.templates().find(seats[static_cast<size_t>(me.seat)].name)
			: vehicle->objectTemplate();
		for (size_t i = 0; known && i < seats.size(); ++i) {
			if (static_cast<int>(i) == me.seat) {
				continue;
			}
			dice::hfe::menu::OtherSeat other;
			other.bundle = static_cast<int>(i);
			other.hasBundle = world.seatYaw(vehicle->id(), static_cast<int>(i), other.rotation);
			look.others.push_back(other);
		}
		look.repairCells = world.repairCells(vehicle->id());
		// Read the seat's active PIFire group, not the legacy first gun in
		// the layout (which can be an alternate-fire machine gun on tanks).
		const host::HudWorld::Weapon primary = world.seatGroupWeapon(0);
		if (primary.weapon) {
			heldGun = primary.gun;
			heldGunTemplate = primary.weapon;
			hud.set("CrosshairIconShow", crosshairActive ? 1 : 0);
		}
		float yaw = 0, pitch = 0;
		if (known && world.gunTurn(vehicle->id(), me.seat, yaw, pitch)) {
			// VehicleHud already applied the authored hasTurretIcon flag.
			// Having an aim angle must not enable this icon on every vehicle.
			hud.set("VehicleTurretAngle", yaw);
		}
		// The seat's vehicleHud.useChildHud (block +0xb9; the weapon HUD's
		// frame 0x007c38c0): a seat without a gun shows the soldier's own
		// (0x007c1f10 run again for player slot 0xb4) and either way the
		// weapon block takes the soldier's place (UsingChildHud, +0x176).
		if (known && me.seat >= 0 && static_cast<size_t>(me.seat) < seats.size()) {
			childHud = dice::hfe::menu::hudNumber(round.templates().find(seats[static_cast<size_t>(me.seat)].name),
						   "vehicleHud.useChildHud", 0) != 0;
		}
		if (childHud && !heldGunTemplate && !world.joined() && held.gun && held.weapon) {
			heldGun = held.gun;
			heldGunTemplate = held.weapon;
		}
		// The gui index (0x007a01e0 -> the runtime VehicleHud's 0x00657660,
		// HudMenusVehicles.h vehicleGuiIndex): by the seat camera's mode and
		// the HUD components' showOnCamMode lists, so the overlays go in the
		// outside views. It replaces the guns' own set above.
		if (known) {
			dice::hfe::menu::VehicleGuiInput gui;
			gui.controlled = look.seat;
			gui.weapon = primary.weapon;
			gui.second = world.seatGroupWeapon(1).weapon;
			gui.child = !world.joined() ? held.weapon : nullptr;
			gui.cameraMode = g_cameraMode;
			game::GameWorld::ProjectileCamera riding;
			gui.projectileCamera = !world.joined() && round.projectileCamera(world.localPlayer(), riding);
			gui.zoomed = g_weaponZoomed;
			// The HUD's sprint flag in a seat: the vehicle's boost (0x007a2e60);
			// its weapon flag the last on foot.
			gui.sprintTest = dice::hfe::menu::guiSprintTest(look.sprinting, g_hudWeaponFlag);
			hud.set("GuiIndex", static_cast<float>(dice::hfe::menu::vehicleGuiIndex(gui)));
		}
		// Gunship pilot indices come from the selected weapon; transport
		// indices come from the vehicle. Use the resolved HUD for both.
		look.torque = dice::hfe::menu::aircraftHudThrust(static_cast<int>(hud.number("GuiIndex")),
			world.pilotThrust(vehicle->id()));
		g_vehicleHud.update(hud, look);
	} else {
		g_vehicleHud.clear(hud);
	}
	// Compass headings, degrees clockwise from north (+z): the vehicle's
	// and, for the minimap's ring, the Titan gun camera or soldier.
	const auto heading = [](float yaw) { return yaw - 360.0f * std::floor(yaw / 360.0f); };
	if (const world::Object* vehicle = playerVehicle) {
		hud.set("VehicleAngle", heading(vehicle->rotation().x));
	}
	if (soldier) {
		// <map>DelayedMapAngle (the map node's variables, registered by its
		// constructor 0x00784cf0 with <map>RMBMenuState, MapFilter%iActive,
		// SatelliteActive and the CPNames* ones).
		hud.set("MinimapDelayedMapAngle", heading(viewHeading.value_or(soldier->rotation().x)));
	}
	hud.set("PlayerHealthShow", alive ? 1 : 0);
	const float health = world.health();
	hud.set("PlayerHealth", health);
	hud.setString("PlayerHealthString", std::to_string(static_cast<int>(std::lround(health * 100))));
	const game::FireArm* weapon = held.gun;
	if (weapon && !inVehicle) {
		heldGun = weapon;
		heldGunTemplate = held.weapon;
	} else if (!inVehicle && held.weapon) {
		heldGunTemplate = held.weapon; // a joined game's: the template and the server's counts
	}
	// UsingChildHud (0x007c38c0): on foot with a gun, or in a seat with
	// useChildHud.
	hud.set("UsingChildHud", heldGunTemplate && (!inVehicle || childHud) ? 1 : 0);
	// The sprint's stamina (the player HUD's 0x007a0f80, PlayerStamina +0x1d0
	// from the soldier's slot 0x108; PlayerStaminaShow +0x2a9 while
	// sprinting, slot 0x100, or below 1; PlayerStaminaTextStringShow +0x2b2
	// for 3 s after it last fell, the timer +0x238 run by 0x007a05c0).
	{
		float stamina = 1;
		bool sprinting = false;
		const bool has = alive && !inVehicle && world.soldierStamina(stamina, sprinting);
		static float staminaWas = 1, staminaTimer = -1;
		if (staminaTimer >= 0) {
			staminaTimer += dt;
			if (staminaTimer >= 3) {
				staminaTimer = -1;
			}
		}
		if (has && stamina < staminaWas) {
			staminaTimer = 0;
		}
		staminaWas = has ? stamina : 1;
		hud.set("PlayerStamina", has ? stamina : 1);
		hud.set("PlayerStaminaShow", has && (sprinting || stamina < 1) ? 1 : 0);
		hud.set("PlayerStaminaTextStringShow", has && staminaTimer >= 0 ? 1 : 0);
		// The blinks (0x00778040 / 0x00778080, made by 0x0079e5a0):
		// below 0.3, faster the nearer empty.
		static dice::hfe::menu::HudBlink staminaBlink, healthBlink;
		hud.set("PlayerStaminaBlink", staminaBlink.tick(has ? stamina : 1, dt) ? 1 : 0);
		hud.set("PlayerHealthBlink", healthBlink.tick(alive ? health : 1, dt) ? 1 : 0);
	}
	hud.set("PrimaryAmmoShow", 0);
	// A seat's second gun (0x007c2d50, the control object's slot 0x78(1):
	// the active weapon of its PIAltFire group, +0x88; the PIFlareFire
	// group has no HUD) sets the Secondary* block as 0x007c1f10 the Primary*
	// ones; without one, or with weaponHud.showInfo 0 (block +0xd0), the
	// block's show (+0x16d) is cleared. A joined game's from our own state's group 1
	// (its place in the group, rounds and magazines).
	updateSecondaryWeapon(hud, world, playerVehicle, commanderScreen, dt);
	// With the commander screen open (the HUD manager's slot 0x234) the
	// weapon HUD's frame (0x007c40c0) skips the update and clears its show
	// flags (PrimaryAmmoShow +0x15d, the clips, the reload, the crosshair
	// +0xec...), so the ammunition doesn't draw over the screen.
	if (!commanderScreen && (heldGun || (heldGunTemplate && held.rounds >= 0))) {
		// The weapon HUD (0x007c1f10; see HudMenusVehicles.h). The game's
		// magazines count the one in the gun (the firearm's slot 0xb0). A
		// joined game's counts are our player's own state's (the server's
		// firearm: PlayerOwnState's rounds and magazines).
		const game::FireArmSettings gun = heldGun ? heldGun->settings() : game::FireArmSettings::fromTemplate(*heldGunTemplate);
		dice::hfe::menu::WeaponHudInput in;
		in.weapon = heldGunTemplate;
		in.seconds = dt;
		// The fire state (0x005c1b60): the fire held for the gun in hand.
		if (game::GameWorld::PlayerInfo shooter; world.playerInfo(world.localPlayer(), shooter)) {
			in.firing = shooter.firing;
		}
		in.magSize = gun.magSize;
		in.nrOfMags = gun.nrOfMags;
		if (heldGun) {
			in.rounds = heldGun->roundsInMag();
			in.mags = heldGun->magsLeft() + 1;
			in.reload = heldGun->reloading() && gun.reloadTime > 0 ? 1 - heldGun->reloadLeft() / gun.reloadTime : 0;
			in.heat = gun.heatAddWhenFire > 0 ? heldGun->heat() : 0;
			in.overheated = heldGun->overheated();
			in.fireRate = heldGun->fireMode(); // the fire component's mode (FireArm::fireMode)
			in.scanning = heldGun->scanning(); // the pulse meter's (0x007be380 → 0xc4f2 slots 0xf8 / 0xfc)
			in.scan = heldGun->scan();
		} else {
			in.rounds = held.rounds;
			in.mags = held.mags >= 0 ? held.mags : 0;
		}
		// The range finder's ray and the distance counter's projectile
		// (0x007c1470), only for the weapons with them (read once per
		// template); the ray (999 m from the eye along the look, bisected
		// against GameWorld::lineOfSight to a metre) at most ten times a
		// second.
		static const world::ObjectTemplate* rangeTemplate = nullptr;
		static bool rangeFinder = false, distanceCounter = false;
		if (heldGunTemplate != rangeTemplate) {
			rangeTemplate = heldGunTemplate;
			rangeFinder = dice::hfe::menu::hudNumber(heldGunTemplate, "weaponHud.hasRangeFinder", 0) != 0;
			distanceCounter = dice::hfe::menu::hudNumber(heldGunTemplate, "weaponHud.hasDistanceCounter", 0) != 0;
		}
		game::GameWorld::PlayerInfo me;
		// A joined game's the same: our soldier's look (the prediction's) and
		// the level's sight lines; the distance counter's projectiles are
		// the server's (a client's round has none).
		if ((rangeFinder || distanceCounter) && world.playerInfo(world.localPlayer(), me) && me.alive) {
			static float rangeAt = -1;
			static bool rangeHit = false;
			static float rangeDistance = 0;
			static std::array<float, 3> rangePoint {};
			if (rangeFinder && (now - rangeAt > 0.1f || now < rangeAt)) {
				rangeAt = now;
				const float yaw = me.yaw * 3.14159265f / 180, pitch = me.aimPitch * 3.14159265f / 180;
				const physics::Vec3 eye {me.position.x, me.position.y + 1.6f, me.position.z};
				const physics::Vec3 dir {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
				const auto at = [&](float d) { return physics::Vec3 {eye.x + dir.x * d, eye.y + dir.y * d, eye.z + dir.z * d}; };
				const uint32_t self = me.vehicle ? me.vehicle : me.soldier;
				rangeHit = !round.lineOfSight(eye, at(999), self);
				if (rangeHit) {
					float lo = 0, hi = 999;
					for (int i = 0; i < 10; ++i) {
						const float mid = (lo + hi) / 2;
						(round.lineOfSight(eye, at(mid), self) ? lo : hi) = mid;
					}
					const physics::Vec3 hit = at(hi);
					rangePoint = {hit.x, hit.y, hit.z};
					const float dx = hit.x - me.position.x, dy = hit.y - me.position.y - 1, dz = hit.z - me.position.z;
					rangeDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
				}
			}
			in.rangeHit = rangeHit;
			in.targetDistance = rangeDistance;
			in.target = rangePoint;
			if (distanceCounter) {
				float newest = 1e9f;
				for (const game::Projectile& p : round.projectiles()) {
					if (p.alive && p.player == world.localPlayer() && p.shooter == (me.vehicle ? me.vehicle : me.soldier) && p.age < newest) {
						newest = p.age;
						const float dx = p.position.x - p.origin.x, dy = p.position.y - p.origin.y, dz = p.position.z - p.origin.z;
						in.travelled = static_cast<int>(std::lround(std::sqrt(dx * dx + dy * dy + dz * dz)));
					}
				}
			}
		}
		const std::string sound = g_weaponHud.update(hud, in);
		if (std::getenv("BF2142_INPUT_TRACE")) {
			static int lastRounds = -999, lastMags = -999;
			if (in.rounds != lastRounds || in.mags != lastMags) {
				std::printf("hud-ammo rounds=%d mags=%d text=%s clips=%s\n", in.rounds, in.mags,
					hud.string("PrimaryAmmoString").c_str(), hud.string("PrimaryClipString").c_str());
				lastRounds = in.rounds;
				lastMags = in.mags;
			}
		}
		if (!sound.empty() && g_hudSound) {
			g_hudSound(sound);
		}
		// showInfo 0: 0x007c1b90's argument, the local player not the
		// commander (slot 0x128), also takes CrosshairActive (+0xed) off.
		if (!g_weaponHud.fields().showInfo) {
			game::GameWorld::PlayerInfo self;
			if (!world.playerInfo(world.localPlayer(), self) || !self.commander) {
				hud.set("CrosshairIconShow", 0);
			}
		}
	} else {
		g_weaponHud.hide(hud);
	}
	// The ability bar (0x007c3af0, after the weapon block; see
	// HudMenusVehicles.h): the gifts in a vehicle, the tool's targets on
	// foot, the tools' battery as PrimaryAmmo. Its sound is the kit's
	// AbilityHud's (updateHud's caller plays it).
	if (game::GameWorld::AbilityBarInfo ability; alive && !commanderScreen && world.abilityBar(ability)) {
		g_abilityBar.update(ability, inVehicle);
	} else {
		g_abilityBar.hide();
	}
	g_abilityBar.apply(hud);
	// The item selection on foot: the kit's weapons by slot (their
	// weaponHud.selectIcon). A weapon key's pick opens it for 0.4 s with
	// the key's slot flashing (0x004d2a50 -> 0x0079c190; the HUD's input
	// 0x00755d50 sets +0x20 = 0.4 and state 0xb when the new weapon is the
	// key's); the weapon-change event 2/0x20 (0x0079fd30(0)) for 2 s when
	// the new weapon has weaponHud.displaySelectOnActivation.
	g_itemList.allowed = false;
	g_itemList.items.clear();
	g_itemList.inHand = -1;
	if (game::GameWorld::PlayerInfo me; world.playerInfo(world.localPlayer(), me) && me.alive && !inVehicle) {
		std::vector<dice::hfe::menu::SelectItem> items;
		for (const auto& [slot, name] : me.carried) {
			items.push_back({slot, dice::hfe::menu::hudSetting(round.templates().find(name), "weaponHud.selectIcon"), true});
		}
		if (g_inHandSlot >= 0 && me.inHandSlot != g_inHandSlot) {
			const world::ObjectTemplate* now = nullptr;
			for (const auto& [slot, name] : me.carried) {
				now = slot == me.inHandSlot ? round.templates().find(name) : now;
			}
			if (g_footItemKey > 0 && g_footItemKey == me.inHandSlot) {
				g_itemSelection.open(items, me.inHandSlot, 0.4f);
				g_itemSelection.flash(me.inHandSlot);
			} else if (dice::hfe::menu::hudNumber(now, "weaponHud.displaySelectOnActivation", 0) != 0) {
				g_itemSelection.open(items, me.inHandSlot, 2.0f);
			}
			g_footItemKey = 0;
		}
		g_inHandSlot = me.inHandSlot;
		g_itemList.allowed = !g_weaponZoomed;
		g_itemList.items = std::move(items);
		g_itemList.inHand = me.inHandSlot;
	} else {
		g_inHandSlot = -1;
		g_footItemKey = 0;
	}
	// In a seat a weapon key's pick opens it (0x004d2a50 -> 0x0079c190): the
	// list is the seat's PIFire group (0x0079c470: the control object's
	// weapon list 0, the soldier's when it has none) by item index, each
	// dimmed when it can't be selected (0x0079c290: slot 0x170), open
	// 0.4 s (+0x20 = 0x3ecccccd) with the key's slot flashing.
	// A joined client runs the same pick for its own control object: the
	// key's PIFire weapon by item index, selectable (isSelectable), not the
	// one our own state has active already (the server's record of our
	// group 0).
	int seatKey = 0;
	if (inVehicle && !world.joined()) {
		seatKey = round.takeSeatItemSelect(world.localPlayer());
	} else if (inVehicle && g_remoteItemKey > 0) {
		game::GameWorld::PlayerInfo me;
		if (world.playerInfo(world.localPlayer(), me) && me.vehicle) {
			const auto seats = round.vehicleSeats(me.vehicle);
			const world::ObjectTemplate* active = world.seatGroupWeapon(0).weapon;
			if (me.seat >= 0 && static_cast<size_t>(me.seat) < seats.size()) {
				for (const game::GameWorld::SeatWeapon& w : seats[static_cast<size_t>(me.seat)].weapons) {
					const world::ObjectTemplate* t = round.templates().find(w.weapon);
					if (w.group == 0 && w.itemIndex == g_remoteItemKey && w.selectable && t && t != active) {
						seatKey = g_remoteItemKey;
					}
				}
			}
		}
	}
	g_remoteItemKey = 0;
	// The seat's weapon-change event (2/0x20, 0x00756b60's case: 0x0079fd30(0))
	// opens the list for 2 s (+0x20 = 2.0, menu state 0xb) when the control
	// object's new active PIFire weapon has weaponHud.displaySelectOnActivation
	// (block +0xd9; the useSelectionIcons half of 0x0079fd30 is skipped by the
	// 0 it is called with).
	static const world::ObjectTemplate* seatActiveWas = nullptr;
	static uint32_t seatVehicleWas = 0;
	static int seatWas = -1;
	bool seatActivation = false;
	// Entering a seat (0x007a3920, from the enter event 0x007a3cb0 for the
	// local player): with the controlled object's
	// vehicleHud.displaySelectionOnEnter (block +0xbd; the artillery guns
	// as_ag / us_ag) the list opens for 2 s (+0x20 = 2.0, menu state 0xb).
	bool seatEntered = false;
	game::GameWorld::PlayerInfo seated;
	if (inVehicle && world.playerInfo(world.localPlayer(), seated) && seated.vehicle) {
		if (seated.vehicle != seatVehicleWas || seated.seat != seatWas) {
			// The vehicle HUD's +0xb4 (0x007a28e0): the last occupied seat's
			// controlled object a parachute that isn't a pod holds it back.
			const auto seats = round.vehicleSeats(seated.vehicle);
			const auto occupants = world.vehicleOccupants(seated.vehicle);
			const world::ObjectTemplate* lastOccupied = nullptr;
			for (size_t i = 0; i < occupants.size(); ++i) {
				game::GameWorld::PlayerInfo who;
				if (occupants[i] >= 0 && world.playerInfo(occupants[i], who)) {
					const world::Object* root = round.objects().find(seated.vehicle);
					lastOccupied = i > 0 && i < seats.size() ? round.templates().find(seats[i].name)
						: root ? root->objectTemplate()
							   : nullptr;
				}
			}
			seatEntered = dice::hfe::menu::displaySelectionOnEnter(controlledTemplate(round, seated), lastOccupied);
		}
		const world::ObjectTemplate* active = world.seatGroupWeapon(0).weapon;
		if (active && active != seatActiveWas && seated.vehicle == seatVehicleWas && seated.seat == seatWas) {
			seatActivation = dice::hfe::menu::hudNumber(active, "weaponHud.displaySelectOnActivation", 0) != 0;
		}
		seatActiveWas = active;
		seatVehicleWas = seated.vehicle;
		seatWas = seated.seat;
	} else {
		seatActiveWas = nullptr;
		seatVehicleWas = 0;
		seatWas = -1;
	}
	// In a seat the wheel's list is the seat's (0x0079c470), only with the
	// seat's vehicleHud.useSelectionIcons; the active PIFire weapon selected.
	if (game::GameWorld::PlayerInfo me; inVehicle && world.playerInfo(world.localPlayer(), me) && me.vehicle) {
		const auto seats = round.vehicleSeats(me.vehicle);
		if (me.seat >= 0 && static_cast<size_t>(me.seat) < seats.size()) {
			const auto& seat = seats[static_cast<size_t>(me.seat)];
			const world::ObjectTemplate* seatTemplate = round.templates().find(seat.name);
			g_itemList.allowed = dice::hfe::menu::hudNumber(seatTemplate, "vehicleHud.useSelectionIcons", 0) != 0;
			g_itemList.items = dice::hfe::menu::seatSelectItems(seat, seatTemplate,
				[&round](const std::string& name) { return round.templates().find(name); }, me.carried);
			g_itemList.inHand = static_cast<int>(dice::hfe::menu::hudNumber(world.seatGroupWeapon(0).weapon, "itemIndex", -1));
		}
	}
	if (seatKey > 0 || seatActivation || seatEntered) {
		game::GameWorld::PlayerInfo me;
		if (world.playerInfo(world.localPlayer(), me) && me.vehicle) {
			const auto seats = round.vehicleSeats(me.vehicle);
			if (me.seat >= 0 && static_cast<size_t>(me.seat) < seats.size()) {
				const auto& seat = seats[static_cast<size_t>(me.seat)];
				const world::ObjectTemplate* seatTemplate = round.templates().find(seat.name);
				const auto items = dice::hfe::menu::seatSelectItems(seat, seatTemplate,
					[&round](const std::string& name) { return round.templates().find(name); }, me.carried);
				if (seatKey > 0) {
					g_itemSelection.open(items, seatKey, 0.4f);
					g_itemSelection.flash(seatKey);
				} else {
					const int index = static_cast<int>(dice::hfe::menu::hudNumber(seatActiveWas, "itemIndex", -1));
					g_itemSelection.open(items, index, 2.0f);
				}
			}
		}
	}
	g_itemSelection.update(hud, dt);
	// Leaving the combat area (0x0044c7b0, alive): the seconds left, ceiled
	// (0x004480f0), as "%d" for #TIME# in HUD_CENTERINFOBOX_timeuntildamage
	// (the layer's +0xa70, SpawnInfoString: the spawn screen's bar) and, while
	// the game runs, HUD_TEXT_MENU_LEAVING_COMBATAREA through the HUD
	// manager's slot 0x144 (0x007c6fd0: PauseMessageActive +0x4d and the
	// layer's DisconnectMessage, +0xa4, HudElementsGameInfo.con's centre
	// line); the playing frame first takes it down (slot 0x148, 0x007c7020).
	// The voice is the radio's (GameWorld's "leaving_the_combat_area").
	{
		const float areaLeft = alive ? world.combatAreaTimeLeft() : -1.0f;
		const int seconds = areaLeft >= 0 ? static_cast<int>(std::ceil(areaLeft)) : -1;
		game::GameWorld::PlayerInfo respawnInfo;
		const int respawnSeconds = !alive && world.playerInfo(world.localPlayer(), respawnInfo)
			? static_cast<int>(std::ceil(std::max(0.0f, respawnInfo.respawnIn))) : -1;
		// The shipped SpawnInfo node is gated separately from its text.
		hud.set("SpawnInfoShow", seconds >= 0 || respawnSeconds > 0 ? 1 : 0);
		{
			const auto text = [](const char* key) { return g_localize ? g_localize(key) : std::string(key); };
			const auto withTime = [&](const char* key, int value) {
				std::string s = text(key);
				const size_t at = s.find("#TIME#");
				return at == std::string::npos ? s : s.replace(at, 6, std::to_string(value));
			};
			hud.set("PauseMessageActive", seconds >= 0 ? 1 : 0);
			hud.setString("DisconnectMessage", seconds >= 0 ? withTime("HUD_TEXT_MENU_LEAVING_COMBATAREA", seconds) : std::string());
		}
		{
			const auto text = [](const char* key) { return g_localize ? g_localize(key) : std::string(key); };
			const auto withTime = [&](const char* key, int value) {
				std::string s = text(key);
				const size_t at = s.find("#TIME#");
				return at == std::string::npos ? s : s.replace(at, 6, std::to_string(value));
			};
			if (seconds >= 0) hud.setString("SpawnInfoString", withTime("HUD_CENTERINFOBOX_timeuntildamage", seconds));
			else if (respawnSeconds >= 0) hud.setString("SpawnInfoString", withTime("HUD_CENTERINFOBOX_timetospawn", respawnSeconds));
			else hud.setString("SpawnInfoString", std::string());
		}
		// The centred killed lines (KilledState, DeathMessage, TimeToSpawnString of HudElementsKilled.con):
		// man down says "critically wounded" (0x007516e5), dead "you were killed" (0x00751a04); the timer is
		// "%s %2d" of HUD_TEXT_MENU_TIME_TO_RESPAWN and the seconds, 0..1000 (0x0044cd25..0x0044cd5d).
		const bool killed = respawnSeconds >= 0
			&& dice::main::killedInfoShown(respawnInfo.manDown, respawnInfo.respawnIn, g_deploymentOpen, g_spawnScreenHidden);
		hud.set("KilledState", killed ? 1 : 0);
		if (killed) {
			const auto text = [](const char* key) { return g_localize ? g_localize(key) : std::string(key); };
			char number[16];
			std::snprintf(number, sizeof number, " %2d", std::min(respawnSeconds, 1000));
			hud.setString("DeathMessage", text(respawnInfo.manDown ? "HUD_TEXT_MENU_YOU_ARE_IN_MAN_DOWN" : "HUD_TEXT_MENU_YOU_WERE_KILLED"));
			hud.setString("TimeToSpawnString", text("HUD_TEXT_MENU_TIME_TO_RESPAWN") + number);
		}
	}
	// The capture bar (HudElementsConquest.con "Controlpoint Info": shown
	// while the soldier is in a capturable point's radius, the point
	// getOccupyingCP picks; the flag's height 0..1 on the "taken" bar as it
	// rises on a neutral point, on the "neutral" bar as an owned flag is
	// lowered; the Titan bars read the same variables, "Retargeting" /
	// "Deactivating"). shortcut: which bar a stalled flag uses is a guess.
	{
		dice::hfe::menu::CaptureBar bar;
		if (alive || inVehicle) {
			physics::Vec3 at {};
			game::GameWorld::PlayerInfo self;
			if (world.playerInfo(world.localPlayer(), self)) {
				at = self.position;
				bar = dice::hfe::menu::captureBar(world.controlPointList(), at);
			}
		}
		hud.set("CPInterfaceEnabled", bar.enabled ? 1 : 0);
		hud.set("PreCPIsNeutralShow", bar.neutralShow ? 1 : 0);
		hud.set("PreCPIsTakenShow", bar.takenShow ? 1 : 0);
		hud.set("TimePreCPIsNeutral", bar.value);
		hud.set("TimePreCPIsTaken", bar.value);
		// The Titan HUD's uplink state (UplinkHud): the original's per-frame clock is the frame time.
		static dice::hfe::menu::UplinkHud uplink;
		static std::chrono::steady_clock::time_point lastUplink = std::chrono::steady_clock::now();
		const auto nowUplink = std::chrono::steady_clock::now();
		const float frameSeconds = std::min(0.1f, std::chrono::duration<float>(nowUplink - lastUplink).count());
		lastUplink = nowUplink;
		uplink.update(frameSeconds, bar.enabled, bar.value);
		hud.set("ShowTimePreUplink", uplink.show ? 1 : 0);
		hud.set("ShowTimePreUplinkLost", uplink.showLost ? 1 : 0);
		hud.set("TimePreUplinkComplete", uplink.complete);
		hud.set("CaptureComplete", uplink.captureComplete ? 1 : 0);
	}
	const int team = world.playerTeam() == 1 ? 1 : 2, enemy = 3 - team;
	float friendlyTickets = world.tickets(team), enemyTickets = world.tickets(enemy);
	if (round.gameMode() == "gpm_ti") {
		std::array<float, 3> titanHealth {};
		std::array<bool, 3> titanKnown {};
		for (const game::GameWorld::TitanInfo& titan : round.titans()) {
			if (titan.team == 1 || titan.team == 2) {
				titanHealth[static_cast<size_t>(titan.team)] = dice::hfe::menu::titanHudHealth(titan);
				titanKnown[static_cast<size_t>(titan.team)] = true;
			}
		}
		if (titanKnown[static_cast<size_t>(team)]) {
			friendlyTickets = titanHealth[static_cast<size_t>(team)];
		}
		if (titanKnown[static_cast<size_t>(enemy)]) {
			enemyTickets = titanHealth[static_cast<size_t>(enemy)];
		}
	}
	hud.setString("FriendlyTicketsString", std::to_string(static_cast<int>(friendlyTickets)));
	hud.setString("EnemyTicketsString", std::to_string(static_cast<int>(enemyTickets)));
	// The ticket bars' values (HudElementsTitans.con: in Titan mode the
	// tickets are the Titan's armour, the shield span 60-100, the hull 0-60).
	hud.set("FriendlyTickets", friendlyTickets);
	hud.set("EnemyTickets", enemyTickets);
	hud.set("MiddleTicketsShow", dice::hfe::menu::middleTicketsShown(friendlyTickets, enemyTickets) ? 1 : 0);
	// PlayerTeam is the HUD manager's +0x2c8 (BF2142.exe 0x00451a10): the local team; the Titan health nodes pick their Titan by it.
	hud.set("PlayerTeam", static_cast<float>(team));
	// The ticket colours the Titan nodes tint by (HUD manager +0x1c4..0x1dc, copied from runtime globals at 0x00449480
	// whose values are not recovered): taken from the Conquest ticket bars' own blue and red, so the Titans are not black.
	// shortcut: inferred colours, not the retail values.
	for (const auto& [name, rgba] : std::vector<std::pair<const char*, std::array<float, 4>>> {
			 {"FriendlyTicket", {0.165f, 0.55f, 1.0f, 1.0f}}, {"EnemyTicket", {0.63f, 0.15f, 0.17f, 1.0f}}}) {
		hud.set(std::string(name) + "Red", rgba[0]);
		hud.set(std::string(name) + "Green", rgba[1]);
		hud.set(std::string(name) + "Blue", rgba[2]);
		hud.set(std::string(name) + "Alpha", rgba[3]);
	}
	if (round.gameMode() == "gpm_ti") {
		// The Titan HUD shows for GameMode 1 or 3 (the conquest one for 0, 2,
		// 4-7); the consoles' lights (*StrategicObject1-4Show: that console
		// destroyed; 0: the Titan gone), the HUD manager's variables of
		// 0x0044f1a0.
		hud.set("GameMode", 1);
		// The silos' row (0x0044c350, each frame): the ten CPCannon%i slots
		// are cleared, then each silo (the objects of class 10 whose
		// ancestor's control point gives an index 1..9) shows in slot
		// index - 1 with its team as the player sees it (1 his, 2 the
		// enemy's, 0 neutral) and the time to its next missile, "%02d:%02d"
		// minutes and seconds (its weapon's whole seconds), or "--:--" when
		// loaded or neutral.
		std::array<bool, 10> siloShown {};
		const auto silos = round.missileSilos();
		for (size_t i = 0; i < silos.size(); ++i) {
			const int index = silos[i].controlPoint > 0 ? silos[i].controlPoint : static_cast<int>(i) + 1;
			if (index < 1 || index > 9) {
				continue;
			}
			const std::string n = std::to_string(index - 1);
			siloShown[static_cast<size_t>(index - 1)] = true;
			const int side = silos[i].team == team ? 1 : silos[i].team != 0 ? 2 : 0;
			hud.set("CPCannon" + n + "Team", static_cast<float>(side));
			const int seconds = silos[i].loaded ? 0 : static_cast<int>(silos[i].reloadLeft);
			char text[16] = "--:--";
			if (seconds > 0 && side != 0) {
				std::snprintf(text, sizeof(text), "%02d:%02d", seconds / 60, seconds % 60);
			}
			hud.setString("CPCannon" + n + "String", text);
		}
		for (size_t i = 0; i < siloShown.size(); ++i) {
			hud.set("CPCannon" + std::to_string(i) + "Show", siloShown[i] ? 1 : 0);
		}
		// The consoles and shields (StrategicObject%iShow, HudMenusVehicles.h's
		// StrategicObjects: 1 from the round's start, cleared by a destroyed
		// part with vehicleHud.sendDestroyedHudEvent at its guiIndex,
		// 0x0044b680 case 0x32; shown for the local team by 0x00448850).
		static float strategicAt = -1;
		if (now < strategicAt) {
			g_strategicObjects.reset();
		}
		strategicAt = now;
		for (const int side : {1, 2}) {
			for (const auto& part : round.titanParts(side)) {
				if (part.wreck) {
					g_strategicObjects.destroyed(side, round.templates().find(part.name));
				}
			}
		}
		g_strategicObjects.update(hud, team);
	}
	if (soldier && !inVehicle) {
		// The HUD's weapon flag from the soldier's slot 0xa4 (0x007a0f80,
		// with the soldier the control object: not in a seat).
		if (!inVehicle) {
			g_hudWeaponFlag = dice::hfe::menu::soldierWeaponFlag(world.soldierJumpHold(), held.weapon);
		}
		if (const world::ObjectTemplate* rifle = held.weapon) {
			const auto& settings = rifle->settings();
			// 0x007a01e0 on foot: the weapon's gui index (0x00657a70) with
			// the sprint test, the HUD's sprint flag the soldier's sprint
			// (0x007a0f80) and its weapon flag: a disableOnSprint weapon's
			// overlay goes while sprinting, and for a second after a jump or
			// 0.6 s after rising from prone.
			float stamina = 1;
			bool sprinting = false;
			world.soldierStamina(stamina, sprinting);
			hud.set("GuiIndex", static_cast<float>(dice::hfe::menu::weaponGuiIndex(rifle, false,
									dice::hfe::menu::guiSprintTest(sprinting, g_hudWeaponFlag))));
			// The crosshair's parts (0x007be770): the spread in pixels is
			// the deviation (degrees) times weaponHud.deviationFactor
			// (default 10); the parts are 20 long, about the 800 x 600
			// centre. The deviation is the weapon's soldier deviation now
			// (GameWorld::weaponDeviation: the turn, speed, stance and
			// firing), its minimum without one.
			const auto number = [&settings](const char* key, float otherwise) {
				const auto it = settings.find(key);
				return it != settings.end() ? static_cast<float>(std::atof(it->second.c_str())) : otherwise;
			};
			// minVisualDeviation pushes the parts out, maxVisualDeviation
			// holds them (menu::crosshairParts).
			const float now = world.joined() ? std::max(g_remoteDeviation, 0.0f) : round.weaponDeviation(round.localPlayer());
			const auto parts = dice::hfe::menu::crosshairParts(now > 0 ? now : number("deviation.minDev", 0), rifle);
			hud.set("CrossRightPos", parts.right);
			hud.set("CrossLeftPos", parts.left);
			hud.set("CrossUpPos", parts.up);
			hud.set("CrossDownPos", parts.down);
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

// The mod's menu music: Mod.desc's <music> (ModInfo +0x8c; bf2142's
// "sound/music/2142MenuMusic.ogg").
std::string menuMusicFile(const io::FileManager& files)
{
	std::vector<uint8_t> data;
	if (!files.readFile("Mod.desc", data)) {
		return std::string();
	}
	const std::string text(data.begin(), data.end());
	const size_t open = text.find("<music>"), close = text.find("</music>");
	if (open == std::string::npos || close == std::string::npos || close < open) {
		return std::string();
	}
	std::string file = text.substr(open + 7, close - open - 7);
	file.erase(0, file.find_first_not_of(" \t\r\n"));
	file.erase(file.find_last_not_of(" \t\r\n") + 1);
	return file;
}

// 0x007fd060 for the menu's two requests: play (0) starts the mod's track
// when nothing plays (0x007fce80), stop (1) fades it out over 5 s. The
// game asks for play when the menu comes up alone (0x004032b0) and for
// stop when a game takes over (0x00403370); the menu's
// Sound.playMenuMusic / stopMenuMusic (SwiffPlayer 0x10059dd0 / 0x10059df0,
// SwiffHost slots 102 / 103: events 0x16 / 0x15, 0x00404590) the same.
void menuMusic(int& state, bool play, sound::SoundManager& sounds, const io::FileManager& files)
{
	if (play && state == 0) {
		const std::string track = menuMusicFile(files);
		if (!track.empty() && sounds.loadMusic(track) && sounds.playMusic()) {
			std::printf("menu music %s\n", track.c_str());
			state = 1;
		}
	} else if (!play && state == 1) {
		sounds.stopMusic(5.0f);
		state = 0;
	}
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

// The same for a joined game: the server's players, its end-of-round
// block (data block 8: each player's kills, deaths and scores) and the
// winner from its EndOfRound event.
dice::hfe::menu::RoundSummary remoteRoundSummary(const Options& options, const net::RemoteWorld& world)
{
	dice::hfe::menu::RoundSummary summary;
	summary.validRound = true;
	summary.mapPath = summary.previousMapPath = options.level;
	summary.gameMode = summary.previousGameMode = options.gameMode;
	summary.mapSize = summary.previousMapSize = options.mapSize;
	summary.serverName = world.serverName();
	summary.roundsLeft = static_cast<int>(world.endOfRound().roundsLeft);
	summary.winningTeam = static_cast<int>(world.endOfRound().winner);
	if (summary.winningTeam == 1 || summary.winningTeam == 2) {
		summary.victories[static_cast<size_t>(summary.winningTeam - 1)] = 1;
	}
	net::RemoteWorld::EndOfRoundData data;
	const bool haveData = world.endOfRoundData(data);
	for (const net::RemotePlayer& player : world.playerList()) {
		dice::hfe::menu::EndOfRoundPlayer row;
		row.name = player.name;
		row.team = player.team;
		row.score = player.score;
		row.kills = player.kills;
		row.deaths = player.deaths;
		row.teamwork = player.teamwork;
		row.squad = player.squad;
		row.squadLeader = player.squadLeader;
		row.commander = player.commander;
		for (const auto& stats : data.players) {
			if (haveData && stats.id == player.id) {
				row.score = stats.score;
				row.kills = stats.kills;
				row.deaths = stats.deaths;
				row.teamwork = stats.teamworkScore;
			}
		}
		if (player.id == world.localPlayer()) {
			summary.localPlayer = static_cast<int>(summary.players.size());
			summary.myTeam = player.team == 1 ? 1 : 2;
			summary.score = row.score;
			summary.kills = row.kills;
			summary.deaths = row.deaths;
		}
		summary.players.push_back(row);
	}
	return summary;
}

// Plays the menu headless: "wait:<seconds>", "click:<x>,<y>" (stage
// pixels), "type:<text>", "call:<clip>.<method>,<args>", separated by
// spaces; stops early once `done`.
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
		} else if (op == "call") {
			// call:<clip path>.<method>[,<arg>...]: a method of a clip, for
			// off-screen checks of what no click reaches offline (numbers as
			// numbers, the rest as text): call:_root.createEmptyMovieClip,v,9999
			// call:v.loadMovie,?titan_tutorial.bik
			std::vector<std::string> parts;
			std::istringstream items(arg);
			for (std::string item; std::getline(items, item, ',');) {
				parts.push_back(item);
			}
			if (parts.empty() || parts[0].rfind('.') == std::string::npos) {
				continue;
			}
			const size_t dot = parts[0].rfind('.');
			const ui::avm1::Value target = menu.evaluate(parts[0].substr(0, dot));
			std::vector<ui::avm1::Value> values;
			for (size_t i = 1; i < parts.size(); ++i) {
				char* end = nullptr;
				const double number = std::strtod(parts[i].c_str(), &end);
				values.push_back(!parts[i].empty() && *end == 0 ? ui::avm1::Value(number) : ui::avm1::Value(parts[i]));
			}
			menu.vm().callMember(target, parts[0].substr(dot + 1), values);
		} else {
			std::fprintf(stderr, "menu script: unknown command %s\n", command.c_str());
		}
	}
}

// Where rebound keys were kept before the profiles had them: Controls.con's
// format, next to the menu's settings (read when a profile has none).
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
	input::GameControllers gameControllers; // IDFGameController_0..7
	ui::Player menu;
	std::unique_ptr<dice::hfe::menu::MenuBindings> menuBindings;
	bool menuQuit = false;
	std::string restartModPath; // Mod.activateMod: restart with "+modPath <path> +multi 1"
	std::optional<dice::hfe::menu::GameSetup> startedGame;
	bool disconnect = false; // Logic.disconnectGame: back to the menu
	std::vector<dice::hfe::menu::MenuMap> playlist;
	size_t current = 0;
	int roundsPerMap = 1;
	int roundsOnMap = 0;
	bool nextMap = false; // the round ended: the playlist goes on
	bool joinSpawn = false; // --play on a server: ask to spawn once in the game
	bool joinSpectate = false; // --spectate on a server: Logic.readyToPlay(true) once in the game
	bool spectating = false;   // the server said our player spectates (JoinAsSpectator back)
	int joinTeam = 0;       // --play 1|2 on a server: the team asked for first
	// A game on a server (--connect, or the menu's join): the connection
	// and the replicated world, for as long as it lasts.
	// --net-log / --net-strict / --net-capture, for every client: before
	// the client, which still sends its disconnect as it goes.
	std::unique_ptr<net::NetDiagnostics> netDiagnostics;
	std::unique_ptr<net::GameClient> client;
	// A client whose server ended the connection mid-game: kept until the
	// game's frame is done with it (the joined HUD's world, the name tags
	// and the menus read its RemoteWorld until runGame returns).
	std::unique_ptr<net::GameClient> endedClient;
	bool joinRequested = false; // the menu joined a server
	bool levelChange = false;   // the server changed maps: load its new level
	// --host: the server, kept from map to map so the clients stay
	// connected through a map change (HostedGame::changeLevel).
	std::unique_ptr<host::HostedGame> hosted;
	// --host: the server's configuration (sv.*, maplist.*, the bans) and
	// its admin script (the remote console), kept from map to map
	// (ServerAdmin.h, AdminScript.h); the installed maps the map list names.
	std::unique_ptr<host::ServerAdmin> admin;
#if BF2142_HAS_PYTHON
	host::AdminScript adminScript;
#endif
	std::vector<dice::hfe::menu::MenuMap> installedMaps;
	long serverTicksLeft = -1; // --host --dedicated --simulate N: the ticks left over every map (-1: no end)
	// The start-up movies Init.con queues (bf2Engine.playMovie), played
	// once before the menu (Movie/MovieQueue.h).
	dice::hfe::movie::MovieQueue movies;
	bool moviesQueued = false;
	// GeneralSettings, AudioSettings, VideoSettings, GlobalSettings,
	// LocalProfile, SettingsManager (SettingsObjects.h): read from the
	// profile's files once, kept for the next maps.
	dice::hfe::menu::SettingsObjects settings;
	bool settingsLoaded = false;
	// The local profiles (Profiles/0001...), each with its settings and
	// controls (LocalProfileManager).
	dice::hfe::menu::LocalProfileManager profiles {settings};
	// SwiffHost's and MenuTeamManager's (credits.con, gameModes.con,
	// menuTeams.con), run once when the menu first starts.
	dice::hfe::menu::MenuHost menuHost;
	bool menuScriptsRun = false;
	// The menu's music object (0x007fd030, the game's +0xf4): 0 silent,
	// 1 the mod's menu track playing.
	int menuMusic = 0;
	// Voice chat (Voip/VoiceChat.h): the menu's settings and mic test, the
	// joined server's voice; updated on its own thread.
	std::unique_ptr<dice::hfe::voip::VoiceChat> voice;
#if BF2142_HAS_RENDERER
	std::unique_ptr<Window> window; // opened by the first view, kept for the next maps
	AudioOut menuVideoAudio; // a menu clip's movie's own sound (RendDX9's Bink)
#endif
};

int runGame(Options& options, Session& session);

// A dedicated server runs until Ctrl+C or a console "quit".
std::atomic<bool> g_stopServer {false};
void stopServer(int) { g_stopServer = true; }

// --rcon: a remote console client (admin/default.py's protocol).
int runRemoteConsole(const Options& options);
// --movie: one movie, in a window or (with --screenshot) one frame of it.
int runMovieCommand(const Options& options);
int runOpenSpyEmulator(const Options& options);
// --host: the server's settings and map list, and its playlist from them.
bool setupServer(Options& options, Session& session);
// A map list entry as a level to load: the level folder's own name and a
// size its mode has (none for a level that isn't installed).
std::optional<dice::hfe::menu::MenuMap> serverMap(const Session& session, const net::MapListEntry& entry);

} // namespace

int main(int argc, char** argv)
{
	if (std::getenv("BF2142_INPUT_TRACE")) std::setvbuf(stdout, nullptr, _IOLBF, 0);
	Options options;
	if (!parseOptions(argc, argv, options)) {
		return 2;
	}
	if (!options.rcon.empty()) {
		return runRemoteConsole(options);
	}
	if (!options.movie.empty()) {
		return runMovieCommand(options);
	}
	if (options.host) {
		std::setvbuf(stdout, nullptr, _IOLBF, 0); // a server's log as it happens
	}
	// Where the online services are and who logs in (Net/OnlineConfig.h):
	// this machine's emulators unless --online.
	net::setOnlineConfig(net::loadOnlineConfig(options.online));
	if (options.online) {
		std::printf("online: the live services (%s, %s, %s)\n", net::onlineConfig().endpoints.fesl.text().c_str(),
			net::onlineConfig().endpoints.gpcm.text().c_str(), net::onlineConfig().endpoints.master.text().c_str());
	}
	if (options.openspyEmulator) {
		return runOpenSpyEmulator(options);
	}
	// The account service: a greeting, or the whole login (FESL, the
	// soldier, GameSpy Presence) whose profile id, name checksum and login
	// ticket go to the game server.
	uint32_t profileId = 0xffffffff;
	static net::OnlineSession onlineSession; // open while the game runs
	if (options.fesl) {
		net::FeslClient fesl;
		std::string error;
		const net::ServiceEndpoint endpoint = net::onlineConfig().endpoints.fesl;
		if (!fesl.connectConfigured(10, &error)) {
			std::printf("%s: %s\n", endpoint.text().c_str(), error.c_str());
			return 1;
		}
		const auto hello = fesl.hello();
		if (!hello) {
			std::printf("%s: no greeting\n", endpoint.text().c_str());
			return 1;
		}
		std::printf("%s greeted us:\n", endpoint.text().c_str());
		for (const auto& [key, value] : hello->fields) {
			std::printf("  %s = %s\n", key.c_str(), net::feslUnquote(value).c_str());
		}
		fesl.goodbye();
		if (options.connect.empty() && options.probe.empty() && options.login.empty()) {
			return 0;
		}
	}
	if (!options.login.empty()) {
		net::OnlineCredentials credentials = net::onlineConfig().credentials;
		if (options.login != "config") {
			const size_t colon = options.login.find(':');
			const size_t at = options.login.rfind('@');
			credentials.account = options.login.substr(0, colon);
			credentials.password = colon == std::string::npos
				? std::string()
				: options.login.substr(colon + 1, (at == std::string::npos || at < colon ? options.login.size() : at) - colon - 1);
			credentials.soldier = at == std::string::npos || at < colon ? std::string() : options.login.substr(at + 1);
		}
		if (!credentials.complete()) {
			std::printf("login: no account and password (BF2142_ACCOUNT / BF2142_PASSWORD, or bf2142-online.cfg: see README)\n");
			return 1;
		}
		onlineSession.onLog = [](const std::string& line) { std::printf("online: %s\n", line.c_str()); };
		std::string error;
		const auto account = onlineSession.loginAccount(credentials.account, credentials.password, &error);
		if (!account) {
			std::printf("login failed: %s\n", error.c_str());
			return 1;
		}
		std::printf("logged in as %s (user %ld); soldiers:", account->displayName.c_str(), account->userId);
		for (const std::string& s : account->soldiers) {
			std::printf(" %s", s.c_str());
		}
		std::printf("\n");
		if (!onlineSession.loginSoldier(credentials.soldier, &error)) {
			std::printf("login failed: %s\n", error.c_str());
			return 1;
		}
		const net::OnlineIdentity identity = onlineSession.identity();
		std::printf("playing as %s (profile %ld, name checksum %08x, GP login ticket %s)\n", identity.soldier.c_str(),
			identity.profileId, identity.nameChecksum, identity.loginTicket.empty() ? "missing" : "received");
		profileId = static_cast<uint32_t>(identity.profileId);
		options.onlineSoldier = identity.soldier;
		options.onlineTicket = identity.loginTicket;
		options.onlineNameChecksum = identity.nameChecksum;
		if (options.playerName.empty()) {
			options.playerName = identity.soldier;
		}
		if (options.connect.empty() && options.probe.empty()) {
			onlineSession.logout();
			return 0;
		}
		onlineSession.keepAlive();
	}
	options.profileId = profileId;
	if (!options.probe.empty()) {
		const size_t colon = options.probe.rfind(':');
		const std::string host = options.probe.substr(0, colon);
		const uint16_t port = colon == std::string::npos ? net::kGamePort
														  : static_cast<uint16_t>(std::atoi(options.probe.c_str() + colon + 1));
		if (!net::hostAllowed(host)) {
			std::printf("%s\n", net::hostRefusal(host).c_str());
			return 1;
		}
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
							const std::string key = net::onlineConfig().credentials.cdKey;
							const std::string response = net::cdKeyResponse(key.empty() ? "XXXX-XXXX-XXXX-XXXX" : key, event.challenge, random);
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
		const net::ServiceEndpoint master = net::onlineConfig().endpoints.master;
		const std::string host = master.text();
		std::string error;
		const auto list = net::fetchServerList(master.host, "", fields, 15, &error, master.port);
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
	// --maps: a host's playlist (level[:mode[:size]], comma separated).
	for (size_t at = 0; !options.maps.empty() && at <= options.maps.size();) {
		const size_t end = std::min(options.maps.find(',', at), options.maps.size());
		const std::string entry = options.maps.substr(at, end - at);
		at = end + 1;
		if (entry.empty()) {
			continue;
		}
		dice::hfe::menu::MenuMap map;
		const size_t first = entry.find(':');
		map.level = entry.substr(0, first);
		map.gameMode = options.gameMode;
		map.size = options.mapSize;
		if (first != std::string::npos) {
			const size_t second = entry.find(':', first + 1);
			map.gameMode = entry.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
			if (second != std::string::npos) {
				map.size = std::atoi(entry.c_str() + second + 1);
			}
		}
		session.playlist.push_back(map);
	}
	if (options.host && !setupServer(options, session)) {
		return 1;
	}
	// +level in a window: a map list of that one map, so the end of round's JOIN GAME (and sv.endDelay)
	// starts it again, as a game the player starts does (getNumRoundsLeft 999, 0x004d3450). Not for
	// headless or screenshot runs, which end with the round.
	if (session.playlist.empty() && !options.level.empty() && options.view && options.connect.empty() && options.simulate == 0 &&
		options.screenshot.empty()) {
		session.playlist.push_back({options.level, options.gameMode, options.mapSize});
	}
	if (!session.playlist.empty()) {
		options.level = session.playlist.front().level;
		options.gameMode = session.playlist.front().gameMode;
		options.mapSize = session.playlist.front().size;
	}
	for (;;) {
		const int result = runGame(options, session);
		if (!session.restartModPath.empty()) {
			// Mod.activateMod's restart (0x00403fa0): the same command line
			// with "+modPath <path> +multi 1" in place of its own.
			std::vector<std::string> args;
			for (int i = 0; i < argc; ++i) {
				const std::string arg = argv[i];
				if ((arg == "+modPath" || arg == "+multi") && i + 1 < argc) {
					++i;
					continue;
				}
				args.push_back(arg);
			}
			args.insert(args.end(), {"+modPath", session.restartModPath, "+multi", "1"});
			std::printf("restarting with +modPath %s\n", session.restartModPath.c_str());
#if BF2142_HAS_RENDERER
			session.window.reset();
#endif
			platform::restartProcess(argv[0], args);
			std::fprintf(stderr, "cannot restart %s\n", argv[0]);
			return 1;
		}
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
			session.endedClient.reset();
			session.joinRequested = false;
			options.connect.clear();
			if (options.menu.empty()) {
				return result;
			}
			session.startedGame.reset();
			if (session.menuBindings) {
				dice::hfe::menu::updateMenuGameState(session.menu, *session.menuBindings, false);
			}
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
		if (!session.nextMap || session.playlist.empty() || g_stopServer) {
			return result;
		}
		session.nextMap = false;
		if (session.admin) {
			// The server's map list (ServerAdmin::nextRound).
			const net::MapListEntry next = session.admin->nextRound();
			const auto map = serverMap(session, next);
			if (!map) {
				std::fprintf(stderr, "next map %s (%s) is not installed\n", next.level.c_str(), next.gameMode.c_str());
				return 1;
			}
			options.level = map->level;
			options.gameMode = map->gameMode;
			options.mapSize = map->size;
			std::printf("next map: %s (%s, %d), map list entry %d of %d\n", options.level.c_str(), options.gameMode.c_str(),
				options.mapSize, session.admin->maps.current() + 1, session.admin->maps.count());
			continue;
		}
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

int runRemoteConsole(const Options& options)
{
	// --rcon host[:port] [--rcon-password p] [--rcon-command "..."]...:
	// the greeting, the seeded login, then each command through "exec".
	const size_t colon = options.rcon.find(':');
	const std::string address = options.rcon.substr(0, colon);
	const uint16_t port = colon == std::string::npos ? 4711 : static_cast<uint16_t>(std::atoi(options.rcon.c_str() + colon + 1));
	net::RemoteConsoleClient rcon;
	std::string error;
	if (!rcon.connect(address, port, 5, &error)) {
		std::fprintf(stderr, "%s:%u: %s\n", address.c_str(), port, error.c_str());
		return 1;
	}
	std::printf("%s\n", rcon.greeting().c_str());
	std::string reply;
	if (!rcon.login(options.rconPassword, &reply)) {
		std::fprintf(stderr, "%s", reply.empty() ? "no answer to the login\n" : reply.c_str());
		return 1;
	}
	std::printf("%s", reply.c_str());
	for (const std::string& command : options.rconCommands) {
		if (!rcon.exec(command, reply)) {
			std::fprintf(stderr, "%s: no answer\n", command.c_str());
			return 1;
		}
		std::printf("> %s\n%s", command.c_str(), reply.c_str());
	}
	return 0;
}

// A movie's file: a queued name ("Movies\\EA.bik") under the mod, or a
// bare name ("Intro") as Movies/<name>.bik; matched without case.
std::string moviePath(const Options& options, std::string file)
{
	std::replace(file.begin(), file.end(), '\\', '/');
	if (file.find('/') == std::string::npos && file.find('.') == std::string::npos) {
		file = "Movies/" + file + ".bik";
	}
	const std::filesystem::path given(file);
	if (given.is_absolute() || std::filesystem::exists(given)) {
		return file;
	}
	return host::findFileNoCase(options.gameDir + "/" + options.modPath + "/" + file);
}

#if BF2142_HAS_RENDERER
// Plays the queued movies in the window, as the main loop does (0x0040bd90,
// 0x00408680): each in turn, one that won't open dropped, to its end or
// to Esc when its mode lets it. False when the window was closed.
bool playMovies(Window& window, dice::hfe::movie::MovieQueue& queue, const Options& options)
{
	while (!queue.empty()) {
		const dice::hfe::movie::QueuedMovie movie = queue.takeNext();
		dice::hfe::movie::MovieDecoder decoder;
		std::string error;
		if (!decoder.open(moviePath(options, movie.file), &error)) {
			std::printf("movie %s: %s\n", movie.file.c_str(), error.c_str());
			continue;
		}
		std::printf("movie %s: %dx%d, %.1f s%s\n", movie.file.c_str(), decoder.width(), decoder.height(), decoder.duration(),
			movie.skippable() ? " (Esc skips)" : "");
		Window::MovieScreen screen(window);
		dice::hfe::movie::VideoFrame shown, next;
		bool more = decoder.nextFrame(next);
		const auto start = std::chrono::steady_clock::now();
		while (more || !shown.rgba.empty()) {
			const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			while (more && next.time <= now) {
				std::swap(shown, next);
				more = decoder.nextFrame(next);
			}
			screen.queueAudio(decoder.takeAudio(), decoder.sampleRate());
			if (!screen.frame(shown.rgba.empty() ? nullptr : shown.rgba.data(), shown.width, shown.height)) {
				return false;
			}
			if (screen.escape() && movie.skippable()) {
				break;
			}
			if (!more && screen.audioQueued() <= 0.0) {
				break; // the picture and the sound are over
			}
		}
	}
	return true;
}
#endif

// --openspy-emulator: OpenSpy's services on 127.0.0.1 (Net/OpenSpyEmulator.h)
// at their usual ports, until Ctrl+C (or --simulate N seconds).
int runOpenSpyEmulator(const Options& options)
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	net::OpenSpyEmulatorOptions e;
	const net::OnlineEndpoints ports = net::localEndpoints();
	e.feslPort = ports.fesl.port;
	e.gpcmPort = ports.gpcm.port;
	e.masterPort = ports.master.port;
	e.stellaPort = ports.stella.port;
	e.cdkeyPort = ports.cdkey.port;
	long id = 1001;
	std::vector<std::string> accounts = options.emulatorAccounts;
	if (accounts.empty()) {
		const net::OnlineCredentials& c = net::onlineConfig().credentials;
		accounts.push_back(c.complete() ? c.account + ":" + c.password + "@" + (c.soldier.empty() ? c.account : c.soldier)
										: "tester:secret@Tester");
	}
	for (const std::string& spec : accounts) {
		e.accounts.push_back(net::OpenSpyEmulator::account(spec, id, id * 1000));
		++id;
	}
	e.cdKeys = options.emulatorCdKeys;
	std::vector<std::string> servers = options.emulatorServers;
	if (servers.empty()) {
		servers.push_back("127.0.0.1:17567:29900");
	}
	for (const std::string& spec : servers) {
		net::ServerEntry server;
		const size_t a = spec.find(':');
		const size_t b = a == std::string::npos ? std::string::npos : spec.find(':', a + 1);
		server.ip = spec.substr(0, a);
		const std::string gamePort = a == std::string::npos ? "17567" : spec.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
		server.port = static_cast<uint16_t>(b == std::string::npos ? 29900 : std::atoi(spec.c_str() + b + 1));
		server.fields = {{"hostname", "local test server"}, {"gamever", "1.51"}, {"mapname", ""}, {"gametype", "gpm_cq"},
			{"numplayers", "0"}, {"maxplayers", "32"}, {"password", "0"}, {"hostport", gamePort}};
		e.servers.push_back(server);
	}
	net::OpenSpyEmulator emulator;
	emulator.onLog = [](const std::string& line) { std::printf("emulator: %s\n", line.c_str()); };
	std::string error;
	if (!emulator.start(e, &error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	const net::OnlineEndpoints at = emulator.endpoints();
	std::printf("OpenSpy emulator on 127.0.0.1: FESL %u (plain TCP), GP %u, master %u, stats %u (HTTP), CD key %u (UDP)\n",
		at.fesl.port, at.gpcm.port, at.master.port, at.stella.port, at.cdkey.port);
	for (const net::EmulatorAccount& a : e.accounts) {
		std::printf("  account %s, soldiers:", a.name.c_str());
		for (const net::EmulatorSoldier& s : a.soldiers) {
			std::printf(" %s (profile %ld)", s.nick.c_str(), s.profileId);
		}
		std::printf("\n");
	}
	std::fflush(stdout);
	const auto started = std::chrono::steady_clock::now();
	while (options.simulate <= 0 || std::chrono::steady_clock::now() - started < std::chrono::seconds(options.simulate)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	emulator.stop();
	return 0;
}

int runMovieCommand(const Options& options)
{
	// --movie <file> [--screenshot out.png --movie-seconds t]
	dice::hfe::movie::MovieDecoder decoder;
	std::string error;
	const std::string path = moviePath(options, options.movie);
	if (!decoder.open(path, &error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	std::printf("%s: %dx%d, %.2f fps, %.1f s, %s\n", path.c_str(), decoder.width(), decoder.height(), decoder.frameRate(),
		decoder.duration(), decoder.hasAudio() ? (std::to_string(decoder.sampleRate()) + " Hz stereo").c_str() : "no sound");
#if BF2142_HAS_RENDERER
	if (!options.screenshot.empty()) {
		dice::hfe::movie::VideoFrame frame;
		size_t samples = 0;
		int frames = 0;
		while (decoder.nextFrame(frame) && frame.time + 1e-6 < options.movieSeconds) {
			samples += decoder.takeAudio().size() / 2;
			++frames;
		}
		std::printf("frame %d at %.2f s (%zu sound samples before it)\n", frames, frame.time, samples);
		return saveMovieFrame(frame.rgba.data(), frame.width, frame.height, 1280, 720, options.screenshot) ? 0 : 1;
	}
	auto window = Window::open(1280, 720);
	if (!window) {
		return 1;
	}
	window->setScript(options.inputScript);
	dice::hfe::movie::MovieQueue queue;
	queue.playMovie(options.movie, 0);
	decoder.close();
	return playMovies(*window, queue, options) ? 0 : 0;
#else
	std::fprintf(stderr, "built without the renderer: nothing to show the movie in\n");
	return 1;
#endif
}

std::optional<dice::hfe::menu::MenuMap> serverMap(const Session& session, const net::MapListEntry& entry)
{
	const auto same = [](const std::string& a, const std::string& b) {
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
			return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
		});
	};
	std::optional<dice::hfe::menu::MenuMap> found;
	for (const dice::hfe::menu::MenuMap& map : session.installedMaps) {
		if (!same(map.level, entry.level) || !same(map.gameMode, entry.gameMode)) {
			continue;
		}
		if (!found || map.size == entry.size) {
			found = map; // the size asked for, else the first the mode has
		}
	}
	return found;
}

bool setupServer(Options& options, Session& session)
{
	// The files (0x00409950): +config / +mapList, else the mod's
	// Settings/ServerSettings.con and maplist.con; then the command line's
	// choices over them.
	auto admin = std::make_unique<host::ServerAdmin>();
	admin->settings.configFile = options.configFile;
	admin->maps.configFile = options.mapListFile;
	admin->setDefaultFiles(options.gameDir + "/" + options.modPath, options.overlayPath);
	session.installedMaps = scanMenuMaps(options);
	const Session* installed = &session;
	admin->maps.validate = [installed](const net::MapListEntry& entry, std::string& error) {
		if (serverMap(*installed, entry)) {
			return true;
		}
		error = "map not found: " + entry.level + " gameMode: " + entry.gameMode;
		return false;
	};
	Console console;
	admin->registerConsole(console);
	const bool settings = admin->loadSettings(console);
	std::printf("server settings %s%s\n", admin->settings.configFile.c_str(), settings ? "" : " (not found: the defaults)");
	admin->loadBanList(console);
	if (!session.playlist.empty() || !options.level.empty()) {
		// --maps or +level: those maps, not the map list file's.
		admin->maps.clear();
		if (session.playlist.empty()) {
			session.playlist.push_back({options.level, options.gameMode, options.mapSize});
		}
		for (const dice::hfe::menu::MenuMap& map : session.playlist) {
			std::string error;
			if (!admin->maps.append({map.level, map.gameMode, map.size}, &error)) {
				std::fprintf(stderr, "could not add to maplist. %s\n", error.c_str());
			}
		}
	} else if (admin->loadMapList(console)) {
		std::printf("map list %s: %d maps\n", admin->maps.configFile.c_str(), admin->maps.count());
	}
	if (admin->maps.count() == 0) {
		std::fprintf(stderr, "no maps to host (+level, --maps or %s)\n", admin->maps.configFile.c_str());
		return false;
	}
	net::ServerSettings& sv = admin->settings;
	if (options.hostPortGiven) {
		sv.serverPort = options.hostPort;
	}
	if (options.queryPortGiven) {
		sv.gameSpyPort = options.queryPort;
	}
	if (options.timeLimitGiven) {
		sv.timeLimit = options.timeLimit;
	}
	options.hostPort = static_cast<uint16_t>(sv.serverPort);
	options.queryPort = static_cast<uint16_t>(sv.gameSpyPort);
	options.timeLimit = sv.timeLimit;
	options.ticketRatio = sv.ticketRatio;
	options.scoreLimit = sv.scoreLimit;
	options.maxPlayers = sv.maxPlayers;
	if (options.botSkill < 0) {
		options.botSkill = sv.botSkill;
	}
	session.roundsPerMap = std::max(sv.roundsPerMap, 1);
	session.playlist.clear();
	for (const net::MapListEntry& entry : admin->maps.entries()) {
		if (const auto map = serverMap(session, entry)) {
			session.playlist.push_back(*map);
		}
	}
	if (session.playlist.empty()) {
		std::fprintf(stderr, "none of the map list's maps is installed\n");
		return false;
	}
	if (options.dedicated) {
		if (options.simulate > 0) {
			session.serverTicksLeft = static_cast<long>(options.simulate) * 30;
		}
		std::signal(SIGINT, stopServer);
		std::signal(SIGTERM, stopServer);
	}
	std::printf("%s server \"%s\": %d maps, %d rounds a map, port %d, query port %d, admin script %s\n",
		options.dedicated ? "dedicated" : "hosted", sv.serverName.c_str(), admin->maps.count(), session.roundsPerMap,
		sv.serverPort, sv.gameSpyPort, sv.adminScript.empty() ? "none" : sv.adminScript.c_str());
	session.admin = std::move(admin);
	return true;
}

// One level's round (after the menu, when it runs first). Every manager
// lives here, so the next map starts from nothing.
int runGame(Options& options, Session& session)
{
	g_initialDeploymentPending = false;
	io::FileManager files;
	files.setRootPath(options.gameDir);
	files.addSearchPath(options.modPath);
	if (options.modPath != "mods/bf2142") {
		files.addSearchPath("mods/bf2142"); // loose base-game files not replaced by the mod
	}

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
	scene::RainSettings rain;
	scene::registerRainConsoleMethods(console, rain);
	scene::RendererSettings rendererSettings;
	scene::registerRendererConsoleMethods(console, rendererSettings);
	scene::EnhancedSettings enhancedSettings;
	enhancedSettings.enabled = options.enhanced;
	scene::registerEnhancedConsoleMethods(console, enhancedSettings);
	// The launcher's Enhanced options, before the audio, the renderer and the
	// bots read them (--console runs only once a round has started).
	for (const std::string& line : options.enhancedSettings) {
		std::string output;
		console.execute(line, output);
	}
	// The post-production, the dynamic shadows and the hemi maps (RendDX9).
	scene::PostProductionSettings postSettings;
	scene::registerPostProductionConsoleMethods(console, postSettings);
	scene::ShadowSettings shadowSettings;
	scene::registerShadowConsoleMethods(console, shadowSettings);
	scene::HemiMapSettings hemiSettings;
	scene::registerHemiMapConsoleMethods(console, hemiSettings);
	// The particle systems' manager, the wind, the video quality levels and
	// the cold breath (scene/EffectSettings.h).
	scene::ParticleSystemSettings particleSettings;
	scene::registerParticleSystemConsoleMethods(console, particleSettings);
	scene::WindSettings windSettings;
	scene::registerWindConsoleMethods(console, windSettings);
	scene::QualitySettings qualitySettings;
	scene::registerQualityConsoleMethods(console, qualitySettings);
	scene::ColdBreathSettings coldBreath;
	scene::registerColdBreathConsoleMethods(console, coldBreath);
	// The overgrowth's rows (Overgrowth.con; scene/OvergrowthSettings.h).
	scene::OvergrowthSettings overgrowthSettings;
	scene::registerOvergrowthConsoleMethods(console, overgrowthSettings);

	// Sound, as 0x007fcd50: the engine from getSoundEngine() reading files
	// through the virtual file system, wrapped by the game's sound manager.
	sound::VfsSoundFileSystem soundFiles(files);
	sound::ISoundEngine* soundEngine = options.sound ? sound::getSoundEngine() : nullptr;
	if (soundEngine) {
		soundEngine->setFileSystem(&soundFiles);
		sound::setEnhancedAudio(enhancedSettings.enabled); // HRTF and filters: read when the provider opens
		if (soundEngine->initialize(2, "0", 32) != 0) {
			std::printf("sound: engine initialisation failed, continuing without sound\n");
			sound::shutdownSoundEngine();
			soundEngine = nullptr;
		}
	}
	sound::SoundManager sounds(soundEngine, templates);
	sounds.registerTemplateType();
	sound::registerSoundConsoleMethods(console, sounds);
	// The game logic's voice messages (0x004cdab0: made once the level has
	// set the teams' languages) and the objects' sounds (GameAudio.h).
	sound::VoiceMessages voices(sounds);
	sound::registerVoiceMessageConsoleMethods(console, voices);
	sound::GameSounds gameSounds(sounds);

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
	// The vehicles' warnings, the help pop-ups and their voices (HudAlerts.h).
	dice::hfe::menu::registerWarningHudMethods(console, templates);
	dice::hfe::menu::WarningHud warningHud;
	dice::hfe::menu::HelpPopups helpPopups;
	helpPopups.bind(console);
	g_itemSelection.bind(console);
	bindMethod(console, "IngameHelp.showIngameHelp", ConsoleAccess::Anyone,
		[&helpPopups, &hudManager](std::string text, float seconds) { helpPopups.showText(hudManager, text, seconds); });
	g_tagIcons.bind(console);
	g_tagIcons.textureSize = [&files](const std::string& texture, int& width, int& height) {
		std::vector<uint8_t> data;
		if (!files.readFile(texture, data) || data.size() < 20) {
			return false;
		}
		height = static_cast<int>(data[12] | data[13] << 8 | data[14] << 16 | data[15] << 24);
		width = static_cast<int>(data[16] | data[17] << 8 | data[18] << 16 | data[19] << 24);
		return true;
	};
	g_hudSound = [&sounds](const std::string& sound) { sounds.playSound(sound); };
	// The warning tones through the sound layer (by sound name), the help
	// voices through its voice messages (VoiceMessages::playHelp).
	warningHud.setSoundHook([&sounds](const std::string& sound, bool start) {
		start ? sounds.playSound(sound) : sounds.stopSound(sound);
	});
	helpPopups.setVoiceHook([&voices](const std::string& key) { voices.playHelp(key); });
	// A help key shown goes on GeneralSettings' played list (saved with
	// General.con; the settings object chains back to helpPopups).
	helpPopups.onPlayed = [&console, &session](const std::string& key) {
		std::string output;
		console.execute("GeneralSettings.setPlayedVOHelp \"" + key + "\"", output);
		session.settings.saveGeneral(); // remembered across launches
	};
	// The player's controls (Settings/Controls.con builds the maps once;
	// the controls page's changes stay for the next maps).
	input::ControlMaps& controls = session.controls;
	input::registerControlMapConsoleMethods(console, controls);
	session.menuHost.scroller = &session.menu.scroller();
	dice::hfe::menu::registerMenuHostConsole(console, session.menuHost);
	// Localized text (0x00405360: Localization/strings.csv in the
	// registry's Language, here --language, kept as GSLanguage; English
	// when the file has no such column; the game reads no other file).
	// The fonts follow it (hud::language: Fonts/<language>/).
	io::Localization localization;
	{
		std::vector<uint8_t> data;
		if (files.readFile("Localization/strings.csv", data)) {
			std::string language = options.language;
			if (!localization.load(data, language)) {
				language = "English";
				localization.load(data, language);
			}
			hud::setLanguage(language);
		}
	}
	// The texts' "#PISprint#" and "#GIYes#" as the keys bound to them
	// (0x00752b70).
	const auto localizeWithKeys = [&localization, &controls](const std::string& id) {
		std::string text = localization.text(id);
		if (text.find("#PI") != std::string::npos || text.find("#GI") != std::string::npos) {
			text = controls.withKeyNames(text, [&localization](const std::string& key) { return localization.text(key); });
		}
		return text;
	};
	hudManager.setLocalizer(localizeWithKeys);
	g_localize = localizeWithKeys;
	helpPopups.setLocalizer(localizeWithKeys);
	setupScoreboard(hudManager);
	// The comm roses, filled by the HUD's Communication scripts (HudMenusComm.h).
	dice::hfe::menu::CommRoses commRoses(hudManager);
	commRoses.setLocalizer([&localization](const std::string& id) { return localization.text(id); });
	commRoses.bind(console, [&console](const std::string& line) {
		std::string output;
		console.execute(line, output);
	});

	game::GameLogic gameLogic;
	game::registerGameLogicConsoleMethods(console, gameLogic);
	// The level's combat areas (CombatArea.*): the Titans keep inside them.
	game::CombatAreas combatAreas;
	dice::hfe::menu::CombatAreaLayer combatAreaLayer; // the minimap's (HudCombatArea.h)
	game::registerCombatAreaConsoleMethods(console, combatAreas);
	gameLogic.mapSize = options.mapSize;

	// The bots' settings (AI/AIBehaviours.ai, AI/AIDefault.ai).
	game::BotSettings botSettings;
	// The AI's random numbers, made at its start before the scripts
	// (0x10001740 -> 0x100309a0): seeded from the clock, but for
	// --simulate's measured rounds (Bots.h, botRandomSeed).
	botSettings.random = ai::Random::seeded(game::botRandomSeed(options.simulate <= 0));
	botSettings.enhanced = &enhancedSettings.enabled;
	botSettings.botRecovery = &enhancedSettings.botRecovery;
	botSettings.cornerSteer = &enhancedSettings.cornerSteer;
	game::registerBotConsoleMethods(console, botSettings);
	// NetworkableInfo.* (Objects/Common/Networkables.con): the ghosts' infos.
	host::registerNetworkableInfoConsoleMethods(console);
	// Ranks (rankSettings.setRank; Settings/RankSettings.con, 0x0043ac80).
	stats::registerRankConsoleMethods(console);
	// The kits' unlock properties the unlock ids come from (also without --api).
	world::registerUntypedTemplateProperties(console, templates,
		{"unlockIndex", "unlockLevel", "unlockBranch", "detonation.useMMOnEndEffect"});

	anim::AnimationSystems animationSystems(
		[&files](const std::string& path, std::vector<uint8_t>& data) { return files.readFile(path, data); });
	anim::registerAnimationConsoleMethods(console, animationSystems);

	// bf2Engine.playMovie (0x004082a0): Init.con queues the start-up
	// movies, the first time it runs.
	session.movies.dedicated = options.dedicated;
	session.movies.northAmerica = dice::hfe::movie::MovieQueue::userInNorthAmerica();
	bindMethod(console, "bf2Engine.playMovie", ConsoleAccess::Anyone, [&session](std::string file, int mode) {
		if (!session.moviesQueued) {
			session.movies.playMovie(file, mode);
		}
	});
	// --host: sv.*, maplist.*, admin.* on the server's configuration
	// (ServerAdmin.h); "quit" ends a dedicated server.
	if (session.admin) {
		session.admin->registerConsole(console);
		if (options.dedicated) {
			console.setExitHandler([] { g_stopServer = true; });
		}
	}
	// The settings and profile objects (after the systems that bind some of
	// their methods themselves: help pop-ups, bloom, shadows).
	session.settings.registerConsole(console);
	session.settings.onChange = [&hudManager, &session](const std::string& key) {
		// GeneralSettings.setCrosshairColor r g b a (0..255): the HUD's
		// crosshair colour variables, as game.setCrossHairColor sets them.
		if (key == "GeneralSettings.CrosshairColor") {
			float c[4] = {255, 255, 255, 255};
			std::sscanf(session.settings.get(key).c_str(), "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]);
			const char* names[] = {"CrossHairColorRed", "CrossHairColorGreen", "CrossHairColorBlue", "CrossHairColorAlpha"};
			for (int i = 0; i < 4; ++i) {
				hudManager.set(names[i], std::clamp(c[i] / 255.0f, 0.0f, 1.0f));
			}
		}
	};
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
	session.moviesQueued = true;
	run("Settings/RankSettings.con");
	// Remaster's widescreen HUD fix: its replacement PortedMenuShader.fx
	// multiplies every HUD vertex's x by fAspectComp (12/16) about the
	// screen's centre (the HUD in 4:3, pillarboxed), and its HudRotation.con
	// moves the side pieces out by 120. Native doesn't run the .fx; the HUD
	// pass and its pointer mapping apply the same scale (LevelSources::hudXScale).
	float hudXScale = 1.0f;
	if (std::string menuShader; files.readText("Shaders/PortedMenuShader.fx", menuShader)
		&& menuShader.find("fAspectComp = 12.0/16.0") != std::string::npos) {
		hudXScale = 12.0f / 16.0f;
		std::printf("native HUD: Direct3D widescreen patch, x scaled by 12/16\n");
	}
	run("Menu/HUD/HudSetup/HudSetupMain.con");
	// The name tags' icons and bars (the HUD manager's set-up, 0x0075cfa0).
	run("Menu/Nametag/nametags.con");
	if (!options.dedicated) {
		// The player profile's options (the crosshair colour among them).
		hudManager.set("CrossHairColorAlpha", 1);
		run("Settings/Profiles/Default/GeneralOptions.con");
		// A profile's controls: its Controls.con (0x0048f490 runs it after the
		// maps' reset), else the keys kept before profiles had them, else the
		// game's.
		const auto loadControls = [&controls, &console, &run](const std::string& profileFile) {
			controls.clear();
			for (const std::string& path : {profileFile, controlsFile()}) {
				std::ifstream saved(path);
				if (path.empty() || !saved) {
					continue;
				}
				std::string output;
				for (std::string line; std::getline(saved, line);) {
					console.execute(line, output, ConsoleAccess::Anyone);
				}
				std::printf("controls from %s\n", path.c_str());
				return;
			}
			run("Settings/Controls.con");
		};
		const auto profileControls = [&session](const std::string& folder) {
			const std::string& dir = session.profiles.directory();
			return dir.empty() || folder.empty() ? std::string() : (std::filesystem::path(dir) / folder / "Controls.con").string();
		};
		session.profiles.onLoad = [&session, &console, loadControls, profileControls](const std::string& folder) {
			if (session.settings.load(console)) {
				std::printf("settings from %s/%s\n", session.settings.directory().c_str(), folder.c_str());
			}
			loadControls(profileControls(folder));
		};
		session.profiles.onSaveControls = [&controls, profileControls](const std::string& folder) {
			const std::string path = profileControls(folder);
			if (!controls.maps().empty() && !path.empty()) {
				std::ofstream(path) << controls.script();
			}
		};
		session.profiles.onResetControls = [&controls, &run] {
			controls.clear();
			run("Settings/Controls.con");
		};
		// The profile's settings files (Global.con, Profile.con, General.con,
		// Audio.con, Video.con, Usersettings.con), as the original runs them
		// after the defaults; later maps keep the values. At start-up the
		// original loads Default (0x0048fc30, made when missing); the rewrite
		// then takes Global.con's default user, the profile last logged in,
		// so that a run without the menu has its options (the menu's login
		// selects the soldier's own again).
		if (!session.settingsLoaded) {
			session.settingsLoaded = true;
			const std::string dir = dice::hfe::menu::SettingsObjects::defaultDirectory();
			session.profiles.setDirectory(dir);
			session.profiles.ensureDefault();
			session.settings.setDirectory(dir, "Default");
			session.settings.load(console);
			std::string folder = session.settings.get("GlobalSettings.DefaultUser");
			std::error_code missing;
			if (folder.empty() || dir.empty() || !std::filesystem::exists(std::filesystem::path(dir) / folder / "Profile.con", missing)) {
				folder = "Default";
			}
			if (folder != "Default") {
				session.settings.resetProfile();
				session.settings.setDirectory(dir, folder);
			}
			session.profiles.onLoad(folder);
		} else {
			if (controls.maps().empty()) {
				loadControls(profileControls(session.settings.profile()));
			}
			if (session.settings.get("GeneralSettings.CrosshairColor") != "255 255 255 255") {
				session.settings.onChange("GeneralSettings.CrosshairColor");
			}
		}
	}
	// Apply the loaded profile before loading the remaining sound files.
	// Retail maps Medium to index 1; startup's temporary index 2 is only
	// needed until profile scripts have run.
	if (soundEngine && session.settingsLoaded) {
		const std::string quality = session.settings.get("AudioSettings.SoundQuality");
		const int index = dice::hfe::menu::audioQualityIndex(quality);
		if (sounds.initialize(index, "0", 32) != 0) {
			std::fprintf(stderr, "sound: failed to apply profile quality %s (index %d)\n", quality.c_str(), index);
		} else {
			std::printf("sound: profile quality %s (index %d)\n", quality.c_str(), index);
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
		// The scripts SwiffHost (slot 5, 0x004697c0: credits.con, once) and
		// MenuTeamManager (0x004957f0: menuTeams.con, gameModes.con) run.
		if (!session.menuScriptsRun) {
			session.menuScriptsRun = true;
			run("Menu/External/FlashMenu/credits.con");
			run("Menu/External/FlashMenu/menuTeams.con");
			run("Menu/External/FlashMenu/gameModes.con");
		}
		// The credits page: Logic.enableScroller (0x1003be40) and
		// setScrollSpeed (0x1003be90), SwiffHost slots 0x368 and 0x364.
		menu.natives().setFunction("dice.bf2.Logic", "enableScroller",
			[&session](ui::avm1::Interpreter&, const std::vector<ui::avm1::Value>& args) {
				session.menu.scroller().enable(!args.empty() && ui::avm1::toBoolean(args[0]));
				return ui::avm1::Value();
			});
		menu.natives().setFunction("dice.bf2.Logic", "setScrollSpeed",
			[&session](ui::avm1::Interpreter& vm, const std::vector<ui::avm1::Value>& args) {
				if (!args.empty()) {
					session.menu.scroller().setSpeed(static_cast<float>(vm.toNumber(args[0])));
				}
				return ui::avm1::Value();
			});
		// 0x100425c0: the fade speed setScrollFadeSpeed kept.
		menu.natives().setFunction("dice.bf2.Multiplay", "getScrollFadeSpeed",
			[&session](ui::avm1::Interpreter&, const std::vector<ui::avm1::Value>&) {
				return ui::avm1::Value(session.menu.scroller().fadeSpeed());
			});
		// The menu's music (not in an off-screen shot).
		if (options.screenshot.empty()) {
			for (const bool play : {true, false}) {
				menu.natives().setFunction("dice.bf2.Sound", play ? "playMenuMusic" : "stopMenuMusic",
					[&session, &sounds, &files, play](ui::avm1::Interpreter&, const std::vector<ui::avm1::Value>&) {
						// The Flash menu can remain loaded over a round (Esc).
						// Its music requests must not restart the menu track in battle.
						menuMusic(session.menuMusic, play && !session.startedGame, sounds, files);
						return ui::avm1::Value();
					});
			}
			// A new manager for each map: what played before is gone.
			session.menuMusic = 0;
			if (!startedGame) {
				menuMusic(session.menuMusic, true, sounds, files);
			} else {
				sounds.stopMusic(5.0f); // a game took over (0x00403370)
			}
		}
		// The menu's movies in their clips ("?titan_tutorial.bik"): through
		// the Bink decoder, unless the movies are off.
		if (options.movies) {
			// Its sound to the playback device while the menu is in a window
			// (RendDX9's Bink plays it itself, 0x1011e0d0).
			menu.setVideoOpener([&options, &session](const std::string& url) -> std::shared_ptr<ui::VideoSource> {
				dice::hfe::movie::MenuVideo::AudioSink sink;
#if BF2142_HAS_RENDERER
				if (session.window) {
					AudioOut* audio = &session.menuVideoAudio;
					sink.play = [audio](const std::vector<float>& samples, int rate) { audio->queue(samples, rate); };
					sink.stop = [audio] { audio->clear(); };
				}
#endif
				auto video = std::make_shared<dice::hfe::movie::MenuVideo>(std::move(sink));
				std::string error;
				const std::string path = moviePath(options, dice::hfe::movie::menuVideoFile(url));
				if (!video->open(path, &error)) {
					std::printf("menu movie %s: %s\n", url.c_str(), error.c_str());
					return nullptr;
				}
				std::printf("menu movie %s\n", url.c_str());
				return video;
			});
		}
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
		if (soundEngine && session.settingsLoaded) {
			const std::string quality = session.menuBindings
				? session.menuBindings->text("AudioSettings.SoundQuality")
				: session.settings.get("AudioSettings.SoundQuality");
			const int index = dice::hfe::menu::audioQualityIndex(quality);
			if (soundEngine->qualityLevel() != index) {
				if (sounds.initialize(index, "0", 32) != 0)
					std::fprintf(stderr, "sound: failed to apply quality %s (index %d)\n", quality.c_str(), index);
				else
					std::printf("sound: applied quality %s (index %d)\n", quality.c_str(), index);
			}
		}
		if (!session.menuBindings) {
			// No menu: the profile's AudioSettings (Audio.con) as the audio
			// page's apply sets them (0x00496830).
			if (session.settingsLoaded && !options.dedicated) {
				const auto& objects = session.settings;
				sounds.setMasterVolume(1);
				for (const int group : {0, 4, 3}) {
					sounds.setVolumeForGroup(group, static_cast<float>(objects.number("AudioSettings.EffectsVolume", 1)));
				}
				sounds.setVolumeForGroup(1, static_cast<float>(objects.number("AudioSettings.MusicVolume", 1)));
				sounds.setVolumeForGroup(2, static_cast<float>(objects.number("AudioSettings.HelpVoiceVolume", 1)));
			}
			return;
		}
		applyVoiceSettings();
		const dice::hfe::menu::MenuBindings& settings = *session.menuBindings;
		// The controls page's keys into the profile's Controls.con.
		if (session.profiles.onSaveControls) {
			session.profiles.onSaveControls(session.settings.profile());
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
		// The video page's dynamic shadows (0 off .. 3 high) and bloom.
		shadowSettings.quality = std::clamp(static_cast<int>(settings.setting("VideoSettings.DynamicShadowsQuality", 2)), 0, 3);
		postSettings.useBloom = settings.setting("VideoSettings.BloomEnabled", 1) != 0;
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
	}
	if (session.menuBindings || options.menu.empty()) {
		applySettings();
	}
	if (!options.menu.empty() && !session.menuBindings) {
		std::vector<uint8_t> dfm;
		std::string error;
		const std::string movie = "Menu/External/FlashMenu/" + options.menu + ".dfm";
		// What the menu calls in the game (MenuBindings.h).
		dice::hfe::menu::MenuServices services = dice::hfe::menu::MenuServices::openSpy();
		// The LOCAL page's search: --lan picks where it probes.
		services.browseLan = [address = options.lanAddress] { return net::browseLan(address); };
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
		menuBindings->setSettingsObjects(&session.settings);
		menuBindings->setLocalProfiles(&session.profiles);
		// The controls page's maps and the settings' hook: the two lines
		// above only reach bindings made by an earlier map, so the first
		// menu needs them here (without them its controls page shows no keys).
		menuBindings->setControls(&controls);
		menuBindings->onSettingsApplied = applySettings;
		// The voice natives (dice.bf2.Sound.voip*) in place of MenuBindings' stand-ins.
		if (!session.voice) {
			session.voice = std::make_unique<dice::hfe::voip::VoiceChat>();
			session.voice->runInBackground(true);
		}
		dice::hfe::voip::VoipSettingsStore voiceStore;
		voiceStore.store = [menuBindings](const std::string& name, double value) {
			menuBindings->storeSetting(name, name == "AudioSettings.VoipEnabled" || name == "AudioSettings.VoipBoostEnabled"
					? dice::hfe::ui::avm1::Value(value != 0)
					: dice::hfe::ui::avm1::Value(value));
		};
		voiceStore.read = [menuBindings](const std::string& name, double fallback) {
			return menuBindings->setting(name, fallback);
		};
		dice::hfe::voip::bindVoipNatives(menu.natives(), *session.voice, voiceStore);
		applyVoiceSettings();
		menuBindings->setProfileFile(dice::hfe::menu::MenuBindings::defaultProfileFile());
		menuBindings->setStatsFile(dice::hfe::menu::MenuBindings::defaultStatsFile());
		menuBindings->onQuit = [&session]() { session.menuQuit = true; };
		menuBindings->onLog = [](const std::string& line) { std::printf("menu: %s\n", line.c_str()); };
		menuBindings->setMaps(scanMenuMaps(options));
		// The Mod page's mods (ModInfo, each mods/<name>/Mod.desc).
		menuBindings->setMods(dice::hfe::menu::MenuBindings::findMods(options.gameDir), options.modPath);
		// The award definitions (0x0046a0c0 runs awards.con at start-up).
		if (std::string awards; files.readText("Menu/External/FlashMenu/awards.con", awards)) {
			menuBindings->setAwards(stats::parseAwards(awards));
			std::printf("%zu award definitions\n", menuBindings->awards().size());
		}
		menuBindings->onStartGame = [&session, &sounds, &files](const dice::hfe::menu::GameSetup& setup) {
			session.startedGame = setup;
			menuMusic(session.menuMusic, false, sounds, files);
		};
		menuBindings->onDisconnect = [&session] { session.disconnect = true; };
		menuBindings->onJoin = [&session, &options](const std::string& host, uint16_t port) {
			options.connect = host + ":" + std::to_string(port);
			session.joinRequested = true;
		};
		// The command line's server (0x00404780): the main page joins or
		// hosts it once logged in (Logic.getCommandLine).
		{
			dice::hfe::menu::MenuBindings::CommandLine commandLine;
			const size_t colon = options.joinServer.rfind(':');
			commandLine.joinAddress = options.joinServer.substr(0, colon);
			commandLine.port = colon != std::string::npos ? std::atoi(options.joinServer.c_str() + colon + 1) : options.hostPort;
			commandLine.password = options.joinPassword;
			commandLine.hostServer = options.hostServer;
			menuBindings->setCommandLine(commandLine);
		}
		// Mod.activateMod: the original restarts with "+modPath <path>
		// +multi 1" (host event 0xc, 0x00403fa0); main() does once the menu
		// is left.
		menuBindings->onActivateMod = [&session](const std::string& path) {
			session.restartModPath = path;
			session.menuQuit = true;
		};
		// Multiplay.playDemo (host event 0xd): the demo plays as --demo does.
		menuBindings->onPlayDemo = [&session, &options](const std::string& path) {
			options.demo = path;
			options.connect = "demo";
			session.joinRequested = true;
		};
		// The library: <profiles>/Default/demos/ (the original's
		// @HOME@/Profiles/Default/demos/, 0x0049e1e0).
		if (const std::string profiles = dice::hfe::menu::SettingsObjects::defaultDirectory(); !profiles.empty()) {
			menuBindings->setDemoDirectory((std::filesystem::path(profiles) / "Default" / "demos").string());
		}
		menuBindings->readFile = [&files](const std::string& path, std::string& text) { return files.readText(path, text); };
		menuBindings->menuHost = &session.menuHost;
		menuBindings->gameModeName = [&session](const std::string& mode) { return session.menuHost.gameModeName(mode); };
		menuBindings->setPlayerName(options.playerName);
#if BF2142_HAS_RENDERER
		menuBindings->capsLock = [] { return capsLockOn(); };
#endif
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
			viewer.gameControllers = &session.gameControllers;
			viewer.onControllerInput = [&session](const std::string& device, const std::string& control, float direction) {
				session.menuBindings->setLastInput(device, control, direction);
			};
			viewer.wantsQuit = over;
			viewer.menuCursor = [&session] { return session.menuBindings->waitCursor(); };
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
			// The start-up movies first (Esc skips the skippable ones).
			if (options.movies && !playMovies(*session.window, session.movies, options)) {
				return 0;
			}
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
		// A capture or a demo plays from its file: no host to guard.
		if (options.netReplay.empty() && options.demo.empty() && !net::hostAllowed(host)) {
			std::printf("%s\n", net::hostRefusal(host).c_str());
			return 1;
		}
		session.client = std::make_unique<net::GameClient>();
		if (!session.voice) {
			// Voice chat for a game joined without the menu.
			session.voice = std::make_unique<dice::hfe::voip::VoiceChat>();
			session.voice->runInBackground(true);
		}
		net::GameClientOptions clientOptions;
		clientOptions.profileId = options.profileId;
		clientOptions.playerName = options.playerName;
		clientOptions.contentRoot = options.gameDir; // a pure server's content check (Net/ContentCheck.h)
		clientOptions.modDirectory = options.modPath;
		if (clientOptions.playerName.empty() && session.menuBindings) {
			clientOptions.playerName = session.menuBindings->soldierName(); // the soldier the menu plays as
		}
		// The login's part (Net/PlayerAuth.h): the CD key for the challenge,
		// the player info's checksum and GP login ticket, from --login or
		// the menu's soldier login.
		if (const std::string key = net::onlineConfig().credentials.cdKey; !key.empty()) {
			clientOptions.cdKey = key;
		}
		clientOptions.nameChecksum = options.onlineNameChecksum;
		clientOptions.auth = options.onlineTicket;
		if (session.menuBindings && session.menuBindings->soldierLogin().profileId > 0) {
			const dice::hfe::menu::AccountReply& login = session.menuBindings->soldierLogin();
			clientOptions.profileId = static_cast<uint32_t>(login.profileId);
			clientOptions.nameChecksum = login.nameChecksum;
			clientOptions.auth = login.loginTicket;
		}
		showMenu = false; // the menu's join hands over to the game
		net::GameClient& client = *session.client;
		if (!session.netDiagnostics && (!options.netLog.empty() || options.netStrict || !options.netCapture.empty())) {
			session.netDiagnostics = std::make_unique<net::NetDiagnostics>();
			session.netDiagnostics->strict = options.netStrict;
			if (!options.netLog.empty() && !session.netDiagnostics->openLog(options.netLog)) {
				std::printf("%s: cannot write the net log\n", options.netLog.c_str());
			}
			session.netDiagnostics->captureTo(options.netCapture);
		}
		client.setDiagnostics(session.netDiagnostics.get());
		client.answerLoadAfterLevel = true; // LoadComplete once our level has loaded (0x004b5eb0 case 2)
		if (!options.netReplay.empty()) {
			std::vector<net::CaptureRecord> records;
			if (!net::readCapture(options.netReplay, records)) {
				std::printf("%s: not a capture\n", options.netReplay.c_str());
				session.client.reset();
				return 1;
			}
			std::printf("capture %s: %zu datagrams\n", options.netReplay.c_str(), records.size());
			client.playCapture(std::move(records), true, clientOptions);
		} else if (!options.demo.empty()) {
			net::DemoHeader header;
			std::vector<net::DemoRecord> records;
			if (!net::readDemo(options.demo, header, records)) {
				std::printf("%s: not a demo\n", options.demo.c_str());
				session.client.reset();
				return 1;
			}
			std::printf("demo %s: %s, %s (%s), %zu records\n", options.demo.c_str(), header.serverName.c_str(),
				header.level.c_str(), header.gameMode.c_str(), records.size());
			// The clock: 30 ticks a second of real time (or of the
			// simulation's, which steps the client as fast as it runs).
			auto player = std::make_shared<net::DemoPlayer>(std::move(records));
			auto started = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
			auto ticked = std::make_shared<int>(0);
			auto playback = std::make_shared<host::DemoPlayback>();
			auto carry = std::make_shared<double>(0);
			g_demo = playback;
			// Without a window the demo's clock is the run's steps
			// (DemoPlayback::steppedTicks), whatever the machine's speed.
			playback->steppedTicks = options.view ? -1 : 0;
			// demo.cameramode (DemoCamera.h): a camera file played
			// (0x00516830) or recorded (0x005171f0), then the user's
			// Settings/demo-playcam.con or demo-recordcam.con.
			if (!options.demoCamera.empty()) {
				std::vector<net::CameraKeyframe> frames;
				if (!net::readCameraFile(options.demoCamera, frames)) {
					std::printf("%s: not a camera file\n", options.demoCamera.c_str());
				} else {
					std::printf("demo camera %s: %zu frames\n", options.demoCamera.c_str(), frames.size());
					playback->cameraTrack = host::DemoCameraTrack(std::move(frames));
					playback->playingCamera = true;
					std::string output;
					console.run("Settings/demo-playcam.con", {}, output);
				}
			} else if (!options.demoRecordCamera.empty()) {
				playback->recordingCamera = true;
				playback->cameraRecordPath = options.demoRecordCamera;
				std::string output;
				console.run("Settings/demo-recordcam.con", {}, output);
			}
			client.playDemo([player, started, ticked, playback, carry](std::vector<uint8_t>& payload) {
				if (!playback->running) { // loading: the clock waits (DemoPlayback::running)
					*started = std::chrono::steady_clock::now()
						- std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(*ticked / 30.0));
				}
				const auto elapsed = std::chrono::steady_clock::now() - *started;
				int due = !playback->running ? *ticked
					: playback->steppedTicks >= 0 ? playback->steppedTicks
												  : static_cast<int>(std::chrono::duration<double>(elapsed).count() * 30.0);
				// A stall of over half a second (a hitch) doesn't count
				// either: counted, the demo raced through its records at
				// once and a camera file played out before the next frame.
				if (playback->steppedTicks < 0 && due - *ticked > 15) {
					*started += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
						std::chrono::duration<double>((due - *ticked - 1) / 30.0));
					due = *ticked + 1;
				}
				if (due > *ticked) {
					// The game speed (0x004b3b90: the time manager's scale).
					const int ticks = playback->advance(due - *ticked, *carry);
					// The camera recorded once the demo's clock runs
					// (0x005f9630 waits for the reader's +0xdc), a frame
					// for each tick gone by.
					// Headless (no view drawn) the camera's own state is
					// recorded: its follow values, at the origin unturned
					// until a view is set.
					if (playback->recordingCamera && player->started() && (playback->hasView || playback->steppedTicks >= 0)) {
						playback->cameraRecorder.record(ticks, static_cast<uint32_t>(player->tick()), playback->cameraFrame());
					}
					player->advance(ticks);
					*ticked = due;
				}
				playback->tick = static_cast<uint32_t>(player->tick());
				playback->fraction = static_cast<float>(*carry);
				// A camera file played out ends the demo (0x00516bb0 ->
				// 0x005f8c70 in 0x00432ee0).
				if (playback->playingCamera && playback->cameraTrack.ended()) {
					player->finish();
				}
				playback->ended = player->finished();
				if (playback->ended) {
					playback->saveCamera();
				}
				return player->next(payload);
			});
			// The playback controls (the demo interface 0x00928fa8 through
			// QueryInterface 0x1d4c2) and the demo camera: DemoCamera.h. The
			// view follows the camera (playerView); free, it is the viewer's.
			bindMethod(console, "demo.toggleDemoPauseOrPlay", ConsoleAccess::Anyone, [playback] { playback->togglePause(); });
			bindMethod(console, "demo.restartDemo", ConsoleAccess::Anyone, [player, started, ticked, playback] {
				player->restart();
				playback->restart();
				*started = std::chrono::steady_clock::now();
				*ticked = 0;
			});
			bindMethod(console, "demo.shutdownDemo", ConsoleAccess::Anyone, [player, playback] {
				playback->speed = 100;
				player->finish();
			});
			bindProperty<int>(console, "Game.simulationRate", ConsoleAccess::Anyone, [playback] { return playback->speed; },
				[playback](const int& v) { playback->speed = std::max(v, 0); });
			bindMethod(console, "demo.adjustDemoFov", ConsoleAccess::Anyone, [playback](float fov) { playback->fov = fov; });
			net::GameClient* demoClient = &client;
			const auto eligible = [demoClient](int id) {
				const net::RemotePlayer* p = demoClient->world().player(id);
				return p && p->alive && p->soldierGhost != 0;
			};
			bindMethod(console, "demo.togglePlayerDemo", ConsoleAccess::Anyone,
				[playback, eligible](bool next) { playback->camera.step(next ? 1 : -1, eligible); });
			bindMethod(console, "demo.toggleCameraDemo", ConsoleAccess::Anyone,
				[playback, eligible] { playback->camera.toggle(eligible); });
		} else {
			client.connect(host, port, clientOptions);
		}
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
		// --play: ask to spawn (the first kit, any point) once in the game.
		session.joinSpawn = options.playTeam != 0 && !options.spectate;
		session.joinSpectate = options.spectate;
		session.joinTeam = options.playTeamGiven ? options.playTeam : 0;
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
	loadingBindings.gameModeName = [&session](const std::string& mode) { return session.menuHost.gameModeName(mode); };
	// A game started from the menu waits on the loaded level's JOIN GAME (retail's endOfRound.dfm, Logic.readyToPlay):
	// set at the end of the build, cleared by the button. Not for --screenshot, --simulate,
	// demos, a game we host or a joined server, which enter the game at once as before.
	bool joinPending = false;
	loadingBindings.onReadyToPlay = [&joinPending](bool) { joinPending = false; };
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
	const auto loadingStep = [&](float progress, [[maybe_unused]] int frames = 3) {
		loadingBindings.setLoading(progress);
#if BF2142_HAS_RENDERER
		for (int i = 0; loadingScreen && i < frames; ++i) {
			loadingScreen->frame();
		}
#endif
	};
	loadingStep(0);
	if (session.client) {
		session.client->loadingLevel(); // pings answered while the level loads
	}
	std::set<uint32_t> levelObjects; // the static objects, before the game mode's
	// Level loading, after FUN_004d3450: the level's archives are mounted at
	// its directory, then materials, static objects and the game mode's
	// gameplay objects are created.
	if (!options.level.empty()) {
		const std::string levelPath = "Levels/" + options.level + "/";
		// Mounting stays allowed (file manager slot 0x78) across the
		// level's own archive lists: server.zip, ServerArchives.con, then
		// client.zip and ClientArchives.con on a game with a view, and only
		// then is it turned off. The HEAT levels mount their _HEAT
		// textures this way.
		files.setMountingAllowed(true);
		files.mountArchive(levelPath + "server.zip", levelPath);
		if (files.exists(levelPath + "ServerArchives.con")) {
			run((levelPath + "ServerArchives.con").c_str());
		}
		if (!options.dedicated) {
			files.mountArchive(levelPath + "client.zip", levelPath);
			if (files.exists(levelPath + "ClientArchives.con")) {
				run((levelPath + "ClientArchives.con").c_str());
			}
		}
		files.setMountingAllowed(false);
		loadingStep(0.1f);
		std::printf("%zu template scripts run from Objects/\n",
			world::loadObjectTemplates(console, files, "Objects/", options.bots > 0));
		loadingStep(0.5f);
		run("Common/Material/MaterialManagerSettings.con");
		run((levelPath + "Init.con").c_str());
		loadingStep(0.6f);
		objects.setGameMode(options.gameMode); // Object.notInAI's test
		run((levelPath + "StaticObjects.con").c_str());
		// The level's own objects: a client loads them too (no ghosts).
		for (const auto& object : objects.objects()) {
			levelObjects.insert(object->id());
		}
		loadingStep(0.8f);
		// v_arg1 is "host" on the server (a local game hosts) and "client"
		// otherwise; spawners and control points only exist on the host
		// (on a server's game they come as its ghosts).
		run((levelPath + "GameModes/" + options.gameMode + "/" + std::to_string(options.mapSize) +
				"/GamePlayObjects.con").c_str(),
			{session.client ? "client" : "host"});
		if (!options.dedicated) {
			// The level loaded (the sound manager's event 5/3, 0x007fb6c0):
			// Sound/GameSounds.con's S_Game_* sounds (0x007fbb90), then
			// Sound/AudioTweak.con's template tweaks. Then the voice
			// messages in the teams' languages (0x004cdab0).
			run("Sound/GameSounds.con");
			run("Sound/AudioTweak.con");
			voices.reset(gameLogic.team(1).language, gameLogic.team(2).language);
			run("Sound/VoiceMessages.con");
			voices.build(options.dedicated);
			std::printf("voice messages: %zu, team prefixes %s %s\n", voices.messageCount(), voices.teamPrefix(1).c_str(),
				voices.teamPrefix(2).c_str());
		} else if (options.host) {
			// A dedicated server has the game logic's message table too
			// (0x004cdab0 runs Sound/VoiceMessages.con on every machine; its
			// sounds, 0x004d8f00, leave out the local voices there): the
			// RadioMessage events carry the messages' indices in it.
			voices.reset(gameLogic.team(1).language, gameLogic.team(2).language);
			run("Sound/VoiceMessages.con");
		}
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

	if (options.view || !options.screenshot.empty() || options.simulate > 0 || (options.dedicated && session.admin)) {
		// Start the round: spawners take their control points' teams and
		// put their vehicles out on the first tick.
		game::GameWorld round(templates, objects);
		round.setAutoReload(session.settings.number("GeneralSettings.AutoReload", 0) != 0);
		// The players as the HUD has them (HudWorld.h): a joined game's,
		// the server's (g_remoteHud, set while one runs), else the round's.
		const auto hudPlayer = [&round](int id, game::GameWorld::PlayerInfo& out) {
			return g_remoteHud ? g_remoteHud->playerInfo(id, out) : round.playerInfo(id, out);
		};
		const auto hudLocal = [&round] { return g_remoteHud ? g_remoteHud->localPlayer() : round.localPlayer(); };
		const auto hudPlayers = [&round] { return g_remoteHud ? g_remoteHud->playerList() : round.playerList(); };
		// The server of a joined game (nullptr: a local round, or a host).
		const auto joinedServer = [&session]() -> net::GameClient* {
			return g_remoteHud && session.client ? session.client.get() : nullptr;
		};
		// Titan mode's Titans and silos run in gpm_ti (the Titan sync's
		// 0x00561e20 checks the server's game mode).
		round.setGameMode(options.gameMode);
		round.setCombatAreas(&combatAreas);
		if (session.admin) {
			const net::ServerSettings& sv = session.admin->settings;
			round.setFriendlyFire({static_cast<float>(sv.soldierFriendlyFire), static_cast<float>(sv.vehicleFriendlyFire),
				static_cast<float>(sv.soldierSplashFriendlyFire), static_cast<float>(sv.vehicleSplashFriendlyFire)});
			round.setFriendlyFireWithMines(sv.friendlyFireWithMines);
			round.setHitIndicator(sv.hitIndicator);
		}
		// The bots see as far as the level's fog lets them (AIDLL 0x10056b20).
		botSettings.levelFog = lights.fogStartEndAndBase;
		game::BotManager bots(round, botSettings);
		bots.attach(botSettings); // the scripted-bot commands (ai.botMoveTo...)
		bots.setServerMaxPlayers(options.maxPlayers); // AIMain's kick and fill (sv.maxPlayers when hosting)
		round.unlockAllByDefault = options.unlockAll;
		if (heightmaps.hasSea()) {
			round.setWaterLevel(heightmaps.seaWaterLevel());
		}
		// The spawn screen's kit buttons: the kit (by the team's kit index,
		// gameLogic.setKit) of the local player's next spawn.
		int localKit = 1;
		bindMethod(console, "spawnManager.setPlayerKit", ConsoleAccess::Anyone, [&](int index) {
			if (net::GameClient* server = joinedServer()) {
				// A joined game's (SelectKit, 0x0050ae90): the server's round
				// takes it by the team's index (0x0050b010).
				game::GameWorld::PlayerInfo me;
				const game::Team& team = gameLogic.team(hudPlayer(hudLocal(), me) && me.team == 1 ? 1 : 2);
				if (index >= 0 && static_cast<size_t>(index) < team.kits.size()) {
					localKit = index;
					server->selectKit(index);
				}
				return;
			}
			const game::Team& team = gameLogic.team(round.playerTeam() == 1 ? 1 : 2);
			if (index >= 0 && static_cast<size_t>(index) < team.kits.size() && round.localPlayer() >= 0) {
				localKit = index;
				round.setKit(round.localPlayer(), team.kits[static_cast<size_t>(index)].name,
					team.kits[static_cast<size_t>(index)].soldier);
			}
		});
		// The shipped deployment screen's faction buttons. A team change is
		// accepted only before deployment or after death, as in GameWorld.
		bindMethod(console, "spawnManager.setPlayerTeam", ConsoleAccess::Anyone, [&](int team) {
			if (team != 1 && team != 2) return;
			if (net::GameClient* server = joinedServer()) {
				server->selectTeam(team);
				return;
			}
			if (dice::main::selectDeploymentTeam(round, gameLogic, team, localKit)) g_spawnVehicle = 0;
		});
		// The commander (Commander.con's buttons and rose): applying and
		// resigning, a tool or an order armed by its button and used where
		// the map is clicked next, the satellite scan at once.
		using Ability = game::GameWorld::CommanderAbility;
		bool commanderScreen = false;
		bool commanderKeyWas = false; // Caps Lock as of the last input
		std::optional<std::pair<float, float>> mapMenuPoint;
		std::function<bool(float, float)> placeAt;
		std::optional<Ability> armedTool;
		std::optional<game::GameWorld::OrderType> armedOrder;
		const auto myTeam = [&] {
			game::GameWorld::PlayerInfo me;
			return hudPlayer(hudLocal(), me) && me.team == 1 ? 1 : 2;
		};
		// A joined game's tools go to the server (GameProtocol.h's
		// CommanderToolUse: the HUD's uses send the event, 0x007d88d0..).
		const auto toolOf = [](Ability ability) {
			switch (ability) {
			case Ability::OrbitalStrike: return net::CommanderToolUse::OrbitalStrike;
			case Ability::Uav: return net::CommanderToolUse::Uav;
			case Ability::Satellite: return net::CommanderToolUse::Satellite;
			case Ability::Supplies: return net::CommanderToolUse::Supplies;
			default: return net::CommanderToolUse::Artillery;
			}
		};
		// A tool used where the commander says: on the server of a joined
		// game (its recharge kept here as the exe's client keeps it), else
		// in the round.
		const auto useTool = [&](Ability ability, float x, float z) {
			if (net::GameClient* server = joinedServer()) {
				const auto state = round.abilityState(myTeam(), ability);
				if (!state.available || !state.ready) {
					return false;
				}
				server->commanderTool(toolOf(ability), x, z);
				round.noteCommanderUse(myTeam(), ability);
				return true;
			}
			return round.commanderAction(myTeam(), ability, x, z);
		};
		bindMethod(console, "Commander.commanderApply", ConsoleAccess::Anyone, [&] {
			if (net::GameClient* server = joinedServer()) {
				server->applyForCommander(); // the Commander event, kind 4
				return;
			}
			round.applyForCommander(round.localPlayer());
		});
		bindMethod(console, "Commander.commanderResign", ConsoleAccess::Anyone, [&] {
			if (net::GameClient* server = joinedServer()) {
				server->resignCommander(); // kind 2
				return;
			}
			round.resignCommander(round.localPlayer());
		});
		// A tool that can't be used says why to the commander alone
		// (commanderToolRefusal: 0x007d73a0.. on arming, 0x007d88d0.. on
		// use); a use that goes ahead plays "mapAction".
		const auto toolRefused = [&](Ability tool) {
			const auto state = round.abilityState(myTeam(), tool);
			const std::string why = dice::hfe::menu::commanderToolRefusal(static_cast<int>(tool), state.available, state.ready);
			if (!why.empty()) {
				round.hearRadio(hudLocal(), 2, why);
			}
			return !why.empty();
		};
		const auto arm = [&](Ability tool) {
			armedTool = armedTool == tool || toolRefused(tool) ? std::nullopt : std::optional<Ability>(tool);
			armedOrder.reset();
			if (mapMenuPoint && armedTool && placeAt) {
				const auto point = *mapMenuPoint;
				mapMenuPoint.reset();
				g_mapMenu = 0;
				placeAt(point.first, point.second);
			}
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
			[&] {
				if (!toolRefused(Ability::Satellite) && useTool(Ability::Satellite, 0, 0) && g_hudSound) {
					g_hudSound("mapAction");
				}
			});
		// The Titan's move (Commander.moveTitan / toggleMoveTitanState: the
		// HUD manager's slots 0x328 / 0x334 arm it; the map's next click
		// orders the Titan there, 0x0050f1f0 on the server).
		bool moveTitanArmed = false;
		for (const char* command : {"Commander.moveTitan", "Commander.toggleMoveTitanState"}) {
			bindMethod(console, command, ConsoleAccess::Anyone, [&] {
				moveTitanArmed = !moveTitanArmed;
				armedTool.reset();
				armedOrder.reset();
			});
		}
		// sendOrder <kind> <add>: the order kind (0..3 move, attack, defend,
		// repair from the issue-order menu, 4..7 the same placed on the map,
		// 10 or more drops it), sent where the map is clicked next. Both the
		// squad leader's and the commander's run the same handler
		// (0x00770889 → 0x007d6ab0). The commander's goes to the squads
		// selected in the screen's squad list (CommanderMenu), else to the
		// squad whose leader is nearest the click; the squad leader's to his
		// own squad.
		bool leaderOrder = false;
		// The right-click menu's point (the map node's +0xdbc, the click in
		// the screen): an order picked from the menu goes there at once.
		const auto armOrder = [&](int kind, bool leader) {
			const game::GameWorld::OrderType type = game::GameWorld::orderTypeFromKind(kind);
			armedOrder = type == game::GameWorld::OrderType::None ? std::nullopt : std::optional(type);
			leaderOrder = leader;
			armedTool.reset();
			if (mapMenuPoint && armedOrder && placeAt) {
				const auto point = *mapMenuPoint;
				mapMenuPoint.reset();
				g_mapMenu = 0;
				placeAt(point.first, point.second);
			}
		};
		bindMethod(console, "Commander.sendOrder", ConsoleAccess::Anyone, [&armOrder](int kind, int) { armOrder(kind, false); });
		bindMethod(console, "SquadLeader.sendOrder", ConsoleAccess::Anyone, [&armOrder](int kind, int) { armOrder(kind, true); });
		bindMethod(console, "Commander.issueOrder", ConsoleAccess::Anyone, [&armOrder] { armOrder(4, false); });
		// The offer of command after applying (0x004c8c20 accepts it).
		bindMethod(console, "Commander.accept", ConsoleAccess::Anyone, [&](int accept) {
			if (net::GameClient* server = joinedServer()) {
				server->answerCommanderOffer(accept != 0); // the Commander event, kind 0 / 1
				return;
			}
			round.answerCommanderOffer(round.localPlayer(), accept != 0);
		});
		// The squad leaders' requests (SquadLeader.sendRequest <kind> <add>,
		// 0x007708d4 → HUD slot 0x3f8 0x007d6d20) and the commander's
		// answer: Commander.commRoseAccept on a selected request tag (slot
		// 0x390, 0x007d8d30 → 0x007c7e20), else the leader's answer to an
		// order.
		dice::hfe::menu::SquadRequests squadRequests(round);
		bool uavWasUp = false;
		struct OrderMarker {
			int kind = -1;
			physics::Vec3 at;
			uint32_t object = 0;
		};
		std::array<OrderMarker, 2> orderMarkers; // the leader's, the commander's
		// A mesh object's render radius (RendDX9's instance slot 0x44,
		// nametags::meshRenderRadius), for the order markers: the marker's
		// height is taken as it is made, its instance still at LOD 0 (the
		// LOD pick 0x100c3390 runs as it is drawn); read once a template.
		// A marker's radius (its
		// object's slot 0x8c, 0x006f7ec0): these Bundles have no collision
		// mesh, so the render object's plus its geometry template's slot
		// 0xe4 (the BundledMesh's 0x100a7da0: 0) and the object's slot 0x94
		// (0). 0x006f7ec0 also takes the larger of that and each child's
		// radius plus its offset's length (light sources, 0xc4ca, left out);
		// the marker's one child, e_marker_* 0.08 m up, is an EffectBundle
		// (vtable 0x0097b658, the same getter, no mesh of its own) whose
		// particle systems' radii are RendDX9's emitters', which the rewrite
		// doesn't keep: the mesh's alone here.
		std::map<std::string, float> markerRadii;
		const auto markerRadius = [&markerRadii, &geometries, &files](const world::ObjectTemplate& t) {
			const auto known = markerRadii.find(t.name());
			if (known != markerRadii.end()) {
				return known->second;
			}
			float radius = 0;
			const geom::GeometryTemplate* geometry = geometries.find(t.geometry());
			std::vector<uint8_t> data;
			if (geometry && files.readFile(geometry->path, data)) {
				if (const std::optional<geom::MeshFile> mesh = geom::parseMesh(data, geometry->kind)) {
					radius = nametags::meshRenderRadius(*mesh);
				}
			}
			markerRadii[t.name()] = radius;
			return radius;
		};
		squadRequests.localize = [&localization](const std::string& id) { return localization.text(id); };
		squadRequests.noCommanderNeeded = options.gameMode == "sp1" || options.gameMode == "gpm_sp" || options.gameMode == "gpm_coop";
		// The rose's point (0x00753b40: the view's ray out to 1000 m, HUD
		// slot 0x18c, into the order record's +0x28c): where the look meets
		// the ground, else 1000 m along it.
		const auto rosePoint = [&round, &hudPlayer, &hudLocal](float& x, float& z) {
			game::GameWorld::PlayerInfo me;
			if (!hudPlayer(hudLocal(), me)) {
				return false;
			}
			const float yaw = me.yaw * 3.14159265f / 180, pitch = me.aimPitch * 3.14159265f / 180;
			const float dx = std::sin(yaw) * std::cos(pitch), dy = std::sin(pitch), dz = std::cos(yaw) * std::cos(pitch);
			physics::Vec3 at = me.position + physics::Vec3 {0, 1.6f, 0};
			for (float d = 0; d < 1000; d += 2) {
				const physics::Vec3 p = at + physics::Vec3 {dx * d, dy * d, dz * d};
				x = p.x;
				z = p.z;
				if (p.y <= round.terrainHeight(p.x, p.z)) {
					break;
				}
			}
			return true;
		};
		bindMethod(console, "SquadLeader.sendRequest", ConsoleAccess::Anyone, [&](int kind, int add) {
			float x = 0, z = 0;
			bool placed = false;
			if (session.client && g_remoteHud) {
				// A joined game (0x007d6d20 on a client): alive, under the
				// count's limit, then the ClientSquadManager's slot 0x50
				// sends it to the server (IssueSquadOrder, store 2) at the
				// map menu's point or the rose's aim; the server's 2/0x1e
				// brings the leader's line; a team without a commander gets
				// no_commander_available as radio, to the leader alone.
				game::GameWorld::PlayerInfo me;
				if (!g_remoteHud->playerInfo(g_remoteHud->localPlayer(), me)) {
					return;
				}
				if (!me.alive) {
					if (squadRequests.onPersonal) {
						squadRequests.onPersonal(localization.text("HUD_HUD_SQUAD_ALIVE_TO_REQUEST"));
					}
					return;
				}
				if (!squadRequests.count()) {
					return;
				}
				if (!squadRequests.noCommanderNeeded && g_remoteHud->commander(me.team) < 0) {
					round.hearRadio(me.id, 2, "no_commander_available");
					return;
				}
				if (!me.squadLeader || kind < 0 || kind >= game::GameWorld::kRequestKinds) {
					return;
				}
				if (add) {
					placed = mapMenuPoint && hudManager.mapWorldAt(mapMenuPoint->first, mapMenuPoint->second, x, z);
					if (placed) {
						mapMenuPoint.reset();
						g_mapMenu = 0;
					}
				} else {
					const float yaw = me.yaw * 3.14159265f / 180, pitch = me.aimPitch * 3.14159265f / 180;
					const float dx = std::sin(yaw) * std::cos(pitch), dy = std::sin(pitch), dz = std::cos(yaw) * std::cos(pitch);
					const physics::Vec3 eye = me.position + physics::Vec3 {0, 1.6f, 0};
					for (float d = 0; d < 1000; d += 2) {
						const physics::Vec3 p = eye + physics::Vec3 {dx * d, dy * d, dz * d};
						x = p.x;
						z = p.z;
						if (p.y <= round.terrainHeight(p.x, p.z)) {
							break;
						}
					}
					placed = true;
				}
				if (placed) {
					session.client->requestAsset(kind, {x, round.terrainHeight(x, z), z});
				}
				return;
			}
			if (add) {
				// The map's point (0x00780820): the right-click menu's.
				if (mapMenuPoint && hudManager.mapWorldAt(mapMenuPoint->first, mapMenuPoint->second, x, z)) {
					placed = true;
					mapMenuPoint.reset();
					g_mapMenu = 0;
				}
			} else {
				placed = rosePoint(x, z);
			}
			if (placed) {
				squadRequests.send(round.localPlayer(), kind, x, z);
			}
		});
		bindMethod(console, "Commander.commRoseAccept", ConsoleAccess::Anyone, [&](int accept) {
			if (net::GameClient* server = joinedServer()) {
				// A joined game's commander (0x007c7e20 on a client): the
				// request is the selected tag's (the server's requests are
				// the name tags' there too), else the team's first standing
				// one. Accepted, the tool is used quietly at the request's
				// point (its use goes to the server); then each squad with a
				// request of the kind is answered (the squad manager's slot
				// 0x24: SetAcceptOrder) and its leader told, on TCPlayer
				// (0x007c7810 → the client's slot 0x50): "<tool>_accept" to
				// the one answered, "<tool>_deny" to the others. Refused, the
				// leader hears "<tool>_deny"; a tool that can't be used says
				// "<tool>_unavailable" to the commander alone and the
				// request stays.
				game::GameWorld::PlayerInfo me;
				if (!g_remoteHud->playerInfo(g_remoteHud->localPlayer(), me)) {
					return;
				}
				if (!me.commander) {
					// A squad leader's answer to the commander's order
					// (SetAcceptOrder, store 1: 0x004c2f60 keeps it accepted
					// or takes it off).
					game::GameWorld::SquadOrder order;
					if (me.squadLeader && g_remoteHud->squadOrder(me.team, me.squad, true, order)) {
						server->answerOrder(me.squad, static_cast<int>(order.type), accept != 0);
					}
					return;
				}
				int squad = -1, kind = -1;
				const auto standing = g_remoteHud->squadRequests(me.team);
				if (const int tag = g_nametags ? g_nametags->selectedRequest() : -1; tag >= 0) {
					squad = tag / 4;
					kind = tag % 4;
				} else if (!standing.empty()) {
					squad = standing.front().squad;
					kind = standing.front().kind;
				}
				const auto request = std::find_if(standing.begin(), standing.end(),
					[&](const game::GameWorld::SquadRequest& r) { return r.squad == squad && r.kind == kind; });
				if (request == standing.end()) {
					return;
				}
				const std::string tool = dice::hfe::menu::SquadRequests::toolName(kind);
				const auto tell = [&](int toSquad, const std::string& voice) {
					for (const auto& p : g_remoteHud->playerList()) {
						if (p.team == me.team && p.squad == toSquad && p.squadLeader && voices.message(voice) >= 0) {
							server->radioMessage(14, voices.message(voice), p.id); // TCPlayer
						}
					}
				};
				if (!accept) {
					server->answerRequest(squad, kind);
					tell(squad, tool + "_deny");
					std::printf("request of squad %d (%s) denied\n", squad, tool.c_str());
					return;
				}
				const Ability ability = game::GameWorld::requestAbility(kind);
				if (!useTool(ability, request->position.x, request->position.z)) {
					round.hearRadio(me.id, 2, tool + "_unavailable");
					std::printf("request of squad %d (%s): the tool is unavailable\n", squad, tool.c_str());
					return;
				}
				if (g_hudSound) {
					g_hudSound("mapAction");
				}
				for (const auto& other : standing) {
					if (other.kind == kind) {
						server->answerRequest(other.squad, kind);
						tell(other.squad, tool + (other.squad == squad ? "_accept" : "_deny"));
					}
				}
				std::printf("request of squad %d (%s) accepted\n", squad, tool.c_str());
				return;
			}
			const int request = g_nametags ? g_nametags->selectedRequest() : -1;
			if (request >= 0) {
				squadRequests.answer(round.localPlayer(), request / 4, request % 4, accept != 0);
			} else {
				round.acceptOrder(round.localPlayer(), accept != 0);
			}
		});
		// The commander screen's tabs and squad list (CommanderMenu.*).
		dice::hfe::menu::CommanderMenu commanderMenu(round, hudManager);
		commanderMenu.bind(console);
		commanderMenu.ctrlHeld = [] { return g_ctrlHeld; };
		commanderMenu.shiftHeld = [] { return g_shiftHeld; };
		// The leader's voice on or off (as the scoreboard's mute box), and
		// VoipPlayerMute to the server (0x00510de0: the bit, our id, his).
		commanderMenu.localize = [&localization](const std::string& id) { return localization.text(id); };
		commanderMenu.onTutorial = [&helpPopups](int index) { helpPopups.enableTutorial(index); };
		// A leader's voice (player slots 0x2e4 / 0x2f0 / 0x2f4): a joined
		// game's voice client's mute and talk; bots and an offline round
		// have none.
		commanderMenu.voice = [&](int player) {
			if (!session.voice || !session.client) {
				return 0;
			}
			const net::RemotePlayer* p = session.client->world().player(player);
			if (!p || p->ai || p->local) {
				return 0;
			}
			return session.voice->client().muted(player) ? 1 : session.voice->client().talking(player) ? 2 : 3;
		};
		commanderMenu.onMuteLeader = [&](int leader) {
			if (!session.voice) {
				return;
			}
			const bool mute = !session.voice->client().muted(leader);
			session.voice->client().setMuted(leader, mute);
			if (net::GameClient* server = joinedServer()) {
				net::GameEvent e;
				e.type = net::kEvVoipPlayerMute;
				e.values = {mute ? 1u : 0u, static_cast<uint32_t>(server->world().localPlayer()) & 0xff,
					static_cast<uint32_t>(leader) & 0xff};
				server->sendEvent(e);
			}
		};
		// The assets' defend buttons (Commander.defend<Asset>: HUD manager
		// slots 0x374, 0x37c, 0x384, ..., 0x007d7a30 / 0x007d7ac0 /
		// 0x007d7b50): the asset object (0x007d92f0: the team's object of
		// class 0x9c42 whose +0x29c is the asset's kind, 1 artillery, 2 the
		// satellite, 3 the UAV) is where a defend order (kind 6, placed on
		// the map) goes to the selected squads (slot 0x3f4); here the squad
		// whose leader is nearest the asset, as for a map click. The zoom
		// buttons (<asset>Zoom, slots 0x370 / 0x378 / 0x380 → 0x398,
		// 0x007d8e00) move the commander's camera to the asset (the "0xf"
		// there is a local dispatch): CommanderCam to a server, a local
		// round's own camera (g_overheadCamera) here.
		const auto assetAt = [&](Ability ability, float& x, float& z) {
			const auto state = round.abilityState(myTeam(), ability);
			const world::Object* asset = state.available ? round.objects().find(state.asset) : nullptr;
			if (asset) {
				x = asset->position().x;
				z = asset->position().z;
			}
			return asset != nullptr;
		};
		// An order to the squads (a joined game's to the server: the
		// commander's IssueSquadOrder, store 1).
		const auto orderSquad = [&](int squad, game::GameWorld::OrderType type, float x, float z) {
			if (net::GameClient* server = joinedServer()) {
				server->issueOrder(1, squad, static_cast<int>(type), {x, round.terrainHeight(x, z), z});
				return;
			}
			round.issueOrder(myTeam(), squad, type, x, z, round.localPlayer());
		};
		// The squad whose leader is nearest a point (the HUD's players).
		const auto nearestSquad = [&](float x, float z) {
			int squad = 0;
			float best = 1e30f;
			for (const auto& player : hudPlayers()) {
				const float dx = player.position.x - x, dz = player.position.z - z;
				if (player.team == myTeam() && player.squadLeader && player.squad && dx * dx + dz * dz < best) {
					best = dx * dx + dz * dz;
					squad = player.squad;
				}
			}
			return squad;
		};
		const auto defendAt = [&](float x, float z) {
			if (!commanderMenu.selected().empty()) {
				for (const int squad : commanderMenu.selected()) {
					orderSquad(squad, game::GameWorld::orderTypeFromKind(6), x, z);
				}
				return;
			}
			if (const int squad = nearestSquad(x, z)) {
				orderSquad(squad, game::GameWorld::orderTypeFromKind(6), x, z);
			}
		};
		for (const auto& [name, ability] : std::vector<std::pair<std::string, Ability>> {{"Artillery", Ability::Artillery},
				 {"Satellite", Ability::Satellite}, {"UAV", Ability::Uav}, {"OrbitalStrike", Ability::OrbitalStrike}}) {
			const Ability tool = ability;
			bindMethod(console, "Commander.defend" + name, ConsoleAccess::Anyone, [&, tool] {
				float x = 0, z = 0;
				if (assetAt(tool, x, z)) {
					defendAt(x, z);
				}
			});
		}
		// The Titan's (defendMoveTitan): at the team's Titan.
		bindMethod(console, "Commander.defendMoveTitan", ConsoleAccess::Anyone, [&] {
			if (const world::Object* titan = round.objects().find(round.titanOf(myTeam()))) {
				defendAt(titan->position().x, titan->position().z);
			}
		});
		// The spawn screen's kit and squad halves (SpawnManager.toggleMembers
		// 0/1, 0x007b8b80: menu states 1 and 0x12/0x13, which show KitsShow
		// or MembersShow) and its squad menu.
		bool membersShown = false;
		bindMethod(console, "SpawnManager.toggleMembers", ConsoleAccess::Anyone, [&](bool members) { membersShown = members; });
		bindMethod(console, "spawnManager.commitSuicide", ConsoleAccess::Anyone,
			[&] { if (session.client) session.client->suicide(); else round.setHitPoints(round.playerObject(), 0); });
		// The kit buttons, the customise and the unlock screens (HudMenusKits.h).
		dice::hfe::menu::KitMenu kitMenu(round, gameLogic, templates, hudManager);
		kitMenu.setLocalizer([&localization](const std::string& id) { return localization.text(id); });
		kitMenu.bind(console);
		kitMenu.onEquip = [&](const std::vector<std::string>& containers) {
			if (round.localPlayer() < 0) {
				return;
			}
			round.setUnlocks(round.localPlayer(), containers, false);
			const game::Team& team = gameLogic.team(round.playerTeam() == 1 ? 1 : 2);
			if (localKit >= 0 && static_cast<size_t>(localKit) < team.kits.size()) {
				const game::Kit& kit = team.kits[static_cast<size_t>(localKit)];
				round.setKit(round.localPlayer(), kit.name,
					kitMenu.heavyArmor() && !kit.heavySoldier.empty() ? kit.heavySoldier : kit.soldier);
			}
		};
		// The radio's console methods (HudMenusComm.h): the local player's
		// messages, heard by whom the target channel reaches.
		// game.radioMessage (0x00423e61 → the game's slot 0x50 with the
		// spam check and the local echo on: 0x004a5a80 on a server,
		// 0x004b2b60 on a client): to the server of a joined game, else
		// heard in the round; the sender hears his own at once (0x0042e0c0).
		// The spam check (player slot 0x340, HudMenusComm.h's RadioSpam)
		// with the server's limits (data block 0, else our own settings).
		dice::hfe::menu::RadioSpam radioSpam;
		std::function<void(const std::string&)> spamLine; // the personal line (the chat, set below)
		const auto spamRefused = [&](bool countIt) {
			if (net::GameClient* server = joinedServer(); server && server->world().dataBlocks().count(0)) {
				const net::RemoteWorld::ServerSettings sv = server->world().settings();
				radioSpam.interval = sv.radioSpamInterval;
				radioSpam.maxCount = sv.radioMaxSpamFlagCount;
				radioSpam.blockedTime = sv.radioBlockedDurationTime;
			} else if (session.admin) {
				radioSpam.interval = session.admin->settings.radioSpamInterval;
				radioSpam.maxCount = session.admin->settings.radioMaxSpamFlagCount;
				radioSpam.blockedTime = session.admin->settings.radioBlockedDurationTime;
			}
			const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
			game::GameWorld::PlayerInfo me;
			const bool commander = hudPlayer(hudLocal(), me) && me.commander;
			radioSpam.tick(commander, now);
			if (!radioSpam.check(countIt, commander, now)) {
				return false;
			}
			// 0x004b2b60: the local "spam" voice (game slot 0x60) and the line.
			round.hearRadio(hudLocal(), 2, "spam");
			if (spamLine) {
				std::string line = localization.text("HUD_HUD_MENU_CHAT_SPAM_MESSAGE");
				if (const size_t at = line.find("#SEC#"); at != std::string::npos) {
					line.replace(at, 5, std::to_string(radioSpam.secondsLeft(now)));
				}
				spamLine(line);
			}
			return true;
		};
		const auto sayRadio = [&](int channel, const std::string& voice, int value, bool checkSpam = true) {
			if (channel < 0 || voice.empty()) {
				std::printf("radio-say: dropped channel=%d voice=\"%s\"\n", channel, voice.c_str());
				std::fflush(stdout);
				return;
			}
			if (checkSpam && spamRefused(true)) {
				std::printf("radio-say: refused as spam channel=%d voice=%s\n", channel, voice.c_str());
				std::fflush(stdout);
				return;
			}
			std::printf("radio-say: channel=%d voice=%s\n", channel, voice.c_str());
			std::fflush(stdout);
			if (net::GameClient* server = joinedServer()) {
				const int index = voices.message(voice);
				if (index >= 0) {
					server->radioMessage(channel, index, value);
					round.hearRadio(hudLocal(), channel, voice, hudLocal());
				}
				return;
			}
			round.radioMessage(round.localPlayer(), channel, voice, value);
			round.hearRadio(round.localPlayer(), channel, voice, round.localPlayer());
		};
		bindMethod(console, "game.radioMessage", ConsoleAccess::Anyone, [sayRadio](std::string channel, std::string voice) {
			sayRadio(game::GameWorld::targetChannel(channel), voice, 0);
		});
		for (const auto& [command, channel, voice] : std::vector<std::tuple<const char*, int, const char*>> {
				 {"Radio.callForMedic", 6, "medic"}, {"Radio.callForAmmo", 1, "need_ammo"},
				 {"Radio.callForRepair", 1, "need_repair"}}) {
			const int c = channel;
			const std::string v = voice;
			// The HUD's codes 0x33..0x35 (the game's slot 0xd8) reach the
			// NametagManager too (0x100f19c0): the local player's own call.
			const int code = v == "medic" ? nametags::CallMedic : v == "need_ammo" ? nametags::CallAmmo : nametags::CallRepair;
			// 0x0042d220: the Spotted event with the call's code (the caller's
			// soldier and team: to the server from a client, handed out by a
			// server, 0x0050e910: the team's name tags), then the game's slot
			// 0x54 (the radio, as slot 0x50 without the spam check).
			bindMethod(console, command, ConsoleAccess::Anyone, [sayRadio, &hudLocal, &joinedServer, &session, &round, c, v, code] {
				net::SpottedInfo call;
				call.message = static_cast<uint32_t>(code);
				if (net::GameClient* server = joinedServer()) {
					if (const net::RemotePlayer* me = server->world().player(server->world().localPlayer())) {
						call.team = me->team;
					}
					server->sendEvent(net::makeSpotted(call));
				} else if (session.hosted && round.localPlayer() >= 0) {
					session.hosted->spotted(call, round.localPlayer());
				}
				sayRadio(c, v, 0, false);
				if (g_nametags) {
					g_nametags->radioCall(hudLocal(), code, true);
				}
			});
		}
		// The crosshair's enemy reported to the squad (0x007c8160: the game's
		// slot 0xd4, 0x0042d110: the spam check, then the Spotted event with
		// message 0x2e and the spotted object's network id) as the rose
		// closes; with its first argument the spotted object's voice
		// (radio.spottedMessage: "apc_spotted", "infantry_spotted", ...; the
		// game's slot 0xd0 then 0x50 on TCTeam; HudMenusComm.h SpotChoice).
		// A joined client picks the object from its own view (its ghost's
		// id goes up); a server takes the reporter's crosshair from its view.
		bindMethod(console, "Radio.sendSpottedMessage", ConsoleAccess::Anyone,
			[&commRoses, &round, &gameLogic, &joinedServer, &session, &hudManager, &commanderScreen, &mapMenuPoint, &hudLocal, spamRefused, sayRadio](bool say, std::string) {
				commRoses.close();
				dice::hfe::menu::SpotChoice choice;
				float mapX = 0, mapZ = 0;
				if (commanderScreen && hudManager.commanderMapZoom() == 2 && mapMenuPoint
					&& hudManager.mapWorldAt(mapMenuPoint->first, mapMenuPoint->second, mapX, mapZ)) {
					const float eye[3] = {mapX, round.terrainHeight(mapX, mapZ) + g_overheadHeight, mapZ};
					const float dir[3] = {0, -1, 0};
					const host::LocalHudWorld local(round, gameLogic);
					const host::HudWorld& world = g_remoteHud ? static_cast<const host::HudWorld&>(*g_remoteHud) : local;
					choice = dice::hfe::menu::spotChoice(world, round,
						dice::hfe::menu::crosshairEnemy(world, round, hudLocal(), eye, dir, g_overheadHeight + 100));
				} else if (net::GameClient* server = joinedServer(); server && g_remoteHud) {
					game::GameWorld::PlayerInfo me;
					if (g_remoteHud->playerInfo(g_remoteHud->localPlayer(), me) && me.alive) {
						const float yaw = me.yaw * 3.14159265f / 180.0f, pitch = me.aimPitch * 3.14159265f / 180.0f;
						const float eye[3] = {me.position.x, me.position.y + 1.6f, me.position.z};
						const float dir[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
						choice = dice::hfe::menu::spotChoice(*g_remoteHud, round,
							dice::hfe::menu::crosshairEnemy(*g_remoteHud, round, me.id, eye, dir));
					}
				} else if (round.localPlayer() >= 0) {
					const host::LocalHudWorld local(round, gameLogic);
					choice = dice::hfe::menu::spotChoice(local, round, round.crosshairTarget(round.localPlayer()));
				}
				if (!spamRefused(false)) {
					net::SpottedInfo spot;
					spot.message = 0x2e;
					if (net::GameClient* server = joinedServer()) {
						if (const net::RemotePlayer* me = server->world().player(server->world().localPlayer())) {
							spot.team = me->team;
						}
						spot.spotted = g_remoteHud && choice.object ? g_remoteHud->ghostOf(choice.object) : 0;
						server->sendEvent(net::makeSpotted(spot));
					} else if (session.hosted && round.localPlayer() >= 0) {
						session.hosted->spotted(spot, round.localPlayer());
					} else if (choice.player >= 0) {
						round.radioSpot(round.localPlayer(), choice.player);
					}
				}
				if (say) {
					sayRadio(1, choice.voice, 0);
				}
			});
		dice::hfe::menu::SquadMenu squadMenu(round, hudManager);
		squadMenu.setLocalizer([&localization](const std::string& id) { return localization.text(id); });
		squadMenu.onMessage = [](const std::string& id) { std::printf("squad menu: %s\n", id.c_str()); };
		squadMenu.bind(console);
		// The chat and kill lines, the say boxes, the squad name box, the map
		// vote and the VOIP list (HudMenusChat.h).
		dice::hfe::menu::HudChat chat(hudManager);
		chat.setLocalizer([&localization](const std::string& id) { return localization.text(id); });
		dice::hfe::menu::SquadNameBox squadNameBox(hudManager);
		squadMenu.typedName = [&squadNameBox] { return squadNameBox.take(); };
		bindMethod(console, "SquadMenu.setSquadNameSelect", ConsoleAccess::Anyone, [&squadNameBox] { squadNameBox.select(); });
		dice::hfe::menu::MapVoteList mapVote(hudManager);
		using Box = dice::hfe::menu::HudChat::Box;
		const auto sayLocal = [&](Box box, const std::string& line) {
			// A line said: to the server, else into our own list.
			const uint32_t channel = box == Box::kTeam ? net::kChatTeam : box == Box::kSquad ? net::kChatSquad : net::kChatGlobal;
			if (box == Box::kPlayer) {
				// A whisper (0x007d9e20): to the server as chat channel 14
				// for the HUD manager's player (+0x48: -1, which nothing in
				// BF2142.exe changes after 0x0075c6b0), and into our own list
				// (its slot 0x158).
				if (session.client) {
					session.client->whisper(line, -1);
				}
				chat.addChat(localization.text("HUD_TEXT_CHAT_WHIPSER"), line, static_cast<int>(net::kChatGlobal),
					dice::hfe::menu::HudChat::Speaker::Neutral);
				return;
			}
			if (session.client) {
				session.client->say(line, channel);
				return;
			}
			game::GameWorld::PlayerInfo me;
			if (round.playerInfo(round.localPlayer(), me)) {
				chat.addChat(me.name, line, static_cast<int>(channel),
					box == Box::kSquad ? dice::hfe::menu::HudChat::Speaker::Squad : dice::hfe::menu::HudChat::Speaker::Team, !me.alive);
			}
		};
		chat.onSay = sayLocal;
		bindMethod(console, "HudManager.enableSayAllChatBox", ConsoleAccess::Anyone, [&chat](bool) { chat.open(Box::kAll); });
		bindMethod(console, "HudManager.enableSayTeamChatBox", ConsoleAccess::Anyone, [&chat](bool) { chat.open(Box::kTeam); });
		bindMethod(console, "HudManager.enableSaySquadChatBox", ConsoleAccess::Anyone, [&chat](bool) { chat.open(Box::kSquad); });
		bindMethod(console, "HudManager.enableSayPlayerChatBox", ConsoleAccess::Anyone, [&chat](bool) { chat.open(Box::kPlayer); });
		// HudManager.addFavouriteServer (0x00760987 → slot 0x434 →
		// 0x00752580 → the HUD information layer's slot 0xa8, 0x00449180):
		// 1 adds the joined server to GeneralSettings' favourites (its slot
		// 0x17c with the address, the port, the name and time()), 0 removes
		// it (slot 0x190); ServerIsFavourite (+0xb24) follows the list.
		bindMethod(console, "HudManager.addFavouriteServer", ConsoleAccess::Anyone, [&](bool add) {
			hudManager.set("ServerIsFavourite", add ? 1 : 0);
			if (add && session.client) {
				std::string host = options.connect;
				int port = 17567;
				if (const size_t colon = host.rfind(':'); colon != std::string::npos) {
					port = std::atoi(host.c_str() + colon + 1);
					host.resize(colon);
				}
				std::string output;
				console.execute("GeneralSettings.addFavouriteServer \"" + host + "\" " + std::to_string(port) + " \""
						+ session.client->world().serverName() + "\" " + std::to_string(static_cast<long long>(std::time(nullptr))),
					output);
			}
		});
		bindMethod(console, "Chat.sayAll", ConsoleAccess::Anyone, [sayLocal](std::string line) { sayLocal(Box::kAll, line); });
		bindMethod(console, "Chat.sayTeam", ConsoleAccess::Anyone, [sayLocal](std::string line) { sayLocal(Box::kTeam, line); });
		// The map vote (LevelsList): the playlist's maps, or the server's.
		bindMethod(console, "levelsList.setVoteMapShow", ConsoleAccess::Anyone, [&mapVote](int show) { mapVote.setShow(show != 0); });
		bindMethod(console, "levelsList.singleClick", ConsoleAccess::Anyone, [&mapVote, &hudManager](int) {
			mapVote.click(hudManager.lastListClick().row, hudManager.lastListClick().column);
		});
		// The votes' HUD lines (0x005740e0 / 0x005725e0), from the server's
		// Vote events.
		dice::hfe::menu::VoteMessages voteMessages;
		voteMessages.localize = localizeWithKeys;
		voteMessages.onLine = [&chat](const std::string& line) { chat.addPersonal(line); };
		// Commander.commanderMutiny (HUD 0x007d7390 → the vote object's
		// 0x00573cb0): a mutiny vote (kind 3) against the team's commander,
		// from the local player, unless one runs or the last start is too
		// recent (else the line, 0x005725e0); a client sends it to the
		// server (Vote, phase 0), a host's goes to its own votes.
		bindMethod(console, "Commander.commanderMutiny", ConsoleAccess::Anyone, [&] {
			game::GameWorld::PlayerInfo me;
			if (!hudPlayer(hudLocal(), me)) {
				return;
			}
			const int commander = g_remoteHud ? g_remoteHud->commander(me.team) : round.commander(me.team);
			if (commander < 0 || !voteMessages.mayStart(3, commander, false)) {
				return;
			}
			if (net::GameClient* server = joinedServer()) {
				server->startVote(3, commander);
			} else if (session.hosted) {
				session.hosted->localVote(me.id, game::Votes::kMutiny, 0, commander, 0);
			}
		});
		squadRequests.onPersonal = [&chat](const std::string& line) { chat.addPersonal(line); };
		spamLine = [&chat](const std::string& line) { chat.addPersonal(line); };
		// The HUD's questions (HudMenusChat.h): an invite to a squad (2/0x10)
		// and the offer of command (2/0x23), answered with Radio.answer and
		// c_GIYes / c_GINo (0x0079e320); their answers (0x0079d6e0).
		dice::hfe::menu::HudQuestions questions;
		// The prompts' '#GIYes#' / '#GINo#' as the bound keys (0x00752b70).
		questions.localize = localizeWithKeys;
		questions.onLine = [&chat](const std::string& line) { chat.addPersonal(line); };
		questions.onQuestion = [&chat, &options](const std::string& line) {
			chat.addQuestion(line);
			if (options.verbose) {
				std::printf("question: %s\n", line.c_str());
			}
		};
		questions.onClear = [&chat] { chat.clearQuestions(); };
		questions.onAnswer = [&](const dice::hfe::menu::HudQuestions::Question& q, bool yes, bool quiet) {
			using Q = dice::hfe::menu::HudQuestions;
			net::GameClient* server = joinedServer();
			if (q.kind == Q::Invite) {
				// Kind 1: yes joins the squad (the squad menu's 0x007a5410:
				// JoinSquad), no clears the invite (squad manager slot 0x30
				// with false: Invite without the leader bit); the leader
				// hears roger_that or negative (TCPlayer, slot 0x40's leader).
				int leader = -1;
				for (const game::GameWorld::PlayerInfo& p : hudPlayers()) {
					game::GameWorld::PlayerInfo me;
					if (hudPlayer(hudLocal(), me) && p.team == me.team && p.squad == q.value && p.squadLeader) {
						leader = p.id;
					}
				}
				if (server) {
					if (yes) {
						server->joinSquad(q.value);
					} else {
						server->invite(server->world().localPlayer(), q.value, false);
					}
				} else if (yes) {
					round.joinSquad(round.localPlayer(), q.value);
				}
				if (!quiet && leader >= 0) {
					sayRadio(14, yes ? "roger_that" : "negative", leader);
				}
			} else if (q.kind == Q::CommanderOffer) {
				// Kind 3: squad manager slot 0x68 (take it) or 0x6c (pass it
				// on) with HUD_HUD_DECLINED_COMMANDER.
				if (!yes && !quiet) {
					chat.addPersonal(localization.text("HUD_HUD_DECLINED_COMMANDER"));
				}
				if (server) {
					server->answerCommanderOffer(yes);
				} else {
					round.answerCommanderOffer(round.localPlayer(), yes);
				}
			}
		};
		bindMethod(console, "Radio.answer", ConsoleAccess::Anyone, [&questions](bool yes) { questions.answer(yes); });
		int localOfferAsked = -1; // the local round's offer to our player, asked once
		const auto askCommanderOffer = [&questions, &localization, &gameLogic](int team) {
			std::string line = localization.text("HUD_HUD_COMMANDER_ACCEPTED");
			if (const size_t at = line.find("#TEAMNAME#"); at != std::string::npos) {
				line.replace(at, 10, localization.text("HUD_TEAMNAME_" + gameLogic.team(team == 1 ? 1 : 2).name));
			}
			questions.ask(dice::hfe::menu::HudQuestions::CommanderOffer, team, line);
		};
		squadRequests.onInfo = [&chat](const std::string& line) { chat.addInfo(line); };
		mapVote.onVote = [&](int map) {
			if (session.client && voteMessages.mayStart(1, map, false)) {
				session.client->startVote(1, map); // a map vote (0x00573df0, kind 1)
			} else if (!session.playlist.empty() && static_cast<size_t>(map) < session.playlist.size()) {
				// Offline the only voter carries it: the playlist goes on there.
				session.current = (static_cast<size_t>(map) + session.playlist.size() - 1) % session.playlist.size();
				session.roundsOnMap = session.roundsPerMap;
			}
		};
		// The scoreboard's boxes (Scoreboard.singleClick → 0x007bc590, by the
		// clicked column's type; the player's id in cell 9): the mute box
		// (column 6, type 7) turns a player's voice on or off; the Manage
		// tab's kick box (column 8, types 10 / 11) starts a kick vote
		// against him (the vote object 0x00572320, its gate 0x005722e0 and
		// start 0x00573f20: not oneself, not the host, none running or
		// within the wait, else the line; a client sends the Vote event,
		// kind 2, a host's goes to its own votes, 0x00573f20's event 0/0xf).
		bindMethod(console, "Scoreboard.singleClick", ConsoleAccess::Anyone, [&](int list) {
			const auto& click = hudManager.lastListClick();
			const std::string id = hudManager.listCell(list, click.row, 9);
			if (id.empty()) {
				return;
			}
			const int player = std::atoi(id.c_str());
			if (click.column == 6 && session.voice) {
				session.voice->client().setMuted(player, !session.voice->client().muted(player));
			}
			if (click.column == 8 && hudManager.number("ToggleManage") != 0
				&& !hudManager.listCell(list, click.row, 8).empty()) {
				if (!voteMessages.mayStartKick(player, false)) {
					return;
				}
				if (net::GameClient* server = joinedServer()) {
					server->startVote(game::Votes::kKick, player);
				} else if (session.hosted) {
					game::GameWorld::PlayerInfo me;
					if (hudPlayer(hudLocal(), me)) {
						session.hosted->localVote(me.id, game::Votes::kKick, 0, player, 0);
					}
				}
			}
		});
		// The commander's zoom buttons (Commander.<asset>Zoom, slots 0x370..
		// 0x380 → 0x398, 0x007d8e00): the view to the asset; on a server
		// the CommanderCam event (0x24: x, the camera's height, z).
		const auto zoomTo = [&](float x, float z) {
			// A local round's camera keeps 80 m inside the terrain (0x005f8170).
			if (!session.client) {
				g_overheadCamera.moveTo(x, z);
				x = g_overheadCamera.x;
				z = g_overheadCamera.z;
			}
			g_commanderView = std::make_pair(x, z);
			hudManager.setCommanderMapZoom(2); // the overhead view (0x007d7f90: the map's zoom 2)
			if (session.client) {
				net::GameEvent cam;
				cam.type = net::kEvCommanderCam;
				const float xyz[3] = {x, 80.0f, z}; // HudManager +0x2d0: 80 until the camera template says
				for (const float v : xyz) {
					uint32_t bits = 0;
					std::memcpy(&bits, &v, 4);
					cam.values.push_back(bits);
				}
				session.client->sendEvent(cam);
			}
		};
		for (const auto& [name, ability] : std::vector<std::pair<std::string, Ability>> {{"artilleryZoom", Ability::Artillery},
				 {"satelliteZoom", Ability::Satellite}, {"uavZoom", Ability::Uav}, {"orbitalStrikeZoom", Ability::OrbitalStrike}}) {
			const Ability tool = ability;
			bindMethod(console, "Commander." + name, ConsoleAccess::Anyone, [&, tool] {
				float x = 0, z = 0;
				if (assetAt(tool, x, z)) {
					zoomTo(x, z);
				}
			});
		}
		bindMethod(console, "Commander.moveTitanZoom", ConsoleAccess::Anyone, [&] {
			if (const world::Object* titan = round.objects().find(round.titanOf(myTeam()))) {
				zoomTo(titan->position().x, titan->position().z);
			}
		});
		// Commander.zoomCommander (0x0077168e -> HUD slot 0x39c, 0x007d7be0;
		// the CommanderZoomFCT button): the map's zoom index steps
		// (0x0077e850(-1, 1): up while below the last, then back to 0; the
		// overhead view at 2), the map's cursor becomes the world's point
		// (slot 0x3a0, 0x007d8ec0) and the camera goes there (slot 0x398:
		// CommanderCam).
		const auto sendCommanderCam = [&](float x, float z) {
			if (!joinedServer()) {
				g_overheadCamera.moveTo(x, z);
				x = g_overheadCamera.x;
				z = g_overheadCamera.z;
			}
			g_commanderView = std::make_pair(x, z);
			if (net::GameClient* server = joinedServer()) {
				net::GameEvent cam;
				cam.type = net::kEvCommanderCam;
				const float xyz[3] = {x, 80.0f, z};
				for (const float v : xyz) {
					uint32_t bits = 0;
					std::memcpy(&bits, &v, 4);
					cam.values.push_back(bits);
				}
				server->sendEvent(cam);
			}
		};
		bindMethod(console, "Commander.zoomCommander", ConsoleAccess::Anyone, [&] {
			const int zoom = hudManager.commanderMapZoom();
			float x = 0, z = 0;
			const auto point = mapMenuPoint.value_or(std::make_pair(hudManager.pointerX(), hudManager.pointerY()));
			const bool at = hudManager.mapWorldAt(point.first, point.second, x, z);
			hudManager.setCommanderMapZoom(dice::hfe::menu::commanderZoomStep(zoom));
			if (at) {
				sendCommanderCam(x, z);
			}
			return true;
		});
		// Commander.detailedView (0x0077161d -> slot 0x394, 0x007d8db0; the
		// Commander rose's DETAILVIEW): HUD +0x2f4 holds the commander screen
		// open (0x00754290 forces its input while set) and keeps the overhead
		// camera's point (+0x2f8..+0x300); leaving the screen sends
		// CommanderCam there and sets the map's zoom 2.
		bindMethod(console, "Commander.detailedView", ConsoleAccess::Anyone, [&] {
			game::GameWorld::PlayerInfo me;
			const bool known = hudPlayer(hudLocal(), me);
			const auto [x, z] = dice::hfe::menu::detailedViewPoint(g_commanderView ? &*g_commanderView : nullptr, known ? &me : nullptr);
			sendCommanderCam(x, z);
			hudManager.setCommanderMapZoom(2);
			commanderScreen = true;
		});
		// Commander.spottedMouseOver <n> (0x00771b67 -> slot 0x450,
		// 0x00751f80): Spotted<i>MouseOver = (i == n) for i 1..11.
		bindMethod(console, "Commander.spottedMouseOver", ConsoleAccess::Anyone,
			[&](int n) { dice::hfe::menu::spottedMouseOver(hudManager, n); });
		// CommanderMenu.sendRadioMessage <voice> <channel> (0x00771fe2 ->
		// slot 0x2c4 -> 0x007a9f60): the message to each selected squad
		// (Game slot 0x50, the spam check on the first only), then no squad
		// shows (+0xac) and state 0x11 goes back to 0x10.
		bindMethod(console, "CommanderMenu.sendRadioMessage", ConsoleAccess::Anyone,
			[&](std::string voice, std::string channel) {
				const int target = game::GameWorld::targetChannel(channel);
				commanderMenu.sendRadioMessage([&](int squad, bool first) { sayRadio(target, voice, squad, first); });
			});
		// SquadManager.joinSquad <squad> / leaveSquad <squad> (0x004192c1 /
		// 0x004193a3 -> Game slots 0xb4 / 0xb8 -> the squad manager's 0x34 /
		// 0x38 with our team and id: a client's JoinSquad / LeaveSquad).
		bindMethod(console, "SquadManager.joinSquad", ConsoleAccess::Anyone, [&](int squad) {
			if (net::GameClient* server = joinedServer()) {
				server->joinSquad(squad);
			} else {
				round.joinSquad(round.localPlayer(), squad);
			}
		});
		const auto leave = [&](int squad) {
			if (net::GameClient* server = joinedServer()) {
				server->leaveSquad(squad);
				return;
			}
			game::GameWorld::PlayerInfo me;
			if (round.playerInfo(round.localPlayer(), me) && me.squad == squad && squad > 0) {
				round.joinSquad(round.localPlayer(), 0);
			}
		};
		bindMethod(console, "SquadManager.leaveSquad", ConsoleAccess::Anyone, leave);
		// Squad.leaveSquad (0x00771bdd -> slot 0x30c, 0x007d6750): out of our
		// squad (slot 0x38) and its invite cleared (slot 0x30, no leader bit).
		bindMethod(console, "Squad.leaveSquad", ConsoleAccess::Anyone, [&] {
			game::GameWorld::PlayerInfo me;
			if (!hudPlayer(hudLocal(), me) || me.squad <= 0) {
				return;
			}
			leave(me.squad);
			if (net::GameClient* server = joinedServer()) {
				server->invite(server->world().localPlayer(), me.squad, false);
			}
		});
		// SquadManager.changeSquadName <name> <bool> (0x004195aa -> slot
		// 0xc4, 0x0042cff0; the bool unused): our squad named, the server
		// refusing a squad that isn't empty (0x004c6720) with 2/0xf. So in
		// effect it names a new squad.
		bindMethod(console, "SquadManager.changeSquadName", ConsoleAccess::Anyone, [&](std::string name, bool) {
			game::GameWorld::PlayerInfo me;
			if (!hudPlayer(hudLocal(), me)) {
				return;
			}
			// Offline the local round's squad has us in it: refused, as
			// the server would.
			if (net::GameClient* server = joinedServer()) {
				server->setSquadName(me.squad, name);
			}
		}, 1);
		// SquadLeader.sendRequestForOrder (0x00770a7e -> slot 0x314,
		// 0x007c7cd0; the SquadLeader rose's needorders): req_new_order to
		// the commander, then the Request event of kind 1 (0x007c7d50).
		bindMethod(console, "SquadLeader.sendRequestForOrder", ConsoleAccess::Anyone, [&] {
			dice::hfe::menu::sendRequestForOrder([&](int channel, const std::string& voice) { sayRadio(channel, voice, 0, true); },
				[&](int kind) {
					if (net::GameClient* server = joinedServer()) {
						server->squadRequest(kind);
					}
				});
		});
		// SquadLeader.issueSquadRequest (0x00770b5f -> slot 0x3b0,
		// 0x007d6820): the armed order (+0x284) at the rose's point
		// (+0x28c), through sendOrder's handler.
		bindMethod(console, "SquadLeader.issueSquadRequest", ConsoleAccess::Anyone, [&] {
			dice::hfe::menu::issueSquadRequest(armedOrder.has_value(), rosePoint, [&](float x, float z) {
				if (placeAt) {
					leaderOrder = true;
					placeAt(x, z);
				}
			});
		});
		// The kills seen since the last frame (players' counters), for the
		// kill messages (0x00759bf0).
		std::map<int, std::array<int, 3>> killCounts;
		const auto trackKills = [&] {
			const auto players = round.playerList();
			std::vector<int> victims, killers;
			for (const auto& p : players) {
				auto& seen = killCounts[p.id];
				if (p.deaths > seen[1]) {
					victims.push_back(p.id);
				}
				if (p.kills > seen[0] || p.teamKills > seen[2]) {
					killers.push_back(p.id);
				}
				seen = {p.kills, p.deaths, p.teamKills};
			}
			game::GameWorld::PlayerInfo me;
			round.playerInfo(round.localPlayer(), me);
			for (const int victim : victims) {
				game::GameWorld::PlayerInfo v, k;
				round.playerInfo(victim, v);
				const bool byOther = !killers.empty() && round.playerInfo(killers.front(), k) && k.id != victim;
				std::string weapon;
				if (byOther) {
					if (const world::Object* vehicle = k.vehicle ? round.objects().find(k.vehicle) : nullptr) {
						const auto it = vehicle->objectTemplate()->settings().find("vehicleHud.hudName");
						weapon = it != vehicle->objectTemplate()->settings().end() ? it->second : "";
					} else {
						for (const auto& [slot, name] : k.carried) {
							const world::ObjectTemplate* t = slot == k.inHandSlot ? templates.find(name) : nullptr;
							const auto it = t ? t->settings().find("weaponHud.hudName") : decltype(t->settings().end()) {};
							if (t && it != t->settings().end()) {
								weapon = it->second;
							}
						}
					}
					weapon.erase(std::remove(weapon.begin(), weapon.end(), '"'), weapon.end());
				}
				const auto speaker = !byOther                ? dice::hfe::menu::HudChat::Speaker::Neutral
					: k.team == me.team && k.squad && k.squad == me.squad ? dice::hfe::menu::HudChat::Speaker::Squad
					: k.team == me.team                     ? dice::hfe::menu::HudChat::Speaker::Team
															: dice::hfe::menu::HudChat::Speaker::Enemy;
				chat.addKill(byOther ? k.name : std::string(), v.name, weapon, byOther && k.team == v.team, speaker);
				// HUD event 3/14 (0x0075a07a): the local, non-commander killer, enemy team only.
				if (byOther && k.id == round.localPlayer() && k.team != v.team && !k.commander) {
					chat.addKillScore(v.name);
				}
				if (victim == round.localPlayer()) {
					chat.setKilledBy(byOther ? k.name : v.name);
				}
			}
			if (me.alive) {
				chat.setKilledBy("");
			}
		};
		// A click on the map with a tool or order armed: used there.
		const auto commanderClick = [&](float x, float y) {
			float worldX = 0, worldZ = 0;
			// The mini-map tab (0x007e7d00, button 1): the commander's camera
			// to the point under the click (slot 0x398).
			if (commanderScreen && hudManager.miniMapWorldAt(x, y, worldX, worldZ)) {
				sendCommanderCam(worldX, worldZ);
				return true;
			}
			if (moveTitanArmed && hudManager.mapWorldAt(x, y, worldX, worldZ)) {
				moveTitanArmed = false;
				if (net::GameClient* server = joinedServer()) {
					server->commanderTool(net::CommanderToolUse::MoveTitan, worldX, worldZ); // Titan, kind 1 (0x0050f1f0)
					return true;
				}
				const auto answer = round.moveTitan(myTeam(), worldX, worldZ);
				std::printf("titan move: %s\n", answer == game::GameWorld::TitanMove::Accepted ? "AUTO_TITAN_CC_moveaccepted"
						: answer == game::GameWorld::TitanMove::Unable                  ? "AUTO_TITAN_CC_unablemove"
																						: "refused");
				if (answer == game::GameWorld::TitanMove::Accepted || answer == game::GameWorld::TitanMove::Unable) {
					round.hearRadio(round.localPlayer(), 2,
						answer == game::GameWorld::TitanMove::Accepted ? "AUTO_TITAN_CC_moveaccepted" : "AUTO_TITAN_CC_unablemove");
				}
				return true;
			}
			if ((!armedTool && !armedOrder) || !hudManager.mapWorldAt(x, y, worldX, worldZ)) {
				return false;
			}
			if (armedTool) {
				// 0x007d8370: a UAV sent lets 0x16 UAVplaced through and holds
				// 0x13 place_UAV again (neither has a case in 0x00646cb0, so
				// neither shows).
				const bool used = !toolRefused(*armedTool) && useTool(*armedTool, worldX, worldZ);
				if (used && g_hudSound) {
					g_hudSound("mapAction");
				}
				if (used && *armedTool == Ability::Uav) {
					helpPopups.enableTutorial(0x16);
					helpPopups.disableTutorial(0x13);
				}
			} else {
				if (leaderOrder) {
					// The leader's own squad's (a joined game's: IssueSquadOrder store 0).
					game::GameWorld::PlayerInfo me;
					if (net::GameClient* server = joinedServer()) {
						if (hudPlayer(hudLocal(), me) && me.squadLeader) {
							server->issueOrder(0, me.squad, static_cast<int>(*armedOrder),
								{worldX, round.terrainHeight(worldX, worldZ), worldZ});
						}
					} else {
						round.issueSquadLeaderOrder(round.localPlayer(), *armedOrder, worldX, worldZ);
					}
				} else if (!commanderMenu.selected().empty()) {
					// The squads selected on the commander screen (0x007d6ab0).
					for (const int squad : commanderMenu.selected()) {
						orderSquad(squad, *armedOrder, worldX, worldZ);
					}
				} else if (const int squad = nearestSquad(worldX, worldZ)) {
					orderSquad(squad, *armedOrder, worldX, worldZ);
				}
			}
			armedTool.reset();
			armedOrder.reset();
			return true;
		};
		placeAt = commanderClick;
		// The map's right click (Bf2MenuMapNode's event handler 0x00788a90,
		// its case for event 0x15 at 0x00788c00):
		// OrderMenuPosX/Y the click, MinimapRMBMenuState what is under it
		// (0x00782d90: 2 / 3 the player's / the enemy's Titan, 4 / 5 / 6 a
		// control point of his team / the enemy's / neutral, 0 one of his
		// commander's assets, 7 nothing), then the menu for his role: the
		// commander's (state 0x16), a squad leader's (0x15), the others'
		// (0x14); the HUD lists stack each menu's rows there.
		const auto mapRightClick = [&](float x, float y) {
			float worldX = 0, worldZ = 0;
			game::GameWorld::PlayerInfo me;
			if (!hudManager.mapWorldAt(x, y, worldX, worldZ) || !hudPlayer(hudLocal(), me)) {
				g_mapMenu = 0;
				mapMenuPoint.reset();
				return false;
			}
			hudManager.set("OrderMenuPosX", std::floor(x));
			hudManager.set("OrderMenuPosY", std::floor(y));
			// What is under it: within 16 pixels of the map (as its icons).
			float sx0 = 0, sz0 = 0, sx1 = 0, sz1 = 0;
			hudManager.mapWorldAt(x - 16, y - 16, sx0, sz0);
			hudManager.mapWorldAt(x + 16, y + 16, sx1, sz1);
			const float reach = std::max(std::abs(sx1 - sx0), std::abs(sz1 - sz0)) * 0.5f;
			const auto near = [&](float px, float pz) {
				return (px - worldX) * (px - worldX) + (pz - worldZ) * (pz - worldZ) < reach * reach;
			};
			int state = 7;
			for (const int side : {me.team, 3 - me.team}) {
				if (const world::Object* titan = round.objects().find(round.titanOf(side))) {
					if (state == 7 && near(titan->position().x, titan->position().z)) {
						state = side == me.team ? 2 : 3;
					}
				}
			}
			for (const auto& point : round.controlPointList()) {
				if (state == 7 && near(point.position.x, point.position.z)) {
					state = point.team == me.team ? 4 : point.team != 0 ? 5 : 6;
				}
			}
			for (const Ability tool : {Ability::Artillery, Ability::Uav, Ability::Satellite, Ability::OrbitalStrike}) {
				float ax = 0, az = 0;
				if (state == 7 && assetAt(tool, ax, az) && near(ax, az)) {
					state = 0;
				}
			}
			hudManager.set("MinimapRMBMenuState", static_cast<float>(state));
			g_mapMenu = me.commander ? 0x16 : me.squadLeader ? 0x15 : 0x14;
			mapMenuPoint = std::make_pair(x, y);
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
		const auto loadAnimationSystem = [&](const std::string& path) {
			std::string output;
			if (!valueHoldersLoaded) {
				valueHoldersLoaded = true;
				console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, output);
			}
			return animationSystems.get(path, [&] { console.run(path, {}, output); });
		};
		round.setAnimationSystems(&animationSystems, loadAnimationSystem);
		// The player's camera: first person by default (--camera-mode third
		// for the chase view), its first-person systems built the same way.
		PlayerCamera playerCamera(round, animationSystems, loadAnimationSystem);
		playerCamera.firstPerson = playerCamera.spawnFirstPerson = options.cameraMode != "third";
		// FreeCam.* (the HUD manager's slots 0x130 / 0x134, 0x00752050 /
		// 0x00752150): the next player to follow, the free and following
		// cameras swapped.
		// For a spectator on a server each change goes up as JoinAsSpectator
		// (kind 6 with the player followed; the new mode).
		bindMethod(console, "FreeCam.togglePlayer", ConsoleAccess::Anyone, [&playerCamera, &session](bool) {
			playerCamera.nextPlayer();
			if (session.client && playerCamera.spectator) {
				session.client->spectatorCamera(net::JoinAsSpectatorInfo::kCameraFollow, playerCamera.followedPlayer());
			}
		});
		bindMethod(console, "FreeCam.toggleCamera", ConsoleAccess::Anyone, [&playerCamera, &session] {
			if (const int mode = playerCamera.toggleSpectatorCamera(); mode != 0 && session.client) {
				session.client->spectatorCamera(mode, playerCamera.followedPlayer());
			}
		});
		if (session.admin) {
			// A hosted or local game's own sv.* camera rules.
			playerCamera.allowExternalViews = session.admin->settings.allowExternalViews;
			playerCamera.allowNoseCam = session.admin->settings.allowNoseCam;
			playerCamera.allowFreeCam = session.admin->settings.allowFreeCam;
		}
		// The effect bundles the world's objects carry (engines, exhausts,
		// trails), following them (effects::ObjectEffects).
		effects::EffectManager objectEffects(templates, 0x2f6b1d3u);
		effects::ObjectEffects objectEffectBundles(templates);
		// Bone parts carry their bundles as the renderer draws them.
		objectEffectBundles.setPartBone([&round](uint32_t object, const std::string& part, std::array<float, 16>& local) {
			return round.partBone(object, part, local);
		});
		// The wheel and draft bundles' ground: the water, else what the
		// projectiles' collision meets (its point taken on the terrain under
		// it, or the ray's middle on an object), and the materials' effect.
		objectEffectBundles.setGround(
			[&round](const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore, physics::Vec3& hit,
				uint32_t& material) {
				float sea = 0;
				if (round.waterLevel(sea) && from.y >= sea && to.y < sea) {
					const float t = (from.y - sea) / std::max(from.y - to.y, 1e-4f);
					const float ground = round.terrainHeight(from.x + (to.x - from.x) * t, from.z + (to.z - from.z) * t);
					if (ground < sea) {
						hit = from + (to - from) * t;
						material = 1; // the water's (MaterialManager's cells x/1: the *_water effects)
						return true;
					}
				}
				if (!round.groundMaterialAlong(from, to, ignore, material)) {
					return false;
				}
				const float ground = round.terrainHeight(to.x, to.z);
				hit = ground >= to.y && ground <= from.y ? physics::Vec3 {to.x, ground, to.z} : (from + to) * 0.5f;
				return true;
			},
			[&materials](uint32_t bundle, uint32_t ground) {
				const physics::MaterialCell* cell = materials.cell(bundle, ground);
				return cell && !cell->effectTemplates.empty() ? cell->effectTemplates[0] : std::string();
			});
		client::RemoteStrikes remoteStrikes; // a joined client's strikes (RemoteGuns.h)
		std::map<const world::ObjectTemplate*, bool> projectileEffectTemplates;
		std::map<uint32_t, uint32_t> projectileEffectIds;
		uint32_t nextProjectileEffectId = 0x80000000u;
		const auto stepObjectEffects = [&] {
			std::vector<effects::ObjectEffects::ObjectState> states;
			states.reserve(objects.objects().size());
			std::set<uint32_t> usedEffectIds;
			const std::map<uint32_t, uint32_t> fired = round.takeObjectEffectEvents();
			auto weaponFires = round.takeObjectWeaponFires();
			for (const auto& object : objects.objects()) {
				usedEffectIds.insert(object->id());
				effects::ObjectEffects::ObjectState state;
				state.id = object->id();
				state.objectTemplate = object->objectTemplate();
				state.position = {object->position().x, object->position().y, object->position().z};
				state.rotation = physics::Mat3::fromYawPitchRoll(
					{object->rotation().x, object->rotation().y, object->rotation().z});
				// A player in a control object (event 4; soldiers get none).
				state.occupied = state.objectTemplate && state.objectTemplate->typeName() != "Soldier"
					&& round.playerOf(state.id) >= 0;
				// The cold breath (0x00588830): not the local player's own
				// soldier seen in first person, not with a kit that has
				// disableBreathEffect (+0x384), not in a vehicle (+0x264).
				state.coldBreath = coldBreath.useColdBreath && state.objectTemplate
					&& state.objectTemplate->typeName() == "Soldier"
					&& !(state.id == round.playerObject() && playerCamera.firstPerson);
				if (state.coldBreath) {
					game::GameWorld::PlayerInfo info;
					const int player = round.playerOf(state.id);
					if (player >= 0 && round.playerInfo(player, info) && info.soldier == state.id) {
						const world::Object* kit = info.kit ? objects.find(info.kit) : nullptr;
						bool disabled = false;
						if (kit && kit->objectTemplate()) {
							const auto& kitSettings = kit->objectTemplate()->settings();
							const auto it = kitSettings.find("disableBreathEffect");
							disabled = it != kitSettings.end() && std::atoi(it->second.c_str()) != 0;
						}
						state.coldBreath = info.vehicle == 0 && !disabled;
					}
				}
				// A soldier seated in a vehicle: his depth-gated bundles
				// (e_sold_swim) stay off (0x0061a760).
				if (state.objectTemplate && state.objectTemplate->typeName() == "Soldier") {
					game::GameWorld::PlayerInfo seat;
					const int player = round.playerOf(state.id);
					state.seated = player >= 0 && round.playerInfo(player, seat) && seat.soldier == state.id && seat.vehicle != 0;
				}
				// A vehicle's wheels and throttle (the wheel bundles' contact
				// and FromEngine).
				if (const game::Vehicle* vehicle = round.vehicle(state.id)) {
					state.engine = state.occupied ? std::min(std::fabs(vehicle->input().throttle), 1.0f) : 0.0f;
					const physics::Vec3 up = state.rotation.rows[1];
					for (const game::Vehicle::Wheel& wheel : vehicle->wheels()) {
						effects::ObjectEffects::ObjectState::WheelContact contact;
						contact.spring = wheel.spring;
						contact.contact = wheel.contact;
						contact.point = state.position + state.rotation.transform(wheel.mount) + up * (wheel.compression - wheel.radius);
						state.wheels.push_back(contact);
					}
				}
				// The local player's vehicle seen from inside (the inside or
				// nose view): its bundles' first-person form, unless it or his
				// seat is neverDrawAs1p (the Titans) (0x006187c0).
				state.firstPerson = state.occupied
					&& (playerCamera.mode() == game::camera::Mode::Inside || playerCamera.mode() == game::camera::Mode::Nose)
					&& round.firstPersonEffects(state.id);
				if (!fired.empty()) {
					const auto it = fired.find(state.id);
					state.effects = it != fired.end() ? it->second : 0;
				}
				if (const auto it = weaponFires.find(state.id); it != weaponFires.end()) {
					for (const auto& fire : it->second) {
						state.firedWeapons.push_back({fire.weapon, fire.seat});
					}
				}
				states.push_back(state);
			}
			// GenericProjectile instances live in GameWorld's projectile list,
			// not ObjectManager, so ObjectEffects never sees their shipped
			// EffectBundle children unless they are represented here. Keep only
			// templates that actually own effects (most bullets have none), and
			// map each live round's stable serial to a free effect-anchor ID.
			std::set<uint32_t> activeProjectileEffectSerials;
			for (const game::Projectile& projectile : round.projectiles()) {
				if (!projectile.alive || !projectile.settings) {
					continue;
				}
				const world::ObjectTemplate* projectileTemplate = templates.find(projectile.settings->name);
				if (!projectileTemplate) {
					continue;
				}
				auto [hasEffects, inserted] = projectileEffectTemplates.emplace(projectileTemplate, false);
				if (inserted) {
					hasEffects->second = !effects::ObjectEffects::bundlesOf(templates, *projectileTemplate).empty();
				}
				if (!hasEffects->second) {
					continue;
				}
				activeProjectileEffectSerials.insert(projectile.serial);
				auto effectId = projectileEffectIds.find(projectile.serial);
				if (effectId == projectileEffectIds.end() || usedEffectIds.count(effectId->second)) {
					while (usedEffectIds.count(nextProjectileEffectId)) {
						++nextProjectileEffectId;
					}
					if (effectId == projectileEffectIds.end()) {
						effectId = projectileEffectIds.emplace(projectile.serial, nextProjectileEffectId++).first;
					} else {
						effectId->second = nextProjectileEffectId++;
					}
				}
				usedEffectIds.insert(effectId->second);
				effects::ObjectEffects::ObjectState state;
				state.id = effectId->second;
				state.objectTemplate = projectileTemplate;
				state.position = {projectile.position.x, projectile.position.y, projectile.position.z};
				state.velocity = {projectile.velocity.x, projectile.velocity.y, projectile.velocity.z};
				const float horizontal = std::sqrt(projectile.velocity.x * projectile.velocity.x
					+ projectile.velocity.z * projectile.velocity.z);
				constexpr float radiansToDegrees = 57.29577951308232f;
				state.rotation = physics::Mat3::fromYawPitchRoll({
					std::atan2(projectile.velocity.x, projectile.velocity.z) * radiansToDegrees,
					-std::atan2(projectile.velocity.y, horizontal) * radiansToDegrees, 0});
				states.push_back(std::move(state));
			}
			for (auto it = projectileEffectIds.begin(); it != projectileEffectIds.end();) {
				if (!activeProjectileEffectSerials.count(it->first)) {
					it = projectileEffectIds.erase(it);
				} else {
					++it;
				}
			}
			objectEffectBundles.update(states, objectEffects,
				[&round](uint32_t object, const std::string& part, physics::Vec3& offset, physics::Vec3& rotation) {
					world::Vec3 o, r;
					if (!round.partPose(object, part, o, r)) {
						return false;
					}
					offset = {o.x, o.y, o.z};
					rotation = {r.x, r.y, r.z};
					return true;
				});
			// The commanders' strike firearms, fired where 0x005a0bf0 moved
			// them: event 0 to their children there (the EMP strike's
			// GEB_emp_strike), turned as the firearm sits in its asset. A
			// host tells its clients of each (HostedGame::strikeFired); a
			// joined client fires the asset's firearm as the server's
			// event says (client::RemoteStrikes, 0x0050eeb0).
			const auto strikeAt = [&](uint32_t assetId, const std::string& fireArmName, const physics::Vec3& at) {
				const world::ObjectTemplate* fireArm = templates.find(fireArmName);
				const world::Object* asset = objects.find(assetId);
				if (!fireArm) {
					return;
				}
				physics::Mat3 turn;
				if (asset && asset->objectTemplate()) {
					turn = physics::Mat3::fromYawPitchRoll({asset->rotation().x, asset->rotation().y, asset->rotation().z});
					for (const world::ChildTemplate& child : asset->objectTemplate()->children()) {
						if (child.name.size() == fireArmName.size()
							&& std::equal(child.name.begin(), child.name.end(), fireArmName.begin(), [](char a, char b) {
								   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
							   })) {
							turn = physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z}) * turn;
							break;
						}
					}
				}
				objectEffectBundles.weaponFiredAt(*fireArm, at, turn, objectEffects);
			};
			for (const game::GameWorld::StrikeFired& strike : round.takeStrikeFires()) {
				if (session.hosted) {
					session.hosted->strikeFired(strike);
				}
				strikeAt(strike.asset, strike.fireArm, strike.at);
			}
			for (const client::RemoteStrikes::Round& strike : remoteStrikes.step(1.0f / 30.0f)) {
				strikeAt(strike.asset, strike.fireArm, strike.at);
			}
			// The WindManager's local winds: the objects' WindAffectors.
			windSettings.localWinds = objectEffectBundles.localWinds();
			round.effects().appendLocalWinds(windSettings.localWinds); // the explosions'
			objectEffects.update(1.0f / 30.0f);
		};
		// Sound, once a tick (GameAudio.h): the listener at the camera, what
		// the round and the effects started, then the manager's update.
		GameAudio gameAudio(round, objects, templates, sounds, gameSounds, options.dedicated ? nullptr : &voices);
#if BF2142_HAS_STEAM_AUDIO
		// Enhanced mode: the 3D sounds' occlusion through the level's static
		// meshes (their soldier LOD; not the terrain).
		sound::SteamAudioOcclusion steamAudio; // built once the round's static collision is (startRound)
#endif
		// The calls the local player hears go on the callers' name tags
		// (events 0x33..0x37, 0x100f19c0; a man-down player's "medic" is the
		// revive call).
		// The radio's text (0x0042d7f0 -> 0x0042bef0): a sender's as his
		// chat line (slot 0x158, ChatList 0x007b4cd0), else a game line.
		gameAudio.onRadioText = [&hudPlayer, &hudLocal, &chat, &localization](int sender, int, const std::string& id) {
			const std::string line = localization.text(id);
			game::GameWorld::PlayerInfo from, me;
			if (sender < 0 || !hudPlayer(sender, from)) {
				chat.addInfo(line);
				return;
			}
			hudPlayer(hudLocal(), me);
			using Speaker = dice::hfe::menu::HudChat::Speaker;
			const Speaker speaker = from.commander ? Speaker::Commander
				: from.team == me.team && from.squad && from.squad == me.squad ? Speaker::Squad
				: from.team == me.team ? Speaker::Team : Speaker::Enemy;
			chat.addChat(from.name, line, 0, speaker, !from.alive);
		};
		// The name tags' calls from the radio heard: the rewrite's stand-in
		// for the bots' (theirs don't go through the game's slot 0xd8); a
		// joined game's players' come with the Spotted event's message.
		gameAudio.onHeard = [&hudPlayer, &joinedServer](int sender, const std::string& voice) {
			if (!g_nametags || sender < 0) {
				return;
			}
			game::GameWorld::PlayerInfo p;
			const bool known = hudPlayer(sender, p);
			if (joinedServer() && (!known || !p.ai)) {
				return;
			}
			const bool down = known && p.manDown;
			const int code = voice == "medic" ? (down ? nametags::CallRevive : nametags::CallMedic)
				: voice == "need_ammo"        ? nametags::CallAmmo
				: voice == "need_repair"      ? nametags::CallRepair
											  : 0;
			if (code) {
				g_nametags->radioCall(sender, code, false);
			}
		};
		// The indoor level's rays (0x007fa100) against the level, the hit's
		// distance found by halving.
		sounds.setRayTest([&round](const sound::Vec3& from, const sound::Vec3& to, float& distance) {
			const physics::Vec3 a {from.x, from.y, from.z}, b {to.x, to.y, to.z};
			if (round.lineOfSight(a, b)) {
				return false;
			}
			float lo = 0, hi = 1;
			for (int i = 0; i < 5; ++i) {
				const float mid = (lo + hi) * 0.5f;
				(round.lineOfSight(a, a + (b - a) * mid) ? lo : hi) = mid;
			}
			distance = (to - from).length() * hi;
			return true;
		});
		// The obstruction rays (0x007fa350's line test): the first object
		// met, a Titan counting as over 50 m across (slot 0x90).
		sounds.setObstructionTest([&round](const sound::Vec3& from, const sound::Vec3& to, uint32_t ignore,
									  sound::SoundManager::RayHit& hit) {
			uint32_t object = 0;
			if (!round.segmentHit({from.x, from.y, from.z}, {to.x, to.y, to.z}, ignore, hit.fraction, object)) {
				return false;
			}
			hit.root = object;
			hit.large = object != 0 && round.isTitan(object);
			return true;
		});
		// Leaving the round: its loops stop and the rays no longer reach it.
		struct RoundSounds {
			sound::SoundManager& manager;
			sound::GameSounds& sounds;
			~RoundSounds()
			{
				manager.setObstructionTest({});
				manager.setRayTest({});
				manager.setMuted(4, false);
				sounds.stopAll();
			}
		} roundSounds {sounds, gameSounds};
		double soundSeconds = 0;
		int soundTicks = 0;
		const auto stepSounds = [&] {
			if (options.dedicated) {
				return;
			}
			const auto started = std::chrono::steady_clock::now();
			GameAudio::Camera camera;
			PlayerCamera::View eye;
			game::GameWorld::PlayerInfo me;
			const bool alive = round.playerInfo(round.localPlayer(), me);
			float yaw = alive ? me.yaw * 3.14159265f / 180.0f : 0, pitch = alive ? me.aimPitch * 3.14159265f / 180.0f : 0;
			if (alive && playerCamera.firstPerson && playerCamera.view(eye, 4.0f / 3.0f)) {
				camera.position = {eye.position[0], eye.position[1], eye.position[2]};
				yaw = eye.yaw;
				pitch = eye.pitch;
				camera.firstPerson = true;
			} else if (playerCamera.externalView(eye, 4.0f / 3.0f)) {
				camera.position = {eye.position[0], eye.position[1], eye.position[2]};
				yaw = eye.yaw;
				pitch = eye.pitch;
			} else if (const world::Object* at = round.playerVehicle() ? round.playerVehicle() : round.player()) {
				// The chase camera behind (as the screenshot frames it).
				camera.position = {at->position().x - std::sin(yaw) * 5, at->position().y + 2.5f,
					at->position().z - std::cos(yaw) * 5};
			}
			camera.forward = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
			camera.up = {-std::sin(yaw) * std::sin(pitch), std::cos(pitch), -std::cos(yaw) * std::sin(pitch)};
			if (alive) {
				camera.velocity = {me.velocity.x, me.velocity.y, me.velocity.z};
			}
			if (commanderScreen && hudManager.commanderMapZoom() == 2) {
				render::OverheadView view;
				view.x = hudManager.overheadX();
				view.z = hudManager.overheadZ();
				view.height = g_overheadHeight;
				float origin[2] = {0, 0}, extent = 0, xs[5], zs[5], heights[5] = {};
				const auto* ground = heightmaps.primary();
				if (ground) {
					origin[0] = ground->originX();
					origin[1] = ground->originZ();
					extent = ground->worldWidth();
				}
				render::overheadGroundPoints(view, origin, extent, xs, zs);
				for (int i = 0; ground && i < 5; ++i) heights[i] = ground->heightAt(xs[i], zs[i]);
				float sea = 0;
				round.waterLevel(sea);
				const auto at = render::overheadCameraPosition(view, origin, extent, heights, sea);
				camera.position = {at.x, at.y, at.z};
				camera.forward = {0, -1, 0};
				camera.up = {0, 0, 1};
				camera.velocity = {};
				camera.firstPerson = false;
			}
			for (const auto& sound : playerCamera.takeSounds()) {
				gameAudio.effectSound(sound.name, {sound.position.x, sound.position.y, sound.position.z});
			}
			for (const auto& sound : objectEffects.takeSounds()) {
				gameAudio.effectSound(sound.name, {sound.position.x, sound.position.y, sound.position.z});
			}
			// The bundles' looping start sounds, at their sound intensity.
			for (const auto& loop : objectEffects.bundleLoops()) {
				gameAudio.effectLoop(loop.key, loop.name, {loop.position.x, loop.position.y, loop.position.z},
					loop.intensity);
			}
			for (const auto& loop : round.effects().bundleLoops()) { // the armour's (burning aircraft)
				gameAudio.effectLoop(loop.key | (1ull << 48), loop.name, {loop.position.x, loop.position.y, loop.position.z},
					loop.intensity);
			}
			gameAudio.tick(camera);
			soundSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			++soundTicks;
		};
		// The HUD's map: the level's ingameMap.dds over the primary terrain.
		if (const terrain::Heightmap* ground = heightmaps.primary(); ground && ground->loaded()) {
			hudManager.setMap("Levels/" + options.level + "/Hud/Minimap/ingameMap.dds", ground->originX(), ground->originZ(),
				ground->worldWidth(), ground->worldDepth());
			combatAreaLayer.setLevel(options.level, ground->originX(), ground->originZ(), ground->worldWidth(),
				ground->worldDepth());
			g_overheadCamera.originX = ground->originX();
			g_overheadCamera.originZ = ground->originZ();
			g_overheadCamera.width = ground->worldWidth();
			g_overheadCamera.depth = ground->worldDepth();
		}
		if (const terrain::Heightmap* ground = heightmaps.primary(); ground && ground->loaded()) {
			physics::Ground surface;
			surface.height = [ground](float x, float z) { return ground->heightAt(x, z); };
			surface.raycast = [ground](const physics::Vec3& from, const physics::Vec3& to, float& fraction, physics::Vec3& point, physics::Vec3& normal) {
				std::array<float, 3> n {}, p {};
				if (!ground->raycast({from.x, from.y, from.z}, {to.x, to.y, to.z}, fraction, n, &p)) return false;
				point = {p[0], p[1], p[2]};
				normal = {n[0], n[1], n[2]};
				return true;
			};
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
		// The game logic's supply drop settings (GameLogicInit.con).
		round.commanderSettings.supplyDropHeight = gameLogic.supplyDropHeight;
		round.commanderSettings.supplyDropNumSecsToLive = gameLogic.supplyDropNumSecsToLive;
		round.startRound();
#if BF2142_HAS_STEAM_AUDIO
		if (soundEngine && sound::enhancedAudioEnabled()) {
			std::vector<sound::Vec3> corners;
			corners.reserve(round.staticCollision().triangles() * 3);
			round.staticCollision().forEachFace([&corners](const physics::Vec3& a, const physics::Vec3& b, const physics::Vec3& c) {
				corners.insert(corners.end(), {{a.x, a.y, a.z}, {b.x, b.y, b.z}, {c.x, c.y, c.z}});
			});
			if (steamAudio.build(corners)) {
				sound::setEnhancedOcclusion(&steamAudio);
			}
		}
#endif
		// --host: the round served to clients (HostedGame.h).
		host::HostedGame* hosted = nullptr;
		if (options.host && !session.client) {
			host::HostSetup setup;
			setup.level = options.level;
			setup.gameMode = options.gameMode;
			setup.mapSize = options.mapSize;
			setup.maxPlayers = options.maxPlayers;
			setup.gamePort = options.hostPort;
			setup.queryPort = options.queryPort;
			setup.contentRoot = options.gameDir; // pure with the mod's std_archive.md5 (Net/ContentCheck.h)
			setup.modDirectory = options.modPath;
			if (options.requireLogin) {
				// The retail pre-authorisation against the configured
				// services' CD-key and stats hosts (the local emulators).
				const net::OnlineEndpoints& e = net::onlineConfig().endpoints;
				setup.login.required = true;
				setup.login.cdKeyHost = e.cdkey.host;
				setup.login.cdKeyPort = e.cdkey.port;
				setup.login.statsHost = e.stella.host;
				setup.login.statsPort = e.stella.port;
			}
			if (session.admin) {
				const net::ServerSettings& sv = session.admin->settings;
				setup.serverName = sv.serverName;
				setup.password = sv.password;
				setup.settings = &sv;
				setup.maps = &session.admin->maps;
				setup.bans = &session.admin->bans;
				setup.reserved = &session.admin->reserved;
			}
			// The terrain the teams' overhead cameras stay over (0x005f8170).
			const auto overheadTerrain = [&heightmaps](host::HostedGame& host) {
				if (const terrain::Heightmap* primary = heightmaps.primary(); primary && primary->loaded()) {
					host.setTerrainExtent(primary->originX(), primary->originZ(),
						std::min(primary->worldWidth(), primary->worldDepth()));
				}
			};
			if (session.hosted) {
				// The next map on the same connections.
				session.hosted->changeLevel(round, gameLogic, templates, setup);
				session.hosted->setStaticObjects(levelObjects);
				overheadTerrain(*session.hosted);
				hosted = session.hosted.get();
			} else {
				session.hosted = std::make_unique<host::HostedGame>(round, gameLogic, templates, setup);
				session.hosted->setStaticObjects(levelObjects);
				overheadTerrain(*session.hosted);
				session.hosted->onLog = [](const std::string& line) { std::printf("host: %s\n", line.c_str()); };
				if (session.hosted->start()) {
					hosted = session.hosted.get();
					std::printf("hosting %s (%s, %d) on 127.0.0.1:%u, LAN query port %u\n", options.level.c_str(),
						options.gameMode.c_str(), options.mapSize, hosted->gamePort(), options.queryPort);
				} else {
					std::fprintf(stderr, "cannot host on port %u\n", options.hostPort);
					session.hosted.reset();
				}
			}
		}
		// The round goes with this run: the server waits for the next one.
		struct Detach {
			host::HostedGame* hosted;
			~Detach()
			{
				if (hosted) {
					hosted->detach();
					hosted->onRemoteCommand = nullptr; // this round's scripts
					hosted->onChat = nullptr;
					hosted->onRequestStored = nullptr;
					hosted->onSpotted = nullptr;
					hosted->onVoteReply = nullptr;
					hosted->radioIndex = nullptr;
					hosted->radioVoice = nullptr;
				}
			}
		} detachHosted {hosted};
		if (hosted) {
			// The host's own player: a squad's request for him as commander
			// (2/0x1d), and his vote object (the replies, 0x005740e0), his
			// map votes going to the server's votes as a client's would.
			// The radio's message table (the voices' indices, both ways).
			hosted->radioIndex = [&voices](const std::string& voice) { return voices.message(voice); };
			hosted->radioVoice = [&voices](int index) { return voices.messageName(index); };
			// The Spotted event's message for the host's own player (0x0050e910
			// posts it there): a client's radio call on its name tag.
			hosted->onSpotted = [hosted, &round](const net::SpottedInfo& spot) {
				const int caller = hosted->playerOfGhost(spot.spotter);
				if (g_nametags && caller >= 0 && caller != round.localPlayer() && spot.message >= 0x33 && spot.message <= 0x37) {
					g_nametags->radioCall(caller, static_cast<int>(spot.message), false);
				}
			};
			hosted->onRequestStored = [&squadRequests, &round](int team, int squad, int kind) {
				if (round.commander(team) == round.localPlayer() && round.localPlayer() >= 0) {
					squadRequests.received(team, squad, kind);
				}
			};
			voteMessages.localPlayer = round.localPlayer();
			voteMessages.playerName = [&round](int id) {
				game::GameWorld::PlayerInfo p;
				return round.playerInfo(id, p) ? p.name : std::string();
			};
			voteMessages.mapName = [&session](int map) {
				if (session.admin && map >= 0 && map < session.admin->maps.count()) {
					return session.admin->maps.entries()[static_cast<size_t>(map)].level;
				}
				return std::string();
			};
			hosted->onVoteReply = [&voteMessages](const game::Votes::Reply& r) {
				dice::hfe::menu::VoteMessages::Event v;
				v.target = r.target;
				v.choice = r.choice;
				v.needed = r.needed;
				v.voter = r.voter;
				v.phase = r.phase;
				v.kind = r.kind;
				v.seconds = r.seconds;
				voteMessages.handle(v);
			};
			mapVote.onVote = [&voteMessages, hosted, &round](int map) {
				if (voteMessages.mayStart(1, map, false)) {
					hosted->localVote(round.localPlayer(), 1, 0, map, 0); // a map vote (0x00573df0, kind 1)
				}
			};
		}
		// admin.* on this round (ServerAdmin.h): the players, kicks, and the
		// map changes it asks for, which end the map at once.
		bool serverNextMap = false;
		if (!hosted && !session.client) {
			// A local game is its own server (the menu's ServerSettings.TimeLimit,
			// or --time-limit): the game logic raises TimeLimitReached at it, as
			// it does for sv.timeLimit below.
			gameLogic.timeLimit = static_cast<float>(std::max(options.timeLimit, 0));
		}
		if (hosted && session.admin) {
			host::ServerAdmin& admin = *session.admin;
			// The round's sv settings: the time limit (seconds), spawn and
			// man-down times.
			gameLogic.timeLimit = static_cast<float>(admin.settings.timeLimit);
			round.setSpawnTime(admin.settings.spawnTime);
			round.setManDownTime(admin.settings.manDownTime);
			admin.hooks.players = [&round, hosted] {
				std::vector<host::AdminHooks::Player> out;
				const auto clients = hosted->clientPlayers();
				for (const game::GameWorld::PlayerInfo& p : round.playerList()) {
					host::AdminHooks::Player row;
					row.id = p.id;
					row.name = p.name;
					row.ai = p.ai;
					for (const host::HostedGame::ClientPlayer& c : clients) {
						if (c.player == p.id) {
							row.remote = true;
							row.name = c.name;
							row.address = c.address;
							row.cdKeyHash = c.cdKeyHash;
						}
					}
					out.push_back(row);
				}
				return out;
			};
			admin.hooks.kick = [hosted](int player, bool ban) { return hosted->kickPlayer(player, ban); };
			admin.hooks.runNextLevel = [&admin, &serverNextMap] {
				admin.request = host::ServerAdmin::Request::NextLevel;
				serverNextMap = true;
			};
			admin.hooks.restartMap = [&admin, &serverNextMap] {
				admin.request = host::ServerAdmin::Request::Restart;
				serverNextMap = true;
			};
			admin.hooks.runLevel = [&admin, &serverNextMap, &session](const net::MapListEntry& map) {
				if (!serverMap(session, map)) {
					std::printf("admin.runLevel: %s (%s) is not installed\n", map.level.c_str(), map.gameMode.c_str());
					return;
				}
				admin.request = host::ServerAdmin::Request::RunLevel;
				admin.requestedLevel = map;
				serverNextMap = true;
			};
			// A passed map vote runs that map (DAT_00a1c170 slot 0x30, 0x2c);
			// a mutiny removes the team's commander (DAT_00a1c174 slot 0x70).
			hosted->onMapVote = [&admin](int index) {
				if (index >= 0 && index < admin.maps.count()) {
					admin.hooks.runLevel(admin.maps.entries()[static_cast<size_t>(index)]);
				}
			};
			hosted->onMutiny = [&round](int team) {
				const int commander = round.commander(team);
				if (commander >= 0) {
					round.resignCommander(commander);
				}
			};
			admin.hooks.sayAll = [hosted](const std::string& text) { hosted->sayAll(text); };
			// demo.recordDemo <name> / demo.stopRecording (0x004246fb ->
			// 0x00432cc0 -> the server's 0x004afcc0; 0x00403080 -> 0x004afdb0).
			bindMethod(console, "demo.recordDemo", ConsoleAccess::Anyone, [hosted, &console](std::string name) {
				const std::string path = net::demoPath(name);
				if (hosted->recordingDemo()) {
					return std::string("already recording");
				}
				if (!hosted->recordDemo(path, host::demoCameraPath(name))) {
					return "Failed to record demo. Maybe the demo '" + path + "' already exists.";
				}
				if (hosted->recordingCamera()) {
					std::string output; // 0x005172f0: the user's camera settings
					console.run("Settings/demo-recordcam.con", {}, output);
				}
				return "recording demo " + path;
			});
			bindMethod(console, "demo.stopRecording", ConsoleAccess::Anyone, [hosted] { hosted->stopDemo(); });
			admin.hooks.sayTeam = [hosted](int team, const std::string& text) { hosted->sayTeam(team, text); };
		}
		struct ClearHooks {
			host::ServerAdmin* admin;
			~ClearHooks()
			{
				if (admin) {
					admin->hooks = {};
				}
			}
		} clearHooks {session.admin.get()};
		gameLogic.startRound();
		std::function<int(int)> scoreOf; // the scripts' scores, by player
		bool scoreboardHeld = options.scoreboard;
		// The end-of-round movie (endOfRound.dfm, its own player: its lists
		// are not the main menu's), shown once the round is over until the
		// next map.
		ui::Player endOfRound;
		dice::hfe::menu::EndOfRoundBindings endOfRoundBindings(endOfRound.natives());
		endOfRoundBindings.gameModeName = [&session](const std::string& mode) { return session.menuHost.gameModeName(mode); };
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
		// A joined game's own soldier, predicted from our moves and corrected
		// by the server's records (Prediction.h).
		host::SoldierPrediction prediction(round);
		// And the vehicle we drive: a record of the driver's seat (its
		// root's SimpleObject state, record.vehicle its ghost).
		host::VehiclePrediction carPrediction(round);
		uint32_t recordsSeen = 0;
		// A joined game's soldier deviation for the crosshair (the client
		// runs its own soldier's weapon feel, as the server's soldier does,
		// 0x005de980): each move's inputs into the weapon in hand's
		// SoldierDeviation (GameWorld::stepWeaponFeel's, without the sway).
		struct RemoteDeviation {
			net::MoveInputState inputs;
			game::SoldierDeviation deviation;
			const world::ObjectTemplate* weapon = nullptr;
			float lastPitch = 0;
		} remoteDeviation;
		g_remoteDeviation = -1;
		if (session.client) {
			session.client->onMoveSent = [&prediction, &carPrediction, &remoteDeviation, &playerCamera](const net::PlayerMove& move) {
				prediction.step(move);
				carPrediction.step(move);
				const game::SoldierInput in = net::soldierInputFromMove(move, remoteDeviation.inputs);
				const world::ObjectTemplate* weapon = g_remoteHud && prediction.active() ? g_remoteHud->heldWeapon().weapon : nullptr;
				if (weapon != remoteDeviation.weapon) {
					remoteDeviation.weapon = weapon;
					remoteDeviation.deviation = weapon ? game::SoldierDeviation::fromTemplate(*weapon) : game::SoldierDeviation {};
				}
				if (!weapon || !remoteDeviation.deviation.present) {
					g_remoteDeviation = -1;
					return;
				}
				constexpr float kDegreesPerUnit = 5;
				const float lookX = in.turn / kDegreesPerUnit, lookY = -(in.aimPitch - remoteDeviation.lastPitch) / kDegreesPerUnit;
				remoteDeviation.lastPitch = in.aimPitch;
				const int stance = in.pose == game::SoldierPose::Crouch ? 1 : in.pose == game::SoldierPose::Prone ? 2 : 0;
				remoteDeviation.deviation.update(in.throttle, in.strafe, lookX, lookY, !prediction.onGround(), in.fire, stance,
					playerCamera.zoomLevel() > 0);
				g_remoteDeviation = remoteDeviation.deviation.deviation();
			};
			session.client->sendOnTicks = true; // a move a tick (remoteStep's simulatedTick)
		}
		// The connection outlives this level (a map change): its hook doesn't.
		struct Unhook {
			Session& session;
			~Unhook()
			{
				if (session.client) {
					session.client->onMoveSent = nullptr;
					session.client->sendOnTicks = false;
				}
			}
		} unhook {session};
		// Where our soldier is drawn and seen from: the prediction's, else the server's.
		const auto ownFeet = [&prediction, &carPrediction, &session]() -> net::Vec3f {
			if (prediction.active()) {
				return prediction.feet();
			}
			if (carPrediction.active()) {
				return carPrediction.position();
			}
			return session.client ? session.client->world().controlObject().position : net::Vec3f {};
		};
		// A joined game's look, as our input turns it (PlayerCamera::Remote).
		struct RemoteLook {
			bool started = false;
			float yaw = 0, pitch = 0;
			bool fire = false;
			int stance = 0;
		} remoteLook;
		// The seated players' guns on a joined client (RemoteGuns.h).
		client::RemoteSeatGuns remoteSeatGuns;
		// What the HUD shows (HudWorld.h): this round, or the server's game.
		host::LocalHudWorld localHud(round, gameLogic);
		std::optional<host::RemoteHudWorld> remoteHud;
		if (session.client) {
			remoteHud.emplace(session.client->world(), round, ghostObjects);
			// The vehicles' turrets as their ghosts turn them, for the seated
			// players' mounts and hands (GameWorld::animateRemoteSeated).
			round.setRemotePartPose([&session, &ghostObjects](uint32_t object, const std::string& part, world::Vec3& offset,
										world::Vec3& rotation) {
				if (!session.client) {
					return false;
				}
				for (const auto& [ghost, id] : ghostObjects) {
					net::Vec3f o, r;
					if (id == object && session.client->world().partPose(ghost, part, o, r)) {
						offset = {o.x, o.y, o.z};
						rotation = {r.x, r.y, r.z};
						return true;
					}
				}
				return false;
			});
			remoteHud->ownStamina = [&prediction](float& stamina, bool& sprinting) {
				if (!prediction.active()) {
					return false;
				}
				stamina = prediction.control().stamina().stamina();
				sprinting = prediction.control().stamina().sprinting();
				return true;
			};
			remoteHud->ownJumpHold = [&prediction] { return prediction.active() && prediction.control().jumpHold(); };
			remoteHud->ownLook = [&ownFeet, &remoteLook, &session](physics::Vec3& feet, float& yaw, float& pitch) {
				if (!session.client || !session.client->world().hasControlObject() || !remoteLook.started) {
					return false;
				}
				const net::Vec3f f = ownFeet();
				feet = {f.x, f.y, f.z};
				yaw = remoteLook.yaw;
				pitch = remoteLook.pitch;
				return true;
			};
		}
		const auto hudWorld = [&]() -> const host::HudWorld& {
			if (session.client && remoteHud) {
				return *remoteHud;
			}
			return localHud;
		};
		struct HudWorldHook {
			~HudWorldHook() { g_hudWorld = nullptr; g_remoteHud = nullptr; }
		} hudWorldHook;
		g_hudWorld = &localHud;
		g_remoteHud = remoteHud ? &*remoteHud : nullptr;
		gameAudio.setHudWorld(g_remoteHud); // a joined game's radio: the server's players
		playerCamera.setPlayers(g_remoteHud); // a joined spectator follows the server's players
		// A joined game's squad and commander menus: the server's players,
		// the squad changes sent as the ClientSquadManager's events.
		commanderMenu.setWorld(g_remoteHud);
		if (g_remoteHud) {
			dice::hfe::menu::SquadMenu::Remote squads;
			squads.world = g_remoteHud;
			squads.join = [&session](int squad) {
				if (session.client) {
					session.client->joinSquad(squad);
				}
			};
			squads.leave = [&session](int squad) {
				if (session.client) {
					session.client->leaveSquad(squad);
				}
			};
			squads.create = [&session](int squad, const std::string& name) {
				if (session.client) {
					session.client->createSquad(squad, name);
				}
			};
			squads.lock = [&session](int squad, bool locked) {
				if (session.client) {
					session.client->setPrivateSquad(squad, locked);
				}
			};
			squads.invite = [&session](int player, int squad) {
				if (session.client) {
					session.client->invite(player, squad);
				}
			};
			squads.kick = [&session](int player, int squad) {
				if (session.client) {
					session.client->leaveSquad(squad, player);
				}
			};
			squadMenu.setRemote(std::move(squads));
			dice::hfe::menu::SquadRequests::Remote requests;
			requests.world = g_remoteHud;
			requests.takeOff = [&session](int squad, int kind) {
				if (session.client) {
					session.client->answerRequest(squad, kind);
				}
			};
			requests.say = [&session, &voices](int leader, const std::string& voice) {
				if (session.client && voices.message(voice) >= 0) {
					session.client->radioMessage(14, voices.message(voice), leader);
				}
			};
			squadRequests.setRemote(std::move(requests));
		} else {
			squadMenu.setRemote({});
			squadRequests.setRemote({});
		}
		struct MenusUnhook {
			dice::hfe::menu::SquadMenu& squads;
			dice::hfe::menu::CommanderMenu& commander;
			dice::hfe::menu::SquadRequests& requests;
			~MenusUnhook()
			{
				squads.setRemote({});
				commander.setWorld(nullptr);
				requests.setRemote({});
			}
		} menusUnhook {squadMenu, commanderMenu, squadRequests};
		// A joined game's soldier for the first-person view.
		if (session.client) {
			playerCamera.setRemote([&session, &ghostObjects, &objects, &templates, &remoteLook, &ownFeet, &prediction, &round](
									   PlayerCamera::Remote& me) {
				if (!session.client || !session.client->world().hasControlObject()) {
					return false;
				}
				const net::RemoteWorld& world = session.client->world();
				const net::ControlObjectRecord& control = world.controlObject();
				const net::RemoteObject* ghost = world.object(control.ghost);
				const auto object = ghostObjects.find(control.ghost);
				const world::Object* soldier = object != ghostObjects.end() ? objects.find(object->second) : nullptr;
				if (ghost && control.vehicle != 0) {
					// In a seat: the control object is the seat's ghost (a
					// PlayerControlObject of the vehicle's tree); the view is
					// the seat's camera (the client's 0x005948e0 -> the seat's
					// 0x005c7cb0), placed by the vehicle ghost and the bundles'.
					const net::RemotePlayer* player = world.player(world.localPlayer());
					const auto vehicleObject = ghostObjects.find(ghost->rootGhost ? ghost->rootGhost : ghost->ghost);
					const auto soldierObject = player ? ghostObjects.find(player->soldierGhost) : ghostObjects.end();
					const world::Object* seated = soldierObject != ghostObjects.end() ? objects.find(soldierObject->second) : nullptr;
					const world::Object* vehicle =
						vehicleObject != ghostObjects.end() ? objects.find(vehicleObject->second) : nullptr;
					if (!player || !vehicle || !seated || !seated->objectTemplate()) {
						return false;
					}
					me.alive = player->alive;
					me.soldier = seated->id();
					me.soldierTemplate = seated->objectTemplate();
					me.vehicle = vehicle->id();
					me.seat = ghost->part;
					me.feet = {vehicle->position().x, vehicle->position().y, vehicle->position().z};
					if (ghost->hasVelocity) {
						me.velocity = {ghost->velocity.x, ghost->velocity.y, ghost->velocity.z};
					}
					me.yaw = remoteLook.yaw;
					me.aimPitch = remoteLook.pitch;
					me.firing = remoteLook.fire;
					return true;
				}
				// Our soldier (its template's kind; the ghost's networkable kind
				// comes with its first update).
				if (!ghost || !soldier || !soldier->objectTemplate() || soldier->objectTemplate()->typeName() != "Soldier"
					|| control.vehicle != 0) {
					return false;
				}
				if (!remoteLook.started && ghost->hasAngles) {
					remoteLook.yaw = ghost->yaw;
					remoteLook.started = true;
				}
				me.alive = ghost->health != 0;
				me.soldier = soldier->id();
				me.weapon = round.remoteSoldierWeapon(soldier->id()); // its third-person weapon (not in the eye's view)
				me.soldierTemplate = soldier->objectTemplate();
				const net::Vec3f feet = ownFeet();
				me.feet = {feet.x, feet.y, feet.z};
				if (prediction.active()) {
					const net::Vec3f v = prediction.velocity();
					me.velocity = {v.x, v.y, v.z};
				} else if (ghost->hasVelocity) {
					me.velocity = {ghost->velocity.x, ghost->velocity.y, ghost->velocity.z};
				}
				me.yaw = remoteLook.yaw;
				me.aimPitch = remoteLook.pitch;
				me.stance = remoteLook.stance;
				me.firing = remoteLook.fire;
				// The held weapon: the kit's firearm in the selected slot (the
				// soldier's mask 0x2000; itemIndex, 3 the primary).
				if (const net::RemotePlayer* player = world.player(world.localPlayer()); player && player->kitGhost) {
					if (const auto kit = ghostObjects.find(player->kitGhost); kit != ghostObjects.end()) {
						me.kit = kit->second;
						if (const world::Object* kitObject = objects.find(kit->second); kitObject && kitObject->objectTemplate()) {
							const int wanted = ghost->weapon >= 0 ? ghost->weapon : 3;
							const world::ObjectTemplate* first = nullptr;
							for (const world::ChildTemplate& child : kitObject->objectTemplate()->children()) {
								const world::ObjectTemplate* t = templates.find(child.name);
								if (!t || t->typeName() != "GenericFireArm") {
									continue;
								}
								const auto index = t->settings().find("itemIndex");
								if (index != t->settings().end() && std::atoi(index->second.c_str()) == wanted) {
									me.weaponTemplate = t;
								}
								first = first ? first : t;
							}
							if (!me.weaponTemplate) {
								me.weaponTemplate = first;
							}
						}
					}
				}
				return true;
			});
		}
		std::map<uint16_t, uint32_t> ragdollUpdates; // ghost -> the ragdoll update last applied
		std::map<uint16_t, uint32_t> projectileUpdates; // a projectile ghost -> the update last applied to its round
		std::map<int, uint32_t> reloadsSeen;          // a player -> the reloads (mask 0x10000) already shown
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
		float remoteActionsPitch = 0; // the aim the last --remote-actions move carried
		bool joinedLevelLoaded = false; // told the client (LoadComplete goes now)
		const auto remoteStep = [&] {
			runTimedConsole();
			if (g_demo) {
				g_demo->running = true; // the demo's clock from the game's first tick
				if (g_demo->steppedTicks >= 0) {
					++g_demo->steppedTicks; // a headless run's: one a step
				}
			}
			if (!session.client) {
				return;
			}
			net::GameClient& client = *session.client;
			if (!joinedLevelLoaded) {
				joinedLevelLoaded = true;
				client.levelLoaded();
			}
			round.setAmbienceClient(true); // the networked ambient areas are the server's
			// --walk without a window: forward, turning, sprinting now and
			// then (a check on the prediction against the server).
			if (options.walk && !options.view) {
				game::SoldierInput walk;
				const int t = static_cast<int>(gameLogic.elapsed * 30);
				walk.throttle = (t / 90) % 4 == 3 ? 0.0f : 1.0f;
				walk.strafe = (t / 150) % 2 == 0 ? 0.0f : 0.5f;
				walk.turn = (t / 60) % 3 == 0 ? 3.0f : 0.0f;
				walk.sprint = (t / 120) % 3 == 1;
				walk.jump = t % 200 == 100;
				client.setInput(net::playerInputFrom(walk));
			}
			if (!options.remoteActions.empty()) {
				// --remote-actions: the inputs held from each step's second.
				game::SoldierInput act;
				bool reloadNow = false;
				std::string facing; // face:<name>: turn towards that player's soldier
				std::istringstream steps(options.remoteActions);
				for (std::string step; steps >> step;) {
					const size_t colon = step.find(':');
					if (colon == std::string::npos || std::atof(step.c_str()) > gameLogic.elapsed) {
						continue;
					}
					const std::string op = step.substr(colon + 1, step.find(':', colon + 1) - colon - 1);
					const size_t second = step.find(':', colon + 1);
					const float arg = second == std::string::npos ? 0.0f : static_cast<float>(std::atof(step.c_str() + second + 1));
					if (op == "crouch") {
						act.pose = game::SoldierPose::Crouch;
					} else if (op == "prone") {
						act.pose = game::SoldierPose::Prone;
					} else if (op == "stand") {
						act.pose = game::SoldierPose::Stand;
					} else if (op == "fire") {
						act.fire = true;
					} else if (op == "hold") {
						act.fire = false;
					} else if (op == "forward") {
						act.throttle = 1;
					} else if (op == "stop") {
						act.throttle = 0;
						act.turn = 0;
					} else if (op == "still") {
						act.turn = 0;
						facing.clear();
					} else if (op == "turn") {
						act.turn = arg;
					} else if (op == "face") {
						facing = second == std::string::npos ? std::string() : step.substr(second + 1);
					} else if (op == "reload") {
						// A press on the tick of its second only.
						reloadNow = gameLogic.elapsed - std::atof(step.c_str()) < 0.05;
					} else if (op == "pitch") {
						act.aimPitch = arg; // degrees, up positive
					} else if (op == "zoom") {
						// The alternate fire's press (a rifle's sight).
						act.altFire = gameLogic.elapsed - std::atof(step.c_str()) < 0.05;
					}
				}
				act.reload = reloadNow;
				for (const net::RemotePlayer& p : facing.empty() || !prediction.active() ? std::vector<net::RemotePlayer> {}
																						: client.world().playerList()) {
					const net::RemoteObject* them = p.name == facing ? client.world().object(p.soldierGhost) : nullptr;
					if (them && them->hasPosition) {
						const net::Vec3f me = prediction.feet();
						const float want = std::atan2(them->position.x - me.x, them->position.z - me.z) * 57.29578f;
						act.turn = std::clamp(std::remainder(want - remoteLook.yaw, 360.0f), -10.0f, 10.0f);
					}
				}
				remoteLook.yaw = std::remainder(remoteLook.yaw + act.turn, 360.0f); // the camera's, as the window's mouse turns it
				remoteLook.pitch = act.aimPitch;
				playerCamera.setAltFire(act.altFire);
				remoteLook.fire = act.fire;
				remoteLook.stance = act.pose == game::SoldierPose::Prone ? 2 : act.pose == game::SoldierPose::Crouch ? 1 : 0;
				client.setInput(net::playerInputFrom(act, remoteActionsPitch));
				remoteActionsPitch = act.aimPitch;
			}
			client.update();
			// The server's camera rules (its settings block): no chase view
			// without sv.allowExternalViews, no nose view without sv.allowNoseCam.
			if (client.world().dataBlocks().count(0)) {
				const net::RemoteWorld::ServerSettings sv = client.world().settings();
				playerCamera.allowExternalViews = sv.allowExternalViews;
				playerCamera.allowNoseCam = sv.allowNoseCam;
				playerCamera.allowFreeCam = sv.allowFreeCam;
				if (!sv.allowExternalViews) {
					playerCamera.firstPerson = playerCamera.spawnFirstPerson = true;
				}
			}
			// A new control-object record: the prediction takes the server's
			// state (or starts from it); none in the last block: no soldier.
			const net::RemoteWorld& own = client.world();
			if (own.controlRecords() != recordsSeen) {
				recordsSeen = own.controlRecords();
				const net::ControlState* state = own.controlState();
				const net::ControlObjectRecord& record = own.controlObject();
				if (state && record.vehicle != 0 && !state->groups.empty()
					&& state->groups.front().kind == net::NetworkableKind::SimpleObject) {
					prediction.stop();
					const net::RemoteObject* car = own.object(record.vehicle);
					const world::ObjectTemplate* t = car ? templates.findById(car->templateId) : nullptr;
					if (carPrediction.active() && carPrediction.vehicleTemplate() == t) {
						carPrediction.reconcile(record.lastMove, state->groups.front(), record.position);
					} else if (!t || !carPrediction.start(t, record.lastMove, state->groups.front(), record.position)) {
						carPrediction.stop();
					}
				} else if (state && record.vehicle == 0 && !state->groups.empty()
					&& state->groups.front().kind == net::NetworkableKind::Soldier && !(state->groups.front().mask & 0x10000)) {
					carPrediction.stop();
					if (!prediction.active()) {
						const auto object = ghostObjects.find(record.ghost);
						const world::Object* soldier = object != ghostObjects.end() ? objects.find(object->second) : nullptr;
						const net::RemoteObject* ghost = own.object(record.ghost);
						const world::ObjectTemplate* t = soldier ? soldier->objectTemplate()
							: ghost ? templates.findById(ghost->templateId) : nullptr;
						prediction.start(t, record.lastMove, state->groups.front(), record.position);
					} else {
						prediction.reconcile(record.lastMove, state->groups.front(), record.position);
						if (options.verbose && prediction.stats().lastCorrection > 0.5f) {
							const net::Vec3f f = prediction.feet();
							std::printf("prediction: corrected by %.2f m at move %d (now at %.2f/%.2f/%.2f)\n",
								prediction.stats().lastCorrection, record.lastMove, f.x, f.y, f.z);
						}
					}
				} else {
					prediction.stop();
					carPrediction.stop();
				}
			} else if (!own.controlInLastBlock() && (prediction.active() || carPrediction.active())) {
				prediction.stop();
				carPrediction.stop();
			}
			client.simulatedTick(); // this tick's move goes up (and is predicted)
			std::set<uint16_t> seen;
			// Soldiers with a player: their wrecks wait for the player (0x00652380).
			std::set<uint16_t> attached;
			std::set<uint16_t> occupied; // vehicle ghosts with a seat taken
			for (const net::RemotePlayer& p : client.world().playerList()) {
				if (p.soldierGhost) {
					attached.insert(p.soldierGhost);
				}
				if (p.alive && p.vehicleGhost) {
					const net::RemoteObject* seat = client.world().object(p.vehicleGhost);
					occupied.insert(seat && seat->rootGhost ? seat->rootGhost : p.vehicleGhost);
				}
			}
			for (const net::RemoteObject& ghost : client.world().objectList()) {
				seen.insert(ghost.ghost);
				if (ghost.part > 0 || !ghost.hasPosition) {
					continue; // a seat of a vehicle ghost (drawn with its vehicle), or not placed yet
				}
				world::Vec3 at {ghost.position.x, ghost.position.y, ghost.position.z};
				world::Vec3 turned {ghost.rotation.x, ghost.rotation.y, ghost.rotation.z};
				const bool predicted = prediction.active() && ghost.ghost == client.world().controlObject().ghost;
				if (predicted) {
					const net::Vec3f feet = prediction.feet();
					at = {feet.x, feet.y, feet.z};
				}
				// The networked orientation (SimpleObject's quaternion) or a
				// soldier's yaw, over CreateObject's rotation.
				if (ghost.hasOrientation) {
					host::yawPitchRollFromQuaternion(ghost.orientation, turned.x, turned.y, turned.z);
				} else if (ghost.hasAngles) {
					turned = {ghost.yaw, 0, 0};
				}
				if (predicted) {
					turned = {prediction.yaw(), 0, 0};
				}
				// The car we drive: where its prediction has it.
				if (carPrediction.active() && ghost.ghost == client.world().controlObject().vehicle) {
					const net::Vec3f p = carPrediction.position();
					const physics::Vec3 ypr = carPrediction.rotation().yawPitchRoll();
					at = {p.x, p.y, p.z};
					turned = {ypr.x, ypr.y, ypr.z};
				}
				if (const auto it = ghostObjects.find(ghost.ghost); it != ghostObjects.end()) {
					// A soldier's networked health: clients run the corpse
					// countdown and fade themselves (0x005fcdb0 → 0x00655460).
					if (ghost.kind == net::NetworkableKind::Soldier && ghost.health >= 0) {
						round.setRemoteHealth(it->second, ghost.health, attached.count(ghost.ghost) > 0);
					}
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
					// A server projectile's ghost (GenericProjectile): its round
					// here flies on from each update's place and velocity, and
					// moves the object (GameWorld::launchRemoteProjectile).
					const bool flying = round.hasRemoteProjectile(it->second);
					if (flying && projectileUpdates[ghost.ghost] != ghost.updates) {
						projectileUpdates[ghost.ghost] = ghost.updates;
						const physics::Vec3 v {ghost.velocity.x, ghost.velocity.y, ghost.velocity.z};
						round.moveRemoteProjectile(it->second, {at.x, at.y, at.z}, ghost.hasVelocity ? &v : nullptr);
					}
					// Its StickyProjectile event (0x0050f5d0): it stops and lies
					// where the server stuck it, riding the object it stuck to.
					if (flying && !round.remoteProjectileStuck(it->second)) {
						if (const net::StickyProjectileInfo* stuck = client.world().stuckProjectile(ghost.ghost)) {
							const auto target = stuck->target ? ghostObjects.find(stuck->target) : ghostObjects.end();
							const net::Vec3f place = stuck->rows[3], up = stuck->rows[1];
							if (!stuck->target) {
								round.stickRemoteProjectile(it->second, 0, {place.x, place.y, place.z}, {up.x, up.y, up.z});
							} else if (target != ghostObjects.end()) { // once its parent is placed here
								round.stickRemoteProjectile(it->second, target->second, {place.x, place.y, place.z},
									{up.x, up.y, up.z});
							}
						}
					}
					if (world::Object* object = round.remoteRagDoll(it->second) || flying ? nullptr : objects.find(it->second)) {
						object->setPosition(at);
						object->setRotation(turned);
						// A control point taken (its networkable's team, 0x006084b0):
						// the object's team follows, as the exe's client sets it
						// (the team-change radio, the name tags, the assets').
						if (object->team() != ghost.team) {
							object->setTeam(ghost.team);
						}
						// A walker's bundle animation, run here as the exe's
						// client runs it (GameWorld::animateRemoteVehicle): its
						// legs and the seats on its torso follow.
						if (ghost.kind != net::NetworkableKind::Soldier && object->objectTemplate()
							&& object->objectTemplate()->settings().count("bundleAnimationSystem")) {
							const physics::Vec3 v = ghost.hasVelocity
								? physics::Vec3 {ghost.velocity.x, ghost.velocity.y, ghost.velocity.z}
								: physics::Vec3 {};
							// The legs' own messages from the server's MechLegs
							// state (0x00650530 on the client, net::mechLegsMessages).
							int legMessages = -1;
							for (const net::RemotePart& part : ghost.parts) {
								if (part.kind == net::NetworkableKind::MechLegs && part.legState >= 0) {
									legMessages = static_cast<int>(net::mechLegsMessages(part.legState, part.legHolds));
								}
							}
							round.animateRemoteVehicle(it->second, v, occupied.count(ghost.ghost) > 0, 1.0f / 30.0f, legMessages);
						}
					}
				} else if (world::ObjectTemplate* t = ghost.templateName.empty() ? templates.findById(ghost.templateId)
																			   : templates.find(ghost.templateName)) {
					if (world::Object* object = objects.create(t, at, turned)) {
						object->setTeam(ghost.team);
						ghostObjects[ghost.ghost] = object->id();
						if (t->typeName() == "GenericProjectile") {
							const physics::Vec3 v = ghost.hasVelocity
								? physics::Vec3 {ghost.velocity.x, ghost.velocity.y, ghost.velocity.z}
								: physics::Vec3 {};
							round.launchRemoteProjectile(object->id(), t->name(), {at.x, at.y, at.z}, v);
							projectileUpdates[ghost.ghost] = ghost.updates;
						}
						if (options.verbose) {
							std::printf("ghost %u: %s\n", ghost.ghost, t->name().c_str());
						}
					}
				}
			}
			// The players' living soldiers on foot, animated from their ghosts
			// (GameWorld::animateRemoteSoldier: posed and holding the weapon
			// in hand, the kit's firearm of the selected item, mask 0x2000;
			// ours from its prediction).
			std::set<uint32_t> animated;
			std::vector<client::RemoteSeatGuns::Seat> gunners;
			for (const net::RemotePlayer& p : client.world().playerList()) {
				uint16_t ghost = p.soldierGhost;
				if (p.local && !ghost && client.world().hasControlObject() && !client.world().controlObject().vehicle) {
					ghost = client.world().controlObject().ghost;
				}
				const net::RemoteObject* body = ghost ? client.world().object(ghost) : nullptr;
				const auto found = ghost ? ghostObjects.find(ghost) : ghostObjects.end();
				if (p.alive && p.vehicleGhost && p.soldierGhost && body && !body->ragdoll && found != ghostObjects.end()) {
					// Seated: posed by the seat's system with its own look
					// (GameWorld::animateRemoteSeated). The seat's occupant
					// state (0x00605b00) carries the soldier's +0x25c, else
					// the soldier's own networkable does.
					const net::RemoteObject* seatGhost = client.world().object(p.vehicleGhost);
					const auto vehicle = seatGhost ? ghostObjects.find(seatGhost->rootGhost ? seatGhost->rootGhost : p.vehicleGhost)
												   : ghostObjects.end();
					if (seatGhost && vehicle != ghostObjects.end()) {
						float bodyYaw = body->hasAngles ? body->yaw : 0;
						if (seatGhost->hasOccupantState && (seatGhost->occupantState.mask & 2)) {
							bodyYaw = seatGhost->occupantState.yaw;
						}
						round.animateRemoteSeated(found->second, vehicle->second, seatGhost->part, bodyYaw, 1.0f / 30.0f);
						animated.insert(found->second);
						// The seat's guns from the player's fire states (the
						// client's 0x00590e10 -> 0x004cc7a0: the control
						// object's input): their rounds' sound and muzzle
						// effects (RemoteSeatGuns). Our own from the fire held.
						client::RemoteSeatGuns::Seat gunner;
						gunner.player = p.id;
						gunner.vehicle = vehicle->second;
						gunner.seat = seatGhost->part;
						for (int g = 0; g < 3; ++g) {
							gunner.firing[g] = p.fire[static_cast<size_t>(g)].on;
							gunner.rounds[g] = p.fire[static_cast<size_t>(g)].on ? p.fire[static_cast<size_t>(g)].rounds : -1;
							gunner.fireMode[g] = p.fire[static_cast<size_t>(g)].on ? p.fire[static_cast<size_t>(g)].fireMode : -1;
							gunner.active[g] = seatGhost->seatWeapon[g];
						}
						gunner.presses = p.firePresses;
						if (const net::RemoteObject* root = client.world().object(seatGhost->rootGhost ? seatGhost->rootGhost : p.vehicleGhost);
							root && root->hasVelocity) {
							gunner.velocity = {root->velocity.x, root->velocity.y, root->velocity.z};
						}
						if (p.local) {
							gunner.firing[0] = remoteLook.fire;
							gunner.firing[1] = gunner.firing[2] = false;
							gunner.presses = -1;
						}
						gunners.push_back(gunner);
					}
					continue;
				}
				if (!p.alive || p.vehicleGhost || !body || body->ragdoll || found == ghostObjects.end()) {
					continue;
				}
				game::GameWorld::RemoteSoldierState state;
				// The look: the body's yaw and the twist ahead of it (masks 2
				// and 4, +0x25c / +0x278), the aim (0x10, +0x270); the pose
				// (0x20, +0x268); the fire state from the player's networkable.
				state.yaw = body->hasAngles ? body->yaw + body->pitch : 0;
				state.aimPitch = body->aimPitch;
				if (body->hasVelocity) {
					state.velocity = {body->velocity.x, body->velocity.y, body->velocity.z};
				}
				state.pose = body->pose == 2 ? game::SoldierPose::Prone
					: body->pose == 1        ? game::SoldierPose::Crouch
					: body->pose == 3        ? game::SoldierPose::Swim
											 : game::SoldierPose::Stand;
				state.ladder = body->ladder; // 0x20000: the ladder and the place on it
				state.ladderPosition = body->ladderPosition;
				state.remoteFire = true;
				state.player = p.id;
				state.firing = p.fire[0].on;
				state.firePresses = p.firePresses;
				state.rounds = p.fire[0].on ? p.fire[0].rounds : -1;
				state.fireMode = p.fire[0].on ? p.fire[0].fireMode : -1;
				if (p.local && prediction.active()) {
					state.yaw = prediction.yaw();
					state.onGround = prediction.onGround();
					const net::Vec3f v = prediction.velocity();
					state.velocity = {v.x, v.y, v.z};
					state.pose = remoteLook.stance == 2 ? game::SoldierPose::Prone
						: remoteLook.stance == 1     ? game::SoldierPose::Crouch
													 : game::SoldierPose::Stand;
					// Our own: the fire held now (the server's rounds when it
					// says it fires), no muzzle effects seen from inside.
					state.firing = remoteLook.fire;
					state.firePresses = -1;
					state.muzzleEffects = !playerCamera.insideView();
				}
				const auto kit = ghostObjects.find(p.kitGhost);
				const world::Object* worn = p.kitGhost && kit != ghostObjects.end() ? objects.find(kit->second) : nullptr;
				if (worn && worn->objectTemplate()) {
					const int wanted = body->weapon >= 0 ? body->weapon : 3;
					std::string first;
					for (const world::ChildTemplate& child : worn->objectTemplate()->children()) {
						const world::ObjectTemplate* t = templates.find(child.name);
						if (!t || t->typeName() != "GenericFireArm") {
							continue;
						}
						const auto index = t->settings().find("itemIndex");
						if (index != t->settings().end() && std::atoi(index->second.c_str()) == wanted) {
							state.weapon = t->name();
						}
						first = first.empty() ? t->name() : first;
					}
					state.weapon = state.weapon.empty() ? first : state.weapon;
				}
				round.animateRemoteSoldier(found->second, state, 1.0f / 30.0f);
				animated.insert(found->second);
				// A reload pressed on the server (mask 0x10000): the weapon in
				// its hands reloads (0x00590e10's message 10).
				if (!p.local) {
					uint32_t& reloads = reloadsSeen.try_emplace(p.id, p.reloads).first->second;
					if (p.reloads < reloads) {
						reloads = p.reloads; // a new level's player
					} else if (p.reloads != reloads) {
						reloads = p.reloads;
						round.reloadRemoteSoldier(found->second, p.fire[0].rounds);
					}
				}
				if (options.verbose && !p.local) {
					// What the others' soldiers show: a line when it changes.
					static std::map<int, std::string> shown;
					char line[160];
					std::snprintf(line, sizeof line, "pose %d firing %d presses %d rounds %d weapon %s at %.0f/%.0f/%.0f aim %.0f/%.0f",
						body->pose, state.firing ? 1 : 0, state.firePresses, p.fire[0].rounds, state.weapon.c_str(), body->position.x,
						body->position.y, body->position.z, state.yaw, state.aimPitch);
					if (shown[p.id] != line) {
						shown[p.id] = line;
						std::printf("remote soldier %s: %s\n", p.name.c_str(), line);
					}
				}
			}
			remoteSeatGuns.step(round, gunners, 1.0f / 30.0f);
			// The server's strikes (kEvArtillery kind 2, 0x0050eeb0): the
			// asset's firearm fires here for its effects.
			for (const net::StrikeFireInfo& strike : client.world().takeStrikeFires()) {
				const auto asset = ghostObjects.find(strike.asset);
				const world::Object* object = asset != ghostObjects.end() ? objects.find(asset->second) : nullptr;
				if (object && object->objectTemplate()) {
					remoteStrikes.fired(templates, object->id(), *object->objectTemplate(), strike.x, strike.z,
						round.commanderSettings.orbitalBombardmentHeight);
				}
				if (options.verbose) {
					std::printf("strike fired at %.0f/%.0f by ghost %u (%s)\n", strike.x, strike.z, strike.asset,
						object && object->objectTemplate() ? object->objectTemplate()->name().c_str() : "unknown");
				}
			}
			for (const auto& [ghost, id] : ghostObjects) {
				if (round.isRemoteSoldier(id) && !animated.count(id)) {
					round.removeRemoteSoldier(id);
				}
			}
			if (options.verbose) {
				// The client's own rounds of everyone's firing (fireRemoteRound)
				// and the server's projectile ghosts flying here: a line a second.
				static int shownAt = -1;
				const int second = static_cast<int>(gameLogic.elapsed);
				if (second != shownAt) {
					shownAt = second;
					size_t rounds = 0, tracers = 0, ghosts = 0;
					for (const game::Projectile& p : round.projectiles()) {
						if (p.visual && p.alive) {
							(p.ghostObject ? ghosts : rounds) += 1;
							tracers += p.tracer && !p.ghostObject;
						}
					}
					static size_t impactsSeen = 0;
					const auto& impacts = round.impacts();
					if (rounds || ghosts || impacts.size() != impactsSeen) {
						std::printf("client rounds: %zu in flight (%zu tracers), %zu projectile ghosts, %zu impacts", rounds, tracers,
							ghosts, impacts.size());
						if (!impacts.empty()) {
							const auto& last = impacts.back();
							std::printf(" (last at %.1f/%.1f/%.1f on %s)", last.point.x, last.point.y, last.point.z,
								last.object == 0 ? "the terrain" : round.isRemoteSoldier(last.object) ? "a soldier" : "an object");
						}
						for (auto p = round.projectiles().rbegin(); p != round.projectiles().rend(); ++p) {
							if (p->visual && p->alive && !p->ghostObject) {
								std::printf(" newest from %.1f/%.1f/%.1f at %.0f/%.0f/%.0f m/s", p->origin.x, p->origin.y,
									p->origin.z, p->velocity.x, p->velocity.y, p->velocity.z);
								break;
							}
						}
						std::printf("\n");
						impactsSeen = impacts.size();
					}
				}
			}
			// A kit a living player carries is his soldier's child (0x005718d0
			// -> 0x00571d10): it follows him and is skinned to his skeleton
			// (GameWorld::followKit); a dropped one stays (geometry.dropGeom).
			std::map<uint32_t, uint32_t> wornKits;
			for (const net::RemotePlayer& p : client.world().playerList()) {
				const auto kit = p.alive && p.kitGhost ? ghostObjects.find(p.kitGhost) : ghostObjects.end();
				const auto body = p.soldierGhost ? ghostObjects.find(p.soldierGhost) : ghostObjects.end();
				const world::Object* soldier = body != ghostObjects.end() ? objects.find(body->second) : nullptr;
				if (world::Object* worn = kit != ghostObjects.end() && soldier ? objects.find(kit->second) : nullptr) {
					worn->setPosition(soldier->position());
					worn->setRotation(soldier->rotation());
					worn->setHidden(soldier->hidden());
					wornKits[kit->second] = body->second;
				}
			}
			std::set<uint32_t> droppedKits; // a kit ghost no one wears: no parent (0x0056cc50 without one)
			for (const auto& [ghost, id] : ghostObjects) {
				const net::RemoteObject* o = client.world().object(ghost);
				if (o && o->kit && !wornKits.count(id)) {
					droppedKits.insert(id);
				}
			}
			round.setRemoteKits(std::move(wornKits), std::move(droppedKits));
			// The control points' allowCaptureByTeam from their ghosts (masks 8 /
			// 0x10, every set: 0x006084b0 -> 0x005bcd00), for the takeable
			// radio of the local player (GameAudio::autoRadio).
			for (const net::RemoteWorld::TakeableSet& set : client.world().takeTakeable()) {
				const auto it = ghostObjects.find(set.ghost);
				if (const game::ControlPoint* cp = it != ghostObjects.end() ? round.controlPointOf(it->second) : nullptr) {
					round.noteTakeable(cp->id, set.team, set.allowed);
				}
			}
			for (auto it = ghostObjects.begin(); it != ghostObjects.end();) {
				if (!seen.count(it->first)) {
					round.endRemoteProjectile(it->second); // its detonation, unless it went off here
					projectileUpdates.erase(it->first);
					round.removeRemoteRagDoll(it->second);
					round.removeRemoteSoldier(it->second);
					ragdollUpdates.erase(it->first);
					objects.remove(it->second);
					it = ghostObjects.erase(it);
				} else {
					++it;
				}
			}
			if (session.joinSpectate && client.world().gameState() == 1 && client.world().localPlayer() >= 0) {
				// The menu's SPECTATOR (Logic.readyToPlay(true), 0x0046c780).
				session.joinSpectate = false;
				client.joinAsSpectator();
			}
			if (const net::RemotePlayer* me = client.world().player(client.world().localPlayer())) {
				if (me->spectator != session.spectating) {
					session.spectating = me->spectator;
					std::printf("%s\n", me->spectator ? "spectating" : "no longer spectating");
				}
				playerCamera.spectator = me->spectator;
			}
			if (session.joinSpawn && client.world().gameState() == 1 && client.world().localPlayer() >= 0) {
				session.joinSpawn = false;
				const net::RemotePlayer* me = client.world().player(client.world().localPlayer());
				if (session.joinTeam != 0 && me && me->team != session.joinTeam) {
					client.selectTeam(session.joinTeam); // PostRemote 6/7, before the kit (the team's)
				}
				// The spawn screen's kit (spawnManager.setPlayerKit; the
				// original's spawn screen sends the kit selected there, as
				// the local round takes it: not always the team's first, the
				// recon's).
				client.selectKit(localKit);
				client.selectSpawnGroup(-1);
			}
			const net::RemotePlayer* self = client.world().player(client.world().localPlayer());
			const auto speakerOf = [self](const net::RemotePlayer* p) {
				using Speaker = dice::hfe::menu::HudChat::Speaker;
				if (!p || !self) {
					return Speaker::Neutral;
				}
				return p->commander && p->team == self->team ? Speaker::Commander
					: p->team == self->team && p->squad && p->squad == self->squad ? Speaker::Squad
					: p->team == self->team ? Speaker::Team
											: Speaker::Enemy;
			};
			for (const net::ChatLine& line : client.world().takeChat()) {
				const net::RemotePlayer* from = client.world().player(line.player);
				std::printf("chat %s: %s\n", from ? from->name.c_str() : "server", line.text.c_str());
				if (from) {
					chat.addChat(from->name, line.text, static_cast<int>(line.channel), speakerOf(from), !from->alive);
				} else {
					chat.addInfo(line.text);
				}
			}
			for (const net::KillReport& kill : client.world().takeKills()) {
				const net::RemotePlayer* victim = client.world().player(kill.victim);
				const net::RemotePlayer* killer = client.world().player(kill.attacker);
				if (victim) {
					chat.addKill(killer && killer != victim ? killer->name : std::string(), victim->name, "",
						killer && killer->team == victim->team, speakerOf(killer));
					if (killer && self && kill.attacker == client.world().localPlayer() && !killer->commander && killer->team != victim->team) {
						chat.addKillScore(victim->name); // 0x0075a07a
					}
				}
			}
			// The squads' requests (0x004c5720's messages): 2/0x1e to the
			// leader that asked (his line; his radio to the commander comes
			// as a RadioMessage), 2/0x1d to the commander (the squad and the
			// kind).
			std::vector<net::PostRemoteInfo> offers;
			for (const net::PostRemoteInfo& message : client.world().takeMessages()) {
				if (message.category == 2 && message.id == 0x23) {
					offers.push_back(message);
				}
				// An invite (2/0x10, the squad: 0x004c3190's message): the
				// question's kind 1 (HUD_HUD_SQUAD_INVITED_TO, #SQUADNAME#).
				if (message.category == 2 && message.id == 0x10) {
					std::string line = localization.text("HUD_HUD_SQUAD_INVITED_TO");
					if (const size_t at = line.find("#SQUADNAME#"); at != std::string::npos) {
						line.replace(at, 11, dice::hfe::menu::squadName(message.payloadInt(0)));
					}
					questions.ask(dice::hfe::menu::HudQuestions::Invite, message.payloadInt(0), line);
				}
				// The Spotted event's messages (0x0050e910): a radio call's
				// code on the caller's name tag (the NametagManager's events
				// 0x33..0x37, 0x100f19c0); the caller by its soldier's ghost.
				net::SpottedInfo spot;
				if (message.category == 2 && message.id >= 0x33 && message.id <= 0x37
					&& net::parseSpottedPayload(message.payload, message.id, spot)) {
					for (const net::RemotePlayer& p : client.world().playerList()) {
						if (spot.spotter && p.soldierGhost == spot.spotter && !p.local) {
							if (g_nametags) {
								g_nametags->radioCall(p.id, static_cast<int>(message.id), false);
							}
							if (options.verbose) {
								std::printf("spotted message 0x%x: %s's call\n", message.id, p.name.c_str());
							}
						}
					}
				}
				// The close-to indications (2/0x3e in, 2/0x3f out: the
				// player and the type, 0x0059a6c0) for our player.
				if (message.category == 2 && (message.id == 0x3e || message.id == 0x3f) && message.payload.size() >= 8
					&& message.payloadInt(0) == client.world().localPlayer()) {
					dice::hfe::menu::closeToObject(hudManager, message.payloadInt(1), message.id == 0x3e);
				}
				// Team damage (0/0xb {kind, victim, attacker}, TeamDamage.cpp):
				// the HUD's answer (0x00759bf0 case 0xb). The victim's own
				// call goes to the server as our radio; the other's voice
				// (the client's slot 0x54, a radio in the victim's name) has
				// no message of ours to carry it.
				if (message.category == net::kMsgGameCategory && message.id == 0xb && message.payload.size() >= 12) {
					game::GameWorld::TeamDamage e;
					e.kind = message.payloadInt(0);
					e.victim = message.payloadInt(1);
					e.attacker = message.payloadInt(2);
					const net::RemotePlayer* victim = client.world().player(e.victim);
					const bool up = victim && victim->alive && !victim->manDown;
					using Notice = game::GameWorld::TeamDamageNotice;
					switch (game::GameWorld::teamDamageNotice(e, client.world().localPlayer(), up)) {
					case Notice::SayFriendlyFire:
						if (voices.message(game::GameWorld::kFriendlyFireVoice) >= 0) {
							client.radioMessage(14, voices.message(game::GameWorld::kFriendlyFireVoice), e.attacker);
						}
						break;
					case Notice::PlayerLine:
						chat.addPersonal(localization.text("HUD_HUD_TEAM_DAMAGE_PLAYER"));
						break;
					case Notice::VehicleLine:
						chat.addPersonal(localization.text("HUD_HUD_TEAM_DAMAGE_VEHICLE"));
						break;
					case Notice::VictimSaysFriendlyFire:
					case Notice::None:
						break;
					}
				}
				if (message.category == 2 && message.id == 0x1e && squadRequests.onPersonal) {
					squadRequests.onPersonal(localization.text("HUD_HUD_SQUAD_REQUEST_TO_COMMANDER"));
				} else if (message.category == 2 && message.id == 0x1d) {
					// 0x007c8f10 as on the host: the line with the tool usable
					// (the assets are our round's ghosts, the reloads our own
					// uses'), else the 4 s turn-down.
					if (const net::RemotePlayer* me = client.world().player(client.world().localPlayer())) {
						squadRequests.received(me->team, message.payloadInt(0), message.payloadInt(1));
					}
				}
			}
			// Voice chat: the server's voice settings (data block 0), its
			// VoipSession and mute events (not used by the world), our squad.
			// The server's Vote events (0x005740e0): target, choice, yes,
			// target, no, needed, voter, phase, kind, seconds.
			std::vector<net::GameEvent> unhandled = client.world().takeUnhandled();
			voteMessages.localPlayer = client.world().localPlayer();
			voteMessages.playerName = [&client](int id) {
				const net::RemotePlayer* p = client.world().player(id);
				return p ? p->name : std::string();
			};
			voteMessages.mapName = [&client](int map) {
				const auto& maps = client.world().mapList();
				return map >= 0 && static_cast<size_t>(map) < maps.size() ? maps[static_cast<size_t>(map)].name : std::string();
			};
			for (const net::GameEvent& event : unhandled) {
				if (event.type == net::kEvVote) {
					dice::hfe::menu::VoteMessages::Event v;
					v.target = static_cast<int>(event.u(0));
					v.choice = static_cast<int>(event.u(1));
					v.needed = static_cast<int>(event.u(5));
					v.voter = static_cast<int>(event.u(6));
					v.phase = static_cast<int>(event.u(7));
					v.kind = static_cast<int>(event.u(8));
					v.seconds = event.f(9);
					voteMessages.handle(v);
				}
			}
			voteMessages.tick(1.0f / 30.0f);
			// The radio messages we hear (RadioMessage, 0x0050d300 → the
			// event manager's category 6 → 0x0042e0c0): the voice and its
			// text, the name tags' calls (GameAudio's radio).
			for (const net::GameEvent& event : unhandled) {
				net::RadioMessageInfo radio;
				if (net::parseRadioMessage(event, radio) && client.world().localPlayer() >= 0) {
					const std::string voice = voices.messageName(radio.index);
					if (!voice.empty()) {
						round.hearRadio(client.world().localPlayer(), radio.channel, voice,
							radio.sender == 0xff || radio.source == 0 ? -1 : radio.sender);
					}
				}
			}
			// The offer of command (2/0x23, 0x004c5070): the question's kind 3
			// (0x0075xxxx's case 0x23 → 0x0079e440), answered by Radio.answer /
			// c_GIYes / c_GINo, or Commander.accept.
			for (const net::PostRemoteInfo& message : offers) {
				(void)message;
				if (const net::RemotePlayer* me = client.world().player(client.world().localPlayer())) {
					std::printf("the server offers us command (Radio.answer 1 takes it)\n");
					askCommanderOffer(me->team);
				}
			}
			// The server's ambient effects (AmbientEffectArea, 0x00510230).
			for (const net::GameEvent& event : unhandled) {
				if (event.type == net::kEvAmbientEffectArea) {
					const world::Object* on = nullptr;
					if (const auto it = ghostObjects.find(static_cast<uint16_t>(event.u(2))); event.u(2) && it != ghostObjects.end()) {
						on = objects.find(it->second);
					}
					const physics::Vec3 at = on ? physics::Vec3 {on->position().x, on->position().y, on->position().z}
												: physics::Vec3 {};
					round.playAmbientEvent(event.u(0), event.i(1), {event.f(3), event.f(4), event.f(5)}, on ? &at : nullptr);
				}
			}
			if (session.voice) {
				if (const auto block = client.world().dataBlocks().find(0); block != client.world().dataBlocks().end()) {
					session.voice->setServerSettingsBlock(block->second, options.connect.substr(0, options.connect.rfind(':')));
				}
				for (const net::GameEvent& event : unhandled) {
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
			for (const net::KickInfo& kick : client.world().takeKicks()) {
				const net::RemotePlayer* who = client.world().player(kick.player);
				const std::string name = who ? who->name : "player " + std::to_string(kick.player);
				std::printf("%s was %s\n", name.c_str(), kick.banned ? "banned" : "kicked");
				chat.addInfo(name + (kick.banned ? " was banned" : " was kicked"));
			}
			if (client.state() != net::GameClient::State::Connected) {
				std::printf("the server ended the connection (reason %u: %s)\n", client.disconnectReason(),
					net::disconnectReason(client.disconnectReason()));
				if (session.voice) {
					session.voice->leave();
				}
				gameAudio.setHudWorld(nullptr); // its world goes with the client
				// Not freed yet: the remote HUD world (g_remoteHud) and what
				// was handed it still read the client's world this frame
				// (a demo's end drawn by --screenshot read it freed).
				session.endedClient = std::move(session.client);
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
		// On a server, ss_getParam reads its sv settings and rcon_invoke runs
		// its console (ServerScriptBridge.h).
		host::ServerScriptBridge bridge(round, gameLogic, serverSettings, options.modPath,
			hosted ? session.admin.get() : nullptr, &console);
		bridge.overlay = options.overlayPath.empty() ? options.gameDir + "/" : options.overlayPath;
		bridge.onFeedback = [hosted](int player, const std::string& text) {
			if (hosted && player >= 0) {
				hosted->sendRemoteFeedback(player, text);
			} else {
				std::printf("rcon: %s", text.c_str());
			}
		};
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
		bridge.events = raise;
		// The players' ranks (stats +0x20): stats.py gives each 0 as it
		// connects; rank.py asks the stats server for those with a profile
		// id and sets its "rnk". The local store plays the stats server for
		// the local soldier (Stats.h); bots and remote players without an
		// account keep 0, as the original's did.
		bridge.profileOf = [&round, &session](int player) -> int64_t {
			if (player != round.localPlayer() || !session.menuBindings || !session.menuBindings->soldierStats()) {
				return 0;
			}
			return stats::localProfileId(session.menuBindings->soldierName());
		};
		bridge.statsServer = [&session](int64_t profile, game::ScriptBridge::StatsRecord& record) {
			stats::SoldierStats* soldier = session.menuBindings ? session.menuBindings->soldierStats() : nullptr;
			if (!soldier || stats::localProfileId(session.menuBindings->soldierName()) != profile) {
				return false;
			}
			record = stats::playerInfoKeys(*soldier);
			return true;
		};
		gameLogic.onEndGame = [&](int winner, int victoryType) {
			std::printf("round over: team %d won (victory type %d)\n", winner, victoryType);
			scripts.setGameStatus(game::GameStatus::EndGame);
		};
		// The scripts' ticket state (gpm_ti: the Titans' shields and hulls)
		// announced to the local player (0x0044b7e8 -> 0x00449070, slot 0x60).
		gameLogic.onTicketState = [&round](int team, int state) {
			const int local = round.localPlayer();
			game::GameWorld::PlayerInfo me;
			if (local < 0 || !round.playerInfo(local, me)) {
				return;
			}
			const std::string message = GameAudio::ticketStateMessage(team, state, me.team, round.gameMode());
			if (!message.empty()) {
				std::printf("ticket state: team %d -> %d: %s\n", team, state, message.c_str());
				round.hearRadio(local, 13, message);
			}
		};
		if (session.client) {
			std::printf("on %s: the server runs the round\n", session.client->world().serverName().c_str());
		} else if (scripts.ok() && scripts.startGameMode(options.gameMode)) {
			scripts.setGameStatus(game::GameStatus::PreGame);
			if (hosted && session.admin) {
				// A server waits for sv.numPlayersNeededToStart, counts down
				// sv.startDelay, then plays (0x004c03b0's states 1-3).
				const net::ServerSettings& sv = session.admin->settings;
				gameLogic.waitToStart(sv.numPlayersNeededToStart, static_cast<float>(sv.startDelay),
					sv.notEnoughPlayersRestartDelay, [&round] { return static_cast<int>(round.playerList().size()); });
				gameLogic.onPlaying = [&scripts] { scripts.setGameStatus(game::GameStatus::Playing); };
				std::printf("waiting for %d players, then %d s to the start\n", sv.numPlayersNeededToStart, sv.startDelay);
			} else {
				scripts.setGameStatus(game::GameStatus::Playing);
			}
			std::printf("game mode %s started: tickets %d / %d\n", options.gameMode.c_str(),
				static_cast<int>(gameLogic.team(1).tickets), static_cast<int>(gameLogic.team(2).tickets));
			// The server's admin script (sv.adminScript: admin/default.py's
			// remote console, standard_admin's auto-balance and TK punish).
			if (hosted && session.admin && !session.admin->settings.adminScript.empty()
				&& session.adminScript.start(scripts, options.gameDir, session.admin->settings.adminScript, true)) {
				std::printf("admin script %s running\n", session.admin->settings.adminScript.c_str());
			}
			if (hosted) {
				hosted->onRemoteCommand = [&scripts](int player, const std::string& command) {
					scripts.raise("RemoteCommand", {game::ScriptValue::integer(player), game::ScriptValue::string(command)});
				};
				hosted->onChat = [&scripts](int player, const net::ChatMessage& line) {
					scripts.raise("ChatMessage", {game::ScriptValue::integer(player), game::ScriptValue::string(line.text),
													 game::ScriptValue::integer(line.channel), game::ScriptValue::integer(0)});
				};
			}
		} else {
			std::fprintf(stderr, "game mode %s did not start\n", options.gameMode.c_str());
		}
		// Testing aid: BF2142_TITAN_SHIELDS_DOWN=<team> takes that team's Titan
		// to 55 % once, so its shield is down (the terminals and core show).
		// Diagnostic for "clipped into the Titan": twice a second while the local
		// soldier is over or inside a Titan, its place in the Titan's frame and
		// vertical speed (the log shows where it sinks).
		auto titanWalkTrace = [&round, tick = 0]() mutable {
			if (++tick % 15 != 0) {
				return;
			}
			game::GameWorld::PlayerInfo me;
			if (!round.playerInfo(round.localPlayer(), me) || !me.alive || me.vehicle) {
				return;
			}
			for (const auto& titan : round.titans()) {
				const float dx = me.position.x - titan.position.x, dz = me.position.z - titan.position.z;
				if (std::hypot(dx, dz) < titan.radius + 10 && std::fabs(me.position.y - titan.position.y) < 60) {
					std::printf("titan-walk: team %d shield %d at %.1f %.1f %.1f vy %.1f hull %u\n", titan.team, titan.shield ? 1 : 0, dx,
						me.position.y - titan.position.y, dz, me.velocity.y, round.titanHolding(me.position) ? 1u : 0u);
				}
			}
		};
		auto shieldsDown = [&round, done = false]() mutable {
			static const char* team = std::getenv("BF2142_TITAN_SHIELDS_DOWN");
			if (!team || done) {
				return;
			}
			for (const auto& titan : round.titans()) {
				if (titan.team == std::atoi(team) && titan.maxHitPoints > 0) {
					round.setHitPoints(titan.object, titan.maxHitPoints * 0.55f);
					done = true;
					if (std::getenv("BF2142_TITAN_WRECK_CONSOLES")) {
						for (const auto& part : round.titanParts(titan.team)) {
							if (part.kind == game::GameWorld::TitanPartInfo::Console) {
								std::printf("titan console %s at %.1f %.1f %.1f\n", part.name.c_str(), part.position.x, part.position.y, part.position.z);
								round.applyDamage(part.id, 1e7f);
							}
						}
					}
				}
			}
		};
		host::TitanShotTrace titanShotTrace;
		const auto step = [&] {
			remoteStep();
			if (hosted) {
				hosted->tick();
				voteMessages.tick(1.0f / 30.0f); // the host's own vote object
			}
			bots.update(1.0f / 30.0f);
			round.localMuzzleEffects = !playerCamera.insideView();
			round.setZoomed(round.localPlayer(), playerCamera.zoomLevel() > 0);
			round.setAutoReload(session.settings.number("GeneralSettings.AutoReload", 0) != 0);
			round.tick(1.0f / 30.0f);
			shieldsDown();
			titanWalkTrace();
			titanShotTrace.update(round, objects);
			playerCamera.tick(1.0f / 30.0f);
			stepObjectEffects();
			gameLogic.tick(1.0f / 30.0f);
			scripts.update();
			bridge.deliverStatsResponses();
			session.adminScript.update(scripts);
			stepSounds();
		};
#else
		const auto step = [&] {
			remoteStep();
			if (hosted) {
				hosted->tick();
				voteMessages.tick(1.0f / 30.0f); // the host's own vote object
			}
			bots.update(1.0f / 30.0f);
			round.localMuzzleEffects = !playerCamera.insideView();
			round.setAutoReload(session.settings.number("GeneralSettings.AutoReload", 0) != 0);
			round.tick(1.0f / 30.0f);
			shieldsDown();
			titanWalkTrace();
			titanShotTrace.update(round, objects);
			playerCamera.tick(1.0f / 30.0f);
			stepObjectEffects();
			gameLogic.tick(1.0f / 30.0f);
			stepSounds();
		};
#endif
		// After the round, sv.endDelay (15 s) before a playlist's next map.
		const float kEndDelay = session.admin ? static_cast<float>(session.admin->settings.endDelay) : 15.0f;
		float sinceEnd = 0;
		const auto nextMapDue = [&] {
			return (serverNextMap && !session.playlist.empty()) || (!session.playlist.empty() && sinceEnd >= kEndDelay);
		};
		const auto showEndOfRound = [&] {
			if (endOfRoundShown || options.dedicated) {
				return;
			}
			endOfRoundShown = true;
			// The round's data first: the movie's first frame reads it
			// (getMyTeam, isValidEOR) and keeps what it got.
			dice::hfe::menu::RoundSummary summary = session.client
				? remoteRoundSummary(options, session.client->world())
				: roundSummary(options, round, gameLogic, scoreOf, "BF2142 server");
			if (session.menuBindings) {
				session.menuBindings->recordRound(summary); // the soldier's stats and rank
			}
			// The played level's texts (EndOfRound.getPreviousMap*Description).
			if (std::string desc; files.readText("Levels/" + summary.previousMapPath + "/Info/" + summary.previousMapPath + ".desc", desc)) {
				const dice::hfe::menu::LevelDescription level =
					dice::hfe::menu::parseLevelDescription(desc, summary.previousGameMode, summary.previousMapSize);
				summary.previousMapDescriptionKey = level.briefingKey.empty() ? level.briefing : level.briefingKey;
				summary.previousGameModeDescriptionKey = level.gameModeKey.empty() ? level.gameModeText : level.gameModeKey;
			}
			summary.playerName = options.playerName;
			endOfRoundBindings.setSummary(summary);
			std::vector<uint8_t> dfm;
			std::string error;
			if (!files.readFile("Menu/External/FlashMenu/endOfRound.dfm", dfm) || !endOfRound.loadDfm(dfm, &error)) {
				std::fprintf(stderr, "cannot load endOfRound.dfm: %s\n", error.c_str());
				return;
			}
			// JOIN GAME: on to the next map without waiting out sv.endDelay.
			// SPECTATOR (readyToPlay(true): the host's event 0x10 -> SwiffHost
			// slot 0x3c0, 0x0046c780): JoinAsSpectator to the server; a game
			// of our own runs the server's handler (0x005142a0) itself.
			endOfRoundBindings.onReadyToPlay = [&](bool asSpectator) {
				std::printf("end of round: readyToPlay(%d), %zu playlist maps, %.0f of %.0f s\n", asSpectator, session.playlist.size(),
					sinceEnd, kEndDelay);
				sinceEnd = std::max(sinceEnd, kEndDelay);
				if (!asSpectator) {
					return;
				}
				if (session.client) {
					session.client->joinAsSpectator();
				} else if (session.admin && !session.admin->settings.ranked && session.admin->settings.allowSpectators) {
					round.setSpectator(round.localPlayer(), true);
					playerCamera.spectator = true;
				}
			};
			showMenu = false;
		};
		const size_t before = objects.objects().size();
		// For a screenshot, let the spawned vehicles settle for 3 seconds.
		const int ticks = options.screenshot.empty() ? 1 : 90;
		for (int i = 0; i < ticks; ++i) {
			step();
		}
		// A screenshot of a joined game waits (in real time, up to 5 s; 25 s
		// with --play, the server's start delay and spawn time) for the
		// server's first control-object record and a second more of
		// updates, so the view has our soldier.
		// (Not for a demo: it has no control object of its own.)
		if (session.client && !options.screenshot.empty() && !g_demo) {
			const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(options.playTeam != 0 ? 25 : 5);
			int after = -1;
			while (std::chrono::steady_clock::now() < until && after != 0 && session.client) {
				step();
				if (after < 0 && session.client && session.client->world().hasControlObject()) {
					after = 30;
				} else if (after > 0) {
					--after;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(33));
			}
		}
		std::printf("round started: %zu control points, %zu spawners, %zu objects spawned, %zu with physics\n",
			round.controlPoints().size(), round.spawners().size(), objects.objects().size() - before,
			round.physicsBodies());
		if (options.bots > 0 && !bots.enabled()) {
			// As the original: a level without AI data (no AI/AI.ai, no
			// navigation mesh) has no bots.
			std::printf("no bots on this level (it has no AI navigation data)\n");
		}
		// The AI runs on this server (AIDLL's interface; a wreck's armour
		// drains three times as fast, 0x00652380).
		round.setAiRunning(bots.enabled() && options.bots > 0);
		// The SAI's kit choice at a bot's respawn (AIDLL 0x100f04a0) draws
		// from the team's kits.
		for (int team = 1; team <= 2; ++team) {
			std::vector<std::pair<std::string, std::string>> teamKits;
			for (const game::Kit& kit : gameLogic.team(team).kits) {
				teamKits.push_back({kit.name, kit.soldier});
			}
			bots.setTeamKits(team, std::move(teamKits));
		}
		for (int i = 0; i < (bots.enabled() ? options.bots : 0); ++i) {
			const int team = i % 2 == 0 ? (options.playTeam == 1 ? 2 : 1) : (options.playTeam == 1 ? 1 : 2);
			// A bot's name: the next of AI/BotNames.ai's (aiSettings.addBotName,
			// AIDLL 0x10050a20 from the AI player factory 0x0089e7a0).
			std::string botName = botSettings.nextBotName();
			const int bot = round.addPlayer(botName.empty() ? "Bot" + std::to_string(i + 1) : botName, team, true);
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
				// The profile's unlocks are both the soldier's chosen ones and
				// those it owns (player slot 0x2a8; the kit menu's equip below
				// changes only the chosen).
				round.localPlayerUnlocks = stats::containersFor(stats::unlockContainers(templates), session.menuBindings->soldierUnlocks());
				round.localPlayerOwned = round.localPlayerUnlocks;
			}
			const bool enterDeploymentFirst = options.view && options.drive.empty() && options.simulate == 0 && !hosted
				&& (options.spawnScreen || (options.screenshot.empty() && options.actions.empty() && options.remoteActions.empty()
					&& options.inputScript.empty() && options.menuScript.empty() && options.hudClicks.empty() && options.console.empty()));
			const int local = enterDeploymentFirst ? round.joinPlayer(options.playTeam) : -1;
			const world::Object* player = enterDeploymentFirst ? nullptr : round.spawnPlayer(options.playTeam);
			if (enterDeploymentFirst || player) {
				if (enterDeploymentFirst) {
					const std::vector<int> points = round.spawnablePoints(options.playTeam);
					round.setSpawnChoice(local, points.empty() ? -1 : points.front());
				}
				round.joinSquad(round.localPlayer(), round.openSquad(options.playTeam));
				g_initialDeploymentPending = enterDeploymentFirst;
				if (enterDeploymentFirst) {
					std::printf("player joined team %d; awaiting initial deployment (player %d)\n", options.playTeam, local);
				}
				if (!enterDeploymentFirst) {
					std::printf("playing %s at %.1f/%.1f/%.1f\n", player->objectTemplate()->name().c_str(),
						player->position().x, player->position().y, player->position().z);
				}
				if (!options.drive.empty()) {
					// Spawn clear of the soldier so a large tank cannot crush him while settling.
					world::ObjectTemplate* vehicle = templates.find(options.drive);
					const float yaw = player->rotation().x * 3.14159265f / 180.0f;
					const float x = player->position().x + std::sin(yaw) * 8.0f, z = player->position().z + std::cos(yaw) * 8.0f;
					const terrain::Heightmap* terrainHeights = heightmaps.primary();
					const float y = terrainHeights && terrainHeights->loaded() ? terrainHeights->heightAt(x, z) : player->position().y;
					const world::Object* spawnedVehicle = vehicle ? objects.create(vehicle, {x, y + 1.5f, z}, {player->rotation().x, 0, 0}) : nullptr;
					const uint32_t spawnedVehicleId = spawnedVehicle ? spawnedVehicle->id() : 0;
					if (!spawnedVehicle) {
						std::fprintf(stderr, "cannot spawn %s\n", options.drive.c_str());
					} else {
						// Let it land (briefly: on a slope it would roll out of reach).
						for (int i = 0; i < 15; ++i) {
							step();
						}
						// Enter the vehicle this probe spawned, using an explicit entry
						// point where present. A map may already contain the same type.
						if (const world::Object* car = objects.find(spawnedVehicleId)) {
							const auto rotation = physics::Mat3::fromYawPitchRoll({car->rotation().x, car->rotation().y, car->rotation().z});
							physics::Vec3 local {-1.8f, 0, 0};
							for (const auto& child : vehicle->children()) {
								const auto* entry = templates.find(child.name);
								if (entry && entry->typeName() == "EntryPoint") {
									local = {child.position.x, child.position.y, child.position.z};
									break;
								}
							}
							const auto offset = rotation.transform(local);
							const physics::Vec3 side {car->position().x + offset.x, car->position().y + offset.y, car->position().z + offset.z};
							round.setSoldierPosition(round.playerObject(),
								{side.x, terrainHeights ? terrainHeights->heightAt(side.x, side.z) : side.y, side.z});
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
						if (options.seat == 0) { // --seat: stay parked (an aircraft would fly off and crash)
						drive(1, 0, 90);
						report("after 3 s of throttle");
						drive(-1, 0, 30);
						report("after 1 s braking");
						drive(0.5f, 1, 45);
						report("after 1.5 s turning right");
						}
						if (options.seat > 0) {
							// --seat N: stay aboard in that seat for the screenshot.
							game::SoldierInput seat;
							seat.switchSeat = options.seat;
							round.setPlayerInput(seat);
							step();
							for (int i = 0; i < 30; ++i) {
								round.setPlayerInput({});
								step();
							}
							game::GameWorld::PlayerInfo me;
							round.playerInfo(round.localPlayer(), me);
							std::printf("  in seat %d of %zu\n", me.seat + 1, round.vehicleSeats(me.vehicle).size());
						} else {
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
						// the soldier aboard dies with it (not before a screenshot:
						// the picture is of the player aboard).
						const uint32_t carId = me.vehicle;
						if (options.screenshot.empty()) {
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
						}
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
					if (options.zoom) {
						// The right button once: the weapon's zoom, a second to settle.
						playerCamera.setAltFire(true);
						round.setPlayerInput({});
						step();
						playerCamera.setAltFire(false);
						for (int i = 0; i < 30; ++i) {
							round.setPlayerInput({});
							step();
						}
					}
				}
				// Screenshot actions also operate the selected vehicle seat.
				if (!options.screenshot.empty()) {
					// --actions: the player's inputs, a tick each unless a count
					// follows ("fire:30 reload wait:60 item:2 firemode prone:20
					// crouch:20 jump turn:90 pitch:-10 sprint:30 forward:30"), and
					// what the weapon in hand says after each.
					std::istringstream actions(options.actions);
					for (std::string command; actions >> command;) {
						const size_t colon = command.find(':');
						const std::string op = command.substr(0, colon);
						const float value = colon == std::string::npos ? 1.0f : static_cast<float>(std::atof(command.c_str() + colon + 1));
						const int ticks = std::max(1, static_cast<int>(value));
						game::GameWorld::PlayerInfo me;
						round.playerInfo(round.localPlayer(), me);
						game::SoldierInput act;
						act.aimPitch = me.aimPitch;
						bool known = true;
						if (op == "fire" || op == "altfire" || op == "wait" || op == "forward" || op == "sprint" || op == "prone" || op == "crouch") {
							act.fire = op == "fire";
							act.altFire = op == "altfire";
							act.throttle = op == "forward" || op == "sprint" ? 1.0f : 0.0f;
							act.sprint = op == "sprint";
							act.pose = op == "prone" ? game::SoldierPose::Prone : op == "crouch" ? game::SoldierPose::Crouch
																							   : game::SoldierPose::Stand;
							for (int i = 0; i < ticks; ++i) {
								round.setPlayerInput(act);
								step();
							}
						} else if (op == "reload" || op == "firemode" || op == "jump" || op == "item" || op == "use") {
							act.reload = op == "reload";
							act.toggleFireRate = op == "firemode";
							act.jump = op == "jump";
							act.use = op == "use";
							act.selectItem = op == "item" ? ticks : 0;
							round.setPlayerInput(act);
							step();
							round.setPlayerInput({});
							step();
						} else if (op == "turn" || op == "pitch") {
							act.turn = op == "turn" ? value : 0.0f;
							act.aimPitch = op == "pitch" ? value : me.aimPitch;
							round.setPlayerInput(act);
							step();
						} else {
							known = false;
							std::fprintf(stderr, "actions: unknown command %s\n", command.c_str());
						}
						if (known) {
							round.playerInfo(round.localPlayer(), me);
							const game::FireArm* gun = round.playerWeapon();
							std::string held;
							for (const auto& [slot, name] : me.carried) {
								if (slot == me.inHandSlot) {
									held = name;
								}
							}
							std::printf("action %s: %s slot %d %s, %d rounds, %d magazines, fire mode %d%s%s\n", command.c_str(),
								me.alive ? "alive" : "dead", me.inHandSlot, held.c_str(), gun ? gun->roundsInMag() : -1,
								gun ? gun->magsLeft() : -1, gun ? gun->fireMode() : -1, gun && gun->reloading() ? ", reloading" : "",
								me.vehicle ? ", in a vehicle" : "");
						}
					}
					std::printf("first person: %s, eye %.2f m over the origin, field of view %.3f rad, zoom level %d%s;"
								" playing",
						playerCamera.firstPerson ? "on" : "off", playerCamera.eyeHeight(), playerCamera.fieldOfView(),
						playerCamera.zoomLevel(), playerCamera.zoomed() ? " (zoomed in)" : "");
					for (const std::string& name : playerCamera.playing()) {
						std::printf(" %s", name.c_str());
					}
					std::printf("\n");
				}
			} else {
				std::fprintf(stderr, "no spawn point for team %d\n", options.playTeam);
			}
		}
		// (Set before the headless run below: a hosted --simulate's
		// demo.recordDemo records the local player's camera too.)
		nametags::Nametags::View tagView;
		bool tagViewSeen = false; // a frame drawn (tagView is the camera's)
		if (hosted) {
			// The demo's camera file on a server with a local player
			// (0x005f9630): the player's view as last drawn; without a
			// window its soldier's eye, looking its way; without a control
			// object (0, 100, 0) unturned.
			hosted->localCamera = [&round, &tagView, &tagViewSeen](net::CameraKeyframe& frame) {
				const int local = round.localPlayer();
				if (local < 0) {
					return false;
				}
				frame = {};
				frame.followed = 0xffffffffu;
				game::GameWorld::PlayerInfo info;
				if (!round.playerInfo(local, info) || !info.alive) {
					frame.position[1] = 100;
					return true;
				}
				float position[3] = {info.position.x, info.position.y + 1.6f, info.position.z};
				float yaw = info.yaw * 3.14159265f / 180.0f, pitch = info.aimPitch * 3.14159265f / 180.0f;
				if (tagViewSeen) {
					std::copy(tagView.position.begin(), tagView.position.end(), position);
					yaw = tagView.yaw;
					pitch = tagView.pitch;
				}
				float rows[9];
				host::demo_camera::rowsFromYawPitch(yaw, pitch, rows);
				host::demo_camera::quaternionFromRows(rows, frame.rotation);
				std::copy(position, position + 3, frame.position);
				return true;
			};
		}
		struct LocalCameraReset {
			host::HostedGame* hosted;
			~LocalCameraReset()
			{
				if (hosted) {
					hosted->localCamera = nullptr;
				}
			}
		} localCameraReset {hosted};
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
		// A dedicated server (--host --dedicated) runs on until stopped, or
		// for --simulate seconds over all its maps.
		const bool serverRun = options.dedicated && hosted;
		if (options.simulate > 0 || serverRun) {
			auto due = std::chrono::steady_clock::now();
			for (long i = 0; serverRun ? session.serverTicksLeft != 0 && !g_stopServer : i < options.simulate * 30L; ++i) {
				if (serverRun && session.serverTicksLeft > 0) {
					--session.serverTicksLeft;
				}
				if (hosted || session.client) {
					// A server (or a joined game) runs in real time: 30 ticks a second.
					due += std::chrono::microseconds(33333);
					std::this_thread::sleep_until(due);
				}
				step();
				if (gameLogic.ended() || (session.client && session.client->world().roundEnded())) {
					sinceEnd += 1.0f / 30.0f;
					showEndOfRound();
					if (endOfRound.loaded()) {
						endOfRound.advance(1.0f / 30.0f);
					}
				}
				if (nextMapDue()) {
					session.nextMap = true;
					if (serverNextMap) {
						std::printf("the admin asked for the next map\n");
					} else {
						std::printf("round over %.0f s ago: the next map\n", sinceEnd);
					}
					break;
				}
			}
#if BF2142_HAS_PYTHON
			if (serverRun && g_stopServer) {
				std::printf("the server stops\n");
				session.adminScript.shutdown(scripts);
			}
#endif
			if (endOfRound.loaded()) {
				std::printf("end of round shown: %zu script errors\n", endOfRound.scriptErrors().size());
				for (const std::string& error : endOfRound.scriptErrors()) {
					std::printf("  end of round script error: %s\n", error.c_str());
				}
			}
			if (soundTicks > 0) {
				std::printf("sound: %d ticks, %.3f ms a tick (%d weapon sounds, %d radio messages heard, %d voice-overs, "
							"%zu loops at the end)\n",
					soundTicks, soundSeconds * 1000.0 / soundTicks, gameAudio.weaponSounds, gameAudio.radioHeard,
					gameAudio.voiceOvers, gameSounds.loops());
				if (sound::enhancedAudioEnabled()) {
					std::printf("enhanced audio: HRTF status %d", sound::enhancedHrtfStatus());
#if BF2142_HAS_STEAM_AUDIO
					const sound::SteamAudioOcclusion::Stats& sa = steamAudio.stats();
					std::printf(", Steam Audio scene %zu triangles (built in %.0f ms), %.3f ms a tick for %.1f sources a tick (%.1f%% muffled)",
						sa.triangles, sa.buildSeconds * 1000.0, sa.simulateSeconds * 1000.0 / soundTicks,
						static_cast<double>(sa.sources) / soundTicks, sa.sources ? 100.0 * sa.muffled / sa.sources : 0.0);
#endif
					std::printf("\n");
				}
			}
			std::printf("after %d s: tickets PAC %.0f, EU %.0f; control points:", options.simulate,
				gameLogic.team(1).tickets, gameLogic.team(2).tickets);
			for (const auto& point : round.controlPointList()) {
				std::printf(" %d:%s", point.id, point.team == 1 ? "PAC" : point.team == 2 ? "EU" : "-");
			}
			std::printf("\n");
			std::printf("  state fingerprint %016llx\n", static_cast<unsigned long long>(round.stateFingerprint()));
			if (const auto lost = round.objectsOutOfWorld(); !lost.empty()) {
				std::printf("  %zu objects out of the world\n", lost.size());
			}
			for (const auto& titan : round.titans()) {
				std::printf("  titan %s: %.0f / %.0f hit points (%.1f %%%s), consoles %d of %d, core %s, at %.0f/%.0f/%.0f\n",
					titan.team == 1 ? "PAC" : "EU", titan.hitPoints, titan.maxHitPoints, titan.percent,
					titan.wreck ? ", a wreck" : titan.shield ? ", shield up" : ", shield down", titan.consolesLeft,
					titan.consoles, titan.coreDestroyed ? "destroyed" : titan.coreExposed ? "exposed" : "closed",
					titan.position.x, titan.position.y, titan.position.z);
			}
			// The spawners waiting on a game event (activateOnEvent: the Titan
			// shields' destroyedGameEvent switches them on).
			int eventOn = 0, eventOff = 0;
			for (const auto& spawner : round.spawners()) {
				if (spawner->settings().activateOnEvent != -1 || spawner->settings().deActivateOnEvent != -1) {
					(spawner->active() ? eventOn : eventOff) += 1;
				}
			}
			if (eventOn + eventOff > 0) {
				std::printf("  spawners on game events: %d on, %d off\n", eventOn, eventOff);
			}
			for (const auto& silo : round.missileSilos()) {
				std::printf("  silo %d: %s, %s, %d missiles fired\n", silo.controlPoint,
					silo.team == 1 ? "PAC" : silo.team == 2 ? "EU" : "neutral",
					silo.loaded ? "loaded" : ("reloading " + std::to_string(static_cast<int>(silo.reloadLeft)) + " s").c_str(),
					silo.launched);
			}
			if (session.client) {
				// A joined game: the server's players as we see them, and how
				// the prediction of our own soldier went.
				const net::RemoteWorld& world = session.client->world();
				for (const net::RemotePlayer& p : world.playerList()) {
					const net::RemoteObject* soldier = p.soldierGhost ? world.object(p.soldierGhost) : nullptr;
					if (p.local && prediction.active()) {
						const net::Vec3f f = prediction.feet();
						std::printf("  %-8s %s (ours) predicted at %.2f/%.2f/%.2f, server at %.2f/%.2f/%.2f\n", p.name.c_str(),
							p.team == 1 ? "PAC" : "EU ", f.x, f.y, f.z, world.controlObject().position.x,
							world.controlObject().position.y, world.controlObject().position.z);
					} else if (soldier && soldier->hasPosition) {
						std::printf("  %-8s %s at %.2f/%.2f/%.2f health %.2f (tick %u)\n", p.name.c_str(), p.team == 1 ? "PAC" : "EU ",
							soldier->position.x, soldier->position.y, soldier->position.z, soldier->health, soldier->lastTick);
					}
				}
				const host::SoldierPrediction::Stats& st = prediction.stats();
				std::printf("  prediction: %u moves stepped, %u replayed, %u records (%u for moves not remembered), "
							"correction mean %.4f m, max %.4f m, %u over 1 cm; %u of %u records with a mode-3 state\n",
					st.steps, st.replayed, st.records, st.unknownMoves, st.records ? st.sumCorrection / st.records : 0.0,
					st.maxCorrection, st.corrections, world.controlStates(), world.controlRecords());
			}
			for (const auto& player : round.playerList()) {
				const auto fired = round.playerRounds(player.id, false, false);
				const auto hit = round.playerRounds(player.id, true, false);
				const std::string seated = player.vehicle && round.isTitan(player.vehicle)
					? " (Titan gun " + std::to_string(player.seat + 1) + ")"
					: player.vehicle ? " (seat " + std::to_string(player.seat + 1) + ")" : "";
				std::printf("  %-8s %s %-6s at %.2f/%.2f/%.2f kills %d deaths %d, %d rounds fired, %d hit%s%s%s\n",
					player.name.c_str(), player.team == 1 ? "PAC" : "EU ", player.alive ? "alive" : "dead", player.position.x,
					player.position.y, player.position.z, player.kills, player.deaths,
					fired.empty() ? 0 : fired[0].count, hit.empty() ? 0 : hit[0].count, player.ai ? "  " : "",
					player.ai ? bots.behaviour(player.id).c_str() : "", seated.c_str());
				if (std::getenv("BF2142_BOT_STATS") && player.alive && !player.vehicle) {
					// The weapons' magazines (in hand marked *): the bots' reloads.
					std::string list;
					for (const auto& ammo : round.weaponAmmo(player.id)) {
						if (ammo.magSize > 0) {
							const bool held = std::any_of(player.carried.begin(), player.carried.end(), [&](const auto& c) {
								return c.first == player.inHandSlot && c.second == ammo.weapon;
							});
							list += " " + ammo.weapon + (held ? "*" : "") + " " + std::to_string(ammo.inMag) + "/"
								+ std::to_string(ammo.rounds);
						}
					}
					std::printf("    magazines:%s\n", list.c_str());
				}
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
		sources.hudXScale = hudXScale;
		// GenericProjectile simulation states intentionally live outside
		// ObjectManager. Submit them as transient render instances so their
		// shipped geometry and bundled child parts follow local rounds and
		// corrected remote projectile ghosts, then disappear with simulation.
		sources.transientModels = [&round, &templates](std::vector<render::TransientModel>& models) {
			game::GameWorld::ProjectileCamera camera;
			const uint32_t hidden = round.projectileCamera(round.localPlayer(), camera) ? camera.serial : 0;
			for (const game::ProjectileRenderState& projectile : game::projectileRenderStates(templates, round.projectiles(), hidden)) {
				const physics::Vec3 angles = projectile.rotation.yawPitchRoll();
				models.push_back({projectile.objectTemplate,
					{projectile.position.x, projectile.position.y, projectile.position.z}, {angles.x, angles.y, angles.z}});
			}
		};
		sources.animationFor = [&round](uint32_t object) { return round.animationFor(object); };
		sources.animationDurationFor = [&round](uint32_t object) { return round.animationDurationFor(object); };
		// The loading screen stays up while the renderer builds the level
		// (its last tenth), then the level's renderer takes the window.
		sources.buildProgress = [&](float done) {
			if (!loadingScreen) {
				return;
			}
			loadingStep(0.9f + 0.1f * done, 1);
			if (done >= 1) {
				// Not for a game we host: its clients would wait on our click (onTicks stops).
				if (startedGame && !session.client && !session.hosted && !g_demo && options.screenshot.empty() &&
					options.simulate == 0) {
					loadingBindings.setLevelLoaded(); // 0x004cbea0: the movie now offers JOIN GAME
					joinPending = true;
				}
				loadingScreen.reset();
			}
		};
		sources.effects = &round.effects();
		sources.rain = &rain;
		sources.renderer = &rendererSettings;
		sources.post = &postSettings;
		sources.shadows = &shadowSettings;
		sources.enhanced = &enhancedSettings;
		sources.hemi = &hemiSettings;
		sources.particles = &particleSettings;
		sources.overgrowth = &overgrowthSettings;
		// The decals' surfaces: the hit object's collision faces.
		sources.collisionFaces = [&round](uint32_t object, uint32_t owner, const effects::Vec3& center, float radius,
									 std::vector<std::array<effects::Vec3, 3>>& faces) {
			const physics::CollisionWorld* mesh = round.projectileCollision(object);
			if (!mesh) {
				return;
			}
			mesh->forEachFaceNear(center, radius,
				[&](const physics::Vec3& a, const physics::Vec3& b, const physics::Vec3& c, uint32_t, uint32_t by) {
					if (owner == 0 || by == owner) {
						faces.push_back({a, b, c});
					}
				});
		};
		sources.wind = &windSettings;
		sources.quality = &qualitySettings;
		// GSViewDistance (GameLogic.maximumLevelViewDistance, 0x004cc870):
		// where the objects' distance fade cuts (render/DistanceFade.h).
		sources.viewDistance = [&botSettings] { return botSettings.levelViewDistance; };
		// The seat camera's static interior sun (PlayerCamera::insideSun).
		sources.insideSun = [&playerCamera](render::InsideSun& sun) {
			PlayerCamera::InsideSun inside;
			if (!playerCamera.insideSun(inside)) {
				return false;
			}
			sun.dynamicLights = inside.dynamicLights;
			sun.direction = inside.direction;
			sun.color = inside.color;
			sun.ambient = inside.ambient;
			return true;
		};
		// The commander's overhead view (the map node's rectangle as drawn).
		if (const world::ObjectTemplate* camera = templates.find("MultiPlayerOverheadCamera")) {
			const auto value = [camera](const char* name, float fallback) {
				const auto it = camera->settings().find(name);
				return it != camera->settings().end() ? static_cast<float>(std::atof(it->second.c_str())) : fallback;
			};
			g_overheadFov = value("fOV", g_overheadFov);
			g_overheadHeight = value("height", g_overheadHeight);
			g_overheadCamera.speed = value("speed", g_overheadCamera.speed);
		}
		sources.titanObject = [&round](int team) { return round.titanOf(team); };
		sources.overheadView = [&hudManager](render::OverheadView& view) {
			if (!hudManager.overheadRect(view.rect)) {
				return false;
			}
			view.x = hudManager.overheadX();
			view.z = hudManager.overheadZ();
			view.fovY = g_overheadFov;
			view.height = g_overheadHeight;
			return true;
		};
		// Cloaked soldiers (GameWorld hides them) drawn as active camouflage.
		sources.cloaked = [&round](uint32_t object) {
			game::GameWorld::PlayerInfo info;
			const int id = round.playerOf(object);
			return id >= 0 && round.playerInfo(id, info) && info.cloaked && info.soldier == object;
		};
		// What the post-production follows: the vehicle's EMP, a Titan's core
		// exploding, and a zoomed vehicle weapon's camera effect
		// (weaponHud.enablePostProcessingOnGuiIndex naming the HUD shown).
		sources.postEffects = [&round, &playerCamera, cores = std::vector<uint32_t> {}](render::PostEffects& effects) mutable {
			game::GameWorld::PlayerInfo me;
			// The soldier's own EMP on foot, and a tinnitus from an explosion near.
			if (round.playerInfo(round.localPlayer(), me) && me.alive && !me.vehicle) {
				effects.soldierEmpLevel = me.empLevel;
			}
			effects.tinnitusSeconds = round.takeTinnitus();
			// Through a guided round's camera (useProjectileCamera): the
			// TVEffect pass (RendDX9 0x10048f80, PostProduction's "TVEffect").
			host::roundCameraEffects(round, round.localPlayer(), effects);
			if (round.playerInfo(round.localPlayer(), me) && me.alive && me.vehicle) {
				effects.vehicleEmpLevel = round.empLevel(me.vehicle);
				const auto seats = round.vehicleSeats(me.vehicle);
				const world::ObjectTemplate* gun = me.seat >= 0 && static_cast<size_t>(me.seat) < seats.size()
					? round.templates().find(seats[static_cast<size_t>(me.seat)].weapon)
					: nullptr;
				// The seat's vehicleHud.enablePostProcessingOnGuiIndex, or the
				// gun's weaponHud index naming the HUD shown (HudMenusVehicles.h).
				if (playerCamera.insideView()) {
					effects.cameraEffect =
						dice::hfe::menu::seatPostProcessing(controlledTemplate(round, me), gun, playerCamera.zoomed());
				}
			}
			for (const auto& titan : round.titans()) {
				const bool seen = std::find(cores.begin(), cores.end(), titan.object) != cores.end();
				if (titan.coreExplosion && !seen) {
					cores.push_back(titan.object);
					effects.titanExplosion = true;
				} else if (!titan.coreExplosion && seen) {
					cores.erase(std::find(cores.begin(), cores.end(), titan.object));
				}
			}
		};
		sources.objectEffects = &objectEffects;
		nametags::Nametags nametags(files);
		// The vehicles' render LODs for their tags' height (0x100c7000 at the
		// LOD 0x100c3390 picks; read once a template).
		std::map<std::string, nametags::MeshLods> tagLods;
		nametags.setMeshLods([&tagLods, &geometries, &files, &rendererSettings, &qualitySettings](const world::ObjectTemplate& t) {
			auto known = tagLods.find(t.name());
			if (known == tagLods.end()) {
				nametags::MeshLods lods;
				const geom::GeometryTemplate* geometry = geometries.find(t.geometry());
				std::vector<uint8_t> data;
				if (geometry && files.readFile(geometry->path, data)) {
					if (const std::optional<geom::MeshFile> mesh = geom::parseMesh(data, geometry->kind)) {
						const scene::RendererSettings& r = rendererSettings;
						const float scale = geometry->kind == geom::MeshKind::Skinned ? r.skinnedMeshLodDistanceScale
							: geometry->kind == geom::MeshKind::Bundled            ? r.bundleMeshLodDistanceScale
																				   : r.staticMeshLodDistanceScale;
						lods = nametags::meshLods(*mesh, geometry->lodDistances, scale * qualitySettings.geometryLodFactor());
					}
				}
				known = tagLods.emplace(t.name(), std::move(lods)).first;
			}
			return &known->second;
		});
		nametags.setIcons(&g_tagIcons);
		nametags.setFog(lights.fogStartEndAndBase); // the enemies' fog fade (0x100f09b0)
		nametags.setWorld(g_remoteHud); // a joined game's: the server's players and requests
		g_nametags = &nametags;
		struct TagsReset {
			~TagsReset() { g_nametags = nullptr; }
		} tagsReset;
		// The camera inside a Titan, as the renderer found it last frame.
		static bool cameraInTitan = false;
		sources.cameraInTitanOut = &cameraInTitan;
		// The lock-on weapons' markers (Bf2ObjectMarkerNode, 0x007e6520):
		// the enemy objects carrying a target object of the marker's type
		// (HeatObject TTHeat on the aircraft, LaserTarget_vehicle TTLaser,
		// Beacon TTBeacon; -1 any of them) in front of the camera, inside the
		// marker's rectangle and within reach, nearest first. Their state is 0
		// (a candidate), or the local weapon's lock on it
		// (GameWorld::weaponLock: locking 1, locked 2); the reach is the
		// weapon's target.maxDistance when it has one.
		std::map<const world::ObjectTemplate*, int> targetTypes; // a template's target object's type (-1 none)
		std::function<int(const world::ObjectTemplate*, int)> targetTypeOf = [&](const world::ObjectTemplate* t, int depth) {
			if (!t || depth > 6) {
				return -1;
			}
			const auto known = targetTypes.find(t);
			if (known != targetTypes.end()) {
				return known->second;
			}
			int type = -1;
			const auto own = t->settings().find("targetType");
			if (own != t->settings().end()) {
				type = dice::hfe::menu::targetTypeIndex(own->second);
			}
			for (const world::ChildTemplate& child : t->children()) {
				if (type < 0) {
					type = targetTypeOf(round.templates().find(child.name), depth + 1);
				}
			}
			targetTypes[t] = type;
			return type;
		};
		hudManager.setMarkerSource([&](const hud::HudManager::MarkerQuery& query) {
			std::vector<hud::HudManager::MarkerTarget> targets;
			const int team = round.playerTeam();
			const game::GameWorld::WeaponLockInfo lock = round.weaponLock(round.localPlayer());
			const float kReach = lock.maxDistance > 0 ? lock.maxDistance : 600;
			for (const auto& object : round.objects().objects()) {
				if (!object || object->destroyed() || object->hidden() || object->team() == 0 || object->team() == team) {
					continue;
				}
				const int type = targetTypeOf(object->objectTemplate(), 0);
				if (type < 0 || (query.lockOnType >= 0 && type != query.lockOnType)) {
					continue;
				}
				const auto& at = object->position();
				const float dx = at.x - tagView.position[0], dy = at.y - tagView.position[1], dz = at.z - tagView.position[2];
				const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
				float x = 0, y = 0;
				if (distance > kReach || !nametags::Nametags::project(tagView, {at.x, at.y, at.z}, x, y, false, 0)) {
					continue;
				}
				if (x < query.x0 || x > query.x1 || y < query.y0 || y > query.y1) {
					continue;
				}
				targets.push_back({x, y, object->id() == lock.target ? lock.state : 0, distance});
			}
			std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return a.distance < b.distance; });
			return targets;
		});
		sources.overlay = [&](float seconds) {
			// Leaving the spawn screen closes the customise screen (HudManager's menu hide calls 0x00798070, e.g. 0x00754df4);
			// CustomInfoShow otherwise keeps the minimap's MapItems node hidden (HudElementsMap.con: AND CustomInfoShow 0).
			if (kitMenu.customOpen() && hudManager.number("SpawnShow") == 0) {
				kitMenu.closeCustomInterface();
			}
			kitMenu.setUnlocked(round.localPlayerUnlocks, round.unlockAllByDefault);
			kitMenu.setKit(localKit);
			kitMenu.skipUnchanged = enhancedSettings.enabled;
			kitMenu.update(round.playerTeam() == 1 ? 1 : 2);
			kitMenu.commit();
			trackKills();
			chat.update(seconds);
			mapVote.setMaps([&] {
				std::vector<std::string> names;
				if (session.client) {
					for (const auto& map : session.client->world().mapList()) {
						names.push_back(map.name);
					}
				} else {
					for (const auto& map : session.playlist) {
						names.push_back(map.level);
					}
					if (names.empty()) {
						names.push_back(options.level);
					}
				}
				return names;
			}());
			mapVote.update();
			{
				std::vector<dice::hfe::menu::VoipSpeaker> talking;
				bool sending = false, commanding = false;
				if (session.client && session.voice) {
					sending = session.voice->sendingVoice();
					for (const net::RemotePlayer& p : session.client->world().playerList()) {
						if (!p.local && session.voice->client().talking(p.id)) {
							talking.push_back({p.name, p.commander, p.squad});
						}
						commanding = commanding || (p.local && p.commander);
					}
				}
				dice::hfe::menu::updateVoipList(hudManager, talking, sending, commanding);
			}
			// A joined game's scoreboard has the players' voice boxes.
			g_scoreboardVoice = nullptr;
			squadMenu.voiceState = nullptr;
			if (session.client && session.voice) {
				squadMenu.voiceState = [&session, &squadMenu](int id) {
					game::GameWorld::PlayerInfo info;
					if (!squadMenu.playerInfoFor(id, info) || info.ai) {
						return -1; // a bot has no voice
					}
					return session.voice->client().muted(id) ? 2 : session.voice->client().talking(id) ? 1 : 0;
				};
				g_scoreboardVoice = [&session](int id) -> std::string {
					const bool muted = session.voice->client().muted(id);
					const bool talking = session.voice->client().talking(id);
					return muted ? "Ingame/Scoreboard/Icons/checkbox_voipMute.tga"
						: talking ? "Ingame/Scoreboard/Icons/checkbox_voipTalk.tga"
								  : "Ingame/Scoreboard/Icons/checkbox_voipInactive.tga";
				};
			}
			localHud.scoreOf = scoreOf;
			g_weaponZoomed = playerCamera.zoomLevel() > 0; // the firearm's slot 300 (its zoom)
			// The seat camera's mode; a third-person choice not yet given to
			// the seat's camera (it starts in Chase, PlayerCamera.h) is Chase.
			g_cameraMode = static_cast<int>(playerCamera.mode());
			if (!playerCamera.firstPerson && playerCamera.mode() == game::camera::Mode::Inside) {
				g_cameraMode = static_cast<int>(game::camera::Mode::Chase);
			}
			float gunHeading = 0;
			const std::optional<float> mapHeading = playerCamera.seatMapHeading(gunHeading)
				? std::optional<float>(gunHeading) : std::nullopt;
			updateHud(hudManager, hudWorld(), round, gameLogic, scoreboardHeld, commanderScreen, mapHeading);
			// The minimap's combat-area layer and framing (HudCombatArea.h):
			// the areas that apply to the local player's team and the
			// category of what he controls (0 on foot).
			{
				game::GameWorld::PlayerInfo me;
				int category = 0;
				const bool known = hudWorld().playerInfo(hudWorld().localPlayer(), me);
				if (known && me.alive && me.vehicle) {
					if (const world::ObjectTemplate* controlled = controlledTemplate(round, me)) {
						category = game::GameWorld::vehicleCategory(*controlled);
					}
				}
				combatAreaLayer.update(files, combatAreas, known ? me.team : 0, category, hudManager);
			}
			{
				// The vehicle's warnings: the seekers locked on it (their
				// seek's target type; chasing once past the tracking delay).
				std::vector<dice::hfe::menu::WarningHud::Seeker> seekers;
				const world::Object* vehicle = round.playerVehicle();
				const auto& flying = round.projectiles();
				for (size_t i = 0; vehicle && i < flying.size(); ++i) {
					const game::Projectile& p = flying[i];
					if (!p.alive || !p.settings || p.lockedOn != vehicle->id()) {
						continue;
					}
					using Seek = game::ProjectileSettings::Seek;
					const int type = p.settings->seek == Seek::Magnet ? 3 : p.settings->seek == Seek::Aim ? 4
						: p.settings->seek == Seek::Beacon ? 5 : 2;
					seekers.push_back({static_cast<uint32_t>(i + 1), type, p.followFrom >= 0 && p.age >= p.followFrom});
				}
				warningHud.update(hudManager, vehicle ? vehicle->objectTemplate() : nullptr, seekers);
				// The help pop-ups: the vehicle or the weapon taken, low
				// armour, the objects with closeTo help within reach (looked
				// for twice a second).
				dice::hfe::menu::HelpPopups::Context help;
				help.vehicle = vehicle ? vehicle->objectTemplate() : nullptr;
				help.vehicleHitPoints = vehicle ? round.hitPoints(vehicle->id()) : 0;
				if (vehicle) {
					const float most = static_cast<float>(
						dice::hfe::menu::hudNumber(vehicle->objectTemplate(), "armor.maxHitPoints", 0));
					help.vehicleShare = most > 0 ? help.vehicleHitPoints / most : 1;
				}
				if (const world::Object* held = round.objects().find(round.playerWeaponObject())) {
					help.weapon = held->objectTemplate();
				}
				// The help manager's state (0x00649d80 / 0x00646cb0).
				{
					game::GameWorld::PlayerInfo self;
					const bool known = hudPlayer(hudLocal(), self);
					help.playing = known;
					help.alive = known && self.alive;
					help.manDown = known && self.manDown;
					help.inVehicle = vehicle != nullptr;
					help.onFoot = help.alive && !vehicle;
					help.spawnScreen = known && (self.manDown ? g_deploymentOpen : dice::main::deploymentScreenShown(self.alive, g_deploymentOpen, g_spawnScreenHidden));
					help.commanderScreen = commanderScreen;
					help.menuOpen = scoreboardHeld || commanderScreen || g_mapMenu != 0;
					help.squad = known ? self.squad : 0;
					help.squadLeader = known && self.squadLeader;
					help.roundTime = round.roundTime();
					help.zoomed = playerCamera.zoomed();
					// The menu state (0x007b8b80: the spawn screen's members half
					// 0x12 for a squad leader, 0x13 for the rest; the kits 1).
					// The commander screen is 0x10, its artillery's and orbital
					// strike's placement 0x17 and 0x22 (toggleArtilleryState →
					// slot 0x360, 0x007d73a0; toggleOrbitalStrikeState → slot
					// 0x340, 0x007d77a0).
					const int commanderState = armedTool == Ability::Artillery ? 0x17
						: armedTool == Ability::OrbitalStrike                 ? 0x22
																			  : 0x10;
					help.menuState = help.spawnScreen ? (membersShown ? (help.squadLeader ? 0x12 : 0x13) : 1)
						: commanderScreen              ? commanderState
						: help.menuOpen                ? -1
													   : 0;
					// The parachute's (case 0x0d): the controlled object as a
					// soldier (0x0074e8c0: the player's slot 0xb4 as 0xc4d7)
					// falling (its slot 0x78, +0x328: under -10 m/s).
					help.parachute = known && round.soldierFalling(round.localPlayer());
					help.commander = known && self.commander;
					// The game's reach tests (cases 0x43, 0x47).
					help.ladderNear = help.onFoot && round.ladderInReach(round.localPlayer(), 3);
					help.vehicleNear = help.onFoot && round.vehicleInReach(round.localPlayer());
					// The controlled object's standardHelpEnabled (0x00648872 /
					// 0x00648948): the seat's or vehicle's, on foot the soldier's.
					{
						const world::ObjectTemplate* controlled = nullptr;
						if (known && self.vehicle) {
							controlled = controlledTemplate(round, self);
						} else if (const world::Object* soldier = round.player()) {
							controlled = soldier->objectTemplate();
						}
						help.standardHelp = dice::hfe::menu::standardHelpEnabled(controlled);
					}
					// Event-only messages the HUD lets through (slot 0x128,
					// 0x00751540): 0x33 captureFlag as the player comes into a
					// control point's area outside Titan mode (0x0044b560 on the
					// capture state), 0x18 commanderApply once he commands
					// (0x007d.. after the commander screen opens for him).
					static bool wasInArea = false, wasCommander = false;
					bool inArea = false;
					if (help.alive && options.gameMode.find("ti") == std::string::npos) {
						// The control points as the HUD has them (a joined game's
						// are the server's ghosts).
						for (const auto& cp : hudWorld().controlPointList()) {
							const float dx = cp.position.x - self.position.x, dz = cp.position.z - self.position.z;
							inArea = inArea || dx * dx + dz * dz <= cp.radius * cp.radius;
						}
					}
					if (inArea && !wasInArea) {
						helpPopups.enableTutorial(0x33);
					}
					wasInArea = inArea;
					if (help.commander && !wasCommander) {
						helpPopups.enableTutorial(0x18);
					}
					wasCommander = help.commander;
					// A weapon key whose weapon the squad is too small for
					// (0x004d2a50 at 0x004d2ea9: slot 0x188,
					// minimumSquadSizeForSelection) lets through 0x52
					// Squadleader_inventory_selectunlock (shown to a squad
					// leader, case 22).
					if (round.takeSquadSizeRefused(round.localPlayer()) > 0) {
						helpPopups.enableTutorial(0x52);
					}
					// The commander HUD's asset updates (0x007aa9b0 each frame):
					// a damaged asset (its armour share below 1, above 0) lets
					// through 0x36 uavDamaged (0x007aa780), 0x37 radarDamaged
					// (0x007aa6e0), 0x38 artilleryDamaged (0x007aa500), 0x39
					// bombardmentDamaged (0x007a9500), unless the player leads
					// a squad of a team without a commander (0x007a9240).
					if (known && (help.commander || !help.squadLeader || hudWorld().commander(self.team) >= 0)) {
						static const std::pair<Ability, int> kDamaged[] = {{Ability::Uav, 0x36},
							{Ability::Satellite, 0x37}, {Ability::Artillery, 0x38}, {Ability::OrbitalStrike, 0x39}};
						for (const auto& [ability, index] : kDamaged) {
							const auto state = round.abilityState(self.team, ability);
							if (state.available && state.armor > 0 && state.armor < 1) {
								helpPopups.enableTutorial(index);
							}
						}
					}
					// The map's zoom step in the commander screen (0x0077e850):
					// 0x1e ZOOM_inOut is held again, 0x2a SATELLITEVIEW_moveKeys
					// let through once zoomed in.
					static int commanderZoomWas = -1;
					if (commanderScreen && hudManager.commanderMapZoom() != commanderZoomWas && commanderZoomWas >= 0) {
						helpPopups.disableTutorial(0x1e);
						if (hudManager.commanderMapZoom() > 0) {
							helpPopups.enableTutorial(0x2a);
						}
					}
					commanderZoomWas = commanderScreen ? hudManager.commanderMapZoom() : -1;
					std::set<int> squads;
					for (const auto& p : round.playerList()) {
						if (known && p.team == self.team && p.squad > 0) {
							squads.insert(p.squad);
						}
					}
					help.teamSquads = static_cast<int>(squads.size());
					help.playTime = session.menuBindings && session.menuBindings->soldierStats()
						? static_cast<float>(session.menuBindings->soldierStats()->timePlayed)
						: 0;
					// The kit's weapons' magazines (case 0x3b): the one in hand.
					if (const game::FireArm* gun = round.playerWeapon(); gun && help.weapon && gun->settings().magSize > 0) {
						help.ammo.push_back({help.weapon, static_cast<float>(gun->roundsInMag()) / static_cast<float>(gun->settings().magSize)});
					}
				}
				static float nearbyCheck = 0;
				static std::vector<uint32_t> helpful;
				nearbyCheck -= seconds;
				const world::Object* me = round.player();
				if (nearbyCheck <= 0 && me) {
					nearbyCheck = 0.5f;
					helpful.clear();
					for (const auto& object : round.objects().objects()) {
						if (object && object->objectTemplate()
							&& object->objectTemplate()->settings().count("helpHud.closeToDistance")) {
							helpful.push_back(object->id());
						}
					}
				}

				for (const uint32_t id : me ? helpful : std::vector<uint32_t> {}) {
					if (const world::Object* object = round.objects().find(id)) {
						const float dx = object->position().x - me->position().x, dz = object->position().z - me->position().z;
						const int side = object->team() == 0 ? 0 : object->team() == round.playerTeam() ? 1 : 2;
						help.nearby.push_back({object->objectTemplate(), std::sqrt(dx * dx + dz * dz), side});
					}
				}
				helpPopups.update(hudManager, help, seconds);
				if (!help.spawnScreen) helpPopups.tutorial(hudManager, help, seconds);
				// The lock tones (GameWorld::takeLockTones, 0x00657ce0).
				for (const auto& tone : round.takeLockTones()) {
					tone.play ? sounds.playSound(tone.sound) : sounds.stopSound(tone.sound);
				}
				// The close-to indications of the server's own player (the
				// local event manager's 2/0x3e and 2/0x3f, 0x0059a6c0): its
				// CloseToObject<type> variables (0x0079a0e0). A joined
				// client's come as PostRemote messages (below).
				if (!session.client) {
					for (const auto& e : round.takeCloseToEvents(round.localPlayer())) {
						dice::hfe::menu::closeToObject(hudManager, e.type, e.enter);
					}
					// The team damage limits (sgl_setParam, scoringCommon.py's
					// setTeamDamagePointLimit) and the server's own player's
					// team damage events (0/0xb, TeamDamage.cpp).
					const auto limit = [&gameLogic](const char* name, float fallback) {
						const auto it = gameLogic.limits.find(name);
						return it != gameLogic.limits.end() ? static_cast<float>(it->second.asNumber()) : fallback;
					};
					round.setTeamDamageLimits(limit("teamDamageScoreLimit", 50), limit("teamVehicleDamageScoreLimit", 25));
					for (const auto& e : round.takeTeamDamage(round.localPlayer())) {
						game::GameWorld::PlayerInfo victim;
						const bool up = round.playerInfo(e.victim, victim) && victim.alive && !victim.manDown;
						using Notice = game::GameWorld::TeamDamageNotice;
						switch (game::GameWorld::teamDamageNotice(e, round.localPlayer(), up)) {
						case Notice::SayFriendlyFire:
							sayRadio(14, game::GameWorld::kFriendlyFireVoice, e.attacker); // TCPlayer
							break;
						case Notice::VictimSaysFriendlyFire:
							round.radioMessage(e.victim, 14, game::GameWorld::kFriendlyFireVoice, round.localPlayer());
							break;
						case Notice::PlayerLine:
							chat.addPersonal(localization.text("HUD_HUD_TEAM_DAMAGE_PLAYER"));
							break;
						case Notice::VehicleLine:
							chat.addPersonal(localization.text("HUD_HUD_TEAM_DAMAGE_VEHICLE"));
							break;
						case Notice::None:
							break;
						}
					}
				}
				// AbilityHud (on the kits: S_Heal, S_Repair, S_Resupply): the
				// ability bar's (0x007c3af0 through the kit's AbilityHud
				// slots 0x3c / 0x40 / 0x44): in a vehicle while what the
				// player gave shows, on foot while the tool in hand treats.
				static std::string supplySound;
				std::string wanted;
				game::GameWorld::PlayerInfo self;
				const world::Object* kit = round.playerInfo(round.localPlayer(), self) ? round.objects().find(self.kit) : nullptr;
				if (kit && g_abilityBar.out().sound >= 0) {
					wanted = dice::hfe::menu::abilitySound(*kit->objectTemplate(), g_abilityBar.out().sound);
				}
				if (wanted != supplySound) {
					if (!supplySound.empty()) {
						sounds.stopSound(supplySound);
					}
					supplySound = wanted;
					if (!supplySound.empty()) {
						sounds.playSound(supplySound);
					}
				}
			}
			squadMenu.update(hudLocal());
			updateKitAbilities(hudManager, round, gameLogic, templates);
			commanderMenu.allSquads = g_allSquads;
			commanderMenu.update(hudLocal());
			squadRequests.localPlayer = hudLocal();
			squadRequests.tick(seconds);
			questions.tick(seconds);
			// The local round's offer of command to our player (offline the
			// game posts 2/0x23 itself, 0x004c5070).
			if (!joinedServer() && round.localPlayer() >= 0) {
				const int team = round.playerTeam();
				const int offered = round.commanderOffer(team) == round.localPlayer() ? round.localPlayer() : -1;
				if (offered >= 0 && localOfferAsked != offered) {
					askCommanderOffer(team);
				}
				localOfferAsked = offered;
			}
			{
				game::GameWorld::PlayerInfo me;
				const host::HudWorld& shown = hudWorld();
				const bool known = shown.playerInfo(shown.localPlayer(), me);
				dice::hfe::menu::updateSquadInfo(hudManager, known, me.commander, me.squad, me.squadLeader, commanderScreen,
					[&localization](const std::string& id) { return localization.text(id); });
			}
			{
				// The minimap's UAV (0x00788600): its time run out or its
				// asset gone, the local player hears "uav_offline" (game
				// slot 0x58).
				physics::Vec3 uavAt;
				float uavRadius = 0;
				const bool uavUp = round.uavArea(myTeam(), uavAt, uavRadius);
				if (uavWasUp && !uavUp) {
					round.hearRadio(round.localPlayer(), 1, "uav_offline");
				}
				uavWasUp = uavUp;
			}
			{
				// The squad's orders in the world (the order object
				// 0x005b8b20): a placed order (kind 4 move, 5 attack, 6 / 7
				// defend and repair, 8 / 9 attack) with no target object sets
				// the Bundle ordermarker_move / _attack / _defend (its
				// e_marker_* effects) on the ground at its point; the name tag
				// (0x100f4620) draws its icon. Replaced when the order changes,
				// gone with it.
				game::GameWorld::PlayerInfo me;
				const bool known = hudPlayer(hudLocal(), me) && me.squad > 0;
				// The squad's orders as the HUD has them (a joined game's: the
				// server's IssueSquadOrder).
				game::GameWorld::SquadOrder held[2];
				const game::GameWorld::SquadOrder* orders[2] = {
					known && hudWorld().squadOrder(me.team, me.squad, false, held[0]) ? &held[0] : nullptr,
					known && hudWorld().squadOrder(me.team, me.squad, true, held[1]) ? &held[1] : nullptr};
				for (size_t i = 0; i < 2; ++i) {
					OrderMarker want;
					const int kind = orders[i] ? static_cast<int>(orders[i]->type) : -1;
					if (kind >= 4 && kind <= 9) {
						want.kind = kind;
						want.at = orders[i]->position;
					}
					OrderMarker& have = orderMarkers[i];
					if (have.kind == want.kind && have.at.x == want.at.x && have.at.z == want.at.z) {
						continue;
					}
					if (have.object) {
						round.objects().remove(have.object);
					}
					have = want;
					const char* name = want.kind == 4 ? "ordermarker_move"
						: want.kind == 6 || want.kind == 7 ? "ordermarker_defend"
						: want.kind >= 0                   ? "ordermarker_attack"
														   : nullptr;
					nametags.setOrderTag(static_cast<int>(i), false);
					if (world::ObjectTemplate* t = name ? templates.find(name) : nullptr) {
						// On the ground, the water or a static object under the
						// point, up by half its radius (GameWorld::orderMarkerHeight);
						// its tag by it (orderTagPoint).
						const float height = round.orderMarkerHeight(want.at, markerRadius(*t));
						if (world::Object* marker = round.objects().create(t, {want.at.x, height, want.at.z})) {
							have.object = marker->id();
						}
						const physics::Vec3 tag = game::GameWorld::orderTagPoint(want.at, height);
						nametags.setOrderTag(static_cast<int>(i), true, {want.at.x, want.at.y, want.at.z}, {tag.x, tag.y, tag.z});
					}
				}
			}
			{
				// ShowCommanderAcceptReject (0x00753b40): the commander's rose
				// over a request of his team, its Accept and Reject.
				game::GameWorld::PlayerInfo me;
				const bool commands = hudPlayer(hudLocal(), me) && me.commander;
				hudManager.set("ShowCommanderAcceptReject", commands && g_nametags && g_nametags->selectedRequest() >= 0 ? 1 : 0);
			}
			hudManager.set("KitsShow", membersShown ? 0 : 1);
			hudManager.set("MembersShow", membersShown ? 1 : 0);
			// Zoomed in, the weapon's zoomed HUD (weaponHud.altGuiIndex: the
			// scopes' dots and reticles).
			if (playerCamera.zoomed() && !round.playerVehicle()) {
				if (const world::Object* held = round.objects().find(round.playerWeaponObject())) {
					const auto& settings = held->objectTemplate()->settings();
					const auto alt = settings.find("weaponHud.altGuiIndex");
					if (alt != settings.end()) {
						hudManager.set("GuiIndex", static_cast<float>(std::atoi(alt->second.c_str())));
					}
				}
			}
			// The 3D name tags under the HUD (RendDX9's NametagManager).
			nametags.setInsideTitan(cameraInTitan);
			std::vector<hud::HudQuad> quads = nametags.frame(round, round.objects(), tagView, seconds);
			if (!rendererSettings.drawNametags) {
				quads.clear(); // renderer.drawNametags (+0x697, 0x1002cef0)
			}
			std::vector<hud::HudQuad> hud = hudManager.frame(seconds);
			if (std::getenv("BF2142_INPUT_TRACE")) {
				static int lastGui = -999, lastDeployment = -1;
				const int gui = static_cast<int>(hudManager.number("GuiIndex"));
				if (gui != lastGui || lastDeployment != static_cast<int>(g_deploymentOpen)) {
					lastGui = gui; lastDeployment = g_deploymentOpen;
					std::printf("hud-trace gui=%d camera=%d deployment=%d map=%g custom=%g\n", gui, g_cameraMode, g_deploymentOpen, hudManager.number("MapShow"), hudManager.number("CustomInfoShow"));
					for (const auto& q : hud) {
						if (q.texture.find("ingameMap") != std::string::npos || q.texture.find("combatArea") != std::string::npos)
							std::printf("hud-map %s rect=%g/%g/%g/%g uv=%g/%g/%g/%g\n", q.texture.c_str(), q.x0,q.y0,q.x1,q.y1,q.u0,q.v0,q.u1,q.v1);
					}
				}
			}
			quads.insert(quads.end(), hud.begin(), hud.end());
			// The commander screen's pointer (the system cursor is hidden
			// there): over the map the armed tool's (HudManager.setMouseTexture*),
			// else the map's.
			float mapX = 0, mapZ = 0;
			if (commanderScreen && !(showMenu && startedGame)) {
				using Pointer = hud::HudManager::Pointer;
				Pointer kind = Pointer::Full;
				const bool overMap = hudManager.mapWorldAt(hudManager.pointerX(), hudManager.pointerY(), mapX, mapZ);
				if (overMap && moveTitanArmed) {
					kind = Pointer::MoveTitan;
				} else if (overMap && armedTool) {
					kind = *armedTool == Ability::Artillery ? Pointer::Artillery
						: *armedTool == Ability::Uav         ? Pointer::Uav
						: *armedTool == Ability::Supplies    ? Pointer::Supply
															 : Pointer::OrbitalStrike;
				}
				hud::HudQuad pointer;
				if (hudManager.pointerQuad(kind, pointer)) {
					quads.push_back(pointer);
				}
			}
			return quads;
		};
		// A game started from the menu shows it on Esc (Logic.isGameRunning).
		if (session.menuBindings) {
			dice::hfe::menu::updateMenuGameState(menu, *session.menuBindings, startedGame.has_value());
		}
		{
			sources.menu = [&menu, &options, &showMenu, &endOfRound, &joinPending, &loadingMovie](float seconds) {
				ui::Player* shown = joinPending ? &loadingMovie : showMenu && menu.loaded() ? &menu : endOfRound.loaded() ? &endOfRound : nullptr;
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
		sources.partBone = [&round](uint32_t object, const std::string& part, anim::Matrix& local) {
			return round.partBone(object, part, local);
		};
		sources.partLook = [&round](uint32_t object, const std::string& part) { return round.titanPartLook(object, part); };
		sources.partPose = [&round, &session, &ghostObjects](uint32_t object, const std::string& part, world::Vec3& offset,
							   world::Vec3& rotation) {
			if (round.partPose(object, part, offset, rotation)) {
				return true;
			}
			// A joined game's vehicle: its ghost groups' turrets and wheels.
			if (!session.client) {
				return false;
			}
			for (const auto& [ghost, id] : ghostObjects) {
				net::Vec3f o, r;
				if (id == object && session.client->world().partPose(ghost, part, o, r)) {
					offset = {o.x, o.y, o.z};
					rotation = {r.x, r.y, r.z};
					return true;
				}
			}
			return false;
		};
		// The first-person pass: the arms and the weapon at the eye, the
		// player's own soldier left out of the world.
		sources.hiddenInView = [&playerCamera](uint32_t object) { return playerCamera.hides(object); };
		sources.cockpitVehicle = [&playerCamera]() -> uint32_t {
			PlayerCamera::Cockpit cockpit;
			return playerCamera.cockpit(cockpit) ? cockpit.vehicle : 0;
		};
		// Inside a vehicle in first person its cockpit geometry (0x00612880).
		sources.geometryFor = [&playerCamera, &round, &objects](uint32_t object, const std::string& source, int& geometry, int& lod) {
			// A kit on the ground: its template's geometry.dropGeom.
			const world::Object* kit = round.isDroppedKit(object) ? objects.find(object) : nullptr;
			if (kit && kit->objectTemplate()) {
				const auto& settings = kit->objectTemplate()->settings();
				const auto drop = settings.find("geometry.dropGeom");
				if (drop != settings.end()) {
					geometry = std::atoi(drop->second.c_str());
					lod = 0;
					return true;
				}
			}
			// A seated crew's kit: its seat's crewKitIndex (0x0056cc50).
			if (const int crew = round.kitGeometry(object); crew >= 0) {
				geometry = crew;
				lod = 0;
				return true;
			}
			// A worn kit: its template's geometry.kit (0x0056cc50 with a parent).
			if (const world::Object* worn = objects.find(object); worn && worn->objectTemplate() &&
				worn->objectTemplate()->typeName() == "Kit") {
				const auto& settings = worn->objectTemplate()->settings();
				if (const auto index = settings.find("geometry.kit"); index != settings.end()) {
					geometry = std::atoi(index->second.c_str());
					lod = 0;
					return true;
				}
			}
			// A broken prop of two geometries or more: its second (Props.cpp).
			if (const int wreck = round.propGeometry(object); wreck >= 0) {
				geometry = wreck;
				lod = 0;
				return true;
			}
			PlayerCamera::Cockpit cockpit;
			if (!playerCamera.cockpit(cockpit, object) || !PlayerCamera::cockpitCovers(cockpit, source)) {
				return false;
			}
			geometry = cockpit.subGeometry;
			lod = cockpit.lod;
			return true;
		};
		sources.firstPerson = [&playerCamera](render::FirstPersonScene& scene) {
			const std::vector<PlayerCamera::Model> models = playerCamera.models();
			if (models.empty()) {
				return false;
			}
			for (const PlayerCamera::Model& model : models) {
				render::FirstPersonModel drawn;
				drawn.geometry = model.geometry;
				drawn.geometryIndex = model.geometryIndex;
				drawn.lod = model.lod;
				std::copy(model.world.begin(), model.world.end(), drawn.world.m.begin());
				drawn.pose = model.pose;
				for (const anim::Matrix& part : model.parts) {
					render::Mat4 m;
					std::copy(part.begin(), part.end(), m.m.begin());
					drawn.parts.push_back(m);
				}
				scene.models.push_back(std::move(drawn));
			}
			scene.effects = &playerCamera.effects();
			return true;
		};
		ViewerOptions viewer;
		viewer.screenshot = options.screenshot;
		viewer.camera = options.camera;
		// Before the first spawn the view is the level's before-spawn camera
		// (GameLogic.setBeforeSpawnCamera); a joined game's soldier is the
		// server's control object, which the view follows once it has one.
		// (Not a demo's: its own camera, or a camera file, is the view.)
		if (viewer.camera.empty() && gameLogic.beforeSpawnCamera.set && !round.player() && !g_demo &&
			!(session.client && session.client->world().hasControlObject())) {
			const auto& c = gameLogic.beforeSpawnCamera;
			char text[128];
			std::snprintf(text, sizeof(text), "%f/%f/%f/%f/%f", c.position[0], c.position[1], c.position[2], c.rotation[0],
				c.rotation[1]);
			viewer.camera = text;
		}
		{
			// Input to the menu while it is up, else to the end-of-round movie.
			const auto toMenu = menuInput(menu, [&session](int key) {
				if (session.menuBindings) {
					session.menuBindings->setLastInput(controlForFlashKey(key));
				}
			});
			const auto toEndOfRound = menuInput(endOfRound);
			const auto toLoading = menuInput(loadingMovie);
			viewer.onMenuInput = [toMenu, toEndOfRound, toLoading, &showMenu, &menu, &joinPending](const ViewerOptions::MenuInput& input) {
				(joinPending ? toLoading : showMenu && menu.loaded() ? toMenu : toEndOfRound)(input);
			};
			if (menu.loaded() && !startedGame) {
				viewer.menuActive = [&showMenu] { return showMenu; }; // --menu over a level: always up
			} else {
				viewer.menuActive = [&showMenu, &menu, &endOfRound, &joinPending] {
					return joinPending || (showMenu && menu.loaded()) || endOfRound.loaded();
				};
			}
			if (startedGame) {
				viewer.onEscape = [&showMenu, &joinPending] { if (joinPending) return; if (g_deploymentOpen) g_deploymentOpen = false; else showMenu = !showMenu; };
			}
			viewer.menuCursor = [&session, &showMenu] {
				return showMenu && session.menuBindings ? session.menuBindings->waitCursor() : 0;
			};
		}
		if (session.menuBindings) {
			// ControlSettings: 0.5 is the game's default sensitivity.
			const dice::hfe::menu::MenuBindings* settings = session.menuBindings.get();
			viewer.mouseScale = [settings, &playerCamera] {
				return static_cast<float>(settings->setting("ControlSettings.MouseSensitivity", 0.5) / 0.5)
					* playerCamera.mouseScale();
			};
			viewer.invertMouse = [settings] { return settings->setting("ControlSettings.InvertMouse", 0) != 0; };
		}
		if (!controls.maps().empty()) {
			viewer.controls = &controls;
		}
		viewer.gameControllers = &session.gameControllers;
		if (session.menuBindings) {
			viewer.onControllerInput = [&session](const std::string& device, const std::string& control, float direction) {
				session.menuBindings->setLastInput(device, control, direction);
			};
		}
		if (!viewer.mouseScale) {
			viewer.mouseScale = [&playerCamera] { return playerCamera.mouseScale(); }; // zoomed in: slower (0x00586420)
		}
		// Ragdolls' level of detail goes by the camera (GameWorld::setCamera).
		viewer.onCamera = [&round](const float position[3], const float forward[3]) {
			round.setCamera({position[0], position[1], position[2]}, {forward[0], forward[1], forward[2]});
		};
		viewer.onView = [&tagView, &tagViewSeen](const float position[3], float yaw, float pitch, float fovY, float aspect) {
			tagView = {{position[0], position[1], position[2]}, yaw, pitch, fovY, aspect};
			tagViewSeen = true;
			if (g_demo) {
				// The view a camera recording takes (0x005f9630: the camera's
				// matrix +0x18 and position +0x48).
				g_demo->setView(position, yaw, pitch);
			}
		};
		viewer.wantsQuit = [&] {
			session.nextMap = nextMapDue();
			return menuQuit || session.nextMap || session.disconnect || session.levelChange ||
				(session.window && session.window->closed());
		};
		if (session.client) {
			// Our soldier is the server's: the camera follows the control
			// object (in first person at its eye, PlayerCamera::Remote), the
			// keys go up as moves, a click on a spawn point asks for it (with
			// the first kit).
			viewer.playerView = [&session, &playerCamera, &ownFeet](ViewerOptions::PlayerView& view) {
				// A demo's camera following a player (0x005f8f40), with the
				// demo's fov (0x00434610); free, the viewer's own camera.
				// A camera file played (0x005fa4a0 -> 0x005fa210): its free
				// frames as they are, its follow frames as the follow view
				// (attached: that eye with the file's rotation).
				if (session.client && g_demo && g_demo->playingCamera) {
					host::DemoCameraTrack::Pose pose;
					if (g_demo->cameraTrack.update(g_demo->tick, g_demo->fraction, g_demo->camera, pose)) {
						g_demo->cameraPose = pose;
						g_demo->hasCameraPose = true;
					}
					if (g_demo->hasCameraPose) {
						const host::DemoCameraTrack::Pose& at = g_demo->cameraPose;
						float rows[9];
						host::demo_camera::rowsFromQuaternion(at.rotation, rows);
						float yaw = 0, pitch = 0;
						host::demo_camera::yawPitchOfRows(rows, yaw, pitch);
						float eye[3] = {at.position[0], at.position[1], at.position[2]};
						bool placed = !at.follow;
						if (at.follow) {
							const net::RemotePlayer* p = session.client->world().player(g_demo->camera.followed);
							const net::RemoteObject* o =
								p ? session.client->world().object(p->vehicleGhost ? p->vehicleGhost : p->soldierGhost) : nullptr;
							if (o && o->hasPosition) {
								const float target[3] = {o->position.x, o->position.y + 1.6f, o->position.z};
								const host::DemoCamera::Look look =
									g_demo->camera.follow(target, o->hasAngles ? o->yaw : o->rotation.x);
								std::copy(look.eye, look.eye + 3, eye);
								if (!at.attached) {
									yaw = look.yaw;
									pitch = look.pitch;
								}
								placed = true;
							}
						}
						if (placed) {
							const float aspect = view.aspect;
							view = {eye[0], eye[1], eye[2], 0};
							view.aspect = aspect;
							view.external = true;
							std::copy(eye, eye + 3, view.eye);
							view.eyeYaw = yaw;
							view.eyePitch = pitch;
							view.fovY = g_demo->fovY();
							return true;
						}
					}
				}
				if (session.client && g_demo && g_demo->camera.mode == host::DemoCamera::Mode::Follow) {
					const net::RemotePlayer* p = session.client->world().player(g_demo->camera.followed);
					const net::RemoteObject* o =
						p ? session.client->world().object(p->vehicleGhost ? p->vehicleGhost : p->soldierGhost) : nullptr;
					if (o && o->hasPosition) {
						const float at[3] = {o->position.x, o->position.y + 1.6f, o->position.z};
						const host::DemoCamera::Look look = g_demo->camera.follow(at, o->hasAngles ? o->yaw : o->rotation.x);
						const float aspect = view.aspect;
						view = {at[0], o->position.y, at[2], 0};
						view.aspect = aspect;
						view.external = true;
						std::copy(look.eye, look.eye + 3, view.eye);
						view.eyeYaw = look.yaw;
						view.eyePitch = look.pitch;
						view.fovY = g_demo->fovY();
						return true;
					}
				}
				if (!session.client || !session.client->world().hasControlObject()) {
					return false;
				}
				const net::Vec3f p = ownFeet();
				const float aspect = view.aspect;
				view = {p.x, p.y, p.z, 0};
				view.aspect = aspect;
				PlayerCamera::View eye;
				if (playerCamera.view(eye, aspect)) {
					view.firstPerson = true;
					std::copy(eye.position.begin(), eye.position.end(), view.eye);
					view.eyeYaw = eye.yaw;
					view.eyePitch = eye.pitch;
					view.fovY = eye.fovY;
					view.eyeRoll = eye.roll;
				}
				return true;
			};
			viewer.onPlayerInput = [&session, &scoreboardHeld, &options, &playerCamera, &remoteLook, lastAimPitch = 0.0f,
										toggleCameraWas = false, cameraWas = 0](const ViewerOptions::PlayerInput& raw) mutable {
				scoreboardHeld = options.scoreboard || raw.scoreboard;
				if (!session.client) {
					return;
				}
				ViewerOptions::PlayerInput in = raw;
				itemListInput(in); // the HUD's pass first (0x00755d50)
				// The look as we turn it (the server's angles lag behind).
				remoteLook.yaw = std::remainder(remoteLook.yaw + in.turnDegrees, 360.0f);
				remoteLook.pitch = in.aimPitchDegrees;
				remoteLook.fire = in.fire;
				remoteLook.stance = in.lie ? 2 : in.crouch ? 1 : 0;
				if (in.toggleCamera && !toggleCameraWas) {
					playerCamera.toggleMode();
					if (std::getenv("BF2142_INPUT_TRACE")) std::printf("camera-toggle mode=%d first=%d\n", static_cast<int>(playerCamera.mode()), playerCamera.firstPerson);
				}
				toggleCameraWas = in.toggleCamera;
				if (in.camera && in.camera != cameraWas) playerCamera.selectView(in.camera);
				cameraWas = in.camera;
				playerCamera.setAltFire(in.altFire);
				if (session.voice) {
					session.voice->setKeys(in.voipPushToTalk, in.voipLeaderChannel);
				}
				game::SoldierInput input;
				input.throttle = in.throttle;
				input.strafe = in.strafe;
				input.pitchAxis = in.pitch;
				input.rollAxis = in.roll;
				input.sprint = in.sprint;
				input.turn = in.turnDegrees;
				input.aimPitch = in.aimPitchDegrees;
				input.fire = in.fire;
				input.altFire = in.altFire;
				input.flareFire = in.flareFire;
				input.toggleWeapon = in.toggleWeapon;
				input.use = in.use;
				input.toggleFireRate = in.toggleFireRate;
				input.reload = in.reload;
				input.mouseLook = in.mouseLook;
				// The position and weapon keys go to the server as
				// PIPositionSelect1..8 / PIWeaponSelect1..9; a weapon key's
				// press is also our own 0x004d2a50's (the seat's item popup,
				// updateHud).
				input.switchSeat = in.seat;
				input.selectItem = in.item;
				input.itemStep = in.itemStep;
				if (in.item > 0 && !g_remoteItemHeld) {
					g_remoteItemKey = in.item;
					g_footItemKey = in.item;
				}
				g_remoteItemHeld = in.item > 0;
				session.client->setInput(net::playerInputFrom(input, lastAimPitch));
				lastAimPitch = input.aimPitch;
			};
			viewer.wantsPointer = [&session] { return g_deploymentOpen || (session.client && !session.client->world().hasControlObject()); };
			viewer.onClick = [&session, &hudManager, &localKit](float x, float y) {
				const int point = hudManager.mapMarkerAt(x, y);
				if (session.client && point >= 0) {
					session.client->selectKit(localKit);
					session.client->selectSpawnGroup(point);
					if (g_remoteHud) {
						g_remoteHud->chosenSpawn = point; // the spawn screen's selected icon
					}
				} else if (session.client && !session.client->world().hasControlObject()) {
					// The client's own round has no control points (they are
					// the server's ghosts): a click asks for any spawn point.
					session.client->selectKit(localKit);
					session.client->selectSpawnGroup(-1);
				}
			};
		} else if (round.localPlayer() >= 0) {
			// Deployment already owns input before the first soldier exists.
			viewer.onPlayerInput = [&round, &hudPlayer, &hudLocal, &scoreboardHeld, &options, &showMenu, &startedGame, &endOfRound, &joinPending, &commanderScreen,
										&session, &hudManager,
										&commanderKeyWas, &playerCamera, wasAlive = false, spawnedProne = false,
										toggleCameraWas = false, cameraWas = 0, actionWas = false, lastPitch = 0.0f, itemHeld = false](
										const ViewerOptions::PlayerInput& raw) mutable {
				scoreboardHeld = options.scoreboard || raw.scoreboard;
				ViewerOptions::PlayerInput in = raw;
				// Caps Lock opens and closes the commander's screen.
				if (in.commanderScreen && !commanderKeyWas) {
					game::GameWorld::PlayerInfo me;
					commanderScreen = !commanderScreen && hudPlayer(hudLocal(), me) && me.commander;
				}
				commanderKeyWas = in.commanderScreen;
				if (joinPending || (showMenu && startedGame) || endOfRound.loaded()) {
					round.setPlayerInput({}); // the menu has the keys
					return;
				}
				// The local round's commander on the overhead view pans its
				// camera with the movement keys (0x005f8510), which then
				// don't reach the soldier.
				{
					static auto last = std::chrono::steady_clock::now();
					const auto now = std::chrono::steady_clock::now();
					const float seconds = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
					last = now;
					if (commanderScreen && !session.client && g_commanderView && hudManager.commanderMapZoom() == 2) {
						g_overheadCamera.x = g_commanderView->first;
						g_overheadCamera.z = g_commanderView->second;
						g_overheadCamera.pan(in.strafe, in.throttle, seconds);
						g_commanderView = std::make_pair(g_overheadCamera.x, g_overheadCamera.z);
						in.strafe = in.throttle = 0;
					}
				}
				itemListInput(in); // the HUD's pass first (0x00755d50)
				if (in.item > 0 && !itemHeld) {
					g_footItemKey = in.item;
				}
				itemHeld = in.item > 0;
				game::SoldierInput input;
				input.throttle = in.throttle;
				input.strafe = in.strafe;
				input.pitchAxis = in.pitch;
				input.rollAxis = in.roll;
				input.sprint = in.sprint;
				input.turn = in.turnDegrees;
				input.aimPitch = in.aimPitchDegrees;
				input.fire = in.fire;
				input.altFire = in.altFire;
				input.flareFire = in.flareFire;
				input.toggleWeapon = in.toggleWeapon;
				input.use = in.use;
				input.toggleFireRate = in.toggleFireRate;
				input.reload = in.reload;
				input.mouseLook = in.mouseLook;
				input.switchSeat = in.seat;
				input.selectItem = in.item;
				input.itemStep = in.itemStep;
				// C: first person or the chase view; the right button's press
				// steps a zoomed weapon's zoom.
				if (in.toggleCamera && !toggleCameraWas) {
					playerCamera.toggleMode();
					if (std::getenv("BF2142_INPUT_TRACE")) std::printf("camera-toggle mode=%d first=%d\n", static_cast<int>(playerCamera.mode()), playerCamera.firstPerson);
				}
				toggleCameraWas = in.toggleCamera;
				if (in.camera && in.camera != cameraWas) playerCamera.selectView(in.camera);
				cameraWas = in.camera;
				playerCamera.setAltFire(in.altFire);
				playerCamera.setItemStep(in.itemStep);
				// Crouch in a seat with allowDucking ducks (0x005ca2e0), and the
				// seat's inputs are cleared while it does (0x005c9ec0).
				playerCamera.setCrouch(in.crouch);
				if (playerCamera.duckingDisablesInput()) {
					input.throttle = input.strafe = input.turn = input.pitchAxis = input.rollAxis = 0;
					input.fire = input.altFire = input.flareFire = false;
				}
				// Stance: crouch or lie while their keys are held. A spawn where
				// only lying fits (the squad leader's scatter, 0x006c3340) starts
				// prone until a stance key is pressed.
				game::GameWorld::PlayerInfo me;
				const bool alive = round.playerInfo(round.localPlayer(), me) && me.alive;
				if (alive && !wasAlive) {
					spawnedProne = me.spawnedProne;
				}
				wasAlive = alive;
				if (!alive) {
					// Dead: the keys fly the free camera, c_PIAction (Space)
					// swaps it with the death / followed view (sv.allowFreeCam).
					game::camera::FreeInput free;
					free.strafe = in.strafe;
					free.throttle = in.throttle;
					free.turnDegrees = in.turnDegrees;
					free.pitchDegrees = in.aimPitchDegrees - lastPitch;
					free.up = in.lie;
					free.down = in.crouch;
					free.slow = in.sprint;
					playerCamera.setFreeInput(free);
					if (in.jump && !actionWas) {
						playerCamera.spectatorAction();
					}
				}
				actionWas = in.jump;
				lastPitch = in.aimPitchDegrees;
				// Jumping while lying gets up (a crouch press), else jumps.
				if (in.crouch || in.lie || (in.jump && spawnedProne)) {
					spawnedProne = false;
				}
				input.jump = in.jump && !in.lie;
				input.pose = in.lie ? game::SoldierPose::Prone
					: in.crouch     ? game::SoldierPose::Crouch
					: spawnedProne  ? game::SoldierPose::Prone
									: game::SoldierPose::Stand;
				round.setPlayerInput(input);
			};
			// The spawn screen: a pointer, and clicking a spawn icon picks it.
			const auto pointerReason = [&hudPlayer, &hudLocal, &showMenu, &startedGame, &endOfRound, &joinPending, &commanderScreen]() -> const char* {
				game::GameWorld::PlayerInfo me;
				if (joinPending) return "join-game";
				if (showMenu && startedGame) return "esc-menu";
				if (endOfRound.loaded()) return "end-of-round";
				if (commanderScreen) return "commander-screen";
				if (g_deploymentOpen) return "deployment";
				if (hudPlayer(hudLocal(), me) && !me.alive) return "dead-or-no-player";
				return nullptr;
			};
			viewer.wantsPointer = [pointerReason] { return pointerReason() != nullptr; };
			viewer.pointerReason = pointerReason;
			viewer.onClick = [&round, &hudManager, &commanderClick, &mapMenuPoint](float x, float y) {
				std::printf("pointer-click: %.1f,%.1f rose=%d\n", x, y, g_mapMenu);
				std::fflush(stdout);
				if (g_mapMenu != 0) {
					// An open right-click menu takes the click (its rows'
					// buttons), and closes unless a row armed an order there.
					hudManager.click(x, y);
					g_mapMenu = 0;
					mapMenuPoint.reset();
					return;
				}
				if (commanderClick(x, y)) {
					return;
				}
				const int point = hudManager.mapMarkerAt(x, y);
				if (point >= kMarkerVehicle && point - kMarkerVehicle < static_cast<int>(g_spawnVehicles.size())) {
					// A mobile spawn point: spawn in that vehicle.
					g_spawnVehicle = g_spawnVehicles[static_cast<size_t>(point - kMarkerVehicle)];
					round.setSpawnVehicle(round.localPlayer(), g_spawnVehicle);
				} else if (point >= kMarkerTitan && point - kMarkerTitan < static_cast<int>(g_spawnTitans.size())) {
					// Select only the chosen Titan's moving interior SpawnPoints.
					round.setSpawnTitan(round.localPlayer(), g_spawnTitans[static_cast<size_t>(point - kMarkerTitan)]);
				} else if (point == kMarkerLeader || point == kMarkerBeacon) {
					round.setSpawnChoice(round.localPlayer(),
						point == kMarkerLeader ? game::GameWorld::kSpawnOnLeader : game::GameWorld::kSpawnOnBeacon);
				} else if (point >= 0) {
					round.setSpawnChoice(round.localPlayer(), point);
				} else {
					hudManager.click(x, y);
				}
			};
			viewer.onRightClick = [&hudManager, &mapRightClick](float x, float y) {
				if (!mapRightClick(x, y)) {
					hudManager.click(x, y, true); // the buttons' alternate commands
				}
			};
			viewer.onPointerWheel = [&](float x, float y, int steps) {
				float worldX = 0, worldZ = 0;
				if (!commanderScreen || g_mapMenu || !hudManager.mapWorldAt(x, y, worldX, worldZ)) return;
				const int zoom = hudManager.commanderMapZoom();
				const int next = dice::hfe::menu::commanderWheelZoom(zoom, steps);
				if (next != zoom) {
					sendCommanderCam(worldX, worldZ);
					hudManager.setCommanderMapZoom(next);
				}
			};
			viewer.onPointerMove = [&hudManager](float x, float y) {
				g_pointerX = x;
				g_pointerY = y;
				hudManager.setPointer(x, y);
			};
			viewer.takePointerWarp = [](float& x, float& y) {
				x = g_pointerX;
				y = g_pointerY;
				return std::exchange(g_pointerWarp, false);
			};
			viewer.takeAimKick = [&round] { return round.takeAimKick(round.localPlayer()); };
			viewer.playerView = [&round, &playerCamera, &hudPlayer, &hudLocal](ViewerOptions::PlayerView& view) {
				// The soldier and its vehicle as the HUD has them (a joined
				// game's: the server's ghosts as the level's objects).
				const world::Object* player = g_remoteHud ? g_remoteHud->soldier() : round.player();
				const world::Object* const playerVehicle = g_remoteHud ? g_remoteHud->vehicle() : round.playerVehicle();
				const auto external = [&playerCamera](ViewerOptions::PlayerView& view) {
					// The game's own third person: a seat camera's external
					// modes, the death and spectator cameras (Cameras.h).
					PlayerCamera::View placed;
					if (!playerCamera.externalView(placed, view.aspect)) {
						return false;
					}
					view.external = true;
					std::copy(placed.position.begin(), placed.position.end(), view.eye);
					view.eyeYaw = placed.yaw;
					view.eyePitch = placed.pitch;
					view.fovY = placed.fovY;
					view.eyeRoll = placed.roll;
					return true;
				};
				if (!player) {
					// Dead: the death camera (or the free / followed one).
					const float aspect = view.aspect;
					view = {};
					view.aspect = aspect;
					if (!external(view)) {
						return false;
					}
					view.x = view.eye[0];
					view.y = view.eye[1];
					view.z = view.eye[2];
					return true;
				}
				const float aspect = view.aspect;
				// The camera turns with the look (the head turns before the body, 0x0058a6d0).
				game::GameWorld::PlayerInfo look;
				const float lookYaw = hudPlayer(hudLocal(), look) ? look.yaw : player->rotation().x;
				view = {player->position().x, player->position().y, player->position().z, lookYaw};
				if (const world::Object* vehicle = playerVehicle) {
					// A gunner looks along the gun.
					game::GameWorld::PlayerInfo me;
					float gunYaw = 0, gunPitch = 0;
					hudPlayer(hudLocal(), me);
					round.gunAim(vehicle->id(), me.seat, gunYaw, gunPitch);
					view = {vehicle->position().x, vehicle->position().y, vehicle->position().z, vehicle->rotation().x + gunYaw};
					view.driving = true;
					// The seat camera's worldFOV for the chase view (0x00610150).
					view.chaseFovY = playerCamera.seatFov(false);
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
				// First person: the soldier camera at the eye, or the seat's.
				PlayerCamera::View eye;
				view.aspect = aspect;
				if (playerCamera.view(eye, aspect)) {
					view.firstPerson = true;
					game::GameWorld::ProjectileCamera missile;
					if (!g_remoteHud && round.projectileCamera(round.localPlayer(), missile)) view.projectileSerial = missile.serial;
					std::copy(eye.position.begin(), eye.position.end(), view.eye);
					view.eyeYaw = eye.yaw;
					view.eyePitch = eye.pitch;
					view.fovY = eye.fovY;
					view.eyeRoll = eye.roll;
				} else {
					external(view);
				}
				return true;
			};
			if (!options.screenshot.empty() && options.camera.empty() && !playerCamera.firstPerson && !session.client) {
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
		// The HUD's text boxes: the say keys open the chat's (0x00755020: not
		// over a menu), and while a box is open the keys type into it and the
		// soldier gets no input.
		viewer.wantsText = [&chat, &squadNameBox] { return chat.typing() != Box::kNone || squadNameBox.typing(); };
		viewer.onHudText = [&chat, &squadNameBox](const ViewerOptions::MenuInput& in) {
			const bool text = in.kind == ViewerOptions::MenuInput::Kind::Text;
			if (squadNameBox.typing()) {
				text ? squadNameBox.type(in.text) : squadNameBox.key(in.key);
			} else {
				text ? chat.type(in.text) : chat.key(in.key);
			}
		};
		if (viewer.onPlayerInput) {
			// The comm roses while their keys are held (Q the radio, T the
			// tactical rose), with the pointer to pick an entry.
			viewer.onPlayerInput = [inner = viewer.onPlayerInput, &commRoses, &round, &hudPlayer, &hudLocal, &helpPopups, &hudManager, &playerCamera,
										&questions, radioWas = false, tacticalWas = false, mapSizeWas = false, mapZoomWas = false,
										map3dWas = false, yesWas = false, noWas = false, deploymentWas = false](const ViewerOptions::PlayerInput& in) mutable {
				g_ctrlHeld = in.ctrl;
				g_shiftHeld = in.shift;
				game::GameWorld::PlayerInfo me;
				const bool playerPresent = hudPlayer(hudLocal(), me);
				const bool alive = playerPresent && me.alive;
				const bool deploymentPressed = in.deployment && !deploymentWas;
				if (deploymentPressed && alive) {
					g_deploymentOpen = !g_deploymentOpen;
					hudManager.set("HudDone", 0);
					g_bigMap = false;
				} else if (deploymentPressed && playerPresent && !g_initialDeploymentPending) {
					// 0x00754d74: the same toggle (0x0074fd90) when dead; it is DONE, not Enter, that spawns.
					g_spawnScreenHidden = !g_spawnScreenHidden;
				}
				if (alive) g_spawnScreenHidden = false;
				deploymentWas = in.deployment;
				const bool done = hudManager.number("HudDone") != 0 || (deploymentPressed && !alive && g_initialDeploymentPending);
				const auto doneAction = dice::main::deploymentDoneAction(done, playerPresent, alive, g_initialDeploymentPending, me.respawnIn);
				if (done) {
					if (doneAction == dice::main::DeploymentDoneAction::InitialSpawn
						|| doneAction == dice::main::DeploymentDoneAction::Respawn) {
						if (round.spawnSoldier(round.localPlayer())) {
							g_initialDeploymentPending = false;
							g_deploymentOpen = false;
							// Spawned between ticks: the camera takes the soldier now, or this
							// frame draws him from outside (the next tick would flip it).
							playerCamera.tick(0);
						}
					} else if (alive) {
						g_deploymentOpen = false;
					}
					hudManager.set("HudDone", 0);
				}
				if (!alive) g_deploymentOpen = false; // death already opens deployment
				if (alive && ((in.radioComm && !radioWas) || (in.tacticalComm && !tacticalWas))) {
					// A rose opens with the pointer on its middle entry (HudManager.setCommMousePos 400 300, the
					// centre of RadioMiddle: Spotted, request/issue order), so a tap of Q or T picks that entry.
					const std::array<float, 4> at = hudManager.setting("HudManager.setCommMousePos");
					g_pointerX = at[0] > 0 ? at[0] : 400.0f;
					g_pointerY = at[1] > 0 ? at[1] : 300.0f;
					hudManager.setPointer(g_pointerX, g_pointerY);
					g_pointerWarp = true;
				}
				if (alive && in.radioComm && !radioWas) {
					// The vehicle's rose with the controlled object's
					// useVehicleCommRose (0x0075bf68), else the soldier's.
					commRoses.openRadio(me.vehicle != 0
						&& dice::hfe::menu::usesVehicleCommRose(controlledTemplate(round, me)));
					// 0x0075be30 opens the radio rose (menu state 5 / 6) and
					// lets 0x35 specificSpotted through.
					helpPopups.enableTutorial(0x35);
				} else if (alive && in.tacticalComm && !tacticalWas) {
					commRoses.openTactical(me.commander, me.squadLeader, me.squad != 0);
					g_allSquads = me.commander || me.squad != 0;
				} else if ((!in.radioComm && radioWas) || (!in.tacticalComm && tacticalWas) || !alive) {
					// Releasing the rose key chooses the entry under the pointer, as a click would (the user's mouse clicks
					// do not reach the rose); a tap leaves it on the middle entry.
					if (alive && !commRoses.shown().empty() && !in.radioComm && !in.tacticalComm) {
						std::printf("comm-rose: release selects at %.1f,%.1f\n", g_pointerX, g_pointerY);
						std::fflush(stdout);
						hudManager.click(g_pointerX, g_pointerY);
					}
					if (!commRoses.shown().empty() && !in.radioComm && !in.tacticalComm) {
						commRoses.close();
					}
				}
				radioWas = in.radioComm;
				tacticalWas = in.tacticalComm;
				g_allSquads = g_allSquads && in.tacticalComm && alive;
				// The map keys (0x00754290): c_GIMapSize toggles the big map
				// (menu state 2) while alive; c_GIMapZoom steps the minimap's
				// zoom 0, 1, 2 (0x0079a040 / 0x00799fb0) outside the spawn
				// screen (state 1), with event 2/9 and tutorial 0x3d off;
				// c_GI3dMap flips the 3D name tags and icons (the
				// NametagManager's slots 0x64 / 0x68).
				if (in.mapSize && !mapSizeWas && alive) {
					g_bigMap = !g_bigMap;
				}
				if (!alive) {
					g_bigMap = false;
				}
				if (in.mapZoom && !mapZoomWas && alive) {
					hudManager.cycleMapZoom();
					helpPopups.disableTutorial(0x3d);
				}
				if (in.map3d && !map3dWas && g_nametags) {
					g_nametags->setShown(!g_nametags->shown());
				}
				// c_GIYes / c_GINo answer the HUD's front question (0x0079e320).
				if (in.yes && !yesWas) {
					questions.answer(true);
				}
				if (in.no && !noWas) {
					questions.answer(false);
				}
				yesWas = in.yes;
				noWas = in.no;
				mapSizeWas = in.mapSize;
				mapZoomWas = in.mapZoom;
				map3dWas = in.map3d;
				if (g_deploymentOpen) {
					ViewerOptions::PlayerInput idle;
					idle.aimPitchDegrees = in.aimPitchDegrees;
					inner(idle);
				} else {
					inner(in);
				}
			};
			viewer.wantsPointer = [inner = viewer.wantsPointer, &commRoses] {
				return !commRoses.shown().empty() || (inner && inner());
			};
			viewer.pointerReason = [inner = viewer.pointerReason, &commRoses]() -> const char* {
				return !commRoses.shown().empty() ? "comm-rose" : inner ? inner() : nullptr;
			};
			viewer.onPlayerInput = [inner = viewer.onPlayerInput, &chat, &squadNameBox, &round, &session,
										keysWere = std::array<bool, 3> {}](const ViewerOptions::PlayerInput& in) mutable {
				const std::array<bool, 3> keys {in.sayAll, in.sayTeam, in.saySquad};
				for (int i = 0; i < 3; ++i) {
					if (keys[static_cast<size_t>(i)] && !keysWere[static_cast<size_t>(i)] && chat.typing() == Box::kNone) {
						// The squad's box needs a squad (player slot 0x120).
						game::GameWorld::PlayerInfo me;
						const bool squad = session.client ? (session.client->world().player(session.client->world().localPlayer())
																   && session.client->world().player(session.client->world().localPlayer())->squad)
														  : round.playerInfo(round.localPlayer(), me) && me.squad;
						if (i < 2 || squad) {
							chat.open(i == 0 ? Box::kAll : i == 1 ? Box::kTeam : Box::kSquad);
						}
					}
				}
				keysWere = keys;
				if (chat.typing() != Box::kNone || squadNameBox.typing()) {
					ViewerOptions::PlayerInput idle;
					idle.aimPitchDegrees = in.aimPitchDegrees;
					inner(idle);
					return;
				}
				inner(in);
			};
		}
		std::array<int, 3> remoteTicketState {-1, -1, -1}; // a joined game's last seen team +0x8c
		viewer.onTicks = [&](int ticks) {
			if (joinPending) {
				return; // the loaded level waits on JOIN GAME: nothing runs yet
			}
			for (int i = 0; i < ticks; ++i) {
				step(); // its sounds too (stepSounds)
				if (gameLogic.ended() || (session.client && session.client->world().roundEnded())) {
					sinceEnd += 1.0f / 30.0f;
					showEndOfRound();
				}
			}
			if (session.client && g_remoteHud) {
				// The server's ticket state, sent with the tickets, announced
				// as it changes (0x0044b7e8 -> 0x00449070); not the first seen.
				game::GameWorld::PlayerInfo me;
				const int local = g_remoteHud->localPlayer();
				for (int team = 1; team <= 2; ++team) {
					const int state = session.client->world().teamState().ticketState(team);
					const int was = std::exchange(remoteTicketState[static_cast<size_t>(team)], state);
					if (state < 0 || was < 0 || state == was || !g_remoteHud->playerInfo(local, me)) {
						continue;
					}
					const std::string message = GameAudio::ticketStateMessage(team, state, me.team, round.gameMode());
					if (!message.empty()) {
						round.hearRadio(local, 13, message);
					}
				}
			}
		};
		if (options.commanderScreen && round.localPlayer() >= 0) {
			// Command the player's team (the game mode enables it) with its
			// screen open.
			round.setCommanderEnabled(true);
			commanderScreen = round.setCommander(round.playerTeam() == 1 ? 1 : 2, round.localPlayer());
		}
		if (options.spawnScreen && session.client && session.client->world().hasControlObject()) {
			// A joined game: ask the server to die (6/0xc) and wait for it
			// (the player's networkable turns dead), as long as two seconds.
			session.client->suicide();
			for (int i = 0; i < 60 && session.client; ++i) {
				step();
				const net::RemotePlayer* me = session.client ? session.client->world().player(session.client->world().localPlayer()) : nullptr;
				if (me && !me->alive && i >= 30) {
					break;
				}
			}
		}
		if (options.spawnScreen && round.player()) {
			// Die (and pick the first point to spawn at) to show the spawn screen.
			const std::vector<int> points = round.spawnablePoints(round.playerTeam());
			round.setSpawnChoice(round.localPlayer(), points.empty() ? -1 : points.front());
			round.setHitPoints(round.playerObject(), 0);
			// A second dead: the death camera (0x00615970) out from the body,
			// the view behind the spawn screen.
			for (int i = 0; i < 30; ++i) {
				step();
			}
		}
		for (const std::string& click : options.hudClicks) {
			// The HUD as drawn first (the map's view the clicks land on).
			if (sources.overlay) {
				sources.overlay(0);
			}
			float x = 0, y = 0;
			char button[16] = "left";
			std::sscanf(click.c_str(), "%f/%f/%15s", &x, &y, button);
			hudManager.setPointer(x, y);
			if (std::strcmp(button, "right") == 0 && viewer.onRightClick) {
				viewer.onRightClick(x, y);
			} else if (std::strcmp(button, "left") == 0 && viewer.onClick) {
				viewer.onClick(x, y);
			}
		}
		if (!options.hudClicks.empty() && sources.overlay) {
			sources.overlay(1); // a second for the menus' fades to finish
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
