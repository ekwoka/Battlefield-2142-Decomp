#include "HostedGame.h"

#include "Net/PlayerAuth.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameLogic.h"
#include "Net/BitStream.h"
#include "Net/GameClient.h"
#include "Net/GameProtocol.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>
#include <utility>

namespace dice::hfe::host {

namespace {

constexpr float kDegreesPerMouseLookUnit = 5.0f; // 0x0058a6d0's turn per PIMouseLook unit
constexpr float kTickSeconds = 1.0f / 30;
// The ghost block's header (0x00582cd0 reserves 0x2a bits: present, the
// tick, the count, the record bit).
constexpr size_t kGhostHeaderBits = 42;

net::GameEvent eventOf(uint32_t type, std::vector<uint32_t> values, std::vector<std::string> bytes = {})
{
	net::GameEvent e;
	e.type = type;
	e.values = std::move(values);
	e.bytes = std::move(bytes);
	return e;
}

float settingFloat(const world::ObjectTemplate& t, const char* key, float fallback)
{
	const auto it = t.settings().find(key);
	return it == t.settings().end() ? fallback : static_cast<float>(std::atof(it->second.c_str()));
}

} // namespace

net::Quat quaternionFromYawPitchRoll(float yaw, float pitch, float roll)
{
	// The column matrix R = M^T (M acts on row vectors).
	const physics::Mat3 m = physics::Mat3::fromYawPitchRoll({yaw, pitch, roll});
	const auto r = [&m](int i, int j) {
		const physics::Vec3& row = m.rows[static_cast<size_t>(j)];
		return i == 0 ? row.x : i == 1 ? row.y : row.z;
	};
	net::Quat q;
	const float trace = r(0, 0) + r(1, 1) + r(2, 2);
	if (trace > 0) {
		const float s = std::sqrt(trace + 1.0f) * 2;
		q.w = 0.25f * s;
		q.x = (r(2, 1) - r(1, 2)) / s;
		q.y = (r(0, 2) - r(2, 0)) / s;
		q.z = (r(1, 0) - r(0, 1)) / s;
	} else if (r(0, 0) > r(1, 1) && r(0, 0) > r(2, 2)) {
		const float s = std::sqrt(1.0f + r(0, 0) - r(1, 1) - r(2, 2)) * 2;
		q.w = (r(2, 1) - r(1, 2)) / s;
		q.x = 0.25f * s;
		q.y = (r(0, 1) + r(1, 0)) / s;
		q.z = (r(0, 2) + r(2, 0)) / s;
	} else if (r(1, 1) > r(2, 2)) {
		const float s = std::sqrt(1.0f + r(1, 1) - r(0, 0) - r(2, 2)) * 2;
		q.w = (r(0, 2) - r(2, 0)) / s;
		q.x = (r(0, 1) + r(1, 0)) / s;
		q.y = 0.25f * s;
		q.z = (r(1, 2) + r(2, 1)) / s;
	} else {
		const float s = std::sqrt(1.0f + r(2, 2) - r(0, 0) - r(1, 1)) * 2;
		q.w = (r(1, 0) - r(0, 1)) / s;
		q.x = (r(0, 2) + r(2, 0)) / s;
		q.y = (r(1, 2) + r(2, 1)) / s;
		q.z = 0.25f * s;
	}
	if (q.w < 0) {
		q = {-q.x, -q.y, -q.z, -q.w};
	}
	return q;
}

void yawPitchRollFromQuaternion(const net::Quat& q, float& yaw, float& pitch, float& roll)
{
	const float x = q.x, y = q.y, z = q.z, w = q.w;
	const float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
		{2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
		{2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
	physics::Mat3 m;
	for (int row = 0; row < 3; ++row) {
		m.rows[static_cast<size_t>(row)] = {r[0][row], r[1][row], r[2][row]};
	}
	const physics::Vec3 angles = m.yawPitchRoll();
	yaw = angles.x;
	pitch = angles.y;
	roll = angles.z;
}

HostedGame::HostedGame(game::GameWorld& round, game::GameLogic& logic, world::ObjectTemplateManager& templates,
	HostSetup setup)
	: m_round(&round), m_logic(&logic), m_templates(&templates), m_setup(std::move(setup))
{
	// The voters: the clients' players (bots don't vote).
	m_votes.players = [this] {
		std::vector<game::Votes::Voter> out;
		for (const auto& [id, client] : m_clients) {
			if (client.player >= 0 && !client.full) {
				out.push_back({client.player, client.team, true});
			}
		}
		return out;
	};
	m_votes.kickable = [this](int player) {
		for (const auto& [id, client] : m_clients) {
			if (client.player == player) {
				return true;
			}
		}
		return false;
	};
	m_votes.commander = [this](int team) { return m_round->commander(team); };
	m_server.onJoined = [this](int client) { joined(client); };
	// A delivered packet with a fresh record makes its slot the client's
	// (0x005830e0: +0x20f0 = the packet's slot, for the record's ghost).
	m_server.onNotified = [this](int id, uint32_t sequence, bool delivered) {
		const auto found = m_clients.find(id);
		if (found == m_clients.end()) {
			return;
		}
		Client& client = found->second;
		ghostLost(client, sequence, delivered);
		const auto fresh = client.freshIn.find(sequence);
		if (fresh == client.freshIn.end()) {
			return;
		}
		if (delivered) {
			client.ackedSlot = fresh->second;
		}
		client.freshIn.erase(fresh);
	};
	m_server.onLeft = [this](int client) { left(client); };
	m_server.onPlayerInfo = [this](int client) { m_loginChecks.push_back(client); };
	m_server.onEvent = [this](int client, const net::GameEvent& e) { event(client, e); };
	m_server.onMove = [this](int client, const net::PlayerMove& move) {
		if (const auto it = m_clients.find(client); it != m_clients.end()) {
			// The moves wait to be stepped one a tick; past four the oldest
			// go (the buffer the original keeps per player; its depth here
			// is OpenBattlefield2's reading of BF2's, not traced in BF2142.exe).
			it->second.moves.push_back(move);
			while (it->second.moves.size() > 4) {
				it->second.moves.pop_front();
				++it->second.movesDropped;
			}
		}
	};
}

HostedGame::~HostedGame()
{
	stopAutoDemo();
	stopDemo();
	for (const auto& [id, client] : m_clients) {
		if (onLog) {
			onLog("client " + std::to_string(id) + "'s moves: " + std::to_string(client.movesDropped) + " dropped, "
				+ std::to_string(client.ticksStarved) + " ticks without one");
		}
	}
	m_server.stop();
}

void registerNetworkableInfoConsoleMethods(Console& console)
{
	const uint32_t access = ConsoleAccess::Anyone;
	const auto lower = [](std::string text) {
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	};
	const auto active = [lower]() -> net::NetworkableInfoTable::Info* {
		net::NetworkableInfoTable& t = net::networkableInfos();
		const auto it = t.infos.find(lower(t.active));
		return it != t.infos.end() ? &it->second : nullptr;
	};
	// A name already made is refused (the manager's slot 0x2c(0)).
	bindMethod(console, "NetworkableInfo.createNewInfo", access, [lower](std::string name) {
		net::NetworkableInfoTable& t = net::networkableInfos();
		t.active = name;
		t.infos.emplace(lower(name), net::NetworkableInfoTable::Info {});
	});
	bindMethod(console, "NetworkableInfo.setPredictionMode", access, [active, lower](std::string mode) {
		if (net::NetworkableInfoTable::Info* i = active()) {
			i->predictionMode = lower(mode) == "pmlinear" ? 1 : 0;
		}
	});
	// c_NIGhostAlways is -1.0 (0x00669489); at most 255 (0x006ac060).
	console.registerConstant("c_NIGhostAlways", "-1");
	bindMethod(console, "NetworkableInfo.setBasePriority", access, [active](float value) {
		if (net::NetworkableInfoTable::Info* i = active()) {
			i->basePriority = std::min(value, 255.0f);
		}
	});
	const auto flagList = [lower](std::vector<std::string>& list, bool on) {
		const std::string name = net::networkableInfos().active;
		list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& s) { return lower(s) == lower(name); }),
			list.end());
		if (on) {
			list.push_back(name);
		}
	};
	bindMethod(console, "NetworkableInfo.setIsUnique", access,
		[flagList](bool on) { flagList(net::networkableInfos().unique, on); });
	bindMethod(console, "NetworkableInfo.setForceNetworkableId", access,
		[flagList](bool on) { flagList(net::networkableInfos().forceNetworkableId, on); });
}

bool HostedGame::start()
{
	net::GameServerOptions options;
	options.port = m_setup.gamePort;
	options.loopbackOnly = m_setup.loopbackOnly;
	options.password = m_setup.password;
	// The transport takes ten over sv.maxPlayers (0x00688300): the join
	// decides the rest (reserved slots).
	options.maxClients = m_setup.maxPlayers + 10;
	if (const net::BanList* bans = m_setup.bans) {
		options.banned = [bans](const std::string& address) { return bans->bannedAddress(address); };
	}
	if (!m_server.start(options)) {
		return false;
	}
	// 0x004aff80: pure when the mod has std_archive.md5 (0x004adb60; the
	// settings' content check flag, DAT_00a3fcc0 +0x54).
	if (!m_setup.contentRoot.empty() && m_content.loadCommon(m_setup.contentRoot, m_setup.modDirectory) && onLog) {
		onLog("pure server: clients' content is checked");
	}
	startContentLevel();
	if (m_setup.queryPort != 0 && !m_query.start(m_setup.queryPort, m_setup.loopbackOnly) && onLog) {
		onLog("no query port " + std::to_string(m_setup.queryPort) + ": the server isn't listed on the LAN");
	}
	return true;
}

bool HostedGame::spectatorCamera(int player, int& mode, int& follow) const
{
	for (const auto& [id, client] : m_clients) {
		if (client.player == player) {
			mode = client.cameraMode;
			follow = client.cameraFollow;
			return true;
		}
	}
	return false;
}

size_t HostedGame::clientCount() const { return m_clients.size(); }

bool HostedGame::kickPlayer(int player, bool ban, uint32_t reason)
{
	for (const auto& [id, client] : m_clients) {
		if (client.player != player) {
			continue;
		}
		// KickBan (0x0050cf30): the player, banned, a byte.
		sendToPlaying(eventOf(net::kEvKickBan, {static_cast<uint32_t>(player) & 0xff, ban ? 1u : 0u, reason & 0xff}));
		m_server.sendTick([](int) { return net::GhostBlock {}; }); // the event goes before the connection ends
		const int connection = id;
		if (onLog) {
			onLog("player " + std::to_string(player) + (ban ? " banned" : " kicked"));
		}
		m_server.kick(connection, reason); // onLeft: left()
		return true;
	}
	return false;
}

std::vector<HostedGame::ClientPlayer> HostedGame::clientPlayers() const
{
	std::vector<ClientPlayer> out;
	for (const auto& [id, client] : m_clients) {
		ClientPlayer p;
		p.player = client.player;
		p.name = client.name;
		if (const net::GameServer::ClientInfo* info = m_server.client(id)) {
			p.address = info->address;
			p.cdKeyHash = info->cdKeyHash;
		}
		out.push_back(p);
	}
	return out;
}

void HostedGame::sayAll(const std::string& text)
{
	net::ChatMessage line;
	line.channel = net::kChatServerMessage;
	line.source = 1;
	line.player = 0xff;
	line.text = text;
	for (const net::GameEvent& out : net::stringBlockEvents(net::encodeChat(line))) {
		sendToPlaying(out);
	}
	if (onLog) {
		onLog("server: " + text);
	}
}

void HostedGame::sayTeam(int team, const std::string& text)
{
	net::ChatMessage line;
	line.channel = net::kChatServerTeamMessage;
	line.source = 1;
	line.player = 0xff;
	line.text = text;
	const std::vector<net::GameEvent> events = net::stringBlockEvents(net::encodeChat(line));
	for (const auto& [id, client] : m_clients) {
		if (client.team == team && client.stage == Client::Stage::Playing) {
			for (const net::GameEvent& out : events) {
				sendTo(id, out);
			}
		}
	}
}

void HostedGame::sendRemoteFeedback(int player, const std::string& text)
{
	// 6/0x13 to the player's client: -1 (its console) and the text, as
	// many messages as the text needs (a PostRemote payload holds 52 bytes).
	const std::string reply = "rcon: " + text;
	for (const auto& [id, client] : m_clients) {
		if (client.player != player) {
			continue;
		}
		for (size_t at = 0; at < reply.size() || at == 0; at += 48) {
			sendTo(id, net::makePostRemote(net::kMsgNetworkCategory, net::kNeRemoteConsoleFeedback,
				net::int32Payload(-1) + reply.substr(at, 48)));
			if (reply.empty()) {
				break;
			}
		}
	}
}

void HostedGame::changeLevel(game::GameWorld& round, game::GameLogic& logic, world::ObjectTemplateManager& templates,
	HostSetup setup)
{
	stopAutoDemo(); // the round's restart (0x004ba0c0 -> 0x00585920)
	stopDemo();
	m_lastRoundState = -1;
	m_overheadCameras[1] = m_overheadCameras[2] = 0; // the old round's objects
	m_round = &round;
	m_logic = &logic;
	m_templates = &templates;
	m_attached = true;
	m_setup.level = setup.level;
	m_setup.gameMode = setup.gameMode;
	m_setup.mapSize = setup.mapSize;
	startContentLevel();
	// The old round's players go (DestroyPlayer); the new round makes its own.
	for (const auto& [player, seen] : m_players) {
		sendToPlaying(eventOf(net::kEvDestroyPlayer, {static_cast<uint32_t>(player) & 0xff}));
	}
	m_static.clear();
	m_layouts.clear();
	m_ghosts.clear();
	m_players.clear();
	m_names.clear();
	m_motion.clear();
	m_snapshot.clear();
	m_reloadsSeen.clear();
	m_stateCache.clear();
	m_nextGhost = kFirstGhost;
	m_spawnGroups.clear();
	m_squadSpawnGroups.clear();
	m_vehicleSpawnGroups.clear();
	m_requestsSent.clear();
	m_ordersSent.clear();
	m_offerSent = {-1, -1, -1};
	m_roundEndSent = false;
	m_endOfRoundSerialSent = 0;
	for (auto& [id, client] : m_clients) {
		// A new round: a new player of the client's team, a new database.
		const int team = client.team ? client.team : 1;
		client.player = m_round->addPlayer(client.name, team, false);
		m_names[client.player] = client.name;
		if (const auto& kits = m_logic->team(team).kits; !kits.empty()) {
			m_round->setKit(client.player, kits.front().name, kits.front().soldier);
		}
		client.created.clear();
		client.ghosts.clear();
		client.carried.clear();
		client.lostRemovals.clear();
		client.rescoreIn = 0;
		client.hasEye = false;
		client.moves.clear();
		client.stepping = client.stepped = -1;
		client.spawnRequested = false;
		if (client.stage != Client::Stage::Connected) {
			client.stage = Client::Stage::Loading;
			sendLevel(id);
		}
	}
	if (onLog) {
		onLog("level " + m_setup.level + " (" + m_setup.gameMode + ") for " + std::to_string(m_clients.size()) + " clients");
	}
}

void HostedGame::sendLevel(int id)
{
	const auto block = [&](uint32_t type, const std::string& data) {
		sendTo(id, net::makeDataBlockStart(type, static_cast<uint32_t>(data.size())));
		for (size_t at = 0; at < data.size(); at += 63) {
			sendTo(id, net::makeDataBlockChunk(data.substr(at, 63)));
		}
	};
	// The map list (0x00441410: every map, the current one's index).
	std::vector<net::RemoteWorld::MapEntry> maps;
	int current = 0;
	if (m_setup.maps && m_setup.maps->count() > 0) {
		for (const net::MapListEntry& map : m_setup.maps->entries()) {
			maps.push_back({map.level, map.gameMode, static_cast<uint32_t>(map.size)});
		}
		current = m_setup.maps->current();
	} else {
		maps.push_back({m_setup.level, m_setup.gameMode, static_cast<uint32_t>(m_setup.mapSize)});
	}
	block(5, net::RemoteWorld::encodeMapList(maps, current));
	net::RemoteWorld::LevelInfo level;
	level.gameMode = m_setup.gameMode;
	level.levelDirectory = "levels/" + m_setup.level + "/";
	level.mapName = m_setup.level;
	level.maxPlayers = m_setup.mapSize;
	level.commander = true;
	level.seed = m_ordinal;
	block(2, net::RemoteWorld::encodeLevel(level));
}

void HostedGame::startContentLevel()
{
	// 0x004aefe0: the level's ordinal (rand() % 10 just after srand of the
	// clock's milliseconds: msvcrt's first rand(), without touching ours)
	// and the level's archive.md5 (0x004adda0, +0x1ad).
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	const uint32_t seed = static_cast<uint32_t>(ms) * 214013u + 2531011u;
	m_ordinal = static_cast<int>((seed >> 16) & 0x7fff) % 10;
	if (m_content.pure()) {
		m_content.loadLevel(m_setup.contentRoot, "levels/" + m_setup.level + "/", m_setup.modDirectory);
	}
}

void HostedGame::checkContent(int id, const net::GameEvent& e)
{
	// Game server slot 51 (0x004aec40, from the event's server handler
	// 0x005123a0): a pure server compares the fingerprint with its own.
	const auto it = m_clients.find(id);
	if (!m_content.pure() || it == m_clients.end() || it->second.stage == Client::Stage::Connected) {
		return;
	}
	Client& client = it->second;
	if (std::any_of(m_kickLater.begin(), m_kickLater.end(), [id](const auto& k) { return k.first == id; })) {
		return;
	}
	net::ContentDigests digests;
	if (net::parseContentCheck(e, digests) && m_content.matches(m_ordinal, digests)) {
		client.contentChecked = true;
		return;
	}
	if (onLog) {
		onLog("dep: kicking client " + std::to_string(id) + " (" + client.name + ") for using modified data");
	}
	m_kickLater.push_back({id, net::kKickModifiedContent}); // the game server's kick list (+0x20)
}

std::string HostedGame::nameOf(int player) const
{
	if (const auto it = m_names.find(player); it != m_names.end()) {
		return it->second;
	}
	game::GameWorld::PlayerInfo info;
	return m_round->playerInfo(player, info) ? info.name : std::string();
}

net::ServerReport HostedGame::report() const
{
	// The keys 0x0044xxxx's qr2 callbacks give for a hosted game.
	net::ServerReport r;
	int players = 0;
	for (const auto& p : m_round->playerList()) {
		net::ServerReport::Player player;
		player.name = nameOf(p.id);
		player.team = p.team;
		player.deaths = p.deaths;
		player.score = p.kills;
		r.players.push_back(player);
		++players;
	}
	r.keys = {{"hostname", m_setup.serverName}, {"gamename", "stella"}, {"gamever", "1.10.112.0"},
		{"hostport", std::to_string(m_server.port())}, {"mapname", m_setup.level}, {"gametype", m_setup.gameMode},
		{"gamevariant", "bf2142"}, {"numplayers", std::to_string(players)}, {"numteams", "2"},
		{"maxplayers", std::to_string(m_setup.maxPlayers)}, {"gamemode", "openplaying"},
		{"password", m_setup.password.empty() ? "0" : "1"}};
	addRuleKeys(r);
	r.teams = {m_logic->team(1).name, m_logic->team(2).name};
	return r;
}

// The server rules' keys. The host's server key callback (0x00578bb0)
// answers each key from the server info object (0x00a75560, field slot 1
// 0x00642210), which 0x006423d0 fills from the sv settings (DAT_00a201c0)
// before each report: "%d" or "%f" of a setting, "1" / "0" for a switch.
// A ranked server (sv.ranked, +0x26d) reports its fixed rules: a spawn
// time of 15 (0x00430f60), a ticket ratio of 100 (0x00430fa0) and a team
// ratio of 100 (0x00430fc0). A game without settings reports the defaults.
void HostedGame::addRuleKeys(net::ServerReport& r) const
{
	const net::ServerSettings defaults;
	const net::ServerSettings& sv = m_setup.settings ? *m_setup.settings : defaults;
	const auto decimal = [](int value) { return std::to_string(value); };
	const auto real = [](float value) {
		char text[32];
		std::snprintf(text, sizeof(text), "%f", static_cast<double>(value));
		return std::string(text);
	};
	const auto flag = [](bool on) { return std::string(on ? "1" : "0"); };
	// 0x00430a00: a ranked game also needs GameSpyRanked, which a LAN
	// host never has, so only sv.ranked's fixed rules apply here.
	const bool rankedGame = false;
	r.keys["roundtime"] = decimal(sv.roundsPerMap);                         // field 9, +0x11c
	r.keys["timelimit"] = decimal(sv.timeLimit);                            // field 0xb, +0x120
	r.keys["bf2142_ranked"] = flag(rankedGame);                             // field 0xe, 0x00430a00
	r.keys["bf2142_autorec"] = flag(sv.autoRecord);                         // field 0x11, +0x16d
	r.keys["bf2142_voip"] = flag(sv.voipEnabled);                           // field 0x14 (the voip slot 0x254)
	r.keys["bf2142_autobalanced"] = flag(sv.autoBalanceTeam);               // field 0x15, +0x1e8
	r.keys["bf2142_friendlyfire"] = flag(sv.soldierFriendlyFire >= 100);    // field 0x16, +0x128 at 100 or more
	r.keys["bf2142_tkmode"] = sv.tkPunishEnabled ? "Punish" : "No Punish"; // field 0x17, +0x140
	r.keys["bf2142_startdelay"] = decimal(sv.startDelay);                   // field 0x18, +0x108
	r.keys["bf2142_spawntime"] = real(sv.ranked ? 15.0f : sv.spawnTime);    // field 0x19, 0x00430f60
	r.keys["bf2142_sponsortext"] = sv.sponsorText;                          // field 0x1a, +0x1f0
	r.keys["bf2142_scorelimit"] = decimal(sv.scoreLimit);                   // field 8, +0x124
	r.keys["bf2142_ticketratio"] = decimal(sv.ranked ? 100 : sv.ticketRatio); // field 0x1c, 0x00430fa0
	r.keys["bf2142_teamratio"] = real(sv.ranked ? 100.0f : sv.teamRatioPercent); // field 0x1d, 0x00430fc0
	r.keys["bf2142_team1"] = m_logic->team(1).name;                         // field 0x1e (slot 0x48)
	r.keys["bf2142_team2"] = m_logic->team(2).name;                         // field 0x1f (slot 0x48(2))
	r.keys["bf2142_mapsize"] = decimal(m_setup.mapSize);                    // field 0x22 (slot 0x100)
	r.keys["bf2142_reservedslots"] = decimal(sv.numReservedSlots);         // field 0x26, 0x00430d80 (+0x274)
	r.keys["bf2142_maxrank"] = flag(false);                                 // field 0x27: ranked games only
	r.keys["bf2142_allow_spectators"] = flag(!rankedGame && sv.allowSpectators); // field 0x2d, +0x290
}

// --- joining

void HostedGame::joined(int id)
{
	// A banned CD key (admin.banPlayerKey) goes as soon as its hash is known.
	if (const net::GameServer::ClientInfo* info = m_server.client(id);
		info && m_setup.bans && m_setup.bans->bannedKey(info->cdKeyHash)) {
		if (onLog) {
			onLog("client " + std::to_string(id) + " (" + info->address + ") is banned");
		}
		m_server.kick(id, 0x16);
		return;
	}
	// The retail pre-authorisation: the settings and the map list now, the
	// player once its info has been checked (playerInfo).
	const net::GameServer::ClientInfo* connection = m_server.client(id);
	const bool afterLogin = m_setup.login.required && connection && connection->hasPlayerInfo;
	if (m_setup.login.required && !afterLogin) {
		m_awaitingLogin.insert(id);
		sendSettings(id);
		if (onLog) {
			onLog("client " + std::to_string(id) + " (" + connection->address + ") waits for its login to be checked");
		}
		return;
	}
	// A full server (0x004ae6a0: the players at sv.maxPlayers): decided when
	// the client's name comes (reservedSlotCheck).
	int humans = 0;
	for (const auto& [other, c] : m_clients) {
		humans += other != id && c.player >= 0 ? 1 : 0;
	}
	const bool full = humans >= m_setup.maxPlayers;
	Client& client = m_clients[id];
	client.full = full;
	// A team with fewer players.
	int counts[3] {};
	for (const auto& p : m_round->playerList()) {
		if (p.team == 1 || p.team == 2) {
			++counts[p.team];
		}
	}
	int team = counts[1] <= counts[2] ? 1 : 2;
	// A left client's id is free again (GameWorld::removePlayer): the
	// lowest free one is taken.
	const std::string name = afterLogin ? connection->playerInfo.name : "Player" + std::to_string(id);
	client.player = m_round->addPlayer(name, team, false);
	client.name = name;
	client.team = team;
	m_names[client.player] = client.name;
	if (const auto& kits = m_logic->team(team).kits; !kits.empty()) {
		m_round->setKit(client.player, kits.front().name, kits.front().soldier);
	}
	client.stage = Client::Stage::Loading;
	sendJoinBlocks(id, !afterLogin);
	if (onLog) {
		onLog("client " + std::to_string(id) + " (" + m_server.client(id)->address + ") joined as player "
			+ std::to_string(client.player) + ", team " + std::to_string(team));
	}
}

void HostedGame::playerInfo(int id)
{
	const net::GameServer::ClientInfo* info = m_server.client(id);
	if (!m_setup.login.required || !info || !m_awaitingLogin.erase(id)) {
		return;
	}
	const net::PlayerInfoBlock& p = info->playerInfo;
	const auto refuse = [&](uint32_t reason, const std::string& why) {
		if (onLog) {
			onLog("client " + std::to_string(id) + " (" + p.name + ", profile " + std::to_string(p.profileId) + ") refused: " + why);
		}
		m_server.kick(id, reason);
	};
	// 0x004ae6a0: the name against the checksum the login kept.
	if (net::nameChecksum(p.name) != p.nameChecksum) {
		refuse(0x24, "the name's checksum differs");
		return;
	}
	// A ranked server takes no player without a profile (reason 0xd).
	if (p.profileId <= 0 || static_cast<int32_t>(info->profileId) != p.profileId) {
		refuse(0xd, "no logged-in profile");
		return;
	}
	// GameSpy's CD-key service (0x00642f90 -> 0x007184b0, game 0x52c).
	std::string why;
	if (!net::cdKeyAuthenticate(m_setup.login.cdKeyHost, m_setup.login.cdKeyPort, m_server.challenge(), info->cdKeyResponse, 0,
			&why)) {
		refuse(0x14, "CD key: " + why); // the reason is the rewrite's (the retail server's gcd callback, 0x00642e70, isn't traced)
		return;
	}
	// The stats service's ValidatePlayer (0x00584c00).
	std::string body;
	const std::string path = net::validatePlayerPath(p.auth, p.name, p.profileId);
	if (!net::httpGet(m_setup.login.statsHost, m_setup.login.statsPort, path, body)) {
		refuse(0x16, "the stats service did not answer");
		return;
	}
	if (const std::string result = net::validatePlayerResult(body); result != "Ok") {
		refuse(0x16, "ValidatePlayer: " + (result.empty() ? std::string("no result") : result)); // playerconnect.py bans
		return;
	}
	if (onLog) {
		onLog("client " + std::to_string(id) + " logged in as " + p.name + " (profile " + std::to_string(p.profileId) + ")");
	}
	joined(id);
}

void HostedGame::sendJoinBlocks(int id, bool withSettings)
{
	if (withSettings) {
		sendSettings(id);
	}
	sendLevel(id);
}

void HostedGame::sendSettings(int id)
{
	// The settings (block 0), the map list (5) and the level (2).
	net::RemoteWorld::ServerSettings settings;
	settings.name = m_setup.serverName;
	settings.initial = true;
	settings.gamePort = m_server.port();
	settings.gameSpyPort = m_setup.queryPort;
	settings.playersNeededToStart = 1;
	// The radio's spam limits (the client's spam check, player slot 0x340):
	// the sv settings' defaults without settings of our own.
	const net::ServerSettings defaults;
	settings.radioSpamInterval = defaults.radioSpamInterval;
	settings.radioMaxSpamFlagCount = defaults.radioMaxSpamFlagCount;
	settings.radioBlockedDurationTime = defaults.radioBlockedDurationTime;
	if (const net::ServerSettings* sv = m_setup.settings) {
		// 0x00515bc0's fields from the sv settings (the welcome message in
		// up to three 256-byte pieces).
		for (size_t at = 0; at < sv->welcomeMessage.size() && settings.welcome.size() < 3; at += 256) {
			settings.welcome.push_back(sv->welcomeMessage.substr(at, 256));
		}
		settings.voipEnabled = sv->voipEnabled;
		settings.allowNoseCam = sv->allowNoseCam;
		settings.hitIndicator = sv->hitIndicator;
		settings.tkPunishByDefault = sv->tkPunishByDefault;
		settings.allowFreeCam = sv->allowFreeCam;
		settings.allowExternalViews = sv->allowExternalViews;
		settings.voipServerPort = static_cast<uint32_t>(sv->voipServerPort);
		settings.voipClientPort = static_cast<uint32_t>(sv->voipBFClientPort);
		settings.voipQuality = sv->voipQuality;
		settings.radioSpamInterval = sv->radioSpamInterval;
		settings.radioMaxSpamFlagCount = sv->radioMaxSpamFlagCount;
		settings.radioBlockedDurationTime = sv->radioBlockedDurationTime;
		settings.votingEnabled = sv->votingEnabled;
		settings.playersNeededToStart = static_cast<uint32_t>(std::max(sv->numPlayersNeededToStart, 0));
		settings.friendlyFireWithMines = sv->friendlyFireWithMines;
		settings.allowTitanMovement = sv->allowTitanMovement;
	}
	const auto block = [&](uint32_t type, const std::string& data) {
		sendTo(id, net::makeDataBlockStart(type, static_cast<uint32_t>(data.size())));
		for (size_t at = 0; at < data.size(); at += 63) {
			sendTo(id, net::makeDataBlockChunk(data.substr(at, 63)));
		}
	};
	settings.contentCheck = m_content.pure();
	block(0, net::RemoteWorld::encodeSettings(settings));
}

void HostedGame::sendTo(int client, const net::GameEvent& event)
{
	if (client == kDemoClient) {
		m_demoEvents.queue(event);
	} else {
		m_server.send(client, event);
	}
}

// --- demos (the server's recordDemo, 0x004afcc0; stopRecording 0x004afdb0)

bool HostedGame::recordDemo(const std::string& path, const std::string& cameraPath)
{
	if (m_demo.isOpen()) {
		return false; // "already recording"
	}
	net::DemoHeader header;
	header.serverName = m_setup.serverName;
	header.level = m_setup.level;
	header.gameMode = m_setup.gameMode;
	if (!m_demo.open(path, header)) {
		return false;
	}
	// The demo connection 0x81 (0x004ac920): no player, the join's data
	// (blocks 0 and 5 then the level), the load and database steps
	// (0x004ae6a0, 0x004a7c00, 0x004a7c80, 0x004af710), each flushed into
	// the file: the first two at once (-1), the rest by the tick; then the
	// marker (-2, 0).
	m_demoEvents.clear();
	m_demoSequence = 0;
	Client& demo = m_clients[kDemoClient];
	demo = {};
	demo.player = -1;
	demo.name = "DEMORECORD";
	demo.stage = Client::Stage::Loading;
	sendJoinBlocks(kDemoClient);
	flushDemo(-1, false);
	event(kDemoClient, net::makePostRemote(net::kMsgNetworkCategory, net::kNeLoadComplete));
	flushDemo(-1, false);
	event(kDemoClient, net::makePostRemote(net::kMsgNetworkCategory, net::kNeDatabaseComplete));
	flushDemo(static_cast<int32_t>(m_tick), false);
	m_demo.marker(0);
	if (onLog) {
		onLog("recording demo " + path);
	}
	// 0x005172f0: with a local player, the camera too (0x005171f0).
	net::CameraKeyframe probe;
	if (!cameraPath.empty() && localCamera && localCamera(probe)) {
		m_cameraPath = cameraPath;
		m_cameraRecorder.restart();
		m_recordingCamera = true;
	}
	return true;
}

// The AutoRecorder's file name (0x00585620): strftime("%Y-%m-%d %H:%M:%S")
// of the local time (0x00669050) with every character but 0-9 and '_' as
// '_', under GSModDirectory's demos/.
std::string autoDemoPath(const std::string& modDirectory, std::time_t now)
{
	char stamp[64] = {};
	std::tm local {};
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
	for (char* c = stamp; *c; ++c) {
		if (!((*c >= '0' && *c <= '9') || *c == '_')) {
			*c = '_';
		}
	}
	return modDirectory + "/demos/auto_" + stamp + ".bf2142demo";
}

// 0x00585860: once the round plays, with sv.autoRecord and no demo running.
void HostedGame::startAutoDemo()
{
	if (m_demo.isOpen() || !m_setup.settings || !m_setup.settings->autoRecord) {
		return;
	}
	const std::string path = autoDemoPath(m_setup.modDirectory, std::time(nullptr));
	std::error_code ignored;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ignored);
	if (recordDemo(path)) {
		m_autoDemo = path;
	}
}

// 0x00585920: the demo stops; the hook (sv.autoDemoHook, run by the
// recorder's thread 0x005853b0 with the file in quotes, ShellExecuteExA
// "open") gets it unless the last one is still being handled.
void HostedGame::stopAutoDemo()
{
	if (m_autoDemo.empty()) {
		return;
	}
	stopDemo();
	const std::string file = m_autoDemo;
	m_autoDemo.clear();
	const std::string hook = m_setup.settings ? m_setup.settings->autoDemoHook : std::string();
	if (m_hookBusy->load()) {
		if (onLog) {
			onLog("dep: cannot call auto-recording hook for " + file + ", last file (" + m_lastAutoDemo
				+ ") is still processing");
		}
		return;
	}
	if (onLog) {
		onLog("Uploading demo file " + file + " ...");
	}
	m_lastAutoDemo = file;
	std::error_code missing;
	if (hook.empty() || !std::filesystem::is_regular_file(hook, missing)) {
		return; // no hook program (the original's launch fails: "failed to launch hook program")
	}
	m_hookBusy->store(true);
	std::thread([busy = m_hookBusy, command = "\"" + hook + "\" \"" + file + "\""] {
		const int result = std::system(command.c_str());
		if (result != 0) {
			std::fprintf(stderr, "dep: autodemo hook program failed w/ non-zero exit code\n");
		}
		busy->store(false);
	}).detach();
}

void HostedGame::stopDemo()
{
	if (!m_demo.isOpen()) {
		return;
	}
	m_demo.close(static_cast<int32_t>(m_tick));
	if (m_recordingCamera) {
		m_recordingCamera = false;
		if (!net::writeCameraFile(m_cameraPath, m_cameraRecorder.frames()) && onLog) {
			onLog("cannot write " + m_cameraPath);
		}
	}
	m_clients.erase(kDemoClient);
	m_demoEvents.clear();
	if (onLog) {
		onLog("stopping recording");
	}
}

void HostedGame::flushDemo(int32_t time, bool ghosts)
{
	// 0x004b02a0's demo branch: the payload goes to the file, not the
	// socket, and counts as delivered at once (0x0057ab30): no resends.
	for (int guard = 0; guard < 64; ++guard) {
		net::GamePayload payload;
		const std::optional<net::ReliableEventBlock> block = m_demoEvents.next();
		if (block) {
			payload.hasEvents = true;
			payload.events = *block;
			m_demoSequence = (m_demoSequence + 1) & 0x3f;
			m_demoEvents.sent(m_demoSequence, block->sequence);
			m_demoEvents.acknowledge(m_demoSequence, 1);
		}
		if (ghosts) {
			const net::DemoRates rates = net::demoRates(m_setup.settings ? m_setup.settings->demoQuality : 1);
			const size_t used = net::writeGamePayload(payload).size() * 8;
			const size_t cap = static_cast<size_t>(rates.packetBytes) * 8;
			const size_t demoBits = cap > used ? cap - used : 0;
			payload.ghosts = ghostsFor(kDemoClient, demoBits);
		}
		if (!block && !ghosts) {
			return;
		}
		const std::vector<uint8_t> bytes = net::writeGamePayload(payload, net::kDemoGhostStateLengthBits);
		if (!bytes.empty()) { // 0x00516950 writes no empty payload
			m_demo.write(time, bytes);
		}
		if (ghosts || m_demoEvents.idle()) {
			return;
		}
	}
}

// 0x004ae6a0's full-server rule: with no reserved slots, or as many
// reserved players in as there are slots, or a nick not on the list, the
// joiner is refused (2, full); else the connected player without a reserved
// nick with the highest player index is kicked (0x23) and the joiner stays.
// No slots are held back: the others fill the server to sv.maxPlayers. The
// nick is the name without a clan prefix (to its first space, 0x0043f7e0).
void HostedGame::sendVoteReplies(const std::vector<game::Votes::Reply>& replies)
{
	for (const game::Votes::Reply& r : replies) {
		uint32_t seconds = 0;
		std::memcpy(&seconds, &r.seconds, sizeof seconds);
		const net::GameEvent e = eventOf(net::kEvVote,
			{static_cast<uint32_t>(r.target) & 0xff, static_cast<uint32_t>(r.choice) & 0xff,
				static_cast<uint32_t>(r.yesCount) & 0xff, static_cast<uint32_t>(r.target) & 0xff,
				static_cast<uint32_t>(r.noCount) & 0xff, static_cast<uint32_t>(std::max(r.needed, 0)) & 0xff,
				static_cast<uint32_t>(r.voter) & 0xff, static_cast<uint32_t>(r.phase) & 0xff,
				static_cast<uint32_t>(r.kind) & 0xff, seconds});
		if (onVoteReply && (r.to < 0 || !clientHas(r.to))) {
			onVoteReply(r); // the host's own player's vote object
		}
		if (r.to < 0) {
			sendToPlaying(e);
			continue;
		}
		for (const auto& [id, client] : m_clients) {
			if (client.player == r.to) {
				sendTo(id, e);
			}
		}
	}
}

bool HostedGame::clientHas(int player) const
{
	for (const auto& [id, client] : m_clients) {
		if (client.player == player) {
			return true;
		}
	}
	return false;
}

void HostedGame::localVote(int player, int kind, int phase, int target, int choice)
{
	sendVoteReplies(m_votes.handle(player, kind, phase, target, choice));
}

bool HostedGame::reservedSlotCheck(int id)
{
	Client& client = m_clients[id];
	client.full = false;
	const auto nick = [](const std::string& name) {
		const size_t space = name.find(' ');
		return space == std::string::npos ? name : name.substr(space + 1);
	};
	const net::ReservedSlots* list = m_setup.reserved;
	const int slots = m_setup.settings ? m_setup.settings->numReservedSlots : 0;
	int reservedIn = 0;
	int bump = -1, bumpPlayer = -1;
	for (const auto& [other, c] : m_clients) {
		if (other == id || c.player < 0) {
			continue;
		}
		if (list && list->has(nick(c.name))) {
			++reservedIn;
		} else if (c.player > bumpPlayer) {
			bumpPlayer = c.player;
			bump = other;
		}
	}
	if (slots <= 0 || !list || reservedIn >= slots || !list->has(nick(client.name)) || bump < 0) {
		if (onLog) {
			onLog("client " + std::to_string(id) + " refused: the server is full");
		}
		m_kickLater.push_back({id, 2});
		return false;
	}
	if (onLog) {
		onLog("client " + std::to_string(bump) + " kicked for " + client.name + "'s reserved slot");
	}
	m_kickLater.push_back({bump, 0x23});
	return true;
}

void HostedGame::left(int id)
{
	m_awaitingLogin.erase(id);
	const auto it = m_clients.find(id);
	if (it == m_clients.end()) {
		return;
	}
	const int player = it->second.player;
	sendVoteReplies(m_votes.playerLeft(player));
	if (onLog) {
		onLog("client " + std::to_string(id) + "'s moves: " + std::to_string(it->second.movesDropped) + " dropped, "
			+ std::to_string(it->second.ticksStarved) + " ticks without one");
	}
	m_clients.erase(it);
	// The game server's destroyPlayer (0x004acfb0): the round lets the
	// player go (its soldier dies where it is, it leaves its squad and the
	// command, PlayerDisconnect), every client hears DestroyPlayer (event 8,
	// vtable 0x0091cb58) and the player's networkable ghost goes
	// (0x0067bb30); its id is free for the next to join.
	m_round->removePlayer(player);
	sendToPlaying(eventOf(net::kEvDestroyPlayer, {static_cast<uint32_t>(player) & 0xff}));
	m_players.erase(player);
	m_names.erase(player);
	if (onLog) {
		onLog("client " + std::to_string(id) + " left");
	}
}

net::GameEvent HostedGame::createPlayerEvent(const game::GameWorld::PlayerInfo& player, bool own, uint16_t ghost) const
{
	net::CreatePlayerInfo info;
	info.id = player.id;
	info.team = player.team;
	info.remote = !own;
	info.ai = player.ai;
	info.object = ghost;
	info.name = nameOf(player.id);
	info.flag2 = player.spectator; // 0x0050a7c0: player slot 0x3bc
	return net::makeCreatePlayer(info);
}

std::vector<net::GameEvent> HostedGame::createEvents(const Ghost& ghost) const
{
	const world::Object* object = m_round->objects().find(ghost.object);
	net::CreateObjectInfo info;
	info.templateId = ghost.objectTemplate->id();
	info.ghost = ghost.first;
	info.count = static_cast<uint32_t>(ghost.layout->size());
	if (const game::Projectile* p = projectileOf(ghost)) {
		info.team = std::clamp(p->team, 0, 3);
		info.hasPosition = info.hasRotation = true;
		info.position = {p->position.x, p->position.y, p->position.z};
		const float horizontal = std::sqrt(p->velocity.x * p->velocity.x + p->velocity.z * p->velocity.z);
		info.rotation = {std::atan2(p->velocity.x, p->velocity.z) * 57.29578f,
			std::atan2(p->velocity.y, horizontal) * 57.29578f, 0};
	} else if (object) {
		info.team = std::clamp(object->team(), 0, 3);
		info.hasPosition = info.hasRotation = true;
		info.position = {object->position().x, object->position().y, object->position().z};
		info.rotation = {object->rotation().x, object->rotation().y, object->rotation().z};
	}
	return {net::makeCreateObject(info)};
}

void HostedGame::sendDatabase(int id)
{
	Client& client = m_clients[id];
	// The players, the objects, who holds which soldier.
	for (const auto& p : m_round->playerList()) {
		const auto seen = m_players.find(p.id);
		if (seen != m_players.end()) {
			sendTo(id, createPlayerEvent(p, p.id == client.player, seen->second.ghost));
		}
	}
	for (const auto& [object, ghost] : m_ghosts) {
		for (const net::GameEvent& e : createEvents(ghost)) {
			sendTo(id, e);
		}
		client.created.insert(ghost.first);
	}
	for (const auto& [player, seen] : m_players) {
		if (seen.alive && seen.soldier) {
			game::GameWorld::PlayerInfo info;
			m_round->playerInfo(player, info);
			sendTo(id, eventOf(net::kEvHandlePickup, {static_cast<uint32_t>(player) & 0xff, ghostOfObject(info.kit),
				ghostOfObject(seen.soldier), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
		}
	}
	// The rounds stuck to an object (ServerConnection 0x0057dda0: those
	// with a parent, +0x34).
	for (const game::Projectile& p : m_round->projectiles()) {
		if (p.alive && p.stuck && p.stuckTo && m_stuckSent.count(p.serial)) {
			sendTo(id, stickyEvent(p));
		}
	}
	// The spawn groups and the squads' requests standing (their events, as
	// they went to the others).
	for (const auto& [group, info] : m_spawnGroups) {
		sendTo(id, net::makeCreateSpawnGroup(info));
	}
	for (const auto& [key, at] : m_requestsSent) {
		sendTo(id, requestEvent(std::get<0>(key), std::get<1>(key), std::get<2>(key), at));
	}
	// And the orders standing, the commander's answered ones with the answer.
	for (const auto& [key, order] : m_ordersSent) {
		const auto [team, squad, store] = key;
		net::SquadOrderInfo info;
		info.team = team;
		info.squad = squad;
		info.kind = order.kind;
		info.store = store;
		info.floats[0] = order.at.x;
		info.floats[1] = order.at.y;
		info.floats[2] = order.at.z;
		sendTo(id, net::makeSquadOrder(info));
		if (store == 1 && order.accepted) {
			sendTo(id, net::makeAcceptOrder({squad, team, true, store, order.kind}));
		}
	}
	sendTo(id, net::makePostRemote(net::kMsgNetworkCategory, net::kNeDatabaseComplete));
	sendTo(id, net::makePostRemote(net::kMsgNetworkCategory, net::kNeReset, net::int32Payload(0)));
	client.stage = Client::Stage::Database;
}

void HostedGame::sendToPlaying(const net::GameEvent& event)
{
	for (const auto& [id, client] : m_clients) {
		if (client.stage != Client::Stage::Loading && client.stage != Client::Stage::Connected) {
			sendTo(id, event);
		}
	}
}

// --- the clients' events

void HostedGame::event(int id, const net::GameEvent& e)
{
	Client& client = m_clients[id];
	const int player = client.player;
	switch (e.type) {
	case net::kEvPostRemote: {
		net::PostRemoteInfo info;
		if (!net::parsePostRemote(e, info) || info.category != net::kMsgNetworkCategory) {
			return;
		}
		switch (info.id) {
		case net::kNeLoadComplete: // 0x004a7c00
			if (client.stage == Client::Stage::Loading) {
				sendDatabase(id);
			}
			break;
		case net::kNeDatabaseComplete: { // 0x004af710: StartSimulation and the game state
			if (m_content.pure() && !client.contentChecked && id != kDemoClient) {
				// A pure server's client that sent no matching fingerprint
				// (the demo connection 0x81 is never checked: 0x004af710's
				// first test).
				if (onLog) {
					onLog("client " + std::to_string(id) + " sent no content check");
				}
				m_kickLater.push_back({id, net::kKickModifiedContent});
				return;
			}
			sendTo(id, net::makePostRemote(net::kMsgNetworkCategory, net::kNeStartSimulation));
			sendTo(id, net::makePostRemote(net::kMsgGameCategory, 1, net::int32Payload(1)));
			const uint32_t end = m_logic->timeLimit > 0 ? static_cast<uint32_t>(m_logic->timeLimit * 30) : 0;
			sendTo(id, eventOf(net::kEvBeginRound, {3, end, static_cast<uint32_t>(-1)}));
			client.stage = Client::Stage::Playing;
			break;
		}
		case net::kNeSelectSpawnGroup: { // 0x004c1d50's request
			// A squad's group: its leader, or the leader's beacon that holds
			// the squad's spawn point once set down; a vehicle's group: that
			// vehicle; else a control point's.
			const int group = info.payloadInt();
			game::GameWorld::PlayerInfo me;
			m_round->playerInfo(player, me);
			bool taken = false;
			for (const auto& [squad, id] : m_squadSpawnGroups) {
				if (id == group && squad == std::make_pair(me.team, me.squad)) {
					m_round->setSpawnChoice(player, m_round->squadBeacon(me.team, me.squad)
							? game::GameWorld::kSpawnOnBeacon
							: game::GameWorld::kSpawnOnLeader);
					taken = true;
				}
			}
			for (const auto& [vehicle, id] : m_vehicleSpawnGroups) {
				if (id == group) {
					m_round->setSpawnVehicle(player, vehicle);
					taken = true;
				}
			}
			if (!taken) {
				m_round->setSpawnChoice(player, group);
			}
			client.spawnRequested = true;
			break;
		}
		case net::kNeSelectTeam: { // 0x004c1dc0's request; 0x004b0690 case 7 -> the game logic's slot 0x60
			const int team = info.payloadInt();
			if (m_round->setPlayerTeam(player, team)) {
				client.team = team;
				if (const auto& kits = m_logic->team(team).kits; !kits.empty()) {
					m_round->setKit(player, kits.front().name, kits.front().soldier);
				}
				if (onLog) {
					onLog("player " + std::to_string(player) + " goes to team " + std::to_string(team));
				}
			}
			break;
		}
		case net::kNeRemoteConsoleCommand: // "rcon <command>" typed in the client's console
			if (onRemoteCommand) {
				onRemoteCommand(player, info.payload.substr(0, info.payload.find('\0')));
			}
			break;
		case net::kNeSuicide: {
			game::GameWorld::PlayerInfo me;
			if (m_round->playerInfo(player, me) && me.alive) {
				m_round->setHitPoints(me.soldier, 0);
			}
			break;
		}
		default:
			break;
		}
		return;
	}
	case net::kEvContentCheck:
		checkContent(id, e);
		return;
	case net::kEvSelectKit: { // 0x0050b010: the team's kit by index
		game::GameWorld::PlayerInfo me;
		if (m_round->playerInfo(player, me)) {
			const auto& kits = m_logic->team(me.team).kits;
			const size_t kit = e.u(0);
			if (kit < kits.size()) {
				m_round->setKit(player, kits[kit].name, kits[kit].soldier);
			}
		}
		return;
	}
	case net::kEvChangePlayerName: // 0x0050bb90: only the sender's own player
		if (e.i(0) == player) {
			std::string name = e.text(0);
			name = name.substr(0, name.find('\0'));
			if (!name.empty()) {
				client.name = name;
				m_names[player] = name;
				m_round->setPlayerName(player, name);
				if (client.full && !reservedSlotCheck(id)) {
					return; // refused: the server is full
				}
				net::GameEvent renamed = eventOf(net::kEvChangePlayerName, {static_cast<uint32_t>(player) & 0xff}, {name});
				sendToPlaying(renamed);
				if (onLog) {
					onLog("player " + std::to_string(player) + " is " + name);
				}
			}
		}
		return;
	case net::kEvStringBlock: { // a chat line (0x006430c0 reassembles it)
		net::DataBlockChunk chunk;
		if (!net::parseStringBlock(e, chunk)) {
			return;
		}
		if (chunk.start) {
			client.chat.clear();
			client.chatSize = chunk.size;
			return;
		}
		client.chat += chunk.bytes;
		if (client.chat.size() < client.chatSize) {
			return;
		}
		net::ChatMessage line;
		if (net::decodeChat(client.chat, line) && line.source == 2 && line.channel != net::kChatServerMessage
			&& line.channel != net::kChatServerTeamMessage) {
			line.player = player; // 0x004a5460: the sender's player
			for (const net::GameEvent& out : net::stringBlockEvents(net::encodeChat(line))) {
				if (line.channel == net::kChatPlayer) {
					// A whisper (byte 9 the player it is for, 0xff from the
					// retail HUD, 0x007d9e20): only to that player's client
					// (the rewrite's routing; the sender shows its own line).
					for (auto& [to, other] : m_clients) {
						if (other.player == line.b9 && other.stage == Client::Stage::Playing) {
							sendTo(to, out);
						}
					}
					continue;
				}
				sendToPlaying(out);
			}
			if (onLog) {
				onLog("chat " + nameOf(player) + ": " + line.text);
			}
			if (onChat) {
				onChat(player, line);
			}
		}
		client.chat.clear();
		return;
	}
	case net::kEvJoinSquad: // team, player, squad
		if (e.i(1) == player) {
			m_round->joinSquad(player, e.i(2));
		}
		return;
	case net::kEvLeaveSquad: { // team, player, squad, bit (0x004c9c50)
		if (e.i(1) == player) {
			m_round->joinSquad(player, 0);
			return;
		}
		// Another player: the squad's leader removes a member (the invite
		// list's apply, squad manager slot 0x38).
		game::GameWorld::PlayerInfo me, other;
		if (m_round->playerInfo(player, me) && me.squadLeader && me.squad == e.i(2) && m_round->playerInfo(e.i(1), other)
			&& other.team == me.team && other.squad == me.squad) {
			m_round->joinSquad(other.id, 0);
		}
		return;
	}
	case net::kEvInvite: { // 0x0050e700: team, player, squad, from the leader
		// From the squad's leader (squad manager slot 0x40) only; slot 0x30
		// invites. A bot in no squad takes it at once (as offline: the
		// rewrite's stand-in for its answer); a client's player is asked.
		// Without the leader bit it is the invited player's own refusal
		// (the HUD's answer, 0x0079d6e0 kind 1: slot 0x30 with false, which
		// clears the squad's bit).
		if (e.u(3) == 0) {
			if (e.i(1) == player) {
				m_invites[player] &= ~(1u << (e.u(2) & 31));
			}
			return;
		}
		game::GameWorld::PlayerInfo me, other;
		if (!m_round->playerInfo(player, me) || !me.squadLeader || me.squad != e.i(2) || !m_round->playerInfo(e.i(1), other)
			|| other.team != me.team || other.commander) {
			return;
		}
		if (other.ai && other.squad == 0) {
			m_round->joinSquad(other.id, me.squad);
		}
		// Squad manager slot 0x30 (0x004c3190): the invited player's mask
		// of the squads that invited it (player vtable 0x224 / 0x228) gets
		// the squad's bit and its client message 2/0x10 (the squad), which
		// its HUD asks about (the question queue's kind 1, 0x0079e440).
		m_invites[other.id] |= 1u << (static_cast<uint32_t>(me.squad) & 31);
		for (const auto& [id, c] : m_clients) {
			if (c.player == other.id) {
				sendTo(id, net::makePostRemote(2, 0x10, net::int32Payload(me.squad)));
			}
		}
		return;
	}
	case net::kEvSetPrivateSquad: // team, squad, bit: the leader's lock (slot 0x20)
		if (onLog) {
			onLog("player " + std::to_string(player) + (e.u(2) ? " locks" : " unlocks") + " squad " + std::to_string(e.i(1)));
		}
		return;
	case net::kEvChangeSquadName: // a new squad (0x004c6720)
		if (e.i(2) == player) {
			m_round->joinSquad(player, e.i(0));
		}
		return;
	case net::kEvVote: { // 0x00590230: target, choice, .., voter, phase, kind
		if (e.i(6) != player && e.u(6) != 0xff) {
			return; // only the sender's own
		}
		sendVoteReplies(m_votes.handle(player, static_cast<int>(e.u(8)), static_cast<int>(e.u(7)), static_cast<int>(e.u(0)),
			static_cast<int>(e.u(1))));
		return;
	}
	case net::kEvIssueSquadOrder: { // 0x0050e3a0 with the sender's player
		net::SquadOrderInfo order;
		game::GameWorld::PlayerInfo me;
		if (!net::parseSquadOrder(e, order) || !m_round->playerInfo(player, me) || me.team != order.team) {
			return;
		}
		if (order.store == 0 || order.store == 1) {
			// An order: the leader's to his own squad (slot 0x48), the
			// commander's to a squad (0x4c); on an object, at the object.
			net::Vec3f at = order.point();
			if (order.object) {
				size_t part = 0;
				if (const Ghost* g = ghostOf(order.object, part)) {
					if (const world::Object* o = m_round->objects().find(g->object)) {
						at = {o->position().x, o->position().y, o->position().z};
					}
				}
			}
			const auto type = game::GameWorld::orderTypeFromKind(order.kind);
			if (order.store == 0 && me.squadLeader && me.squad == order.squad) {
				m_round->issueSquadLeaderOrder(player, type, at.x, at.z);
			} else if (order.store == 1 && m_round->commander(me.team) == player) {
				m_round->issueOrder(me.team, order.squad, type, at.x, at.z, player);
			}
			return; // trackOrders sends it on
		}
		if (order.store != net::kSquadOrderStoreRequest) {
			return;
		}
		// From the squad's leader or the team's commander (0x0050e3a0's
		// slot 0x40 / 0x78 checks), into the store (0x004c5720 with 0:
		// trackRequests sends it on).
		const bool leader = me.squadLeader && me.squad == order.squad;
		if ((!leader && m_round->commander(me.team) != player) || order.kind < 0
			|| order.kind >= game::GameWorld::kRequestKinds) {
			return;
		}
		const net::Vec3f at = order.point();
		if (m_round->storeSquadRequest(order.team, order.squad, order.kind, {at.x, at.y, at.z})) {
			// The leader's radio to the commander (0x007c7690, TCPlayer).
			static const char* const kVoices[] = {"req_artillery", "PLAYER_REQUEST_TM_requestforcestrike",
				"PLAYER_REQUEST_requestuav", "PLAYER_REQUEST_requestsupplies"};
			if (const int commander = m_round->commander(me.team); commander >= 0) {
				m_round->radioMessage(player, 14, kVoices[order.kind], commander);
			}
			if (onLog) {
				onLog("player " + std::to_string(player) + " requests " + std::to_string(order.kind) + " for squad "
					+ std::to_string(order.squad));
			}
		}
		return;
	}
	case net::kEvSetAcceptOrder: { // 0x0050df60 with the sender's player
		net::AcceptOrderInfo answer;
		game::GameWorld::PlayerInfo me;
		if (!net::parseAcceptOrder(e, answer) || !m_round->playerInfo(player, me) || me.team != answer.team) {
			return;
		}
		if (answer.store == 1) {
			// The leader's answer to the commander's order (0x004c2f60).
			if (me.squadLeader && me.squad == answer.squad) {
				m_round->acceptOrder(player, answer.flag);
			}
			return;
		}
		if (answer.store != net::kSquadOrderStoreRequest) {
			return;
		}
		if ((me.squadLeader && me.squad == answer.squad) || m_round->commander(me.team) == player) {
			m_round->removeSquadRequest(answer.team, answer.squad, answer.kind); // trackRequests passes it on
		}
		return;
	}
	case net::kEvCommander: // 0x0050cb00: only from the sender's own player
		if (e.i(2) != player) {
			return;
		}
		if (e.u(0) == net::GameClient::kCommanderApply) { // SquadManager slot 0x60
			m_round->applyForCommander(player);
		} else if (e.u(0) == net::GameClient::kCommanderResign) { // slot 0x70
			m_round->resignCommander(player);
		} else if (e.u(0) == net::GameClient::kCommanderStepDown || e.u(0) == net::GameClient::kCommanderWithdraw) {
			// The offer's answer: kind 0 takes command (slot 0x68,
			// 0x004c8c20), kind 1 passes it on (slot 0x6c).
			m_round->answerCommanderOffer(player, e.u(0) == net::GameClient::kCommanderStepDown);
		}
		return;
	case net::kEvRadioMessage: { // 0x0050d3b0: the connection's player says it
		net::RadioMessageInfo radio;
		if (!net::parseRadioMessage(e, radio) || player < 0) {
			return;
		}
		const std::string voice = radioVoice ? radioVoice(radio.index) : std::string();
		if (!voice.empty()) {
			m_round->radioMessage(player, radio.channel, voice, radio.value);
		}
		return;
	}
	case net::kEvSpotted: { // 0x0050e910
		net::SpottedInfo spot;
		game::GameWorld::PlayerInfo me;
		if (!net::parseSpotted(e, spot) || player < 0 || !m_round->playerInfo(player, me)) {
			return;
		}
		spotted(spot, player);
		return;
	}
	case net::kEvJoinAsSpectator: { // 0x005142a0
		net::JoinAsSpectatorInfo info;
		if (!net::parseJoinAsSpectator(e, info) || info.player != player) {
			return;
		}
		// Only on a server that isn't ranked (0x00430a00) and allows
		// spectators (sv.allowSpectators, +0x290); a game without settings
		// has the default, off.
		const net::ServerSettings* sv = m_setup.settings;
		if (!sv || sv->ranked || !sv->allowSpectators) {
			if (onLog) {
				onLog("player " + std::to_string(player) + " may not spectate here");
			}
			return;
		}
		if (info.kind == net::JoinAsSpectatorInfo::kJoinSpectator) {
			// The player's slot 0x3b8, then the event to everyone (game
			// server slot 0x8c), whose handler (0x00514260) marks it too.
			m_round->setSpectator(player, info.spectator);
			client.spectator = info.spectator;
			sendToPlaying(net::makeJoinAsSpectator(info));
			if (onLog) {
				onLog("player " + std::to_string(player) + (info.spectator ? " spectates" : " plays"));
			}
		} else {
			// The spectator's camera (player slot 0x1b4(0) -> slot 0x18):
			// the mode, and with 6 the player followed (+0x9c).
			client.cameraMode = info.kind;
			if (info.kind == net::JoinAsSpectatorInfo::kCameraFollow && info.follow != -1) {
				client.cameraFollow = info.follow;
			}
		}
		return;
	}
	case net::kEvCommanderCam:
		// 0x00510b30: the sender's team's overhead camera (squad manager
		// slot 0x7c, class 0x948c, made on first use by 0x004c5120) moves
		// there when he commands (0x005f8410: on the server, game logic slot
		// 0x144; position, then 0x005f8170(0, 0, -1) places it over the
		// point, so the event's height isn't kept). It goes to the clients
		// as its OverheadCamera ghost (GhostAlwaysWithPrediction).
		if (e.values.size() >= 3) {
			game::GameWorld::PlayerInfo info;
			if (m_round->playerInfo(player, info) && info.commander && (info.team == 1 || info.team == 2)) {
				float x = 0, z = 0;
				std::memcpy(&x, &e.values[0], 4);
				std::memcpy(&z, &e.values[2], 4);
				if (std::isfinite(x) && std::isfinite(z)) {
					moveOverheadCamera(info.team, x, z);
				}
			}
		}
		return;
	case net::kEvArtillery:
	case net::kEvUav:
	case net::kEvSupplyDrop:
	case net::kEvTitan: {
		net::CommanderToolUse use;
		if (net::parseCommanderToolUse(e, use)) {
			commanderTool(player, use);
		}
		return;
	}
	default:
		return;
	}
}

// The Spotted event's hand-out (0x0050e910): a spot (message 0x2e) is the
// reporter's report of the object (the world's category 3 message 0x16,
// here the round's reportBattlefieldObjects: the squad's minimap knows the
// enemy; with no object, the reporter's crosshair object, as its client's
// HUD would have found it, 0x0075be30); then every player of the team gets
// message 2/<message> with the event's 0x18 bytes: a client's player as
// PostRemote, the host's own through onSpotted.
void HostedGame::strikeFired(const game::GameWorld::StrikeFired& strike)
{
	if (!strike.first) {
		return; // the burst's later rounds: the clients' firearms fire them
	}
	const uint16_t ghost = ghostOfObject(strike.asset);
	if (!ghost) {
		return; // 0x005a0e50: only with the root's networkable (slot 0xd0)
	}
	net::StrikeFireInfo info;
	info.x = strike.at.x;
	info.z = strike.at.z;
	info.asset = ghost;
	// The tick (0x00666090) plus the firearm's place (the rewrite's assets
	// hold one firearm: 0).
	info.seed = static_cast<uint32_t>(std::max(0.0, m_round->roundTime()) * 30.0);
	sendToPlaying(net::makeStrikeFire(info));
}

void HostedGame::spotted(const net::SpottedInfo& from, int reporter)
{
	// The reporter is the connection's player (or the host's own): its
	// soldier and team (0x0042d220 / 0x0042d110 fill them from the player).
	game::GameWorld::PlayerInfo me;
	if (!m_round->playerInfo(reporter, me)) {
		return;
	}
	net::SpottedInfo spot = from;
	spot.spotter = me.soldier ? ghostOfObject(me.soldier) : 0;
	spot.team = me.team;

	if (spot.message == 0x2e) {
		const int enemy = spot.spotted ? playerOfGhost(spot.spotted) : -1;
		if (enemy >= 0) {
			m_round->reportBattlefieldObjects(reporter, {enemy});
		} else {
			m_round->spotCrosshair(reporter);
		}
	}
	const std::string payload = net::spottedPayload(spot);
	for (const auto& p : m_round->playerList()) {
		if (p.team != spot.team) {
			continue;
		}
		bool remote = false;
		for (const auto& [id, client] : m_clients) {
			if (client.player == p.id && client.stage == Client::Stage::Playing) {
				sendTo(id, net::makePostRemote(2, spot.message, payload));
				remote = true;
			}
		}
		if (!remote && p.id == m_round->localPlayer() && onSpotted) {
			onSpotted(spot);
		}
	}
}

int HostedGame::playerOfGhost(uint32_t id) const
{
	for (const auto& [object, ghost] : m_ghosts) {
		if (id >= ghost.first && id < ghost.first + ghost.layout->size()) {
			return m_round->playerOf(object);
		}
	}
	return -1;
}

// The commander's tools from a client: only the team's commander's
// (0x0050ee10 / 0x0050eb90 / 0x00511220 / 0x0050f1f0 check the sender's
// player slot 0x128), for his own team.
void HostedGame::commanderTool(int player, const net::CommanderToolUse& use)
{
	game::GameWorld::PlayerInfo me;
	if (!m_round->playerInfo(player, me) || me.team != use.team || m_round->commander(me.team) != player) {
		return;
	}
	using Ability = game::GameWorld::CommanderAbility;
	bool done = false;
	switch (use.tool) {
	case net::CommanderToolUse::Artillery:
		done = m_round->commanderAction(me.team, Ability::Artillery, use.x, use.z);
		break;
	case net::CommanderToolUse::OrbitalStrike:
		done = m_round->commanderAction(me.team, Ability::OrbitalStrike, use.x, use.z);
		break;
	case net::CommanderToolUse::Uav:
		done = m_round->commanderAction(me.team, Ability::Uav, use.x, use.z);
		break;
	case net::CommanderToolUse::Satellite:
		done = m_round->commanderAction(me.team, Ability::Satellite, use.x, use.z);
		break;
	case net::CommanderToolUse::Supplies:
		done = m_round->commanderAction(me.team, Ability::Supplies, use.x, use.z);
		break;
	case net::CommanderToolUse::MoveTitan: {
		// 0x0050f1f0 → the Titan's move (0x005abba0); its answer is the
		// commander's radio: AUTO_TITAN_CC_moveaccepted / _unablemove.
		const auto answer = m_round->moveTitan(me.team, use.x, use.z);
		done = answer == game::GameWorld::TitanMove::Accepted;
		if (answer == game::GameWorld::TitanMove::Accepted || answer == game::GameWorld::TitanMove::Unable) {
			m_round->hearRadio(player, 2, done ? "AUTO_TITAN_CC_moveaccepted" : "AUTO_TITAN_CC_unablemove");
		}
		break;
	}
	}
	if (onLog) {
		onLog("player " + std::to_string(player) + " uses commander tool " + std::to_string(static_cast<int>(use.tool))
			+ (done ? "" : " (refused)"));
	}
	if (onCommanderTool) {
		onCommanderTool(player, use, done);
	}
}

// Each message a client's player heard in the round this tick goes to its
// client as the RadioMessage event (0x004a6c60): the sender (0xff for
// none: the game's own automatic messages), the sender's team, the
// channel, source 3 from a player, 0 from the game. A voice the table
// doesn't have can't go.
void HostedGame::sendRadio()
{
	const auto heard = m_round->takeRadioHeard([this](int receiver) { return receiver >= 0 && clientHas(receiver); });
	for (const game::GameWorld::RadioHeard& message : heard) {
		const int index = radioIndex ? radioIndex(message.voice) : -1;
		if (index < 0) {
			continue;
		}
		net::RadioMessageInfo info;
		info.index = index;
		info.channel = message.channel;
		game::GameWorld::PlayerInfo from;
		if (message.sender >= 0 && m_round->playerInfo(message.sender, from)) {
			info.sender = message.sender;
			info.team = from.team;
			info.source = 3;
		}
		for (const auto& [id, client] : m_clients) {
			if (client.player == message.receiver && client.stage == Client::Stage::Playing) {
				sendTo(id, net::makeRadioMessage(info));
			}
		}
	}
}

void HostedGame::sendCloseTo()
{
	for (const auto& [id, client] : m_clients) {
		if (client.stage != Client::Stage::Playing || client.player < 0) {
			continue;
		}
		for (const game::GameWorld::CloseToEvent& e : m_round->takeCloseToEvents(client.player)) {
			sendTo(id, net::makePostRemote(2, e.enter ? 0x3e : 0x3f, net::int32Payload(e.player) + net::int32Payload(e.type)));
		}
	}
}

void HostedGame::sendTeamDamage()
{
	for (const auto& [id, client] : m_clients) {
		if (client.stage != Client::Stage::Playing || client.player < 0) {
			continue;
		}
		for (const game::GameWorld::TeamDamage& e : m_round->takeTeamDamage(client.player)) {
			sendTo(id, net::makePostRemote(net::kMsgGameCategory, 0xb,
						   net::int32Payload(e.kind) + net::int32Payload(e.victim) + net::int32Payload(e.attacker)));
		}
	}
}

// The squads' orders (the leader's, slot 0x48 → 0x004c5360, and the
// commander's, 0x4c → 0x004c5530) as they change: each new or moved one
// to everyone as IssueSquadOrder (store 0 / 1, the map kind and the
// point), the leader's answer to the commander's as SetAcceptOrder with
// the bit, an order gone as SetAcceptOrder without it (0x004c2f60).
void HostedGame::trackOrders()
{
	std::map<std::tuple<int, int, int>, OrderSent> now;
	for (int team = 1; team <= 2; ++team) {
		for (int squad = 1; squad <= 9; ++squad) {
			const game::GameWorld::SquadOrder* orders[2] = {m_round->squadLeaderOrder(team, squad), m_round->squadOrder(team, squad)};
			for (int store = 0; store < 2; ++store) {
				const game::GameWorld::SquadOrder* o = orders[store];
				if (o && o->type != game::GameWorld::OrderType::None) {
					now[{team, squad, store}] = {static_cast<int>(o->type), o->position, o->accepted};
				}
			}
		}
	}
	for (const auto& [key, order] : now) {
		const auto [team, squad, store] = key;
		const auto was = m_ordersSent.find(key);
		const bool moved = was == m_ordersSent.end() || was->second.kind != order.kind
			|| physics::lengthSquared(was->second.at - order.at) > 1e-4f;
		if (moved) {
			net::SquadOrderInfo info;
			info.team = team;
			info.squad = squad;
			info.kind = order.kind;
			info.store = store;
			info.floats[0] = order.at.x;
			info.floats[1] = order.at.y;
			info.floats[2] = order.at.z;
			sendToPlaying(net::makeSquadOrder(info));
		}
		if (store == 1 && order.accepted && (moved || !was->second.accepted)) {
			sendToPlaying(net::makeAcceptOrder({squad, team, true, store, order.kind}));
		}
	}
	for (const auto& [key, order] : m_ordersSent) {
		if (!now.count(key)) {
			const auto [team, squad, store] = key;
			sendToPlaying(net::makeAcceptOrder({squad, team, false, store, order.kind}));
		}
	}
	m_ordersSent = std::move(now);
}

// The offer of command (0x004c5070: PostRemote 2/0x23 to the applicant
// picked, its id as the payload), once per offer.
void HostedGame::trackCommanderOffers()
{
	for (int team = 1; team <= 2; ++team) {
		const int candidate = m_round->commanderOffer(team);
		if (candidate == m_offerSent[static_cast<size_t>(team)]) {
			continue;
		}
		m_offerSent[static_cast<size_t>(team)] = candidate;
		for (const auto& [id, client] : m_clients) {
			if (candidate >= 0 && client.player == candidate) {
				sendTo(id, net::makePostRemote(2, 0x23, net::int32Payload(candidate)));
			}
		}
	}
}

// --- each tick

void HostedGame::applyMoves()
{
	for (auto& [id, client] : m_clients) {
		if (client.player < 0) {
			continue; // the demo's connection
		}
		// The move the round stepped last tick is the record's from now on.
		client.stepped = client.stepping;
		game::SoldierInput input = client.held;
		input.turn = 0;
		input.use = false;
		input.switchSeat = 0;
		input.selectItem = 0;
		input.jump = false;
		// After running dry the moves wait until two are there (the
		// client's and the server's ticks run on their own clocks: a move a
		// tick late otherwise leaves a tick stepped with none, which the
		// client didn't predict). While dry the soldier keeps the last
		// move's held inputs.
		if (client.moves.empty()) {
			client.starved = true;
			++client.ticksStarved;
		} else if (client.starved && client.moves.size() >= 2) {
			client.starved = false;
		}
		if (!client.moves.empty() && !client.starved) {
			// One move a tick, as the client stepped it (Prediction.h).
			const net::PlayerMove move = client.moves.front();
			client.moves.pop_front();
			input = net::soldierInputFromMove(move, client.moveState);
			client.stepping = move.index;
			client.held = input;
		}
		m_round->setInput(client.player, input);
		game::GameWorld::PlayerInfo me;
		// A spawn asked for comes once the player's spawn time has (the
		// death's sv.spawnTime wait, 0x004bb220), and not in the start's
		// countdown (0x004bd2a0 sets every spawn time to its end).
		if (client.spawnRequested && m_round->playerInfo(client.player, me) && !me.alive) {
			if (me.respawnIn <= 0 && m_logic->roundState() != 2 && m_round->spawnSoldier(client.player)) {
				client.spawnRequested = false;
			}
		} else if (me.alive) {
			client.spawnRequested = false;
		}
	}
}

const std::vector<net::GhostPartLayout>* HostedGame::layoutFor(const world::ObjectTemplate& t)
{
	auto it = m_layouts.find(t.id());
	if (it == m_layouts.end()) {
		auto layout = std::make_unique<std::vector<net::GhostPartLayout>>(
			net::buildGhostLayout(net::ghostTemplateFrom(*m_templates, t)));
		it = m_layouts.emplace(t.id(), std::move(layout)).first;
	}
	const auto& layout = *it->second;
	return layout.empty() || layout.front().groups.empty() ? nullptr : &layout;
}

uint16_t HostedGame::ghostOfObject(uint32_t object, int part) const
{
	const auto it = m_ghosts.find(object);
	if (it == m_ghosts.end()) {
		return 0;
	}
	const int parts = static_cast<int>(it->second.layout->size());
	return static_cast<uint16_t>(it->second.first + std::clamp(part, 0, parts - 1));
}

void HostedGame::trackObjects()
{
	std::set<uint32_t> present;
	for (const auto& object : m_round->objects().objects()) {
		const world::ObjectTemplate* t = object->objectTemplate();
		if (!t || m_static.count(object->id())) {
			continue;
		}
		present.insert(object->id());
		if (m_ghosts.count(object->id())) {
			continue;
		}
		// Hand fire arms go with their kit (setForceNetworkableId), and
		// objects without a networkable aren't replicated.
		if (t->typeName() == "GenericFireArm" || t->typeName() == "ObjectSpawner") {
			continue;
		}
		const std::vector<net::GhostPartLayout>* layout = layoutFor(*t);
		if (!layout) {
			continue;
		}
		Ghost ghost;
		ghost.object = object->id();
		ghost.objectTemplate = t;
		ghost.layout = layout;
		ghost.first = m_nextGhost;
		const auto info = t->settings().find("networkableInfo");
		ghost.basePriority = info != t->settings().end() ? net::networkableInfos().basePriority(info->second) : 1.0f;
		ghost.projectile = t->typeName() == "GenericProjectile";
		m_nextGhost = static_cast<uint16_t>(m_nextGhost + layout->size());
		m_ghosts[object->id()] = ghost;
		for (auto& [id, client] : m_clients) {
			if (client.stage == Client::Stage::Database || client.stage == Client::Stage::Playing) {
				for (const net::GameEvent& e : createEvents(ghost)) {
					sendTo(id, e);
				}
				client.created.insert(ghost.first);
			}
		}
	}
	for (auto it = m_ghosts.begin(); it != m_ghosts.end();) {
		if (present.count(it->first) || (it->first & kProjectileKey)) {
			++it; // a projectile's goes with trackProjectiles
			continue;
		}
		const uint16_t ghost = it->second.first;
		for (auto& [id, client] : m_clients) {
			if (client.created.erase(ghost)) {
				sendTo(id, eventOf(net::kEvDestroyObject, {ghost, 0}));
			}
			for (size_t part = 0; part < it->second.layout->size(); ++part) {
				client.ghosts.forget(static_cast<uint16_t>(ghost + part));
			}
		}
		it = m_ghosts.erase(it);
	}
}

// The battlefield reports (event 0x41, SquadRadar): the server's handler
// (0x005088f0) hands one to the AI (IAIEventCourier vt+0xe0) and posts a
// copy (0x005087b0) to each player of the reporter's team and squad (the
// reporter alone without a squad), whose client puts the enemies on its
// minimap for the report's seconds (0x00508590). The rewrite's round makes
// them in reportBattlefieldObjects (the bots' trigger, the recon drones'
// pulses, the cloak, the radio's spot). Before posting, the handler reads
// each recipient's ability block (slot 0x2a0, squadReportAbility): the
// report lasts the longest networkableHudRevealTime among them (+0xc;
// 3 s, 6 with the recon's unlock), and a cloak report (kind 2) goes on
// only as kind 1 when one of them has networkableHudShowCloaked (+0x12),
// else to no one (battlefieldReportFor).
void HostedGame::sendBattlefieldReports()
{
	for (const game::GameWorld::BattlefieldReport& report : m_round->takeBattlefieldReports()) {
		if (report.players.empty() || report.reporter < 0) {
			continue;
		}
		std::vector<int> recipients;
		std::vector<std::pair<int, bool>> readings;
		for (auto& [id, client] : m_clients) {
			game::GameWorld::PlayerInfo p;
			if (client.stage != Client::Stage::Playing || !m_round->playerInfo(client.player, p) || p.team != report.team) {
				continue;
			}
			if (report.squad == 0 ? client.player == report.reporter : p.squad == report.squad) {
				recipients.push_back(id);
				int reveal = 3;
				bool cloaked = false;
				m_round->squadReportAbility(client.player, reveal, cloaked);
				readings.push_back({reveal, cloaked});
			}
		}
		int kind = report.kind, seconds = report.seconds;
		if (!battlefieldReportFor(readings, kind, seconds)) {
			continue;
		}
		std::vector<uint32_t> values {static_cast<uint32_t>(kind), static_cast<uint32_t>(report.reporter & 0xff),
			static_cast<uint32_t>(std::min<size_t>(report.players.size(), 0x40)),
			static_cast<uint32_t>(std::clamp(seconds, 0, 0x3c))};
		for (size_t i = 0; i < report.players.size() && i < 0x40; ++i) {
			values.push_back(static_cast<uint32_t>(report.players[i] & 0xff));
		}
		const net::GameEvent e = eventOf(net::kEvSquadRadar, values);
		for (const int id : recipients) {
			sendTo(id, e);
		}
	}
}

bool battlefieldReportFor(const std::vector<std::pair<int, bool>>& recipients, int& kind, int& seconds)
{
	bool cloakReader = false;
	for (const auto& [reveal, showCloaked] : recipients) {
		seconds = std::max(seconds, reveal);
		cloakReader = cloakReader || (kind == 2 && showCloaked);
	}
	if (kind == 2 && cloakReader) {
		kind = 1;
	}
	return kind != 2;
}

const game::Projectile* HostedGame::projectileOf(const Ghost& ghost) const
{
	if (!ghost.projectileSerial) {
		return nullptr;
	}
	for (const game::Projectile& p : m_round->projectiles()) {
		if (p.serial == ghost.projectileSerial && p.alive) {
			return &p;
		}
	}
	return nullptr;
}

// The round's networked projectiles (the rewrite's round keeps them out of
// its object manager): each a ghost of its template (one, a projectile
// being its own tree's root), created when it is fired and destroyed when
// it goes, as the server's object manager does with a projectile object.
// The rounds without a networkable (bullets, shells) aren't ghosted: each
// client makes them from the firing (0x005f31d0).
void HostedGame::moveOverheadCamera(int team, float x, float z)
{
	world::Object* camera = m_overheadCameras[team] ? m_round->objects().find(m_overheadCameras[team]) : nullptr;
	world::ObjectTemplate* t = m_round->templates().find("MultiPlayerOverheadCamera");
	if (!camera) {
		if (!t) {
			return;
		}
		camera = m_round->objects().create(t);
		if (!camera) {
			return;
		}
		camera->setTeam(team);
		m_overheadCameras[team] = camera->id();
	}
	// 0x005f8170: kept 80 m inside the terrain, over the mean of the ground
	// at the point and 2 m off along both axes (or the sea's level when
	// higher), plus the template's height (+0x2a4, 260); looking straight
	// down, north up.
	if (m_terrainExtent > 160) {
		x = std::clamp(x, m_terrainOrigin[0] + 80, m_terrainOrigin[0] + m_terrainExtent - 80);
		z = std::clamp(z, m_terrainOrigin[1] + 80, m_terrainOrigin[1] + m_terrainExtent - 80);
	}
	const physics::Ground& ground = m_round->ground();
	float y = 0;
	if (ground.height) {
		const float dx[5] = {0, 0, 0, 2, -2}, dz[5] = {0, 2, -2, 0, 0};
		for (int i = 0; i < 5; ++i) {
			y += ground.height(x + dx[i], z + dz[i]) * 0.2f;
		}
	}
	if (ground.waterLevel) {
		y = std::max(y, ground.waterLevel(x, z));
	}
	float height = 260;
	if (t) {
		const auto it = t->settings().find("height");
		if (it != t->settings().end()) {
			height = static_cast<float>(std::atof(it->second.c_str()));
		}
	}
	camera->setPosition({x, y + height, z});
	camera->setRotation({0, -90, 0});
}

net::GameEvent HostedGame::stickyEvent(const game::Projectile& p) const
{
	net::StickyProjectileInfo info;
	const auto it = m_ghosts.find(kProjectileKey | p.serial);
	info.projectile = it != m_ghosts.end() ? it->second.first : 0;
	info.target = p.stuckTo ? ghostOfObject(p.stuckTo) : 0;
	info.player = p.player;
	// Its matrix: up along the face's normal, facing as it was thrown;
	// in the vehicle's frame on one (the sticky component's +0x18), else
	// in the world.
	const physics::Vec3 up = info.target ? p.stuckLocalNormal : p.stuckNormal;
	physics::Vec3 forward {std::sin(p.yaw * 0.017453292f), 0, std::cos(p.yaw * 0.017453292f)};
	if (info.target) {
		if (const world::Object* parent = m_round->objects().find(p.stuckTo)) {
			forward = physics::Mat3::fromYawPitchRoll({parent->rotation().x, parent->rotation().y, parent->rotation().z})
						  .untransform(forward);
		}
	}
	forward = forward - up * physics::dot(forward, up);
	const float length = std::sqrt(physics::lengthSquared(forward));
	forward = length > 1e-4f ? forward * (1 / length) : physics::Vec3 {0, 0, 1};
	const physics::Vec3 right = physics::cross(up, forward);
	const physics::Vec3 place = info.target ? p.stuckLocal : p.position;
	info.rows = {net::Vec3f {right.x, right.y, right.z}, net::Vec3f {up.x, up.y, up.z},
		net::Vec3f {forward.x, forward.y, forward.z}, net::Vec3f {place.x, place.y, place.z}};
	return net::makeStickyProjectile(info);
}

void HostedGame::trackProjectiles()
{
	std::set<uint32_t> present;
	for (const game::Projectile& p : m_round->projectiles()) {
		if (!p.alive || !p.serial || p.visual || !p.settings) {
			continue;
		}
		const uint32_t key = kProjectileKey | p.serial;
		if (m_ghosts.count(key)) {
			present.insert(key);
			continue;
		}
		const world::ObjectTemplate* t = m_round->templates().find(p.settings->name);
		if (!t) {
			continue;
		}
		const auto info = t->settings().find("networkableInfo");
		if (info == t->settings().end() || info->second.empty()) {
			continue;
		}
		const std::vector<net::GhostPartLayout>* layout = layoutFor(*t);
		if (!layout || layout->empty()) {
			continue;
		}
		present.insert(key);
		Ghost ghost;
		ghost.object = key;
		ghost.objectTemplate = t;
		ghost.layout = layout;
		ghost.first = m_nextGhost;
		ghost.basePriority = net::networkableInfos().basePriority(info->second);
		ghost.projectile = true;
		ghost.projectileSerial = p.serial;
		m_nextGhost = static_cast<uint16_t>(m_nextGhost + layout->size());
		m_ghosts[key] = ghost;
		for (auto& [id, client] : m_clients) {
			if (client.stage == Client::Stage::Database || client.stage == Client::Stage::Playing) {
				for (const net::GameEvent& e : createEvents(ghost)) {
					sendTo(id, e);
				}
				client.created.insert(ghost.first);
			}
		}
	}
	// The rounds stuck since the last tick (0x005e81a0: sent once placed).
	std::set<uint32_t> stuck;
	for (const game::Projectile& p : m_round->projectiles()) {
		if (!p.alive || !p.stuck || !p.serial || p.visual || !m_ghosts.count(kProjectileKey | p.serial)) {
			continue;
		}
		stuck.insert(p.serial);
		if (m_stuckSent.insert(p.serial).second) {
			sendToPlaying(stickyEvent(p));
		}
	}
	for (auto it = m_stuckSent.begin(); it != m_stuckSent.end();) {
		it = stuck.count(*it) ? std::next(it) : m_stuckSent.erase(it); // fallen again, or gone
	}
	for (auto it = m_ghosts.begin(); it != m_ghosts.end();) {
		if (!(it->first & kProjectileKey) || present.count(it->first)) {
			++it;
			continue;
		}
		const uint16_t ghost = it->second.first;
		for (auto& [id, client] : m_clients) {
			if (client.created.erase(ghost)) {
				sendTo(id, eventOf(net::kEvDestroyObject, {ghost, 0}));
			}
			for (size_t part = 0; part < it->second.layout->size(); ++part) {
				client.ghosts.forget(static_cast<uint16_t>(ghost + part));
			}
		}
		it = m_ghosts.erase(it);
	}
}

void HostedGame::trackPlayers()
{
	// Kills, as the game mode's scripts tell them (the PythonCommand kill
	// message, type 100: kind, victim, attacker; RemoteWorld reads it).
	std::vector<int> victims, killers;
	for (const auto& p : m_round->playerList()) {
		const auto seen = m_players.find(p.id);
		if (seen == m_players.end()) {
			continue;
		}
		if (p.deaths > seen->second.deaths) {
			victims.push_back(p.id);
		}
		if (p.kills > seen->second.kills || p.teamKills > seen->second.teamKills) {
			killers.push_back(p.id);
		}
		seen->second.kills = p.kills;
		seen->second.deaths = p.deaths;
		seen->second.teamKills = p.teamKills;
	}
	for (const int victim : victims) {
		const int killer = killers.empty() ? victim : killers.front();
		sendToPlaying(eventOf(net::kEvPythonCommand, {100, 0, 3, 0, static_cast<uint32_t>(victim), static_cast<uint32_t>(killer)}));
	}
	for (const auto& p : m_round->playerList()) {
		auto seen = m_players.find(p.id);
		if (seen == m_players.end()) {
			PlayerSeen fresh;
			fresh.ghost = m_nextGhost++;
			fresh.kills = p.kills;
			fresh.deaths = p.deaths;
			fresh.teamKills = p.teamKills;
			seen = m_players.emplace(p.id, fresh).first;
			for (const auto& [id, client] : m_clients) {
				if (client.stage == Client::Stage::Database || client.stage == Client::Stage::Playing) {
					sendTo(id, createPlayerEvent(p, p.id == client.player, fresh.ghost));
				}
			}
		}
		PlayerSeen& s = seen->second;
		if (p.alive && (!s.alive || s.soldier != p.soldier)) {
			// Spawned: the kit and the soldier it holds (HandlePickup, 0x0050bfd0).
			sendToPlaying(eventOf(net::kEvHandlePickup, {static_cast<uint32_t>(p.id) & 0xff, ghostOfObject(p.kit),
				ghostOfObject(p.soldier), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
		}
		const uint32_t vehicle = p.alive ? p.vehicle : 0;
		if (vehicle != s.vehicle || (vehicle && p.seat != s.seat)) {
			if (vehicle) {
				sendToPlaying(eventOf(net::kEvEnterVehicle, {static_cast<uint32_t>(p.id) & 0xff, ghostOfObject(vehicle, p.seat), 0}));
			} else {
				sendToPlaying(eventOf(net::kEvExitVehicle, {static_cast<uint32_t>(p.id) & 0xff, 0}));
			}
		}
		s.alive = p.alive;
		s.soldier = p.alive ? p.soldier : 0;
		s.vehicle = vehicle;
		s.seat = p.seat;
	}
}

net::NetworkableState HostedGame::soldierState(uint32_t soldier, const world::ObjectTemplate& t,
	const net::NetworkableHints& h, unsigned mode) const
{
	// The fields 0x005fc1a0 fills for the Soldier's writer (0x005fb8c0).
	net::NetworkableState s;
	s.kind = net::NetworkableKind::Soldier;
	const world::Object* object = m_round->objects().find(soldier);
	const int player = m_round->playerOf(soldier);
	game::GameWorld::PlayerInfo info;
	const bool known = player >= 0 && m_round->playerInfo(player, info) && info.soldier == soldier;
	s.mask = 0x1 | 0x2 | 0x10 | 0x40 | 0x100;
	if (object) {
		s.position = {object->position().x, object->position().y, object->position().z};
		s.hasPosition = true;
	}
	s.yaw = known ? info.yaw : object ? object->rotation().x : 0;
	s.aimPitch = known ? info.aimPitch : 0;
	s.velocity = known ? net::Vec3f {info.velocity.x, info.velocity.y, info.velocity.z} : net::Vec3f {};
	const float maximum = settingFloat(t, "armor.maxHitPoints", 100);
	// GameWorld keeps no hit points for a soldier not hurt yet whose
	// template sets no armor.maxHitPoints (-1): a whole one.
	const float hp = m_round->hitPoints(soldier);
	s.health = hp < 0 ? 1.0f : maximum > 0 ? std::clamp(hp / maximum, 0.0f, 1.0f) : 0;
	if (known && h.soldierItemSlots >= 0 && info.inHandSlot >= 0 && info.inHandSlot <= h.soldierItemSlots) {
		s.mask |= 0x2000;
		s.weapon = info.inHandSlot; // the item index in hand (+0x218)
	}
	const game::SoldierControl* control = known ? m_round->soldierControl(player) : nullptr;
	const physics::SoldierBody* body = known ? m_round->soldierBody(player) : nullptr;
	if (known) {
		// The pose and the pose asked for (0x20: soldier +0x268 / +0x26c,
		// 0..4; 3 swimming, which 0x0058a6d0 sets from the response's +0xfa),
		// both the input's here (the rewrite's soldier takes the pose it is
		// asked for at once).
		s.mask |= 0x20;
		s.soldierValues[0] = s.soldierValues[1] = static_cast<int>(info.pose);
		// The always-sent bits (0x005fc1a0: +0x328, +0x30c the swimming,
		// +0x3e9 the weapon put away, 0x005878b0: swimming among others).
		const bool swimming = info.pose == game::SoldierPose::Swim;
		s.soldierBits = (swimming ? 4u : 0u) | (swimming ? 8u : 0u);
	}
	if (control && body) {
		s.yaw = body->yawDegrees;
		s.velocity = {body->velocity.x, body->velocity.y, body->velocity.z};
		// The look's twist ahead of the body and the twist still to catch
		// up (+0x278, +0x280: masks 4 and 8 in both modes, 0x005fc1a0):
		// the look is the body's yaw plus the twist.
		s.mask |= 0x4 | 0x8;
		s.pitch = control->twist();
		s.roll = control->catchUp();
		// The eased throttle and strafe (+0x264, +0x260), in both modes.
		s.mask |= 0x800 | 0x1000;
		s.soldierLean[0] = control->throttle();
		s.soldierLean[1] = control->strafe();
		if (control->stamina().configured()) {
			s.mask |= 0x8000;
			s.stamina = control->stamina().stamina();
			s.sprinting = control->stamina().sprinting();
		}
		if (mode == 3) {
			// The node's acceleration and velocity change (slots 0x74, 0xc4).
			s.mask |= 0x200 | 0x80000;
			// The kit's ability battery (0x40000, mode 3 only: soldier
			// +0x338, 0..1, -1 without a kit).
			s.mask |= 0x40000;
			s.battery = std::clamp(info.battery, -1.0f, 1.0f);
			s.soldierAcceleration = {body->acceleration.x, body->acceleration.y, body->acceleration.z};
			s.soldierImpulse = {body->impulse.x, body->impulse.y, body->impulse.z};
		}
	}
	if (known && info.seat >= 0) {
		// Seated (0x005fc1a0 fills the same fields): +0x25c is the body's
		// turn from the seat and +0x278 the head's twist on it (0x0058a6d0),
		// which a client's 0x00587a30 turns the body by and the seat's
		// LookAroundTrigger takes; no catch-up.
		s.mask |= 0x4 | 0x8;
		s.yaw = info.seatBodyYaw;
		s.pitch = info.seatTwist;
		s.roll = 0;
	}
	if (known) {
		// The ladder (0x20000): the soldier's +0x18c interface slot 0x84,
		// the ladder's id (+0x184; 0 none), and the place on it
		// (0x0059ebf0); a client seats the soldier in a container of its
		// own at that ladder (0x0059fbf0) and moves it there (0x0059ec80),
		// and takes it out on a 0 (0x0059d440). The dirty masks send it on
		// a change (snapshotGhosts).
		s.mask |= 0x20000;
		s.ladder = std::min(info.ladder, 255);
		s.ladderPosition = info.ladderPosition;
	}
	return s;
}

std::vector<net::NetworkableState> HostedGame::statesOf(const Ghost& ghost, size_t part, unsigned mode) const
{
	const net::GhostPartLayout& layout = (*ghost.layout)[part];
	std::vector<net::NetworkableState> states(layout.groups.size());
	if (ghost.projectileSerial) {
		// GenericProjectile (0x006067c0): the position (2), the rotation (1)
		// and the speed block (4: its velocity), as the projectile flies.
		const game::Projectile* p = projectileOf(ghost);
		if (!p || states.empty()) {
			return states;
		}
		net::NetworkableState& s = states.front();
		s.kind = layout.groups.front().hints.kind;
		s.mask = 0x1 | 0x2 | 0x4;
		s.position = {p->position.x, p->position.y, p->position.z};
		const float horizontal = std::sqrt(p->velocity.x * p->velocity.x + p->velocity.z * p->velocity.z);
		s.orientation = quaternionFromYawPitchRoll(std::atan2(p->velocity.x, p->velocity.z) * 57.29578f,
			std::atan2(p->velocity.y, horizontal) * 57.29578f, 0);
		s.hasVelocity = true;
		s.velocity = {p->velocity.x, p->velocity.y, p->velocity.z};
		return states;
	}
	const world::Object* object = m_round->objects().find(ghost.object);
	if (!object) {
		return states;
	}
	for (size_t g = 0; g < layout.groups.size(); ++g) {
		net::NetworkableState& s = states[g];
		const net::NetworkableHints& h = layout.groups[g].hints;
		s.kind = h.kind;
		const bool root = part == 0 && g == 0;
		const std::string& name = layout.groups[g].members.empty()
			? layout.name
			: layout.nodes[static_cast<size_t>(layout.groups[g].members.front())].name;
		const net::Vec3f position {object->position().x, object->position().y, object->position().z};
		world::Vec3 offset, rotation;
		switch (h.kind) {
		case net::NetworkableKind::Soldier:
			if (root) {
				s = soldierState(ghost.object, *ghost.objectTemplate, h, mode);
			}
			break;
		case net::NetworkableKind::SimpleObject:
		case net::NetworkableKind::GenericProjectile:
			if (!root) {
				break; // a part's own SimpleObject: it moves with its vehicle
			}
			s.mask = 0x2 | 0x1 | (h.hasArmor ? 0x10u : 0u);
			s.position = position;
			s.orientation = quaternionFromYawPitchRoll(object->rotation().x, object->rotation().y, object->rotation().z);
			s.health = m_round->hitPoints(ghost.object);
			if (h.kind == net::NetworkableKind::SimpleObject && h.mobile) {
				if (const auto motion = m_motion.find(ghost.object); motion != m_motion.end()) {
					s.mask |= 0x4 | 0x8;
					s.velocity = {motion->second.velocity.x, motion->second.velocity.y, motion->second.velocity.z};
					s.angularVelocity = {motion->second.angularVelocity.x, motion->second.angularVelocity.y,
						motion->second.angularVelocity.z};
				}
			}
			// The armoured parts below it (0x800000): the rewrite's vehicles
			// keep their armour on the root, so every part is whole but the
			// repair cells (the heavy IFV's shield generators:
			// hudRepairCellNo, in the root's armour order), each in eighths
			// of its hit points as 0x005ffec0 captures them
			// (GameWorld::repairCellEighths), which the clients' vehicle
			// HUD (0x007a1e50) and the cells' armour effects follow.
			if (h.kind == net::NetworkableKind::SimpleObject && h.armoredChildrenKnown && !h.armoredChildren.empty()) {
				s.mask |= 0x800000;
				s.childDamage.assign(h.armoredChildren.size(), 8);
				const std::vector<int> cells = m_round->repairCellEighths(ghost.object);
				for (size_t i = 0, cell = 0; i < h.armoredRepairCells.size() && i < s.childDamage.size(); ++i) {
					if (h.armoredRepairCells[i] && cell < cells.size()) {
						s.childDamage[i] = cells[cell++];
					}
				}
			}
			break;
		case net::NetworkableKind::OverheadCamera:
			// 0x006033a0 / 0x006037a0: the pan (1), where it stands (2), the
			// fov's byte (4). The server's camera doesn't pan (0x005f8510
			// runs for a local commander only).
			if (root) {
				s.mask = 0x2 | 0x4;
				s.position = position;
				s.cameraValue = 0;
			}
			break;
		case net::NetworkableKind::ControlPoint:
			s.mask = 0x1 | 0x4 | 0x20;
			s.team = object->team();
			for (const game::ControlPoint& cp : m_round->controlPoints()) {
				if (cp.object == object) {
					s.team = cp.team;
					s.cpValues[0] = cp.flagHeight;
					s.cpValues[1] = cp.takeOverChangePerSecond;
					s.cpValue = cp.flag;
					// allowCaptureByTeam for teams 1 and 2 (8, 0x10: cp +0x1b4,
					// +0x1b5), the clients' takeable radio.
					s.mask |= 0x8 | 0x10;
					s.cpAllowTeam1 = cp.allowCapture[1];
					s.cpAllowTeam2 = cp.allowCapture[2];
				}
			}
			break;
		case net::NetworkableKind::DestroyableObject:
			s.mask = h.hasArmor ? 0x1u | 0x2u : 0u;
			s.health = m_round->hitPoints(ghost.object);
			s.position = position;
			break;
		case net::NetworkableKind::Kit:
			s.mask = 0x1;
			s.position = position;
			break;
		case net::NetworkableKind::RotationalBundle:
			// A turret's, a steering's or a rotor's angles (0x006015a0:
			// object +0x188, degrees), as the round turns the part.
			if (m_round->partPose(ghost.object, name, offset, rotation)) {
				s.mask = 0x1 | 0x2 | 0x4;
				s.rotation[0] = rotation.x;
				s.rotation[1] = rotation.y;
				s.rotation[2] = rotation.z;
			}
			break;
		case net::NetworkableKind::MechLegs:
			// The walker's legs (0x0060a000): the state (+0x1bc, mask 1) and
			// whether its input still holds (+0x1c0, mask 2), from which the
			// client's legs send their animation messages (0x00650530,
			// net::mechLegsMessages).
			if (const game::Vehicle* v = m_round->vehicle(ghost.object); v && v->legs()) {
				const game::Vehicle::Legs& legs = *v->legs();
				s.mask = 0x1 | 0x2;
				s.legState = legs.state;
				s.legFlag1 = (legs.messages & 0x780u) != 0 || legs.crouching;
			}
			break;
		case net::NetworkableKind::Spring:
			// A wheel's suspension (0x00605170: the offset, vtable 0xe0).
			if (m_round->partPose(ghost.object, name, offset, rotation)) {
				s.springPresent = true;
				s.mask = 1;
				s.springOffset = {offset.x, offset.y, offset.z};
			}
			break;
		case net::NetworkableKind::PlayerControlObject: {
			// A seat (0x00605650): its occupant's soldier, or its weapons.
			if (g != 0) {
				break;
			}
			const std::vector<int> occupants = m_round->vehicleOccupants(ghost.object);
			const int player = part < occupants.size() ? occupants[part] : -1;
			if (h.seatCameras != 0) {
				// Each group's active weapon (masks 1, 2, 4: its place in the
				// group's list + 1, as 0x005a98b0's): the clients fire that
				// one from the occupant's fire state (RemoteGuns.h).
				const auto seats = m_round->vehicleSeats(ghost.object);
				if (part >= seats.size()) {
					break;
				}
				const auto& weapons = seats[part].weapons;
				for (int wg = 0; wg < 3; ++wg) {
					const int active = m_round->seatWeaponIndex(ghost.object, static_cast<int>(part), wg);
					if (active < 0 || static_cast<size_t>(active) >= weapons.size()) {
						continue;
					}
					int place = 0;
					for (int i = 0; i < active; ++i) {
						place += weapons[static_cast<size_t>(i)].group == wg ? 1 : 0;
					}
					s.mask |= 1u << wg;
					s.seatWeapon[wg] = place;
				}
				break;
			}
			game::GameWorld::PlayerInfo info;
			if (player >= 0 && m_round->playerInfo(player, info) && info.soldier) {
				if (const world::Object* soldier = m_round->objects().find(info.soldier); soldier && soldier->objectTemplate()) {
					net::NetworkableHints sh;
					sh.kind = net::NetworkableKind::Soldier;
					s.occupant = std::make_shared<net::NetworkableState>(
						soldierState(info.soldier, *soldier->objectTemplate(), sh, mode));
					s.occupant->mask &= ~0x2000u; // the seat's soldier hints carry no item count
				}
			}
			break;
		}
		default:
			break;
		}
	}
	return states;
}

std::vector<uint8_t> HostedGame::encodeGhost(const GhostNow& now, const net::GhostMasks& masks, size_t& bits,
	net::GhostMasks& written) const
{
	// Each group with its mask (0x0057f420: the dirty mask & the
	// networkable's slot 0x24 for the mode; here & the fields the state
	// holds).
	written = {};
	net::BitWriter out;
	if (now.team) {
		net::TeamNetState t = now.teamState;
		t.mask &= masks[0];
		written[0] = t.mask;
		net::encodeTeamState(out, t);
	} else if (now.player >= 0) {
		net::NetworkableState s = now.states.front();
		s.mask &= masks[0];
		written[0] = s.mask;
		net::NetworkableHints hints;
		hints.kind = net::NetworkableKind::Player;
		net::encodeNetworkable(out, hints, nullptr, s);
	} else if (now.layout) {
		std::vector<net::NetworkableState> states = now.states;
		for (size_t g = 0; g < states.size() && g < net::kGhostMaskGroups; ++g) {
			states[g].mask &= masks[g];
			written[g] = states[g].mask;
		}
		net::encodeGhostState(out, *now.layout, nullptr, states);
	}
	bits = out.bits();
	return out.data();
}

const HostedGame::GhostNow* HostedGame::ghostNow(uint16_t ghost)
{
	if (const auto it = m_stateCache.find(ghost); it != m_stateCache.end()) {
		return &it->second;
	}
	GhostNow now;
	if (ghost == net::kTeamStateGhost) {
		now.team = true;
		now.teamState = teamStateOf();
	} else {
		for (const auto& [player, seen] : m_players) {
			if (seen.ghost == ghost) {
				now.player = player;
			}
		}
		if (now.player >= 0) {
			// Every group's fields are there to write; the mask's 0x10000
			// goes only with a reload (snapshotGhosts).
			now.states.push_back(playerStateOf(now.player, true));
		} else {
			size_t part = 0;
			const Ghost* g = ghostOf(ghost, part);
			if (!g) {
				return nullptr;
			}
			now.layout = &(*g->layout)[part];
			now.states = statesOf(*g, part, 2);
		}
	}
	return &(m_stateCache[ghost] = std::move(now));
}

void HostedGame::snapshotGhosts()
{
	if (m_snapshotTaken && m_snapshotTick == m_tick) {
		return;
	}
	m_snapshotTaken = true;
	m_snapshotTick = m_tick;
	std::vector<uint16_t> ghosts;
	for (const auto& [object, ghost] : m_ghosts) {
		for (size_t part = 0; part < ghost.layout->size(); ++part) {
			ghosts.push_back(static_cast<uint16_t>(ghost.first + part));
		}
	}
	for (const auto& [player, seen] : m_players) {
		ghosts.push_back(seen.ghost);
	}
	ghosts.push_back(net::kTeamStateGhost);
	std::map<uint16_t, GhostSeen> snapshot;
	for (const uint16_t ghost : ghosts) {
		const GhostNow* now = ghostNow(ghost);
		if (!now) {
			continue;
		}
		const auto before = m_snapshot.find(ghost);
		GhostSeen& seen = snapshot[ghost];
		net::GhostMasks changed {};
		if (now->team) {
			// The teams' state: each value against the last (0x00560ff0's
			// snapshot).
			seen.team = now->teamState;
			seen.hasTeam = true;
			const bool had = before != m_snapshot.end() && before->second.hasTeam;
			for (int bit = 0; bit < 32; ++bit) {
				const uint32_t b = 1u << bit;
				if ((seen.team.mask & b)
					&& (!had || !(before->second.team.mask & b)
						|| before->second.team.values[static_cast<size_t>(bit)] != seen.team.values[static_cast<size_t>(bit)])) {
					changed[0] |= b;
				}
			}
		} else if (now->player >= 0) {
			// The player's (0x005929f0): its fields; mask 0x10000 from a
			// reload pressed since (player +0x28c, which the snapshot
			// turns into the bit and clears).
			net::NetworkableState s = now->states.front();
			s.mask &= ~0x10000u;
			net::NetworkableHints hints;
			hints.kind = net::NetworkableKind::Player;
			seen.groups.push_back(net::networkableFields(hints, s));
			changed[0] = net::changedFields(
				before != m_snapshot.end() && !before->second.groups.empty() ? before->second.groups.front() : net::NetworkableFields {},
				seen.groups.front());
			game::GameWorld::PlayerInfo info;
			if (m_round->playerInfo(now->player, info)) {
				const auto presses = m_reloadsSeen.try_emplace(now->player, info.reloadPresses).first;
				if (presses->second != info.reloadPresses) {
					changed[0] |= 0x10000;
					presses->second = info.reloadPresses;
				}
			}
		} else {
			for (size_t g = 0; g < now->states.size() && g < now->layout->groups.size() && g < net::kGhostMaskGroups; ++g) {
				seen.groups.push_back(net::networkableFields(now->layout->groups[g].hints, now->states[g]));
				changed[g] = net::changedFields(before != m_snapshot.end() && g < before->second.groups.size()
						? before->second.groups[g]
						: net::NetworkableFields {},
					seen.groups.back());
			}
		}
		bool any = false;
		for (const uint32_t m : changed) {
			any = any || m != 0;
		}
		if (any) {
			for (auto& [id, client] : m_clients) {
				client.ghosts.markDirty(ghost, changed);
			}
		}
	}
	m_snapshot = std::move(snapshot);
}

net::PlayerOwnState HostedGame::ownState(int player) const
{
	// What the player's vtable 0x1c8 (0x00594cc0) writes from the player.
	net::PlayerOwnState s;
	game::GameWorld::PlayerInfo info;
	if (!m_round->playerInfo(player, info)) {
		return s;
	}
	s.spawnTick = m_tick + static_cast<uint32_t>(std::ceil(std::max(0.0f, info.respawnIn) * 30));
	s.hitIndicator = m_round->takeHitIndicator(player); // slot 0x210 (0x0059b480)
	// What the player gave (the ability bar's flags, set by the game
	// logic's heal 0x004bc410 and ammo 0x004bd5f0): +0x16a ammo, +0x168
	// healed, +0x169 repaired; +0x16b is RTTeam's (slot 0x284), never set.
	if (game::GameWorld::AbilityBarInfo gifts; m_round->abilityBar(player, gifts)) {
		s.flag16a = gifts.ammoFlag;
		s.flag168 = gifts.healFlag;
		s.flag169 = gifts.repairFlag;
	}
	s.manDown = info.manDown;
	s.commander = info.commander;
	if (info.soldier) {
		if (const world::Object* soldier = m_round->objects().find(info.soldier); soldier && soldier->objectTemplate()) {
			const float maximum = settingFloat(*soldier->objectTemplate(), "armor.maxHitPoints", 100);
			const float hp = m_round->hitPoints(info.soldier);
			s.health = hp < 0 ? 1.0f : maximum > 0 ? std::clamp(std::ceil(hp) / maximum, 0.0f, 1.0f) : 0;
		}
	}
	// On foot, group 0 is the soldier's own weapons (its kit's, in slot
	// order): the one in hand, its rounds and magazines.
	if (info.alive && !info.vehicle) {
		for (size_t i = 0; i < info.carried.size(); ++i) {
			if (info.carried[i].first != info.inHandSlot) {
				continue;
			}
			net::PlayerOwnState::Weapon& w = s.weapons[0];
			w.active = true;
			w.index = static_cast<int>(i);
			for (const game::GameWorld::WeaponAmmo& ammo : m_round->weaponAmmo(player)) {
				if (ammo.weapon == info.carried[i].second) {
					w.ammo = ammo.inMag >= 0 ? ammo.inMag : ammo.rounds;
					w.magazines = ammo.magazines;
				}
			}
			break;
		}
	}
	// In a seat, the seat's three groups (0x005a98b0's group indices: each
	// group's active weapon's place in the group's list, 0x005cc5e0's order).
	if (info.alive && info.vehicle) {
		const auto seats = m_round->vehicleSeats(info.vehicle);
		if (info.seat >= 0 && static_cast<size_t>(info.seat) < seats.size()) {
			const auto& weapons = seats[static_cast<size_t>(info.seat)].weapons;
			for (int g = 0; g < 3; ++g) {
				const int active = m_round->seatWeaponIndex(info.vehicle, info.seat, g);
				if (active < 0 || static_cast<size_t>(active) >= weapons.size()) {
					continue;
				}
				int place = 0;
				for (int i = 0; i < active; ++i) {
					place += weapons[static_cast<size_t>(i)].group == g ? 1 : 0;
				}
				net::PlayerOwnState::Weapon& w = s.weapons[g];
				w.active = g == 0;
				w.index = place;
				if (const game::FireArm* arm = m_round->seatFireArm(info.vehicle, info.seat, active)) {
					w.ammo = arm->roundsInMag();
					w.magazines = arm->magsLeft();
				}
			}
		}
	}
	return s;
}

const HostedGame::Ghost* HostedGame::ghostOf(uint16_t id, size_t& part) const
{
	for (const auto& [object, ghost] : m_ghosts) {
		if (id >= ghost.first && id < ghost.first + ghost.layout->size()) {
			part = id - ghost.first;
			return &ghost;
		}
	}
	return nullptr;
}

net::TeamNetState HostedGame::teamStateOf() const
{
	// Both teams' tickets (the bits 0x400 and 0x400000, team +0x84, as
	// 0x0055f180 clamps them), each with its bit and value (+0x90, +0x8c:
	// not kept by the rewrite's game logic, 0).
	net::TeamNetState state;
	for (int team = 1; team <= 2; ++team) {
		const int tickets = static_cast<int>(std::lround(m_logic->team(team).tickets));
		state.setTickets(team, std::clamp(tickets, 0, 9999));
	}
	return state;
}

std::vector<net::SpawnGroupInfo> HostedGame::spawnGroupsNow() const
{
	// The control points a team may spawn at (the spawn manager's groups
	// the team's spawn screen offers, 0x0078e830): a group each, its id the
	// point's (what the client's 6/6 picks, setSpawnChoice), at the point's
	// ghost, with spawn points (hasPoints). The map bytes stay 0: the
	// rewrite's clients place a group by its ghost's object, and its points'
	// slot 0x4c flag (pointFlag) isn't kept.
	std::vector<net::SpawnGroupInfo> out;
	std::map<int, uint16_t> ghosts;
	for (const game::ControlPoint& cp : m_round->controlPoints()) {
		if (cp.object) {
			ghosts[cp.id] = ghostOfObject(cp.object->id());
		}
	}
	for (int team = 1; team <= 2; ++team) {
		for (const int id : m_round->spawnablePoints(team)) {
			const auto ghost = ghosts.find(id);
			if (ghost == ghosts.end() || ghost->second == 0 || id < 0 || id > 0xff) {
				continue;
			}
			net::SpawnGroupInfo info;
			info.id = id;
			info.team = team;
			info.ghost = ghost->second;
			info.hasPoints = true;
			out.push_back(info);
		}
	}
	return out;
}

int HostedGame::freeSpawnGroupId(const std::set<int>& taken) const
{
	for (int id = 0xc0; id <= 0xff; ++id) {
		if (!taken.count(id)) {
			return id;
		}
	}
	return -1; // 0x006347c0: none past 0xff
}

void HostedGame::trackSpawnGroups()
{
	std::map<int, net::SpawnGroupInfo> now;
	for (const net::SpawnGroupInfo& info : spawnGroupsNow()) {
		now.emplace(info.id, info);
	}
	// The groups made as the round goes, kept while their squad has a
	// leader or their vehicle can be spawned in; sent while they have a
	// place (the leader on foot and up, or the beacon; the vehicle).
	std::set<int> taken;
	for (const auto& [id, info] : now) {
		taken.insert(id);
	}
	std::map<std::pair<int, int>, uint16_t> squads;
	for (const game::GameWorld::PlayerInfo& p : m_round->playerList()) {
		if (p.squad == 0 || !p.squadLeader) {
			continue;
		}
		// Placed at the leader while he is up on foot (where the spawn
		// screen shows him), else at his beacon; a member who picks it
		// spawns at the beacon when there is one (0x004c7f10 moves the
		// squad's spawn point onto it).
		uint16_t ghost = 0;
		if (p.alive && !p.manDown && !p.vehicle && p.soldier) {
			ghost = ghostOfObject(p.soldier);
		} else if (const uint32_t beacon = m_round->squadBeacon(p.team, p.squad)) {
			ghost = ghostOfObject(beacon);
		}
		squads[{p.team, p.squad}] = ghost;
	}
	std::map<uint32_t, int> vehicles;
	for (int team = 1; team <= 2; ++team) {
		for (const uint32_t vehicle : m_round->spawnVehicles(team)) {
			vehicles[vehicle] = team;
		}
	}
	for (auto it = m_squadSpawnGroups.begin(); it != m_squadSpawnGroups.end();) {
		if (!squads.count(it->first)) {
			it = m_squadSpawnGroups.erase(it); // the squad is gone (or has no leader)
			continue;
		}
		taken.insert(it->second);
		++it;
	}
	for (auto it = m_vehicleSpawnGroups.begin(); it != m_vehicleSpawnGroups.end();) {
		if (!vehicles.count(it->first)) {
			it = m_vehicleSpawnGroups.erase(it);
			continue;
		}
		taken.insert(it->second);
		++it;
	}
	const auto add = [&](int id, int team, uint16_t ghost) {
		if (id >= 0 && ghost != 0) {
			net::SpawnGroupInfo info;
			info.id = id;
			info.team = team;
			info.ghost = ghost;
			info.hasPoints = true;
			now.emplace(id, info);
		}
	};
	for (const auto& [squad, ghost] : squads) {
		auto it = m_squadSpawnGroups.find(squad);
		if (it == m_squadSpawnGroups.end()) {
			const int id = freeSpawnGroupId(taken);
			if (id < 0) {
				continue;
			}
			taken.insert(id);
			it = m_squadSpawnGroups.emplace(squad, id).first;
		}
		add(it->second, squad.first, ghost);
	}
	for (const auto& [vehicle, team] : vehicles) {
		auto it = m_vehicleSpawnGroups.find(vehicle);
		if (it == m_vehicleSpawnGroups.end()) {
			const int id = freeSpawnGroupId(taken);
			if (id < 0) {
				continue;
			}
			taken.insert(id);
			it = m_vehicleSpawnGroups.emplace(vehicle, id).first;
		}
		add(it->second, team, ghostOfObject(vehicle));
	}
	for (const auto& [id, info] : m_spawnGroups) {
		if (!now.count(id)) {
			sendToPlaying(net::makeRemoveSpawnGroup(id, info.ghost));
		}
	}
	for (const auto& [id, info] : now) {
		const auto was = m_spawnGroups.find(id);
		if (was == m_spawnGroups.end() || was->second.team != info.team || was->second.ghost != info.ghost) {
			if (was != m_spawnGroups.end()) {
				sendToPlaying(net::makeRemoveSpawnGroup(id, was->second.ghost));
			}
			sendToPlaying(net::makeCreateSpawnGroup(info));
		}
	}
	m_spawnGroups = std::move(now);
}

net::GameEvent HostedGame::requestEvent(int team, int squad, int kind, const physics::Vec3& at) const
{
	net::SquadOrderInfo order;
	order.team = team;
	order.squad = squad;
	order.kind = kind;
	order.floats[0] = at.x;
	order.floats[1] = at.y;
	order.floats[2] = at.z;
	order.store = net::kSquadOrderStoreRequest;
	return net::makeSquadOrder(order);
}

void HostedGame::trackRequests()
{
	std::map<std::tuple<int, int, int>, physics::Vec3> now;
	for (int team = 1; team <= 2; ++team) {
		for (const game::GameWorld::SquadRequest& r : m_round->squadRequests(team)) {
			now[{r.team, r.squad, r.kind}] = r.position;
		}
	}
	// Taken off: SetAcceptOrder to everyone (0x004c3300 with 0).
	for (const auto& [key, at] : m_requestsSent) {
		if (!now.count(key)) {
			net::AcceptOrderInfo answer;
			answer.team = std::get<0>(key);
			answer.squad = std::get<1>(key);
			answer.store = net::kSquadOrderStoreRequest;
			answer.kind = std::get<2>(key);
			sendToPlaying(net::makeAcceptOrder(answer));
		}
	}
	// New: the event to everyone, 2/0x1e to the leader (the kind) and
	// 2/0x1d to the commander (the squad and the kind), 0x004c5720.
	for (const auto& [key, at] : now) {
		if (m_requestsSent.count(key)) {
			continue;
		}
		const auto [team, squad, kind] = key;
		sendToPlaying(requestEvent(team, squad, kind, at));
		int leader = -1;
		for (const game::GameWorld::PlayerInfo& p : m_round->playerList()) {
			if (p.team == team && p.squad == squad && p.squadLeader) {
				leader = p.id;
			}
		}
		const int commander = m_round->commander(team);
		std::string pair = net::int32Payload(squad) + net::int32Payload(kind);
		for (const auto& [id, client] : m_clients) {
			if (client.stage != Client::Stage::Playing) {
				continue;
			}
			if (client.player == leader && leader >= 0) {
				sendTo(id, net::makePostRemote(2, 0x1e, net::int32Payload(kind)));
			}
			if (client.player == commander && commander >= 0) {
				sendTo(id, net::makePostRemote(2, 0x1d, pair));
			}
		}
		if (onRequestStored && commander >= 0 && !clientHas(commander) && (leader < 0 || clientHas(leader))) {
			onRequestStored(team, squad, kind);
		}
	}
	m_requestsSent = std::move(now);
}

net::NetworkableState HostedGame::playerStateOf(int player, bool everyGroup) const
{
	game::GameWorld::PlayerInfo p;
	net::NetworkableState s;
	s.kind = net::NetworkableKind::Player;
	if (m_round->playerInfo(player, p)) {
		s.mask = 0x1 | 0x8 | 0x200 | 0x2000 | 0x8000000;
		s.player.alive = p.alive;
		s.player.kills = p.kills;
		s.player.deaths = p.deaths;
		s.player.teamKills = p.teamKills;
		s.player.score = p.kills;
		s.player.team = p.team;
		s.player.squad = p.squad;
		s.player.squadLeader = p.squadLeader;
		s.player.commander = p.commander;
		// The stats' rank (mask 0x4000, rank + 1; the client's 0x005a53b0):
		// the name tags' rank icons and the scoreboard.
		s.mask |= 0x4000;
		s.player.rank = p.rank;
		// The time outside the combat area (+0x294, mask 0x800: the
		// client's "TIME UNTIL PUNISH" countdown): the snapshot's byte is
		// (u8)floor(+0x294) (0x00592410 at 0x592856), no clamp, in 9 bits
		// (0x00596100); the client takes it back as whole seconds
		// (0x599209), so the first second outside reads 0.
		const float outside = m_round->outsideCombatArea(player);
		s.mask |= 0x800;
		s.player.value294 = outside > 0 ? static_cast<uint8_t>(static_cast<int>(std::floor(outside))) : 0;
		// The spawn tick and man down (0x20: player +0xdc, +0xe0).
		s.mask |= 0x20;
		s.player.manDown = p.manDown;
		s.player.spawnTick = m_tick + static_cast<uint32_t>(std::max(0.0f, p.respawnIn) * 30.0f);
		// The fire states (0x00592410's loop: masks 0x40 / 0x80 / 0x20000
		// from player +0x19c / +0x19d / +0x19e, the fire and alt fire held;
		// each bit goes when its group's fields change) with group 0's firearm,
		// the weapon in hand: its rounds and fire mode; and the presses
		// (0x4000000, +0x250).
		// The fire held goes out in a seat too (0x004bda00 sets +0x19c from
		// c_PIFire whatever the control object): the others' clients fire
		// the seat's guns from it (0x00590e10 -> 0x004cc7a0, RemoteGuns.h),
		// with each group's firearm's rounds and fire mode.
		s.mask |= 0x40 | 0x80 | 0x20000 | 0x4000000;
		s.player.fire[0].on = p.alive && p.firing;
		s.player.fire[1].on = p.alive && p.altFiring;
		for (net::PlayerNetState::FireState& f : s.player.fire) {
			f.rounds = p.roundsInMag;
			f.fireMode = p.fireMode;
			f.heat = p.heat;
		}
		if (p.alive && p.vehicle) {
			for (int g = 0; g < 3; ++g) {
				net::PlayerNetState::FireState& f = s.player.fire[static_cast<size_t>(g)];
				const int active = m_round->seatWeaponIndex(p.vehicle, p.seat, g);
				if (const game::FireArm* arm = active >= 0 ? m_round->seatFireArm(p.vehicle, p.seat, active) : nullptr) {
					f.rounds = arm->roundsInMag();
					f.fireMode = arm->fireMode();
				} else {
					f.rounds = -1;
				}
			}
		}
		s.player.firePresses = static_cast<int>(p.firePresses & 3);
		// A reload pressed (0x004bda00 -> player vtable 0x1fc, 0x0059b330:
		// +0x28c, which the snapshot 0x005929f0 turns into mask 0x10000 and
		// clears): every group's fields, so the others' clients reload the
		// soldier's weapon (0x005981a0 -> 0x00590e10).
		if (everyGroup) {
			s.mask |= 0x10000;
		}
	}
	return s;
}

net::GhostBlock HostedGame::ghostsFor(int id, size_t budgetBits)
{
	net::GhostBlock block;
	Client& client = m_clients[id];
	if (client.stage != Client::Stage::Playing) {
		return block;
	}
	block.present = true;
	block.tick = m_tick;
	// Our soldier, or the seat of the vehicle we sit in.
	game::GameWorld::PlayerInfo me;
	std::vector<uint16_t> first;
	if (m_round->playerInfo(client.player, me) && me.alive && ghostOfObject(me.soldier)) {
		net::ControlObjectRecord control;
		const net::GameServer::ClientInfo* info = m_server.client(id);
		control.lastMove = client.stepped >= 0 ? client.stepped : info ? info->lastMove : 0;
		const world::Object* body = m_round->objects().find(me.vehicle ? me.vehicle : me.soldier);
		if (body) {
			control.position = {body->position().x, body->position().y, body->position().z};
		}
		control.ghost = me.vehicle ? ghostOfObject(me.vehicle, me.seat) : ghostOfObject(me.soldier);
		if (me.vehicle) {
			control.vehicle = ghostOfObject(me.vehicle);
			first.push_back(ghostOfObject(me.vehicle));
		}
		first.push_back(ghostOfObject(me.soldier));
		// The prediction state (0x00580830): every group of the record's
		// ghost in mode 3 (a seat's: and its vehicle's root as the second
		// ghost), then the player's own state. Always a fresh state, in
		// the next history slot.
		size_t part = 0, rootPart = 0;
		const Ghost* ghost = ghostOf(control.ghost, part);
		if (me.vehicle && part > 0) {
			control.second = ghostOfObject(me.vehicle);
		}
		const Ghost* vehicle = control.second ? ghostOf(control.second, rootPart) : nullptr;
		if (ghost) {
			net::ControlState state;
			state.groups = statesOf(*ghost, part, 3);
			if (vehicle) {
				state.second = statesOf(*vehicle, rootPart, 3);
			}
			// A new control object starts again from fresh records (the
			// slot the client acknowledged was another ghost's).
			if (control.ghost != client.recordGhost) {
				client.recordGhost = control.ghost;
				client.ackedSlot = -1;
				client.relativeLeft = 0;
				client.freshIn.clear();
			}
			const std::vector<net::MotionHistory>* against = nullptr;
			if (client.ackedSlot < 0 || client.relativeLeft < 1) {
				// Fresh, into the next slot, our copy kept; 5 relative
				// records may follow (DAT_00a00ce4).
				client.historySlot = (client.historySlot + 1) & 7;
				state.slot = client.historySlot;
				if (client.ackedSlot == state.slot) {
					client.ackedSlot = -1; // our copy of it changes now
				}
				client.slots[static_cast<size_t>(state.slot)] = net::motionHistoryOf(state);
				client.freshIn[m_server.nextSequence(id)] = state.slot;
				client.relativeLeft = 5;
				++client.freshRecords;
			} else {
				// Against the slot the client has.
				state.recorded = true;
				state.slot = client.ackedSlot;
				against = &client.slots[static_cast<size_t>(state.slot)];
				--client.relativeLeft;
				++client.relativeRecords;
			}
			state.hasOwn = true;
			state.own = ownState(client.player);
			net::BitWriter rest;
			net::encodeControlState(rest, (*ghost->layout)[part], vehicle ? &(*vehicle->layout)[rootPart] : nullptr,
				&control.position, net::PlayerOwnStateHints {}, state, against);
			control.rest = rest.data();
			control.restBits = rest.bits();
		}
		block.control = control;
	}
	const bool demo = id == kDemoClient;
	const unsigned lengthBits = demo ? net::kDemoGhostStateLengthBits : net::kGhostStateLengthBits;
	const size_t maxUpdates = demo
		? static_cast<size_t>(net::demoRates(m_setup.settings ? m_setup.settings->demoQuality : 1).maxUpdates)
		: net::kGhostEntriesPerPacket;
	// The rescore (connection +0x20: at most every 0.1 s).
	if (client.rescoreIn <= 0) {
		rescore(client);
		client.rescoreIn = net::kGhostRescoreSeconds;
	}
	// What changed this frame, into every client's masks.
	snapshotGhosts();
	// What the block has used: the header and the record.
	size_t used = kGhostHeaderBits;
	if (block.control) {
		net::GhostBlock record;
		record.present = true;
		record.control = block.control;
		net::BitWriter w;
		net::writeGhostBlock(w, record, lengthBits);
		used = w.bits();
	}
	const uint32_t sequence = m_server.nextSequence(id);
	std::vector<Client::Carried>& carried = client.carried[sequence];
	carried.clear();
	const auto fits = [&](size_t bits) { return used + bits <= budgetBits; };
	// Lost removals first (gm +0x10c0).
	for (auto it = client.lostRemovals.begin(); it != client.lostRemovals.end();) {
		if (block.updates.size() >= maxUpdates || !fits(18)) {
			break;
		}
		if (client.ghosts.has(*it)) {
			it = client.lostRemovals.erase(it); // back in scope meanwhile
			continue;
		}
		net::GhostUpdate update;
		update.kind = 3;
		update.ghost = *it;
		block.updates.push_back(update);
		used += 18;
		carried.push_back({*it, true, {}});
		it = client.lostRemovals.erase(it);
	}
	// The heap (0x005823c0: a state out of scope or with a mask set) and
	// the writes (0x00582cd0): at most the block's updates; one that
	// doesn't fit is skipped until under 24 bits are left.
	for (const uint16_t ghost : client.ghosts.queue()) {
		if (block.updates.size() >= maxUpdates || used + net::kGhostMinimumBitsLeft > budgetBits) {
			break;
		}
		const net::GhostScheduler::State* st = client.ghosts.state(ghost);
		if (!st) {
			continue;
		}
		net::GhostUpdate update;
		update.ghost = ghost;
		if (st->status == net::GhostScheduler::Status::OutOfScope) {
			if (!fits(18)) {
				continue;
			}
			update.kind = 3;
			used += 18;
			block.updates.push_back(update);
			client.ghosts.written(ghost);
			carried.push_back({ghost, true, {}});
			continue;
		}
		const GhostNow* now = ghostNow(ghost);
		if (!now) {
			client.ghosts.written(ghost);
			continue;
		}
		size_t stateBits = 0;
		net::GhostMasks written {};
		std::vector<uint8_t> state = encodeGhost(*now, st->masks, stateBits, written);
		const size_t bits = 2 + 16 + 1 + lengthBits + stateBits;
		if (!fits(bits) || stateBits >= (size_t {1} << lengthBits)) {
			continue;
		}
		used += bits;
		update.kind = 1;
		update.state = std::move(state);
		update.stateBits = stateBits;
		block.updates.push_back(std::move(update));
		client.ghosts.written(ghost);
		carried.push_back({ghost, false, written});
	}
	return block;
}

// The rescore (0x0057dc10) for a client: everything not always out of
// scope (0x00581d40); the always ghosts (a negative base priority, and the
// players' networkables, which have no object) at 255; each tree within
// the radius scored by its root (0x0057d300) and each ghost of it at
// (score + 0.3 for a projectile) x its base priority (0x0057d560); each
// living teammate's soldier or vehicle at least 0.03. The demo connection
// takes everything at 1 (0x0057d8f0). Not rewritten: the commander's
// satellite scan and the UAV (enemies at 0.5), his soldiers with +0x29c
// set (0.03), OptimizeEnclosingObjectsNetworking, and the eye of player
// vt+0xd4 (a team object's). The client's own soldier and vehicle are
// scored like the rest here (the original skips the player's own, which
// the record carries); the eye is the soldier's head, kept from its last
// life while it has none (the original's camera object).
void HostedGame::rescore(Client& client)
{
	net::GhostScheduler& g = client.ghosts;
	g.beginRescore();
	// The always ghosts (the teams' networkable, the players', a negative
	// base priority) are given to a connection once, with every mask set
	// (0x0057dad0, and 0x004a9ba0 for one registered later); the rescore
	// keeps them in scope at 255 (0x00581d40).
	const auto always = [&](uint16_t ghost) {
		if (!g.has(ghost)) {
			g.setPriority(ghost, -1, false);
		}
	};
	always(net::kTeamStateGhost);
	for (const auto& [player, seen] : m_players) {
		always(seen.ghost);
	}
	const auto everyPart = [&](const Ghost& ghost, float p, bool keepMax) {
		if (!client.created.count(ghost.first)) {
			return;
		}
		for (size_t part = 0; part < ghost.layout->size(); ++part) {
			g.setPriority(static_cast<uint16_t>(ghost.first + part), p, keepMax);
		}
	};
	for (const auto& [object, ghost] : m_ghosts) {
		if (ghost.basePriority < 0 && client.created.count(ghost.first)) {
			for (size_t part = 0; part < ghost.layout->size(); ++part) {
				always(static_cast<uint16_t>(ghost.first + part));
			}
		}
	}
	game::GameWorld::PlayerInfo me;
	if (client.player >= 0 && m_round->playerInfo(client.player, me) && me.alive) {
		const float yaw = me.yaw * 0.017453292f, pitch = me.aimPitch * 0.017453292f;
		client.eye.position = {me.position.x, me.position.y + 1.6f, me.position.z};
		client.eye.forward = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
		client.eye.radius = net::ghostScopeRadius(850); // GSViewDistance's default (0x004cc8d0)
		client.hasEye = true;
	}
	const bool everything = client.player < 0 || !client.hasEye;
	for (const auto& [object, ghost] : m_ghosts) {
		if (ghost.basePriority < 0) {
			continue;
		}
		if (everything) {
			everyPart(ghost, 1, false);
			continue;
		}
		if (ghost.basePriority == 0) {
			continue; // never scored
		}
		net::GhostVec3 at, velocity;
		if (const game::Projectile* p = projectileOf(ghost)) {
			at = {p->position.x, p->position.y, p->position.z};
			velocity = {p->velocity.x, p->velocity.y, p->velocity.z};
		} else if (const world::Object* o = m_round->objects().find(object)) {
			at = {o->position().x, o->position().y, o->position().z};
			if (const auto m = m_motion.find(object); m != m_motion.end()) {
				velocity = {m->second.velocity.x, m->second.velocity.y, m->second.velocity.z};
			}
		} else {
			continue;
		}
		const float score = net::ghostScore(client.eye, at, velocity);
		if (score > 0) {
			everyPart(ghost, (score + (ghost.projectile ? net::kGhostProjectileBonus : 0)) * ghost.basePriority, false);
		}
	}
	if (everything) {
		return;
	}
	for (const auto& [player, seen] : m_players) {
		game::GameWorld::PlayerInfo p;
		if (player == client.player || !m_round->playerInfo(player, p) || !p.alive || p.team != me.team) {
			continue;
		}
		const auto it = m_ghosts.find(p.vehicle ? p.vehicle : p.soldier);
		if (it != m_ghosts.end() && it->second.basePriority > 0) {
			everyPart(it->second, net::kGhostTeammatePriority, true);
		}
	}
	// The commander's tools (0x0057dc10's end, the asset manager
	// DAT_00a6a390): for the team's commander with the satellite scan
	// running (0x005a2370: the team's +0xc of its 0x3c bytes over 0) every
	// enemy (radius -1), else with the team's UAV up (0x005a27c0: +0x4 over
	// 0, its point at +0x14) the enemies within the game logic's +0x230
	// (uavRadius, slot 0x28c) of it: each living enemy's soldier or vehicle
	// (vt+0x4c) whose networkable's base priority is over 0 at 0.5, kept
	// over a higher score (0x0057cfb0 -> 0x00581e80(..., 0.5, 1)). Then the
	// commander's own assets (0x0057db50: the objects of the game logic's
	// slot 0x1d0 list of his team whose +0x29c kind is 1-4) at 0.03.
	if (me.team != 1 && me.team != 2) {
		return;
	}
	const bool scan = me.commander && m_round->satelliteScanActive(me.team);
	physics::Vec3 uavAt;
	float uavRadius = 0;
	const bool uav = !scan && m_round->uavArea(me.team, uavAt, uavRadius);
	if (scan || uav) {
		for (const auto& [player, seen] : m_players) {
			game::GameWorld::PlayerInfo p;
			if (!m_round->playerInfo(player, p) || !p.alive || p.team == me.team || (p.team != 1 && p.team != 2)) {
				continue;
			}
			const uint32_t object = p.vehicle ? p.vehicle : p.soldier;
			const auto it = m_ghosts.find(object);
			if (it == m_ghosts.end() || it->second.basePriority <= 0) {
				continue;
			}
			if (uav && uavRadius > 0) {
				const world::Object* o = m_round->objects().find(object);
				if (!o) {
					continue;
				}
				const float dx = o->position().x - uavAt.x, dy = o->position().y - uavAt.y, dz = o->position().z - uavAt.z;
				if (dx * dx + dy * dy + dz * dz > uavRadius * uavRadius) {
					continue;
				}
			}
			everyPart(it->second, net::kGhostScanPriority, true);
		}
	}
	if (me.commander) {
		using Ability = game::GameWorld::CommanderAbility;
		for (const Ability ability : {Ability::OrbitalStrike, Ability::Artillery, Ability::Uav, Ability::Satellite}) {
			const auto state = m_round->abilityState(me.team, ability);
			const auto it = state.available ? m_ghosts.find(state.asset) : m_ghosts.end();
			if (it != m_ghosts.end()) {
				everyPart(it->second, net::kGhostTeammatePriority, true);
			}
		}
	}
}

// A packet's fate (0x005830e0): lost, the masks it carried that no later
// packet carried are set again, at 10000; each removal it carried goes
// first next packet.
void HostedGame::ghostLost(Client& client, uint32_t sequence, bool delivered)
{
	const auto it = client.carried.find(sequence);
	if (it == client.carried.end()) {
		return;
	}
	if (!delivered) {
		for (const Client::Carried& entry : it->second) {
			if (entry.removal) {
				client.lostRemovals.push_back(entry.ghost);
				continue;
			}
			net::GhostMasks lost = entry.masks;
			for (auto later = std::next(it); later != client.carried.end(); ++later) {
				for (const Client::Carried& next : later->second) {
					if (!next.removal && next.ghost == entry.ghost) {
						for (size_t g = 0; g < net::kGhostMaskGroups; ++g) {
							lost[g] &= ~next.masks[g];
						}
					}
				}
			}
			client.ghosts.markDirty(entry.ghost, lost, true);
		}
	}
	client.carried.erase(it);
}

void HostedGame::tick()
{
	if (!m_attached) {
		return; // between levels: the clients' packets wait in the socket
	}
	++m_tick;
	m_stateCache.clear();
	// The AutoRecorder starts when the round turns to playing (state 3).
	if (m_logic->roundState() == 3 && m_lastRoundState != 3) {
		startAutoDemo();
	}
	m_lastRoundState = m_logic->roundState();
	m_motion.clear();
	for (const game::GameWorld::VehicleInfo& v : m_round->vehicleList()) {
		m_motion[v.object] = {v.velocity, v.angularVelocity};
	}
	m_server.poll();
	for (const int id : std::exchange(m_loginChecks, {})) {
		playerInfo(id);
	}
	// The votes (0x0058fdc0): their ends, and what passed.
	{
		const net::ServerSettings* sv = m_setup.settings;
		m_votes.settings.enabled = !sv || sv->votingEnabled;
		if (sv) {
			m_votes.settings.voteTime = static_cast<float>(sv->voteTime);
			m_votes.settings.minPlayers = sv->minPlayersForVoting;
		}
		m_votes.maps = m_setup.maps ? m_setup.maps->count() : 0;
		m_votes.currentMap = m_setup.maps ? m_setup.maps->current() : -1;
		std::vector<game::Votes::Reply> replies;
		for (const game::Votes::Result& result : m_votes.tick(1.0f / 30, replies)) {
			if (result.kind == game::Votes::kMap) {
				m_mapVotes.push_back({5.0f, result.target}); // 5 s on (0x0058fb90)
			} else if (result.kind == game::Votes::kKick) {
				for (const auto& [id, client] : m_clients) {
					if (client.player == result.target) {
						m_kickLater.push_back({id, 0x15}); // "KICKED"
					}
				}
			} else if (result.kind == game::Votes::kMutiny && onMutiny) {
				onMutiny(result.team);
			}
		}
		sendVoteReplies(replies);
		for (auto it = m_mapVotes.begin(); it != m_mapVotes.end();) {
			it->first -= 1.0f / 30;
			if (it->first <= 0) {
				if (onMapVote) {
					onMapVote(it->second);
				}
				it = m_mapVotes.erase(it);
			} else {
				++it;
			}
		}
	}
	// Kicks decided while the server walked a client's events.
	for (const auto& [client, reason] : std::exchange(m_kickLater, {})) {
		m_server.kick(client, reason);
	}
	m_query.poll([this] { return report(); });
	// The networked ambient areas' effects (0x005ba9c0 -> 0x00510270, to
	// everyone, DAT_00a1b5d8 slot 0x94): the area's id, the Bundle child,
	// the activator's ghost, the offset.
	for (const game::GameWorld::AmbientEvent& a : m_round->takeAmbientEvents()) {
		const auto bits = [](float f) {
			uint32_t u;
			std::memcpy(&u, &f, sizeof u);
			return u;
		};
		const uint16_t ghost = a.attachedTo ? ghostOfObject(a.attachedTo, 0) : 0;
		sendToPlaying(eventOf(net::kEvAmbientEffectArea, {static_cast<uint32_t>(a.area) & 0xffff,
			static_cast<uint32_t>(a.index) & 0xff, ghost, bits(a.offset.x), bits(a.offset.y), bits(a.offset.z)}));
	}
	applyMoves();
	trackObjects();
	trackProjectiles();
	sendBattlefieldReports();
	trackPlayers();
	trackSpawnGroups();
	trackRequests();
	trackCommanderOffers();
	trackOrders();
	sendRadio();
	sendCloseTo();
	sendTeamDamage();
	// The scripts' end-of-round text (gl_sendEndOfRoundData, 0x00568930:
	// data block 8 to everyone, with sv.endOfRoundDelay and each player's
	// kills and deaths).
	if (m_logic->endOfRoundSerial() != m_endOfRoundSerialSent) {
		m_endOfRoundSerialSent = m_logic->endOfRoundSerial();
		net::RemoteWorld::EndOfRoundData data;
		data.text = m_logic->endOfRoundData();
		data.endOfRoundDelay = m_setup.settings ? m_setup.settings->endOfRoundDelay : 15;
		for (const game::GameWorld::PlayerInfo& p : m_round->playerList()) {
			net::RemoteWorld::EndOfRoundData::Player row;
			row.id = p.id;
			row.kills = p.kills;
			row.deaths = p.deaths;
			row.score = p.kills;
			data.players.push_back(row);
		}
		const std::string encoded = net::RemoteWorld::encodeEndOfRoundData(data);
		for (const auto& [id, client] : m_clients) {
			if (client.stage == Client::Stage::Loading || client.stage == Client::Stage::Connected) {
				continue;
			}
			sendTo(id, net::makeDataBlockStart(8, static_cast<uint32_t>(encoded.size())));
			for (size_t at = 0; at < encoded.size(); at += 63) {
				sendTo(id, net::makeDataBlockChunk(encoded.substr(at, 63)));
			}
		}
	}
	if (m_logic->ended() && !m_roundEndSent) {
		m_roundEndSent = true;
		sendToPlaying(eventOf(net::kEvEndOfRound,
			{0, static_cast<uint32_t>(m_logic->winner()), static_cast<uint32_t>(m_logic->victoryType())}));
	}
	for (auto& [id, client] : m_clients) {
		client.rescoreIn -= kTickSeconds;
	}
	m_server.sendPaced([this](int client, size_t bits) { return ghostsFor(client, bits); }, kTickSeconds);
	if (m_recordingCamera) {
		// 0x00432680 -> 0x00516b20 -> 0x005f9630, a frame each tick.
		net::CameraKeyframe frame;
		if (!localCamera || !localCamera(frame)) {
			frame = {};
			frame.followed = 0xffffffffu;
			frame.position[1] = 100;
		}
		m_cameraRecorder.record(1, m_tick, frame);
	}
	if (m_demo.isOpen()) {
		// The demo connection is paced by sv.demoQuality's rate too
		// (0x004b02a0 adds 1 / its packets a second).
		m_demoSendIn -= kTickSeconds;
		if (m_demoSendIn <= 0) {
			const net::DemoRates rates = net::demoRates(m_setup.settings ? m_setup.settings->demoQuality : 1);
			m_demoSendIn += 1.0f / static_cast<float>(rates.packetsPerSecond);
			flushDemo(static_cast<int32_t>(m_tick), true);
		}
	}
}

} // namespace dice::hfe::host
