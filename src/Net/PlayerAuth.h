// What a joining client tells a server about its player, and how a
// retail server checks it.
//
// After the server's settings (data block 0, sent with the writer's
// argument 1 on connecting) the client answers with data block 1, the
// player info (0x004b5eb0 case 0 builds it when settings +0x30 is set;
// written by 0x005850a0 into 199 bytes, read by 0x00584ff0): a bit stream
// of
//   the name (u16 length + bytes, at most 0x40: GSPlayerNameNoClanTag),
//   a u32: the checksum (0x00579410) of the soldier's name, kept at
//     0x00a6801c when the soldier logs in (0x004a1d70 case 0xb ->
//     0x005795f0),
//   the profile id (sign + 31 bits, 0x00515350; ProfileManager slot 0x20,
//     0 without one),
//   the clan tag (u16 + at most 0x20 bytes, GSPlayerClanTag),
//   the auth (u16 + at most 0x5c bytes): EAOnlineManager slot 0x74, the
//     GameSpy Presence login ticket ("lt", gpGetLoginTicket 0x00717fb0)
//     that 0x0048b350 stores with 0x0049f9e0 ('+' -> '[', '/' -> ']',
//     '=' -> '_'),
//   1 bit: settings +0x31 (the server's voice is on).
// The server (0x004aef10 type 1 -> 0x004ae6a0) refuses a name whose
// checksum differs (0x24), on a ranked server one without a profile id
// (0xd), then asks the stats service (GameSpyAuth, vtable 0x00965538 slot
// 4, 0x00584c00) "ValidatePlayer.aspx?auth=<auth>&tid=0&SoldierNick=<name>
// &pid=<pid>", makes the player (CreatePlayer, the level) and, when the
// answer's result isn't "Ok" (InvalidAuthProfileID, InvalidReportedNick,
// InvalidReportedProfileID: 0x00584560), the admin script's
// onPlayerNameValidated (admin/standard_admin/playerconnect.py) bans the
// player. Without this block a retail server never makes the player.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dice::hfe::net {

constexpr uint32_t kPlayerInfoBlock = 1; // the data block's type
constexpr int kCdKeyGameId = 0x52c;      // the CD-key SDK's game id (0x00642f90 -> 0x007184b0)

struct PlayerInfoBlock {
	std::string name = "Player";
	uint32_t nameChecksum = 0;
	int32_t profileId = -1;
	std::string clanTag;
	std::string auth;
	bool voice = false;
};
std::string encodePlayerInfo(const PlayerInfoBlock& info);
bool decodePlayerInfo(const std::string& data, PlayerInfoBlock& out);

// 0x00579410: Adler-32 with the bytes of each whole 16-byte run taken
// lower-case and the rest as they are (signed), in runs of 0x15b0.
uint32_t nameChecksum(const std::string& name);
// 0x0049f9e0 / 0x00584e70: the ticket made safe for a URL.
std::string urlSafeTicket(const std::string& ticket);

// The server's question to the stats service (0x00584c00).
std::string validatePlayerPath(const std::string& auth, const std::string& nick, int32_t profileId);
// The answer's "result" column (stella's tab-separated format: O, H and D
// lines, "$\t<length>\t$"); empty when it has none.
std::string validatePlayerResult(const std::string& body);

// A blocking HTTP/1.0 GET (the body of a 200 answer; empty and false
// otherwise). Checks hostAllowed().
bool httpGet(const std::string& host, uint16_t port, const std::string& path, std::string& body,
	int timeoutMilliseconds = 5000);

// GameSpy's CD-key service, as a server asks it (the gcd SDK in
// BF2142.exe, 0x007184b0): UDP, every datagram XORed with "gamespy";
// "\auth\\pid\<game id>\ch\<challenge>\resp\<response>\ip\<ip>\skey\<n>"
// is answered "\uok\\cd\<md5 of the key>\skey\<n>" or "\unok\...\errmsg\".
std::vector<uint8_t> gameSpyXor(std::string text);
// True when the service accepted; `message` holds its error otherwise.
bool cdKeyAuthenticate(const std::string& host, uint16_t port, const std::string& challenge,
	const std::string& response, uint32_t clientIp, std::string* message = nullptr, int timeoutMilliseconds = 3000);

} // namespace dice::hfe::net
