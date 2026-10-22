#include "VoipServer.h"

#include <algorithm>
#include <cstdio>

namespace dice::hfe::voip {

namespace {

// The game server's silence after which it counts as gone (0x1000c2b8).
constexpr double kGameServerTimeout = 28.0;
// Packets handled per update (0x10008760).
constexpr int kPacketsPerUpdate = 128;
// The receive buffer (0x800); a forwarded packet must stay below it.
constexpr size_t kMaxPacket = 0x800;

std::string addressText(const net::UdpServerSocket::Address& a)
{
	const uint8_t* ip = reinterpret_cast<const uint8_t*>(&a.ip);
	const uint8_t* port = reinterpret_cast<const uint8_t*>(&a.port);
	char text[32];
	std::snprintf(text, sizeof text, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port[0] << 8 | port[1]);
	return text;
}

} // namespace

bool VoipServerInstance::start(const Options& options)
{
	stop();
	if (!m_socket.bind(options.port, options.loopbackOnly)) {
		log("create socket failed");
		return false;
	}
	m_password = options.password;
	m_random.seed(options.seed ? options.seed : std::random_device {}());
	m_running = true;
	m_gameServer = {};
	m_lastGameServerPacket = m_lastAccepted = -1;
	newChallenge();
	log("Instance Started  Port: " + std::to_string(m_socket.port()));
	return true;
}

void VoipServerInstance::stop()
{
	m_socket.close();
	m_running = false;
	removeAllClients();
	m_routing = {};
	m_gameServer = {};
}

void VoipServerInstance::newChallenge()
{
	// 0x10002510: ten letters; the answer is md5(challenge + password).
	m_challenge = makeChallenge(m_random);
	m_expected = sessionStringFor(m_challenge, m_password);
}

void VoipServerInstance::log(const std::string& line)
{
	if (onLog) {
		onLog(line);
	}
}

void VoipServerInstance::update(double now)
{
	if (!m_running) {
		return;
	}
	std::vector<uint8_t> packet;
	Address from;
	for (int i = 0; i < kPacketsPerUpdate; ++i) {
		if (m_socket.receiveFrom(packet, from, 0) <= 0) {
			break;
		}
		handlePacket(packet, from, now);
	}
	// 0x100026c0: the game server went quiet.
	if (gameServerConnected() && now - m_lastAccepted > kGameServerTimeout) {
		log("BF2142server " + addressText(m_gameServer) + " disconnected");
		m_gameServer = {};
		m_counters.invalidAuthentication = m_counters.invalidVersion = m_counters.inUse = 0;
	}
}

void VoipServerInstance::handlePacket(const std::vector<uint8_t>& packet, const Address& from, double now)
{
	if (packet.empty()) {
		return;
	}
	const uint8_t type = packet.back();
	if (type >= kVoiceDefaultChannelSet && type <= kKeepAlive) {
		handleClient(packet, from);
		return;
	}
	if (type == kSetPlayerData || type == kRemovePlayerData || type == kResetPlayerRouting || type == kFullRoutingDataSet
		|| type == kDebugData) {
		ServerPacket parsed;
		if (authenticate(packet, from, now, parsed)) {
			handleGameServer(parsed);
		}
	}
}

bool VoipServerInstance::authenticate(const std::vector<uint8_t>& packet, const Address& from, double now, ServerPacket& out)
{
	// 0x10001d40.
	if (m_lastGameServerPacket >= 0 && now - m_lastGameServerPacket > kGameServerTimeout) {
		log("BF2142 Server Connection Timed Out");
		newChallenge();
	}
	m_lastGameServerPacket = now;
	if (!parseServerPacket(packet, out)) {
		return false;
	}
	if (gameServerConnected() && !(from == m_gameServer) && now - m_lastAccepted < kGameServerTimeout) {
		++m_counters.inUse;
		log("BF2142server " + addressText(from) + " attempting to connect to instance in use");
		return false;
	}
	if (out.sessionString == m_expected) {
		if (out.version != kVersionString) {
			++m_counters.invalidVersion;
			log("BF2142server " + addressText(from) + " attempted to connect with an invalid version");
			return false;
		}
		if (!(from == m_gameServer)) {
			m_counters.invalidAuthentication = 0;
			log("BF2142server " + addressText(from) + " connected");
		}
		m_gameServer = from;
		m_lastAccepted = now;
		return true;
	}
	// 0x100021f0: the challenge, then count a wrong answer (the first
	// packet, before any challenge, carries 20 spaces).
	m_socket.sendTo(from, buildChallengePacket(m_challenge));
	if (out.sessionString != unauthenticatedSessionString()) {
		++m_counters.invalidAuthentication;
		log("BF2142server " + addressText(from) + " attempting to connect with invalid authentication");
	}
	return false;
}

void VoipServerInstance::handleGameServer(const ServerPacket& packet)
{
	// 0x10004040.
	switch (packet.type) {
	case kSetPlayerData: {
		PlayerData player;
		if (decodePlayerData(packet.payload.data(), packet.payload.size(), player)) {
			setPlayerData(player);
		}
		break;
	}
	case kRemovePlayerData:
		if (packet.payload.size() >= 2) {
			removeSession(static_cast<uint16_t>(packet.payload[0] | packet.payload[1] << 8));
		}
		break;
	case kResetPlayerRouting:
		// 0x100021d0.
		removeAllClients();
		m_routing = {};
		log("  Remove All Clients");
		break;
	case kFullRoutingDataSet: {
		RoutingTable table;
		if (decodeRoutingTable(packet.payload, table)) {
			fullRouting(table);
		}
		break;
	}
	case kDebugData:
	default:
		break;
	}
}

void VoipServerInstance::setPlayerData(const PlayerData& player)
{
	// 0x10003850: the routing record, then the client by its session.
	if (player.playerId < 64) {
		m_routing[player.playerId] = routingRecordFor(player);
	} else if (player.playerId == kServerPlayerId) {
		m_routing[kServerPlayerSlot] = routingRecordFor(player);
	}
	ChannelPlan plan;
	if (!planChannels(player, plan)) {
		log("Squad out of range");
		return;
	}
	if (player.session == 0) {
		return; // 0x10005720 refuses session 0
	}
	// 0x10008600: replaced whole, so its address comes back with its next packet.
	eraseClient(player.session);
	Client client;
	client.session = player.session;
	client.playerId = player.playerId;
	client.incoming = plan.incoming;
	client.sets = std::move(plan.sets);
	m_clients[client.session] = client;
	m_listeners[static_cast<size_t>(client.incoming)].push_back(client.session);
	log("  Update Client Data - Client Id: " + std::to_string(player.playerId) + "  Session Id: " + std::to_string(player.session));
}

void VoipServerInstance::removeSession(uint16_t session)
{
	// 0x10003360: the player's routing record goes too.
	const auto it = m_clients.find(session);
	if (it == m_clients.end()) {
		return;
	}
	if (it->second.playerId < 64) {
		m_routing[it->second.playerId] = {};
	} else if (it->second.playerId == kServerPlayerId) {
		m_routing[kServerPlayerSlot] = {};
	}
	eraseClient(session);
	log("  Remove Client Session Id: " + std::to_string(session));
}

void VoipServerInstance::eraseClient(uint16_t session)
{
	// 0x100085d0: out of every channel and the table.
	for (std::vector<uint16_t>& channel : m_listeners) {
		channel.erase(std::remove(channel.begin(), channel.end(), session), channel.end());
	}
	m_clients.erase(session);
}

void VoipServerInstance::removeAllClients()
{
	m_clients.clear();
	for (std::vector<uint16_t>& channel : m_listeners) {
		channel.clear();
	}
}

void VoipServerInstance::fullRouting(const RoutingTable& table)
{
	// 0x10003dd0: put right every record that differs.
	if (table == m_routing) {
		return;
	}
	log("BEGIN Handling Packet Full Routing Data Set - Mismatch Detected");
	for (size_t slot = 0; slot < kRoutingSlots; ++slot) {
		const RoutingRecord old = m_routing[slot];
		const RoutingRecord now = table[slot];
		if (old == now) {
			continue;
		}
		const uint16_t playerId = slot == kServerPlayerSlot ? kServerPlayerId : static_cast<uint16_t>(slot);
		if (old.flags != 0) {
			m_routing[slot] = {};
			removeSession(old.session);
		}
		if (now.flags != 0) {
			setPlayerData(playerDataFor(now, playerId));
		}
		m_routing[slot] = now;
	}
	log("END Handling Packet Full Routing Data Set");
}

void VoipServerInstance::handleClient(const std::vector<uint8_t>& packet, const Address& from)
{
	// 0x10004040 / 0x10008b40.
	ClientPacket parsed;
	if (!parseClientPacket(packet, parsed)) {
		return;
	}
	const auto it = m_clients.find(parsed.session);
	if (it == m_clients.end()) {
		++m_counters.unknownSession;
		return;
	}
	Client& client = it->second;
	client.address = from;
	switch (parsed.type) {
	case kVoiceDefaultChannelSet:
	case kVoiceAlternateChannelSet: {
		// 0x100086c0 / 0x10008700 / 0x100055c0.
		const uint8_t set = parsed.type == kVoiceDefaultChannelSet ? 0 : parsed.channels.alternateSet;
		client.multi = false;
		client.multiSets.clear();
		if (set >= client.sets.size()) {
			return;
		}
		client.selectedSet = set;
		++m_counters.voicePackets;
		route(client, parsed.payload, {set});
		break;
	}
	case kVoiceMultiChannelSet: {
		// 0x100089d0 / 0x10005630.
		client.multi = true;
		client.multiSets.clear();
		for (uint8_t set : parsed.channels.multiSets) {
			if (set >= client.sets.size()) {
				return;
			}
			client.multiSets.push_back(set);
		}
		++m_counters.voicePackets;
		route(client, parsed.payload, client.multiSets);
		break;
	}
	default: // keep-alive and the debug log switches
		break;
	}
}

void VoipServerInstance::route(const Client& from, const std::vector<uint8_t>& payload, const std::vector<uint8_t>& sets)
{
	// 0x10008830: the speaker's player id goes on the end.
	if (payload.size() + 4 >= kMaxPacket) {
		return;
	}
	const std::vector<uint8_t> out = buildForwardedVoice(payload, from.playerId);
	for (uint8_t set : sets) {
		// 0x100088e0: every listener of every channel of the set, but not
		// the speaker, nor anyone without an address yet, nor one at the
		// speaker's own address.
		for (uint8_t channel : from.sets[set]) {
			if (channel >= kChannelCount) {
				continue;
			}
			for (uint16_t session : m_listeners[channel]) {
				if (session == from.session) {
					continue;
				}
				const auto it = m_clients.find(session);
				if (it == m_clients.end() || it->second.address.ip == 0 || it->second.address.port == 0
					|| it->second.address == from.address) {
					continue;
				}
				if (m_socket.sendTo(it->second.address, out)) {
					++m_counters.forwarded;
				}
			}
		}
	}
}

const VoipServerInstance::Client* VoipServerInstance::client(uint16_t session) const
{
	const auto it = m_clients.find(session);
	return it == m_clients.end() ? nullptr : &it->second;
}

const std::vector<uint16_t>& VoipServerInstance::listeners(int channel) const
{
	static const std::vector<uint16_t> none;
	return channel >= 0 && channel < kChannelCount ? m_listeners[static_cast<size_t>(channel)] : none;
}

// --- CBF2VoipServer

bool VoipServer::initialize()
{
	if (m_running) {
		return false; // 0x10001110: already started
	}
	m_running = true;
	return true;
}

bool VoipServer::createInstance(const VoipServerInstance::Options& options, int& id)
{
	id = -1;
	if (!m_running) {
		return false;
	}
	auto instance = std::make_unique<VoipServerInstance>();
	if (!instance->start(options)) {
		return false;
	}
	id = m_nextId++;
	m_instances.push_back({id, std::move(instance)});
	return true;
}

bool VoipServer::releaseInstance(int id)
{
	for (auto it = m_instances.begin(); it != m_instances.end(); ++it) {
		if (it->id == id) {
			it->instance->stop();
			m_instances.erase(it);
			return true;
		}
	}
	return false;
}

void VoipServer::releaseAllInstances()
{
	for (Entry& entry : m_instances) {
		entry.instance->stop();
	}
	m_instances.clear();
}

void VoipServer::shutdown()
{
	releaseAllInstances();
	m_running = false;
	m_nextId = 1;
}

void VoipServer::update(double now)
{
	if (!m_running) {
		return;
	}
	for (Entry& entry : m_instances) {
		entry.instance->update(now);
	}
}

VoipServerInstance* VoipServer::instance(size_t index)
{
	return index < m_instances.size() ? m_instances[index].instance.get() : nullptr;
}

VoipServerInstance* VoipServer::instanceById(int id)
{
	for (Entry& entry : m_instances) {
		if (entry.id == id) {
			return entry.instance.get();
		}
	}
	return nullptr;
}

} // namespace dice::hfe::voip
