// A local game served to other bf2142 clients (`bf2142 --host`): the game
// server's side of BF2142.exe's protocol on top of the rewrite's own round
// (game::GameWorld and GameLogic), so `bf2142 --connect 127.0.0.1` (or the
// menu's LOCAL page) can join it.
//
// What a joining client gets, in the original's order (the game server's
// PostRemote handler 0x004b0690 and its helpers):
//   - after the connection type (net::GameServer): the server settings
//     (data block 0, 0x00515bc0's layout), the map list (block 5,
//     0x00441410) and the level to load (block 2, 0x005156f0);
//   - its LoadComplete (6/2, 0x004a7c00 counts the client's pending level
//     loads down): the players (CreatePlayer, its own with the remote bit
//     clear), every replicated object (CreateObject, one ghost per
//     PlayerControlObject of its tree, net/GhostLayout.h), the soldiers
//     players hold (HandlePickup), then DatabaseComplete (6/4) and the
//     networkable checksum (6/5);
//   - its echo of 6/4 (0x004af710): StartSimulation (6/3) and the game
//     state (0/1, the game logic's slot 0x164), then BeginRound.
// From then on, each tick: ghost blocks with the control-object record of
// the client's soldier (or the seat it sits in) and the updates of the
// ghosts whose state changed (and of every ghost again every two
// seconds), as much as a packet holds; created and removed objects,
// players' spawns and seat changes as events. The client's moves drive its
// player's soldier, one move a tick (the move's inputs as the server's
// PlayerInput, 0x005838d0, through net::soldierInputFromMove), its events
// pick its kit (SelectKit, 0x0050b010), spawn point (6/6), team (6/7,
// while it has no soldier: the game logic's slot 0x60), name
// (ChangePlayerName, 0x0050bb90), squad (JoinSquad, LeaveSquad), suicide
// (6/0xc) and chat (the StringBlock chat lines, sent on to everyone).
//
// The control-object record (0x00580830) carries the last move the
// server stepped, the position and ghost, and the prediction state: every
// group of the record's ghost in mode 3 (a soldier's physics node and
// control state at full precision; a seat's, with the vehicle's root
// ghost as the second ghost) and the player's own state (the player's
// vtable 0x1c8: spawn tick, man down, health, the weapon in hand's rounds),
// which a client predicting its soldier (Prediction.h) corrects itself by.
// Every group of every ghost goes out (the turrets' and wheels' poses,
// the seats' occupants, the armoured parts' health), not only the roots'.
//
// A map change (changeLevel) keeps the connections: the new level's block
// goes to every client, which loads it and answers LoadComplete, and the
// join's database follows on the new round, as a map change does on the
// original server (0x004abbc0(-1) broadcasts the level block and counts a
// pending load per connection).
#pragma once

#include "Game/GameWorld.h"
#include "Net/GameServer.h"
#include "Net/GhostLayout.h"
#include "Net/RemoteInput.h"
#include "Net/RemoteWorld.h"
#include "Net/ServerQuery.h"

#include <cstdint>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dice::hfe::game {
class GameLogic;
} // namespace dice::hfe::game

namespace dice::hfe::host {

struct HostSetup {
	std::string serverName = "bf2142-recomp server";
	std::string level;      // the level's folder name ("Camp_Gibraltar")
	std::string gameMode;   // "gpm_cq"
	int mapSize = 32;
	int maxPlayers = 32;
	uint16_t gamePort = 17567;
	uint16_t queryPort = 29900; // 0: no LAN query responder
	bool loopbackOnly = true;
	std::string password;
};

// The engine's yaw/pitch/roll (degrees) as the networkables' quaternion
// and back (physics::Mat3's convention, rows acting on row vectors).
net::Quat quaternionFromYawPitchRoll(float yaw, float pitch, float roll);
void yawPitchRollFromQuaternion(const net::Quat& q, float& yaw, float& pitch, float& roll);

class HostedGame {
public:
	HostedGame(game::GameWorld& round, game::GameLogic& logic, world::ObjectTemplateManager& templates, HostSetup setup);
	~HostedGame();
	bool start();
	// A new level (the next map, or the same one again for a new round) on
	// the same connections: the clients get its level block and join it.
	void changeLevel(game::GameWorld& round, game::GameLogic& logic, world::ObjectTemplateManager& templates,
		HostSetup setup);
	// Between levels: nothing of the old round is touched until changeLevel.
	void detach() { m_attached = false; }
	// Ends a player's connection: KickBan (0x37; 0x0050cf30 writes the
	// player, whether banned and a byte, and a client's handler 0x0050cfd0
	// posts it as message 2/0x4d for the HUD) to everyone, then the
	// disconnect (type 5, sent three times as 0x0068e520 does).
	bool kickPlayer(int player, bool ban = false, uint32_t reason = 0);
	uint16_t gamePort() const { return m_server.port(); }
	// Objects the clients load themselves (the level's static objects): never ghosts.
	void setStaticObjects(std::set<uint32_t> ids) { m_static = std::move(ids); }
	// One simulation tick (call before GameWorld::tick): takes what the
	// clients sent and sends them the world.
	void tick();
	size_t clientCount() const;
	size_t ghostCount() const { return m_ghosts.size(); }
	uint32_t tickCount() const { return m_tick; } // the ghost blocks' tick (the last sent)
	// Log lines (joins, leaves, chat).
	std::function<void(const std::string&)> onLog;

private:
	struct Ghost {
		uint32_t object = 0;
		uint16_t first = 0; // its ghost ids: first .. first + layout size - 1
		const std::vector<net::GhostPartLayout>* layout = nullptr;
		const world::ObjectTemplate* objectTemplate = nullptr;
	};
	struct Client {
		int player = -1;
		std::string name;
		int team = 0;
		enum class Stage { Connected, Loading, Database, Playing } stage = Stage::Connected;
		std::set<uint16_t> created;              // the ghosts it was told of
		std::map<uint16_t, std::vector<uint8_t>> sent; // each ghost's last state sent
		std::map<uint16_t, uint32_t> sentTick;   // and when
		size_t cursor = 0;                       // round-robin over the ghosts
		std::deque<net::PlayerMove> moves;       // moves waiting, one a tick
		net::MoveInputState moveState;           // the aim and the keys held
		int32_t stepping = -1;                   // the move the round steps this tick
		int32_t stepped = -1;                    // the last move the round has stepped (the record's)
		bool starved = true;                     // no move was waiting: wait for two
		uint32_t movesDropped = 0, ticksStarved = 0; // for the log
		int historySlot = 7;                     // the record's slot (0x00580830: +0x20f8)
		// The relative records (0x00580830's other path): the slot the
		// client has acknowledged (+0x20f0, -1 none), how many relative
		// records are left before the next fresh one (+0x20fc), our copy
		// of each slot's state (+0x20f4: 0x0057f750), the fresh slot each
		// unsettled packet carried, and the record's ghost they belong to.
		int ackedSlot = -1;
		int relativeLeft = 0;
		std::array<std::vector<net::MotionHistory>, 8> slots;
		std::map<uint32_t, int> freshIn;
		uint16_t recordGhost = 0;
		uint32_t freshRecords = 0, relativeRecords = 0;
		std::string chat;                        // a StringBlock coming in
		uint32_t chatSize = 0;
		bool spawnRequested = false;
		game::SoldierInput held;                 // the last move's held inputs
	};
	struct PlayerSeen {
		uint16_t ghost = 0; // its networkable's
		bool alive = false;
		uint32_t soldier = 0, vehicle = 0;
		int seat = -1;
		int kills = 0, deaths = 0, teamKills = 0;
	};
	void joined(int client);
	void left(int client);
	void event(int client, const net::GameEvent& event);
	void sendDatabase(int client);
	void sendLevel(int client); // the map list and the level block
	void applyMoves();
	void trackObjects();
	void trackPlayers();
	net::GhostBlock ghostsFor(int client);
	std::vector<net::GameEvent> createEvents(const Ghost& ghost) const;
	net::GameEvent createPlayerEvent(const game::GameWorld::PlayerInfo& player, bool own, uint16_t ghost) const;
	std::vector<uint8_t> stateOf(const Ghost& ghost, size_t part, size_t& bits) const;
	// Every group's state of one of a ghost's parts (mode 2 or 3).
	std::vector<net::NetworkableState> statesOf(const Ghost& ghost, size_t part, unsigned mode) const;
	net::NetworkableState soldierState(uint32_t soldier, const world::ObjectTemplate& t, const net::NetworkableHints& h,
		unsigned mode) const;
	net::PlayerOwnState ownState(int player) const;
	const Ghost* ghostOf(uint16_t ghost, size_t& part) const;
	std::vector<uint8_t> playerStateOf(int player, size_t& bits) const;
	const std::vector<net::GhostPartLayout>* layoutFor(const world::ObjectTemplate& t);
	uint16_t ghostOfObject(uint32_t object, int part = 0) const;
	std::string nameOf(int player) const;
	void sendToPlaying(const net::GameEvent& event);
	net::ServerReport report() const;

	game::GameWorld* m_round;
	game::GameLogic* m_logic;
	world::ObjectTemplateManager* m_templates;
	bool m_attached = true;
	struct Motion {
		physics::Vec3 velocity, angularVelocity;
	};
	std::map<uint32_t, Motion> m_motion; // the vehicles', this tick
	HostSetup m_setup;
	net::GameServer m_server;
	net::QueryServer m_query;
	std::set<uint32_t> m_static;
	std::map<uint32_t, std::unique_ptr<std::vector<net::GhostPartLayout>>> m_layouts; // by template id
	std::map<uint32_t, Ghost> m_ghosts; // by object id
	uint16_t m_nextGhost = 1;
	std::map<int, Client> m_clients;
	std::map<int, PlayerSeen> m_players; // by player id
	std::map<int, std::string> m_names;  // names the clients gave
	uint32_t m_tick = 0;
	bool m_roundEndSent = false;
};

} // namespace dice::hfe::host
