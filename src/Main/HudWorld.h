// What the HUD reads of the game it shows: the players, the teams'
// tickets, the control points and spawn groups, the squads' requests and
// the local player's soldier, vehicle and weapon. BF2142.exe's HUD reads
// the same objects on a server and on a client: the player manager's
// players, the team manager's teams (the teams' networkable on a client,
// Networkables.h TeamNetState), the control points' objects and the spawn
// manager's groups, which a client has from the server's ghosts and events
// (CreatePlayer and the players' networkables, ghost 0x201, the control
// points' ghosts, CreateSpawnGroup, IssueSquadOrder). The rewrite's local
// round keeps them in game::GameWorld / GameLogic, a joined game in
// net::RemoteWorld: LocalHudWorld and RemoteHudWorld give the one HUD
// update (main.cpp's updateHud and its parts) either.
//
// What only the local round has (the bodies' gun aims, projectiles, Titans,
// silos, the commanders' assets) the HUD still asks the round for: a
// client's round has the level's objects (the ghosts as objects) and
// answers those empty.
#pragma once

#include "Game/GameWorld.h"

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace dice::hfe::game {
class GameLogic;
} // namespace dice::hfe::game

namespace dice::hfe::net {
class RemoteWorld;
struct RemotePlayer;
} // namespace dice::hfe::net

namespace dice::hfe::host {

class HudWorld {
public:
	using PlayerInfo = game::GameWorld::PlayerInfo;
	using ControlPointInfo = game::GameWorld::ControlPointInfo;
	using VehicleInfo = game::GameWorld::VehicleInfo;
	using SquadRequest = game::GameWorld::SquadRequest;
	virtual ~HudWorld() = default;

	// Whether the game is a server's (a joined game).
	virtual bool joined() const = 0;
	virtual int localPlayer() const = 0;
	virtual bool playerInfo(int player, PlayerInfo& out) const = 0;
	virtual std::vector<PlayerInfo> playerList() const = 0;
	// The local player's team (0 without one).
	int playerTeam() const;
	// The scoreboard's numbers (the player's networkable on a client: score,
	// rplScore, ping; the scripts' score locally).
	struct Scores {
		int score = 0, teamwork = 0, ping = -1;
	};
	virtual Scores scores(int player) const = 0;
	virtual float tickets(int team) const = 0;
	virtual int commander(int team) const = 0; // -1: none
	virtual std::vector<ControlPointInfo> controlPointList() const = 0;
	// The control points a team may spawn at (the spawn manager's groups)
	// and the one a player picked.
	virtual std::vector<int> spawnablePoints(int team) const = 0;
	virtual int spawnChoice(int player) const = 0;
	virtual std::vector<VehicleInfo> vehicleList() const = 0;
	virtual std::vector<int> vehicleOccupants(uint32_t vehicle) const = 0;
	virtual float hitPoints(uint32_t object) const = 0;
	// An object's armour as a fraction of its maxHitPoints, -1 when not known
	// (no armour, or a ghost whose health hasn't come): the name tags' bars
	// and useAliveArmor (RendDX9 0x1016a950: the armour's slot 0x60).
	virtual float objectHealth(uint32_t object) const = 0;
	virtual std::vector<SquadRequest> squadRequests(int team) const = 0;
	// A squad's order: its leader's own (fromCommander false) or the
	// commander's; false for none.
	using SquadOrder = game::GameWorld::SquadOrder;
	virtual bool squadOrder(int team, int squad, bool fromCommander, SquadOrder& out) const = 0;
	// The local player's soldier and the vehicle it sits in, as objects of
	// the level (nullptr: none, or dead).
	virtual const world::Object* soldier() const = 0;
	virtual const world::Object* vehicle() const = 0;
	// The soldier's health, 0..1 (the player's own state on a client).
	virtual float health() const = 0;
	// The weapon in hand: the local round's firearm, or (a joined game) its
	// template and the rounds and magazines the server's record of our
	// player carries (PlayerOwnState's weapon groups).
	struct Weapon {
		const world::ObjectTemplate* weapon = nullptr;
		const game::FireArm* gun = nullptr;
		int rounds = -1, mags = -1;
	};
	virtual Weapon heldWeapon() const = 0;
	// The enemies on the team's minimap: a local round's revealed ones (the
	// recon drones' pulses), a joined game's from the squad's battlefield
	// reports (event 0x41, the client's 0x00508590).
	virtual std::vector<int> revealedPlayers(int team) const
	{
		(void)team;
		return {};
	}
};

// A local round (also a hosting server's own player).
class LocalHudWorld : public HudWorld {
public:
	LocalHudWorld(game::GameWorld& round, const game::GameLogic& logic) : m_round(round), m_logic(logic) {}
	std::function<int(int)> scoreOf; // the scripts' scores (else kills x 2 - team kills x 4)

	bool joined() const override { return false; }
	int localPlayer() const override { return m_round.localPlayer(); }
	bool playerInfo(int player, PlayerInfo& out) const override { return m_round.playerInfo(player, out); }
	std::vector<PlayerInfo> playerList() const override { return m_round.playerList(); }
	Scores scores(int player) const override;
	float tickets(int team) const override;
	int commander(int team) const override { return m_round.commander(team); }
	std::vector<ControlPointInfo> controlPointList() const override { return m_round.controlPointList(); }
	std::vector<int> spawnablePoints(int team) const override { return m_round.spawnablePoints(team); }
	int spawnChoice(int player) const override { return m_round.spawnChoice(player); }
	std::vector<VehicleInfo> vehicleList() const override { return m_round.vehicleList(); }
	std::vector<int> vehicleOccupants(uint32_t vehicle) const override { return m_round.vehicleOccupants(vehicle); }
	float hitPoints(uint32_t object) const override { return m_round.hitPoints(object); }
	float objectHealth(uint32_t object) const override;
	std::vector<SquadRequest> squadRequests(int team) const override { return m_round.squadRequests(team); }
	bool squadOrder(int team, int squad, bool fromCommander, SquadOrder& out) const override
	{
		const SquadOrder* o = fromCommander ? m_round.squadOrder(team, squad) : m_round.squadLeaderOrder(team, squad);
		if (o) {
			out = *o;
		}
		return o != nullptr;
	}
	const world::Object* soldier() const override { return m_round.player(); }
	const world::Object* vehicle() const override { return m_round.playerVehicle(); }
	float health() const override { return 1; }
	Weapon heldWeapon() const override;
	std::vector<int> revealedPlayers(int team) const override { return m_round.revealedPlayers(team); }

private:
	game::GameWorld& m_round;
	const game::GameLogic& m_logic;
};

// A joined game: the server's players (CreatePlayer and their
// networkables), tickets (ghost 0x201), control points (their ghosts:
// position, team; the ids of the spawn groups at them), spawn groups,
// requests, and our own soldier (the control-object record, our player's
// own state), the ghosts as the level's objects (`ghostObjects`: ghost ->
// object, kept by main.cpp's remote step).
class RemoteHudWorld : public HudWorld {
public:
	RemoteHudWorld(const net::RemoteWorld& world, game::GameWorld& round, const std::map<uint16_t, uint32_t>& ghostObjects)
		: m_world(world), m_round(round), m_ghostObjects(ghostObjects)
	{
	}
	// Our soldier's feet and look as the client has them (its prediction and
	// the mouse's aim); false: the server's.
	std::function<bool(physics::Vec3& feet, float& yaw, float& pitch)> ownLook;
	// The spawn group the local player asked for (6/6), -1 none.
	int chosenSpawn = -1;

	bool joined() const override { return true; }
	int localPlayer() const override;
	bool playerInfo(int player, PlayerInfo& out) const override;
	std::vector<PlayerInfo> playerList() const override;
	Scores scores(int player) const override;
	float tickets(int team) const override;
	int commander(int team) const override;
	std::vector<ControlPointInfo> controlPointList() const override;
	std::vector<int> spawnablePoints(int team) const override;
	int spawnChoice(int player) const override { return player == localPlayer() ? chosenSpawn : -1; }
	std::vector<VehicleInfo> vehicleList() const override;
	std::vector<int> vehicleOccupants(uint32_t vehicle) const override;
	float hitPoints(uint32_t object) const override;
	float objectHealth(uint32_t object) const override;
	std::vector<SquadRequest> squadRequests(int team) const override;
	bool squadOrder(int team, int squad, bool fromCommander, SquadOrder& out) const override;
	const world::Object* soldier() const override;
	const world::Object* vehicle() const override;
	float health() const override;
	Weapon heldWeapon() const override;
	std::vector<int> revealedPlayers(int team) const override;

	// The object of a ghost (0 none) and the ghost of an object.
	uint32_t objectOf(uint16_t ghost) const;
	uint16_t ghostOf(uint32_t object) const;

private:
	void fill(const net::RemotePlayer& p, PlayerInfo& out) const;
	const net::RemoteWorld& m_world;
	game::GameWorld& m_round;
	const std::map<uint16_t, uint32_t>& m_ghostObjects;
};

} // namespace dice::hfe::host
