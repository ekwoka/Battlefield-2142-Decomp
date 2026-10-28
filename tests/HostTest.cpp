// The server side (GameServer, the QR2 query responder and LAN search,
// HostedGame) against the rewrite's own client, over UDP on 127.0.0.1.
#include "TestMain.h"

#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Main/HostedGame.h"
#include "Main/Prediction.h"
#include "Physics/CollisionTemplateManager.h"
#include "Net/GameClient.h"
#include "Net/GameServer.h"
#include "Net/ServerQuery.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <chrono>
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
	client.disconnect();
	CHECK(pumpUntil(step, [&] { return hosted.clientCount() == 0; }));
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
				worstHealth = std::max(worstHealth, std::fabs(seen->health - round.hitPoints(at->second.soldier) / 100.0f));
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
