#include "GameProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dice::hfe::net {

// ---------------------------------------------------------------------------
// Player input

const std::vector<int>& movedTriggerInputs()
{
	// 0x009653a8, ended by 64 (c_PINumInput).
	static const std::vector<int> ids {32, 34, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 8, 31,
		47, 9, 10, 13, 12, 37, 38, 62, 36, 35, 39, 40, 41, 42, 11, 43, 44, 33, 45, 46, 49, 48};
	return ids;
}

bool PlayerMove::trigger(int input) const
{
	const int bit = input - 8;
	if (bit < 0) {
		return false;
	}
	return bit < 32 ? (triggersLow >> bit & 1u) != 0 : bit < 43 && (triggersHigh >> (bit - 32) & 1u) != 0;
}

void PlayerMove::setTrigger(int input, bool on)
{
	const int bit = input - 8;
	if (bit < 0 || bit >= 64) {
		return;
	}
	uint32_t& word = bit < 32 ? triggersLow : triggersHigh;
	const uint32_t mask = 1u << (bit % 32);
	word = on ? word | mask : word & ~mask;
}

void PlayerMove::setAxis(int input, float value)
{
	// 0x005837d0: value x 100 clamped to +-32768, rounded.
	const float scaled = std::clamp(value * 100.0f, -32768.0f, 32767.0f);
	axes[static_cast<size_t>(input)] = static_cast<int16_t>(std::lround(scaled));
}

PlayerMove PlayerInput::toMove(int32_t index) const
{
	PlayerMove move;
	move.index = index;
	for (int axis = 0; axis < 6; ++axis) {
		move.setAxis(axis, values[static_cast<size_t>(axis)]);
	}
	for (int id : movedTriggerInputs()) {
		if (values[static_cast<size_t>(id)] > 0.5f) {
			move.setTrigger(id, true);
		}
	}
	move.triggersHigh &= 0x7ff; // only 11 bits travel
	move.axisPitchRoll = axisPitchRoll;
	return move;
}

namespace {

void writeMove(BitWriter& out, const PlayerMove& move)
{
	for (int16_t axis : move.axes) {
		const int v = axis;
		out.write(v < 0 ? 1 : 0, 1);
		out.write(static_cast<uint32_t>(v < 0 ? -v : v), 15);
	}
	out.write(move.triggersLow, 32);
	out.write(move.triggersHigh, 11);
	out.write(move.axisPitchRoll ? 1 : 0, 1);
}

void readMove(BitReader& in, PlayerMove& move)
{
	for (int16_t& axis : move.axes) {
		const bool negative = in.read(1) == 1;
		const auto magnitude = static_cast<int>(in.read(15));
		axis = static_cast<int16_t>(negative ? -magnitude : magnitude);
	}
	move.triggersLow = in.read(32);
	move.triggersHigh = in.read(11);
	move.axisPitchRoll = in.read(1) == 1;
}

} // namespace

void writeInputBlock(BitWriter& out, const InputBlock& block)
{
	if (!block.present) {
		out.write(0, 1);
		return;
	}
	out.write(1, 1);
	out.write(static_cast<uint32_t>(std::min<size_t>(block.moves.size(), 15)), 4);
	out.write(block.value9, 9);
	if (!block.moves.empty()) {
		out.writeSigned(block.moves.front().index, 32);
		for (size_t m = 0; m < block.moves.size() && m < 15; ++m) {
			writeMove(out, block.moves[m]);
		}
	}
}

bool readInputBlock(BitReader& in, InputBlock& block)
{
	block = {};
	block.present = in.read(1) == 1;
	if (!block.present) {
		return !in.bad();
	}
	const uint32_t count = in.read(4);
	block.value9 = in.read(9);
	if (count > 0) {
		const int32_t first = in.readSigned(32);
		for (uint32_t m = 0; m < count && !in.bad(); ++m) {
			PlayerMove move;
			readMove(in, move);
			move.index = first + static_cast<int32_t>(m);
			block.moves.push_back(move);
		}
	}
	return !in.bad();
}

// ---------------------------------------------------------------------------
// Events

namespace {

const char* const kEventNames[kEventTypeCount] = {
	nullptr, "Challenge", "ChallengeResponse", "ConnectionType", "DataBlock", "CreatePlayer", "CreateObject",
	"DestroyObject", "DestroyPlayer", "EnterVehicle", "ExitVehicle", "PostRemote", "ChangePlayerName", "HandleDrop",
	"HandlePickup", "StringBlock", "JoinSquad", "LeaveSquad", nullptr, "Commander", "RadioMessage", "KilledBy",
	"ChangeSquadName", "SetPrivateSquad", "IssueSquadOrder", "Invite", "Rank", "SetAcceptOrder", "SetSquadLeader",
	"Spotted", "Artillery", "CreateKit", "StickyProjectile", "Titan", "AmbientEffectArea", "VoipOnOff", "CommanderCam",
	"SupplyDrop", "VoipPlayerMute", "Vote", "ToggleFreeCamera", "Medal", "Unlock", "MissileInit", "FieldUnlock", "Uav",
	"ContentCheck", "TargetDirection", "EndOfRound", "PythonCommand", "Request", "ChangePcoTeam", "SiloSync", nullptr,
	"VoipSession", "KickBan", "BeginRound", "CreateSpawnGroup", "RemoveSpawnGroup", "AutoControllerFire", "SetBeacon",
	"UavWarning", "UpdateKit", "SelectKit", "ReloadUpdate", "SquadRadar", "RefreshUnlocks", "CommanderTool",
	"MissileSiloFire", "CommanderToolsUpdate", "Entitlement", "JoinAsSpectator"};

// Bits for a value below `count` (the engine's loop: the smallest n with
// 2^n - 1 >= count - 1... written as `(1 << n) - 1 < max`).
unsigned bitsFor(uint32_t max)
{
	unsigned n = 0;
	while ((1u << n) - 1 < max) {
		++n;
	}
	return n;
}

// Reads or writes an event's fields in wire order, so each layout is
// written once.
class Codec {
public:
	Codec(BitReader& in, GameEvent& event) : m_in(&in), m_event(&event) {}
	Codec(BitWriter& out, const GameEvent& event) : m_out(&out), m_event(const_cast<GameEvent*>(&event)) {}

	bool ok() const { return m_ok && (!m_in || !m_in->bad()); }

	uint32_t u(unsigned bits)
	{
		if (m_in) {
			const uint32_t v = m_in->read(bits);
			m_event->values.push_back(v);
			return v;
		}
		if (m_value >= m_event->values.size()) {
			m_ok = false;
			m_out->write(0, bits);
			return 0;
		}
		const uint32_t v = m_event->values[m_value++];
		m_out->write(v, bits);
		return v;
	}
	// Sign bit + bits - 1 (0x0050a460).
	int32_t s(unsigned bits)
	{
		if (m_in) {
			const int32_t v = m_in->readSigned(bits);
			m_event->values.push_back(static_cast<uint32_t>(v));
			return v;
		}
		if (m_value >= m_event->values.size()) {
			m_ok = false;
			m_out->writeSigned(0, bits);
			return 0;
		}
		const auto v = static_cast<int32_t>(m_event->values[m_value++]);
		m_out->writeSigned(v, bits);
		return v;
	}
	void vec()
	{
		u(32);
		u(32);
		u(32);
	}
	// The inline loops `while ((1 << n) - 1 < max) n++`.
	uint32_t upTo(uint32_t max) { return u(bitsFor(max)); }
	// A value min..max read by 0x00508370 (bits for max - min + 1, one more
	// than upTo's loop gives at powers of two minus one).
	uint32_t ranged(uint32_t max) { return u(bitsFor(max + 1)); }
	// Fixed bytes.
	void fixed(size_t bytes)
	{
		if (m_in) {
			std::string text;
			for (size_t i = 0; i < bytes; ++i) {
				text.push_back(static_cast<char>(m_in->read(8)));
			}
			m_event->bytes.push_back(text);
			return;
		}
		std::string text = nextBytes();
		text.resize(bytes, '\0');
		m_out->writeBytes(text.data(), bytes);
	}
	// An 8-bit length and that many bytes.
	void counted()
	{
		if (m_in) {
			const uint32_t length = m_in->read(8);
			std::string text;
			for (uint32_t i = 0; i < length; ++i) {
				text.push_back(static_cast<char>(m_in->read(8)));
			}
			m_event->bytes.push_back(text);
			return;
		}
		std::string text = nextBytes();
		if (text.size() > 255) {
			text.resize(255);
		}
		m_out->write(static_cast<uint32_t>(text.size()), 8);
		m_out->writeBytes(text.data(), text.size());
	}
	// A delta-coded position (0x006876c0 / 0x00687510): 2 bits mode; mode 0
	// three floats, else three signed values of 20, 16 or 12 bits
	// (0x00a05edc) scaled from a reference. Kept as mode and three raw values.
	void deltaPosition()
	{
		static const unsigned kBits[4] = {32, 20, 16, 12};
		const uint32_t mode = u(2);
		for (int axis = 0; axis < 3; ++axis) {
			if (mode == 0) {
				u(32);
			} else {
				s(kBits[mode]);
			}
		}
	}

private:
	std::string nextBytes()
	{
		if (m_bytes >= m_event->bytes.size()) {
			m_ok = false;
			return {};
		}
		return m_event->bytes[m_bytes++];
	}

	BitReader* m_in = nullptr;
	BitWriter* m_out = nullptr;
	GameEvent* m_event;
	size_t m_value = 0, m_bytes = 0;
	bool m_ok = true;
};

// Each type's fields, from its reader (vtable slot 10).
bool codeFields(Codec& c, uint32_t type)
{
	switch (type) {
	case kEvChallenge: // 0x00509cc0
		c.fixed(10);
		c.counted();
		break;
	case kEvChallengeResponse: // 0x00509e20
		c.fixed(73);
		c.u(32);
		c.u(32);
		c.u(32);
		break;
	case kEvConnectionType: // 0x0050a030: 0..6
		c.ranged(6);
		break;
	case kEvDataBlock: // 0x0050a1a0
		if (c.u(1)) {
			c.u(32);
			c.u(32);
		} else {
			c.counted();
		}
		break;
	case kEvCreatePlayer: // 0x0050a4c0
		c.u(3);
		c.u(4);
		c.u(1);
		c.u(8);
		c.u(16);
		c.u(16);
		c.u(1);
		c.fixed(32);
		c.s(32);
		c.s(4);
		c.u(1);
		break;
	case kEvCreateObject: // 0x0050b150
		c.u(32);
		c.u(16);
		c.u(2);
		if (c.u(1)) {
			c.u(8);
		} else {
			if (c.u(1)) {
				c.vec();
			}
			if (c.u(1)) {
				c.vec();
			}
		}
		break;
	case kEvDestroyObject: // 0x0050b6a0
		c.u(16);
		c.u(1);
		break;
	case kEvDestroyPlayer: // 0x0050b550
		c.u(8);
		break;
	case kEvEnterVehicle: // 0x0050b850
		c.u(8);
		c.u(16);
		c.u(1);
		break;
	case kEvExitVehicle: // 0x00510c20
	case kEvVoipOnOff:
		c.u(8);
		c.u(1);
		break;
	case kEvPostRemote: // 0x005096a0
		c.u(4);
		c.u(32);
		c.u(32);
		c.counted();
		break;
	case kEvChangePlayerName: // 0x0050ba40
		c.u(8);
		c.fixed(32);
		break;
	case kEvHandleDrop: // 0x0050c280
		c.u(8);
		c.u(16);
		c.u(16);
		c.vec();
		break;
	case kEvHandlePickup: // 0x0050bdb0
		c.u(8);
		c.u(16);
		c.u(16);
		for (int i = 0; i < 8; ++i) {
			c.u(8);
		}
		c.u(1);
		c.u(32);
		break;
	case kEvStringBlock: // 0x0050c360
		if (c.u(1)) {
			c.u(8);
		} else {
			c.counted();
		}
		break;
	case kEvJoinSquad: // 0x0050c590
	case kEvSetSquadLeader:
		c.u(8);
		c.u(8);
		c.u(8);
		break;
	case kEvLeaveSquad: // 0x0050c790
	case kEvInvite:     // 0x0050e5a0
		c.u(8);
		c.u(8);
		c.u(8);
		c.u(1);
		break;
	case kEvCommander: // 0x0050c940
		c.upTo(9);
		c.u(8);
		c.s(16);
		break;
	case kEvRadioMessage: // 0x0050d100
		c.u(16);
		c.u(8);
		c.u(2);
		c.u(8);
		c.upTo(0x13);
		c.upTo(5);
		break;
	case kEvKilledBy: // 0x0050d620
		c.u(8);
		c.u(8);
		c.u(1);
		c.u(16);
		c.u(32);
		c.u(32);
		c.u(8);
		c.deltaPosition();
		c.u(10);
		break;
	case kEvChangeSquadName: // 0x0050d8b0
		c.u(8);
		c.u(8);
		c.u(8);
		c.counted();
		break;
	case kEvSetPrivateSquad: // 0x0050db90
		c.u(8);
		c.u(8);
		c.u(1);
		break;
	case kEvIssueSquadOrder: // 0x0050e220
		c.u(8);
		c.u(8);
		c.u(8);
		if (c.u(1)) {
			c.u(16);
		} else {
			for (int i = 0; i < 9; ++i) {
				c.u(32);
			}
		}
		c.u(8);
		c.u(1);
		break;
	case kEvRank: // 0x0050cc80
		c.upTo(2);
		c.ranged(100); // the value + 1
		c.u(32);
		c.u(8);
		break;
	case kEvSetAcceptOrder: // 0x0050dd90
		c.u(8);
		c.u(8);
		c.u(1);
		c.u(8);
		c.u(8);
		break;
	case kEvSpotted: // 0x0050e770
		c.u(16);
		c.u(16);
		c.u(32);
		c.u(32);
		c.u(8);
		c.u(32);
		c.u(32);
		break;
	case kEvArtillery: { // 0x0050ec60
		const uint32_t kind = c.u(2);
		c.u(32);
		c.u(32);
		c.u(32);
		if (kind == 1) {
			c.u(32);
		} else if (kind == 2) {
			c.u(16);
			c.u(32);
		}
		break;
	}
	case kEvCreateKit: // 0x0050ac80
		c.u(32);
		c.u(16);
		c.vec();
		c.u(4);
		for (int i = 0; i < 4; ++i) {
			c.u(8);
		}
		break;
	case kEvStickyProjectile: // 0x0050f720
		c.u(16);
		if (c.u(16)) {
			c.u(8);
			c.u(8);
		}
		c.u(8);
		for (int i = 0; i < 4; ++i) {
			c.vec();
		}
		break;
	case kEvTitan: { // 0x0050efd0
		const uint32_t kind = c.u(2);
		c.u(32);
		if (kind == 1) {
			c.u(32);
			c.u(32);
		} else if (kind == 2) {
			c.u(16);
		}
		break;
	}
	case kEvAmbientEffectArea: // 0x005100a0
		c.u(16);
		c.u(8);
		c.u(16);
		c.u(32);
		c.u(32);
		c.u(32);
		break;
	case kEvCommanderCam: // 0x00510ae0
		c.vec();
		break;
	case kEvSupplyDrop: // 0x00511110
	case kEvEndOfRound: // 0x00511750
		c.u(32);
		c.u(32);
		c.u(32);
		break;
	case kEvVoipPlayerMute: // 0x00510e10
		c.u(1);
		c.u(8);
		c.u(8);
		break;
	case kEvVote: // 0x005112b0
		for (int i = 0; i < 9; ++i) {
			c.u(8);
		}
		c.u(32);
		break;
	case kEvToggleFreeCamera: // 0x005102e0: no fields
		break;
	case kEvMedal: // 0x005103c0
		c.u(32);
		c.u(8);
		c.u(4);
		c.u(12);
		break;
	case kEvUnlock: // 0x00508b90
		c.upTo(3);
		c.u(8);
		for (int i = 0; i < 2 * 7 * 4; ++i) {
			c.u(8);
		}
		break;
	case kEvMissileInit: // 0x0050f9e0
		c.u(16);
		c.u(8);
		c.u(4);
		c.u(1);
		break;
	case kEvFieldUnlock: { // 0x00510660
		const uint32_t kind = c.upTo(2);
		c.u(8);
		c.u(8);
		if (kind == 1) {
			c.u(8);
			c.u(8);
		}
		break;
	}
	case kEvUav: // 0x0050ea60
		c.u(32);
		c.u(32);
		c.u(8);
		c.u(1);
		break;
	case kEvContentCheck: // 0x00512470
		c.fixed(16);
		c.fixed(16);
		c.fixed(16);
		break;
	case kEvTargetDirection: // 0x0050fd90
		c.u(16);
		c.u(8);
		for (int i = 0; i < 6; ++i) {
			c.u(32);
		}
		break;
	case kEvPythonCommand: { // 0x00511a50
		c.u(32);
		c.u(8);
		const uint32_t count = c.u(4);
		for (uint32_t i = 0; i < count; ++i) {
			c.u(32);
		}
		break;
	}
	case kEvRequest: // 0x00511fb0
		c.u(8);
		c.u(8);
		c.upTo(4);
		break;
	case kEvChangePcoTeam: // 0x00512a20
		c.u(16);
		c.u(2);
		break;
	case kEvSiloSync: // 0x00513f40
		c.u(16);
		c.u(8);
		break;
	case kEvVoipSession: // 0x00512250
		c.u(16);
		break;
	case kEvKickBan: // 0x0050cec0
		c.u(8);
		c.u(1);
		c.u(8);
		break;
	case kEvBeginRound: // 0x00511540
		c.u(32);
		c.u(32);
		c.s(8);
		break;
	case kEvCreateSpawnGroup: // 0x005125c0
		c.u(8);
		c.u(4);
		c.u(1);
		c.u(1);
		c.u(1);
		c.u(8);
		c.u(8);
		c.u(16);
		break;
	case kEvRemoveSpawnGroup: // 0x00512880
		c.u(8);
		c.u(16);
		break;
	case kEvAutoControllerFire: // 0x00512c00
		c.u(16);
		if (!c.u(1)) {
			c.u(1);
			c.u(1);
			c.u(1);
			c.u(1);
		}
		break;
	case kEvSetBeacon: { // 0x00513000
		const uint32_t has = c.u(1);
		c.u(8);
		c.u(8);
		c.u(8);
		if (has) {
			c.u(32);
			c.u(32);
			c.u(32);
		}
		break;
	}
	case kEvUavWarning: // 0x00509b20
		c.u(1);
		break;
	case kEvUpdateKit: // 0x0050ad50: value + 1
		c.u(4);
		break;
	case kEvSelectKit: // 0x0050ae90
		c.u(4);
		for (int i = 0; i < 4; ++i) {
			c.u(8);
		}
		c.u(1);
		break;
	case kEvReloadUpdate: // 0x00513410
		c.u(16);
		c.u(32);
		break;
	case kEvSquadRadar: { // 0x00508800
		c.ranged(1);
		c.ranged(0xff);
		const uint32_t count = c.ranged(0x40);
		c.ranged(0x3c);
		for (uint32_t i = 0; i < count; ++i) {
			c.upTo(0x100);
		}
		break;
	}
	case kEvRefreshUnlocks: // 0x00513560
		c.u(32);
		break;
	case kEvCommanderTool: // 0x005136f0
		c.u(32);
		c.u(32);
		break;
	case kEvMissileSiloFire: // 0x00513910
		c.u(16);
		c.u(16);
		break;
	case kEvCommanderToolsUpdate: // 0x00513b10
		for (int i = 0; i < 6; ++i) {
			c.u(32);
		}
		break;
	case kEvEntitlement: // 0x00513d80
		c.u(8);
		c.u(4);
		break;
	case kEvJoinAsSpectator: // 0x00514100
		c.u(8);
		c.u(1);
		c.u(8);
		c.u(8);
		break;
	default:
		return false;
	}
	return c.ok();
}

float rawFloat(uint32_t raw)
{
	float value;
	std::memcpy(&value, &raw, 4);
	return value;
}

uint32_t floatRaw(float value)
{
	uint32_t raw;
	std::memcpy(&raw, &value, 4);
	return raw;
}

void pushVec(GameEvent& e, const Vec3f& v)
{
	e.values.push_back(floatRaw(v.x));
	e.values.push_back(floatRaw(v.y));
	e.values.push_back(floatRaw(v.z));
}

std::string trimmed(const std::string& text)
{
	return text.substr(0, std::min(text.find('\0'), text.size()));
}

} // namespace

const char* gameEventName(uint32_t type) { return type < kEventTypeCount ? kEventNames[type] : nullptr; }

float GameEvent::f(size_t n) const { return rawFloat(u(n)); }

const std::string& GameEvent::text(size_t n) const
{
	static const std::string empty;
	return n < bytes.size() ? bytes[n] : empty;
}

bool readGameEvent(BitReader& in, GameEvent& event)
{
	event = {};
	event.type = in.read(bitsFor(kEventTypeCount));
	if (in.bad() || !gameEventName(event.type)) {
		return false;
	}
	Codec codec(in, event);
	return codeFields(codec, event.type);
}

bool writeGameEvent(BitWriter& out, const GameEvent& event)
{
	if (!gameEventName(event.type)) {
		return false;
	}
	out.write(event.type, bitsFor(kEventTypeCount));
	Codec codec(out, event);
	return codeFields(codec, event.type);
}

void writeEventBlock(BitWriter& out, const ReliableEventBlock* block)
{
	if (!block || block->events.empty()) {
		out.write(0, 1);
		return;
	}
	out.write(1, 1);
	out.write(static_cast<uint32_t>(std::min<size_t>(block->events.size(), 255)), 8);
	out.write(block->sequence & 0x1f, 5);
	out.write(block->resent ? 1 : 0, 1);
	for (size_t i = 0; i < block->events.size() && i < 255; ++i) {
		writeGameEvent(out, block->events[i]);
	}
}

bool readEventBlock(BitReader& in, bool& present, ReliableEventBlock& block)
{
	block = {};
	present = in.read(1) == 1;
	if (!present) {
		return !in.bad();
	}
	const uint32_t count = in.read(8);
	block.sequence = in.read(5);
	block.resent = in.read(1) == 1;
	for (uint32_t i = 0; i < count; ++i) {
		GameEvent event;
		if (!readGameEvent(in, event)) {
			return false;
		}
		block.events.push_back(std::move(event));
	}
	return !in.bad();
}

// --- typed views

bool parseCreatePlayer(const GameEvent& e, CreatePlayerInfo& out)
{
	if (e.type != kEvCreatePlayer || e.values.size() < 10) {
		return false;
	}
	out.team = e.i(0);
	out.spawnGroup = e.i(1);
	out.remote = e.u(2) != 0;
	out.id = e.i(3);
	out.object = static_cast<uint16_t>(e.u(4));
	out.value16 = static_cast<uint16_t>(e.u(5));
	out.ai = e.u(6) != 0;
	out.name = trimmed(e.text(0));
	out.profileId = e.i(7);
	out.flags = e.i(8);
	out.flag2 = e.u(9) != 0;
	return true;
}

GameEvent makeCreatePlayer(const CreatePlayerInfo& info)
{
	GameEvent e;
	e.type = kEvCreatePlayer;
	e.values = {static_cast<uint32_t>(info.team), static_cast<uint32_t>(info.spawnGroup), info.remote ? 1u : 0u,
		static_cast<uint32_t>(info.id), info.object, info.value16, info.ai ? 1u : 0u,
		static_cast<uint32_t>(info.profileId), static_cast<uint32_t>(info.flags), info.flag2 ? 1u : 0u};
	e.bytes = {info.name};
	return e;
}

bool parseCreateObject(const GameEvent& e, CreateObjectInfo& out)
{
	if (e.type != kEvCreateObject || e.values.size() < 5) {
		return false;
	}
	out = {};
	out.templateId = e.u(0);
	out.ghost = static_cast<uint16_t>(e.u(1));
	out.team = e.i(2);
	size_t n = 3;
	if (e.u(n++)) {
		out.count = e.u(n++);
		return true;
	}
	out.count = 1;
	out.hasPosition = e.u(n++) != 0;
	if (out.hasPosition) {
		out.position = e.vec(n);
		n += 3;
	}
	out.hasRotation = e.u(n++) != 0;
	if (out.hasRotation) {
		out.rotation = e.vec(n);
	}
	return true;
}

GameEvent makeCreateObject(const CreateObjectInfo& info)
{
	GameEvent e;
	e.type = kEvCreateObject;
	e.values = {info.templateId, info.ghost, static_cast<uint32_t>(info.team)};
	if (info.count > 1) {
		e.values.push_back(1);
		e.values.push_back(info.count);
		return e;
	}
	e.values.push_back(0);
	e.values.push_back(info.hasPosition ? 1 : 0);
	if (info.hasPosition) {
		pushVec(e, info.position);
	}
	e.values.push_back(info.hasRotation ? 1 : 0);
	if (info.hasRotation) {
		pushVec(e, info.rotation);
	}
	return e;
}

bool parseCreateKit(const GameEvent& e, CreateKitInfo& out)
{
	if (e.type != kEvCreateKit || e.values.size() < 10) {
		return false;
	}
	out.templateId = e.u(0);
	out.ghost = static_cast<uint16_t>(e.u(1));
	out.position = e.vec(2);
	out.value4 = e.u(5);
	for (size_t i = 0; i < 4; ++i) {
		out.items[i] = e.i(6 + i) - 1; // sent as value + 1
	}
	return true;
}

int32_t PostRemoteInfo::payloadInt(size_t index) const
{
	if (payload.size() < (index + 1) * 4) {
		return 0;
	}
	int32_t v;
	std::memcpy(&v, payload.data() + index * 4, 4);
	return v;
}

bool parsePostRemote(const GameEvent& e, PostRemoteInfo& out)
{
	if (e.type != kEvPostRemote || e.values.size() < 3) {
		return false;
	}
	out.category = e.u(0);
	out.id = e.u(1);
	out.delay = e.f(2);
	out.payload = e.text(0);
	return true;
}

GameEvent makePostRemote(uint32_t category, uint32_t id, const std::string& payload, float delay)
{
	GameEvent e;
	e.type = kEvPostRemote;
	e.values = {category, id, floatRaw(delay)};
	e.bytes = {payload.substr(0, 52)};
	return e;
}

std::string int32Payload(int32_t value)
{
	std::string bytes(4, '\0');
	std::memcpy(bytes.data(), &value, 4);
	return bytes;
}

bool parseDataBlock(const GameEvent& e, DataBlockChunk& out)
{
	if (e.type != kEvDataBlock || e.values.empty()) {
		return false;
	}
	out = {};
	out.start = e.u(0) != 0;
	if (out.start) {
		out.type = e.u(1);
		out.size = e.u(2);
	} else {
		out.bytes = e.text(0);
	}
	return true;
}

bool parseStringBlock(const GameEvent& e, DataBlockChunk& out)
{
	if (e.type != kEvStringBlock || e.values.empty()) {
		return false;
	}
	out = {};
	out.start = e.u(0) != 0;
	if (out.start) {
		out.size = e.u(1);
	} else {
		out.bytes = e.text(0);
	}
	return true;
}

GameEvent makeDataBlockStart(uint32_t type, uint32_t size)
{
	GameEvent e;
	e.type = kEvDataBlock;
	e.values = {1, type, size};
	return e;
}

GameEvent makeDataBlockChunk(const std::string& bytes)
{
	GameEvent e;
	e.type = kEvDataBlock;
	e.values = {0};
	e.bytes = {bytes};
	return e;
}

std::string encodeChat(const ChatMessage& message)
{
	std::string block = message.text.substr(0, 255 - 12);
	char header[12] {};
	std::memcpy(header, &message.channel, 4);
	std::memcpy(header + 4, &message.source, 4);
	header[8] = static_cast<char>(message.player < 0 ? 0 : message.player);
	header[9] = static_cast<char>(message.b9);
	header[10] = static_cast<char>(message.b10);
	block.append(header, 12);
	return block;
}

bool decodeChat(const std::string& block, ChatMessage& out)
{
	if (block.size() < 12) {
		return false;
	}
	const char* header = block.data() + block.size() - 12;
	std::memcpy(&out.channel, header, 4);
	std::memcpy(&out.source, header + 4, 4);
	out.player = static_cast<uint8_t>(header[8]);
	out.b9 = static_cast<uint8_t>(header[9]);
	out.b10 = static_cast<uint8_t>(header[10]);
	out.text = trimmed(block.substr(0, block.size() - 12));
	return true;
}

std::vector<GameEvent> stringBlockEvents(const std::string& block)
{
	std::vector<GameEvent> events;
	GameEvent start;
	start.type = kEvStringBlock;
	start.values = {1, static_cast<uint32_t>(block.size() & 0xff)};
	events.push_back(start);
	for (size_t at = 0; at < block.size(); at += 63) {
		GameEvent chunk;
		chunk.type = kEvStringBlock;
		chunk.values = {0};
		chunk.bytes = {block.substr(at, 63)};
		events.push_back(chunk);
	}
	return events;
}

bool parseKilledBy(const GameEvent& e, KilledByInfo& out)
{
	if (e.type != kEvKilledBy || e.values.size() < 12) {
		return false;
	}
	out.attacker = e.i(0);
	out.victim = e.i(1);
	out.hasAttacker = e.u(2) != 0;
	out.object = static_cast<uint16_t>(e.u(3));
	out.value1 = e.u(4);
	out.value2 = e.u(5);
	out.armorValue = e.i(6);
	const uint32_t mode = e.u(7);
	if (mode == 0) {
		out.position = e.vec(8);
	} else {
		// Scaled by 1000 from the stream's reference (0x006876c0 is passed
		// 1000.0). The writer (0x0050d5b0) delta-codes only while the
		// stream has a reference, and 0x0057b040 resets the stream to the
		// origin without one (0x00687460) before each packet's events; the
		// ghost block sets it later. So servers send raw floats, and a
		// delta would be from the origin.
		out.position = {e.i(8) * 1000.0f, e.i(9) * 1000.0f, e.i(10) * 1000.0f};
	}
	out.damage = static_cast<float>(e.u(11)) * 0.0009775171f * 100.0f;
	return true;
}

bool parsePythonCommand(const GameEvent& e, PythonCommandInfo& out)
{
	if (e.type != kEvPythonCommand || e.values.size() < 3) {
		return false;
	}
	out.type = e.i(0);
	out.value8 = e.u(1);
	out.args.clear();
	for (size_t i = 3; i < e.values.size(); ++i) {
		out.args.push_back(e.i(i));
	}
	return true;
}

GameEvent makeSelectKit(int kit, const std::array<int32_t, 4>& unlocks, bool flag)
{
	GameEvent e;
	e.type = kEvSelectKit;
	e.values.push_back(static_cast<uint32_t>(kit) & 0xf);
	for (int32_t unlock : unlocks) {
		e.values.push_back(static_cast<uint32_t>(unlock + 1) & 0xff); // read back as value - 1
	}
	e.values.push_back(flag ? 1 : 0);
	return e;
}

GameEvent makeChallengeResponse(const std::string& response, uint32_t mixed, uint32_t build, uint32_t profileId)
{
	GameEvent e;
	e.type = kEvChallengeResponse;
	e.bytes = {response};
	e.values = {mixed, build, profileId};
	return e;
}

GameEvent makeConnectionType(uint32_t type)
{
	GameEvent e;
	e.type = kEvConnectionType;
	e.values = {type};
	return e;
}

// ---------------------------------------------------------------------------
// Ghosts

namespace {

void copyBits(BitReader& in, size_t bits, std::vector<uint8_t>& out)
{
	BitWriter w;
	while (bits > 0 && !in.bad()) {
		const unsigned n = bits > 32 ? 32 : static_cast<unsigned>(bits);
		w.write(in.read(n), n);
		bits -= n;
	}
	out = w.data();
}

void writeBits(BitWriter& out, const std::vector<uint8_t>& data, size_t bits)
{
	for (size_t i = 0; i < bits; ++i) {
		out.write(i / 8 < data.size() ? data[i / 8] >> (i % 8) & 1u : 0u, 1);
	}
}

} // namespace

bool readGhostBlock(BitReader& in, GhostBlock& block, unsigned lengthBits)
{
	block = {};
	block.present = in.read(1) == 1;
	if (!block.present) {
		return !in.bad();
	}
	block.tick = in.read(32);
	const uint32_t count = in.read(8);
	const bool control = in.read(1) == 1;
	if (control) {
		ControlObjectRecord record;
		const uint32_t length = in.read(12);
		const size_t start = in.position();
		record.lastMove = in.readSigned(32);
		record.position.x = in.readFloat();
		record.position.y = in.readFloat();
		record.position.z = in.readFloat();
		record.ghost = static_cast<uint16_t>(in.read(16));
		if (in.read(1) == 1) {
			record.second = static_cast<uint16_t>(in.read(16));
		}
		if (in.read(1) == 1 && in.read(1) == 1) {
			record.vehicle = static_cast<uint16_t>(in.read(16));
		}
		const size_t used = in.position() - start;
		if (in.bad() || used > length) {
			return false;
		}
		record.restBits = length - used;
		copyBits(in, record.restBits, record.rest);
		block.control = record;
	}
	for (uint32_t i = 0; i < count && !in.bad(); ++i) {
		GhostUpdate update;
		update.kind = in.read(2);
		update.ghost = static_cast<uint16_t>(in.read(16));
		if (update.kind == 1) {
			update.full = in.read(1) == 1;
			update.stateBits = in.read(lengthBits);
			copyBits(in, update.stateBits, update.state);
		} else if (update.kind == 2) {
			return false; // the engine flags the connection (0x0057fce0)
		}
		block.updates.push_back(std::move(update));
	}
	return !in.bad();
}

void writeGhostBlock(BitWriter& out, const GhostBlock& block, unsigned lengthBits)
{
	if (!block.present) {
		out.write(0, 1);
		return;
	}
	out.write(1, 1);
	out.write(block.tick, 32);
	out.write(static_cast<uint32_t>(std::min<size_t>(block.updates.size(), 255)), 8);
	out.write(block.control ? 1 : 0, 1);
	if (block.control) {
		const ControlObjectRecord& r = *block.control;
		BitWriter body;
		body.writeSigned(r.lastMove, 32);
		body.writeFloat(r.position.x);
		body.writeFloat(r.position.y);
		body.writeFloat(r.position.z);
		body.write(r.ghost, 16);
		body.write(r.second ? 1 : 0, 1);
		if (r.second) {
			body.write(r.second, 16);
		}
		body.write(r.vehicle ? 1 : 0, 1);
		if (r.vehicle) {
			body.write(1, 1);
			body.write(r.vehicle, 16);
		}
		writeBits(body, r.rest, r.restBits);
		out.write(static_cast<uint32_t>(body.bits()), 12);
		out.append(body);
	}
	for (size_t i = 0; i < block.updates.size() && i < 255; ++i) {
		const GhostUpdate& u = block.updates[i];
		out.write(u.kind, 2);
		out.write(u.ghost, 16);
		if (u.kind == 1) {
			out.write(u.full ? 1 : 0, 1);
			out.write(static_cast<uint32_t>(u.stateBits), lengthBits);
			writeBits(out, u.state, u.stateBits);
		}
	}
}

// ---------------------------------------------------------------------------
// Payloads

bool readGamePayload(const std::vector<uint8_t>& payload, GamePayload& out, unsigned lengthBits)
{
	out = {};
	BitReader in(payload.data(), payload.size());
	if (!readInputBlock(in, out.input)) {
		out.complete = false;
		return false;
	}
	if (!readEventBlock(in, out.hasEvents, out.events)) {
		out.complete = false;
		return true; // the events before the undecoded one are usable
	}
	if (!readGhostBlock(in, out.ghosts, lengthBits)) {
		out.complete = false;
	}
	return true;
}

std::vector<uint8_t> writeGamePayload(const GamePayload& payload, unsigned lengthBits)
{
	BitWriter out;
	writeInputBlock(out, payload.input);
	writeEventBlock(out, payload.hasEvents ? &payload.events : nullptr);
	writeGhostBlock(out, payload.ghosts, lengthBits);
	return out.data();
}

} // namespace dice::hfe::net
