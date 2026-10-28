// Titan mode (gpm_ti): the Titans, their armour and parts, their movement,
// the missile silos and their missiles, the drop pods (docs/engine.md,
// "Titans"). GameWorld's members kept apart from GameWorld.cpp.
//
// In BF2142.exe the Titans are kept by the Titan sync manager, a static at
// DAT_00a67410 (0x005628f0 makes it; vtable 0x00962698). 0x00561c00 (run by
// the round's start, 0x00561cf0) registers it for the physics event 9
// ("ECPhysicsETitanSync") and makes one sync object a team (0x005ac0a0,
// vtable 0x009682f4, team at +0x10) at +8 and +0xc. The server's frame
// (0x004afb20) and the client's (0x00590e10) run 0x00561e20 with the tick,
// which does nothing unless the game mode is "gpm_ti". 0x005ab390 finds the
// team's Titan: the RemoteControlledObject of rcType RCTitan (7, its
// template's +0x29c) of the team whose armour is no wreck; its root is the
// Titan, and the object's minTimeBetweenActivation (+0x2a0, 5 s) is kept
// in ticks at +0x18. 0x00561d90 answers whether an object is either team's
// Titan; the engine asks it all over (the list is in docs/engine.md).
//
// The move (0x005aca70, a tick at a time): the commander's targets wait in
// a map by tick (+0x3c; 0x005abba0 adds one 2 s ahead, 0x005ab4b0 finds the
// one in force). Towards it the Titan pushes with min(*0x00a6afd0 = 0.33,
// (distance - *0x00a6af98 = 5) x *0x00a6afa4 = 5) while it may move
// (+0x35: its armour above *0x00a6afbc = 60 %); other Titans and the tall
// static objects (0x00562360) within *0x00a6af9c = 50 m of its edge push
// it off by (50 - gap) x *0x00a6afc0 = 10 / distance, and a combat area's
// border within 1.25 radii by *0x00a6afb0 = 10; the sum keeps the push's
// length (only its way changes). Then velocity = *0x00a6afb8 = 0.99 x
// (velocity + push x tick) and the position moves by it, x and z only
// (0x005aa0e0 sets the object's position and the node's velocity); with
// sv.allowTitanMovement off it stands. It never turns.
#include "CombatAreas.h"
#include "GameWorld.h"

#include "Physics/CollisionMeshFile.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>

namespace dice::hfe::game {

namespace {

const std::string* setting(const world::ObjectTemplate& t, const char* name)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? nullptr : &it->second;
}

float number(const world::ObjectTemplate& t, const char* name, float fallback)
{
	const std::string* value = setting(t, name);
	return value ? static_cast<float>(std::atof(value->c_str())) : fallback;
}

int integer(const world::ObjectTemplate& t, const char* name, int fallback)
{
	const std::string* value = setting(t, name);
	return value ? std::atoi(value->c_str()) : fallback;
}

bool sameName(const std::string& a, const std::string& b)
{
	const world::CaseInsensitiveLess less;
	return !less(a, b) && !less(b, a);
}

physics::Vec3 vec3Of(const std::string& text)
{
	physics::Vec3 v;
	std::sscanf(text.c_str(), "%f/%f/%f", &v.x, &v.y, &v.z);
	return v;
}

// The node's collision mesh: ObjectTemplate.collisionMesh and
// setCollisionMesh set the same field, the .tweak's (included last) wins
// (eu_titan_playtest's .con names eu_titan_exterior_bellytexturing_combined3,
// its .tweak eu_titan_exterior_main_back).
const std::string* collisionMeshOf(const world::ObjectTemplate& t)
{
	if (const std::string* later = setting(t, "setCollisionMesh")) {
		return later;
	}
	return setting(t, "collisionMesh");
}

// An armour component (createComponent Armor, or armor.* settings).
bool hasArmour(const world::ObjectTemplate& t)
{
	for (const std::string& component : t.components()) {
		if (sameName(component, "Armor")) {
			return true;
		}
	}
	return t.settings().count("armor.maxHitPoints") || t.settings().count("armor.hitPoints");
}

// The tree's RemoteControlledObject of rcType RCTitan (0x004daf74's names:
// RCTitan is 7).
const world::ObjectTemplate* titanControl(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t,
	int depth)
{
	if (depth > 8) {
		return nullptr;
	}
	if (sameName(t.typeName(), "RemoteControlledObject")) {
		if (const std::string* type = setting(t, "rcType"); type && (sameName(*type, "RCTitan") || *type == "7")) {
			return &t;
		}
	}
	for (const world::ChildTemplate& child : t.children()) {
		if (const world::ObjectTemplate* c = templates.find(child.name)) {
			if (const world::ObjectTemplate* found = titanControl(templates, *c, depth + 1)) {
				return found;
			}
		}
	}
	return nullptr;
}

// A mesh's box at a LOD (the vehicle's, else the projectiles'), its first
// geometry's parts.
bool meshBox(const physics::CollisionMeshFile& mesh, physics::Vec3& low, physics::Vec3& high)
{
	low = {1e9f, 1e9f, 1e9f};
	high = {-1e9f, -1e9f, -1e9f};
	for (const uint32_t type : {1u, 0u, 2u}) {
		for (const physics::CollisionSubGeometry& part : physics::collisionParts(mesh, 0)) {
			if (const physics::CollisionLod* lod = physics::collisionLodFor(mesh, part, type)) {
				low = {std::min(low.x, lod->boundsMin[0]), std::min(low.y, lod->boundsMin[1]), std::min(low.z, lod->boundsMin[2])};
				high = {std::max(high.x, lod->boundsMax[0]), std::max(high.y, lod->boundsMax[1]), std::max(high.z, lod->boundsMax[2])};
			}
		}
		if (low.x <= high.x) {
			return true;
		}
	}
	return false;
}

// The Python component's events by id: the host module's event names,
// numbered as the shipped templates use them (Python.onDamageEvent 26 on
// the Titans, onDestroyEvent 46 on the consoles, 48 on the cores, 50 on the
// Titans' guns, 52 on the missile silos, 54 on the silo missile).
const char* pythonEvent(int id)
{
	switch (id) {
	case 25: return "VehicleDestroyed";
	case 26: return "VehicleDamage";
	case 27: return "VehicleRepair";
	case 28: return "VehicleSpawn";
	case 46: return "TitanPartDestroyed";
	case 47: return "TitanPartRepaired";
	case 48: return "TitanCoreDestroyed";
	case 49: return "TitanCoreRepaired";
	case 50: return "TitanWeaponDestroyed";
	case 51: return "TitanWeaponRepaired";
	case 52: return "GroundCannonDestroyed";
	case 53: return "GroundCannonRepaired";
	case 54: return "MissileDestroyed";
	case 55: return "CommanderAssetRepaired";
	case 56: return "VehicleShieldDestroyed";
	default: return nullptr;
	}
}

// The silos' auto controller (AI/AutoControllers.ai, CP_CANNON): fov 179,
// viewDistance 2000, reloadOnTeamChange 1; its weapon's AI template
// (missile_silo_Weapon): minRange 10, maxRange 2000, strength only against
// Titans.
constexpr float kSiloRangeMin = 10;
constexpr float kSiloRangeMax = 2000;
constexpr float kSiloFov = 179;

// The Titan sync's tunables (static initialisers 0x008e2b40-0x008e2d20).
constexpr float kArrival = 5;          // *0x00a6af98
constexpr float kPushPerMetre = 5;     // *0x00a6afa4
constexpr float kPush = 0.33f;         // *0x00a6afd0 (0x3ea8f5c3)
constexpr float kDamping = 0.99f;      // *0x00a6afb8 (0x3f7d70a4)
constexpr float kAvoidRange = 50;      // *0x00a6af9c
constexpr float kAvoidStrength = 10;   // *0x00a6afc0
constexpr float kBorderStrength = 10;  // *0x00a6afb0
constexpr float kMoveAbove = 60;       // *0x00a6afbc: the armour's percent above which it moves
constexpr float kOrderDelay = 2;       // 0x0050f1f0 / 0x005aca70: the tick + round(2 / tick length)

} // namespace

bool GameWorld::titanModeOn() const { return sameName(m_gameMode, "gpm_ti"); }

GameWorld::Titan* GameWorld::titanByObject(uint32_t object)
{
	for (Titan& titan : m_titans) {
		if (titan.object == object) {
			return &titan;
		}
	}
	return nullptr;
}

const GameWorld::Titan* GameWorld::titanByObject(uint32_t object) const
{
	for (const Titan& titan : m_titans) {
		if (titan.object == object) {
			return &titan;
		}
	}
	return nullptr;
}

bool GameWorld::titanPart(uint32_t id, Titan*& titan, int& part)
{
	const Titan* found = nullptr;
	if (!static_cast<const GameWorld*>(this)->titanPart(id, found, part)) {
		return false;
	}
	titan = const_cast<Titan*>(found);
	return true;
}

bool GameWorld::titanPart(uint32_t id, const Titan*& titan, int& part) const
{
	if (m_titans.empty() || id == 0) {
		return false;
	}
	if (const Titan* t = titanByObject(id)) {
		titan = t;
		part = 0;
		return true;
	}
	if (id < kTitanPartIds) {
		return false;
	}
	const uint32_t serial = (id - kTitanPartIds) >> 12, index = (id - kTitanPartIds) & 0xfff;
	for (const Titan& t : m_titans) {
		if (t.serial == serial && index < t.parts.size()) {
			titan = &t;
			part = static_cast<int>(index);
			return true;
		}
	}
	return false;
}

bool GameWorld::isTitan(uint32_t object) const { return titanByObject(object) != nullptr; }

// A Titan gun seat fires while its Titan and its armour are whole (the guns'
// armour: disableInputOnDestroy 1).
bool GameWorld::titanGunWorks(uint32_t object, int seat) const
{
	const Titan* titan = titanByObject(object);
	if (!titan || titan->wrecked) {
		return false;
	}
	if (seat < 0 || static_cast<size_t>(seat) >= titan->seatParts.size()) {
		return true;
	}
	const int part = titan->seatParts[static_cast<size_t>(seat)];
	return part < 0 || titan->parts[static_cast<size_t>(part)].state == 0;
}

// The Titan's wreck puts its gunners out at their seats' exits (the hubs'
// soldierExitLocation, at the consoles inside).
void GameWorld::titanSeatsOut(Titan& titan)
{
	Body* body = findBody(titan.object);
	if (!body) {
		return;
	}
	for (const int occupant : std::vector<int>(body->occupants)) {
		Player* player = occupant >= 0 ? findPlayer(occupant) : nullptr;
		world::Object* soldier = player ? m_objects.find(player->object) : nullptr;
		if (player && soldier && player->vehicle == titan.object) {
			enterOrLeaveVehicle(*player, *soldier);
		}
	}
}

uint32_t GameWorld::titanOf(int team) const
{
	for (const Titan& titan : m_titans) {
		if (titan.team == team && !titan.wrecked) {
			return titan.object;
		}
	}
	return 0;
}

// The armour's percent (vtable 0x40, 0x00651f90): of maxHitPoints above
// 0, of wreckHitPoints below.
float GameWorld::titanPercent(const TitanPart& part) const
{
	if (part.hitPoints >= 0) {
		return part.maxHitPoints > 0 ? part.hitPoints / part.maxHitPoints * 100.0f : 0.0f;
	}
	return part.wreckHitPoints > 0 ? part.hitPoints / part.wreckHitPoints * 100.0f : -100.0f;
}

// A spawned object whose tree holds the team's RCTitan object (0x005ab390).
void GameWorld::registerTitan(uint32_t id, int controlPoint)
{
	world::Object* object = m_objects.find(id);
	const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
	if (!t || titanByObject(id)) {
		return;
	}
	const world::ObjectTemplate* control = titanControl(m_templates, *t, 0);
	if (!control) {
		return;
	}
	Titan titan;
	titan.object = id;
	titan.team = object->team();
	titan.controlPoint = controlPoint;
	titan.orderInterval = number(*control, "minTimeBetweenActivation", 5);
	titan.stayAsWreck = number(*t, "armor.timeToStayAsWreck", 20);         // 0x00630040: 20
	titan.afterDestroyed = number(*t, "armor.timeToStayAfterDestroyed", 0); // and 0
	titan.canBeDestroyed = integer(*t, "armor.canBeDestroyed", 1) != 0;     // +0x166, 1
	titan.serial = m_titanSerial++;
	m_titans.push_back(std::move(titan));
	buildTitanParts(m_titans.back());
	// The Python component's onSpawnEvent (28, VehicleSpawn): gpm_ti.py
	// sets the team's tickets from the Titan's armour, scoringCommon.py
	// keeps it in its Titans' list.
	if (const int event = integer(*t, "Python.onSpawnEvent", 0); event > 0) {
		if (const char* name = pythonEvent(event)) {
			raise(name, {ScriptValue::object(id)});
		}
	}
}

// The Titan's tree: each node with armour is a part (ArmorGLComp with the
// template's settings, ctor 0x00630040's defaults: 10 hit points, 10 wreck
// hit points, parentDamage 0, destroyOnParentDamageLevel and
// destroyOnRootParentDamageLevel -1, onlyDamageFromChildrenLevel -100);
// each node with a collision mesh adds its LODs in the Titan's frame (the
// node is an object of the tree in the engine, so a hit on it is the
// node's, its armour that of the node or the nearest armoured ancestor,
// 0x004c0e40's start). SpawnPoint nodes are the Titan's spawn points.
// Nodes with collisionGroups 256 (the team forcefields: doors, the spawn
// room's, the vehicle bay's shield) are left out of the collision (which
// team they let through is not decoded).
void GameWorld::buildTitanParts(Titan& titan)
{
	world::Object* object = m_objects.find(titan.object);
	if (!object) {
		return;
	}
	titan.parts.clear();
	titan.meshes.clear();
	std::vector<const world::ObjectTemplate*> carriedTemplates;
	const auto addPart = [&](const world::ObjectTemplate& t, int parent, const physics::Mat3& rotation,
							   const physics::Vec3& position) {
		TitanPart part;
		part.t = &t;
		part.parent = parent;
		part.rotation = rotation;
		part.position = position;
		part.armoured = true;
		part.maxHitPoints = number(t, "armor.maxHitPoints", 10);
		part.hitPoints = number(t, "armor.hitPoints", part.maxHitPoints);
		part.effectHitPoints = part.hitPoints;
		part.wreckHitPoints = number(t, "armor.wreckHitPoints", 10);
		part.criticalDamage = number(t, "armor.criticalDamage", 0);
		part.hpLostWhileCritical = number(t, "armor.hpLostWhileCriticalDamage", 0);
		part.parentDamageAlive = number(t, "armor.parentDamageAlive", 0);
		part.parentDamageWreck = number(t, "armor.parentDamageWreck", 0);
		part.destroyOnParentLevel = number(t, "armor.destroyOnParentDamageLevel", -1);
		part.destroyOnRootLevel = number(t, "armor.destroyOnRootParentDamageLevel", -1);
		part.onlyFromChildrenLevel = number(t, "armor.onlyDamageFromChildrenLevel", -100);
		part.destroyEvent = integer(t, "Python.onDestroyEvent", 0);
		// The armour effects, highest level first (the list's order).
		if (const std::string* list = setting(t, "armor.addArmorEffect.list")) {
			size_t from = 0;
			while (from < list->size()) {
				const size_t end = std::min(list->find(';', from), list->size());
				char effect[128] = {}, position[64] = {}, turn[64] = {};
				int level = 0;
				if (std::sscanf(list->substr(from, end - from).c_str(), "%d %127s %63s %63s", &level, effect, position, turn) == 4) {
					const physics::Vec3 r = vec3Of(turn);
					part.effects.push_back({level, effect, vec3Of(position), physics::Mat3::fromYawPitchRoll(r)});
				}
				from = end + 1;
			}
			std::stable_sort(part.effects.begin(), part.effects.end(),
				[](const TitanPart::ArmorEffect& a, const TitanPart::ArmorEffect& b) { return a.level > b.level; });
		}
		part.kind = part.destroyEvent == 46 ? TitanPartInfo::Console
			: part.destroyEvent == 48      ? TitanPartInfo::Core
			: part.destroyEvent == 50      ? TitanPartInfo::Gun
			: part.destroyOnParentLevel > 0 ? TitanPartInfo::Shield
											: TitanPartInfo::Other;
		const size_t index = titan.parts.size();
		part.id = index == 0 ? titan.object : kTitanPartIds + (titan.serial << 12) + static_cast<uint32_t>(index);
		titan.parts.push_back(part);
		return static_cast<int>(index);
	};
	std::function<void(const world::ObjectTemplate&, const physics::Mat3&, const physics::Vec3&, int,
		std::vector<int>, int, std::string)>
		visit = [&](const world::ObjectTemplate& t, const physics::Mat3& rotation, const physics::Vec3& position,
					int parentPart, std::vector<int> chain, int depth, std::string seat) {
			if (depth > 8) {
				return;
			}
			if (depth > 0 && sameName(t.typeName(), "PlayerControlObject") && setting(t, "seatInformation")) {
				seat = t.name();
			}
			int node = parentPart;
			if (depth == 0 || hasArmour(t)) {
				node = addPart(t, parentPart, rotation, position);
				titan.parts[static_cast<size_t>(node)].seat = seat;
				chain.push_back(node);
			}
			if (sameName(t.typeName(), "SpawnPoint") && depth > 0) {
				titan.spawnPoints.push_back({0, position, rotation.yawPitchRoll().x});
			}
			if ((sameName(t.typeName(), "ObjectSpawner") || sameName(t.typeName(), "SupplyObject")) && depth > 0) {
				carriedTemplates.push_back(&t);
				titan.carried.push_back({0, position, rotation.yawPitchRoll().x});
			}
			const std::string* meshName = collisionMeshOf(t);
			if (meshName && integer(t, "hasCollisionPhysics", 1) != 0 && m_collisionTemplates && m_files) {
				const std::string& file = t.creationFile();
				const size_t slash = file.find_last_of("/\\");
				if (const physics::CollisionMeshFile* mesh = m_collisionTemplates->mesh(*meshName, *m_files,
						slash == std::string::npos ? std::string() : file.substr(0, slash + 1))) {
					TitanMesh m;
					m.mesh = mesh;
					m.materials = materialMap(t);
					m.rotation = rotation;
					m.position = position;
					m.node = node;
					m.chain = chain;
					m.groups = integer(t, "collisionGroups", 0);
					titan.meshes.push_back(std::move(m));
				}
			}
			for (const world::ChildTemplate& child : t.children()) {
				const world::ObjectTemplate* c = m_templates.find(child.name);
				if (!c) {
					continue;
				}
				const physics::Mat3 local =
					physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
				visit(*c, local * rotation,
					rotation.transform({child.position.x, child.position.y, child.position.z}) + position, node, chain,
					depth + 1, seat);
			}
		};
	visit(*object->objectTemplate(), physics::Mat3 {}, physics::Vec3 {}, -1, {}, 0, std::string());
	// The gun seats' armour: the first armoured node under each seat's hub
	// (eu_groundcanone3 / eu_aacannon under their _hub: Python.onDestroyEvent
	// 50, disableInputOnDestroy 1).
	titan.seatParts.clear();
	for (const SeatLayout& seat : vehicleLayout(*object->objectTemplate()).seats) {
		int found = -1;
		for (size_t i = 1; i < titan.parts.size() && found < 0; ++i) {
			if (!titan.parts[i].seat.empty() && sameName(titan.parts[i].seat, seat.name)) {
				found = static_cast<int>(i);
			}
		}
		titan.seatParts.push_back(found);
	}
	// The pod arrays: PlayerControlObjects below the root with EntryPoints
	// and a pod-launching firearm in their subtree.
	titan.podArrays.clear();
	std::function<void(const world::ObjectTemplate&, const physics::Mat3&, const physics::Vec3&, int)> arrays =
		[&](const world::ObjectTemplate& t, const physics::Mat3& rotation, const physics::Vec3& position, int depth) {
			if (depth > 8) {
				return;
			}
			Titan::PodArray array;
			bool launcher = false;
			if (depth > 0 && sameName(t.typeName(), "PlayerControlObject")) {
				std::function<void(const world::ObjectTemplate&, const physics::Mat3&, const physics::Vec3&, int)> scan =
					[&](const world::ObjectTemplate& node, const physics::Mat3& r, const physics::Vec3& at, int d) {
						for (const world::ChildTemplate& child : node.children()) {
							const world::ObjectTemplate* c = m_templates.find(child.name);
							if (!c || d > 4) {
								continue;
							}
							const physics::Mat3 local =
								physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z}) * r;
							const physics::Vec3 here = r.transform({child.position.x, child.position.y, child.position.z}) + at;
							if (d == 0 && sameName(c->typeName(), "EntryPoint")) {
								array.entries.push_back({here, number(*c, "entryRadius", number(*c, "setEntryRadius", 2))});
							}
							std::string projectile;
							float velocity = 0;
							physics::Vec3 start;
							if (!launcher && podLauncherOf(*c, projectile, velocity, start, false)) {
								launcher = true;
								array.projectile = projectile;
								array.velocity = velocity;
								array.launcher = here + local.transform(start);
								array.direction = local.transform({0, 0, 1});
							}
							scan(*c, local, here, d + 1);
						}
					};
				scan(t, rotation, position, 0);
			}
			if (launcher && !array.entries.empty()) {
				titan.podArrays.push_back(array);
				return;
			}
			for (const world::ChildTemplate& child : t.children()) {
				if (const world::ObjectTemplate* c = m_templates.find(child.name)) {
					arrays(*c, physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z}) * rotation,
						rotation.transform({child.position.x, child.position.y, child.position.z}) + position, depth + 1);
				}
			}
		};
	arrays(*object->objectTemplate(), physics::Mat3 {}, physics::Vec3 {}, 0);
	// The doors bundle: the part the consoles hang from.
	for (TitanPart& part : titan.parts) {
		if (part.kind == TitanPartInfo::Console && part.parent > 0
			&& titan.parts[static_cast<size_t>(part.parent)].kind == TitanPartInfo::Other) {
			titan.parts[static_cast<size_t>(part.parent)].kind = TitanPartInfo::CoreDoors;
		}
	}
	// Its spawn points: SpawnPoint objects of the Titan's control point,
	// carried with it (stepTitanSpawnPoints).
	for (Titan::Spawn& spawn : titan.spawnPoints) {
		world::ObjectTemplate* t = nullptr;
		const std::function<void(const world::ObjectTemplate&, int)> find = [&](const world::ObjectTemplate& node, int depth) {
			for (const world::ChildTemplate& child : node.children()) {
				world::ObjectTemplate* c = m_templates.find(child.name);
				if (!t && c && sameName(c->typeName(), "SpawnPoint")) {
					t = c;
				} else if (!t && c && depth < 8) {
					find(*c, depth + 1);
				}
			}
		};
		find(*object->objectTemplate(), 0);
		if (t) {
			if (world::Object* point = m_objects.create(t, object->position(), object->rotation())) {
				point->setControlPointId(titan.controlPoint);
				point->setTeam(titan.team);
				spawn.object = point->id();
			}
		}
	}
	// Its object spawners and supply point: objects of their own at their
	// places, carried with it (stepTitanSpawnPoints). A spawner's held spawn
	// (holdObject) is put at the spawner each tick, so it rides along.
	for (size_t i = 0; i < titan.carried.size() && i < carriedTemplates.size(); ++i) {
		world::ObjectTemplate* t = m_templates.find(carriedTemplates[i]->name());
		const physics::Vec3 at = physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y,
									 object->rotation().z}).transform(titan.carried[i].offset)
			+ physics::Vec3 {object->position().x, object->position().y, object->position().z};
		world::Object* made = t ? m_objects.create(t, {at.x, at.y, at.z},
			{object->rotation().x + titan.carried[i].yaw, 0, 0}) : nullptr;
		if (!made) {
			continue;
		}
		titan.carried[i].object = made->id();
		if (sameName(t->typeName(), "ObjectSpawner")) {
			made->setTeam(integer(*t, "team", titan.team));
			auto spawner = std::make_unique<ObjectSpawner>(*made, SpawnerSettings::fromTemplate(*t));
			spawner->setSpotCheck([this](const world::ObjectTemplate& objectTemplate, const world::Vec3& spot,
									  const world::Vec3& rotation, const world::Vec3& place) {
				return testSpawnSpot(objectTemplate, spot, rotation, place);
			});
			spawner->reset(m_random);
			m_newSpawners.push_back(std::move(spawner));
		} else {
			// The supply point: its storage left at -1, never spent
			// (0x005d0650 / 0x005d06a0).
			Crate crate;
			crate.object = made->id();
			crate.team = integer(*t, "team", titan.team);
			made->setTeam(crate.team);
			crate.storage = 1e30f;
			crate.timeLeft = 1e30f;
			crate.settings = t;
			crate.carried = true;
			m_crates.push_back(crate);
		}
	}
	stepTitanSpawnPoints(titan);
	titan.meshesDirty = true;
}

// The Titan's body (made by addBodies from its root's mesh) gets the whole
// tree's soldier and projectile LODs, each face reporting its part; a
// part whose armour is down takes its subtree's collision with it
// (0x00651c60 hides a wrecked node's children; the core room's doors open).
void GameWorld::rebuildTitanMeshes(Titan& titan)
{
	Body* body = findBody(titan.object);
	const world::Object* object = m_objects.find(titan.object);
	if (!body && object && m_files && m_collisionTemplates) {
		// A root mesh without a vehicle LOD (eu_titan_exterior_main_back) gets
		// no body from addBodies; the Titan's is made here: its box from the
		// root's mesh, no points of its own (it meets other objects as a
		// candidate's faces only).
		const world::ObjectTemplate& t = *object->objectTemplate();
		const std::string* name = collisionMeshOf(t);
		const physics::CollisionMeshFile* mesh = name ? m_collisionTemplates->mesh(*name, *m_files) : nullptr;
		physics::Vec3 low, high;
		if (mesh && meshBox(*mesh, low, high)) {
			Body made {};
			made.object = titan.object;
			made.boxMin = low;
			made.boxMax = high;
			made.radius = physics::boxRadius({low.x, low.y, low.z}, {high.x, high.y, high.z});
			made.objectRadius = made.radius;
			made.mass = number(t, "mass", 1);
			made.majorActor = integer(t, "isMajorActor", 0) != 0;
			made.controlObject = true;
			made.vehicleMesh = std::make_shared<physics::CollisionWorld>();
			made.body.position = {object->position().x, object->position().y, object->position().z};
			made.body.rotation =
				physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
			made.body.setBounds(high - low);
			made.body.gravityModifier = 0;
			m_bodies.push_back(std::move(made));
			body = &m_bodies.back();
		}
	}
	if (!body) {
		return;
	}
	// The team forcefields (collisionGroups 0x100 on the PAC Titan's doors,
	// spawn room, corridor and vehicle bay fields and its outer shield, 0x200
	// on the EU Titan's): an object of a team passes what carries its own
	// team's bit (0x006c3890 for a soldier, 0x006bb050 mesh against mesh;
	// the team the first PlayerControlObject up the object's tree's, slot
	// 0x9c; team 1 has 0x100, team 2 0x200); so each team's soldiers meet the
	// soldier mesh and the other team's fields' soldier LODs. The shipped
	// fields have none (their meshes carry the projectile and vehicle LODs
	// only, and a mesh without a soldier LOD stops no soldier): soldiers pass
	// them all, and the team rule sorts vehicles (the vehicle bay's shield,
	// material 4117, lets its own team's in). A projectile has no
	// PlayerControlObject and group 8: rounds meet every field, and only a
	// penetrable material lets them through (the projectiles' penetration).
	auto soldier = std::make_shared<physics::CollisionWorld>();
	auto projectile = std::make_shared<physics::CollisionWorld>();
	std::shared_ptr<physics::CollisionWorld> teams[2];
	std::vector<Titan::VehiclePart> vehicleParts;
	const auto soldierLod = [](const physics::CollisionMeshFile& mesh) {
		for (const physics::CollisionSubGeometry& part : physics::collisionParts(mesh, 0)) {
			if (physics::collisionLodFor(mesh, part, static_cast<uint32_t>(physics::CollisionUser::Soldier))) {
				return true;
			}
		}
		return false;
	};
	const auto live = [&](const TitanMesh& m) {
		return std::none_of(m.chain.begin(), m.chain.end(), [&](int part) {
			return part > 0 && titan.parts[static_cast<size_t>(part)].off;
		});
	};
	const bool teamFaces = std::any_of(titan.meshes.begin(), titan.meshes.end(),
		[&](const TitanMesh& m) { return (m.groups & 0x300) != 0 && live(m) && soldierLod(*m.mesh); });
	for (int team = 0; team < 2 && teamFaces; ++team) {
		teams[team] = std::make_shared<physics::CollisionWorld>();
	}
	for (const TitanMesh& m : titan.meshes) {
		if (!live(m)) {
			continue;
		}
		const uint32_t owner = m.node >= 0 ? titan.parts[static_cast<size_t>(m.node)].id : titan.object;
		const int field = m.groups & 0x300;
		if (field == 0) {
			soldier->addMesh(*m.mesh, physics::CollisionUser::Soldier, m.rotation, m.position, owner, m.materials);
		}
		for (int team = 0; team < 2 && teamFaces; ++team) {
			if (field == 0 || (field & (0x100 << team)) == 0) {
				teams[team]->addMesh(*m.mesh, physics::CollisionUser::Soldier, m.rotation, m.position, owner, m.materials);
			}
		}
		projectile->addMesh(*m.mesh, physics::CollisionUser::Projectile, m.rotation, m.position, owner, m.materials);
		// Its vehicle LOD: a part the vehicles meet (with its own groups).
		for (const physics::CollisionSubGeometry& sub : physics::collisionParts(*m.mesh, 0)) {
			const physics::CollisionLod* lod =
				physics::collisionLodFor(*m.mesh, sub, static_cast<uint32_t>(physics::CollisionUser::Vehicle));
			if (!lod || lod->faces.empty()) {
				continue;
			}
			Titan::VehiclePart part;
			part.mesh = std::make_shared<physics::CollisionWorld>();
			part.mesh->addMesh(*m.mesh, physics::CollisionUser::Vehicle, m.rotation, m.position, owner, m.materials);
			part.groups = static_cast<uint32_t>(m.groups);
			for (size_t i = 0; i < lod->vertices.size(); ++i) {
				const auto& v = lod->vertices[i];
				const physics::Vec3 p = m.rotation.transform({v[0], v[1], v[2]}) + m.position;
				part.boxMin = part.points.empty() ? p : physics::Vec3 {std::min(part.boxMin.x, p.x), std::min(part.boxMin.y, p.y), std::min(part.boxMin.z, p.z)};
				part.boxMax = part.points.empty() ? p : physics::Vec3 {std::max(part.boxMax.x, p.x), std::max(part.boxMax.y, p.y), std::max(part.boxMax.z, p.z)};
				part.points.push_back(p);
				const uint16_t index = i < lod->vertexMaterials.size() ? lod->vertexMaterials[i] : 0;
				part.pointMaterials.push_back(index < m.materials.size() ? m.materials[index] : index);
			}
			for (const physics::Vec3& p : part.points) {
				part.radius = std::max(part.radius, std::sqrt(physics::lengthSquared(p)));
			}
			vehicleParts.push_back(std::move(part));
			break;
		}
	}
	titan.vehicleParts = std::move(vehicleParts);
	body->teamSoldierMesh[0] = teams[0];
	body->teamSoldierMesh[1] = teams[1];
	body->soldierMesh = soldier;
	body->projectileMesh = projectile;
	// The radius (0x00562090): half the diagonal of the root's box across x
	// and z; its bottom (0x005aa090: the box's lowest y).
	titan.radius = 0.5f * std::sqrt((body->boxMax.x - body->boxMin.x) * (body->boxMax.x - body->boxMin.x)
									 + (body->boxMax.z - body->boxMin.z) * (body->boxMax.z - body->boxMin.z));
	titan.bottom = body->boxMin.y;
	titan.meshesDirty = false;
}

void GameWorld::stepTitanSpawnPoints(Titan& titan)
{
	const world::Object* object = m_objects.find(titan.object);
	if (!object) {
		return;
	}
	const physics::Mat3 rotation =
		physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
	const physics::Vec3 at {object->position().x, object->position().y, object->position().z};
	for (const std::vector<Titan::Spawn>* list : {&titan.spawnPoints, &titan.carried}) {
		for (const Titan::Spawn& spawn : *list) {
			if (world::Object* point = m_objects.find(spawn.object)) {
				const physics::Vec3 p = at + rotation.transform(spawn.offset);
				point->setPosition({p.x, p.y, p.z});
				point->setRotation({object->rotation().x + spawn.yaw, 0, 0});
			}
		}
	}
}

// A Titan's carried objects go with it: its spawners first (they hold
// their objects by reference), its supply point.
void GameWorld::removeTitanCarried(Titan& titan)
{
	for (const Titan::Spawn& spawn : titan.carried) {
		const auto erase = [&](auto& list) {
			list.erase(std::remove_if(list.begin(), list.end(),
						   [&](const std::unique_ptr<ObjectSpawner>& s) { return s->object().id() == spawn.object; }),
				list.end());
		};
		erase(m_spawners);
		erase(m_newSpawners);
		m_crates.erase(std::remove_if(m_crates.begin(), m_crates.end(), [&](const Crate& c) { return c.object == spawn.object; }),
			m_crates.end());
		m_objects.remove(spawn.object);
	}
	titan.carried.clear();
}

std::vector<uint32_t> GameWorld::titanSpawnPoints(int team) const
{
	std::vector<uint32_t> list;
	for (const Titan& titan : m_titans) {
		if (titan.team == team) {
			for (const Titan::Spawn& spawn : titan.spawnPoints) {
				if (spawn.object) {
					list.push_back(spawn.object);
				}
			}
		}
	}
	return list;
}

physics::Vec3 GameWorld::titanAim(const Titan& titan) const
{
	const world::Object* object = m_objects.find(titan.object);
	if (!object) {
		return {};
	}
	const physics::Vec3 at {object->position().x, object->position().y, object->position().z};
	if (const Body* body = const_cast<GameWorld*>(this)->findBody(titan.object)) {
		return at + body->body.rotation.transform((body->boxMin + body->boxMax) * 0.5f);
	}
	return at;
}

std::vector<GameWorld::TitanInfo> GameWorld::titans() const
{
	std::vector<TitanInfo> list;
	for (const Titan& titan : m_titans) {
		const world::Object* object = m_objects.find(titan.object);
		if (!object || titan.parts.empty()) {
			continue;
		}
		TitanInfo info;
		info.object = titan.object;
		info.team = titan.team;
		info.controlPoint = titan.controlPoint;
		info.position = {object->position().x, object->position().y, object->position().z};
		info.velocity = titan.velocity;
		const TitanPart& root = titan.parts[0];
		info.hitPoints = root.hitPoints;
		info.maxHitPoints = root.maxHitPoints;
		info.percent = titanPercent(root);
		info.shield = info.percent > kMoveAbove;
		info.canMove = info.percent > kMoveAbove;
		info.wreck = titan.wrecked;
		info.radius = titan.radius;
		info.effectLevel = root.effectLevel;
		info.coreExplosion = titan.coreExplosion;
		for (auto it = titan.targets.rbegin(); it != titan.targets.rend(); ++it) {
			if (it->first <= m_ticks) {
				info.hasTarget = true;
				info.target = {it->second.first, 0, it->second.second};
				break;
			}
		}
		for (const TitanPart& part : titan.parts) {
			if (part.kind == TitanPartInfo::Console) {
				++info.consoles;
				info.consolesLeft += part.state == 0 ? 1 : 0;
			} else if (part.kind == TitanPartInfo::CoreDoors) {
				info.coreExposed = info.coreExposed || part.state != 0;
			} else if (part.kind == TitanPartInfo::Core) {
				info.coreDestroyed = info.coreDestroyed || part.state != 0;
			}
		}
		list.push_back(info);
	}
	return list;
}

std::vector<GameWorld::TitanPartInfo> GameWorld::titanParts(int team) const
{
	std::vector<TitanPartInfo> list;
	for (const Titan& titan : m_titans) {
		const world::Object* object = m_objects.find(titan.object);
		if (titan.team != team || !object) {
			continue;
		}
		const physics::Mat3 rotation =
			physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
		const physics::Vec3 at {object->position().x, object->position().y, object->position().z};
		for (size_t i = 1; i < titan.parts.size(); ++i) {
			const TitanPart& part = titan.parts[i];
			TitanPartInfo info;
			info.name = part.t->name();
			info.id = part.id;
			info.kind = part.kind;
			info.hitPoints = part.hitPoints;
			info.maxHitPoints = part.maxHitPoints;
			info.wreck = part.state != 0;
			info.position = at + rotation.transform(part.position);
			list.push_back(info);
		}
	}
	return list;
}

// The combat areas binding the Titans (vehicle category 4, 0x004d9cc0):
// the point must lie inside them (0x004ca7d0) and, near a border, not
// closer to it than 1.25 radii (0x005aba40 -> 0x005ab740 -> 0x004cad50).
bool GameWorld::titanTerrainClear(const Titan&, float x, float z) const
{
	if (!m_combatAreas || !m_combatAreas->use || !m_combatAreas->any(4)) {
		return true;
	}
	if (!m_combatAreas->inside(x, z, 4)) {
		return false;
	}
	return true;
}

GameWorld::TitanMove GameWorld::moveTitan(int team, float x, float z)
{
	Titan* titan = nullptr;
	for (Titan& t : m_titans) {
		if (t.team == team && !t.wrecked) {
			titan = &t;
		}
	}
	if (!titan) {
		return TitanMove::NoTitan;
	}
	if (m_time - titan->lastOrder < titan->orderInterval) {
		return TitanMove::TooSoon;
	}
	if (!titanTerrainClear(*titan, x, z) || !allowTitanMovement) {
		return TitanMove::Unable;
	}
	titan->lastOrder = m_time;
	titan->targets[m_ticks + static_cast<uint32_t>(std::lround(kOrderDelay * 30.0f))] = {x, z};
	return TitanMove::Accepted;
}

// The tall static objects (0x00562360, made once at the manager's start):
// objects with a static response whose box reaches up to the lowest
// Titan's bottom; with the Titans themselves, what a Titan steers round
// (0x005626a0), each by its position and radius (0x00562090).
void GameWorld::findTitanObstacles()
{
	m_titanObstacles.clear();
	m_titanObstaclesFound = true;
	if (!m_collisionTemplates || !m_files) {
		return;
	}
	float lowest = 1e30f;
	for (const Titan& titan : m_titans) {
		if (const world::Object* object = m_objects.find(titan.object)) {
			lowest = std::min(lowest, object->position().y + titan.bottom);
		}
	}
	for (const auto& owned : m_objects.objects()) {
		const world::ObjectTemplate* t = owned->objectTemplate();
		if (!t || titanByObject(owned->id()) || integer(*t, "hasMobilePhysics", 0) != 0) {
			continue;
		}
		const std::string* name = collisionMeshOf(*t);
		const physics::CollisionMeshFile* mesh = name ? m_collisionTemplates->mesh(*name, *m_files) : nullptr;
		physics::Vec3 low, high;
		if (!mesh || !meshBox(*mesh, low, high) || owned->position().y + high.y < lowest) {
			continue;
		}
		const float radius = 0.5f * std::sqrt((high.x - low.x) * (high.x - low.x) + (high.z - low.z) * (high.z - low.z));
		m_titanObstacles.push_back({{owned->position().x, owned->position().y, owned->position().z}, radius});
	}
}

// Moves the Titan's object and body (0x005aa0e0: the position, then the
// node's velocity across x and z); its spawn points follow. Soldiers
// standing in it ride along by its velocity (the node's, which the
// soldiers' contacts take: 0x006ca660).
void GameWorld::moveTitanBody(Titan& titan, const physics::Vec3& delta)
{
	world::Object* object = m_objects.find(titan.object);
	if (!object) {
		return;
	}
	const world::Vec3 at = object->position();
	object->setPosition({at.x + delta.x, at.y + delta.y, at.z + delta.z});
	if (Body* body = findBody(titan.object)) {
		body->lastPosition = body->body.position;
		body->body.position = body->body.position + delta;
		body->body.velocity = titan.velocity;
		body->body.angularVelocity = {};
		body->response = physics::ContactResponse {}; // the sync moves it, no contact does
	}
	stepTitanSpawnPoints(titan);
}

void GameWorld::stepTitans(float seconds)
{
	if (m_titans.empty()) {
		return;
	}
	for (auto it = m_titans.begin(); it != m_titans.end();) {
		if (!m_objects.find(it->object)) {
			for (const Titan::Spawn& spawn : it->spawnPoints) {
				m_objects.remove(spawn.object);
			}
			removeTitanCarried(*it);
			it = m_titans.erase(it);
		} else {
			++it;
		}
	}
	for (Titan& titan : m_titans) {
		if (titan.meshesDirty) {
			rebuildTitanMeshes(titan);
		}
	}
	if (!m_titanObstaclesFound && std::all_of(m_titans.begin(), m_titans.end(), [](const Titan& t) { return !t.meshesDirty; })) {
		findTitanObstacles();
	}
	for (Titan& titan : m_titans) {
		// The parts' armour update (0x00654d80 on the server): destroyed
		// with the root's or the parent's armour at or below their levels,
		// and burning down below their critical damage
		// (hpLostWhileCriticalDamage a second, taken each whole second).
		for (size_t i = 1; i < titan.parts.size(); ++i) {
			TitanPart& part = titan.parts[i];
			if (part.state != 0) {
				continue;
			}
			const TitanPart& parent = titan.parts[static_cast<size_t>(std::max(part.parent, 0))];
			if ((part.destroyOnRootLevel >= 0 && titanPercent(titan.parts[0]) <= part.destroyOnRootLevel)
				|| (part.destroyOnParentLevel >= 0 && titanPercent(parent) <= part.destroyOnParentLevel)) {
				titanDamage(part.id, part.hitPoints, -1, true);
				continue;
			}
			if (part.criticalDamage > 0 && part.hpLostWhileCritical > 0 && part.hitPoints < part.criticalDamage) {
				part.criticalTime += seconds;
				if (part.criticalTime >= 1.0f) {
					const float loss = part.hpLostWhileCritical * part.criticalTime;
					part.criticalTime = 0;
					titanDamage(part.id, loss, -1, true);
				}
			} else {
				part.criticalTime = 0;
			}
		}
		if (titan.meshesDirty) {
			rebuildTitanMeshes(titan);
		}
		world::Object* object = m_objects.find(titan.object);
		if (object) {
			// Its armour effects follow it.
			m_effects.setAnchor(titan.object, {object->position().x, object->position().y, object->position().z},
				physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z}));
		}
		if (object && titan.wrecked) {
			// The wreck coasts to a stop (no push: velocity x 0.99 a tick).
			titan.velocity = titan.velocity * std::pow(kDamping, seconds * 30.0f);
			moveTitanBody(titan, titan.velocity * seconds);
			stepTitanWreck(titan, seconds);
			continue;
		}
		if (!object || !titanModeOn()) {
			titan.velocity = {};
			if (Body* body = findBody(titan.object)) {
				body->body.velocity = {};
			}
			continue;
		}
		// The order in force (0x005ab4b0): the latest whose tick has come.
		auto target = titan.targets.end();
		for (auto it = titan.targets.begin(); it != titan.targets.end() && it->first <= m_ticks; ++it) {
			target = it;
		}
		if (target == titan.targets.end()) {
			moveTitanBody(titan, {});
			continue; // no order yet: it hangs where it was spawned
		}
		const bool canMove = titanPercent(titan.parts[0]) > kMoveAbove;
		const float x = object->position().x, z = object->position().z;
		float dx = target->second.first - x, dz = target->second.second - z;
		const float distance = std::sqrt(dx * dx + dz * dz);
		if (distance > 0) {
			dx /= distance;
			dz /= distance;
		}
		const float push = std::min(canMove ? kPush : 0.0f, std::max(distance - kArrival, 0.0f) * kPushPerMetre);
		float ax = dx * push, az = dz * push;
		// Steering off the others (0x005626a0): the other Titans and the tall
		// static objects within the avoidance range of its edge.
		float avoidX = 0, avoidZ = 0;
		const auto avoid = [&](float ox, float oz, float radius) {
			const float px = x - ox, pz = z - oz;
			const float d2 = px * px + pz * pz;
			const float d = std::sqrt(d2);
			if (d > titan.radius + radius + kAvoidRange) {
				return;
			}
			const float gap = std::clamp(d - (titan.radius + radius), 0.0f, kAvoidRange);
			if (d2 > 0) {
				const float f = (kAvoidRange - gap) * kAvoidStrength / d;
				avoidX += px * f;
				avoidZ += pz * f;
			}
		};
		for (const Titan& other : m_titans) {
			const world::Object* o = &other != &titan ? m_objects.find(other.object) : nullptr;
			if (o) {
				avoid(o->position().x, o->position().z, other.radius);
			}
		}
		for (const auto& [at, radius] : m_titanObstacles) {
			avoid(at.x, at.z, radius);
		}
		// A combat area's border within 1.25 radii (0x005ab740) pushes it in.
		float borderX = 0, borderZ = 0;
		if (m_combatAreas && m_combatAreas->use) {
			float d = 0, bx = 0, bz = 0;
			if (m_combatAreas->nearestBorder(x, z, titan.radius * 1.25f, 4, d, bx, bz) && d < titan.radius * 1.25f) {
				borderX = -bx * kBorderStrength;
				borderZ = -bz * kBorderStrength;
			}
		}
		if (!allowTitanMovement) {
			titan.velocity = {};
		} else {
			const float wanted = std::sqrt(ax * ax + az * az);
			float sx = ax + avoidX + borderX, sz = az + avoidZ + borderZ;
			const float sum = std::sqrt(sx * sx + sz * sz);
			if (sum > 0) {
				sx *= wanted / sum;
				sz *= wanted / sum;
			}
			titan.velocity.x = kDamping * (sx * seconds + titan.velocity.x);
			titan.velocity.z = kDamping * (sz * seconds + titan.velocity.z);
			titan.velocity.y = 0;
		}
		moveTitanBody(titan, {titan.velocity.x * seconds, 0, titan.velocity.z * seconds});
	}
}

// The game logic's damage (0x004c0e40) on the Titan's tree. The armour is
// the hit node's or its nearest armoured ancestor's; under 0.1 nothing;
// the Titan itself takes none once its hit points are out (the sync's
// check). A part's onlyDamageFromChildrenLevel above -100 caps what it
// takes directly at what leaves it at that level (the Titan's 0: shots
// bring it down to 0 and no further; only its children's damage, the
// core's, goes below). takeDamageFromOwnTeam 0 spares the team's own.
// Then (the function's end): a part turning wreck hands its parent
// parentDamageWreck % of the parent's maxHitPoints (the consoles' 20: 2 of
// the doors' 10; the doors' and the core's 1); a part alive before or
// after hands it parentDamageAlive x the damage over its hit points before
// x the parent's hit points (the core's 1: the Titan loses the share of
// its hit points the core loses of its own). The Python component's
// onDamageEvent (VehicleDamage) is raised with the damage before the
// armour takes it, as gpm_ti.py subtracts it from getDamage.
bool GameWorld::titanDamage(uint32_t id, float amount, int attacker, bool fromChild, bool drain)
{
	Titan* titan = nullptr;
	int index = 0;
	if (!titanPart(id, titan, index)) {
		return false;
	}
	TitanPart& part = titan->parts[static_cast<size_t>(index)];
	if (amount < 0.1f) {
		return true;
	}
	// The sync's check (0x4c0eb6) lets the wreck's drain (damage type -5) by.
	if (index == 0 && part.hitPoints <= 0 && !drain) {
		return true;
	}
	if (const Player* by = attacker >= 0 ? findPlayer(attacker) : nullptr;
		by && by->team == titan->team && number(*part.t, "armor.takeDamageFromOwnTeam", 1) == 0) {
		return true;
	}
	if (!fromChild && part.onlyFromChildrenLevel > -100) {
		const float level = part.onlyFromChildrenLevel;
		const float percent = titanPercent(part);
		if (percent <= level) {
			return true;
		}
		float cap = 0;
		if (percent < 0) {
			cap = std::fabs(percent) * part.wreckHitPoints * 0.01f;
		} else if (level < 0) {
			cap = (percent * part.maxHitPoints + std::fabs(level)) * part.wreckHitPoints * 0.01f;
		} else {
			cap = part.hitPoints - part.maxHitPoints * level * 0.01f;
		}
		amount = std::min(amount, cap);
		if (amount <= 0) {
			return true;
		}
	}
	// The killing blow (the armour's damage, 0x00656320, on the server):
	// what goes past the hit points left is limited, so a part lands a
	// wreck rather than destroyed: hit points + min(0.8 x wreckHitPoints,
	// (damage - hit points) x 0.5); 0.1 and 0.1 for a mobile object, 0.9 and
	// 0.9 without canBeDestroyed; a Titan's root takes its hit points and
	// stops at 0. So an overkilled console is a wreck (state 1) and its
	// parent, the core room's doors, gets its parentDamageWreck share.
	if (part.hitPoints > 0 && amount > part.hitPoints) {
		if (index == 0) {
			amount = part.hitPoints;
		} else {
			const bool mobile = integer(*part.t, "hasMobilePhysics", 0) != 0;
			const bool destroyable = integer(*part.t, "armor.canBeDestroyed", 1) != 0;
			const float k = !destroyable ? 0.9f : mobile ? 0.1f : 0.8f;
			const float f = !destroyable ? 0.9f : mobile ? 0.1f : 0.5f;
			amount = part.hitPoints + std::min(k * part.wreckHitPoints, (amount - part.hitPoints) * f);
		}
	}
	const int stateBefore = part.state;
	const float before = part.hitPoints;
	if (const int event = integer(*part.t, "Python.onDamageEvent", 0); event > 0) {
		if (const char* name = pythonEvent(event)) {
			raise(name, {ScriptValue::player(attacker), ScriptValue::object(titan->object), ScriptValue::number(amount)});
		}
	}
	const uint32_t serial = titan->serial;
	setTitanPartHitPoints(*titan, index, before - amount, attacker);
	// The scripts may have changed the world: find the Titan again.
	if (!titanPart(id, titan, index) || titan->serial != serial) {
		return true;
	}
	const TitanPart after = titan->parts[static_cast<size_t>(index)];
	if (after.parent < 0) {
		return true;
	}
	const TitanPart& parent = titan->parts[static_cast<size_t>(after.parent)];
	const uint32_t parentId = parent.id;
	const float parentMax = parent.maxHitPoints, parentLeft = parent.hitPoints;
	if (after.state == 1 && stateBefore == 0 && after.parentDamageWreck > 0) {
		titanDamage(parentId, after.parentDamageWreck * 0.01f * parentMax, attacker, true);
	}
	if ((after.state == 0 || stateBefore == 0) && after.parentDamageAlive > 0 && before > 0) {
		titanDamage(parentId, after.parentDamageAlive / before * amount * parentLeft, attacker, true);
	}
	return true;
}

// The armour's setter (0x00655460: kept within -wreckHitPoints and
// maxHitPoints) and its state change (0x00654120).
void GameWorld::setTitanPartHitPoints(Titan& titan, int index, float value, int attacker)
{
	TitanPart& part = titan.parts[static_cast<size_t>(index)];
	part.hitPoints = std::clamp(value, -part.wreckHitPoints, part.maxHitPoints);
	const int state = part.hitPoints > 0 ? 0 : part.hitPoints <= -part.wreckHitPoints ? 2 : 1;
	const bool wrecked = part.state == 0 && state != 0;
	part.state = state;
	titanArmorEffects(titan, index);
	if (wrecked) {
		titanPartWrecked(titan, index, attacker);
	}
}

// The armour effects (0x00654730, then 0x006549a0, after each change of
// the hit points): the percents before (+0x254) and now, each
// ceil(hit points / maxHitPoints x 100) at or above 0 and of
// wreckHitPoints below. With the highest level under the percent now, the
// group started is stopped (0x00652aa0) and none starts; otherwise, when a
// level was passed on the way down (before > level >= now), the group of
// the lowest level at or above the percent now starts, in place of the
// last (one group at a time; skipped levels' groups never start). Its
// effects follow the object (slot 0xf0), except at levels 0 and -100
// (+0x188), which are left in the world. For a Titan's root, the percent
// passing -38 (*0x00a7938c) sends ECGameLogicGLTitanCoreExplosion (event
// 0x14) while the round runs or has ended (no listener of it was found).
void GameWorld::titanArmorEffects(Titan& titan, int index)
{
	TitanPart& part = titan.parts[static_cast<size_t>(index)];
	const auto percentOf = [&](float hitPoints) {
		const float of = hitPoints < 0 ? part.wreckHitPoints : part.maxHitPoints;
		return of > 0 ? static_cast<int>(std::ceil(hitPoints / of * 100.0f)) : 0;
	};
	const int before = percentOf(part.effectHitPoints), now = percentOf(part.hitPoints);
	part.effectHitPoints = part.hitPoints;
	if (index == 0 && before > -38 && now <= -38 && !titan.coreExplosion) {
		titan.coreExplosion = true;
	}
	if (part.effects.empty() || before == now) {
		return;
	}
	const uint32_t tag = 0x7e000000u + static_cast<uint32_t>(index);
	if (part.effects.front().level < now) {
		if (part.effectLevel != 0) {
			m_effects.stopAttached(titan.object, tag);
			part.effectLevel = 0;
		}
		return;
	}
	int group = part.effects.front().level;
	bool passed = false;
	for (const TitanPart::ArmorEffect& effect : part.effects) {
		if (effect.level < now) {
			break;
		}
		group = effect.level;
		passed = passed || effect.level < before;
	}
	if (!passed || group == part.effectLevel) {
		return;
	}
	m_effects.stopAttached(titan.object, tag);
	part.effectLevel = group;
	const world::Object* object = m_objects.find(titan.object);
	if (!object) {
		return;
	}
	const physics::Mat3 turn =
		physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
	const physics::Vec3 at {object->position().x, object->position().y, object->position().z};
	m_effects.setAnchor(titan.object, at, turn);
	for (const TitanPart::ArmorEffect& effect : part.effects) {
		if (effect.level != group) {
			continue;
		}
		const physics::Vec3 offset = part.position + part.rotation.transform(effect.position);
		const physics::Mat3 rotation = effect.rotation * part.rotation;
		if (group == 0 || group == -100) {
			m_effects.spawn(effect.effect, at + turn.transform(offset), rotation * turn);
		} else {
			m_effects.spawnAttached(effect.effect, titan.object, offset, rotation, effects::EffectManager::View::Any, tag);
		}
	}
}

// The wreck (the armour's update, 0x00654d80's wreck branch -> 0x00652380,
// for a root that is no soldier, with canBeDestroyed and timeToStayAsWreck
// not -1): on the server its hit points drain by wreckHitPoints /
// (timeToStayAsWreck x 30) a tick (the game logic's damage, type -5),
// three times that with the AI running and the object mobile; the Titan's
// 3,000,000 over 100 s: 1 % a second (3 % with bots), its explosions
// (addArmorEffect -1 to -36) going off on the way. Once timeToStayAsWreck
// has passed or the hit points reach -wreckHitPoints, the countdown starts
// (+0x64) and timeToStayAfterDestroyed (0) later the object is deleted
// (0x00651db0; 0x004a5f90 only logs a Titan deleted while the round runs).
// Meanwhile the sync still moves it (the RCTitan object has no armour, so
// 0x005ab390 keeps the wreck its team's Titan): with no push (its armour is
// under 60 %), its velocity dies away by 0.99 a tick and it coasts to a
// stop at its height. Nothing in the binary makes it fall (gravityModifier
// 0; 0x005aa0e0 keeps the object's y and sets the node's velocity's y to
// 0).
void GameWorld::stepTitanWreck(Titan& titan, float seconds)
{
	if (titan.wreckedAt < 0) {
		titan.wreckedAt = m_time;
	}
	const TitanPart& root = titan.parts[0];
	if (titan.removeFrom < 0 && titan.canBeDestroyed && titan.stayAsWreck != -1) {
		if (m_time > titan.wreckedAt + titan.stayAsWreck || root.hitPoints <= -root.wreckHitPoints) {
			titan.removeFrom = m_time;
		} else if (titan.stayAsWreck > 0) {
			const float perTick = root.wreckHitPoints / (titan.stayAsWreck * 30.0f) * (m_aiRunning ? 3.0f : 1.0f);
			titanDamage(titan.object, perTick * seconds * 30.0f, -1, true, true);
		}
	}
	if (titan.removeFrom >= 0 && m_time >= titan.removeFrom + titan.afterDestroyed) {
		m_effects.releaseAnchor(titan.object);
		m_objects.remove(titan.object);
	}
}

// A part's armour out (0x00654120's wreck branch): its subtree's nodes are
// put out (0x00651c60), its Python component's onDestroyEvent is raised
// with the Titan (the scripts take the root: TitanPartDestroyed for a
// console, TitanCoreDestroyed, TitanWeaponDestroyed). The Titan's own: it
// is a wreck, out of the sync's list (0x005ab390 skips wrecks); its
// destruction is VehicleDestroyed, on which gpm_ti.py ends the round.
void GameWorld::titanPartWrecked(Titan& titan, int index, int attacker)
{
	TitanPart& part = titan.parts[static_cast<size_t>(index)];
	if (index == 0) {
		titan.wrecked = true;
		titan.wreckedAt = m_time;
		titanSeatsOut(titan);
		const uint32_t object = titan.object;
		for (const Titan::Spawn& spawn : titan.spawnPoints) {
			m_objects.remove(spawn.object);
		}
		titan.spawnPoints.clear();
		if (world::Object* o = m_objects.find(object)) {
			o->setDestroyed(true);
		}
		raise("VehicleDestroyed", {ScriptValue::object(object), ScriptValue::player(attacker)});
		return;
	}
	part.off = true;
	titan.meshesDirty = true;
	raiseTitanEvent(part.destroyEvent, titan.object, attacker);
}

void GameWorld::raiseTitanEvent(int eventId, uint32_t object, int attacker)
{
	if (const char* name = eventId > 0 ? pythonEvent(eventId) : nullptr) {
		raise(name, {ScriptValue::object(object), ScriptValue::player(attacker)});
	}
}

// Missile silos: a PlayerControlObject template with isMissileSilo, its
// GenericFireArm's projectile (msilo_missile), the auto controller's
// rules (AI/AutoControllers.ai's CP_CANNON).
void GameWorld::registerSilo(world::Object& object)
{
	const world::ObjectTemplate* t = object.objectTemplate();
	if (!t || integer(*t, "isMissileSilo", 0) == 0) {
		return;
	}
	for (const Silo& silo : m_silos) {
		if (silo.object == object.id()) {
			return;
		}
	}
	Silo silo;
	silo.object = object.id();
	silo.team = object.team();
	// The firearm: depth first, the parents' offsets summed to the barrel's
	// pivot (the bundles turn about their own origins), the muzzle the
	// firearm's offset plus fire.projectileStartPosition along the aim.
	const world::ObjectTemplate* firearm = nullptr;
	std::function<bool(const world::ObjectTemplate&, physics::Vec3, int)> find = [&](const world::ObjectTemplate& node,
																					   physics::Vec3 at, int depth) {
		for (const world::ChildTemplate& child : node.children()) {
			const world::ObjectTemplate* c = m_templates.find(child.name);
			if (!c || depth > 6) {
				continue;
			}
			const physics::Vec3 offset {child.position.x, child.position.y, child.position.z};
			if (sameName(c->typeName(), "GenericFireArm") && setting(*c, "projectileTemplate")) {
				firearm = c;
				silo.muzzle = at;
				const std::string* start = setting(*c, "fire.projectileStartPosition");
				silo.muzzleLength = std::sqrt(physics::lengthSquared(offset)) + (start ? vec3Of(*start).z : 0.0f);
				return true;
			}
			if (find(*c, at + offset, depth + 1)) {
				return true;
			}
		}
		return false;
	};
	find(*t, {}, 0);
	if (!firearm) {
		return;
	}
	silo.reloadTime = number(*firearm, "ammo.reloadTime", 120);
	silo.launchDelay = number(*firearm, "fire.fireLaunchDelay", 0);
	silo.velocity = number(*firearm, "velocity", 4);
	silo.missile = projectileFor(*setting(*firearm, "projectileTemplate"));
	silo.loaded = true; // a full magazine (ammo.magSize 1)
	m_silos.push_back(silo);
}

std::vector<GameWorld::SiloInfo> GameWorld::missileSilos() const
{
	std::vector<SiloInfo> list;
	for (const Silo& silo : m_silos) {
		const world::Object* object = m_objects.find(silo.object);
		if (!object) {
			continue;
		}
		SiloInfo info;
		info.object = silo.object;
		info.team = object->team();
		info.position = {object->position().x, object->position().y, object->position().z};
		info.loaded = silo.loaded;
		info.reloadLeft = silo.loaded ? 0 : silo.reloadLeft;
		info.launched = silo.launched;
		for (const auto& spawner : m_spawners) {
			if (std::find(spawner->spawned().begin(), spawner->spawned().end(), silo.object) != spawner->spawned().end()) {
				info.controlPoint = spawner->settings().controlPointId;
			}
		}
		list.push_back(info);
	}
	return list;
}

// Each silo: a change of hands reloads it (reloadOnTeamChange), the
// magazine refills ammo.reloadTime after a launch (autoReload,
// reloadWithoutPlayer); loaded and held by a team, it fires at the enemy
// Titan in range (10 to 2000 m) and in its view (179 degrees about the
// barrel's upward aim), the missile leaving fire.fireLaunchDelay later.
void GameWorld::stepSilos(float seconds)
{
	for (auto it = m_silos.begin(); it != m_silos.end();) {
		world::Object* object = m_objects.find(it->object);
		if (!object || object->destroyed()) {
			it = m_silos.erase(it);
			continue;
		}
		Silo& silo = *it;
		++it;
		if (object->team() != silo.team) {
			silo.team = object->team();
			silo.loaded = false;
			silo.reloadLeft = silo.reloadTime;
			silo.launchIn = -1;
		}
		if (!silo.loaded) {
			silo.reloadLeft -= seconds;
			if (silo.reloadLeft <= 0) {
				silo.loaded = true;
				silo.reloadLeft = 0;
			}
		}
		if (silo.launchIn >= 0) {
			silo.launchIn -= seconds;
			const Titan* target = titanByObject(silo.target);
			if (silo.launchIn <= 0) {
				silo.launchIn = -1;
				if (target && !target->wrecked) {
					launchMissile(silo, *target);
				}
			}
			continue;
		}
		if (!silo.loaded || (silo.team != 1 && silo.team != 2) || !silo.missile || !titanModeOn()) {
			continue;
		}
		const Titan* enemy = nullptr;
		for (const Titan& titan : m_titans) {
			if (titan.team == 3 - silo.team && !titan.wrecked) {
				enemy = &titan;
			}
		}
		if (!enemy) {
			continue;
		}
		const physics::Vec3 from {object->position().x, object->position().y, object->position().z};
		const physics::Vec3 d = titanAim(*enemy) - from;
		const float range = std::sqrt(physics::lengthSquared(d));
		const float up = range > 0 ? std::asin(std::clamp(d.y / range, -1.0f, 1.0f)) * 57.29578f : 90.0f;
		if (range < kSiloRangeMin || range > kSiloRangeMax || 90.0f - up > kSiloFov * 0.5f) {
			continue;
		}
		silo.loaded = false;
		silo.reloadLeft = silo.reloadTime;
		silo.target = enemy->object;
		silo.launchIn = silo.launchDelay;
		if (silo.launchIn <= 0) {
			silo.launchIn = -1;
			launchMissile(silo, *enemy);
		}
	}
}

// The launch: the barrel (Missile_Silo_Barrel: setMinRotation 0/-90/0,
// setAcceleration 0/-50/0, startAutoRotateOnEnter) turns on its own to its
// limit, straight up; the missile leaves its muzzle upwards at the
// firearm's velocity, locked onto the Titan (the wire-guided LPTargetComp's
// TitanBeacon on it), and turns to it once it tracks.
void GameWorld::launchMissile(Silo& silo, const Titan& target)
{
	const world::Object* object = m_objects.find(silo.object);
	if (!object || !silo.missile) {
		return;
	}
	const physics::Vec3 base {object->position().x, object->position().y, object->position().z};
	const physics::Mat3 turn =
		physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
	const physics::Vec3 pivot = base + turn.transform(silo.muzzle);
	const physics::Vec3 direction {0, 1, 0};
	Projectile p;
	p.origin = pivot + direction * silo.muzzleLength;
	p.position = p.origin;
	p.velocity = direction * silo.velocity;
	p.settings = silo.missile;
	p.timeLeft = silo.missile->timeToLive.sample(m_random);
	p.shooter = silo.object;
	p.player = -1;
	p.team = silo.team;
	p.lockedOn = target.object;
	p.newTarget = true; // the follow's target set (0x005ed260)
	p.yaw = std::atan2(direction.x, direction.z) * 57.29578f;
	m_projectiles.push_back(p);
	++silo.launched;
}

// A TTBeacon missile's flight (FixedTargetComp, DefaultFollowComp; the
// projectile's update 0x005b3e60): from seek.trackingDelay on it turns
// towards the locked Titan's middle (followStep: msilo_missile's
// follow.maxYaw 4 and maxPitch 1 degrees a tick, predictionFactor 0.3 by
// default, magneticBehaviour 1: the first turn after the lock is not
// capped, a Titan behind it is turned to at the full rates); the speed is
// kept along the new heading (hasDirection). From startDelay on its
// acceleration pushes it along its heading up to maxSpeed, unless its
// target is gone (magneticBehaviour without a target: it coasts).
void GameWorld::steerBeacon(Projectile& p, float seconds)
{
	const ProjectileSettings& s = *p.settings;
	const Titan* titan = titanByObject(p.lockedOn);
	if (p.age >= s.seekDelay && titan) {
		followStep(p, titanAim(*titan), titan->velocity, seconds, p.newTarget);
		p.newTarget = false;
	}
	const float speed = std::sqrt(physics::lengthSquared(p.velocity));
	if (p.age < s.startDelay || speed <= 0.01f || speed >= s.maxSpeed || (s.magneticBehaviour && !titan)) {
		return;
	}
	p.velocity = p.velocity * ((speed + std::min(s.acceleration * seconds, s.maxSpeed - speed)) / speed);
}

// Drop pods. A pod launcher is a GenericFireArm with fire.isPodLauncher
// (ThrownFireComp): titan_pod_launcher's on the Titans' pod arrays (left
// button), pod_launcher on the APCs' pod seats (fire.fireInput PIAltFire);
// its projectile is the pod, a Parachute PlayerControlObject with isPod and
// enterOnSpawn that the soldier rides (dontAllowExit) until it lands. The
// pod is a rigid body of its own (its Mesh physics: gravityModifier 1.5,
// drag 1, dragModifier 3/1/3); its ForceObjects steer it: eu_pod_FOFWD
// pushes on PIThrottle with force 35, eu_pod_FOStrafe on PIYaw with 20,
// applied as the drones' are (node slot 0xa0: force x 30 / mass a second).
bool GameWorld::podLauncherOf(const world::ObjectTemplate& node, std::string& projectile, float& velocity,
	physics::Vec3& start, bool altFire) const
{
	if (!sameName(node.typeName(), "GenericFireArm") || integer(node, "fire.isPodLauncher", 0) == 0) {
		return false;
	}
	const std::string* input = setting(node, "fire.fireInput");
	if (altFire != (input && sameName(*input, "PIAltFire"))) {
		return false;
	}
	const std::string* name = setting(node, "projectileTemplate");
	if (!name || !m_templates.find(*name)) {
		return false;
	}
	projectile = *name;
	velocity = number(node, "velocity", 100);
	const std::string* from = setting(node, "fire.projectileStartPosition");
	start = from ? vec3Of(*from) : physics::Vec3 {};
	return true;
}

bool GameWorld::startPod(Player& player, const std::string& projectile, const physics::Vec3& from,
	const physics::Vec3& velocity, uint32_t launcher)
{
	world::ObjectTemplate* t = m_templates.find(projectile);
	world::Object* soldier = m_objects.find(player.object);
	if (!t || !soldier) {
		return false;
	}
	const float yaw = std::atan2(velocity.x, velocity.z) * 57.29578f;
	world::Object* pod = m_objects.create(t, {from.x, from.y, from.z}, {yaw, 0, 0});
	if (!pod) {
		return false;
	}
	pod->setTeam(player.team);
	addBodies();
	if (Body* body = findBody(pod->id())) {
		body->body.velocity = velocity;
		// Clear of what launched it (noInitialCollisionWithLauncher's way,
		// for as long as they touch).
		body->ignored = launcher;
		body->ignoreUntil = m_ticks + 30;
		body->ignoreSet = launcher != 0;
		wake(*body);
	}
	Pod p;
	p.object = pod->id();
	p.player = player.id;
	p.position = from;
	p.velocity = velocity;
	p.mass = std::max(number(*t, "mass", 1), 1.0f);
	for (const world::ChildTemplate& child : t->children()) {
		const world::ObjectTemplate* c = m_templates.find(child.name);
		const std::string* input = c ? setting(*c, "input") : nullptr;
		if (c && input && sameName(c->typeName(), "ForceObject")) {
			if (sameName(*input, "PIThrottle")) {
				p.forward = number(*c, "force", 0);
			} else if (sameName(*input, "PIYaw")) {
				p.strafe = number(*c, "force", 0);
			}
		}
	}
	m_pods.push_back(p);
	player.vehicle = pod->id();
	player.seat = 0;
	soldier->setHidden(true);
	player.body.position = from + physics::Vec3 {0, 1, 0};
	player.body.velocity = {};
	raise("EnterVehicle", {ScriptValue::player(player.id), ScriptValue::object(pod->id())});
	return true;
}

bool GameWorld::launchPod(int id)
{
	Player* player = findPlayer(id);
	if (!player || !player->alive || player->manDown) {
		return false;
	}
	for (const Pod& pod : m_pods) {
		if (pod.player == id) {
			return false;
		}
	}
	if (!player->vehicle) {
		// At a Titan's pod array: the soldier within one of its entry points.
		for (const Titan& titan : m_titans) {
			const world::Object* object = m_objects.find(titan.object);
			if (!object || titan.wrecked || titan.team != player->team) {
				continue;
			}
			const physics::Mat3 rotation =
				physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
			const physics::Vec3 at {object->position().x, object->position().y, object->position().z};
			for (const Titan::PodArray& array : titan.podArrays) {
				for (const auto& [entry, radius] : array.entries) {
					const physics::Vec3 e = at + rotation.transform(entry);
					if (physics::lengthSquared(e - player->body.position) <= (radius + 1) * (radius + 1)) {
						return startPod(*player, array.projectile, at + rotation.transform(array.launcher),
							rotation.transform(array.direction) * array.velocity + titan.velocity, titan.object);
					}
				}
			}
		}
		return false;
	}
	// A seat with a pod launcher on alternate fire (the APCs' pod seats):
	// the pod leaves along the soldier's aim from the seat.
	Body* body = findBody(player->vehicle);
	const world::Object* vehicle = body ? m_objects.find(body->object) : nullptr;
	if (!body || !vehicle || player->seat < 0) {
		return false;
	}
	const VehicleLayout& layout = vehicleLayout(*vehicle->objectTemplate());
	if (static_cast<size_t>(player->seat) >= layout.seats.size()) {
		return false;
	}
	const SeatLayout& seat = layout.seats[static_cast<size_t>(player->seat)];
	const world::ObjectTemplate* seatTemplate = m_templates.find(seat.name);
	std::string projectile;
	float velocity = 0;
	physics::Vec3 start;
	bool found = false;
	std::function<void(const world::ObjectTemplate&, int)> scan = [&](const world::ObjectTemplate& node, int depth) {
		for (const world::ChildTemplate& child : node.children()) {
			const world::ObjectTemplate* c = m_templates.find(child.name);
			if (!c || depth > 4) {
				continue;
			}
			std::string p;
			float v = 0;
			physics::Vec3 s;
			// Conquest's pod_launcher_cq and Titan mode's pod_launcher both
			// sit on the seat; the round's mode picks.
			if (podLauncherOf(*c, p, v, s, true)) {
				const bool cq = c->name().size() > 3 && sameName(c->name().substr(c->name().size() - 3), "_cq");
				if (!found || cq != titanModeOn()) {
					projectile = p;
					velocity = v;
					start = s;
					found = true;
				}
			}
			scan(*c, depth + 1);
		}
	};
	if (seatTemplate) {
		scan(*seatTemplate, 0);
	}
	if (!found) {
		return false;
	}
	const float yaw = body->body.rotation.yawPitchRoll().x * 3.14159265f / 180.0f;
	float gunYaw = 0, gunPitch = player->input.aimPitch;
	gunAim(body->object, player->seat, gunYaw, gunPitch);
	const float y = yaw + gunYaw * 3.14159265f / 180.0f, p = gunPitch * 3.14159265f / 180.0f;
	const physics::Vec3 direction {std::sin(y) * std::cos(p), std::sin(p), std::cos(y) * std::cos(p)};
	const physics::Vec3 from = body->body.position + body->body.rotation.transform(seat.position) + direction * (start.z + 1.5f)
		+ physics::Vec3 {0, 1.5f, 0};
	// Out of the seat first.
	if (static_cast<size_t>(player->seat) < body->occupants.size()) {
		body->occupants[static_cast<size_t>(player->seat)] = -1;
	}
	raise("ExitVehicle", {ScriptValue::player(player->id), ScriptValue::object(body->object)});
	player->vehicle = 0;
	player->seat = -1;
	player->seatAnimation.reset();
	player->seatAnimationPath.clear();
	return startPod(*player, projectile, from, direction * velocity + body->body.velocity, body->object);
}

std::vector<GameWorld::PodInfo> GameWorld::pods() const
{
	std::vector<PodInfo> list;
	for (const Pod& pod : m_pods) {
		list.push_back({pod.object, pod.player, pod.position, pod.velocity});
	}
	return list;
}

// Each pod: steered by its rider's throttle and strafe along the rider's
// heading, the rider carried in it; at rest for half a second (landed) or
// gone, the rider steps out beside it (the Parachute's landing) and the pod
// stays as it lies.
void GameWorld::stepPods(float seconds)
{
	for (auto it = m_pods.begin(); it != m_pods.end();) {
		Pod& pod = *it;
		Player* player = findPlayer(pod.player);
		Body* body = findBody(pod.object);
		world::Object* soldier = player ? m_objects.find(player->object) : nullptr;
		bool done = !player || !player->alive || player->vehicle != pod.object || !soldier;
		pod.age += seconds;
		if (body && !done) {
			pod.position = body->body.position;
			pod.velocity = body->body.velocity;
			const float yaw = player->body.yawDegrees * 3.14159265f / 180.0f;
			const physics::Vec3 forward {std::sin(yaw), 0, std::cos(yaw)}, right {std::cos(yaw), 0, -std::sin(yaw)};
			body->body.velocity = body->body.velocity
				+ (forward * (player->input.throttle * pod.forward) + right * (player->input.strafe * pod.strafe))
					* (30.0f * seconds / pod.mass);
			// keepUp: it stays upright.
			body->body.rotation = physics::Mat3::fromYawPitchRoll({body->body.rotation.yawPitchRoll().x, 0, 0});
			body->body.angularVelocity = {};
			const float speed = std::sqrt(physics::lengthSquared(body->body.velocity));
			pod.still = speed < 1.5f && pod.age > 1.0f ? pod.still + seconds : 0;
			player->body.position = body->body.position + physics::Vec3 {0, 1, 0};
			player->body.velocity = {};
			soldier->setPosition({body->body.position.x, body->body.position.y, body->body.position.z});
			done = pod.still > 0.5f || pod.age > 90.0f;
		} else if (!body) {
			done = true;
		}
		if (done) {
			if (player && player->vehicle == pod.object) {
				player->vehicle = 0;
				player->seat = -1;
				if (soldier) {
					soldier->setHidden(false);
					const physics::Vec3 at = pod.position + physics::Vec3 {1.2f, 1.0f, 0};
					player->body.position = at;
					player->body.velocity = {};
					soldier->setPosition({at.x, at.y - 1.0f, at.z});
				}
				raise("ExitVehicle", {ScriptValue::player(player->id), ScriptValue::object(pod.object)});
			}
			it = m_pods.erase(it);
			continue;
		}
		++it;
	}
}

bool GameWorld::ridingPod(int player) const
{
	return std::any_of(m_pods.begin(), m_pods.end(), [player](const Pod& pod) { return pod.player == player; });
}

bool GameWorld::titanPodEntry(int id, physics::Vec3& entry, float& radius) const
{
	const Player* player = findPlayer(id);
	if (!player || !player->alive || player->vehicle) {
		return false;
	}
	for (const Titan& titan : m_titans) {
		const world::Object* object = m_objects.find(titan.object);
		const Body* body = const_cast<GameWorld*>(this)->findBody(titan.object);
		if (!object || !body || titan.wrecked || titan.team != player->team || titan.podArrays.empty()) {
			continue;
		}
		const physics::Vec3 at {object->position().x, object->position().y, object->position().z};
		const physics::Vec3 local = body->body.rotation.untransform(player->body.position - at);
		if (local.x < body->boxMin.x - 5 || local.x > body->boxMax.x + 5 || local.y < body->boxMin.y - 5
			|| local.y > body->boxMax.y + 5 || local.z < body->boxMin.z - 5 || local.z > body->boxMax.z + 5) {
			continue;
		}
		float best = 1e30f;
		for (const Titan::PodArray& array : titan.podArrays) {
			for (const auto& [offset, r] : array.entries) {
				const physics::Vec3 e = at + body->body.rotation.transform(offset);
				const float d = physics::lengthSquared(e - player->body.position);
				if (d < best) {
					best = d;
					entry = e;
					radius = r;
				}
			}
		}
		return best < 1e29f;
	}
	return false;
}

bool GameWorld::titanGunEntry(int id, physics::Vec3& entry, float& radius, int& seat, const std::vector<int>& skip) const
{
	const int wanted = seat;
	const Player* player = findPlayer(id);
	if (!player || !player->alive || player->vehicle) {
		return false;
	}
	for (const Titan& titan : m_titans) {
		const world::Object* object = m_objects.find(titan.object);
		const Body* body = const_cast<GameWorld*>(this)->findBody(titan.object);
		if (!object || !body || titan.wrecked || titan.team != player->team) {
			continue;
		}
		const physics::Vec3 at {object->position().x, object->position().y, object->position().z};
		const physics::Vec3 local = body->body.rotation.untransform(player->body.position - at);
		if (local.x < body->boxMin.x - 5 || local.x > body->boxMax.x + 5 || local.y < body->boxMin.y - 5
			|| local.y > body->boxMax.y + 5 || local.z < body->boxMin.z - 5 || local.z > body->boxMax.z + 5) {
			continue;
		}
		const auto layout = m_layouts.find(object->objectTemplate()->name());
		if (layout == m_layouts.end()) {
			continue;
		}
		float best = 1e30f;
		for (const VehicleLayout::Entry& e : layout->second.entries) {
			const bool taken = e.seat < 0 || (static_cast<size_t>(e.seat) < body->occupants.size() && body->occupants[static_cast<size_t>(e.seat)] >= 0);
			if (taken || !titanGunWorks(titan.object, e.seat) || (wanted >= 0 && e.seat != wanted)
				|| std::find(skip.begin(), skip.end(), e.seat) != skip.end()) {
				continue;
			}
			const physics::Vec3 w = at + body->body.rotation.transform(e.position);
			const float d = physics::lengthSquared(w - player->body.position);
			if (d < best) {
				best = d;
				entry = w;
				radius = e.radius;
				seat = e.seat;
			}
		}
		return best < 1e29f;
	}
	return false;
}

} // namespace dice::hfe::game
