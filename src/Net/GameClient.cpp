#include "GameClient.h"

#include "ContentCheck.h"

#include "GameEvents.h"
#include "PlayerAuth.h"

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
	m_playerInfoSent = false;
	m_moves.clear();
	m_moveIndex = 0;
	m_levelsAnswered = 0;
	m_levelsLoaded = 0;
	m_replay = false;
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

void GameClient::playCapture(std::vector<CaptureRecord> records, bool paced, const GameClientOptions& options)
{
	m_options = options;
	m_world.clear();
	m_events.clear();
	m_nameSent = false;
	m_playerInfoSent = false;
	m_moves.clear();
	m_moveIndex = 0;
	m_levelsAnswered = 0;
	m_levelsLoaded = 0;
	m_replayRecords = std::move(records);
	m_replayNext = 0;
	m_replayPaced = paced;
	m_replay = true;
	m_replayStart = std::chrono::steady_clock::now();
	m_connection.beginReplay();
	m_state = State::Connecting;
}

void GameClient::setDiagnostics(NetDiagnostics* diagnostics)
{
	m_diagnostics = diagnostics;
	if (!diagnostics) {
		m_connection.onDatagram = nullptr;
		m_world.onProblem = nullptr;
		return;
	}
	m_connection.onDatagram = [diagnostics](bool outgoing, const std::vector<uint8_t>& datagram) {
		diagnostics->datagram(outgoing, datagram);
	};
	m_world.onProblem = [diagnostics](const std::string& category, const std::string& text) {
		diagnostics->problem(category, text);
	};
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
	m_connection.endKeepAlive(); // the game is back: the connection is ours again
	const auto onData = [this](const ConnectedPacket& packet) { onPacket(packet); };
	if (m_replay) {
		// The captured datagrams the server sent, as they come due.
		const uint32_t first = m_replayRecords.empty() ? 0 : m_replayRecords.front().milliseconds;
		const auto elapsed = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - m_replayStart).count());
		bool handed = false;
		while (m_replayNext < m_replayRecords.size()) {
			const CaptureRecord& r = m_replayRecords[m_replayNext];
			if (!r.outgoing && (m_replayPaced ? r.milliseconds - first > elapsed : handed)) {
				break;
			}
			++m_replayNext;
			if (!r.outgoing) {
				handed = true;
				m_connection.inject(r.datagram, onData);
			}
		}
		if (m_replayNext >= m_replayRecords.size()) {
			m_replay = false;
			m_state = State::Disconnected;
			return false;
		}
	}
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
		// 0x004b4d10 runs 0x004b48a0 as soon as the connection is made: the
		// connection type goes first, before the challenge comes.
		m_events.queue(makeConnectionType(m_options.connectionType));
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
	// 0x004b3df0: a packet when the 1 / rate timer runs out, and after each
	// simulated tick. Driven by the game's ticks (simulatedTick), the
	// timer's packets carry no new move (the last three again), so a move is
	// exactly one tick of the client's, as the server steps it; and they go
	// only every 200 ms without ticks (loading, a menu): the exe's extra
	// packets between ticks carry nothing new, and our host steps moves
	// more evenly without them.
	if (now - m_lastTick >= (sendOnTicks ? std::chrono::microseconds(200000) : tick)) {
		m_lastTick = now;
		sendTick(!sendOnTicks);
	}
	// 0x0068f280: once connected a ping (type 7, 0x0068dc40 -> 0x006a9bb0)
	// every 200 ms (+0xc) until 32 pongs have come back within a second
	// (0x0068ef80 sets +0x11), then every 3000 ms; the pongs give the round
	// trip.
	const auto pingEvery = std::chrono::milliseconds(m_connection.fastPongs() < 32 ? 200 : 3000);
	if (now - m_lastPing >= pingEvery) {
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
	sendTick(true);
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
		// 0x004b2790: GameSpy's CD-key answer, the build and the profile
		// (the connection type went when the connection was made).
		static std::mt19937 random {std::random_device {}()};
		const std::string challenge = event.text(0).substr(0, event.text(0).find('\0'));
		const std::string response = cdKeyResponse(m_options.cdKey, challenge, static_cast<uint32_t>(random()));
		m_events.queue(makeChallengeResponse(response, static_cast<uint32_t>(random()), kBuild, m_options.profileId));
		return;
	}
	m_world.apply(event);
	sendPlayerInfo();
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

void GameClient::sendPlayerInfo()
{
	// 0x004b5eb0 case 0: the first settings block (written with argument
	// 1, settings +0x30) is answered with the player info, data block 1
	// (0x00585290 / 0x00585270 give its bytes to the data channel).
	if (m_playerInfoSent || m_demo || !m_world.dataBlocks().count(0)) {
		return;
	}
	const RemoteWorld::ServerSettings settings = m_world.settings();
	if (!settings.initial) {
		return;
	}
	m_playerInfoSent = true;
	PlayerInfoBlock info;
	info.name = m_options.playerName.empty() ? "Player" : m_options.playerName; // GSPlayerNameNoClanTag, "player" by default
	info.nameChecksum = m_options.nameChecksum;
	info.profileId = static_cast<int32_t>(m_options.profileId == 0xffffffff ? 0 : m_options.profileId);
	info.clanTag = m_options.clanTag;
	info.auth = urlSafeTicket(m_options.auth);
	info.voice = settings.voipEnabled; // settings +0x31
	const std::string data = encodePlayerInfo(info);
	m_events.queue(makeDataBlockStart(kPlayerInfoBlock, static_cast<uint32_t>(data.size())));
	for (size_t at = 0; at < data.size(); at += 63) {
		m_events.queue(makeDataBlockChunk(data.substr(at, 63)));
	}
}

void GameClient::levelLoaded()
{
	m_connection.endKeepAlive();
	m_levelsLoaded = m_world.levelsReceived();
	if (autoAnswer) {
		answerLevels();
	}
}

void GameClient::answer(const GameEvent& event)
{
	answerLevels();
	answerMessage(event);
}

void GameClient::answerLevels()
{
	// 0x004b5eb0 case 2: the level loads (vtable 0x1c, 0x004b3990), and only
	// once it has does LoadComplete go, then the content check; a load that
	// fails sends nothing. With answerLoadAfterLevel the game says when
	// (levelLoaded); else each level block is answered at once.
	const uint32_t ready = answerLoadAfterLevel ? std::min(m_levelsLoaded, m_world.levelsReceived()) : m_world.levelsReceived();
	while (m_levelsAnswered < ready) {
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
}

void GameClient::answerMessage(const GameEvent& event)
{
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

void GameClient::sendTick(bool newMove)
{
	// The input: this frame's move and the two before (0x00590450). Only
	// once the server made our player: 0x00590740 writes a lone 0 bit while
	// the input manager has no player (+0x84), and the server's reader
	// hands moves to its player for the connection. Each simulated tick
	// makes its move whether a packet goes or not (0x00590c00 steps the
	// player, then 0x004b2750 adds the move to his list and to these
	// three), so a held packet's move goes in the next one.
	if (m_world.localPlayer() >= 0 && (newMove || m_moves.empty())) {
		m_moves.push_back(m_world.input().toMove(m_moveIndex++));
		if (onMoveSent) {
			onMoveSent(m_moves.back());
		}
		while (m_moves.size() > 3) {
			m_moves.pop_front();
		}
	}
	// 0x006aa140 holds a packet more than 30 ahead of the server's last
	// acknowledgement; nothing is taken from the queues meanwhile.
	if (!m_connection.canSendData()) {
		return;
	}
	GamePayload payload;
	if (m_world.localPlayer() >= 0 && !m_moves.empty()) {
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
		} else {
			m_events.sentWithout(m_connection.lastSentSequence());
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
	// The whole line as data block 6 (PlayerAuth.h kRconCommandBlock); an
	// empty block would wedge the server's transfers, so none is sent.
	if (command.empty()) {
		return;
	}
	m_events.queue(makeDataBlockStart(kRconCommandBlock, static_cast<uint32_t>(command.size())));
	for (size_t at = 0; at < command.size(); at += 63) {
		m_events.queue(makeDataBlockChunk(command.substr(at, 63)));
	}
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

void GameClient::whisper(const std::string& text, int target)
{
	ChatMessage message;
	message.channel = kChatPlayer;
	message.source = 2;
	message.player = m_world.localPlayer();
	message.b9 = static_cast<uint8_t>(target < 0 ? 0xff : target);
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

void GameClient::answerJoinRequest(int player, int squad, bool yes)
{
	// 0x0079d6e0 kind 0 on a client: 2/0x13 (yes) or 2/0x12 with {squad,
	// the player} to the server (connection -1), which hands it on.
	sendEvent(makePostRemote(2, yes ? 0x13 : 0x12, int32Payload(squad) + int32Payload(player)));
	if (yes) {
		invite(player, squad); // slot 0x30
	}
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

void GameClient::requestAsset(int kind, const Vec3f& point)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	SquadOrderInfo order;
	order.squad = me ? me->squad : 0;
	order.team = me ? me->team : 0;
	order.kind = kind;
	order.floats[0] = point.x;
	order.floats[1] = point.y;
	order.floats[2] = point.z;
	order.store = kSquadOrderStoreRequest;
	sendEvent(makeSquadOrder(order));
}

void GameClient::answerRequest(int squad, int kind)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	AcceptOrderInfo answer;
	answer.squad = squad;
	answer.team = me ? me->team : 0;
	answer.store = kSquadOrderStoreRequest;
	answer.kind = kind;
	sendEvent(makeAcceptOrder(answer));
}

void GameClient::issueOrder(int store, int squad, int kind, const Vec3f& point)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	SquadOrderInfo order;
	order.squad = squad;
	order.team = me ? me->team : 0;
	order.kind = kind;
	order.floats[0] = point.x;
	order.floats[1] = point.y;
	order.floats[2] = point.z;
	order.store = store;
	sendEvent(makeSquadOrder(order));
}

void GameClient::answerOrder(int squad, int kind, bool accept)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	AcceptOrderInfo answer;
	answer.squad = squad;
	answer.team = me ? me->team : 0;
	answer.flag = accept;
	answer.store = 1;
	answer.kind = kind;
	sendEvent(makeAcceptOrder(answer));
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

void GameClient::radioMessage(int channel, int index, int value)
{
	// 0x004b2b60: the index, the player (slot 0x58) and its team (0xf8),
	// the value, the channel and 3.
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	RadioMessageInfo info;
	info.index = index;
	info.sender = m_world.localPlayer();
	info.team = me ? me->team : 0;
	info.value = value;
	info.channel = channel;
	info.source = 3;
	sendEvent(makeRadioMessage(info));
}

void GameClient::commanderTool(CommanderToolUse::Tool tool, float x, float z)
{
	const RemotePlayer* me = m_world.player(m_world.localPlayer());
	const int team = me ? me->team : 0;
	switch (tool) {
	case CommanderToolUse::Artillery:
		sendEvent(makeArtilleryUse(team, x, z, 1)); // 0x007d88d0: RCArtillery
		break;
	case CommanderToolUse::OrbitalStrike:
		sendEvent(makeArtilleryUse(team, x, z, 4)); // 0x007d7d80: RCOrbitalStrike
		break;
	case CommanderToolUse::Uav:
		sendEvent(makeUavUse(team, x, z, false)); // 0x007d8370
		break;
	case CommanderToolUse::Satellite:
		sendEvent(makeUavUse(team, x, z, true)); // 0x007d8660
		break;
	case CommanderToolUse::Supplies:
		sendEvent(makeSupplyDrop(team, x, z)); // 0x007d81d0 → 0x0043d6d0
		break;
	case CommanderToolUse::MoveTitan:
		sendEvent(makeTitanMove(team, x, z));
		break;
	}
}

void GameClient::toggleFreeCamera()
{
	GameEvent event;
	event.type = kEvToggleFreeCamera;
	sendEvent(event);
}

void GameClient::joinAsSpectator(bool spectator)
{
	JoinAsSpectatorInfo info;
	info.player = m_world.localPlayer();
	info.spectator = spectator;
	sendEvent(makeJoinAsSpectator(info)); // SwiffHost slot 0x3c0 (0x0046c780): to the server
}

void GameClient::spectatorCamera(int kind, int follow)
{
	JoinAsSpectatorInfo info;
	info.player = m_world.localPlayer();
	info.kind = kind;
	info.follow = follow;
	sendEvent(makeJoinAsSpectator(info));
}

} // namespace dice::hfe::net
