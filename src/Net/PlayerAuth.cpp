#include "PlayerAuth.h"

#include "BitStream.h"
#include "OnlineConfig.h"
#include "Socket.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <sstream>

namespace dice::hfe::net {

namespace {

void writeString(BitWriter& out, const std::string& text, size_t most)
{
	const std::string s = text.substr(0, most);
	out.write(static_cast<uint32_t>(s.size()) & 0xffff, 16);
	out.writeBytes(s.data(), s.size());
}

bool readString(BitReader& in, std::string& out)
{
	const uint32_t length = in.read(16);
	if (in.bad() || in.remaining() < static_cast<size_t>(length) * 8) {
		return false;
	}
	out.clear();
	for (uint32_t i = 0; i < length; ++i) {
		out.push_back(static_cast<char>(in.read(8)));
	}
	return true;
}

std::string urlEncode(const std::string& s)
{
	std::string out;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '[' || c == ']' || c == '~') {
			out += static_cast<char>(c);
		} else {
			char hex[4];
			std::snprintf(hex, sizeof(hex), "%%%02X", c);
			out += hex;
		}
	}
	return out;
}

} // namespace

std::string encodePlayerInfo(const PlayerInfoBlock& info)
{
	// 0x005850a0, field by field.
	BitWriter out;
	writeString(out, info.name, 0x40);
	out.write(info.nameChecksum, 32);
	out.writeSigned(info.profileId, 32);
	writeString(out, info.clanTag, 0x20);
	writeString(out, info.auth, 0x5c);
	out.write(info.voice ? 1 : 0, 1);
	const auto& bytes = out.data();
	return std::string(bytes.begin(), bytes.end());
}

bool decodePlayerInfo(const std::string& data, PlayerInfoBlock& out)
{
	// 0x00584ff0.
	BitReader in(reinterpret_cast<const uint8_t*>(data.data()), data.size());
	out = {};
	if (!readString(in, out.name)) {
		return false;
	}
	out.nameChecksum = in.read(32);
	out.profileId = in.readSigned(32);
	if (!readString(in, out.clanTag) || !readString(in, out.auth)) {
		return false;
	}
	out.voice = in.read(1) == 1;
	return !in.bad();
}

uint32_t nameChecksum(const std::string& name)
{
	// 0x00579410 (an Adler-32 whose unrolled 16-byte loop lower-cases;
	// the remainder loop doesn't, and takes the bytes signed).
	uint32_t a = 0, b = 0;
	const char* p = name.c_str();
	size_t left = name.size();
	while (left != 0) {
		size_t run = left > 0x15af ? 0x15b0 : left;
		left -= run;
		while (run >= 16) {
			for (int i = 0; i < 16; ++i) {
				const int c = static_cast<signed char>(p[i]);
				a += static_cast<uint32_t>(c >= 0 ? std::tolower(c) : c);
				b += a;
			}
			p += 16;
			run -= 16;
		}
		for (; run != 0; --run) {
			a += static_cast<uint32_t>(static_cast<int>(static_cast<signed char>(*p++)));
			b += a;
		}
		a %= 0xfff1;
		b %= 0xfff1;
	}
	return b << 16 | a;
}

std::string urlSafeTicket(const std::string& ticket)
{
	std::string out = ticket;
	for (char& c : out) {
		c = c == '+' ? '[' : c == '/' ? ']' : c == '=' ? '_' : c;
	}
	return out;
}

std::string validatePlayerPath(const std::string& auth, const std::string& nick, int32_t profileId)
{
	// 0x00584c00: "ValidatePlayer.aspx" + "?auth=" + auth + "&tid=" + "0" +
	// "&SoldierNick=" + nick + "&pid=" + pid (the base is GameSpyAuth's
	// stats URL, http://stella.prod.gamespy.com/ at 0x009645a4).
	return "/ValidatePlayer.aspx?auth=" + auth + "&tid=0&SoldierNick=" + urlEncode(nick) + "&pid=" + std::to_string(profileId);
}

std::string validatePlayerResult(const std::string& body)
{
	// "H\t...result\t\nD\t<value>\t\n": the column named result.
	std::istringstream in(body);
	std::string line;
	std::vector<std::string> header;
	while (std::getline(in, line)) {
		std::vector<std::string> cells;
		std::string cell;
		std::istringstream row(line);
		while (std::getline(row, cell, '\t')) {
			cells.push_back(cell);
		}
		if (cells.empty()) {
			continue;
		}
		if (cells[0] == "H") {
			header = cells;
		} else if (cells[0] == "D") {
			for (size_t i = 1; i < header.size() && i < cells.size(); ++i) {
				if (header[i] == "result") {
					return cells[i];
				}
			}
		}
	}
	return {};
}

bool httpGet(const std::string& host, uint16_t port, const std::string& path, std::string& body, int timeoutMilliseconds)
{
	body.clear();
	if (!hostAllowed(host)) {
		return false;
	}
	TcpSocket socket;
	if (!socket.connect(host, port, std::max(1, timeoutMilliseconds / 1000))) {
		return false;
	}
	const std::string request = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: GameSpyHTTP/1.0\r\nConnection: close\r\n\r\n";
	if (!socket.send(std::vector<uint8_t>(request.begin(), request.end()))) {
		return false;
	}
	std::vector<uint8_t> reply;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
	while (std::chrono::steady_clock::now() < deadline) {
		if (socket.receiveFor(reply, 100) < 0) {
			break; // closed: the whole answer is in
		}
	}
	const std::string text(reply.begin(), reply.end());
	const size_t end = text.find("\r\n\r\n");
	if (text.compare(0, 9, "HTTP/1.0 ") != 0 && text.compare(0, 9, "HTTP/1.1 ") != 0) {
		return false;
	}
	if (text.compare(9, 3, "200") != 0 || end == std::string::npos) {
		return false;
	}
	body = text.substr(end + 4);
	return true;
}

std::vector<uint8_t> gameSpyXor(std::string text)
{
	static const char key[] = "gamespy";
	for (size_t i = 0; i < text.size(); ++i) {
		text[i] = static_cast<char>(text[i] ^ key[i % 7]);
	}
	return std::vector<uint8_t>(text.begin(), text.end());
}

bool cdKeyAuthenticate(const std::string& host, uint16_t port, const std::string& challenge, const std::string& response,
	uint32_t clientIp, std::string* message, int timeoutMilliseconds)
{
	if (!hostAllowed(host)) {
		if (message) {
			*message = hostRefusal(host);
		}
		return false;
	}
	UdpSocket socket;
	if (!socket.open(host, port)) {
		if (message) {
			*message = "cannot reach the CD-key service";
		}
		return false;
	}
	static uint32_t nextKey = 1;
	const uint32_t skey = nextKey++;
	const std::string request = "\\auth\\\\pid\\" + std::to_string(kCdKeyGameId) + "\\ch\\" + challenge + "\\resp\\" + response
		+ "\\ip\\" + std::to_string(static_cast<int32_t>(clientIp)) + "\\skey\\" + std::to_string(skey);
	socket.send(gameSpyXor(request));
	std::vector<uint8_t> reply;
	if (socket.receive(reply, timeoutMilliseconds) <= 0) {
		if (message) {
			*message = "no answer from the CD-key service";
		}
		return false;
	}
	std::vector<uint8_t> plain = gameSpyXor(std::string(reply.begin(), reply.end()));
	const std::string text(plain.begin(), plain.end());
	if (text.compare(0, 5, "\\uok\\") == 0) {
		return true;
	}
	if (message) {
		const size_t at = text.find("\\errmsg\\");
		*message = at == std::string::npos ? text : text.substr(at + 8, text.find('\\', at + 8) - at - 8);
	}
	return false;
}

} // namespace dice::hfe::net
