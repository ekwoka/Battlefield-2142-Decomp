#include "GameServer.h"

#include "GameClient.h"

#include "BitStream.h"

#include <algorithm>
#include <cstdio>

namespace dice::hfe::net {

namespace {

constexpr auto kSilenceLimit = std::chrono::seconds(30);

std::string addressText(const UdpServerSocket::Address& a)
{
	const uint8_t* ip = reinterpret_cast<const uint8_t*>(&a.ip);
	const uint16_t port = static_cast<uint16_t>((a.port & 0xff) << 8 | a.port >> 8);
	char text[32];
	std::snprintf(text, sizeof(text), "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);
	return text;
}

} // namespace

bool GameServer::start(const GameServerOptions& options)
{
	m_options = options;
	m_clients.clear();
	m_running = m_socket.bind(options.port, options.loopbackOnly);
	return m_running;
}

void GameServer::stop()
{
	for (auto& [id, client] : m_clients) {
		m_socket.sendTo(client.address, disconnectPacket(static_cast<uint8_t>(id), 0));
	}
	m_clients.clear();
	m_socket.close();
	m_running = false;
}

uint32_t GameServer::now() const
{
	return static_cast<uint32_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count());
}

GameServer::Client* GameServer::find(const UdpServerSocket::Address& from)
{
	for (auto& [id, client] : m_clients) {
		if (client.address == from) {
			return &client;
		}
	}
	return nullptr;
}

std::vector<GameServer::ClientInfo> GameServer::clients() const
{
	std::vector<ClientInfo> out;
	for (const auto& [id, client] : m_clients) {
		out.push_back(client.info);
	}
	return out;
}

const GameServer::ClientInfo* GameServer::client(int id) const
{
	const auto it = m_clients.find(id);
	return it == m_clients.end() ? nullptr : &it->second.info;
}

void GameServer::poll()
{
	if (!m_running) {
		return;
	}
	std::vector<uint8_t> datagram;
	UdpServerSocket::Address from;
	while (m_socket.receiveFrom(datagram, from, 0) > 0) {
		handle(datagram, from);
		datagram.clear();
	}
	const auto now = std::chrono::steady_clock::now();
	std::vector<int> silent;
	for (auto& [id, client] : m_clients) {
		if (now - client.lastHeard > kSilenceLimit) {
			silent.push_back(id);
		} else if (client.acknowledged && now - client.lastPing >= std::chrono::seconds(3)) {
			// Pings every 3 s as the client does (0x0068f280): the pongs give the round trip.
			client.lastPing = now;
			m_socket.sendTo(client.address, pingPacket(static_cast<uint8_t>(id), client.notify, this->now()));
		}
	}
	for (const int id : silent) {
		kick(id, 0);
	}
}

void GameServer::refuse(const UdpServerSocket::Address& to, uint32_t reason)
{
	// Type 3 (0x0068de90): the reason, no message.
	BitWriter out;
	out.write(static_cast<uint32_t>(PacketType::Refuse), 4);
	out.write(0, 8);
	out.write(reason, 32);
	out.write(0, 1);
	m_socket.sendTo(to, out.data());
}

void GameServer::handleConnect(BitReader& in, const UdpServerSocket::Address& from)
{
	// 0x00689d30's checks, in its order.
	const uint32_t protocol = in.read(32);
	const uint32_t build = in.read(32);
	in.read(1); // the anti-cheat's bit
	in.readText(4);
	const std::string password = in.readText(32);
	const std::string mod = in.readText(32);
	if (in.bad() || protocol != kProtocol || build != kBuild) {
		refuse(from, 9);
		return;
	}
	if (!m_options.password.empty() && password != m_options.password) {
		refuse(from, 0x11);
		return;
	}
	if (mod != m_options.modDirectory) {
		refuse(from, 0x24);
		return;
	}
	if (m_options.banned && m_options.banned(addressText(from))) {
		refuse(from, 0x16); // banned (admin.banPlayer's address bans)
		return;
	}
	Client* client = find(from);
	if (!client) {
		if (static_cast<int>(m_clients.size()) >= m_options.maxClients) {
			refuse(from, 0x1e);
			return;
		}
		int id = 1;
		while (m_clients.count(id) && id < 0x80) {
			++id;
		}
		if (id >= 0x80) {
			refuse(from, 7);
			return;
		}
		client = &m_clients[id];
		client->info.id = id;
		client->info.address = addressText(from);
		client->address = from;
	}
	client->lastHeard = client->lastPing = std::chrono::steady_clock::now();
	// Type 2 (0x00689400): the connection id, our clock and the anti-cheat's bit.
	BitWriter accept;
	accept.write(static_cast<uint32_t>(PacketType::Accept), 4);
	accept.write(0, 8);
	accept.write(static_cast<uint32_t>(client->info.id), 8);
	accept.write(now(), 32);
	accept.write(0, 1);
	m_socket.sendTo(from, accept.data());
}

void GameServer::handle(const std::vector<uint8_t>& datagram, const UdpServerSocket::Address& from)
{
	BitReader in(datagram.data(), datagram.size());
	const uint32_t type = in.read(4);
	in.read(8);
	if (type == static_cast<uint32_t>(PacketType::Connect)) {
		handleConnect(in, from);
		return;
	}
	Client* client = find(from);
	if (!client) {
		return;
	}
	client->lastHeard = std::chrono::steady_clock::now();
	const int id = client->info.id;
	switch (type) {
	case static_cast<uint32_t>(PacketType::Acknowledge):
		if (!client->acknowledged) {
			client->acknowledged = true;
			// The CD-key challenge (0x00509cc0): ten bytes and a length-prefixed second field.
			GameEvent challenge;
			challenge.type = kEvChallenge;
			challenge.bytes = {m_options.challenge.substr(0, 9) + '\0', ""};
			client->events.queue(challenge);
		}
		return;
	case static_cast<uint32_t>(PacketType::Disconnect):
		drop(id);
		return;
	default:
		break;
	}
	ConnectedPacket packet;
	if (!readConnectedPacket(datagram, packet)) {
		return;
	}
	client->events.acknowledge(packet.acknowledged, packet.mask);
	client->notify.lastAcknowledged = packet.acknowledged;
	// Each data packet of ours the header now settles, delivered or lost
	// (the notify layer's report to the ghost manager: 0x005830e0).
	for (auto it = client->unsettled.begin(); it != client->unsettled.end();) {
		const uint32_t behind = (packet.acknowledged - *it) & 0x3f;
		if (behind >= 32) {
			++it;
			continue;
		}
		const uint32_t sequence = *it;
		it = client->unsettled.erase(it);
		if (onNotified) {
			onNotified(id, sequence, (packet.mask >> behind & 1u) != 0);
		}
	}
	if (type == 7) {
		if (packet.sequenced) {
			client->notify.receive(packet.sequence, false);
		}
		m_socket.sendTo(from, pongPacket(static_cast<uint8_t>(id), client->notify, packet.time, now()));
	} else if (type == 8) {
		if (packet.sequenced) {
			client->notify.receive(packet.sequence, false);
		}
		if (packet.time <= now()) {
			client->info.roundTrip = now() - packet.time;
		}
	} else if (type == 15) {
		if (client->notify.receive(packet.sequence, true)) {
			handleData(*client, packet);
		}
	}
}

void GameServer::handleData(Client& client, const ConnectedPacket& packet)
{
	if (packet.payload.size() < 2) {
		return;
	}
	const size_t length = static_cast<size_t>(packet.payload[0]) | static_cast<size_t>(packet.payload[1]) << 8;
	std::vector<uint8_t> payload(packet.payload.begin() + 2, packet.payload.end());
	if (length < payload.size()) {
		payload.resize(length);
	}
	GamePayload decoded;
	if (!readGamePayload(payload, decoded)) {
		return;
	}
	const int id = client.info.id;
	// The moves (0x00590990 -> the player's input, 0x005838d0): each index once.
	if (decoded.input.present) {
		client.info.lagMs = decoded.input.value9;
		for (const PlayerMove& move : decoded.input.moves) {
			if (move.index > client.info.lastMove) {
				client.info.lastMove = move.index;
				if (client.info.joined && onMove) {
					onMove(id, move);
				}
			}
		}
	}
	if (decoded.hasEvents) {
		client.events.receive(decoded.events, [this, id](const GameEvent& event) {
			if (const auto it = m_clients.find(id); it != m_clients.end()) {
				deliver(it->second, event);
			}
		});
	}
}

void GameServer::deliver(Client& client, const GameEvent& event)
{
	if (event.type == kEvChallengeResponse) {
		// 0x00509eb0's fields: the response, a number, the build, the profile.
		client.info.cdKeyHash = event.text(0).substr(0, 32);
		client.info.profileId = event.u(2);
		return;
	}
	if (event.type == kEvConnectionType) {
		// The type (0x0050a070): the rate settings for this client (0x004a6160).
		client.info.connectionType = event.u(0);
		// Type 6 and over leave the rates as they are (0x004a6160).
		if (client.info.connectionType <= 5) {
			const ConnectionRates& rates = kConnectionRates[client.info.connectionType];
			client.info.packetsPerSecond = rates.serverPacketsPerSecond;
			client.info.packetBytes = rates.serverMaxBytes;
		}
		if (!client.info.joined) {
			client.info.joined = true;
			if (onJoined) {
				onJoined(client.info.id);
			}
			return;
		}
	}
	if (client.info.joined && onEvent) {
		onEvent(client.info.id, event);
	}
}

void GameServer::sendTick(const std::function<GhostBlock(int client)>& ghosts)
{
	for (auto& [id, client] : m_clients) {
		if (client.acknowledged) {
			sendTo(client, id, [&ghosts](int c, size_t) { return ghosts ? ghosts(c) : GhostBlock {}; });
		}
	}
}

void GameServer::sendPaced(const std::function<GhostBlock(int client, size_t ghostBits)>& ghosts, float seconds)
{
	for (auto& [id, client] : m_clients) {
		if (!client.acknowledged) {
			continue;
		}
		client.sendIn -= seconds;
		if (client.sendIn > 0) {
			continue;
		}
		client.sendIn += 1.0f / static_cast<float>(std::max(client.info.packetsPerSecond, 1));
		sendTo(client, id, ghosts);
	}
}

void GameServer::sendTo(Client& client, int id, const std::function<GhostBlock(int client, size_t ghostBits)>& ghosts)
{
	{
		GamePayload payload; // the input block: a lone 0 bit from a server
		const std::optional<ReliableEventBlock> block = client.events.next();
		if (block) {
			payload.hasEvents = true;
			payload.events = *block;
		}
		if (client.info.joined && ghosts) {
			// The writers share the stream in order (0x0057b380): input,
			// events, then the ghosts with what is left.
			const size_t used = writeGamePayload(payload).size() * 8;
			const size_t cap = static_cast<size_t>(client.info.packetBytes) * 8;
			payload.ghosts = ghosts(id, cap > used ? cap - used : 0);
		}
		client.notify.sendSequence = client.notify.sendSequence < 0 ? 1 : (client.notify.sendSequence + 1) & 0x3f;
		const uint32_t sequence = static_cast<uint32_t>(client.notify.sendSequence);
		m_socket.sendTo(client.address, dataPacket(static_cast<uint8_t>(id), sequence, client.notify, writeGamePayload(payload)));
		client.unsettled.insert(sequence);
		if (block) {
			client.events.sent(sequence, block->sequence);
		}
	}
}

uint32_t GameServer::nextSequence(int client) const
{
	const auto it = m_clients.find(client);
	if (it == m_clients.end()) {
		return 0;
	}
	const int last = it->second.notify.sendSequence;
	return last < 0 ? 1u : static_cast<uint32_t>(last + 1) & 0x3f;
}

void GameServer::send(int client, const GameEvent& event)
{
	if (const auto it = m_clients.find(client); it != m_clients.end()) {
		it->second.events.queue(event);
	}
}

void GameServer::broadcast(const GameEvent& event)
{
	for (auto& [id, client] : m_clients) {
		if (client.info.joined) {
			client.events.queue(event);
		}
	}
}

void GameServer::kick(int client, uint32_t reason)
{
	const auto it = m_clients.find(client);
	if (it == m_clients.end()) {
		return;
	}
	// 0x0068e520 sends the disconnect three times.
	for (int i = 0; i < 3; ++i) {
		m_socket.sendTo(it->second.address, disconnectPacket(static_cast<uint8_t>(client), reason));
	}
	drop(client);
}

void GameServer::drop(int id)
{
	const auto it = m_clients.find(id);
	if (it == m_clients.end()) {
		return;
	}
	const bool joined = it->second.info.joined;
	m_clients.erase(it);
	if (joined && onLeft) {
		onLeft(id);
	}
}

} // namespace dice::hfe::net
