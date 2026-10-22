// BF2142's voice chat on the wire.
//
// Three parties talk over UDP:
//
// - The game server tells the voice server who is in which team and squad
//   (BF2142.exe's VoipServerConnection.cpp, 0x0057c0a0-0x0057ce10). Its
//   packets end with a 24-byte version, a 20-byte session string and a
//   packet type (100-103, 200).
// - The voice server (BF2VoipServer.dll, CBF2VoipServerInstance) routes
//   voice between clients by channels.
// - Game clients (BF2142.exe's Bf2VoipClient, 0x0043ba40-0x0043cd40, on
//   GameSpy Voice 2's client, 0x00711070-0x00711e90) send Speex frames
//   that end with their channel choice, their session id and a type (1-3,
//   6). The voice server forwards each frame with the speaker's player id
//   appended.
//
// Every packet is read from its end: the type is the last byte and each
// layer takes its fields off the tail (BF2VoipServer.dll 0x10008660,
// 0x10008690, 0x10001cc0). Integers are little-endian.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace dice::hfe::voip {

// --- Packet types (the last byte; names from BF2VoipServer.dll 0x10002c50)

enum PacketType : uint8_t {
	// Client to voice server.
	kVoiceDefaultChannelSet = 1,   // talk into channel set 0
	kVoiceAlternateChannelSet = 2, // talk into the set whose index precedes the session
	kVoiceMultiChannelSet = 3,     // talk into several sets
	kDebugStart = 4,               // start logging this player's voice (debug builds' logs)
	kDebugStop = 5,
	kKeepAlive = 6,                // registers the client's address (sent every 3 s)
	// Game server to voice server (BF2_VOIP_PACKET_*).
	kSetPlayerData = 100,
	kRemovePlayerData = 101,
	kResetPlayerRouting = 102,
	kFullRoutingDataSet = 103,
	kChallenge = 104,              // voice server to game server: 'h'
	kDebugData = 200,
};

// --- Game server <-> voice server

// The version both sides compare: sprintf("%06d%06d%06d%06d", 1, 10, 112,
// 0) (BF2VoipServer.dll 0x10002a0d, VoipServerConnection 0x0057c68c).
constexpr const char* kVersionString = "000001000010000112000000";
constexpr size_t kVersionSize = 24;
// The session string: the first 20 hex digits of md5(challenge + password)
// (0x10002510, 0x0057c430); 20 spaces before the voice server challenged
// (the instance's "no session yet" value, 0x10003430).
constexpr size_t kSessionStringSize = 20;
constexpr size_t kChallengeSize = 10; // lower-case letters
const std::string& unauthenticatedSessionString();
std::string sessionStringFor(const std::string& challenge, const std::string& password);
// Ten random lower-case letters (the DLL seeds rand() from the computer's name).
std::string makeChallenge(std::mt19937& random);

// A game server packet: payload, version, session string, type.
std::vector<uint8_t> buildServerPacket(PacketType type, const std::vector<uint8_t>& payload,
	const std::string& sessionString, const std::string& version = kVersionString);
struct ServerPacket {
	uint8_t type = 0;
	std::string sessionString; // 20 bytes
	std::string version;       // 24 bytes (only read once the session string matched)
	std::vector<uint8_t> payload;
};
// Splits the tail off. False when the packet is too short.
bool parseServerPacket(const std::vector<uint8_t>& packet, ServerPacket& out);
// The challenge the voice server answers with: the letters, then 'h'.
std::vector<uint8_t> buildChallengePacket(const std::string& challenge);

// A player as SET_PLAYER_DATA carries it (8 bytes, 0x0057c8a0 / 0x10003850).
struct PlayerData {
	uint8_t team = 0;         // 1 or 2
	bool commander = false;
	bool squadLeader = false;
	uint8_t squad = 0;        // 1-9, 0 for none
	uint16_t session = 0;     // the player's voice session id
	uint16_t playerId = 0;    // 0-63; 0xff for the server's own player
	bool operator==(const PlayerData& o) const
	{
		return team == o.team && commander == o.commander && squadLeader == o.squadLeader && squad == o.squad
			&& session == o.session && playerId == o.playerId;
	}
};
std::vector<uint8_t> encodePlayerData(const PlayerData& player);
bool decodePlayerData(const uint8_t* data, size_t size, PlayerData& out);

// The routing table of FULL_ROUTING_DATA_SET: 65 records of 4 bytes, by
// player id (64 is the server's own player): flags, 0, session. Flags:
// 0x80 team 2, 0x40 commander, 0x20 squad leader, the low nibble the
// squad (0x0057cbe0, decoded by 0x10003d70).
constexpr size_t kRoutingSlots = 65;
constexpr size_t kServerPlayerSlot = 64;
constexpr uint16_t kServerPlayerId = 0xff;
struct RoutingRecord {
	uint8_t flags = 0;
	uint16_t session = 0;
	bool operator==(const RoutingRecord& o) const { return flags == o.flags && session == o.session; }
	bool operator!=(const RoutingRecord& o) const { return !(*this == o); }
};
using RoutingTable = std::array<RoutingRecord, kRoutingSlots>;
RoutingRecord routingRecordFor(const PlayerData& player);
PlayerData playerDataFor(const RoutingRecord& record, uint16_t playerId);
std::vector<uint8_t> encodeRoutingTable(const RoutingTable& table);
bool decodeRoutingTable(const std::vector<uint8_t>& payload, RoutingTable& out); // 0x104 bytes

// --- Channels (BF2VoipServer.dll 0x10002e70, 0x10003850)
//
// 38 channels: per team a commander channel, nine squad-leader channels
// and nine squad-member channels. Team 1 uses 0-18, team 2 19-37.
// A player listens to one channel and talks into channel sets:
//
// | Player        | Listens to          | Set 0                      | Other sets                 |
// |---------------|---------------------|----------------------------|----------------------------|
// | Commander     | commander           | all nine leader channels   | 1-9: squad n's leader      |
// | Squad leader  | squad n's leader    | squad n's members          | 1: the commander           |
// | Squad member  | squad n's members   | squad n's members + leader |                            |
//
// Players without a squad (and not commander) get no channels.
constexpr int kChannelCount = 38;
int commanderChannel(int team);
int leaderChannel(int team, int squad);
int memberChannel(int team, int squad);
const char* channelName(int channel); // the DLL's names (with its typos)

struct ChannelPlan {
	int incoming = -1;
	std::vector<std::vector<uint8_t>> sets;
};
// False for a player who gets no channels (squad out of 1-9 without being
// commander, or a team other than 1 and 2).
bool planChannels(const PlayerData& player, ChannelPlan& out);

// --- Client <-> voice server

// The channel choice a client sends with its voice (GameSpy Voice's
// 0x007116d0 / 0x00711710 / 0x00711c10).
struct ChannelSelection {
	PacketType mode = kVoiceDefaultChannelSet;
	uint8_t alternateSet = 0;
	std::vector<uint8_t> multiSets;
	bool operator==(const ChannelSelection& o) const
	{
		return mode == o.mode && alternateSet == o.alternateSet && multiSets == o.multiSets;
	}
};
// What Bf2VoipClient picks (0x0043bd10 dispatches on the player: squad
// leaders to 0x0043be40, the commander to 0x0043cd40): squad members talk
// to their squad (the default set); squad leaders to their squad, or to the
// commander while the leader-channel key is held (alternate set 1); the
// commander to all squad leaders while it is held (the default set). With
// the key up the commander talks to the squads (1-9) selected on the
// commander screen (HUD 0x00b2d148 vtable 0x228 == 0x10; vtable 0x2c4's
// +0x34 per squad), as a multi set. With none selected and the screen open
// the commander is held back (+0xc0, cleared by 0x0043c8d0 once the screen
// closes or the player stops commanding); with the screen closed the
// selection doesn't change.
struct ChannelChoice {
	bool keep = false;   // the current selection stays
	bool silent = false; // the commander is held back (+0xc0)
	ChannelSelection channels;
};
ChannelChoice selectChannels(bool commander, bool squadLeader, bool leaderKeyHeld,
	const std::vector<int>& selectedSquads = {}, bool commanderScreenOpen = false);

// Voice from a client: the payload (frames and their sequence number),
// the channel choice, the session id, the type.
std::vector<uint8_t> buildVoicePacket(const std::vector<uint8_t>& payload, const ChannelSelection& channels, uint16_t session);
std::vector<uint8_t> buildKeepAlivePacket(uint16_t session);
struct ClientPacket {
	uint8_t type = 0;
	uint16_t session = 0;
	ChannelSelection channels;
	std::vector<uint8_t> payload;
};
bool parseClientPacket(const std::vector<uint8_t>& packet, ClientPacket& out);

// The voice payload: whole Speex frames (each `frameBytes` long), then
// the 16-bit sequence number of the first (counted in frames). The voice
// server appends the speaker's player id (16 bits) when forwarding
// (0x10008830); the client reads it back (0x0043c0b0).
std::vector<uint8_t> buildVoicePayload(const std::vector<uint8_t>& frames, uint16_t sequence);
std::vector<uint8_t> buildForwardedVoice(const std::vector<uint8_t>& payload, uint16_t speaker);
struct ForwardedVoice {
	uint16_t speaker = 0;
	uint16_t sequence = 0;
	std::vector<uint8_t> frames;
};
bool parseForwardedVoice(const std::vector<uint8_t>& packet, ForwardedVoice& out);

// --- Codec settings

// The server's voipQuality (1-5) as GameSpy Voice's codec, as Speex's
// quality (0x00711120, 0x007217f0): 1 -> 1, 2 -> 2, 3 -> 4, 4 -> 7,
// 5 -> 10; anything else 2.
int speexQualityFor(int voipQuality);

// --- The voice server settings the game server sends its clients
//
// In the settings transfer (data block 0, written by 0x00515bc0): the
// server's name, a list of strings, seven flags (the second is
// voipEnabled), the voice server's IPv4 address (network order; 1 means
// "the game server's own"), its port (sv.voipServerPort), the client's
// local port (sv.voipBFClientPort) and the quality. The client starts its
// voice client with them (0x004ab030, 0x004cbe40) once a VoipSession event
// gave it a session id.
struct ServerVoipSettings {
	bool enabled = false;
	uint32_t address = 0; // network order; 1: the game server's
	uint16_t serverPort = 55125;
	uint16_t clientPort = 55123;
	int quality = 3;
};
// The seven flags (0x00515bc0 writes them, 0x00516230 reads them back in
// the same order): bit 0 the writer's argument (+0x30), bit 1 voipEnabled
// (game logic slot 0x254 reads +0x1cc, the reader stores it with slot 0x250,
// and 0x004cbe40 starts the voice client on it), bits 2-6 the server
// settings at 0x00a201c0 +0xfa, +0xfb, +0x148, +0xf8, +0xf9. Then the
// address (32 bits, +0x1d0), the server port (16, +0x1d4), the client port
// (16, +0x1d6) and the quality (sign + 31, +0x1dc); the fields after those
// aren't needed here.
bool parseServerVoipSettings(const std::string& settingsBlock, ServerVoipSettings& out);
std::string buildServerVoipSettingsBlock(const std::string& serverName, const ServerVoipSettings& settings); // for tests

// Default ports (mods/bf2142/Settings/ServerSettings.con).
constexpr uint16_t kDefaultServerPort = 55125;   // sv.voipServerPort: where the voice server listens
constexpr uint16_t kDefaultBFClientPort = 55123; // sv.voipBFClientPort: game clients' local port
constexpr uint16_t kDefaultBFServerPort = 55124; // sv.voipBFServerPort: the game server's local port

} // namespace dice::hfe::voip
