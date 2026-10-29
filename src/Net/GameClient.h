// Playing on a server: the client side of a game connection after the
// handshake (BF2142.exe's ServerConnection 0x0057b3f0 and the game client
// "dice.bf.gameClient", vtable 0x00929028).
//
// update() (call it every frame; it never blocks):
//   - receives data packets, reads their input/event/ghost blocks
//     (GameProtocol.h), hands events to the RemoteWorld in block order
//     (the engine's 5-bit block sequence, 0x006447b0) and ghost blocks as
//     they come;
//   - answers the CD-key challenge (event 1) with event 2 and tells the
//     connection type (event 3), as 0x004b2790 / 0x004b48a0 do;
//   - sends a data packet every tick (30 a second by default; it also
//     carries our acknowledgements): once the server has made our player,
//     the input block with the last three moves (0x00590450), and, when there are
//     queued events, a reliable event block. Blocks whose packet the server
//     reports lost (its notify acknowledgement) go out again with the
//     resent bit, as 0x00644bf0 does; at most 15 blocks are in flight.
//
// Requests a player makes (team, kit, spawn point, chat, suicide) are
// queued as events. Joining a real server additionally needs an account
// login for the profile id and a valid CD key: see docs/engine.md.
#pragma once

#include "EventChannel.h"
#include "GameConnection.h"
#include "GameProtocol.h"
#include "RemoteWorld.h"

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe::net {

// The rates per connection type (0x00928408, six ints a row; 0x004b48a0
// applies a client's, 0x004a6160 a server's per client; type 6 is
// ignored). The last two (200, 200) go to the networkable manager's +0x50
// and +0x54.
struct ConnectionRates {
	int clientPacketsPerSecond, serverPacketsPerSecond;
	int clientMaxBytes, serverMaxBytes;
	int lagMs, value54;
};
constexpr ConnectionRates kConnectionRates[6] = {
	{10, 4, 160, 260, 200, 200}, {10, 4, 160, 260, 200, 200}, {30, 10, 160, 350, 200, 200},
	{30, 20, 320, 400, 200, 200}, {30, 20, 320, 512, 200, 200}, {30, 20, 320, 640, 200, 200}};
// How often a client sends: its table rate, and 0x004b3df0 also sends
// after each simulated tick (30 a second) for types other than 0.
int clientSendRate(uint32_t connectionType);

struct GameClientOptions {
	std::string cdKey = "XXXX-XXXX-XXXX-XXXX"; // answers the challenge; a real server checks it with GameSpy
	uint32_t profileId = 0xffffffff;           // -1 without an account login
	uint32_t connectionType = 4;               // 0-6 (event 3), an index into the rate settings
	std::string password;
	// Our name: sent as ChangePlayerName (0x0c) once the server has made our
	// player; the server takes it only for the sender's own player
	// (0x0050bb90 compares the id with the connection's). Empty: the
	// server's name for us stays.
	std::string playerName;
	// The input block's 9-bit value (0x0057b040): the networkable manager's
	// +0x50 (the rate table's 200 ms) plus 1000 x 0x006b4e70's
	// interpolation time (from settings at 0x00a83e4c-0x00a83e54, taken as 0).
	uint32_t lagCompensationMs = 200;
	// The game's folder on disk (holding mods/), for a pure server's
	// content check (ContentCheck.h): with it, each level of a server whose
	// settings ask for the check is answered, after its LoadComplete, with
	// the ContentCheck event of our archives' fingerprint for the level's
	// ordinal (0x004b5eb0 case 2 -> 0x0068ba40(0), 0x005123f0). Empty: no
	// answer, and a pure server kicks us (0x1b).
	std::string contentRoot;
	std::string modDirectory = "mods/bf2142";
};

class GameClient {
public:
	GameClient();
	// A client going away says so (the disconnect packet), as the game
	// does when it quits, so the server drops its player at once rather
	// than at its time-out.
	~GameClient();
	GameClient(const GameClient&) = delete;
	GameClient& operator=(const GameClient&) = delete;

	// Starts connecting; update() completes it.
	bool connect(const std::string& host, uint16_t port, const GameClientOptions& options = {});
	void disconnect();

	enum class State { Idle, Connecting, Connected, Refused, Failed, Disconnected };
	State state() const { return m_state; }
	const ConnectResult& connectResult() const { return m_connection.result(); }
	uint32_t disconnectReason() const { return m_connection.disconnectReason(); }

	// Receives, answers and sends what is due. Returns false once the
	// connection is over (refused, failed or disconnected).
	bool update();

	// Plays a demo instead of a connection (BF2142.exe's client pump with
	// no socket, 0x004b52e0): each update() takes `next`'s payloads (the
	// recorded data payloads, as a client would have received them) while
	// it returns 1; 0 is none due yet, -1 the end. Nothing is sent.
	void playDemo(std::function<int(std::vector<uint8_t>& payload)> next);
	bool playingDemo() const { return static_cast<bool>(m_demo); }

	RemoteWorld& world() { return m_world; }
	const RemoteWorld& world() const { return m_world; }
	// The game simulated a tick: our packet with this tick's move goes now
	// (0x004b3df0 sends after each simulated tick). With sendOnTicks set,
	// update() sends only while no ticks come (every 200 ms), so a move is
	// exactly one tick of the client's, as the server steps it.
	void simulatedTick();
	bool sendOnTicks = false;
	// Shortcut for world().setInput().
	void setInput(const PlayerInput& input) { m_world.setInput(input); }

	// Requests (queued reliable events).
	void sendEvent(const GameEvent& event) { m_events.queue(event); }
	void selectTeam(int team);                 // PostRemote 6/7 (0x004c1dc0)
	void selectSpawnGroup(int group);          // PostRemote 6/6 (0x004c1d50)
	void selectKit(int kit, const std::array<int32_t, 4>& unlocks = {-1, -1, -1, -1}); // SelectKit (0x3f)
	void suicide();                            // PostRemote 6/0xc (0x004c22e0)
	void say(const std::string& text, uint32_t channel = 0); // a StringBlock chat line (channel: ChatChannel)
	// A whisper (the HUD's 0x007d9e20: channel 14, kChatPlayer, through
	// the game client's slots 0x3c / 0x4c / 0x30, 0x004b2af0 -> the chat
	// queue 0x00643370): the header's byte 9 is the HUD manager's player
	// (+0x48), which 0x0075c6b0 sets to -1 and nothing changes, so 0xff.
	void whisper(const std::string& text, int target = -1);
	// "rcon <command>" typed in the console: PostRemote 6/0x12 with the
	// line (0x004b6630 case 0x12); the server's answers come back as 6/0x13
	// messages (world().takeMessages(): -1 and "rcon: <text>").
	void remoteCommand(const std::string& command);
	void toggleFreeCamera();                   // ToggleFreeCamera (0x28)
	// Squads (SquadManager): the team is our player's, the player us unless
	// given. A squad must exist to be joined (0x004c6450 looks it up). A
	// squad is made by naming one without a leader (createSquad).
	void joinSquad(int squad);                                    // JoinSquad (0x10; sent by 0x004c9bf0): team, player, squad
	void leaveSquad(int squad, int player = -1, bool flag = false); // LeaveSquad (0x11; 0x004c9c50); another player: the leader kicks
	void invite(int player, int squad, bool fromLeader = true);   // Invite (0x19; server 0x0050e700)
	void setSquadLeader(int player, int squad);                   // SetSquadLeader (0x1c)
	void setSquadName(int squad, const std::string& name);        // ChangeSquadName (0x16): squad, team, our id, name (at most 31)
	// A new squad: ChangeSquadName for an empty squad (0x0050da80 ->
	// 0x004c6720: the squad must exist and have no members and the name be
	// unused, else PostRemote 2/0xf). The server names it, joins us to it
	// (0x004c7290), broadcasts the name and answers PostRemote 2/0xe.
	void createSquad(int squad, const std::string& name) { setSquadName(squad, name); }
	void setPrivateSquad(int squad, bool isPrivate);              // SetPrivateSquad (0x17; leader only)
	void squadRequest(int type);                                  // Request (0x32): 0-3, to the commander (leader only)
	// An asset request to the commander (SquadLeader.sendRequest: the
	// ClientSquadManager's slot 0x50, 0x004c9b00, sends IssueSquadOrder with
	// store 2): kind 0 artillery, 1 orbital strike, 2 UAV, 3 supplies, at a
	// point; our team and squad.
	void requestAsset(int kind, const Vec3f& point);
	// The commander's answer to a squad's request (0x007c7e20 → the squad
	// manager's slot 0x24, 0x004c99a0: SetAcceptOrder with 0, store 2 and
	// the kind), which takes it off on the server and every client.
	void answerRequest(int squad, int kind);
	// An order (IssueSquadOrder, 0x0050e170): store 0 the squad leader's to
	// his own squad (squad manager slot 0x48), 1 the commander's to a squad
	// (slot 0x4c); kind the order kind (0..3 the issue-order menu's, 4..7
	// placed on the map), at a point. The server takes it from the squad's
	// leader or the team's commander (0x0050e3a0).
	void issueOrder(int store, int squad, int kind, const Vec3f& point);
	// The squad leader's answer to the commander's order (SetAcceptOrder,
	// store 1, the bit set for an accept; Commander.commRoseAccept).
	void answerOrder(int squad, int kind, bool accept);
	// Commander (0x13; 0x0050c8f0): a kind (0-8), our team, our player id.
	enum CommanderKind { kCommanderStepDown = 0, kCommanderWithdraw = 1, kCommanderResign = 2, kCommanderSet = 3,
		kCommanderApply = 4, kCommanderLeaveScreen = 6, kCommanderEnterScreen = 7 };
	void commander(int kind);
	void applyForCommander() { commander(kCommanderApply); }  // HUD "commanderApply" (SquadManager 0x60)
	void resignCommander() { commander(kCommanderResign); }   // HUD "commanderResign"
	// Votes (0x27; writer 0x005113c0, server 0x00590230): target, choice,
	// top count, target again, top choice, votes needed, voter, phase,
	// type, a 32-bit value (0 from every builder). Types: 0 internal, 1 map
	// (target a map-list index), 2 kick (a player), 3 mutiny (the
	// commander). Phases: 0 start, 1 failed, 2 progress / ended, 3 passed,
	// 4 cast, 5 bad target, 6 refused (one running or too soon), 7 too few
	// players to vote (sv.minPlayersForVoting; the server's game::Votes).
	void startVote(int type, int target);
	void castVote(int type, int target, int choice);
	// A radio / comm-rose message (0x14; the client's slot 0x50,
	// 0x004b2b60 → 0x004b26b0, written by 0x0050d1f0): the voice message's
	// index in the game logic's table (16 bits), our player and team, the
	// team, squad or player the channel names, the target channel and the
	// source 3 (a player's). The server hands it to those who hear it
	// (RadioMessageInfo).
	void radioMessage(int channel, int index, int value = 0);
	// The commander's tools (GameProtocol.h's makeArtilleryUse..): the
	// artillery and the orbital strike as Artillery (rcType 1 / 4), the UAV
	// and the satellite scan as Uav, the supply drop as SupplyDrop and the
	// Titan's move as Titan kind 1, for our team at x, z.
	void commanderTool(CommanderToolUse::Tool tool, float x, float z);
	// The answer to the offer of command (PostRemote 2/0x23): the
	// Commander event's kind 0 accepts (0x004c8c20), 1 declines.
	void answerCommanderOffer(bool accept) { commander(accept ? kCommanderStepDown : kCommanderWithdraw); }
	// Answers the original client gives by itself (all on by default):
	// PostRemote 6/2 (LoadComplete) for each level block (data block 2),
	// echoes of 6/4 (DatabaseComplete) and 6/5 (Reset, the networkable
	// checksum: ours isn't computed), and the connection type again on 6/3
	// (StartSimulation, 0x004b6630).
	bool autoAnswer = true;

	// Observers (logging): each event as it is handled, in block order, and
	// each received payload as decoded.
	std::function<void(const GameEvent&)> onEvent;
	std::function<void(const GamePayload&)> onPayload;
	// Each move as it goes up (once, with its index): the client's
	// prediction steps it (src/Main/Prediction.h).
	std::function<void(const PlayerMove&)> onMoveSent;

	// Counters for diagnostics.
	size_t packetsReceived() const { return m_packetsReceived; }
	size_t packetsSent() const { return m_packetsSent; }
	size_t eventsReceived() const { return m_eventsReceived; }
	size_t undecodedPayloads() const { return m_undecoded; }
	size_t blocksResent() const { return m_events.blocksResent(); }
	// The last payload whose events couldn't all be decoded (for logging).
	const std::vector<uint8_t>& lastUndecodedPayload() const { return m_lastUndecoded; }

private:
	void onPacket(const ConnectedPacket& packet);
	void handlePayload(const std::vector<uint8_t>& payload, unsigned lengthBits = kGhostStateLengthBits);
	std::function<int(std::vector<uint8_t>&)> m_demo;
	void handleEvents(const ReliableEventBlock& block);
	void deliver(const GameEvent& event);
	void sendTick();
	void answer(const GameEvent& event);
	uint32_t m_levelsAnswered = 0;

	GameConnection m_connection;
	GameClientOptions m_options;
	State m_state = State::Idle;
	RemoteWorld m_world;

	// Our events and the server's, in reliable ordered blocks.
	EventChannel m_events;
	bool m_nameSent = false;

	std::deque<PlayerMove> m_moves; // the last three
	int32_t m_moveIndex = 0;
	std::chrono::steady_clock::time_point m_lastTick;
	std::chrono::steady_clock::time_point m_lastPing; // 0x0068f280 pings every 3 s

	size_t m_packetsReceived = 0, m_packetsSent = 0, m_eventsReceived = 0, m_undecoded = 0;
	std::vector<uint8_t> m_lastUndecoded;
};

} // namespace dice::hfe::net
