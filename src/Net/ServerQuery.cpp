#include "ServerQuery.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dice::hfe::net {

namespace {

constexpr uint8_t kMagic1 = 0xfe, kMagic2 = 0xfd;
constexpr uint8_t kPacketQuery = 0, kPacketEcho = 2, kPacketEchoResponse = 5;

const char* const kKeyNames[] = {"", "hostname", "gamename", "gamever", "hostport", "mapname", "gametype", "gamevariant",
	"numplayers", "numteams", "maxplayers", "gamemode", "teamplay", "fraglimit", "teamfraglimit", "timeelapsed",
	"timelimit", "roundtime", "roundelapsed", "password", "groupid", "player_", "score_", "skill_", "ping_", "team_",
	"deaths_", "pid_", "team_t", "score_t"};
constexpr int kKeyCount = sizeof(kKeyNames) / sizeof(kKeyNames[0]);

void addString(std::vector<uint8_t>& out, const std::string& text)
{
	out.insert(out.end(), text.begin(), text.end());
	out.push_back(0);
}

std::string playerValue(const ServerReport::Player& p, uint8_t key)
{
	switch (key) {
	case 21: return p.name;
	case 22: return std::to_string(p.score);
	case 23: return std::to_string(p.skill);
	case 24: return std::to_string(p.ping);
	case 25: return std::to_string(p.team);
	case 26: return std::to_string(p.deaths);
	case 27: return std::to_string(p.pid);
	default: return {};
	}
}

uint32_t readKey(const std::vector<uint8_t>& data, size_t at)
{
	uint32_t key = 0;
	std::memcpy(&key, data.data() + at, 4);
	return key;
}

} // namespace

const char* qr2KeyName(uint8_t id) { return id < kKeyCount ? kKeyNames[id] : ""; }

int qr2KeyId(const std::string& name)
{
	for (int i = 1; i < kKeyCount; ++i) {
		if (name == kKeyNames[i]) {
			return i;
		}
	}
	return -1;
}

std::vector<uint8_t> qr2Query(uint32_t instanceKey, const std::vector<uint8_t>& serverKeys,
	const std::vector<uint8_t>& playerKeys, const std::vector<uint8_t>& teamKeys)
{
	std::vector<uint8_t> out {kMagic1, kMagic2, kPacketQuery};
	out.resize(7);
	std::memcpy(out.data() + 3, &instanceKey, 4);
	for (const auto* keys : {&serverKeys, &playerKeys, &teamKeys}) {
		out.push_back(static_cast<uint8_t>(keys->size()));
		out.insert(out.end(), keys->begin(), keys->end());
	}
	return out;
}

std::vector<uint8_t> qr2LanProbe()
{
	// 0x007249c0: FE FD 02 and five zero bytes.
	return {kMagic1, kMagic2, kPacketEcho, 0, 0, 0, 0, 0};
}

std::vector<uint8_t> qr2Answer(const std::vector<uint8_t>& request, const ServerReport& report)
{
	if (request.size() < 7 || request[0] != kMagic1 || request[1] != kMagic2) {
		return {};
	}
	std::vector<uint8_t> out(5);
	out[0] = request[2];
	std::copy(request.begin() + 3, request.begin() + 7, out.begin() + 1);
	if (request[2] == kPacketEcho) {
		// Type 5 with up to 32 bytes of the request's data.
		out[0] = kPacketEchoResponse;
		const size_t length = std::min<size_t>(request.size() - 7, 32);
		out.insert(out.end(), request.begin() + 7, request.begin() + 7 + static_cast<long>(length));
		return out;
	}
	if (request[2] != kPacketQuery) {
		return {};
	}
	// 0x00719de0: three key lists (a count, 0xff for all, then the ids).
	size_t at = 7;
	std::vector<uint8_t> lists[3];
	bool all[3] {};
	for (int list = 0; list < 3; ++list) {
		if (at >= request.size()) {
			return {};
		}
		const uint8_t count = request[at++];
		all[list] = count == 0xff;
		if (count != 0 && count != 0xff) {
			if (at + count > request.size()) {
				return {};
			}
			lists[list].assign(request.begin() + static_cast<long>(at), request.begin() + static_cast<long>(at + count));
			at += count;
		}
	}
	const auto value = [&report](const std::string& name) {
		const auto it = report.keys.find(name);
		return it == report.keys.end() ? std::string() : it->second;
	};
	if (all[0]) {
		for (const auto& [name, text] : report.keys) {
			addString(out, name);
			addString(out, text);
		}
		out.push_back(0);
	} else {
		for (const uint8_t key : lists[0]) {
			addString(out, value(qr2KeyName(key)));
		}
	}
	// Players: a 16-bit count (big-endian, 0x00719890), then every
	// player's values; for all keys the names come first.
	if (all[1] || !lists[1].empty()) {
		const auto count = static_cast<uint16_t>(report.players.size());
		out.push_back(static_cast<uint8_t>(count >> 8));
		out.push_back(static_cast<uint8_t>(count));
		std::vector<uint8_t> keys = lists[1];
		if (all[1]) {
			keys = {21, 22, 24, 25, 26, 27};
			for (const uint8_t key : keys) {
				addString(out, qr2KeyName(key));
			}
			out.push_back(0);
		}
		for (const ServerReport::Player& player : report.players) {
			for (const uint8_t key : keys) {
				addString(out, playerValue(player, key));
			}
		}
	}
	if (all[2] || !lists[2].empty()) {
		const auto count = static_cast<uint16_t>(report.teams.size());
		out.push_back(static_cast<uint8_t>(count >> 8));
		out.push_back(static_cast<uint8_t>(count));
		std::vector<uint8_t> keys = lists[2];
		if (all[2]) {
			keys = {28};
			addString(out, qr2KeyName(28));
			out.push_back(0);
		}
		for (const std::string& team : report.teams) {
			for (const uint8_t key : keys) {
				addString(out, key == 28 ? team : std::string("0"));
			}
		}
	}
	return out;
}

bool parseQr2Reply(const std::vector<uint8_t>& reply, uint32_t instanceKey, const std::vector<uint8_t>& serverKeys,
	const std::vector<uint8_t>& playerKeys, ServerReport& out)
{
	out = {};
	if (reply.size() < 5 || reply[0] != kPacketQuery || readKey(reply, 1) != instanceKey) {
		return false;
	}
	size_t at = 5;
	const auto next = [&](std::string& text) {
		const auto end = std::find(reply.begin() + static_cast<long>(at), reply.end(), 0);
		if (end == reply.end()) {
			return false;
		}
		text.assign(reply.begin() + static_cast<long>(at), end);
		at = static_cast<size_t>(end - reply.begin()) + 1;
		return true;
	};
	for (const uint8_t key : serverKeys) {
		std::string text;
		if (!next(text)) {
			return false;
		}
		out.keys[qr2KeyName(key)] = text;
	}
	if (!playerKeys.empty()) {
		if (at + 2 > reply.size()) {
			return false;
		}
		const int count = reply[at] << 8 | reply[at + 1];
		at += 2;
		for (int i = 0; i < count; ++i) {
			ServerReport::Player player;
			for (const uint8_t key : playerKeys) {
				std::string text;
				if (!next(text)) {
					return false;
				}
				const int number = std::atoi(text.c_str());
				switch (key) {
				case 21: player.name = text; break;
				case 22: player.score = number; break;
				case 23: player.skill = number; break;
				case 24: player.ping = number; break;
				case 25: player.team = number; break;
				case 26: player.deaths = number; break;
				case 27: player.pid = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10)); break;
				default: break;
				}
			}
			out.players.push_back(std::move(player));
		}
	}
	return true;
}

bool QueryServer::start(uint16_t port, bool loopbackOnly) { return m_socket.bind(port, loopbackOnly); }

void QueryServer::poll(const std::function<ServerReport()>& report)
{
	std::vector<uint8_t> datagram;
	UdpServerSocket::Address from;
	while (m_socket.receiveFrom(datagram, from, 0) > 0) {
		const std::vector<uint8_t> answer = qr2Answer(datagram, report ? report() : ServerReport {});
		if (!answer.empty()) {
			m_socket.sendTo(from, answer);
		}
		datagram.clear();
	}
}

const std::vector<uint8_t>& lanQueryKeys()
{
	// hostname, gametype, mapname, numplayers, maxplayers, hostport,
	// gamevariant, password, gamever: the standard keys among those
	// 0x00467d40 asks for (its others are the game's own, from 0x35 on).
	static const std::vector<uint8_t> keys {1, 6, 5, 8, 10, 4, 7, 19, 3};
	return keys;
}

std::vector<ServerEntry> browseLan(const std::string& address, uint16_t firstPort, uint16_t lastPort, int waitMilliseconds)
{
	std::vector<ServerEntry> found;
	UdpServerSocket::Address target;
	if (!UdpServerSocket::resolve(address, firstPort, target)) {
		return found;
	}
	const uint8_t* ip = reinterpret_cast<const uint8_t*>(&target.ip);
	const bool loopback = ip[0] == 127;
	UdpServerSocket socket;
	if (!socket.bind(0, loopback)) {
		return found;
	}
	if (!loopback) {
		socket.enableBroadcast();
	}
	// 0x007249c0: one probe per port, at most 500 ports.
	lastPort = static_cast<uint16_t>(std::min<int>(lastPort, firstPort + 500));
	for (int port = firstPort; port <= lastPort; ++port) {
		UdpServerSocket::Address to = target;
		UdpServerSocket::resolve(address, static_cast<uint16_t>(port), to);
		socket.sendTo(to, qr2LanProbe());
	}
	// 0x00724440: every new address that answers within the time is a server.
	std::vector<UdpServerSocket::Address> servers;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMilliseconds);
	std::vector<uint8_t> datagram;
	UdpServerSocket::Address from;
	while (std::chrono::steady_clock::now() < deadline) {
		datagram.clear();
		if (socket.receiveFrom(datagram, from, 20) <= 0) {
			continue;
		}
		if (!datagram.empty() && datagram[0] == kPacketEchoResponse
			&& std::find(servers.begin(), servers.end(), from) == servers.end()) {
			servers.push_back(from);
		}
	}
	// The query engine: each server's keys.
	std::map<size_t, ServerReport> reports;
	for (size_t i = 0; i < servers.size(); ++i) {
		socket.sendTo(servers[i], qr2Query(static_cast<uint32_t>(i + 1), lanQueryKeys()));
	}
	const auto queryDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
	while (reports.size() < servers.size() && std::chrono::steady_clock::now() < queryDeadline) {
		datagram.clear();
		if (socket.receiveFrom(datagram, from, 20) <= 0) {
			continue;
		}
		for (size_t i = 0; i < servers.size(); ++i) {
			ServerReport report;
			if (servers[i] == from && parseQr2Reply(datagram, static_cast<uint32_t>(i + 1), lanQueryKeys(), {}, report)) {
				reports[i] = std::move(report);
			}
		}
	}
	for (const auto& [i, report] : reports) {
		ServerEntry entry;
		const uint8_t* b = reinterpret_cast<const uint8_t*>(&servers[i].ip);
		char text[20];
		std::snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
		entry.ip = text;
		entry.port = static_cast<uint16_t>((servers[i].port & 0xff) << 8 | servers[i].port >> 8);
		entry.fields = report.keys;
		found.push_back(std::move(entry));
	}
	return found;
}

} // namespace dice::hfe::net
