// The game's server list: GameSpy's master server protocol (server browsing
// version 2), as BF2142.exe's GameSpy SDK speaks it (0x00723490 connects,
// 0x00723d30 reads the reply), to OpenSpy's master servers, which the
// OpenSpy patch points the game at (<game>.ms<N>.openspy.net:28910).
//
// The request (TCP, big-endian):
//   u16 length; u8 0 (list request); u8 1 (protocol); u8 3 (encoding);
//   u32 game version; "stella\0" twice (query for, query from);
//   8-byte client challenge; filter\0; "\\field\\field..."\0; u32 options.
// The reply is encrypted after a header: byte 0 ^ 0xEC plus 2 is the
// header's length, its last byte ^ 0xEA the length of the server challenge
// that follows. The cipher key is the client challenge with the secret key
// ("M8o1Qw", built at 0x00574510) and the server challenge mixed in
// (0x00723890). The plain text: our public IP (4 bytes), the default query
// port (2), the field count and per field a type byte and name, the
// "popular values" (strings the server entries refer to by index), then per
// server a flags byte, IP, optional port, private IP, ICMP IP, and the
// field values, until the IP 255.255.255.255.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::net {

constexpr const char* kGameName = "stella";
constexpr const char* kSecretKey = "M8o1Qw";
constexpr uint16_t kMasterPort = 28910;
// List request options: the fields of every server in the list (without
// it the master sends them for some servers only).
constexpr uint32_t kSendFieldsForAll = 1;

// The master server's host for a game: <game>.ms<hash % 20>.<domain>
// (0x00723450: h = tolower(c) + h * 0x9ccf9319).
std::string masterServerHost(const std::string& game, const std::string& domain = "openspy.net");

struct ServerEntry {
	std::string ip;
	uint16_t port = 0; // query port
	std::string privateIp;
	uint16_t privatePort = 0;
	uint8_t flags = 0;
	std::map<std::string, std::string> fields;
};

struct ServerList {
	std::string publicIp; // as the master sees us
	uint16_t defaultPort = 0;
	std::vector<ServerEntry> servers;
};

// The request bytes.
std::vector<uint8_t> listRequest(const std::array<uint8_t, 8>& challenge, const std::string& filter,
	const std::vector<std::string>& fields, uint32_t options = 0, const char* game = kGameName);

// Decodes a whole reply (decrypting it). Empty on malformed data; `error`
// says why (or carries the master's error message).
std::optional<ServerList> parseListReply(std::vector<uint8_t> reply, const std::array<uint8_t, 8>& challenge,
	const std::vector<std::string>& fields, const char* secretKey = kSecretKey, std::string* error = nullptr);

// Connects to the master server and fetches the list (blocking, with a
// timeout in seconds).
std::optional<ServerList> fetchServerList(const std::string& host, const std::string& filter,
	const std::vector<std::string>& fields, int timeoutSeconds, std::string* error = nullptr);

} // namespace dice::hfe::net
