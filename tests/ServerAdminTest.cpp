// A server's configuration and administration: the sv settings, the map
// list and its rotation, the bans, the admin.* console objects and
// rcon_invoke (Net/ServerSettings.h, Main/ServerAdmin.h), against a hosted
// game and the rewrite's own client over UDP on 127.0.0.1.
#include "TestMain.h"

#include "Console/Console.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Main/HostedGame.h"
#include "Main/ServerAdmin.h"
#include "Net/Demo.h"
#include "Net/GameClient.h"
#include "Net/GameProtocol.h"
#include "Net/RemoteConsole.h"
#include "Net/ServerSettings.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace net = dice::hfe::net;
namespace host = dice::hfe::host;
using dice::hfe::Console;
namespace ConsoleAccess = dice::hfe::ConsoleAccess;

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

std::string run(Console& console, const std::string& line)
{
	std::string output;
	console.execute(line, output, ConsoleAccess::Anyone);
	return output;
}

} // namespace

TEST(ServerAdmin_SettingsAndSave)
{
	net::ServerSettings sv;
	CHECK(sv.serverName == "Battlefield 2142" && sv.maxPlayers == 64 && sv.roundsPerMap == 3 && sv.adminScript == "default");
	CHECK(sv.set("SERVERNAME", "Loopback Test") && sv.serverName == "Loopback Test");
	CHECK(sv.set("teamRatioPercent", "75.5") && sv.teamRatioPercent == 75.5f);
	CHECK(sv.set("autoBalanceTeam", "1") && sv.autoBalanceTeam);
	CHECK(sv.set("maxPlayers", "not a number") && sv.maxPlayers == 64); // kept
	CHECK(!sv.set("noSuchSetting", "1"));
	std::string value;
	CHECK(sv.get("botSkill", value) && value == "0.5");
	// 0x00431880's order and quoting; sv.ranked only on a ranked server.
	const std::string saved = sv.save();
	CHECK(saved.rfind("sv.serverName \"Loopback Test\"\nsv.password \"\"\nsv.internet 0\n", 0) == 0);
	CHECK(saved.find("sv.interfaceIP \n") != std::string::npos);
	CHECK(saved.find("sv.adminScript \"default\"\n") != std::string::npos);
	CHECK(saved.find("sv.ranked") == std::string::npos && saved.find("sv.configFile") == std::string::npos);
	const std::string last = "sv.allowTitanMovement 1\n";
	CHECK(saved.size() > last.size() && saved.compare(saved.size() - last.size(), last.size(), last) == 0);
	sv.ranked = true;
	CHECK(sv.save().find("sv.useGlobalUnlocks 1\nsv.ranked 1\nsv.minUnlockLevel 0\n") != std::string::npos);
}

TEST(ServerAdmin_MapListRotationAndBans)
{
	net::MapList maps;
	maps.validate = [](const net::MapListEntry& e, std::string& error) {
		error = "map not found: " + e.level;
		return e.level != "nowhere";
	};
	CHECK(maps.append({"minsk", "gpm_cq", 32}));
	CHECK(maps.append({"Camp_Gibraltar", "gpm_ti", 64}));
	CHECK(!maps.append({"nowhere", "gpm_cq", 32}));
	CHECK(maps.insert(1, {"Suez_Canal", "gpm_cq", 16}));
	CHECK(maps.list() == "0: \"minsk\" \"gpm_cq\" 32\n1: \"Suez_Canal\" \"gpm_cq\" 16\n2: \"Camp_Gibraltar\" \"gpm_ti\" 64\n");
	CHECK(maps.save().rfind("maplist.append \"minsk\" \"gpm_cq\" 32\n", 0) == 0);
	CHECK(maps.current() == 0 && maps.next() == 1);

	host::ServerAdmin admin;
	admin.maps = maps;
	admin.settings.roundsPerMap = 2;
	CHECK(admin.nextRound().level == "minsk");          // round 2 of 2 on minsk
	CHECK(admin.nextRound().level == "Suez_Canal");     // on to the next
	admin.request = host::ServerAdmin::Request::Restart;
	CHECK(admin.nextRound().level == "Suez_Canal");
	admin.maps.setNext(0);                              // admin.nextLevel 0
	admin.request = host::ServerAdmin::Request::NextLevel;
	CHECK(admin.nextRound().level == "minsk" && admin.maps.current() == 0);
	admin.request = host::ServerAdmin::Request::RunLevel;
	admin.requestedLevel = {"Shuhia_Taiba", "gpm_cq", 32};
	CHECK(admin.nextRound().level == "Shuhia_Taiba" && admin.maps.current() == 0);
	admin.settings.roundsPerMap = 1;
	CHECK(admin.nextRound().level == "Suez_Canal");
	CHECK(admin.nextRound().level == "Camp_Gibraltar");
	CHECK(admin.nextRound().level == "minsk"); // round and round
	CHECK(maps.remove(5) == -1 && maps.remove(0) == 2 && maps.current() == 0);

	// 0x00505db0's periods: "perm" (or nothing), "round", seconds.
	net::BanList bans;
	const std::time_t now = 1000000000;
	CHECK(bans.addAddress("10.0.0.1:12345", "perm", now));
	CHECK(bans.addAddress("10.0.0.2", "round", now));
	CHECK(bans.addKey("abcdef", "60", now));
	CHECK(!bans.addKey("x", "-5", now));
	CHECK(bans.bannedAddress("10.0.0.1:999", now) && bans.bannedAddress("10.0.0.2", now));
	CHECK(bans.bannedKey("abcdef", now + 59) && !bans.bannedKey("abcdef", now + 61));
	CHECK(bans.save(now) == "admin.addKeyToBanList abcdef 1000000060\nadmin.addAddressToBanList 10.0.0.1 perm\n");
	bans.endRound();
	CHECK(bans.bannedAddress("10.0.0.1", now) && !bans.bannedAddress("10.0.0.2", now));
	// A saved ban reads back with its end time.
	net::BanList again;
	CHECK(again.addKey("abcdef", "1000000060", now) && again.bannedKey("abcdef", now + 30) && !again.bannedKey("abcdef", now + 90));
}

TEST(ServerAdmin_ConsoleAndRconInvoke)
{
	host::ServerAdmin admin;
	Console console;
	admin.registerConsole(console);
	CHECK(run(console, "sv.serverName \"Recomp Test\"").empty() && admin.settings.serverName == "Recomp Test");
	CHECK(run(console, "sv.serverName") == "Recomp Test");
	CHECK(run(console, "sv.roundsPerMap 2").empty() && admin.settings.roundsPerMap == 2);
	CHECK(run(console, "GameServerSettings.setTeamRatio 50").empty() && admin.settings.teamRatioPercent == 50);
	CHECK(run(console, "GameServerSettings.setPunishTeamKills 0").empty() && !admin.settings.tkPunishEnabled);
	CHECK(run(console, "maplist.append minsk gpm_cq 32") == "1");
	CHECK(run(console, "mapList.append Camp_Gibraltar gpm_cq") == "1"); // the size optional; any case
	CHECK(run(console, "maplist.mapCount") == "2" && run(console, "maplist.currentMap") == "0");
	CHECK(run(console, "admin.nextLevel") == "1");
	CHECK(run(console, "reservedSlots.addNick Somebody").empty() && admin.reserved.has("Somebody"));

	// admin.* through the hooks.
	int kicked = -1;
	bool banned = false;
	std::string said;
	bool nextLevel = false;
	admin.hooks.players = [] {
		host::AdminHooks::Player local, remote, bot;
		local.id = 0, local.name = "Host";
		remote.id = 1, remote.name = "Joiner", remote.remote = true, remote.address = "127.0.0.1:50000",
		remote.cdKeyHash = "0123456789abcdef0123456789abcdef";
		bot.id = 2, bot.name = "Bot", bot.ai = true;
		return std::vector<host::AdminHooks::Player> {local, remote, bot};
	};
	admin.hooks.kick = [&](int player, bool ban) {
		kicked = player;
		banned = ban;
		return true;
	};
	admin.hooks.sayAll = [&](const std::string& text) { said = text; };
	admin.hooks.runNextLevel = [&] { nextLevel = true; };
	CHECK(run(console, "admin.listPlayers")
		== "Id:  0 - Host is local.\nId:  1 - Joiner is remote ip: 127.0.0.1:50000 ->\n         CD-key hash: "
		   "0123456789abcdef0123456789abcdef \nId:  2 - Bot is local. is an AI bot.\n");
	CHECK(run(console, "admin.kickPlayer 7") == "Player not found.");
	CHECK(run(console, "admin.kickPlayer 1").empty() && kicked == 1 && !banned);
	CHECK(run(console, "admin.banPlayer 1 round").empty() && kicked == 1 && banned);
	CHECK(admin.bans.bannedAddress("127.0.0.1"));
	CHECK(run(console, "admin.listBannedAddresses") == "127.0.0.1 round\n");
	CHECK(run(console, "Game.sayAll \"hello server\"").empty() && said == "hello server");
	CHECK(run(console, "admin.runNextLevel").empty() && nextLevel);

	// rcon_invoke (0x004427b0): the output ends with a newline; nothing
	// for nothing; a ranked server refuses vars.set.
	CHECK(admin.rconInvoke(console, "maplist.list") == "0: \"minsk\" \"gpm_cq\" 32\n1: \"Camp_Gibraltar\" \"gpm_cq\" 0\n");
	CHECK(admin.rconInvoke(console, "sv.maxPlayers") == "64\n");
	CHECK(admin.rconInvoke(console, "").empty());
	CHECK(admin.rconInvoke(console, "sv.maxPlayers 32\nsv.maxPlayers") == "32\n");
	admin.settings.ranked = true;
	CHECK(admin.rconInvoke(console, "  vars.set x 1") == "Ranked server blocks vars.set from remote console\n");

	// The shipped ServerSettings.con through sv.* (with the game).
	if (const char* dir = std::getenv("BF2142_GAME_DIR")) {
		host::ServerAdmin shipped;
		shipped.setDefaultFiles(std::string(dir) + "/mods/bf2142");
		Console other;
		shipped.registerConsole(other);
		CHECK(shipped.loadSettings(other));
		CHECK(shipped.settings.serverName == "Battlefield 2142" && shipped.settings.maxPlayers == 64
			&& shipped.settings.voipServerPort == 55125 && shipped.settings.autoDemoHook == "adminutils/demo/rotate_demo.exe");
		CHECK(shipped.loadMapList(other) && shipped.maps.count() == 1 && shipped.maps.entries()[0].level == "minsk");
	}
}

// A hosted game with a server configuration: the settings and the whole
// map list go to the joining client, admin.listPlayers shows it, a ban
// kicks it and refuses it after; the in-game remote console (6/0x12 and
// 6/0x13) and a server message.
TEST(ServerAdmin_HostedKickBanAndMessages)
{
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::game::GameWorld round(templates, objects);
	dice::hfe::game::GameLogic logic;
	round.startRound();
	logic.startRound();

	host::ServerAdmin admin;
	admin.settings.serverName = "Admin Test";
	admin.settings.welcomeMessage = "welcome";
	CHECK(admin.maps.append({"Camp_Gibraltar", "gpm_cq", 32}));
	CHECK(admin.maps.append({"Minsk", "gpm_cq", 32}));
	host::HostSetup setup;
	setup.serverName = admin.settings.serverName;
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_cq";
	setup.mapSize = 32;
	setup.gamePort = 0;
	setup.queryPort = 0;
	setup.settings = &admin.settings;
	setup.maps = &admin.maps;
	setup.bans = &admin.bans;
	host::HostedGame hosted(round, logic, templates, setup);
	CHECK(hosted.start());
	std::vector<std::string> commands;
	hosted.onRemoteCommand = [&](int player, const std::string& command) {
		commands.push_back(command);
		hosted.sendRemoteFeedback(player, "Authentication successful, rcon ready.\n");
	};
	Console console;
	admin.registerConsole(console);
	admin.hooks.players = [&] {
		std::vector<host::AdminHooks::Player> out;
		for (const host::HostedGame::ClientPlayer& c : hosted.clientPlayers()) {
			host::AdminHooks::Player p;
			p.id = c.player, p.name = c.name, p.remote = true, p.address = c.address, p.cdKeyHash = c.cdKeyHash;
			out.push_back(p);
		}
		return out;
	};
	admin.hooks.kick = [&](int player, bool ban) { return hosted.kickPlayer(player, ban); };
	admin.hooks.sayAll = [&](const std::string& text) { hosted.sayAll(text); };

	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Joiner";
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
	const auto step = [&] {
		client.update();
		hosted.tick();
		round.tick(1.0f / 30);
	};
	CHECK(pumpUntil(step, [&] { return client.world().gameState() == 1 && client.world().roundStarted(); }));
	CHECK(client.world().serverName() == "Admin Test" && client.world().mapList().size() == 2);
	CHECK(client.world().settings().welcome.size() == 1 && client.world().settings().welcome[0] == "welcome");
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* me = client.world().player(client.world().localPlayer());
		return me && me->name == "Joiner";
	}));
	const int me = client.world().localPlayer();
	const std::string list = run(console, "admin.listPlayers");
	CHECK(list.find("Joiner is remote ip: 127.0.0.1:") != std::string::npos);

	// In-game rcon: the line to the server, its answer back.
	client.remoteCommand("login secret");
	bool answered = false;
	CHECK(pumpUntil(step, [&] {
		for (const net::PostRemoteInfo& m : client.world().takeMessages()) {
			if (m.category == net::kMsgNetworkCategory && m.id == net::kNeRemoteConsoleFeedback && m.payloadInt() == -1
				&& m.payload.find("rcon: Authentication successful") != std::string::npos) {
				answered = true;
			}
		}
		return answered;
	}));
	CHECK(commands.size() == 1 && commands[0] == "login secret");

	// A server message (Game.sayAll).
	CHECK(run(console, "Game.sayAll \"server here\"").empty());
	bool heard = false;
	CHECK(pumpUntil(step, [&] {
		for (const net::ChatLine& line : client.world().takeChat()) {
			heard = heard || (line.text == "server here" && line.channel == net::kChatServerMessage);
		}
		return heard;
	}));

	// admin.banPlayer: kicked (KickBan) and refused (0x16) after.
	CHECK(run(console, "admin.banPlayer " + std::to_string(me)).empty());
	CHECK(pumpUntil(step, [&] { return client.state() != net::GameClient::State::Connected; }));
	CHECK(admin.bans.bannedAddress("127.0.0.1"));
	net::GameClient again;
	CHECK(again.connect("127.0.0.1", hosted.gamePort(), options));
	CHECK(pumpUntil(
		[&] {
			again.update();
			hosted.tick();
		},
		[&] { return again.state() == net::GameClient::State::Refused; }));
	CHECK(again.connectResult().reason == 0x16);
	CHECK(run(console, "admin.removeAddressFromBanList 127.0.0.1").empty() && !admin.bans.bannedAddress("127.0.0.1"));
}

// Reserved slots on a full server (0x004ae6a0): with one player at
// sv.maxPlayers, a listed nick (its clan prefix aside) bumps the player who
// isn't listed (0x23); an unlisted one is refused as full (2).
TEST(ServerAdmin_ReservedSlotAdmitsToAFullServer)
{
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::game::GameWorld round(templates, objects);
	dice::hfe::game::GameLogic logic;
	round.startRound();
	logic.startRound();
	host::ServerAdmin admin;
	admin.settings.numReservedSlots = 1;
	admin.reserved.addNick("vip");
	host::HostSetup setup;
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_cq";
	setup.mapSize = 32;
	setup.maxPlayers = 1;
	setup.gamePort = 0;
	setup.queryPort = 0;
	setup.settings = &admin.settings;
	setup.reserved = &admin.reserved;
	host::HostedGame hosted(round, logic, templates, setup);
	CHECK(hosted.start());
	std::vector<std::unique_ptr<net::GameClient>> clients;
	const auto join = [&](const char* name) {
		auto client = std::make_unique<net::GameClient>();
		net::GameClientOptions options;
		options.playerName = name;
		CHECK(client->connect("127.0.0.1", hosted.gamePort(), options));
		clients.push_back(std::move(client));
		return clients.back().get();
	};
	const auto step = [&] {
		for (auto& c : clients) {
			c->update();
		}
		hosted.tick();
		round.tick(1.0f / 30);
	};
	net::GameClient* joe = join("joe");
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* me = joe->world().player(joe->world().localPlayer());
		return me && me->name == "joe";
	}));
	net::GameClient* vip = join("[CLAN] vip");
	CHECK(pumpUntil(step, [&] { return joe->state() != net::GameClient::State::Connected; }));
	CHECK(joe->disconnectReason() == 0x23);
	CHECK(vip->state() == net::GameClient::State::Connected);
	net::GameClient* bob = join("bob");
	CHECK(pumpUntil(step, [&] { return bob->state() != net::GameClient::State::Connected && bob->state() != net::GameClient::State::Connecting; }));
	CHECK(bob->disconnectReason() == 2);
	CHECK(vip->state() == net::GameClient::State::Connected);
}

// The server's round start (0x004c03b0): waiting for
// sv.numPlayersNeededToStart, sv.startDelay's countdown, then playing; too
// few players for sv.notEnoughPlayersRestartDelay raise TimeLimitReached(0)
// (the time limit's own is -1).
TEST(ServerAdmin_StartDelayAndTooFewPlayers)
{
	dice::hfe::game::GameLogic logic;
	logic.startRound();
	int players = 1;
	bool playing = false;
	std::vector<int> limits;
	logic.setEventSink([&](const std::string& event, const dice::hfe::game::ScriptArgs& args) {
		if (event == "TimeLimitReached") {
			limits.push_back(static_cast<int>(args[0].asInt()));
		}
	});
	logic.waitToStart(2, 3, 5, [&players] { return players; });
	logic.onPlaying = [&playing] { playing = true; };
	for (int i = 0; i < 300; ++i) {
		logic.tick(1.0f / 30);
	}
	CHECK(logic.roundState() == 1 && !playing && logic.elapsed == 0);
	players = 2;
	for (int i = 0; i < 89; ++i) {
		logic.tick(1.0f / 30);
	}
	CHECK(logic.roundState() == 2 && !playing);
	for (int i = 0; i < 3; ++i) {
		logic.tick(1.0f / 30);
	}
	CHECK(logic.roundState() == 3 && playing);
	players = 1;
	for (int i = 0; i < 30 * 5 + 2; ++i) {
		logic.tick(1.0f / 30);
	}
	CHECK(limits.size() == 1 && limits[0] == 0);
	logic.timeLimit = 6;
	players = 2;
	for (int i = 0; i < 30 * 2; ++i) {
		logic.tick(1.0f / 30);
	}
	CHECK(limits.size() == 2 && limits[1] == -1);
}

// The server's votes (game::Votes, 0x00590230 / 0x0058f470 / 0x0058fb90):
// too few voters refused (7), a map vote needing ceil(2/3) yes votes, a
// second vote refused while one runs (6), a kick vote failing on time, a
// mutiny only in the commander's team.
TEST(ServerAdmin_Votes)
{
	using dice::hfe::game::Votes;
	Votes votes;
	votes.settings.minPlayers = 2;
	votes.settings.voteTime = 10;
	std::vector<Votes::Voter> players {{1, 1, true}, {2, 1, true}, {3, 2, true}, {4, 2, false}};
	votes.players = [&players] { return players; };
	votes.maps = 3;
	votes.currentMap = 0;
	votes.kickable = [](int p) { return p >= 1 && p <= 4; };
	votes.commander = [](int team) { return team == 1 ? 1 : -1; };
	// One voter (the others opted out): refused for too few.
	votes.handle(2, Votes::kMap, Votes::kOptOut, 0, 1);
	votes.handle(3, Votes::kMap, Votes::kOptOut, 0, 1);
	auto r = votes.handle(1, Votes::kMap, Votes::kStart, 2, 0);
	CHECK(r.size() == 1 && r[0].phase == Votes::kTooFewPlayers && r[0].to == 1);
	votes.handle(2, Votes::kMap, Votes::kOptOut, 0, 0);
	votes.handle(3, Votes::kMap, Votes::kOptOut, 0, 0);
	// Three humans: 2 yes needed; the starter's is one.
	CHECK(votes.handle(1, Votes::kMap, Votes::kStart, 0, 0).empty()); // the map running
	r = votes.handle(1, Votes::kMap, Votes::kStart, 2, 0);
	CHECK(r.size() == 1 && r[0].phase == Votes::kStart && r[0].needed == 2 && r[0].seconds == 10 && r[0].to == -1);
	r = votes.handle(3, Votes::kKick, Votes::kStart, 2, 0);
	CHECK(r.size() == 1 && r[0].phase == Votes::kRefused);
	std::vector<Votes::Reply> replies;
	CHECK(votes.tick(1, replies).empty() && replies.empty());
	r = votes.handle(2, Votes::kMap, Votes::kCast, 2, 0);
	CHECK(r.size() == 1 && r[0].phase == Votes::kCast && r[0].yesCount == 2);
	auto results = votes.tick(1, replies);
	CHECK(results.size() == 1 && results[0].kind == Votes::kMap && results[0].target == 2);
	CHECK(replies.size() == 1 && replies[0].phase == Votes::kPassed);
	// A kick vote: a 3 refused (0..2 only), a 2 counts for neither, so it
	// runs on; still short when the time runs out.
	replies.clear();
	r = votes.handle(1, Votes::kKick, Votes::kStart, 3, 0);
	CHECK(r.size() == 1 && r[0].needed == 2); // the target doesn't vote
	CHECK(votes.handle(2, Votes::kKick, Votes::kCast, 3, 3).empty());
	r = votes.handle(2, Votes::kKick, Votes::kCast, 3, 2);
	CHECK(r.size() == 1 && r[0].yesCount == 1 && r[0].noCount == 0);
	votes.tick(1, replies);
	CHECK(replies.empty()); // yes + no (1) under the two eligible
	votes.tick(10, replies);
	CHECK(replies.size() == 1 && replies[0].phase == Votes::kFailed && replies[0].needed == 1);
	// A no from the last voter ends it at once (0x00645bb0: yes + no >= eligible).
	replies.clear();
	votes.handle(1, Votes::kKick, Votes::kStart, 3, 0);
	r = votes.handle(2, Votes::kKick, Votes::kCast, 3, 1);
	CHECK(r.size() == 1 && r[0].noCount == 1);
	votes.tick(0.1f, replies);
	CHECK(replies.size() == 1 && replies[0].phase == Votes::kFailed);
	// A mutiny: only the commander's team votes, and the target can't.
	replies.clear();
	votes.settings.minPlayers = 1;
	CHECK(votes.handle(3, Votes::kMutiny, Votes::kStart, 1, 0).empty()); // not team 1's commander from team 2
	r = votes.handle(2, Votes::kMutiny, Votes::kStart, 1, 0);
	CHECK(r.size() == 1 && r[0].phase == Votes::kStart && r[0].needed == 1);
	results = votes.tick(0.1f, replies);
	CHECK(results.size() == 1 && results[0].kind == Votes::kMutiny && results[0].team == 1);
	// Voting off: nothing.
	votes.settings.enabled = false;
	CHECK(votes.handle(1, Votes::kMap, Votes::kStart, 1, 0).empty());
}

// A demo (Net/Demo.h): the host records what its demo connection 0x81 is
// sent (the join's data at once, then a payload a tick) into a zlib'd
// .bf2142demo; played back headless, a client with no socket gets the
// server's settings, the level, the round's start and the players.
TEST(ServerAdmin_DemoRecordAndPlayBack)
{
	const std::string path = "bf2142_test_demo.bf2142demo";
	std::remove(path.c_str());
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::game::GameWorld round(templates, objects);
	dice::hfe::game::GameLogic logic;
	round.startRound();
	logic.startRound();
	host::ServerAdmin admin;
	admin.settings.serverName = "Demo Test";
	CHECK(admin.maps.append({"Camp_Gibraltar", "gpm_cq", 32}));
	host::HostSetup setup;
	setup.serverName = admin.settings.serverName;
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_cq";
	setup.mapSize = 32;
	setup.gamePort = 0;
	setup.queryPort = 0;
	setup.settings = &admin.settings;
	setup.maps = &admin.maps;
	host::HostedGame hosted(round, logic, templates, setup);
	CHECK(hosted.start());
	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Recorded";
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
	const auto step = [&] {
		client.update();
		hosted.tick();
		round.tick(1.0f / 30);
	};
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* me = client.world().player(client.world().localPlayer());
		return me && me->name == "Recorded";
	}));
	CHECK(hosted.recordDemo(path));
	CHECK(!hosted.recordDemo(path)); // already recording
	for (int i = 0; i < 60; ++i) {
		step();
	}
	hosted.stopDemo();
	CHECK(!hosted.recordingDemo());

	net::DemoHeader header;
	std::vector<net::DemoRecord> records;
	CHECK(net::readDemo(path, header, records));
	CHECK(header.version == 0x70 && header.serverName == "Demo Test" && header.level == "Camp_Gibraltar");
	// sv.demoQuality 1: 4 records a second (net::demoRates), 60 ticks of them.
	CHECK(records.size() > 8);
	CHECK(!records.empty() && records[0].time == -1);
	const bool marker = std::any_of(records.begin(), records.end(), [](const net::DemoRecord& r) { return r.time == -2; });
	CHECK(marker);
	CHECK(records.back().time > 0 && records.back().payload.empty()); // the end record

	// Played back: the clock follows the records, nothing goes out.
	net::DemoPlayer player(records);
	net::GameClient viewer;
	viewer.playDemo([&player](std::vector<uint8_t>& payload) { return player.next(payload); });
	CHECK(viewer.playingDemo() && viewer.state() == net::GameClient::State::Connected);
	int ticks = 0;
	while (viewer.update() && ticks < 600) {
		player.advance(1);
		++ticks;
	}
	CHECK(player.finished() && viewer.state() == net::GameClient::State::Disconnected);
	CHECK(ticks >= 59 && ticks < 100);
	CHECK(viewer.world().serverName() == "Demo Test" && viewer.world().hasLevel());
	CHECK(viewer.world().gameState() == 1 && viewer.world().roundStarted());
	bool sawPlayer = false;
	for (int id = 0; id < 256; ++id) {
		const net::RemotePlayer* p = viewer.world().player(id);
		sawPlayer = sawPlayer || (p && p->name == "Recorded");
	}
	CHECK(sawPlayer);
	CHECK(viewer.world().localPlayer() < 0); // no player of its own
	std::remove(path.c_str());
	CHECK(net::demoPath("match1") == "Demos/match1.bf2142demo" && net::demoPath("a\\b.dem") == "a/b.dem");
}

// sv.autoRecord (AutoRecorder 0x00585860 / 0x00585920): a demo from the
// round's start, named by the local time (0x00585620), stopped at the
// round's restart; without the hook program nothing is run.
TEST(ServerAdmin_AutoRecorder)
{
	CHECK(host::autoDemoPath("mods/bf2142", 0).rfind("mods/bf2142/demos/auto_", 0) == 0);
	const std::string name = host::autoDemoPath("m", 1700000000);
	CHECK(name.size() == std::string("m/demos/auto_2023_11_14_22_13_20.bf2142demo").size());
	CHECK(name.find(':') == std::string::npos && name.find(' ') == std::string::npos && name.find('-') == std::string::npos);
	const std::string root = "bf2142_test_autodemo";
	std::filesystem::remove_all(root);
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::game::GameWorld round(templates, objects);
	dice::hfe::game::GameLogic logic;
	round.startRound();
	logic.startRound();
	host::ServerAdmin admin;
	admin.settings.autoRecord = true;
	admin.settings.autoDemoHook = root + "/no_such_hook.exe";
	host::HostSetup setup;
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	setup.settings = &admin.settings;
	setup.modDirectory = root;
	std::string file;
	{
		host::HostedGame hosted(round, logic, templates, setup);
		CHECK(hosted.start());
		for (int i = 0; i < 10; ++i) {
			hosted.tick();
			round.tick(1.0f / 30);
		}
		CHECK(hosted.recordingDemo() && !hosted.autoDemo().empty());
		file = hosted.autoDemo();
		hosted.changeLevel(round, logic, templates, setup); // the round's restart stops it
		CHECK(!hosted.recordingDemo() && hosted.lastAutoDemo() == file);
	}
	net::DemoHeader header;
	std::vector<net::DemoRecord> records;
	CHECK(net::readDemo(file, header, records) && !records.empty());
	std::filesystem::remove_all(root);
}
