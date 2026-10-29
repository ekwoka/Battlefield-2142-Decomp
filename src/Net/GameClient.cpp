#include "GameClient.h"

#include "ContentCheck.h"

#include "GameEvents.h"

#include <algorithm>
#include <random>

namespace dice::hfe::net {

GameClient::GameClient()
{
	m_connection.onAcknowledge = [this](uint32_t acknowledged, uint32_t mask) { m_events.acknowledge(acknowledged, mask); };
}

bool GameClient::connect(const std::string& host, uint16_t port, const GameClientOptions& options)
{
	m_options = options;
	m_world.clear();
	m_events.clear();
	m_nameSent = false;
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

GameClient::~GameClient()
{
	disconnect();
}

void GameClient::disconnect()
{
	if (m_state == State::Connected || m_state == State::Connecting) {
		m_connection.disconnect();
		m_state = State::Disconnected;
	}
}

void GameClient::playDemo(std::function<int(std::vector<uint8_t>& payload)> next)
{
	m_demo = std::move(next);
	m_state = m_demo ? State::Connected : State::Idle;
}

bool GameClient::update()
{
	if (m_demo) {
		// A demo (0x004b52e0 with no socket: 0x00517290 -> 0x00516c80): the
		// recorded payloads as they come due, nothing sent back.
		std::vector<uint8_t> payload;
		for (int got; (got = m_demo(payload)) != 0;) {
			if (got < 0) {
				m_state = State::Disconnected;
				m_demo = {};
				return false;
			}
			handlePayload(payload, kDemoGhostStateLengthBits); // a kind-2 connection's (0x004b4a80)
			payload.clear();
		}
		return true;
	}
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
	// Driven by the game's ticks (simulatedTick), the timer only keeps the
	// connection going while none come (loading, a menu).
	const auto due = sendOnTicks ? std::chrono::microseconds(200000) : tick;
	if (now - m_lastTick >= due) {
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

void GameClient::simulatedTick()
{
	// 0x004b3df0: a client sends after each simulated tick (connection
	// types other than 0), so each move is one of its ticks.
	if (m_state != State::Connected || !sendOnTicks || m_demo) {
		return;
	}
	m_lastTick = std::chrono::steady_clock::now();
	sendTick();
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
	handlePayload(payload);
}

void GameClient::handlePayload(const std::vector<uint8_t>& payload, unsigned lengthBits)
{
	GamePayload decoded;
	readGamePayload(payload, decoded, lengthBits);
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
	m_events.receive(block, [this](const GameEvent& event) { deliver(event); });
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
		m_events.queue(makeChallengeResponse(response, static_cast<uint32_t>(random()), kBuild, m_options.profileId));
		m_events.queue(makeConnectionType(m_options.connectionType));
		return;
	}
	m_world.apply(event);
	if (!m_nameSent && !m_options.playerName.empty() && m_world.localPlayer() >= 0) {
		m_nameSent = true;
		GameEvent name;
		name.type = kEvChangePlayerName;
		name.values = {static_cast<uint32_t>(m_world.localPlayer()) & 0xff};
		name.bytes = {m_options.playerName.substr(0, 31)};
		m_events.queue(name);
	}
	if (autoAnswer) {
		answer(event);
	}
}

void GameClient::answer(const GameEvent& event)
{
	// 0x004b5eb0: each level block is answered with LoadComplete at once.
	while (m_levelsAnswered < m_world.levelsReceived()) {
		++m_levelsAnswered;
		m_events.queue(makePostRemote(kMsgNetworkCategory, kNeLoadComplete));
		// A pure server (the settings' contentCheck, DAT_00a3fcc0 +0x54):
		// our fingerprint for the level's ordinal (0x0068ba40(0), sent as
		// 0x005123f0's event).
		if (!m_options.contentRoot.empty() && m_world.settings().contentCheck) {
			const RemoteWorld::LevelInfo& level = m_world.level();
			m_events.queue(makeContentCheck(computeContentDigests(m_options.contentRoot, m_options.modDirectory,
				level.levelDirectory, level.seed)));
		}
	}
	PostRemoteInfo info;
	if (event.type != kEvPostRemote || !parsePostRemote(event, info) || info.category != kMsgNetworkCategory) {
		return;
	}
	switch (info.id) {
	case kNeStartSimulation: // 0x004b6630 case 3: the connection type (0x004b48a0)
		m_events.queue(makeConnectionType(m_options.connectionType));
		break;
	case kNeDatabaseComplete: // case 4: echoed
	case kNeReset:            // case 5: echoed when the checksum matches (0x00678cf0)
		m_events.queue(makePostRemote(kMsgNetworkCategory, info.id));
		break;
	default:
		break;
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
		if (onMoveSent) {
			onMoveSent(m_moves.back());
		}
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

	// A lost block goes first, then new events (EventChannel).
	const std::optional<ReliableEventBlock> block = m_events.next();
	if (block) {
		payload.hasEvents = true;
		payload.events = *block;
	}
	if (m_connection.sendData(writeGamePayload(payload))) {
		++m_packetsSent;
		if (block) {
			m_events.sent(m_connection.lastSentSequence(), block->sequence);
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

void GameClient::remoteCommand(const std::string& command)
{
	// The line and its terminating NUL in one message (a payload holds 52 bytes).
	sendEvent(makePostRemote(kMsgNetworkCategory, kNeRemoteConsoleCommand, command.substr(0, 51) + std::string(1, '\0')));
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
