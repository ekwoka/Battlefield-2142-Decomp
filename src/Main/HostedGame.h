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

#include "DemoCamera.h"
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
#include <tuple>
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
	// A retail server's pre-authorisation (Net/PlayerAuth.h), for trying a
	// login against the local services: the server sends its settings and
	// map list and waits for the client's player info (data block 1); the
	// name's checksum must match (else refused 0x24), the profile id be set
	// (0xd, as a ranked server: 0x004ae6a0), the CD-key answer pass
	// GameSpy's CD-key service at `cdKeyService` and the stats service at
	// `statsService` say "Ok" to ValidatePlayer for the GP login ticket.
	// Only then is the player made (CreatePlayer, the level). The retail
	// server asks the last two without waiting and bans afterwards
	// (admin/standard_admin/playerconnect.py); here they are asked first.
	struct LoginCheck {
		bool required = false;
		std::string cdKeyHost, statsHost;
		uint16_t cdKeyPort = 0, statsPort = 0;
	} login;
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
	// The terrain the teams' overhead cameras are kept over (0x005f8170:
	// 80 m inside an extent over 160 m; none if never set).
	void setTerrainExtent(float originX, float originZ, float extent)
	{
		m_terrainOrigin[0] = originX;
		m_terrainOrigin[1] = originZ;
		m_terrainExtent = extent;
	}
	// A team's overhead camera object (MultiPlayerOverheadCamera, class
	// 0x948c), 0 before its commander first moved it.
	uint32_t overheadCamera(int team) const { return team == 1 || team == 2 ? m_overheadCameras[team] : 0; }
	// One simulation tick (call before GameWorld::tick): takes what the
	// clients sent and sends them the world.
	void tick();
	size_t clientCount() const;
	// A client's spectator camera as JoinAsSpectator last set it (the mode,
	// 0 none; the player followed, -1 none); false for no such player.
	bool spectatorCamera(int player, int& mode, int& follow) const;
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
	// The host's own player votes as a client's Vote event does (0x00590230
	// with the host's player); the replies meant for it (to everyone, or to
	// a player no client has: the host's) come to onVoteReply, as the Vote
	// events a client's vote object (0x005740e0) takes.
	void localVote(int player, int kind, int phase, int target, int choice);
	std::function<void(const game::Votes::Reply&)> onVoteReply;
	// The squads' requests (the round's store, GameWorld::squadRequests):
	// each new one goes to every client (IssueSquadOrder with store 2, as
	// 0x004c5720 broadcasts it), with 2/0x1e to its squad's leader and
	// 2/0x1d to the team's commander when they are clients' players; each
	// one taken off goes as SetAcceptOrder (0x004c3300). A new one whose
	// commander is the host's own player and whose leader isn't comes to
	// onRequestStored (the host's 2/0x1d).
	std::function<void(int team, int squad, int kind)> onRequestStored;
	// The Spotted event's message for the host's own player (0x0050e910
	// posts 2/<message> there): a radio call's code (0x33..0x36) or a spot
	// (0x2e), the reporter's soldier ghost in `spotter`.
	std::function<void(const net::SpottedInfo&)> onSpotted;
	// The host's own player's report (the game's slots 0xd8 / 0xd4 on a
	// server: the event handled at once).
	void spotted(const net::SpottedInfo& spot, int reporter);
	// The player whose soldier (or seat) a ghost id is, -1 for none.
	int playerOfGhost(uint32_t ghost) const;
	// The radio (net::RadioMessageInfo): the game logic's voice message
	// table by name and by index (Sound/VoiceMessages.con's addMessage
	// order, 0x004d8af0; the server has it too, 0x004cdab0). A client's
	// RadioMessage is said in the round by the connection's player
	// (0x0050d3b0 → GameWorld::radioMessage); every message a client's
	// player hears in the round goes to that client (0x004a6c60), the
	// sender's id and team in it (0xff and 0: the game's own, source 0).
	std::function<int(const std::string& voice)> radioIndex;
	std::function<std::string(int index)> radioVoice;
	// A client's commander's tool used (net::CommanderToolUse: Artillery,
	// Uav, SupplyDrop, Titan; 0x0050ee10, 0x0050eb90, 0x00511220,
	// 0x0050f1f0), after the round took it, and the Titan's answer.
	std::function<void(int player, const net::CommanderToolUse& use, bool done)> onCommanderTool;
	// Demo recording (demo.recordDemo / stopRecording; Net/Demo.h): what
	// the server sends a client with no player, connection 0x81, into a
	// .bf2142demo file. False if one is recording or the file exists.
	// `cameraPath`: demo.recordDemo's camera file (demoCameraPath), kept
	// with a local player; the AutoRecorder (0x00585860, the server's own
	// 0x004afcc0) makes none.
	bool recordDemo(const std::string& path, const std::string& cameraPath = {});
	void stopDemo();
	bool recordingDemo() const { return m_demo.isOpen(); }
	// The local player's view for the demo's camera file (demo.recordDemo
	// 0x00432cc0: with a local player, DAT_00a85228 slot 0x30, the demo
	// object records <name>-def.bf2142cam too, cameramode 1; 0x005f9630
	// takes the player's camera, its control object's camera matrix, or
	// (0, 100, 0) unturned without one). False: no local player.
	std::function<bool(net::CameraKeyframe& frame)> localCamera;
	bool recordingCamera() const { return m_recordingCamera; }
	const std::string& cameraPath() const { return m_cameraPath; }
	const DemoCameraRecorder& cameraRecorder() const { return m_cameraRecorder; }
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
		// A projectile of the round's (not an object of its object
		// manager): its serial (game::Projectile::serial), keyed in
		// m_ghosts as kProjectileKey | serial.
		uint32_t projectileSerial = 0;
	};
	static constexpr uint32_t kProjectileKey = 0x80000000u;
	// The round's projectiles with a networkable (GenericProjectile with a
	// networkableInfo: missiles, grenades, mines, C4...; FireComp makes
	// them on the server only, 0x005f31d0 / 0x005f1440) as ghosts.
	void trackProjectiles();
	// A sticky round stuck (StickyCollisionComp 0x005e81a0 -> the
	// StickyProjectile event, 0x0050f850): to everyone once, and to a
	// joining client for each one stuck to an object (ServerConnection
	// 0x0057dda0). The rewrite's rounds ride the whole vehicle, so the
	// event names the vehicle's root ghost, part 0, bundle 0 (the root).
	net::GameEvent stickyEvent(const game::Projectile& p) const;
	std::set<uint32_t> m_stuckSent; // the serials sent
	// CommanderCam (0x00510b30 -> 0x005f8410 / 0x005f8170): the team's
	// overhead camera, made on first use (squad manager slot 0x7c,
	// 0x004c5120), over (x, z).
	void moveOverheadCamera(int team, float x, float z);
	uint32_t m_overheadCameras[3] {};
	float m_terrainOrigin[2] {};
	float m_terrainExtent = 0;
	void sendBattlefieldReports();
	const game::Projectile* projectileOf(const Ghost& ghost) const;
	struct Client {
		int player = -1;
		std::string name;
		int team = 0;
		// JoinAsSpectator (0x005142a0): a spectator, its camera's mode (the
		// kinds other than 8: 1 free, 6 following) and the player followed.
		bool spectator = false;
		int cameraMode = 0, cameraFollow = -1;
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
		// Each player's reload presses this client has had a state with
		// mask 0x10000 for (the exe's per-client dirty mask keeps the bit
		// until it goes out).
		std::map<int, uint32_t> reloadsSent;
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
	void sendJoinBlocks(int client, bool withSettings = true);
	void sendSettings(int client);
	void playerInfo(int client);
	std::set<int> m_awaitingLogin; // connections whose player info isn't checked yet
	std::vector<int> m_loginChecks; // player infos come, checked after the poll (a refusal drops the connection)
	void sendTo(int client, const net::GameEvent& event); // the demo's connection: its file
	void flushDemo(int32_t time, bool ghosts);
	static constexpr int kDemoClient = 0x81;
	net::DemoWriter m_demo;
	bool m_recordingCamera = false;
	std::string m_cameraPath;
	DemoCameraRecorder m_cameraRecorder;
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
	bool clientHas(int player) const;
	// The teams' networkable (ghost net::kTeamStateGhost): its state now.
	std::vector<uint8_t> teamStateOf(size_t& bits) const;
	// The spawn groups (CreateSpawnGroup / RemoveSpawnGroup) and the
	// requests (IssueSquadOrder / SetAcceptOrder) sent, to send the changes.
	std::map<int, net::SpawnGroupInfo> m_spawnGroups;
	std::vector<net::SpawnGroupInfo> spawnGroupsNow() const;
	void trackSpawnGroups();
	std::map<std::tuple<int, int, int>, physics::Vec3> m_requestsSent;
	void trackRequests();
	void sendRadio();
	// The offers of command made (PostRemote 2/0x23 to the candidate,
	// 0x004c5070), per team.
	std::array<int, 3> m_offerSent {-1, -1, -1};
	void trackCommanderOffers();
	struct OrderSent {
		int kind = 0;
		physics::Vec3 at;
		bool accepted = false;
	};
	std::map<std::tuple<int, int, int>, OrderSent> m_ordersSent; // (team, squad, store)
	void trackOrders();
	void commanderTool(int player, const net::CommanderToolUse& use);
	net::GameEvent requestEvent(int team, int squad, int kind, const physics::Vec3& at) const;
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
	std::vector<uint8_t> playerStateOf(int player, size_t& bits, bool everyGroup = false) const;
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
	std::map<int, uint32_t> m_invites;  // per player: the squads that invited it (player +0x224's mask)
	uint16_t m_nextGhost = kFirstGhost;
	// The teams' networkable takes 0x201 first (0x0042b8b0), so the ghost
	// manager hands out ids from 0x202 (0x00678610).
	static constexpr uint16_t kFirstGhost = net::kTeamStateGhost + 1;
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
