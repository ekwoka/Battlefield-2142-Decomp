// The server side (GameServer, the QR2 query responder and LAN search,
// HostedGame) against the rewrite's own client, over UDP on 127.0.0.1.
#include "TestMain.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Main/HostedGame.h"
#include "Main/Prediction.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
#include "Net/GameClient.h"
#include "Net/GameServer.h"
#include "Net/ServerQuery.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/TemplateFiles.h"
#include "World/WorldConsole.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <thread>

namespace net = dice::hfe::net;
namespace host = dice::hfe::host;

namespace {

bool pumpUntil(const std::function<void()>& step, const std::function<bool()>& done, int milliseconds = 4000)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	while (std::chrono::steady_clock::now() < deadline) {
		step();
		if (done()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return false;
}

} // namespace

TEST(NetHost_ServerHandshakeEventsAndMoves)
{
	net::GameServer server;
	net::GameServerOptions options;
	options.port = 0;
	CHECK(server.start(options));
	int joined = -1;
	std::vector<net::GameEvent> events;
	std::vector<net::PlayerMove> moves;
	server.onJoined = [&](int client) { joined = client; };
	server.onEvent = [&](int, const net::GameEvent& e) { events.push_back(e); };
	server.onMove = [&](int, const net::PlayerMove& m) { moves.push_back(m); };

	net::GameClient client;
	net::GameClientOptions clientOptions;
	clientOptions.playerName = "Joiner";
	CHECK(client.connect("127.0.0.1", server.port(), clientOptions));
	const auto step = [&] {
		client.update();
		server.poll();
		server.sendTick([&](int) {
			net::GhostBlock block;
			block.present = true;
			block.tick = 77;
			net::ControlObjectRecord control;
			control.ghost = 9;
			control.position = {1, 2, 3};
			block.control = control;
			return block;
		});
	};
	// The challenge answered and the connection type given: joined.
	CHECK(pumpUntil(step, [&] { return joined > 0; }));
	CHECK(server.client(joined) && server.client(joined)->cdKeyHash.size() == 32);
	// Our player: the client's name goes up, its moves follow.
	net::CreatePlayerInfo me;
	me.id = 4;
	me.team = 2;
	me.name = "Player1";
	server.send(joined, net::makeCreatePlayer(me));
	net::PlayerInput input;
	input.set(net::kPIThrottle, 1);
	client.setInput(input);
	CHECK(pumpUntil(step, [&] { return !events.empty() && moves.size() >= 3; }));
	CHECK(client.world().localPlayer() == 4);
	CHECK(!events.empty() && events[0].type == net::kEvChangePlayerName && events[0].i(0) == 4
		&& events[0].text(0).substr(0, 6) == "Joiner");
	bool ordered = true;
	for (size_t i = 1; i < moves.size(); ++i) {
		ordered = ordered && moves[i].index == moves[i - 1].index + 1;
	}
	CHECK(ordered && moves.back().axis(net::kPIThrottle) == 1);
	// The ghost block reaches the client.
	CHECK(pumpUntil(step, [&] { return client.world().hasControlObject(); }));
	CHECK(client.world().serverTick() == 77 && client.world().controlObject().position.z == 3);
	// Kicked.
	server.kick(joined, 3);
	CHECK(pumpUntil(step, [&] { return client.state() != net::GameClient::State::Connected; }));
	CHECK(server.clients().empty());
}

TEST(NetHost_ServerRefusals)
{
	net::GameServer server;
	net::GameServerOptions options;
	options.port = 0;
	options.password = "secret";
	CHECK(server.start(options));
	net::GameClient client;
	CHECK(client.connect("127.0.0.1", server.port()));
	CHECK(pumpUntil([&] { client.update(); server.poll(); }, [&] { return client.state() == net::GameClient::State::Refused; }));
	CHECK(client.connectResult().reason == 0x11);
}

TEST(NetHost_QueryAndLanSearch)
{
	net::ServerReport report;
	report.keys = {{"hostname", "Loopback"}, {"mapname", "Camp_Gibraltar"}, {"gametype", "gpm_cq"}, {"numplayers", "3"},
		{"maxplayers", "32"}, {"hostport", "17567"}, {"gamevariant", "bf2142"}, {"password", "0"}, {"gamever", "1.10.112.0"}};
	report.players = {{"Tester", 10, 50, 1, 2, 0, 7}, {"Bot1", 3, 0, 2, 5, 0, 0}};
	// The echo of the LAN search: type 5, the key, the data.
	const std::vector<uint8_t> echo = net::qr2Answer(net::qr2LanProbe(), report);
	CHECK(echo.size() == 6 && echo[0] == 5);
	// A query with player keys, read back.
	const std::vector<uint8_t> keys {1, 5, 8}, playerKeys {21, 22, 25};
	const std::vector<uint8_t> answer = net::qr2Answer(net::qr2Query(0x01020304, keys, playerKeys), report);
	net::ServerReport back;
	CHECK(net::parseQr2Reply(answer, 0x01020304, keys, playerKeys, back));
	CHECK(back.keys["hostname"] == "Loopback" && back.keys["mapname"] == "Camp_Gibraltar" && back.keys["numplayers"] == "3");
	CHECK(back.players.size() == 2 && back.players[0].name == "Tester" && back.players[0].score == 10
		&& back.players[1].team == 2);
	CHECK(!net::parseQr2Reply(answer, 0x05060708, keys, playerKeys, back));

	// The LAN search finds a responder on this machine.
	net::QueryServer responder;
	CHECK(responder.start(0));
	std::thread serving([&] {
		const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
		while (std::chrono::steady_clock::now() < until) {
			responder.poll([&] { return report; });
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	});
	const auto found = net::browseLan("127.0.0.1", responder.port(), responder.port(), 300);
	serving.join();
	CHECK(found.size() == 1);
	if (found.size() == 1) {
		CHECK(found[0].ip == "127.0.0.1" && found[0].port == responder.port());
		CHECK(found[0].fields.at("hostname") == "Loopback" && found[0].fields.at("hostport") == "17567");
	}
}

TEST(NetHost_QuaternionRoundTrip)
{
	for (const auto& [yaw, pitch, roll] : std::vector<std::tuple<float, float, float>> {
			 {0, 0, 0}, {90, 0, 0}, {-135, 20, 5}, {30, -40, 170}}) {
		float y = 0, p = 0, r = 0;
		host::yawPitchRollFromQuaternion(host::quaternionFromYawPitchRoll(yaw, pitch, roll), y, p, r);
		CHECK(std::fabs(std::remainder(y - yaw, 360.0f)) < 0.05f && std::fabs(p - pitch) < 0.05f
			&& std::fabs(std::remainder(r - roll, 360.0f)) < 0.05f);
	}
}

// A hosted round of nothing but its players: the join sequence (blocks 0,
// 5, 2; LoadComplete; the players and DatabaseComplete; the echo;
// StartSimulation and the game state), the client's name and kit.
TEST(NetHost_HostedGameJoin)
{
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::game::GameWorld round(templates, objects);
	dice::hfe::game::GameLogic logic;
	round.startRound();
	logic.startRound();
	host::HostSetup setup;
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_cq";
	setup.mapSize = 32;
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(round, logic, templates, setup);
	CHECK(hosted.start());
	round.addPlayer("Bot1", 2, true);

	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Joiner";
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
	const auto step = [&] {
		client.update();
		hosted.tick();
		round.tick(1.0f / 30);
	};
	CHECK(pumpUntil(step, [&] { return client.world().hasLevel(); }));
	CHECK(client.world().level().mapName == "Camp_Gibraltar" && client.world().level().gameMode == "gpm_cq");
	CHECK(client.world().serverName() == setup.serverName && client.world().mapList().size() == 1);
	// The client answers the level with LoadComplete (GameClient's autoAnswer).
	CHECK(pumpUntil(step, [&] { return client.world().gameState() == 1 && client.world().roundStarted(); }));
	CHECK(client.world().localPlayer() >= 0 && client.world().playerList().size() == 2);
	const net::RemotePlayer* bot = nullptr;
	for (const net::RemotePlayer& p : client.world().playerList()) {
		bot = p.name == "Bot1" ? client.world().player(p.id) : bot;
	}
	CHECK(bot && bot->team == 2 && !bot->local);
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* me = client.world().player(client.world().localPlayer());
		return me && me->name == "Joiner";
	}));
	dice::hfe::game::GameWorld::PlayerInfo me;
	CHECK(round.playerInfo(client.world().localPlayer(), me) && me.team == 1 && !me.ai);
	// A chat line goes round to every client, from our player.
	std::vector<std::string> log;
	hosted.onLog = [&log](const std::string& line) { log.push_back(line); };
	client.say("hello all");
	std::vector<net::ChatLine> heard;
	CHECK(pumpUntil(step, [&] {
		for (const net::ChatLine& line : client.world().takeChat()) {
			heard.push_back(line);
		}
		return !heard.empty();
	}));
	CHECK(!heard.empty() && heard[0].text == "hello all" && heard[0].player == client.world().localPlayer());
	CHECK(hosted.clientCount() == 1);
	const int joinerId = client.world().localPlayer();
	client.disconnect();
	CHECK(pumpUntil(step, [&] { return hosted.clientCount() == 0; }));
	// The player went with its client (GameWorld::removePlayer): out of the
	// round's list, its id free; the next client takes it.
	dice::hfe::game::GameWorld::PlayerInfo gone;
	CHECK(!round.playerInfo(joinerId, gone) && round.playerList().size() == 1);
	net::GameClient second;
	options.playerName = "Second";
	CHECK(second.connect("127.0.0.1", hosted.gamePort(), options));
	const auto stepSecond = [&] {
		second.update();
		hosted.tick();
		round.tick(1.0f / 30);
	};
	CHECK(pumpUntil(stepSecond, [&] {
		const net::RemotePlayer* me = second.world().player(second.world().localPlayer());
		return me && me->name == "Second";
	}));
	CHECK(second.world().localPlayer() == joinerId && round.playerList().size() == 2);
	second.disconnect();
	CHECK(pumpUntil(stepSecond, [&] { return hosted.clientCount() == 0; }));
}

namespace {

// A flat range with a spawn point per team and two soldier templates
// that sprint (as GameWorldTest's): enough for soldiers to walk on.
struct HostRange {
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::io::FileManager files;
	dice::hfe::physics::CollisionTemplateManager collisionTemplates;
	std::unique_ptr<dice::hfe::game::GameWorld> round;
	dice::hfe::game::GameLogic logic;

	HostRange()
	{
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			dice::hfe::world::ObjectTemplate* soldier = templates.create("Soldier", name);
			soldier->settings()["armor.maxHitPoints"] = "100";
			soldier->settings()["sprintDissipationTime"] = "6";
			soldier->settings()["sprintRecoverTime"] = "10";
			soldier->settings()["sprintLimit"] = "0.2";
			soldier->settings()["sprintLossAtJump"] = "0.1";
		}
		for (int team = 1; team <= 2; ++team) {
			dice::hfe::world::ObjectTemplate* point = templates.create("ControlPoint", team == 1 ? "cp_pac" : "cp_eu");
			point->settings()["controlPointId"] = std::to_string(team);
			point->settings()["team"] = std::to_string(team);
			point->settings()["unableToChangeTeam"] = "1";
			objects.create(point, {team == 1 ? 100.0f : -100.0f, 0, 0});
		}
		dice::hfe::world::ObjectTemplate* spawn = templates.create("SpawnPoint", "sp");
		objects.create(spawn, {0, 0, 20}, {90, 0, 0})->setControlPointId(1);
		objects.create(spawn, {0, 0, 0}, {0, 0, 0})->setControlPointId(2);
		round = std::make_unique<dice::hfe::game::GameWorld>(templates, objects);
		dice::hfe::physics::Ground ground;
		ground.height = [](float x, float) { return 0.02f * x; }; // a gentle slope
		ground.heightAndNormal = [](float x, float, dice::hfe::physics::Vec3& normal) {
			normal = {-0.019996f, 0.9998f, 0}; // (-0.02, 1, 0) normalised
			return 0.02f * x;
		};
		round->enablePhysics(files, collisionTemplates, ground);
		round->startRound();
		logic.startRound();
	}
};

float distance(const net::Vec3f& a, const dice::hfe::physics::Vec3& b)
{
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

} // namespace

// The control-object record's mode-3 state end to end: a client predicts
// its soldier from its own moves (Prediction.h) while the host steps the
// same moves one a tick; every record corrects the prediction. A bot
// walks on the host, and the client's view of it (position, health) is
// checked against what the host had at the update's tick. Then a team
// change, a map change on the same connection and a kick.
TEST(NetHost_PredictionAndSession)
{
	HostRange range;
	dice::hfe::game::GameWorld& round = *range.round;
	host::HostSetup setup;
	setup.level = "Range";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(round, range.logic, range.templates, setup);
	CHECK(hosted.start());
	const int bot = round.addPlayer("Bot1", 2, true);
	CHECK(round.spawnSoldier(bot));

	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Joiner";
	client.world().setGhostTemplates([&range](uint32_t id) {
		const dice::hfe::world::ObjectTemplate* t = range.templates.findById(id);
		return t ? net::ghostTemplateFrom(range.templates, *t) : net::GhostTemplateNode {};
	});
	host::SoldierPrediction prediction(round);
	client.onMoveSent = [&prediction](const net::PlayerMove& move) { prediction.step(move); };
	client.sendOnTicks = true;
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));

	// The host's bot, by the tick of the ghost block that carried it.
	std::map<uint32_t, dice::hfe::game::GameWorld::PlayerInfo> botAt;
	uint32_t records = 0, compared = 0;
	float worstBot = 0, worstHealth = 0;
	// The host's at each tick: the server sends at the connection's rate
	// (20 a second for type 4), so an update can be a tick or two old.
	std::map<uint32_t, float> healthAt;
	int frame = 0;
	const auto step = [&] {
		++frame;
		// The client walks: forward, turning, a sprint, a jump now and then.
		dice::hfe::game::SoldierInput walk;
		walk.throttle = (frame / 45) % 4 == 3 ? 0.0f : 1.0f;
		walk.strafe = (frame / 70) % 2 == 0 ? 0.0f : 0.6f;
		walk.turn = (frame / 30) % 3 == 0 ? 4.0f : 0.0f;
		walk.sprint = (frame / 60) % 3 == 1;
		walk.jump = frame % 97 == 50;
		walk.aimPitch = 10.0f * std::sin(frame * 0.05f);
		client.setInput(net::playerInputFrom(walk));
		client.update();
		const net::RemoteWorld& world = client.world();
		if (world.controlRecords() != records) {
			records = world.controlRecords();
			const net::ControlState* state = world.controlState();
			if (state && !state->groups.empty() && state->groups.front().kind == net::NetworkableKind::Soldier) {
				if (!prediction.active()) {
					prediction.start(range.templates.find("pac_light_soldier"), world.controlObject().lastMove, state->groups.front(),
						world.controlObject().position);
				} else {
					prediction.reconcile(world.controlObject().lastMove, state->groups.front(), world.controlObject().position);
				}
			}
		}
		client.simulatedTick();
		// The bot's ghost against the host's bot at that update's tick.
		dice::hfe::game::GameWorld::PlayerInfo b;
		if (round.playerInfo(bot, b) && b.alive) {
			const uint16_t ghost = [&] {
				for (const net::RemotePlayer& p : world.playerList()) {
					if (p.name == "Bot1") {
						return p.soldierGhost;
					}
				}
				return uint16_t {0};
			}();
			const net::RemoteObject* seen = ghost ? world.object(ghost) : nullptr;
			if (const auto at = seen && seen->hasPosition ? botAt.find(seen->lastTick) : botAt.end(); at != botAt.end()) {
				worstBot = std::max(worstBot, distance(seen->position, at->second.position));
				worstHealth = std::max(worstHealth, std::fabs(seen->health - healthAt[seen->lastTick] / 100.0f));
				++compared;
			}
		}
		// The bot walks in a circle; it loses some health on the way.
		dice::hfe::game::SoldierInput botWalk;
		botWalk.throttle = 1;
		botWalk.turn = 2;
		round.setInput(bot, botWalk);
		if (frame % 40 == 0 && round.playerInfo(bot, b) && b.alive && round.hitPoints(b.soldier) > 30) {
			round.setHitPoints(b.soldier, round.hitPoints(b.soldier) - 3);
		}
		round.playerInfo(bot, b);
		botAt[hosted.tickCount() + 1] = b;
		healthAt[hosted.tickCount() + 1] = round.hitPoints(b.soldier);
		hosted.tick();
		round.tick(1.0f / 30);
		std::this_thread::sleep_for(std::chrono::milliseconds(33)); // about the game's 30 ticks a second
	};
	CHECK(pumpUntil(step, [&] { return client.world().gameState() == 1 && client.world().localPlayer() >= 0; }, 6000));
	// A team change first (6/7, while we have no soldier), then a spawn.
	const int me = client.world().localPlayer();
	dice::hfe::game::GameWorld::PlayerInfo info;
	CHECK(round.playerInfo(me, info) && info.team == 1);
	client.selectTeam(2);
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && info.team == 2; }));
	client.selectSpawnGroup(-1);
	CHECK(pumpUntil(step, [&] { return prediction.active(); }, 6000));
	for (int i = 0; i < 30 * 8; ++i) {
		step();
	}
	const host::SoldierPrediction::Stats& st = prediction.stats();
	std::printf("  prediction: %u steps, %u records (%u for moves not remembered), %u replayed, correction mean %.4f m, "
				"max %.4f m (%u over 1 cm)\n",
		st.steps, st.records, st.unknownMoves, st.replayed, st.records ? st.sumCorrection / st.records : 0.0,
		st.maxCorrection, st.corrections);
	std::printf("  the bot as the client saw it: %u updates compared, at most %.4f m and %.3f health from the host's\n",
		compared, worstBot, worstHealth);
	CHECK(client.world().controlStates() > 100 && client.world().controlStates() == client.world().controlRecords());
	CHECK(st.records > 100 && st.unknownMoves < st.records / 10);
	CHECK(st.sumCorrection / st.records < 0.05);
	CHECK(compared > 100 && worstBot < 0.01f && worstHealth < 0.01f);

	// A map change on the same connection: the level block again, and the
	// join's database for the new round.
	HostRange next;
	setup.level = "Range2";
	hosted.changeLevel(*next.round, next.logic, next.templates, setup);
	const auto stepNext = [&] {
		client.update();
		hosted.tick();
		next.round->tick(1.0f / 30);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	};
	CHECK(pumpUntil(stepNext, [&] {
		return client.world().levelsReceived() == 2 && client.world().localPlayer() >= 0 && client.world().roundStarted();
	}, 6000));
	CHECK(client.world().level().mapName == "Range2" && client.state() == net::GameClient::State::Connected);
	// A kick: KickBan to everyone, then the disconnect.
	CHECK(hosted.kickPlayer(client.world().localPlayer()));
	CHECK(pumpUntil(stepNext, [&] { return client.state() != net::GameClient::State::Connected; }));
	CHECK(hosted.clientCount() == 0);
}

namespace {

// The shipped object templates and materials on flat ground (as
// VehicleModelsTest's garage), with a control point and a spawn point for
// team 2 and one vehicle.
struct HostGarage {
	dice::hfe::io::FileManager files;
	dice::hfe::Console console {&files};
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::physics::CollisionTemplateManager collisionTemplates;
	dice::hfe::physics::MaterialManager materials;
	std::unique_ptr<dice::hfe::game::GameWorld> round;
	dice::hfe::game::GameLogic logic;
	uint32_t vehicle = 0;

	HostGarage(const char* gameDir, const char* vehicleName, const dice::hfe::physics::Vec3& at)
	{
		using namespace dice::hfe;
		files.setRootPath(gameDir);
		files.addSearchPath("mods/bf2142");
		files.setMountingAllowed(true);
		bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
			[this](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
		std::string out;
		console.run("ServerArchives.con", {}, out);
		files.setMountingAllowed(false);
		world::registerWorldConsoleMethods(console, templates, objects);
		templates.setCurrentFileProvider([this] { return console.currentFile(); });
		physics::registerCollisionConsoleMethods(console, collisionTemplates);
		physics::registerMaterialConsoleMethods(console, materials);
		std::vector<std::string> untyped;
		std::ifstream table(std::string(BF2142_SOURCE_DIR) + "/symbols/BF2142.exe.console.csv");
		std::string line;
		std::getline(table, line);
		while (std::getline(table, line)) {
			std::vector<std::string> cols;
			std::stringstream row(line);
			for (std::string col; std::getline(row, col, ',');) {
				cols.push_back(col);
			}
			if (cols.size() >= 6 && cols[0] == "ObjectTemplate" && !console.hasMethod("ObjectTemplate." + cols[1])
				&& (cols[2] == "property" || (cols[2] == "method" && cols[4] == "1" && cols[5] == "1"))) {
				untyped.push_back(cols[1]);
			}
		}
		world::registerUntypedTemplateProperties(console, templates, untyped);
		std::ifstream constants(std::string(BF2142_SOURCE_DIR) + "/symbols/BF2142.exe.constants.csv");
		while (std::getline(constants, line)) {
			const size_t comma = line.find(',');
			if (comma != std::string::npos) {
				console.registerConstant(line.substr(0, comma), line.substr(comma + 1));
			}
		}
		templates.setLoading(true);
		world::loadObjectTemplates(console, files, "Objects/", false);
		templates.setLoading(false);
		console.run("Common/Material/MaterialManagerSettings.con", {}, out);
		world::ObjectTemplate* point = templates.create("ControlPoint", "garage_cp");
		point->settings()["controlPointId"] = "2";
		point->settings()["team"] = "2";
		point->settings()["unableToChangeTeam"] = "1";
		objects.create(point, {-200, 0, 0});
		world::ObjectTemplate* spawn = templates.create("SpawnPoint", "garage_sp");
		objects.create(spawn, {0, 0, 0}, {0, 0, 0})->setControlPointId(2);
		world::ObjectTemplate* t = templates.find(vehicleName);
		if (!t) {
			return;
		}
		world::Object* made = objects.create(t, {at.x, at.y, at.z}, {0, 0, 0});
		made->setTeam(2);
		vehicle = made->id();
		round = std::make_unique<game::GameWorld>(templates, objects);
		round->enablePhysics(files, collisionTemplates,
			{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return 0.0f;
			 }},
			&materials);
		round->startRound();
		logic.startRound();
	}
};

} // namespace

// The vehicle prediction end to end: a client drives a shipped car on the
// host over 127.0.0.1; its own copy (GameWorld::LoneVehicle) is stepped
// with each move it sends and corrected by each record of the driver's
// seat (Prediction.h's VehiclePrediction). Needs $BF2142_GAME_DIR.
TEST(NetHost_VehiclePrediction)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		std::printf("  (BF2142_GAME_DIR not set: skipped)\n");
		return;
	}
	HostGarage garage(gameDir, "eu_fav", {0, 0, 12});
	CHECK(garage.round != nullptr);
	if (!garage.round) {
		return;
	}
	dice::hfe::game::GameWorld& round = *garage.round;
	host::HostSetup setup;
	setup.level = "Garage";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(round, garage.logic, garage.templates, setup);
	CHECK(hosted.start());

	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Driver";
	client.world().setGhostTemplates([&garage](uint32_t id) {
		const dice::hfe::world::ObjectTemplate* t = garage.templates.findById(id);
		return t ? net::ghostTemplateFrom(garage.templates, *t) : net::GhostTemplateNode {};
	});
	host::SoldierPrediction soldier(round);
	host::VehiclePrediction car(round);
	client.onMoveSent = [&](const net::PlayerMove& move) {
		soldier.step(move);
		car.step(move);
	};
	client.sendOnTicks = true;
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));

	dice::hfe::game::SoldierInput drive;
	uint32_t records = 0;
	float worstSeen = 0;
	int frame = 0;
	const auto step = [&] {
		++frame;
		client.setInput(net::playerInputFrom(drive));
		client.update();
		const net::RemoteWorld& world = client.world();
		if (world.controlRecords() != records) {
			records = world.controlRecords();
			const net::ControlState* state = world.controlState();
			const net::ControlObjectRecord& record = world.controlObject();
			if (state && !state->groups.empty()) {
				const net::NetworkableState& root = state->groups.front();
				if (root.kind == net::NetworkableKind::Soldier) {
					car.stop();
					if (!soldier.active()) {
						soldier.start(garage.templates.find("eu_light_soldier"), record.lastMove, root, record.position);
					} else {
						soldier.reconcile(record.lastMove, root, record.position);
					}
				} else if (root.kind == net::NetworkableKind::SimpleObject && record.vehicle) {
					soldier.stop();
					const net::RemoteObject* seen = world.object(record.vehicle);
					const dice::hfe::world::ObjectTemplate* t = seen ? garage.templates.findById(seen->templateId) : nullptr;
					if (!car.active()) {
						car.start(t, record.lastMove, root, record.position);
					} else {
						car.reconcile(record.lastMove, root, record.position);
					}
				}
			}
		}
		client.simulatedTick();
		hosted.tick();
		round.tick(1.0f / 30);
		if (car.active()) {
			// The prediction against the host's car at the same moment
			// (the host has taken every move sent but those in flight).
			const dice::hfe::world::Object* v = garage.objects.find(garage.vehicle);
			worstSeen = std::max(worstSeen, distance(car.position(), {v->position().x, v->position().y, v->position().z}));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(33));
	};
	CHECK(pumpUntil(step, [&] { return client.world().gameState() == 1 && client.world().localPlayer() >= 0; }, 8000));
	const int me = client.world().localPlayer();
	client.selectTeam(2);
	dice::hfe::game::GameWorld::PlayerInfo info;
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && info.team == 2; }));
	client.selectSpawnGroup(-1);
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && info.alive; }, 6000));
	// Beside the car, then in: the use key until the host seats us.
	for (int tries = 0; tries < 40 && !(round.playerInfo(me, info) && info.vehicle); ++tries) {
		const float a = static_cast<float>(tries % 8) * 0.785398f, r = 1.5f + static_cast<float>(tries / 8);
		round.playerInfo(me, info);
		round.setSoldierPosition(info.soldier, {r * std::sin(a), 0, 12 + r * std::cos(a)});
		drive = {};
		drive.use = true;
		for (int i = 0; i < 4; ++i) {
			step();
		}
		drive = {};
		for (int i = 0; i < 4; ++i) {
			step();
		}
	}
	CHECK(round.playerInfo(me, info) && info.vehicle == garage.vehicle && info.seat == 0);
	CHECK(pumpUntil(step, [&] { return car.active(); }, 4000));
	const dice::hfe::world::Object* v = garage.objects.find(garage.vehicle);
	const dice::hfe::physics::Vec3 from {v->position().x, v->position().y, v->position().z};
	// Drive: full throttle, steering one way then the other, a stop.
	for (int i = 0; i < 30 * 10; ++i) {
		drive = {};
		drive.throttle = i < 30 * 8 ? 1.0f : -1.0f;
		drive.strafe = (i / 60) % 3 == 1 ? 0.7f : (i / 60) % 3 == 2 ? -0.5f : 0.0f;
		step();
	}
	const host::VehiclePrediction::Stats& st = car.stats();
	const float driven = distance(car.position(), from);
	std::printf("  vehicle prediction: %u steps, %u records (%u for moves not remembered), %u replayed, driven %.1f m; "
				"correction mean %.4f m, max %.4f m (%u over 1 cm); the prediction at most %.2f m from the host's car\n",
		st.steps, st.records, st.unknownMoves, st.replayed, driven, st.records ? st.sumCorrection / st.records : 0.0,
		st.maxCorrection, st.corrections, worstSeen);
	std::printf("  control records: %u decoded, %u of them relative to a history slot\n", client.world().controlStates(),
		client.world().relativeControlStates());
	CHECK(client.world().relativeControlStates() > client.world().controlStates() / 2);
	CHECK(client.world().controlStates() + 2 >= client.world().controlRecords());
	CHECK(st.records > 100 && st.unknownMoves < st.records / 10);
	CHECK(driven > 20);
	CHECK(st.sumCorrection / std::max(1u, st.records) < 0.05);
	client.disconnect();
}

// GameWorld::removePlayer (the game server's destroyPlayer, 0x004acfb0):
// the soldier dies and lies, the squad and the command are left, the id is
// free for the next player.
TEST(GameWorld_RemovePlayerFreesItsSlot)
{
	HostRange range;
	dice::hfe::game::GameWorld& round = *range.round;
	const int a = round.addPlayer("A", 1, false);
	const int b = round.addPlayer("B", 1, false);
	CHECK(round.spawnSoldier(a) && round.joinSquad(a, 1) && round.joinSquad(b, 1));
	round.setCommander(1, b); // may be refused without a commander kit
	dice::hfe::game::GameWorld::PlayerInfo info;
	CHECK(round.playerInfo(a, info) && info.alive && info.squad == 1);
	const uint32_t soldier = info.soldier;
	CHECK(round.removePlayer(a));
	CHECK(!round.playerInfo(a, info) && !round.removePlayer(a));
	CHECK(round.objects().find(soldier) != nullptr); // the body lies on
	CHECK(round.playerList().size() == 1);
	CHECK(round.playerInfo(b, info) && info.squad == 1);
	const int commanderBefore = round.commander(1);
	CHECK(round.removePlayer(b));
	CHECK(commanderBefore != b || round.commander(1) != b);
	// The lowest free id first.
	CHECK(round.addPlayer("C", 2, false) == a && round.addPlayer("D", 2, false) == b);
	CHECK(round.addPlayer("E", 2, false) == 2);
	CHECK(round.playerInfo(a, info) && info.name == "C" && info.team == 2 && !info.alive && info.squad == 0);
	for (int i = 0; i < 30; ++i) {
		round.tick(1.0f / 30);
	}
}

// The crosshair object and Radio.sendSpottedMessage (0x0075be30, 0x007c8160):
// the enemy the view's ray meets is reported to the spotter's squad.
TEST(GameWorld_SpottedCrosshairTarget)
{
	HostRange range;
	dice::hfe::game::GameWorld& round = *range.round;
	const int a = round.addPlayer("A", 1, false);
	const int b = round.addPlayer("B", 2, false);
	const int c = round.addPlayer("C", 1, false);
	CHECK(round.spawnSoldier(a) && round.spawnSoldier(b) && round.spawnSoldier(c));
	round.tick(1.0f / 30);
	dice::hfe::game::GameWorld::PlayerInfo me, enemy, mate;
	round.playerInfo(a, me);
	round.playerInfo(b, enemy);
	round.playerInfo(c, mate);
	const float yaw = me.yaw * 3.14159265f / 180.0f;
	const dice::hfe::physics::Vec3 ahead {std::sin(yaw), 0, std::cos(yaw)};
	round.setSoldierPosition(mate.soldier, me.position + ahead * 8.0f + dice::hfe::physics::Vec3 {3, 0, 0});
	round.setSoldierPosition(enemy.soldier, me.position + ahead * 30.0f);
	round.tick(1.0f / 30);
	CHECK(round.crosshairTarget(c) == -1 || round.crosshairTarget(c) == b);
	dice::hfe::physics::Vec3 at;
	CHECK(round.crosshairTarget(a, 300, &at) == b);
	CHECK(round.crosshairTarget(a, 10) == -1); // out of reach
	CHECK(!round.squadKnows(1, 0, b));
	CHECK(round.spotCrosshair(a) == b);
	CHECK(round.squadKnows(1, 0, b));
}

// NetworkableInfo.* (Objects/Common/Networkables.con): c_NIGhostAlways is
// -1, base priorities at most 255, unique and forced ids listed.
TEST(NetHost_NetworkableInfoMethods)
{
	using namespace dice::hfe;
	Console console;
	host::registerNetworkableInfoConsoleMethods(console);
	std::string out;
	for (const char* line : {"NetworkableInfo.createNewInfo TestAlwaysInfo", "NetworkableInfo.setBasePriority c_NIGhostAlways",
			 "NetworkableInfo.setPredictionMode PMLinear", "NetworkableInfo.createNewInfo TestHighInfo",
			 "NetworkableInfo.setBasePriority 300", "NetworkableInfo.setIsUnique 1"}) {
		console.execute(line, out);
	}
	const net::NetworkableInfoTable& t = net::networkableInfos();
	CHECK(t.basePriority("testalwaysinfo") == -1 && t.infos.at("testalwaysinfo").predictionMode == 1);
	CHECK(t.basePriority("TestHighInfo") == 255 && t.basePriority("NoSuchInfo") == 1);
	CHECK(std::find(t.unique.begin(), t.unique.end(), "TestHighInfo") != t.unique.end());
	console.execute("NetworkableInfo.setIsUnique 0", out);
	CHECK(std::find(t.unique.begin(), t.unique.end(), "TestHighInfo") == t.unique.end());
}
