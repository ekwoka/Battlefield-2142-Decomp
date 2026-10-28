// Asking a game server directly: GameSpy's query and reporting protocol
// version 2 (QR2) as BF2142.exe's GameSpy SDK has it, both sides.
//
// The server side (qr2's packet parser, 0x0071a7d0's switch on the third
// byte) answers datagrams starting FE FD on its query port (29900 by
// default), after the type byte and a 4-byte instance key:
//   0 a query (0x00719de0): three key lists, server, player and team keys
//     (a count byte, 0xff for all, then the key ids), and an optional
//     flags byte (bit 0: split replies, 0x00719b30). The reply (0x00719af0
//     -> 0x00719890): 0, the instance key, then the server keys' values,
//     each a zero-terminated string (an empty one when there is none,
//     0x007194c0); for player and team keys a 16-bit big-endian count,
//     then for each player (team) every key's value.
//     For "all" server keys the names and values in turn, then a 0.
//   2 an echo (LAN discovery): answered with type 5, the instance key and
//     up to 32 bytes of the request's data.
//   1, 4, 6, 9 are the master server's (challenges, client messages, IP
//     checks); a server not reporting to a master never gets them.
//
// The client side: the server browser's LAN search (0x00715900 ->
// 0x007249c0, started by 0x00467d40 for the "lan" browser) sends FE FD 02
// 00 00 00 00 00 to the broadcast address (0x0058ccb0: 255.255.255.255)
// on every port from 29900 to 29950 (0x74cc-0x74fe, at most 500), with
// SO_BROADCAST; every address that answers within 2 seconds
// (0x00724440) becomes a server, which the browser's query engine then
// asks for its keys with a query.
//
// The key ids are qr2's registered keys (the table at 0x00a08238):
// 1 hostname, 2 gamename, 3 gamever, 4 hostport, 5 mapname, 6 gametype,
// 7 gamevariant, 8 numplayers, 9 numteams, 10 maxplayers, 11 gamemode,
// 12 teamplay, 13 fraglimit, 14 teamfraglimit, 15 timeelapsed,
// 16 timelimit, 17 roundtime, 18 roundelapsed, 19 password, 20 groupid;
// player keys 21 player_, 22 score_, 23 skill_, 24 ping_, 25 team_,
// 26 deaths_, 27 pid_; team keys 28 team_t, 29 score_t.
#pragma once

#include "ServerBrowser.h"
#include "Socket.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe::net {

constexpr uint16_t kQueryPort = 29900;      // 0x74cc
constexpr uint16_t kLastLanQueryPort = 29950; // 0x74fe

// A registered key's name ("" for ids without one).
const char* qr2KeyName(uint8_t id);
int qr2KeyId(const std::string& name); // -1 for none

// What a server reports.
struct ServerReport {
	std::map<std::string, std::string> keys; // server keys by name
	struct Player {
		std::string name;
		int score = 0, ping = 0, team = 0, deaths = 0, skill = 0;
		uint32_t pid = 0;
	};
	std::vector<Player> players;
	std::vector<std::string> teams;
};

// The packets.
std::vector<uint8_t> qr2Query(uint32_t instanceKey, const std::vector<uint8_t>& serverKeys,
	const std::vector<uint8_t>& playerKeys = {}, const std::vector<uint8_t>& teamKeys = {});
std::vector<uint8_t> qr2LanProbe(); // the LAN search's echo request
// The server's answer to a datagram (empty: none).
std::vector<uint8_t> qr2Answer(const std::vector<uint8_t>& request, const ServerReport& report);
// Reads a query's reply for the keys asked (by id): server key values by
// name, players. False when it isn't a reply to `instanceKey`.
bool parseQr2Reply(const std::vector<uint8_t>& reply, uint32_t instanceKey, const std::vector<uint8_t>& serverKeys,
	const std::vector<uint8_t>& playerKeys, ServerReport& out);

// The responder a server runs on its query port.
class QueryServer {
public:
	bool start(uint16_t port = kQueryPort, bool loopbackOnly = true);
	uint16_t port() const { return m_socket.port(); }
	void stop() { m_socket.close(); }
	// Answers what has arrived with the report `report` gives.
	void poll(const std::function<ServerReport()>& report);

private:
	UdpServerSocket m_socket;
};

// The browser's server keys for a LAN server (those the menu's list shows).
const std::vector<uint8_t>& lanQueryKeys();

// The LAN search: probes `address` (the original's 255.255.255.255, which
// needs broadcasting; 127.0.0.1 finds the servers on this machine) on the
// query ports, waits `waitMilliseconds` for answers and queries each
// server that answered. Entries carry the server keys as fields; `ip` and
// `port` are where it answered (its query port).
std::vector<ServerEntry> browseLan(const std::string& address = "255.255.255.255", uint16_t firstPort = kQueryPort,
	uint16_t lastPort = kLastLanQueryPort, int waitMilliseconds = 2000);

} // namespace dice::hfe::net
