#include "GameConnection.h"

#include "BitStream.h"

#include <chrono>
#include <cstring>

namespace dice::hfe::net {

namespace {

BitWriter header(PacketType type, uint8_t connection)
{
	BitWriter out;
	out.write(static_cast<uint32_t>(type), 4);
	out.write(connection, 8);
	return out;
}

} // namespace

const char* refusalReason(uint32_t code)
{
	switch (code) {
	case 4: return "single-player server";
	case 7: return "no free connection";
	case 9: return "bad request";
	case 0x11: return "wrong password";
	case 0x16: return "banned";
	case 0x1e: return "server full";
	case 0x24: return "different mod";
	default: return "refused";
	}
}

const char* disconnectReason(uint32_t code)
{
	switch (code) {
	case 0: return "the connection was closed";
	case 0x16: return "banned";
	case 0x1b: return "no content check"; // 0x004af710's refusal of a client without one
	default: return refusalReason(code);
	}
}

std::vector<uint8_t> connectPacket(const ConnectRequest& request)
{
	BitWriter out = header(PacketType::Connect, 1); // 0x0068ddd1: mov dl, 1 (the server ignores it for type 1)
	out.write(kProtocol, 32);
	out.write(request.build, 32);
	out.write(0, 1); // the anti-cheat's bit: none here
	out.writeText(request.key, 4);
	out.writeText(request.password, 32);
	out.writeText(request.modDirectory, 32);
	return out.data();
}

std::vector<uint8_t> acknowledgePacket(uint8_t connection) { return header(PacketType::Acknowledge, connection).data(); }

std::vector<uint8_t> disconnectPacket(uint8_t connection, uint32_t reason)
{
	BitWriter out = header(PacketType::Disconnect, connection);
	out.write(reason, 32);
	return out.data();
}

ConnectResult readConnectReply(const std::vector<uint8_t>& datagram)
{
	ConnectResult result;
	BitReader in(datagram.data(), datagram.size());
	const uint32_t type = in.read(4);
	in.read(8); // the connection id the header carries
	if (type == static_cast<uint32_t>(PacketType::Accept)) {
		result.connection = static_cast<uint8_t>(in.read(8));
		result.serverClock = in.read(32);
		in.read(1); // the anti-cheat's bit
		result.outcome = in.bad() ? ConnectResult::Outcome::Error : ConnectResult::Outcome::Accepted;
	} else if (type == static_cast<uint32_t>(PacketType::Refuse)) {
		result.reason = in.read(32);
		if (in.read(1) == 1) {
			result.message = in.readText(32);
		}
		result.outcome = ConnectResult::Outcome::Refused;
	} else {
		result.outcome = ConnectResult::Outcome::Error;
	}
	return result;
}

std::vector<uint8_t> pingPacket(uint8_t connection, const NotifyState& state, uint32_t now)
{
	BitWriter out = header(static_cast<PacketType>(7), connection);
	out.write(static_cast<uint32_t>(state.sendSequence) & 0x3f, 6);
	out.write(state.lastReceived & 0x3f, 6);
	out.write(state.receivedMask, 32);
	out.write(state.sendSequence != -1 ? 1 : 0, 1); // 0x006a9bb0: sequenced once data went out
	out.write(now, 32);
	return out.data();
}

std::vector<uint8_t> dataPacket(uint8_t connection, uint32_t sequence, const NotifyState& state,
	const std::vector<uint8_t>& payload)
{
	BitWriter out = header(PacketType::Data, connection);
	out.write(sequence & 0x3f, 6);
	out.write(state.lastReceived & 0x3f, 6);
	out.write(state.receivedMask, 32);
	out.write(static_cast<uint32_t>(payload.size()), 16);
	out.writeBytes(payload.data(), payload.size());
	return out.data();
}

bool NotifyState::receive(uint32_t sequence, bool delivered)
{
	const uint32_t gap = (sequence - lastReceived) & 0x3f;
	if (gap == 0 || gap >= 32) {
		return false;
	}
	receivedMask <<= gap - 1; // the ones missed
	receivedMask = receivedMask << 1 | (delivered ? 1u : 0u);
	lastReceived = sequence & 0x3f;
	return true;
}

bool readConnectedPacket(const std::vector<uint8_t>& datagram, ConnectedPacket& packet)
{
	BitReader in(datagram.data(), datagram.size());
	packet.type = static_cast<PacketType>(in.read(4));
	in.read(8);
	const uint32_t type = static_cast<uint32_t>(packet.type);
	if (type != 7 && type != 8 && type != 15) {
		return !in.bad();
	}
	packet.sequence = in.read(6);
	packet.acknowledged = in.read(6);
	packet.mask = in.read(32);
	if (type == 7 || type == 8) {
		packet.sequenced = in.read(1) == 1;
		packet.time = in.read(32);
		if (type == 8) {
			packet.echo = in.read(32);
		}
	} else {
		packet.payloadBitOffset = 56;
		packet.payload.assign(datagram.begin() + 7, datagram.end());
	}
	return !in.bad();
}

std::vector<uint8_t> pongPacket(uint8_t connection, const NotifyState& state, uint32_t pingTime, uint32_t now)
{
	BitWriter out = header(static_cast<PacketType>(8), connection);
	out.write(static_cast<uint32_t>(state.sendSequence) & 0x3f, 6);
	out.write(state.lastReceived & 0x3f, 6);
	out.write(state.receivedMask, 32);
	out.write(state.sendSequence != -1 ? 1 : 0, 1);
	out.write(pingTime, 32);
	out.write(now, 32);
	return out.data();
}

uint32_t GameConnection::now() const
{
	return static_cast<uint32_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count());
}

bool GameConnection::send(const std::vector<uint8_t>& datagram)
{
	if (onDatagram) {
		onDatagram(true, datagram);
	}
	if (m_replay) {
		return true; // a replayed session sends nothing
	}
	return m_socket.send(datagram);
}

bool GameConnection::receive(std::vector<uint8_t>& datagram, int milliseconds)
{
	if (m_replay) {
		return false;
	}
	const bool got = m_socket.receive(datagram, milliseconds) > 0;
	if (got && onDatagram) {
		onDatagram(false, datagram);
	}
	return got;
}

void GameConnection::beginKeepAlive()
{
	if (m_replay || !m_connected || m_keepAliveRunning) {
		return;
	}
	m_keepAliveRunning = true;
	m_keepAlive = std::thread([this] {
		auto lastPing = std::chrono::steady_clock::now();
		while (m_keepAliveRunning && m_connected) {
			std::vector<uint8_t> datagram;
			if (receive(datagram, 50)) {
				ConnectedPacket packet;
				if (readConnectedPacket(datagram, packet) && static_cast<uint32_t>(packet.type) == 7) {
					handleDatagram(datagram, {}); // a pong straight back
				} else {
					m_backlog.push_back(std::move(datagram));
				}
			}
			if (std::chrono::steady_clock::now() - lastPing >= std::chrono::seconds(3)) {
				lastPing = std::chrono::steady_clock::now();
				send(pingPacket(m_connection, m_notify, now()));
			}
		}
	});
}

void GameConnection::endKeepAlive()
{
	m_keepAliveRunning = false;
	if (m_keepAlive.joinable()) {
		m_keepAlive.join();
	}
}

void GameConnection::beginReplay()
{
	m_replay = true;
	m_notify = {};
	m_fastPongs = 0;
	m_result = {};
	m_connected = false;
	m_connecting = true;
	m_attempts = 1;
	m_lastAttempt = std::chrono::steady_clock::now();
}

bool GameConnection::inject(const std::vector<uint8_t>& datagram, const std::function<void(const ConnectedPacket&)>& onData)
{
	if (onDatagram) {
		onDatagram(false, datagram);
	}
	if (m_connecting) {
		takeConnectReply(datagram);
		return m_connecting || m_connected;
	}
	if (m_connected) {
		handleDatagram(datagram, onData);
	}
	return m_connected;
}

void GameConnection::takeConnectReply(const std::vector<uint8_t>& datagram)
{
	const ConnectResult reply = readConnectReply(datagram);
	if (reply.outcome == ConnectResult::Outcome::Accepted) {
		m_result = reply;
		m_connection = reply.connection;
		m_connecting = false;
		m_connected = send(acknowledgePacket(m_connection));
	} else if (reply.outcome == ConnectResult::Outcome::Refused) {
		m_result = reply;
		m_connecting = false;
	}
}

void GameConnection::handleDatagram(const std::vector<uint8_t>& datagram,
	const std::function<void(const ConnectedPacket&)>& onData)
{
	ConnectedPacket packet;
	if (!readConnectedPacket(datagram, packet)) {
		return;
	}
	const uint32_t type = static_cast<uint32_t>(packet.type);
	// A stale or repeated data packet is dropped before its acknowledgement
	// is looked at (0x0068e150).
	const bool fresh = type != 15 || m_notify.receive(packet.sequence, true);
	if ((type == 7 || type == 8 || type == 15) && fresh) {
		// Our sequences the peer has seen: only a step under 32 counts
		// (0x006aa750 ignores the rest).
		if (((packet.acknowledged - m_notify.lastAcknowledged) & 0x3f) < 32) {
			m_notify.lastAcknowledged = packet.acknowledged;
		}
		if (onAcknowledge) {
			onAcknowledge(packet.acknowledged, packet.mask);
		}
	}
	switch (type) {
	case 2: // 0x0068f280 case 2: an accept again (our type 4 was lost): acknowledge again
		send(acknowledgePacket(m_connection));
		break;
	case 5: { // disconnected
		BitReader in(datagram.data(), datagram.size());
		in.read(12);
		m_disconnectReason = in.read(32);
		m_connected = false;
		break;
	}
	case 7: // ping: answer with a pong (0x0068e050)
		if (packet.sequenced) {
			m_notify.receive(packet.sequence, false);
		}
		send(pongPacket(m_connection, m_notify, packet.time, now()));
		break;
	case 8: // a pong: `time` echoes our ping's clock
		if (packet.sequenced) {
			m_notify.receive(packet.sequence, false);
		}
		if (packet.time <= now()) {
			m_roundTrip = now() - packet.time;
			if (m_roundTrip < 1000) {
				++m_fastPongs; // 0x0068ef80: 32 of these end the quick pings
			}
		}
		break;
	case 15:
		if (fresh && onData) {
			onData(packet);
		}
		break;
	default:
		break; // 0x0068f280's default: other types are ignored
	}
}

bool GameConnection::pump(int milliseconds, const std::function<void(const ConnectedPacket&)>& onData)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	while (m_connected && std::chrono::steady_clock::now() < deadline) {
		std::vector<uint8_t> datagram;
		if (!receive(datagram, 50)) {
			continue;
		}
		handleDatagram(datagram, onData);
	}
	return m_connected;
}

bool GameConnection::start(const std::string& host, uint16_t port, const ConnectRequest& request)
{
	m_result = {};
	m_connected = false;
	m_connecting = false;
	m_replay = false;
	m_notify = {};
	m_fastPongs = 0;
	m_roundTrip = 0;
	if (!m_socket.open(host, port)) {
		m_result.outcome = ConnectResult::Outcome::Error;
		m_result.message = "cannot reach " + host;
		return false;
	}
	m_connectPacket = connectPacket(request);
	m_attempts = 1;
	m_lastAttempt = std::chrono::steady_clock::now();
	m_connecting = send(m_connectPacket);
	return m_connecting;
}

bool GameConnection::poll(const std::function<void(const ConnectedPacket&)>& onData)
{
	std::vector<uint8_t> datagram;
	while (m_connecting) {
		datagram.clear();
		if (!receive(datagram, 0)) {
			if (m_replay) {
				return true; // replies come from inject()
			}
			if (std::chrono::steady_clock::now() - m_lastAttempt >= std::chrono::seconds(1)) {
				if (m_attempts >= 3) {
					m_connecting = false;
					m_result.outcome = ConnectResult::Outcome::NoAnswer;
					return false;
				}
				++m_attempts;
				m_lastAttempt = std::chrono::steady_clock::now();
				send(m_connectPacket);
			}
			return true;
		}
		takeConnectReply(datagram);
		if (!m_connecting && !m_connected) {
			return false;
		}
	}
	while (m_connected && !m_backlog.empty()) {
		const std::vector<uint8_t> waiting = std::move(m_backlog.front());
		m_backlog.pop_front();
		handleDatagram(waiting, onData);
	}
	while (m_connected) {
		datagram.clear();
		if (!receive(datagram, 0)) {
			break;
		}
		handleDatagram(datagram, onData);
	}
	return m_connected;
}

void GameConnection::ping()
{
	if (m_connected) {
		send(pingPacket(m_connection, m_notify, now()));
	}
}

ConnectResult GameConnection::connect(const std::string& host, uint16_t port, const ConnectRequest& request,
	int timeoutMilliseconds)
{
	ConnectResult result;
	if (!m_socket.open(host, port)) {
		result.outcome = ConnectResult::Outcome::Error;
		result.message = "cannot reach " + host;
		return result;
	}
	const std::vector<uint8_t> packet = connectPacket(request);
	for (int attempt = 0; attempt < 3; ++attempt) {
		send(packet);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds / 3);
		while (std::chrono::steady_clock::now() < deadline) {
			std::vector<uint8_t> reply;
			if (!receive(reply, 100)) {
				continue;
			}
			result = readConnectReply(reply);
			if (result.outcome == ConnectResult::Outcome::Accepted) {
				m_connection = result.connection;
				m_connected = send(acknowledgePacket(m_connection));
				return result;
			}
			if (result.outcome == ConnectResult::Outcome::Refused) {
				return result;
			}
		}
	}
	return result;
}

bool GameConnection::canSendData() const
{
	// 0x006aa140 holds a packet more than 30 ahead of the last one the
	// peer acknowledged: past 31 the peer drops it as too far ahead.
	const uint32_t next = m_notify.sendSequence < 0 ? 1 : (static_cast<uint32_t>(m_notify.sendSequence) + 1) & 0x3f;
	return ((next - m_notify.lastAcknowledged) & 0x3f) <= 30;
}

bool GameConnection::sendData(const std::vector<uint8_t>& payload)
{
	if (!canSendData()) {
		return false;
	}
	// The first data packet is sequence 1 (the peer's last received starts
	// at 0, and a packet must be 1-31 ahead of it).
	m_notify.sendSequence = m_notify.sendSequence < 0 ? 1 : (m_notify.sendSequence + 1) & 0x3f;
	return send(dataPacket(m_connection, static_cast<uint32_t>(m_notify.sendSequence), m_notify, payload));
}

void GameConnection::disconnect(uint32_t reason)
{
	endKeepAlive();
	if (m_connected) {
		// 0x0068e520 sends it three times.
		for (int i = 0; i < 3; ++i) {
			send(disconnectPacket(m_connection, reason));
		}
		m_connected = false;
	}
	m_socket.close();
}

} // namespace dice::hfe::net
