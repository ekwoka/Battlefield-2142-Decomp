// What a game data packet carries once connected (BF2142.exe 1.51). A data
// payload (the bytes after the 16-bit length of a type 15 packet) is read by
// the connection's three subsystems in order (ServerConnection 0x0057b3f0,
// read loop 0x0057a170):
//
//   1. the input block (0x00590990 reads, 0x00590740 writes): the player's
//      moves, sent by clients; servers write a single 0 bit;
//   2. the event block (0x006447b0 reads, 0x00644bf0 writes): reliable,
//      ordered events;
//   3. the ghost block (0x00581290 reads): object replication, sent by
//      servers; clients write a single 0 bit.
//
// Everything is a bit stream (BitStream.h). Fields whose meaning could not
// be found offline say so where they are declared.
#pragma once

#include "BitStream.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::net {

// ---------------------------------------------------------------------------
// Player input

// The player inputs (c_PI... console constants, registered at 0x0066xxxx in
// the constant table built near "c_PIYaw"; PlayerInput holds 64 floats).
enum PlayerInputId : int {
	kPIYaw = 0,
	kPIPitch = 1,
	kPIRoll = 2,
	kPIThrottle = 3,
	kPIMouseLookX = 4,
	kPIMouseLookY = 5,
	kPICameraX = 6,
	kPICameraY = 7,
	kPIFire = 8,
	kPIAction = 9,
	kPIUse = 10,
	kPIMouseLook = 11,
	kPIAltSprint = 12,
	kPISprint = 13,
	kPIWeaponSelect1 = 14, // ..9 = 22
	kPIPositionSelect1 = 23, // ..8 = 30
	kPIAltFire = 31,
	kPIReload = 32,
	kPISelectFunc = 33,
	kPIDrop = 34,
	kPIToggleCameraMode = 35,
	kPIToggleCamera = 36,
	kPILie = 37,
	kPICrouch = 38,
	kPICameraMode1 = 39, // ..4 = 42
	kPISelectPrimWeapon = 43,
	kPISelectSecWeapon = 44,
	kPIToggleWeapon = 45,
	kPIToggleFireRate = 46,
	kPIFlareFire = 47,
	kPINextItem = 48,
	kPIPrevItem = 49,
	kPICommunication = 62,
	kPIShowScoreBoard = 63,
	kPINumInputs = 64,
};

// One frame of input as a client sends it (0x00583b50 builds it from the
// player's 64 input floats; 0x00590570 reads, 0x00590650 writes):
//   6 x (sign bit + 15 bits): inputs 0-5 (yaw, pitch, roll, throttle,
//                              mouse look x/y) times 100, clamped to +-32768;
//   32 bits + 11 bits: trigger bits, bit (id - 8) for the inputs in the table
//                      at 0x009653a8 that are above 0.5 (ids 8-50; the
//                      high part keeps only 11 bits);
//   1 bit: PlayerInput +0x110: pitch and roll come from a device axis.
//          The server puts it back into the player's PlayerInput
//          (0x005838d0) with the rest of the move; no code in BF2142.exe
//          reads PlayerInput +0x110 after that (every byte read at +0x110
//          in the binary was checked: the others are other classes'), so
//          it has no effect on the server.
//          0x00696db0 fills PlayerInput from a control map: the bit starts
//          at 1 and, for inputs 1 and 2 when the map binds them (ControlMap
//          slot 0x20), is ANDed with slot 0x6c (0x00681fc0: the mapping's
//          +0xb0, set by 0x00680dc0 when the active binding is an
//          axis-to-axis one, kind 1). So 1 on foot (unbound) and with the
//          mouse (Land, Air, Helicopter maps); 0 when keys drive them (the
//          Sea map's arrow keys).
struct PlayerMove {
	int32_t index = 0; // counts up per frame (0x005836b0); not sent per move
	std::array<int16_t, 6> axes {};
	uint32_t triggersLow = 0;  // inputs 8..39
	uint32_t triggersHigh = 0; // inputs 40..50 (11 bits)
	bool axisPitchRoll = true;

	float axis(int input) const { return axes[static_cast<size_t>(input)] * 0.01f; }
	bool trigger(int input) const;
	void setAxis(int input, float value);
	void setTrigger(int input, bool on);
};

// The input ids the engine packs into a move's trigger bits (0x009653a8).
const std::vector<int>& movedTriggerInputs();

// A player's 64 input values (the engine's PlayerInput floats).
struct PlayerInput {
	std::array<float, kPINumInputs> values {};
	bool axisPitchRoll = true; // see PlayerMove::axisPitchRoll

	void set(int input, float value) { values[static_cast<size_t>(input)] = value; }
	float get(int input) const { return values[static_cast<size_t>(input)]; }
	PlayerMove toMove(int32_t index) const;
};

// The input block: 1 bit present; 4 bits move count; 9 bits the client's
// view delay in ms for lag compensation (0x0057b040: the networkable
// manager's +0x50, 200 ms from the rate table, plus 1000 x the interpolation time of 0x006b4e70; the
// server's player vtable slot 100 keeps it at +0xf4, capped at 512); if there are moves, the
// first move's index (sign + 31 bits) and the moves. Clients keep and resend
// their last three moves (0x00590450).
struct InputBlock {
	bool present = false;
	uint32_t value9 = 0;
	std::vector<PlayerMove> moves; // consecutive indices
};
void writeInputBlock(BitWriter& out, const InputBlock& block);
bool readInputBlock(BitReader& in, InputBlock& block);

// ---------------------------------------------------------------------------
// Events

struct Vec3f {
	float x = 0, y = 0, z = 0;
};

// Event types (the registry at 0x00a3ba44 is filled by 0x0042cxxx; the
// names come from the table at 0x00420000.c:15306, "ChallengeEvent"...).
// A type is written in 7 bits (enough for the 0x48 registered).
enum GameEventType : uint32_t {
	kEvChallenge = 0x01,          // s->c CD-key challenge
	kEvChallengeResponse = 0x02,  // c->s
	kEvConnectionType = 0x03,     // c->s
	kEvDataBlock = 0x04,          // s->c server settings, map list
	kEvCreatePlayer = 0x05,       // s->c
	kEvCreateObject = 0x06,       // s->c a ghost's object
	kEvDestroyObject = 0x07,      // s->c
	kEvDestroyPlayer = 0x08,      // s->c
	kEvEnterVehicle = 0x09,       // s->c
	kEvExitVehicle = 0x0a,        // s->c
	kEvPostRemote = 0x0b,         // both: a message for the other side's event manager
	kEvChangePlayerName = 0x0c,   // both
	kEvHandleDrop = 0x0d,         // s->c ("HandleDropEvent", registered by 0x0042d980)
	kEvHandlePickup = 0x0e,       // s->c
	kEvStringBlock = 0x0f,        // both: chat text in chunks
	kEvJoinSquad = 0x10,          // c->s
	kEvLeaveSquad = 0x11,         // c->s
	kEvCommander = 0x13,          // both
	kEvRadioMessage = 0x14,       // both
	kEvKilledBy = 0x15,           // s->c
	kEvChangeSquadName = 0x16,
	kEvSetPrivateSquad = 0x17,
	kEvIssueSquadOrder = 0x18,
	kEvInvite = 0x19,
	kEvRank = 0x1a,
	kEvSetAcceptOrder = 0x1b,
	kEvSetSquadLeader = 0x1c,
	kEvSpotted = 0x1d,
	kEvArtillery = 0x1e,
	kEvCreateKit = 0x1f,          // s->c a dropped kit's object
	kEvStickyProjectile = 0x20,
	kEvTitan = 0x21,
	kEvAmbientEffectArea = 0x22,
	kEvVoipOnOff = 0x23,
	kEvCommanderCam = 0x24,
	kEvSupplyDrop = 0x25,
	kEvVoipPlayerMute = 0x26,
	kEvVote = 0x27,
	kEvToggleFreeCamera = 0x28,
	kEvMedal = 0x29,
	kEvUnlock = 0x2a,
	kEvMissileInit = 0x2b,
	kEvFieldUnlock = 0x2c,
	kEvUav = 0x2d,
	kEvContentCheck = 0x2e,
	kEvTargetDirection = 0x2f,
	kEvEndOfRound = 0x30,
	kEvPythonCommand = 0x31,
	kEvRequest = 0x32,
	kEvChangePcoTeam = 0x33,
	kEvSiloSync = 0x34,
	kEvVoipSession = 0x36,
	kEvKickBan = 0x37,
	kEvBeginRound = 0x38,
	kEvCreateSpawnGroup = 0x39,
	kEvRemoveSpawnGroup = 0x3a,
	kEvAutoControllerFire = 0x3b,
	kEvSetBeacon = 0x3c,
	kEvUavWarning = 0x3d,
	kEvUpdateKit = 0x3e,
	kEvSelectKit = 0x3f,
	kEvReloadUpdate = 0x40,
	kEvSquadRadar = 0x41,
	kEvRefreshUnlocks = 0x42,
	kEvCommanderTool = 0x43,
	kEvMissileSiloFire = 0x44,
	kEvCommanderToolsUpdate = 0x45,
	kEvEntitlement = 0x46,
	kEvJoinAsSpectator = 0x47,
	kEventTypeCount = 0x48,
};
const char* gameEventName(uint32_t type); // nullptr for unregistered ids (0, 0x12, 0x35)

// An event as on the wire: every field in wire order. Integers (and the raw
// bits of floats) go to `values`, signed fields as int32 bit patterns;
// fixed and length-prefixed byte fields go to `bytes`. Each type's layout is
// its reader's (the event's vtable slot 10; slot 11 writes, slot 8 handles
// it on a client, slot 9 on a server), listed in GameProtocol.cpp.
struct GameEvent {
	uint32_t type = 0;
	std::vector<uint32_t> values;
	std::vector<std::string> bytes;

	int32_t i(size_t n) const { return n < values.size() ? static_cast<int32_t>(values[n]) : 0; }
	uint32_t u(size_t n) const { return n < values.size() ? values[n] : 0; }
	float f(size_t n) const;
	Vec3f vec(size_t n) const { return {f(n), f(n + 1), f(n + 2)}; }
	const std::string& text(size_t n = 0) const;
};

// Reads one event (its 7-bit type, then its fields). False for a type the
// engine doesn't register or a short stream.
bool readGameEvent(BitReader& in, GameEvent& event);
// Writes one; false when `values`/`bytes` don't fit the type's layout.
bool writeGameEvent(BitWriter& out, const GameEvent& event);

// The reliable event block: 1 bit present; 8 bits count; 5 bits block
// sequence (handled in order, modulo 32, 0x006447b0); 1 bit set when the
// block is sent again after a loss (0x00644bf0 / 0x00509a90); the events.
struct ReliableEventBlock {
	uint32_t sequence = 0;
	bool resent = false;
	std::vector<GameEvent> events;
};
void writeEventBlock(BitWriter& out, const ReliableEventBlock* block); // nullptr: one 0 bit
// `present` false when the block's bit is 0. False when an event can't be
// decoded: `block.events` then holds those before it and the rest of the
// payload (the ghost block) can't be read.
bool readEventBlock(BitReader& in, bool& present, ReliableEventBlock& block);

// Typed views and builders of the events a client needs.
struct CreatePlayerInfo { // kEvCreatePlayer (0x0050a4c0, handled 0x0050a8c0)
	int team = 0;          // 3 bits (player vtable +0xf4)
	int spawnGroup = 0;    // 4 bits: player vtable 0x18c (+0xe8; the client handler ignores it)
	bool remote = false;   // 0: the receiving client's own player
	int id = 0;            // 8 bits
	uint16_t object = 0;   // 16 bits: the ghost id of the player's networkable (interface 0x58165ffe, registered by 0x0067d3d0)
	uint16_t value16 = 0;  // 16 bits (always 0 from the builder 0x0050a7c0)
	bool ai = false;       // a bot: the builder reads player vtable 0x68 (+0xd4); the handler sets it with 0x64 on remote players
	std::string name;      // 32 bytes
	int32_t profileId = 0; // sign + 31 bits (player vtable +0x328)
	int32_t flags = 0;     // sign + 3 bits: bit 0 vtable +0x3b0, bit 2 +0x3a8
	bool flag2 = false;    // player vtable +0x3b8
};
bool parseCreatePlayer(const GameEvent& e, CreatePlayerInfo& out);
GameEvent makeCreatePlayer(const CreatePlayerInfo& info);

struct CreateObjectInfo { // kEvCreateObject (0x0050b150, built 0x0050b0f0, handled 0x0050b450)
	uint32_t templateId = 0; // ObjectTemplate +0x44: creation index of network-shared templates (0x0070a700)
	uint16_t ghost = 0;
	int team = 0;            // 2 bits
	uint32_t count = 1;      // >1: that many ghosts from `ghost` on, at the origin
	bool hasPosition = false, hasRotation = false;
	Vec3f position;
	Vec3f rotation;          // degrees, yaw/pitch/roll: 0x004ab8a0 builds the matrix with 0x006adb30(-z, -y, -x), which
	                         // takes degrees (x pi/180, 0x00a065c8); the late-join replay makes it with 0x00568c30 -> 0x006ad2a0
};
bool parseCreateObject(const GameEvent& e, CreateObjectInfo& out);
GameEvent makeCreateObject(const CreateObjectInfo& info);

struct CreateKitInfo { // kEvCreateKit (0x0050ac80, built 0x0050ac20)
	uint32_t templateId = 0;
	uint16_t ghost = 0;
	Vec3f position;
	uint32_t value4 = 0;
	std::array<int32_t, 4> items {};
};
bool parseCreateKit(const GameEvent& e, CreateKitInfo& out);

// kEvIssueSquadOrder (0x18; built 0x0050e170, written 0x0050dfe0, read
// 0x0050e220): a squad's order or request: the squad (+0xd), the team
// (+0xc), the kind (+0xe; an order's type, or a request's 0 artillery, 1
// orbital strike, 2 UAV, 3 supplies), then 1 bit: the order's object ghost
// (16 bits), else 9 floats (the record's +8..+0x28, written +0x10, +0x1c,
// +0x28, +0x14, +0x20, +0x2c, +0x18, +0x24, +0x30: the point is +0x10,
// +0x14, +0x18), which store (8 bits, +0x36: 0 the squad's order, squad
// manager slot 0x48; 1 the commander's, slot 0x4c; 2 a request, slot 0x50)
// and a bit (+0x37, the record's +0x2c). A client's squad manager
// (ClientSquadManager 0x004c9b00 for a request) sends it to the server,
// whose handler (0x0050e3a0) takes it from the squad's leader (an order or
// a request) or the team's commander (his order or a request) and stores
// it with the broadcast (0x004c5720: the event to everyone, 2/0x1e to the
// leader, 2/0x1d to the commander); a client's handler (0x0050e4d0) only
// stores it.
struct SquadOrderInfo {
	int squad = 0, team = 0, kind = 0;
	uint16_t object = 0; // 0: placed at `point`
	std::array<float, 9> floats {}; // the record's +8..+0x28 (point first)
	int store = 0;       // 0 order, 1 commander order, 2 request
	bool flag = false;
	Vec3f point() const { return {floats[0], floats[1], floats[2]}; }
};
constexpr int kSquadOrderStoreRequest = 2;
bool parseSquadOrder(const GameEvent& e, SquadOrderInfo& out);
GameEvent makeSquadOrder(const SquadOrderInfo& info);
// kEvSetAcceptOrder (0x1b; built 0x0050dd50, read 0x0050dd90, written
// 0x0050de40): the squad (+0xd), the team (+0xc), a bit (+0xe), the order's
// store (+0xf: 2 a request) and its kind (+0x10). Squad manager slot 0x24:
// the commander's answer to a request (0x007c7e20 passes 0 for the bit, 2
// and the request's kind) takes it off. A client's (0x004c99a0) goes to the
// server, whose handler (0x0050df60) takes it from the squad's leader or
// the commander and passes it on (0x004c3300: the record's 0x004c2f60, the
// event to everyone, the commander's request tag off); a client's
// (0x0050df20) only applies it.
struct AcceptOrderInfo {
	int squad = 0, team = 0;
	bool flag = false;
	int store = 0, kind = 0;
};
bool parseAcceptOrder(const GameEvent& e, AcceptOrderInfo& out);
GameEvent makeAcceptOrder(const AcceptOrderInfo& info);
// kEvSpotted (0x1d; read 0x0050e770, written 0x0050e830, the event's
// vtable 0x0091e6f0 made by 0x0042cfa0): +0xc the reporter's soldier
// (its network id, 16 bits), +0xe the object reported (16), +0x14 / +0x18
// two floats (the point; 0 from the radio calls), +0x10 the team (8),
// +0x1c a value (32), +0x20 the message (32).
//   - The game's slot 0xd8 (0x0042d220, Radio.callForMedic / Ammo /
//     Repair and the revive call): message 0x33..0x36, the caller's
//     soldier and team; a client sends it, a server hands it out itself
//     (slot 0x90(-1)). No spam check; the radio line goes as its own
//     message (slot 0x54).
//   - The game's slot 0xd4 (0x0042d110, the spot of Radio.sendSpottedMessage
//     and AIDLL's IAIGame slot 0x68): message 0x2e with the object, the
//     point and the value, after the reporter's spam check (player slot
//     0x340, counted for a player other than the local one).
//   - The server's handler (0x0050e910): the object reported (by network
//     id, with its +0x40) goes to the world's category 3 message 0x16 with
//     the team; then every player of the event's team gets message 2/<the
//     message> with the event's 0x18 bytes from +0xc: a server's own player
//     posted there, the others' as PostRemote to their connection.
struct SpottedInfo {
	uint32_t spotter = 0; // network id (a ghost)
	uint32_t spotted = 0;
	float x = 0, z = 0;
	int team = 0;
	uint32_t value = 0;
	uint32_t message = 0;
};
bool parseSpotted(const GameEvent& e, SpottedInfo& out);
GameEvent makeSpotted(const SpottedInfo& info);
// The 0x18 bytes message 2/<message> carries (from the event's +0xc:
// spotter u16, spotted u16, team u8 and three bytes, x, z, value).
std::string spottedPayload(const SpottedInfo& info);
bool parseSpottedPayload(const std::string& payload, uint32_t message, SpottedInfo& out);
// kEvRadioMessage (0x14; read 0x0050d100, written 0x0050d1f0): +0xc the
// voice message's index in the game logic's message table (16 bits: the
// order of Sound/VoiceMessages.con's addMessage, 0x004d8af0, looked up by
// name with 0x004d8040 at game logic +0x170), +0xe the sender's player id
// (8), +0xf its team (2), +0x10 the team, squad or player the channel
// names (8), +0x14 the target channel (0..0x13) and +0x18 the source
// (0..4: 3 a player's, 0 the game's own).
//   - A player's (the client's slot 0x50, 0x004b2b60 → 0x004b26b0): the
//     index, the player's id and team (slots 0x58, 0xf8), the value, the
//     channel, 3; after the spam check (player slot 0x340: "spam" and
//     HUD_HUD_MENU_CHAT_SPAM_MESSAGE) it goes to the server and the sender
//     hears his own at once (0x0042e0c0 with source 3).
//   - The server's handler (0x0050d3b0) makes the message with the
//     connection's player as its sender (0x004a54d0: +0x28) and hands it
//     out (slot 0xa8 → 0x004a7800 for each player, 0x004a6f70 who hears);
//     each hearer's client gets the event again from it (0x004a6c60: the
//     sender +0x28 into +0xe, the rest as they came).
//   - A client's handler (0x0050d300) posts it to the event manager
//     (category 6) for the HUD and the voice (0x0042e0c0: source 2 or 3
//     with a sender; 0 none, the commander's voice).
struct RadioMessageInfo {
	int index = 0;
	int sender = 0xff; // 0xff: no player
	int team = 0;
	int value = 0;
	int channel = 0;
	int source = 0;
};
bool parseRadioMessage(const GameEvent& e, RadioMessageInfo& out);
GameEvent makeRadioMessage(const RadioMessageInfo& info);
// The commander's tools as a client sends them to the server (the HUD's
// uses, 0x007d88d0..0x007d8660: on a server the event manager's slot 0x18
// takes the event at once, on a client the game client's slot 0x90 sends
// it):
//   - kEvArtillery (0x1e; vtable 0x0091e42c, built 0x007d7c20, read
//     0x0050ec60, server 0x0050ee10): kind 1 (an RC object's use; 2 bits),
//     the team (32 bits, a byte), x and z (floats), then the rcType (32: 1
//     RCArtillery, 4 RCOrbitalStrike); the server takes it from the team's
//     commander or no player (0x0050ee10: slot 0x128) and fires the team's
//     asset (0x005a11c0, 0x005a1270);
//   - kEvUav (0x2d; vtable 0x0091e3fc, built 0x007d7ca0, read 0x0050ea60,
//     server 0x0050eb90): x, z (floats), the team (8), a bit set for the
//     satellite scan (else the UAV), from the team's commander (game logic
//     slot 0x78): events 0/0x11 (the UAV at the point) or 0/0x12 (the scan);
//   - kEvSupplyDrop (0x25; vtable 0x0091e54c, built 0x0043d6d0 on a client,
//     read 0x00511110, server 0x00511220): the team (32 bits, a byte), x, z;
//     the server runs 0x0043d6d0 (its reload and 0x0043d510's crate);
//   - kEvTitan (0x21; read 0x0050efd0, server 0x0050f1f0): kind 1 the
//     commander's move (the team, x, z), kind 2 the team and a ghost.
GameEvent makeArtilleryUse(int team, float x, float z, int rcType);
GameEvent makeUavUse(int team, float x, float z, bool satellite);
GameEvent makeSupplyDrop(int team, float x, float z);
GameEvent makeTitanMove(int team, float x, float z);
struct CommanderToolUse {
	enum Tool { Artillery, OrbitalStrike, Uav, Satellite, Supplies, MoveTitan } tool = Artillery;
	int team = 0;
	float x = 0, z = 0;
};
// One of the four above as the server reads it; false for another event
// (or an Artillery / Titan event of another kind).
bool parseCommanderToolUse(const GameEvent& e, CommanderToolUse& out);
// kEvCreateSpawnGroup (0x39; read 0x005125c0, written 0x005126b0, handled
// 0x005127f0 → the client's spawn manager slot 0x44) and
// kEvRemoveSpawnGroup (0x3a; read 0x00512880, written 0x005128e0, handled
// 0x00512970 → slot 0x48 with the id): a spawn group the spawn screen
// offers: its id (8 bits), team (4), three bits (+0x14..+0x16) and two
// bytes (+0x17, +0x18) whose meanings weren't traced, and the ghost of the
// object it is at (16 bits: a control point, or a vehicle carrying a
// SpawnPoint).
struct SpawnGroupInfo {
	int id = 0, team = 0;
	bool flag14 = false, flag15 = false, flag16 = false;
	int byte17 = 0, byte18 = 0;
	uint16_t ghost = 0;
};
bool parseSpawnGroup(const GameEvent& e, SpawnGroupInfo& out);
GameEvent makeCreateSpawnGroup(const SpawnGroupInfo& info);
GameEvent makeRemoveSpawnGroup(int id, uint16_t ghost);

// kEvPostRemote (0x005096a0, built 0x00509990): a message for the event
// manager (0x00a7d06c) on the other side: category (4 bits), id (32 bits),
// delay (float), payload (8-bit length and bytes, at most 52).
struct PostRemoteInfo {
	uint32_t category = 0;
	uint32_t id = 0;
	float delay = 0;
	std::string payload;
	int32_t payloadInt(size_t index = 0) const;
};
bool parsePostRemote(const GameEvent& e, PostRemoteInfo& out);
GameEvent makePostRemote(uint32_t category, uint32_t id, const std::string& payload = {}, float delay = 0);
std::string int32Payload(int32_t value);
// Known messages (names from the table at 0x00420000.c:15997).
constexpr uint32_t kMsgGameCategory = 0;   // id 1: the game state (4 bytes; 1 playing, 3 before the start)
constexpr uint32_t kMsgNetworkCategory = 6; // ECNetwork...
enum NetworkMessage : uint32_t {
	kNeDataBlockReady = 1,
	kNeLoadComplete = 2,
	kNeStartSimulation = 3,
	kNeDatabaseComplete = 4,
	kNeReset = 5,
	kNeSelectSpawnGroup = 6, // c->s, 4 bytes: the spawn group (0x004c1d50)
	kNeSelectTeam = 7,       // c->s, 4 bytes: the team (0x004c1dc0)
	kNeSelectKit = 8,
	kNePlayerSpawned = 9,
	kNePlayerDead = 10,
	kNeStringReceived = 0xb,
	kNeSuicide = 0xc,        // c->s (0x004c22e0)
	kNeRadioMessageReceived = 0xd,
	// 0xe-0x10 belong to the distributed lightmap renderer
	// ("DistributedLightRenderer", 0x00a0f7a0): a mode the byte at
	// 0x00a1fc02 (0x0042b820) switches on. It is set only by command-line
	// option 0x22 (0x00409950 case 0x22, which also caps allocations at 26
	// MB), and the retail option table (0x004409d0's 34 registrations,
	// 0x00409950) registers no option with that id, so these are never
	// sent. 0x10 goes c->s, without a payload, after the client echoes 6/4
	// (0x004b6630 case 4); the server (0x004b0690) hands the sender's
	// connection to the renderer (slot 0x18). 0xe (slot 0x20) and 0xf (a
	// chat line of type 9 to the sender) go the same way. In that mode
	// data block 4 goes to the renderer too (slot 0x24, 0x004b5eb0).
	kNeLightRenderJob = 0xe,
	kNeLightRenderMessage = 0xf,
	kNeLightRenderReady = 0x10,
	kNeNetworkableDestroyed = 0x11,
	kNeRemoteConsoleCommand = 0x12,
	kNeRemoteConsoleFeedback = 0x13,
	kNeEndOfRoundGuard = 0x14,
};

// kEvDataBlock / kEvStringBlock transfers: a start then chunks.
struct DataBlockChunk {
	bool start = false;
	uint32_t type = 0, size = 0; // DataBlock start: what (0 server settings, 5 map list) and its size
	std::string bytes;           // a chunk (at most 63 bytes are sent at a time)
};
bool parseDataBlock(const GameEvent& e, DataBlockChunk& out);   // kEvDataBlock
bool parseStringBlock(const GameEvent& e, DataBlockChunk& out); // kEvStringBlock: start carries the size only
GameEvent makeDataBlockStart(uint32_t type, uint32_t size);
GameEvent makeDataBlockChunk(const std::string& bytes);

// Chat travels as a StringBlock (0x00643370 queues it, 0x006431c0 sends it,
// 0x006430c0 reassembles it): the text followed by a 12-byte header
// (little-endian u32 channel, u32 source, u8 player, u8, u8, u8 0). A server
// takes a client's line only with source 2 and a channel other than 0xc
// and 0xd (server messages); it posts the result as a chat message
// (0x004a5460: player +0x28, channel +0x34, source +0x38).
// The channels (chat message +0x34), as 0x004ab6b0 names them for the
// scripts' ChatMessage handler. A server's lines to a channel 12 or 13
// carry player 0xff.
enum ChatChannel : uint32_t {
	kChatGlobal = 0,
	kChatTeam = 1,
	kChatCommander = 2,
	kChatSquad = 3,
	kChatSquadLeader = 4,
	kChatSquadId = 5,
	kChatLocalTeamMedic = 6,
	kChatLocalGroundVehicle = 7,
	kChatLocalAirVehicle = 8,
	kChatLocal = 9,
	kChatLocalTeam = 10,
	kChatVehicle = 11,
	kChatServerMessage = 12,
	kChatServerTeamMessage = 13,
	kChatPlayer = 14,
	kChatLocalRevive = 15, // 16 is named the same
	kChatTeamAndLocalEnemies = 17,
	kChatSquadAndLocalEnemies = 18,
	kChatInvalid = 19,
};
struct ChatMessage {
	uint32_t channel = kChatGlobal; // ChatChannel
	uint32_t source = 2;  // the scripts' "type" (GameLogic slot 37, 0x004a5ec0): 1 from a server, 2 typed by a player
	int player = -1;
	uint8_t b9 = 0, b10 = 0;
	std::string text;
};
std::string encodeChat(const ChatMessage& message);
bool decodeChat(const std::string& block, ChatMessage& out);
// The StringBlock events carrying a block: the start, then 63-byte chunks.
std::vector<GameEvent> stringBlockEvents(const std::string& block);

struct KilledByInfo { // kEvKilledBy (0x0050d620 reads, 0x0050d750 writes; built by 0x005b7680 -> 0x005b7130, handled 0x0050d520)
	int attacker = 0;      // 8 bits: the attacker's player id (vtable 0x58), when hasAttacker
	int victim = 0;        // 8 bits: the player hit (the receiving client's own)
	bool hasAttacker = false;
	uint16_t object = 0;   // 16 bits: an id the caller passes
	// The damage source's template values (template vtable 0x40): the
	// weapon or projectile (unless the caller says not to), then the
	// attacker's vehicle when it is in one, else the source's root object.
	uint32_t value1 = 0, value2 = 0;
	int armorValue = 0;    // 8 bits: the victim's armor vtable 0xdc (the HUD's slot 0xd8 gets it)
	Vec3f position;        // armor vtable 0xe4: where the damage came from
	float damage = 0;      // 10 bits: armor vtable 0xec x 0.01, 0..1, as 0-100
};
bool parseKilledBy(const GameEvent& e, KilledByInfo& out);

// kEvPythonCommand (0x00511a50, handled 0x00511ba0): a scripted message:
// 32-bit type, 8 bits, up to 15 32-bit arguments. Type 100 with arguments
// (kind, victim, attacker, ...) are kills: kind 0 and 1 the attacker's
// view, 2 the victim's.
struct PythonCommandInfo {
	int32_t type = 0;
	uint32_t value8 = 0;
	std::vector<int32_t> args;
};
bool parsePythonCommand(const GameEvent& e, PythonCommandInfo& out);

// kEvSelectKit (0x0050ae90, handled on the server by 0x0050b010: the game
// logic's slot 25 gets the kit and the four unlocks; the last bit goes to
// player slot 0x388).
GameEvent makeSelectKit(int kit, const std::array<int32_t, 4>& unlocks = {-1, -1, -1, -1}, bool flag = false);
// The CD-key answer and connection type as GameEvents.
GameEvent makeChallengeResponse(const std::string& response, uint32_t mixed, uint32_t build, uint32_t profileId);
GameEvent makeConnectionType(uint32_t type);

// ---------------------------------------------------------------------------
// Ghosts

// The ghost block (0x00581290): 1 bit present; 32 bits the server's tick
// (x 1/30 s); 8 bits update count; 1 bit a control-object record follows
// (0x00580cd0); then the updates (0x0057fce0), each 2 bits kind and a 16-bit
// ghost id:
//   kind 1: an update: 1 bit (full), the state's length (the ghost
//           manager's +0x2140, kGhostStateLengthBits) and that many bits of networkable state, read by
//           the ghost's networkables (vtable slot 0x30, 0x0057f670); left
//           undecoded here (per-template layouts: e.g. the soldier's
//           0x00602070 reads a 12-bit mask, then rotations, velocities
//           and a 3 x 14-bit vector);
//   kind 3: the ghost is removed (0x0067a010);
//   kind 0: nothing; kind 2: invalid.
// The state length's bits: the ghost manager's +0x2140 (0x00582a50: 11,
// or 13 when built with a non-zero kind). ServerConnection 0x0057b3f0
// builds it with its kind (+0x170): a client connecting to a server uses
// kind 0 (0x004b4d10), and a server's per-client managers too (0x004ac920:
// 0x0057cec0(id == 0x81)); only demo recording (connection id 0x81,
// 0x004afcc0) and playback (kind 2, 0x004b4a80) use 13. The reader
// 0x0057fce0 and the writer 0x0057fbc0 both take it from +0x2140.
constexpr unsigned kGhostStateLengthBits = 11;
constexpr unsigned kDemoGhostStateLengthBits = 13;

// The control-object record (the client's own soldier or vehicle, for its
// prediction): 12 bits length (of the rest), the last move the server took
// (sign + 31 bits), the position (3 floats; it becomes the stream's
// reference for delta positions, 0x006874f0), the object's ghost id, an
// optional second ghost (1 bit + 16 bits), an optional vehicle check (1 bit,
// 1 bit + 16 bits), then networkable state, kept undecoded in `rest`. What
// 0x00580cd0 reads there depends on the client's own copy: only with its
// control object's ghost (0x00678d70) does it read 1 bit (whether the
// state is recorded for prediction) and a 3-bit history slot
// (0x00508340(3)); then every group of the ghost (and of the second ghost)
// in mode 3, the prediction's mode (0x0057f720 / 0x0057f9f0 -> slot 0x30
// with 3; the Soldier's reader 0x005fcdb0 then takes raw floats and
// 16-bit angles where mode 2 has 12 bits, RotationalBundle 0x00602070 16
// bits an angle, the PCO 0x00605b00 skips a check, Engine 0x00609340 and
// Spring 0x00605320 differ too), then a 10-bit length and the control
// object's own state (its vtable 0x1cc), checked against that length.
// Without the object everything is skipped by the 12-bit length. `rest`
// holds that state; GhostLayout.h's ControlState reads and writes it with
// the ghost's layout (RemoteWorld::controlState, HostedGame).
struct ControlObjectRecord {
	int32_t lastMove = 0;
	Vec3f position;
	uint16_t ghost = 0;
	uint16_t second = 0;
	uint16_t vehicle = 0;
	std::vector<uint8_t> rest; // the bits after these fields, packed from bit 0
	size_t restBits = 0;
};

struct GhostUpdate {
	uint32_t kind = 0;
	uint16_t ghost = 0;
	bool full = false;
	std::vector<uint8_t> state; // kind 1: the state's bits, packed from bit 0
	size_t stateBits = 0;
};

struct GhostBlock {
	bool present = false;
	uint32_t tick = 0;
	std::optional<ControlObjectRecord> control;
	std::vector<GhostUpdate> updates;
};
bool readGhostBlock(BitReader& in, GhostBlock& block, unsigned lengthBits = kGhostStateLengthBits);
void writeGhostBlock(BitWriter& out, const GhostBlock& block, unsigned lengthBits = kGhostStateLengthBits);

// ---------------------------------------------------------------------------
// Whole payloads

struct GamePayload {
	InputBlock input;
	bool hasEvents = false;
	ReliableEventBlock events;
	GhostBlock ghosts;
	bool complete = true; // false when an event couldn't be decoded (ghosts unread)
};
// lengthBits: the ghost states' length field (kDemoGhostStateLengthBits on
// a demo connection, recording and playback).
bool readGamePayload(const std::vector<uint8_t>& payload, GamePayload& out, unsigned lengthBits = kGhostStateLengthBits);
std::vector<uint8_t> writeGamePayload(const GamePayload& payload, unsigned lengthBits = kGhostStateLengthBits);

} // namespace dice::hfe::net
