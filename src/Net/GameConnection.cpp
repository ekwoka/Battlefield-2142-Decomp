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
	case 0x1f: return "PunkBuster required";
	case 0x24: return "different mod";
	default: return "refused";
	}
}

std::vector<uint8_t> connectPacket(const ConnectRequest& request)
{
	BitWriter out = header(PacketType::Connect, 0);
	out.write(kProtocol, 32);
	out.write(request.build, 32);
	out.write(request.punkBuster ? 1 : 0, 1);
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
	if (datagram.size() >= 7 && std::memcmp(datagram.data(), "\xff\xff\xff\xffPB_", 7) == 0) {
		return result; // PunkBuster's own traffic
	}
	BitReader in(datagram.data(), datagram.size());
	const uint32_t type = in.read(4);
	in.read(8); // the connection id the header carries
	if (type == static_cast<uint32_t>(PacketType::Accept)) {
		result.connection = static_cast<uint8_t>(in.read(8));
		result.serverClock = in.read(32);
		result.punkBuster = in.read(1) != 0;
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
	out.write(0, 1); // unsequenced
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

void GameConnection::handleDatagram(const std::vector<uint8_t>& datagram,
	const std::function<void(const ConnectedPacket&)>& onData)
{
	if (datagram.size() >= 7 && std::memcmp(datagram.data(), "\xff\xff\xff\xffPB_", 7) == 0) {
		return;
	}
	ConnectedPacket packet;
	if (!readConnectedPacket(datagram, packet)) {
		return;
	}
	const uint32_t type = static_cast<uint32_t>(packet.type);
	if ((type == 7 || type == 8 || type == 15) && onAcknowledge) {
		onAcknowledge(packet.acknowledged, packet.mask);
	}
	switch (type) {
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
		m_notify.lastAcknowledged = packet.acknowledged;
		m_socket.send(pongPacket(m_connection, m_notify, packet.time, now()));
		break;
	case 8: // a pong: `time` echoes our ping's clock
		if (packet.sequenced) {
			m_notify.receive(packet.sequence, false);
		}
		if (packet.time <= now()) {
			m_roundTrip = now() - packet.time;
		}
		break;
	case 15:
		if (m_notify.receive(packet.sequence, true) && onData) {
			onData(packet);
		}
		break;
	default:
		break;
	}
}

bool GameConnection::pump(int milliseconds, const std::function<void(const ConnectedPacket&)>& onData)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	while (m_connected && std::chrono::steady_clock::now() < deadline) {
		std::vector<uint8_t> datagram;
		if (m_socket.receive(datagram, 50) <= 0) {
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
	if (!m_socket.open(host, port)) {
		m_result.outcome = ConnectResult::Outcome::Error;
		m_result.message = "cannot reach " + host;
		return false;
	}
	m_connectPacket = connectPacket(request);
	m_attempts = 1;
	m_lastAttempt = std::chrono::steady_clock::now();
	m_connecting = m_socket.send(m_connectPacket);
	return m_connecting;
}

bool GameConnection::poll(const std::function<void(const ConnectedPacket&)>& onData)
{
	std::vector<uint8_t> datagram;
	while (m_connecting) {
		datagram.clear();
		if (m_socket.receive(datagram, 0) <= 0) {
			if (std::chrono::steady_clock::now() - m_lastAttempt >= std::chrono::seconds(1)) {
				if (m_attempts >= 3) {
					m_connecting = false;
					m_result.outcome = ConnectResult::Outcome::NoAnswer;
					return false;
				}
				++m_attempts;
				m_lastAttempt = std::chrono::steady_clock::now();
				m_socket.send(m_connectPacket);
			}
			return true;
		}
		const ConnectResult reply = readConnectReply(datagram);
		if (reply.outcome == ConnectResult::Outcome::Accepted) {
			m_result = reply;
			m_connection = reply.connection;
			m_connecting = false;
			m_connected = m_socket.send(acknowledgePacket(m_connection));
		} else if (reply.outcome == ConnectResult::Outcome::Refused) {
			m_result = reply;
			m_connecting = false;
			return false;
		}
	}
	while (m_connected) {
		datagram.clear();
		if (m_socket.receive(datagram, 0) <= 0) {
			break;
		}
		handleDatagram(datagram, onData);
	}
	return m_connected;
}

void GameConnection::ping()
{
	if (m_connected) {
		m_socket.send(pingPacket(m_connection, m_notify, now()));
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
		m_socket.send(packet);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds / 3);
		while (std::chrono::steady_clock::now() < deadline) {
			std::vector<uint8_t> reply;
			if (m_socket.receive(reply, 100) <= 0) {
				continue;
			}
			result = readConnectReply(reply);
			if (result.outcome == ConnectResult::Outcome::Accepted) {
				m_connection = result.connection;
				m_connected = m_socket.send(acknowledgePacket(m_connection));
				return result;
			}
			if (result.outcome == ConnectResult::Outcome::Refused) {
				return result;
			}
		}
	}
	return result;
}

bool GameConnection::sendData(const std::vector<uint8_t>& payload)
{
	// The first data packet is sequence 1 (the peer's last received starts
	// at 0, and a packet must be 1-31 ahead of it).
	m_notify.sendSequence = m_notify.sendSequence < 0 ? 1 : (m_notify.sendSequence + 1) & 0x3f;
	return m_socket.send(dataPacket(m_connection, static_cast<uint32_t>(m_notify.sendSequence), m_notify, payload));
}

void GameConnection::disconnect(uint32_t reason)
{
	if (m_connected) {
		m_socket.send(disconnectPacket(m_connection, reason));
		m_connected = false;
	}
	m_socket.close();
}

} // namespace dice::hfe::net
