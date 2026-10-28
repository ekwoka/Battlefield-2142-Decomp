#include "HostedGame.h"

#include "Game/GameLogic.h"
#include "Net/BitStream.h"
#include "Net/GameProtocol.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
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
	: m_round(round), m_logic(logic), m_templates(templates), m_setup(std::move(setup))
{
	m_server.onJoined = [this](int client) { joined(client); };
	m_server.onLeft = [this](int client) { left(client); };
	m_server.onEvent = [this](int client, const net::GameEvent& e) { event(client, e); };
	m_server.onMove = [this](int client, const net::PlayerMove& move) {
		if (const auto it = m_clients.find(client); it != m_clients.end()) {
			it->second.moves.push_back(move);
		}
	};
}

HostedGame::~HostedGame() { m_server.stop(); }

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

std::string HostedGame::nameOf(int player) const
{
	if (const auto it = m_names.find(player); it != m_names.end()) {
		return it->second;
	}
	game::GameWorld::PlayerInfo info;
	return m_round.playerInfo(player, info) ? info.name : std::string();
}

net::ServerReport HostedGame::report() const
{
	// The keys 0x0044xxxx's qr2 callbacks give for a hosted game.
	net::ServerReport r;
	int players = 0;
	for (const auto& p : m_round.playerList()) {
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
	r.teams = {m_logic.team(1).name, m_logic.team(2).name};
	return r;
}

// --- joining

void HostedGame::joined(int id)
{
	Client& client = m_clients[id];
	// A team with fewer players.
	int counts[3] {};
	for (const auto& p : m_round.playerList()) {
		if (p.team == 1 || p.team == 2) {
			++counts[p.team];
		}
	}
	int team = counts[1] <= counts[2] ? 1 : 2;
	if (!m_freePlayers.empty()) {
		client.player = m_freePlayers.back();
		m_freePlayers.pop_back();
		game::GameWorld::PlayerInfo info;
		if (m_round.playerInfo(client.player, info)) {
			team = info.team; // the player keeps its team
		}
	} else {
		client.player = m_round.addPlayer("Player" + std::to_string(id), team, false);
	}
	client.name = "Player" + std::to_string(id);
	m_names[client.player] = client.name;
	if (const auto& kits = m_logic.team(team).kits; !kits.empty()) {
		m_round.setKit(client.player, kits.front().name, kits.front().soldier);
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
	block(5, net::RemoteWorld::encodeMapList({{m_setup.level, m_setup.gameMode, static_cast<uint32_t>(m_setup.mapSize)}}, 0));
	net::RemoteWorld::LevelInfo level;
	level.gameMode = m_setup.gameMode;
	level.levelDirectory = "levels/" + m_setup.level + "/";
	level.mapName = m_setup.level;
	level.maxPlayers = m_setup.mapSize;
	level.commander = true;
	block(2, net::RemoteWorld::encodeLevel(level));
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
	m_clients.erase(it);
	// The player stays in the round (GameWorld keeps its players) without
	// a soldier, for the next client to take.
	game::GameWorld::PlayerInfo info;
	if (m_round.playerInfo(player, info) && info.alive) {
		m_round.setHitPoints(info.soldier, 0);
	}
	m_freePlayers.push_back(player);
	sendToPlaying(eventOf(net::kEvDestroyPlayer, {static_cast<uint32_t>(player) & 0xff}));
	m_players.erase(player);
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
	const world::Object* object = m_round.objects().find(ghost.object);
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
	for (const auto& p : m_round.playerList()) {
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
			m_round.playerInfo(player, info);
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
			const uint32_t end = m_logic.timeLimit > 0 ? static_cast<uint32_t>(m_logic.timeLimit * 30) : 0;
			m_server.send(id, eventOf(net::kEvBeginRound, {3, end, static_cast<uint32_t>(-1)}));
			client.stage = Client::Stage::Playing;
			break;
		}
		case net::kNeSelectSpawnGroup: // 0x004c1d50's request
			m_round.setSpawnChoice(player, info.payloadInt());
			client.spawnRequested = true;
			break;
		case net::kNeSuicide: {
			game::GameWorld::PlayerInfo me;
			if (m_round.playerInfo(player, me) && me.alive) {
				m_round.setHitPoints(me.soldier, 0);
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
		if (m_round.playerInfo(player, me)) {
			const auto& kits = m_logic.team(me.team).kits;
			const size_t kit = e.u(0);
			if (kit < kits.size()) {
				m_round.setKit(player, kits[kit].name, kits[kit].soldier);
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
			m_round.joinSquad(player, e.i(2));
		}
		return;
	case net::kEvLeaveSquad:
		if (e.i(1) == player) {
			m_round.joinSquad(player, 0);
		}
		return;
	case net::kEvChangeSquadName: // a new squad (0x004c6720)
		if (e.i(2) == player) {
			m_round.joinSquad(player, e.i(0));
		}
		return;
	case net::kEvCommander:
		if (e.i(2) == player && e.u(0) == 4) {
			m_round.applyForCommander(player);
		} else if (e.i(2) == player && e.u(0) == 2) {
			m_round.resignCommander(player);
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
		game::SoldierInput input = client.held;
		input.turn = 0;
		input.use = false;
		input.switchSeat = 0;
		input.selectItem = 0;
		for (const net::PlayerMove& move : client.moves) {
			// 0x005838d0 puts the move into the player's PlayerInput.
			input.throttle = move.axis(net::kPIThrottle);
			input.strafe = move.axis(net::kPIYaw);
			input.turn += move.axis(net::kPIMouseLookX) * kDegreesPerMouseLookUnit;
			client.pitch = std::clamp(client.pitch - move.axis(net::kPIMouseLookY) * kDegreesPerMouseLookUnit, -89.0f, 89.0f);
			input.sprint = move.trigger(net::kPISprint);
			input.fire = move.trigger(net::kPIFire);
			input.altFire = move.trigger(net::kPIAltFire);
			input.jump = move.trigger(net::kPIAction);
			input.pose = move.trigger(net::kPILie) ? game::SoldierPose::Prone
				: move.trigger(net::kPICrouch)     ? game::SoldierPose::Crouch
												   : game::SoldierPose::Stand;
			const bool use = move.trigger(net::kPIUse);
			input.use = input.use || (use && !client.useWas);
			client.useWas = use;
			int seat = 0, item = 0;
			for (int i = 0; i < 8; ++i) {
				seat = move.trigger(net::kPIPositionSelect1 + i) ? i + 1 : seat;
			}
			for (int i = 0; i < 9; ++i) {
				item = move.trigger(net::kPIWeaponSelect1 + i) ? i + 1 : item;
			}
			if (seat && seat != client.seatWas) {
				input.switchSeat = seat;
			}
			if (item && item != client.itemWas) {
				input.selectItem = item;
			}
			client.seatWas = seat;
			client.itemWas = item;
		}
		input.aimPitch = client.pitch;
		client.moves.clear();
		client.held = input;
		m_round.setInput(client.player, input);
		game::GameWorld::PlayerInfo me;
		if (client.spawnRequested && m_round.playerInfo(client.player, me) && !me.alive) {
			if (m_round.spawnSoldier(client.player)) {
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
			net::buildGhostLayout(net::ghostTemplateFrom(m_templates, t)));
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
	for (const auto& object : m_round.objects().objects()) {
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
	for (const auto& p : m_round.playerList()) {
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
	for (const auto& p : m_round.playerList()) {
		auto seen = m_players.find(p.id);
		if (seen == m_players.end()) {
			if (std::find(m_freePlayers.begin(), m_freePlayers.end(), p.id) != m_freePlayers.end()) {
				continue; // a client's that left
			}
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

std::vector<uint8_t> HostedGame::stateOf(const Ghost& ghost, size_t part, size_t& bits) const
{
	const net::GhostPartLayout& layout = (*ghost.layout)[part];
	std::vector<net::NetworkableState> states(layout.groups.size());
	const world::Object* object = m_round.objects().find(ghost.object);
	if (object && part == 0 && !states.empty()) {
		net::NetworkableState& s = states[0];
		const net::NetworkableHints& h = layout.groups[0].hints;
		s.kind = h.kind;
		const net::Vec3f position {object->position().x, object->position().y, object->position().z};
		switch (h.kind) {
		case net::NetworkableKind::Soldier: {
			const int player = m_round.playerOf(ghost.object);
			game::GameWorld::PlayerInfo info;
			const bool known = player >= 0 && m_round.playerInfo(player, info) && info.soldier == ghost.object;
			s.mask = 0x1 | 0x2 | 0x10 | 0x40 | 0x100;
			s.position = position;
			s.yaw = known ? info.yaw : object->rotation().x;
			s.aimPitch = known ? info.aimPitch : 0;
			s.velocity = known ? net::Vec3f {info.velocity.x, info.velocity.y, info.velocity.z} : net::Vec3f {};
			const float maximum = settingFloat(*ghost.objectTemplate, "armor.maxHitPoints", 100);
			s.health = maximum > 0 ? std::clamp(m_round.hitPoints(ghost.object) / maximum, 0.0f, 1.0f) : 0;
			break;
		}
		case net::NetworkableKind::SimpleObject:
		case net::NetworkableKind::GenericProjectile:
			s.mask = 0x2 | 0x1 | (h.hasArmor ? 0x10u : 0u);
			s.position = position;
			s.orientation = quaternionFromYawPitchRoll(object->rotation().x, object->rotation().y, object->rotation().z);
			s.health = m_round.hitPoints(ghost.object);
			break;
		case net::NetworkableKind::ControlPoint:
			s.mask = 0x1 | 0x4 | 0x20;
			s.team = object->team();
			for (const game::ControlPoint& cp : m_round.controlPoints()) {
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
			s.health = m_round.hitPoints(ghost.object);
			s.position = position;
			break;
		case net::NetworkableKind::Kit:
			s.mask = 0x1;
			s.position = position;
			break;
		default:
			break;
		}
	}
	net::BitWriter out;
	net::encodeGhostState(out, layout, nullptr, states);
	bits = out.bits();
	return out.data();
}

std::vector<uint8_t> HostedGame::playerStateOf(int player, size_t& bits) const
{
	game::GameWorld::PlayerInfo p;
	net::NetworkableState s;
	s.kind = net::NetworkableKind::Player;
	if (m_round.playerInfo(player, p)) {
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
	if (m_round.playerInfo(client.player, me) && me.alive && ghostOfObject(me.soldier)) {
		net::ControlObjectRecord control;
		const net::GameServer::ClientInfo* info = m_server.client(id);
		control.lastMove = info ? info->lastMove : 0;
		const world::Object* body = m_round.objects().find(me.vehicle ? me.vehicle : me.soldier);
		if (body) {
			control.position = {body->position().x, body->position().y, body->position().z};
		}
		control.ghost = me.vehicle ? ghostOfObject(me.vehicle, me.seat) : ghostOfObject(me.soldier);
		if (me.vehicle) {
			control.vehicle = ghostOfObject(me.vehicle);
			first.push_back(ghostOfObject(me.vehicle));
		}
		first.push_back(ghostOfObject(me.soldier));
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
	++m_tick;
	m_server.poll();
	m_query.poll([this] { return report(); });
	applyMoves();
	trackObjects();
	trackPlayers();
	if (m_logic.ended() && !m_roundEndSent) {
		m_roundEndSent = true;
		sendToPlaying(eventOf(net::kEvEndOfRound,
			{0, static_cast<uint32_t>(m_logic.winner()), static_cast<uint32_t>(m_logic.victoryType())}));
	}
	m_server.sendTick([this](int client) { return ghostsFor(client); });
}

} // namespace dice::hfe::host
