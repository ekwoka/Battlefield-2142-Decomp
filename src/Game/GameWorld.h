// The game's simulation of a round: control points and the object
// spawners that put vehicles and emplacements into the world.
//
// In the original these are object classes in BF2142.exe driven by the
// game's per-tick update (docs/engine.md, "A simulation tick"); the object
// spawner is BF2/Game/Objects/Game/ObjectSpawner.cpp (update 0x005af230,
// spawn 0x005aeb80, team change 0x005ae490, round reset 0x005ae580).
// Not rewritten yet: the collision test that keeps a spawn spot clear,
// holding a spawned vehicle in place until it is entered (holdObject: the
// update's +0x1fc branch; set by two shipped levels, operation_clean_sweep
// and the_hell), the time to live of abandoned vehicles (the spawn hands
// the vehicle's PlayerControlObject interface 0xc4c5 the spawner, slot
// 0xc0, timeToLive, slot 0xa4, and the time, slot 0xb8; set by most
// levels' spawners), and activation by events. useButtonRadius (waiting
// for a player to use the spawner) is set by no shipped level.
#pragma once

#include "Animation/AnimationSystem.h"
#include "Effects/Effects.h"
#include "Physics/RigidBody.h"
#include "Random.h"
#include "ScriptGame.h"
#include "SoldierControl.h"
#include "Vehicle.h"
#include "Weapons.h"
#include "World/WorldMath.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dice::hfe::io {
class FileManager;
}
namespace dice::hfe::physics {
class CollisionTemplateManager;
class MaterialManager;
struct CollisionMeshFile;
}

namespace dice::hfe::world {
class Object;
class ObjectManager;
class ObjectTemplate;
class ObjectTemplateManager;
} // namespace dice::hfe::world

namespace dice::hfe::game {

// The game logic's state (slot 0x164); spawners treat team changes before
// the round starts as the round's starting teams.
enum class RoundState { BeforeStart = 3, Playing = 1 };

// An ObjectSpawner template's settings (ObjectSpawner template 0x2c0-0x304,
// defaults from its constructor 0x005af920).
struct SpawnerSettings {
	float minSpawnDelay = 30;
	float maxSpawnDelay = 60;
	float timeToLive = 60;
	float distance = 50;
	int team = 0;
	int nrOfObjectToSpawn = -1; // spawns in total; -1 without limit
	int maxNrOfObjectSpawned = 1;
	float radius = 5;           // a spawned object nearer than this blocks the next
	float useButtonRadius = 0;
	world::Vec3 spawnOffset;
	int controlPointId = -1;
	bool holdObject = false;
	bool teamOnVehicle = false;
	bool paused = false;            // 0x2fa
	bool spawnDelayAtStart = false; // 0x2fb: wait a spawn delay before the first spawn
	bool active = true;             // 0x2fc
	bool forwardTeamChangeToSpawnedVehicles = false;
	int activateOnEvent = -1;
	std::string objectTemplates[3]; // per team (0 unused): setObjectTemplate

	static SpawnerSettings fromTemplate(const world::ObjectTemplate& objectTemplate);
};

class ObjectSpawner {
public:
	ObjectSpawner(world::Object& object, SpawnerSettings settings);

	world::Object& object() const { return m_object; }
	const SpawnerSettings& settings() const { return m_settings; }
	int team() const { return m_team; }
	float spawnCountdown() const { return m_countdown; }
	// Ids of what the spawner's slots hold (0 when empty).
	const std::vector<uint32_t>& spawned() const { return m_spawned; }

	// 0x005ae490.
	void setTeam(int team, RoundState state);
	// 0x005ae580: back to the round's starting state.
	void reset(Random& random);
	// 0x005af230: one tick of `seconds`.
	void update(float seconds, world::ObjectManager& objects, world::ObjectTemplateManager& templates,
		Random& random);

private:
	uint32_t spawn(world::ObjectManager& objects, world::ObjectTemplateManager& templates);
	void startCountdown(Random& random);

	world::Object& m_object;
	SpawnerSettings m_settings;
	int m_team;
	int m_startTeam;
	bool m_active;
	int m_remaining;           // 0x1e4: spawns left, -1 without limit
	uint32_t m_lastSpawned = 0; // 0x1b4
	float m_countdown = 0;      // 0x1b8: spawns when it is not above 0
	std::vector<uint32_t> m_spawned;
};

struct ControlPoint {
	world::Object* object;
	int id;   // controlPointId
	int team; // owner
	// The flag the game mode's script raises and lowers (cp_getParam /
	// cp_setParam): whose flag it is, how high (0 bottom, 1 top), how fast.
	int flag = 0;
	float flagHeight = 0;
	float takeOverChangePerSecond = 0;
	int playerId = -1;           // who took it last
	bool allowCapture[3] = {true, true, true}; // allowCaptureByTeam
};

class GameWorld {
public:
	GameWorld(world::ObjectTemplateManager& templates, world::ObjectManager& objects, uint32_t seed = 0x759dc150);
	~GameWorld();

	// Finds the control points and spawners among the placed objects and
	// gives each spawner its control point's team (before the round starts).
	void startRound();
	// One simulation tick.
	void tick(float seconds);
	// A body left slumped in a vehicle seat (the seat's die bundle), 0 for
	// none.
	uint32_t seatCorpse(uint32_t vehicle, int seat) const;
	// Whether a dead player's body has landed (0x005eda20): no ragdoll, or
	// one a particle of which has hit something, ragdollLandedDelay (the
	// soldier template's +0x3bc, 4 s) after the death. The corpse's removal
	// countdown (the armour's, 0x00652380) waits for it, or 60 s.
	bool bodyLanded(int player) const;
	// The bodies left after PlayerDeath (see stepCorpses): their objects.
	std::vector<uint32_t> corpses() const;
	// A joined game's dead soldiers (a ghost's Soldier networkable with its
	// ragdoll group, 0x10000): `object` (the ghost's, a soldier template
	// with skeleton3P) gets a network-driven ragdoll on the first update
	// (0x005f0a00 starts it on a client for a soldier with a player,
	// 0x008acc00), each update drives its networked particles (`particles`,
	// as decoded: 0x008acce0), and between updates (and 0.3 s past the
	// last) it simulates on its own. The object then sits at the middle
	// particle, unturned, posed by skeletonPose. False when the object or
	// its skeleton is missing.
	bool driveRemoteRagDoll(uint32_t object, const std::vector<physics::Vec3>& particles);
	// The body is gone (the ghost removed, the soldier revived).
	void removeRemoteRagDoll(uint32_t object);
	const anim::RagDoll* remoteRagDoll(uint32_t object) const;
	// A joined game's soldier ghost's health (its Soldier networkable's 0x40
	// group: the fraction of maxHitPoints) and whether a player is still
	// attached to it (a RemotePlayer's soldier ghost). The client runs the
	// armour's corpse countdown from it as the server does (0x005fcdb0 sets
	// the hit points through the armour's setter, slot 0x38 -> 0x00655460 ->
	// 0x00654120: at 0 or less the armour turns wreck at the client's time,
	// +0x60; the armour's update 0x00654d80 -> 0x00652380 has no server test
	// up to the fade), so the body fades as on the server (stepRemoteWrecks);
	// only the removal (0x00651db0) is the server's, which the client sees
	// as the ghost going. Back above 0 (revived) it is whole again.
	void setRemoteHealth(uint32_t object, float health, bool playerAttached);
	// Where the camera is and looks (unit), for the ragdolls' levels of
	// detail (0x008ae120); without one (a server) every body steps at
	// level 0.
	void setCamera(const physics::Vec3& position, const physics::Vec3& forward);
	// The clock the ragdolls' frame budget (ragdoll_clientMaxUpdateTime)
	// is measured with, in seconds (by default the steady clock: the
	// original reads the performance counter, 0x004423f0).
	void setClock(std::function<double()> clock) { m_clock = std::move(clock); }
	// Sounds the game asks for (to be played by the sound layer), taken
	// once: a ragdoll's impact (S_Soldier_RagDoll_Impact and the like).
	struct SoundEvent {
		std::string sound;
		physics::Vec3 position;
	};
	std::vector<SoundEvent> takeSounds() { return std::exchange(m_sounds, {}); }

	RoundState state() const { return m_state; }
	const std::vector<ControlPoint>& controlPoints() const { return m_controlPoints; }
	const std::vector<std::unique_ptr<ObjectSpawner>>& spawners() const { return m_spawners; }
	// Changes a control point's owner and the teams of its spawners.
	void setControlPointTeam(int controlPointId, int team);
	ControlPoint* controlPointOf(uint32_t object);

	// Events for the game mode's scripts (PlayerSpawn, EnterVehicle,
	// ControlPointChangedOwner...).
	void setEventSink(ScriptEventSink sink) { m_events = std::move(sink); }
	world::ObjectManager& objects() { return m_objects; }
	world::ObjectTemplateManager& templates() { return m_templates; }
	// The local player's soldier and what it holds; 0 while dead.
	uint32_t playerObject() const { return m_player && m_player->alive ? m_player->object : 0; }
	uint32_t playerVehicleObject() const { return m_player && m_player->alive ? m_player->vehicle : 0; }
	uint32_t playerWeaponObject() const { return m_player && m_player->alive ? m_player->weaponObject : 0; }
	uint32_t playerKitObject() const { return m_player && m_player->alive ? m_player->kitObject : 0; }
	int playerTeam() const { return m_player ? m_player->team : 0; }

	// Players: the local one joins with spawnPlayer, others with addPlayer
	// (bots, remote players); ids count up from 0. A player's soldier dies when its
	// armor runs out (PlayerKilled, PlayerDeath for the scripts) and
	// respawns after the spawn time.
	int addPlayer(const std::string& name, int team, bool ai);
	// Puts the player's soldier at a spawn point of its team; false when
	// there is none or it lives already.
	bool spawnSoldier(int player);
	struct PlayerInfo {
		int id = -1;
		std::string name;
		int team = 0;
		bool ai = false;
		bool alive = false;
		uint32_t soldier = 0, vehicle = 0, kit = 0, weapon = 0;
		int seat = -1; // in the vehicle (0 the driver's)
		float respawnIn = 0; // seconds until the next spawn while dead
		bool manDown = false; // down, a medic may revive it
		int kills = 0, deaths = 0, teamKills = 0;
		int squad = 0; // 0: none
		bool squadLeader = false;
		bool commander = false;
		bool cloaked = false; // active camouflage on
		// Spawned on the squad leader where only a prone soldier fits (the
		// spawn point's +0x18a): the soldier starts lying down.
		bool spawnedProne = false;
		float battery = -1; // the ability battery, 0..1 (-1 without a kit)
		physics::Vec3 position;  // the soldier's feet
		float yaw = 0;           // degrees, 0 facing +z
		physics::Vec3 velocity;
		std::vector<std::pair<int, std::string>> carried; // (slot, weapon template)
		int inHandSlot = 0;
		float heat = 0; // the weapon in hand's (or the seat's gun's), 0..1
	};
	std::vector<PlayerInfo> playerList() const;
	bool playerInfo(int player, PlayerInfo& info) const;
	// The player whose soldier (or vehicle) an object is, -1 for none.
	int playerOf(uint32_t object) const;
	void setInput(int player, const SoldierInput& input);
	// Moves a soldier (its feet) at once, as the scripts' setPosition does;
	// false when the object is no living player's soldier on foot.
	bool setSoldierPosition(uint32_t soldier, const physics::Vec3& feet);
	// Whether nothing solid (static objects' projectile LOD, vehicles, the
	// terrain) lies between two points.
	// `ignore`: an object (a body) that doesn't count, such as the looker's own.
	bool lineOfSight(const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore = 0) const;
	// Control points with their position, for the bots' strategy.
	struct ControlPointInfo {
		int id;
		int team;
		physics::Vec3 position;
		float radius;
		// Not ObjectTemplate.unableToChangeTeam (the template's vt+0x64 at
		// +0x298; IAIGame vt+0x24, BF2142.exe 0x00895d90, answers it to
		// the AI).
		bool capturable = true;
	};
	std::vector<ControlPointInfo> controlPointList() const;
	// Where a player wants to spawn next (a control point id, -1 for any of
	// the team's): the spawn menu's choice. Held points the team owns
	// with spawn points; anywhere else falls back to any.
	void setSpawnChoice(int player, int controlPoint);
	// Squads (1..9 per team, up to six soldiers; the first is the leader):
	// members may spawn on their leader or the leader's spawn beacon
	// (spawnChoice kSpawnOnLeader / kSpawnOnBeacon). The scripts hear
	// PlayerChangedSquad.
	static constexpr int kSquadSize = 6;
	static constexpr int kSpawnOnLeader = -2;
	static constexpr int kSpawnOnBeacon = -3;
	// Joins a squad of the player's team (0 leaves), false when full;
	// a new squad's first member leads it.
	bool joinSquad(int player, int squad);
	// A squad of the team with room (a new one if none): the squad id.
	int openSquad(int team) const;
	// The squad's beacon object (0 for none).
	uint32_t squadBeacon(int team, int squad) const;
	// Unlocks: a kit's ItemContainer children (one per branch and level,
	// unlockLevel) add their items once unlocked; a slot holds the first
	// unlocked item for it (the customise screen's choice is not in yet).
	// Without a stats server nothing is unlocked; `all` unlocks everything.
	void setUnlocks(int player, std::vector<std::string> containers, bool all = false);
	void setBattery(int player, float battery); // tests and the scripts' view
	bool unlockAllByDefault = false; // players joining from now on have everything
	// The local player's unlocks (its soldier's chosen ItemContainers,
	// Stats/Stats.h), given when spawnPlayer adds the player.
	std::vector<std::string> localPlayerUnlocks;
	// The kit (and its soldier template) for the player's next spawn.
	void setKit(int player, const std::string& kit, const std::string& soldier);
	int spawnChoice(int player) const;
	// The control points a team may spawn at (held, with spawn points).
	std::vector<int> spawnablePoints(int team) const;
	// Seconds from death to the next spawn (sv.spawnTime, 15 by default).
	void setSpawnTime(float seconds) { m_spawnTime = seconds; }
	// The sea's height (heightmapcluster.setSeaWaterLevel), for vehicles
	// that drown.
	void setWaterLevel(float level) { m_waterLevel = level; m_hasWater = true; }
	// How long a killed soldier lies revivable (sv.manDownTime, 15; 0: none).
	void setManDownTime(float seconds) { m_manDownTime = seconds; }
	// Respawn on their own when the time is up (no spawn menu yet).
	bool autoRespawn = true;
	bool playerInsideControlPoint = false; // set by the scripts (setIsInsideCP)
	// The player's rounds fired and rounds that damaged something, by
	// weapon template; `clear` starts counting again.
	struct RoundCount {
		std::string weapon;
		int count;
	};
	std::vector<RoundCount> playerRounds(bool hits, bool clear) { return playerRounds(localPlayer(), hits, clear); }
	std::vector<RoundCount> playerRounds(int player, bool hits, bool clear);
	// The local player's id, -1 before spawnPlayer.
	int localPlayer() const { return m_player ? m_player->id : -1; }
	bool isVehicle(uint32_t object) const { return vehicle(object) != nullptr; }

	// Mobile objects (hasMobilePhysics, physicsType Mesh, a collision mesh)
	// get rigid bodies that fall and rest on `ground`.
	void enablePhysics(const io::FileManager& files, physics::CollisionTemplateManager& collisionTemplates,
		physics::Ground ground, const physics::MaterialManager* materials = nullptr);
	size_t physicsBodies() const { return m_bodies.size(); }
	const physics::CollisionWorld& staticCollision() const { return m_collision; }

	// The local player's soldier: spawned at a spawn point of a control
	// point the team holds (team 1 PAC, team 2 EU). Returns its object.
	world::Object* spawnPlayer(int team);
	world::Object* player() const;
	const physics::SoldierBody* playerBody() const { return m_player ? &m_player->body : nullptr; }
	// Where a projectile stopped (for effects and checks).
	struct Impact {
		physics::Vec3 point;
		physics::Vec3 normal;
		uint32_t object; // 0 for the terrain
		uint32_t material;
		float damage;
	};
	const std::vector<Impact>& impacts() const { return m_impacts; }
	size_t projectilesInFlight() const { return m_projectiles.size(); }
	const std::vector<Projectile>& projectiles() const { return m_projectiles; }
	// Armor: hit points left (from armor.hitPoints), -1 without armor.
	float hitPoints(uint32_t object) const;
	// Sets them (the scripts' setDamage); none left kills a soldier or
	// destroys the object.
	void setHitPoints(uint32_t object, float hitPoints);
	const FireArm* playerWeapon() const { return m_player && m_player->alive ? m_player->weapon.get() : nullptr; }
	const effects::EffectManager& effects() const { return m_effects; }

	// Animation systems for soldiers and their weapons: `load` gives the
	// system of a script path (running it the first time).
	using AnimationLoader = std::function<const anim::AnimationSystemTemplate*(const std::string& path)>;
	void setAnimationSystems(anim::AnimationSystems* systems, AnimationLoader load);
	// The pose of an object's skeleton (model-space matrices per node) from
	// its animation systems, as of the last tick; false for objects without.
	bool skeletonPose(uint32_t object, const anim::Skeleton& skeleton, std::vector<anim::Matrix>& world);
	// The transforms of an object's mesh parts relative to it, when the game
	// places them: a held weapon's parts follow the soldier's skeleton nodes
	// mesh1, mesh2... (posed by the weapon's animation system).
	bool partTransforms(uint32_t object, std::vector<anim::Matrix>& parts) const;
	// The bundles an object's body system plays, by priority.
	std::vector<std::string> playingAnimations(uint32_t object) const;
	// The animation an object plays (a .baf path), or empty.
	std::string animationFor(uint32_t object) const;
	// The vehicle the player drives (its object), or null on foot.
	world::Object* playerVehicle() const;
	const Vehicle* playerVehicleState() const;
	// How the game moves a part of an object's template tree (wheels: the
	// suspension's offset and the spin; steering bundles: the steering), as
	// an offset and yaw/pitch/roll in degrees before the part's own
	// placement; false for parts the game does not move.
	bool partPose(uint32_t object, const std::string& part, world::Vec3& offset, world::Vec3& rotation) const;
	// A vehicle's seats and entry points, from its template tree: each
	// PlayerControlObject with seatInformation is a seat (the root first:
	// the driver), with its exit (setSoldierExitLocation), and the gun its
	// subtree holds, aimed by the RotationalBundles that take the mouse
	// (inputToYaw, inputToPitch). Positions are in the vehicle's frame,
	// summed from the children's offsets (their rotations are left out).
	struct SeatLayout {
		std::string name;            // the seat's PlayerControlObject
		physics::Vec3 position;      // where the soldier sits
		physics::Vec3 exit;          // where a soldier leaving goes
		bool hasExit = false;
		std::string weapon;          // a GenericFireArm, or empty
		physics::Vec3 weaponPosition;
		std::string yawBundle, pitchBundle;
		float minPitch = -90, maxPitch = 90; // the pitch bundle's limits (its rotation's y; down positive)
		physics::Vec3 rotation;      // seatInformation's yaw/pitch/roll, degrees
		std::string animationSystem; // seatAnimationSystem: the sitter's pose (empty: hidden)
	};
	struct VehicleLayout {
		std::vector<SeatLayout> seats;
		struct Entry {
			physics::Vec3 position;
			float radius;
		};
		std::vector<Entry> entries; // EntryPoint children (entryRadius)
	};
	// A vehicle's seats (for checks and the HUD) and who sits in them.
	std::vector<SeatLayout> vehicleSeats(uint32_t object);
	std::vector<int> vehicleOccupants(uint32_t object) const;
	// A seat's gun aim (degrees, yaw relative to the vehicle, pitch up
	// positive); false for seats without a gun.
	bool gunAim(uint32_t vehicle, int seat, float& yaw, float& pitch) const;
	const FireArm* gun(uint32_t vehicle, int seat) const;
	// Vehicles (for the bots): where, which way, how many seats are free.
	struct VehicleInfo {
		uint32_t object;
		physics::Vec3 position;
		float yaw;
		int freeSeats;
		bool driverFree;
		bool working; // not a wreck, not shut down by EMP
		// Its motion (world frame): for the bots' driving.
		physics::Vec3 velocity;
		physics::Vec3 angularVelocity; // radians per second about each axis
		physics::Vec3 forward;         // its +z
		physics::Vec3 right;           // its +x
		// Its box in object space (the vehicle collision LOD's points): the
		// bots' AI object's box (AIDLL 0x100fe990).
		physics::Vec3 boxMin, boxMax;
	};
	std::vector<VehicleInfo> vehicleList();
	// The battlefield-objects report (BF2142.exe event 0x41, server
	// 0x005088f0 -> IAIEventCourier vt+0xe0, AIDLL 0x1004e5e0 -> 0x100bf000):
	// `reporter`'s squad now knows of the enemy `players` (and the
	// occupants of their vehicles). The bots send one as their trigger
	// presses or lets go at their target (AIDLL 0x1009a1d0, 0x1009a2a0).
	void reportBattlefieldObjects(int reporter, const std::vector<int>& players);
	// Whether a team's squad had a player reported within the last 3 s
	// (0x100bea30).
	bool squadKnows(int team, int squad, int player) const;
	// A player's carried weapons' ammunition (for the bots' AmmoAssist):
	// rounds left in all, the magazine size and the kit's magazines.
	struct WeaponAmmo {
		std::string weapon;
		int rounds = 0;
		int magSize = 0;
		int magazines = 0;
	};
	std::vector<WeaponAmmo> weaponAmmo(int player) const;
	// A projectile template's detonation at a point, as if it went off
	// there (false without one).
	bool detonate(const std::string& projectileTemplate, const physics::Vec3& at, int attacker = -1);
	// A vehicle's or drone's EMP level (0: running) and whether it is a wreck.
	int empLevel(uint32_t vehicle) const;
	// What a vehicle's driver inputs are scaled by (0x005ca2e0): EMP and
	// critical damage; see GameWorld.cpp.
	static float driverInputScale(float empLevel, float hitPoints, const world::ObjectTemplate& vehicle);
	// Deployed hubs (their objects).
	std::vector<uint32_t> supplyObjects() const;
	// Squad leaders' drones (a detonator whose projectile carries a
	// ForceObject): the drone hovering by a player (its object, 0 for
	// none), and whether it has strayed past its warningDistance (the
	// owner's "uav_warning").
	uint32_t drone(int owner) const;
	bool droneWarning(int owner) const;
	// Recon drones' pulses: whether a player shows on a team's minimap
	// (revealed for pulse.revealTime seconds), and every player that does.
	bool isRevealed(int player, int byTeam) const;
	std::vector<int> revealedPlayers(int byTeam) const;

	// The commander (one per team; docs/engine.md, "Commander"). The game
	// mode's script enables it (host.sh_setEnableCommander). Players apply
	// (Commander.commanderApply) and the team's best applicant leads; the
	// scripts hear ChangedCommander(team, old, new). A commander leaves its
	// squad.
	void setCommanderEnabled(bool enabled);
	bool commanderEnabled() const { return m_commanderEnabled; }
	int commander(int team) const; // the player, -1 for none
	// Makes a player the team's commander (-1: nobody); false when the
	// commander is disabled or the player isn't on the team.
	bool setCommander(int team, int player);
	// Commander.commanderApply / commanderResign (docs/engine.md,
	// "Commander"): applications wait for the apply period, then the
	// highest-ranked applicant is offered command and answers.
	void applyForCommander(int player);
	void resignCommander(int player);
	// The applicant offered command (-1: none) and its answer (the
	// Commander event's kind 0 accepts, kind 1 declines).
	int commanderOffer(int team) const { return team == 1 || team == 2 ? m_commanders[team].candidate : -1; }
	void answerCommanderOffer(int player, bool accept);
	// The player's rank (the stats' rank, player slot 0x1a8 +0x20) the
	// applicants are ranked by; 0 until set.
	void setPlayerRank(int player, int rank);
	// The commander's tools (Commander.orbitalStrike, .artillery, .uav,
	// .satellite, .supplies). All but the supply drop need the team's asset
	// intact: a placed object whose template holds a RemoteControlledObject
	// of the tool's rcType (commander_emp_strike: RCOrbitalStrike,
	// commander_orbital_strike: RCArtillery, commander_uav_drone_control:
	// RCUav, commander_movement_analyst: RCSatellite).
	enum class CommanderAbility { OrbitalStrike, Artillery, Uav, Satellite, Supplies };
	static constexpr int kCommanderAbilities = 5;
	struct CommanderAbilityState {
		bool available = false;   // the asset stands (the supply drop needs none)
		uint32_t asset = 0;       // its object (the first intact one)
		float armor = 0;          // the asset's hit points over its maximum (1 for supplies)
		bool ready = false;
		float timeUntilReady = 0; // seconds
		float reloaded = 0;       // 0..1 of the recharge done (the HUD's *TimeUntilReloaded bar)
		bool active = false;      // a UAV or scan running, a strike on its way
	};
	CommanderAbilityState abilityState(int team, CommanderAbility ability) const;
	bool abilityReady(int team, CommanderAbility ability) const { return abilityState(team, ability).ready; }
	// Uses a tool at a point of the map (x, z; the ground's height is
	// found); false when it isn't ready. The orbital strike and the
	// artillery fire their asset's weapon from orbitalBombardmentHeight
	// over the point; the UAV watches round the point; the satellite scan
	// covers the map; the supply drop falls from supplyDropHeight.
	// byAi: the AI commander's use (AIDLL's IBFCommander, sender -1),
	// which needs no commander.
	bool commanderAction(int team, CommanderAbility ability, float x, float z, bool byAi = false);
	// The team's UAV while it is up: where, and how far it reveals.
	bool uavArea(int team, physics::Vec3& at, float& radius) const;
	bool satelliteScanActive(int team) const;
	// The enemies a running satellite scan shows: to the team's commander
	// only (0x0057dc90), empty for anyone else. (revealedPlayers holds the
	// UAV's and the drones' for the whole team.)
	std::vector<int> scannedPlayers(int viewer) const;
	// Strikes on their way, for the HUD and the map.
	struct Strike {
		CommanderAbility ability;
		int team;
		physics::Vec3 target;
		float fireIn;      // seconds until the asset fires (its launch delay)
		int salvosLeft;    // rounds of the burst still to fire
	};
	const std::vector<Strike>& strikes() const { return m_strikes; }
	// Supply crates dropped by the commanders (their objects).
	std::vector<uint32_t> supplyCrates() const;
	// The gameLogic settings for the tools (GameLogicInit.con and the
	// constructor 0x004d69d0's defaults).
	struct CommanderSettings {
		float supplyDropHeight = 50;          // 0x1b8
		float supplyDropNumSecsToLive = 300;  // 0x1bc
		float supplyDropReloadTime = 60;      // "SupplyDrop-Reload-Time" (0x0043d6d0)
		float uavTime = 30;                   // 0x228: how long the UAV stays
		float uavReloadTime = 5;              // 0x22c
		float uavRadius = 100;                // 0x230: the minimap's ring round it
		float satelliteScanTime = 11;         // 0x238
		float satelliteScanReloadTime = 16;   // 0x23c
		float orbitalBombardmentHeight = 500; // 0x254
		float extraUAVVehicleFlightHeight = 0; // 0x234 (slots 0xc8/0xcc; no shipped script sets it)
	};
	// The team's UAV drone in the air (eu_drone), 0 for none.
	uint32_t uavDrone(int team) const { return team == 1 || team == 2 ? m_commanders[team].uavDrone : 0; }
	CommanderSettings commanderSettings;
	// Orders to squads. The HUD's SquadLeader.sendOrder <kind> <bool> and
	// Commander.sendOrder <kind> <bool> both run HudManager slot 0x3f4
	// (0x007d6ab0); the kind is the engine's order kind (a byte on the wire,
	// the order event 0x0050dfe0):
	// - 0 move, 1 attack, 2 defend, 3 repair: the issue-order menu
	//   (HudElementsIssueOrders.con: "sendOrder 0" by Move, "1" by Attack,
	//   "2" by Defend) and the order icons (Ingame/Commander/Orders/
	//   orderMove..orderRepair at 0x184 + 4 kind, 0x007a0000);
	// - 4 move, 5 attack, 6 defend, 7 repair: placed on the map (the
	//   right-click menus' "sendOrder 4 1" .. "7 1"; a map click arms 4, or
	//   5 on an enemy and 6 on a friendly object, 0x00753c90). Only 5 and 6
	//   count for the scripts' ifAcceptedAttackDefendOrder (0x00592160).
	// A kind of 10 or more drops the armed order. The bool takes the
	// position from the minimap's cursor as the order's next point
	// (0x00780820, up to three; the 9 floats of the order) instead of the
	// point already armed. A squad leader's order goes to his own squad
	// (SquadManager slot 0x48, 0x004c5360: the squad's leader order +0x30,
	// accepted at once); the commander's to each selected squad with
	// members (slot 0x4c, 0x004c5530: the squad's commander order +0x120,
	// for its leader to accept).
	enum class OrderType { None = -1, Move = 0, Attack = 1, Defend = 2, Repair = 3, MapMove = 4, MapAttack = 5,
		MapDefend = 6, MapRepair = 7 };
	// The engine's kind number as an order (None for 10 and more, and for
	// numbers that are no kind).
	static OrderType orderTypeFromKind(int kind)
	{
		return kind >= 0 && kind <= 7 ? static_cast<OrderType>(kind) : OrderType::None;
	}
	static bool isAttackOrDefend(OrderType type) { return type == OrderType::MapAttack || type == OrderType::MapDefend; }
	struct SquadOrder {
		OrderType type = OrderType::None;
		physics::Vec3 position;
		// The player's orderRadius (player +0x144, set to 80 by the
		// constructor 0x0059c010; nothing in the game changes it).
		float radius = 80;
		bool fromCommander = true;
		bool accepted = false; // the squad leader's answer (Commander.commRoseAccept 1)
	};
	// The commander's order: false for a squad that doesn't exist or an
	// order from someone else than the team's commander (-1 for none: the
	// game itself).
	bool issueOrder(int team, int squad, OrderType type, float x, float z, int from = -1);
	void acceptOrder(int player, bool accept);
	const SquadOrder* squadOrder(int team, int squad) const;
	// A squad leader's order to his own squad (SquadLeader.sendOrder):
	// false when the player leads no squad. The scripts' orderPosition,
	// orderRadius and ifAccepted* read this one (squad slot 0x10,
	// 0x004c2ee0), not the commander's.
	bool issueSquadLeaderOrder(int player, OrderType type, float x, float z);
	const SquadOrder* squadLeaderOrder(int team, int squad) const;
	// The vehicle model of an object with one.
	const Vehicle* vehicle(uint32_t object) const;
	// The input for the next ticks.
	void setPlayerInput(const SoldierInput& input) { m_playerInput = input; }
	// A soldier's hit capsules (setSkeletonCollisionBone) in world space as
	// of the last tick: segment ends and radius; for checks and drawing.
	struct Capsule {
		physics::Vec3 a, b;
		float radius;
		uint32_t material;
	};
	std::vector<Capsule> soldierCapsules(uint32_t soldier) const;
	// A downed soldier's or a body's ragdoll (null for others; docs/engine.md, "Ragdolls").
	const anim::RagDoll* ragDoll(uint32_t soldier) const;

private:
	world::ObjectTemplateManager& m_templates;
	world::ObjectManager& m_objects;
	Random m_random;
	Gaussian m_gaussian; // the deviation components' normal numbers
	RoundState m_state = RoundState::BeforeStart;
	std::vector<ControlPoint> m_controlPoints;
	std::vector<std::unique_ptr<ObjectSpawner>> m_spawners;

	const VehicleLayout& vehicleLayout(const world::ObjectTemplate& vehicle);
	std::map<std::string, VehicleLayout> m_layouts;

	struct Player;
	struct Body {
		uint32_t object;
		physics::RigidBody body;
		physics::ContactResponse response;
		std::vector<physics::Vec3> points; // vehicle collision LOD
		std::shared_ptr<physics::CollisionWorld> projectileMesh; // object space, projectile LOD
		std::shared_ptr<physics::CollisionWorld> soldierMesh;    // object space, soldier LOD (ragdolls)
		std::unique_ptr<Vehicle> vehicle; // engines and wheels
		// Seats: who sits in each (player id, -1 for nobody) and the guns.
		std::vector<int> occupants;
		double occupiedAt = -1e9; // when someone last sat in it (MagneticObject's disableWhenEmptyVehicle)
		struct Gun {
			std::unique_ptr<FireArm> weapon;
			const ProjectileSettings* projectile = nullptr;
			float yaw = 0, pitch = 0; // the bundles' rotation, degrees (pitch up positive)
		};
		std::vector<Gun> guns;
		// Armour (armor.*): the EMP level (armor 0x27c, see empBlast),
		// falling by one every empLevelTime; destroyed, it lies as a wreck
		// for timeToStayAsWreck.
		float empLevel = 0;
		float wreckTime = -1; // >= 0 while a wreck
		float wetTime = 0;    // seconds in water
		// The vehicle LOD's edges (indices into `points`) of its faces
		// steeper than 30 degrees (|normal y| at most cos 30), each face's
		// three in turn (the loader 0x006e12a0 -> 0x006e1430, LOD +0xe4).
		std::vector<std::array<uint16_t, 2>> edges;
		// The vehicle LOD's box (object space; the mesh's slot 0x54), which
		// the third pass tests trees against.
		physics::Vec3 boxMin, boxMax;
		// Its first Engine (0x006b6c50: the tree searched siblings first)
		// is a helicopter's (engineType c_ETHelicopter, 33): hitting a static
		// object is the end of it (0x006bb050's early exit).
		bool helicopterEngine = false;
		// For the mobile mesh physics (0x006bb050, see MeshCollision.cpp):
		// the vehicle LOD in object space, its vertices' materials, its
		// radius (the mesh's slot 0x50: the box's farthest corner), the
		// template's mass (+0x94, 1 by default) and isMajorActor (+0xf0),
		// whether it is a PlayerControlObject (the edge pass and the
		// helicopter exit ask for one) or a Particle (0x947b), and its
		// trunk (a type-4 LOD, or treeCollisionDiameter / 2 above 0).
		std::shared_ptr<physics::CollisionWorld> vehicleMesh;
		std::vector<uint32_t> pointMaterials;
		float radius = 0;
		float mass = 1;
		bool majorActor = false;
		bool controlObject = false;
		bool particle = false;
		std::shared_ptr<physics::CollisionWorld> trunk;
		float trunkRadius = 0;
		bool trunkFromTypeFour = false;
		// The position before this tick's step (the helicopter exit's start).
		physics::Vec3 lastPosition;
		// The wheels' parts' radius (their vehicle LOD's, slot 0x50), by wheel.
		std::vector<float> wheelRadii;
		uint32_t material = 0; // the template's material
	};
	void destroyVehicle(Body& body, int attacker);
	void stepVehicleDamage(float seconds);
	bool disabled(const Body& body) const { return body.empLevel > 0 || body.wreckTime >= 0; }

	void seatPlayer(Player& player, Body& body, int seat);
	void stepGunner(Player& player, Body& body, float seconds);
	Body* findBody(uint32_t object);
	int firstEngineType(const world::ObjectTemplate& root) const;
	void addBodies();
	void stepPhysics(float seconds);
	const io::FileManager* m_files = nullptr;
	physics::CollisionTemplateManager* m_collisionTemplates = nullptr;
	physics::Ground m_ground;
	const physics::MaterialManager* m_materials = nullptr;
	std::vector<uint32_t> m_checked; // object ids already considered for a body (sorted)
	std::vector<Body> m_bodies;
	physics::CollisionWorld m_collision; // static objects, soldier LOD
	physics::CollisionWorld m_projectileCollision; // static objects, projectile LOD

	// The mobile mesh physics between objects (BF2/Physics, see
	// MeshCollision.cpp): the static objects' parts (each template of a
	// static object's tree with a collision mesh), in a grid for the
	// broad phase (0x006dc7b0).
	struct MeshShape {
		std::shared_ptr<physics::CollisionWorld> mesh; // the vehicle LOD's faces, object space
		std::vector<physics::Vec3> points;             // its vertices
		std::vector<uint32_t> pointMaterials;          // their materials (through mapMaterial)
		physics::Vec3 boxMin, boxMax;                  // its box (the mesh's slot 0x54)
		float radius = 0;                              // slot 0x50
		bool hasVehicleLod = false;
		std::shared_ptr<physics::CollisionWorld> trunk; // the type-4 faces, or the trunk line
		bool trunkFromTypeFour = false;
	};
	struct StaticPart {
		uint32_t object = 0;
		bool root = true;    // the object itself, not a template of its tree
		bool ladder = false; // template class Ladder (40000: 0x006bb050 leaves it out)
		physics::Mat3 rotation;
		physics::Vec3 position;
		std::shared_ptr<const MeshShape> shape;
		float trunkRadius = 0; // the node's +0x198: treeCollisionDiameter / 2 (0x005f6ba0), 0.3 under 0.01 with a type-4 LOD
		uint32_t material = 0; // the template's material
		uint32_t stamp = 0;
	};
	void addStaticPart(StaticPart part);
	std::vector<StaticPart> m_staticParts;
	std::unordered_map<int64_t, std::vector<uint32_t>> m_staticGrid; // 16 m cells (x, z) -> parts
	std::map<std::pair<const physics::CollisionMeshFile*, std::vector<uint32_t>>, std::shared_ptr<const MeshShape>> m_shapes;
	uint32_t m_staticStamp = 0;
	std::shared_ptr<const MeshShape> meshShape(const physics::CollisionMeshFile& mesh, const std::vector<uint32_t>& materials);
	// One side of a pair: a body (mobile, a root) or a static part.
	struct MeshActor {
		Body* body = nullptr;
		const StaticPart* part = nullptr;
		const std::vector<physics::Vec3>* points = nullptr;      // the vehicle LOD's vertices
		const std::vector<uint32_t>* pointMaterials = nullptr;
		const std::vector<std::array<uint16_t, 2>>* edges = nullptr;
		const physics::CollisionWorld* mesh = nullptr;            // object space
		const physics::CollisionWorld* trunk = nullptr;           // object space
		bool trunkFromTypeFour = false;
		physics::Vec3 boxMin, boxMax;
		float radius = 0, trunkRadius = 0;
		physics::Mat3 rotation;
		physics::Vec3 position;
		physics::Vec3 centerOfMass;   // object space
		physics::Vec3 velocity;       // the tick's average (node +0x44)
		bool fixed = false;           // the non-mobile response (0xc430)
		bool controlObject = false, helicopter = false, ladder = false, particle = false;
		uint32_t material = 0; // the template's material (pass 2's B side)
		physics::ShareNode node;
		physics::Vec3 pointVelocity(const physics::Vec3& point) const
		{
			return body ? body->body.pointVelocity(point) : physics::Vec3 {};
		}
	};
	MeshActor actorOf(Body& body) const;
	MeshActor actorOf(const StaticPart& part) const;
	void collideBodies(float seconds);
	void collideMover(Body& mover, size_t index, float seconds);
	// Slot 9 of the mobile mesh response (0x006bb050): A's mesh against
	// B's; `trunkOnly` runs the third pass alone (its last argument).
	// Returns false when it destroyed A (the helicopter exit).
	bool meshPair(MeshActor& a, MeshActor& b, float seconds, bool trunkOnly);
	void wheelProbes(Body& body, MeshActor& b, float seconds);
	void addMeshContact(MeshActor& a, MeshActor& b, const physics::Vec3& point, const physics::Vec3& normal, float depth,
		uint32_t materialA, uint32_t materialB, bool trunkPass);
	std::vector<uint32_t> materialMap(const world::ObjectTemplate& objectTemplate) const;
	void buildStaticCollision();
	void addStaticCollision(const world::ObjectTemplate& objectTemplate, const physics::Mat3& rotation,
		const physics::Vec3& position, uint32_t owner, int depth);

	struct HitBone {
		int node;            // skeleton node
		uint32_t material;
		physics::Vec3 offset; // in the bone's frame
		float radius;
		float length;         // along the bone's -y
	};
	struct Player {
		int id = 0;
		std::string name;
		bool ai = false;
		bool alive = false;
		int rank = 0; // setPlayerRank (the commander's applicants)
		float respawnIn = 0;
		int kills = 0, deaths = 0, teamKills = 0;
		SoldierInput input;
		std::vector<HitBone> hitBones;
		int spawnChoice = -1;
		int squad = 0;
		bool squadLeader = false;
		std::string kitTemplate, soldierTemplate; // the next spawn's kit (empty: the team's assault kit)
		std::vector<std::string> unlocked; // ItemContainer templates
		bool allUnlocked = false;
		// The kit's weapons by slot (itemIndex: 1 knife, 2 sidearm, 3
		// primary, then gadgets); the one in hand is `weapon`, the others
		// keep their state here.
		struct Carried {
			std::string name;
			int slot;
			std::unique_ptr<FireArm> weapon;
		};
		std::vector<Carried> carried;
		int inHand = -1; // index into carried
		bool selectHeld = false;
		bool abilityActive = false; // the gadget in hand is working this tick
		bool cloaked = false;       // CamouflageAmmoComp held with the trigger down
		bool manDown = false;       // killed, the body lying (sv.manDownTime)
		bool spawnedProne = false;  // the leader's scatter found room only lying (spawn point +0x18a)
		float manDownLeft = 0;
		// The ability battery (soldier +0x338, read and drawn through
		// component 0xc4d7 slots 0x90/0x98): 0..1, -1 without a kit.
		float battery = -1;
		// The unlocks' ability.increasedRepairAbility (player +0x30c
		// +0x48, copied by 0x005706b0 when above 0): scales the
		// abilities' strength when above 0.
		float abilityModifier = 0;
		double diedAt = 0;          // soldier +0x290: the death's time
		uint32_t object = 0;
		physics::SoldierBody body;
		SoldierControl control;
		std::unique_ptr<FireArm> weapon;
		const ProjectileSettings* projectile = nullptr;
		std::string muzzleEffect; // the weapon's e_muzz_* child
		int team = 0;
		uint32_t vehicle = 0;     // the vehicle sat in, 0 on foot
		int seat = -1;            // its seat (0 the driver's)
		bool switchHeld = false;
		std::string weaponTemplate;
		int roundsFired = 0;      // since the scripts last cleared them
		int roundsHit = 0;
		bool useHeld = false;
		// Animation: the soldier's system and the weapon's over it.
		std::unique_ptr<anim::AnimationState> bodyAnimation;
		std::unique_ptr<anim::AnimationState> weaponAnimation;
		anim::LocalPose pose; // kept between updates: bones nothing moves keep theirs
		const anim::Skeleton* skeleton = nullptr; // skeleton3P
		std::vector<anim::Matrix> worldPose; // the last pose, for drawing and the weapon's parts
		std::vector<int> weaponNodes;        // skeleton nodes mesh1, mesh2...
		uint32_t weaponObject = 0;           // the held weapon, an object of its own
		uint32_t kitObject = 0;
		float lastYaw = 0;
		bool fired = false;
		// Dead: the body as a ragdoll, and the push of the explosion that
		// killed it (the soldier's 0x2b0, kept by 0x005ee4d0).
		std::unique_ptr<anim::RagDoll> ragDoll;
		physics::Vec3 ragDollForce;
		double ragDollForceAt = -1; // when it came: it lasts 0.5 s (0x2bc)
		// Seated: the seat's animation system (seatAnimationSystem).
		std::unique_ptr<anim::AnimationState> seatAnimation;
		std::string seatAnimationPath;
	};
	anim::AnimationSystems* m_animationSystems = nullptr;
	AnimationLoader m_loadAnimationSystem;
	void stepAnimation(Player& player, float seconds);
	// `thrown`: a body thrown out of its seat, pushed along its forward by
	// rd_dieInVehicleForce (0x005f0f50 with +0x2eb) instead of a blast's push.
	bool startRagDoll(Player& victim, bool thrown = false);
	void stepRagDoll(Player& player, float seconds);
	anim::RagDollWorld ragDollWorld(const anim::Vec& middle, float reach) const;
	void poseSeated(Player& player, world::Object& soldier, const Body& body, const SeatLayout& seat, float seconds);
	void stepControlPoints(float seconds);
	ScriptEventSink m_events;
	void raise(const std::string& event, const ScriptArgs& args)
	{
		if (m_events) {
			m_events(event, args);
		}
	}
	void followSoldier(const Player& player, const world::Object& soldier);
	// Where a soldier looks (degrees): its body's yaw plus the head's
	// twist (SoldierControl, 0x0058a6d0).
	float lookYaw(const Player& player) const;
	void enterOrLeaveVehicle(Player& player, world::Object& soldier);
	std::map<std::string, std::unique_ptr<ProjectileSettings>> m_projectileSettings;
	std::vector<Projectile> m_projectiles;
	std::vector<Impact> m_impacts;
	std::map<uint32_t, float> m_hitPoints;
	effects::EffectManager m_effects;
	void fire(const Player& player, float aimPitch);
	void explode(const physics::Vec3& at, const ProjectileSettings& settings, int attacker, float facing = 0);
	float blastShare(const physics::Vec3& at, const Player& target, float beside) const;
	void empBlast(const physics::Vec3& at, const ProjectileSettings& settings,
		const std::function<float(const physics::Vec3&)>& distanceTo);
	// Gadgets' abilities (ammo.replenishingType): a source treats what is
	// within its radius each tick, by strength times the cell of its
	// ability material and the target's armour material, per second.
	// Hubs are thrown out and stay (one per owner); the repair tool works
	// round the soldier while firing.
	struct Ability {
		bool ammo = false;     // RTAmmo; RTHeal otherwise
		float radius = 0;
		float strength = 0;
		uint32_t material = 0;
		float drain = 0;       // abilityDrain: of the holder's battery a second per target treated
	};
	static bool abilityOf(const world::ObjectTemplate& gadget, Ability& ability, bool& whileFiring);
	struct Supply {
		uint32_t object;
		int owner;
		int team;
		Ability ability;
		std::string gadget; // fire.maxProjectilesInWorld 1: one per owner and gadget
		bool crate = false; // works as a SupplyObject (m_crates), not by the gadget's ability
	};
	// The in-hand ability's targets (0x005d8ac0): rescanned every second
	// with none, every two with some, and treated in between.
	struct AbilityScan {
		float timer = 0;
		std::vector<uint32_t> targets;
	};
	std::map<int, AbilityScan> m_abilityScans;
	std::vector<Supply> m_supplies;
	std::map<uint32_t, float> m_resupply; // ammo owed to soldiers (fractions of a magazine)
	// `holder`: the player whose battery pays and whose unlocks scale it
	// (the gadget's holder, or the hub's owner); none: no battery.
	static constexpr uint32_t kSoldierFootStepsMaterial = 5500; // 0x157c
	void playRagDollImpact(anim::RagDoll& ragDoll, const anim::Vec& at);
	void applyAbility(const Ability& ability, const physics::Vec3& at, int team, float seconds, uint32_t skip,
		AbilityScan* scan = nullptr, Player* holder = nullptr);
	void stepAbilities(float seconds);
	void stepProjectiles(float seconds);
	void steerSeeker(Projectile& projectile, float seconds);
	void damage(uint32_t object, float amount, uint32_t projectileMaterial, uint32_t surfaceMaterial,
		int attacker = -1); // the attacking player
	std::vector<std::unique_ptr<Player>> m_players; // by id
	Player* m_player = nullptr; // the local player
	SoldierInput m_playerInput;
	float m_spawnTime = 15;
	Player* findPlayer(int id) const;
	Player* soldierPlayer(uint32_t object) const;
	world::Object* spawnSoldier(Player& player);
	void kill(Player& victim, Player* attacker, uint32_t weapon);
	// What becomes of the body at PlayerDeath: it stays (a corpse, see
	// stepCorpses), it is slumped in its seat (m_seatCorpses), or it goes
	// at once (the occupants of a destroyed vehicle, 0x005cacf0).
	enum class BodyFate { Corpse, Seat, Remove };
	void finishDeath(Player& victim, BodyFate fate = BodyFate::Corpse);
	void revive(Player& victim, Player* medic, float restoreHitPoints);
	Player* downedPlayer(uint32_t object) const;
	void shockDowned(Player& victim, const ProjectileSettings& shock, float amount, uint32_t surfaceMaterial, int attacker);
	float m_manDownTime = 15;
	std::map<std::pair<int, int>, uint32_t> m_beacons; // (team, squad) -> beacon object
	std::vector<int> squadMembers(int team, int squad) const;
	bool spawnAtSquad(Player& player, world::Object*& at, physics::Vec3& position, float& yaw);
	bool scatterOnLeader(const Player& leader, physics::Vec3& feet, float& yaw, bool* prone = nullptr);
	bool soldierFits(const physics::Vec3& origin, SoldierPose pose, uint32_t ignore) const;
	float m_waterLevel = 0;
	bool m_hasWater = false;
	void stepPlayers(float seconds);
	void stepSoldier(Player& player, float seconds);
	// Takes the carried weapon of a slot in hand.
	void equip(Player& player, int slot);
	const ProjectileSettings* projectileFor(const std::string& projectileTemplate);
	world::ObjectTemplate* beaconTemplate(const world::ObjectTemplate* weapon) const;
	world::ObjectTemplate* deployableTemplate(const world::ObjectTemplate* weapon) const;
	std::map<std::pair<int, std::string>, uint32_t> m_deployed; // (owner, gadget) -> object
	bool removeDeployed(uint32_t object);
	// Drones: the ForceObject's template settings (0x0062d4f0; offsets in
	// the template) that hold the drone by its target, the owner.
	struct ForceSettings {
		// regulatePosX/Y/Z (0x270, 0x278, 0x280): each axis's regulator,
		// as the real and imaginary parts of its poles.
		float regulatePos[3][2] = {};
		float positionToleranceXZ = 0;    // 0x2a0
		float warningDistance = 0;        // 0x2a4
		float destroyDistance = 0;        // 0x2a8
		float minHeightAdjustDistance = 0; // 0x2ac
		bool destroyWhenTargetLost = false; // 0x295
	};
	struct Drone {
		uint32_t object = 0;
		int owner = -1;
		int team = 0;
		// The sentry's auto controller: the gun's heading (radians), its
		// target and when it was last seen.
		float gunYaw = 0;
		int target = -1;
		float targetSeenAt = 0;
		ForceSettings force;
		float hoverHeight = 0; // the target's physics origin over its feet (see stepDrones)
		physics::Vec3 position, velocity;
		float yaw = 0;
		float height = 0;      // the height it holds (0x1a8), kept while the owner is near
		bool hasHeight = false;
		bool warning = false;  // past warningDistance (0x1e9)
		// Its weapon: a recon drone's pulse (PulseMeterAmmoComp) or a
		// sentry drone's gun, fired by its auto controller.
		std::unique_ptr<FireArm> weapon;
		const ProjectileSettings* projectile = nullptr;
		bool pulse = false;
		float pulseDistance = 0; // pulse.viewDistance
		float revealTime = 0;    // pulse.revealTime
		float empLevel = 0;      // its armour's EMP level (see empBlast)
		DeviationBuildUp gunDeviation; // the gun's firing build-up (setFireDev)
	};
	std::vector<Drone> m_drones;
	// Soldiers killed in a seat that has a die bundle: the body stays,
	// slumped by the seat's animation system (0x005cea00, 0x005ed890).
	struct SeatCorpse {
		uint32_t object = 0;
		uint32_t vehicle = 0;
		int seat = -1;
		std::unique_ptr<anim::AnimationState> animation;
		const anim::Skeleton* skeleton = nullptr;
		anim::LocalPose pose;
		std::vector<anim::Matrix> worldPose;
	};
	std::vector<SeatCorpse> m_seatCorpses;
	std::vector<SoundEvent> m_sounds;
	void stepSeatCorpses(float seconds);
	// Bodies after PlayerDeath, with their ragdolls (the soldier's armour
	// stays a wreck until its countdown, 0x00652380, removes the object).
	struct Corpse {
		uint32_t object = 0;
		std::unique_ptr<anim::RagDoll> ragDoll;
		const anim::Skeleton* skeleton = nullptr;
		anim::LocalPose pose;
		std::vector<anim::Matrix> worldPose;
		double diedAt = 0;          // the armour's wreck time (+0x60)
		float landedDelay = 4;      // ragdollLandedDelay (+0x3bc)
		float stayAsWreck = 20;     // armor.timeToStayAsWreck (+0x158)
		float afterDestroyed = 0;   // armor.timeToStayAfterDestroyed (+0x15c)
		double fadeFrom = -1;       // +0x64: when the countdown began
	};
	std::vector<Corpse> m_corpses;
	void stepCorpses(float seconds);
	// Remote soldiers' bodies (driveRemoteRagDoll); no armour countdown:
	// the server removes the ghost.
	struct RemoteBody {
		uint32_t object = 0;
		std::unique_ptr<anim::RagDoll> ragDoll;
		const anim::Skeleton* skeleton = nullptr;
		anim::LocalPose pose;
		std::vector<anim::Matrix> worldPose;
	};
	std::vector<RemoteBody> m_remoteBodies;
	void stepRemoteBodies(float seconds);
	struct RemoteWreck {
		double wreckedAt = 0;  // armour +0x60
		double fadeFrom = -1;  // +0x64: when the countdown began
		bool attached = false; // a player still on it (the PCO's slot 0x10)
		bool soldier = false;  // class 0x9493: lands, waits and fades
		float landedDelay = 4, stayAsWreck = 20, afterDestroyed = 0;
	};
	std::map<uint32_t, RemoteWreck> m_remoteWrecks;
	void stepRemoteWrecks();
	std::map<uint32_t, double> m_remoteBodyStarted; // when each remote body began (its death on the client)
	bool m_hasCamera = false;
	physics::Vec3 m_cameraPosition, m_cameraForward {0, 0, 1};
	void setRagDollLevels();
	float ragDollDistance(const anim::RagDoll& body) const;
	std::function<double()> m_clock;
	double m_ragDollFrameStart = 0; // the frame's ragdoll updates began (0x008ae120's counter)
	std::map<std::pair<int, int>, float> m_revealed; // (player, team) -> seconds left
	// The squads' battlefield objects (AIDLL 0x100bf000, 0x100bea30): per
	// (team, squad), when each enemy player was last reported.
	std::map<std::pair<int, int>, std::map<int, double>> m_squadKnowledge;
	world::ObjectTemplate* droneTemplate(const world::ObjectTemplate* weapon) const;
	void launchDrone(Player& player, const world::ObjectTemplate& detonator, world::ObjectTemplate& drone);
	void stepDrones(float seconds);
	void pulse(const Drone& drone);
	void fireSentry(Drone& drone, float seconds);
	bool removeDrone(uint32_t object);
	Drone* findDrone(uint32_t object);

	// The commander (Commander.cpp). Per team (index 1, 2): the SquadManager's
	// commander (0x004c8120) and applicants (0x004c4fb0), the assets' tools
	// (DAT_00a69cb8: 0x005a1440 gathers them, 0x005a1270 fires), the UAV and
	// scan timers (DAT_00a6a390: 0x005a23b0, 0x005a2450) and the supply drop
	// (0x0043d6d0).
	double m_time = 0; // seconds since the round started
	bool m_commanderEnabled = false;
	struct CommanderTeam {
		int commander = -1;
		std::vector<int> applicants;
		int candidate = -1;   // offered command, awaiting the answer (+8 of 0x004c4fb0's list)
		double pickAt = -1;   // when the best applicant is offered command (event 7), -1: not due
		double lastUse[kCommanderAbilities] = {-1e9, -1e9, -1e9, -1e9, -1e9}; // when each tool was used
		float minTime[kCommanderAbilities] = {};  // its recharge then (minTimeBetweenActivation...)
		physics::Vec3 uavAt;
		// The UAV's drone (the UAV manager DAT_00a6a090, slots +4/+8): its
		// object and the circle it flies.
		uint32_t uavDrone = 0;
		physics::Vec3 uavCentre;
		float uavCircle = 0, uavTurnRate = 0, uavAngle = 0;
	};
	void launchUavDrone(int team, const physics::Vec3& at);
	void flyUavDrones(float seconds);
	void removeUavDrone(int team);
	CommanderTeam m_commanders[3];
	std::map<std::pair<int, int>, SquadOrder> m_orders;       // (team, squad): the commander's
	std::map<std::pair<int, int>, SquadOrder> m_leaderOrders; // (team, squad): the leader's
	// An asset: a placed object whose template holds a RemoteControlledObject.
	struct CommanderAsset {
		uint32_t object;
		CommanderAbility ability;
		float minTimeBetweenActivation;
		std::string fireArm; // its GenericFireArm child (the strikes)
	};
	std::vector<CommanderAsset> m_assets;
	std::map<const world::ObjectTemplate*, std::vector<CommanderAsset>> m_assetTemplates; // what each holds
	float m_assetScan = 0; // seconds until the objects are looked through again
	std::vector<Strike> m_strikes;
	struct StrikeFire {
		std::string fireArm;
		int player = -1; // the commander who called it (the kills are its)
		float subUp = 0, subRight = 0; // the last sub-projectile's offset (deviation +0xc, +0x10)
		DeviationBuildUp deviation;    // its firing's build-up (setFireDev)
	};
	std::vector<StrikeFire> m_strikeFire; // alongside m_strikes
	struct Crate {
		uint32_t object = 0;
		int team = 0;
		float storage = 0;   // sharedStorageSize left
		float timeLeft = 0;  // supplyDropNumSecsToLive
		float tick = 0;  // to the next of its 0.2 s updates (event 0x1d)
		std::map<const FireArm*, float> owed; // magazines' fractions refilled
		// The SupplyObject's template when it is a child of the object (a
		// deployed hub's Unl_Hub_*_Supply); null: the object's own.
		const world::ObjectTemplate* settings = nullptr;
	};
	std::vector<Crate> m_crates;
	const CommanderAsset* intactAsset(int team, CommanderAbility ability) const;
	float commanderRecharge(int team, CommanderAbility ability) const;
	void scanCommanderAssets();
	void stepCommander(float seconds);
	void offerCommand(int team);
	void fireStrike(Strike& strike, StrikeFire& fire);
	void stepCrates(float seconds);
	bool isCommanderAsset(uint32_t object) const;
};

} // namespace dice::hfe::game
