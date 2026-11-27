// Demo recording in a local round (main.cpp's local recorder: a HostedGame
// never start()ed, as retail's singleplayer records through its local
// gameServer), the auto-recorder's camera file, the demo connection's
// update rates with 64 players, and --demo's file names.
#include "TestMain.h"
#include "TitanLevel.h"

#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Main/HostedGame.h"
#include "Net/Demo.h"
#include "Net/GameClient.h"
#include "Net/ServerSettings.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace net = dice::hfe::net;
namespace host = dice::hfe::host;
namespace game = dice::hfe::game;

namespace {

host::HostSetup localSetup(const net::ServerSettings* settings, const std::string& modDirectory = "mods/bf2142")
{
	host::HostSetup setup;
	setup.serverName = "Local Demo";
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_sp";
	setup.mapSize = 16;
	setup.settings = settings;
	setup.modDirectory = modDirectory;
	return setup;
}

// A demo played back headless to its end: the viewer's world.
struct Played {
	net::GameClient viewer;
	int ticks = 0;
	bool finished = false;
};

void playBack(const std::vector<net::DemoRecord>& records, Played& out)
{
	net::DemoPlayer player(records);
	out.viewer.playDemo([&player](std::vector<uint8_t>& payload) { return player.next(payload); });
	while (out.viewer.update() && out.ticks < 3000) {
		player.advance(1);
		++out.ticks;
	}
	out.finished = player.finished();
}

} // namespace

// A local round's recorder: never started, so no socket and no clients but
// the demo's; primed mid-round, its demo has the round's players (the
// local one with the bots) from the join's database on, and plays back.
TEST(Demo_LocalRecorderNeverStarted)
{
	const std::string path = "bf2142_test_local_demo.bf2142demo";
	std::remove(path.c_str());
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	game::GameWorld round(templates, objects);
	game::GameLogic logic;
	round.startRound();
	logic.startRound();
	const int me = round.joinPlayer(1);
	round.setPlayerName(me, "LocalHero");
	for (int i = 0; i < 3; ++i) {
		round.addPlayer("Bot" + std::to_string(i), 1 + i % 2, true);
	}
	for (int i = 0; i < 30; ++i) {
		round.tick(1.0f / 30);
	}
	net::ServerSettings sv;
	host::HostedGame recorder(round, logic, templates, localSetup(&sv));
	recorder.prime();
	CHECK(recorder.gamePort() == 0); // no socket
	CHECK(recorder.recordDemo(path));
	CHECK(recorder.clientCount() == 1); // the demo's connection only
	for (int i = 0; i < 90; ++i) {
		recorder.tick();
		round.tick(1.0f / 30);
	}
	CHECK(round.playerList().size() == 4); // the recorder adds no player
	recorder.stopDemo();
	CHECK(recorder.idle());

	net::DemoHeader header;
	std::vector<net::DemoRecord> records;
	CHECK(net::readDemo(path, header, records));
	CHECK(header.serverName == "Local Demo" && header.level == "Camp_Gibraltar" && header.gameMode == "gpm_sp");
	CHECK(records.size() > 10);
	Played played;
	playBack(records, played);
	CHECK(played.finished && played.ticks >= 89 && played.ticks < 150);
	CHECK(played.viewer.world().hasLevel() && played.viewer.world().roundStarted());
	int seen = 0;
	bool sawLocal = false;
	for (const net::RemotePlayer& p : played.viewer.world().playerList()) {
		++seen;
		sawLocal = sawLocal || p.name == "LocalHero";
	}
	CHECK(seen == 4 && sawLocal);
	CHECK(played.viewer.world().localPlayer() < 0);
	std::remove(path.c_str());
}

// The recorder leaves a local round a local round: what the local player
// hears (the radio), the squads' messages to it and its players are the
// round's, as with no recorder; the HUD reads them from the round.
TEST(Demo_LocalRecorderLeavesTheRoundLocal)
{
	const std::string path = "bf2142_test_local_demo2.bf2142demo";
	std::remove(path.c_str());
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	game::GameWorld round(templates, objects);
	game::GameLogic logic;
	round.startRound();
	logic.startRound();
	const int me = round.joinPlayer(1);
	const int bot = round.addPlayer("Bot", 1, true);
	net::ServerSettings sv;
	host::HostedGame recorder(round, logic, templates, localSetup(&sv));
	recorder.prime();
	CHECK(recorder.recordDemo(path));
	bool localChat = false, spotted = false;
	recorder.onLocalChat = [&localChat](const net::ChatMessage&) { localChat = true; };
	recorder.onSpotted = [&spotted](const net::SpottedInfo&) { spotted = true; };
	round.hearRadio(me, 1, "HUD_TEXT_RADIO_AFFIRMATIVE", bot);
	round.hearRadio(bot, 1, "HUD_TEXT_RADIO_NEGATIVE", me);
	recorder.tick();
	round.tick(1.0f / 30);
	recorder.tick();
	const auto heard = round.takeRadioHeard([me](int receiver) { return receiver == me; });
	CHECK(heard.size() == 1 && heard[0].voice == "HUD_TEXT_RADIO_AFFIRMATIVE");
	const auto botHeard = round.takeRadioHeard([bot](int receiver) { return receiver == bot; });
	CHECK(botHeard.size() == 1); // the bots' too (Bots.cpp takes them)
	CHECK(!localChat && !spotted);
	const auto clients = recorder.clientPlayers();
	CHECK(clients.size() == 1 && clients[0].player < 0 && round.localPlayer() == me); // the demo's connection, no player
	recorder.stopDemo();
	std::remove(path.c_str());
}

// sv.autoRecord (0x00585860 -> demo.recordDemo's 0x00432cc0): with a local
// player the auto demo has its camera file, <file>-def.bf2142cam; the
// frames go into it as they come (0x005f8d30), so it reads before the
// recording ends, and whole after.
TEST(Demo_AutoDemoWritesItsCameraAsItGoes)
{
	const std::string root = "bf2142_test_autocam";
	std::filesystem::remove_all(root);
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	game::GameWorld round(templates, objects);
	game::GameLogic logic;
	round.startRound();
	logic.startRound();
	round.joinPlayer(1);
	net::ServerSettings sv;
	sv.autoRecord = true;
	std::string demo, camera;
	{
		host::HostedGame recorder(round, logic, templates, localSetup(&sv, root));
		CHECK(!recorder.idle()); // waiting for the round to play
		float x = 0;
		// The local player joins a few ticks in (main.cpp's first step comes
		// before it): the auto demo's camera starts then.
		recorder.localCamera = [&x](net::CameraKeyframe& frame) {
			frame = {};
			frame.position[0] = x;
			frame.position[1] = 50;
			return x > 5;
		};
		for (int i = 0; i < 70; ++i) {
			x += 1;
			recorder.tick();
			round.tick(1.0f / 30);
		}
		CHECK(recorder.recordingDemo() && recorder.recordingCamera());
		demo = recorder.autoDemo();
		camera = recorder.cameraPath();
		CHECK(camera == demo + "-def.bf2142cam");
		std::vector<net::CameraKeyframe> partial;
		CHECK(net::readCameraFile(camera, partial)); // flushed each second
		CHECK(partial.size() >= 25 && partial.size() <= 66);
		recorder.stopDemo();
	}
	std::vector<net::CameraKeyframe> frames;
	CHECK(net::readCameraFile(camera, frames));
	CHECK(frames.size() >= 60 && frames.size() <= 66);
	CHECK(!frames.empty() && frames.back().position[1] > 40 && frames.back().position[0] > frames.front().position[0]);
	net::DemoHeader header;
	std::vector<net::DemoRecord> records;
	CHECK(net::readDemo(demo, header, records) && !records.empty());
	std::filesystem::remove_all(root);
}

// --demo's file (net::demoFileToPlay): a file that is there as given, else
// demo.recordDemo's place for the name, so "match1" plays
// Demos/match1.bf2142demo, all from the working folder.
TEST(Demo_PlaybackFileNames)
{
	CHECK(net::demoFileToPlay("bf2142_no_such_demo") == "Demos/bf2142_no_such_demo.bf2142demo");
	CHECK(net::demoFileToPlay("a\\b") == "Demos/a/b.bf2142demo");
	CHECK(net::demoFileToPlay("other/x.bf2142demo") == "other/x.bf2142demo");
	const std::string here = "bf2142_test_demo_here";
	{
		std::ofstream f(here);
		f << "x";
	}
	CHECK(net::demoFileToPlay(here) == here); // there as given: that one
	std::remove(here.c_str());
	CHECK(net::demoFileToPlay(here) == "Demos/" + here + ".bf2142demo");
	CHECK(host::demoCameraPath("match1") == "Demos/match1-def.bf2142cam");
}

// A 64-player round (Suez_Canal gpm_cq 64, every soldier on the move): with
// enhanced.demoQuality each soldier is in the demo every tick, as a live
// client would see it; retail's sv.demoQuality 1 (4 packets a second of 40
// updates) starves them.
TEST(Demo_SixtyFourPlayersUpdateRate)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	titantest::TitanLevel level(gameDir, "Suez_Canal", 64, true, nullptr, false, nullptr, "gpm_cq");
	CHECK(level.loaded);
	if (!level.loaded) {
		return;
	}
	game::GameWorld& round = *level.world;
	game::GameLogic logic;
	logic.startRound();
	std::vector<int> players;
	for (int i = 0; i < 64; ++i) {
		const int id = round.addPlayer("P" + std::to_string(i), 1 + i % 2, true);
		players.push_back(id);
		round.spawnSoldier(id);
	}
	level.run(1);
	int alive = 0;
	for (const int id : players) {
		game::GameWorld::PlayerInfo info;
		alive += round.playerInfo(id, info) && info.alive;
	}
	CHECK(alive >= 60);
	const auto move = [&](int tick) {
		for (size_t i = 0; i < players.size(); ++i) {
			game::SoldierInput input;
			input.throttle = (tick / 45 + i) % 3 == 0 ? -1.0f : 1.0f;
			input.strafe = i % 2 ? 0.5f : -0.5f;
			input.turn = 2.0f;
			round.setInput(players[i], input);
		}
	};
	net::ServerSettings sv; // sv.demoQuality 1
	const bool on = true;
	const std::string enhancedPath = "bf2142_test_demo64_enhanced.bf2142demo", retailPath = "bf2142_test_demo64_q1.bf2142demo";
	std::remove(enhancedPath.c_str());
	std::remove(retailPath.c_str());
	host::HostedGame enhanced(round, logic, level.templates, localSetup(&sv));
	enhanced.setEnhancedDemo(&on, &on);
	host::HostedGame retail(round, logic, level.templates, localSetup(&sv));
	for (host::HostedGame* r : {&enhanced, &retail}) {
		r->prime();
	}
	CHECK(enhanced.recordDemo(enhancedPath) && retail.recordDemo(retailPath));
	for (int tick = 0; tick < 150; ++tick) {
		move(tick);
		enhanced.tick();
		retail.tick();
		round.tick(1.0f / 30);
	}
	enhanced.stopDemo();
	retail.stopDemo();
	net::DemoHeader header;
	std::vector<net::DemoRecord> records;
	CHECK(net::readDemo(enhancedPath, header, records));
	const net::DemoStats fast = net::demoStats(records);
	std::printf("  enhanced: %s\n", net::formatDemoStats(fast, static_cast<size_t>(std::filesystem::file_size(enhancedPath))).c_str());
	CHECK(fast.soldierGhosts >= 60);
	CHECK(fast.soldierMedian <= 1 && fast.soldierP95 <= 2 && fast.soldierOver3 < 0.01);
	CHECK(net::readDemo(retailPath, header, records));
	const net::DemoStats slow = net::demoStats(records);
	std::printf("  sv.demoQuality 1: %s\n", net::formatDemoStats(slow, static_cast<size_t>(std::filesystem::file_size(retailPath))).c_str());
	CHECK(fast.soldierUpdateRate() >= 25);
	CHECK(slow.soldierUpdateRate() < 2); // 160 updates a second for 64 soldiers and the rest
	// Played back, every soldier is there.
	CHECK(net::readDemo(enhancedPath, header, records));
	Played played;
	playBack(records, played);
	CHECK(played.finished && played.viewer.world().playerList().size() == 64);
	std::remove(enhancedPath.c_str());
	std::remove(retailPath.c_str());
}
