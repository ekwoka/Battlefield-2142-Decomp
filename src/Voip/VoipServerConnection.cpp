#include "VoipServerConnection.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <cstring>

namespace dice::hfe::voip {

namespace {

constexpr double kRoutingInterval = 5.0; // 0x0057cbe0

} // namespace

bool resolveAddress(const std::string& host, uint16_t port, net::UdpServerSocket::Address& out)
{
#ifdef _WIN32
	static const bool started = [] {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	(void)started;
#endif
	addrinfo hints {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo* found = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) {
		return false;
	}
	sockaddr_in address {};
	std::memcpy(&address, found->ai_addr, std::min<size_t>(sizeof address, found->ai_addrlen));
	freeaddrinfo(found);
	out.ip = address.sin_addr.s_addr;
	out.port = htons(port);
	return true;
}

bool VoipServerConnection::start(const Options& options)
{
	stop();
	if (!resolveAddress(options.host, options.port, m_server) || !m_socket.bind(options.localPort, options.loopbackOnly)) {
		return false;
	}
	m_password = options.password;
	m_sessionString = unauthenticatedSessionString();
	m_nextRouting = -1;
	m_running = true;
	return true;
}

void VoipServerConnection::stop()
{
	m_socket.close();
	m_running = false;
}

void VoipServerConnection::send(PacketType type, const std::vector<uint8_t>& payload)
{
	if (m_running && m_socket.sendTo(m_server, buildServerPacket(type, payload, m_sessionString))) {
		++m_sent;
	}
}

void VoipServerConnection::update(double now)
{
	if (!m_running) {
		return;
	}
	// 0x0057c4f0: an 'h' packet is a challenge.
	std::vector<uint8_t> packet;
	net::UdpServerSocket::Address from;
	while (m_socket.receiveFrom(packet, from, 0) > 0) {
		if (from == m_server && packet.size() == kChallengeSize + 1 && packet.back() == kChallenge) {
			const std::string challenge(packet.begin(), packet.end() - 1);
			m_sessionString = sessionStringFor(challenge, m_password);
			if (onLog) {
				onLog("voip: challenged, session string " + m_sessionString);
			}
			m_nextRouting = now; // answer at once
		}
	}
	if (m_nextRouting < 0 || now >= m_nextRouting) {
		send(kFullRoutingDataSet, encodeRoutingTable(m_table));
		m_nextRouting = now + kRoutingInterval;
	}
}

void VoipServerConnection::setPlayer(const PlayerData& player)
{
	ChannelPlan plan;
	if (!planChannels(player, plan)) {
		removePlayer(player.session);
		return;
	}
	send(kSetPlayerData, encodePlayerData(player));
}

void VoipServerConnection::removePlayer(uint16_t session)
{
	send(kRemovePlayerData, {static_cast<uint8_t>(session), static_cast<uint8_t>(session >> 8)});
}

void VoipServerConnection::resetRouting()
{
	send(kResetPlayerRouting, {});
}

void VoipServerConnection::setPlayers(const std::vector<PlayerData>& players)
{
	m_table = routingTableFor(players);
	m_nextRouting = -1;
}

RoutingTable VoipServerConnection::routingTableFor(const std::vector<PlayerData>& players)
{
	RoutingTable table {};
	for (const PlayerData& player : players) {
		ChannelPlan plan;
		if (!planChannels(player, plan)) {
			continue; // 0x0057cd68: neither commander nor in a squad
		}
		if (player.playerId < 64) {
			table[player.playerId] = routingRecordFor(player);
		} else if (player.playerId == kServerPlayerId) {
			table[kServerPlayerSlot] = routingRecordFor(player);
		}
	}
	return table;
}

uint16_t VoipServerConnection::newSessionId(std::mt19937& random, const std::vector<uint16_t>& inUse)
{
	// 0x0057ce10: (rand() & 0xffff) % (int)pow(256.0, 2.0), the CRT's rand()
	// giving 0-0x7fff; while a player (PlayerManager 0x00a85228, vtable
	// 0x40; player vtable 0x2e8) has it, 1 + rand() % 500 more and the
	// players are checked again from the first.
	std::uniform_int_distribution<int> crtRand(0, 0x7fff);
	int id = (crtRand(random) & 0xffff) % 65536;
	for (bool clash = true; clash;) {
		clash = std::find(inUse.begin(), inUse.end(), static_cast<uint16_t>(id)) != inUse.end();
		if (clash) {
			id += 1 + crtRand(random) % 500;
		}
	}
	return static_cast<uint16_t>(id);
}

std::string VoipServerConnection::makeLocalPassword(std::mt19937& random)
{
	std::uniform_int_distribution<int> length(8, 17), letter(0, 25);
	std::string password = "v";
	for (int i = length(random); i > 0; --i) {
		password.push_back(static_cast<char>('a' + letter(random)));
	}
	return password;
}

} // namespace dice::hfe::voip
