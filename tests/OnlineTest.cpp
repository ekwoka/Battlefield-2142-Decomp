// The login chain against the local OpenSpy emulator (Net/OpenSpyEmulator.h)
// and a host doing a retail server's pre-authorisation, all on 127.0.0.1:
// FESL, GameSpy Presence, the master server's list, the CD-key service,
// the stats service's ValidatePlayer, the player info block, and a join
// that is refused without a login and accepted, made and spawned with one.
#include "TestMain.h"

#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Main/HostedGame.h"
#include "Net/GameClient.h"
#include "Net/GameEvents.h"
#include "Net/GameSpyPresence.h"
#include "Net/Md5.h"
#include "Net/OnlineConfig.h"
#include "Net/OnlineSession.h"
#include "Net/OpenSpyEmulator.h"
#include "Net/PlayerAuth.h"
#include "Net/ServerBrowser.h"
#include "Physics/CollisionTemplateManager.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace net = dice::hfe::net;
namespace host = dice::hfe::host;

namespace {

bool waitUntil(const std::function<void()>& step, const std::function<bool()>& done, int milliseconds = 5000)
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

net::OpenSpyEmulatorOptions emulatorOptions()
{
	net::OpenSpyEmulatorOptions o;
	o.accounts.push_back(net::OpenSpyEmulator::account("tester:secret@Tester,Second", 1001, 2001));
	return o;
}

constexpr const char* kKey = "ABCD-EFGH-IJKL-MNOP-QRST";

} // namespace

TEST(Online_NameChecksum)
{
	// 0x00579410 is Adler-32 started at 0 rather than 1 (its low sum,
	// +0x14 of the loop, starts at 0) over the raw bytes of short names...
	CHECK(net::nameChecksum("abc") == 0x024a0126u);
	CHECK(net::nameChecksum("Wikipedia") == 0x11dd0397u);
	CHECK(net::nameChecksum("") == 0);
	// ...and lower-cases each whole run of 16, not the rest.
	CHECK(net::nameChecksum("ABCDEFGHIJKLMNOPQ") == net::nameChecksum("abcdefghijklmnopQ"));
	CHECK(net::nameChecksum("ABCDEFGHIJKLMNOPQ") != net::nameChecksum("abcdefghijklmnopq"));
	CHECK(net::nameChecksum("Tester") != net::nameChecksum("tester"));
}

TEST(Online_PlayerInfoBlock)
{
	net::PlayerInfoBlock info;
	info.name = "Tester";
	info.nameChecksum = net::nameChecksum("Tester");
	info.profileId = 2002;
	info.clanTag = "[RCP]";
	info.auth = net::urlSafeTicket("ab+cd/ef==");
	info.voice = true;
	CHECK(info.auth == "ab[cd]ef__");
	const std::string data = net::encodePlayerInfo(info);
	// 2+6, 4, 4, 2+5, 2+10 bytes and a bit.
	CHECK(data.size() == 36);
	net::PlayerInfoBlock back;
	CHECK(net::decodePlayerInfo(data, back));
	CHECK(back.name == "Tester" && back.nameChecksum == info.nameChecksum && back.profileId == 2002 && back.clanTag == "[RCP]"
		&& back.auth == "ab[cd]ef__" && back.voice);
	// The writer's limits: 0x40, 0x20 and 0x5c bytes.
	info.name = std::string(100, 'n');
	info.auth = std::string(200, 'a');
	CHECK(net::decodePlayerInfo(net::encodePlayerInfo(info), back) && back.name.size() == 0x40 && back.auth.size() == 0x5c);
	CHECK(!net::decodePlayerInfo(data.substr(0, 10), back));
	CHECK(net::validatePlayerPath("t", "A B", 7) == "/ValidatePlayer.aspx?auth=t&tid=0&SoldierNick=A%20B&pid=7");
	CHECK(net::validatePlayerResult("O\nH\tpid\tnick\t\nD\t7\tA\t\nH\tresult\t\nD\tOk\t\n$\t20\t$\n") == "Ok");
}

TEST(Online_GpMessagesAndHashes)
{
	std::string stream = "\\lc\\1\\challenge\\ABCDEFGHIJ\\id\\1\\final\\\\ka\\\\final\\\\lc\\2";
	const auto messages = net::takeGpMessages(stream);
	CHECK(messages.size() == 2 && stream == "\\lc\\2");
	if (messages.size() == 2) {
		CHECK(net::gpValue(messages[0], "lc") == "1" && net::gpValue(messages[0], "challenge") == "ABCDEFGHIJ");
		CHECK(messages[1].front().first == "ka" && net::gpHas(messages[1], "ka"));
	}
	CHECK(net::encodeGp({{"login", ""}, {"challenge", "x"}}) == "\\login\\\\challenge\\x\\final\\");
	// GameSpy's login hash: md5(md5(pw) + 48 spaces + user + cc + sc + md5(pw)).
	const std::string pw = net::md5Hex("pc");
	CHECK(net::gpLoginResponse("pc", "tok", "CC", "SC") == net::md5Hex(pw + std::string(48, ' ') + "tok" + "CC" + "SC" + pw));
	CHECK(net::gpLoginProof("pc", "tok", "CC", "SC") == net::md5Hex(pw + std::string(48, ' ') + "tok" + "SC" + "CC" + pw));
}

TEST(Online_OfflineGuardAndConfig)
{
	// Offline (the default), only this machine and its network.
	CHECK(!net::onlineConfig().online);
	CHECK(net::hostAllowed("127.0.0.1") && net::hostAllowed("localhost") && net::hostAllowed("192.168.1.20")
		&& net::hostAllowed("10.0.0.5") && net::hostAllowed("172.20.1.1"));
	CHECK(!net::hostAllowed("fesl.openspy.net") && !net::hostAllowed("8.8.8.8") && !net::hostAllowed("172.32.0.1"));
	net::FeslClient fesl;
	std::string error;
	CHECK(!fesl.connect(net::kFeslHost, net::kFeslPort, 1, &error) && error.find("--online") != std::string::npos);
	net::GpClient gp;
	CHECK(!gp.connect("gpcm.openspy.net", 29900, 1, &error));
	CHECK(!net::fetchServerList(net::masterServerHost(net::kGameName), "", {"hostname"}, 1, &error));
	// The offline endpoints are all local, FESL plain.
	const net::OnlineEndpoints local = net::localEndpoints();
	CHECK(local.fesl.host == "127.0.0.1" && !local.feslTls && local.fesl.port == net::kFeslGamePort && local.gpcm.port == 29900);
	const net::OnlineEndpoints live = net::liveEndpoints();
	CHECK(live.fesl.host == "fesl.openspy.net" && live.feslTls && live.gpcm.host == "gpcm.openspy.net"
		&& live.master.host == "stella.ms5.openspy.net" && live.stella.host == "stella.prod.openspy.net");
	// The file's keys; host overrides only offline.
	net::OnlineConfig config;
	net::applyOnlineConfigText(config, "# local\naccount = me\npassword=pw # comment\nsoldier=Me\ncdkey=K\nservices=127.0.0.2\nfesl=127.0.0.3:1234\n");
	CHECK(config.credentials.account == "me" && config.credentials.password == "pw" && config.credentials.soldier == "Me"
		&& config.credentials.cdKey == "K" && config.credentials.complete());
	CHECK(config.endpoints.gpcm.host == "127.0.0.2" && config.endpoints.fesl.host == "127.0.0.3" && config.endpoints.fesl.port == 1234);
	net::OnlineConfig live2;
	live2.online = true;
	live2.endpoints = net::liveEndpoints();
	net::applyOnlineConfigText(live2, "services=127.0.0.2\n");
	CHECK(live2.endpoints.gpcm.host == "gpcm.openspy.net");
}

TEST(Online_EmulatorLoginChain)
{
	net::OpenSpyEmulator emulator;
	std::string error;
	CHECK(emulator.start(emulatorOptions(), &error));
	net::OnlineSession session;
	session.setEndpoints(emulator.endpoints());
	// A wrong password and an unknown name, as OpenSpy answers them.
	long code = 0;
	CHECK(!session.loginAccount("tester", "wrong", &error, &code) && code == 122);
	CHECK(!session.loginAccount("nobody", "secret", &error, &code) && code == 101);
	const auto account = session.loginAccount("tester", "secret", &error);
	CHECK(account && account->userId == 1001 && account->soldiers.size() == 2);
	CHECK(!session.loginSoldier("Nobody", &error, &code) && code == 101);
	CHECK(session.loginSoldier("Second", &error));
	const net::OnlineIdentity id = session.identity();
	CHECK(id.valid() && id.soldier == "Second" && id.profileId == 2003 && id.nameChecksum == net::nameChecksum("Second"));
	CHECK(id.loginTicket.size() == 24 && emulator.gpLogins() == 1);
	// The session stays up (pings answered meanwhile) until logout.
	session.keepAlive();
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	session.logout();
	CHECK(!session.loggedIn());
}

TEST(Online_GpRefusesBadTickets)
{
	net::OpenSpyEmulator emulator;
	CHECK(emulator.start(emulatorOptions()));
	const net::OnlineEndpoints e = emulator.endpoints();
	net::GpClient gp;
	std::string error;
	CHECK(gp.connect(e.gpcm.host, e.gpcm.port, 2, &error));
	CHECK(!gp.loginPreAuthenticated("no-such-ticket", "x", &error, 3000) && error.find("265") != std::string::npos);
	// The login as sent: the retail field order.
	const std::string login = gp.lastLogin();
	CHECK(login.rfind("\\login\\\\challenge\\", 0) == 0);
	CHECK(login.find("\\authtoken\\no-such-ticket\\response\\") != std::string::npos);
	CHECK(login.find("\\productid\\10709\\gamename\\stella\\namespaceid\\20\\sdkrevision\\3\\id\\1\\final\\") != std::string::npos);
}

TEST(Online_EmulatorMasterAndCdKey)
{
	net::OpenSpyEmulatorOptions o = emulatorOptions();
	o.cdKeys = {kKey};
	net::ServerEntry server;
	server.ip = "127.0.0.1";
	server.port = 29900;
	server.fields = {{"hostname", "local test server"}, {"hostport", "17567"}, {"mapname", "Camp_Gibraltar"}};
	o.servers.push_back(server);
	net::OpenSpyEmulator emulator;
	CHECK(emulator.start(o));
	const net::OnlineEndpoints e = emulator.endpoints();
	std::string error;
	const auto list = net::fetchServerList(e.master.host, "", {"hostname", "hostport", "mapname"}, 5, &error, e.master.port);
	CHECK(list && list->servers.size() == 1);
	if (list && !list->servers.empty()) {
		CHECK(list->servers[0].ip == "127.0.0.1" && list->servers[0].port == 29900
			&& list->servers[0].fields.at("hostname") == "local test server" && list->servers[0].fields.at("hostport") == "17567");
	}
	// The CD-key service: the key's own answer passes, another key's not.
	const std::string good = net::cdKeyResponse(kKey, "BF2142rcp", 0x12345678);
	const std::string bad = net::cdKeyResponse("ZZZZ-ZZZZ-ZZZZ-ZZZZ-ZZZZ", "BF2142rcp", 0x12345678);
	CHECK(net::cdKeyAuthenticate(e.cdkey.host, e.cdkey.port, "BF2142rcp", good, 0, &error));
	CHECK(!net::cdKeyAuthenticate(e.cdkey.host, e.cdkey.port, "BF2142rcp", bad, 0, &error) && error == "Invalid CD Key");
	CHECK(!net::cdKeyAuthenticate(e.cdkey.host, e.cdkey.port, "otherchal", good, 0, &error));
}

namespace {

// A level with a spawn point a side (as HostTest's range).
struct LoginRange {
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::io::FileManager files;
	dice::hfe::physics::CollisionTemplateManager collisionTemplates;
	std::unique_ptr<dice::hfe::game::GameWorld> round;
	dice::hfe::game::GameLogic logic;

	LoginRange()
	{
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			templates.create("Soldier", name)->settings()["armor.maxHitPoints"] = "100";
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
		ground.height = [](float, float) { return 0.0f; };
		ground.heightAndNormal = [](float, float, dice::hfe::physics::Vec3& normal) {
			normal = {0, 1, 0};
			return 0.0f;
		};
		round->enablePhysics(files, collisionTemplates, ground);
		round->startRound();
		logic.startRound();
	}
};

} // namespace

// Login -> the server list -> join -> CreatePlayer -> spawn, against the
// emulator and a host that checks logins as a retail server does; a client
// without a login is refused first.
TEST(Online_PreAuthorisedJoinEndToEnd)
{
	net::OpenSpyEmulatorOptions o = emulatorOptions();
	o.cdKeys = {kKey};
	net::OpenSpyEmulator emulator;
	CHECK(emulator.start(o));
	const net::OnlineEndpoints e = emulator.endpoints();

	LoginRange range;
	dice::hfe::game::GameWorld& round = *range.round;
	host::HostSetup setup;
	setup.level = "Range";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	setup.login.required = true;
	setup.login.cdKeyHost = e.cdkey.host;
	setup.login.cdKeyPort = e.cdkey.port;
	setup.login.statsHost = e.stella.host;
	setup.login.statsPort = e.stella.port;
	std::vector<std::string> hostLog;
	host::HostedGame hosted(round, range.logic, range.templates, setup);
	hosted.onLog = [&hostLog](const std::string& line) { hostLog.push_back(line); std::printf("    host: %s\n", line.c_str()); };
	CHECK(hosted.start());
	net::ServerEntry listed;
	listed.ip = "127.0.0.1";
	listed.port = 29900;
	listed.fields = {{"hostname", "login test"}, {"hostport", std::to_string(hosted.gamePort())}};
	emulator.addServer(listed);

	const auto stepWith = [&](net::GameClient& client) {
		return [&] {
			client.update();
			hosted.tick();
			round.tick(1.0f / 30);
		};
	};

	// Without a login: the settings come, the player info goes up with no
	// name checksum (the retail client's 0x00a6801c stays 0 without a
	// soldier login) and no profile, and the server refuses (0x24, the
	// first check); no player was made.
	{
		net::GameClient anonymous;
		net::GameClientOptions options;
		options.playerName = "Anonymous";
		options.cdKey = kKey;
		CHECK(anonymous.connect("127.0.0.1", hosted.gamePort(), options));
		CHECK(waitUntil(stepWith(anonymous), [&] { return anonymous.state() == net::GameClient::State::Disconnected; }));
		CHECK(anonymous.disconnectReason() == 0x24 && anonymous.world().localPlayer() < 0 && !anonymous.world().hasLevel());
		CHECK(round.playerList().empty());
	}
	// The right checksum but no profile: 0xd.
	{
		net::GameClient noProfile;
		net::GameClientOptions options;
		options.playerName = "Tester";
		options.nameChecksum = net::nameChecksum("Tester");
		options.cdKey = kKey;
		CHECK(noProfile.connect("127.0.0.1", hosted.gamePort(), options));
		CHECK(waitUntil(stepWith(noProfile), [&] { return noProfile.state() == net::GameClient::State::Disconnected; }));
		CHECK(noProfile.disconnectReason() == 0xd);
	}

	// The login: FESL, the soldier, GP.
	net::OnlineSession session;
	session.setEndpoints(e);
	session.setCdKey(kKey);
	std::string error;
	CHECK(session.login("tester", "secret", "Tester", &error));
	const net::OnlineIdentity id = session.identity();
	CHECK(id.valid() && id.cdKey == kKey);

	// A logged-in soldier with someone else's name: the checksum differs (0x24).
	{
		net::GameClient liar;
		net::GameClientOptions options;
		options.playerName = "Second";
		options.cdKey = id.cdKey;
		options.profileId = static_cast<uint32_t>(id.profileId);
		options.nameChecksum = id.nameChecksum;
		options.auth = id.loginTicket;
		CHECK(liar.connect("127.0.0.1", hosted.gamePort(), options));
		CHECK(waitUntil(stepWith(liar), [&] { return liar.state() == net::GameClient::State::Disconnected; }));
		CHECK(liar.disconnectReason() == 0x24);
	}
	// A wrong CD key is let in: the retail server's gcd callback
	// (0x00642e70) goes on whatever the answer.
	{
		net::GameClient pirate;
		net::GameClientOptions options;
		options.playerName = id.soldier;
		options.cdKey = "ZZZZ-ZZZZ-ZZZZ-ZZZZ-ZZZZ";
		options.profileId = static_cast<uint32_t>(id.profileId);
		options.nameChecksum = id.nameChecksum;
		options.auth = id.loginTicket;
		CHECK(pirate.connect("127.0.0.1", hosted.gamePort(), options));
		CHECK(waitUntil(stepWith(pirate), [&] { return pirate.world().localPlayer() >= 0 || pirate.state() == net::GameClient::State::Disconnected; }, 8000));
		CHECK(pirate.world().localPlayer() >= 0 && pirate.state() != net::GameClient::State::Disconnected);
		pirate.disconnect();
	}

	// The server browser finds the host on the emulator's master.
	const auto list = net::fetchServerList(e.master.host, "", {"hostname", "hostport"}, 5, &error, e.master.port);
	CHECK(list && list->servers.size() == 1);
	const std::string port = list && !list->servers.empty() ? list->servers[0].fields.at("hostport") : "0";
	CHECK(port == std::to_string(hosted.gamePort()));

	// Joined with the login: accepted after ValidatePlayer, made (CreatePlayer
	// with our name), the level, then a spawn.
	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = id.soldier;
	options.cdKey = id.cdKey;
	options.profileId = static_cast<uint32_t>(id.profileId);
	options.nameChecksum = id.nameChecksum;
	options.auth = id.loginTicket;
	const int validationsBefore = emulator.validations();
	CHECK(client.connect("127.0.0.1", static_cast<uint16_t>(std::stoi(port)), options));
	const auto step = stepWith(client);
	CHECK(waitUntil(step, [&] { return client.world().localPlayer() >= 0 && client.world().hasLevel(); }, 8000));
	CHECK(emulator.validations() == validationsBefore + 1);
	const net::RemotePlayer* me = client.world().player(client.world().localPlayer());
	CHECK(me && me->name == "Tester");
	CHECK(waitUntil(step, [&] { return client.world().gameState() == 1; }, 8000));
	client.selectSpawnGroup(-1);
	dice::hfe::game::GameWorld::PlayerInfo info;
	const int player = client.world().localPlayer();
	CHECK(waitUntil(step, [&] { return round.playerInfo(player, info) && info.alive; }, 8000));
	CHECK(waitUntil(step, [&] { return client.world().hasControlObject(); }, 8000));
	client.disconnect();
	session.logout();
}
