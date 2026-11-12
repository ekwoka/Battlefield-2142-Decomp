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
#include <optional>
#include <string>
#include <tuple>
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
	int legState = -1;        // MechLegs: the state (+0x1bc, mask 1), -1 not yet
	bool legHolds = false;    // and its input still held (+0x1c0, mask 2)
	bool hasPosition = false; // Item, TargetObject, SimpleObject parts
	Vec3f position;
	bool hasOrientation = false;
	Quat orientation;
	float health = -1;
	NetworkableState last;    // the last state its group received
};

// The legs' messages to the walker's animation system (0x00650530, each
// tick from 0x00650e90): 0x780 cleared, then with the state's input held
// (+0x1c0) 0x80 walking (state 1), 0x100 backing (2), 0x400 turning left
// (3), 0x200 right (4); state 5 raises the control object's crouch flag
// (slot 0x134), which the object passes on as 0x10 (0x0063ce30). Both
// fields are in the MechLegs networkable (0x0060a100), so a client's
// remote walker takes the server's.
uint32_t mechLegsMessages(int state, bool holds);

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
	// Soldiers: the pose and the pose asked for (mask 0x20, soldier +0x268 /
	// +0x26c: 0 stand, 1 crouch, 2 prone, 3 swim; the animation's
	// PoseTrigger takes the first), and the always-sent bits (+0x328, the
	// swimming +0x30c, the weapon put away +0x3e9: 0x005878b0).
	int pose = 0, requestedPose = 0;
	bool swimming = false, weaponAway = false;
	// Soldiers on a ladder (mask 0x20000): its id (the Ladder's +0x184, 1
	// and up in the level's order; 0 none) and the place on it, 0..1.
	int ladder = 0;
	float ladderPosition = 0;
	// Control points (0x006084b0): the flag's height (capture progress,
	// cp +0x19c, 0..1), its rate per second (+0x194, signed) and which
	// team's flag is up (+0x1b0).
	float captureProgress = 0;
	float captureRate = 0;
	int raisedFlag = 0;
	bool hasAllow[2] {}, allow[2] {true, true}; // allowCaptureByTeam 1, 2 (masks 8, 0x10)
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
	// The armored objects below the root as its state carries them (mask
	// 0x800000: eighths of their hit points, 8 intact, 0 a wreck) and of
	// those the vehicle HUD's repair cells (hudRepairCellNo, 0x007a1e50: up
	// while standing), empty until a state with them came.
	std::vector<int> childDamage;
	std::vector<bool> repairCells;
	std::vector<int> repairCellEighths; // the same cells' eighths (GameWorld::setRemoteRepairCells)
	// Soldiers: the selected weapon (mask 0x2000) and, in a vehicle, the seat.
	int weapon = -1;
	uint32_t updates = 0;     // ghost updates received
	uint32_t lastTick = 0;    // the server tick of the last one
	// The server tick of the last one with a position: an update carries
	// only what changed since the client last had it (the server's dirty
	// masks, 0x005800e0), so the others leave the position as it was.
	uint32_t positionTick = 0;
	std::vector<uint8_t> state; // the last update's networkable state (undecoded)
	size_t stateBits = 0;
};

struct RemotePlayer {
	int id = -1;
	// The player manager's list order on the client (0x004b8b60 appends
	// each player CreatePlayer makes): the scoreboard's ties keep it.
	uint64_t created = 0;
	std::string name;
	int team = 0;
	bool local = false;
	bool ai = false;
	int32_t profileId = 0;
	// A spectator (player +0x404): CreatePlayer's flag (+0x40) or
	// JoinAsSpectator from the server (0x00514260).
	bool spectator = false;
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
	bool manDown = false;      // mask 0x20 (player +0xe0)
	// The weapon groups' fire states (masks 0x40, 0x80, 0x20000: the
	// player's +0x19c..: firing, alt-firing; PlayerNetState::FireState) and
	// the fire's presses (mask 0x4000000, two bits), for animating the
	// others' soldiers (0x005981a0 sets them on a remote player only).
	std::array<PlayerNetState::FireState, 3> fire {};
	int firePresses = -1;
	uint32_t fireUpdates = 0; // player states that carried mask 0x4000000
	// Player states that carried mask 0x10000 (the server's flag +0x28c,
	// set by a c_PIReload press: 0x004bda00 -> 0x0059b330): each is a
	// reload seen (the client's 0x00590e10: soldier input message 10).
	uint32_t reloads = 0;
	// The time outside the combat area (player +0x294, the networkable's
	// mask 0x800: whole seconds, 0 inside or after a respawn), the HUD's
	// countdown (0x0044c7b0 through slot 0xf0).
	int outsideSeconds = 0;
};

// KickBan (0x37; written by 0x0050cf30, constructor 0x0050d000): a player
// leaves the server, kicked or banned, with a byte the retail server always
// sends 0 (its ban 0x00505fa0 and kick 0x00506050 pass 0); the client's
// handler (0x0050cfd0) posts the three in game event 0x4d.
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
	// Each disagreement with the server's records (NetDiagnostics.h): an
	// object of a template we don't have, an update of a ghost never made,
	// a ghost state or control-object record we can't decode or whose
	// decoding doesn't end at its length. (category, text)
	std::function<void(const std::string&, const std::string&)> onProblem;

	// Applies a received event; false for events the world doesn't use
	// (they are kept in unhandled()).
	bool apply(const GameEvent& event);
	void apply(const GhostBlock& ghosts);

	// Objects and players.
	std::vector<RemoteObject> objectList() const;
	const RemoteObject* object(uint16_t ghost) const;
	// A sticky round's StickyProjectile event (kEvStickyProjectile,
	// 0x0050f5d0): where it was stuck, kept until its ghost goes; nullptr
	// when it hasn't been.
	const StickyProjectileInfo* stuckProjectile(uint16_t ghost) const;
	// The team's overhead camera ghost (OverheadCamera, 0x00603980) where
	// the server placed it: the commander's view at the map's zoom 2
	// (0x00779680); false before the server made one.
	bool overheadCamera(int team, Vec3f& position) const;
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

	// The demo just recorded (data block 9; 0x004ad7a0 sends it once the
	// AutoRecorder's hook is done, 0x005852b0): sv.demoDownloadURL with
	// "{file}" replaced by the demo's file name (or the name appended),
	// and the map. Each string an 8-bit length (at most 255 bytes kept
	// here) and its bytes (0x00583ec0 writes, 0x00583d80 reads). The
	// client's handler (0x004b5eb0 case 9) hands the URL and the map to
	// its interface at +0xa8 (vtable 0x00928fa8), slot 0x6c 0x004b4710:
	// a demo bookmark (MenuBindings::addDemoBookmark). demoNotice keeps the
	// last; takeDemoNotice gives each once.
	struct DemoNotice {
		std::string url, map;
	};
	bool demoNotice(DemoNotice& out) const;
	bool takeDemoNotice(DemoNotice& out);
	static std::string encodeDemoNotice(const DemoNotice& notice);
	static bool decodeDemoNotice(const std::string& block, DemoNotice& out);

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
	// The control points' allowCaptureByTeam as their ghosts set it (each
	// set, 0x006084b0 -> 0x005bcd00), for the takeable radio.
	struct TakeableSet {
		uint16_t ghost = 0;
		int team = 0;
		bool allowed = true;
	};
	std::vector<TakeableSet> takeTakeable() { return std::exchange(m_takeable, {}); }
	// The players the squad's battlefield reports (event 0x41) put on the
	// minimap, while their seconds last (0x00508590).
	std::vector<int> radarPlayers() const
	{
		std::vector<int> players;
		for (const auto& [id, until] : m_radar) {
			if (until > m_tick) {
				players.push_back(id);
			}
		}
		return players;
	}
	// The vehicles those reports spotted (2/0x2e), while their seconds last.
	std::vector<uint16_t> radarVehicles() const
	{
		std::vector<uint16_t> vehicles;
		for (const auto& [ghost, until] : m_radarVehicles) {
			if (until > m_tick) {
				vehicles.push_back(ghost);
			}
		}
		return vehicles;
	}
	uint32_t radarReports() const { return m_radarReports; }
	std::vector<PostRemoteInfo> takeMessages() { return std::exchange(m_messages, {}); }
	// The server's rcon answers (data block 7, PlayerAuth.h
	// kRconFeedbackBlock), each as the console prints it: the text and a
	// newline (0x004b5eb0 case 7, the suffix at 0x00918bf4).
	std::vector<std::string> takeConsoleLines() { return std::exchange(m_consoleLines, {}); }
	std::vector<GameEvent> takeUnhandled() { return std::exchange(m_unhandled, {}); }
	// The commanders' strike assets the server fired (kEvArtillery kind 2,
	// the client's handler 0x0050eeb0): the client fires the asset's
	// firearm there itself, for its effects (client::RemoteStrikes).
	std::vector<StrikeFireInfo> takeStrikeFires() { return std::exchange(m_strikeFires, {}); }
	std::vector<KickInfo> takeKicks() { return std::exchange(m_kicks, {}); }

	// The teams' networkable (ghost kTeamStateGhost, Networkables.h): each
	// bit's values as last sent; tickets(team) -1 before the server sent them.
	const TeamNetState& teamState() const { return m_teamState; }
	int tickets(int team) const { return m_teamState.tickets(team); }
	// The spawn groups the server offers (CreateSpawnGroup / RemoveSpawnGroup), by id.
	const std::map<int, SpawnGroupInfo>& spawnGroups() const { return m_spawnGroups; }
	// The squads' requests to their commander (IssueSquadOrder with store 2,
	// kept one a kind per squad as the squad's record does, 0x004c3f20;
	// SetAcceptOrder for store 2 takes one off). team 0: every team's.
	struct SquadRequest {
		int team = 0, squad = 0, kind = 0;
		uint16_t object = 0;
		Vec3f point;
	};
	std::vector<SquadRequest> squadRequests(int team = 0) const;
	// The squads' orders (IssueSquadOrder with store 0, the squad leader's
	// to his squad, squad manager slot 0x48; 1 the commander's, slot 0x4c):
	// one of each store a squad; SetAcceptOrder for its store sets the
	// leader's answer (the bit) or, refused, takes it off (0x004c2f60).
	struct SquadOrder {
		int team = 0, squad = 0, store = 0, kind = 0;
		uint16_t object = 0;
		Vec3f point;
		bool accepted = false;
	};
	std::vector<SquadOrder> squadOrders(int team = 0) const;
	const SquadOrder* squadOrder(int team, int squad, int store) const;

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
	// Ghosts out of scope (kind 3, 0x0057fce0 -> 0x0057bb10: the object is
	// hidden and leaves the active map, not destroyed); a kind-1 update
	// brings one back (0x0057bdc0).
	std::map<uint16_t, RemoteObject> m_hidden;
	std::map<uint16_t, StickyProjectileInfo> m_stuck; // a projectile's ghost -> its StickyProjectile event
	std::map<int, RemotePlayer> m_players;
	uint64_t m_playersMade = 0; // RemotePlayer::created
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
	std::vector<TakeableSet> m_takeable;
	std::map<int, uint32_t> m_radar; // a player -> the server tick it leaves the minimap
	std::map<uint16_t, uint32_t> m_radarVehicles; // a vehicle's ghost -> the same
	uint32_t m_radarReports = 0;
	std::vector<PostRemoteInfo> m_messages;
	std::vector<std::string> m_consoleLines;
	bool m_newDemoNotice = false;
	std::vector<GameEvent> m_unhandled;
	std::vector<KickInfo> m_kicks;
	std::vector<StrikeFireInfo> m_strikeFires;
	PlayerInput m_input;
	TeamNetState m_teamState;
	std::map<int, SpawnGroupInfo> m_spawnGroups;
	std::map<std::pair<int, int>, std::array<std::optional<SquadRequest>, 4>> m_requests; // (team, squad)
	std::map<std::tuple<int, int, int>, SquadOrder> m_orders; // (team, squad, store)
};

} // namespace dice::hfe::net
