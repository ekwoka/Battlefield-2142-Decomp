// The server side (GameServer, the QR2 query responder and LAN search,
// HostedGame) against the rewrite's own client, over UDP on 127.0.0.1.
#include "TestMain.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/CombatAreas.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Main/HostedGame.h"
#include "Main/HudMenusComm.h"
#include "Main/HudWorld.h"
#include "Main/Prediction.h"
#include "Main/RemoteGuns.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
#include "Net/BitStream.h"
#include "Net/ContentCheck.h"
#include "Net/GameProtocol.h"
#include "Net/GameClient.h"
#include "Net/GameServer.h"
#include "Net/ServerQuery.h"
#include "Net/ServerSettings.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/TemplateFiles.h"
#include "World/WorldConsole.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <tuple>

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

// The rules a hosted game reports (the server key callback 0x00578bb0
// over 0x006423d0's fields), found by the LAN search's keys as the server
// info popup reads them (0x004661a0).
TEST(NetHost_ReportedRules)
{
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::game::GameWorld round(templates, objects);
	dice::hfe::game::GameLogic logic;
	round.startRound();
	logic.startRound();
	net::ServerSettings sv;
	sv.soldierFriendlyFire = 100;
	sv.roundsPerMap = 2;
	sv.spawnTime = 12;
	sv.timeLimit = 1800;
	sv.ticketRatio = 150;
	sv.teamRatioPercent = 110;
	sv.autoBalanceTeam = true;
	sv.allowSpectators = true;
	host::HostSetup setup;
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	setup.settings = &sv;
	host::HostedGame hosted(round, logic, templates, setup);
	net::ServerReport report = hosted.report();
	CHECK(report.keys["bf2142_friendlyfire"] == "1" && report.keys["roundtime"] == "2" && report.keys["timelimit"] == "1800");
	CHECK(std::atof(report.keys["bf2142_spawntime"].c_str()) == 12.0 && report.keys["bf2142_ticketratio"] == "150");
	CHECK(std::atof(report.keys["bf2142_teamratio"].c_str()) == 110.0 && report.keys["bf2142_autobalanced"] == "1");
	CHECK(report.keys["bf2142_allow_spectators"] == "1" && report.keys["bf2142_tkmode"] == "Punish");
	// Under 100 per cent soldier friendly fire it reports none; a ranked
	// server its fixed rules (0x00430f60 / 0x00430fa0 / 0x00430fc0).
	sv.soldierFriendlyFire = 50;
	sv.ranked = true;
	report = hosted.report();
	CHECK(report.keys["bf2142_friendlyfire"] == "0" && report.keys["bf2142_ticketratio"] == "100");
	CHECK(std::atof(report.keys["bf2142_spawntime"].c_str()) == 15.0 && std::atof(report.keys["bf2142_teamratio"].c_str()) == 100.0);

	// The keys travel: the game's own ids (0x005785b0) by name, asked by the LAN search.
	CHECK(net::qr2KeyId("bf2142_friendlyfire") == 0x3b && net::qr2KeyId("bf2142_allow_spectators") == 0x51);
	CHECK(net::qr2KeyId("") == -1);
	const std::vector<uint8_t>& keys = net::lanQueryKeys();
	net::ServerReport back;
	CHECK(net::parseQr2Reply(net::qr2Answer(net::qr2Query(7, keys), report), 7, keys, {}, back));
	for (const char* key : {"bf2142_friendlyfire", "roundtime", "bf2142_spawntime", "timelimit", "bf2142_ticketratio",
			 "bf2142_teamratio", "bf2142_autobalanced", "bf2142_allow_spectators"}) {
		CHECK(back.keys.count(key) == 1 && back.keys[key] == report.keys[key]);
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

// What a joined game's HUD reads from the server (main.cpp's updateHud
// through HudWorld): the teams' tickets (ghost 0x201), the players with
// their squads, a squad leader's request to the commander and the
// commander's answer (IssueSquadOrder / SetAcceptOrder with store 2,
// 2/0x1e), and the host's own player's vote replies.
TEST(NetHost_JoinedHudStateAndRequests)
{
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::game::GameWorld round(templates, objects);
	dice::hfe::game::GameLogic logic;
	round.startRound();
	logic.startRound();
	logic.setTickets(1, 150);
	logic.setTickets(2, 99);
	host::HostSetup setup;
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(round, logic, templates, setup);
	CHECK(hosted.start());

	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Leader";
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
	const auto step = [&] {
		client.update();
		hosted.tick();
		round.tick(1.0f / 30);
	};
	CHECK(pumpUntil(step, [&] { return client.world().tickets(1) == 150 && client.world().tickets(2) == 99; }));
	logic.setTickets(2, 98);
	CHECK(pumpUntil(step, [&] { return client.world().tickets(2) == 98; }));
	CHECK(client.world().teamState().complete && client.world().tickets(1) == 150);
	const int me = client.world().localPlayer();
	dice::hfe::game::GameWorld::PlayerInfo mine;
	CHECK(me >= 0 && round.playerInfo(me, mine) && mine.team == 1);
	const int bot = round.addPlayer("Bot1", 1, true);
	// Our player leads squad 1; the bot commands team 1.
	CHECK(round.joinSquad(me, 1));
	round.setCommanderEnabled(true);
	CHECK(round.setCommander(1, bot));
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* p = client.world().player(me);
		const net::RemotePlayer* c = client.world().player(bot);
		return p && p->squad == 1 && p->squadLeader && c && c->commander;
	}));
	// The HUD's view of it.
	dice::hfe::world::ObjectManager clientObjects;
	dice::hfe::game::GameWorld clientRound(templates, clientObjects);
	std::map<uint16_t, uint32_t> ghostObjects;
	host::RemoteHudWorld hud(client.world(), clientRound, ghostObjects);
	CHECK(hud.joined() && hud.localPlayer() == me && hud.playerList().size() == 2 && hud.playerTeam() == 1);
	CHECK(hud.tickets(1) == 150 && hud.tickets(2) == 98 && hud.commander(1) == bot && hud.commander(2) == -1);
	dice::hfe::game::GameWorld::PlayerInfo info;
	CHECK(hud.playerInfo(me, info) && info.squadLeader && info.squad == 1 && info.name == "Leader");
	host::LocalHudWorld local(round, logic);
	CHECK(!local.joined() && local.tickets(1) == 150 && local.playerList().size() == 2);
	// A request for the UAV at a point: stored on the server, sent back to
	// everyone (store 2) and the leader's 2/0x1e.
	client.requestAsset(2, {10, 0, 20});
	bool sent = false;
	CHECK(pumpUntil(step, [&] {
		for (const net::PostRemoteInfo& m : client.world().takeMessages()) {
			sent = sent || (m.category == 2 && m.id == 0x1e && m.payloadInt() == 2);
		}
		return sent && round.squadRequest(1, 1, 2) && client.world().squadRequests(1).size() == 1;
	}));
	const auto requests = hud.squadRequests(1);
	CHECK(requests.size() == 1);
	if (requests.size() == 1) {
		CHECK(requests[0].squad == 1 && requests[0].kind == 2);
		CHECK(std::abs(requests[0].position.x - 10) < 1e-3f && std::abs(requests[0].position.z - 20) < 1e-3f);
	}
	// Only a leader or the commander may answer; the leader's answer here
	// (SetAcceptOrder) takes it off everywhere.
	client.answerRequest(1, 2);
	CHECK(pumpUntil(step, [&] { return !round.squadRequest(1, 1, 2) && client.world().squadRequests(1).empty(); }));
	// A request stored on the host (its own leader's) reaches the client too.
	CHECK(round.storeSquadRequest(1, 1, 3, {1, 2, 3}));
	CHECK(pumpUntil(step, [&] { return client.world().squadRequests(1).size() == 1; }));
	round.removeSquadRequest(1, 1, 3);
	CHECK(pumpUntil(step, [&] { return client.world().squadRequests(1).empty(); }));
	// The host's own player's votes: its replies come to onVoteReply (here
	// the bot stands in for the host's player: a kick vote, refused or
	// started, answers it).
	std::vector<dice::hfe::game::Votes::Reply> replies;
	hosted.onVoteReply = [&replies](const dice::hfe::game::Votes::Reply& r) { replies.push_back(r); };
	hosted.localVote(bot, dice::hfe::game::Votes::kKick, 0, me, 0);
	CHECK(!replies.empty());
	CHECK(replies.empty() || replies.front().to == -1 || replies.front().to == bot);
	client.disconnect();
	CHECK(pumpUntil(step, [&] { return hosted.clientCount() == 0; }));
}


// Two clients on a hosted round: the radio both ways (a client's message
// said in the round by its connection's player and heard by the other
// client, the game's own with no sender), the offer of command (2/0x23)
// answered by a client, the commander's tools from a client (the supply
// drop; refused from anyone else), the squads' events (a new squad by
// name, the leader's invite of a bot, his kick), the orders both ways and
// a mutiny vote.
TEST(NetHost_RadioCommanderToolsAndSquads)
{
	dice::hfe::world::ObjectTemplateManager templates;
	templates.registerType(1, "SupplyObject");
	templates.create("SupplyObject", "supply_crate")->settings()["sharedStorageSize"] = "500";
	templates.registerType(2, "OverheadCamera");
	{
		auto* camera = templates.create("OverheadCamera", "MultiPlayerOverheadCamera");
		camera->settings()["height"] = "260";
		camera->settings()["networkableInfo"] = "GhostAlwaysWithPrediction";
	}
	dice::hfe::world::ObjectManager objects;
	dice::hfe::game::GameWorld round(templates, objects);
	dice::hfe::game::GameLogic logic;
	round.startRound();
	logic.startRound();
	round.setCommanderEnabled(true);
	host::HostSetup setup;
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(round, logic, templates, setup);
	// The voice message table both sides have (VoiceMessages.con's order).
	const std::vector<std::string> table {"medic", "need_ammo", "roger_that", "AUTO_MOODGP_TEAM_lostcp", "supply_accept"};
	hosted.radioIndex = [&table](const std::string& voice) {
		const auto it = std::find(table.begin(), table.end(), voice);
		return it == table.end() ? -1 : static_cast<int>(it - table.begin());
	};
	hosted.radioVoice = [&table](int index) {
		return index >= 0 && static_cast<size_t>(index) < table.size() ? table[static_cast<size_t>(index)] : std::string();
	};
	std::vector<std::tuple<int, int, bool>> tools; // (player, tool, done)
	hosted.onCommanderTool = [&tools](int player, const net::CommanderToolUse& use, bool done) {
		tools.push_back({player, static_cast<int>(use.tool), done});
	};
	CHECK(hosted.start());

	net::GameClient a, b;
	for (net::GameClient* c : {&a, &b}) {
		c->world().setGhostTemplates([&templates](uint32_t id) {
			const dice::hfe::world::ObjectTemplate* t = templates.findById(id);
			return t ? net::ghostTemplateFrom(templates, *t) : net::GhostTemplateNode {};
		});
	}
	net::GameClientOptions options;
	options.playerName = "Alpha";
	CHECK(a.connect("127.0.0.1", hosted.gamePort(), options));
	options.playerName = "Bravo";
	CHECK(b.connect("127.0.0.1", hosted.gamePort(), options));
	std::vector<net::GameEvent> heardA, heardB;
	std::vector<net::PostRemoteInfo> messagesA;
	const auto step = [&] {
		a.update();
		b.update();
		hosted.tick();
		round.tick(1.0f / 30);
		for (const net::GameEvent& e : a.world().takeUnhandled()) {
			heardA.push_back(e);
		}
		for (const net::GameEvent& e : b.world().takeUnhandled()) {
			heardB.push_back(e);
		}
		for (const net::PostRemoteInfo& m : a.world().takeMessages()) {
			messagesA.push_back(m);
		}
	};
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* pa = a.world().player(a.world().localPlayer());
		const net::RemotePlayer* pb = b.world().player(b.world().localPlayer());
		return pa && pa->name == "Alpha" && pb && pb->name == "Bravo" && a.world().gameState() == 1 && b.world().gameState() == 1;
	}));
	const int idA = a.world().localPlayer(), idB = b.world().localPlayer();
	dice::hfe::game::GameWorld::PlayerInfo infoA, infoB;
	CHECK(round.playerInfo(idA, infoA));
	const int team = infoA.team;
	b.selectTeam(team); // 6/7, while B has no soldier
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* pb = b.world().player(idB);
		return round.playerInfo(idB, infoB) && infoB.team == team && pb && pb->team == team;
	}));

	// A's message to everyone (TCGlobal): said by A in the round, B hears
	// it with A as the sender (source 3); A doesn't get it back.
	const auto radioOf = [](const std::vector<net::GameEvent>& events, int index, net::RadioMessageInfo& out) {
		for (const net::GameEvent& e : events) {
			if (net::parseRadioMessage(e, out) && out.index == index) {
				return true;
			}
		}
		return false;
	};
	a.radioMessage(0, 2); // roger_that
	net::RadioMessageInfo radio;
	CHECK(pumpUntil(step, [&] { return radioOf(heardB, 2, radio); }));
	CHECK(radio.sender == idA && radio.team == team && radio.channel == 0 && radio.source == 3);
	CHECK(!radioOf(heardA, 2, radio));
	// The game's own (no sender) to the team.
	round.radioMessage(-1, 13, "AUTO_MOODGP_TEAM_lostcp", team);
	CHECK(pumpUntil(step, [&] { return radioOf(heardA, 3, radio) && radioOf(heardB, 3, radio); }));
	CHECK(radio.sender == 0xff && radio.source == 0 && radio.channel == 13);

	// A applies for command; after the apply period the server offers it
	// (2/0x23) and A takes it (the Commander event's kind 0).
	a.applyForCommander();
	CHECK(pumpUntil(step, [&] {
		return std::any_of(messagesA.begin(), messagesA.end(), [](const net::PostRemoteInfo& m) { return m.category == 2 && m.id == 0x23; });
	}, 8000));
	a.answerCommanderOffer(true);
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* pa = a.world().player(idA);
		return round.commander(team) == idA && pa && pa->commander;
	}));

	// The commander's camera (CommanderCam 0x24: x, height, z): the server
	// makes the team's overhead camera and places it over the point at its
	// template's height (0x005f8170); the clients see it as its ghost. B's
	// (not the commander) moves nothing.
	const auto commanderCam = [](float x, float y, float z) {
		net::GameEvent cam;
		cam.type = net::kEvCommanderCam;
		for (const float v : {x, y, z}) {
			uint32_t bits = 0;
			std::memcpy(&bits, &v, 4);
			cam.values.push_back(bits);
		}
		return cam;
	};
	b.sendEvent(commanderCam(-50, 80, 70));
	a.sendEvent(commanderCam(120, 80, -30));
	net::Vec3f overhead;
	CHECK(pumpUntil(step, [&] { return b.world().overheadCamera(team, overhead); }));
	CHECK(std::abs(overhead.x - 120) < 0.01f && std::abs(overhead.z + 30) < 0.01f && std::abs(overhead.y - 260) < 0.01f);
	CHECK(hosted.overheadCamera(team) != 0 && hosted.overheadCamera(3 - team) == 0);
	a.sendEvent(commanderCam(40, 80, 10));
	CHECK(pumpUntil(step, [&] { return a.world().overheadCamera(team, overhead) && std::abs(overhead.x - 40) < 0.01f; }));

	// The commander's supply drop from his client; B's is refused.
	b.commanderTool(net::CommanderToolUse::Supplies, 30, 40);
	a.commanderTool(net::CommanderToolUse::Supplies, 10, 20);
	CHECK(pumpUntil(step, [&] { return tools.size() == 1 && !round.supplyCrates().empty(); }));
	CHECK(!tools.empty() && std::get<0>(tools[0]) == idA && std::get<2>(tools[0]));
	if (const auto crates = round.supplyCrates(); !crates.empty()) {
		const dice::hfe::world::Object* crate = objects.find(crates.front());
		CHECK(crate && std::abs(crate->position().x - 10) < 0.01f && std::abs(crate->position().z - 20) < 0.01f);
	}
	// Again too soon: refused by the round.
	a.commanderTool(net::CommanderToolUse::Supplies, 10, 20);
	CHECK(pumpUntil(step, [&] { return tools.size() == 2; }));
	CHECK(tools.size() == 2 && !std::get<2>(tools[1]));

	// B makes squad 1 by naming it, invites a bot (who takes it), then
	// removes him (LeaveSquad with the member).
	b.createSquad(1, "Alpha");
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* pb = b.world().player(idB);
		return pb && pb->squad == 1 && pb->squadLeader;
	}));
	const int bot = round.addPlayer("Bot1", team, true);
	CHECK(pumpUntil(step, [&] { return b.world().player(bot) != nullptr; }));
	b.invite(bot, 1);
	CHECK(pumpUntil(step, [&] {
		dice::hfe::game::GameWorld::PlayerInfo p;
		return round.playerInfo(bot, p) && p.squad == 1;
	}));
	b.leaveSquad(1, bot);
	CHECK(pumpUntil(step, [&] {
		dice::hfe::game::GameWorld::PlayerInfo p;
		return round.playerInfo(bot, p) && p.squad == 0;
	}));
	// The commander's order to B's squad (IssueSquadOrder store 1, a defend
	// placed on the map), B's answer (SetAcceptOrder), B's own order to his
	// squad (store 0); each reaches the other client.
	a.issueOrder(1, 1, 6, {50, 0, 60});
	CHECK(pumpUntil(step, [&] {
		const net::RemoteWorld::SquadOrder* o = b.world().squadOrder(team, 1, 1);
		return round.squadOrder(team, 1) && o && o->kind == 6 && std::abs(o->point.z - 60) < 0.01f && !o->accepted;
	}));
	b.answerOrder(1, 6, true);
	CHECK(pumpUntil(step, [&] {
		const net::RemoteWorld::SquadOrder* o = a.world().squadOrder(team, 1, 1);
		return round.squadOrder(team, 1) && round.squadOrder(team, 1)->accepted && o && o->accepted;
	}));
	b.issueOrder(0, 1, 4, {-20, 0, 5});
	CHECK(pumpUntil(step, [&] {
		const net::RemoteWorld::SquadOrder* o = a.world().squadOrder(team, 1, 0);
		return round.squadLeaderOrder(team, 1) && o && o->kind == 4 && std::abs(o->point.x + 20) < 0.01f;
	}));
	// A refused order goes (0x004c2f60), here and there.
	a.issueOrder(1, 1, 5, {70, 0, 70});
	CHECK(pumpUntil(step, [&] {
		const net::RemoteWorld::SquadOrder* o = b.world().squadOrder(team, 1, 1);
		return o && o->kind == 5;
	}));
	b.answerOrder(1, 5, false);
	CHECK(pumpUntil(step, [&] { return !round.squadOrder(team, 1) && !a.world().squadOrder(team, 1, 1); }));
	// A non-leader can't remove anyone.
	a.leaveSquad(1, idB);
	for (int i = 0; i < 30; ++i) {
		step();
	}
	CHECK(round.playerInfo(idB, infoB) && infoB.squad == 1);

	// B's mutiny vote against the commander (kind 3) reaches the server's
	// votes, which answer it.
	heardB.clear();
	b.startVote(3, idA);
	CHECK(pumpUntil(step, [&] {
		return std::any_of(heardB.begin(), heardB.end(), [](const net::GameEvent& e) { return e.type == net::kEvVote && e.u(8) == 3; });
	}));
	a.disconnect();
	b.disconnect();
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

// A playtest's findings on a hosted round: a client's name reaches the
// round (ChangePlayerName, 0x0050bb90: the scripts and the scoreboards
// read the round's); a squad's spawn group (ids from 0xc0, 0x006347c0,
// placed at its leader: 0x004c7f10) lets a member spawn on the leader
// through 6/6, as the spawn screen's leader marker asks; and a spawn asked
// for waits out the death's spawn time (0x004bb220).
TEST(NetHost_NameSquadSpawnAndSpawnWait)
{
	HostRange range;
	dice::hfe::game::GameWorld& round = *range.round;
	round.autoRespawn = false;
	round.setManDownTime(0);
	round.setSpawnTime(2);
	host::HostSetup setup;
	setup.level = "Range";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(round, range.logic, range.templates, setup);
	CHECK(hosted.start());
	// The squad's leader, a bot of team 2, walks away from the team's spawn
	// point (0, 0, 0).
	const int leader = round.addPlayer("Leader", 2, true);
	CHECK(round.joinSquad(leader, 1));
	CHECK(round.spawnSoldier(leader));

	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Joiner";
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
	int ticks = 0;
	bool leaderWalks = true;
	const auto step = [&] {
		client.update();
		dice::hfe::game::SoldierInput walk;
		walk.throttle = leaderWalks ? 1.0f : 0.0f;
		round.setInput(leader, walk);
		hosted.tick();
		round.tick(1.0f / 30);
		++ticks;
	};
	CHECK(pumpUntil(step, [&] { return client.world().gameState() == 1 && client.world().localPlayer() >= 0; }, 6000));
	const int me = client.world().localPlayer();
	dice::hfe::game::GameWorld::PlayerInfo info;
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && info.name == "Joiner"; }));
	// 0x0050bb90 renames only the sender's own player (the event's byte
	// against the connection's, +0x2c): a rename of the leader is refused.
	net::GameEvent rename;
	rename.type = net::kEvChangePlayerName;
	rename.values = {static_cast<uint32_t>(leader) & 0xff};
	rename.bytes = {"Impostor"};
	client.sendEvent(rename);
	rename.values = {static_cast<uint32_t>(me) & 0xff};
	rename.bytes = {"Joiner2"};
	client.sendEvent(rename);
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && info.name == "Joiner2"; }));
	CHECK(round.playerInfo(leader, info) && info.name == "Leader");
	client.selectTeam(2);
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && info.team == 2; }));
	client.joinSquad(1);
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && info.squad == 1; }));
	for (int i = 0; i < 30 * 6; ++i) {
		step();
	}
	leaderWalks = false;
	dice::hfe::game::GameWorld::PlayerInfo lead;
	CHECK(round.playerInfo(leader, lead) && lead.alive && lead.squadLeader);
	CHECK(std::hypot(lead.position.x, lead.position.z) > 12);
	// The squad's group, at the leader's soldier ghost.
	uint16_t leaderGhost = 0;
	int group = -1;
	CHECK(pumpUntil(step, [&] {
		for (const net::RemotePlayer& p : client.world().playerList()) {
			leaderGhost = p.name == "Leader" ? p.soldierGhost : leaderGhost;
		}
		for (const auto& [id, g] : client.world().spawnGroups()) {
			group = leaderGhost && g.ghost == leaderGhost && g.team == 2 ? id : group;
		}
		return group >= 0;
	}));
	CHECK(group >= 0xc0 && group <= 0xff);
	client.selectSpawnGroup(group);
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && info.alive; }));
	round.playerInfo(leader, lead);
	CHECK(distance({info.position.x, info.position.y, info.position.z}, lead.position) < 10);
	CHECK(std::hypot(info.position.x, info.position.z) > 5); // not at the team's spawn point

	// Dead, a spawn asked for at once comes after the 2 s spawn time.
	client.suicide();
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && !info.alive; }));
	const int diedAt = ticks;
	client.selectSpawnGroup(-1);
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && info.alive; }));
	CHECK(ticks - diedAt >= 2 * 30 - 1);
	client.disconnect();
	CHECK(pumpUntil(step, [&] { return hosted.clientCount() == 0; }));
}

// The others' soldiers between machines: the soldier ghost's pose (mask
// 0x20, +0x268 / +0x26c) and the look's twist (4), the player networkable's
// fire state (0x40, +0x19c) and presses (0x4000000) and man-down (0x20);
// the Spotted event (0x0050e910: a radio call's code to the team's players
// as 2/<code> with the reporter's soldier); an invite asked of a client's
// player (2/0x10, 0x004c3190) and its refusal (Invite without the bit).
TEST(NetHost_StanceFireSpottedAndInvites)
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

	net::GameClient a, b;
	net::GameClientOptions options;
	for (net::GameClient* c : {&a, &b}) {
		c->world().setGhostTemplates([&range](uint32_t id) {
			const dice::hfe::world::ObjectTemplate* t = range.templates.findById(id);
			return t ? net::ghostTemplateFrom(range.templates, *t) : net::GhostTemplateNode {};
		});
	}
	options.playerName = "Alpha";
	CHECK(a.connect("127.0.0.1", hosted.gamePort(), options));
	options.playerName = "Bravo";
	CHECK(b.connect("127.0.0.1", hosted.gamePort(), options));
	std::vector<net::PostRemoteInfo> messagesA, messagesB;
	dice::hfe::game::SoldierInput botInput;
	const auto step = [&] {
		a.update();
		b.update();
		round.setInput(bot, botInput);
		hosted.tick();
		round.tick(1.0f / 30);
		for (const net::PostRemoteInfo& m : a.world().takeMessages()) {
			messagesA.push_back(m);
		}
		for (const net::PostRemoteInfo& m : b.world().takeMessages()) {
			messagesB.push_back(m);
		}
	};
	CHECK(pumpUntil(step, [&] {
		return a.world().gameState() == 1 && b.world().gameState() == 1 && a.world().localPlayer() >= 0
			&& b.world().localPlayer() >= 0;
	}, 6000));
	const int idA = a.world().localPlayer(), idB = b.world().localPlayer();
	a.selectTeam(2);
	b.selectTeam(2);
	CHECK(pumpUntil(step, [&] {
		dice::hfe::game::GameWorld::PlayerInfo pa, pb;
		return round.playerInfo(idA, pa) && pa.team == 2 && round.playerInfo(idB, pb) && pb.team == 2;
	}));
	const auto botOf = [&](const net::GameClient& c) -> const net::RemotePlayer* { return c.world().player(bot); };
	const auto botSoldier = [&](const net::GameClient& c) -> const net::RemoteObject* {
		const net::RemotePlayer* p = botOf(c);
		return p && p->soldierGhost ? c.world().object(p->soldierGhost) : nullptr;
	};
	// The bot crouches and fires: A sees its pose and its fire state.
	botInput.pose = dice::hfe::game::SoldierPose::Crouch;
	botInput.fire = true;
	CHECK(pumpUntil(step, [&] {
		const net::RemoteObject* o = botSoldier(a);
		const net::RemotePlayer* p = botOf(a);
		return o && o->pose == 1 && o->requestedPose == 1 && p && p->fire[0].on && p->firePresses == 1;
	}));
	botInput.pose = dice::hfe::game::SoldierPose::Prone;
	botInput.fire = false;
	CHECK(pumpUntil(step, [&] {
		const net::RemoteObject* o = botSoldier(a);
		const net::RemotePlayer* p = botOf(a);
		return o && o->pose == 2 && p && !p->fire[0].on;
	}));
	// A second press between updates still counts (the presses' two bits).
	botInput.fire = true;
	step();
	botInput.fire = false;
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* p = botOf(a);
		return p && p->firePresses == 2;
	}));

	// A radio call (the game's slot 0xd8: Spotted with message 0x33): every
	// player of the team gets 2/0x33 with the caller's soldier (A has none
	// yet: 0).
	net::SpottedInfo call;
	call.message = 0x33;
	call.team = 2;
	a.sendEvent(net::makeSpotted(call));
	CHECK(pumpUntil(step, [&] {
		const auto has = [](const std::vector<net::PostRemoteInfo>& list) {
			return std::any_of(list.begin(), list.end(), [](const net::PostRemoteInfo& m) {
				net::SpottedInfo s;
				return m.category == 2 && m.id == 0x33 && net::parseSpottedPayload(m.payload, m.id, s) && s.team == 2;
			});
		};
		return has(messagesA) && has(messagesB);
	}));
	// The host's own report goes the same way, with the reporter's soldier.
	call.message = 0x34;
	hosted.spotted(call, bot);
	CHECK(pumpUntil(step, [&] {
		return std::any_of(messagesB.begin(), messagesB.end(), [&](const net::PostRemoteInfo& m) {
			net::SpottedInfo s;
			const net::RemotePlayer* p = botOf(b);
			return m.id == 0x34 && net::parseSpottedPayload(m.payload, m.id, s) && p && s.spotter == p->soldierGhost && s.spotter;
		});
	}));
	CHECK(hosted.playerOfGhost(botOf(b)->soldierGhost) == bot);

	// B leads squad 1 and invites A: A's client is asked (2/0x10, the squad);
	// A turns it down (Invite without the leader bit), then is invited
	// again and joins (JoinSquad).
	b.createSquad(1, "Alpha");
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* pb = b.world().player(idB);
		return pb && pb->squad == 1 && pb->squadLeader;
	}));
	b.invite(idA, 1);
	CHECK(pumpUntil(step, [&] {
		return std::any_of(messagesA.begin(), messagesA.end(), [](const net::PostRemoteInfo& m) {
			return m.category == 2 && m.id == 0x10 && m.payloadInt(0) == 1;
		});
	}));
	a.invite(idA, 1, false);
	a.joinSquad(1);
	CHECK(pumpUntil(step, [&] {
		dice::hfe::game::GameWorld::PlayerInfo pa;
		return round.playerInfo(idA, pa) && pa.squad == 1;
	}));
}

// Others' rounds and reloads on clients: a reload pressed on the server
// (c_PIReload in 0x004bda00 -> player vtable 0x1fc, 0x0059b330) sends the
// player's state once with mask 0x10000 (every group's fields), which the
// client counts as a reload (0x005981a0's +0x280, taken by 0x00590e10); a
// networked projectile (a GenericProjectile with a networkableInfo) is a
// ghost of its own from its firing until it goes, a bullet without one
// isn't (each client makes it from the firing).
TEST(NetHost_ReloadFlagAndProjectileGhosts)
{
	HostRange range;
	dice::hfe::game::GameWorld& round = *range.round;
	range.templates.registerType(4, "GenericFireArm");
	range.templates.registerType(5, "GenericProjectile");
	range.templates.registerType(6, "Kit");
	dice::hfe::world::ObjectTemplate* launcher = range.templates.create("GenericFireArm", "test_launcher");
	launcher->settings()["projectileTemplate"] = "test_grenade";
	launcher->settings()["velocity"] = "15";
	launcher->settings()["itemIndex"] = "3";
	launcher->settings()["fire.roundsPerMinute"] = "60";
	launcher->settings()["ammo.magSize"] = "4";
	launcher->settings()["ammo.reloadTime"] = "2";
	dice::hfe::world::ObjectTemplate* grenade = range.templates.create("GenericProjectile", "test_grenade");
	grenade->settings()["networkableInfo"] = "BasicInfo";
	grenade->settings()["timeToLive"] = "1.5";
	grenade->settings()["detonation.explosionRadius"] = "4";
	dice::hfe::world::ObjectTemplate* kit = range.templates.create("Kit", "test_grenadier_kit");
	kit->children().push_back({"test_launcher", {}, {}});
	host::HostSetup setup;
	setup.level = "Range";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(round, range.logic, range.templates, setup);
	CHECK(hosted.start());
	const int bot = round.addPlayer("Bot1", 2, true);
	round.setKit(bot, "test_grenadier_kit", "us_light_soldier");
	CHECK(round.spawnSoldier(bot));

	net::GameClient a;
	a.world().setGhostTemplates([&range](uint32_t id) {
		const dice::hfe::world::ObjectTemplate* t = range.templates.findById(id);
		return t ? net::ghostTemplateFrom(range.templates, *t) : net::GhostTemplateNode {};
	});
	net::GameClientOptions options;
	options.playerName = "Alpha";
	CHECK(a.connect("127.0.0.1", hosted.gamePort(), options));
	dice::hfe::game::SoldierInput botInput;
	const auto step = [&] {
		a.update();
		round.setInput(bot, botInput);
		hosted.tick();
		round.tick(1.0f / 30);
	};
	CHECK(pumpUntil(step, [&] { return a.world().gameState() == 1 && a.world().localPlayer() >= 0; }, 6000));
	const auto botOf = [&]() { return a.world().player(bot); };
	CHECK(pumpUntil(step, [&] { return botOf() && botOf()->soldierGhost != 0; }));
	const auto grenades = [&] {
		int n = 0;
		for (const net::RemoteObject& o : a.world().objectList()) {
			n += o.templateId == grenade->id();
		}
		return n;
	};
	// It fires: the grenade comes as a ghost, flying (its speed block).
	botInput.fire = true;
	CHECK(pumpUntil(step, [&] { return grenades() > 0; }));
	botInput.fire = false;
	CHECK(pumpUntil(step, [&] {
		for (const net::RemoteObject& o : a.world().objectList()) {
			if (o.templateId == grenade->id() && o.hasVelocity && o.velocity.z * o.velocity.z + o.velocity.x * o.velocity.x > 1) {
				return true;
			}
		}
		return false;
	}));
	// Its fuse out on the server: the ghost goes.
	CHECK(pumpUntil(step, [&] { return grenades() == 0; }, 6000));
	// A reload pressed: one state with mask 0x10000.
	const uint32_t before = botOf()->reloads;
	botInput.reload = true;
	step();
	botInput.reload = false;
	CHECK(pumpUntil(step, [&] { return botOf()->reloads == before + 1; }));
	for (int i = 0; i < 20; ++i) {
		step();
	}
	CHECK(botOf()->reloads == before + 1); // only once
}

// A whisper (chat channel 14 for the HUD manager's player, -1 in retail:
// 0x007d9e20) reaches nobody else; the control points' allowCaptureByTeam
// reaches the clients (masks 8 / 0x10, the takeable radio); a battlefield
// report (event 0x41) puts the enemy on the reporter's minimap
// (0x005088f0 -> the client's 0x00508590).
TEST(NetHost_WhisperTakeableAndRadar)
{
	HostRange range;
	dice::hfe::game::GameWorld& round = *range.round;
	for (const char* name : {"cp_pac", "cp_eu"}) {
		range.templates.find(name)->settings()["networkableInfo"] = "BasicInfo";
	}
	host::HostSetup setup;
	setup.level = "Range";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(round, range.logic, range.templates, setup);
	CHECK(hosted.start());
	const int bot = round.addPlayer("Bot1", 1, true);
	CHECK(round.spawnSoldier(bot));
	net::GameClient a, b;
	net::GameClientOptions options;
	for (net::GameClient* c : {&a, &b}) {
		c->world().setGhostTemplates([&range](uint32_t id) {
			const dice::hfe::world::ObjectTemplate* t = range.templates.findById(id);
			return t ? net::ghostTemplateFrom(range.templates, *t) : net::GhostTemplateNode {};
		});
	}
	options.playerName = "Alpha";
	CHECK(a.connect("127.0.0.1", hosted.gamePort(), options));
	options.playerName = "Bravo";
	CHECK(b.connect("127.0.0.1", hosted.gamePort(), options));
	std::vector<net::ChatLine> chatB;
	std::vector<net::RemoteWorld::TakeableSet> takeable;
	const auto step = [&] {
		a.update();
		b.update();
		hosted.tick();
		round.tick(1.0f / 30);
		for (const net::ChatLine& line : b.world().takeChat()) {
			chatB.push_back(line);
		}
		for (const auto& set : a.world().takeTakeable()) {
			takeable.push_back(set);
		}
	};
	CHECK(pumpUntil(step, [&] {
		return a.world().gameState() == 1 && b.world().gameState() == 1 && a.world().localPlayer() >= 0
			&& b.world().localPlayer() >= 0;
	}, 6000));
	const int idA = a.world().localPlayer();
	a.selectTeam(2);
	CHECK(pumpUntil(step, [&] {
		dice::hfe::game::GameWorld::PlayerInfo pa;
		return round.playerInfo(idA, pa) && pa.team == 2;
	}));
	// The whisper goes to no one; a team line still reaches B.
	a.whisper("psst");
	a.say("hello", net::kChatGlobal);
	CHECK(pumpUntil(step, [&] {
		return std::any_of(chatB.begin(), chatB.end(), [](const net::ChatLine& l) { return l.text == "hello"; });
	}));
	CHECK(std::none_of(chatB.begin(), chatB.end(), [](const net::ChatLine& l) { return l.text == "psst"; }));
	// The control points' first states tell both teams' allowCaptureByTeam;
	// a change on the host comes again.
	CHECK(pumpUntil(step, [&] { return takeable.size() >= 2; }));
	takeable.clear();
	dice::hfe::game::ControlPoint* cp = nullptr;
	for (const auto& object : range.objects.objects()) {
		if (!cp) {
			cp = round.controlPointOf(object->id());
		}
	}
	CHECK(cp != nullptr);
	if (cp) {
		cp->allowCapture[1] = false;
		CHECK(pumpUntil(step, [&] {
			return std::any_of(takeable.begin(), takeable.end(), [](const auto& t) { return t.team == 1 && !t.allowed; });
		}));
	}
	// A reports the bot: it is on A's minimap for the report's seconds, not B's.
	round.reportBattlefieldObjects(idA, {bot}, 3);
	CHECK(pumpUntil(step, [&] {
		const std::vector<int> seen = a.world().radarPlayers();
		return std::find(seen.begin(), seen.end(), bot) != seen.end();
	}));
	CHECK(b.world().radarReports() == 0);
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

// A joined client's body of a ghost vehicle (its round's physics, asleep
// on a client) is put where the ghost is (GameWorld::placeRemoteBody, as
// 0x005ffec0 sets the root's physics node): the seat guns' rounds start at
// the vehicle's muzzle there, not where the client first saw the vehicle
// (a walker driven 45 m fired from its spawn). Needs $BF2142_GAME_DIR.
TEST(Host_RemoteSeatGunsFireFromTheGhostsPlace)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	namespace game = dice::hfe::game;
	namespace physics = dice::hfe::physics;
	HostGarage garage(gameDir, "us_heavy_mech", {0, 0, 20});
	CHECK(garage.round != nullptr);
	if (!garage.round) {
		return;
	}
	game::GameWorld& round = *garage.round;
	round.tick(1 / 30.0f); // its body
	const std::vector<game::GameWorld::SeatLayout> seats = round.vehicleSeats(garage.vehicle);
	int armed = -1;
	for (size_t i = 0; i < seats.size() && armed < 0; ++i) {
		armed = seats[i].weapons.empty() ? -1 : static_cast<int>(i);
	}
	CHECK(armed >= 0);
	dice::hfe::world::Object* walker = garage.objects.find(garage.vehicle);
	const physics::Vec3 there {60, 0, 90};
	walker->setPosition({there.x, there.y, there.z});
	round.placeRemoteBody(garage.vehicle, there, physics::Mat3 {}, {});
	dice::hfe::client::RemoteSeatGuns guns;
	dice::hfe::client::RemoteSeatGuns::Seat seat;
	seat.player = 1;
	seat.vehicle = garage.vehicle;
	seat.seat = armed;
	seat.firing[0] = seat.firing[1] = seat.firing[2] = true;
	for (int i = 0; i < 10; ++i) {
		guns.step(round, {seat}, 1 / 30.0f);
	}
	int near = 0, far = 0;
	for (const game::Projectile& p : round.projectiles()) {
		if (p.visual && p.shooter == garage.vehicle) {
			(std::hypot(p.origin.x - there.x, p.origin.z - there.z) < 10 ? near : far) += 1;
		}
	}
	CHECK(near > 0 && far == 0);
}

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

// A joined client's soldier standing and walking on a vehicle's roof: the
// prediction meets the moving objects near it (GameWorld::soldierMovers,
// the soldier broad phase 0x006dcce0) as the server's soldier does, so it
// stays on the roof rather than falling through it between the records
// (before: corrections on nearly every record). Needs $BF2142_GAME_DIR.
TEST(NetHost_PredictionOnAVehicleRoof)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		std::printf("  (BF2142_GAME_DIR not set: skipped)\n");
		return;
	}
	HostGarage garage(gameDir, "eu_apc", {0, 0, 12});
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
	options.playerName = "Rider";
	client.world().setGhostTemplates([&garage](uint32_t id) {
		const dice::hfe::world::ObjectTemplate* t = garage.templates.findById(id);
		return t ? net::ghostTemplateFrom(garage.templates, *t) : net::GhostTemplateNode {};
	});
	host::SoldierPrediction soldier(round);
	client.onMoveSent = [&](const net::PlayerMove& move) { soldier.step(move); };
	client.sendOnTicks = true;
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
	dice::hfe::game::SoldierInput walk;
	uint32_t records = 0;
	const auto step = [&] {
		client.setInput(net::playerInputFrom(walk));
		client.update();
		const net::RemoteWorld& world = client.world();
		if (world.controlRecords() != records) {
			records = world.controlRecords();
			const net::ControlState* state = world.controlState();
			const net::ControlObjectRecord& record = world.controlObject();
			if (state && !state->groups.empty() && state->groups.front().kind == net::NetworkableKind::Soldier) {
				if (!soldier.active()) {
					soldier.start(garage.templates.find("us_light_soldier"), record.lastMove, state->groups.front(),
						record.position);
				} else {
					soldier.reconcile(record.lastMove, state->groups.front(), record.position);
				}
			}
		}
		client.simulatedTick();
		hosted.tick();
		round.tick(1.0f / 30);
		std::this_thread::sleep_for(std::chrono::milliseconds(33));
	};
	CHECK(pumpUntil(step, [&] { return client.world().gameState() == 1 && client.world().localPlayer() >= 0; }, 8000));
	const int me = client.world().localPlayer();
	client.selectTeam(2);
	dice::hfe::game::GameWorld::PlayerInfo info;
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && info.team == 2; }));
	client.selectSpawnGroup(-1);
	CHECK(pumpUntil(step, [&] { return round.playerInfo(me, info) && info.alive; }, 6000));
	soldier.setOwner(2, info.soldier);
	// Dropped onto the roof; it settles there.
	const dice::hfe::world::Object* apc = garage.objects.find(garage.vehicle);
	round.setSoldierPosition(info.soldier, {apc->position().x, apc->position().y + 4.0f, apc->position().z});
	for (int i = 0; i < 60; ++i) {
		step();
	}
	CHECK(pumpUntil(step, [&] { return soldier.active(); }, 4000));
	const host::SoldierPrediction::Stats before = soldier.stats();
	// Standing two seconds, then a slow walk along the roof and back.
	for (int i = 0; i < 30 * 4; ++i) {
		walk = {};
		walk.throttle = i < 60 ? 0.0f : i < 90 ? 0.4f : -0.4f;
		step();
	}
	const host::SoldierPrediction::Stats& st = soldier.stats();
	const uint32_t taken = st.records - before.records;
	const double mean = taken ? (st.sumCorrection - before.sumCorrection) / taken : 0.0;
	round.playerInfo(me, info);
	std::printf("  on the roof (%.2f m up): %u records, correction mean %.4f m (%u over 1 cm)\n", info.position.y, taken,
		mean, st.corrections - before.corrections);
	CHECK(info.alive && info.position.y > 1.5f); // on the roof, not beside the vehicle
	CHECK(taken > 60);
	CHECK(mean < 0.01);
	CHECK(st.corrections - before.corrections < taken / 10);
	client.disconnect();
}

// The heavy IFV's repair cells on a joined client: the root's state carries
// its armored children (mask 0x800000, the root's list in post-order,
// armor.fullChildSync eighths), the host the cells' eighths among them
// (GameWorld::repairCellEighths), and the client's vehicle HUD shows the cells
// with hudRepairCellNo (0x007a1e50) from it: all up, then all down as the
// hull is wrecked. Needs $BF2142_GAME_DIR.
TEST(NetHost_JoinedRepairCells)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		std::printf("  (BF2142_GAME_DIR not set: skipped)\n");
		return;
	}
	HostGarage garage(gameDir, "eu_ifv", {0, 0, 12});
	if (!garage.round) {
		return; // no heavy IFV in this install
	}
	dice::hfe::game::GameWorld& round = *garage.round;
	const dice::hfe::world::ObjectTemplate* ifv = garage.templates.find("eu_ifv");
	const net::GhostTemplateNode node = net::ghostTemplateFrom(garage.templates, *ifv);
	const std::vector<bool>& cells = node.hints.armoredRepairCells;
	CHECK(cells.size() == node.hints.armoredChildren.size() && std::count(cells.begin(), cells.end(), true) == 7);
	host::HostSetup setup;
	setup.level = "Garage";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(round, garage.logic, garage.templates, setup);
	CHECK(hosted.start());
	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Gunner";
	client.world().setGhostTemplates([&garage](uint32_t id) {
		const dice::hfe::world::ObjectTemplate* t = garage.templates.findById(id);
		return t ? net::ghostTemplateFrom(garage.templates, *t) : net::GhostTemplateNode {};
	});
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
	const auto step = [&] {
		client.update();
		client.simulatedTick();
		hosted.tick();
		round.tick(1.0f / 30);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	};
	const auto seen = [&]() -> std::vector<bool> {
		for (const net::RemoteObject& o : client.world().objectList()) {
			if (o.templateId == ifv->id() && o.part == 0) {
				return o.repairCells;
			}
		}
		return {};
	};
	const auto all = [](const std::vector<bool>& v, bool up) {
		return v.size() == 7 && std::all_of(v.begin(), v.end(), [up](bool b) { return b == up; });
	};
	CHECK(pumpUntil(step, [&] { return all(seen(), true); }, 8000));
	// The HUD's view of it (RemoteHudWorld, the ghost as the vehicle's
	// object; here the host's own object stands in for the client's): the
	// cells, and the gunner's turret turn from its bundle ghost.
	std::map<uint16_t, uint32_t> ghostObjects;
	for (const net::RemoteObject& o : client.world().objectList()) {
		if (o.templateId == ifv->id() && o.part == 0) {
			ghostObjects[o.ghost] = garage.vehicle;
		}
	}
	host::RemoteHudWorld hudWorld(client.world(), round, ghostObjects);
	CHECK(all(hudWorld.repairCells(garage.vehicle), true));
	float yaw = 99, pitch = 99;
	const auto seats = round.vehicleSeats(garage.vehicle);
	bool turret = false;
	for (size_t i = 0; i < seats.size(); ++i) {
		if (!seats[i].yawBundle.empty() && !seats[i].weapon.empty()) {
			turret = true;
			CHECK(hudWorld.gunTurn(garage.vehicle, static_cast<int>(i), yaw, pitch) && std::fabs(yaw - seats[i].frameYaw) < 1
				&& std::fabs(pitch) < 1);
			CHECK(hudWorld.seatYaw(garage.vehicle, static_cast<int>(i), yaw) && std::fabs(yaw) < 1);
			break;
		}
	}
	CHECK(turret);
	// The cells in eighths (0x005ffec0's capture): the hull at 90 %, each
	// cell mirrors it at 99 of 110, seven eighths, which the client sets
	// its cells to (GameWorld::setRemoteRepairCells) for their effects.
	const auto eighths = [&]() -> std::vector<int> {
		for (const net::RemoteObject& o : client.world().objectList()) {
			if (o.templateId == ifv->id() && o.part == 0) {
				return o.repairCellEighths;
			}
		}
		return {};
	};
	// The dirty masks (0x005800e0, 0x0057f420): the first state carries
	// every field, a later one only what changed. The hull's hit points
	// alone go in fewer bits than the whole state did.
	const auto rootState = [&]() -> std::pair<uint32_t, size_t> {
		for (const net::RemoteObject& o : client.world().objectList()) {
			if (o.templateId == ifv->id() && o.part == 0) {
				return {o.updates, o.stateBits};
			}
		}
		return {0, 0};
	};
	size_t wholeBits = rootState().second;
	for (int i = 0; i < 30; ++i) {
		step(); // standing: the masks stay clear
	}
	const auto standing = rootState();
	wholeBits = std::max(wholeBits, standing.second);
	for (int i = 0; i < 30; ++i) {
		step();
	}
	CHECK(rootState().first <= standing.first + 1);
	round.setHitPoints(garage.vehicle, round.hitPoints(garage.vehicle) * 0.9f);
	CHECK(pumpUntil(step, [&] { return eighths() == std::vector<int>(7, 7); }, 6000));
	CHECK(rootState().second > 0 && rootState().second < wholeBits);
	round.applyDamage(garage.vehicle, 5000);
	CHECK(pumpUntil(step, [&] { return all(seen(), false); }, 6000));
	CHECK(all(round.repairCells(garage.vehicle), false));
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

// A joined client's combat-area countdown (0x0044c7b0 on the client): the
// server's time outside comes in the player networkable's 0x800 part as
// whole seconds (0x00592410 / 0x599209: RemotePlayer::outsideSeconds), and
// the HUD shows the level's timer less it (RemoteHudWorld::
// combatAreaTimeLeft); back inside, nothing.
TEST(NetHost_JoinedCombatAreaCountdown)
{
	using namespace dice::hfe;
	HostRange range;
	game::GameWorld& round = *range.round;
	game::CombatAreas areas;
	areas.use = true;
	game::CombatArea far; // a box well away from both spawns
	far.minX = 500, far.minZ = 500, far.maxX = 600, far.maxZ = 600;
	areas.areas.push_back(far);
	round.setCombatAreas(&areas);
	host::HostSetup setup;
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(round, range.logic, range.templates, setup);
	CHECK(hosted.start());
	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Outsider";
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
	const auto step = [&] {
		client.update();
		hosted.tick();
		round.tick(1.0f / 30);
	};
	CHECK(pumpUntil(step, [&] { return client.world().gameState() == 1 && client.world().localPlayer() >= 0; }));
	const int me = client.world().localPlayer();
	CHECK(round.spawnSoldier(me));
	// The client's round has the level's areas too (the same .con).
	world::ObjectManager clientObjects;
	game::GameWorld clientRound(range.templates, clientObjects);
	clientRound.setCombatAreas(&areas);
	std::map<uint16_t, uint32_t> ghostObjects;
	host::RemoteHudWorld hud(client.world(), clientRound, ghostObjects);
	CHECK(hud.combatAreaTimeLeft() < 0);
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* p = client.world().player(me);
		return p && p->alive && p->outsideSeconds >= 2;
	}, 8000));
	const net::RemotePlayer* p = client.world().player(me);
	CHECK(p && hud.combatAreaTimeLeft() == areas.timeAllowedOutside - static_cast<float>(p->outsideSeconds));
	CHECK(hud.combatAreaTimeLeft() > 0 && hud.combatAreaTimeLeft() <= 13);
	// Inside again: the time goes back to 0 and the countdown with it.
	areas.areas[0].minX = areas.areas[0].minZ = -1000;
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* q = client.world().player(me);
		return q && q->outsideSeconds == 0;
	}, 8000));
	CHECK(hud.combatAreaTimeLeft() < 0);
	client.disconnect();
}

// What Radio.sendSpottedMessage says (0x007c8160 -> slot 0xd0, 0x0042e9d0):
// the spotted object's radio.spottedMessage (a soldier's kit's, quotes
// off), "spotted" without one or with nothing spotted; a joined client's
// own pick of the crosshair's enemy agrees with the server's.
TEST(GameWorld_SpottedMessageVoice)
{
	using namespace dice::hfe;
	HostRange range;
	game::GameWorld& round = *range.round;
	const int a = round.addPlayer("A", 1, false);
	const int b = round.addPlayer("B", 2, false);
	CHECK(round.spawnSoldier(a) && round.spawnSoldier(b));
	round.tick(1.0f / 30);
	game::GameWorld::PlayerInfo me, enemy;
	round.playerInfo(a, me);
	round.playerInfo(b, enemy);
	const float yaw = me.yaw * 3.14159265f / 180.0f;
	const physics::Vec3 ahead {std::sin(yaw), 0, std::cos(yaw)};
	round.setSoldierPosition(enemy.soldier, me.position + ahead * 30.0f);
	round.tick(1.0f / 30);
	round.playerInfo(a, me);
	const host::LocalHudWorld local(round, range.logic);
	CHECK(menu::spotChoice(local, round, -1).voice == "spotted" && menu::spotChoice(local, round, -1).player == -1);
	menu::SpotChoice choice = menu::spotChoice(local, round, round.crosshairTarget(a));
	CHECK(choice.player == b && choice.object == enemy.soldier && choice.voice == "spotted");
	// The kit's Radio component names it.
	world::ObjectTemplate* soldier = range.templates.find("pac_light_soldier");
	CHECK(soldier != nullptr);
	soldier->settings()["Radio.spottedMessage"] = "\"sniper_spotted\"";
	CHECK(menu::spottedMessage(soldier) == "sniper_spotted");
	range.templates.find("us_light_soldier")->settings()["Radio.spottedMessage"] = "sniper_spotted";
	CHECK(menu::spotChoice(local, round, b).voice == "sniper_spotted");
	// The client-side pick: the same enemy along the same view.
	const float eye[3] = {me.position.x, me.position.y + 1.6f, me.position.z};
	const float pitch = me.aimPitch * 3.14159265f / 180.0f;
	const float dir[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
	CHECK(menu::crosshairEnemy(local, round, a, eye, dir) == b);
	CHECK(menu::crosshairEnemy(local, round, a, eye, dir, 10) == -1);
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

namespace {

// A tiny mod for the content check: two archive lists, the two .con files
// the lists' digest takes, three archives (one over ten 16 KB blocks), a
// level with its two archives, and the .md5 files generate_md5.py would
// write for them.
void writeFile(const std::filesystem::path& path, const std::string& data)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << data;
}

std::string noise(size_t size, uint32_t seed)
{
	std::string out(size, '\0');
	for (char& c : out) {
		seed = seed * 1103515245u + 12345u;
		c = static_cast<char>(seed >> 16);
	}
	return out;
}

void writeContentMod(const std::filesystem::path& root)
{
	const std::filesystem::path mod = root / "mods" / "bf2142";
	writeFile(mod / "ClientArchives.con", "fileManager.mountArchive Common_client.zip Common\n"
										  "fileManager.mountArchive Objects/Weapons_client.zip Objects\\Weapons\n");
	writeFile(mod / "ServerArchives.con", "fileManager.mountArchive Common_server.zip Common\n");
	writeFile(mod / "Init.con", "run GameLogicInit.con\n");
	writeFile(mod / "GameLogicInit.con", "gameLogic.setTeamName 1 \"EU\"\n");
	writeFile(mod / "Common_client.zip", noise(300000, 1));
	writeFile(mod / "Objects" / "Weapons_client.zip", noise(40000, 2));
	writeFile(mod / "Common_server.zip", noise(5000, 3));
	writeFile(mod / "Levels" / "Test_Level" / "client.zip", noise(70000, 4));
	writeFile(mod / "Levels" / "Test_Level" / "server.zip", noise(20000, 5));
	std::string common, level;
	for (const std::string& line : net::stripedFingerprints(root.string(), net::commonArchives(mod.string()))) {
		common += line + "\n";
	}
	for (const std::string& line :
		net::stripedFingerprints(root.string(), net::levelArchives("levels/Test_Level/"))) {
		level += "test_level " + line + "\n";
	}
	writeFile(mod / "std_archive.md5", common);
	writeFile(mod / "Levels" / "Test_Level" / "archive.md5", level);
}

} // namespace

// The pure-server check end to end (0x004aec40, 0x005123f0, 0x004af710):
// a client with the server's content plays; one whose archive differs by
// a byte, and one that sends no fingerprint, are kicked with 0x1b.
TEST(NetHost_PureServerContentCheck)
{
	namespace fs = std::filesystem;
	const fs::path base = fs::temp_directory_path() / ("bf2142_content_" + std::to_string(std::rand()));
	const fs::path good = base / "good", modified = base / "modified";
	writeContentMod(good);
	writeContentMod(modified);
	// The fingerprints aren't regenerated for the copy: its archive changed after.
	{
		std::fstream f(modified / "mods" / "bf2142" / "Common_client.zip", std::ios::in | std::ios::out | std::ios::binary);
		f.seekp(0);
		f.put('X');
	}
	net::ContentChecker checker;
	CHECK(checker.loadCommon(good.string()) && checker.pure());
	CHECK(checker.loadLevel(good.string(), "levels/Test_Level/") && checker.hasLevel());
	for (int ordinal : {0, 3, 9}) {
		CHECK(checker.matches(ordinal, net::computeContentDigests(good.string(), "mods/bf2142", "levels/Test_Level/", ordinal)));
	}
	// The byte is in the first block: only the ordinal reading block 0 sees it.
	CHECK(!checker.matches(0, net::computeContentDigests(modified.string(), "mods/bf2142", "levels/Test_Level/", 0)));
	CHECK(checker.matches(1, net::computeContentDigests(modified.string(), "mods/bf2142", "levels/Test_Level/", 1)));
	// The event round-trips its three digests.
	net::ContentDigests digests = net::computeContentDigests(good.string(), "mods/bf2142", "levels/Test_Level/", 4);
	dice::hfe::net::BitWriter out;
	CHECK(net::writeGameEvent(out, net::makeContentCheck(digests)));
	dice::hfe::net::BitReader in(out.data().data(), out.data().size());
	net::GameEvent read;
	net::ContentDigests back;
	CHECK(net::readGameEvent(in, read) && net::parseContentCheck(read, back));
	CHECK(back.lists == digests.lists && back.common == digests.common && back.level == digests.level);

	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::game::GameWorld round(templates, objects);
	dice::hfe::game::GameLogic logic;
	round.startRound();
	logic.startRound();
	host::HostSetup setup;
	setup.level = "Test_Level";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	setup.contentRoot = good.string();
	host::HostedGame hosted(round, logic, templates, setup);
	std::vector<std::string> log;
	hosted.onLog = [&log](const std::string& line) { log.push_back(line); };
	CHECK(hosted.start());

	const auto join = [&](const fs::path& root, bool& kicked, uint32_t& reason) {
		net::GameClient client;
		net::GameClientOptions options;
		options.playerName = "Joiner";
		options.contentRoot = root.string();
		CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
		const auto step = [&] {
			client.update();
			hosted.tick();
			round.tick(1.0f / 30);
		};
		pumpUntil(step, [&] {
			return client.state() == net::GameClient::State::Disconnected || client.world().roundStarted();
		});
		kicked = client.state() == net::GameClient::State::Disconnected;
		reason = client.disconnectReason();
		CHECK(client.world().settings().contentCheck);
		client.disconnect();
		pumpUntil(step, [&] { return hosted.clientCount() == 0; });
	};
	bool kicked = true;
	uint32_t reason = 0;
	join(good, kicked, reason);
	CHECK(!kicked);
	// The modified client: its archive differs in every block, so at any
	// ordinal the round drew.
	writeFile(modified / "mods" / "bf2142" / "Common_client.zip", noise(300000, 99));
	join(modified, kicked, reason);
	CHECK(kicked && reason == net::kKickModifiedContent);
	CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("for using modified data") != std::string::npos; }));
	join(fs::path(), kicked, reason); // no fingerprint at all
	CHECK(kicked && reason == net::kKickModifiedContent);
	std::error_code error;
	fs::remove_all(base, error);
}

// The striped fingerprints of the shipped content are the shipped
// std_archive.md5 and a level's archive.md5 (needs $BF2142_GAME_DIR).
TEST(Net_ContentFingerprintsMatchShippedMd5)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	net::ContentChecker checker;
	CHECK(checker.loadCommon(gameDir) && checker.loadLevel(gameDir, "levels/Suez_Canal/"));
	for (int ordinal : {0, 7}) {
		CHECK(checker.matches(ordinal, net::computeContentDigests(gameDir, "mods/bf2142", "levels/Suez_Canal/", ordinal)));
	}
}

// The strict diagnostics on a loopback session (NetDiagnostics.h): every
// datagram both ways logged and walked, every ghost state checked against
// its length with the client's layouts; our own host must give no problem.
// The session's capture then plays back offline into a fresh client, which
// ends with the same world and, again, no problem.
TEST(NetHost_StrictDiagnosticsAndCaptureReplay)
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

	const auto layouts = [&range](uint32_t id) {
		const dice::hfe::world::ObjectTemplate* t = range.templates.findById(id);
		return t ? net::ghostTemplateFrom(range.templates, *t) : net::GhostTemplateNode {};
	};
	const auto names = [&range](uint32_t id) {
		const dice::hfe::world::ObjectTemplate* t = range.templates.findById(id);
		return t ? t->name() : std::string();
	};
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("bf2142-netcap-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(dir);
	const std::string capture = (dir / "session.bf2142cap").string();

	size_t received = 0;
	{
		net::NetDiagnostics diagnostics;
		CHECK(diagnostics.openLog((dir / "session.log").string()));
		diagnostics.captureTo(capture);
		net::GameClient client;
		client.setDiagnostics(&diagnostics);
		client.world().setGhostTemplates(layouts);
		client.world().setTemplateResolver(names);
		net::GameClientOptions options;
		options.playerName = "Joiner";
		CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
		int frame = 0;
		const auto step = [&] {
			++frame;
			dice::hfe::game::SoldierInput walk;
			walk.throttle = 1;
			walk.turn = (frame / 30) % 2 ? 3.0f : 0.0f;
			client.setInput(net::playerInputFrom(walk));
			client.update();
			hosted.tick();
			round.tick(1.0f / 30);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		};
		CHECK(pumpUntil(step, [&] { return client.world().gameState() == 1 && client.world().localPlayer() >= 0; }, 6000));
		client.selectSpawnGroup(-1);
		CHECK(pumpUntil(step, [&] { return client.world().controlStates() > 20; }, 6000));
		client.say("strict");
		for (int i = 0; i < 30; ++i) {
			step();
		}
		received = client.world().controlStates();
		for (const std::string& p : diagnostics.problemList()) {
			std::printf("  %s\n", p.c_str());
		}
		std::printf("  %zu datagrams, %zu problems; %zu control states\n", diagnostics.datagrams(), diagnostics.problems(),
			received);
		CHECK(diagnostics.datagrams() > 40 && diagnostics.problems() == 0);
		client.disconnect();
		CHECK(diagnostics.saveCapture());
	}
	std::vector<net::CaptureRecord> records;
	CHECK(net::readCapture(capture, records) && records.size() > 40);
	CHECK(std::count_if(records.begin(), records.end(), [](const net::CaptureRecord& r) { return r.outgoing; }) > 10);

	// The capture into a fresh client, offline (nothing reaches the host).
	net::NetDiagnostics replayed;
	net::GameClient again;
	again.setDiagnostics(&replayed);
	again.world().setGhostTemplates(layouts);
	again.world().setTemplateResolver(names);
	again.playCapture(records, false);
	size_t updates = 0;
	while (again.update() && ++updates < 100000) {
	}
	CHECK(replayed.problems() == 0 && replayed.datagrams() > 40);
	CHECK(again.world().controlStates() == received);
	CHECK(again.world().localPlayer() >= 0 && again.world().roundStarted());
	if (!std::getenv("BF2142_KEEP_NETCAP")) {
		std::filesystem::remove_all(dir);
	}
}

// LoadComplete after the level has loaded (0x004b5eb0 case 2), and the
// loading keep-alive (the exe's NetClientThread): while the game "loads"
// without calling update(), pings are answered and the rest waits; then
// LoadComplete goes and the join goes on.
TEST(NetHost_LoadCompleteAfterLoadAndKeepAlive)
{
	HostRange range;
	host::HostSetup setup;
	setup.level = "Range";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(*range.round, range.logic, range.templates, setup);
	CHECK(hosted.start());
	net::GameClient client;
	client.answerLoadAfterLevel = true;
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), {}));
	const auto host = [&] {
		hosted.tick();
		range.round->tick(1.0f / 30);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	};
	CHECK(pumpUntil([&] { client.update(); host(); }, [&] { return client.world().hasLevel(); }));
	client.loadingLevel();
	for (int i = 0; i < 100; ++i) { // a second of "loading"
		host();
	}
	CHECK(hosted.clientCount() == 1 && client.world().localPlayer() < 0);
	client.levelLoaded();
	CHECK(pumpUntil([&] { client.update(); host(); },
		[&] { return client.world().localPlayer() >= 0 && client.world().roundStarted(); }, 6000));
	CHECK(client.state() == net::GameClient::State::Connected);
}

// Joining as a spectator (the menu's SPECTATOR, Logic.readyToPlay(true)):
// JoinAsSpectator (0x47) kind 8 goes up; a server that isn't ranked and
// has sv.allowSpectators (0x005142a0) marks the player and sends the event
// on to every client (0x00514260); later joiners have it in CreatePlayer
// (0x0050a7c0's +0x40). The spectator's camera changes (kind 6 with the
// player followed) set its camera on the server. Without
// sv.allowSpectators nothing changes.
TEST(NetHost_JoinAsSpectator)
{
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::game::GameWorld round(templates, objects);
	dice::hfe::game::GameLogic logic;
	round.startRound();
	logic.startRound();
	net::ServerSettings sv;
	host::HostSetup setup;
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	setup.settings = &sv;
	host::HostedGame hosted(round, logic, templates, setup);
	CHECK(hosted.start());
	const int bot = round.addPlayer("Bot1", 1, true);
	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Watcher";
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
	const auto step = [&] {
		client.update();
		hosted.tick();
		round.tick(1.0f / 30);
	};
	CHECK(pumpUntil(step, [&] { return client.world().gameState() == 1 && client.world().localPlayer() >= 0; }));
	const int me = client.world().localPlayer();
	dice::hfe::game::GameWorld::PlayerInfo info;
	// Refused: sv.allowSpectators is off by default.
	client.joinAsSpectator();
	for (int i = 0; i < 40; ++i) {
		step();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	CHECK(round.playerInfo(me, info) && !info.spectator);
	CHECK(client.world().player(me) && !client.world().player(me)->spectator);
	// Allowed (and not ranked).
	sv.allowSpectators = true;
	client.joinAsSpectator();
	CHECK(pumpUntil(step, [&] { return client.world().player(me) && client.world().player(me)->spectator; }));
	CHECK(round.playerInfo(me, info) && info.spectator && !info.alive);
	// The camera: following the bot (FreeCam.togglePlayer's kind 6).
	client.spectatorCamera(net::JoinAsSpectatorInfo::kCameraFollow, bot);
	int mode = 0, follow = -1;
	CHECK(pumpUntil(step, [&] { return hosted.spectatorCamera(me, mode, follow) && mode == 6; }));
	CHECK(mode == 6 && follow == bot);
	CHECK(round.playerInfo(me, info) && info.spectator); // not a join: the flag stays
	// A second client hears of the spectator in its CreatePlayer.
	net::GameClient second;
	options.playerName = "Later";
	CHECK(second.connect("127.0.0.1", hosted.gamePort(), options));
	const auto both = [&] {
		client.update();
		second.update();
		hosted.tick();
		round.tick(1.0f / 30);
	};
	CHECK(pumpUntil(both, [&] {
		const net::RemotePlayer* p = second.world().player(me);
		return second.world().localPlayer() >= 0 && p && p->spectator;
	}));
	const net::RemotePlayer* other = second.world().player(bot);
	CHECK(other && !other->spectator);
	// A ranked server refuses (0x00430a00).
	sv.ranked = true;
	second.joinAsSpectator();
	for (int i = 0; i < 40; ++i) {
		both();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	CHECK(round.playerInfo(second.world().localPlayer(), info) && !info.spectator);
	// The wire: 8 + 1 + 8 + 8 bits after the type.
	net::JoinAsSpectatorInfo j;
	j.player = 5;
	j.follow = -1;
	const net::GameEvent e = net::makeJoinAsSpectator(j);
	net::BitWriter w;
	CHECK(net::writeGameEvent(w, e));
	net::BitReader r(w.data().data(), w.data().size());
	r.limit(w.bits());
	net::GameEvent back;
	CHECK(net::readGameEvent(r, back));
	net::JoinAsSpectatorInfo parsed;
	CHECK(net::parseJoinAsSpectator(back, parsed) && parsed.player == 5 && parsed.spectator && parsed.follow == -1
		&& parsed.kind == 8);
	second.disconnect();
	client.disconnect();
}

// A sticky round stuck on the server (StickyCollisionComp 0x005e81a0)
// reaches the clients as the StickyProjectile event (0x20: built
// 0x0050f850, written 0x0050f460, read 0x0050f720): its ghost, no parent
// (the ground), the thrower and its world matrix, up along the ground;
// the event's fields survive the wire in the exe's order, a parent's two
// bytes only with a parent.
TEST(NetHost_StickyProjectileEvent)
{
	// The event alone: with a parent and without.
	net::StickyProjectileInfo info;
	info.projectile = 41;
	info.target = 7;
	info.part = 0;
	info.bundle = 2;
	info.player = 3;
	info.rows = {net::Vec3f {1, 0, 0}, net::Vec3f {0, 1, 0}, net::Vec3f {0, 0, 1}, net::Vec3f {0.5f, 1.25f, -2}};
	net::GameEvent e = net::makeStickyProjectile(info);
	CHECK(e.values.size() == 17 && e.u(4) == 3); // 16, 16, 8, 8, 8, then the floats
	CHECK(e.f(5) == 0 && e.f(7) == 1);              // +0x2c first: the third row
	net::StickyProjectileInfo back;
	CHECK(net::parseStickyProjectile(e, back) && back.projectile == 41 && back.target == 7 && back.bundle == 2
		&& back.player == 3 && back.rows[3].y == 1.25f && back.rows[2].z == 1 && back.rows[0].x == 1);
	info.target = 0;
	info.player = -1;
	e = net::makeStickyProjectile(info);
	CHECK(e.values.size() == 15 && e.u(2) == 0xff);
	CHECK(net::parseStickyProjectile(e, back) && back.target == 0 && back.player == -1 && back.rows[3].z == -2);

	HostRange range;
	dice::hfe::game::GameWorld& round = *range.round;
	range.templates.registerType(4, "GenericFireArm");
	range.templates.registerType(5, "GenericProjectile");
	range.templates.registerType(6, "Kit");
	dice::hfe::world::ObjectTemplate* launcher = range.templates.create("GenericFireArm", "test_c4_thrower");
	launcher->settings()["projectileTemplate"] = "test_c4";
	launcher->settings()["velocity"] = "6";
	launcher->settings()["itemIndex"] = "3";
	launcher->settings()["fire.roundsPerMinute"] = "60";
	launcher->settings()["ammo.magSize"] = "4";
	dice::hfe::world::ObjectTemplate* c4 = range.templates.create("GenericProjectile", "test_c4");
	c4->settings()["networkableInfo"] = "BasicInfo";
	c4->settings()["timeToLive"] = "60";
	c4->settings()["collision.bouncing"] = "1";
	c4->components().push_back("StickyCollisionComp");
	dice::hfe::world::ObjectTemplate* kit = range.templates.create("Kit", "test_c4_kit");
	kit->children().push_back({"test_c4_thrower", {}, {}});
	host::HostSetup setup;
	setup.level = "Range";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(round, range.logic, range.templates, setup);
	CHECK(hosted.start());
	const int bot = round.addPlayer("Bot1", 2, true);
	round.setKit(bot, "test_c4_kit", "us_light_soldier");
	CHECK(round.spawnSoldier(bot));
	net::GameClient a;
	a.world().setGhostTemplates([&range](uint32_t id) {
		const dice::hfe::world::ObjectTemplate* t = range.templates.findById(id);
		return t ? net::ghostTemplateFrom(range.templates, *t) : net::GhostTemplateNode {};
	});
	net::GameClientOptions options;
	options.playerName = "Alpha";
	CHECK(a.connect("127.0.0.1", hosted.gamePort(), options));
	dice::hfe::game::SoldierInput botInput;
	const auto step = [&] {
		a.update();
		round.setInput(bot, botInput);
		hosted.tick();
		round.tick(1.0f / 30);
	};
	CHECK(pumpUntil(step, [&] { return a.world().gameState() == 1 && a.world().localPlayer() >= 0; }, 6000));
	CHECK(pumpUntil(step, [&] { return a.world().player(bot) && a.world().player(bot)->soldierGhost != 0; }));
	botInput.fire = true;
	const auto c4Ghost = [&]() -> uint16_t {
		for (const net::RemoteObject& o : a.world().objectList()) {
			if (o.templateId == c4->id()) {
				return o.ghost;
			}
		}
		return 0;
	};
	CHECK(pumpUntil(step, [&] { return c4Ghost() != 0; }));
	botInput.fire = false;
	// It lands and sticks: the event comes.
	CHECK(pumpUntil(step, [&] { return a.world().stuckProjectile(c4Ghost()) != nullptr; }, 3000));
	const net::StickyProjectileInfo* stuck = a.world().stuckProjectile(c4Ghost());
	const dice::hfe::game::Projectile* served = nullptr;
	for (const dice::hfe::game::Projectile& p : round.projectiles()) {
		if (p.alive && p.stuck) {
			served = &p;
		}
	}
	CHECK(served && stuck->target == 0 && stuck->player == bot);
	CHECK(std::fabs(stuck->rows[3].x - served->position.x) < 1e-3f && std::fabs(stuck->rows[3].y - served->position.y) < 1e-3f
		&& std::fabs(stuck->rows[3].z - served->position.z) < 1e-3f);
	CHECK(std::fabs(stuck->rows[1].x - served->stuckNormal.x) < 1e-4f && std::fabs(stuck->rows[1].y - served->stuckNormal.y) < 1e-4f);
	// The rows are a rotation: unit and square to each other.
	const auto dot = [](const net::Vec3f& u, const net::Vec3f& v) { return u.x * v.x + u.y * v.y + u.z * v.z; };
	CHECK(std::fabs(dot(stuck->rows[0], stuck->rows[0]) - 1) < 1e-3f && std::fabs(dot(stuck->rows[0], stuck->rows[1])) < 1e-3f
		&& std::fabs(dot(stuck->rows[1], stuck->rows[2])) < 1e-3f);
}

// The squad report's reading by its recipients (0x005088f0): the longest
// networkableHudRevealTime, and a cloak report (kind 2) only as kind 1 to
// a squad with a networkableHudShowCloaked reader.
TEST(Host_BattlefieldReportReading)
{
	int kind = 0, seconds = 3;
	CHECK(host::battlefieldReportFor({{3, false}, {6, false}}, kind, seconds));
	CHECK(kind == 0 && seconds == 6);
	kind = 2;
	seconds = 3;
	CHECK(!host::battlefieldReportFor({{3, false}, {3, false}}, kind, seconds));
	kind = 2;
	CHECK(host::battlefieldReportFor({{3, false}, {3, true}}, kind, seconds));
	CHECK(kind == 1);
	kind = 0;
	seconds = 10; // a drone's pulse keeps its own when longer
	CHECK(host::battlefieldReportFor({{6, true}}, kind, seconds));
	CHECK(kind == 0 && seconds == 10);
}

// A joined client's seat guns (RemoteGuns.h): the others' players' fire
// states (0x00590e10 -> 0x004cc7a0) fire the seat's active weapon of each
// group at its rate of fire; each round is its Fire sound and the fire's
// event 0 to the weapon's children (FireComp 0x005f31d0: the muzzle
// bundles, the GroundEffectBundles), on the vehicle's object.
TEST(Host_RemoteSeatGunsFireFromTheFireState)
{
	namespace world = dice::hfe::world;
	namespace game = dice::hfe::game;
	namespace client = dice::hfe::client;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::ObjectTemplate* tank = templates.create("PlayerControlObject", "tank");
	tank->settings()["seatInformation"] = "tank 0/1/0 0/0/0";
	tank->children().push_back({"tank_gun", {0, 2, 1}, {}});
	tank->children().push_back({"tank_coax", {0.5f, 2, 1}, {}});
	world::ObjectTemplate* gun = templates.create("GenericFireArm", "tank_gun");
	gun->settings()["fire.roundsPerMinute"] = "60";
	gun->settings()["fire.addFireRate.list"] = "0";
	gun->settings()["ammo.magSize"] = "1";
	gun->settings()["ammo.reloadTime"] = "3";
	gun->settings()["ammo.autoReload"] = "1";
	world::ObjectTemplate* coax = templates.create("GenericFireArm", "tank_coax");
	coax->settings()["fire.fireInput"] = "PIAltFire";
	coax->settings()["fire.roundsPerMinute"] = "600";
	coax->settings()["fire.addFireRate.list"] = "2";
	coax->settings()["ammo.magSize"] = "100";
	const uint32_t id = objects.create(tank, {10, 0, 10})->id();
	game::GameWorld round(templates, objects);
	const auto seats = round.vehicleSeats(id);
	CHECK(seats.size() == 1);
	if (seats.size() != 1) {
		return;
	}
	const game::GameWorld::SeatWeapon* main = client::RemoteSeatGuns::groupWeapon(seats[0], 0, -1);
	const game::GameWorld::SeatWeapon* alt = client::RemoteSeatGuns::groupWeapon(seats[0], 1, 0);
	CHECK(main && main->weapon == "tank_gun" && alt && alt->weapon == "tank_coax");
	CHECK(!client::RemoteSeatGuns::groupWeapon(seats[0], 1, 1) && !client::RemoteSeatGuns::groupWeapon(seats[0], 2, -1));

	client::RemoteSeatGuns guns;
	client::RemoteSeatGuns::Seat seat;
	seat.player = 3;
	seat.vehicle = id;
	seat.seat = 0;
	const auto fires = [&round](const char* weapon) {
		int n = 0;
		for (const auto& [object, list] : round.takeObjectWeaponFires()) {
			n += static_cast<int>(std::count(list.begin(), list.end(), std::string(weapon)));
		}
		return n;
	};
	guns.step(round, {seat}, 1 / 30.0f);
	CHECK(fires("tank_gun") == 0 && round.takeWeaponSounds().empty());
	// The main gun: single fire, one round a press; its magazine of one
	// reloads by itself (3 s).
	seat.firing[0] = true;
	guns.step(round, {seat}, 1 / 30.0f);
	const auto sounds = round.takeWeaponSounds();
	CHECK(sounds.size() == 1 && sounds[0].kind == game::GameWorld::WeaponSoundEvent::Fire && sounds[0].player == 3
		&& sounds[0].object == id && sounds[0].weapon == "tank_gun");
	// At the muzzle: the gun's place and 1 m along it (no projectileStartPosition).
	CHECK(sounds.size() == 1 && std::fabs(sounds[0].position.y - 2) < 1e-4f && std::fabs(sounds[0].position.z - 12) < 1e-4f);
	CHECK(round.takeObjectWeaponFires()[id] == std::vector<std::string> {"tank_gun"});
	int rounds = 0;
	for (int i = 0; i < 30 * 5; ++i) {
		guns.step(round, {seat}, 1 / 30.0f); // held: no second press
		rounds += fires("tank_gun");
	}
	CHECK(rounds == 0);
	seat.firing[0] = false;
	guns.step(round, {seat}, 1 / 30.0f);
	seat.firing[0] = true;
	guns.step(round, {seat}, 1 / 30.0f);
	CHECK(fires("tank_gun") == 1); // reloaded by then
	// The coax: automatic while the alt fire is held, 10 a second.
	seat.firing[0] = false;
	seat.firing[1] = true;
	rounds = 0;
	for (int i = 0; i < 30; ++i) {
		guns.step(round, {seat}, 1 / 30.0f);
		rounds += fires("tank_coax");
	}
	CHECK(rounds == 10);
	// The server's rounds: an empty magazine fires nothing.
	seat.rounds[1] = 0;
	guns.step(round, {seat}, 1 / 3.0f);
	CHECK(fires("tank_coax") == 0);
	// A player not listed is forgotten.
	guns.step(round, {}, 1 / 30.0f);
	seat.rounds[1] = -1;
	guns.step(round, {seat}, 1 / 30.0f);
	CHECK(fires("tank_coax") == 1);
}

// The commander's strike on a joined client: the server's 0x005a0e50 sends
// kEvArtillery kind 2 (written 0x0050ed20, read 0x0050ec60) with the
// asset's ghost and the seed; the client's handler (0x0050eeb0) fires the
// asset's first firearm at (x, orbitalBombardmentHeight, z), its burst at
// its rate (client::RemoteStrikes), each round its effects there.
TEST(Host_StrikeFireEventAndRemoteStrikes)
{
	namespace world = dice::hfe::world;
	namespace client = dice::hfe::client;
	net::StrikeFireInfo info;
	info.x = 120.5f;
	info.z = -64;
	info.asset = 913;
	info.seed = 4321;
	net::BitWriter out;
	CHECK(net::writeGameEvent(out, net::makeStrikeFire(info)));
	net::BitReader in(out.data().data(), out.data().size());
	net::GameEvent read;
	net::StrikeFireInfo back;
	CHECK(net::readGameEvent(in, read) && net::parseStrikeFire(read, back));
	CHECK(back.x == 120.5f && back.z == -64 && back.asset == 913 && back.seed == 4321 && back.team == 0);
	// A client's use (kind 1) isn't one.
	CHECK(!net::parseStrikeFire(net::makeArtilleryUse(1, 0, 0, 4), back));
	net::CommanderToolUse use;
	CHECK(!net::parseCommanderToolUse(net::makeStrikeFire(info), use));
	net::RemoteWorld remote;
	CHECK(remote.apply(read));
	const auto strikes = remote.takeStrikeFires();
	CHECK(strikes.size() == 1 && strikes[0].asset == 913 && remote.takeStrikeFires().empty());

	world::ObjectTemplateManager templates;
	world::ObjectTemplate* asset = templates.create("PlayerControlObject", "test_artillery");
	asset->children().push_back({"test_base", {}, {}});
	world::ObjectTemplate* base = templates.create("Bundle", "test_base");
	base->children().push_back({"test_strike_arm", {}, {0, 90, 0}});
	world::ObjectTemplate* arm = templates.create("GenericFireArm", "test_strike_arm");
	arm->settings()["fire.burstSize"] = "3";
	arm->settings()["fire.roundsPerMinute"] = "60";
	CHECK(client::RemoteStrikes::firstFireArm(templates, *asset) == "test_strike_arm");
	client::RemoteStrikes rounds;
	rounds.fired(templates, 7, *asset, 120.5f, -64, 500);
	std::vector<client::RemoteStrikes::Round> due = rounds.step(1 / 30.0f);
	CHECK(due.size() == 1 && due[0].asset == 7 && due[0].fireArm == "test_strike_arm" && due[0].at.y == 500
		&& due[0].at.x == 120.5f && due[0].at.z == -64);
	int later = 0;
	for (int i = 0; i < 30 * 3; ++i) {
		later += static_cast<int>(rounds.step(1 / 30.0f).size());
	}
	CHECK(later == 2); // the burst's other two, a second apart
	// An asset without a firearm fires nothing.
	world::ObjectTemplate* bare = templates.create("PlayerControlObject", "test_bare");
	rounds.fired(templates, 8, *bare, 0, 0, 500);
	CHECK(rounds.step(1 / 30.0f).empty());
}

// The rounds of those guns on the client (GameWorld::fireRemoteSeatRound):
// FireComp's 0x005f2bb0 makes a round without networkable info on every
// machine, from the muzzle as the client's copy of the vehicle has it: the
// seat's yaw bundle turned as its RotationalBundle ghost says (0x00602070),
// projectileStartPosition along the turned gun, the vehicle's velocity
// added. A networked round (a networkableInfo) is the server's ghost.
TEST(Host_RemoteSeatGunsFireRoundsFromTheTurret)
{
	namespace world = dice::hfe::world;
	namespace game = dice::hfe::game;
	namespace client = dice::hfe::client;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::ObjectTemplate* tank = templates.create("PlayerControlObject", "tank");
	tank->settings()["seatInformation"] = "tank 0/1/0 0/0/0";
	tank->children().push_back({"tank_turret", {0, 2, 0}, {}});
	world::ObjectTemplate* turret = templates.create("RotationalBundle", "tank_turret");
	turret->settings()["inputToYaw"] = "PIMouseLookX";
	turret->children().push_back({"tank_cannon", {0, 0, 1}, {}});
	world::ObjectTemplate* gun = templates.create("GenericFireArm", "tank_cannon");
	gun->settings()["fire.roundsPerMinute"] = "60";
	gun->settings()["fire.addFireRate.list"] = "0";
	gun->settings()["fire.projectileStartPosition"] = "0/0/2";
	gun->settings()["projectileTemplate"] = "tank_shell";
	gun->settings()["velocity"] = "100";
	world::ObjectTemplate* shell = templates.create("GenericProjectile", "tank_shell");
	shell->settings()["timeToLive"] = "5";
	const uint32_t id = objects.create(tank, {10, 0, 10})->id();
	game::GameWorld round(templates, objects);
	float turned = 90;
	round.setRemotePartPose([&](uint32_t object, const std::string& part, world::Vec3& offset, world::Vec3& rotation) {
		if (object != id || part != "tank_turret") {
			return false;
		}
		offset = {};
		rotation = {turned, 0, 0};
		return true;
	});
	client::RemoteSeatGuns guns;
	client::RemoteSeatGuns::Seat seat;
	seat.player = 2;
	seat.vehicle = id;
	seat.velocity = {0, 0, 5};
	seat.firing[0] = true;
	guns.step(round, {seat}, 1 / 30.0f);
	const auto visual = [&round] {
		std::vector<game::Projectile> out;
		for (const game::Projectile& p : round.projectiles()) {
			if (p.alive && p.visual) {
				out.push_back(p);
			}
		}
		return out;
	};
	std::vector<game::Projectile> flying = visual();
	CHECK(flying.size() == 1);
	if (flying.size() == 1) {
		// The turret turned 90 degrees: the gun's (0, 2, 1) about the pivot
		// (0, 2, 0) is (1, 2, 0), its start 2 m on along +x.
		const game::Projectile& p = flying[0];
		CHECK(std::fabs(p.origin.x - 13) < 1e-3f && std::fabs(p.origin.y - 2) < 1e-3f && std::fabs(p.origin.z - 10) < 1e-3f);
		CHECK(std::fabs(p.velocity.x - 100) < 1e-2f && std::fabs(p.velocity.z - 5) < 1e-2f && p.shooter == id);
	}
	const auto sounds = round.takeWeaponSounds();
	CHECK(sounds.size() == 1 && std::fabs(sounds[0].position.x - 13) < 1e-3f);
	// A networked round comes as the server's ghost: none made here.
	shell->settings()["networkableInfo"] = "ProjectileInfo";
	seat.firing[0] = false;
	guns.step(round, {seat}, 1 / 30.0f);
	seat.firing[0] = true;
	for (int i = 0; i < 40; ++i) {
		guns.step(round, {seat}, 1 / 30.0f);
	}
	CHECK(visual().size() == 1);
	CHECK(round.takeWeaponSounds().size() == 1); // its sound and effects still
}

// Their deviation on the client: nothing networks or seeds it, so the
// client's own SimpleDeviationComp (minDev, setFireDev; 0x005dedb0 /
// 0x005f5540) strays its rounds, as fireSeatWeapon does on the server.
TEST(Host_RemoteSeatGunsStrayByTheClientsDeviation)
{
	namespace world = dice::hfe::world;
	namespace game = dice::hfe::game;
	namespace client = dice::hfe::client;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::ObjectTemplate* tank = templates.create("PlayerControlObject", "tank");
	tank->settings()["seatInformation"] = "tank 0/1/0 0/0/0";
	tank->children().push_back({"tank_gun", {0, 2, 0}, {}});
	world::ObjectTemplate* gun = templates.create("GenericFireArm", "tank_gun");
	gun->components().push_back("SimpleDeviationComp");
	gun->settings()["deviation.minDev"] = "3";
	gun->settings()["fire.roundsPerMinute"] = "1800";
	gun->settings()["fire.addFireRate.list"] = "2";
	gun->settings()["projectileTemplate"] = "tank_round";
	gun->settings()["velocity"] = "100";
	templates.create("GenericProjectile", "tank_round")->settings()["timeToLive"] = "5";
	const uint32_t id = objects.create(tank, {0, 0, 0})->id();
	game::GameWorld round(templates, objects);
	client::RemoteSeatGuns guns;
	client::RemoteSeatGuns::Seat seat;
	seat.player = 2;
	seat.vehicle = id;
	seat.firing[0] = true;
	for (int i = 0; i < 10; ++i) {
		guns.step(round, {seat}, 1 / 30.0f);
	}
	int strayed = 0, rounds = 0;
	for (const game::Projectile& p : round.projectiles()) {
		if (p.alive && p.visual) {
			++rounds;
			strayed += std::fabs(p.velocity.x) > 0.01f || std::fabs(p.velocity.y) > 0.01f ? 1 : 0;
			CHECK(p.velocity.z > 90); // within a few degrees of straight ahead
		}
	}
	CHECK(rounds >= 5 && strayed == rounds);
	// Without the component the rounds fly straight.
	gun->components().clear();
	game::GameWorld straight(templates, objects);
	client::RemoteSeatGuns plain;
	for (int i = 0; i < 4; ++i) {
		plain.step(straight, {seat}, 1 / 30.0f);
	}
	for (const game::Projectile& p : straight.projectiles()) {
		CHECK(!p.alive || (std::fabs(p.velocity.x) < 1e-3f && std::fabs(p.velocity.y) < 1e-3f));
	}
}
