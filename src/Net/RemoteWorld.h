// The game as a client sees it on a server: the objects the server
// replicates (ghosts) and the players, built from the events and ghost
// blocks a GameClient receives (GameProtocol.h). It mirrors what
// game::GameWorld offers a local game (objectList/playerList/localPlayer,
// setInput) so the renderer and HUD can draw either.
//
// Ghost updates are decoded for the root networkable of each ghost
// (Networkables.h) when the object's kind is known: from the hints
// resolver (setNetworkableHints), else from nothing (the state is kept as
// bits). Players' networkables (ghost ids from CreatePlayer) give score,
// kills, deaths, ping and team.
#pragma once

#include "GameProtocol.h"
#include "GhostLayout.h"
#include "Networkables.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dice::hfe::net {

// One object of a ghost (a template child: a turret's RotationalBundle, a
// wheel's Spring, the engine...) with the state of its group, kept from
// update to update (each update carries only what changed).
struct RemotePart {
	std::string name;         // the template's name
	uint32_t templateId = 0;
	int parent = -1;          // index in RemoteObject::parts
	NetworkableKind kind = NetworkableKind::Unknown; // Unknown: no networkable
	int group = -1;           // the ghost state group it takes (-1 none)
	bool hasRotation = false; // RotationalBundle: yaw, pitch, roll in degrees
	float rotation[3] {};
	float rotationSpeed[3] {};
	bool hasSpring = false;   // Spring: offset and velocity
	Vec3f springOffset, springVelocity;
	int gear = -1;            // Engine
	float rpm = 0;
	float engineRotation[2] {}; // Engine: rotation speed x, z
	float engineInputs[3] {};
	bool hasPosition = false; // Item, TargetObject, SimpleObject parts
	Vec3f position;
	bool hasOrientation = false;
	Quat orientation;
	float health = -1;
	NetworkableState last;    // the last state its group received
};

struct RemoteObject {
	uint16_t ghost = 0;
	uint32_t templateId = 0;  // ObjectTemplate::id() of the server's network-shared templates
	std::string templateName; // from the resolver, empty when unknown
	int team = 0;
	bool kit = false;         // a dropped kit (CreateKit)
	bool hasPosition = false;
	Vec3f position;
	Vec3f rotation;           // degrees, yaw/pitch/roll (see CreateObjectInfo)
	float health = -1;        // -1: unknown; soldiers: a fraction of maxHitPoints, others: hit points
	NetworkableKind kind = NetworkableKind::Unknown; // the ghost's root networkable
	bool hasOrientation = false;
	Quat orientation;         // SimpleObject / GenericProjectile ghosts
	bool hasVelocity = false;
	Vec3f velocity;
	bool hasAngles = false;   // soldiers: degrees
	float yaw = 0, pitch = 0, aimPitch = 0;
	int weaponZoom = -1;      // soldiers: the weapon's zoom level (0-3)
	// Control points (0x006084b0): the flag's height (capture progress,
	// cp +0x19c, 0..1), its rate per second (+0x194, signed) and which
	// team's flag is up (+0x1b0).
	float captureProgress = 0;
	float captureRate = 0;
	int raisedFlag = 0;
	// Soldiers: the ragdoll group (0x10000) seen; the positions it sent last
	// (its networked particles, see NetworkableState::ragdollParticles), and
	// how many such updates came (a new one drives the body: 0x008acce0).
	bool ragdoll = false;
	std::vector<Vec3f> ragdollParticles;
	uint32_t ragdollUpdates = 0;
	bool decoded = false;     // the last update's root state was read completely
	bool decodedAll = false;  // ... and every group after it (with a ghost layout)
	// Ghost layout (setGhostTemplates): a vehicle is a ghost per
	// PlayerControlObject, numbered from the root's (CreateObject's count).
	uint16_t rootGhost = 0;   // the vehicle's ghost (itself for a root)
	int part = 0;             // which of its ghosts (0 the root, then each seat in pre-order)
	std::vector<uint16_t> seats; // the root's: its other ghosts
	std::vector<RemotePart> parts; // this ghost's objects in pre-order (parts[0] the ghost's root)
	const RemotePart* findPart(const std::string& name) const;
	int occupant = -1;        // the player in this seat (EnterVehicle / ExitVehicle)
	// A seat with weapons: its active weapon per group (-1 none) and
	// whether it ducks (PCO +0x310). Without: the occupant's soldier
	// state (angles, health) as the seat's state carries it.
	int seatWeapon[3] {-1, -1, -1};
	bool seatDucking = false;
	bool hasOccupantState = false;
	NetworkableState occupantState;
	// Soldiers: the selected weapon (mask 0x2000) and, in a vehicle, the seat.
	int weapon = -1;
	uint32_t updates = 0;     // ghost updates received
	uint32_t lastTick = 0;    // the server tick of the last one
	std::vector<uint8_t> state; // the last update's networkable state (undecoded)
	size_t stateBits = 0;
};

struct RemotePlayer {
	int id = -1;
	std::string name;
	int team = 0;
	bool local = false;
	bool ai = false;
	int32_t profileId = 0;
	bool alive = false;
	uint16_t soldierGhost = 0; // the soldier of HandlePickup / HandleDrop (or the control object)
	uint16_t kitGhost = 0;     // the kit HandlePickup gave it (0 after HandleDrop)
	uint16_t vehicleGhost = 0;
	int kills = 0, deaths = 0; // counted from kill messages (PythonCommand type 100)
	int score = 0;             // from the player's networkable (mask 8), as are these
	int teamwork = 0, teamKills = 0;
	int ping = -1;             // ms (player +0xf0, the networkable's mask 0x100)
	uint16_t networkableGhost = 0; // the player's own ghost (CreatePlayer)
	int squad = 0;             // the networkable's mask 0x2000 (or squad events)
	bool squadLeader = false;  // its always-sent bit (player +0x10d)
	bool commander = false;    // mask 0x8000000 (player +0x10c)
	int kit = -1;              // mask 0x1000: the selected kit
	int rank = -1;             // mask 0x4000
};

// KickBan (0x37; written by 0x0050cf30): a player leaves the server,
// kicked or banned, with a byte (the server's reason; meaning not traced).
struct KickInfo {
	int player = -1;
	bool banned = false;
	uint32_t value = 0;
};

struct ChatLine {
	int player = -1;
	uint32_t channel = 0;
	uint32_t source = 0;
	std::string text;
};

struct KillReport {
	int victim = -1, attacker = -1;
	int weapon = -1; // KilledBy: its armor byte (KilledByInfo::armorValue)
	Vec3f from;
};

class RemoteWorld {
public:
	// Names a template id (e.g. from world::ObjectTemplateManager::findById
	// after loading the same level the server runs).
	void setTemplateResolver(std::function<std::string(uint32_t)> resolver) { m_resolver = std::move(resolver); }
	// What the ghost decoder needs to know about a template (its kind,
	// whether its physics moves, its armor). Without it a ghost's kind is
	// Unknown and its updates stay undecoded.
	void setNetworkableHints(std::function<NetworkableHints(uint32_t templateId)> hints) { m_hints = std::move(hints); }
	// The template tree of a template id (e.g. net::ghostTemplateFrom on the
	// level's templates). With it every group of a ghost is decoded, the
	// seats of a vehicle are known and RemoteObject::parts are filled; it
	// takes precedence over setNetworkableHints.
	void setGhostTemplates(std::function<GhostTemplateNode(uint32_t templateId)> templates)
	{
		m_ghostTemplates = std::move(templates);
		m_layouts.clear();
	}
	// The layout used for a template (nullptr without setGhostTemplates).
	const std::vector<GhostPartLayout>* ghostLayout(uint32_t templateId);

	// Applies a received event; false for events the world doesn't use
	// (they are kept in unhandled()).
	bool apply(const GameEvent& event);
	void apply(const GhostBlock& ghosts);

	// Objects and players.
	std::vector<RemoteObject> objectList() const;
	const RemoteObject* object(uint16_t ghost) const;
	// How a vehicle ghost's part stands as its groups last said (the
	// root's ghost and its seats' ghosts searched): a RotationalBundle's
	// angles (degrees) or a Spring's offset; false for a part no group
	// moved. The renderer poses a joined game's vehicles by it.
	bool partPose(uint16_t rootGhost, const std::string& part, Vec3f& offset, Vec3f& rotation) const;
	std::vector<RemotePlayer> playerList() const;
	const RemotePlayer* player(int id) const;
	int localPlayer() const { return m_localPlayer; }
	// Our own soldier or vehicle as the last control-object record had it.
	bool hasControlObject() const { return m_hasControl; }
	const ControlObjectRecord& controlObject() const { return m_control; }
	// The last record's prediction state (mode 3, GhostLayout.h's
	// ControlState), nullptr when that record's couldn't be decoded (no
	// ghost layout for its ghost, or a server that sent none).
	const ControlState* controlState() const { return m_controlStateValid ? &m_controlState : nullptr; }
	// Whether the last ghost block carried a record (the server sends one
	// while we have a living soldier).
	bool controlInLastBlock() const { return m_controlInLastBlock; }
	// Records received, and how many of them had a decoded state.
	uint32_t controlRecords() const { return m_controlRecords; }
	uint32_t controlStates() const { return m_controlStates; }
	// Of them, those written against a history slot (relative records).
	uint32_t relativeControlStates() const { return m_relativeStates; }
	uint32_t serverTick() const { return m_tick; }

	// The game status (PostRemote category 0, id 1; game logic +0x40,
	// 0x004ba2a0): 1 playing, 2 end of game, 3 before the start, 4 paused;
	// 0 unknown.
	uint32_t gameState() const { return m_gameState; }
	// BeginRound (0x38; 0x00511540, handled 0x00511690): the round's state
	// (game logic +0x204: 0 not started, 1 waiting for players, 2 the
	// countdown, 3 playing), the tick it ends at (+0x208, server ticks of
	// 1/30 s: the countdown's end in state 2, the time limit's in 3, 0
	// none) and the players needed to start (-1 unchanged).
	struct RoundInfo {
		uint32_t state = 0;
		uint32_t endTick = 0;
		int playersNeeded = -1;
	};
	bool roundStarted() const { return m_roundStarted; } // a BeginRound came (and no EndOfRound since)
	const RoundInfo& round() const { return m_round; }
	// EndOfRound (0x30; 0x00511750, handled 0x00511870): the rounds left on
	// this map (roundsPerMap - played - 1), the winning team and the victory
	// type (EndOfRoundInfo slots 0x1c / 0x20). The game status goes to 2.
	struct EndOfRoundInfo {
		uint32_t roundsLeft = 0;
		uint32_t winner = 0;
		uint32_t victoryType = 0;
	};
	bool roundEnded() const { return m_roundEnded; }
	const EndOfRoundInfo& endOfRound() const { return m_endOfRound; }
	// The end-of-round block (data block 8; EndOfRoundInfo, BF2142.exe
	// src BF2/Game/EndOfRoundInfo.cpp: 0x0059cbc0 writes it, 0x0059c740
	// reads it). The scripts' gl_sendEndOfRoundData (0x00568930) hands
	// over the text (the end-of-round screen's data) and the delay comes
	// from the settings (0x00430610). The layout: the text's size and its
	// zlib-compressed size (32 bits each; compress2 at the default level,
	// 0x0071bd80, at most 0x8000 bytes compressed), the compressed bytes,
	// the delay (sign + 31 bits, 0x00515350), an 8-bit player count, then
	// per player (the player manager's list, slot 0x40) its id (8 bits)
	// and six values of sign + 15 bits from its stats (player vtable
	// 0x1a8, 0x00840330: *(player +0x100)): kills (+0x14), skillScore
	// (+0x8), deaths (+0x10), score (+0), rplScore (+4) and cmdScore
	// (+0xc), the offsets the scripts' getters read (0x0056b6f0).
	struct EndOfRoundData {
		std::string text;
		// sv.endOfRoundDelay (settings +0x270, clamped to 3..30 by
		// 0x00431640, 15 at first), or 15 in gpm_ti.
		int32_t endOfRoundDelay = 0;
		struct Player {
			int id = 0;
			int kills = 0, skillScore = 0, deaths = 0, score = 0, teamworkScore = 0, commanderScore = 0;
		};
		std::vector<Player> players;
	};
	// The last block 8 decoded; false before one came (or when it doesn't
	// decode).
	bool endOfRoundData(EndOfRoundData& out) const;
	static std::string encodeEndOfRoundData(const EndOfRoundData& data);
	static bool decodeEndOfRoundData(const std::string& block, EndOfRoundData& out);

	// Transfers (DataBlock events; the client's handler is 0x004b5eb0). Each
	// is a bit stream, strings a u16 length and the bytes (0x005155b0).
	const std::map<uint32_t, std::string>& dataBlocks() const { return m_dataBlocks; }
	std::vector<std::string> dataBlockStrings(uint32_t type) const;
	// The map list (data block 5; 0x00441e40 reads it, 0x00441410 writes
	// it): a u16 count, the u16 index of the current map (maplist
	// +0x1c), then per map a string name, a string game mode and a u16
	// size. The server sends it again when the list or its current map
	// changes.
	struct MapEntry {
		std::string name, gameMode;
		uint32_t size = 0;
	};
	std::vector<MapEntry> mapList() const;
	int currentMapIndex() const; // -1 without a map list
	// The server settings (data block 0; 0x00516230 reads, 0x00515bc0
	// writes, from the settings at 0x00a201c0 and the game logic).
	struct ServerSettings {
		std::string name;
		std::vector<std::string> welcome; // up to 3 pieces of up to 256 bytes
		bool initial = false, voipEnabled = false, allowNoseCam = false, hitIndicator = false, tkPunishByDefault = false,
			 allowFreeCam = false, allowExternalViews = false;
		// initial: the writer's argument (1 for the send on connecting,
		// 0x004a7ce0; 0 when resent after a change, 0x004b1c90). The voice
		// fields come from game logic slot 0x254 (0x004cbdf0; filled by
		// 0x0057c250): inet_addr(sv.voipServerRemoteIP) when
		// sv.voipServerRemote, else 1 (the game server's own), the ports
		// sv.voipServerPort and sv.voipBFClientPort, and sv.voipQuality.
		// The flags after voipEnabled: settings +0xfa, +0xfb, +0x148, +0xf8, +0xf9.
		uint32_t voipServerAddress = 0;
		uint32_t voipServerPort = 0, voipClientPort = 0;
		int32_t voipQuality = 0;
		uint32_t gameSpyPort = 0, gamePort = 0;
		int32_t radioSpamInterval = 0, radioMaxSpamFlagCount = 0, radioBlockedDurationTime = 0;
		bool contentCheck = false;
		bool votingEnabled = false;
		uint32_t playersNeededToStart = 0;
		bool friendlyFireWithMines = false;
		bool allowTitanMovement = false;
		bool complete = false; // read to its end
	};
	ServerSettings settings() const;
	// The server's name: the settings start with it.
	std::string serverName() const;
	// The level to load (data block 2; 0x00515650 reads it, 0x005156f0
	// writes it at each level start): the game mode, the level directory,
	// the map name, the map size (the max players), whether there are
	// commanders and a random 0-9 (kept by the ghost manager). A second
	// one is a map change on the same connection: the client loads it and
	// answers each with PostRemote 6/2 (GameClient does).
	struct LevelInfo {
		std::string gameMode;
		std::string levelDirectory;
		std::string mapName;
		int32_t maxPlayers = 0;
		bool commander = false;
		int32_t seed = 0;
	};
	bool hasLevel() const { return m_levels > 0; }
	const LevelInfo& level() const { return m_level; }
	uint32_t levelsReceived() const { return m_levels; } // counts the level blocks
	// The level blocks received since the last call (map changes).
	std::vector<LevelInfo> takeLevelChanges() { return std::exchange(m_levelChanges, {}); }
	// Data block 2's layout, for the loopback server and tests.
	static std::string encodeLevel(const LevelInfo& level);
	static bool decodeLevel(const std::string& data, LevelInfo& out);
	static std::string encodeMapList(const std::vector<MapEntry>& maps, int current);
	static std::string encodeSettings(const ServerSettings& settings);
	static bool decodeSettings(const std::string& data, ServerSettings& out);

	// Things that happened since the last take*().
	std::vector<ChatLine> takeChat() { return std::exchange(m_chat, {}); }
	std::vector<KillReport> takeKills() { return std::exchange(m_kills, {}); }
	std::vector<PostRemoteInfo> takeMessages() { return std::exchange(m_messages, {}); }
	std::vector<GameEvent> takeUnhandled() { return std::exchange(m_unhandled, {}); }
	std::vector<KickInfo> takeKicks() { return std::exchange(m_kicks, {}); }

	// The local player's input, sent with each tick by the GameClient.
	void setInput(const PlayerInput& input) { m_input = input; }
	const PlayerInput& input() const { return m_input; }

	void clear();

private:
	RemotePlayer& playerSlot(int id);
	void applyStringBlock(const DataBlockChunk& chunk);
	void applyDataBlock(const DataBlockChunk& chunk);
	void applyPythonCommand(const PythonCommandInfo& info);

	std::function<std::string(uint32_t)> m_resolver;
	std::function<NetworkableHints(uint32_t)> m_hints;
	std::function<GhostTemplateNode(uint32_t)> m_ghostTemplates;
	std::map<uint32_t, std::shared_ptr<const std::vector<GhostPartLayout>>> m_layouts;
	// A ghost's layout: its template's ghosts and which of them it is.
	std::map<uint16_t, std::pair<std::shared_ptr<const std::vector<GhostPartLayout>>, size_t>> m_ghostParts;
	void applyParts(RemoteObject& o, const GhostPartLayout& layout, const std::vector<NetworkableState>& states);
	std::map<uint16_t, NetworkableHints> m_objectHints;
	std::map<uint16_t, int> m_playerGhosts; // player networkable ghost -> player id
	void applyState(RemoteObject& o, const NetworkableState& state);
	std::map<uint16_t, RemoteObject> m_objects;
	std::map<int, RemotePlayer> m_players;
	int m_localPlayer = -1;
	bool m_hasControl = false;
	ControlObjectRecord m_control;
	void decodeControl();
	ControlState m_controlState;
	std::array<std::vector<MotionHistory>, 8> m_historySlots; // the ghost manager's +0x20f4
	uint32_t m_relativeStates = 0;
	bool m_controlStateValid = false;
	bool m_controlInLastBlock = false;
	uint32_t m_controlRecords = 0, m_controlStates = 0;
	uint32_t m_tick = 0;
	uint32_t m_gameState = 0;
	bool m_roundStarted = false;
	RoundInfo m_round;
	bool m_roundEnded = false;
	EndOfRoundInfo m_endOfRound;
	uint32_t m_levels = 0;
	LevelInfo m_level;
	std::vector<LevelInfo> m_levelChanges;
	void onLevel(const std::string& data);
	std::map<uint32_t, std::string> m_dataBlocks;
	uint32_t m_dataType = 0, m_dataSize = 0;
	std::string m_dataBuffer;
	bool m_inData = false;
	uint32_t m_stringSize = 0;
	std::string m_stringBuffer;
	bool m_inString = false;
	std::vector<ChatLine> m_chat;
	std::vector<KillReport> m_kills;
	std::vector<PostRemoteInfo> m_messages;
	std::vector<GameEvent> m_unhandled;
	std::vector<KickInfo> m_kicks;
	PlayerInput m_input;
};

} // namespace dice::hfe::net
