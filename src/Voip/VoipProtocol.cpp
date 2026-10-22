#include "VoipProtocol.h"

#include "Net/BitStream.h"
#include "Net/Md5.h"

#include <algorithm>

namespace dice::hfe::voip {

namespace {

void putU16(std::vector<uint8_t>& out, uint16_t value)
{
	out.push_back(static_cast<uint8_t>(value));
	out.push_back(static_cast<uint8_t>(value >> 8));
}

uint16_t getU16(const uint8_t* at)
{
	return static_cast<uint16_t>(at[0] | at[1] << 8);
}

// Takes `n` bytes off the end of [data, data + size).
bool popTail(const std::vector<uint8_t>& packet, size_t& size, size_t n, const uint8_t*& at)
{
	if (size < n) {
		return false;
	}
	size -= n;
	at = packet.data() + size;
	return true;
}

} // namespace

const std::string& unauthenticatedSessionString()
{
	static const std::string spaces(kSessionStringSize, ' ');
	return spaces;
}

std::string sessionStringFor(const std::string& challenge, const std::string& password)
{
	// sprintf("%s%s", challenge, password), md5 as lower-case hex, cut to 20.
	return net::md5Hex(challenge + password).substr(0, kSessionStringSize);
}

std::string makeChallenge(std::mt19937& random)
{
	std::string challenge;
	std::uniform_int_distribution<int> letter(0, 25);
	for (size_t i = 0; i < kChallengeSize; ++i) {
		challenge.push_back(static_cast<char>('a' + letter(random)));
	}
	return challenge;
}

std::vector<uint8_t> buildServerPacket(PacketType type, const std::vector<uint8_t>& payload,
	const std::string& sessionString, const std::string& version)
{
	std::vector<uint8_t> out(payload);
	std::string v = version;
	v.resize(kVersionSize, ' ');
	std::string s = sessionString;
	s.resize(kSessionStringSize, ' ');
	out.insert(out.end(), v.begin(), v.end());
	out.insert(out.end(), s.begin(), s.end());
	out.push_back(type);
	return out;
}

bool parseServerPacket(const std::vector<uint8_t>& packet, ServerPacket& out)
{
	size_t size = packet.size();
	const uint8_t* at = nullptr;
	if (!popTail(packet, size, 1, at)) {
		return false;
	}
	out.type = *at;
	if (!popTail(packet, size, kSessionStringSize, at)) {
		return false;
	}
	out.sessionString.assign(reinterpret_cast<const char*>(at), kSessionStringSize);
	if (!popTail(packet, size, kVersionSize, at)) {
		out.version.clear();
		out.payload.clear();
		return true; // the session string is checked first (0x10001d40)
	}
	out.version.assign(reinterpret_cast<const char*>(at), kVersionSize);
	out.payload.assign(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(size));
	return true;
}

std::vector<uint8_t> buildChallengePacket(const std::string& challenge)
{
	std::vector<uint8_t> out(challenge.begin(), challenge.end());
	out.push_back(kChallenge);
	return out;
}

std::vector<uint8_t> encodePlayerData(const PlayerData& player)
{
	std::vector<uint8_t> out {player.team, static_cast<uint8_t>(player.commander), static_cast<uint8_t>(player.squadLeader),
		player.squad};
	putU16(out, player.session);
	putU16(out, player.playerId);
	return out;
}

bool decodePlayerData(const uint8_t* data, size_t size, PlayerData& out)
{
	if (!data || size < 8) {
		return false;
	}
	out.team = data[0];
	out.commander = data[1] != 0;
	out.squadLeader = data[2] != 0;
	out.squad = data[3];
	out.session = getU16(data + 4);
	out.playerId = getU16(data + 6);
	return true;
}

RoutingRecord routingRecordFor(const PlayerData& player)
{
	// 0x10001b60.
	RoutingRecord record;
	record.session = player.session;
	if (player.team == 2) {
		record.flags = 0x80;
	}
	if (player.commander) {
		record.flags |= 0x40;
	}
	if (player.squadLeader) {
		record.flags |= 0x20;
	}
	record.flags |= player.squad & 0x0f;
	return record;
}

PlayerData playerDataFor(const RoutingRecord& record, uint16_t playerId)
{
	// 0x10003d70.
	PlayerData player;
	player.team = (record.flags & 0x80) ? 2 : 1;
	player.commander = (record.flags & 0x40) != 0;
	player.squadLeader = (record.flags & 0x20) != 0;
	player.squad = record.flags & 0x0f;
	player.session = record.session;
	player.playerId = playerId;
	return player;
}

std::vector<uint8_t> encodeRoutingTable(const RoutingTable& table)
{
	std::vector<uint8_t> out;
	out.reserve(kRoutingSlots * 4);
	for (const RoutingRecord& record : table) {
		out.push_back(record.flags);
		out.push_back(0);
		putU16(out, record.session);
	}
	return out;
}

bool decodeRoutingTable(const std::vector<uint8_t>& payload, RoutingTable& out)
{
	if (payload.size() != kRoutingSlots * 4) {
		return false;
	}
	for (size_t i = 0; i < kRoutingSlots; ++i) {
		out[i].flags = payload[i * 4];
		out[i].session = getU16(payload.data() + i * 4 + 2);
	}
	return true;
}

int commanderChannel(int team)
{
	return team == 2 ? 0x13 : 0;
}

int leaderChannel(int team, int squad)
{
	return commanderChannel(team) + squad;
}

int memberChannel(int team, int squad)
{
	return commanderChannel(team) + 9 + squad;
}

const char* channelName(int channel)
{
	// 0x10002e70; team 2's squad D member channel is "... 3" in the DLL too.
	static const char* names[kChannelCount] = {"CHANNEL_TEAM_1_COMMANDER", "CHANNEL_TEAM_1_SQUAD_A_LEADER 1",
		"CHANNEL_TEAM_1_SQUAD_B_LEADER 2", "CHANNEL_TEAM_1_SQUAD_C_LEADER 3", "CHANNEL_TEAM_1_SQUAD_D_LEADER 4",
		"CHANNEL_TEAM_1_SQUAD_E_LEADER 5", "CHANNEL_TEAM_1_SQUAD_F_LEADER 6", "CHANNEL_TEAM_1_SQUAD_G_LEADER 7",
		"CHANNEL_TEAM_1_SQUAD_H_LEADER 8", "CHANNEL_TEAM_1_SQUAD_I_LEADER 9", "CHANNEL_TEAM_1_SQUAD_A_MEMBER 1",
		"CHANNEL_TEAM_1_SQUAD_B_MEMBER 2", "CHANNEL_TEAM_1_SQUAD_C_MEMBER 3", "CHANNEL_TEAM_1_SQUAD_D_MEMBER 4",
		"CHANNEL_TEAM_1_SQUAD_E_MEMBER 5", "CHANNEL_TEAM_1_SQUAD_F_MEMBER 6", "CHANNEL_TEAM_1_SQUAD_G_MEMBER 7",
		"CHANNEL_TEAM_1_SQUAD_H_MEMBER 8", "CHANNEL_TEAM_1_SQUAD_I_MEMBER 9", "CHANNEL_TEAM_2_COMMANDER",
		"CHANNEL_TEAM_2_SQUAD_A_LEADER 1", "CHANNEL_TEAM_2_SQUAD_B_LEADER 2", "CHANNEL_TEAM_2_SQUAD_C_LEADER 3",
		"CHANNEL_TEAM_2_SQUAD_D_LEADER 4", "CHANNEL_TEAM_2_SQUAD_E_LEADER 5", "CHANNEL_TEAM_2_SQUAD_F_LEADER 6",
		"CHANNEL_TEAM_2_SQUAD_G_LEADER 7", "CHANNEL_TEAM_2_SQUAD_H_LEADER 8", "CHANNEL_TEAM_2_SQUAD_I_LEADER 9",
		"CHANNEL_TEAM_2_SQUAD_A_MEMBER 1", "CHANNEL_TEAM_2_SQUAD_B_MEMBER 2", "CHANNEL_TEAM_2_SQUAD_C_MEMBER 3",
		"CHANNEL_TEAM_2_SQUAD_D_MEMBER 3", "CHANNEL_TEAM_2_SQUAD_E_MEMBER 5", "CHANNEL_TEAM_2_SQUAD_F_MEMBER 6",
		"CHANNEL_TEAM_2_SQUAD_G_MEMBER 7", "CHANNEL_TEAM_2_SQUAD_H_MEMBER 8", "CHANNEL_TEAM_2_SQUAD_I_MEMBER 9"};
	return channel >= 0 && channel < kChannelCount ? names[channel] : "INVALID CHANNEL";
}

bool planChannels(const PlayerData& player, ChannelPlan& out)
{
	// 0x10003850.
	out = {};
	if (player.team != 1 && player.team != 2) {
		return false;
	}
	const int team = player.team;
	if (player.commander) {
		out.incoming = commanderChannel(team);
		std::vector<uint8_t> all;
		for (int squad = 1; squad <= 9; ++squad) {
			all.push_back(static_cast<uint8_t>(leaderChannel(team, squad)));
		}
		out.sets.push_back(all);
		for (int squad = 1; squad <= 9; ++squad) {
			out.sets.push_back({static_cast<uint8_t>(leaderChannel(team, squad))});
		}
		return true;
	}
	const int squad = player.squad;
	if (squad < 1 || squad > 9) {
		out = {};
		return false;
	}
	if (player.squadLeader) {
		out.incoming = leaderChannel(team, squad);
		out.sets.push_back({static_cast<uint8_t>(memberChannel(team, squad))});
		out.sets.push_back({static_cast<uint8_t>(commanderChannel(team))});
	} else {
		out.incoming = memberChannel(team, squad);
		out.sets.push_back({static_cast<uint8_t>(memberChannel(team, squad)), static_cast<uint8_t>(leaderChannel(team, squad))});
	}
	return true;
}

ChannelChoice selectChannels(bool commander, bool squadLeader, bool leaderKeyHeld, const std::vector<int>& selectedSquads,
	bool commanderScreenOpen)
{
	ChannelChoice choice;
	if (commander) {
		// 0x0043cd40.
		if (leaderKeyHeld) {
			return choice; // vtable 0x40(1, 0): the default set, +0xc0 cleared
		}
		if (commanderScreenOpen) {
			for (int squad : selectedSquads) {
				if (squad >= 1 && squad <= 9) {
					choice.channels.multiSets.push_back(static_cast<uint8_t>(squad));
				}
			}
		}
		if (!choice.channels.multiSets.empty()) {
			choice.channels.mode = kVoiceMultiChannelSet; // vtable 0x44
			return choice;
		}
		choice.keep = true;
		choice.silent = commanderScreenOpen;
		return choice;
	}
	if (squadLeader && leaderKeyHeld) {
		// 0x0043be40: vtable 0x40(2, 1).
		choice.channels.mode = kVoiceAlternateChannelSet;
		choice.channels.alternateSet = 1;
	}
	return choice;
}

std::vector<uint8_t> buildVoicePacket(const std::vector<uint8_t>& payload, const ChannelSelection& channels, uint16_t session)
{
	std::vector<uint8_t> out(payload);
	switch (channels.mode) {
	case kVoiceAlternateChannelSet:
		out.push_back(channels.alternateSet);
		break;
	case kVoiceMultiChannelSet:
		// The sets go in order and are read back from the end: the server
		// gets them reversed, which doesn't matter (0x00711c10, 0x100089d0).
		out.insert(out.end(), channels.multiSets.begin(), channels.multiSets.end());
		out.push_back(static_cast<uint8_t>(channels.multiSets.size()));
		break;
	default:
		break;
	}
	putU16(out, session);
	out.push_back(channels.mode == kVoiceAlternateChannelSet || channels.mode == kVoiceMultiChannelSet ? channels.mode
																									 : kVoiceDefaultChannelSet);
	return out;
}

std::vector<uint8_t> buildKeepAlivePacket(uint16_t session)
{
	// 0x00711760: three bytes.
	std::vector<uint8_t> out;
	putU16(out, session);
	out.push_back(kKeepAlive);
	return out;
}

bool parseClientPacket(const std::vector<uint8_t>& packet, ClientPacket& out)
{
	out = {};
	size_t size = packet.size();
	const uint8_t* at = nullptr;
	if (!popTail(packet, size, 1, at)) {
		return false;
	}
	out.type = *at;
	if (out.type < kVoiceDefaultChannelSet || out.type > kKeepAlive) {
		return false;
	}
	if (!popTail(packet, size, 2, at)) {
		return false;
	}
	out.session = getU16(at);
	out.channels.mode = static_cast<PacketType>(out.type);
	if (out.type == kVoiceAlternateChannelSet) {
		if (!popTail(packet, size, 1, at)) {
			return false;
		}
		out.channels.alternateSet = *at;
	} else if (out.type == kVoiceMultiChannelSet) {
		if (!popTail(packet, size, 1, at)) {
			return false;
		}
		const size_t count = *at;
		for (size_t i = 0; i < count; ++i) {
			if (!popTail(packet, size, 1, at)) {
				return false;
			}
			out.channels.multiSets.push_back(*at);
		}
	}
	out.payload.assign(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(size));
	return true;
}

std::vector<uint8_t> buildVoicePayload(const std::vector<uint8_t>& frames, uint16_t sequence)
{
	std::vector<uint8_t> out(frames);
	putU16(out, sequence);
	return out;
}

std::vector<uint8_t> buildForwardedVoice(const std::vector<uint8_t>& payload, uint16_t speaker)
{
	std::vector<uint8_t> out(payload);
	putU16(out, speaker);
	return out;
}

bool parseForwardedVoice(const std::vector<uint8_t>& packet, ForwardedVoice& out)
{
	if (packet.size() < 4) {
		return false;
	}
	const size_t n = packet.size();
	out.speaker = getU16(packet.data() + n - 2);
	out.sequence = getU16(packet.data() + n - 4);
	out.frames.assign(packet.begin(), packet.end() - 4);
	return true;
}

int speexQualityFor(int voipQuality)
{
	switch (voipQuality) {
	case 1:
		return 1;
	case 2:
		return 2;
	case 3:
		return 4;
	case 4:
		return 7;
	case 5:
		return 10;
	default:
		return 2;
	}
}

bool parseServerVoipSettings(const std::string& block, ServerVoipSettings& out)
{
	net::BitReader in(reinterpret_cast<const uint8_t*>(block.data()), block.size());
	const uint32_t nameLength = in.read(16);
	for (uint32_t i = 0; i < nameLength && !in.bad(); ++i) {
		in.read(8);
	}
	const uint32_t strings = in.read(16);
	for (uint32_t s = 0; s < strings && s < 16 && !in.bad(); ++s) {
		const uint32_t length = in.read(16);
		for (uint32_t i = 0; i < length && !in.bad(); ++i) {
			in.read(8);
		}
	}
	uint32_t flags[7] = {};
	for (uint32_t& flag : flags) {
		flag = in.read(1);
	}
	ServerVoipSettings settings;
	settings.enabled = flags[1] != 0;
	settings.address = in.read(32);
	settings.serverPort = static_cast<uint16_t>(in.read(16));
	settings.clientPort = static_cast<uint16_t>(in.read(16));
	settings.quality = in.readSigned(32);
	if (in.bad()) {
		return false;
	}
	out = settings;
	return true;
}

std::string buildServerVoipSettingsBlock(const std::string& serverName, const ServerVoipSettings& settings)
{
	net::BitWriter out;
	out.write(static_cast<uint32_t>(serverName.size()), 16);
	out.writeBytes(serverName.data(), serverName.size());
	out.write(0, 16); // no strings
	for (int i = 0; i < 7; ++i) {
		out.write(i == 1 ? settings.enabled : 0, 1);
	}
	out.write(settings.address, 32);
	out.write(settings.serverPort, 16);
	out.write(settings.clientPort, 16);
	out.writeSigned(settings.quality, 32);
	const std::vector<uint8_t>& data = out.data();
	return std::string(data.begin(), data.end());
}

} // namespace dice::hfe::voip
