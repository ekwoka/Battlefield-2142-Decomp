// The server side (GameServer, the QR2 query responder and LAN search,
// HostedGame) against the rewrite's own client, over UDP on 127.0.0.1.
#include "TestMain.h"

#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Main/HostedGame.h"
#include "Net/GameClient.h"
#include "Net/GameServer.h"
#include "Net/ServerQuery.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <chrono>
#include <cmath>
#include <functional>
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
