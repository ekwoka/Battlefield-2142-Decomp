#include "GameClient.h"

#include "GameEvents.h"

#include <algorithm>
#include <random>

namespace dice::hfe::net {

GameClient::GameClient()
{
	m_connection.onAcknowledge = [this](uint32_t acknowledged, uint32_t mask) { onAcknowledge(acknowledged, mask); };
}

bool GameClient::connect(const std::string& host, uint16_t port, const GameClientOptions& options)
{
	m_options = options;
	m_world.clear();
	m_nextIncoming = 0;
	m_pendingIncoming.clear();
	m_outgoing.clear();
	m_nextOutgoing = 0;
	m_unacked.clear();
	m_packetBlock.clear();
	m_resend.clear();
	m_moves.clear();
	m_moveIndex = 0;
	m_levelsAnswered = 0;
	ConnectRequest request;
	request.password = options.password;
	if (!m_connection.start(host, port, request)) {
		m_state = State::Failed;
		return false;
	}
	m_state = State::Connecting;
	return true;
}

void GameClient::disconnect()
{
	if (m_state == State::Connected || m_state == State::Connecting) {
		m_connection.disconnect();
		m_state = State::Disconnected;
	}
}

bool GameClient::update()
{
	const auto onData = [this](const ConnectedPacket& packet) { onPacket(packet); };
	if (m_state == State::Connecting) {
		if (!m_connection.poll(onData)) {
			m_state = m_connection.result().outcome == ConnectResult::Outcome::Refused ? State::Refused : State::Failed;
			return false;
		}
		if (!m_connection.connected()) {
			return true;
		}
		m_state = State::Connected;
		m_lastTick = m_lastPing = std::chrono::steady_clock::now();
	}
	if (m_state != State::Connected) {
		return false;
	}
	if (!m_connection.poll(onData)) {
		m_state = State::Disconnected;
		return false;
	}
	const auto now = std::chrono::steady_clock::now();
	const auto tick = std::chrono::microseconds(1000000 / clientSendRate(m_options.connectionType));
	if (now - m_lastTick >= tick) {
		m_lastTick = now;
		sendTick();
	}
	// 0x0068f280: once connected a ping (type 7, 0x0068dc40 -> 0x006a9bb0)
	// every 3000 ms; the pong gives the round trip.
	if (now - m_lastPing >= std::chrono::milliseconds(3000)) {
		m_lastPing = now;
		m_connection.ping();
	}
	return true;
}

void GameClient::onPacket(const ConnectedPacket& packet)
{
	++m_packetsReceived;
	if (packet.payload.size() < 2) {
		return;
	}
	// The payload follows its 16-bit length (0x006a9cd0 queues it).
	const size_t length = static_cast<size_t>(packet.payload[0]) | static_cast<size_t>(packet.payload[1]) << 8;
	std::vector<uint8_t> payload(packet.payload.begin() + 2, packet.payload.end());
	if (length < payload.size()) {
		payload.resize(length);
	}
	GamePayload decoded;
	readGamePayload(payload, decoded);
	if (onPayload) {
		onPayload(decoded);
	}
	if (!decoded.complete) {
		++m_undecoded;
		m_lastUndecoded = payload;
	}
	if (decoded.hasEvents) {
		handleEvents(decoded.events);
	}
	if (decoded.complete) {
		m_world.apply(decoded.ghosts);
	}
}

void GameClient::handleEvents(const ReliableEventBlock& block)
{
	// Blocks are taken in sequence order; ones from before the next
	// expected are repeats (0x006447b0 keeps a map by sequence).
	const uint32_t ahead = (block.sequence - m_nextIncoming) & 0x1f;
	if (ahead >= 16 || m_pendingIncoming.count(block.sequence)) {
		return;
	}
	m_pendingIncoming[block.sequence] = block.events;
	for (auto it = m_pendingIncoming.find(m_nextIncoming); it != m_pendingIncoming.end();
		 it = m_pendingIncoming.find(m_nextIncoming)) {
		const std::vector<GameEvent> events = std::move(it->second);
		m_pendingIncoming.erase(it);
		m_nextIncoming = (m_nextIncoming + 1) & 0x1f;
		for (const GameEvent& event : events) {
			deliver(event);
		}
	}
}

void GameClient::deliver(const GameEvent& event)
{
	++m_eventsReceived;
	if (onEvent) {
		onEvent(event);
	}
	if (event.type == kEvChallenge) {
		// 0x004b2790: GameSpy's CD-key answer, the build and the profile,
		// then the connection type (0x004b48a0).
		static std::mt19937 random {std::random_device {}()};
		const std::string challenge = event.text(0).substr(0, event.text(0).find('\0'));
		const std::string response = cdKeyResponse(m_options.cdKey, challenge, static_cast<uint32_t>(random()));
		m_outgoing.push_back(makeChallengeResponse(response, static_cast<uint32_t>(random()), kBuild, m_options.profileId));
		m_outgoing.push_back(makeConnectionType(m_options.connectionType));
		return;
	}
	m_world.apply(event);
	if (autoAnswer) {
		answer(event);
	}
}

void GameClient::answer(const GameEvent& event)
{
	// 0x004b5eb0: each level block is answered with LoadComplete at once.
	while (m_levelsAnswered < m_world.levelsReceived()) {
		++m_levelsAnswered;
		m_outgoing.push_back(makePostRemote(kMsgNetworkCategory, kNeLoadComplete));
	}
	PostRemoteInfo info;
	if (event.type != kEvPostRemote || !parsePostRemote(event, info) || info.category != kMsgNetworkCategory) {
		return;
	}
	switch (info.id) {
	case kNeStartSimulation: // 0x004b6630 case 3: the connection type (0x004b48a0)
		m_outgoing.push_back(makeConnectionType(m_options.connectionType));
		break;
	case kNeDatabaseComplete: // case 4: echoed
	case kNeReset:            // case 5: echoed when the checksum matches (0x00678cf0)
		m_outgoing.push_back(makePostRemote(kMsgNetworkCategory, info.id));
		break;
	default:
		break;
	}
}

void GameClient::onAcknowledge(uint32_t acknowledged, uint32_t mask)
{
	for (auto it = m_packetBlock.begin(); it != m_packetBlock.end();) {
		const uint32_t behind = (acknowledged - it->first) & 0x3f;
		if (behind >= 32) {
			++it; // not acknowledged either way yet
			continue;
		}
		const uint32_t blockSequence = it->second;
		if ((mask >> behind & 1u) != 0) {
			m_unacked.erase(blockSequence);
		} else if (m_unacked.count(blockSequence)
			&& std::find(m_resend.begin(), m_resend.end(), blockSequence) == m_resend.end()) {
			m_resend.push_back(blockSequence);
		}
		it = m_packetBlock.erase(it);
	}
}

void GameClient::sendTick()
{
	GamePayload payload;
	// The input: this frame's move and the two before (0x00590450). Only
	// once the server made our player: 0x00590740 writes a lone 0 bit while
	// the input manager has no player (+0x84), and the server's reader
	// hands moves to its player for the connection.
	if (m_world.localPlayer() >= 0) {
		m_moves.push_back(m_world.input().toMove(m_moveIndex++));
		while (m_moves.size() > 3) {
			m_moves.pop_front();
		}
		payload.input.present = true;
		// 0x0057b040: the networkable manager's delay (+0x50, 100 ms) plus
		// 1000 x the client's interpolation time (0x006b4e70; 0 with the
		// defaults); the server's player keeps it at +0xf4 (lag compensation).
		payload.input.value9 = std::min<uint32_t>(m_options.lagCompensationMs, 511);
		payload.input.moves.assign(m_moves.begin(), m_moves.end());
	}

	// A lost block goes first, then new events (at most 15 blocks in flight).
	std::optional<uint32_t> blockSequence;
	while (!m_resend.empty() && !blockSequence) {
		const uint32_t sequence = m_resend.front();
		m_resend.pop_front();
		const auto it = m_unacked.find(sequence);
		if (it != m_unacked.end()) {
			payload.hasEvents = true;
			payload.events = it->second;
			payload.events.resent = true;
			blockSequence = sequence;
			++m_blocksResent;
		}
	}
	if (!blockSequence && !m_outgoing.empty() && m_unacked.size() < 15) {
		ReliableEventBlock block;
		block.sequence = m_nextOutgoing;
		BitWriter size;
		while (!m_outgoing.empty() && block.events.size() < 255) {
			BitWriter one;
			writeGameEvent(one, m_outgoing.front());
			if (!block.events.empty() && size.bits() + one.bits() > 8 * 1000) {
				break;
			}
			size.append(one);
			block.events.push_back(m_outgoing.front());
			m_outgoing.pop_front();
		}
		m_unacked[block.sequence] = block;
		m_nextOutgoing = (m_nextOutgoing + 1) & 0x1f;
		payload.hasEvents = true;
		payload.events = block;
		blockSequence = block.sequence;
	}
	if (m_connection.sendData(writeGamePayload(payload))) {
		++m_packetsSent;
		if (blockSequence) {
			m_packetBlock[m_connection.lastSentSequence()] = *blockSequence;
		}
	}
}

void GameClient::selectTeam(int team)
{
	sendEvent(makePostRemote(kMsgNetworkCategory, kNeSelectTeam, int32Payload(team)));
}

void GameClient::selectSpawnGroup(int group)
{
	sendEvent(makePostRemote(kMsgNetworkCategory, kNeSelectSpawnGroup, int32Payload(group)));
}

void GameClient::selectKit(int kit, const std::array<int32_t, 4>& unlocks)
{
	sendEvent(makeSelectKit(kit, unlocks));
}

void GameClient::suicide()
{
	sendEvent(makePostRemote(kMsgNetworkCategory, kNeSuicide));
}

void GameClient::say(const std::string& text, uint32_t channel)
{
	ChatMessage message;
	message.channel = channel;
	message.source = 2;
	message.player = m_world.localPlayer();
	message.text = text;
	for (const GameEvent& event : stringBlockEvents(encodeChat(message))) {
		sendEvent(event);
	}
}

namespace {

GameEvent eventOf(uint32_t type, std::vector<uint32_t> values, std::vector<std::string> bytes = {})
{
	GameEvent e;
	e.type = type;
	e.values = std::move(values);
	e.bytes = std::move(bytes);
	return e;
}

uint32_t u8(int v) { return static_cast<uint32_t>(v) & 0xff; }

} // namespace

int clientSendRate(uint32_t connectionType)
{
	if (connectionType == 0) {
		return kConnectionRates[0].clientPacketsPerSecond;
	}
	const ConnectionRates& rates = kConnectionRates[std::min<uint32_t>(connectionType, 5)];
	return std::max(rates.clientPacketsPerSecond, 30);
}

void GameClient::joinSquad(int squad)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	sendEvent(eventOf(kEvJoinSquad, {u8(me ? me->team : 0), u8(m_world.localPlayer()), u8(squad)}));
}

void GameClient::leaveSquad(int squad, int player, bool flag)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	sendEvent(eventOf(kEvLeaveSquad,
		{u8(me ? me->team : 0), u8(player < 0 ? m_world.localPlayer() : player), u8(squad), flag ? 1u : 0u}));
}

void GameClient::invite(int player, int squad, bool fromLeader)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	sendEvent(eventOf(kEvInvite, {u8(me ? me->team : 0), u8(player), u8(squad), fromLeader ? 1u : 0u}));
}

void GameClient::setSquadLeader(int player, int squad)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	sendEvent(eventOf(kEvSetSquadLeader, {u8(me ? me->team : 0), u8(player), u8(squad)}));
}

void GameClient::setSquadName(int squad, const std::string& name)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	// 0x0050d960: squad, team, the player (the server joins it to a new
	// squad), the name's length (at most 31) and the name.
	sendEvent(eventOf(kEvChangeSquadName, {u8(squad), u8(me ? me->team : 0), u8(m_world.localPlayer())}, {name.substr(0, 31)}));
}

void GameClient::setPrivateSquad(int squad, bool isPrivate)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	sendEvent(eventOf(kEvSetPrivateSquad, {u8(me ? me->team : 0), u8(squad), isPrivate ? 1u : 0u}));
}

void GameClient::squadRequest(int type)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	sendEvent(eventOf(kEvRequest, {u8(me ? me->squad : 0), u8(me ? me->team : 0), static_cast<uint32_t>(type) & 3}));
}

void GameClient::commander(int kind)
{
	// 0x0050cb00 takes it only with the sender's own player id.
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	sendEvent(eventOf(kEvCommander, {static_cast<uint32_t>(kind), u8(me ? me->team : 0),
		static_cast<uint32_t>(m_world.localPlayer())}));
}

void GameClient::startVote(int type, int target)
{
	// target, choice, +0x10, target, +0x11, +0x12, voter, phase, type, u32
	const uint32_t me = u8(m_world.localPlayer());
	sendEvent(eventOf(kEvVote, {u8(target), 0, 0, u8(target), 0, 0, me, 0, u8(type), 0}));
}

void GameClient::castVote(int type, int target, int choice)
{
	const uint32_t me = u8(m_world.localPlayer());
	sendEvent(eventOf(kEvVote, {u8(target), u8(choice), 0, u8(target), 0, 0, me, 4, u8(type), 0}));
}

void GameClient::radioMessage(int index, int channel, int a, int c)
{
	// 0x004b3020: the channel also in the byte after it, the last field 0.
	sendEvent(eventOf(kEvRadioMessage, {static_cast<uint32_t>(index) & 0xffff, u8(a), static_cast<uint32_t>(channel) & 3,
		u8(channel), static_cast<uint32_t>(std::clamp(c, 0, 0x12)), 0}));
}

void GameClient::toggleFreeCamera()
{
	GameEvent event;
	event.type = kEvToggleFreeCamera;
	sendEvent(event);
}

} // namespace dice::hfe::net
