// The voice server, rewritten from BF2VoipServer.dll (CBF2VoipServer and
// CBF2VoipServerInstance). One instance is one UDP port shared by one game
// server and its clients:
//
// - The game server authenticates with a challenge: it sends anything,
//   the instance answers with ten letters + 'h', and from then on the
//   game server's packets carry md5(challenge + password)'s first 20 hex
//   digits and the version (0x10001d40). An instance serves one game
//   server; another one is refused while the first was heard within 28 s.
//   Silence for 28 s disconnects it and draws a new challenge.
// - The game server's SET_PLAYER_DATA / REMOVE_PLAYER_DATA /
//   RESET_PLAYER_ROUTING / FULL_ROUTING_DATA_SET packets make the
//   client table: by session id, each client's player id, the channel it
//   listens to and its channel sets (planChannels).
// - Clients' packets carry their session id; a known session's address is
//   taken from the packet (keep-alives register it). Voice is forwarded,
//   with the speaker's player id appended, to every listener of every
//   channel of the chosen set(s), except the speaker (0x10008830,
//   0x100088e0).
//
// Not rewritten: the debug logs (mods/bf2142/Logs/Voip/*, packets 4, 5
// and 200 are accepted and ignored) and the DLL's own thread (the game
// calls update(); BF2142.exe runs it every 10 ms, 0x00645480).
#pragma once

#include "Net/Socket.h"
#include "VoipProtocol.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace dice::hfe::voip {

class VoipServerInstance {
public:
	using Address = net::UdpServerSocket::Address;

	struct Options {
		uint16_t port = kDefaultServerPort; // 0: any free port
		std::string password;               // sv.voipSharedPassword
		bool loopbackOnly = false;
		uint32_t seed = 0;                  // the challenge's random numbers; 0 picks one
	};

	VoipServerInstance() = default;
	VoipServerInstance(const VoipServerInstance&) = delete;
	VoipServerInstance& operator=(const VoipServerInstance&) = delete;

	bool start(const Options& options);
	void stop();
	bool running() const { return m_running; }
	uint16_t port() const { return m_socket.port(); }

	// Handles what arrived (up to 128 packets, 0x10008760) and the game
	// server's timeout (0x100026c0). `now` in seconds.
	void update(double now);
	// Handles one datagram (update() calls it; tests may too).
	void handlePacket(const std::vector<uint8_t>& packet, const Address& from, double now);

	// --- State

	struct Client {
		uint16_t session = 0;
		uint16_t playerId = 0;
		Address address;           // from its last packet; 0 until then
		int incoming = -1;
		std::vector<std::vector<uint8_t>> sets;
		uint8_t selectedSet = 0;
		bool multi = false;
		std::vector<uint8_t> multiSets;
	};
	const Client* client(uint16_t session) const;
	size_t clientCount() const { return m_clients.size(); }
	// The sessions listening to a channel.
	const std::vector<uint16_t>& listeners(int channel) const;
	const RoutingTable& routing() const { return m_routing; }

	bool gameServerConnected() const { return m_gameServer.ip != 0; }
	const Address& gameServer() const { return m_gameServer; }
	const std::string& challenge() const { return m_challenge; }

	struct Counters {
		size_t invalidAuthentication = 0, invalidVersion = 0, inUse = 0;
		size_t voicePackets = 0, forwarded = 0, unknownSession = 0;
	};
	const Counters& counters() const { return m_counters; }

	// One line per event the DLL logs.
	std::function<void(const std::string&)> onLog;

private:
	void newChallenge();
	bool authenticate(const std::vector<uint8_t>& packet, const Address& from, double now, ServerPacket& out);
	void handleGameServer(const ServerPacket& packet);
	void handleClient(const std::vector<uint8_t>& packet, const Address& from);
	void setPlayerData(const PlayerData& player);
	void removeSession(uint16_t session);
	void eraseClient(uint16_t session);
	void removeAllClients();
	void fullRouting(const RoutingTable& table);
	void route(const Client& from, const std::vector<uint8_t>& payload, const std::vector<uint8_t>& sets);
	void log(const std::string& line);

	net::UdpServerSocket m_socket;
	bool m_running = false;
	std::string m_password;
	std::mt19937 m_random;
	std::string m_challenge;
	std::string m_expected; // the session string the game server must send

	Address m_gameServer;
	double m_lastGameServerPacket = -1; // any game-server packet (0x1190)
	double m_lastAccepted = -1;          // the bound game server's last accepted packet (0x11c0)

	std::map<uint16_t, Client> m_clients;
	std::array<std::vector<uint16_t>, kChannelCount> m_listeners;
	RoutingTable m_routing {};
	Counters m_counters;
};

// CBF2VoipServer: the instances the game server creates.
class VoipServer {
public:
	// Initialize(false): BF2142.exe runs update() itself.
	bool initialize();
	bool isRunning() const { return m_running; }
	// CreateInstance(password, port, ..., &id): ids count from 1.
	bool createInstance(const VoipServerInstance::Options& options, int& id);
	bool releaseInstance(int id);
	void releaseAllInstances();
	void shutdown();
	void update(double now);
	size_t instanceCount() const { return m_instances.size(); }
	VoipServerInstance* instance(size_t index);
	VoipServerInstance* instanceById(int id);

private:
	struct Entry {
		int id = 0;
		std::unique_ptr<VoipServerInstance> instance;
	};
	bool m_running = false;
	int m_nextId = 1;
	std::vector<Entry> m_instances;
};

} // namespace dice::hfe::voip
