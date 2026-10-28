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
// the client's soldier (or the vehicle it sits in) and the updates of the
// ghosts whose state changed (and of every ghost again every two
// seconds), as much as a packet holds; created and removed objects,
// players' spawns and seat changes as events. The client's moves drive its
// player's soldier (the move's inputs as the server's PlayerInput,
// 0x005838d0), its events pick its kit (SelectKit, 0x0050b010), spawn
// point (6/6), team (6/7, before its first spawn), name (ChangePlayerName,
// 0x0050bb90), squad (JoinSquad, LeaveSquad), suicide (6/0xc) and chat
// (the StringBlock chat lines, sent on to everyone).
//
// The control-object record carries the position and ghost only: the
// mode-3 prediction state after them (0x00580cd0) is left empty, so the
// rewrite's client takes the server's position (an original client would
// read prediction bits there).
#pragma once

#include "Game/GameWorld.h"
#include "Net/GameServer.h"
#include "Net/GhostLayout.h"
#include "Net/RemoteWorld.h"
#include "Net/ServerQuery.h"

#include <cstdint>
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
	uint16_t gamePort() const { return m_server.port(); }
	// Objects the clients load themselves (the level's static objects): never ghosts.
	void setStaticObjects(std::set<uint32_t> ids) { m_static = std::move(ids); }
	// One simulation tick (call before GameWorld::tick): takes what the
	// clients sent and sends them the world.
	void tick();
	size_t clientCount() const;
	size_t ghostCount() const { return m_ghosts.size(); }
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
		enum class Stage { Connected, Loading, Database, Playing } stage = Stage::Connected;
		std::set<uint16_t> created;              // the ghosts it was told of
		std::map<uint16_t, std::vector<uint8_t>> sent; // each ghost's last state sent
		std::map<uint16_t, uint32_t> sentTick;   // and when
		size_t cursor = 0;                       // round-robin over the ghosts
		float pitch = 0;                         // the aim, as the moves turn it
		std::vector<net::PlayerMove> moves;      // this tick's
		std::string chat;                        // a StringBlock coming in
		uint32_t chatSize = 0;
		bool spawnRequested = false;
		game::SoldierInput held;                 // the last move's held inputs
		bool useWas = false;
		int seatWas = 0, itemWas = 0;
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
	void applyMoves();
	void trackObjects();
	void trackPlayers();
	net::GhostBlock ghostsFor(int client);
	std::vector<net::GameEvent> createEvents(const Ghost& ghost) const;
	net::GameEvent createPlayerEvent(const game::GameWorld::PlayerInfo& player, bool own, uint16_t ghost) const;
	std::vector<uint8_t> stateOf(const Ghost& ghost, size_t part, size_t& bits) const;
	std::vector<uint8_t> playerStateOf(int player, size_t& bits) const;
	const std::vector<net::GhostPartLayout>* layoutFor(const world::ObjectTemplate& t);
	uint16_t ghostOfObject(uint32_t object, int part = 0) const;
	std::string nameOf(int player) const;
	void sendToPlaying(const net::GameEvent& event);
	net::ServerReport report() const;

	game::GameWorld& m_round;
	game::GameLogic& m_logic;
	world::ObjectTemplateManager& m_templates;
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
	std::vector<int> m_freePlayers;      // players of clients that left, for the next to join
	uint32_t m_tick = 0;
	bool m_roundEndSent = false;
};

} // namespace dice::hfe::host
