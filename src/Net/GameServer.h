// The server's side of a game connection (BF2142.exe's network session as
// a server: the receive loop 0x0068ab90, the connect checks 0x00689d30,
// the accept 0x00689400; the game server "dice.bf.gameServer" handles
// the connected clients' events, 0x004b0690 for PostRemote category 6).
//
// One UDP socket for every client. A client connects (type 1: the
// protocol 0x1002, the build, the password and the mod directory, checked
// as 0x00689d30 does: a wrong password is refusal 0x11, another mod 0x24,
// no free connection 7, a full server 0x1e), gets an id (8 bits, the
// connection id) and acknowledges (type 4). The server then challenges
// its CD key (event 1, 0x00509cc0) and waits for the response (event 2)
// and the connection type (event 3, an index into the rate table); the
// client is joined from then on (onJoined).
//
// Each tick the server sends every joined client a data packet: a lone 0
// bit for the input block, the next reliable event block (EventChannel.h)
// and the ghost block the game gives for that client. From the client it
// takes the input block's moves (each move index once, in order: the
// client resends its last three, 0x00590450) and the events, in block
// order. Pings are answered (type 8, 0x0068e050); a client silent for
// 30 seconds is dropped.
#pragma once

#include "EventChannel.h"
#include "GameConnection.h"
#include "GameProtocol.h"
#include "Socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dice::hfe::net {

struct GameServerOptions {
	uint16_t port = kGamePort;
	bool loopbackOnly = true;          // bind 127.0.0.1 only
	std::string password;              // empty: none
	std::string modDirectory = "mods/bf2142";
	int maxClients = 64;
	std::string challenge = "BF2142rcp"; // the CD-key challenge (event 1); a real server makes a random one
};

class GameServer {
public:
	bool start(const GameServerOptions& options = {});
	void stop();
	bool running() const { return m_running; }
	uint16_t port() const { return m_socket.port(); }

	// Receives and handles everything that has arrived (never blocks).
	void poll();
	// Sends every joined client its data packet: the queued events and the
	// ghost block from `ghosts` (nullptr: none).
	void sendTick(const std::function<GhostBlock(int client)>& ghosts);

	void send(int client, const GameEvent& event);
	void broadcast(const GameEvent& event);
	// Ends a client's connection (type 5 with the reason).
	void kick(int client, uint32_t reason = 0);

	struct ClientInfo {
		int id = 0;
		bool joined = false;           // the challenge answered and the connection type given
		uint32_t profileId = 0xffffffff;
		uint32_t connectionType = 0;
		std::string cdKeyHash;         // the response's first 32 characters: md5 of the key
		int32_t lastMove = -1;         // the last move index taken
		uint32_t lagMs = 0;            // the input block's 9-bit view delay
		uint32_t roundTrip = 0;        // ms, from our pings
		std::string address;           // "a.b.c.d:port"
	};
	std::vector<ClientInfo> clients() const;
	const ClientInfo* client(int id) const;

	std::function<void(int client)> onJoined;
	std::function<void(int client)> onLeft;
	std::function<void(int client, const GameEvent& event)> onEvent;
	std::function<void(int client, const PlayerMove& move)> onMove;
	// A data packet we sent (its 6-bit sequence) was delivered or lost, as
	// the client's notify header says.
	std::function<void(int client, uint32_t sequence, bool delivered)> onNotified;
	// The sequence the client's next data packet (sendTick's) will carry.
	uint32_t nextSequence(int client) const;

	uint32_t now() const;

private:
	struct Client {
		ClientInfo info;
		UdpServerSocket::Address address;
		bool acknowledged = false;
		NotifyState notify;
		EventChannel events;
		std::set<uint32_t> unsettled; // data sequences sent, not yet reported
		std::chrono::steady_clock::time_point lastHeard, lastPing;
	};
	void handle(const std::vector<uint8_t>& datagram, const UdpServerSocket::Address& from);
	void handleConnect(BitReader& in, const UdpServerSocket::Address& from);
	void handleData(Client& client, const ConnectedPacket& packet);
	void deliver(Client& client, const GameEvent& event);
	void refuse(const UdpServerSocket::Address& to, uint32_t reason);
	void drop(int id);
	Client* find(const UdpServerSocket::Address& from);

	GameServerOptions m_options;
	UdpServerSocket m_socket;
	bool m_running = false;
	std::map<int, Client> m_clients;
	std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

} // namespace dice::hfe::net
