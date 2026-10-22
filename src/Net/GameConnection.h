// The game's own connection between client and server (BF2142.exe's
// network session, 0x00688000-0x0068f000: the server's receive loop
// 0x0068ab90, the client's 0x0068ec..). UDP datagrams of at most 1472
// bytes (0x5c0), each a bit stream starting with a 4-bit packet type and
// an 8-bit connection id (0x006aa930 / 0x006aa9e0). The server listens on
// port 17567 (0x449f) by default.
//
// Connecting (types):
//   1 client -> server: 32 bits 0x1002 (the protocol), 32 bits the build
//     (0x1a007000 for 1.51, 0x00423320), 1 bit PunkBuster on, a 4-byte key,
//     the password (32 bytes) and the mod directory (32 bytes,
//     "mods/bf2142") (0x0068dd00; checked by 0x00689d30)
//   2 server -> client: the connection id (8 bits), the server's clock
//     (32 bits, ms) and whether it runs PunkBuster (1 bit) (0x00689400,
//     read by 0x0068e580)
//   3 server -> client: refused: a reason (32 bits) and, after a 1 bit,
//     a message (32 bytes) (0x0068de90)
//   4 client -> server: accepting the connection (0x0068df20)
//   5 either way: disconnecting, with a reason (32 bits) (0x0068dfa0)
//   15: game data (0x0068a2b0), 7 and 8: connection upkeep.
// Datagrams starting "\xff\xff\xff\xffPB_" are PunkBuster's (0x006aa900).
#pragma once

#include "Socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dice::hfe::net {

constexpr uint16_t kGamePort = 17567;
constexpr uint32_t kProtocol = 0x1002;
constexpr uint32_t kBuild = 0x1a007000; // "1.10.112.0" (BF2142 1.51)

enum class PacketType : uint8_t {
	Connect = 1,
	Accept = 2,
	Refuse = 3,
	Acknowledge = 4,
	Disconnect = 5,
	Data = 15,
};

// Why a server refuses (the codes 0x00689d30 returns).
const char* refusalReason(uint32_t code);

struct ConnectRequest {
	bool punkBuster = false;
	std::string key;      // 4 bytes
	std::string password;
	std::string modDirectory = "mods/bf2142";
	uint32_t build = kBuild;
};

std::vector<uint8_t> connectPacket(const ConnectRequest& request);
std::vector<uint8_t> acknowledgePacket(uint8_t connection);
std::vector<uint8_t> disconnectPacket(uint8_t connection, uint32_t reason);

struct ConnectResult {
	enum class Outcome { Accepted, Refused, NoAnswer, Error } outcome = Outcome::NoAnswer;
	uint8_t connection = 0;
	uint32_t serverClock = 0;
	bool punkBuster = false;
	uint32_t reason = 0;
	std::string message;
};

// Reads a reply to a connect request (types 2 and 3).
ConnectResult readConnectReply(const std::vector<uint8_t>& datagram);

// The "notify" layer of connected packets (types 7, 8 and 15): each starts
// with the sender's sequence number (6 bits), the last sequence it has
// received (6 bits) and a mask of the 32 before that which arrived
// (0x006aa980 / 0x006aaa40). A receiver takes a sequence only if it is
// 1-31 ahead of the last one (0x0068e150), shifting the mask by the gap
// and setting bit 0 for data; a sequenced ping shifts without setting it
// (0x0068d490).
struct NotifyState {
	int sendSequence = -1;   // conn + 0x54: the last data sequence we sent
	uint32_t lastReceived = 0; // conn + 0x4c
	uint32_t receivedMask = 0; // conn + 0xe0
	uint32_t lastAcknowledged = 0; // conn + 0x50: their acks of ours

	// A received sequence: false when it is stale or too far ahead.
	bool receive(uint32_t sequence, bool delivered);
};

// A received connected packet, after its header.
struct ConnectedPacket {
	PacketType type;
	uint32_t sequence = 0, acknowledged = 0, mask = 0;
	bool sequenced = false;   // pings and pongs: their sequence counts
	uint32_t time = 0, echo = 0;
	std::vector<uint8_t> payload; // data: the bits after the header, from the first byte boundary
	size_t payloadBitOffset = 0;  // data: where the payload starts in the datagram, in bits
};
bool readConnectedPacket(const std::vector<uint8_t>& datagram, ConnectedPacket& packet);
// Type 8, the answer to a ping (0x006a9c30): our notify header, whether it
// is sequenced, the ping's time and our clock.
std::vector<uint8_t> pongPacket(uint8_t connection, const NotifyState& state, uint32_t pingTime, uint32_t now);
// Type 7, a ping (0x0068e050 answers it): the notify header, unsequenced, our clock.
std::vector<uint8_t> pingPacket(uint8_t connection, const NotifyState& state, uint32_t now);
// Type 15 with the notify header and a payload (what sendData sends).
std::vector<uint8_t> dataPacket(uint8_t connection, uint32_t sequence, const NotifyState& state,
	const std::vector<uint8_t>& payload);

class GameConnection {
public:
	// Sends connect requests (3 tries) and waits for the answer; on
	// acceptance acknowledges it.
	ConnectResult connect(const std::string& host, uint16_t port, const ConnectRequest& request, int timeoutMilliseconds);
	// The same without waiting: start() sends the first request and poll()
	// takes the answer (resending once a second, 3 tries).
	bool start(const std::string& host, uint16_t port, const ConnectRequest& request);
	bool connecting() const { return m_connecting; }
	const ConnectResult& result() const { return m_result; }
	void disconnect(uint32_t reason = 0);
	bool connected() const { return m_connected; }
	UdpSocket& socket() { return m_socket; }
	// Receives for up to `milliseconds`, answering pings and tracking
	// sequences; data packets go to `onData`. False once disconnected.
	bool pump(int milliseconds, const std::function<void(const ConnectedPacket&)>& onData);
	// Handles what has arrived without waiting (and, while connecting, the
	// connect answer and resends). False once disconnected or refused.
	bool poll(const std::function<void(const ConnectedPacket&)>& onData);
	const NotifyState& notify() const { return m_notify; }
	// Sends a data packet (type 15): our next sequence, the notify header,
	// the payload's 16-bit length and the payload (0x0068ef80).
	bool sendData(const std::vector<uint8_t>& payload);
	// The sequence the last sendData used (6 bits).
	uint32_t lastSentSequence() const { return static_cast<uint32_t>(m_notify.sendSequence) & 0x3f; }
	// Called for each connected packet with the peer's acknowledgement of
	// our sequences: the last one it got and the mask of the 32 before
	// (bit 0 that last one).
	std::function<void(uint32_t acknowledged, uint32_t mask)> onAcknowledge;
	uint32_t disconnectReason() const { return m_disconnectReason; }
	// Our clock (ms since the connection object was made).
	uint32_t now() const;
	// Round trip to the server from its pongs to our pings (ms), 0 before one.
	uint32_t roundTrip() const { return m_roundTrip; }
	// Sends a ping (type 7) carrying our clock; the server's pong gives the round trip.
	void ping();

private:
	void handleDatagram(const std::vector<uint8_t>& datagram, const std::function<void(const ConnectedPacket&)>& onData);

	UdpSocket m_socket;
	bool m_connected = false;
	bool m_connecting = false;
	int m_attempts = 0;
	std::vector<uint8_t> m_connectPacket;
	std::chrono::steady_clock::time_point m_lastAttempt;
	ConnectResult m_result;
	uint8_t m_connection = 0;
	NotifyState m_notify;
	uint32_t m_disconnectReason = 0;
	uint32_t m_roundTrip = 0;
	std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

} // namespace dice::hfe::net
