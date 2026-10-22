// The game server's side of voice chat (BF2142.exe's VoipServerConnection,
// 0x0057bf10-0x0057ce10): a UDP socket from sv.voipBFServerPort to the
// voice server (the built-in one or sv.voipServerRemoteIP) that keeps the
// voice server's client table in step with the players.
//
// - It answers the voice server's challenge ('h' packets, 0x0057c4f0) with
//   md5(challenge + password)'s first 20 hex digits as its session string.
// - A player's change sends SET_PLAYER_DATA, or REMOVE_PLAYER_DATA for one
//   neither commander nor in a squad (0x0057c8a0).
// - Every 5 s the whole table goes as FULL_ROUTING_DATA_SET (0x0057cbe0),
//   which also gets the game server challenged and authenticated after a
//   start or a timeout.
//
// With the built-in voice server (sv.voipServerRemote 0) the password is
// made up for the session: 'v' and 8-17 random letters (0x0057c600).
#pragma once

#include "Net/Socket.h"
#include "VoipProtocol.h"

#include <functional>
#include <random>
#include <string>
#include <vector>

namespace dice::hfe::voip {

class VoipServerConnection {
public:
	struct Options {
		std::string host = "127.0.0.1";       // the voice server
		uint16_t port = kDefaultServerPort;   // sv.voipServerPort
		uint16_t localPort = 0;               // sv.voipBFServerPort (0: any)
		std::string password;                 // sv.voipSharedPassword
		bool loopbackOnly = false;
	};

	bool start(const Options& options);
	void stop();
	bool running() const { return m_running; }

	// Receives challenges and sends the routing table when due. `now` in seconds.
	void update(double now);

	// SET_PLAYER_DATA, or REMOVE_PLAYER_DATA for a player who gets no channels.
	void setPlayer(const PlayerData& player);
	void removePlayer(uint16_t session);
	void resetRouting();
	// The players update() sends every 5 s (and at once on the next update).
	void setPlayers(const std::vector<PlayerData>& players);

	bool authenticated() const { return m_sessionString != unauthenticatedSessionString(); }
	const std::string& sessionString() const { return m_sessionString; }
	size_t packetsSent() const { return m_sent; }

	// A new session id for a player, unlike the ones in use (0x0057ce10,
	// provisional: the original draws from a power-of-two range).
	static uint16_t newSessionId(std::mt19937& random, const std::vector<uint16_t>& inUse);
	// The built-in voice server's password (0x0057c600).
	static std::string makeLocalPassword(std::mt19937& random);
	// The routing table for a set of players (0x0057cbe0).
	static RoutingTable routingTableFor(const std::vector<PlayerData>& players);

	std::function<void(const std::string&)> onLog;

private:
	void send(PacketType type, const std::vector<uint8_t>& payload);

	net::UdpServerSocket m_socket;
	net::UdpServerSocket::Address m_server;
	bool m_running = false;
	std::string m_password;
	std::string m_sessionString = unauthenticatedSessionString();
	RoutingTable m_table {};
	double m_nextRouting = -1;
	size_t m_sent = 0;
};

// Resolves a host name or dotted address (IPv4). False when unknown.
bool resolveAddress(const std::string& host, uint16_t port, net::UdpServerSocket::Address& out);

} // namespace dice::hfe::voip
