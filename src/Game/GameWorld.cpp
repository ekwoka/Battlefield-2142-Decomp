#include "GameWorld.h"

#include "Animation/FirstPerson.h"

#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dice::hfe::game {

namespace {

// The ragdoll's script, with the soldiers' animations. Not determinable:
// what runs it. No string in BF2142.exe names it (the exe only names
// ragDollConstraints.inc, which 0x008a8650 writes the constraints to), and no
// .con, .inc, .tweak or .py file in the shipped archives includes it, so
// the loader is not in anything shipped that names the file.
constexpr const char* kRagDollScript = "Objects/Soldiers/Common/Animations/ragDollInit.con";

float number(const world::ObjectTemplate& t, const char* name, float fallback)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? fallback : static_cast<float>(std::atof(it->second.c_str()));
}

int integer(const world::ObjectTemplate& t, const char* name, int fallback)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? fallback : std::atoi(it->second.c_str());
}

bool flag(const world::ObjectTemplate& t, const char* name, bool fallback)
{
	return integer(t, name, fallback ? 1 : 0) != 0;
}

float distance(const world::Vec3& a, const world::Vec3& b)
{
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

physics::Vec3 vectorOf(const world::ObjectTemplate& t, const char* name, physics::Vec3 fallback)
{
	const auto it = t.settings().find(name);
	if (it != t.settings().end()) {
		std::sscanf(it->second.c_str(), "%f/%f/%f", &fallback.x, &fallback.y, &fallback.z);
	}
	return fallback;
}

bool sameName(const std::string& a, const std::string& b)
{
	const world::CaseInsensitiveLess less;
	return !less(a, b) && !less(b, a);
}

physics::Vec3 parseVec3(const std::string& text)
{
	physics::Vec3 v;
	std::sscanf(text.c_str(), "%f/%f/%f", &v.x, &v.y, &v.z);
	return v;
}

} // namespace

SpawnerSettings SpawnerSettings::fromTemplate(const world::ObjectTemplate& t)
{
	SpawnerSettings s;
	s.minSpawnDelay = number(t, "minSpawnDelay", s.minSpawnDelay);
	s.maxSpawnDelay = number(t, "maxSpawnDelay", s.maxSpawnDelay);
	s.timeToLive = number(t, "timeToLive", s.timeToLive);
	s.distance = number(t, "distance", s.distance);
	s.team = integer(t, "team", s.team);
	s.nrOfObjectToSpawn = integer(t, "nrOfObjectToSpawn", s.nrOfObjectToSpawn);
	s.maxNrOfObjectSpawned = integer(t, "maxNrOfObjectSpawned", s.maxNrOfObjectSpawned);
	s.radius = number(t, "radius", s.radius);
	s.useButtonRadius = number(t, "useButtonRadius", s.useButtonRadius);
	s.controlPointId = integer(t, "controlPointId", s.controlPointId);
	s.holdObject = flag(t, "holdObject", s.holdObject);
	s.teamOnVehicle = flag(t, "teamOnVehicle", s.teamOnVehicle);
	s.forwardTeamChangeToSpawnedVehicles =
		flag(t, "forwardTeamChangeToSpawnedVehicles", s.forwardTeamChangeToSpawnedVehicles);
	s.activateOnEvent = integer(t, "activateOnEvent", s.activateOnEvent);
	for (int team = 0; team < 3; ++team) {
		const auto it = t.settings().find("objectTemplate.team" + std::to_string(team));
		if (it != t.settings().end()) {
			s.objectTemplates[team] = it->second;
		}
	}
	return s;
}

// 0x005af6d0 (the object's constructor).
ObjectSpawner::ObjectSpawner(world::Object& object, SpawnerSettings settings)
	: m_object(object)
	, m_settings(std::move(settings))
	, m_team(m_settings.team)
	, m_startTeam(m_settings.team)
	, m_active(m_settings.active)
	, m_remaining(m_settings.nrOfObjectToSpawn)
	, m_spawned(static_cast<size_t>(std::max(m_settings.maxNrOfObjectSpawned, 0)), 0)
{
	// Without a spawn delay at start the first spawn happens at once; the
	// delayed case draws its delay in reset().
	m_countdown = -1;
}

// 0x005ad660.
void ObjectSpawner::startCountdown(Random& random)
{
	m_countdown = (1.0f - random.unit()) * (m_settings.maxSpawnDelay - m_settings.minSpawnDelay) +
		m_settings.minSpawnDelay;
}

void ObjectSpawner::setTeam(int team, RoundState state)
{
	m_team = team;
	if (state == RoundState::BeforeStart) {
		// The round starts with this team and spawns at once.
		m_startTeam = team;
		m_countdown = -1;
	}
	// With forwardTeamChangeToSpawnedVehicles the spawned objects change team
	// too (GameWorld::setControlPointTeam, which owns the objects); the
	// original also ejects their occupants.
}

void ObjectSpawner::reset(Random& random)
{
	m_active = m_settings.active;
	m_lastSpawned = 0;
	m_remaining = m_settings.nrOfObjectToSpawn;
	m_team = m_startTeam;
	std::fill(m_spawned.begin(), m_spawned.end(), 0);
	if (m_active) {
		if (m_settings.spawnDelayAtStart) {
			startCountdown(random);
		} else {
			m_countdown = -1;
		}
	}
	if (m_settings.activateOnEvent != -1) {
		m_active = false;
	}
}

uint32_t ObjectSpawner::spawn(world::ObjectManager& objects, world::ObjectTemplateManager& templates)
{
	world::ObjectTemplate* objectTemplate = templates.find(m_settings.objectTemplates[m_team]);
	if (!objectTemplate) {
		return 0;
	}
	const world::Vec3& at = m_object.position();
	const world::Vec3 position {at.x + m_settings.spawnOffset.x, at.y + m_settings.spawnOffset.y,
		at.z + m_settings.spawnOffset.z};
	// The spot's test (0x005aeb80): an overlapping object in use holds the
	// spawn back (not counted); overlapping ones wait five tries (+0x204),
	// then go unless a soldier (interface 0xc4d7) is within 7 m of the
	// spawner (the object manager's slot 0xa4). Either way nothing spawns
	// this time: the update tries again in a second.
	if (m_spotCheck) {
		const SpotTest test = m_spotCheck(*objectTemplate, position, m_object.rotation(), at);
		if (test.inUse) {
			return 0;
		}
		if (!test.blockers.empty()) {
			if (m_blockedTries < 5) {
				++m_blockedTries;
				return 0;
			}
			if (!test.soldierNear) {
				for (const uint32_t id : test.blockers) {
					objects.remove(id);
				}
			}
			return 0;
		}
	}
	m_blockedTries = 0;
	world::Object* created = objects.create(objectTemplate, position, m_object.rotation());
	if (!created) {
		return 0;
	}
	// Spawned vehicles belong to the spawner's team only with teamOnVehicle;
	// otherwise either side may take them.
	created->setTeam(m_settings.teamOnVehicle ? m_team : 0);
	if (m_remaining > 0) {
		--m_remaining;
	}
	return created->id();
}

void ObjectSpawner::update(float seconds, world::ObjectManager& objects, world::ObjectTemplateManager& templates,
	Random& random)
{
	if (!m_active || m_remaining == 0) {
		return;
	}
	bool maySpawn = true; // one spawn or countdown step per tick
	for (uint32_t& slot : m_spawned) {
		if (m_settings.paused || m_team < 0 || m_team > 2 || m_settings.objectTemplates[m_team].empty()) {
			continue;
		}
		if (slot != 0) {
			// A slot frees up when its object is gone or destroyed.
			if (m_settings.maxSpawnDelay == -1.0f || m_settings.minSpawnDelay == -1.0f) {
				return; // spawns once and never again
			}
			const world::Object* held = objects.find(slot);
			if (!held || held->destroyed()) {
				slot = 0;
			}
			continue;
		}
		if (!maySpawn) {
			continue;
		}
		maySpawn = false;
		if (m_countdown > 0) {
			// The countdown waits while the last spawned object is still
			// standing on the spawn spot.
			bool parked = false;
			if (m_lastSpawned != 0) {
				const world::Object* last = objects.find(m_lastSpawned);
				if (!last) {
					m_lastSpawned = 0;
				} else if (!last->destroyed() && distance(last->position(), m_object.position()) <= m_settings.radius) {
					parked = true;
				}
			}
			if (!parked) {
				m_countdown -= seconds;
			}
			continue;
		}
		// Clear the wreck of the last spawn off the spot.
		if (m_lastSpawned != 0) {
			const world::Object* last = objects.find(m_lastSpawned);
			if (last && last->destroyed() && distance(last->position(), m_object.position()) < m_settings.radius) {
				objects.remove(m_lastSpawned);
				m_lastSpawned = 0;
			}
		}
		if (m_settings.useButtonRadius > 0) {
			continue; // waits for a player to use it (not rewritten; no shipped level sets it)
		}
		const uint32_t id = spawn(objects, templates);
		slot = id;
		if (id == 0) {
			m_countdown = 1.0f; // retry in a second
			continue;
		}
		m_lastSpawned = id;
		m_justSpawned.push_back(id);
		m_holding = m_settings.holdObject;
		startCountdown(random);
	}
}

GameWorld::GameWorld(world::ObjectTemplateManager& templates, world::ObjectManager& objects, uint32_t seed)
	: m_templates(templates), m_objects(objects), m_random(seed), m_effects(templates, seed)
{
	m_clock = [] {
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	};
}

GameWorld::~GameWorld() = default;

void GameWorld::startRound()
{
	m_controlPoints.clear();
	m_spawners.clear();
	m_titans.clear();
	m_silos.clear();
	m_pods.clear();
	m_titanObstacles.clear();
	m_titanObstaclesFound = false;
	m_state = RoundState::BeforeStart;
	const size_t placed = m_objects.objects().size();
	for (size_t i = 0; i < placed; ++i) {
		world::Object& object = *m_objects.objects()[i];
		const world::ObjectTemplate* t = object.objectTemplate();
		if (!t) {
			continue;
		}
		if (t->typeName() == "ControlPoint") {
			ControlPoint point {&object, integer(*t, "controlPointId", -1), integer(*t, "team", 0)};
			point.flag = point.team;
			point.flagHeight = point.team != 0 ? 1.0f : 0.0f;
			object.setTeam(point.team);
			m_controlPoints.push_back(point);
		} else if (t->typeName() == "ObjectSpawner") {
			SpawnerSettings settings = SpawnerSettings::fromTemplate(*t);
			// The object's own control point (Object.setControlPointId) wins.
			if (object.controlPointId() != -1) {
				settings.controlPointId = object.controlPointId();
			}
			auto spawner = std::make_unique<ObjectSpawner>(object, std::move(settings));
			spawner->setSpotCheck([this](const world::ObjectTemplate& objectTemplate, const world::Vec3& spot,
									  const world::Vec3& rotation, const world::Vec3& at) {
				return testSpawnSpot(objectTemplate, spot, rotation, at);
			});
			spawner->reset(m_random);
			m_spawners.push_back(std::move(spawner));
		}
	}
	// Spawners take their control point's team before the round starts.
	for (const ControlPoint& point : m_controlPoints) {
		for (const auto& spawner : m_spawners) {
			if (spawner->settings().controlPointId == point.id && point.id != -1) {
				spawner->setTeam(point.team, m_state);
			}
		}
	}
	m_state = RoundState::Playing;
	buildStaticCollision();
}

void GameWorld::buildStaticCollision()
{
	m_collision = {};
	m_projectileCollision = {};
	m_staticParts.clear();
	m_staticGrid.clear();
	m_ladders.clear();
	if (!m_files) {
		return;
	}
	for (const auto& owned : m_objects.objects()) {
		const world::Object& object = *owned;
		const world::ObjectTemplate* t = object.objectTemplate();
		if (!t || flag(*t, "hasMobilePhysics", false)) {
			continue;
		}
		addStaticCollision(*t,
			physics::Mat3::fromYawPitchRoll({object.rotation().x, object.rotation().y, object.rotation().z}),
			{object.position().x, object.position().y, object.position().z}, object.id(), 0);
	}
}

void GameWorld::addStaticCollision(const world::ObjectTemplate& t, const physics::Mat3& rotation,
	const physics::Vec3& position, uint32_t owner, int depth)
{
	if (depth > 8) {
		return;
	}
	const auto meshName = t.settings().find("collisionMesh");
	if (meshName != t.settings().end()) {
		if (const physics::CollisionMeshFile* mesh = m_collisionTemplates->mesh(meshName->second, *m_files)) {
			const std::vector<uint32_t> materials = materialMap(t);
			m_collision.addMesh(*mesh, physics::CollisionUser::Soldier, rotation, position, owner, materials);
			m_projectileCollision.addMesh(*mesh, physics::CollisionUser::Projectile, rotation, position, owner, materials);
			// A part for the mobile mesh physics (MeshCollision.cpp). Its
			// trunk radius is the node's +0x198, treeCollisionDiameter / 2
			// (0x005f6ba0, set only above 0); with a type-4 LOD, 0.3 under
			// 0.01 (0x006bb050).
			StaticPart part;
			part.object = owner;
			part.root = depth == 0;
			part.ladder = sameName(t.typeName(), "Ladder");
			part.rotation = rotation;
			part.position = position;
			part.shape = meshShape(*mesh, materials);
			part.trunkRadius = std::max(0.0f, number(t, "treeCollisionDiameter", 0) * 0.5f);
			part.material = static_cast<uint32_t>(integer(t, "material", 0));
			part.collisionGroups = static_cast<uint32_t>(integer(t, "collisionGroups", 0));
			if (part.ladder && !mesh->geometries.empty()) {
				Ladder ladder;
				ladder.owner = owner;
				ladder.rotation = rotation;
				ladder.position = position;
				physics::Vec3 low {1e9f, 1e9f, 1e9f}, high {-1e9f, -1e9f, -1e9f};
				for (const physics::CollisionSubGeometry& sub : physics::collisionParts(*mesh, 0)) {
					if (const physics::CollisionLod* lod = physics::collisionLodFor(*mesh, sub, 2)) {
						low = {std::min(low.x, lod->boundsMin[0]), std::min(low.y, lod->boundsMin[1]), std::min(low.z, lod->boundsMin[2])};
						high = {std::max(high.x, lod->boundsMax[0]), std::max(high.y, lod->boundsMax[1]), std::max(high.z, lod->boundsMax[2])};
					}
				}
				if (low.x <= high.x) {
					ladder.boxMin = low;
					ladder.boxMax = high;
					m_ladders.push_back(ladder);
				}
			}
			if (part.shape->hasVehicleLod) { // slot 9 tests nothing of a mesh without one
				addStaticPart(std::move(part));
			}
		}
	}
	// Children sit at their offsets in the parent's space.
	for (const world::ChildTemplate& child : t.children()) {
		const world::ObjectTemplate* childTemplate = m_templates.find(child.name);
		if (!childTemplate) {
			continue;
		}
		const physics::Mat3 local =
			physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
		addStaticCollision(*childTemplate, local * rotation,
			rotation.transform({child.position.x, child.position.y, child.position.z}) + position, owner, depth + 1);
	}
}

ControlPoint* GameWorld::controlPointOf(uint32_t object)
{
	for (ControlPoint& point : m_controlPoints) {
		if (point.object && point.object->id() == object) {
			return &point;
		}
	}
	return nullptr;
}

// The flag moves by takeOverChangePerSecond, which the game mode's script
// sets from who is near; at the top or the bottom the script hears
// ControlPointChangedOwner(cp, top) and decides the new owner.
void GameWorld::stepControlPoints(float seconds)
{
	for (ControlPoint& point : m_controlPoints) {
		if (point.takeOverChangePerSecond == 0) {
			continue;
		}
		const float before = point.flagHeight;
		point.flagHeight = std::clamp(before + point.takeOverChangePerSecond * seconds, 0.0f, 1.0f);
		const bool top = point.flagHeight >= 1 && before < 1;
		const bool bottom = point.flagHeight <= 0 && before > 0;
		if (top || bottom) {
			point.takeOverChangePerSecond = 0;
			raise("ControlPointChangedOwner", {ScriptValue::object(point.object->id()), ScriptValue::integer(top ? 1 : 0)});
		}
	}
}

void GameWorld::setControlPointTeam(int controlPointId, int team)
{
	for (ControlPoint& point : m_controlPoints) {
		if (point.id == controlPointId) {
			point.team = team;
			point.object->setTeam(team);
		}
	}
	for (const auto& spawner : m_spawners) {
		if (spawner->settings().controlPointId != controlPointId) {
			continue;
		}
		spawner->setTeam(team, m_state);
		if (m_state != RoundState::BeforeStart && spawner->settings().forwardTeamChangeToSpawnedVehicles) {
			for (uint32_t id : spawner->spawned()) {
				if (world::Object* spawned = m_objects.find(id)) {
					spawned->setTeam(team);
				}
			}
		}
	}
}

void GameWorld::tick(float seconds)
{
	m_time += seconds;
	++m_ticks;
	for (const auto& spawner : m_spawners) {
		spawner->update(seconds, m_objects, m_templates, m_random);
		// The spawn's hand-over to a PlayerControlObject root (0x005aeb80:
		// slots 0xc0, 0xa4, 0xb8): the countdown starts half a second on,
		// on the server (0x005c6900).
		for (const uint32_t id : spawner->takeSpawned()) {
			const world::Object* created = m_objects.find(id);
			const world::ObjectTemplate* t = created ? created->objectTemplate() : nullptr;
			if (t && sameName(t->typeName(), "PlayerControlObject")) {
				const float ttl = spawner->settings().timeToLive;
				m_abandonment[id] = {spawner->object().id(), ttl, ttl, m_time + 0.5};
				// Titan mode's Titans (0x005ab390) and missile silos (Titans.cpp).
				if (titanModeOn()) {
					registerTitan(id, spawner->settings().controlPointId);
				}
				registerSilo(*m_objects.find(id));
			}
		}
		holdSpawned(*spawner);
	}
	for (auto& made : m_newSpawners) {
		m_spawners.push_back(std::move(made));
	}
	m_newSpawners.clear();
	stepAbandoned();
	// The armours' memories of collisions (0x00654d80's part) run out.
	for (auto it = m_collisionMemory.begin(); it != m_collisionMemory.end();) {
		auto& list = it->second;
		for (CollisionMemory& m : list) {
			m.left -= seconds;
		}
		list.erase(std::remove_if(list.begin(), list.end(), [](const CollisionMemory& m) { return m.left < 0; }), list.end());
		it = list.empty() ? m_collisionMemory.erase(it) : std::next(it);
	}
	if (m_files) {
		addBodies();
		stepTitans(seconds);
		stepPhysics(seconds);
	} else {
		stepTitans(seconds);
	}
	setRagDollLevels();
	stepPlayers(seconds);
	stepPods(seconds);
	stepSilos(seconds);
	stepDrones(seconds);
	stepSeatCorpses(seconds);
	stepDroppedKits();
	stepCorpses(seconds);
	stepRemoteBodies(seconds);
	stepAbilities(seconds);
	stepCommander(seconds);
	stepVehicleDamage(seconds);
	stepProjectiles(seconds);
	stepControlPoints(seconds);
	m_effects.update(seconds);
}

void GameWorld::weaponEffectEvent(int id, int effect)
{
	if (const Player* player = findPlayer(id); player && effect >= 0 && effect < 10) {
		startEffectsOn(*player, effect);
	}
}

uint32_t GameWorld::footstepMaterial(int id) const
{
	const Player* player = findPlayer(id);
	return player ? player->footstepMaterial : 0xffffffffu;
}

std::vector<uint32_t> GameWorld::materialMap(const world::ObjectTemplate& t) const
{
	std::vector<uint32_t> materials;
	for (uint32_t index = 0; index < 64; ++index) {
		const auto it = t.settings().find("mapMaterial." + std::to_string(index));
		if (it == t.settings().end()) {
			break;
		}
		materials.push_back(static_cast<uint32_t>(std::atoi(it->second.c_str())));
	}
	return materials;
}

float GameWorld::hitPoints(uint32_t object) const
{
	const Titan* titan = nullptr;
	int part = 0;
	if (titanPart(object, titan, part)) {
		return titan->parts[static_cast<size_t>(part)].hitPoints;
	}
	const auto known = m_hitPoints.find(object);
	if (known != m_hitPoints.end()) {
		return known->second;
	}
	const world::Object* found = m_objects.find(object);
	const world::ObjectTemplate* t = found ? found->objectTemplate() : nullptr;
	if (!t || t->settings().find("armor.maxHitPoints") == t->settings().end()) {
		return -1;
	}
	return number(*t, "armor.hitPoints", number(*t, "armor.maxHitPoints", 0));
}

void GameWorld::setHitPoints(uint32_t object, float value)
{
	Titan* titan = nullptr;
	int part = 0;
	if (titanPart(object, titan, part)) {
		setTitanPartHitPoints(*titan, part, value, -1); // the scripts' setDamage
		return;
	}
	if (hitPoints(object) < 0) {
		return;
	}
	m_hitPoints[object] = value;
	if (value <= 0 && isCommanderAsset(object)) {
		m_hitPoints[object] = 0; // a wreck until repaired (armor.canBeDestroyed 0)
		return;
	}
	if (value <= 0) {
		if (Player* victim = soldierPlayer(object)) {
			kill(*victim, nullptr, 0);
		} else if (removeDeployed(object) || removeDrone(object)) {
		} else if (world::Object* target = m_objects.find(object)) {
			target->setDestroyed(true);
		}
	}
}

// Armor: hit points less the damage times the cell of the projectile's and
// the surface's materials; nothing left destroys the object, or kills the
// soldier of a player.
void GameWorld::damage(uint32_t object, float amount, uint32_t projectileMaterial, uint32_t surfaceMaterial,
	int attacker)
{
	const float mod = m_materials ? m_materials->damageMod(projectileMaterial, surfaceMaterial) : 1.0f;
	loseHitPoints(object, amount * mod, attacker);
}

void GameWorld::loseHitPoints(uint32_t object, float amount, int attacker)
{
	if (titanDamage(object, amount, attacker)) {
		return; // a Titan's armour (0x004c0e40's Titan rules)
	}
	const float left = hitPoints(object);
	if (left < 0) {
		return; // no armor
	}
	const float now = left - amount;
	m_hitPoints[object] = now;
	if (now > 0) {
		return;
	}
	if (isCommanderAsset(object)) {
		m_hitPoints[object] = 0; // a wreck until repaired (armor.canBeDestroyed 0)
		return;
	}
	if (Player* victim = soldierPlayer(object)) {
		kill(*victim, findPlayer(attacker), 0);
		return;
	}
	if (Body* body = findBody(object); body && body->vehicle) {
		destroyVehicle(*body, attacker);
		return;
	}
	if (removeDeployed(object) || removeDrone(object)) {
		return;
	}
	if (world::Object* target = m_objects.find(object)) {
		target->setDestroyed(true);
	}
}

float GameWorld::timeToLiveLeft(uint32_t object) const
{
	const auto it = m_abandonment.find(object);
	return it == m_abandonment.end() ? -1.0f : it->second.left;
}

// The abandoned vehicle's check (0x005c8ba0, event 0x1d). In use (a seat
// taken, 0x005c8b40) it looks again in 5 s; by its spawner (an
// ObjectSpawner, class 0x94a1, nearer than the spawner's `distance`, its
// template's slot 0x4c) it looks again in 5 s with the countdown back at
// timeToLive; with a living soldier on foot within three times its
// armor.explosionRadius (the Armor template's slot 0x98, +0xfc) in 5 s.
// Otherwise every half second the countdown loses 0.5, and once it is
// out the vehicle loses 0.5 x the spawner's +0x2f0 hit points each time
// (25: nothing sets the 50 at +0x2f0, 0x005af920), or 0.5 without its
// spawner, through the game logic's damage (slot 0xe4, 0x004c0e40) with no
// attacker, until it is destroyed.
void GameWorld::stepAbandoned()
{
	for (auto it = m_abandonment.begin(); it != m_abandonment.end();) {
		world::Object* object = m_objects.find(it->first);
		Body* body = findBody(it->first);
		if (!object || object->destroyed() || (body && body->wreckTime >= 0)) {
			it = m_abandonment.erase(it);
			continue;
		}
		Abandonment& a = it->second;
		++it;
		if (m_time + 1e-6 < a.nextCheck) {
			continue;
		}
		const physics::Vec3 at {object->position().x, object->position().y, object->position().z};
		// In use: a seat taken, or a Titan (0x005c8b40 asks the Titan sync first).
		if (isTitan(it->first) || (body && std::any_of(body->occupants.begin(), body->occupants.end(), [](int p) { return p >= 0; }))) {
			a.nextCheck = m_time + 5;
			continue;
		}
		const world::Object* spawner = m_objects.find(a.spawner);
		const world::ObjectTemplate* spawnerTemplate = spawner ? spawner->objectTemplate() : nullptr;
		if (spawnerTemplate && !sameName(spawnerTemplate->typeName(), "ObjectSpawner")) {
			spawnerTemplate = nullptr;
		}
		if (spawnerTemplate && distance(object->position(), spawner->position()) < number(*spawnerTemplate, "distance", 50)) {
			a.nextCheck = m_time + 5;
			a.left = a.timeToLive;
			continue;
		}
		const world::ObjectTemplate* t = object->objectTemplate();
		const float reach = t ? number(*t, "armor.explosionRadius", 0) * 3.0f : 0.0f;
		bool watched = false;
		for (const auto& owned : m_players) {
			const world::Object* soldier = m_objects.find(owned->object);
			if (!owned->alive || owned->manDown || owned->vehicle || !soldier) {
				continue;
			}
			const physics::Vec3 p {soldier->position().x, soldier->position().y + 1, soldier->position().z};
			if (physics::lengthSquared(p - at) < reach * reach) {
				watched = true;
				break;
			}
		}
		if (watched) {
			a.nextCheck = m_time + 5;
			continue;
		}
		a.nextCheck = m_time + 0.5;
		a.left -= 0.5f;
		if (a.left <= 0) {
			loseHitPoints(object->id(), spawnerTemplate ? 0.5f * 50.0f : 0.5f, -1);
		}
	}
}

// holdObject (the end of 0x005af230): while the spawner holds its last
// spawn, the object's armour standing and its first Engine (0x005ad420)
// turning at under 0.1 (+0x25c), its node's motion is stopped (slot 0x148)
// and the object put at the spawner's matrix moved by spawnOffset. With no
// Engine, a wreck, or the engine running, the hold ends: the node is put
// to sleep (slot 0x154 with 0) at rest (the spawner's parent's velocity at
// it, nothing for a placed spawner) and the vehicle's hold flag cleared
// (interface 0xc4c5 slot 0xc8). The rewrite's vehicles without an engine
// model (not newCar2) take the driver's throttle for the engine's turning.
void GameWorld::holdSpawned(ObjectSpawner& spawner)
{
	if (!spawner.holding()) {
		return;
	}
	world::Object* object = m_objects.find(spawner.lastSpawned());
	if (!object) {
		return;
	}
	Body* body = findBody(object->id());
	const world::ObjectTemplate* t = object->objectTemplate();
	bool release = !t || object->destroyed() || (body && body->wreckTime >= 0) || firstEngineType(*t) < 0;
	if (!release && body) {
		if (body->vehicle) {
			release = std::fabs(body->vehicle->rpm()) >= 0.1f;
		} else if (!body->occupants.empty() && body->occupants[0] >= 0) {
			const Player* driver = findPlayer(body->occupants[0]);
			release = driver && std::fabs(driver->input.throttle) >= 0.1f;
		}
	}
	if (release) {
		spawner.stopHolding();
		if (body) {
			body->held = false;
			body->body.velocity = {};
			body->body.angularVelocity = {};
			sleep(*body);
		}
		return;
	}
	const world::Vec3& p = spawner.object().position();
	const world::Vec3& offset = spawner.settings().spawnOffset;
	object->setPosition({p.x + offset.x, p.y + offset.y, p.z + offset.z});
	object->setRotation(spawner.object().rotation());
	if (body) {
		body->body.position = {p.x + offset.x, p.y + offset.y, p.z + offset.z};
		body->body.rotation = physics::Mat3::fromYawPitchRoll(
			{spawner.object().rotation().x, spawner.object().rotation().y, spawner.object().rotation().z});
		body->body.velocity = {};
		body->body.angularVelocity = {};
		body->body.acceleration = {};
		body->body.torque = {};
		body->body.impulse = {};
		body->body.angularImpulse = {};
		// Its node's motion stopped (slot 0x148): no step and no mover until
		// it is let go; its engines still run (stepPhysics).
		body->held = true;
	}
}

// A destroyed deployable (a shot-down shield) is gone at once.
bool GameWorld::removeDeployed(uint32_t object)
{
	for (auto it = m_deployed.begin(); it != m_deployed.end(); ++it) {
		if (it->second == object) {
			m_objects.remove(object);
			m_hitPoints.erase(object);
			m_deployed.erase(it);
			return true;
		}
	}
	return false;
}

// A vehicle's end: its own explosion (armor.explosion*: the FAV's 300
// over 6 m), everyone aboard dies, and the wreck stays for
// armor.timeToStayAsWreck seconds.
void GameWorld::destroyVehicle(Body& body, int attacker)
{
	world::Object* object = m_objects.find(body.object);
	if (!object || body.wreckTime >= 0) {
		return;
	}
	const world::ObjectTemplate& t = *object->objectTemplate();
	object->setDestroyed(true);
	body.wreckTime = number(t, "armor.timeToStayAsWreck", 10);
	raise("VehicleDestroyed", {ScriptValue::object(body.object), ScriptValue::player(attacker)});
	if (body.vehicle) {
		body.vehicle->setInput({});
	}
	for (const int occupant : body.occupants) {
		if (Player* aboard = findPlayer(occupant)) {
			kill(*aboard, findPlayer(attacker), 0);
		}
	}
	ProjectileSettings blast;
	blast.explosionDamage = number(t, "armor.explosionDamage", 0);
	blast.explosionRadius = number(t, "armor.explosionRadius", 0);
	blast.explosionMaterial = integer(t, "armor.explosionMaterial", 0);
	if (const auto effect = t.settings().find("armor.explosionEffect"); effect != t.settings().end()) {
		blast.endEffect = effect->second;
	}
	if (blast.explosionRadius > 0) {
		explode(body.body.position, blast, attacker, 0, body.object);
	}
}

// Each tick: EMP wears off a step at a time; below armor.criticalDamage a
// vehicle burns down (hpLostWhileCriticalDamage a second); wrecks go when
// their time is up.
void GameWorld::stepVehicleDamage(float seconds)
{
	for (Body& body : m_bodies) {
		world::Object* object = m_objects.find(body.object);
		if (!object || !body.vehicle) {
			continue;
		}
		const world::ObjectTemplate& t = *object->objectTemplate();
		if (body.empLevel > 0) {
			// 0x00652910: one step every empLevelTime (default 5 s).
			body.empLevel = std::max(0.0f, body.empLevel - seconds / number(t, "armor.empLevelTime", 5));
			body.vehicle->setInput({});
		}
		if (body.wreckTime >= 0) {
			body.wreckTime -= seconds;
			if (body.wreckTime < 0) {
				m_objects.remove(body.object); // the body goes with it (stepPhysics)
			}
			continue;
		}
		// What wears a vehicle down: burning below its critical damage,
		// water over armor.waterLevel (deep past deepWaterLevel; metres over
		// its centre) after waterDamageDelay seconds, lying upside down.
		const float left = hitPoints(body.object);
		if (left <= 0) {
			continue;
		}
		float loss = 0;
		const float critical = number(t, "armor.criticalDamage", 0);
		if (critical > 0 && left < critical) {
			loss += number(t, "armor.hpLostWhileCriticalDamage", 0);
		}
		const float depth = m_hasWater ? m_waterLevel - body.body.position.y : -1;
		if (depth > number(t, "armor.waterLevel", 1e9f)) {
			body.wetTime += seconds;
			if (body.wetTime > number(t, "armor.waterDamageDelay", 0)) {
				loss += depth > number(t, "armor.deepWaterLevel", 1e9f) ? number(t, "armor.hpLostWhileInDeepWater", 0)
																		 : number(t, "armor.hpLostWhileInWater", 0);
			}
		} else {
			body.wetTime = 0;
		}
		if (body.body.rotation.transform({0, 1, 0}).y < 0) {
			loss += number(t, "armor.hpLostWhileUpSideDown", 0);
		}
		if (loss > 0) {
			const float now = left - loss * seconds;
			m_hitPoints[body.object] = now;
			if (now <= 0) {
				destroyVehicle(body, -1);
			}
		}
	}
}

std::vector<GameWorld::VehicleInfo> GameWorld::vehicleList()
{
	std::vector<VehicleInfo> list;
	for (Body& body : m_bodies) {
		const world::Object* object = body.vehicle ? m_objects.find(body.object) : nullptr;
		if (!object || !object->objectTemplate()) {
			continue;
		}
		const VehicleLayout& layout = vehicleLayout(*object->objectTemplate());
		int free = 0;
		for (size_t i = 0; i < layout.seats.size(); ++i) {
			free += i >= body.occupants.size() || body.occupants[i] < 0 ? 1 : 0;
		}
		const bool driverFree = !layout.seats.empty() && (body.occupants.empty() || body.occupants[0] < 0);
		list.push_back({body.object, body.body.position, body.body.rotation.yawPitchRoll().x, free, driverFree, !disabled(body),
			body.body.velocity, body.body.angularVelocity, body.body.rotation.rows[2], body.body.rotation.rows[0], {}, {}});
		if (!body.points.empty()) {
			physics::Vec3& lo = list.back().boxMin;
			physics::Vec3& hi = list.back().boxMax;
			lo = hi = body.points.front();
			for (const physics::Vec3& p : body.points) {
				lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
				hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
			}
		}
	}
	return list;
}

std::vector<GameWorld::WeaponAmmo> GameWorld::weaponAmmo(int id) const
{
	std::vector<WeaponAmmo> list;
	const Player* player = findPlayer(id);
	if (!player || !player->alive) {
		return list;
	}
	for (size_t i = 0; i < player->carried.size(); ++i) {
		const Player::Carried& c = player->carried[i];
		const FireArm* weapon = c.weapon ? c.weapon.get()
			: static_cast<int>(i) == player->inHand ? player->weapon.get() : nullptr;
		if (!weapon) {
			// Not taken in hand yet: as the kit gives it.
			const world::ObjectTemplate* t = m_templates.find(c.name);
			if (t) {
				const FireArmSettings settings = FireArmSettings::fromTemplate(*t);
				list.push_back({c.name, settings.magSize * (settings.nrOfMags + 1), settings.magSize, settings.nrOfMags,
					settings.magSize});
			}
			continue;
		}
		const FireArmSettings& settings = weapon->settings();
		list.push_back({c.name, weapon->roundsInMag() + weapon->magsLeft() * std::max(settings.magSize, 0), settings.magSize,
			settings.nrOfMags, weapon->roundsInMag()});
	}
	return list;
}

// The PlayerControlObject's input filter (0x005ca2e0) on the driver's
// throttle and steering (inputs 3 and 0; roll and pitch, 2 and 1, too):
// - EMP (an armour with takesEmpDamage and a level, rounded up, above 0,
//   slot 0x14c): x (1 - level / empDamageNumStep); 0, 1/3 and 2/3 at levels
//   3, 2 and 1 with the default 3 steps. With an autoControllerTemplateName
//   (template +0x298 slot 0x110) all inputs go above level 1 instead.
// - Critical damage: with scaleInputOnCriticalDamage, while the armour is
//   below its criticalDamage (slot 0x5c), x criticalDamageScaleModifier.
// (A child seat's own armour, walked to first, scales mouse look too.)
float GameWorld::driverInputScale(float empLevel, float hitPoints, const world::ObjectTemplate& t)
{
	float scale = 1;
	const int level = static_cast<int>(std::ceil(empLevel));
	if (integer(t, "armor.takesEmpDamage", 0) != 0 && level > 0) {
		const auto controller = t.settings().find("autoControllerTemplateName");
		if (controller != t.settings().end() && !controller->second.empty()) {
			if (level > 1) {
				return 0;
			}
		} else {
			const int steps = std::max(1, integer(t, "armor.empDamageNumStep", 3));
			scale *= std::max(0.0f, 1 - static_cast<float>(level) / static_cast<float>(steps));
		}
	}
	const float critical = number(t, "armor.criticalDamage", 0);
	const float left = hitPoints;
	if (integer(t, "scaleInputOnCriticalDamage", 0) != 0 && critical > 0 && left >= 0 && left < critical) {
		scale *= number(t, "criticalDamageScaleModifier", 1);
	}
	return scale;
}

int GameWorld::empLevel(uint32_t vehicle) const
{
	for (const Drone& drone : m_drones) {
		if (drone.object == vehicle) {
			return static_cast<int>(std::ceil(drone.empLevel));
		}
	}
	for (const Body& body : m_bodies) {
		if (body.object == vehicle) {
			return static_cast<int>(std::ceil(body.empLevel)); // 0x00652a00
		}
	}
	return 0;
}

// A shock that reaches downed soldiers (damageMandownSoldiers): one with
// a ResurrectCollisionComp (collision.resurrectDamage, the
// defibrillator's) revives a teammate when its resurrect damage times the
// material cell is above 0 (0x004c1960); otherwise, and from an enemy, it
// finishes the body off. (0x004c1960's man-down branch reads no team; the
// team test is the rewrite's, as the caller's 0x005a3bd0 side of it is not
// traced.)
void GameWorld::shockDowned(Player& victim, const ProjectileSettings& shock, float amount, uint32_t surfaceMaterial,
	int attacker)
{
	const float mod = m_materials ? m_materials->damageMod(static_cast<uint32_t>(shock.material), surfaceMaterial) : 1.0f;
	Player* medic = findPlayer(attacker);
	if (shock.resurrect && medic && medic->team == victim.team) {
		if (shock.resurrectDamage * mod > 0) {
			revive(victim, medic, shock.restoreHitPoints);
		}
		return;
	}
	if (amount * mod > 0) {
		finishDeath(victim);
	}
}

// A round leaves from the soldier's eyes along the aim.
void GameWorld::fire(const Player& player, float aimPitch)
{
	const physics::SoldierBody& body = player.body;
	const float look = lookYaw(player);
	const float yaw = look * 3.14159265f / 180.0f;
	const float pitch = aimPitch * 3.14159265f / 180.0f;
	const physics::Vec3 direction {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
	Projectile p;
	p.origin = body.position + physics::Vec3 {0, 0.6f, 0} + direction * 0.5f;
	p.position = p.origin;
	p.velocity = body.velocity + direction * player.weapon->settings().velocity;
	p.settings = player.projectile;
	p.timeLeft = player.projectile->timeToLive.sample(m_random);
	p.shooter = player.object;
	p.player = player.id;
	p.team = player.team;
	p.yaw = look;
	m_projectiles.push_back(p);
	// The third-person muzzle flash: the weapon's e_muzz child, placed by
	// the weapon's matrix times its own as FireComp places the muzzle
	// bundles (0x005f25f0 -> 0x006acfa0), so it follows the gun (its anchor
	// is the weapon object, moved in followSoldier while it burns); the
	// systems shown in third person. Without a posed skeleton, at the eyes
	// along the aim.
	if (!player.muzzleEffect.empty() && (localMuzzleEffects || &player != m_player)) {
		physics::Vec3 at;
		physics::Mat3 rotation;
		if (player.weaponObject && weaponPlace(player, at, rotation)) {
			m_effects.setAnchor(player.weaponObject, at, rotation);
			m_effects.spawnAttached(player.muzzleEffect, player.weaponObject, player.muzzleOffset, player.muzzleRotation,
				effects::EffectManager::View::ThirdPerson);
			const_cast<Player&>(player).muzzleUntil = m_time + 1.0;
		} else {
			m_effects.spawn(player.muzzleEffect, p.origin + direction * 0.3f,
				physics::Mat3::fromYawPitchRoll({look, -aimPitch, 0}));
		}
	}
}

bool GameWorld::weaponPlace(const Player& player, physics::Vec3& at, physics::Mat3& rotation) const
{
	const world::Object* soldier = m_objects.find(player.object);
	const int node = player.weaponNodes.empty() ? -1 : player.weaponNodes[0];
	if (!soldier || node < 0 || static_cast<size_t>(node) >= player.worldPose.size()) {
		return false;
	}
	// The skeleton's model space (D3D row vectors) turned by the soldier's yaw.
	const anim::Matrix& m = player.worldPose[static_cast<size_t>(node)];
	physics::Mat3 local;
	for (size_t r = 0; r < 3; ++r) {
		local.rows[r] = {m[r * 4], m[r * 4 + 1], m[r * 4 + 2]};
	}
	rotation = local * physics::Mat3::fromYawPitchRoll({soldier->rotation().x, 0, 0});
	const float yaw = soldier->rotation().x * 3.14159265f / 180.0f;
	const float c = std::cos(yaw), s = std::sin(yaw);
	at = {soldier->position().x + m[12] * c + m[14] * s, soldier->position().y + m[13],
		soldier->position().z - m[12] * s + m[14] * c};
	return true;
}

namespace {

// A point of the soldier's model space (its skeleton's world matrices;
// D3D row vectors) in the world: the object stands at its feet, turned by
// its yaw (model +z is the facing (sin yaw, 0, cos yaw)).
physics::Vec3 modelToWorld(const world::Object& soldier, const physics::Vec3& v)
{
	const float yaw = soldier.rotation().x * 3.14159265f / 180.0f;
	const float c = std::cos(yaw), s = std::sin(yaw);
	return {soldier.position().x + v.x * c + v.z * s, soldier.position().y + v.y,
		soldier.position().z - v.x * s + v.z * c};
}

physics::Vec3 transformPoint(const anim::Matrix& m, const physics::Vec3& v)
{
	return {v.x * m[0] + v.y * m[4] + v.z * m[8] + m[12], v.x * m[1] + v.y * m[5] + v.z * m[9] + m[13],
		v.x * m[2] + v.y * m[6] + v.z * m[10] + m[14]};
}

} // namespace

// An explosion: the projectile's detonation hands it to the server game
// logic (serverGameLogic vtable 0x009294e8), slot 0xe8 (0x004ba7a0 ->
// 0x005a9090) for a blast and slot 0xec (0x004ba830 -> 0x005a7ab0) for an
// EMP (detonation.isEMPWeapon, read at 0x005e9300). Both do nothing but
// the end effect for explosionMaterial -1 (the detonation's default).
//
// A blast (0x005a9090) finds the objects within explosionRadius:
// - Soldiers (0x005a7530): three of the skeleton's bones are looked for
//   from the blast, the head (47) worth 0.5, bone 11 0.35 and bone 3
//   0.15 (table 0x00a01750); a bone is seen when the ray to it is clear,
//   or one from a point min(0.02 radius, 0.5) m beside the blast (below,
//   left, right, above; itself in sight of the blast). The sum is the
//   soldier's share. A directional charge reaches only soldiers in its
//   cone (0x005a6370).
// - Other armoured objects take a share of 1, measured to their centre.
// Damage (0x005a8900) = explosionDamage x falloff x share, x the cell of
// the explosion's material against the target's material in damage();
// 0.1 or less is dropped. The distance is the shorter of the two to the
// object's position (the soldier's physics origin, a metre over its
// feet), its height difference times explosionYMod; the falloff
// (0x005a61a0) is 1 within 0.2 of the radius (0x00a6aab4) and falls
// linearly to 0 at the radius.
// The push (0x005a66e0): explosionForce x falloff x share x the armour's
// explosionForceMod (armor.explosionForceMod, default 1 at the armor
// template's 0x10c; the soldiers' 25), at most explosionForceMax (0x110,
// default 10; the soldiers' 1000); a soldier's is then times 0.8 + 0.005
// x (rand % 100), and its speed counts against the maximum. Its way is
// the soldier's axis (right, up or forward) most along the line from the
// blast plus that line, made a unit, with 5 (0x00a6aac0) added upwards.
// 0x005ee4d0 keeps a soldier's push for 0.5 s for its ragdoll (times
// rd_expForceMod).
// Other objects (0x005a9090) are hidden by a ray: blocked, half the
// damage, or none when the object's radius (slot 0x90, Body::objectRadius)
// is below a fifth of the blast's. The objects come first and the
// soldiers after (0x9493 is put aside), so a soldier's bone hidden only by
// an object that this blast's damage destroys counts half (0x005a7530
// reading 0x005a8900's list). A detonation with a negative
// explosionDamage pushes as a torque pair (BlastPush::twist).
bool GameWorld::detonate(const std::string& projectileTemplate, const physics::Vec3& at, int attacker)
{
	const ProjectileSettings* settings = projectileFor(projectileTemplate);
	if (!settings || !settings->detonates) {
		return false;
	}
	explode(at, *settings, attacker);
	return true;
}

void GameWorld::explode(const physics::Vec3& at, const ProjectileSettings& settings, int attacker, float facing,
	uint32_t exclude)
{
	// A directional charge (claymore-like mines) reaches only its cone.
	const auto inCone = [&](const physics::Vec3& target) {
		if (settings.coneAngle <= 0) {
			return true;
		}
		const float heading = std::atan2(target.x - at.x, target.z - at.z) * 57.29578f;
		return std::fabs(std::remainder(heading - facing, 360.0f)) <= settings.coneAngle;
	};
	if (!settings.endEffect.empty()) {
		m_effects.spawn(settings.endEffect, at, physics::Mat3 {});
	}
	if (settings.explosionMaterial < 0) {
		return;
	}
	const float radius = settings.explosionRadius;
	const float yMod = settings.explosionYMod;
	const auto distanceTo = [&](const physics::Vec3& p) {
		const physics::Vec3 d = p - at;
		return std::sqrt(d.x * d.x + d.y * yMod * d.y * yMod + d.z * d.z);
	};
	if (settings.emp) {
		empBlast(at, settings, distanceTo);
		return;
	}
	const auto falloff = [&](float distance) { return explosionFalloff(radius, distance); };
	struct Hit {
		uint32_t object;
		float amount;
	};
	std::vector<Hit> hits;
	const auto materialOf = [&](uint32_t object) {
		const world::Object* target = m_objects.find(object);
		const world::ObjectTemplate* t = target ? target->objectTemplate() : nullptr;
		return t ? static_cast<uint32_t>(integer(*t, "armor.defaultMaterial", 0)) : 0u;
	};
	// The projectile's detonation with a negative explosionDamage: the push
	// is a torque pair (0x005a66e0's bVar5, see BlastPush).
	const bool twist = settings.detonates && settings.explosionDamage < 0;
	for (Body& body : m_bodies) {
		if (body.object == exclude || findDrone(body.object)) {
			continue;
		}
		const physics::Vec3 position = body.body.position;
		const float distance = distanceTo(position);
		if (distance >= radius || !inCone(position)) {
			continue;
		}
		// The ray test (0x005a9090, DAT_00a85220 slot 0x68): from the blast to
		// the object; something else first (or the object without armour)
		// halves the blast, or stops it when the object's radius (slot 0x90)
		// is under a fifth of the blast's (*0x00a6ab24 = 5).
		float share = 1;
		uint32_t first = 0;
		if (firstHit(at, position, first) && (first != body.object || hitPoints(body.object) < 0)) {
			if (body.objectRadius < radius / 5) {
				continue;
			}
			share = 0.5f;
		}
		if (hitPoints(body.object) >= 0) {
			hits.push_back({body.object, falloff(distance) * settings.explosionDamage * share});
		}
		// The push on its node (0x005a66e0): not a major actor's (slot 0x1c4).
		const world::Object* object = m_objects.find(body.object);
		const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
		if (!t || body.majorActor) {
			continue;
		}
		BlastPush blast;
		blast.line = position - at;
		blast.line.y *= yMod;
		blast.axes = body.body.rotation;
		blast.falloff = falloff(distance);
		if (hitPoints(body.object) < 0) {
			blast.forceMod = 10000.0f / std::max(body.mass, 100.0f);
			blast.forceMax = 500;
		} else if (body.wreckTime >= 0) {
			blast.forceMod = number(*t, "armor.wreckExplosionForceMod", 1);  // +0x124
			blast.forceMax = number(*t, "armor.wreckExplosionForceMax", 100); // +0x128
		} else {
			blast.forceMod = number(*t, "armor.explosionForceMod", 1);  // +0x10c
			blast.forceMax = number(*t, "armor.explosionForceMax", 10); // +0x110
		}
		blast.explosionForce = settings.explosionForce;
		blast.share = share;
		blast.speed = std::sqrt(physics::lengthSquared(body.body.velocity));
		blast.twist = twist;
		const physics::Vec3 push = blastPush(blast);
		if (physics::lengthSquared(push) >= 0.01f * 0.01f) {
			// Slot 0xa4 at a point between the object and the blast, both over
			// max(distance, 1); it wakes the node (slot 0x160). A twist: the
			// torque pair, not spread.
			wake(body);
			if (twist) {
				for (const BlastForce& f : blastTwist(blast.axes, blast.line, blastPushForce(blast), body.boxWidth)) {
					body.body.addForce(f.at, f.force);
				}
			} else {
				const float spread = 1.0f / std::max(distance, 1.0f);
				body.body.addForce((at - position) * spread, push * spread);
			}
		}
	}
	for (const Drone& drone : m_drones) {
		if (findBody(drone.object)) {
			continue; // counted with the bodies
		}
		const float distance = distanceTo(drone.position);
		if (distance < radius && inCone(drone.position)) {
			hits.push_back({drone.object, falloff(distance) * settings.explosionDamage});
		}
	}
	// The commander's assets (static, without bodies): by their position.
	for (const CommanderAsset& asset : m_assets) {
		const world::Object* object = m_objects.find(asset.object);
		if (!object || findBody(asset.object)
			|| std::any_of(hits.begin(), hits.end(), [&](const Hit& hit) { return hit.object == asset.object; })) {
			continue;
		}
		const physics::Vec3 p {object->position().x, object->position().y, object->position().z};
		const float distance = distanceTo(p);
		if (distance < radius && hitPoints(asset.object) > 0 && inCone(p)) {
			hits.push_back({asset.object, falloff(distance) * settings.explosionDamage});
		}
	}
	// The objects come before the soldiers (0x005a9090 puts soldiers,
	// 0x9493, aside for a second loop): what the blast does to them is
	// known when the soldiers' bones are looked for.
	std::vector<std::pair<uint32_t, float>> pending;
	for (const Hit& hit : hits) {
		const float mod = m_materials ? m_materials->damageMod(static_cast<uint32_t>(settings.explosionMaterial),
															  materialOf(hit.object))
									  : 1.0f;
		if (hit.amount > 0.1f) {
			pending.push_back({hit.object, hit.amount * mod});
		}
	}
	for (const auto& owned : m_players) {
		Player& target = *owned;
		const world::Object* soldier = m_objects.find(target.object);
		if (!target.alive || target.vehicle || !soldier || !inCone(target.body.position)) {
			continue;
		}
		const physics::Vec3 origin {soldier->position().x, soldier->position().y + 1, soldier->position().z};
		const float distance = distanceTo(origin);
		if (distance >= radius) {
			continue;
		}
		const float share = blastShare(at, target, std::min(radius * 0.02f, 0.5f), pending);
		if (share <= 0) {
			continue;
		}
		const float amount = falloff(distance) * share * settings.explosionDamage;
		if (amount > 0.1f) {
			hits.push_back({target.object, amount});
		}
		// The push (0x005a66e0): kept for the ragdoll (0x005ee4d0) and given
		// to the soldier's node (slot 0xa4 -> 0x7c: its acceleration).
		const world::ObjectTemplate* t = soldier->objectTemplate();
		BlastPush blast;
		blast.line = origin - at;
		blast.line.y *= yMod;
		blast.axes = physics::Mat3::fromYawPitchRoll({target.body.yawDegrees, 0, 0});
		blast.falloff = falloff(distance);
		blast.forceMod = t ? number(*t, "armor.explosionForceMod", 1) : 1;
		blast.forceMax = t ? number(*t, "armor.explosionForceMax", 10) : 10;
		blast.explosionForce = settings.explosionForce;
		blast.share = share;
		blast.soldier = true;
		blast.soldierRandom = 0.8f + static_cast<float>(m_random.next() % 100) * 0.005f;
		blast.inWater = target.body.waterDepth > 0;
		blast.speed = std::sqrt(physics::lengthSquared(target.body.velocity));
		blast.twist = twist;
		const physics::Vec3 push = blastPush(blast);
		if (physics::lengthSquared(push) >= 0.01f * 0.01f) {
			target.ragDollForce = push;
			target.ragDollForceAt = m_time;
			if (twist) {
				// The soldier's node takes each of the pair as acceleration
				// (slot 0xa4 -> 0x7c); its box is under 2 m across.
				for (const BlastForce& f : blastTwist(blast.axes, blast.line, blastPushForce(blast), 0)) {
					target.body.acceleration += f.force;
				}
			} else {
				target.body.acceleration += push * (1.0f / std::max(distance, 1.0f));
			}
		}
	}
	for (const Hit& hit : hits) {
		if (hit.amount <= 0.1f) {
			continue;
		}
		damage(hit.object, hit.amount, static_cast<uint32_t>(settings.explosionMaterial), materialOf(hit.object), attacker);
	}
}

// How much of a soldier a blast reaches (0x005a7530): the head (bone 47)
// 0.5, bone 11 0.35 and bone 3 0.15 (0x00a01750), each when a ray from
// the blast reaches it, or one from a point `beside` metres off the blast
// across the line to it (below, then left, right, then above when the
// point below isn't in the blast's sight; each point used only when the
// blast sees it). Without a skeleton the bones sit at 1.7, 1.1 and 0.1 m
// over the feet.
float GameWorld::blastShare(const physics::Vec3& at, const Player& target, float beside,
	const std::vector<std::pair<uint32_t, float>>& pending) const
{
	const world::Object* soldier = m_objects.find(target.object);
	if (!soldier) {
		return 0;
	}
	constexpr int kBones[3] = {47, 11, 3};
	constexpr float kWeights[3] = {0.5f, 0.35f, 0.15f};
	constexpr float kHeights[3] = {1.7f, 1.1f, 0.1f};
	const physics::Vec3 feet {soldier->position().x, soldier->position().y, soldier->position().z};
	const physics::Vec3 origin = feet + physics::Vec3 {0, 1, 0};
	physics::Vec3 line = origin - at;
	const float length = std::sqrt(physics::lengthSquared(line));
	line = length > 0 ? line * (1 / length) : physics::Vec3 {0, 0, 1};
	const physics::Vec3 side = physics::Vec3 {-line.z, 0, line.x} * beside;
	const physics::Vec3 below = physics::Vec3 {line.x * line.y, -line.z * line.z - line.x * line.x, line.y * line.z} * beside;
	// Which of the points beside the blast it can see (asked once).
	int seen[4] = {-1, -1, -1, -1};
	const physics::Vec3 points[4] = {at + below, at - side, at + side, at - below};
	// The object the last blocked ray met (0x006f2d20's out, local_14: kept
	// across the rays and the bones, 0 for the terrain).
	uint32_t blocker = 0;
	const auto clear = [&](const physics::Vec3& from, const physics::Vec3& to) {
		uint32_t hit = 0;
		if (firstHit(from, to, hit, target.object)) {
			blocker = hit;
			return false;
		}
		return true;
	};
	const auto usable = [&](int i) {
		if (seen[i] < 0) {
			seen[i] = clear(at, points[i]) ? 1 : 0;
		}
		return seen[i] == 1;
	};
	float share = 0;
	for (int i = 0; i < 3; ++i) {
		physics::Vec3 bone = feet + physics::Vec3 {0, kHeights[i], 0};
		const size_t node = static_cast<size_t>(kBones[i]);
		if (node < target.worldPose.size()) {
			bone = modelToWorld(*soldier, transformPoint(target.worldPose[node], {0, 0, 0}));
		}
		bool visible = clear(at, bone);
		for (int j = 0; j < 4 && !visible; ++j) {
			if (j == 3 && usable(0)) {
				break; // the point above only when the one below is out of sight
			}
			visible = usable(j) && clear(points[j], bone);
		}
		if (visible) {
			share += kWeights[i];
		} else if (blocker != 0) {
			// Hidden by an armoured object (the first of it and its parents
			// with armour, +0x48) that this blast's damage so far destroys
			// (its hit points, armour slot 0x3c, at most a recorded amount
			// for the object hit): half the bone's weight.
			const float left = hitPoints(blocker);
			const bool destroyed = left >= 0 && std::any_of(pending.begin(), pending.end(), [&](const auto& p) {
				return p.first == blocker && left <= p.second;
			});
			if (destroyed) {
				share += kWeights[i] * 0.5f;
			}
		}
	}
	return share;
}

// An EMP (0x005a7ab0, per object 0x005a6500): every armoured object
// within explosionRadius, measured to its centre with the height times
// explosionYMod, takes explosionDamage x (1 - distance / radius) of EMP
// damage, or x armor.empDamageIgnoreRadiusModifier (default 0.1) with
// armor.empDamageIgnoreRadius; 0.1 or less is dropped. Nothing is hurt.
// The armour (0x00652980) turns it into steps: level = min(empDamageNumStep,
// ceil(level + amount / empDamageStepSize)), the level falling by one
// every empLevelTime seconds (0x00652910; the defaults at the armor
// template's 0x180-0x188: 5 s, 120, 3 steps); its level is that, rounded
// up (0x00652a00). Only objects with armor.takesEmpDamage feel it.
void GameWorld::empBlast(const physics::Vec3& at, const ProjectileSettings& settings,
	const std::function<float(const physics::Vec3&)>& distanceTo)
{
	const float radius = settings.explosionRadius;
	const auto amountFor = [&](const world::ObjectTemplate& t, const physics::Vec3& p) {
		const float distance = distanceTo(p);
		const bool ignoreRadius = integer(t, "armor.empDamageIgnoreRadius", 0) != 0;
		if (distance >= radius && !ignoreRadius) {
			return 0.0f;
		}
		const float factor = ignoreRadius ? number(t, "armor.empDamageIgnoreRadiusModifier", 0.1f) : 1 - distance / radius;
		const float amount = factor * settings.explosionDamage;
		return amount > 0.1f ? amount : 0.0f;
	};
	const auto add = [&](float& level, const world::ObjectTemplate& t, float amount) {
		level = addEmpDamage(level, amount, number(t, "armor.empDamageStepSize", 120), integer(t, "armor.empDamageNumStep", 3));
	};
	for (Body& body : m_bodies) {
		const world::Object* object = m_objects.find(body.object);
		const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
		if (!t || findDrone(body.object) || integer(*t, "armor.takesEmpDamage", 0) == 0) {
			continue; // drones below
		}
		if (const float amount = amountFor(*t, body.body.position); amount > 0) {
			add(body.empLevel, *t, amount);
		}
	}
	for (Drone& drone : m_drones) {
		const world::Object* object = m_objects.find(drone.object);
		const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
		if (!t || integer(*t, "armor.takesEmpDamage", 0) == 0) {
			continue;
		}
		if (const float amount = amountFor(*t, drone.position); amount > 0) {
			add(drone.empLevel, *t, amount);
		}
	}
}

// Seekers. A magnet (the motion mine; the projectile's update 0x005b3e60
// with its seek component at +0x1a0 and follow component at +0x1a4) looks
// for a target once armingDelay (3.5 s) and seek.trackingDelay have
// passed since it was thrown (0x005ec260 -> gameLogic slot 0x1c8,
// 0x004cf290): the registered TTMagnet targets (the vehicles'
// MagneticObject children, aimed at, enabled while the vehicle has had
// someone aboard in the last 3 s: disableWhenEmptyVehicle; wrecks stay),
// not its own side's (ignoreFriendly), within maxDistLock, within
// maxAngleLock of its facing, in line of sight; the nearest (cost (1 -
// directionBonus 0) x distance). A held target is kept out to
// distLoseLock. The chase starts trackingDelay after a lock (0x005b2c10).
// The follow (0x005ed1a0 -> 0x005ecb30) heads for the target's position
// plus its velocity x (distance / own speed) x predictionFactor; the
// target's top speed so far arms it past armingSpeed (7.2). The thrust
// (0x005b3e60): acceleration (60 m/s^2) towards that while slower than the
// cap, deAcceleration (50) against the velocity once at it (or, with
// magneticBehaviour, without a target); the cap is maxSpeed, or with
// useTargetSpeed the target's top speed (x speedModifier, 2, once armed):
// a parked vehicle is never chased. It goes off on hitting the vehicle
// (its collision, gameLogic slot 0xf8). An aimed missile turns towards
// where its shooter looks, at most its follow rates.
void GameWorld::steerSeeker(Projectile& p, float seconds)
{
	if (p.settings && p.settings->seek == ProjectileSettings::Seek::Beacon) {
		steerBeacon(p, seconds); // Titans.cpp
		return;
	}
	if (!p.settings || p.settings->seek == ProjectileSettings::Seek::None || p.age < p.settings->seekDelay) {
		return;
	}
	if (p.settings->seek == ProjectileSettings::Seek::Magnet) {
		const ProjectileSettings& s = *p.settings;
		if (p.age < s.armingDelay) {
			return;
		}
		// The target's MagneticObject: where it is, and whether it counts.
		const auto magnet = [this, &p, &s](const Body& body, physics::Vec3& at) {
			const world::Object* object = m_objects.find(body.object);
			if (!body.vehicle || !object || (s.ignoreFriendly && object->team() == p.team)
				|| m_time - body.occupiedAt > 3.0) {
				return false;
			}
			physics::Vec3 offset;
			bool found = false;
			for (const world::ChildTemplate& child : object->objectTemplate()->children()) {
				if (sameName(child.name, "MagneticObject")) {
					offset = {child.position.x, child.position.y, child.position.z};
					found = true;
				}
			}
			if (!found) {
				return false;
			}
			at = body.body.position + body.body.rotation.transform(offset);
			return true;
		};
		const auto sees = [&](const Body& body, const physics::Vec3& at, float reach) {
			const physics::Vec3 d = at - p.position;
			const float distance2 = physics::lengthSquared(d);
			if (distance2 > reach * reach) {
				return false;
			}
			const float speed = std::sqrt(physics::lengthSquared(p.velocity));
			if (s.maxAngleLock < 180 && speed > 0.01f && distance2 > 1e-4f) {
				const float angle = std::acos(std::clamp(physics::dot(d, p.velocity) / (speed * std::sqrt(distance2)), -1.0f, 1.0f));
				if (angle > s.maxAngleLock * 3.14159265f / 180.0f) {
					return false;
				}
			}
			return lineOfSight(p.position, at, body.object);
		};
		const Body* target = p.lockedOn ? findBody(p.lockedOn) : nullptr;
		physics::Vec3 aim;
		if (target && !(magnet(*target, aim) && sees(*target, aim, p.settings->loseDistance))) {
			target = nullptr;
		}
		if (!target) {
			p.lockedOn = 0;
			float best = 1e30f;
			for (const Body& body : m_bodies) {
				physics::Vec3 at;
				if (!magnet(body, at) || !sees(body, at, p.settings->lockDistance)) {
					continue;
				}
				const float distance = physics::lengthSquared(at - p.position);
				if (distance < best) {
					best = distance;
					target = &body;
					aim = at;
				}
			}
			if (target) {
				p.lockedOn = target->object;
				p.followFrom = p.age + p.settings->seekDelay; // 0x005b2c10
				p.targetTopSpeed = 0;
				p.armed = false;
			}
		}
		if (!target || p.age < p.followFrom) {
			return;
		}
		followMagnet(p, aim, target->body.velocity, seconds);
		return;
	}
	// Aimed: towards the point 200 m along the shooter's aim.
	const Player* shooter = findPlayer(p.player);
	if (!shooter || !shooter->alive) {
		return;
	}
	const float yaw = lookYaw(*shooter) * 3.14159265f / 180.0f;
	const float pitch = shooter->input.aimPitch * 3.14159265f / 180.0f;
	const physics::Vec3 aim {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
	const physics::Vec3 eye = shooter->body.position + physics::Vec3 {0, 0.6f, 0};
	// The follow's turn (followStep: follow.maxYaw / maxPitch degrees a
	// tick, the AV missile's 0.4 and 0.7).
	followStep(p, eye + aim * 200.0f, {}, seconds);
}

std::vector<GameWorld::Capsule> GameWorld::soldierCapsules(uint32_t soldier) const
{
	std::vector<Capsule> capsules;
	const Player* player = soldierPlayer(soldier);
	if (!player) {
		player = downedPlayer(soldier);
		if (player && !player->ragDoll) {
			player = nullptr;
		}
	}
	const world::Object* object = m_objects.find(soldier);
	if (!player || !object) {
		return capsules;
	}
	if (player->hitBones.empty() || player->worldPose.empty()) {
		// No skeleton: one upright capsule from the knees to the head.
		const physics::Vec3 feet {object->position().x, object->position().y, object->position().z};
		capsules.push_back({feet + physics::Vec3 {0, 0.5f, 0}, feet + physics::Vec3 {0, 1.5f, 0}, 0.35f, 0});
		return capsules;
	}
	for (const HitBone& bone : player->hitBones) {
		if (bone.node < 0 || static_cast<size_t>(bone.node) >= player->worldPose.size()) {
			continue;
		}
		// The capsule's frame (0x008b6fb0): translate(ox, oy, -+oz) x
		// Rx(+-90) x the bone, by the length's sign: its start in the bone's
		// frame is (ox, oz, oy), or (ox, oz, -oy) for a negative length, and
		// it runs `length` along the bone's -y.
		const anim::Matrix& m = player->worldPose[static_cast<size_t>(bone.node)];
		const physics::Vec3 start {bone.offset.x, bone.offset.z, bone.length >= 0 ? bone.offset.y : -bone.offset.y};
		const physics::Vec3 a = transformPoint(m, start);
		const physics::Vec3 b = transformPoint(m, start + physics::Vec3 {0, -bone.length, 0});
		capsules.push_back({modelToWorld(*object, a), modelToWorld(*object, b), bone.radius, bone.material});
	}
	return capsules;
}

void GameWorld::stepProjectiles(float seconds)
{
	for (Projectile& p : m_projectiles) {
		const physics::Vec3 from = p.position;
		if (!p.alive) {
			continue; // set off already (remote charges)
		}
		p.age += seconds;
		steerSeeker(p, seconds);
		if (p.resting) {
			p.timeLeft -= seconds;
			p.alive = p.timeLeft > 0;
		} else {
			moveProjectile(p, seconds);
		}
		// Out of time: a fused projectile goes off where it is.
		if (!p.alive) {
			if (p.settings && p.settings->explosionRadius > 0 && !p.settings->remote) {
				explode(p.position, *p.settings, p.player, p.yaw);
			}
			continue;
		}
		// A mine lying in wait: an enemy soldier going fast enough inside
		// its trigger cone sets it off.
		if (p.resting && p.settings->triggerRadius > 0) {
			for (const auto& owned : m_players) {
				const Player& victim = *owned;
				const physics::Vec3 feet = victim.body.position - physics::Vec3 {0, 1.0f, 0};
				const physics::Vec3 d = feet - p.position;
				const float speed = std::sqrt(victim.body.velocity.x * victim.body.velocity.x + victim.body.velocity.z * victim.body.velocity.z);
				if (!victim.alive || victim.vehicle || victim.team == p.team || speed < p.settings->triggerMinSpeed
					|| physics::lengthSquared(d) > p.settings->triggerRadius * p.settings->triggerRadius) {
					continue;
				}
				const float heading = std::atan2(d.x, d.z) * 57.29578f;
				if (std::fabs(std::remainder(heading - p.yaw, 360.0f)) <= p.settings->triggerAngle) {
					explode(p.position, *p.settings, p.player, p.yaw);
					p.alive = false;
					break;
				}
			}
			if (!p.alive) {
				continue;
			}
		}
		const physics::Vec3 to = p.position;
		// The nearest of: static objects, vehicles, soldiers, the terrain.
		float best = 2.0f;
		physics::CollisionWorld::Hit hit {};
		uint32_t hitObject = 0;
		physics::CollisionWorld::Hit candidate {};
		const Body* hitBody = nullptr;
		if (m_projectileCollision.raycast(from, to, candidate) && candidate.fraction < best) {
			best = candidate.fraction;
			hit = candidate;
			hitObject = candidate.owner;
		}
		for (const Body& body : m_bodies) {
			if (!body.projectileMesh || body.object == p.shooter) {
				continue;
			}
			const physics::Vec3 localFrom = body.body.rotation.untransform(from - body.body.position);
			const physics::Vec3 localTo = body.body.rotation.untransform(to - body.body.position);
			if (body.projectileMesh->raycast(localFrom, localTo, candidate) && candidate.fraction < best) {
				best = candidate.fraction;
				hitBody = &body;
				hit = candidate;
				hit.point = from + (to - from) * candidate.fraction;
				hit.normal = body.body.rotation.transform(candidate.normal);
				// The face's object: the body's, or a part of a Titan's tree.
				hitObject = candidate.owner ? candidate.owner : body.object;
			}
		}
		// Soldiers on foot: their bones' capsules, the soldier template's
		// setSkeletonCollisionBone records (bone, material, offset, radius,
		// length; 0x004deb29 -> 0x008b7db0 -> 0x008b7d40). The engine cuts
		// the ray to the soldier's bounding sphere first (0x006fa7f0) and
		// takes the roots along that piece; here along the tick's segment.
		// The capsules go in the records' order (0x008b75c0): the first the
		// segment enters (0x006bde70, a root strictly inside the segment)
		// is the hit, not the nearest. The normal is the hit's offset from
		// the capsule's axis.
		for (const auto& owned : m_players) {
			const Player& target = *owned;
			const bool downed = target.manDown && p.settings->damageMandown;
			if ((!target.alive && !downed) || target.vehicle || target.object == p.shooter) {
				continue;
			}
			std::vector<Capsule> capsules = soldierCapsules(target.object);
			if (downed && !target.ragDoll) {
				// Lying without a ragdoll: along the ground from the feet.
				if (const world::Object* body = m_objects.find(target.object)) {
					const float yaw = body->rotation().x * 3.14159265f / 180.0f;
					const physics::Vec3 feet {body->position().x, body->position().y + 0.25f, body->position().z};
					capsules = {{feet, feet + physics::Vec3 {std::sin(yaw), 0, std::cos(yaw)} * 1.7f, 0.35f, 24}};
				}
			}
			for (const Capsule& capsule : capsules) {
				float roots[2];
				if (physics::segmentCapsuleCrossings(from, to, capsule.a, capsule.b, capsule.radius, roots) == 0) {
					continue;
				}
				const float t = roots[0];
				if (t < best) {
					best = t;
					const physics::Vec3 at = from + (to - from) * t;
					const physics::Vec3 axis = capsule.b - capsule.a;
					const float along = physics::lengthSquared(axis) > 0
						? std::clamp(physics::dot(at - capsule.a, axis) / physics::lengthSquared(axis), 0.0f, 1.0f)
						: 0.0f;
					physics::Vec3 out = at - (capsule.a + axis * along);
					const float length = std::sqrt(physics::lengthSquared(out));
					out = length * length > 1e-5f ? out * (1.0f / length) : physics::Vec3 {0, 1, 0};
					hit = {t, at, out, capsule.material, target.object};
					hitObject = target.object;
					hitBody = nullptr;
				}
				break; // the first capsule hit
			}
		}
		if (m_ground.height) {
			// The terrain: the first of a few samples along the segment below it.
			constexpr int kSamples = 8;
			for (int i = 1; i <= kSamples; ++i) {
				const float t = static_cast<float>(i) / kSamples;
				if (t >= best) {
					break;
				}
				const physics::Vec3 at = from + (to - from) * t;
				physics::Vec3 normal {0, 1, 0};
				const float ground = m_ground.heightAndNormal(at.x, at.z, normal);
				if (at.y < ground) {
					best = t;
					// Its material from the level's material map (the
					// heightmap ray 0x8c, 0x006d3210; the grenade's point
					// query 0x80, 0x006c0f90).
					const uint32_t material = m_ground.material ? m_ground.material(at.x, at.z) : 0;
					hit = {t, {at.x, ground, at.z}, normal, material, 0};
					hitObject = 0;
					hitBody = nullptr;
					break;
				}
			}
		}
		// Penetration (DefaultPenetrateComp, 0x005eaca0) of a face whose
		// material has overrideNeverPenetrate and a damageLoss (the Titans'
		// penetrable shields, 4112 / 4118: damageLoss and minDamageLoss
		// 1000): the way out is looked for through the object's mesh over
		// 2.1 x its radius; the loss is the thickness x damageLoss within
		// minDamageLoss and maxDamageLoss (100000 by default, also the loss
		// without a way out). With the damage then left at or above a tenth
		// of the round's, it flies on from a centimetre past the way out
		// with that damage; otherwise it hits the face. Only rounds over
		// about 1111 get through the shields: the silo missiles, not bullets
		// or rockets.
		if (best <= 1.0f && hitBody && m_materials) {
			const physics::Material* surface = m_materials->find(hit.material);
			if (surface && surface->overrideNeverPenetrate && surface->damageLoss > 0) {
				const float base = p.settings->damageAt(std::sqrt(physics::lengthSquared(hit.point - p.origin)));
				const float left = base * p.damageScale;
				physics::Vec3 d = to - from;
				d = d * (1.0f / std::max(std::sqrt(physics::lengthSquared(d)), 1e-6f));
				const physics::Mat3& turn = hitBody->body.rotation;
				const physics::Vec3 at = hitBody->body.position;
				const float reach = 2.1f * hitBody->radius;
				float loss = surface->maxDamageLoss;
				physics::Vec3 exit = hit.point;
				// The way out: the first face of the material on along the way.
				physics::Vec3 start = turn.untransform(hit.point + d * 0.01f - at);
				const physics::Vec3 end = turn.untransform(hit.point + d * reach - at);
				for (int tries = 0; tries < 4; ++tries) {
					physics::CollisionWorld::Hit out {};
					if (!hitBody->projectileMesh->raycast(start, end, out)) {
						break;
					}
					const physics::Vec3 local = start + (end - start) * out.fraction;
					if (out.material == hit.material) {
						exit = at + turn.transform(local);
						const float thickness = std::sqrt(physics::lengthSquared(exit - hit.point));
						loss = std::clamp(thickness * surface->damageLoss, surface->minDamageLoss, surface->maxDamageLoss);
						break;
					}
					start = local + turn.untransform(d) * 0.01f; // another face first: on past it
				}
				if (left - loss >= 0.1f * base) {
					p.damageScale = base > 0 ? (left - loss) / base : 0.0f;
					p.position = exit + d * 0.01f;
					continue;
				}
			}
		}
		// onlyDetonateOnMaterial within its time (0x005b4ff0): an object of
		// another material that is no Titan's is destroyed on the server
		// (3e10 through the game logic's damage) and the projectile flies
		// on; a Titan's any face, or the terrain, sets it off as usual.
		if (best <= 1.0f && p.settings->onlyDetonateOnMaterial >= 0 && hitObject != 0
			&& (p.settings->onlyDetonateOnMaterialTime <= 0 || p.age < p.settings->onlyDetonateOnMaterialTime)
			&& hit.material != static_cast<uint32_t>(p.settings->onlyDetonateOnMaterial)) {
			const Titan* struck = nullptr;
			int part = 0;
			if (!titanPart(hitObject, struck, part)) {
				loseHitPoints(hitObject, 3e10f, -1);
				continue;
			}
		}
		if (best <= 1.0f && p.settings->bounces && p.lockedOn && hitObject == p.lockedOn) {
			// A magnet at its quarry: its collision sets it off.
			explode(hit.point + hit.normal * 0.05f, *p.settings, p.player, p.yaw);
			p.alive = false;
			continue;
		}
		if (best <= 1.0f && p.settings->bounces) {
			// A thrown object's contact (RotationalPoint: the response
			// 0x006c0cb0, its solve 0x006c0d00): pushed out of the surface,
			// the part of the velocity along the normal reversed times the
			// mean of the two materials' elasticity (MaterialManager slot
			// 0x80), the sliding part times 1 - their friction (slot 0x88,
			// at most 1); none on a material with water physics. A frag
			// grenade (Grenade_hand: friction 0.25, elasticity 0.5) on grass
			// (0.8, 0) keeps 0.475 of its slide and 0.25 of its bounce. The
			// engine has no rest speed (only the spin stops, under
			// |v|^2 0.1); the rewrite's projectiles lie still (for mines'
			// triggers and the fuse) under that on a face turned up. Mesh
			// projectiles (C4, mines, hubs) are rigid bodies in the engine
			// (ResponsePhysics); here they bounce the same way. The
			// terrain's material comes from the level's material map.
			const physics::Vec3 n = hit.normal;
			const uint32_t own = static_cast<uint32_t>(std::max(p.settings->material, 0));
			const physics::Material* surface = m_materials ? m_materials->find(hit.material) : nullptr;
			if (surface && surface->hasWaterPhysics) {
				continue;
			}
			const float elasticity = m_materials ? (m_materials->elasticity(hit.material) + m_materials->elasticity(own)) * 0.5f : 0.25f;
			const float friction = m_materials ? m_materials->friction(own, hit.material) : 0.5f;
			p.velocity = bounceVelocity(p.velocity, n, elasticity, friction);
			p.position = hit.point + n * 0.01f;
			p.acceleration = {};
			if (physics::lengthSquared(p.velocity) < 0.1f && n.y > 0.7f) {
				p.velocity = {};
				p.resting = true;
			}
			continue;
		}
		if (best <= 1.0f) {
			const physics::Vec3 travelled = hit.point - p.origin;
			const float amount = p.settings->damageAt(std::sqrt(physics::lengthSquared(travelled))) * p.damageScale;
			if (hitObject != 0) {
				Player* shooter = findPlayer(p.player);
				if (shooter && hitObject != shooter->object && hitPoints(hitObject) >= 0) {
					++shooter->roundsHit; // bulletsGivingDamage
				}
				if (Player* downed = downedPlayer(hitObject)) {
					shockDowned(*downed, *p.settings, amount, hit.material, p.player);
				} else {
					damage(hitObject, amount, static_cast<uint32_t>(p.settings->material), hit.material, p.player);
				}
			}
			m_impacts.push_back({hit.point, hit.normal, hitObject, hit.material, amount});
			if (p.settings->explosionRadius > 0) {
				explode(hit.point + hit.normal * 0.05f, *p.settings, p.player, p.yaw);
			}
			// The impact effect for the two materials, facing out of the surface.
			const physics::MaterialCell* cell =
				m_materials ? m_materials->cell(static_cast<uint32_t>(p.settings->material), hit.material) : nullptr;
			if (cell && !cell->effectTemplates.empty() && !cell->effectTemplates[0].empty()) {
				physics::Mat3 facing;
				const physics::Vec3 up = hit.normal;
				const physics::Vec3 other = std::fabs(up.x) < 0.9f ? physics::Vec3 {1, 0, 0} : physics::Vec3 {0, 0, 1};
				physics::Vec3 side = physics::cross(other, up);
				side = side * (1.0f / std::sqrt(physics::lengthSquared(side)));
				facing.rows = {side, up, physics::cross(side, up)};
				m_effects.spawn(cell->effectTemplates[0], hit.point, facing);
			}
			// Its decal (the cell's decal template) on the terrain or a static
			// object, or in a vehicle's space; none on soldiers.
			if (cell && !hitBody && !cell->decalTemplates.empty() && !cell->decalTemplates[0].empty()
				&& std::none_of(m_players.begin(), m_players.end(),
					[hitObject](const auto& player) { return hitObject != 0 && player->object == hitObject; })) {
				m_effects.spawnDecal(cell->decalTemplates[0], hit.point, hit.normal);
			} else if (cell && hitBody && !cell->decalTemplates.empty() && !cell->decalTemplates[0].empty()) {
				// On a vehicle: kept in its space, moving with it (the
				// object's own decal set, 0x0063c000 / RendDX9 0x100e6b70).
				if (const world::Object* holder = m_objects.find(hitBody->object)) {
					const world::Vec3& at = holder->position();
					const world::Vec3& turn = holder->rotation();
					m_effects.spawnDecal(cell->decalTemplates[0], hit.point, hit.normal, hitBody->object,
						{at.x, at.y, at.z}, {turn.x, turn.y, turn.z});
				}
			}
			if (m_impacts.size() > 64) {
				m_impacts.erase(m_impacts.begin());
			}
			p.alive = false;
		}
	}
	m_projectiles.erase(std::remove_if(m_projectiles.begin(), m_projectiles.end(),
							[](const Projectile& p) { return !p.alive; }),
		m_projectiles.end());
}

bool GameWorld::soldierPathClear(int team, const physics::Vec3& from, const physics::Vec3& to) const
{
	for (const Body& body : m_bodies) {
		const physics::CollisionWorld* mesh = (team == 1 || team == 2) && body.teamSoldierMesh[team - 1]
			? body.teamSoldierMesh[team - 1].get()
			: body.soldierMesh.get();
		physics::CollisionWorld::Hit hit {};
		if (mesh
			&& mesh->raycast(body.body.rotation.untransform(from - body.body.position),
				body.body.rotation.untransform(to - body.body.position), hit)) {
			return false;
		}
	}
	return true;
}

GameWorld::Player* GameWorld::findPlayer(int id) const
{
	return id >= 0 && static_cast<size_t>(id) < m_players.size() ? m_players[static_cast<size_t>(id)].get() : nullptr;
}

GameWorld::Player* GameWorld::soldierPlayer(uint32_t object) const
{
	if (object == 0) {
		return nullptr;
	}
	for (const auto& player : m_players) {
		if (player->alive && player->object == object) {
			return player.get();
		}
	}
	return nullptr;
}

int GameWorld::playerOf(uint32_t object) const
{
	if (object == 0) {
		return -1;
	}
	for (const auto& player : m_players) {
		if (player->alive && (player->object == object || player->vehicle == object)) {
			return player->id;
		}
	}
	return -1;
}

int GameWorld::addPlayer(const std::string& name, int team, bool ai)
{
	auto player = std::make_unique<Player>();
	player->id = static_cast<int>(m_players.size());
	player->name = name;
	player->team = team;
	player->ai = ai;
	player->allUnlocked = unlockAllByDefault;
	m_players.push_back(std::move(player));
	raise("PlayerConnect", {ScriptValue::player(m_players.back()->id)});
	return m_players.back()->id;
}

bool GameWorld::setPlayerTeam(int id, int team)
{
	Player* player = findPlayer(id);
	if (!player || (team != 1 && team != 2) || player->alive) {
		return false;
	}
	if (player->team == team) {
		return true;
	}
	if (player->squad != 0) {
		joinSquad(id, 0);
	}
	if (commander(player->team) == id) {
		resignCommander(id);
	}
	player->team = team;
	raise("PlayerChangeTeams", {ScriptValue::player(id), ScriptValue::integer(0)});
	return true;
}

bool GameWorld::spawnSoldier(int id)
{
	Player* player = findPlayer(id);
	if (!player || player->alive) {
		return false;
	}
	if (player->manDown) {
		finishDeath(*player);
	}
	return spawnSoldier(*player) != nullptr;
}

world::Object* GameWorld::spawnPlayer(int team)
{
	if (!m_player) {
		m_player = findPlayer(addPlayer("Player", team, false));
		m_player->unlocked = localPlayerUnlocks;
	}
	if (m_player->alive) {
		return m_objects.find(m_player->object);
	}
	m_player->team = team;
	if (m_player->manDown) {
		finishDeath(*m_player);
	}
	return spawnSoldier(*m_player);
}

std::vector<GameWorld::PlayerInfo> GameWorld::playerList() const
{
	std::vector<PlayerInfo> list;
	for (const auto& player : m_players) {
		PlayerInfo info;
		playerInfo(player->id, info);
		list.push_back(std::move(info));
	}
	return list;
}

bool GameWorld::playerInfo(int id, PlayerInfo& info) const
{
	const Player* player = findPlayer(id);
	if (!player) {
		return false;
	}
	info.id = player->id;
	info.name = player->name;
	info.team = player->team;
	info.ai = player->ai;
	info.alive = player->alive;
	info.soldier = player->alive ? player->object : 0;
	info.vehicle = player->alive ? player->vehicle : 0;
	info.seat = player->alive && player->vehicle ? player->seat : -1;
	info.driving = info.seat == 0 && !isTitan(player->vehicle);
	info.kit = player->alive ? player->kitObject : 0;
	info.weapon = player->alive ? player->weaponObject : 0;
	// The host's handles: the soldier (or the corpse) while its object is
	// kept; aboard, the vehicle (its seat's remote-controlled object reads
	// as the soldier: slot 0x148).
	const uint32_t kept = player->object && m_objects.find(player->object) ? player->object
		: player->lastSoldier && m_objects.find(player->lastSoldier) ? player->lastSoldier
																	 : 0;
	info.heldSoldier = player->alive ? player->object : 0;
	info.heldVehicle = player->alive && player->vehicle && m_objects.find(player->vehicle) ? player->vehicle : kept;
	info.respawnIn = player->respawnIn;
	info.manDown = player->manDown;
	info.squad = player->squad;
	info.cloaked = player->alive && player->cloaked;
	info.spawnedProne = player->alive && player->spawnedProne;
	info.battery = player->battery;
	info.squadLeader = player->squadLeader;
	info.commander = commander(player->team) == player->id;
	info.aimPitch = std::clamp(player->input.aimPitch + player->aimKickPitch, -89.0f, 89.0f);
	if (player->alive && player->vehicle) {
		// Aboard: the vehicle's place and heading.
		for (const Body& body : m_bodies) {
			if (body.object == player->vehicle) {
				info.position = body.body.position;
				info.yaw = body.body.rotation.yawPitchRoll().x;
				info.velocity = body.body.velocity;
			}
		}
	} else if (player->alive) {
		info.position = player->body.position - physics::Vec3 {0, 1.0f, 0};
		info.yaw = lookYaw(*player);
		info.velocity = player->body.velocity;
	} else if (const world::Object* body = player->manDown ? m_objects.find(player->object) : nullptr) {
		info.position = {body->position().x, body->position().y, body->position().z}; // where it lies
	}
	for (const Player::Carried& item : player->carried) {
		info.carried.emplace_back(item.slot, item.name);
	}
	if (player->inHand >= 0 && static_cast<size_t>(player->inHand) < player->carried.size()) {
		info.inHandSlot = player->carried[static_cast<size_t>(player->inHand)].slot;
	}
	if (player->alive && player->vehicle) {
		const FireArm* seatGun = gun(player->vehicle, player->seat);
		info.heat = seatGun ? seatGun->heat() : 0.0f;
	} else if (player->alive && player->weapon) {
		info.heat = player->weapon->heat();
	}
	info.kills = player->kills;
	info.deaths = player->deaths;
	info.teamKills = player->teamKills;
	return true;
}

// A gadget that sets down an object of its own: its projectile has mesh
// physics and armour, and neither explodes nor holds a beacon.
world::ObjectTemplate* GameWorld::deployableTemplate(const world::ObjectTemplate* weapon) const
{
	if (!weapon) {
		return nullptr;
	}
	const auto projectile = weapon->settings().find("projectileTemplate");
	world::ObjectTemplate* t = projectile != weapon->settings().end() ? m_templates.find(projectile->second) : nullptr;
	if (!t) {
		return nullptr;
	}
	const auto type = t->settings().find("physicsType");
	const bool mesh = type != t->settings().end() && type->second == "Mesh";
	const bool armoured = number(*t, "armor.maxHitPoints", 0) > 0;
	const bool explodes = number(*t, "detonation.explosionDamage", 0) > 0 || number(*t, "detonation.isEMPWeapon", 0) != 0
		|| number(*t, "detonation.triggerRadius", 0) > 0;
	return mesh && armoured && !explodes && integer(*t, "armor.holdsSlBeacon", 0) == 0 ? t : nullptr;
}

world::ObjectTemplate* GameWorld::beaconTemplate(const world::ObjectTemplate* weapon) const
{
	if (!weapon) {
		return nullptr;
	}
	const auto projectile = weapon->settings().find("projectileTemplate");
	world::ObjectTemplate* t = projectile != weapon->settings().end() ? m_templates.find(projectile->second) : nullptr;
	return t && integer(*t, "armor.holdsSlBeacon", 0) != 0 ? t : nullptr;
}

const ProjectileSettings* GameWorld::projectileFor(const std::string& projectileTemplate)
{
	auto& projectile = m_projectileSettings[projectileTemplate];
	if (!projectile) {
		if (const world::ObjectTemplate* p = m_templates.find(projectileTemplate)) {
			projectile = std::make_unique<ProjectileSettings>(ProjectileSettings::fromTemplate(*p));
		}
	}
	return projectile.get();
}

// Switching weapons: the one in hand goes back to the kit (keeping its
// rounds), the slot's comes out, and the held object, the muzzle effect
// and the weapon's animation system change with it.
// The held weapon's muzzle flash (its e_muzz child) and recoil.
void GameWorld::weaponTraits(Player& player, const world::ObjectTemplate& weapon)
{
	const world::ObjectTemplate* t = &weapon;
	// Its recoil (SoldierBasedRecoilComp, template 0x005dd730's defaults).
	player.recoil = {};
	player.recoilGraph.clear();
	player.lastShotTick = -1000;
	if (std::find(t->components().begin(), t->components().end(), "SoldierBasedRecoilComp") != t->components().end()) {
		player.recoil.hasRecoilForce = number(*t, "recoil.hasRecoilForce", 0) != 0;
		player.recoil.goBackOnRecoil = number(*t, "recoil.goBackOnRecoil", 1) != 0;
		player.recoil.zoomModifier = number(*t, "recoil.zoomModifier", 1);
		player.recoil.graphFrameCount = static_cast<int>(number(*t, "recoil.recoilGraphFrameCount", 8));
		player.recoil.graphExponent = number(*t, "recoil.recoilGraphExponent", 2);
		player.recoil.graphTotalMovement = number(*t, "recoil.recoilGraphTotalMovement", 2);
		player.recoilGraph = player.recoil.graph();
		const auto up = t->settings().find("recoil.recoilForceUp");
		const auto side = t->settings().find("recoil.recoilForceLeftRight");
		player.recoilUp = up != t->settings().end() ? RandomDistribution::parse(up->second) : RandomDistribution::parse("1");
		player.recoilLeftRight =
			side != t->settings().end() ? RandomDistribution::parse(side->second) : RandomDistribution::parse("0");
	}
	player.muzzleEffect.clear();
	for (const world::ChildTemplate& child : t->children()) {
		if (child.name.compare(0, 6, "e_muzz") == 0) {
			player.muzzleEffect = child.name;
			player.muzzleOffset = {child.position.x, child.position.y, child.position.z};
			player.muzzleRotation = physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
			break;
		}
	}
}

void GameWorld::equip(Player& player, int slot)
{
	int index = -1;
	for (size_t i = 0; i < player.carried.size(); ++i) {
		if (player.carried[i].slot == slot) {
			index = static_cast<int>(i);
			break;
		}
	}
	if (index < 0 || index == player.inHand) {
		return;
	}
	if (player.inHand >= 0) {
		player.carried[static_cast<size_t>(player.inHand)].weapon = std::move(player.weapon);
	}
	player.inHand = index;
	Player::Carried& next = player.carried[static_cast<size_t>(index)];
	world::ObjectTemplate* t = m_templates.find(next.name);
	if (!t) {
		return;
	}
	const FireArmSettings settings = FireArmSettings::fromTemplate(*t);
	player.weapon = next.weapon ? std::move(next.weapon) : std::make_unique<FireArm>(settings);
	player.projectile = projectileFor(settings.projectileTemplate);
	player.weaponTemplate = next.name;
	weaponTraits(player, *t);
	const world::Object* soldier = m_objects.find(player.object);
	if (player.weaponObject) {
		m_effects.releaseAnchor(player.weaponObject);
		m_objects.remove(player.weaponObject);
		player.weaponObject = 0;
	}
	if (world::Object* held = soldier ? m_objects.create(t, soldier->position(), soldier->rotation()) : nullptr) {
		player.weaponObject = held->id();
	}
	player.weaponAnimation.reset();
	if (m_loadAnimationSystem) {
		const auto it = t->settings().find("animationSystem3P");
		if (const anim::AnimationSystemTemplate* system = it != t->settings().end() ? m_loadAnimationSystem(it->second) : nullptr) {
			player.weaponAnimation = std::make_unique<anim::AnimationState>(*m_animationSystems, *system);
		}
	}
}

std::vector<int> GameWorld::squadMembers(int team, int squad) const
{
	std::vector<int> members;
	for (const auto& player : m_players) {
		if (squad != 0 && player->team == team && player->squad == squad) {
			members.push_back(player->id);
		}
	}
	return members;
}

int GameWorld::openSquad(int team) const
{
	for (int squad = 1; squad <= 9; ++squad) {
		const size_t size = squadMembers(team, squad).size();
		if (size > 0 && size < kSquadSize) {
			return squad;
		}
	}
	for (int squad = 1; squad <= 9; ++squad) {
		if (squadMembers(team, squad).empty()) {
			return squad;
		}
	}
	return 0;
}

bool GameWorld::joinSquad(int id, int squad)
{
	Player* player = findPlayer(id);
	if (!player || squad < 0 || squad > 9 || squad == player->squad) {
		return false;
	}
	if (squad != 0 && squadMembers(player->team, squad).size() >= kSquadSize) {
		return false;
	}
	if (squad != 0 && commander(player->team) == id) {
		return false; // the commander leads no squad and is in none
	}
	const int old = player->squad;
	const bool wasLeader = player->squadLeader;
	player->squad = squad;
	player->squadLeader = squad != 0 && squadMembers(player->team, squad).size() == 1;
	// A leader leaving hands the squad to the next member; its beacon goes.
	if (old != 0 && wasLeader) {
		const std::vector<int> rest = squadMembers(player->team, old);
		if (!rest.empty()) {
			findPlayer(rest.front())->squadLeader = true;
		}
		if (const auto beacon = m_beacons.find({player->team, old}); beacon != m_beacons.end()) {
			m_objects.remove(beacon->second);
			m_beacons.erase(beacon);
		}
	}
	raise("PlayerChangedSquad", {ScriptValue::player(id), ScriptValue::integer(old), ScriptValue::integer(squad)});
	return true;
}

uint32_t GameWorld::squadBeacon(int team, int squad) const
{
	const auto it = m_beacons.find({team, squad});
	return it != m_beacons.end() && m_objects.find(it->second) ? it->second : 0;
}

// Spawning with the squad: at the beacon, or a metre behind a living
// leader on foot. In the engine the leader's soldier carries a
// SquadLeaderSpawnPoint child (0x004c7f10 creates it, Common.con: a
// SpawnPoint with setScatterSpawnPositions 1 and no offset), and members
// spawn through it, scattered round the leader (0x005b2200,
// scatterOnLeader below). When none of its tries fits, 0x005b2200
// returns 0; what its caller does then is not traced, and the rewrite puts
// the member a metre behind.
bool GameWorld::spawnAtSquad(Player& player, world::Object*& at, physics::Vec3& position, float& yaw)
{
	if (player.squad == 0 || (player.spawnChoice != kSpawnOnLeader && player.spawnChoice != kSpawnOnBeacon)) {
		return false;
	}
	if (player.spawnChoice == kSpawnOnBeacon) {
		if (world::Object* beacon = m_objects.find(squadBeacon(player.team, player.squad))) {
			position = {beacon->position().x, beacon->position().y, beacon->position().z};
			yaw = beacon->rotation().x;
			at = beacon;
			return true;
		}
		return false;
	}
	for (const int member : squadMembers(player.team, player.squad)) {
		const Player* leader = findPlayer(member);
		if (leader && leader->squadLeader && leader->alive && !leader->vehicle && leader != &player) {
			at = m_objects.find(leader->object);
			if (!at) {
				return false;
			}
			bool prone = false;
			if (scatterOnLeader(*leader, position, yaw, &prone)) {
				player.spawnedProne = prone;
			} else {
				// No try fits: the rewrite's fallback, a metre behind.
				const float y = leader->body.yawDegrees * 3.14159265f / 180.0f;
				position = leader->body.position - physics::Vec3 {std::sin(y), 1.0f, std::cos(y)};
				yaw = leader->body.yawDegrees;
			}
			return true;
		}
	}
	return false;
}

// The leader's SquadLeaderSpawnPoint (made by 0x004c7f10, a child of its
// soldier, setScatterSpawnPositions 1: template +0x90) scatters its
// spawns (0x005b2200): from the soldier's origin less 0.875, a heading of
// the leader's +-70 degrees (140 x U - 70) and a distance of
// spawnpoint-scatter-mindist + (maxdist - mindist) x U (3 to 9 m, once a
// call), five tries turning on by 0, 72, 144, 216 and 288 degrees more
// each (cumulative: the last two repeat the first ones), each behind the
// leader. A try (0x005b1e70) at the full and then half the distance: the
// terrain's height and a ray 4 m down from the point at the leader's
// height through the objects; standing on the higher of them plus
// spawnpoint-terrain-offset (1.01) when that is no more than the ray's
// reach below, and with the way from the leader to that spot clear
// (0x006f2d20). Then the stance (the leader's soldier interface 0xc4d7
// slot 0x14, 0x00585d00 -> its response's slot 0x98, 0x006c3340): the
// soldier's spheres fitting there standing, else prone (the spawn point's
// +0x18a: the member starts prone), else the try fails. The member faces
// the leader (0x005b1d30). Not rewritten: the extra test some callers ask
// for (IAIPathfindingCourier slot 0xc, DAT_00b4bb94; the callers passing
// it are not traced).
bool GameWorld::scatterOnLeader(const Player& leader, physics::Vec3& feet, float& yaw, bool* prone)
{
	constexpr float kMinDistance = 3, kMaxDistance = 9; // spawnpoint-scatter-mindist/-maxdist
	constexpr float kTerrainOffset = 1.01f;             // spawnpoint-terrain-offset
	constexpr float kProbe = 4;
	const physics::Vec3 base = leader.body.position - physics::Vec3 {0, 0.875f, 0};
	float angle = leader.body.yawDegrees + 140 * m_random.unit() - 70;
	const float distance = kMinDistance + (kMaxDistance - kMinDistance) * m_random.unit();
	for (int i = 0; i < 5; ++i) {
		angle += static_cast<float>(i) * 72;
		const float a = angle * 3.14159265f / 180.0f;
		const physics::Vec3 way = physics::Vec3 {-std::sin(a), 0, -std::cos(a)} * distance;
		for (int k = 1; k <= 2; ++k) {
			const physics::Vec3 p = base + way * (1.0f / static_cast<float>(k));
			const float ground = m_ground.height ? m_ground.height(p.x, p.z) : 0.0f;
			physics::CollisionWorld::Hit hit;
			float y = 0;
			if (m_collision.triangles() > 0 && m_collision.raycast(p, p - physics::Vec3 {0, kProbe, 0}, hit)) {
				y = std::max(ground, hit.point.y) + kTerrainOffset;
			} else if (ground >= p.y - kProbe) {
				y = ground + kTerrainOffset;
			} else {
				continue;
			}
			const physics::Vec3 spot {p.x, y - 0.875f, p.z};
			if (!lineOfSight(base, spot, leader.object)) {
				continue;
			}
			bool lying = false;
			if (!soldierFits({p.x, y, p.z}, SoldierPose::Stand, leader.object)) {
				if (!soldierFits({p.x, y, p.z}, SoldierPose::Prone, leader.object)) {
					continue;
				}
				lying = true;
			}
			if (prone) {
				*prone = lying;
			}
			feet = {p.x, y - 1.0f, p.z}; // the origin a metre over the feet
			const physics::Vec3 toLeader = leader.body.position - physics::Vec3 {p.x, y, p.z};
			yaw = physics::lengthSquared(toLeader) < 1 ? leader.body.yawDegrees : std::atan2(toLeader.x, toLeader.z) * 57.29578f;
			return true;
		}
	}
	return false;
}

// Whether a soldier fits at `origin` in a stance (physics::soldierFits,
// 0x006c3340): against the soldier collision LOD of the objects nearby,
// the static objects and the vehicles (`ignore`, the soldier's own, aside;
// and the response types 0xc42e/0xc42f, which the rewrite doesn't have).
bool GameWorld::soldierFits(const physics::Vec3& origin, SoldierPose pose, uint32_t ignore) const
{
	const int stance = static_cast<int>(pose);
	if (!physics::soldierFits(m_collision, origin, stance)) {
		return false;
	}
	for (const Body& body : m_bodies) {
		if (body.soldierMesh && body.object != ignore
			&& !physics::soldierFits(*body.soldierMesh, origin, stance, body.body.rotation, body.body.position)) {
			return false;
		}
	}
	return true;
}

void GameWorld::setBattery(int id, float battery)
{
	if (Player* player = findPlayer(id)) {
		player->battery = battery;
	}
}

void GameWorld::setUnlocks(int id, std::vector<std::string> containers, bool all)
{
	if (Player* player = findPlayer(id)) {
		player->unlocked = std::move(containers);
		player->allUnlocked = all;
	}
}

void GameWorld::setKit(int id, const std::string& kit, const std::string& soldier)
{
	if (Player* player = findPlayer(id)) {
		player->kitTemplate = kit;
		player->soldierTemplate = soldier;
	}
}

void GameWorld::setSpawnChoice(int id, int controlPoint)
{
	if (Player* player = findPlayer(id)) {
		player->spawnChoice = controlPoint;
	}
}

int GameWorld::spawnChoice(int id) const
{
	const Player* player = findPlayer(id);
	return player ? player->spawnChoice : -1;
}

std::vector<int> GameWorld::spawnablePoints(int team) const
{
	std::vector<int> ids;
	for (const auto& owned : m_objects.objects()) {
		const world::ObjectTemplate* t = owned->objectTemplate();
		if (!t || t->typeName() != "SpawnPoint") {
			continue;
		}
		const int controlPoint = owned->controlPointId() != -1 ? owned->controlPointId() : integer(*t, "controlPointId", -1);
		for (const ControlPoint& point : m_controlPoints) {
			if (point.id == controlPoint && point.team == team
				&& std::find(ids.begin(), ids.end(), point.id) == ids.end()) {
				ids.push_back(point.id);
			}
		}
	}
	std::sort(ids.begin(), ids.end());
	return ids;
}

bool GameWorld::lineOfSight(const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore) const
{
	physics::CollisionWorld::Hit hit {};
	if (m_projectileCollision.raycast(from, to, hit)) {
		return false;
	}
	for (const Body& body : m_bodies) {
		if (!body.projectileMesh || body.object == ignore) {
			continue;
		}
		const physics::Vec3 localFrom = body.body.rotation.untransform(from - body.body.position);
		const physics::Vec3 localTo = body.body.rotation.untransform(to - body.body.position);
		if (body.projectileMesh->raycast(localFrom, localTo, hit)) {
			return false;
		}
	}
	if (m_ground.height) {
		// The terrain, sampled every two metres or so.
		const float length = std::sqrt(physics::lengthSquared(to - from));
		const int samples = std::max(2, static_cast<int>(length / 2));
		for (int i = 1; i < samples; ++i) {
			const physics::Vec3 at = from + (to - from) * (static_cast<float>(i) / samples);
			if (at.y < m_ground.height(at.x, at.z)) {
				return false;
			}
		}
	}
	return true;
}

// The spawn spot's test (0x005aeb80 with 0x006f2e90 per object). The
// template's collision mesh (the collision manager's by the template's
// slot 0x90 name, 0x005ad4d0) placed at the spot by the spawner's matrix
// (its position plus spawnOffset, unturned) against the PlayerControlObjects
// whose bounds come within 10 m of the spot (the scene's slot 0x44 with
// interface 0xc4c5): one overlaps when it is not being deleted, its
// response is a mesh one (0xc42d, 0xc430, 0xc431), it has a soldier LOD
// (slot 0xc4(2)) and a segment from the spot to any vertex of the
// template's vehicle LOD (type 1) placed there crosses that soldier LOD
// (0x006f2760: the instance's slot 22, 0x006e5920, both sides of the
// faces), the spot dropped first (dropSpawnSpot); a template with
// MechLegs probes down too (0x006f28e0). An existing walker's legs'
// objects (0x006f2e90's loop through the responses, slot 0x5c) aren't
// bodies here: its hull alone is tested. The objects a global
// list holds are passed (0x005628f0 / 0x00561d90: the Titan sync manager
// ECPhysicsETitanSync's two Titans, 0x00561c00, in gpm_ti alone:
// isTitan, Titans.cpp).
// The spot's drop (0x005ad8b0, before the test): along the spawner's up
// axis (its matrix's second row) from the template's vehicle-LOD box top
// (slot 0x54(1): max y) to its bottom (min y), the scene's ray (slot
// 0x68, 0x00700b80) through objects with flags (+0x14) 0x1000100 and
// without 8 (0x006f4aa0 / 0x006f4ac0; 8 marks the mobile ones, set with
// their node: 0x005f6ba0, 0x00621b50): static objects alone (the
// rewrite asks their projectile LOD). A hit puts the box's bottom on it. Without one, a box bottom
// under the terrain (DAT_00a83e58 slot 0x6c) is lifted onto it. Only the
// test moves: the spawn itself is at the spawner's spot (0x005aeb80's
// end, the game logic's slot 0x120).
physics::Vec3 GameWorld::dropSpawnSpot(const physics::Vec3& spot, const physics::Vec3& up, float bottom, float top) const
{
	const physics::Vec3 high = spot + up * top;
	const physics::Vec3 low = spot + up * bottom;
	physics::CollisionWorld::Hit hit {};
	if (m_projectileCollision.raycast(high, low, hit)) {
		return high + (low - high) * hit.fraction - up * bottom;
	}
	if (m_ground.height) {
		const float ground = m_ground.height(spot.x, spot.z);
		if (spot.y - ground < -(up.y * bottom)) {
			return {spot.x, ground - up.y * bottom, spot.z};
		}
	}
	return spot;
}

ObjectSpawner::SpotTest GameWorld::testSpawnSpot(const world::ObjectTemplate& objectTemplate, const world::Vec3& spot,
	const world::Vec3& rotation, const world::Vec3& spawner) const
{
	ObjectSpawner::SpotTest test;
	const auto meshName = objectTemplate.settings().find("collisionMesh");
	const physics::CollisionMeshFile* mesh = (!m_files || !m_collisionTemplates || meshName == objectTemplate.settings().end())
		? nullptr
		: m_collisionTemplates->mesh(meshName->second, *m_files);
	if (!mesh || mesh->geometries.empty()) {
		return test; // 0x005ad4d0 finds none: no test
	}
	std::vector<physics::Vec3> vertices;
	const physics::CollisionLod* vehicleLod = nullptr;
	for (const auto& part : physics::collisionParts(*mesh, 0)) {
		if (const physics::CollisionLod* lod = physics::collisionLodFor(*mesh, part, 1)) {
			vehicleLod = lod;
			for (const auto& v : lod->vertices) {
				vertices.push_back({v[0], v[1], v[2]});
			}
		}
	}
	const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({rotation.x, rotation.y, rotation.z});
	physics::Vec3 from {spot.x, spot.y, spot.z};
	if (vehicleLod) {
		from = dropSpawnSpot(from, turn.rows[1], vehicleLod->boundsMin[1], vehicleLod->boundsMax[1]);
	}
	const auto legs = m_legsBottom.find(&objectTemplate);
	const float legsBottom = legs != m_legsBottom.end() ? legs->second : 0.0f;
	for (const Body& body : m_bodies) {
		// A Titan never blocks a spawn (0x005aeb80 asks the Titan sync).
		if (!body.controlObject || !body.soldierMesh || body.soldierMesh->triangles() == 0 || isTitan(body.object)) {
			continue;
		}
		// (A wreck counts: only an object being deleted, +0x14 bit 0, doesn't.)
		if (!m_objects.find(body.object)) {
			continue;
		}
		if (std::sqrt(physics::lengthSquared(body.body.position - from)) > 10.0f + body.objectRadius) {
			continue;
		}
		const physics::Vec3 localFrom = body.body.rotation.untransform(from - body.body.position);
		bool overlaps = false;
		for (const physics::Vec3& v : vertices) {
			const physics::Vec3 to = from + turn.transform(v);
			physics::CollisionWorld::Hit hit {};
			if (body.soldierMesh->raycast(localFrom, body.body.rotation.untransform(to - body.body.position), hit)) {
				overlaps = true;
				break;
			}
		}
		// A walker's legs (0x006f28e0, with the template's +0x4c4 not 0):
		// three segments from the spot and 1.7 m either side along the
		// spawner's x axis, down (world y) by the box bottom.
		for (int side = -1; side <= 1 && !overlaps && std::fabs(legsBottom) >= 1.1920929e-07f; ++side) {
			const physics::Vec3 start = from + turn.rows[0] * (1.7f * static_cast<float>(side));
			const physics::Vec3 end = start + physics::Vec3 {0, legsBottom, 0};
			physics::CollisionWorld::Hit hit {};
			overlaps = body.soldierMesh->raycast(body.body.rotation.untransform(start - body.body.position),
				body.body.rotation.untransform(end - body.body.position), hit);
		}
		if (!overlaps) {
			continue;
		}
		test.blockers.push_back(body.object);
		if (std::any_of(body.occupants.begin(), body.occupants.end(), [](int id) { return id >= 0; })) {
			test.inUse = true;
		}
	}
	if (!test.blockers.empty()) {
		const physics::Vec3 at {spawner.x, spawner.y, spawner.z};
		for (const auto& owned : m_players) {
			const world::Object* soldier = m_objects.find(owned->object);
			if (soldier && !owned->vehicle) {
				const physics::Vec3 feet {soldier->position().x, soldier->position().y, soldier->position().z};
				if (std::sqrt(physics::lengthSquared(feet - at)) < 7.0f) {
					test.soldierNear = true;
				}
			}
		}
	}
	return test;
}

bool GameWorld::firstHit(const physics::Vec3& from, const physics::Vec3& to, uint32_t& object) const
{
	return firstHit(from, to, object, 0);
}

// The landing's ray (0x005ef250): the scene's (slot 0x68, objects with
// flags 0x100: their projectile LOD here), else the heightmap cluster's
// (slot 0x8c); the material of what it meets.
bool GameWorld::groundMaterialAlong(const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore,
	uint32_t& material) const
{
	float nearest = 2;
	physics::CollisionWorld::Hit hit {};
	if (m_projectileCollision.raycast(from, to, hit)) {
		nearest = hit.fraction;
		material = hit.material;
	}
	for (const Body& body : m_bodies) {
		if (!body.projectileMesh || body.object == ignore) {
			continue;
		}
		const physics::Vec3 localFrom = body.body.rotation.untransform(from - body.body.position);
		const physics::Vec3 localTo = body.body.rotation.untransform(to - body.body.position);
		if (body.projectileMesh->raycast(localFrom, localTo, hit) && hit.fraction < nearest) {
			nearest = hit.fraction;
			material = hit.material;
		}
	}
	if (nearest <= 1) {
		return true;
	}
	if (m_ground.height) {
		const float length = std::sqrt(physics::lengthSquared(to - from));
		const int samples = std::max(2, static_cast<int>(length / 2));
		for (int i = 1; i <= samples; ++i) {
			const physics::Vec3 at = from + (to - from) * (static_cast<float>(i) / samples);
			if (at.y < m_ground.height(at.x, at.z)) {
				material = m_ground.material ? m_ground.material(at.x, at.z) : 0;
				return true;
			}
		}
	}
	return false;
}

bool GameWorld::firstHit(const physics::Vec3& from, const physics::Vec3& to, uint32_t& object, uint32_t ignore) const
{
	float nearest = 2;
	physics::CollisionWorld::Hit hit {};
	if (m_projectileCollision.raycast(from, to, hit)) {
		nearest = hit.fraction;
		object = hit.owner;
	}
	for (const Body& body : m_bodies) {
		if (!body.projectileMesh || (ignore != 0 && body.object == ignore)) {
			continue;
		}
		const physics::Vec3 localFrom = body.body.rotation.untransform(from - body.body.position);
		const physics::Vec3 localTo = body.body.rotation.untransform(to - body.body.position);
		if (body.projectileMesh->raycast(localFrom, localTo, hit) && hit.fraction < nearest) {
			nearest = hit.fraction;
			object = body.object;
		}
	}
	if (m_ground.height) {
		const float length = std::sqrt(physics::lengthSquared(to - from));
		const int samples = std::max(2, static_cast<int>(length / 2));
		for (int i = 1; i < samples; ++i) {
			const float fraction = static_cast<float>(i) / samples;
			if (fraction >= nearest) {
				break;
			}
			const physics::Vec3 at = from + (to - from) * fraction;
			if (at.y < m_ground.height(at.x, at.z)) {
				nearest = fraction;
				object = 0;
				break;
			}
		}
	}
	return nearest <= 1;
}

std::vector<GameWorld::ControlPointInfo> GameWorld::controlPointList() const
{
	std::vector<ControlPointInfo> list;
	for (const ControlPoint& point : m_controlPoints) {
		const world::ObjectTemplate* t = point.object->objectTemplate();
		const world::Vec3& p = point.object->position();
		list.push_back({point.id, point.team, {p.x, p.y, p.z}, t ? number(*t, "radius", 10) : 10,
			!(t && number(*t, "unableToChangeTeam", 0) != 0)});
	}
	return list;
}

bool GameWorld::setSoldierPosition(uint32_t soldier, const physics::Vec3& feet)
{
	Player* player = soldierPlayer(soldier);
	world::Object* object = m_objects.find(soldier);
	if (!player || !object || player->vehicle) {
		return false;
	}
	player->body.position = feet + physics::Vec3 {0, 1.0f, 0};
	player->body.velocity = {};
	object->setPosition({feet.x, feet.y, feet.z});
	followSoldier(*player, *object);
	return true;
}

float GameWorld::takeAimKick(int id)
{
	Player* player = findPlayer(id);
	return player ? std::exchange(player->aimKickPitch, 0.0f) : 0.0f;
}

float GameWorld::aimPitchOf(int id) const
{
	const Player* player = findPlayer(id);
	return player ? std::clamp(player->input.aimPitch + player->aimKickPitch, -89.0f, 89.0f) : 0.0f;
}

void GameWorld::setInput(int id, const SoldierInput& input)
{
	if (Player* player = findPlayer(id)) {
		if (player == m_player) {
			m_playerInput = input;
		} else {
			player->input = input;
		}
	}
}

// Spawn points of the team's control points; any spawn point otherwise.
// A spawn point's control point is its object's (Object.setControlPointId)
// or else its template's (ObjectTemplate.setControlPointId).
world::Object* GameWorld::spawnSoldier(Player& player)
{
	const int team = player.team;
	player.spawnedProne = false;
	std::vector<world::Object*> points, fallback;
	for (const auto& owned : m_objects.objects()) {
		world::Object& object = *owned;
		const world::ObjectTemplate* t = object.objectTemplate();
		if (!t || t->typeName() != "SpawnPoint") {
			continue;
		}
		fallback.push_back(&object);
		const int controlPoint = object.controlPointId() != -1 ? object.controlPointId() : integer(*t, "controlPointId", -1);
		for (const ControlPoint& point : m_controlPoints) {
			if (point.id == controlPoint && point.team == team) {
				points.push_back(&object);
			}
		}
	}
	// The spawn menu's choice narrows them to one control point's.
	std::vector<world::Object*> chosen;
	if (player.spawnChoice >= 0) {
		for (world::Object* point : points) {
			const world::ObjectTemplate* t = point->objectTemplate();
			const int controlPoint = point->controlPointId() != -1 ? point->controlPointId() : integer(*t, "controlPointId", -1);
			if (controlPoint == player.spawnChoice) {
				chosen.push_back(point);
			}
		}
	}
	const std::vector<world::Object*>& choices = !chosen.empty() ? chosen : points.empty() ? fallback : points;
	// The kit (the spawn menu's choice, else the team's assault kit), its
	// soldier, and its primary weapon: the GenericFireArm child with
	// itemIndex 3 (knife 1, sidearm 2).
	world::ObjectTemplate* soldier = m_templates.find(
		!player.soldierTemplate.empty() ? player.soldierTemplate : team == 1 ? "pac_light_soldier" : "us_light_soldier");
	world::ObjectTemplate* kitTemplate =
		m_templates.find(!player.kitTemplate.empty() ? player.kitTemplate : team == 1 ? "Pac_Assault" : "EU_Assault");
	world::ObjectTemplate* primary = nullptr;
	if (kitTemplate) {
		for (const world::ChildTemplate& child : kitTemplate->children()) {
			world::ObjectTemplate* item = m_templates.find(child.name);
			if (item && item->typeName() == "GenericFireArm" && integer(*item, "itemIndex", 0) == 3) {
				primary = item;
				break;
			}
		}
	}
	if (!primary) {
		primary = m_templates.find(team == 1 ? "as_ar_rifle" : "eu_ar_rifle");
	}
	world::Object* squadAt = nullptr;
	physics::Vec3 squadPosition;
	float squadYaw = 0;
	const bool withSquad = spawnAtSquad(player, squadAt, squadPosition, squadYaw);
	if ((!withSquad && choices.empty()) || !soldier) {
		return nullptr;
	}
	const world::Object* at = withSquad ? squadAt : choices[m_random.next() % choices.size()];
	const world::Vec3 where = withSquad ? world::Vec3 {squadPosition.x, squadPosition.y, squadPosition.z} : at->position();
	const float facing = withSquad ? squadYaw : at->rotation().x;
	world::Object* created = m_objects.create(soldier, where, {facing, 0, 0});
	if (!created) {
		return nullptr;
	}
	created->setTeam(team);
	// A fresh soldier: what the previous one had goes with it.
	const int id = player.id;
	const std::string name = player.name;
	const bool ai = player.ai;
	const int kills = player.kills, deaths = player.deaths, teamKills = player.teamKills;
	const int fired = player.roundsFired, hits = player.roundsHit;
	const int spawnChoice = player.spawnChoice;
	const std::string kitName = player.kitTemplate, soldierName = player.soldierTemplate;
	const std::vector<std::string> unlocked = player.unlocked;
	const int squad = player.squad;
	const bool squadLeader = player.squadLeader;
	const bool allUnlocked = player.allUnlocked;
	player = Player();
	player.id = id;
	player.name = name;
	player.ai = ai;
	player.kills = kills;
	player.deaths = deaths;
	player.teamKills = teamKills;
	player.roundsFired = fired;
	player.roundsHit = hits;
	player.spawnChoice = spawnChoice;
	player.kitTemplate = kitName;
	player.soldierTemplate = soldierName;
	player.unlocked = unlocked;
	player.squad = squad;
	player.squadLeader = squadLeader;
	player.allUnlocked = allUnlocked;
	player.alive = true;
	player.object = created->id();
	player.lastSoldier = player.object;
	player.team = team;
	// The physics origin is a metre above the feet (the soldier collision
	// mesh's lowest point is at -1); the mesh's origin is at the feet.
	player.body.position = {where.x, where.y + 1.0f, where.z};
	player.body.yawDegrees = facing;
	player.control.reset();
	player.body.world = &m_collision;
	// The kit's rifle (EU_Assault / Pac_Assault) and what else it carries.
	fillKit(player, kitTemplate, primary);
	// The ground's material is not known yet: material 0 ("Default") on both sides.
	if (m_materials) {
		player.body.friction = m_materials->friction(0, 0);
	}
	if (m_loadAnimationSystem) {
		const auto system = [this](const world::ObjectTemplate* t) -> const anim::AnimationSystemTemplate* {
			if (!t) {
				return nullptr;
			}
			const auto it = t->settings().find("animationSystem3P");
			return it != t->settings().end() ? m_loadAnimationSystem(it->second) : nullptr;
		};
		const auto skeletonPath = soldier->settings().find("skeleton3P");
		if (skeletonPath != soldier->settings().end()) {
			player.skeleton = m_animationSystems->skeleton(skeletonPath->second);
		}
		if (player.skeleton) {
			for (int part = 1; part <= 16; ++part) {
				player.weaponNodes.push_back(player.skeleton->find("mesh" + std::to_string(part)));
			}
		}
		if (const anim::AnimationSystemTemplate* body = system(soldier)) {
			player.bodyAnimation = std::make_unique<anim::AnimationState>(*m_animationSystems, *body);
		}
		if (const anim::AnimationSystemTemplate* weapon = system(primary)) {
			player.weaponAnimation = std::make_unique<anim::AnimationState>(*m_animationSystems, *weapon);
		}
	}
	// Hit capsules: "bone material offset radius length" per bone.
	if (player.skeleton) {
		for (int index = 0;; ++index) {
			const auto it = soldier->settings().find("skeletonCollisionBone." + std::to_string(index));
			if (it == soldier->settings().end()) {
				break;
			}
			char bone[64] = {};
			unsigned material = 0;
			HitBone hitBone {};
			if (std::sscanf(it->second.c_str(), "%63s %u %f/%f/%f %f %f", bone, &material, &hitBone.offset.x,
					&hitBone.offset.y, &hitBone.offset.z, &hitBone.radius, &hitBone.length)
				== 7) {
				hitBone.node = player.skeleton->find(bone);
				hitBone.material = material;
				player.hitBones.push_back(hitBone);
			}
		}
	}
	player.lastYaw = player.body.yawDegrees;
	// The kit (an object of its own, as in the original; what the scripts'
	// player.getKit() returns), picked up as the soldier appears.
	if (world::ObjectTemplate* kit = kitTemplate) {
		if (world::Object* worn = m_objects.create(kit, created->position(), created->rotation())) {
			worn->setHidden(true);
			player.kitObject = worn->id();
		}
	}
	// A fresh kit's battery is empty (kit +0x1bc, 0 from 0x00562a80),
	// taken by the soldier as it picks the kit up (0x0058b680).
	if (player.kitObject) {
		player.battery = 0;
	}
	if (player.kitObject) {
		raise("PickupKit", {ScriptValue::player(player.id), ScriptValue::object(player.kitObject)});
	}
	raise("PlayerSpawn", {ScriptValue::player(player.id), ScriptValue::object(created->id())});
	// The rifle in the soldier's hands, drawn by its skeleton's mesh nodes.
	if (world::ObjectTemplate* rifle = primary) {
		if (world::Object* held = m_objects.create(rifle, created->position(), created->rotation())) {
			player.weaponObject = held->id();
		}
	}
	return created;
}

// What a kit carries, by slot, with its primary in hand (a new kit: the
// spawn's, or a revive's that finds none on the ground).
void GameWorld::fillKit(Player& player, world::ObjectTemplate* kitTemplate, world::ObjectTemplate* primary)
{
	player.carried.clear();
	player.inHand = -1;
	// What the kit carries, by slot.
	if (kitTemplate) {
		for (const world::ChildTemplate& child : kitTemplate->children()) {
			const world::ObjectTemplate* item = m_templates.find(child.name);
			const int slot = item && item->typeName() == "GenericFireArm" ? integer(*item, "itemIndex", 0) : 0;
			if (slot > 0) {
				player.carried.push_back({item->name(), slot, nullptr});
				if (item == primary) {
					player.inHand = static_cast<int>(player.carried.size()) - 1;
				}
			}
		}
		// Unlocked items, in free slots.
		for (const world::ChildTemplate& child : kitTemplate->children()) {
			const world::ObjectTemplate* container = m_templates.find(child.name);
			if (!container || container->typeName() != "ItemContainer"
				|| (!player.allUnlocked
					&& std::find(player.unlocked.begin(), player.unlocked.end(), container->name()) == player.unlocked.end())) {
				continue;
			}
			// An unlock's Ability component: its increasedRepairAbility
			// goes to the player when above 0 (0x005706b0), the last one
			// taken winning.
			if (const float modifier = number(*container, "ability.increasedRepairAbility", 0); modifier > 0) {
				player.abilityModifier = modifier;
			}
			for (const world::ChildTemplate& inner : container->children()) {
				const world::ObjectTemplate* item = m_templates.find(inner.name);
				const int slot = item && item->typeName() == "GenericFireArm" ? integer(*item, "itemIndex", 0) : 0;
				const bool taken = std::any_of(player.carried.begin(), player.carried.end(),
					[slot](const Player::Carried& c) { return c.slot == slot; });
				if (slot > 0 && !taken) {
					player.carried.push_back({item->name(), slot, nullptr});
				}
			}
		}
		std::stable_sort(player.carried.begin(), player.carried.end(),
			[](const Player::Carried& a, const Player::Carried& b) { return a.slot < b.slot; });
		for (size_t i = 0; i < player.carried.size(); ++i) {
			if (primary && player.carried[i].name == primary->name()) {
				player.inHand = static_cast<int>(i);
			}
		}
	}
	if (world::ObjectTemplate* rifle = primary) {
		player.weaponTemplate = rifle->name();
		const FireArmSettings settings = FireArmSettings::fromTemplate(*rifle);
		player.projectile = projectileFor(settings.projectileTemplate);
		player.weapon = std::make_unique<FireArm>(settings);
		weaponTraits(player, *rifle);
	}
}

// A soldier's death: the scripts hear PlayerKilled (victim, attacker,
// weapon, assists, the victim's soldier) and PlayerDeath (victim, soldier);
// the kit and the weapon go, the body lies as a ragdoll while man down,
// and the player waits the spawn time.
void GameWorld::kill(Player& victim, Player* attacker, uint32_t weapon)
{
	if (!victim.alive) {
		return;
	}
	const uint32_t soldier = victim.object;
	if (attacker == &victim) {
		attacker = nullptr;
	}
	++victim.deaths;
	victim.diedAt = m_time;
	if (attacker) {
		if (attacker->team == victim.team) {
			++attacker->teamKills;
		} else {
			++attacker->kills;
		}
		if (!weapon) {
			weapon = attacker->weaponObject;
		}
	}
	raise("PlayerKilled", {ScriptValue::player(victim.id),
							  attacker ? ScriptValue::player(attacker->id) : ScriptValue::none(),
							  weapon ? ScriptValue::object(weapon) : ScriptValue::none(), ScriptValue::makeList({}),
							  ScriptValue::object(soldier)});
	// Killed in a seat of a vehicle that still stands (the seat's "occupant
	// died" exit, interface 0xc4c5 slot 10 -> 0x005cea00 with died = 1):
	// the seat's animation system plays its "die" trigger (0x008b9790; the
	// bundle fades in over 0.4 s and holds its last frame), the soldier
	// keeps it as its dead seat (+0x244, 0x005c7790) and is posed by it
	// from then on (0x005ef250): the body stays slumped there, no ragdoll.
	// A seat without one throws the body out at the seat's exit (as a
	// living soldier leaves) as a ragdoll pushed along its forward by
	// rd_dieInVehicleForce (100, 0x005f0f50 with +0x2eb); the body then
	// outlives the player's death (PlayerDeath at once: no man down in a
	// vehicle) as a corpse (stepCorpses). In a vehicle that is destroyed,
	// the occupants die and are deleted (0x005cacf0): gone at once.
	bool slumped = false, thrown = false, deleted = false;
	if (victim.vehicle) {
		deleted = true;
		if (Body* body = findBody(victim.vehicle);
			body && victim.seat >= 0 && static_cast<size_t>(victim.seat) < body->occupants.size()) {
			const int seat = victim.seat;
			body->occupants[static_cast<size_t>(seat)] = -1;
			if (seat == 0 && body->vehicle) {
				body->vehicle->setInput({});
			}
			if (body->wreckTime >= 0) {
				// deleted with the vehicle
			} else if (victim.seatAnimation && victim.skeleton && victim.seatAnimation->play("die")) {
				SeatCorpse corpse;
				corpse.object = soldier;
				corpse.vehicle = victim.vehicle;
				corpse.seat = victim.seat;
				corpse.animation = std::move(victim.seatAnimation);
				corpse.skeleton = victim.skeleton;
				corpse.pose = victim.pose;
				corpse.worldPose = victim.worldPose;
				m_seatCorpses.push_back(std::move(corpse));
				victim.seatAnimationPath.clear();
				slumped = true;
				deleted = false;
			} else if (world::Object* corpse = m_objects.find(soldier)) {
				// Out at the seat's exit, facing as the vehicle (enterOrLeaveVehicle's
				// placement), in the pose it had in the seat.
				physics::Vec3 exit {-2.5f, 0, 0};
				const world::Object* vehicleObject = m_objects.find(body->object);
				if (const world::ObjectTemplate* t = vehicleObject ? vehicleObject->objectTemplate() : nullptr) {
					const VehicleLayout& layout = vehicleLayout(*t);
					if (static_cast<size_t>(seat) < layout.seats.size() && layout.seats[static_cast<size_t>(seat)].hasExit) {
						exit = layout.seats[static_cast<size_t>(seat)].exit;
					} else if (const auto it = t->settings().find("altSoldierExitPosition"); it != t->settings().end()) {
						exit = parseVec3(it->second);
					}
				}
				physics::Vec3 at = body->body.position + body->body.rotation.transform(exit);
				at.y = std::max(at.y, m_ground.height ? m_ground.height(at.x, at.z) : at.y) + 1.0f;
				victim.body.position = at;
				victim.body.velocity = {};
				victim.body.yawDegrees = body->body.rotation.yawPitchRoll().x;
				corpse->setPosition({at.x, at.y - 1.0f, at.z});
				corpse->setRotation({victim.body.yawDegrees, 0, 0});
				corpse->setHidden(false);
				victim.seatAnimation.reset();
				victim.seatAnimationPath.clear();
				thrown = startRagDoll(victim, true);
				deleted = false;
			}
		}
	}
	const bool onFoot = !victim.vehicle;
	// The kit falls where the soldier lies (0x00589890, Kits.cpp); killed
	// in a seat it goes with the body.
	if (onFoot || thrown) {
		dropKit(victim);
	} else if (victim.kitObject) {
		raise("DropKit", {ScriptValue::player(victim.id), ScriptValue::object(victim.kitObject)});
	}
	victim.alive = false;
	victim.vehicle = 0;
	victim.seat = -1;
	victim.respawnIn = m_spawnTime;
	for (const uint32_t object : {victim.weaponObject, victim.kitObject}) {
		if (object) {
			m_hitPoints.erase(object);
			m_effects.releaseAnchor(object);
			m_objects.remove(object);
		}
	}
	victim.weaponObject = victim.kitObject = 0;
	// Man down (sv.manDownTime): the body lies where it fell and a medic
	// may revive it; killed in a vehicle, the soldier is gone at once.
	if (onFoot && m_manDownTime > 0) {
		victim.manDown = true;
		victim.manDownLeft = m_manDownTime;
		if (world::Object* body = m_objects.find(soldier)) {
			body->setHidden(false);
			if (!startRagDoll(victim)) {
				// No skeleton or ragdoll script: lying on its back.
				body->setRotation({body->rotation().x, -90, 0});
				body->setPosition({body->position().x, body->position().y + 0.25f, body->position().z});
			}
		}
	} else {
		if (onFoot && !victim.ragDoll) {
			startRagDoll(victim); // without man down the body falls at once (0x005f0f50)
		}
		(void)thrown;
		finishDeath(victim, slumped ? BodyFate::Seat : deleted ? BodyFate::Remove : BodyFate::Corpse);
	}
}

bool GameWorld::bodyLanded(int player) const
{
	const Player* p = findPlayer(player);
	if (!p || !p->ragDoll) {
		return true;
	}
	const world::ObjectTemplate* t = p->soldierTemplate.empty() ? nullptr : m_templates.find(p->soldierTemplate);
	const float delay = t ? number(*t, "ragdollLandedDelay", 4) : 4.0f;
	return p->ragDoll->touched() && m_time > p->diedAt + delay;
}

uint32_t GameWorld::seatCorpse(uint32_t vehicle, int seat) const
{
	for (const SeatCorpse& corpse : m_seatCorpses) {
		if (corpse.vehicle == vehicle && corpse.seat == seat) {
			return corpse.object;
		}
	}
	return 0;
}

// The slumped bodies ride in their seats, posed by the seat's system
// (its root's still bundle under the die bundle). One goes when the
// vehicle is wrecked or gone or when someone takes the seat (how long the
// engine keeps it is not traced).
void GameWorld::stepSeatCorpses(float seconds)
{
	for (auto it = m_seatCorpses.begin(); it != m_seatCorpses.end();) {
		Body* body = findBody(it->vehicle);
		world::Object* soldier = m_objects.find(it->object);
		const world::Object* vehicleObject = body ? m_objects.find(body->object) : nullptr;
		const bool taken = body && static_cast<size_t>(it->seat) < body->occupants.size()
			&& body->occupants[static_cast<size_t>(it->seat)] >= 0;
		if (!body || !soldier || !vehicleObject || body->wreckTime >= 0 || taken) {
			if (soldier) {
				m_objects.remove(it->object);
			}
			it = m_seatCorpses.erase(it);
			continue;
		}
		const VehicleLayout& layout = vehicleLayout(*vehicleObject->objectTemplate());
		if (static_cast<size_t>(it->seat) < layout.seats.size()) {
			const SeatLayout& seat = layout.seats[static_cast<size_t>(it->seat)];
			const physics::Mat3 rotation = physics::Mat3::fromYawPitchRoll(seat.rotation) * body->body.rotation;
			const physics::Vec3 at
				= body->body.position + body->body.rotation.transform(seat.position) + rotation.transform({0, -1, 0});
			soldier->setPosition({at.x, at.y, at.z});
			soldier->setRotation({rotation.yawPitchRoll().x, rotation.yawPitchRoll().y, rotation.yawPitchRoll().z});
		}
		it->animation->update({}, seconds);
		std::vector<bool> touched;
		it->animation->apply(*it->skeleton, it->pose, touched);
		it->worldPose = anim::worldMatrices(*it->skeleton, it->pose);
		++it;
	}
}

// The end of man down (its time is up, the body is finished off, or the
// player spawns again): the scripts hear PlayerDeath (a ticket lost in
// conquest). The body stays as a corpse with its ragdoll until its
// armour's countdown removes it (stepCorpses), unless it is slumped in a
// seat or deleted with a destroyed vehicle.
void GameWorld::finishDeath(Player& victim, BodyFate fate)
{
	const uint32_t soldier = victim.object;
	victim.manDown = false;
	victim.manDownLeft = 0;
	raise("PlayerDeath", {ScriptValue::player(victim.id), ScriptValue::object(soldier)});
	if (soldier) {
		if (fate == BodyFate::Corpse && m_objects.find(soldier)) {
			// A wreck: its armour stays at 0 or below.
			m_hitPoints[soldier] = std::min(0.0f, m_hitPoints.count(soldier) ? m_hitPoints[soldier] : 0.0f);
			Corpse corpse;
			corpse.object = soldier;
			corpse.ragDoll = std::move(victim.ragDoll);
			corpse.skeleton = victim.skeleton;
			corpse.pose = victim.pose;
			corpse.worldPose = victim.worldPose;
			corpse.diedAt = victim.diedAt;
			// The soldier object's own template (its armour's).
			const world::ObjectTemplate* t = m_objects.find(soldier)->objectTemplate();
			if (!t && !victim.soldierTemplate.empty()) {
				t = m_templates.find(victim.soldierTemplate);
			}
			if (t) {
				corpse.landedDelay = number(*t, "ragdollLandedDelay", 4);
				corpse.stayAsWreck = number(*t, "armor.timeToStayAsWreck", 20);       // 0x00630040: 20
				corpse.afterDestroyed = number(*t, "armor.timeToStayAfterDestroyed", 0); // and 0
			}
			m_corpses.push_back(std::move(corpse));
		} else {
			m_hitPoints.erase(soldier);
			if (fate == BodyFate::Remove) {
				m_objects.remove(soldier);
			}
		}
	}
	victim.ragDoll.reset();
	victim.object = 0;
}

// The soldier's armour once it is a wreck (0x00654d80 -> 0x00652380 while
// its hit points are 0 or less, the template's +0x166 set and
// timeToStayAsWreck not -1): the countdown begins (+0x64) once
// timeToStayAsWreck seconds have passed since the death and the body has
// landed (0x005eda20: no ragdoll, or one that touched something
// ragdollLandedDelay after the death; a body still in a vehicle needn't
// land), or after 60 s (*0x00a79388) whatever; timeToStayAfterDestroyed
// later the server removes the object (0x00651db0). Meanwhile the body
// fades out: its geometry's (+0x44) game alpha (+0x28, slot 0x104 with it
// times the view-distance fade +0x2c) is the time left over
// timeToStayAfterDestroyed, clamped to 0..1, for soldiers only (class
// 0x9493; vehicle wrecks don't fade); the object's fade() here, which the
// renderer draws blended. Soldiers' armour sets 6 and 1 (the .tweak
// files): six seconds after landing, gone in a second. The body neither
// sinks nor shrinks, and the ragdoll keeps stepping meanwhile.
void GameWorld::stepCorpses(float seconds)
{
	for (auto it = m_corpses.begin(); it != m_corpses.end();) {
		Corpse& corpse = *it;
		world::Object* body = m_objects.find(corpse.object);
		if (!body) {
			m_hitPoints.erase(corpse.object);
			it = m_corpses.erase(it);
			continue;
		}
		if (corpse.ragDoll && corpse.skeleton) {
			const anim::RagDollTemplate* ragDoll = corpse.ragDoll->ragDollTemplate();
			const anim::Vec mid = corpse.ragDoll->update(
				seconds, ragDollWorld(corpse.ragDoll->middle(), ragDoll ? ragDoll->settings.completeCollRadius : 1.5f));
			corpse.ragDoll->pose(*corpse.skeleton, corpse.pose, corpse.worldPose);
			playRagDollImpact(*corpse.ragDoll, mid);
			body->setPosition({mid[0], mid[1], mid[2]});
			body->setRotation({0, 0, 0});
		}
		const bool landed = !corpse.ragDoll || (corpse.ragDoll->touched() && m_time > corpse.diedAt + corpse.landedDelay);
		const bool begin = (m_time > corpse.diedAt + corpse.stayAsWreck && landed) || m_time > corpse.diedAt + 60;
		if (begin && corpse.fadeFrom < 0) {
			corpse.fadeFrom = m_time;
		}
		if (corpse.fadeFrom >= 0 && corpse.afterDestroyed > 0) {
			// The fade: linear from 1 to 0 over timeToStayAfterDestroyed
			// (0x00402dd0 clamps it), the geometry's game alpha (+0x28).
			const double left = corpse.fadeFrom + corpse.afterDestroyed - m_time;
			body->setFade(std::clamp(static_cast<float>(left / corpse.afterDestroyed), 0.0f, 1.0f));
		}
		if (corpse.fadeFrom >= 0 && corpse.afterDestroyed + corpse.fadeFrom - m_time <= 0) {
			m_hitPoints.erase(corpse.object);
			m_objects.remove(corpse.object);
			it = m_corpses.erase(it);
			continue;
		}
		++it;
	}
}

// A remote soldier's body (see GameWorld.h): started from the skeleton's
// setup pose turned by the object's yaw at its feet (the first update puts
// the networked particles where the server has them, and the constraints
// pull the rest along), network-driven from then on.
bool GameWorld::driveRemoteRagDoll(uint32_t object, const std::vector<physics::Vec3>& particles)
{
	world::Object* body = m_objects.find(object);
	if (!body || particles.empty()) {
		return false;
	}
	std::vector<anim::Vec> targets;
	for (const physics::Vec3& p : particles) {
		targets.push_back({p.x, p.y, p.z});
	}
	for (RemoteBody& remote : m_remoteBodies) {
		if (remote.object == object) {
			remote.ragDoll->receive(targets);
			return true;
		}
	}
	const anim::RagDollTemplate* ragDoll = m_animationSystems ? m_animationSystems->ragDoll(kRagDollScript) : nullptr;
	const world::ObjectTemplate* t = body->objectTemplate();
	if (!ragDoll || !t || !t->settings().count("skeleton3P")) {
		return false;
	}
	const anim::Skeleton* skeleton = m_animationSystems->skeleton(t->settings().at("skeleton3P"));
	if (!skeleton) {
		return false;
	}
	RemoteBody remote;
	remote.object = object;
	remote.skeleton = skeleton;
	remote.pose = anim::setupPose(*skeleton);
	std::vector<anim::Matrix> pose = anim::worldMatrices(*skeleton, remote.pose);
	const float yaw = body->rotation().x * 3.14159265f / 180.0f;
	const float c = std::cos(yaw), s = std::sin(yaw);
	for (anim::Matrix& m : pose) {
		for (int row = 0; row < 4; ++row) {
			const float x = m[row * 4], z = m[row * 4 + 2];
			m[row * 4] = x * c + z * s;
			m[row * 4 + 2] = -x * s + z * c;
		}
		m[12] += body->position().x;
		m[13] += body->position().y;
		m[14] += body->position().z;
	}
	remote.ragDoll = std::make_unique<anim::RagDoll>();
	remote.ragDoll->start(*ragDoll, pose, {0, 0, 0});
	remote.ragDoll->setNetworkDriven(true);
	remote.ragDoll->receive(targets);
	const anim::Vec mid = remote.ragDoll->middle();
	for (anim::Matrix& m : pose) {
		m[12] -= mid[0];
		m[13] -= mid[1];
		m[14] -= mid[2];
	}
	remote.worldPose = std::move(pose);
	remote.ragDoll->pose(*skeleton, remote.pose, remote.worldPose);
	body->setPosition({mid[0], mid[1], mid[2]});
	body->setRotation({0, 0, 0});
	m_remoteBodyStarted[object] = m_time;
	m_remoteBodies.push_back(std::move(remote));
	return true;
}

void GameWorld::removeRemoteRagDoll(uint32_t object)
{
	m_remoteBodies.erase(std::remove_if(m_remoteBodies.begin(), m_remoteBodies.end(),
							 [object](const RemoteBody& b) { return b.object == object; }),
		m_remoteBodies.end());
	m_remoteBodyStarted.erase(object);
}

const anim::RagDoll* GameWorld::remoteRagDoll(uint32_t object) const
{
	for (const RemoteBody& remote : m_remoteBodies) {
		if (remote.object == object) {
			return remote.ragDoll.get();
		}
	}
	return nullptr;
}

void GameWorld::stepRemoteBodies(float seconds)
{
	for (auto it = m_remoteBodies.begin(); it != m_remoteBodies.end();) {
		if (!m_objects.find(it->object)) {
			m_remoteBodyStarted.erase(it->object);
			it = m_remoteBodies.erase(it);
			continue;
		}
		++it;
	}
	// The manager's frame (0x008ae120) updates the bodies nearest the
	// camera first; a network-driven body whose turn comes after
	// ragdoll_clientMaxUpdateTime milliseconds of the frame's updates (by
	// the performance counter, from before the first) takes level 2. On a
	// client every body is a remote one, so this is the frame's order.
	std::vector<RemoteBody*> order;
	for (RemoteBody& remote : m_remoteBodies) {
		order.push_back(&remote);
	}
	if (m_hasCamera) {
		std::stable_sort(order.begin(), order.end(), [this](const RemoteBody* a, const RemoteBody* b) {
			return ragDollDistance(*a->ragDoll) < ragDollDistance(*b->ragDoll);
		});
	}
	for (RemoteBody* remote : order) {
		world::Object* body = m_objects.find(remote->object);
		anim::RagDoll& ragDoll = *remote->ragDoll;
		const anim::RagDollTemplate* t = ragDoll.ragDollTemplate();
		if (m_hasCamera && ragDoll.networkDriven() && t
			&& (m_clock() - m_ragDollFrameStart) * 1000.0 > t->settings.clientMaxUpdateTime) {
			ragDoll.setLevelOfDetail(2);
		}
		const anim::Vec mid = ragDoll.update(seconds, ragDollWorld(ragDoll.middle(), t ? t->settings.completeCollRadius : 1.5f));
		ragDoll.pose(*remote->skeleton, remote->pose, remote->worldPose);
		playRagDollImpact(ragDoll, mid);
		body->setPosition({mid[0], mid[1], mid[2]});
		body->setRotation({0, 0, 0});
	}
	stepRemoteWrecks();
}

void GameWorld::setRemoteHealth(uint32_t object, float health, bool playerAttached)
{
	world::Object* body = m_objects.find(object);
	if (!body) {
		m_remoteWrecks.erase(object);
		return;
	}
	const auto found = m_remoteWrecks.find(object);
	if (health > 0) {
		// Whole again (0x00654120's other branch): the fade back to 1, the
		// times cleared.
		if (found != m_remoteWrecks.end()) {
			body->setFade(1);
			m_remoteWrecks.erase(found);
		}
		return;
	}
	if (found != m_remoteWrecks.end()) {
		found->second.attached = playerAttached;
		return;
	}
	RemoteWreck wreck;
	wreck.wreckedAt = m_time;
	wreck.attached = playerAttached;
	if (const world::ObjectTemplate* t = body->objectTemplate()) {
		wreck.soldier = sameName(t->typeName(), "Soldier");
		wreck.landedDelay = number(*t, "ragdollLandedDelay", 4);
		wreck.stayAsWreck = number(*t, "armor.timeToStayAsWreck", 20);
		wreck.afterDestroyed = number(*t, "armor.timeToStayAfterDestroyed", 0);
		if (!flag(*t, "armor.canBeDestroyed", true) || wreck.stayAsWreck == -1) {
			return; // no countdown (0x00654d80 needs the template's +0x166 and timeToStayAsWreck not -1)
		}
	}
	m_remoteWrecks[object] = wreck;
}

// The armour's countdown on a client (0x00652380 for a soldier, class
// 0x9493): it begins timeToStayAsWreck after the wreck once the body has
// landed (0x005eda20: no ragdoll, or one that touched something
// ragdollLandedDelay after it began) and no player is attached, or after
// 60 s (*0x00a79388) whatever; the soldier's geometry then fades over
// timeToStayAfterDestroyed. Other objects' countdowns only end in the
// removal, which is the server's (no fade: soldiers only), so they are
// left out here.
void GameWorld::stepRemoteWrecks()
{
	for (auto it = m_remoteWrecks.begin(); it != m_remoteWrecks.end();) {
		RemoteWreck& wreck = it->second;
		world::Object* body = m_objects.find(it->first);
		if (!body) {
			it = m_remoteWrecks.erase(it);
			continue;
		}
		if (!wreck.soldier) {
			++it;
			continue;
		}
		const anim::RagDoll* ragDoll = remoteRagDoll(it->first);
		const auto started = m_remoteBodyStarted.find(it->first);
		const double diedAt = started != m_remoteBodyStarted.end() ? started->second : wreck.wreckedAt;
		const bool landed = !ragDoll || (ragDoll->touched() && m_time > diedAt + wreck.landedDelay);
		bool begin = m_time > wreck.wreckedAt + wreck.stayAsWreck && landed && !wreck.attached;
		if (m_time > wreck.wreckedAt + 60) {
			begin = true;
		}
		if (begin && wreck.fadeFrom < 0) {
			wreck.fadeFrom = m_time;
		}
		if (wreck.fadeFrom >= 0 && wreck.afterDestroyed > 0) {
			const double left = wreck.fadeFrom + wreck.afterDestroyed - m_time;
			body->setFade(std::clamp(static_cast<float>(left / wreck.afterDestroyed), 0.0f, 1.0f));
		}
		++it;
	}
}

std::vector<uint32_t> GameWorld::corpses() const
{
	std::vector<uint32_t> objects;
	for (const Corpse& corpse : m_corpses) {
		objects.push_back(corpse.object);
	}
	return objects;
}

void GameWorld::setCamera(const physics::Vec3& position, const physics::Vec3& forward)
{
	m_hasCamera = true;
	m_cameraPosition = position;
	m_cameraForward = forward;
}

// The ragdolls' levels of detail (the manager's frame 0x008ae120): with a
// camera, each body's distance to it (its middle particle, +0x20), three
// times as far when behind it; level 1 for each with three awake bodies
// nearer (anim::ragDollLevels). Level 2 is for network-driven bodies
// (remote soldiers', driveRemoteRagDoll) whose update comes after
// ragdoll_clientMaxUpdateTime of the frame (stepRemoteBodies); the frame's
// time starts here.
float GameWorld::ragDollDistance(const anim::RagDoll& body) const
{
	const anim::Vec mid = body.middle();
	const physics::Vec3 d = physics::Vec3 {mid[0], mid[1], mid[2]} - m_cameraPosition;
	float distance = std::sqrt(physics::lengthSquared(d));
	if (physics::dot(d, m_cameraForward) < 0) {
		distance *= 3;
	}
	return distance;
}

void GameWorld::setRagDollLevels()
{
	std::vector<anim::RagDoll*> bodies;
	for (const auto& owned : m_players) {
		if (owned->ragDoll) {
			bodies.push_back(owned->ragDoll.get());
		}
	}
	for (Corpse& corpse : m_corpses) {
		if (corpse.ragDoll) {
			bodies.push_back(corpse.ragDoll.get());
		}
	}
	for (RemoteBody& remote : m_remoteBodies) {
		bodies.push_back(remote.ragDoll.get());
	}
	if (!m_hasCamera) {
		for (anim::RagDoll* body : bodies) {
			body->setLevelOfDetail(0);
		}
		return;
	}
	std::vector<float> distances;
	std::vector<bool> awake;
	for (const anim::RagDoll* body : bodies) {
		distances.push_back(ragDollDistance(*body));
		awake.push_back(!body->sleeping());
	}
	const std::vector<int> levels = anim::ragDollLevels(distances, awake);
	for (size_t i = 0; i < bodies.size(); ++i) {
		bodies[i]->setLevelOfDetail(levels[i]);
	}
	m_ragDollFrameStart = m_clock();
}

// A medic's shock brings a downed soldier back where it lies (the
// server's game logic 0x004c1960, reached from the projectile's
// ResurrectCollisionComp 0x005e7a70 through 0x005a3bd0): the shock's
// collision.resurrectDamage times the material cell goes to the downed
// soldier's armour, which is up again; then its hit points are raised to
// the shock's collision.restoreHP when lower (the defibrillator's: 120)
// through the armour's setter (slot 0x38, 0x00655780 -> 0x00655460),
// which clamps to the template's maxHitPoints (template +0xc8, getter
// 0x00630370): a revived soldier is back at its full 100. Its kit and
// weapon come back.
void GameWorld::revive(Player& victim, Player* medic, float restoreHitPoints)
{
	if (!victim.manDown) {
		return;
	}
	world::Object* body = m_objects.find(victim.object);
	if (!body) {
		return;
	}
	victim.manDown = false;
	victim.alive = true;
	victim.respawnIn = 0;
	if (victim.ragDoll) {
		// Up where the body lies (its middle particle, over the ground),
		// facing from it towards the head. The soldier only leaves its
		// ragdoll mode (0x005f0f20 with 0, from the soldier's update
		// 0x0058d3xx once its armour is no longer man down): BF2142.exe
		// never names the 3p system's reviveOnBack / reviveOnStomach (or
		// specialMoves) triggers, and its named trigger lookups (0x008c0e70)
		// are only the prone transitions (0x008b9840), so no get-up
		// animation plays. The medic is heard saying "get_up" (radio
		// event 9, 0x004c1960).
		const anim::Vec mid = victim.ragDoll->middle();
		const anim::RagDoll::Particle* head = nullptr;
		for (const anim::RagDoll::Particle& particle : victim.ragDoll->particles()) {
			if (particle.bone == 47) {
				head = &particle;
			}
		}
		if (head) {
			victim.body.yawDegrees = std::atan2(head->position[0] - mid[0], head->position[2] - mid[2]) * 57.29578f;
		}
		const float ground = m_ground.height ? m_ground.height(mid[0], mid[2]) : mid[1] - 0.2f;
		victim.body.position = {mid[0], std::max(ground, mid[1] - 0.3f) + 1.0f, mid[2]};
		body->setPosition({mid[0], victim.body.position.y - 1.0f, mid[2]});
		body->setRotation({victim.body.yawDegrees, 0, 0});
		victim.lastYaw = victim.body.yawDegrees;
		victim.control.reset();
		victim.ragDoll.reset();
		if (victim.skeleton) {
			victim.pose = anim::setupPose(*victim.skeleton);
		}
	}
	float& hitPoints = m_hitPoints[victim.object];
	hitPoints = std::max(hitPoints, restoreHitPoints);
	if (const world::ObjectTemplate* t = body->objectTemplate(); t && number(*t, "armor.maxHitPoints", 0) > 0) {
		hitPoints = std::min(hitPoints, number(*t, "armor.maxHitPoints", 0));
	}
	victim.body.velocity = {};
	// The revived soldier's kit (its event 0x19, 0x00587b90): the closest
	// on the ground within 10 m (his own, as a rule), else a new one of the
	// player's choice (0x004bfcc0).
	if (!pickUpClosestKit(victim, 10.0f)) {
		world::ObjectTemplate* kit = m_templates.find(
			victim.kitTemplate.empty() ? (victim.team == 1 ? "Pac_Assault" : "EU_Assault") : victim.kitTemplate);
		if (kit) {
			if (world::Object* worn = m_objects.create(kit, body->position(), body->rotation())) {
				worn->setHidden(true);
				victim.kitObject = worn->id();
			}
			world::ObjectTemplate* primary = nullptr;
			for (const world::ChildTemplate& child : kit->children()) {
				world::ObjectTemplate* item = m_templates.find(child.name);
				if (item && item->typeName() == "GenericFireArm" && integer(*item, "itemIndex", 0) == 3) {
					primary = item;
					break;
				}
			}
			fillKit(victim, kit, primary);
			victim.battery = 0;
			raise("PickupKit", {ScriptValue::player(victim.id), ScriptValue::object(victim.kitObject)});
		}
		if (world::ObjectTemplate* inHand = m_templates.find(victim.weaponTemplate)) {
			if (world::Object* held = m_objects.create(inHand, body->position(), body->rotation())) {
				victim.weaponObject = held->id();
			}
		}
	}
	raise("PlayerRevived", {ScriptValue::player(victim.id), medic ? ScriptValue::player(medic->id) : ScriptValue::none()});
}

GameWorld::Player* GameWorld::downedPlayer(uint32_t object) const
{
	for (const auto& player : m_players) {
		if (player->manDown && player->object == object && object != 0) {
			return player.get();
		}
	}
	return nullptr;
}



// At death (0x005f0a00): the skeleton's last pose in world space (the model
// turned by the soldier's yaw at its feet) starts the ragdoll with the
// body's velocity and the explosion's push, if any (addForce, bone -2).
// From then on the object sits at the middle particle, unturned, and the
// pose is relative to it.
bool GameWorld::startRagDoll(Player& victim, bool thrown)
{
	world::Object* body = m_objects.find(victim.object);
	const anim::RagDollTemplate* ragDoll = m_animationSystems ? m_animationSystems->ragDoll(kRagDollScript) : nullptr;
	if (!body || !ragDoll || !victim.skeleton || victim.worldPose.size() != victim.skeleton->nodes.size()) {
		return false;
	}
	const float yaw = body->rotation().x * 3.14159265f / 180.0f;
	const float c = std::cos(yaw), s = std::sin(yaw);
	std::vector<anim::Matrix> pose = victim.worldPose;
	for (anim::Matrix& m : pose) {
		for (int row = 0; row < 4; ++row) {
			const float x = m[row * 4], z = m[row * 4 + 2];
			m[row * 4] = x * c + z * s;
			m[row * 4 + 2] = -x * s + z * c;
		}
		m[12] += body->position().x;
		m[13] += body->position().y;
		m[14] += body->position().z;
	}
	victim.ragDoll = std::make_unique<anim::RagDoll>();
	// 0x005f0a00: the soldier's velocity, or the last hit's push (x 2.5)
	// within 16 ticks of it (0x005ee2b0).
	const physics::Vec3 v = m_time < victim.hitPushUntil ? victim.hitPush : victim.body.velocity;
	victim.hitPushUntil = -1;
	victim.ragDoll->start(*ragDoll, pose, {v.x, v.y, v.z});
	if (thrown) {
		// 0x005f0f50: thrown out of its seat, pushed along the object's
		// forward (its matrix's z row) by rd_dieInVehicleForce, bone -1.
		const float force = ragDoll->settings.dieInVehicleForce;
		victim.ragDoll->addForce(-1, {s * force, 0, c * force});
	} else {
		// 0x005ee4d0 keeps the push for 0.5 s.
		const physics::Vec3 push = victim.ragDollForceAt >= 0 && m_time - victim.ragDollForceAt <= 0.5
			? victim.ragDollForce * ragDoll->settings.expForceMod
			: physics::Vec3 {};
		if (physics::lengthSquared(push) > 0) {
			victim.ragDoll->addForce(-2, {push.x, push.y, push.z});
		}
	}
	victim.ragDollForce = {};
	victim.ragDollForceAt = -1;
	const anim::Vec mid = victim.ragDoll->middle();
	for (anim::Matrix& m : pose) {
		m[12] -= mid[0];
		m[13] -= mid[1];
		m[14] -= mid[2];
	}
	victim.worldPose = std::move(pose);
	victim.ragDoll->pose(*victim.skeleton, victim.pose, victim.worldPose);
	body->setPosition({mid[0], mid[1], mid[2]});
	body->setRotation({0, 0, 0});
	return true;
}

// The terrain, and the static objects' soldier collision LOD. The game's
// list of objects (0x008ab6b0, each step) is every object with collision
// (slot 0xc4 with 2) the object manager finds (slot 0x44) within
// ragdoll_completeCollRadius (1.5 m) of the middle particle, kept when its
// collision mesh's LOD 2 comes within that radius in its own space
// (0x006aec10); vehicles and other moving objects among them are not
// tested here (the rewrite's world has no moving soldier-LOD meshes).
// The ragdoll's world (0x008ab6b0 lists, each solve, the objects with
// collision whose soldier LOD comes within ragdoll_completeCollRadius of
// the middle particle): the static objects (one merged mesh here, first
// in the list) and the bodies (vehicles, physics objects) whose box comes
// that near, each tested in its own frame at its current pose.
anim::RagDollWorld GameWorld::ragDollWorld(const anim::Vec& middle, float reach) const
{
	anim::RagDollWorld world;
	if (m_ground.heightAndNormal) {
		world.ground = [this](float x, float z, anim::Vec& normal) {
			physics::Vec3 n;
			const float height = m_ground.heightAndNormal(x, z, n);
			normal = {n.x, n.y, n.z};
			return height;
		};
	} else if (m_ground.height) {
		world.ground = [this](float x, float z, anim::Vec& normal) {
			normal = {0, 1, 0};
			return m_ground.height(x, z);
		};
	}
	if (m_ground.material) {
		world.groundMaterial = [this](float x, float z) { return m_ground.material(x, z); };
	}
	const physics::Vec3 mid {middle[0], middle[1], middle[2]};
	auto near = std::make_shared<std::vector<const Body*>>();
	for (const Body& body : m_bodies) {
		if (!body.soldierMesh || body.points.empty()) {
			continue;
		}
		physics::Vec3 low = body.points[0], high = body.points[0];
		for (const physics::Vec3& p : body.points) {
			low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
			high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
		}
		const physics::Vec3 local = body.body.rotation.untransform(mid - body.body.position);
		const physics::Vec3 out {std::max({low.x - local.x, 0.0f, local.x - high.x}),
			std::max({low.y - local.y, 0.0f, local.y - high.y}), std::max({low.z - local.z, 0.0f, local.z - high.z})};
		if (physics::lengthSquared(out) <= reach * reach) {
			near->push_back(&body);
		}
	}
	const auto toVec = [](const physics::Vec3& v) { return anim::Vec {v.x, v.y, v.z}; };
	const auto fromVec = [](const anim::Vec& v) { return physics::Vec3 {v[0], v[1], v[2]}; };
	world.raycast = [this, near, toVec, fromVec](const anim::Vec& from, const anim::Vec& to, anim::Vec& point,
						anim::Vec& normal, uint32_t& material) {
		const physics::Vec3 a = fromVec(from), b = fromVec(to);
		physics::CollisionWorld::Hit hit;
		if (m_collision.triangles() > 0 && m_collision.raycast(a, b, hit) && physics::dot(b - a, hit.faceNormal) < 0) {
			point = toVec(hit.point);
			normal = toVec(hit.faceNormal);
			material = hit.material;
			return true;
		}
		for (const Body* body : *near) {
			const physics::Vec3 la = body->body.rotation.untransform(a - body->body.position);
			const physics::Vec3 lb = body->body.rotation.untransform(b - body->body.position);
			if (body->soldierMesh->raycast(la, lb, hit) && physics::dot(lb - la, hit.faceNormal) < 0) {
				point = toVec(a + (b - a) * hit.fraction);
				normal = toVec(body->body.rotation.transform(hit.faceNormal));
				material = hit.material;
				return true;
			}
		}
		return false;
	};
	world.capsule = [this, near, toVec, fromVec](const anim::Vec& a, const anim::Vec& b, float radius,
						std::vector<std::pair<anim::Vec, float>>& contacts) {
		std::vector<physics::SphereContact> found;
		if (m_collision.triangles() > 0) {
			m_collision.capsuleEdgeContacts(fromVec(a), fromVec(b), radius, found);
		}
		for (const physics::SphereContact& c : found) {
			contacts.push_back({toVec(c.normal), c.depth});
		}
		for (const Body* body : *near) {
			found.clear();
			body->soldierMesh->capsuleEdgeContacts(body->body.rotation.untransform(fromVec(a) - body->body.position),
				body->body.rotation.untransform(fromVec(b) - body->body.position), radius, found);
			for (const physics::SphereContact& c : found) {
				contacts.push_back({toVec(body->body.rotation.transform(c.normal)), c.depth});
			}
		}
	};
	world.movingNear = [near](const anim::Vec&, float) {
		for (const Body* body : *near) {
			if (physics::lengthSquared(body->body.velocity) >= 0.001f * 0.001f) {
				return true;
			}
		}
		return false;
	};
	world.carriedBy = [near, toVec, fromVec](const anim::Vec& from, anim::Vec& velocity) {
		const physics::Vec3 a = fromVec(from);
		const physics::Vec3 b = a - physics::Vec3 {0, 2, 0};
		for (const Body* body : *near) {
			physics::CollisionWorld::Hit hit;
			const physics::Vec3 la = body->body.rotation.untransform(a - body->body.position);
			const physics::Vec3 lb = body->body.rotation.untransform(b - body->body.position);
			if (body->soldierMesh->raycast(la, lb, hit)) {
				velocity = toVec(body->body.pointVelocity(a + (b - a) * hit.fraction));
				return true;
			}
		}
		return false;
	};
	return world;
}

void GameWorld::stepRagDoll(Player& player, float seconds)
{
	world::Object* body = m_objects.find(player.object);
	if (!player.ragDoll || !body || !player.skeleton) {
		return;
	}
	const anim::RagDollTemplate* ragDoll = player.ragDoll->ragDollTemplate();
	const anim::Vec mid = player.ragDoll->update(
		seconds, ragDollWorld(player.ragDoll->middle(), ragDoll ? ragDoll->settings.completeCollRadius : 1.5f));
	player.ragDoll->pose(*player.skeleton, player.pose, player.worldPose);
	playRagDollImpact(*player.ragDoll, mid);
	body->setPosition({mid[0], mid[1], mid[2]});
	body->setRotation({0, 0, 0});
}

// The soldier's sound 0 (its sound set +0x324, 0x007fd970): the footstep
// set's sound loaded for what it stands on (0x007fd840: the cell of
// material 0x157c against it, templates 0-3; 0x007fda00 picks the
// material, see Player::footstepMaterial): template 3, else 0.
void GameWorld::playAnimationSound(const Player& player, int)
{
	if (!m_materials) {
		return;
	}
	const physics::Vec3& at = player.body.position;
	const uint32_t surface = player.footstepMaterial;
	const physics::MaterialCell* cell = m_materials->cell(kSoldierFootStepsMaterial, surface);
	if (!cell) {
		return;
	}
	for (const size_t index : {size_t {3}, size_t {0}}) {
		if (cell->soundTemplates.size() > index && !cell->soundTemplates[index].empty()) {
			m_sounds.push_back({cell->soundTemplates[index], {at.x, at.y - 1, at.z}});
			return;
		}
	}
}

// The impact sound (polled each soldier tick, 0x0058d350 -> 0x008a79f0;
// played by 0x007fd780): the material manager's slot 0x74 (0x0060a6d0)
// with material 0x157c (5500, Soldier_FootSteps) against the surface's,
// its sound template 4 (S_Soldier_RagDoll_Impact), falling back to the
// cell against material 0 when that cell has none.
void GameWorld::playRagDollImpact(anim::RagDoll& ragDoll, const anim::Vec& at)
{
	float speed = 0;
	uint32_t material = 0;
	if (!ragDoll.takeImpact(speed, material) || !m_materials) {
		return;
	}
	const std::string* sound = nullptr;
	for (const uint32_t surface : {material, 0u}) {
		const physics::MaterialCell* cell = m_materials->cell(kSoldierFootStepsMaterial, surface);
		if (cell && cell->soundTemplates.size() > 4 && !cell->soundTemplates[4].empty()) {
			sound = &cell->soundTemplates[4];
			break;
		}
	}
	if (sound) {
		m_sounds.push_back({*sound, {at[0], at[1], at[2]}});
	}
}

const anim::RagDoll* GameWorld::ragDoll(uint32_t soldier) const
{
	const Player* player = downedPlayer(soldier);
	if (player) {
		return player->ragDoll.get();
	}
	for (const Corpse& corpse : m_corpses) {
		if (corpse.object == soldier) {
			return corpse.ragDoll.get();
		}
	}
	return nullptr;
}

// Seated: the soldier at its seat (seatInformation's position and
// rotation in the vehicle's frame), posed by the seat's animation system
// (seatAnimationSystem, its "root" trigger: the seat's still bundle) and
// drawn; seats without one keep the soldier out of sight, and the
// handheld weapon is put away.
void GameWorld::poseSeated(Player& player, world::Object& soldier, const Body& body, const SeatLayout& seat, float seconds)
{
	const physics::Mat3 rotation = physics::Mat3::fromYawPitchRoll(seat.rotation) * body.body.rotation;
	// The seat is where the soldier's object would be: the physics origin a
	// metre over the skeleton's (0x005f0a00 poses the skeleton at 0/-1/0
	// from it), so the model goes a metre below, in the seat's frame.
	const physics::Vec3 at = body.body.position + body.body.rotation.transform(seat.position) + rotation.transform({0, -1, 0});
	soldier.setPosition({at.x, at.y, at.z});
	soldier.setRotation({rotation.yawPitchRoll().x, rotation.yawPitchRoll().y, rotation.yawPitchRoll().z});
	if (player.seatAnimationPath != seat.animationSystem) {
		player.seatAnimationPath = seat.animationSystem;
		player.seatAnimation.reset();
		if (!seat.animationSystem.empty() && m_loadAnimationSystem && m_animationSystems) {
			if (const anim::AnimationSystemTemplate* system = m_loadAnimationSystem(seat.animationSystem)) {
				player.seatAnimation = std::make_unique<anim::AnimationState>(*m_animationSystems, *system);
			}
		}
		if (player.skeleton) {
			player.pose = anim::setupPose(*player.skeleton);
		}
	}
	soldier.setHidden(!player.seatAnimation || !player.skeleton);
	if (world::Object* held = player.weaponObject ? m_objects.find(player.weaponObject) : nullptr) {
		held->setHidden(true);
	}
	if (!player.seatAnimation || !player.skeleton) {
		return;
	}
	player.seatAnimation->update({}, seconds);
	std::vector<bool> touched;
	player.seatAnimation->apply(*player.skeleton, player.pose, touched);
	player.worldPose = anim::worldMatrices(*player.skeleton, player.pose);
}

// An effect event N of the held weapon's animation (0x0061a4e0 per
// EffectBundle under the weapon): the bundles whose startOnEffects
// (template +0x454) has bit 9 + N start, placed by their offset from the
// weapon where its rounds leave (the muzzle's spot). stopOnEffects (+0x458)
// would end a looping one; the rewrite's effects are one-shot. A bundle
// already running (+0x18e) isn't started again unless the template's
// +0x3f4 is below 0; the rewrite doesn't track it.
void GameWorld::startEffectsOn(const Player& player, int effect)
{
	const world::ObjectTemplate* weapon = m_templates.find(player.weaponTemplate);
	if (!weapon) {
		return;
	}
	const float look = lookYaw(player);
	const float pitch = player.input.aimPitch;
	const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({look, -pitch, 0});
	const physics::Vec3 origin = player.body.position + physics::Vec3 {0, 0.6f, 0} + turn.transform({0, 0, 0.5f});
	const uint32_t bit = 1u << (9 + effect);
	const std::function<void(const world::ObjectTemplate&, const physics::Vec3&, int)> visit =
		[&](const world::ObjectTemplate& t, const physics::Vec3& offset, int depth) {
			if (depth > 8) {
				return;
			}
			for (const world::ChildTemplate& child : t.children()) {
				const world::ObjectTemplate* ct = m_templates.find(child.name);
				if (!ct) {
					continue;
				}
				const physics::Vec3 at = offset + physics::Vec3 {child.position.x, child.position.y, child.position.z};
				if (sameName(ct->typeName(), "EffectBundle")) {
					const uint32_t start = static_cast<uint32_t>(
						integer(*ct, "setStartOnEffects", integer(*ct, "startOnEffects", 0)));
					if (start & bit) {
						m_effects.spawn(ct->name(), origin + turn.transform(at), turn);
					}
				}
				visit(*ct, at, depth + 1);
			}
		};
	visit(*weapon, {}, 0);
}

void GameWorld::setAnimationSystems(anim::AnimationSystems* systems, AnimationLoader load)
{
	m_animationSystems = systems;
	m_loadAnimationSystem = std::move(load);
}

// What the soldier's animation systems see (the soldier's state the
// triggers read, 0x008b9...): pose, messages, the movement's direction in
// the soldier's frame, speed and turn rate.
void GameWorld::stepAnimation(Player& player, float seconds)
{
	// The footstep set (0x007fda00 -> 0x007fd840): on a ladder 0x19c8; on
	// the ground or swimming (the response's +0xf9, stance 3) the material
	// of the last collision event (the soldier's +0x32c through its +0x18c
	// slot 0x60, 0x0063a0f0: the terrain's or the object's face's); in the
	// air the set stays.
	if (player.ladder >= 0) {
		player.footstepMaterial = 0x19c8u;
	} else if (player.body.onGround() || player.body.stance == 3) {
		player.footstepMaterial = player.contactMaterial;
	}
	if (!player.bodyAnimation) {
		return;
	}
	anim::AnimationInput input;
	const physics::Vec3& v = player.body.velocity;
	const float speed = std::sqrt(v.x * v.x + v.z * v.z);
	const float yaw = player.body.yawDegrees * 3.14159265f / 180.0f;
	input.speed = speed;
	if (speed > 0.01f) {
		input.forward = (v.x * std::sin(yaw) + v.z * std::cos(yaw)) / speed;
		input.side = (v.x * std::cos(yaw) - v.z * std::sin(yaw)) / speed;
	}
	// The up value (0x005ef250 / 0x005eef80): the jump state; standing (or
	// on a ladder) 0. The jump and land sounds are the footstep set's
	// (0x007fd970, see playAnimationSound).
	{
		const float velocity[3] = {v.x, v.y, v.z};
		const physics::Vec3 feet = player.body.position - physics::Vec3 {0, 1, 0};
		const uint32_t self = player.object;
		anim::SoldierJumpAnimation::Sound sound;
		uint32_t landedOn = player.contactMaterial;
		input.upSpeed = player.jumpAnimation.update(player.body.onGround() || player.ladder >= 0, velocity,
			[this, feet, self, &landedOn](const float way[3]) {
				return groundMaterialAlong(feet, feet + physics::Vec3 {way[0], way[1], way[2]}, self, landedOn);
			},
			sound);
		if (sound == anim::SoldierJumpAnimation::Sound::Land) {
			// The landing (0x005ef250): the ray's material becomes the
			// soldier's +0x32c (the footstep set's next material), and the
			// sound plays through 0x007fdcd0 only for the local player
			// (player slot 0x1bc, 0x00591510: the PlayerManager's local one,
			// slot 0x30, in a first-person view, its camera's kind 1 or 2;
			// the rewrite has no view kinds).
			player.contactMaterial = landedOn;
			if (player.id == localPlayer()) {
				playAnimationSound(player, 0);
			}
		} else if (sound != anim::SoldierJumpAnimation::Sound::None) {
			playAnimationSound(player, 0);
		}
	}
	input.movementSeconds = seconds;
	// TurnTrigger's value: the soldier's rotational velocity in degrees per
	// second (at most 1499, 0x0058a6d0) times 0.01 (0x005ef250).
	float turn = player.body.yawDegrees - player.lastYaw;
	turn = std::remainder(turn, 360.0f);
	input.turn = std::clamp(turn / seconds, -1499.0f, 1499.0f) * 0.01f;
	player.lastYaw = player.body.yawDegrees;
	input.pose = static_cast<int>(player.input.pose);
	if (player.weapon && player.weapon->reloading()) {
		input.messages |= 2; // reload
	}
	if (player.fired) {
		input.pulses |= 1; // fire
		player.fired = false;
	}
	player.bodyAnimation->update(input, seconds);
	if (player.weaponAnimation) {
		player.weaponAnimation->update(input, seconds);
		// The weapon's effect events (0x2c + N) go down its tree (the base
		// handler 0x0063ca60 passes each event to the children's slot
		// 0x100); an EffectBundle (vtable 0x0097b658's 0x0061a4e0) starts
		// when its startOnEffects has bit 9 + N (1 << (event - 0x23)).
		for (const anim::AnimationState::FiredEvent& event : player.weaponAnimation->takeEvents()) {
			if (event.type == anim::Bundle::EffectEvent && event.id >= 0 && event.id < 10) {
				startEffectsOn(player, event.id);
			}
		}
	}
	// The body's events go to the soldier's handler (0x00587b90): sound 0
	// (event 0x36) is the footstep set's sound.
	for (const anim::AnimationState::FiredEvent& event : player.bodyAnimation->takeEvents()) {
		if (event.type == anim::Bundle::SoundEvent && event.id == 0) {
			playAnimationSound(player, 0);
		}
	}
	// The pose, kept for drawing, hits and the weapon's parts.
	if (const anim::Skeleton* skeleton = player.skeleton) {
		anim::LocalPose& pose = player.pose;
		if (pose.rotations.size() != skeleton->nodes.size()) {
			pose = anim::setupPose(*skeleton);
		}
		std::vector<bool> touched;
		player.bodyAnimation->apply(*skeleton, pose, touched);
		if (player.weaponAnimation) {
			player.weaponAnimation->apply(*skeleton, pose, touched);
		}
		player.worldPose = anim::worldMatrices(*skeleton, pose);
	}
}

bool GameWorld::skeletonPose(uint32_t object, const anim::Skeleton& skeleton, std::vector<anim::Matrix>& world)
{
	const Player* player = soldierPlayer(object);
	if (!player) {
		player = downedPlayer(object); // the ragdoll's pose
	}
	if (!player) {
		for (const SeatCorpse& corpse : m_seatCorpses) {
			if (corpse.object == object && corpse.worldPose.size() == skeleton.nodes.size()) {
				world = corpse.worldPose;
				return true;
			}
		}
		for (const Corpse& corpse : m_corpses) {
			if (corpse.object == object && corpse.worldPose.size() == skeleton.nodes.size()) {
				world = corpse.worldPose;
				return true;
			}
		}
		for (const RemoteBody& remote : m_remoteBodies) {
			if (remote.object == object && remote.worldPose.size() == skeleton.nodes.size()) {
				world = remote.worldPose;
				return true;
			}
		}
	}
	if (!player || player->worldPose.size() != skeleton.nodes.size()) {
		return false;
	}
	world = player->worldPose;
	return true;
}

bool GameWorld::partTransforms(uint32_t object, std::vector<anim::Matrix>& parts) const
{
	if (object == 0) {
		return false;
	}
	for (const auto& owned : m_players) {
		const Player& player = *owned;
		if (!player.alive || player.weaponObject != object || player.worldPose.empty()) {
			continue;
		}
		for (size_t i = 0; i < parts.size() && i < player.weaponNodes.size(); ++i) {
			const int node = player.weaponNodes[i];
			if (node >= 0 && static_cast<size_t>(node) < player.worldPose.size()) {
				parts[i] = player.worldPose[static_cast<size_t>(node)];
			}
		}
		return true;
	}
	return false;
}

std::vector<std::string> GameWorld::playingAnimations(uint32_t object) const
{
	const Player* player = soldierPlayer(object);
	if (player && player->vehicle && player->seatAnimation) {
		return player->seatAnimation->playing();
	}
	if (!player || !player->bodyAnimation) {
		return {};
	}
	return player->bodyAnimation->playing();
}

std::string GameWorld::animationFor(uint32_t object) const
{
	const Player* player = soldierPlayer(object);
	if (!player || player->bodyAnimation) {
		return {};
	}
	// Lower body by speed (without the animation systems loaded).
	const physics::Vec3& v = player->body.velocity;
	const float speed = std::sqrt(v.x * v.x + v.z * v.z);
	const char* base = "Objects/Soldiers/Common/Animations/3P/";
	if (speed < 0.5f) {
		return std::string(base) + "3p_stand.baf";
	}
	const float yaw = player->body.yawDegrees * 3.14159265f / 180.0f;
	const float forward = v.x * std::sin(yaw) + v.z * std::cos(yaw);
	if (forward < 0) {
		return std::string(base) + "3p_runBackward.baf";
	}
	return std::string(base) + (speed > 5.0f ? "3p_Sprint.baf" : "3p_runForward.baf");
}

world::Object* GameWorld::player() const { return m_objects.find(playerObject()); }

void GameWorld::stepPlayers(float seconds)
{
	if (m_player) {
		m_player->input = m_playerInput;
		m_playerInput.turn = 0; // a turn is used once
	}
	for (size_t i = 0; i < m_players.size(); ++i) {
		Player& player = *m_players[i];
		if (player.alive) {
			stepSoldier(player, seconds);
			player.input.turn = 0;
		} else {
			if (player.manDown) {
				stepRagDoll(player, seconds);
				player.manDownLeft -= seconds;
				if (player.manDownLeft <= 0) {
					finishDeath(player);
				}
			}
			if (player.respawnIn > 0) {
				player.respawnIn = std::max(0.0f, player.respawnIn - seconds);
				if (player.respawnIn == 0 && autoRespawn) {
					if (player.manDown) {
						finishDeath(player);
					}
					spawnSoldier(player);
				}
			}
		}
	}
}

void GameWorld::stepSoldier(Player& player, float seconds)
{
	if (!m_ground.height) {
		return;
	}
	world::Object* object = m_objects.find(player.object);
	if (!object) {
		player.alive = false;
		player.respawnIn = m_spawnTime;
		return;
	}
	SoldierInput& input = player.input;
	if (input.use && !player.useHeld) {
		enterOrLeaveVehicle(player, *object);
	}
	player.useHeld = input.use;
	if (player.vehicle) {
		Body* body = findBody(player.vehicle);
		if (!body) {
			player.vehicle = 0;
			object->setHidden(false);
		} else {
			// Changing seats (to a free one).
			if (input.switchSeat > 0 && !player.switchHeld) {
				const int wanted = input.switchSeat - 1;
				if (wanted != player.seat && static_cast<size_t>(wanted) < body->occupants.size()
					&& body->occupants[static_cast<size_t>(wanted)] < 0) {
					if (player.seat == 0 && body->vehicle) {
						body->vehicle->setInput({});
					}
					seatPlayer(player, *body, wanted);
				}
			}
			player.switchHeld = input.switchSeat > 0;
			if (player.seat == 0 && body->vehicle && body->wreckTime < 0) {
				const world::Object* vehicleObject = m_objects.find(body->object);
				const float scale = vehicleObject
					? driverInputScale(body->empLevel, hitPoints(body->object), *vehicleObject->objectTemplate())
					: 1.0f;
				// The land and air maps: the mouse's x is PIRoll and its y
				// PIPitch (a tick's look over 5 degrees, as PIMouseLook's
				// units), A/D PIYaw, W/S PIThrottle, Z PICrouch.
				VehicleInput driven;
				driven.throttle = input.throttle * scale;
				driven.steer = input.strafe * scale;
				driven.roll = std::clamp(input.turn / 5.0f, -1.0f, 1.0f) * scale;
				driven.pitch = (input.hasPitch ? std::clamp(input.pitch, -1.0f, 1.0f)
										   : std::clamp(-(input.aimPitch - player.lastLookPitch) / 5.0f, -1.0f, 1.0f))
					* scale;
				driven.crouch = input.pose == SoldierPose::Crouch ? 1.0f : 0.0f;
				driven.sprint = input.sprint;
				body->vehicle->setInput(driven);
				// The pilot's own gun (the gunships' seat GenericFireArm,
				// fixed along the hull): PIFire fires it; the mouse flies the
				// vehicle, so the gun keeps its aim. (Ground drivers' guns
				// turn on the mouse in the original, which the rewrite's
				// drivers spend on PIRoll: not fired here.)
				if (body->vehicle->flies() && !body->guns.empty() && body->guns[0].weapon) {
					stepGunner(player, *body, seconds, false);
				}
			} else if (body->wreckTime < 0 && player.seat != 0 && input.altFire && launchPod(player.id)) {
				return; // a pod seat's alternate fire (Titans.cpp): off in a drop pod
			} else if (body->wreckTime < 0 && (player.seat != 0 || !body->vehicle)) {
				// The guns under EMP keep firing: the PlayerControlObject's
				// input filter (0x005ca2e0) scales only the driver's throttle,
				// yaw, pitch and roll and leaves PIFire and a gunner's aim
				// (PIMouseLook) alone, unless the seat's armour is its own
				// (a child's), which the rewrite's vehicles don't have; no
				// firearm component reads the level (only the active defence's
				// ammo, 0x005d9560, and the camouflage's drain, 0x005d9030).
				// An auto-controlled one loses every input above level 1 (see
				// the sentry drone's gun).
				stepGunner(player, *body, seconds);
			}
			stepSeatCamera(player, *body, seconds);
			// The soldier rides in the seat.
			physics::Vec3 seatAt = body->body.position;
			const SeatLayout* seat = nullptr;
			if (const world::Object* vehicleObject = m_objects.find(body->object)) {
				const VehicleLayout& layout = vehicleLayout(*vehicleObject->objectTemplate());
				if (player.seat >= 0 && static_cast<size_t>(player.seat) < layout.seats.size()) {
					seat = &layout.seats[static_cast<size_t>(player.seat)];
					seatAt = seatAt + body->body.rotation.transform(seat->position);
				}
			}
			player.body.position = {seatAt.x, seatAt.y + 1.0f, seatAt.z};
			player.body.velocity = body->body.velocity;
			followSoldier(player, *object);
			if (seat) {
				poseSeated(player, *object, *body, *seat, seconds);
			} else {
				object->setPosition({seatAt.x, seatAt.y, seatAt.z});
				object->setRotation({body->body.rotation.yawPitchRoll().x, 0, 0});
				object->setHidden(true);
			}
			return;
		}
	}
	if (input.selectItem > 0 && !player.selectHeld) {
		equip(player, input.selectItem);
	}
	player.selectHeld = input.selectItem > 0;
	// Swimming (the response's +0xfa, soldier +0x30c; 0x0058a6d0): the pose
	// is the swimming one (3) and back to standing out of the water; the
	// stance keys do nothing (0x005889d0), strafing stops (0x00588420) and
	// the weapon is put away (0x005878b0).
	if (player.body.swimming()) {
		input.pose = SoldierPose::Swim;
		input.strafe = 0;
		input.fire = false;
		input.altFire = false;
	} else if (input.pose == SoldierPose::Swim) {
		input.pose = SoldierPose::Stand;
	}
	player.body.stance = static_cast<int>(input.pose);
	// A ladder: the soldier sits in its container and climbs (0x0059f6b0)
	// instead of moving; getting on is the ladder's collision with it
	// (0x0059fd40).
	if (player.ladder < 0) {
		tryLadder(player, input);
	}
	if (player.ladder >= 0) {
		climbLadder(player, input, seconds);
	}
	if (player.ladder >= 0) {
		input.fire = false;
		input.altFire = false;
		player.body.velocity = {};
		const physics::Vec3 p = player.body.position;
		object->setPosition({p.x, p.y - 1.0f, p.z});
		object->setRotation({player.body.yawDegrees, 0, 0});
		followSoldier(player, *object);
		return;
	}
	// The moving objects near it (the soldier's broad phase 0x006dcce0): their
	// soldier LOD in their own frame.
	player.body.movers.clear();
	for (const Body& b : m_bodies) {
		if (!b.soldierMesh || b.object == player.vehicle || findDrone(b.object)) {
			continue;
		}
		const float reach = b.radius + 3.0f;
		if (physics::lengthSquared(b.body.position - player.body.position) > reach * reach) {
			continue;
		}
		physics::SoldierBody::Mover mover;
		mover.mesh = b.soldierMesh.get();
		if ((player.team == 1 || player.team == 2) && b.teamSoldierMesh[player.team - 1]) {
			mover.mesh = b.teamSoldierMesh[player.team - 1].get(); // a Titan's forcefields
		}
		mover.rotation = b.body.rotation;
		mover.position = b.body.position;
		mover.velocity = b.body.velocity;
		mover.angularVelocity = b.body.angularVelocity;
		mover.centerOfMass = b.body.worldCenterOfMass();
		mover.owner = b.object;
		mover.mass = b.mass;
		mover.asleep = b.sleep.asleep();
		mover.collisionGroups = b.collisionGroups;
		mover.ignoring = b.ignoreSet && m_ticks <= b.ignoreUntil;
		mover.ignoresThis = mover.ignoring && b.ignored == player.object;
		mover.pushed = b.pushedBySoldiers;
		mover.own = b.launcher != 0 && b.launcher == player.object;
		player.body.movers.push_back(mover);
	}
	// The other soldiers near it on foot (0x006c2620: a soldier's response
	// in the broad phase's list; one with a parent, in a seat or on a
	// ladder's container, is left out).
	player.body.soldiers.clear();
	for (const auto& owned : m_players) {
		const Player& other = *owned;
		if (&other == &player || !other.alive || other.manDown || other.vehicle != 0 || other.ladder >= 0) {
			continue;
		}
		const physics::Vec3 d = other.body.position - player.body.position;
		if (physics::lengthSquared(d) > 3.0f * 3.0f) {
			continue;
		}
		player.body.soldiers.push_back({other.body.position, other.body.stance});
	}
	// The hit's push a ragdoll starts with within 16 ticks (0x005ee2b0),
	// and the solve's crush, run-over and segment kills (1000 through the
	// game logic, slot 0xe4; the crushing object's driver credited).
	Player* self = &player;
	player.body.onPush = [this, self](const physics::Vec3& push) {
		self->hitPush = push;
		self->hitPushUntil = m_time + 16 / 30.0;
	};
	player.body.onCrushed = [this, self](uint32_t attacker) {
		if (!self->alive) {
			return;
		}
		int credit = -1;
		if (Body* crusher = attacker ? findBody(attacker) : nullptr; crusher && !crusher->occupants.empty()) {
			credit = crusher->occupants[0];
		}
		loseHitPoints(self->object, physics::soldier::kCrushDamage, credit);
	};
	// A launcher's projectile keeps ignoring it while they touch
	// (0x006b8de0 again: the tick + 2); soldiers push a small projectile
	// (its response's slot 0x2c, 0x006ba4c0; an awake one: a sleeping node
	// isn't solved).
	player.body.team = player.team;
	player.body.onIgnored = [this](uint32_t owner) {
		if (Body* b = findBody(owner)) {
			b->ignoreUntil = m_ticks + 2;
		}
	};
	player.body.onPushObject = [this](uint32_t owner, const physics::Vec3& point, const physics::Vec3& velocity,
								   const physics::Vec3& normal, float depth, uint32_t ownMaterial, uint32_t otherMaterial) {
		Body* b = findBody(owner);
		if (!b || asleep(*b)) {
			return;
		}
		physics::ContactMaterial material;
		if (m_materials) {
			material.friction = m_materials->friction(ownMaterial, otherMaterial);
			material.elasticity = (m_materials->elasticity(ownMaterial) + m_materials->elasticity(otherMaterial)) * 0.5f;
		}
		b->response.addContact(point - b->body.position, velocity, normal, depth, material);
	};
	// The collision events: the ground's and each object's on the soldier
	// (0x00586a90 -> the base), and the object's own (0x006c3890).
	const uint32_t soldierId = player.object;
	player.body.onContact = [this, soldierId, self](const physics::SoldierBody::Contact& c) {
		self->contactMaterial = c.otherMaterial; // 0x00586a90: +0x32c
		collisionEvent(soldierId, c.owner, c.velocity, c.normal, c.point, c.ownMaterial, c.otherMaterial);
		if (c.owner != 0 && m_objects.find(soldierId)) {
			collisionEvent(c.owner, soldierId, -c.velocity, c.normal, c.point, c.otherMaterial, c.ownMaterial);
		}
		return m_objects.find(soldierId) != nullptr;
	};
	if (const world::ObjectTemplate* t = object->objectTemplate()) {
		player.body.material = static_cast<uint32_t>(integer(*t, "material", 0));
		// The sprint's stamina from the soldier template (0x00645790 on
		// spawning; the ability modifiers, the player's +0x4c / +0x50, at 1)
		// and the jump's loss (+0x308).
		if (!player.control.stamina().configured()) {
			player.control.stamina().configure(number(*t, "sprintDissipationTime", 0),
				number(*t, "sprintRecoverTime", 0), number(*t, "sprintLimit", 0));
			player.control.jumpLoss = number(*t, "sprintLossAtJump", 0);
		}
	}
	// The recoil kick (0x0058a6d0, while the weapon has hasRecoilForce):
	// k ticks after the last shot the graph's value times the forces drawn
	// (0x005dd5d0); the left-right one (0x00586140) is added to the turn,
	// the up one (0x005861a0) to the pitch (+0x270, down positive: the graph
	// is negative, so the aim rises; with goBackOnRecoil it comes back).
	SoldierInput turned = input;
	if (player.recoil.hasRecoilForce && !player.recoilGraph.empty()) {
		const int k = static_cast<int>(static_cast<int64_t>(m_ticks) - player.lastShotTick);
		turned.turn += anim::recoilAt(player.recoilGraph, k, player.recoilForceLeftRight);
		player.aimKickPitch -= anim::recoilAt(player.recoilGraph, k, player.recoilForceUp);
	}
	player.control.update(turned, player.body, seconds);
	player.body.step(seconds, m_ground);
	if (!player.alive) {
		return; // killed by a collision (a fall, a road kill)
	}
	// Alternate fire sets off the soldier's remote charges (C4).
	if (input.altFire) {
		for (Projectile& charge : m_projectiles) {
			if (charge.alive && charge.player == player.id && charge.settings->remote) {
				explode(charge.position, *charge.settings, player.id, charge.yaw);
				charge.alive = false;
			}
		}
		if (!player.alive) {
			return; // its own blast: the soldier's object is gone
		}
	}
	// Active camouflage (a weapon with CamouflageAmmoComp): cloaked while
	// the trigger is held and it has heat to spare; drawn hidden as a
	// stand-in for the shimmer.
	{
		const world::ObjectTemplate* held = player.weaponTemplate.empty() ? nullptr : m_templates.find(player.weaponTemplate);
		const auto& components = held ? held->components() : std::vector<std::string> {};
		const bool camouflage = std::find(components.begin(), components.end(), "CamouflageAmmoComp") != components.end();
		if (camouflage) {
			const bool on = input.fire && player.weapon && !player.weapon->overheated();
			player.weapon->tick(seconds, on);
			const bool was = player.cloaked;
			player.cloaked = on && !player.weapon->overheated();
			if (was != player.cloaked) {
				// Toggled: reported to each enemy squad leader's squad
				// (0x005d92c0, type 2).
				for (const auto& other : m_players) {
					if (other->squadLeader && other->team != player.team) {
						reportBattlefieldObjects(other->id, {player.id});
					}
				}
			}
			object->setHidden(player.cloaked);
			// No firing anything else while it is in hand (the body has
			// moved already this tick).
			stepAnimation(player, seconds);
			const physics::Vec3& p = player.body.position;
			object->setPosition({p.x, p.y - 1.0f, p.z});
			object->setRotation({player.body.yawDegrees, 0, 0});
			followSoldier(player, *object);
			return;
		}
		if (player.cloaked) {
			player.cloaked = false;
			object->setHidden(false);
			for (const auto& other : m_players) {
				if (other->squadLeader && other->team != player.team) {
					reportBattlefieldObjects(other->id, {player.id});
				}
			}
		}
	}
	// Gadgets: a hub is thrown out when "fired"; the repair tool works
	// while the trigger is held. A gadget without onlyActiveWhileFiring
	// (the hubs) works round its holder while it is in hand, as the
	// ReplenishingAmmoComp's update does (0x005d8740 from the comp's tick
	// when the template's +0x88 flag is off; the flag's own path
	// 0x005d84a0 runs while firing).
	Ability ability;
	bool whileFiring = false;
	world::ObjectTemplate* inHand = player.weaponTemplate.empty() ? nullptr : m_templates.find(player.weaponTemplate);
	const bool gadget = inHand && abilityOf(*inHand, ability, whileFiring);
	player.abilityActive = gadget && (!whileFiring || input.fire);
	if (player.weapon && player.weapon->tick(seconds, input.fire)) {
		if (gadget && !whileFiring) {
			for (auto it = m_supplies.begin(); it != m_supplies.end();) {
				if (it->owner == player.id && it->gadget == inHand->name()) {
					m_objects.remove(it->object);
					it = m_supplies.erase(it);
				} else {
					++it;
				}
			}
			const float yaw = lookYaw(player) * 3.14159265f / 180.0f;
			physics::Vec3 at = player.body.position + physics::Vec3 {std::sin(yaw), 0, std::cos(yaw)} * 1.0f;
			at.y = m_ground.height(at.x, at.z);
			const auto projectile = inHand->settings().find("projectileTemplate");
			world::ObjectTemplate* hub = projectile != inHand->settings().end() ? m_templates.find(projectile->second) : nullptr;
			if (world::Object* placed = hub ? m_objects.create(hub, {at.x, at.y, at.z}, {lookYaw(player), 0, 0}) : nullptr) {
				placed->setTeam(player.team);
				Supply supply {placed->id(), player.id, player.team, ability, inHand->name()};
				// A hub that carries a SupplyObject works as one (the
				// supply crates' update, 0x005d1710; stepCrates).
				for (const world::ChildTemplate& child : hub->children()) {
					const world::ObjectTemplate* c = m_templates.find(child.name);
					if (c && sameName(c->typeName(), "SupplyObject")) {
						Crate crate;
						crate.object = placed->id();
						crate.team = player.team;
						crate.storage = number(*c, "sharedStorageSize", 0) + number(*c, "healStorageSize", 0)
							+ number(*c, "ammoStorageSize", 0);
						crate.timeLeft = 1e9f;
						crate.settings = c;
						m_crates.push_back(std::move(crate));
						supply.crate = true;
						break;
					}
				}
				m_supplies.push_back(std::move(supply));
			}
		} else if (world::ObjectTemplate* beacon = beaconTemplate(inHand)) {
			// A squad leader's spawn beacon (armor.holdsSlBeacon): set down a
			// metre ahead, one per squad.
			if (player.squadLeader) {
				if (const auto old = m_beacons.find({player.team, player.squad}); old != m_beacons.end()) {
					m_objects.remove(old->second);
				}
				const float yaw = lookYaw(player) * 3.14159265f / 180.0f;
				physics::Vec3 at = player.body.position + physics::Vec3 {std::sin(yaw), 0, std::cos(yaw)};
				at.y = m_ground.height(at.x, at.z);
				if (world::Object* placed = m_objects.create(beacon, {at.x, at.y, at.z}, {lookYaw(player), 0, 0})) {
					placed->setTeam(player.team);
					m_beacons[{player.team, player.squad}] = placed->id();
				}
			}
		} else if (world::ObjectTemplate* drone = droneTemplate(inHand)) {
			// A squad leader's drone: launched to hover by its owner.
			if (player.squadLeader) {
				launchDrone(player, *inHand, *drone);
			}
		} else if (world::ObjectTemplate* deployable = deployableTemplate(inHand)) {
			// A deployable (the portable shield): set down a metre ahead,
			// one per owner and gadget; it stands until shot to pieces.
			const auto key = std::make_pair(player.id, std::string(inHand->name()));
			if (const auto old = m_deployed.find(key); old != m_deployed.end()) {
				m_objects.remove(old->second);
			}
			const float yaw = lookYaw(player) * 3.14159265f / 180.0f;
			physics::Vec3 at = player.body.position + physics::Vec3 {std::sin(yaw), 0, std::cos(yaw)};
			at.y = m_ground.height(at.x, at.z);
			if (world::Object* placed = m_objects.create(deployable, {at.x, at.y, at.z}, {lookYaw(player), 0, 0})) {
				placed->setTeam(player.team);
				m_deployed[key] = placed->id();
				m_launchers[placed->id()] = player.object;
			}
		} else if (!gadget && player.projectile) {
			fire(player, std::clamp(input.aimPitch + player.aimKickPitch, -89.0f, 89.0f));
		}
		// The shot's recoil forces (0x005dd3f0; zoomModifier is the
		// viewer's: the world keeps no zoom).
		if (player.recoil.hasRecoilForce) {
			player.recoilForceUp = player.recoilUp.sample(m_random);
			player.recoilForceLeftRight = player.recoilLeftRight.sample(m_random);
			player.lastShotTick = static_cast<int64_t>(m_ticks);
		}
		player.fired = true;
		++player.roundsFired;
	}
	stepAnimation(player, seconds);
	const physics::Vec3& p = player.body.position;
	object->setPosition({p.x, p.y - 1.0f, p.z});
	object->setRotation({player.body.yawDegrees, 0, 0});
	followSoldier(player, *object);
}

float GameWorld::lookYaw(const Player& player) const { return player.body.yawDegrees + player.control.twist(); }

void GameWorld::followSoldier(const Player& player, const world::Object& soldier)
{
	if (world::Object* held = player.weaponObject ? m_objects.find(player.weaponObject) : nullptr) {
		held->setPosition(soldier.position());
		held->setRotation(soldier.rotation());
		held->setHidden(soldier.hidden());
		// The muzzle flash still burning follows the gun.
		physics::Vec3 at;
		physics::Mat3 rotation;
		if (player.muzzleUntil > m_time && weaponPlace(player, at, rotation)) {
			m_effects.setAnchor(player.weaponObject, at, rotation);
		}
	}
	if (world::Object* kit = player.kitObject ? m_objects.find(player.kitObject) : nullptr) {
		kit->setPosition(soldier.position());
	}
}

GameWorld::Body* GameWorld::findBody(uint32_t object)
{
	for (Body& body : m_bodies) {
		if (body.object == object) {
			return &body;
		}
	}
	return nullptr;
}

const Vehicle* GameWorld::vehicle(uint32_t object) const
{
	for (const Body& body : m_bodies) {
		if (body.object == object) {
			return body.vehicle.get();
		}
	}
	return nullptr;
}

float GameWorld::soldierFriction() const { return m_materials ? m_materials->friction(0, 0) : 1.0f; }

const SoldierControl* GameWorld::soldierControl(int id) const
{
	const Player* player = findPlayer(id);
	return player && player->alive ? &player->control : nullptr;
}

const physics::SoldierBody* GameWorld::soldierBody(int id) const
{
	const Player* player = findPlayer(id);
	return player && player->alive ? &player->body : nullptr;
}

bool GameWorld::partPose(uint32_t object, const std::string& part, world::Vec3& offset, world::Vec3& rotation) const
{
	const Vehicle* state = vehicle(object);
	if (!state && !isTitan(object)) {
		return false;
	}
	// A gunner's bundles: the yaw one turns, the pitch one tilts (down positive).
	for (const Body& body : m_bodies) {
		if (body.object != object || body.guns.empty()) {
			continue;
		}
		const auto known = m_layouts.find(m_objects.find(object)->objectTemplate()->name());
		// A seat's camera turned by the mouse, about its pivot.
		for (size_t i = 0; known != m_layouts.end() && i < known->second.seats.size() && i < body.looks.size(); ++i) {
			const SeatLayout& seat = known->second.seats[i];
			if (!seat.camera.empty() && sameName(seat.camera, part)) {
				const Body::Look& look = body.looks[i];
				const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({look.yaw, look.pitch, 0});
				const physics::Vec3 o = seat.cameraPivot - turn.transform(seat.cameraPivot);
				offset = {o.x, o.y, o.z};
				rotation = {look.yaw, look.pitch, 0};
				return true;
			}
		}
		for (size_t i = 0; known != m_layouts.end() && i < known->second.seats.size() && i < body.guns.size(); ++i) {
			const SeatLayout& seat = known->second.seats[i];
			if (!seat.yawBundle.empty() && sameName(seat.yawBundle, part)) {
				offset = {};
				rotation = {body.guns[i].yaw, 0, 0};
				return true;
			}
			if (!seat.pitchBundle.empty() && sameName(seat.pitchBundle, part)) {
				offset = {};
				rotation = {0, -body.guns[i].pitch, 0};
				return true;
			}
		}
	}
	if (!state) {
		return false;
	}
	for (const Vehicle::Wheel& wheel : state->wheels()) {
		if (wheel.spring == part) {
			offset = {0, wheel.compression, 0};
			rotation = {0, wheel.angle * 57.29578f, 0};
			return true;
		}
		if (!wheel.steering.empty() && wheel.steering == part) {
			offset = {};
			rotation = {wheel.steer, 0, 0};
			return true;
		}
	}
	// The moving parts (RotationalBundle, Engine, Wing: rotors, thrusters,
	// flaps, turrets) turn by their axes' angles (degrees) in their own
	// frame, as Vehicle::updateParts composes them (turn * base * parent).
	for (const Vehicle::Part& p : state->parts()) {
		if (p.moves && sameName(p.name, part)) {
			offset = {};
			rotation = {p.angle[0], p.angle[1], p.angle[2]};
			return true;
		}
	}
	return false;
}

std::vector<GameWorld::RoundCount> GameWorld::playerRounds(int id, bool hits, bool clear)
{
	Player* player = findPlayer(id);
	if (!player || player->weaponTemplate.empty()) {
		return {};
	}
	int& count = hits ? player->roundsHit : player->roundsFired;
	std::vector<RoundCount> counts {{player->weaponTemplate, count}};
	if (clear) {
		count = 0;
	}
	return counts;
}

world::Object* GameWorld::playerVehicle() const
{
	return m_objects.find(playerVehicleObject());
}

const Vehicle* GameWorld::playerVehicleState() const
{
	const uint32_t object = playerVehicleObject();
	return object ? vehicle(object) : nullptr;
}


const GameWorld::VehicleLayout& GameWorld::vehicleLayout(const world::ObjectTemplate& vehicle)
{
	const auto known = m_layouts.find(vehicle.name());
	if (known != m_layouts.end()) {
		return known->second;
	}
	VehicleLayout& layout = m_layouts[vehicle.name()];
	// seatInformation is optional: a vehicle that names no seat (eu_tank,
	// as_tank, the walkers, the APCs, as_ag) has a seat at each
	// PlayerControlObject of its tree, at its place; one that names any (the
	// Titans' gun hubs, the transports) has those alone.
	const std::function<bool(const world::ObjectTemplate&, int)> namesSeats = [&](const world::ObjectTemplate& t, int depth) {
		if (depth > 12) {
			return false;
		}
		if (t.typeName() == "PlayerControlObject" && t.settings().count("seatInformation")) {
			return true;
		}
		for (const world::ChildTemplate& child : t.children()) {
			const world::ObjectTemplate* c = m_templates.find(child.name);
			if (c && namesSeats(*c, depth + 1)) {
				return true;
			}
		}
		return false;
	};
	const bool named = namesSeats(vehicle, 0);
	// Depth first from the root; `seat` is the seat whose subtree this is.
	// Each child sits at its offset turned by its parents' rotations, and
	// adds its own rotation (as Vehicle::fromTemplate places the wheels).
	const std::function<void(const world::ObjectTemplate&, physics::Vec3, physics::Mat3, int, int)> walk =
		[&](const world::ObjectTemplate& t, physics::Vec3 at, physics::Mat3 turn, int seat, int depth) {
			if (depth > 12) {
				return;
			}
			const auto& settings = t.settings();
			const auto info = settings.find("seatInformation");
			if (t.typeName() == "PlayerControlObject" && (info != settings.end() || !named)) {
				SeatLayout added;
				added.name = t.name();
				if (info != settings.end()) {
					char name[128] = {};
					char position[64] = {};
					char rotation[64] = {};
					std::sscanf(info->second.c_str(), "%127s %63s %63s", name, position, rotation);
					added.position = at + turn.transform(parseVec3(position));
					added.rotation = parseVec3(rotation);
				} else {
					added.position = at;
					added.rotation = turn.yawPitchRoll();
				}
				const auto animation = settings.find("seatAnimationSystem");
				if (animation != settings.end()) {
					added.animationSystem = animation->second;
				}
				const auto exit = settings.find("soldierExitLocation");
				if (exit != settings.end()) {
					added.exit = at + turn.transform(parseVec3(exit->second));
					added.hasExit = true;
				}
				layout.seats.push_back(added);
				seat = static_cast<int>(layout.seats.size()) - 1;
			}
			if (seat >= 0) {
				SeatLayout& owner = layout.seats[static_cast<size_t>(seat)];
				const auto look = [&](const char* key, const char* input) {
					const auto it = settings.find(key);
					return it != settings.end() && sameName(it->second, input);
				};
				if (t.typeName() == "Camera" && owner.camera.empty()
					&& (look("inputToYaw", "PIMouseLookX") || look("inputToPitch", "PIMouseLookY"))) {
					// 0x00563bd0's axes (Vehicle.h's RotationAxis), x for yaw, y for pitch.
					const auto vec = [&](const char* key) {
						const auto it = settings.find(key);
						return it != settings.end() ? parseVec3(it->second) : physics::Vec3 {};
					};
					const auto axis = [&](int i, bool on) {
						RotationAxis a;
						if (!on) {
							return a;
						}
						const auto pick = [i](const physics::Vec3& v) { return i == 0 ? v.x : v.y; };
						a.minRotation = pick(vec("minRotation"));
						a.maxRotation = pick(vec("maxRotation"));
						a.maxSpeed = pick(vec("maxSpeed"));
						a.acceleration = pick(vec("acceleration"));
						a.deAcceleration = pick(vec("deAcceleration"));
						a.continuousSpeed = pick(vec("continousRotationSpeed"));
						a.automaticReset = number(t, "automaticReset", 0) != 0;
						a.snapToZero = number(t, "snapToZeroOnNoInput", 0) != 0;
						a.useDeAcceleration = number(t, "useDeAcceleration", 0) != 0;
						a.capMaxSpeed = number(t, "capMaxSpeed", 0) != 0;
						a.maxSpeedMod = number(t, "maxSpeedMod", 0);
						return a;
					};
					owner.camera = t.name();
					owner.cameraRegulateYaw = vec("regulateVector").x;
					owner.cameraYaw = axis(0, look("inputToYaw", "PIMouseLookX"));
					owner.cameraPitch = axis(1, look("inputToPitch", "PIMouseLookY"));
					owner.cameraPivot = vec("pivotPosition");
				}
				if (t.typeName() == "GenericFireArm" && owner.weapon.empty()) {
					owner.weapon = t.name();
					owner.weaponPosition = at;
				}
				if (t.typeName() == "RotationalBundle") {
					if (settings.count("inputToYaw") && owner.yawBundle.empty()) {
						owner.yawBundle = t.name();
						owner.frameYaw = turn.yawPitchRoll().x;
						if (const auto reg = settings.find("regulateVector"); reg != settings.end()) {
							owner.regulateYaw = parseVec3(reg->second).x;
						}
						if (const auto speed = settings.find("maxSpeed"); speed != settings.end()) {
							owner.yawSpeed = parseVec3(speed->second).x;
						}
						const auto low = settings.find("minRotation");
						const auto high = settings.find("maxRotation");
						if (low != settings.end() && high != settings.end()
							&& parseVec3(low->second).x < parseVec3(high->second).x) {
							owner.minYaw = parseVec3(low->second).x;
							owner.maxYaw = parseVec3(high->second).x;
						}
					}
					if (settings.count("inputToPitch") && owner.pitchBundle.empty()) {
						owner.pitchBundle = t.name();
						const auto low = settings.find("minRotation");
						const auto high = settings.find("maxRotation");
						if (low != settings.end()) {
							owner.minPitch = parseVec3(low->second).y;
						}
						if (high != settings.end()) {
							owner.maxPitch = parseVec3(high->second).y;
						}
					}
				}
			}
			for (const world::ChildTemplate& child : t.children()) {
				const world::ObjectTemplate* c = m_templates.find(child.name);
				if (!c) {
					continue;
				}
				const physics::Vec3 childAt = at + turn.transform({child.position.x, child.position.y, child.position.z});
				const physics::Mat3 childTurn =
					physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z}) * turn;
				// The vehicle's EntryPoints, and a seat's own (the Titans' gun
				// hubs: EntryPoint children of each seat's PlayerControlObject,
				// setEntryRadius 5, at its console), which lead to that seat.
				if (c->typeName() == "EntryPoint" && (depth == 0 || (seat >= 0 && t.typeName() == "PlayerControlObject"))) {
					const float radius = number(*c, "entryRadius", number(*c, "setEntryRadius", 3));
					layout.entries.push_back({childAt, radius, depth == 0 ? -1 : seat});
					continue;
				}
				walk(*c, childAt, childTurn, seat, depth + 1);
			}
		};
	walk(vehicle, {}, physics::Mat3 {}, -1, 0);
	return layout;
}

std::vector<GameWorld::SeatLayout> GameWorld::vehicleSeats(uint32_t object)
{
	const world::Object* found = m_objects.find(object);
	if (!found || !found->objectTemplate()) {
		return {};
	}
	return vehicleLayout(*found->objectTemplate()).seats;
}

std::vector<int> GameWorld::vehicleOccupants(uint32_t object) const
{
	for (const Body& body : m_bodies) {
		if (body.object == object) {
			return body.occupants;
		}
	}
	return {};
}

bool GameWorld::gunAim(uint32_t vehicle, int seat, float& yaw, float& pitch) const
{
	for (const Body& body : m_bodies) {
		if (body.object == vehicle && seat >= 0 && static_cast<size_t>(seat) < body.guns.size()
			&& body.guns[static_cast<size_t>(seat)].weapon) {
			const auto known = m_layouts.find(m_objects.find(vehicle)->objectTemplate()->name());
			const float frame = known != m_layouts.end() && static_cast<size_t>(seat) < known->second.seats.size()
				? known->second.seats[static_cast<size_t>(seat)].frameYaw
				: 0.0f;
			yaw = std::remainder(frame + body.guns[static_cast<size_t>(seat)].yaw, 360.0f);
			pitch = body.guns[static_cast<size_t>(seat)].pitch;
			return true;
		}
	}
	return false;
}

bool GameWorld::drivesSeat(uint32_t vehicle, int seat) const { return seat == 0 && this->vehicle(vehicle) != nullptr; }

bool GameWorld::abilityOf(const world::ObjectTemplate& gadget, Ability& ability, bool& whileFiring)
{
	const auto type = gadget.settings().find("ammo.replenishingType");
	if (type == gadget.settings().end()) {
		return false;
	}
	ability.ammo = type->second == "RTAmmo";
	ability.radius = number(gadget, "ammo.abilityRadius", 0);
	ability.strength = number(gadget, "ammo.abilityStrength", 0);
	ability.material = static_cast<uint32_t>(integer(gadget, "ammo.abilityMaterial", 0));
	ability.drain = number(gadget, "ammo.abilityDrain", 0);
	whileFiring = integer(gadget, "ammo.onlyActiveWhileFiring", 0) != 0;
	return true;
}

std::vector<uint32_t> GameWorld::supplyObjects() const
{
	std::vector<uint32_t> objects;
	for (const Supply& supply : m_supplies) {
		objects.push_back(supply.object);
	}
	return objects;
}

// A ReplenishingAmmoComp's work (the object's update 0x005d8ac0): for
// each target, strength / 30 a tick (the template keeps abilityStrength x
// 0.033333, setter 0x005d7b00) times the material table's cell of the
// ability's material and the target's armour material, times the
// holder's increasedRepairAbility when above 0 (0x005d7a40: player slot
// 0x2a0 -> +0x30c, +0x48):
// - RTHeal (gameLogic slot 0x108 -> 0x004cc380 -> armour slot 0x48
//   0x00655a00): that many per cent of the target's maximum hit points,
//   for one of the team that can be repaired, alive and not full;
// - RTAmmo (slot 0x10c -> 0x004cf7b0): min(100, it) per cent of a
//   magazine for each of the target's weapons (0x005f5110: magSize x
//   per cent / 100 rounds, the fraction kept).
// Only the holder's team or team 0 (0x005d7d40). The targets come from a
// sphere of abilityRadius round the holder (0x005d8740), rescanned every
// second with none and every two seconds with some, those held treated
// in between even out of reach.
// The battery: nothing is treated unless the holder's battery holds
// abilityDrain / 30 (template +0x6c, setter 0x005d7ae0; read through
// 0x005d8160 -> 0x005d7fe0 -> soldier component 0xc4d7 slot 0x90), and
// each target treated takes that much (0x005d81b0 -> slot 0x98 0x00586310,
// only when it is there). The battery is the soldier's whose player owns
// the object: a hub's owner off foot or dead has none (0).
// RTTeam (slot 0x110) is used by no ammo in the game's data: not here.
void GameWorld::applyAbility(const Ability& ability, const physics::Vec3& at, int team, float seconds, uint32_t skip,
	AbilityScan* scan, Player* holder)
{
	const float drain = ability.drain * seconds;
	float noBattery = 0;
	float& battery = holder && holder->alive && !holder->vehicle && holder->battery >= 0 ? holder->battery : noBattery;
	const float modifier = holder && holder->abilityModifier > 0 ? holder->abilityModifier : 1.0f;
	const auto cellOf = [&](uint32_t object) {
		const world::Object* target = m_objects.find(object);
		const world::ObjectTemplate* t = target ? target->objectTemplate() : nullptr;
		const uint32_t material = t ? static_cast<uint32_t>(integer(*t, "armor.defaultMaterial", 0)) : 0;
		return m_materials ? m_materials->damageMod(ability.material, material) : 1.0f;
	};
	const auto amountFor = [&](uint32_t object) { return modifier * ability.strength * seconds * cellOf(object); };
	const auto heal = [&](uint32_t object, float percent) {
		const world::Object* target = m_objects.find(object);
		const world::ObjectTemplate* t = target ? target->objectTemplate() : nullptr;
		const float left = hitPoints(object);
		if (!t || left < 0 || percent <= 0) {
			return false;
		}
		const float most = number(*t, "armor.maxHitPoints", left);
		if (left >= most) {
			return false;
		}
		m_hitPoints[object] = std::min(left + percent * 0.01f * most, most);
		return true;
	};
	// Each target treated draws on the battery, when it holds that much.
	const auto draw = [&] {
		if (battery >= drain) {
			battery -= drain;
		}
	};
	const auto inReach = [&](const physics::Vec3& p) { return physics::lengthSquared(p - at) <= ability.radius * ability.radius; };
	// The targets: held ones between rescans.
	std::vector<uint32_t> found;
	const bool rescan = !scan || scan->timer >= (scan->targets.empty() ? 1.0f : 2.0f);
	if (rescan) {
		for (const auto& owned : m_players) {
			const Player& target = *owned;
			if (target.alive && !target.vehicle && target.team == team && target.object != skip
				&& inReach(target.body.position - physics::Vec3 {0, 1.0f, 0})) {
				found.push_back(target.object);
			}
		}
		if (!ability.ammo) {
			for (const Body& body : m_bodies) {
				const world::Object* object = m_objects.find(body.object);
				if (object && (object->team() == team || object->team() == 0) && inReach(body.body.position)) {
					found.push_back(body.object);
				}
			}
			// The commander's assets are mended as well (armor.canBeRepairedWhenWreck).
			for (const CommanderAsset& asset : m_assets) {
				const world::Object* object = m_objects.find(asset.object);
				if (object && !findBody(asset.object) && (object->team() == team || object->team() == 0)
					&& inReach({object->position().x, object->position().y, object->position().z})) {
					found.push_back(asset.object);
				}
			}
		}
		if (scan) {
			scan->targets = found;
			scan->timer = 0;
		}
	} else {
		found = scan->targets;
	}
	if (scan) {
		scan->timer += seconds;
	}
	if (drain > battery) {
		return; // flat: nothing treated
	}
	for (const uint32_t object : found) {
		if (!ability.ammo) {
			if (heal(object, amountFor(object))) {
				draw();
			}
			continue;
		}
		Player* target = soldierPlayer(object);
		if (!target || !target->alive) {
			continue;
		}
		const float percent = std::min(100.0f, amountFor(object));
		const auto refill = [&](FireArm* weapon) {
			if (!weapon) {
				return false;
			}
			if (weapon->magsLeft() >= weapon->settings().nrOfMags || weapon->settings().magSize < 0) {
				m_resupply[object] = 0;
				return false; // full: nothing builds up
			}
			float& owed = m_resupply[object];
			owed += percent / 100.0f;
			while (owed >= 1) {
				owed -= 1;
				if (!weapon->addMagazine()) {
					owed = 0;
				}
			}
			return true;
		};
		if (refill(target->weapon.get())) {
			draw();
		}
	}
}

void GameWorld::stepAbilities(float seconds)
{
	m_supplies.erase(std::remove_if(m_supplies.begin(), m_supplies.end(),
						 [this](const Supply& supply) {
							 const world::Object* object = m_objects.find(supply.object);
							 return !object || object->destroyed();
						 }),
		m_supplies.end());
	for (const Supply& supply : m_supplies) {
		if (supply.crate) {
			continue; // a SupplyObject: stepCrates
		}
		const world::Vec3& p = m_objects.find(supply.object)->position();
		applyAbility(supply.ability, {p.x, p.y, p.z}, supply.team, seconds, 0, nullptr, findPlayer(supply.owner));
	}
	for (const auto& owned : m_players) {
		Player& player = *owned;
		// The battery fills by the kit's abilityRestoreRate / 30 a tick
		// (kit +0x31c, setter 0x0056cd90) up to 1, in the soldier's
		// update (0x0058d350).
		if (player.alive && player.battery >= 0) {
			const world::Object* worn = player.kitObject ? m_objects.find(player.kitObject) : nullptr;
			const world::ObjectTemplate* kit = worn && worn->objectTemplate() ? worn->objectTemplate()
				: m_templates.find(player.kitTemplate.empty() ? (player.team == 1 ? "Pac_Assault" : "EU_Assault")
																: player.kitTemplate);
			const float rate = kit ? number(*kit, "abilityRestoreRate", 0) : 0;
			player.battery = std::min(1.0f, player.battery + rate * seconds);
		}
		if (!player.alive || !player.abilityActive) {
			m_abilityScans.erase(player.id);
			continue;
		}
		Ability ability;
		bool whileFiring = false;
		if (const world::ObjectTemplate* t = m_templates.find(player.weaponTemplate); t && abilityOf(*t, ability, whileFiring)) {
			// Round the point a metre ahead of the soldier's hands.
			const float yaw = lookYaw(player) * 3.14159265f / 180.0f;
			const physics::Vec3 at = player.body.position + physics::Vec3 {std::sin(yaw), -0.5f, std::cos(yaw)} * 1.0f;
			auto scan = m_abilityScans.try_emplace(player.id).first;
			if (scan->second.targets.empty() && scan->second.timer == 0) {
				scan->second.timer = 1; // the first scan at once
			}
			applyAbility(ability, at, player.team, seconds, 0, &scan->second, &player);
		}
	}
}

const FireArm* GameWorld::gun(uint32_t vehicle, int seat) const
{
	for (const Body& body : m_bodies) {
		if (body.object == vehicle && seat >= 0 && static_cast<size_t>(seat) < body.guns.size()) {
			return body.guns[static_cast<size_t>(seat)].weapon.get();
		}
	}
	return nullptr;
}

// Takes a seat (or leaves the current one for it): the seat's gun is made
// on first use from its GenericFireArm.
void GameWorld::seatPlayer(Player& player, Body& body, int seat)
{
	const world::Object* object = m_objects.find(body.object);
	const VehicleLayout& layout = vehicleLayout(*object->objectTemplate());
	body.occupants.resize(layout.seats.size(), -1);
	body.guns.resize(layout.seats.size());
	body.looks.resize(layout.seats.size());
	body.looks[static_cast<size_t>(seat)] = {}; // restoreRotationOnExit: a new sitter looks ahead
	player.lastLookPitch = player.input.aimPitch;
	if (player.vehicle == body.object && player.seat >= 0 && static_cast<size_t>(player.seat) < body.occupants.size()) {
		body.occupants[static_cast<size_t>(player.seat)] = -1;
	}
	player.vehicle = body.object;
	player.seat = seat;
	body.occupants[static_cast<size_t>(seat)] = player.id;
	Body::Gun& gun = body.guns[static_cast<size_t>(seat)];
	const SeatLayout& place = layout.seats[static_cast<size_t>(seat)];
	if (!gun.weapon && !place.weapon.empty()) {
		if (const world::ObjectTemplate* t = m_templates.find(place.weapon)) {
			const FireArmSettings settings = FireArmSettings::fromTemplate(*t);
			auto& projectile = m_projectileSettings[settings.projectileTemplate];
			if (!projectile) {
				if (const world::ObjectTemplate* p = m_templates.find(settings.projectileTemplate)) {
					projectile = std::make_unique<ProjectileSettings>(ProjectileSettings::fromTemplate(*p));
				}
			}
			gun.projectile = projectile.get();
			gun.weapon = std::make_unique<FireArm>(settings);
		}
	}
}

// Entering: inside an EntryPoint's radius (entryRadius, 3 m) of a vehicle
// with a free seat; the soldier takes the first free one (the driver's
// first). Leaving: to the seat's setSoldierExitLocation, else the
// vehicle's altSoldierExitPosition, else 2.5 m to the left.
void GameWorld::enterOrLeaveVehicle(Player& player, world::Object& soldier)
{
	if (std::any_of(m_pods.begin(), m_pods.end(), [&](const Pod& pod) { return pod.player == player.id; })) {
		return; // a drop pod's rider stays in until it lands (dontAllowExit)
	}
	if (!player.vehicle && launchPod(player.id)) {
		return; // at a Titan's pod array (Titans.cpp)
	}
	if (player.vehicle) {
		Body* body = findBody(player.vehicle);
		const int seat = player.seat;
		player.vehicle = 0;
		player.seat = -1;
		soldier.setHidden(false);
		player.seatAnimation.reset();
		player.seatAnimationPath.clear();
		if (player.skeleton) {
			player.pose = anim::setupPose(*player.skeleton);
		}
		if (!body) {
			return;
		}
		if (seat >= 0 && static_cast<size_t>(seat) < body->occupants.size()) {
			body->occupants[static_cast<size_t>(seat)] = -1;
		}
		physics::Vec3 exit {-2.5f, 0, 0};
		const world::Object* vehicleObject = m_objects.find(body->object);
		if (const world::ObjectTemplate* t = vehicleObject ? vehicleObject->objectTemplate() : nullptr) {
			const VehicleLayout& layout = vehicleLayout(*t);
			if (seat >= 0 && static_cast<size_t>(seat) < layout.seats.size() && layout.seats[static_cast<size_t>(seat)].hasExit) {
				exit = layout.seats[static_cast<size_t>(seat)].exit;
			} else if (const auto it = t->settings().find("altSoldierExitPosition"); it != t->settings().end()) {
				exit = parseVec3(it->second);
			}
		}
		physics::Vec3 at = body->body.position + body->body.rotation.transform(exit);
		at.y = std::max(at.y, m_ground.height(at.x, at.z)) + 1.0f;
		player.body.position = at;
		player.body.velocity = {};
		player.body.yawDegrees = body->body.rotation.yawPitchRoll().x;
		player.control.reset();
		if (seat == 0 && body->vehicle) {
			body->vehicle->setInput({});
		}
		soldier.setPosition({at.x, at.y - 1.0f, at.z});
		raise("ExitVehicle", {ScriptValue::player(player.id), ScriptValue::object(body->object)});
		return;
	}
	Body* nearest = nullptr;
	int freeSeat = -1;
	float best = 1e30f;
	const auto freeAt = [](const Body& body, size_t i) { return i >= body.occupants.size() || body.occupants[i] < 0; };
	for (Body& body : m_bodies) {
		// A vehicle, or a Titan (its gun seats, Titans.cpp).
		const Titan* titan = body.vehicle ? nullptr : titanByObject(body.object);
		const world::Object* object = body.vehicle || (titan && !titan->wrecked) ? m_objects.find(body.object) : nullptr;
		if (!object || !object->objectTemplate()) {
			continue;
		}
		const VehicleLayout& layout = vehicleLayout(*object->objectTemplate());
		int seat = -1;
		for (size_t i = 0; i < layout.seats.size() && seat < 0; ++i) {
			if (freeAt(body, i)) {
				seat = static_cast<int>(i);
			}
		}
		if (seat < 0) {
			continue;
		}
		// No entry points (unusual): within 4 m of the centre.
		const std::vector<VehicleLayout::Entry> fallback {{{0, 0, 0}, 4}};
		for (const VehicleLayout::Entry& entry : layout.entries.empty() ? fallback : layout.entries) {
			// A seat's own entry leads to that seat only; a Titan has no other.
			if ((entry.seat >= 0 && !freeAt(body, static_cast<size_t>(entry.seat))) || (titan && entry.seat < 0)) {
				continue;
			}
			const physics::Vec3 at = body.body.position + body.body.rotation.transform(entry.position);
			const float distance = physics::lengthSquared(at - player.body.position);
			if (distance <= entry.radius * entry.radius && distance < best) {
				best = distance;
				nearest = &body;
				freeSeat = entry.seat >= 0 ? entry.seat : seat;
			}
		}
	}
	if (nearest) {
		seatPlayer(player, *nearest, freeSeat);
		soldier.setHidden(true);
		raise("EnterVehicle", {ScriptValue::player(player.id), ScriptValue::object(nearest->object)});
	} else if (m_time - player.kitTimeStamp > 2.0) {
		// No vehicle takes him: the closest kit on the ground within 3 m
		// (0x004bda00 -> pickUpClosestKit, Kits.cpp).
		pickUpClosestKit(player, 3.0f);
	}
}

// The seat's camera on the mouse (a Camera bundle with inputToYaw
// PIMouseLookX / inputToPitch PIMouseLookY; the axis update 0x00563bd0,
// rotationAxisStep): PIMouseLookX is the turn this tick over 5 degrees,
// PIMouseLookY the pitch's change over 5 (down positive), as the mouse's
// look units are (net::kDegreesPerMouseLookUnit). The drivers' cameras
// (eu_fav_driver_camera: -125..120 / -20..40 at 90 a second) turn and
// stay; the FAV gunners' (eu_fav_MachineGun_Camera: +-1 / +-0.5,
// automaticReset) lean with the mouse and settle back. PlayerCamera poses
// them through partPose.
void GameWorld::stepSeatCamera(Player& player, Body& body, float seconds)
{
	const float pitchChange = player.input.aimPitch - player.lastLookPitch;
	player.lastLookPitch = player.input.aimPitch;
	const world::Object* object = m_objects.find(body.object);
	if (!object || player.seat < 0 || static_cast<size_t>(player.seat) >= body.looks.size()) {
		return;
	}
	const VehicleLayout& layout = vehicleLayout(*object->objectTemplate());
	if (static_cast<size_t>(player.seat) >= layout.seats.size()) {
		return;
	}
	const SeatLayout& seat = layout.seats[static_cast<size_t>(player.seat)];
	if (seat.camera.empty()) {
		return;
	}
	// An axis input is -1..1 (what goes past is lost: only
	// rememberExcessInput, not rewritten, banks it).
	Body::Look& look = body.looks[static_cast<size_t>(player.seat)];
	float yawIn = std::clamp(player.input.turn / 5.0f, -1.0f, 1.0f);
	yawIn = regulatedInput(yawIn, regulation(seat.cameraRegulateYaw, body.body));
	rotationAxisStep(seat.cameraYaw, yawIn, seconds, look.yaw, look.yawSpeed);
	rotationAxisStep(seat.cameraPitch, std::clamp(-pitchChange / 5.0f, -1.0f, 1.0f), seconds, look.pitch, look.pitchSpeed);
}

// A bundle's regulation of its yaw (0x005644b0, regulateFromParent -1 as
// every shipped bundle has it): the root node's angular velocity (slot
// 0x68, rad/s) about the yaw axis (the hull's up) times regulateVector.x,
// within +-1. The original carries the angular velocity through the
// bundle's and the root's matrices first (0x006acfa0, 0x004b9d60); for a
// bundle upright in its hull that is the up component.
float GameWorld::regulation(float regulate, const physics::RigidBody& body)
{
	if (regulate == 0) {
		return 0;
	}
	return std::clamp(regulate * physics::dot(body.angularVelocity, body.rotation.rows[1]), -1.0f, 1.0f);
}

// The handler's merge (0x00565180): the regulated value replaces an input
// that is 0, or of the same sign and smaller.
float GameWorld::regulatedInput(float input, float regulated)
{
	if (regulated == 0) {
		return input;
	}
	const auto sign = [](float x) { return x > 0 ? 1 : x < 0 ? -1 : 0; };
	if (input == 0 || (sign(input) == sign(regulated) && std::fabs(input) < std::fabs(regulated))) {
		return regulated;
	}
	return input;
}

// A gunner turns the seat's bundles with the mouse (yaw free, pitch within
// the bundle's limits) and fires the seat's gun along them.
void GameWorld::stepGunner(Player& player, Body& body, float seconds, bool aims)
{
	if (player.seat < 0 || static_cast<size_t>(player.seat) >= body.guns.size()) {
		return;
	}
	Body::Gun& gun = body.guns[static_cast<size_t>(player.seat)];
	const world::Object* object = m_objects.find(body.object);
	const SeatLayout& seat = vehicleLayout(*object->objectTemplate()).seats[static_cast<size_t>(player.seat)];
	// regulateVector (0x005644b0 through the input handler 0x00565180 or
	// the input pass 0x00565700): the bundle's yaw input becomes
	// clamp(regulateVector.x x the hull's yaw rate, -1, 1) where the
	// gunner's is 0 or weaker the same way, so a turret holds its heading
	// while the hull turns (eu_tank's -0.5 at 90 degrees a second). The
	// rewrite's gunner turns by degrees a tick: the regulation's are the
	// input x maxSpeed x the tick.
	if (aims) {
		float turn = player.input.turn;
		if (seat.regulateYaw != 0 && seat.yawSpeed != 0) {
			const float reg = regulation(seat.regulateYaw, body.body) * seat.yawSpeed * seconds;
			turn = regulatedInput(turn, reg);
		}
		gun.yaw = std::clamp(std::remainder(gun.yaw + turn, 360.0f), seat.minYaw, seat.maxYaw);
		// Bundle pitch is down positive: its limits [min, max] bound the aim at [-max, -min].
		gun.pitch = std::clamp(player.input.aimPitch, -seat.maxPitch, -seat.minPitch);
	}
	// A Titan's gun whose armour is out (disableInputOnDestroy) doesn't fire.
	const bool working = !isTitan(body.object) || titanGunWorks(body.object, player.seat);
	if (!gun.weapon || !gun.weapon->tick(seconds, player.input.fire && working) || !gun.projectile) {
		return;
	}
	const float yaw = (seat.frameYaw + gun.yaw) * 3.14159265f / 180.0f;
	const float pitch = gun.pitch * 3.14159265f / 180.0f;
	const physics::Vec3 local {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
	const physics::Vec3 direction = body.body.rotation.transform(local);
	Projectile p;
	p.origin = body.body.position + body.body.rotation.transform(seat.weaponPosition) + direction * 1.0f;
	p.position = p.origin;
	p.velocity = body.body.velocity + direction * gun.weapon->settings().velocity;
	p.settings = gun.projectile;
	p.timeLeft = gun.projectile->timeToLive.sample(m_random);
	p.shooter = body.object;
	p.player = player.id;
	m_projectiles.push_back(p);
	++player.roundsFired;
}

void GameWorld::enablePhysics(const io::FileManager& files, physics::CollisionTemplateManager& collisionTemplates,
	physics::Ground ground, const physics::MaterialManager* materials)
{
	m_materials = materials;
	m_files = &files;
	m_collisionTemplates = &collisionTemplates;
	m_ground = std::move(ground);
	if (m_hasWater && !m_ground.waterLevel) {
		m_ground.waterLevel = [this](float, float) { return m_waterLevel; };
	}
}

// The template setup that creates physics nodes (0x005f6ba0): mobile mesh
// physics gets a PhysicsNode.Basic with the template's mass, centre of mass,
// inertia and gravity modifiers.
namespace {

int engineTypeOf(const std::string& text)
{
	// The c_ET constants (symbols/BF2142.exe.constants.csv), or a number.
	static const std::pair<const char*, int> kTypes[] = {{"c_etcar", 2}, {"c_ethelicopter", 33}, {"c_etnewcar", 66},
		{"c_etnewcar2", 194}, {"c_etnewtank", 70}, {"c_etplane", 1}, {"c_etrocket", 17}, {"c_etship", 9},
		{"c_ettank", 6}, {"c_ettorpedo", 25}};
	std::string l = text;
	std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	for (const auto& [name, value] : kTypes) {
		if (l == name) {
			return value;
		}
	}
	return std::atoi(text.c_str());
}

// 0x006b6c50 -> 0x005c63b0: the first Engine (ObjectTemplate class 0x9476)
// of the tree, checking a node, then its next siblings, then its first
// child's chain: a template's children are all checked before any of
// theirs. Its engineType (template +0x6e8 through slot 0x1a8), or -1.
int firstEngineIn(const world::ObjectTemplateManager& templates, const std::vector<world::ChildTemplate>& list, size_t i,
	int depth)
{
	if (i >= list.size() || depth > 8) {
		return -1;
	}
	const world::ObjectTemplate* t = templates.find(list[i].name);
	if (t && t->typeName() == "Engine") {
		const auto type = t->settings().find("engineType");
		return type == t->settings().end() ? 0 : engineTypeOf(type->second);
	}
	if (const int found = firstEngineIn(templates, list, i + 1, depth); found >= 0) {
		return found;
	}
	return t ? firstEngineIn(templates, t->children(), 0, depth + 1) : -1;
}

// A collision mesh instance's radius (slot 0x74, 0x006dedb0 on the
// instance's geometry, +0xc): its first sub-geometry with a LOD, that
// one's LOD of the lowest collision type, its radius (+0xc0, the box's
// farthest corner). The instance's state (+0x14) is not read: the wreck's
// sub-geometry doesn't change it.
float meshRadius(const physics::CollisionMeshFile& mesh, size_t geometry = 0)
{
	if (geometry >= mesh.geometries.size()) {
		return 0;
	}
	for (const physics::CollisionSubGeometry& part : mesh.geometries[geometry].subGeometries) {
		for (uint32_t type = 0; type < part.lods.size(); ++type) {
			if (const physics::CollisionLod* lod = physics::collisionLodFor(mesh, part, type)) {
				return physics::boxRadius(lod->boundsMin, lod->boundsMax);
			}
		}
	}
	return 0;
}

// A collision mesh instance's box (slot 0x78, 0x006deeb0 on its
// geometry: every sub-geometry's LODs' boxes together); false without one.
bool meshBox(const physics::CollisionMeshFile& mesh, size_t geometry, physics::Vec3& low, physics::Vec3& high)
{
	bool found = false;
	if (geometry < mesh.geometries.size()) {
		for (const physics::CollisionSubGeometry& part : mesh.geometries[geometry].subGeometries) {
			for (const physics::CollisionLod& lod : part.lods) {
				const physics::Vec3 a {lod.boundsMin[0], lod.boundsMin[1], lod.boundsMin[2]};
				const physics::Vec3 b {lod.boundsMax[0], lod.boundsMax[1], lod.boundsMax[2]};
				low = found ? physics::Vec3 {std::min(low.x, a.x), std::min(low.y, a.y), std::min(low.z, a.z)} : a;
				high = found ? physics::Vec3 {std::max(high.x, b.x), std::max(high.y, b.y), std::max(high.z, b.z)} : b;
				found = true;
			}
		}
	}
	return found;
}

// The width across x of a collision mesh's box (slot 0x78, 0x006deeb0).
float meshWidth(const physics::CollisionMeshFile& mesh)
{
	physics::Vec3 low, high;
	return meshBox(mesh, 0, low, high) && high.x > low.x ? high.x - low.x : 0.0f;
}

// Whether a child of the tree is of `type` (0x005c63b0's search: a
// template's children before any of theirs).
bool childOfType(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t, const char* type, int depth)
{
	if (depth > 8) {
		return false;
	}
	for (const world::ChildTemplate& child : t.children()) {
		const world::ObjectTemplate* ct = templates.find(child.name);
		if (ct && ct->typeName() == type) {
			return true;
		}
	}
	for (const world::ChildTemplate& child : t.children()) {
		const world::ObjectTemplate* ct = templates.find(child.name);
		if (ct && childOfType(templates, *ct, type, depth + 1)) {
			return true;
		}
	}
	return false;
}

// The object's box at rest (slot 0x98(0), 0x006f7050): its mesh
// instance's (0x006deeb0) and each child's, by the child's offset and
// rotation (slot 0xe4); a child's instance is the root's mesh's geometry
// its collisionPart names (0x0063ccc0, the collision manager's slot 0x28).
bool objectBoxOf(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t,
	const physics::CollisionMeshFile& mesh, int geometry, physics::Vec3& low, physics::Vec3& high, int depth)
{
	bool found = geometry >= 0 && meshBox(mesh, static_cast<size_t>(geometry), low, high);
	if (depth > 8) {
		return found;
	}
	for (const world::ChildTemplate& child : t.children()) {
		const world::ObjectTemplate* ct = templates.find(child.name);
		if (!ct) {
			continue;
		}
		physics::Vec3 a, b;
		if (!objectBoxOf(templates, *ct, mesh, static_cast<int>(number(*ct, "collisionPart", -1)), a, b, depth + 1)) {
			continue;
		}
		const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
		const physics::Vec3 offset {child.position.x, child.position.y, child.position.z};
		for (int corner = 0; corner < 8; ++corner) {
			const physics::Vec3 c = turn.transform({corner & 1 ? b.x : a.x, corner & 2 ? b.y : a.y, corner & 4 ? b.z : a.z})
				+ offset;
			low = found ? physics::Vec3 {std::min(low.x, c.x), std::min(low.y, c.y), std::min(low.z, c.z)} : c;
			high = found ? physics::Vec3 {std::max(high.x, c.x), std::max(high.y, c.y), std::max(high.z, c.z)} : c;
			found = true;
		}
	}
	return found;
}

// An object's radius (slot 0x90, 0x006f6b90): its mesh instance's (above;
// slot 0x94 adds 0) or, when farther, a child's: the length of its offset
// (slot 0xdc) plus the child's own radius, for children with a radius. A
// child's instance is the root's mesh's geometry its collisionPart names
// (0x0063ccc0); the rewrite builds the tree from the templates.
float objectRadiusOf(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t,
	const physics::CollisionMeshFile& mesh, int geometry, int depth)
{
	float result = geometry >= 0 ? meshRadius(mesh, static_cast<size_t>(geometry)) : 0.0f;
	if (depth > 8) {
		return result;
	}
	for (const world::ChildTemplate& child : t.children()) {
		const world::ObjectTemplate* ct = templates.find(child.name);
		if (!ct) {
			continue;
		}
		const int part = static_cast<int>(number(*ct, "collisionPart", -1));
		const float own = objectRadiusOf(templates, *ct, mesh, part, depth + 1);
		if (own > 0) {
			const float offset = std::sqrt(child.position.x * child.position.x + child.position.y * child.position.y
				+ child.position.z * child.position.z);
			result = std::max(result, offset + own);
		}
	}
	return result;
}

} // namespace

int GameWorld::firstEngineType(const world::ObjectTemplate& root) const
{
	if (const auto found = m_engineTypes.find(&root); found != m_engineTypes.end()) {
		return found->second;
	}
	int type = -1;
	if (root.typeName() == "Engine") {
		const auto setting = root.settings().find("engineType");
		type = setting == root.settings().end() ? 0 : engineTypeOf(setting->second);
	} else {
		type = firstEngineIn(m_templates, root.children(), 0, 0);
	}
	m_engineTypes[&root] = type;
	return type;
}

void GameWorld::addBodies()
{
	for (const auto& owned : m_objects.objects()) {
		world::Object& object = *owned;
		const auto checked = std::lower_bound(m_checked.begin(), m_checked.end(), object.id());
		if (checked != m_checked.end() && *checked == object.id()) {
			continue;
		}
		m_checked.insert(checked, object.id());
		const world::ObjectTemplate* t = object.objectTemplate();
		if (!t || !flag(*t, "hasMobilePhysics", false)) {
			continue;
		}
		const auto type = t->settings().find("physicsType");
		if (type == t->settings().end() || (type->second != "Mesh" && type->second != "mesh" && type->second != "3")) {
			continue;
		}
		const auto meshName = t->settings().find("collisionMesh");
		const physics::CollisionMeshFile* mesh = meshName == t->settings().end()
			? nullptr
			: m_collisionTemplates->mesh(meshName->second, *m_files);
		if (!mesh || mesh->geometries.empty()) {
			continue;
		}
		// LOD 1 is the vehicles' (0 projectiles', 2 soldiers').
		Body body {};
		body.object = object.id();
		physics::Vec3 low {1e9f, 1e9f, 1e9f}, high {-1e9f, -1e9f, -1e9f};
		const std::vector<uint32_t> materials = materialMap(*t);
		bool typeFour = false;
		for (const auto& part : physics::collisionParts(*mesh, 0)) {
			typeFour = typeFour || physics::collisionLodFor(*mesh, part, 4) != nullptr;
			const physics::CollisionLod* found = physics::collisionLodFor(*mesh, part, 1);
			if (!found) {
				continue;
			}
			const physics::CollisionLod& lod = *found;
			const size_t base = body.points.size();
			for (size_t i = 0; i < lod.vertices.size(); ++i) {
				const uint16_t index = i < lod.vertexMaterials.size() ? lod.vertexMaterials[i] : 0;
				body.pointMaterials.push_back(index < materials.size() ? materials[index] : index);
			}
			for (const auto& v : lod.vertices) {
				body.points.push_back({v[0], v[1], v[2]});
			}
			// The edge list (0x006e1430): a face whose normal (degenerate:
			// up) has |y| <= cos 30 degrees (*0x00a85178) gives its edges
			// v0-v1, v1-v2, v2-v0.
			for (const physics::CollisionFace& face : lod.faces) {
				const auto& v = lod.vertices;
				if (face.vertices[0] >= v.size() || face.vertices[1] >= v.size() || face.vertices[2] >= v.size()) {
					continue;
				}
				const physics::Vec3 p0 {v[face.vertices[0]][0], v[face.vertices[0]][1], v[face.vertices[0]][2]};
				const physics::Vec3 a = physics::Vec3 {v[face.vertices[1]][0], v[face.vertices[1]][1], v[face.vertices[1]][2]} - p0;
				const physics::Vec3 b = physics::Vec3 {v[face.vertices[2]][0], v[face.vertices[2]][1], v[face.vertices[2]][2]} - p0;
				float ny = 1;
				if (physics::lengthSquared(a) > 1e-8f && physics::lengthSquared(b) > 1e-8f) {
					const physics::Vec3 n = physics::cross(a, b);
					const float l = std::sqrt(physics::lengthSquared(n));
					ny = l > 0 ? n.y / l : 0;
				}
				if (std::fabs(ny) > 0.866f) { // 0x3f5db22d
					continue;
				}
				for (int k = 0; k < 3; ++k) {
					body.edges.push_back({static_cast<uint16_t>(base + face.vertices[k]),
						static_cast<uint16_t>(base + face.vertices[(k + 1) % 3])});
				}
			}
			low = {std::min(low.x, lod.boundsMin[0]), std::min(low.y, lod.boundsMin[1]), std::min(low.z, lod.boundsMin[2])};
			high = {std::max(high.x, lod.boundsMax[0]), std::max(high.y, lod.boundsMax[1]), std::max(high.z, lod.boundsMax[2])};
		}
		if (body.points.empty()) {
			continue;
		}
		body.boxMin = low;
		body.boxMax = high;
		body.helicopterEngine = firstEngineType(*t) == 33;
		// For the mobile mesh physics (MeshCollision.cpp).
		body.radius = physics::boxRadius({low.x, low.y, low.z}, {high.x, high.y, high.z});
		body.objectRadius = objectRadiusOf(m_templates, *t, *mesh, 0, 0);
		// A template with MechLegs keeps its object's box bottom
		// (0x005cd4f0: the PlayerControlObject template's +0x4c4 from slot
		// 0x98(0)'s min y) for the spawn spot's probes (0x006f28e0).
		if (sameName(t->typeName(), "PlayerControlObject") && childOfType(m_templates, *t, "MechLegs", 0)) {
			physics::Vec3 low, high;
			if (objectBoxOf(m_templates, *t, *mesh, 0, low, high, 0)) {
				m_legsBottom[t] = low.y;
			}
		}
		body.boxWidth = meshWidth(*mesh);
		body.vehicleMesh = std::make_shared<physics::CollisionWorld>();
		body.vehicleMesh->addMesh(*mesh, physics::CollisionUser::Vehicle, {}, {}, object.id(), materials);
		body.mass = number(*t, "mass", 1); // the template's +0x94 (1: 0x005f... ctor)
		body.majorActor = flag(*t, "isMajorActor", false);
		body.controlObject = sameName(t->typeName(), "PlayerControlObject");
		body.particle = sameName(t->typeName(), "Particle");
		body.material = static_cast<uint32_t>(integer(*t, "material", 0));
		body.collisionGroups = static_cast<uint32_t>(integer(*t, "collisionGroups", 0));
		body.handleCollisionSpeed = number(*t, "handleCollisionSpeed", 1);
		body.drag = number(*t, "drag", 0);
		body.dragModifier = vectorOf(*t, "dragModifier", {1, 1, 1});
		body.floaterMod = number(*t, "floaterMod", 1);
		body.trunkRadius = std::max(0.0f, number(*t, "treeCollisionDiameter", 0) * 0.5f);
		body.trunkFromTypeFour = typeFour;
		if (typeFour || body.trunkRadius > 0) {
			body.trunk = std::const_pointer_cast<MeshShape>(meshShape(*mesh, materials))->trunk;
		}
		body.projectileMesh = std::make_shared<physics::CollisionWorld>();
		body.projectileMesh->addMesh(*mesh, physics::CollisionUser::Projectile, {}, {}, object.id(), materialMap(*t));
		body.soldierMesh = std::make_shared<physics::CollisionWorld>();
		body.soldierMesh->addMesh(*mesh, physics::CollisionUser::Soldier, {}, {}, object.id(), materialMap(*t));
		// Its launcher (0x005b48b0, the projectile interface's slot 17):
		// with noInitialCollisionWithLauncher (template +0x355) and a mesh
		// response, the response ignores the launcher's root for two ticks
		// on the server (0x006b8de0; *0x00a84284 = 2).
		if (const auto launcher = m_launchers.find(object.id()); launcher != m_launchers.end()) {
			body.launcher = launcher->second;
			m_launchers.erase(launcher);
			if (flag(*t, "noInitialCollisionWithLauncher", false)) {
				body.ignored = body.launcher;
				body.ignoreUntil = m_ticks + 2;
				body.ignoreSet = true;
			}
		}
		// Soldiers push a small projectile (0x006c3890: class 0x9539, the
		// soldier LOD's radius, slot 0x50(2) -> 0x006de7d0, under 0.6 m).
		if (sameName(t->typeName(), "GenericProjectile")) {
			const physics::CollisionSubGeometry* part = physics::collisionPart(*mesh, 0);
			const physics::CollisionLod* lod = part ? physics::collisionLodFor(*mesh, *part, 2) : nullptr;
			body.pushedBySoldiers = lod && physics::boxRadius(lod->boundsMin, lod->boundsMax) < 0.6f;
		}
		const auto vector = [t](const char* name, physics::Vec3 fallback) {
			const auto it = t->settings().find(name);
			if (it != t->settings().end()) {
				std::sscanf(it->second.c_str(), "%f/%f/%f", &fallback.x, &fallback.y, &fallback.z);
			}
			return fallback;
		};
		physics::RigidBody& rigid = body.body;
		rigid.position = {object.position().x, object.position().y, object.position().z};
		rigid.rotation = physics::Mat3::fromYawPitchRoll({object.rotation().x, object.rotation().y, object.rotation().z});
		rigid.setBounds(high - low);
		rigid.centerOfMass = vector("centerOfMassOffset", {});
		rigid.inertiaModifier = vector("inertiaModifier", {1, 1, 1});
		rigid.gravityModifier = number(*t, "gravityModifier", 1);
		body.vehicle = Vehicle::fromTemplate(*t, m_templates, mesh);
		// A vehicle placed with its box's bottom under the terrain (the
		// shipped spawners stand on the ground and spawn at their spot,
		// 0x005aeb80) is lifted onto it, as the spawn spot's test drops it
		// (dropSpawnSpot). A deviation: the original spawns it sunk and its
		// responses come out of the ground without the rewrite's kick (the
		// whole push out in a tick, the springs' rate after it) that wrecked
		// the gunships and APCs on their pads.
		if (body.vehicle && m_ground.height) {
			const float up = rigid.rotation.rows[1].y;
			const float ground = m_ground.height(rigid.position.x, rigid.position.z);
			if (up > 0 && rigid.position.y + up * low.y < ground) {
				rigid.position.y = ground - up * low.y;
			}
		}
		if (body.vehicle) {
			// Each wheel's part radius: its geometry's vehicle LOD's (slot 0x50),
			// and its material (its first face's, through mapMaterial).
			const auto& wheels = body.vehicle->wheels();
			for (size_t w = 0; w < wheels.size(); ++w) {
				const Vehicle::Wheel& wheel = wheels[w];
				float radius = std::fabs(wheel.radius);
				if (wheel.collisionPart >= 0 && static_cast<size_t>(wheel.collisionPart) < mesh->geometries.size()) {
					for (const auto& part : physics::collisionParts(*mesh, static_cast<size_t>(wheel.collisionPart))) {
						if (const physics::CollisionLod* lod = physics::collisionLodFor(*mesh, part, 1)) {
							radius = physics::boxRadius(lod->boundsMin, lod->boundsMax);
							if (!lod->faces.empty()) {
								const uint32_t index = lod->faces.front().material;
								body.vehicle->setWheelMaterial(w, index < materials.size() ? materials[index] : index);
							}
						}
					}
				}
				body.wheelRadii.push_back(radius);
			}
		}
		m_bodies.push_back(std::move(body));
	}
}

// A walker's own animation system (bundleAnimationSystem; the object
// 0x0063d340 builds for the template, whose update 0x0063d2f0 runs
// 0x0063ce30 and the system's update, slot 0xc). 0x0063ce30 gives it:
// - the movement: the root node's velocity (slot 0x64) less the velocity
//   of what it stands on (slot 0xd8; nothing moves under a walker here, so
//   the 0.7 m/s floor that subtraction brings never applies), flattened
//   into the object's frame (x along its right, z its forward, y 0; not
//   normalised) for a root object, its length the speed divided by the
//   system's speed factor (slot 0x54) when that is over 0; the vector is
//   the node's angular velocity (slot 0x68);
// - the slope (slot 0x3c): the terrain's normal at the object's position
//   (the heightmap's slot 0x7c) against the object's forward and right,
//   times 3.2 (*0x00a75504), each within +-1 (the forward one not zeroed:
//   *0x00a75510 is 0);
// - the messages: 0x800 while the control object is occupied (interface
//   0xc4c5 slot 0x18), 0x10 while its crouch flag is up (slot 0x138, set by
//   the legs in state 5), 0x20 once its armour is destroyed (+0x48 slot
//   0x58: the walkers' "alive" switch takes its first child, "dead"),
//   0x40 a landing gear's down (+0x18, 0x00650010: none on a walker), and
//   the legs' movement messages (0x00650530).
// Its effect events go to the object's tree (takeObjectEffectEvents).
void GameWorld::stepObjectAnimation(Body& b, float seconds)
{
	if (!b.animation) {
		if (b.animationTried || !m_loadAnimationSystem || !m_animationSystems) {
			return;
		}
		b.animationTried = true;
		const world::Object* object = m_objects.find(b.object);
		const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
		std::string path;
		if (t) {
			const auto found = t->settings().find("bundleAnimationSystem");
			path = found != t->settings().end() ? found->second : std::string();
		}
		if (path.empty()) {
			return;
		}
		if (const anim::AnimationSystemTemplate* system = m_loadAnimationSystem(path)) {
			b.animation = std::make_unique<anim::AnimationState>(*m_animationSystems, *system);
		}
		if (!b.animation) {
			return;
		}
	}
	const Vehicle::Legs& legs = *b.vehicle->legs();
	const physics::Mat3& m = b.body.rotation;
	const physics::Vec3& v = b.body.velocity;
	anim::AnimationInput input;
	input.movementSeconds = seconds;
	float speed = std::sqrt(physics::lengthSquared(v));
	if (speed > 0.001f) {
		input.forward = v.x * m.rows[2].x + v.z * m.rows[2].z;
		input.side = v.x * m.rows[0].x + v.z * m.rows[0].z;
		speed = std::sqrt(input.forward * input.forward + input.side * input.side);
	}
	const float factor = b.animation->speedFactor();
	input.speed = factor > 0 ? speed / factor : speed;
	input.lookAroundX = b.body.angularVelocity.x;
	input.turn = b.body.angularVelocity.y;
	input.lookAroundY = b.body.angularVelocity.y;
	if (m_ground.heightAndNormal) {
		physics::Vec3 n {0, 1, 0};
		m_ground.heightAndNormal(b.body.position.x, b.body.position.z, n);
		input.slopeForward = std::clamp(physics::dot(n, m.rows[2]) * 3.2f, -1.0f, 1.0f);
		input.slopeSide = std::clamp(physics::dot(n, m.rows[0]) * 3.2f, -1.0f, 1.0f);
	}
	const bool occupied = std::any_of(b.occupants.begin(), b.occupants.end(), [](int p) { return p >= 0; });
	input.messages = legs.messages | (occupied ? 0x800u : 0u) | (legs.crouching ? 0x10u : 0u)
		| (b.wreckTime >= 0 ? 0x20u : 0u);
	b.animation->update(input, seconds);
	for (const anim::AnimationState::FiredEvent& event : b.animation->takeEvents()) {
		if (event.type == anim::Bundle::EffectEvent && event.id >= 0 && event.id < 10) {
			m_objectEffectEvents[b.object] |= 1u << (9 + event.id);
		}
	}
}

std::vector<std::string> GameWorld::objectAnimation(uint32_t object) const
{
	for (const Body& b : m_bodies) {
		if (b.object == object && b.animation) {
			return b.animation->playing();
		}
	}
	return {};
}

void GameWorld::stepPhysics(float seconds)
{
	// Each body's step and ground contact, then the mobile mesh physics
	// between the objects (MeshCollision.cpp), then each response.
	for (auto it = m_bodies.begin(); it != m_bodies.end();) {
		world::Object* object = m_objects.find(it->object);
		if (!object) {
			it = m_bodies.erase(it);
			continue;
		}
		if (findDrone(it->object) || isTitan(it->object)) {
			++it; // held in the air by its ForceObject (stepDrones), or a Titan moved by its sync (stepTitans)
			continue;
		}
		physics::RigidBody& body = it->body;
		if (std::any_of(it->occupants.begin(), it->occupants.end(), [](int p) { return p >= 0; })) {
			it->occupiedAt = m_time;
		}
		if (it->held) {
			// Held by its spawner: the engines turn (the hold lets go at 0.1),
			// the forces go nowhere.
			if (it->vehicle) {
				it->vehicle->setOccupied(
					std::any_of(it->occupants.begin(), it->occupants.end(), [](int p) { return p >= 0; }));
				it->vehicle->tick(body, m_ground, seconds, 0.8f);
			}
			body.velocity = body.angularVelocity = body.acceleration = body.torque = {};
			body.impulse = body.angularImpulse = body.frictionAcceleration = body.frictionTorque = {};
			body.frictionCount = 0;
			++it;
			continue;
		}
		if (it->vehicle && asleep(*it)) {
			// The engine wakes its node (0x0064e920): a throttle or the
			// engine still turning.
			if (std::fabs(it->vehicle->input().throttle) > 0.001f || std::fabs(it->vehicle->rpm()) >= 0.05f) {
				wake(*it);
			}
		}
		stepSleep(*it, seconds);
		if (asleep(*it)) {
			++it; // neither steps nor collides (0x006c9060, 0x006dd0b0)
			continue;
		}
		const bool legs = it->vehicle && it->vehicle->legs();
		if (it->vehicle) {
			// The terrain's material under the vehicle (the level's material
			// map) against material 0; the other models' wheels against their
			// parts' materials (the hover tanks' air cushions, 4351, multiply).
			const uint32_t ground = m_ground.material ? m_ground.material(body.position.x, body.position.z) : 0;
			const float grip = m_materials ? m_materials->friction(0, ground) : 0.8f;
			const auto& wheels = it->vehicle->wheels();
			for (size_t w = 0; w < wheels.size(); ++w) {
				if (!wheels[w].newCar2 && wheels[w].material && m_materials) {
					it->vehicle->setWheelGrip(w, m_materials->friction(wheels[w].material, ground));
				}
			}
			it->vehicle->setOccupied(
				std::any_of(it->occupants.begin(), it->occupants.end(), [](int p) { return p >= 0; }));
			if (it->animation) {
				it->vehicle->setLegsAnimation(it->animation->blocksInput(), it->animation->speedFactor());
			}
			it->vehicle->tick(body, m_ground, seconds, grip);
		}
		it->lastPosition = body.position;
		physics::applyDrag(body, it->drag, it->mass, it->dragModifier, it->boxMax - it->boxMin, it->submerged);
		body.step(seconds);
		if (legs) {
			// A walker's response is its legs' (MechLegs 0xc432), not the
			// hull's points.
			it->vehicle->legsRespond(body, m_ground, seconds);
		}
		// collisionGroups bit 0: no ground (0x006b9260's start).
		if (!(it->collisionGroups & 1) && !legs) {
			// Each contact (0x006b9260): the collision event while |v|^2 is
			// over handleCollisionSpeed (compared unsquared, as the original
			// does) and the contact effect from handleCollisionSpeed^2 on,
			// with the point's material against the terrain's.
			Body& self = *it;
			const auto touched = [this, &self](size_t index, const physics::Vec3& point, const physics::Vec3& velocity,
									 const physics::Vec3& normal) {
				const uint32_t own = index < self.pointMaterials.size() ? self.pointMaterials[index] : 0;
				const uint32_t terrain = m_ground.material ? m_ground.material(point.x, point.z) : 0;
				const float speed = physics::lengthSquared(velocity);
				if (self.handleCollisionSpeed < speed) {
					collisionEvent(self.object, 0, velocity, normal, point - self.body.position, own, terrain);
				}
				if (self.handleCollisionSpeed * self.handleCollisionSpeed <= speed) {
					recordContactEffect(self, own, terrain, velocity, point, normal);
				}
			};
			coolContactEffects(self, seconds);
			physics::collideWithGround(body, it->response, it->points, m_ground, {}, touched);
			// Buoyancy, after the ground's points (0x006b9260).
			if (m_ground.waterLevel) {
				const float level = m_ground.waterLevel(body.position.x, body.position.z);
				it->submerged = physics::applyBuoyancy(body, it->boxMin, it->boxMax, it->points, level, it->floaterMod,
					physics::tuning::kGravity, it->submerged, true);
			}
		}
		++it;
	}
	// The walkers' own animation systems, after their legs' state.
	for (Body& b : m_bodies) {
		if (b.vehicle && b.vehicle->legs() && !b.held) {
			stepObjectAnimation(b, seconds);
		}
	}
	collideBodies(seconds);
	for (Body& b : m_bodies) {
		world::Object* object = m_objects.find(b.object);
		if (!object || findDrone(b.object) || isTitan(b.object) || asleep(b)) {
			continue;
		}
		b.response.solve(b.body);
		if (b.vehicle && b.vehicle->legs()) {
			b.vehicle->legsDrive(b.body);
		}
		// A body thrown out of the world (a non-finite or absurd state) is
		// stopped where it last was sane rather than carried on.
		const auto sane = [](const physics::Vec3& v, float limit) {
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) < limit
				&& std::fabs(v.y) < limit && std::fabs(v.z) < limit;
		};
		if (!sane(b.body.position, 1e5f) || !sane(b.body.velocity, 1e4f) || !sane(b.body.angularVelocity, 1e4f)) {
			if (!m_reportedBlowUp) {
				m_reportedBlowUp = true;
				std::fprintf(stderr, "physics: %s left the world (position %g %g %g)\n",
					object->objectTemplate() ? object->objectTemplate()->name().c_str() : "?", b.body.position.x,
					b.body.position.y, b.body.position.z);
			}
			b.body.position = b.lastPosition;
			if (!sane(b.body.position, 1e5f)) {
				b.body.position = {object->position().x, object->position().y, object->position().z};
			}
			b.body.velocity = b.body.angularVelocity = {};
			b.body.rotation = sane(b.body.rotation.rows[0], 2) && sane(b.body.rotation.rows[1], 2) ? b.body.rotation : physics::Mat3 {};
		}
		playContactEffects(b);
		object->setPosition({b.body.position.x, b.body.position.y, b.body.position.z});
		const physics::Vec3 angles = b.body.rotation.yawPitchRoll();
		object->setRotation({angles.x, angles.y, angles.z});
	}
}


// Squad leader drones (the recon and sentry drones' detonators): the
// detonator's projectile is a PlayerControlObject that carries a
// ForceObject child (unl_drone_recon_hover, unl_drone_sentry_ForceObjectFollow).
world::ObjectTemplate* GameWorld::droneTemplate(const world::ObjectTemplate* weapon) const
{
	if (!weapon) {
		return nullptr;
	}
	const auto projectile = weapon->settings().find("projectileTemplate");
	world::ObjectTemplate* t = projectile != weapon->settings().end() ? m_templates.find(projectile->second) : nullptr;
	if (!t) {
		return nullptr;
	}
	for (const world::ChildTemplate& child : t->children()) {
		const world::ObjectTemplate* c = m_templates.find(child.name);
		if (c && sameName(c->typeName(), "ForceObject")) {
			return t;
		}
	}
	return nullptr;
}

namespace {

// The first template of a type in a template's tree (children first, then
// their children), for the drones' ForceObject and firearm.
const world::ObjectTemplate* findInTree(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t,
	const char* type, int depth = 0)
{
	if (depth > 8) {
		return nullptr;
	}
	for (const world::ChildTemplate& child : t.children()) {
		const world::ObjectTemplate* c = templates.find(child.name);
		if (c && sameName(c->typeName(), type)) {
			return c;
		}
	}
	for (const world::ChildTemplate& child : t.children()) {
		const world::ObjectTemplate* c = templates.find(child.name);
		if (const world::ObjectTemplate* found = c ? findInTree(templates, *c, type, depth + 1) : nullptr) {
			return found;
		}
	}
	return nullptr;
}

// The sentry drone's auto controller, UAVDrone in AI/AutoControllers.ai
// (the .ai files aren't read into templates yet, so its values are here).
constexpr float kSentryViewDistance = 100;          // viewDistance
constexpr float kSentryDiscoverCloakedDistance = 8; // discoverCloakedEnemiesDistance
constexpr float kSentryFov = 90;                    // fov (degrees)
constexpr float kSentryScanningSpeed = 0.13f;       // scanningSpeed (radians per 1/30 s)
constexpr float kSentryTargetLostTimeout = 0.25f;   // targetLostTimeout
constexpr float kSentryCamMaxSpeed = 250;           // unl_drone_sentry_cam setMaxSpeed (degrees/s)
constexpr float kSquadKnowledgeTime = 3.0f;         // 0x100bea30

} // namespace

// The detonator "fires" its drone: one per owner (the old one goes), set
// out at the detonator's fire.projectileStartPosition (1/1.75/1 from the
// soldier's feet, turned by its heading) and moving off at its velocity.
void GameWorld::launchDrone(Player& player, const world::ObjectTemplate& detonator, world::ObjectTemplate& t)
{
	for (auto it = m_drones.begin(); it != m_drones.end(); ++it) {
		if (it->owner == player.id) {
			removeDrone(it->object);
			break;
		}
	}
	const world::Object* soldier = m_objects.find(player.object);
	if (!soldier) {
		return;
	}
	const physics::Vec3 start = parseVec3(detonator.settings().count("fire.projectileStartPosition")
			? detonator.settings().find("fire.projectileStartPosition")->second
			: "0/1.75/0");
	const float yaw = lookYaw(player) * 3.14159265f / 180.0f;
	const float c = std::cos(yaw), s = std::sin(yaw);
	const physics::Vec3 feet {soldier->position().x, soldier->position().y, soldier->position().z};
	Drone drone;
	drone.owner = player.id;
	drone.team = player.team;
	drone.position = feet + physics::Vec3 {start.x * c + start.z * s, start.y, -start.x * s + start.z * c};
	drone.velocity = physics::Vec3 {s, 0, c} * number(detonator, "velocity", 0);
	drone.yaw = lookYaw(player);
	drone.gunYaw = drone.yaw * 3.14159265f / 180.0f;
	drone.hoverHeight = 1; // the soldier's physics origin over its feet (0x005f0a00)
	if (const world::ObjectTemplate* force = findInTree(m_templates, t, "ForceObject")) {
		const char* axes[3] = {"regulatePosX", "regulatePosY", "regulatePosZ"};
		for (int axis = 0; axis < 3; ++axis) {
			if (const auto it = force->settings().find(axes[axis]); it != force->settings().end()) {
				std::sscanf(it->second.c_str(), "%f/%f", &drone.force.regulatePos[axis][0], &drone.force.regulatePos[axis][1]);
			}
		}
		drone.force.positionToleranceXZ = number(*force, "positionToleranceXZ", 0);
		drone.force.warningDistance = number(*force, "warningDistance", 0);
		drone.force.destroyDistance = number(*force, "destroyDistance", 0);
		drone.force.minHeightAdjustDistance = number(*force, "minHeightAdjustDistance", 0);
		drone.force.destroyWhenTargetLost = flag(*force, "destroyWhenTargetLost", false);
	}
	if (const world::ObjectTemplate* gun = findInTree(m_templates, t, "GenericFireArm")) {
		const FireArmSettings settings = FireArmSettings::fromTemplate(*gun);
		drone.weapon = std::make_unique<FireArm>(settings);
		drone.gunDeviation = DeviationBuildUp::fromTemplate(*gun);
		drone.projectile = projectileFor(settings.projectileTemplate);
		const auto& components = gun->components();
		drone.pulse = std::find_if(components.begin(), components.end(), [](const std::string& c) {
			return sameName(c, "PulseMeterAmmoComp");
		}) != components.end();
		drone.pulseDistance = number(*gun, "pulse.viewDistance", 0);
		drone.revealTime = number(*gun, "pulse.revealTime", 10); // 10 by default (0x005d9a70)
	}
	world::Object* placed = m_objects.create(&t, {drone.position.x, drone.position.y, drone.position.z}, {drone.yaw, 0, 0});
	if (!placed) {
		return;
	}
	placed->setTeam(player.team);
	drone.object = placed->id();
	m_drones.push_back(std::move(drone));
}

GameWorld::Drone* GameWorld::findDrone(uint32_t object)
{
	for (Drone& drone : m_drones) {
		if (drone.object == object) {
			return &drone;
		}
	}
	return nullptr;
}

// A drone shot down, lost or recalled is gone at once.
bool GameWorld::removeDrone(uint32_t object)
{
	for (auto it = m_drones.begin(); it != m_drones.end(); ++it) {
		if (it->object == object) {
			m_objects.remove(object);
			m_hitPoints.erase(object);
			m_drones.erase(it);
			return true;
		}
	}
	return false;
}

uint32_t GameWorld::drone(int owner) const
{
	for (const Drone& drone : m_drones) {
		if (drone.owner == owner) {
			return drone.object;
		}
	}
	return 0;
}

bool GameWorld::droneWarning(int owner) const
{
	for (const Drone& drone : m_drones) {
		if (drone.owner == owner) {
			return drone.warning;
		}
	}
	return false;
}

bool GameWorld::isRevealed(int player, int byTeam) const
{
	const auto it = m_revealed.find({player, byTeam});
	return it != m_revealed.end() && it->second > 0;
}

std::vector<int> GameWorld::revealedPlayers(int byTeam) const
{
	std::vector<int> players;
	for (const auto& [key, left] : m_revealed) {
		if (key.second == byTeam && left > 0) {
			players.push_back(key.first);
		}
	}
	return players;
}

// The ForceObject's update (0x0062b750, called from 0x0062c920 each tick)
// holds a drone by its target, the owner's soldier (the detonator's
// ParentTargetComp):
// - Lost target: with destroyWhenTargetLost the drone is destroyed once
//   the target's handle (0x1b0) is empty. The handle is the owner's
//   soldier; here it counts as gone from the soldier's death, as when its
//   object goes the ParentTargetComp's handle empties (whether the
//   original's downed soldier still holds it isn't settled by this code).
// - The goal: across the ground, the point positionToleranceXZ short of
//   the target, or where the drone is when it is that near already; in
//   height, the target's position, taken again only while the target is
//   more than minHeightAdjustDistance off across the ground (0x1a8). The
//   target's position is the soldier's physics origin, a metre over its
//   feet (0x005f0a00 poses the skeleton 0/-1/0 from it; our soldier's
//   object stands at its feet).
// - Farther than destroyDistance from the goal the drone is destroyed;
//   past warningDistance the owner hears "uav_warning" (0x1e9 while on).
// - Each axis has a regulator (0x00667710, set up by 0x0062cf60 with the
//   object's mass m and regulatePos (a, b)): force = 2am v + (a² + b²) m
//   (goal - p).
// - The force goes to the physics node's slot 0xa0 (0x006c7b10: added to
//   the acceleration, the moment to the torque) times 30 over the mass,
//   so each 30 Hz tick changes the velocity by 2a v + (a² + b²)(goal - p);
//   the node's step (0x006c8380) adds gravity (Physics.gravity times
//   gravityModifier) and damps by 0.99.
// - EMP: while its armour's level (rounded up) is above 1 the update ends
//   before any force, so the drone falls (and its height held becomes
//   where it is).
// - The drone turns to face where it is going.
void GameWorld::stepDrones(float seconds)
{
	for (auto it = m_revealed.begin(); it != m_revealed.end();) {
		it->second -= seconds;
		it = it->second <= 0 ? m_revealed.erase(it) : std::next(it);
	}
	std::vector<uint32_t> lost;
	for (Drone& drone : m_drones) {
		world::Object* object = m_objects.find(drone.object);
		const Player* owner = findPlayer(drone.owner);
		const world::Object* target = owner && owner->alive ? m_objects.find(owner->object) : nullptr;
		if (!object || (!target && drone.force.destroyWhenTargetLost)) {
			lost.push_back(drone.object);
			continue;
		}
		const world::ObjectTemplate* t = object->objectTemplate();
		if (drone.empLevel > 0) {
			drone.empLevel = std::max(0.0f, drone.empLevel - seconds / (t ? number(*t, "armor.empLevelTime", 5) : 5));
		}
		const bool stunned = std::ceil(drone.empLevel) > 1;
		physics::Vec3 goal = drone.position;
		bool held = true;
		if (target) {
			const physics::Vec3 at {target->position().x, target->position().y + drone.hoverHeight, target->position().z};
			const float dx = at.x - drone.position.x, dz = at.z - drone.position.z;
			const float across = std::sqrt(dx * dx + dz * dz);
			const float tolerance = drone.force.positionToleranceXZ;
			if (across >= tolerance && across > 0) {
				goal.x = drone.position.x + dx / across * (across - tolerance);
				goal.z = drone.position.z + dz / across * (across - tolerance);
			}
			if (across > drone.force.minHeightAdjustDistance || !drone.hasHeight) {
				drone.height = at.y;
				drone.hasHeight = true;
			} else if (stunned) {
				drone.height = drone.position.y;
			}
			goal.y = drone.height;
			if (stunned) {
				held = false;
			} else {
				const float off = physics::lengthSquared(goal - drone.position);
				const float destroy = drone.force.destroyDistance, warning = drone.force.warningDistance;
				if (destroy > 0 && off > destroy * destroy) {
					lost.push_back(drone.object);
					continue;
				}
				drone.warning = warning > 0 && off > warning * warning;
			}
		}
		float* p[3] = {&drone.position.x, &drone.position.y, &drone.position.z};
		float* v[3] = {&drone.velocity.x, &drone.velocity.y, &drone.velocity.z};
		const float want[3] = {goal.x, goal.y, goal.z};
		const float gravity = physics::tuning::kGravity * (t ? number(*t, "gravityModifier", 1) : 1);
		for (int axis = 0; axis < 3; ++axis) {
			const float a = drone.force.regulatePos[axis][0], b = drone.force.regulatePos[axis][1];
			float acceleration = held ? 30 * (2 * a * *v[axis] + (a * a + b * b) * (want[axis] - *p[axis])) : 0;
			if (axis == 1) {
				acceleration += gravity;
			}
			const float previous = *v[axis];
			*v[axis] = (*v[axis] + acceleration * seconds) * physics::tuning::kLinearDamping;
			*p[axis] += (previous + *v[axis]) * 0.5f * seconds;
		}
		if (m_ground.height) {
			const float floor = m_ground.height(drone.position.x, drone.position.z) + 0.5f;
			if (drone.position.y < floor) {
				drone.position.y = floor;
				drone.velocity.y = std::max(drone.velocity.y, 0.0f);
			}
		}
		if (drone.velocity.x * drone.velocity.x + drone.velocity.z * drone.velocity.z > 0.01f * 0.01f) {
			drone.yaw = std::atan2(drone.velocity.x, drone.velocity.z) * 57.29578f;
		}
		object->setPosition({drone.position.x, drone.position.y, drone.position.z});
		object->setRotation({drone.yaw, 0, 0});
		if (Body* body = findBody(drone.object)) {
			body->body.position = drone.position;
			body->body.velocity = drone.velocity;
			body->body.rotation = physics::Mat3::fromYawPitchRoll({drone.yaw, 0, 0});
		}
		// Its weapon, run by its auto controller: the recon drone's
		// (ReconDrone: simpleFirePlan) fires all the time; the sentry's
		// (UAVDrone) at an enemy. Under EMP above level 1 the sentry's
		// controller inputs are cut (fireSentry, 0x005ca2e0).
		if (drone.weapon && drone.pulse) {
			if (drone.weapon->tick(seconds, true)) {
				pulse(drone);
			}
		} else if (drone.weapon) {
			fireSentry(drone, seconds);
		}
	}
	for (const uint32_t object : lost) {
		removeDrone(object);
	}
}

// A recon drone's pulse (PulseMeterAmmoComp's fire, 0x005d9cf0): with
// pulse.fov below 0 it reaches round about, every player whose object is
// within pulse.viewDistance (+0.01) of the drone, with no line of sight
// needed. The server sends them to the drone's team with pulse.revealTime
// (seconds) to show on its minimap; the team's own are left out here.
void GameWorld::pulse(const Drone& drone)
{
	const float reach = drone.pulseDistance + 0.01f;
	std::vector<int> reported;
	for (const auto& owned : m_players) {
		const Player& player = *owned;
		if (!player.alive || player.team == drone.team) {
			continue;
		}
		const world::Object* object = m_objects.find(player.vehicle ? player.vehicle : player.object);
		if (!object) {
			continue;
		}
		const physics::Vec3 at {object->position().x, object->position().y, object->position().z};
		if (physics::lengthSquared(at - drone.position) <= reach * reach) {
			float& left = m_revealed[{player.id, drone.team}];
			left = std::max(left, drone.revealTime);
			reported.push_back(player.id);
		}
	}
	// The pulse reports them as its owner's (0x005d9cf0, type 0).
	if (!reported.empty()) {
		reportBattlefieldObjects(drone.owner, reported);
	}
}

void GameWorld::reportBattlefieldObjects(int reporter, const std::vector<int>& players)
{
	const Player* from = findPlayer(reporter);
	if (!from) {
		return;
	}
	auto& known = m_squadKnowledge[{from->team, from->squad}];
	for (const int id : players) {
		const Player* player = findPlayer(id);
		if (!player || player->team == from->team) {
			continue;
		}
		known[id] = m_time;
		if (player->vehicle) {
			for (const int occupant : vehicleOccupants(player->vehicle)) {
				if (occupant >= 0) {
					known[occupant] = m_time;
				}
			}
		}
	}
}

bool GameWorld::squadKnows(int team, int squad, int player) const
{
	const auto it = m_squadKnowledge.find({team, squad});
	if (it == m_squadKnowledge.end()) {
		return false;
	}
	const auto at = it->second.find(player);
	return at != it->second.end() && m_time - at->second < kSquadKnowledgeTime;
}

// The sentry drone's gun (unl_drone_sentry_FireArm: 600 a minute, rounds
// of 10 falling to 5 from 15 to 40 m) aimed by its auto controller
// (UAVDrone). Without a target the ScanForEnemies statement (AIDLL
// 0x100f9a90, 0x100f9cf0) turns the camera's yaw (an unlimited bundle:
// round and round one way) by scanningSpeed x 30 radians a second. It
// sees in a frustum of fov (90 degrees: +-45 about the gun's facing each
// way) out to viewDistance / cos(fov / 2) (0x100feea0): enemy soldiers
// on foot in line of sight, cloaked ones within
// discoverCloakedEnemiesDistance (8 m), never its deployer
// (neverAttackDeployer). With onlyAttackNetworkBattlefieldObjects
// (0x100ea470) one counts only when the owner's squad had it reported in
// the last 3 s (squadKnows; an owner without a squad attacks anyone). A
// target out of sight for targetLostTimeout (0.25 s) is dropped. While
// tracking, the yaw turns at most maxTrackingInput (1) x the bundle's 250
// degrees a second; the gun fires once it points within 2 degrees (the
// rewrite's gate; the fire plan's own is the bots' aim gate).
void GameWorld::fireSentry(Drone& drone, float seconds)
{
	const float half = kSentryFov * 0.5f * 3.14159265f / 180.0f;
	const float far = kSentryViewDistance / std::cos(half);
	const Player* ownerPlayer = findPlayer(drone.owner);
	const int ownerSquad = ownerPlayer ? ownerPlayer->squad : 0;
	const auto visible = [&](const Player& player, physics::Vec3& chest, bool inView) {
		if (!player.alive || player.vehicle || player.team == drone.team || player.id == drone.owner) {
			return false;
		}
		const world::Object* soldier = m_objects.find(player.object);
		if (!soldier) {
			return false;
		}
		chest = {soldier->position().x, soldier->position().y + 1.3f, soldier->position().z};
		const physics::Vec3 d = chest - drone.position;
		const float flat = std::sqrt(d.x * d.x + d.z * d.z);
		const float depth = d.x * std::sin(drone.gunYaw) + d.z * std::cos(drone.gunYaw);
		if (inView && (depth <= 0.01f || depth > far || std::fabs(std::remainder(std::atan2(d.x, d.z) - drone.gunYaw, 6.2831853f)) > half
			|| std::fabs(std::atan2(d.y, flat)) > half)) {
			return false;
		}
		if (player.cloaked && physics::lengthSquared(d) > kSentryDiscoverCloakedDistance * kSentryDiscoverCloakedDistance) {
			return false;
		}
		if (ownerSquad != 0 && !squadKnows(drone.team, ownerSquad, player.id)) {
			return false;
		}
		return lineOfSight(drone.position, chest, drone.object);
	};
	physics::Vec3 aim;
	const Player* best = nullptr;
	if (const Player* held = drone.target >= 0 ? findPlayer(drone.target) : nullptr) {
		if (visible(*held, aim, false)) {
			best = held;
			drone.targetSeenAt = static_cast<float>(m_time);
		} else if (m_time - drone.targetSeenAt > kSentryTargetLostTimeout || !held->alive) {
			drone.target = -1;
		}
	} else {
		drone.target = -1;
	}
	if (drone.target < 0) {
		float bestDistance = far * far;
		for (const auto& owned : m_players) {
			physics::Vec3 chest;
			if (!visible(*owned, chest, true)) {
				continue;
			}
			const float distance = physics::lengthSquared(chest - drone.position);
			if (distance < bestDistance) {
				best = owned.get();
				bestDistance = distance;
				aim = chest;
			}
		}
		if (best) {
			drone.target = best->id;
			drone.targetSeenAt = static_cast<float>(m_time);
		}
	}
	// Under EMP (the PlayerControlObject's input filter 0x005ca2e0): an
	// auto-controlled one (setAutoControllerTemplateName, UAVDrone on the
	// sentry) has every input its controller gives cut above level 1: the
	// look (PIMouseLookX) and PIFire too. The gun neither turns nor fires.
	const world::Object* droneObject = m_objects.find(drone.object);
	const world::ObjectTemplate* droneTemplate = droneObject ? droneObject->objectTemplate() : nullptr;
	const bool autoControlled = droneTemplate && droneTemplate->settings().count("autoControllerTemplateName")
		&& !droneTemplate->settings().at("autoControllerTemplateName").empty();
	if (autoControlled && std::ceil(drone.empLevel) > 1) {
		drone.gunDeviation.update(false);
		drone.weapon->tick(seconds, false);
		return;
	}
	bool onTarget = false;
	if (best) {
		const float want = std::atan2(aim.x - drone.position.x, aim.z - drone.position.z);
		const float error = std::remainder(want - drone.gunYaw, 6.2831853f);
		const float most = kSentryCamMaxSpeed * 3.14159265f / 180.0f * seconds;
		drone.gunYaw += std::clamp(error, -most, most);
		onTarget = std::fabs(error) <= most + 2 * 3.14159265f / 180.0f;
	} else {
		drone.gunYaw = std::remainder(drone.gunYaw + kSentryScanningSpeed * 30 * seconds, 6.2831853f);
	}
	drone.gunDeviation.update(onTarget); // the auto controller holds the trigger while on target
	if (!drone.weapon->tick(seconds, onTarget) || !onTarget || !drone.projectile) {
		return;
	}
	physics::Vec3 direction = aim - drone.position;
	direction = direction * (1.0f / std::sqrt(physics::lengthSquared(direction)));
	const world::Object* object = m_objects.find(drone.object);
	const world::ObjectTemplate* gun = object ? findInTree(m_templates, *object->objectTemplate(), "GenericFireArm") : nullptr;
	// The round strays by the gun's deviation component (0x005f5540): two
	// normal numbers x (minDev + the firing's build-up) x 0.01 x its
	// velocity, along its up and right; the round adds to the build-up.
	const float deviation = gun ? drone.gunDeviation.deviation() : 0;
	drone.gunDeviation.fired();
	if (deviation > 0.01f) {
		const physics::Vec3 right = physics::lengthSquared(physics::Vec3 {direction.z, 0, -direction.x}) > 1e-8f
			? physics::Vec3 {direction.z, 0, -direction.x} * (1 / std::sqrt(direction.z * direction.z + direction.x * direction.x))
			: physics::Vec3 {1, 0, 0};
		const physics::Vec3 up = physics::cross(direction, right);
		const float alongUp = m_gaussian.next(m_random), alongRight = m_gaussian.next(m_random);
		direction = deviateVelocity(direction, right, up, 1, deviation, alongUp, alongRight);
		direction = direction * (1 / std::sqrt(physics::lengthSquared(direction)));
	}
	const float yaw = std::atan2(direction.x, direction.z);
	Projectile p;
	p.origin = drone.position + direction * 0.5f;
	p.position = p.origin;
	p.velocity = direction * drone.weapon->settings().velocity;
	p.settings = drone.projectile;
	p.timeLeft = drone.projectile->timeToLive.sample(m_random);
	p.shooter = drone.object;
	p.player = drone.owner;
	p.team = drone.team;
	p.yaw = yaw * 57.29578f;
	m_projectiles.push_back(p);
}

} // namespace dice::hfe::game
