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
#include "Game/Votes.h"
#include "Net/ContentCheck.h"
#include "Net/Demo.h"
#include "Net/EventChannel.h"
#include "Net/GameServer.h"
#include "Net/GhostLayout.h"
#include "Net/GhostPriority.h"
#include "Net/RemoteInput.h"
#include "Net/RemoteWorld.h"
#include "Net/ServerQuery.h"
#include "Net/ServerSettings.h"

#include <cstdint>
#include <ctime>
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
} // namespace dice::hfe

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
	std::string modDirectory = "mods/bf2142"; // GSModDirectory: the auto demos' root
	std::string password;
	// A server's configuration (Main/ServerAdmin.h), when it has one: the
	// settings block's fields, the whole map list (block 5, with the map
	// running), the query's keys, and the bans checked on connecting.
	const net::ServerSettings* settings = nullptr;
	const net::MapList* maps = nullptr;
	const net::BanList* bans = nullptr;
	// reservedSlots.* (with sv.numReservedSlots): a listed nick joining a
	// full server bumps someone who isn't (0x004ae6a0).
	const net::ReservedSlots* reserved = nullptr;
	// The game's folder on disk (holding mods/): a mod with std_archive.md5
	// there makes the server pure (net/ContentCheck.h, 0x004adb60): its
	// clients must send their content's fingerprint or are kicked (0x1b).
	// Empty: not pure.
	std::string contentRoot;
};

// The engine's yaw/pitch/roll (degrees) as the networkables' quaternion
// and back (physics::Mat3's convention, rows acting on row vectors).
net::Quat quaternionFromYawPitchRoll(float yaw, float pitch, float roll);
void yawPitchRollFromQuaternion(const net::Quat& q, float& yaw, float& pitch, float& roll);

// NetworkableInfo.* (Objects/Common/Networkables.con; BF2142.exe
// 0x0067de4c createNewInfo, 0x0067df7d setPredictionMode, 0x0067e06b
// setBasePriority, 0x0067e14e setIsUnique, 0x0067e238 setForceNetworkableId):
// into net::networkableInfos().
void registerNetworkableInfoConsoleMethods(Console& console);
// The AutoRecorder's file (0x00585620): <mod>/demos/auto_<local time as
// %Y_%m_%d_%H_%M_%S>.bf2142demo.
std::string autoDemoPath(const std::string& modDirectory, std::time_t now);

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

	// The connected clients' players (admin.listPlayers, the bans).
	struct ClientPlayer {
		int player = -1;
		std::string name;
		std::string address;   // "a.b.c.d:port"
		std::string cdKeyHash; // the challenge response's (GameServer::ClientInfo)
	};
	std::vector<ClientPlayer> clientPlayers() const;
	// A server message (Game.sayAll / sayTeam): a chat line on channel 12
	// (13 for a team) from player 0xff with source 1, as the exe's server
	// messages go (GameProtocol.h ChatMessage).
	void sayAll(const std::string& text);
	void sayTeam(int team, const std::string& text);
	// The remote console in the game: a client's "rcon <command>" comes as
	// message 6/0x12 (the client's 0x004b6630 case 0x12 sends the line;
	// the server's 0x004b0690 case 0x12 raises the scripts' RemoteCommand
	// with the player), the reply as 6/0x13 (host.rcon_feedback, 0x00442940:
	// "rcon: <text>").
	std::function<void(int player, const std::string& command)> onRemoteCommand;
	void sendRemoteFeedback(int player, const std::string& text);
	// A chat line a client said (for the scripts' ChatMessage: player, text,
	// channel, flags).
	std::function<void(int player, const net::ChatMessage& line)> onChat;
	// The votes' results (game::Votes, the server's 0x0058fb90): a map vote
	// passed (5 s on, the map-list index to run), a mutiny against a team's
	// commander. A kick vote kicks here ("KICKED", 0x15).
	std::function<void(int mapIndex)> onMapVote;
	std::function<void(int team)> onMutiny;
	game::Votes& votes() { return m_votes; }
	// Demo recording (demo.recordDemo / stopRecording; Net/Demo.h): what
	// the server sends a client with no player, connection 0x81, into a
	// .bf2142demo file. False if one is recording or the file exists.
	bool recordDemo(const std::string& path);
	void stopDemo();
	bool recordingDemo() const { return m_demo.isOpen(); }
	// sv.autoRecord's AutoRecorder (AutoRecorder.cpp; constructor 0x005854a0,
	// at the server's +0x164): a demo of each round from its start (0x00585860,
	// when the game logic turns to playing, 0x004c03b0) to its restart
	// (0x00585920, from 0x004ba0c0), named <mod>/demos/auto_<date>.bf2142demo
	// (0x00585620), then handed to sv.autoDemoHook.
	const std::string& autoDemo() const { return m_autoDemo; }
	const std::string& lastAutoDemo() const { return m_lastAutoDemo; }

private:
	struct Ghost {
		uint32_t object = 0;
		uint16_t first = 0; // its ghost ids: first .. first + layout size - 1
		const std::vector<net::GhostPartLayout>* layout = nullptr;
		const world::ObjectTemplate* objectTemplate = nullptr;
		// Its networkable info's base priority (copied in by 0x006fa190):
		// under 0 always ghosted (c_NIGhostAlways), 0 never scored.
		float basePriority = 1;
		bool projectile = false; // interface 0x94c1: +0.3 (0x0057d560)
	};
	struct Client {
		int player = -1;
		std::string name;
		int team = 0;
		enum class Stage { Connected, Loading, Database, Playing } stage = Stage::Connected;
		std::set<uint16_t> created;              // the ghosts it was told of
		std::map<uint16_t, std::vector<uint8_t>> sent; // each ghost's last state sent
		// The client's GhostManager (net::GhostScheduler): its states, the
		// rescore timer (connection +0x20, every 0.1 s), what each packet
		// carried (by its sequence; kind 3 as the ghost | 0x10000) and the
		// packet that last carried each ghost, for a loss (0x005830e0).
		net::GhostScheduler ghosts;
		float rescoreIn = 0;
		std::map<uint32_t, std::vector<uint32_t>> carried;
		std::map<uint16_t, uint32_t> lastCarried;
		std::vector<uint16_t> lostRemovals; // written first (gm +0x10c0)
		bool hasEye = false;
		net::GhostEye eye;
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
		bool contentChecked = false; // the connection's slot 0x28(1): its ContentCheck matched (0x004aec40)
		// Joined while the server was full: admitted or refused once its name
		// comes (the exe's join check has the name; the rewrite's clients
		// send it after connecting).
		bool full = false;
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
	bool reservedSlotCheck(int client);
	void sendJoinBlocks(int client);
	void sendTo(int client, const net::GameEvent& event); // the demo's connection: its file
	void flushDemo(int32_t time, bool ghosts);
	static constexpr int kDemoClient = 0x81;
	net::DemoWriter m_demo;
	net::EventChannel m_demoEvents;
	uint32_t m_demoSequence = 0;
	float m_demoSendIn = 0; // the demo connection's pacing (sv.demoQuality)
	std::string m_autoDemo, m_lastAutoDemo; // the AutoRecorder's file and its last (+0x10, +0x2c)
	int m_lastRoundState = -1;
	std::shared_ptr<std::atomic<bool>> m_hookBusy = std::make_shared<std::atomic<bool>>(false);
	void startAutoDemo();
	void stopAutoDemo();
	game::Votes m_votes;
	void sendVoteReplies(const std::vector<game::Votes::Reply>& replies);
	std::vector<std::pair<float, int>> m_mapVotes; // (seconds left, map) passed, waiting the 5 s
	std::vector<std::pair<int, uint32_t>> m_kickLater; // (client, reason) after the poll
	void event(int client, const net::GameEvent& event);
	void sendDatabase(int client);
	void sendLevel(int client); // the map list and the level block
	void applyMoves();
	void trackObjects();
	void trackPlayers();
	net::GhostBlock ghostsFor(int client, size_t budgetBits);
	void rescore(Client& client);
	void ghostLost(Client& client, uint32_t sequence, bool delivered);
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
	// The pure-server check: the mod's and the level's fingerprints, and
	// the level's ordinal (0x004aefe0: rand() % 10 after srand(ms)).
	net::ContentChecker m_content;
	int m_ordinal = 0;
	void startContentLevel();
	void checkContent(int id, const net::GameEvent& e);
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
	// Each ghost id's state this tick, shared by the clients.
	std::map<uint16_t, std::pair<std::vector<uint8_t>, size_t>> m_stateCache;
	bool m_roundEndSent = false;
	int m_endOfRoundSerialSent = 0;
};

} // namespace dice::hfe::host
