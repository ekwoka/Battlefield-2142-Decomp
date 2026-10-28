#include "HostedGame.h"

#include "Game/GameLogic.h"
#include "Net/BitStream.h"
#include "Net/GameProtocol.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dice::hfe::host {

namespace {

constexpr float kDegreesPerMouseLookUnit = 5.0f; // 0x0058a6d0's turn per PIMouseLook unit
constexpr size_t kGhostBudgetBits = 8 * 1100;    // a packet is at most 1472 bytes (0x5c0)
constexpr uint32_t kRefreshTicks = 60;           // every ghost again after two seconds

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
	m_server.onJoined = [this](int client) { joined(client); };
	// A delivered packet with a fresh record makes its slot the client's
	// (0x005830e0: +0x20f0 = the packet's slot, for the record's ghost).
	m_server.onNotified = [this](int id, uint32_t sequence, bool delivered) {
		const auto found = m_clients.find(id);
		if (found == m_clients.end()) {
			return;
		}
		Client& client = found->second;
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
	for (const auto& [id, client] : m_clients) {
		if (onLog) {
			onLog("client " + std::to_string(id) + "'s moves: " + std::to_string(client.movesDropped) + " dropped, "
				+ std::to_string(client.ticksStarved) + " ticks without one");
		}
	}
	m_server.stop();
}

bool HostedGame::start()
{
	net::GameServerOptions options;
	options.port = m_setup.gamePort;
	options.loopbackOnly = m_setup.loopbackOnly;
	options.password = m_setup.password;
	options.maxClients = m_setup.maxPlayers;
	if (!m_server.start(options)) {
		return false;
	}
	if (m_setup.queryPort != 0 && !m_query.start(m_setup.queryPort, m_setup.loopbackOnly) && onLog) {
		onLog("no query port " + std::to_string(m_setup.queryPort) + ": the server isn't listed on the LAN");
	}
	return true;
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

void HostedGame::changeLevel(game::GameWorld& round, game::GameLogic& logic, world::ObjectTemplateManager& templates,
	HostSetup setup)
{
	m_round = &round;
	m_logic = &logic;
	m_templates = &templates;
	m_attached = true;
	m_setup.level = setup.level;
	m_setup.gameMode = setup.gameMode;
	m_setup.mapSize = setup.mapSize;
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
	m_nextGhost = 1;
	m_roundEndSent = false;
	for (auto& [id, client] : m_clients) {
		// A new round: a new player of the client's team, a new database.
		const int team = client.team ? client.team : 1;
		client.player = m_round->addPlayer(client.name, team, false);
		m_names[client.player] = client.name;
		if (const auto& kits = m_logic->team(team).kits; !kits.empty()) {
			m_round->setKit(client.player, kits.front().name, kits.front().soldier);
		}
		client.created.clear();
		client.sent.clear();
		client.sentTick.clear();
		client.cursor = 0;
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
		m_server.send(id, net::makeDataBlockStart(type, static_cast<uint32_t>(data.size())));
		for (size_t at = 0; at < data.size(); at += 63) {
			m_server.send(id, net::makeDataBlockChunk(data.substr(at, 63)));
		}
	};
	block(5, net::RemoteWorld::encodeMapList({{m_setup.level, m_setup.gameMode, static_cast<uint32_t>(m_setup.mapSize)}}, 0));
	net::RemoteWorld::LevelInfo level;
	level.gameMode = m_setup.gameMode;
	level.levelDirectory = "levels/" + m_setup.level + "/";
	level.mapName = m_setup.level;
	level.maxPlayers = m_setup.mapSize;
	level.commander = true;
	block(2, net::RemoteWorld::encodeLevel(level));
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
	r.teams = {m_logic->team(1).name, m_logic->team(2).name};
	return r;
}

// --- joining

void HostedGame::joined(int id)
{
	Client& client = m_clients[id];
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
	client.player = m_round->addPlayer("Player" + std::to_string(id), team, false);
	client.name = "Player" + std::to_string(id);
	client.team = team;
	m_names[client.player] = client.name;
	if (const auto& kits = m_logic->team(team).kits; !kits.empty()) {
		m_round->setKit(client.player, kits.front().name, kits.front().soldier);
	}
	client.stage = Client::Stage::Loading;
	// The settings (block 0), the map list (5) and the level (2).
	net::RemoteWorld::ServerSettings settings;
	settings.name = m_setup.serverName;
	settings.initial = true;
	settings.gamePort = m_server.port();
	settings.gameSpyPort = m_setup.queryPort;
	settings.playersNeededToStart = 1;
	const auto block = [&](uint32_t type, const std::string& data) {
		m_server.send(id, net::makeDataBlockStart(type, static_cast<uint32_t>(data.size())));
		for (size_t at = 0; at < data.size(); at += 63) {
			m_server.send(id, net::makeDataBlockChunk(data.substr(at, 63)));
		}
	};
	block(0, net::RemoteWorld::encodeSettings(settings));
	sendLevel(id);
	if (onLog) {
		onLog("client " + std::to_string(id) + " (" + m_server.client(id)->address + ") joined as player "
			+ std::to_string(client.player) + ", team " + std::to_string(team));
	}
}

void HostedGame::left(int id)
{
	const auto it = m_clients.find(id);
	if (it == m_clients.end()) {
		return;
	}
	const int player = it->second.player;
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
	return net::makeCreatePlayer(info);
}

std::vector<net::GameEvent> HostedGame::createEvents(const Ghost& ghost) const
{
	const world::Object* object = m_round->objects().find(ghost.object);
	net::CreateObjectInfo info;
	info.templateId = ghost.objectTemplate->id();
	info.ghost = ghost.first;
	info.count = static_cast<uint32_t>(ghost.layout->size());
	if (object) {
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
			m_server.send(id, createPlayerEvent(p, p.id == client.player, seen->second.ghost));
		}
	}
	for (const auto& [object, ghost] : m_ghosts) {
		for (const net::GameEvent& e : createEvents(ghost)) {
			m_server.send(id, e);
		}
		client.created.insert(ghost.first);
	}
	for (const auto& [player, seen] : m_players) {
		if (seen.alive && seen.soldier) {
			game::GameWorld::PlayerInfo info;
			m_round->playerInfo(player, info);
			m_server.send(id, eventOf(net::kEvHandlePickup, {static_cast<uint32_t>(player) & 0xff, ghostOfObject(info.kit),
				ghostOfObject(seen.soldier), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
		}
	}
	m_server.send(id, net::makePostRemote(net::kMsgNetworkCategory, net::kNeDatabaseComplete));
	m_server.send(id, net::makePostRemote(net::kMsgNetworkCategory, net::kNeReset, net::int32Payload(0)));
	client.stage = Client::Stage::Database;
}

void HostedGame::sendToPlaying(const net::GameEvent& event)
{
	for (const auto& [id, client] : m_clients) {
		if (client.stage != Client::Stage::Loading && client.stage != Client::Stage::Connected) {
			m_server.send(id, event);
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
			m_server.send(id, net::makePostRemote(net::kMsgNetworkCategory, net::kNeStartSimulation));
			m_server.send(id, net::makePostRemote(net::kMsgGameCategory, 1, net::int32Payload(1)));
			const uint32_t end = m_logic->timeLimit > 0 ? static_cast<uint32_t>(m_logic->timeLimit * 30) : 0;
			m_server.send(id, eventOf(net::kEvBeginRound, {3, end, static_cast<uint32_t>(-1)}));
			client.stage = Client::Stage::Playing;
			break;
		}
		case net::kNeSelectSpawnGroup: // 0x004c1d50's request
			m_round->setSpawnChoice(player, info.payloadInt());
			client.spawnRequested = true;
			break;
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
				sendToPlaying(out);
			}
			if (onLog) {
				onLog("chat " + nameOf(player) + ": " + line.text);
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
	case net::kEvLeaveSquad:
		if (e.i(1) == player) {
			m_round->joinSquad(player, 0);
		}
		return;
	case net::kEvChangeSquadName: // a new squad (0x004c6720)
		if (e.i(2) == player) {
			m_round->joinSquad(player, e.i(0));
		}
		return;
	case net::kEvCommander:
		if (e.i(2) == player && e.u(0) == 4) {
			m_round->applyForCommander(player);
		} else if (e.i(2) == player && e.u(0) == 2) {
			m_round->resignCommander(player);
		}
		return;
	default:
		return;
	}
}

// --- each tick

void HostedGame::applyMoves()
{
	for (auto& [id, client] : m_clients) {
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
		if (client.spawnRequested && m_round->playerInfo(client.player, me) && !me.alive) {
			if (m_round->spawnSoldier(client.player)) {
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
		m_nextGhost = static_cast<uint16_t>(m_nextGhost + layout->size());
		m_ghosts[object->id()] = ghost;
		for (auto& [id, client] : m_clients) {
			if (client.stage == Client::Stage::Database || client.stage == Client::Stage::Playing) {
				for (const net::GameEvent& e : createEvents(ghost)) {
					m_server.send(id, e);
				}
				client.created.insert(ghost.first);
			}
		}
	}
	for (auto it = m_ghosts.begin(); it != m_ghosts.end();) {
		if (present.count(it->first)) {
			++it;
			continue;
		}
		const uint16_t ghost = it->second.first;
		for (auto& [id, client] : m_clients) {
			if (client.created.erase(ghost)) {
				m_server.send(id, eventOf(net::kEvDestroyObject, {ghost, 0}));
			}
			for (size_t part = 0; part < it->second.layout->size(); ++part) {
				client.sent.erase(static_cast<uint16_t>(ghost + part));
				client.sentTick.erase(static_cast<uint16_t>(ghost + part));
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
					m_server.send(id, createPlayerEvent(p, p.id == client.player, fresh.ghost));
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
	if (control && body) {
		s.yaw = body->yawDegrees;
		s.velocity = {body->velocity.x, body->velocity.y, body->velocity.z};
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
			// The control's twist and catch-up (+0x278, +0x280) and the
			// node's acceleration and velocity change (slots 0x74, 0xc4).
			s.mask |= 0x4 | 0x8 | 0x200 | 0x80000;
			s.pitch = control->twist();
			s.roll = control->catchUp();
			s.soldierAcceleration = {body->acceleration.x, body->acceleration.y, body->acceleration.z};
			s.soldierImpulse = {body->impulse.x, body->impulse.y, body->impulse.z};
		}
	}
	return s;
}

std::vector<net::NetworkableState> HostedGame::statesOf(const Ghost& ghost, size_t part, unsigned mode) const
{
	const net::GhostPartLayout& layout = (*ghost.layout)[part];
	std::vector<net::NetworkableState> states(layout.groups.size());
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
			// keep their armour on the root, so every part is whole.
			if (h.kind == net::NetworkableKind::SimpleObject && h.armoredChildrenKnown && !h.armoredChildren.empty()) {
				s.mask |= 0x800000;
				s.childDamage.assign(h.armoredChildren.size(), 8);
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
			if (g != 0 || h.seatCameras != 0) {
				break;
			}
			const std::vector<int> occupants = m_round->vehicleOccupants(ghost.object);
			const int player = part < occupants.size() ? occupants[part] : -1;
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

std::vector<uint8_t> HostedGame::stateOf(const Ghost& ghost, size_t part, size_t& bits) const
{
	const net::GhostPartLayout& layout = (*ghost.layout)[part];
	net::BitWriter out;
	net::encodeGhostState(out, layout, nullptr, statesOf(ghost, part, 2));
	bits = out.bits();
	return out.data();
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

std::vector<uint8_t> HostedGame::playerStateOf(int player, size_t& bits) const
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
	}
	net::NetworkableHints hints;
	hints.kind = net::NetworkableKind::Player;
	net::BitWriter out;
	net::encodeNetworkable(out, hints, nullptr, s);
	bits = out.bits();
	return out.data();
}

net::GhostBlock HostedGame::ghostsFor(int id)
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
	size_t used = 0;
	const auto add = [&](uint16_t ghost, std::vector<uint8_t> state, size_t bits) {
		const auto sent = client.sent.find(ghost);
		const auto when = client.sentTick.find(ghost);
		const bool due = when == client.sentTick.end() || m_tick - when->second >= kRefreshTicks;
		if (!due && sent != client.sent.end() && sent->second == state) {
			return true; // unchanged
		}
		if (used + bits + 32 > kGhostBudgetBits || block.updates.size() >= 255) {
			return false;
		}
		used += bits + 32;
		net::GhostUpdate update;
		update.kind = 1;
		update.ghost = ghost;
		update.state = state;
		update.stateBits = bits;
		block.updates.push_back(std::move(update));
		client.sent[ghost] = std::move(state);
		client.sentTick[ghost] = m_tick;
		return true;
	};
	// The players' networkables.
	for (const auto& [player, seen] : m_players) {
		size_t bits = 0;
		std::vector<uint8_t> state = playerStateOf(player, bits);
		add(seen.ghost, std::move(state), bits);
	}
	std::vector<const Ghost*> order;
	for (const uint16_t g : first) {
		for (const auto& [object, ghost] : m_ghosts) {
			if (ghost.first == g) {
				order.push_back(&ghost);
			}
		}
	}
	std::vector<const Ghost*> all;
	for (const auto& [object, ghost] : m_ghosts) {
		if (client.created.count(ghost.first)) {
			all.push_back(&ghost);
		}
	}
	for (size_t i = 0; i < all.size(); ++i) {
		order.push_back(all[(client.cursor + i) % all.size()]);
	}
	size_t visited = 0;
	for (const Ghost* ghost : order) {
		bool fitted = true;
		for (size_t part = 0; part < ghost->layout->size() && fitted; ++part) {
			size_t bits = 0;
			std::vector<uint8_t> state = stateOf(*ghost, part, bits);
			fitted = add(static_cast<uint16_t>(ghost->first + part), std::move(state), bits);
		}
		if (!fitted) {
			break;
		}
		++visited;
	}
	if (!all.empty()) {
		client.cursor = (client.cursor + std::max<size_t>(1, visited > first.size() ? visited - first.size() : 1)) % all.size();
	}
	return block;
}

void HostedGame::tick()
{
	if (!m_attached) {
		return; // between levels: the clients' packets wait in the socket
	}
	++m_tick;
	m_motion.clear();
	for (const game::GameWorld::VehicleInfo& v : m_round->vehicleList()) {
		m_motion[v.object] = {v.velocity, v.angularVelocity};
	}
	m_server.poll();
	m_query.poll([this] { return report(); });
	applyMoves();
	trackObjects();
	trackPlayers();
	if (m_logic->ended() && !m_roundEndSent) {
		m_roundEndSent = true;
		sendToPlaying(eventOf(net::kEvEndOfRound,
			{0, static_cast<uint32_t>(m_logic->winner()), static_cast<uint32_t>(m_logic->victoryType())}));
	}
	m_server.sendTick([this](int client) { return ghostsFor(client); });
}

} // namespace dice::hfe::host
