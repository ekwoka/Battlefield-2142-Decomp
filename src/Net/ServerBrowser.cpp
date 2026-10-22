#include "ServerBrowser.h"

#include "GameSpyCrypt.h"
#include "Socket.h"

#include <cctype>
#include <chrono>
#include <cstring>
#include <random>

namespace dice::hfe::net {

namespace {

// Server flags.
constexpr uint8_t kPrivateIp = 0x02;
constexpr uint8_t kIcmpIp = 0x08;
constexpr uint8_t kNonstandardPort = 0x10;
constexpr uint8_t kNonstandardPrivatePort = 0x20;
constexpr uint8_t kHasKeys = 0x40;
// Field types.
constexpr uint8_t kString = 0, kByte = 1, kShort = 2;

class Reader {
public:
	Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}
	bool has(size_t n) const { return m_at + n <= m_size; }
	uint8_t byte() { return m_data[m_at++]; }
	uint16_t u16()
	{
		const uint16_t v = static_cast<uint16_t>(m_data[m_at] << 8 | m_data[m_at + 1]);
		m_at += 2;
		return v;
	}
	std::string ip()
	{
		char text[16];
		std::snprintf(text, sizeof(text), "%u.%u.%u.%u", m_data[m_at], m_data[m_at + 1], m_data[m_at + 2], m_data[m_at + 3]);
		m_at += 4;
		return text;
	}
	bool string(std::string& out)
	{
		const void* end = std::memchr(m_data + m_at, 0, m_size - m_at);
		if (!end) {
			return false;
		}
		const size_t length = static_cast<size_t>(static_cast<const uint8_t*>(end) - (m_data + m_at));
		out.assign(reinterpret_cast<const char*>(m_data + m_at), length);
		m_at += length + 1;
		return true;
	}
	size_t position() const { return m_at; }

private:
	const uint8_t* m_data;
	size_t m_size;
	size_t m_at = 0;
};

void put32(std::vector<uint8_t>& out, uint32_t v)
{
	for (int shift = 24; shift >= 0; shift -= 8) {
		out.push_back(static_cast<uint8_t>(v >> shift));
	}
}

void putString(std::vector<uint8_t>& out, const std::string& s)
{
	out.insert(out.end(), s.begin(), s.end());
	out.push_back(0);
}

} // namespace

std::string masterServerHost(const std::string& game, const std::string& domain)
{
	uint32_t hash = 0;
	for (char c : game) {
		hash = static_cast<uint32_t>(std::tolower(static_cast<unsigned char>(c))) + hash * 0x9ccf9319u;
	}
	return game + ".ms" + std::to_string(hash % 20) + "." + domain;
}

std::vector<uint8_t> listRequest(const std::array<uint8_t, 8>& challenge, const std::string& filter,
	const std::vector<std::string>& fields, uint32_t options, const char* game)
{
	std::vector<uint8_t> out {0, 0, 0, 1, 3};
	put32(out, 0); // game version
	putString(out, game);
	putString(out, game);
	out.insert(out.end(), challenge.begin(), challenge.end());
	putString(out, filter);
	std::string list;
	for (const std::string& field : fields) {
		list += "\\" + field;
	}
	putString(out, list);
	put32(out, options);
	out[0] = static_cast<uint8_t>(out.size() >> 8);
	out[1] = static_cast<uint8_t>(out.size());
	return out;
}

std::optional<ServerList> parseListReply(std::vector<uint8_t> reply, const std::array<uint8_t, 8>& challenge,
	const std::vector<std::string>&, const char* secretKey, std::string* error)
{
	const auto fail = [error](const std::string& why) -> std::optional<ServerList> {
		if (error) {
			*error = why;
		}
		return std::nullopt;
	};
	// The header and the server's challenge.
	if (reply.empty()) {
		return fail("incomplete");
	}
	const size_t headerLength = static_cast<size_t>(reply[0] ^ 0xec) + 2;
	if (reply.size() < headerLength) {
		return fail("incomplete");
	}
	const size_t keyLength = reply[headerLength - 1] ^ 0xea;
	if (reply.size() < headerLength + keyLength) {
		return fail("incomplete");
	}
	// 0x00723890: mix the secret key and the server's challenge into ours.
	std::array<uint8_t, 8> key = challenge;
	const size_t secretLength = std::strlen(secretKey);
	const uint8_t* serverKey = reply.data() + headerLength;
	for (size_t i = 0; i < keyLength; ++i) {
		const size_t at = (static_cast<size_t>(static_cast<int>(static_cast<signed char>(secretKey[i % secretLength])) * static_cast<int>(i)) & 7);
		key[at] = static_cast<uint8_t>(key[at] ^ key[i & 7] ^ serverKey[i]);
	}
	GameSpyCrypt crypt(key.data(), key.size());
	const size_t start = headerLength + keyLength;
	crypt.decrypt(reply.data() + start, reply.size() - start);

	Reader in(reply.data() + start, reply.size() - start);
	ServerList list;
	if (!in.has(6)) {
		return fail("incomplete");
	}
	list.publicIp = in.ip();
	list.defaultPort = in.u16();
	if (list.defaultPort == 0xffff) { // an error message follows
		std::string message;
		in.string(message);
		return fail("master server: " + message);
	}
	if (!in.has(1)) {
		return fail("incomplete");
	}
	std::vector<std::pair<uint8_t, std::string>> keys(in.byte());
	for (auto& [type, name] : keys) {
		if (!in.has(1)) {
			return fail("incomplete");
		}
		type = in.byte();
		if (!in.string(name)) {
			return fail("incomplete");
		}
	}
	if (!in.has(1)) {
		return fail("incomplete");
	}
	std::vector<std::string> popular(in.byte());
	for (std::string& value : popular) {
		if (!in.string(value)) {
			return fail("incomplete");
		}
	}
	for (;;) {
		if (!in.has(5)) {
			return fail("incomplete");
		}
		ServerEntry server;
		server.flags = in.byte();
		const size_t ipAt = in.position();
		server.ip = in.ip();
		if (server.ip == "255.255.255.255") {
			(void)ipAt;
			break; // the end of the list
		}
		server.port = list.defaultPort;
		if (server.flags & kNonstandardPort) {
			if (!in.has(2)) {
				return fail("incomplete");
			}
			server.port = in.u16();
		}
		if (server.flags & kPrivateIp) {
			if (!in.has(4)) {
				return fail("incomplete");
			}
			server.privateIp = in.ip();
			server.privatePort = list.defaultPort;
			if (server.flags & kNonstandardPrivatePort) {
				if (!in.has(2)) {
					return fail("incomplete");
				}
				server.privatePort = in.u16();
			}
		}
		if (server.flags & kIcmpIp) {
			if (!in.has(4)) {
				return fail("incomplete");
			}
			in.ip();
		}
		if (server.flags & kHasKeys) {
			for (const auto& [type, name] : keys) {
				if (!in.has(1)) {
					return fail("incomplete");
				}
				if (type == kString) {
					const uint8_t index = in.byte();
					std::string value;
					if (index == 0xff) {
						if (!in.string(value)) {
							return fail("incomplete");
						}
					} else if (index < popular.size()) {
						value = popular[index];
					}
					server.fields[name] = value;
				} else if (type == kByte) {
					server.fields[name] = std::to_string(in.byte());
				} else if (type == kShort) {
					if (!in.has(2)) {
						return fail("incomplete");
					}
					server.fields[name] = std::to_string(in.u16());
				}
			}
		}
		list.servers.push_back(std::move(server));
	}
	return list;
}

std::optional<ServerList> fetchServerList(const std::string& host, const std::string& filter,
	const std::vector<std::string>& fields, int timeoutSeconds, std::string* error)
{
	std::array<uint8_t, 8> challenge {};
	std::mt19937 random(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
	for (uint8_t& c : challenge) {
		c = static_cast<uint8_t>(0x21 + random() % 94); // printable, as the SDK makes it
	}
	TcpSocket socket;
	if (!socket.connect(host, kMasterPort, timeoutSeconds)) {
		if (error) {
			*error = "cannot connect to " + host;
		}
		return std::nullopt;
	}
	if (!socket.send(listRequest(challenge, filter, fields, kSendFieldsForAll))) {
		if (error) {
			*error = "cannot send the request";
		}
		return std::nullopt;
	}
	std::vector<uint8_t> reply;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
	std::string why;
	while (std::chrono::steady_clock::now() < deadline) {
		const int received = socket.receive(reply, 1);
		if (received < 0) {
			break; // closed
		}
		if (received > 0) {
			auto list = parseListReply(reply, challenge, fields, kSecretKey, &why);
			if (list || why != "incomplete") {
				if (!list && error) {
					*error = why;
				}
				return list;
			}
		}
	}
	auto list = parseListReply(reply, challenge, fields, kSecretKey, &why);
	if (!list && error) {
		*error = why == "incomplete" ? "the reply ended early (" + std::to_string(reply.size()) + " bytes)" : why;
	}
	return list;
}

} // namespace dice::hfe::net
