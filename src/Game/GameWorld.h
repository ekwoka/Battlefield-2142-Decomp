// The game's simulation of a round: control points and the object
// spawners that put vehicles and emplacements into the world.
//
// In the original these are object classes in BF2142.exe driven by the
// game's per-tick update (docs/engine.md, "A simulation tick"); the object
// spawner is BF2/Game/Objects/Game/ObjectSpawner.cpp (update 0x005af230,
// spawn 0x005aeb80, team change 0x005ae490, round reset 0x005ae580).
// Not rewritten yet: the collision test that keeps a spawn spot clear,
// holding a spawned vehicle in place until it is entered, time to live of
// abandoned vehicles, and activation by events.
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
#include <vector>

namespace dice::hfe::io {
class FileManager;
}
namespace dice::hfe::physics {
class CollisionTemplateManager;
class MaterialManager;
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
		physics::Vec3 position;  // the soldier's feet
		float yaw = 0;           // degrees, 0 facing +z
		physics::Vec3 velocity;
		std::vector<std::pair<int, std::string>> carried; // (slot, weapon template)
		int inHandSlot = 0;
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
	};
	std::vector<VehicleInfo> vehicleList();
	// A player's carried weapons' ammunition (for the bots' AmmoAssist):
	// rounds left in all, the magazine size and the kit's magazines.
	struct WeaponAmmo {
		std::string weapon;
		int rounds = 0;
		int magSize = 0;
		int magazines = 0;
	};
	std::vector<WeaponAmmo> weaponAmmo(int player) const;
	// A vehicle's EMP level (0: running) and whether it is a wreck.
	int empLevel(uint32_t vehicle) const;
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
	void applyForCommander(int player);
	void resignCommander(int player);
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
	bool commanderAction(int team, CommanderAbility ability, float x, float z);
	// The team's UAV while it is up: where, and how far it reveals.
	bool uavArea(int team, physics::Vec3& at, float& radius) const;
	bool satelliteScanActive(int team) const;
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
	};
	CommanderSettings commanderSettings;
	// Orders to squads (Commander.sendOrder / issueOrder): kept per squad
	// for the HUD and the bots, until the next one. Provisional: the
	// order's types are the minimap's order icons in their load order
	// (0x00780000's orderMove..orderMine); the radius (orderRadius) is 50 m.
	enum class OrderType { None, Move, Attack, Defend, Repair, Destroy, Mine };
	struct SquadOrder {
		OrderType type = OrderType::None;
		physics::Vec3 position;
		float radius = 50;
		bool fromCommander = true;
		bool accepted = false; // the squad leader's answer (Commander.commRoseAccept 1)
	};
	// False for a squad that doesn't exist or an order from someone else
	// than the team's commander (-1 for none: the game itself).
	bool issueOrder(int team, int squad, OrderType type, float x, float z, int from = -1);
	void acceptOrder(int player, bool accept);
	const SquadOrder* squadOrder(int team, int squad) const;
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
	// A downed soldier's ragdoll (null for others; docs/engine.md, "Ragdolls").
	const anim::RagDoll* ragDoll(uint32_t soldier) const;

private:
	world::ObjectTemplateManager& m_templates;
	world::ObjectManager& m_objects;
	Random m_random;
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
		std::unique_ptr<Vehicle> vehicle; // engines and wheels
		// Seats: who sits in each (player id, -1 for nobody) and the guns.
		std::vector<int> occupants;
		struct Gun {
			std::unique_ptr<FireArm> weapon;
			const ProjectileSettings* projectile = nullptr;
			float yaw = 0, pitch = 0; // the bundles' rotation, degrees (pitch up positive)
		};
		std::vector<Gun> guns;
		// Armour (armor.*): EMP shuts it down for empLevelTime a step (it
		// takes empDamageNumStep steps at once); destroyed, it lies as a
		// wreck for timeToStayAsWreck.
		int empLevel = 0;
		float empTime = 0;
		float wreckTime = -1; // >= 0 while a wreck
		float wetTime = 0;    // seconds in water
	};
	void destroyVehicle(Body& body, int attacker);
	void stepVehicleDamage(float seconds);
	bool disabled(const Body& body) const { return body.empLevel > 0 || body.wreckTime >= 0; }
	void seatPlayer(Player& player, Body& body, int seat);
	void stepGunner(Player& player, Body& body, float seconds);
	Body* findBody(uint32_t object);
	void collideWithStatics(Body& body);
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
	physics::CollisionWorld m_vehicleCollision; // static objects, vehicle LOD
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
		bool abilityActive = false; // a while-firing gadget is working this tick
		bool cloaked = false;       // CamouflageAmmoComp held with the trigger down
		bool manDown = false;       // killed, the body lying (sv.manDownTime)
		float manDownLeft = 0;
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
		// Seated: the seat's animation system (seatAnimationSystem).
		std::unique_ptr<anim::AnimationState> seatAnimation;
		std::string seatAnimationPath;
	};
	anim::AnimationSystems* m_animationSystems = nullptr;
	AnimationLoader m_loadAnimationSystem;
	void stepAnimation(Player& player, float seconds);
	bool startRagDoll(Player& victim);
	void stepRagDoll(Player& player, float seconds);
	anim::RagDollWorld ragDollWorld() const;
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
	void enterOrLeaveVehicle(Player& player, world::Object& soldier);
	std::map<std::string, std::unique_ptr<ProjectileSettings>> m_projectileSettings;
	std::vector<Projectile> m_projectiles;
	std::vector<Impact> m_impacts;
	std::map<uint32_t, float> m_hitPoints;
	effects::EffectManager m_effects;
	void fire(const Player& player, float aimPitch);
	void explode(const physics::Vec3& at, const ProjectileSettings& settings, int attacker, float facing = 0);
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
	};
	static bool abilityOf(const world::ObjectTemplate& gadget, Ability& ability, bool& whileFiring);
	struct Supply {
		uint32_t object;
		int owner;
		int team;
		Ability ability;
		std::string gadget; // fire.maxProjectilesInWorld 1: one per owner and gadget
	};
	std::vector<Supply> m_supplies;
	std::map<uint32_t, float> m_resupply; // ammo owed to soldiers (fractions of a magazine)
	void applyAbility(const Ability& ability, const physics::Vec3& at, int team, float seconds, uint32_t skip);
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
	void finishDeath(Player& victim);
	void revive(Player& victim, Player* medic);
	Player* downedPlayer(uint32_t object) const;
	void shockDowned(Player& victim, float amount, uint32_t projectileMaterial, uint32_t surfaceMaterial, int attacker);
	float m_manDownTime = 15;
	std::map<std::pair<int, int>, uint32_t> m_beacons; // (team, squad) -> beacon object
	std::vector<int> squadMembers(int team, int squad) const;
	bool spawnAtSquad(Player& player, world::Object*& at, physics::Vec3& position, float& yaw);
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
		ForceSettings force;
		float hoverHeight = 0; // over the owner's feet (provisional, see stepDrones)
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
		float empTime = 0;       // shut down by EMP (provisional)
	};
	std::vector<Drone> m_drones;
	std::map<std::pair<int, int>, float> m_revealed; // (player, team) -> seconds left
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
		double lastUse[kCommanderAbilities] = {-1e9, -1e9, -1e9, -1e9, -1e9}; // when each tool was used
		float minTime[kCommanderAbilities] = {};  // its recharge then (minTimeBetweenActivation...)
		physics::Vec3 uavAt;
	};
	CommanderTeam m_commanders[3];
	std::map<std::pair<int, int>, SquadOrder> m_orders; // (team, squad)
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
	};
	std::vector<StrikeFire> m_strikeFire; // alongside m_strikes
	struct Crate {
		uint32_t object;
		int team;
		float storage;   // sharedStorageSize left
		float timeLeft;  // supplyDropNumSecsToLive
	};
	std::vector<Crate> m_crates;
	const CommanderAsset* intactAsset(int team, CommanderAbility ability) const;
	float commanderRecharge(int team, CommanderAbility ability) const;
	void scanCommanderAssets();
	void stepCommander(float seconds);
	void fireStrike(Strike& strike, StrikeFire& fire);
	void stepCrates(float seconds);
	bool isCommanderAsset(uint32_t object) const;
};

} // namespace dice::hfe::game
