// The commander, with the game's own templates where $BF2142_GAME_DIR is
// set (Weapons_server.zip: the commander's assets commander_emp_strike,
// commander_orbital_strike_64, commander_uav_drone_control and
// commander_movement_analyst, their RemoteControlledObjects, and the
// supply_crate); the assignment and the orders run without them.
#include "Console/Console.h"
#include "Game/Bots.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Main/GameAudio.h"
#include "Main/HostedGame.h"
#include "Main/HudMenus.h"
#include "Main/MapCommanderMarkers.h"
#include "Net/GameClient.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
#include "Sound/Game/GameSounds.h"
#include "Sound/Game/SoundConsole.h"
#include "Sound/Game/SoundManager.h"
#include "Sound/Game/VfsSoundFileSystem.h"
#include "Sound/SoundEngine/ISoundEngine.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

using Ability = game::GameWorld::CommanderAbility;

// Flat ground, a control point and a spawn point per team (PAC's at z 20,
// EU's at the origin), and the commander's templates as the game loads
// them.
struct Field {
	io::FileManager files;
	Console console {&files};
	world::ObjectTemplateManager templates;
	sound::SoundManager soundManager;
	world::ObjectManager objects;
	physics::MaterialManager materials;
	physics::CollisionTemplateManager collisionTemplates;
	std::unique_ptr<game::GameWorld> world;
	std::vector<std::pair<std::string, game::ScriptArgs>> events;
	bool loaded = false;
	uint32_t groundMaterial = 0;

	// `materialTable`: the shipped damage table (Common/Material) handed to
	// the world, as the game does.
	explicit Field(const std::string& mod = "mods/bf2142", bool collisionMeshes = false,
		sound::ISoundEngine* audioEngine = nullptr, bool materialTable = false)
		: soundManager(audioEngine, templates)
	{
		if (audioEngine) {
			soundManager.registerTemplateType();
			sound::registerSoundConsoleMethods(console, soundManager);
		}
		if (audioEngine || materialTable) {
			physics::registerMaterialConsoleMethods(console, materials);
		}
		if (const char* gameDir = std::getenv("BF2142_GAME_DIR")) {
			files.setRootPath(gameDir);
			files.addSearchPath(mod);
			if (materialTable && files.mountArchive("Common_server.zip", "Common")) {
				std::string out;
				console.run("Common/Material/materialManagerDefine.con", {}, out);
				console.run("Common/Material/materialManagerSettings.con", {}, out);
			}
			if (files.mountArchive("Objects/Weapons_server.zip", "Objects\\Weapons")) {
				if (collisionMeshes) {
					physics::registerCollisionConsoleMethods(console, collisionTemplates);
				}
				world::registerWorldConsoleMethods(console, templates, objects);
				if (collisionMeshes) {
					// As the game keeps them (its console tables' ObjectTemplate properties).
					world::registerUntypedTemplateProperties(console, templates,
						{"collisionMesh", "collisionPart", "hasCollisionPhysics"});
				}
				world::registerUntypedTemplateProperties(console, templates,
					{"rcType", "minTimeBetweenActivation", "uavVehicleTemplate", "uavVehicleSpeed", "uavVehicleRadius",
						"uavVehicleFlightHeight", "uavVehicleSampleResolution", "armor.maxHitPoints", "armor.hitPoints",
						"armor.defaultMaterial", "armor.takesEmpDamage", "physicsType", "hasMobilePhysics",
						"fire.roundsPerMinute", "fire.burstSize", "fire.batchSize", "fire.fireLaunchDelay",
						"deviation.subProjectileDev", "ammo.magSize", "ammo.nrOfMags", "velocity", "projectileTemplate",
						"detonation.explosionDamage", "detonation.explosionRadius", "detonation.explosionMaterial",
						"detonation.isEMPWeapon", "detonation.endEffectTemplate", "detonation.useMMOnEndEffect",
						"damage", "minDamage", "timeToLive", "gravityModifier", "material",
						"seek.targetType", "radius", "healSpeed", "refillAmmoSpeed", "sharedStorageSize",
						"healStorageSize", "ammoStorageSize", "destroyWhenEmptyStorage", "workOnVehicles",
						"continousRotationSpeed"});
				std::string out;
				const std::string base = "Objects/Weapons/";
				for (const char* script : {"common/RemoteControlledObjects/OrbitalStrikeControlObjectUS.con",
						 "common/RemoteControlledObjects/ArtilleryControlObjectUS.con",
						 "common/RemoteControlledObjects/UAVControlObject.con",
						 "common/RemoteControlledObjects/SatelliteControlObject.con",
						 "stationary/commander_EMP_strike/commander_EMP_strike.con",
						 "stationary/commander_orbital_Strike/commander_orbital_Strike.con",
						 "stationary/commander_orbital_Strike/commander_orbital_strike_64.con",
						 "stationary/commander_uav_drone_control/commander_uav_drone_control.con",
						 "stationary/commander_movement_analyst/commander_movement_analyst.con",
						 "stationary/commander_supplybox/supply_crate.con",
						 "stationary/missile_silo/missile_silo_v2.con"}) {
					console.run(base + script, {}, out);
				}
				loaded = templates.find("commander_emp_strike") && templates.find("commander_orbital_strike_64")
					&& templates.find("commander_uav_drone_control") && templates.find("commander_movement_analyst")
					&& templates.find("supply_crate") && templates.find("missile_silo_v2")
					&& templates.find("OrbitalStrikeControlObjectUS");
			}
		}
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			templates.create("Soldier", name)->settings()["armor.maxHitPoints"] = "100";
		}
		for (int team = 1; team <= 2; ++team) {
			world::ObjectTemplate* point = templates.create("ControlPoint", team == 1 ? "cp_pac" : "cp_eu");
			point->settings()["controlPointId"] = std::to_string(team);
			point->settings()["team"] = std::to_string(team);
			point->settings()["unableToChangeTeam"] = "1";
			objects.create(point, {team == 1 ? 100.0f : -100.0f, 0, 0});
		}
		world::ObjectTemplate* spawn = templates.create("SpawnPoint", "sp");
		objects.create(spawn, {0, 0, 20}, {90, 0, 0})->setControlPointId(1);
		objects.create(spawn, {0, 0, 0}, {0, 0, 0})->setControlPointId(2);
		world = std::make_unique<game::GameWorld>(templates, objects);
		if (loaded) {
			world->enablePhysics(files, collisionTemplates,
				{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
					 normal = {0, 1, 0};
					 return 0.0f;
				 }, [this](float, float) { return groundMaterial; }}, audioEngine || materialTable ? &materials : nullptr);
		}
		world->setEventSink([this](const std::string& event, const game::ScriptArgs& args) {
			events.emplace_back(event, args);
		});
		world->startRound();
	}

	void run(float seconds)
	{
		for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
			world->tick(1 / 30.0f);
		}
	}
	// An asset of the team, set down as its spawner would (teamOnVehicle).
	uint32_t place(const char* name, int team, float x, float z)
	{
		world::Object* asset = objects.create(templates.find(name), {x, 0, z});
		asset->setTeam(team);
		run(1.1f); // looked through once a second
		return asset->id();
	}
	// A soldier of the team at a place (its feet).
	int soldier(int team, float x, float z)
	{
		const int id = world->addPlayer(team == 1 ? "Pac" : "Eu", team, true);
		world->spawnSoldier(id);
		game::GameWorld::PlayerInfo info;
		world->playerInfo(id, info);
		world->setSoldierPosition(info.soldier, {x, 0, z});
		return id;
	}
	bool alive(int player) const
	{
		game::GameWorld::PlayerInfo info;
		return world->playerInfo(player, info) && info.alive;
	}
};

} // namespace

// Static PCO structures created by a round's ObjectSpawners arrive after
// GameWorld::startRound has built the level collision. Use the shipped Suez
// Titan silo and commander scanning station assets to ensure they join that
// collision snapshot before the next soldier movement step, in both mods.
TEST(Commander_LateStaticAssetsEnterCollisionWorld)
{
	if (!std::getenv("BF2142_GAME_DIR")) {
		return;
	}
	for (const std::string& mod : {std::string("mods/bf2142"), std::string("mods/Project_Remaster_v17_Final")}) {
		if (!test::runs(mod)) continue;
		for (const char* name : {"missile_silo_v2", "commander_movement_analyst"}) {
			Field field(mod, true);
			if (!field.loaded) {
				CHECK(field.loaded);
				continue;
			}
			world::ObjectTemplate* structure = field.templates.find(name);
			CHECK(structure != nullptr);
			if (!structure) {
				continue;
			}
			const auto mobile = structure->settings().find("hasMobilePhysics");
			CHECK(mobile != structure->settings().end() && mobile->second == "0");
			const world::Object* placed = field.objects.create(structure, {0, 0, 0}, {90, 0, 0}); // Suez's runtime spawns use a yawed map rotation
			if (std::string(name) == "missile_silo_v2") {
				// The silo's barrel rests upright (a deviation from the flat pose its mesh has at rest).
				world::Vec3 offset, rotation;
				CHECK(field.world->partPose(placed->id(), "Missile_Silo_Barrel", offset, rotation));
				CHECK(std::fabs(rotation.y + 90.0f) < 1e-3f);
			}
			field.world->tick(1 / 30.0f);
			float hitFraction = 0;
			uint32_t hitObject = 0;
			const bool hit = field.world->segmentHit({0, 1, -15}, {0, 1, 15}, 0, hitFraction, hitObject);
			std::fprintf(stderr, "  %s/%s: collision mesh %s, ray hit %d at %.2f owner %u\n", mod.c_str(), name,
				structure->settings().at("collisionMesh").c_str(), hit, hitFraction, hitObject);
			CHECK(hit);
			CHECK(field.world->spawnPlayer(2) != nullptr);
			const int player = field.world->localPlayer();
			game::GameWorld::PlayerInfo info;
			CHECK(field.world->playerInfo(player, info) && info.alive);
			field.world->setSoldierPosition(info.soldier, {0, 0, -15});
			game::SoldierInput walk;
			walk.throttle = 1;
			walk.sprint = true;
			walk.turn = -info.yaw; // face +z, toward the structure
			float tailMin = 1e9f, tailMax = -1e9f;
			for (int i = 0; i < 150; ++i) {
				field.world->setInput(player, walk);
				walk.turn = 0;
				field.world->tick(1 / 30.0f);
				if (i >= 120 && field.world->playerInfo(player, info)) {
					tailMin = std::min(tailMin, info.position.z);
					tailMax = std::max(tailMax, info.position.z);
				}
			}
			CHECK(field.world->playerInfo(player, info));
			std::fprintf(stderr, "  %s/%s: final z %.2f\n", mod.c_str(), name, info.position.z);
			CHECK(info.position.z < -1.0f);
			CHECK(tailMax - tailMin < 0.5f);
		}
	}
}

// Child parts that take the root's collision mesh through collisionPart
// (no collisionMesh of their own): the silo's base and launch tube, the EMP
// station's mount and dish, the orbital controller's antenna. Each part's
// LODs join the static worlds at the part's place: the soldier world only
// where the part authors a soldier (or vehicle) LOD, so the EMP and orbital
// parts add projectile faces alone. The silo's tube stands upright as
// partPose turns it, not along its flat rest pose. Placed unturned and
// yawed 90 degrees (Suez's spawns are yawed).
TEST(Commander_InheritedCollisionPartsCollide)
{
	if (!std::getenv("BF2142_GAME_DIR")) {
		return;
	}
	struct Probe {
		physics::Vec3 from, to;
		bool hits;
	};
	struct Asset {
		const char* name;
		size_t soldierTriangles, projectileTriangles; // root + child parts (work/collision-trace/probe.txt)
		std::vector<Probe> projectile, soldier;
	};
	const std::vector<Asset> assets = {
		{"missile_silo_v2", 92 + 60 + 12, 2264 + 532 + 244,
			{{{-10, 8, 0.97f}, {10, 8, 0.97f}, true}, // the upright tube, above the base
				{{-10, 3.2f, 5}, {10, 3.2f, 5}, false}}, // where the flat rest-pose tube would lie
			{{{-10, 6, 0.97f}, {10, 6, 0.97f}, true}, // the tube's soldier LOD
				{{-10, 3.2f, 5}, {10, 3.2f, 5}, false}}},
		{"commander_emp_strike", 10, 907 + 358 + 596,
			{{{-10, 3.9f, 0}, {10, 3.9f, 0}, true}, // the mount, above the root's projectile LOD
				{{-10, 4.9f, 0.4f}, {10, 4.9f, 0.4f}, true}}, // the dish, above the mount (4.46 m)
			{}},
		{"commander_orbital_strike_64", 30, 448 + 2327, {{{-10, 4, 0}, {10, 4, 0}, true}}, {}}, // the antenna
	};
	for (const std::string& mod : {std::string("mods/bf2142"), std::string("mods/Project_Remaster_v17_Final")}) {
		if (!test::runs(mod)) continue;
		for (const Asset& asset : assets) {
			for (const float yaw : {0.0f, 90.0f}) {
				Field field(mod, true);
				if (!field.loaded) {
					CHECK(field.loaded);
					continue;
				}
				const world::Object* placed = field.objects.create(field.templates.find(asset.name), {0, 0, 0}, {yaw, 0, 0});
				field.world->tick(1 / 30.0f);
				CHECK(field.world->projectileCollision(placed->id()) == nullptr); // static, not a body
				const size_t soldierTriangles = field.world->staticCollision().triangles();
				const size_t projectileTriangles = field.world->projectileCollision(0)->triangles();
				std::fprintf(stderr, "  %s/%s yaw %.0f: soldier %zu, projectile %zu triangles\n", mod.c_str(), asset.name, yaw,
					soldierTriangles, projectileTriangles);
				CHECK(soldierTriangles == asset.soldierTriangles);
				CHECK(projectileTriangles == asset.projectileTriangles);
				const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({yaw, 0, 0});
				for (const Probe& probe : asset.projectile) {
					float fraction = 0;
					uint32_t owner = 0;
					const bool hit =
						field.world->segmentHit(turn.transform(probe.from), turn.transform(probe.to), 0, fraction, owner);
					std::fprintf(stderr, "    projectile y %.1f z %.2f: hit %d at %.2f\n", probe.from.y, probe.from.z, hit, fraction);
					CHECK(hit == probe.hits);
					CHECK(!probe.hits || owner == placed->id());
					// A round flown down the same line strikes the part (or passes).
					world::ObjectTemplate* round = field.templates.find("collision_test_round");
					if (!round) {
						round = field.templates.create("GenericProjectile", "collision_test_round");
						round->settings()["gravityModifier"] = "0";
					}
					const size_t before = field.world->impacts().size();
					const physics::Vec3 way = turn.transform(probe.to - probe.from);
					CHECK(field.world->launchProjectile("collision_test_round", turn.transform(probe.from),
						way * (300.0f / std::sqrt(physics::lengthSquared(way)))));
					for (int i = 0; i < 6; ++i) {
						field.world->tick(1 / 30.0f);
					}
					bool struck = false;
					for (size_t i = before; i < field.world->impacts().size(); ++i) {
						struck = struck || field.world->impacts()[i].object == placed->id();
					}
					std::fprintf(stderr, "    round y %.1f z %.2f: struck %d\n", probe.from.y, probe.from.z, struck);
					CHECK(struck == probe.hits);
				}
				for (const Probe& probe : asset.soldier) {
					physics::CollisionWorld::Hit hit {};
					const bool met = field.world->staticCollision().raycast(turn.transform(probe.from), turn.transform(probe.to), hit);
					std::fprintf(stderr, "    soldier y %.1f z %.2f: hit %d\n", probe.from.y, probe.from.z, met);
					CHECK(met == probe.hits);
					CHECK(!probe.hits || hit.owner == placed->id());
				}
			}
		}
	}
}

// A static made mid-round and then deleted (a spawner's swap, a commander
// asset's end) takes its collision with it, as retail's goes with the
// object (the object manager's delete 0x006fdd90 -> slot 0x108, 0x006fa730):
// no invisible wall where it stood, and placing it again and again leaves
// one copy of its faces, not a pile.
TEST(Commander_DeletedStaticLeavesNoCollision)
{
	if (!std::getenv("BF2142_GAME_DIR")) {
		return;
	}
	Field field("mods/bf2142", true);
	CHECK(field.loaded);
	if (!field.loaded || !field.templates.find("missile_silo_v2")) {
		return;
	}
	field.world->tick(1 / 30.0f);
	const size_t soldierBase = field.world->staticCollision().triangles();
	const size_t projectileBase = field.world->projectileCollision(0)->triangles();
	const size_t partBase = field.world->staticPartCount();
	const physics::Vec3 from {-10, 8, 0.97f}, to {10, 8, 0.97f}; // through the upright tube
	for (int round = 0; round < 5; ++round) {
		const world::Object* placed = field.objects.create(field.templates.find("missile_silo_v2"), {0, 0, 0}, {0, 0, 0});
		const uint32_t id = placed->id();
		field.world->tick(1 / 30.0f);
		float fraction = 0;
		uint32_t owner = 0;
		CHECK(field.world->segmentHit(from, to, 0, fraction, owner) && owner == id);
		physics::CollisionWorld::Hit hit {};
		CHECK(field.world->staticCollision().raycast({-10, 6, 0.97f}, {10, 6, 0.97f}, hit) && hit.owner == id);
		CHECK(field.world->staticCollision().triangles() == soldierBase + 92 + 60 + 12);
		CHECK(field.world->projectileCollision(0)->triangles() == projectileBase + 2264 + 532 + 244);
		CHECK(field.world->staticPartCount() > partBase);
		field.objects.remove(id);
		field.world->tick(1 / 30.0f);
		std::fprintf(stderr, "  placing %d: soldier %zu, projectile %zu faces, %zu parts after the delete\n", round,
			field.world->staticCollision().triangles(), field.world->projectileCollision(0)->triangles(),
			field.world->staticPartCount());
		CHECK(!field.world->segmentHit(from, to, 0, fraction, owner));
		CHECK(!field.world->staticCollision().raycast({-10, 6, 0.97f}, {10, 6, 0.97f}, hit));
		CHECK(field.world->staticCollision().triangles() == soldierBase);
		CHECK(field.world->projectileCollision(0)->triangles() == projectileBase);
		CHECK(field.world->staticPartCount() == partBase); // the mesh physics' parts too
	}
}

// The EMP station and the orbital controller stop a running soldier from
// each side, in both mods: their soldier shapes are the root's (geometry 0,
// up to the dish and antenna; the child parts author no soldier LOD).
TEST(Commander_StrikeStationsBlockInfantry)
{
	if (!std::getenv("BF2142_GAME_DIR")) {
		return;
	}
	for (const std::string& mod : {std::string("mods/bf2142"), std::string("mods/Project_Remaster_v17_Final")}) {
		if (!test::runs(mod)) continue;
		for (const char* name : {"commander_emp_strike", "commander_orbital_strike_64"}) {
			for (int side = 0; side < 4; ++side) {
				Field field(mod, true);
				if (!field.loaded) {
					CHECK(field.loaded);
					continue;
				}
				field.objects.create(field.templates.find(name), {0, 0, 0}, {90, 0, 0});
				field.world->tick(1 / 30.0f);
				CHECK(field.world->spawnPlayer(2) != nullptr);
				const int player = field.world->localPlayer();
				game::GameWorld::PlayerInfo info;
				CHECK(field.world->playerInfo(player, info) && info.alive);
				const float heading = side * 90.0f; // degrees from +z toward +x
				const physics::Vec3 way {std::sin(heading / 57.29578f), 0, std::cos(heading / 57.29578f)};
				field.world->setSoldierPosition(info.soldier, {-way.x * 12, 0, -way.z * 12});
				game::SoldierInput walk;
				walk.throttle = 1;
				walk.sprint = true;
				walk.turn = heading - info.yaw;
				for (int i = 0; i < 150; ++i) {
					field.world->setInput(player, walk);
					walk.turn = 0;
					field.world->tick(1 / 30.0f);
				}
				CHECK(field.world->playerInfo(player, info));
				const float along = info.position.x * way.x + info.position.z * way.z; // 0 at the centre
				std::fprintf(stderr, "  %s/%s side %d: along %.2f at %.2f %.2f %.2f\n", mod.c_str(), name, side, along,
					info.position.x, info.position.y, info.position.z);
				CHECK(along < -0.9f);
			}
		}
	}
}

TEST(Commander_OnePerTeam)
{
	Field field;
	game::GameWorld& world = *field.world;
	const int a = world.addPlayer("A", 2, false);
	const int b = world.addPlayer("B", 2, true);
	const int c = world.addPlayer("C", 1, true);
	// Only once the game mode enables it (sh_setEnableCommander).
	CHECK(!world.setCommander(2, a) && world.commander(2) == -1);
	world.setCommanderEnabled(true);
	// Applications wait out the apply period (15 s), then the
	// highest-ranked applicant is offered command and answers.
	world.joinSquad(a, world.openSquad(2));
	const int d = world.addPlayer("D", 2, false);
	world.setPlayerRank(d, 3);
	world.setPlayerRank(a, 3);
	world.applyForCommander(a);
	field.run(5);
	world.applyForCommander(d);
	world.applyForCommander(b);
	field.run(9.8f);
	CHECK(world.commander(2) == -1 && world.commanderOffer(2) == -1);
	field.run(0.3f);
	CHECK(world.commanderOffer(2) == a && world.commander(2) == -1); // the first of the highest
	// Declined: the next best at once.
	world.answerCommanderOffer(a, false);
	CHECK(world.commanderOffer(2) == d);
	world.answerCommanderOffer(b, true); // not offered: nothing
	CHECK(world.commander(2) == -1);
	world.answerCommanderOffer(d, true);
	CHECK(world.commander(2) == d && world.commander(1) == -1 && world.commanderOffer(2) == -1);
	game::GameWorld::PlayerInfo info;
	CHECK(world.playerInfo(d, info) && info.commander);
	CHECK(!field.events.empty() && field.events.back().first == "ChangedCommander");
	CHECK(field.events.back().second.size() == 3 && field.events.back().second[0].asInt() == 2
		&& field.events.back().second[1].kind == game::ScriptValue::Kind::None
		&& field.events.back().second[2].kind == game::ScriptValue::Kind::Player && field.events.back().second[2].i == d);
	// The commander leaves its squad.
	world.setCommander(2, a);
	CHECK(world.playerInfo(a, info) && info.commander && info.squad == 0);
	CHECK(!world.joinSquad(a, 1));
	// Not across teams.
	CHECK(!world.setCommander(1, a) && world.commander(1) == -1);
	CHECK(world.setCommander(1, c) && world.commander(1) == c);
	// Taking command drops the applicants (POSFILLED); after a
	// resignation the next applicant is offered command 10 s later.
	world.applyForCommander(b);
	world.resignCommander(a);
	CHECK(world.commander(2) == -1 && world.playerInfo(a, info) && !info.commander);
	field.run(9.9f);
	CHECK(world.commanderOffer(2) == -1);
	field.run(0.2f);
	CHECK(world.commanderOffer(2) == b);
	world.answerCommanderOffer(b, true);
	CHECK(world.commander(2) == b);
	// Disabled, nobody commands.
	world.setCommanderEnabled(false);
	CHECK(world.commander(1) == -1 && world.commander(2) == -1);
}

TEST(Commander_OrdersAndBots)
{
	Field field;
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	const int boss = world.addPlayer("Boss", 2, false);
	world.setCommander(2, boss);
	const int leader = world.addPlayer("Lead", 2, true);
	const int member = world.addPlayer("Member", 2, true);
	world.joinSquad(leader, 1);
	world.joinSquad(member, 1);
	CHECK(world.spawnSoldier(leader) && world.spawnSoldier(member));
	// Only the commander orders, and only squads that exist.
	// (Beyond the order's 80 m, orderRadius, so they set off.)
	CHECK(!world.issueOrder(2, 1, game::GameWorld::OrderType::MapDefend, 0, 160, leader));
	CHECK(!world.issueOrder(2, 4, game::GameWorld::OrderType::MapDefend, 0, 160, boss));
	CHECK(world.issueOrder(2, 1, game::GameWorld::OrderType::MapDefend, 0, 160, boss));
	const game::GameWorld::SquadOrder* order = world.squadOrder(2, 1);
	CHECK(order && order->type == game::GameWorld::OrderType::MapDefend && order->position.z == 160 && !order->accepted
		&& order->radius == 80);
	const uint32_t firstSerial = order->serial;
	world.acceptOrder(member, true); // not the leader's
	CHECK(!world.squadOrder(2, 1)->accepted);
	world.acceptOrder(leader, true);
	CHECK(world.squadOrder(2, 1)->accepted);
	// The squad's bots make for the order rather than the enemy's point
	// (100 m the other way). Their walk needs the game's templates (the
	// soldier and its kit, $BF2142_GAME_DIR): without them it is skipped.
	CHECK(field.loaded || !std::getenv("BF2142_GAME_DIR"));
	if (field.loaded) {
		Console console;
		game::BotSettings settings;
		game::registerBotConsoleMethods(console, settings);
		game::BotManager bots(world, settings);
		for (int i = 0; i < 30 * 8; ++i) { // the member follows its slot beside the leader (AIDLL 0x100c0890)
			bots.update(1 / 30.0f);
			world.tick(1 / 30.0f);
		}
		game::GameWorld::PlayerInfo info;
		CHECK(world.playerInfo(member, info) && info.position.z > 5 && std::fabs(info.position.x) < 10); // beside its leader in the line (AIDLL 0x100f53a0: 6.9 m)
	}
	// An identical order issued again is a new one (0x004c5530 sends each):
	// its serial moves on and the leader's answer is asked for again.
	CHECK(world.issueOrder(2, 1, game::GameWorld::OrderType::MapDefend, 0, 160, boss));
	CHECK(world.squadOrder(2, 1)->serial > firstSerial && !world.squadOrder(2, 1)->accepted);
	// The commander's map node (a local commander): the leader's request state, 0 for no member.
	// (a commander on another machine keeps none here: the host relays it as 2/0x19)
	CHECK(world.requestNewOrder(leader, 2, 1, 1) && world.squadOrderState(2, 1) == 0);
	// The leader's answer to the question (0x0079d6e0 case 2): no clears the
	// commander's order; yes passes it on as the leader's own and clears it.
	world.issueSquadLeaderOrder(leader, game::GameWorld::OrderType::None, 0, 0); // (the bots' own, above)
	world.answerOrder(member, true); // not the leader's
	CHECK(world.squadOrder(2, 1) && !world.squadLeaderOrder(2, 1));
	world.answerOrder(leader, false);
	CHECK(!world.squadOrder(2, 1) && !world.squadLeaderOrder(2, 1));
	CHECK(world.issueOrder(2, 1, game::GameWorld::OrderType::MapDefend, 0, 160, boss));
	world.answerOrder(leader, true);
	const game::GameWorld::SquadOrder* passedOn = world.squadLeaderOrder(2, 1);
	CHECK(!world.squadOrder(2, 1) && passedOn && passedOn->type == game::GameWorld::OrderType::MapDefend
		&& passedOn->position.z == 160 && !passedOn->fromCommander);
	// Turned down, it goes.
	CHECK(world.issueOrder(2, 1, game::GameWorld::OrderType::MapDefend, 0, 160, boss));
	world.acceptOrder(leader, false);
	CHECK(world.squadOrder(2, 1) == nullptr);
}

TEST(Commander_BotsNeverApply)
{
	Field field;
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	world.addPlayer("Bot", 1, true);
	const int other = world.addPlayer("Other", 1, true);
	world.joinSquad(other, 1);
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	game::BotManager bots(world, settings);
	for (int i = 0; i < 30 * 20; ++i) {
		bots.update(1 / 30.0f);
		world.tick(1 / 30.0f);
	}
	// AIDLL's commander is the team's AI, not a player.
	CHECK(world.commander(1) == -1 && world.commander(2) == -1 && world.commanderOffer(1) == -1);
}

TEST(Commander_OrbitalStrike)
{
	Field field;
	CHECK(field.loaded || !std::getenv("BF2142_GAME_DIR"));
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	const int boss = world.addPlayer("Boss", 2, false);
	world.setCommander(2, boss);
	// No asset, no strike.
	CHECK(!world.abilityState(2, Ability::OrbitalStrike).available);
	CHECK(!world.commanderAction(2, Ability::OrbitalStrike, 0, 50));
	const uint32_t station = field.place("commander_emp_strike", 2, -200, -200);
	game::GameWorld::CommanderAbilityState state = world.abilityState(2, Ability::OrbitalStrike);
	CHECK(state.available && state.ready && state.asset == station && state.armor == 1);
	CHECK(!world.abilityState(1, Ability::OrbitalStrike).available); // EU's

	const int near = field.soldier(1, 0, 50);
	const int edge = field.soldier(1, 10, 50);   // within explosionRadius 15
	const int far = field.soldier(1, 0, 80);
	const int mate = field.soldier(2, 0, 120);
	CHECK(world.commanderAction(2, Ability::OrbitalStrike, 0, 50));
	CHECK(world.strikes().size() == 1);
	// Recharging: minTimeBetweenActivation 115 s.
	state = world.abilityState(2, Ability::OrbitalStrike);
	CHECK(!state.ready && std::fabs(state.timeUntilReady - 115) < 0.1f && state.active);
	CHECK(!world.commanderAction(2, Ability::OrbitalStrike, 0, 80));
	// The launch delay (fireLaunchDelay 1.5, 2 whole seconds) and the fall
	// from orbitalBombardmentHeight (500 m at 100 m/s, five times gravity):
	// nothing yet after 3 s, the blast by 6.
	field.run(3);
	CHECK(field.alive(near) && field.alive(edge));
	// The firearm fired once, 500 m over the target: its event 0 goes to
	// GEB_emp_strike there (the effects layer, ObjectEffects::weaponFiredAt).
	const auto fired = world.takeStrikeFires();
	CHECK(fired.size() == 1);
	if (fired.size() == 1) {
		std::string name = fired[0].fireArm;
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
		CHECK(fired[0].asset == station && name == "commander_emp_strike_genericfirearm" && fired[0].first);
		CHECK(std::fabs(fired[0].at.x) < 1e-3f && std::fabs(fired[0].at.z - 50) < 1e-3f
			&& std::fabs(fired[0].at.y - world.terrainHeight(0, 50) - 500) < 1);
	}
	CHECK(world.takeStrikeFires().empty());
	field.run(3);
	// Its projectile (damage 3000) kills the soldier it lands on; its blast
	// is an EMP (detonation.isEMPWeapon: the detonation hands it to the
	// game logic's EMP, 0x005a7ab0), which hurts nobody.
	CHECK(!field.alive(near) && field.alive(edge) && field.alive(far) && field.alive(mate));
	game::GameWorld::PlayerInfo info;
	CHECK(world.playerInfo(boss, info) && info.kills == 1); // the commander's kill
	CHECK(world.strikes().empty());
	// Ready again after 115 s, not before.
	field.run(105);
	CHECK(!world.commanderAction(2, Ability::OrbitalStrike, 0, 80));
	field.run(5);
	CHECK(world.abilityReady(2, Ability::OrbitalStrike));
	// A wrecked station disables it (armour 1000, canBeDestroyed 0: a
	// wreck until repaired).
	world.setHitPoints(station, 0);
	CHECK(field.objects.find(station) && !world.abilityState(2, Ability::OrbitalStrike).available);
	CHECK(!world.commanderAction(2, Ability::OrbitalStrike, 0, 80));
	world.setHitPoints(station, 500);
	CHECK(world.abilityState(2, Ability::OrbitalStrike).available && world.abilityState(2, Ability::OrbitalStrike).armor == 0.5f);
}

TEST(Commander_Artillery)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	world.setCommander(1, world.addPlayer("Boss", 1, false));
	field.place("commander_orbital_strike_64", 1, 200, 200);
	CHECK(world.abilityReady(1, Ability::Artillery) && !world.abilityState(1, Ability::OrbitalStrike).available);
	const int target = field.soldier(2, 0, -60);
	CHECK(world.commanderAction(1, Ability::Artillery, 0, -60));
	// 16 rounds of 7 shells (fire.batchSize) a second (60 a minute), no
	// launch delay.
	CHECK(world.strikes().size() == 1 && world.strikes()[0].salvosLeft == 16);
	field.run(0.1f);
	CHECK(world.strikes()[0].salvosLeft == 15 && world.projectilesInFlight() == 7);
	// Each shell strays (SimpleDeviationComp, 0x005f5540): subProjectileDev
	// 45 x 0.01 x the firearm's velocity 10 = 4.5 m/s, one standard
	// deviation on each axis across its fall (the main one by the last
	// sub's); the barrage lands round the point.
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(target, info);
	const uint32_t soldier = info.soldier;
	field.run(20);
	std::vector<float> landed; // the last 64 (the world keeps that many)
	for (const auto& impact : world.impacts()) {
		landed.push_back(std::sqrt(impact.point.x * impact.point.x + (impact.point.z + 60) * (impact.point.z + 60)));
	}
	std::sort(landed.begin(), landed.end());
	const float median = landed.empty() ? -1 : landed[landed.size() / 2];
	std::printf("  the last %zu shells landed a median %.1f m from the point, the nearest %.1f m\n", landed.size(), median,
		landed.empty() ? -1.0f : landed.front());
	CHECK(world.strikes().empty() && landed.size() == 64);
	CHECK(median > 3 && median < 60);
	CHECK(!field.alive(target) || world.hitPoints(soldier) < 100);
	CHECK(std::fabs(world.abilityState(1, Ability::Artillery).timeUntilReady - (85 - 20.1f)) < 0.2f);
}

TEST(Commander_UavAndScanReveal)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	world.setCommander(2, world.addPlayer("Boss", 2, false));
	const int inside = field.soldier(1, 300, 330);
	const int outside = field.soldier(1, 300, 450);
	CHECK(!world.commanderAction(2, Ability::Uav, 300, 300)); // no UAV trailer
	field.place("commander_uav_drone_control", 2, -200, -200);
	if (!field.templates.find("eu_drone")) {
		field.templates.registerType(9, "UAVVehicle");
		field.templates.create("UAVVehicle", "eu_drone")->settings()["armor.maxHitPoints"] = "1000";
	}
	CHECK(world.commanderAction(2, Ability::Uav, 300, 300));
	// UAVControlObject: eu_drone at x + 40 (uavVehicleRadius), 70 m up
	// (uavVehicleFlightHeight over flat ground), circling at 14 m/s with
	// asin(14 / 1200) x 57.29578 x 0.5263158 = 0.3518 rad/s: 39.8 m round.
	const uint32_t drone = world.uavDrone(2);
	const world::Object* flying = field.objects.find(drone);
	CHECK(drone != 0 && flying && std::fabs(flying->position().x - 340) < 1e-3f && std::fabs(flying->position().y - 70) < 1e-3f);
	field.run(5);
	flying = field.objects.find(drone);
	if (flying) {
		const float rate = std::asin(14.0f / 1200) * 57.29578f * 0.5263158f;
		const float circle = 14 / rate;
		const float dx = flying->position().x - (340 - circle), dz = flying->position().z - 300;
		CHECK(std::fabs(std::sqrt(dx * dx + dz * dz) - circle) < 0.01f && std::fabs(circle - 39.79f) < 0.05f);
		CHECK(dz > 0); // round from +x towards +z
		CHECK(std::fabs(std::atan2(dz, dx) - rate * 5.0f) < 0.02f);
	}
	physics::Vec3 at;
	float radius = 0;
	CHECK(world.uavArea(2, at, radius) && at.x == 300 && radius == 100);
	CHECK(world.isRevealed(inside, 2) && !world.isRevealed(outside, 2) && !world.isRevealed(inside, 1));
	// It stays 30 s and is ready again 5 s later.
	field.run(24);
	CHECK(world.isRevealed(inside, 2) && world.uavDrone(2) == drone);
	field.run(1.5f);
	CHECK(!world.isRevealed(inside, 2) && !world.uavArea(2, at, radius) && !world.abilityReady(2, Ability::Uav));
	CHECK(world.uavDrone(2) == 0 && !field.objects.find(drone)); // gone with the UAV's time
	field.run(5);
	CHECK(world.abilityReady(2, Ability::Uav));
	// The satellite scan (commander_movement_analyst) shows everyone for
	// 11 s, to the commander only.
	field.place("commander_movement_analyst", 2, -220, -200);
	const int mate = field.soldier(2, -300, -300);
	CHECK(!world.abilityState(2, Ability::Satellite).satelliteSweep);
	CHECK(world.commanderAction(2, Ability::Satellite, 0, 0));
	CHECK(world.abilityState(2, Ability::Satellite).satelliteSweep == 0.0f);
	field.run(0.1f);
	const std::vector<int> seen = world.scannedPlayers(world.commander(2));
	CHECK(seen.size() == 2 && world.scannedPlayers(mate).empty() && !world.isRevealed(outside, 2));
	field.run(5.4f);
	const auto halfway = world.abilityState(2, Ability::Satellite).satelliteSweep;
	CHECK(halfway && std::abs(*halfway - 0.5f) < 0.01f);
	CHECK(!world.abilityState(1, Ability::Satellite).satelliteSweep);
	field.run(5.6f);
	CHECK(world.scannedPlayers(world.commander(2)).empty());
	CHECK(!world.abilityState(2, Ability::Satellite).satelliteSweep);
	CHECK(std::fabs(world.abilityState(2, Ability::Satellite).timeUntilReady - (27 - 11.1f)) < 0.2f);
}

TEST(Commander_SupplyDrop)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	world.setCommander(2, world.addPlayer("Boss", 2, false));
	// No asset needed.
	CHECK(world.abilityReady(2, Ability::Supplies));
	const int hurt = field.soldier(2, 40, 40);
	const int enemy = field.soldier(1, 41, 40);
	game::GameWorld::PlayerInfo info;
	world.playerInfo(hurt, info);
	world.setHitPoints(info.soldier, 40);
	world.playerInfo(enemy, info);
	world.setHitPoints(info.soldier, 40);
	CHECK(world.commanderAction(2, Ability::Supplies, 40, 41));
	CHECK(!world.commanderAction(2, Ability::Supplies, 40, 41)); // 60 s to the next
	CHECK(world.supplyCrates().size() == 1);
	const uint32_t crate = world.supplyCrates()[0];
	CHECK(field.objects.find(crate)->position().y == 50); // supplyDropHeight
	// It comes down, then heals its team (healSpeed 3 a second) within 5 m.
	field.run(12);
	CHECK(field.objects.find(crate)->position().y < 1.0f);
	CHECK(field.objects.find(crate)->childDisabled("supply_parachute")); // down (0x0043d9c0)
	world.playerInfo(hurt, info);
	const float before = world.hitPoints(info.soldier);
	field.run(5);
	const float after = world.hitPoints(info.soldier);
	CHECK(before > 40 && after > before + 14 && after < before + 16);
	world.playerInfo(enemy, info);
	CHECK(world.hitPoints(info.soldier) == 40);
	field.run(45);
	CHECK(world.abilityReady(2, Ability::Supplies));
	// Gone supplyDropNumSecsToLive (300 s) after it landed, 10 s after the
	// drop at 5 m/s (0x0043d820 / 0x0043d880), not after the drop.
	field.run(240);
	CHECK(world.supplyCrates().size() == 1);
	field.run(10);
TEST(Commander_UavOrbitKeepsItsCollisionMesh)
{
	if (!std::getenv("BF2142_GAME_DIR")) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		Field field(mod, true, nullptr, true);
		CHECK(field.loaded);
		std::string output;
		field.console.run("Objects/Weapons/stationary/eu_drone/eu_drone.con", {}, output);
		CHECK(field.templates.find("eu_drone"));
		if (!field.loaded || !field.templates.find("eu_drone")) continue;

		auto& world = *field.world;
		world.setCommanderEnabled(true);
		world.setCommander(2, world.addPlayer("Commander", 2, false));
		field.place("commander_uav_drone_control", 2, -200, -200);
		CHECK(world.commanderAction(2, Ability::Uav, 300, 300));
		const uint32_t drone = world.uavDrone(2);
		CHECK(drone != 0);
		field.run(1);
		const auto* object = field.objects.find(drone);
		CHECK(object && world.projectileCollision(drone));
		if (!object) continue;
		const auto at = object->position();
		CHECK(std::fabs(at.y - 70) < 0.01f && at.z > 313 && at.z < 314);
		std::vector<physics::SoldierBody::Mover> movers;
		world.soldierMovers({at.x, at.y, at.z}, 1, 0, 0, movers);
		CHECK(std::any_of(movers.begin(), movers.end(), [drone](const auto& mover) {
			return mover.owner == drone && std::fabs(mover.position.y - 70) < 0.01f
				&& std::fabs(std::sqrt(physics::lengthSquared(mover.velocity)) - 14) < 0.01f;
		}));

		// An ordinary small-arms hit through the authored projectile LOD.
		auto& round = field.templates.create("GenericProjectile", "uav_orbit_test_bullet")->settings();
		round["material"] = "38";
		round["damage"] = "30";
		round["timeToLive"] = "1";
		round["gravityModifier"] = "0";
		CHECK(world.launchProjectile("uav_orbit_test_bullet", {at.x, at.y + 10, at.z}, {0, -900, 0}));
		field.run(0.1f);
		CHECK(world.hitPoints(drone) == 970);
		field.run(4);
		object = field.objects.find(drone);
		CHECK(object && std::fabs(object->position().y - 70) < 0.01f);
	}
}

	CHECK(world.supplyCrates().empty() && field.objects.find(crate) == nullptr);
}

TEST(Commander_MapMarkersUseOwningTeamAndToolTint)
{
	if (!std::getenv("BF2142_GAME_DIR")) {
		return;
	}
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) {
			continue;
		}
		Field field(mod);
		CHECK(field.loaded);
		if (!field.loaded) {
			continue;
		}
		std::string consoleOutput;
		field.console.run("Objects/Weapons/stationary/eu_drone/eu_drone.con", {}, consoleOutput);
		CHECK(field.templates.find("eu_drone"));
		auto& world = *field.world;
		world.setCommanderEnabled(true);
		for (int team : {1, 2}) {
			world.setCommander(team, world.addPlayer("Boss", team, false));
			field.place("commander_emp_strike", team, team * 100.0f, -100);
			field.place("commander_orbital_strike_64", team, team * 100.0f, -120);
			field.place("commander_uav_drone_control", team, team * 100.0f, -140);
		}
		for (int team : {1, 2}) {
			const float x = team * 100.0f;
			CHECK(world.commanderAction(team, Ability::Uav, x, 20));
			const world::Object* drone = field.objects.find(world.uavDrone(team));
			CHECK(drone && drone->team() == team);
			CHECK(world.commanderAction(team, Ability::OrbitalStrike, x, 40));
			CHECK(world.commanderAction(team, Ability::Artillery, x, 60));
			CHECK(world.commanderAction(team, Ability::Supplies, x, 80));
		}

		using Marker = hud::HudManager::MapMarker;
		for (int team : {1, 2}) {
			std::vector<Marker> marks;
			dice::hfe::main::appendCommanderMapMarkers(marks, world, team);
			CHECK(marks.size() == 7);
			if (marks.size() != 7) {
				continue;
			}
			for (const Marker& mark : marks) {
				CHECK(mark.x == team * 100.0f && mark.upright);
			}
			CHECK(marks[0].texture == "Ingame/Minimap/icon_uav_circle.tga" && marks[0].radar && marks[0].worldSize > 0);
			CHECK(marks[0].color[0] == 1 && marks[0].color[1] == 1 && marks[0].color[2] == 1);
			CHECK(marks[2].texture == "Ingame/Commander/tools/orbit_strike.tga");
			CHECK(marks[4].texture == "Ingame/Commander/tools/force_strike.tga");
			for (size_t i = 1; i <= 4; ++i) {
				CHECK(marks[i].color[0] == 1 && marks[i].color[1] == 0 && marks[i].color[2] == 0);
			}
			CHECK(marks[6].texture == "Ingame/Minimap/Icons/icon_supplies.tga");
			for (size_t i : {5, 6}) {
				CHECK(marks[i].color[0] == 0.254f && marks[i].color[1] == 0.552f && marks[i].color[2] == 0.89f);
			}
		}

		std::vector<Marker> marks;
		dice::hfe::main::appendCommanderMapMarkers(marks, world, 0);
		CHECK(marks.empty());
		field.run(3);
		for (int team : {1, 2}) {
			for (float z : {40.0f, 60.0f}) {
				CHECK(std::any_of(world.projectiles().begin(), world.projectiles().end(), [team, z](const game::Projectile& shot) {
					return shot.team == team && shot.origin.x == team * 100.0f && shot.origin.z == z;
				}));
			}
		}
		field.run(23);
		CHECK(world.supplyCrates().size() == 2);
		dice::hfe::main::appendCommanderMapMarkers(marks, world, 1);
		CHECK(marks.size() == 1 && marks.front().texture == "Ingame/Minimap/icon_uav_circle.tga");
	}
}

TEST(Commander_OrderKindsAndLeaderOrders)
{
	using Order = game::GameWorld::OrderType;
	// The engine's kinds (HudManager slot 0x3f4): 0-3 from the issue-order
	// menu, 4-7 placed on the map, 10 and up drop the armed order.
	CHECK(game::GameWorld::orderTypeFromKind(0) == Order::Move && game::GameWorld::orderTypeFromKind(1) == Order::Attack
		&& game::GameWorld::orderTypeFromKind(2) == Order::Defend && game::GameWorld::orderTypeFromKind(7) == Order::MapRepair
		&& game::GameWorld::orderTypeFromKind(10) == Order::None);
	// Only 5 and 6 count as attack or defend for the scripts.
	CHECK(game::GameWorld::isAttackOrDefend(Order::MapAttack) && game::GameWorld::isAttackOrDefend(Order::MapDefend)
		&& !game::GameWorld::isAttackOrDefend(Order::Attack) && !game::GameWorld::isAttackOrDefend(Order::MapMove));
	Field field;
	game::GameWorld& world = *field.world;
	const int leader = world.addPlayer("Lead", 2, false);
	const int member = world.addPlayer("Member", 2, true);
	world.joinSquad(leader, 1);
	world.joinSquad(member, 1);
	// A squad leader orders his own squad, accepted at once; a member can't.
	CHECK(!world.issueSquadLeaderOrder(member, Order::MapAttack, 10, 20));
	CHECK(world.issueSquadLeaderOrder(leader, Order::MapAttack, 10, 20));
	const game::GameWorld::SquadOrder* order = world.squadLeaderOrder(2, 1);
	CHECK(order && order->type == Order::MapAttack && order->accepted && !order->fromCommander && order->position.z == 20);
	CHECK(world.squadOrder(2, 1) == nullptr); // apart from the commander's
	CHECK(world.issueSquadLeaderOrder(leader, Order::None, 0, 0) && world.squadLeaderOrder(2, 1) == nullptr);
}

TEST(Commander_AiUsesToolsWithoutCommander)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	field.place("commander_orbital_strike_64", 1, 200, 200);
	// Nobody commands: a player's request is refused, the game's own
	// (IBFCommander, no sender) goes through.
	CHECK(world.commander(1) == -1 && !world.commanderAction(1, Ability::Artillery, 0, -60));
	CHECK(world.commanderAction(1, Ability::Artillery, 0, -60, true) && world.strikes().size() == 1);
}

// The squad leaders' requests (SquadLeader.sendRequest → 0x007d6d20, the
// store 0x004c5720) and the commander's answers (0x007c7e20), the 4 s
// auto-deny of a tool that can't be used (0x007c8a60 / 0x007c7a00).
TEST(Commander_SquadRequests)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	// The commander's lines to the leaders are his radio on TCPlayer
	// (0x007c7810: the client's slot 0x50), handed out by the server's
	// rule (0x004a93b0, 0x004a6f70 case 0xe): a dead one's reach no living
	// leader, so he is on the field.
	const int boss = world.addPlayer("Boss", 2, false);
	world.spawnSoldier(boss);
	const int leader = field.soldier(2, 10, 10);
	const int other = field.soldier(2, 30, 10);
	world.joinSquad(leader, 1);
	world.joinSquad(other, 2);
	menu::SquadRequests requests(world);
	std::vector<std::string> personal, info;
	requests.onPersonal = [&](const std::string& s) { personal.push_back(s); };
	requests.onInfo = [&](const std::string& s) { info.push_back(s); };
	requests.localPlayer = boss;
	// Without a commander: refused, the leader alone hears why.
	world.takeRadioHeard();
	CHECK(!requests.send(leader, 3, 40, 41));
	auto heard = world.takeRadioHeard();
	CHECK(heard.size() == 1 && heard[0].voice == "no_commander_available" && heard[0].receiver == leader);
	world.setCommander(2, boss);
	// Supplies (kind 3): stored, the leader's radio to the commander, the
	// commander's line (the tool is ready).
	CHECK(requests.send(leader, 3, 40, 41) && world.squadRequest(2, 1, 3));
	heard = world.takeRadioHeard();
	CHECK(!heard.empty() && heard[0].voice == "PLAYER_REQUEST_requestsupplies" && heard[0].receiver == boss);
	CHECK(info.size() == 1 && info[0] == "HUD_HUD_REQUEST_RECIEVED_FROM_SQUAD");
	CHECK(requests.send(other, 3, 30, 31) && world.squadRequests(2).size() == 2);
	world.takeRadioHeard();
	// Accepted: the drop at the request's point, "supply_accept" to its
	// leader, "supply_deny" to the other squad's; both taken off.
	CHECK(requests.answer(boss, 1, 3, true) == menu::SquadRequests::Answer::Accepted);
	CHECK(world.supplyCrates().size() == 1 && world.squadRequests(2).empty());
	heard = world.takeRadioHeard();
	bool accepted = false, denied = false;
	for (const auto& h : heard) {
		accepted = accepted || (h.voice == "supply_accept" && h.receiver == leader);
		denied = denied || (h.voice == "supply_deny" && h.receiver == other);
	}
	CHECK(accepted && denied);
	// Again: the drop reloads, so accepting fails and the request stays.
	CHECK(requests.send(leader, 3, 40, 41));
	world.takeRadioHeard();
	CHECK(requests.answer(boss, 1, 3, true) == menu::SquadRequests::Answer::Unavailable && world.squadRequest(2, 1, 3));
	heard = world.takeRadioHeard();
	CHECK(heard.size() == 1 && heard[0].voice == "supply_unavailable" && heard[0].receiver == boss);
	CHECK(requests.answer(boss, 1, 3, false) == menu::SquadRequests::Answer::Denied && !world.squadRequest(2, 1, 3));
	// Artillery with no asset: no line for the commander, and 4 s later
	// it goes by itself with "artillery_unavailable" to the leader.
	info.clear();
	CHECK(requests.send(leader, 0, 40, 41) && info.empty());
	world.takeRadioHeard();
	requests.tick(3.9f);
	CHECK(world.squadRequest(2, 1, 0));
	requests.tick(0.2f);
	heard = world.takeRadioHeard();
	CHECK(!world.squadRequest(2, 1, 0) && heard.size() == 1 && heard[0].voice == "artillery_unavailable"
		&& heard[0].receiver == leader);
	// At most six requests until 30 s after the first (sv.radio* defaults);
	// eight sent so far would be refused had they not reset: reset now.
	requests.tick(31);
	int stored = 0;
	for (int i = 0; i < 8; ++i) {
		stored += requests.send(leader, 2, 40, 41) ? 1 : 0;
	}
	CHECK(stored == 6);
	// A dead leader can't request.
	game::GameWorld::PlayerInfo me;
	world.playerInfo(leader, me);
	world.setHitPoints(me.soldier, 0);
	field.run(0.1f);
	requests.tick(31);
	personal.clear();
	CHECK(!requests.send(leader, 2, 40, 41) && personal.size() == 1 && personal[0] == "HUD_HUD_SQUAD_ALIVE_TO_REQUEST");
}

// An order on an object (the record's +4, 0x005b8b20): the order keeps the object and takes its position as the
// point; a leader's and a commander's alike; one with no such object is a point order.
TEST(Commander_OrderOnAnObject)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	const uint32_t station = field.place("commander_orbital_strike_64", 1, 120, 80);
	const int leader = world.addPlayer("Lead", 1, false);
	CHECK(world.spawnSoldier(leader));
	world.joinSquad(leader, 1);
	CHECK(world.issueSquadLeaderOrder(leader, game::GameWorld::OrderType::MapAttack, 5, 6, station));
	const game::GameWorld::SquadOrder* order = world.squadLeaderOrder(1, 1);
	CHECK(order && order->object == station && std::fabs(order->position.x - 120) < 0.01f && std::fabs(order->position.z - 80) < 0.01f);
	CHECK(world.issueSquadLeaderOrder(leader, game::GameWorld::OrderType::MapMove, 5, 6));
	order = world.squadLeaderOrder(1, 1);
	CHECK(order && order->object == 0 && std::fabs(order->position.x - 5) < 0.01f);
	CHECK(world.issueOrder(1, 1, game::GameWorld::OrderType::MapDefend, 0, 0, -1, station));
	order = world.squadOrder(1, 1);
	CHECK(order && order->object == station && order->type == game::GameWorld::OrderType::MapDefend);
}

// The AI commander's answers to the squad leaders' requests (AIDLL: the
// store's AI event 3/0xf → IAIEventCourier 0x1004e6e0.. → the SAI's
// 0x1009e020 artillery, 0x1009e200 orbital strike, 0x1009e3e0 UAV,
// 0x1009e500 supplies; turn-downs said by 0x1009ce20 after 2 + 3U s).
TEST(Commander_AiAnswersSquadRequests)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	field.place("commander_orbital_strike_64", 1, 200, 200); // the artillery; no orbital strike, no UAV
	const int leader = world.addPlayer("Lead", 1, false);
	CHECK(world.spawnSoldier(leader));
	world.joinSquad(leader, 1);
	field.soldier(1, 60, 60); // the team has a bot
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	game::BotManager bots(world, settings);
	const auto step = [&](float seconds) {
		for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
			bots.update(1 / 30.0f);
			world.tick(1 / 30.0f);
		}
	};
	const auto said = [&](const std::string& voice) {
		return std::count_if(bots.commanderSaid().begin(), bots.commanderSaid().end(),
			[&](const game::BotManager::CommanderLine& l) { return l.voice == voice && l.team == 1 && l.squad == 1; });
	};
	step(0.1f);
	// Artillery before its clock (120 s from the level's start): turned
	// down 2..5 s on with artillery_deny, and taken off.
	CHECK(world.storeSquadRequest(1, 1, 0, {0, 0, -60}));
	step(1.9f);
	CHECK(said("artillery_deny") == 0 && world.squadRequest(1, 1, 0));
	step(3.2f);
	CHECK(said("artillery_deny") == 1 && !world.squadRequest(1, 1, 0));
	// The orbital strike with no asset: the artillery's line, and the
	// squad's artillery request taken off rather than its own (the
	// original's 0x1009e200 queues the artillery's turn-downs).
	CHECK(world.storeSquadRequest(1, 1, 0, {0, 0, -60}) && world.storeSquadRequest(1, 1, 1, {0, 0, -60}));
	step(5.5f);
	CHECK(said("artillery_unavailable") == 1 && world.squadRequest(1, 1, 1));
	world.removeSquadRequest(1, 1, 1);
	// The UAV after 35 s and the supplies after 60 s: used at once, taken off.
	step(49.8f); // to 60.5 s
	CHECK(world.storeSquadRequest(1, 1, 3, {40, 0, 41}));
	step(0.1f);
	CHECK(!world.squadRequest(1, 1, 3) && world.supplyCrates().size() == 1);
	// Asked again within its 60 s: supply_deny.
	CHECK(world.storeSquadRequest(1, 1, 3, {40, 0, 41}));
	step(5.1f);
	CHECK(said("supply_deny") == 1 && !world.squadRequest(1, 1, 3));
	// The artillery after its 120 s: fired at the request's point.
	step(55.3f); // to 121 s
	CHECK(world.storeSquadRequest(1, 1, 0, {0, 0, -60}));
	step(0.1f);
	CHECK(!world.squadRequest(1, 1, 0) && !world.strikes().empty() && std::fabs(world.strikes()[0].target.z + 60) < 0.01f);
	// A human commander answers for himself: the AI leaves the request.
	const int boss = world.addPlayer("Boss", 1, false);
	world.setCommander(1, boss);
	CHECK(world.storeSquadRequest(1, 1, 2, {0, 0, 0}));
	step(6);
	CHECK(world.squadRequest(1, 1, 2) && said("uav_deny") == 0);
}

// enhanced.botCommander's supplies to a silo with two of its soldiers on foot there: not while a crate of the
// team's still serves the point (it went on dropping one each time the tool was ready); an enemy's crate there or
// the team's at another point doesn't count; once it expired, another.
TEST(Commander_TitanAiDropsOneCrateAtATime)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	for (float x : {40.0f, 42.0f, 240.0f, 242.0f}) {
		field.soldier(1, x, 40);
	}
	game::BotSettings settings;
	game::BotManager bots(world, settings);
	const auto drops = [&](const physics::Vec3& at) {
		bots.update(1 / 30.0f); // the players as the bots see them
		const size_t before = world.supplyCrates().size();
		bots.titanCommandAssetsNow(1, at, true);
		return world.supplyCrates().size() - before;
	};
	const physics::Vec3 silo {40, 0, 40}, another {240, 0, 40};
	CHECK(world.commanderAction(2, Ability::Supplies, 40, 41, true)); // the enemy's, right there
	CHECK(drops(silo) == 1);
	field.run(61); // the tool ready again, the crate down
	CHECK(drops(silo) == 0);
	CHECK(drops(another) == 1);
	field.run(252); // the first gone 300 s after it landed
	CHECK(drops(silo) == 1);
}

// enhanced.botCommander's answers to the squad leaders' T-rose requests in gpm_ti (not retail: the decision is the
// virtual commander's, the lines and flow are the commander's): with the switch off the refusal stands; on, a
// request is stored and answered, accepted (the tool used at the point, "<tool>_accept" to the leader, "_deny" to
// another squad's request of the kind) or turned down (the tool recharging: "_auto_deny"; its asset gone:
// "_unavailable"), the lines said to the leader alone; with a human commander nothing changes.
TEST(Commander_BotCommanderAnswersSquadRequests)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	world.setGameMode("gpm_ti");
	const int leader = world.addPlayer("Lead", 1, false);
	CHECK(world.spawnSoldier(leader));
	world.joinSquad(leader, 1);
	const int other = world.addPlayer("Other", 1, false);
	CHECK(world.spawnSoldier(other));
	world.joinSquad(other, 2);
	field.soldier(1, 60, 60); // the team has a bot
	field.place("commander_uav_drone_control", 1, -200, -200); // a UAV trailer, no artillery
	bool on = false;
	const bool enhanced = true;
	game::BotSettings settings;
	settings.enhanced = &enhanced;
	settings.botCommander = &on;
	game::BotManager bots(world, settings);
	menu::SquadRequests requests(world);
	requests.maxRequests = 100;
	const auto step = [&](float seconds) {
		for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
			bots.update(1 / 30.0f);
			world.tick(1 / 30.0f);
		}
	};
	const auto heard = [&](const std::string& voice, int receiver) {
		int n = 0;
		for (const auto& h : world.takeRadioHeard()) {
			n += h.voice == voice && h.receiver == receiver ? 1 : 0;
		}
		return n;
	};
	step(0.1f);
	// The switch off: the refusal, nothing stored.
	world.takeRadioHeard();
	CHECK(!requests.send(leader, 2, 40, 41) && !world.squadRequest(1, 1, 2));
	CHECK(heard("no_commander_available", leader) == 1);
	// On: stored, no refusal, nothing before the 2 s, then used at the point and accepted; the other squad's
	// UAV request is denied and taken off.
	on = true;
	step(0.1f);
	CHECK(requests.send(leader, 2, 40, 41) && requests.send(other, 2, 90, 91));
	CHECK(heard("no_commander_available", leader) == 0);
	// Its own UAV leaves the tool to the request.
	bots.titanCommandAssetsNow(1, {40, 0, 40}, false);
	CHECK(world.abilityState(1, game::GameWorld::CommanderAbility::Uav).ready);
	step(1.5f);
	CHECK(world.squadRequest(1, 1, 2) && world.squadRequest(1, 2, 2));
	step(0.8f);
	CHECK(!world.squadRequest(1, 1, 2) && !world.squadRequest(1, 2, 2));
	CHECK(!world.abilityState(1, game::GameWorld::CommanderAbility::Uav).ready);
	const auto lines = world.takeRadioHeard();
	const auto said = [&](const std::string& voice, int receiver) {
		return std::count_if(lines.begin(), lines.end(),
			[&](const game::GameWorld::RadioHeard& h) { return h.voice == voice && h.receiver == receiver; });
	};
	CHECK(said("uav_accept", leader) == 1 && said("uav_deny", other) == 1 && said("uav_accept", other) == 0);
	// The UAV recharging: the turn-down after 4 s (the bots' own UAV can't take it meanwhile).
	CHECK(requests.send(leader, 2, 40, 41));
	step(3.5f);
	CHECK(world.squadRequest(1, 1, 2));
	step(0.7f);
	CHECK(!world.squadRequest(1, 1, 2) && heard("uav_auto_deny", leader) == 1);
	// No artillery standing in the mode's level: unavailable.
	CHECK(requests.send(leader, 0, 40, 41));
	step(4.3f);
	CHECK(!world.squadRequest(1, 1, 0) && heard("artillery_unavailable", leader) == 1);
	// A human commander answers for himself.
	const int boss = world.addPlayer("Boss", 1, false);
	world.setCommander(1, boss);
	step(0.1f);
	CHECK(requests.send(leader, 2, 40, 41));
	step(6);
	CHECK(world.squadRequest(1, 1, 2) && heard("uav_auto_deny", leader) == 0);
}

// Exercise the installed Remaster scripts through the same Sound console
// registration and VFS used by the game. The null OpenAL driver still builds
// and starts actual Sound instances; StartedSound counts alone did not prove
// that a playable Sound template existed.
TEST(Commander_RemasterOrbitalStrikeBlastSound)
{
	REMASTER_ONLY();
	test::setEnv("ALSOFT_DRIVERS", "null");
	sound::ISoundEngine* engine = sound::getSoundEngine();
	CHECK(engine && engine->initialize(0, "Software Audio", 32) == 0);
	if (!engine || !engine->isInitialized()) {
		return;
	}
	{
		Field field("mods/Project_Remaster_v17_Final", false, engine);
		if (!field.loaded) {
			CHECK(false);
		} else {
			// Match Project_Remaster_v17_Final/ServerArchives.con and
			// ClientArchives.con mount points used by the game.
			CHECK(field.files.mountArchive("Objects/Common_server.zip", "Common"));
			CHECK(field.files.mountArchive("Objects/Sound_client.zip", "Sound"));
			CHECK(field.files.mountArchive("mods/bf2142/Sound_client.zip", "Sound"));
			CHECK(field.files.mountArchive("Objects/Effects_server.zip", "Objects\\Effects"));
			field.templates.registerType(0x9474, "EffectBundle");
			std::string out;
			field.console.run("Common/Material/ImpactSounds.con", {}, out);
			field.console.run("Common/Material/MaterialManagerSettings.con", {}, out);
			field.console.run("Objects/Effects/impacts/explosions/e_exp_orbital_strike/e_exp_orbital_strike.con", {}, out);
			field.console.run("Objects/Effects/impacts/explosions/e_exp_orbital_strike_distant/e_exp_orbital_strike_distant.con", {}, out);
			world::registerUntypedTemplateProperties(field.console, field.templates,
				{"detonation.endEffectTemplate", "detonation.useMMOnEndEffect"});
			field.console.run("Objects/Weapons/stationary/commander_orbital_strike/commander_orbital_strike.con", {}, out);
			const world::ObjectTemplate* shell = field.templates.find("commander_orbital_strike_Projectile");
			CHECK(shell && shell->settings().count("detonation.endEffectTemplate")
				&& shell->settings().at("detonation.endEffectTemplate") == "e_exp_orbital_strike");
			CHECK(shell && game::ProjectileSettings::fromTemplate(*shell).useMMOnEndEffect);
			CHECK(field.templates.find("S_e_exp_orbital_strike_Start")
				&& field.templates.find("S_e_exp_orbital_strike_distant_Start"));
			const std::string assetFire3P = "S_commander_orbital_strike_64_GenericFireArm_Fire3P";
			CHECK(field.templates.find(assetFire3P));
			sound::VfsSoundFileSystem vfs(field.files);
			engine->setFileSystem(&vfs);
			engine->reloadSoundFiles();
			sound::GameSounds gameSounds(field.soundManager);
			std::vector<sound::GameSounds::Triggered> triggered;
			gameSounds.onTrigger = [&triggered](const auto& event) { triggered.push_back(event); };
			GameAudio audio(*field.world, field.objects, field.templates, field.soundManager, gameSounds, nullptr);
			GameAudio::Camera camera;
			camera.position = {0, 3, -60};
			camera.forward = {0, -0.1f, -1};
			game::GameWorld& world = *field.world;
			world.setCommanderEnabled(true);
			world.setCommander(1, world.addPlayer("Boss", 1, false));
			field.place("commander_orbital_strike_64", 1, 200, 200);
			world.takeEffectSounds();
			CHECK(world.commanderAction(1, Ability::Artillery, 0, -60));
			const std::string falling = "S_commander_orbital_strike_Projectile_Looping";
			bool shellPlayingBeforeImpact = false;
			for (int i = 0; i < 20 * 30; ++i) {
				world.tick(1 / 30.0f);
				audio.tick(camera);
				// While the falling loops are the only events, a playing voice
				// proves the mixer found and started their sample.
				if (!triggered.empty() && std::all_of(triggered.begin(), triggered.end(), [&](const auto& event) {
						return event.name == falling;
					})) {
					const auto* stats = engine->statistics();
					shellPlayingBeforeImpact |= stats && stats->playingSamples > 0;
				}
			}
			CHECK(shellPlayingBeforeImpact);
			const auto heard = [&triggered](const std::string& name) {
				return std::any_of(triggered.begin(), triggered.end(), [&name](const auto& event) { return event.name == name; });
			};
			// Each of the 16 seven-shell salvos starts seven independent moving loops.
			CHECK(std::count_if(triggered.begin(), triggered.end(), [&](const auto& event) {
				return event.name == falling;
			}) == 16 * 7);
			CHECK(heard("S_e_exp_orbital_strike_Start"));
			CHECK(heard("S_Impact_Artillery"));
			// Remaster v17 creates this firearm event template but does not
			// assign it a soundFilename in its shipped tweak; retail consequently
			// has no Fire3P sample for GameSounds to play.
			CHECK(gameSounds.sampleCount(assetFire3P) == 0);
			const sound::SoundStatistics* stats = engine->statistics();
			CHECK(stats && stats->soundsAlive > 0 && stats->samplesAlive > 0 && stats->playingSamples > 0);
			std::printf("  actual strike sound instances: sounds=%d samples=%d playing=%d\n",
				stats ? stats->soundsAlive : 0, stats ? stats->samplesAlive : 0, stats ? stats->playingSamples : 0);
		}
	}
	engine->shutdown();
	sound::shutdownSoundEngine();
}

// The ghost priorities of a commander's client (0x0057dc10's end): with
// the satellite scan running every living enemy's control object comes in
// at 0.5 (0x0057cfb0, radius -1), however far it is; else enemies are
// scored by the eye like any ghost (out of scope beyond 850 m). Over
// 127.0.0.1: an enemy 3 km away moves; its ghost's updates reach the
// commander's client only while the scan runs.
TEST(Commander_ScanBringsFarEnemiesIntoScope)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	field.place("commander_movement_analyst", 2, -220, -200);
	game::GameLogic logic;
	logic.startRound();
	host::HostSetup setup;
	setup.level = "Field";
	setup.gameMode = "gpm_cq";
	setup.gamePort = 0;
	setup.queryPort = 0;
	host::HostedGame hosted(world, logic, field.templates, setup);
	CHECK(hosted.start());
	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Boss";
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
	const int enemy = field.soldier(1, 3000, 0);
	float x = 3000;
	const auto step = [&] {
		client.update();
		hosted.tick();
		world.tick(1 / 30.0f);
		// The enemy walks, so its ghost has something to send.
		game::GameWorld::PlayerInfo info;
		if (world.playerInfo(enemy, info) && info.alive) {
			x += 0.1f;
			world.setSoldierPosition(info.soldier, {x, 0, 0});
		}
	};
	const auto pump = [&](const std::function<bool()>& done, int milliseconds) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
		while (std::chrono::steady_clock::now() < deadline) {
			step();
			if (done()) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return false;
	};
	CHECK(pump([&] { return client.world().gameState() == 1 && client.world().localPlayer() >= 0; }, 6000));
	const int boss = client.world().localPlayer();
	client.selectTeam(2);
	game::GameWorld::PlayerInfo me;
	CHECK(pump([&] { return world.playerInfo(boss, me) && me.team == 2; }, 4000));
	client.selectSpawnGroup(-1);
	CHECK(pump([&] { return world.playerInfo(boss, me) && me.alive; }, 6000)); // an eye at the origin
	CHECK(world.setCommander(2, boss));
	const auto updates = [&] {
		const net::RemotePlayer* p = client.world().player(enemy);
		const net::RemoteObject* o = p && p->soldierGhost ? client.world().object(p->soldierGhost) : nullptr;
		return o ? o->updates : 0u;
	};
	const auto during = [&](int ticks) {
		const uint32_t from = updates();
		for (int i = 0; i < ticks; ++i) {
			step();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return updates() - from;
	};
	during(60);
	const uint32_t before = during(90);
	CHECK(world.commanderAction(2, Ability::Satellite, 0, 0));
	during(15);
	const uint32_t scanned = during(90);
	std::printf("  the far enemy's ghost: %u updates in 3 s without the scan, %u with it\n", before, scanned);
	CHECK(before == 0);
	CHECK(scanned > 20);
	client.disconnect();
}

// C4 thrown at a commander asset sticks to its face and, set off by its
// owner's alternate fire, hurts it (Unl_c4_Projectile: 1000 at radius 10,
// explosion material 80).
TEST(Commander_C4HurtsAssets)
{
	if (!std::getenv("BF2142_GAME_DIR")) {
		return;
	}
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		for (const char* name : {"commander_uav_drone_control", "commander_movement_analyst", "commander_emp_strike",
				 "commander_orbital_strike_64"}) {
			Field field(mod, true);
			if (!field.loaded) {
				CHECK(field.loaded);
				return;
			}
			// The charge's own properties as the game registers them.
			world::registerUntypedTemplateProperties(field.console, field.templates,
				{"collision.bouncing", "collision.maxStickAngle", "drag", "detonation.explosionForce",
					"detonation.detonateOnWaterCollision", "floaterMod"});
			std::string out;
			field.console.run("Objects/Weapons/Handheld/Unl_c4/unl_c4.con", {}, out);
			const uint32_t asset = field.place(name, 2, 0, 40);
			const float before = field.world->hitPoints(asset);
			const int enemy = field.soldier(1, 0, 25);
			CHECK(field.world->launchProjectile("Unl_c4_Projectile", {0, 1.5f, 34}, {0, 0, 15}, enemy));
			field.run(2);
			const game::Projectile* c4 = nullptr;
			for (const auto& p : field.world->projectiles()) {
				c4 = p.alive && p.settings && p.settings->name == "Unl_c4_Projectile" ? &p : c4;
			}
			CHECK(c4 && c4->stuck && c4->settings->remote);
			game::SoldierInput detonate;
			detonate.altFire = true;
			field.world->setInput(enemy, detonate);
			field.world->tick(1 / 30.0f);
			const float after = field.world->hitPoints(asset);
			std::fprintf(stderr, "  %s/%s: hp %.0f -> %.0f\n", mod, name, before, after);
			CHECK(after < before);
			game::GameWorld::PlayerInfo attackerInfo;
			CHECK(field.world->playerInfo(enemy, attackerInfo));
			CHECK(attackerInfo.hitIndicator > 0);
		}
	}
}

// Damaging and destroying a static commander asset (armor.canBeDestroyed 0):
// hits flash the attacker's hit indicator and spawn static armor damage
// effects; reaching 0 HP switches to the wreck look (geom2), raises
// VehicleDestroyed once, stops the asset's spinning dish, and stays
// repairable so repairing restores normal look and rotation.
TEST(Commander_AssetDamageWreckEffectsAndSpinner)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "ControlPoint");
	templates.registerType(2, "SpawnPoint");
	templates.registerType(3, "Soldier");
	templates.registerType(4, "PlayerControlObject");
	templates.registerType(5, "RotationalBundle");
	templates.registerType(6, "EffectBundle");
	templates.registerType(7, "SpriteParticleSystem");
	templates.registerType(8, "RemoteControlledObject");
	auto* cp = templates.create("ControlPoint", "Home");
	cp->settings()["controlPointId"] = "1";
	cp->settings()["team"] = "1";
	objects.create(cp, {0, 0, 0});
	auto* sp = templates.create("SpawnPoint", "Spawn");
	objects.create(sp, {0, 1, 0})->setControlPointId(1);
	templates.create("Soldier", "soldier")->settings()["armor.maxHitPoints"] = "100";

	const auto makeEffect = [&](const char* name) {
		auto* fx = templates.create("EffectBundle", name);
		fx->settings()["timeToLive"] = "CRD_NONE/1/0/0";
		auto* ps = templates.create("SpriteParticleSystem", std::string(name) + "_ps");
		ps->settings()["emitFrequency"] = "10";
		ps->settings()["emitTime"] = "10";
		ps->settings()["timeToLive"] = "1";
		ps->settings()["textureName"] = "spark";
		fx->children().push_back({ps->name(), {}, {}});
	};
	makeEffect("e_asset_smoke");
	makeEffect("e_asset_fire");
	makeEffect("e_asset_explode");

	auto* dish = templates.create("RotationalBundle", "asset_dish");
	dish->settings()["continousRotationSpeed"] = "90/0/0";
	auto* rc = templates.create("RemoteControlledObject", "asset_rc");
	rc->settings()["rcType"] = "RCSatellite";

	auto* asset = templates.create("PlayerControlObject", "commander_radar_asset");
	asset->settings()["armor.maxHitPoints"] = "100";
	asset->settings()["armor.hitPoints"] = "100";
	asset->settings()["armor.canBeDestroyed"] = "0";
	asset->settings()["armor.canBeRepairedWhenWreck"] = "1";
	asset->settings()["armor.addArmorEffect.list"] =
		"50 e_asset_smoke 0/1/0 0/0/0;25 e_asset_fire 0/1/0 0/0/0;0 e_asset_explode 0/1/0 0/0/0;";
	asset->children().push_back({"asset_dish", {0, 2, 0}, {0, 0, 0}});
	asset->children().push_back({"asset_rc", {}, {}});

	const world::Object* placed = objects.create(asset, {0, 0, 20});
	const_cast<world::Object*>(placed)->setTeam(2);
	io::FileManager files;
	physics::CollisionTemplateManager collisionTemplates;
	game::GameWorld world(templates, objects);
	physics::Ground ground;
	ground.height = [](float, float) { return 0.0f; };
	ground.heightAndNormal = [](float, float, physics::Vec3& n) {
		n = {0, 1, 0};
		return 0.0f;
	};
	ground.material = [](float, float) { return 0u; };
	world.enablePhysics(files, collisionTemplates, ground);
	int destroyedEvents = 0;
	world.setEventSink([&](const std::string& event, const game::ScriptArgs& args) {
		if (event == "VehicleDestroyed" && args.size() >= 1 && args[0].kind == game::ScriptValue::Kind::Object
			&& static_cast<uint32_t>(args[0].i) == placed->id()) {
			++destroyedEvents;
		}
	});
	world.startRound();
	const int enemy = world.joinPlayer(1);
	world.setKit(enemy, "", "soldier");
	CHECK(world.spawnSoldier(enemy));

	// Dish rotates while the asset is alive.
	world::Vec3 off, rot0, rot1;
	world.tick(0.25f);
	CHECK(world.isCommanderAsset(placed->id()));
	CHECK(world.partPose(placed->id(), "asset_dish", off, rot0));
	world.tick(0.25f);
	CHECK(world.partPose(placed->id(), "asset_dish", off, rot1));
	CHECK(std::fabs(rot1.x - rot0.x) > 10.0f);

	// Partial damage spawns static armor smoke effect without wrecking yet.
	world.applyDamage(placed->id(), 60.0f, enemy);
	world.tick(1 / 30.0f);
	CHECK(!world.vehicleWreckLook(placed->id()));
	CHECK(world.effects().emitters() > 0);
	CHECK(destroyedEvents == 0);

	// Destroying the asset triggers wreck look, VehicleDestroyed once, and stops the dish.
	world.applyDamage(placed->id(), 50.0f, enemy);
	CHECK(world.hitPoints(placed->id()) == 0.0f);
	CHECK(!placed->destroyed()); // stays repairable
	CHECK(world.vehicleWreckLook(placed->id()));
	CHECK(destroyedEvents == 1);
	world.applyDamage(placed->id(), 50.0f, enemy);
	CHECK(destroyedEvents == 1);

	world::Vec3 wreckRot0, wreckRot1;
	CHECK(world.partPose(placed->id(), "asset_dish", off, wreckRot0));
	world.tick(0.5f);
	CHECK(world.partPose(placed->id(), "asset_dish", off, wreckRot1));
	CHECK(std::fabs(wreckRot1.x - wreckRot0.x) < 1e-3f);

	// Repairing restores normal look and resumes dish rotation.
	CHECK(world.repair(placed->id(), 100.0f));
	CHECK(!world.vehicleWreckLook(placed->id()));
	world::Vec3 repairedRot0, repairedRot1;
	CHECK(world.partPose(placed->id(), "asset_dish", off, repairedRot0));
	world.tick(0.25f);
	CHECK(world.partPose(placed->id(), "asset_dish", off, repairedRot1));
	CHECK(std::fabs(repairedRot1.x - repairedRot0.x) > 10.0f);
}

// The shipped damage table against the armour of C4 and the mines
// (defaultMaterial 90): of the weapons' rounds and blasts only
// Detonation_Defuse_DMG 32 (the explosive shotgun round's blast) x10,
// Detonation_C4 80 x1, Bullet_Heavy_Sniper 131 x12 and Engineer_Defuse 132
// x20 do anything; the bullets, shells, missiles and grenades are 0. The
// vehicles' own explosions (99-101) have no cell and take the lookup's
// fallback of 1.
TEST(ArmouredRounds_DamageTableAgainstCharges)
{
	if (!std::getenv("BF2142_GAME_DIR")) {
		return;
	}
	Field field("mods/bf2142", true, nullptr, true);
	CHECK(field.materials.cellCount() > 0);
	const physics::MaterialManager& m = field.materials;
	CHECK(m.damageMod(32, 90) == 10.0f && m.damageMod(80, 90) == 1.0f && m.damageMod(131, 90) == 12.0f
		&& m.damageMod(132, 90) == 20.0f);
	for (const uint32_t weapon : {27u, 29u, 30u, 31u, 38u, 39u, 40u, 41u, 42u, 43u, 44u, 45u, 46u, 49u, 50u, 52u, 53u,
			 55u, 56u, 57u, 69u, 70u, 71u, 88u, 92u, 109u, 118u, 121u, 124u, 130u, 133u}) {
		if (m.damageMod(weapon, 90) != 0) {
			std::fprintf(stderr, "  material %u hurts armour 90: %g\n", weapon, m.damageMod(weapon, 90));
		}
		CHECK(m.damageMod(weapon, 90) == 0);
	}
	CHECK(m.damageMod(99, 90) == 1.0f && m.damageMod(101, 90) == 1.0f);
}

// C4 lying on the ground: a submachine-gun round does nothing, a heavy
// sniper round (100 x 12 against its 1000) sets it off, and a charge 1 m
// from a blast (Detonation_C4, 1000 x 1) goes off with it.
TEST(ArmouredRounds_C4ShotAndChained)
{
	if (!std::getenv("BF2142_GAME_DIR")) {
		return;
	}
	Field field("mods/bf2142", true, nullptr, true);
	if (!field.loaded) {
		CHECK(field.loaded);
		return;
	}
	world::registerUntypedTemplateProperties(field.console, field.templates,
		{"collision.bouncing", "collision.maxStickAngle", "drag", "detonation.explosionForce",
			"detonation.detonateOnWaterCollision", "floaterMod", "hasOnTimeEffect"});
	std::string out;
	field.console.run("Objects/Weapons/Handheld/Unl_c4/unl_c4.con", {}, out);
	field.console.run("Objects/Weapons/Handheld/unl_adv_sni/unl_adv_sni.con", {}, out);
	field.console.run("Objects/Weapons/Handheld/EU_SMG/eu_smg.con", {}, out);
	const world::ObjectTemplate* c4 = field.templates.find("Unl_c4_Projectile");
	CHECK(c4 && field.templates.find("unl_adv_sni_Projectile") && field.templates.find("eu_smg_Projectile"));
	if (!c4) {
		return;
	}
	const game::ProjectileSettings settings = game::ProjectileSettings::fromTemplate(*c4);
	CHECK(settings.hitPoints == 1000 && settings.armourMaterial == 90 && settings.onTimeEffect);
	const int owner = field.soldier(1, 60, 60);
	const int shooter = field.soldier(2, 70, 60);
	for (const float x : {0.0f, 1.0f, 30.0f}) {
		CHECK(field.world->launchProjectile("Unl_c4_Projectile", {x, 0.3f, 0}, {0, -3, 0}, owner));
	}
	field.run(1);
	const auto charge = [&](float x) -> const game::Projectile* {
		for (const auto& p : field.world->projectiles()) {
			if (p.alive && p.settings && p.settings->name == "Unl_c4_Projectile" && std::fabs(p.position.x - x) < 0.3f) {
				return &p;
			}
		}
		return nullptr;
	};
	CHECK(charge(0) && charge(1) && charge(30));
	const auto shoot = [&](const char* round, float x, float speed) {
		const game::Projectile* target = charge(x);
		if (!target) {
			return;
		}
		const physics::Vec3 from {x, 1.0f, -4};
		physics::Vec3 d = target->position - from;
		d = d * (speed / std::sqrt(physics::lengthSquared(d)));
		CHECK(field.world->launchProjectile(round, from, d, shooter));
		for (int i = 0; i < 10; ++i) {
			field.world->tick(1 / 30.0f);
		}
	};
	shoot("eu_smg_Projectile", 30, 300);
	CHECK(charge(30) != nullptr);
	shoot("unl_adv_sni_Projectile", 30, 1200);
	CHECK(charge(30) == nullptr);
	CHECK(charge(0) && charge(1));
	shoot("unl_adv_sni_Projectile", 0, 1200);
	CHECK(charge(0) == nullptr);
	CHECK(charge(1) == nullptr);
}

// The engineer's defuser (Unl_Defuser: RTHeal with Engineer_Defuse 132,
// 10 m, while firing) aimed at an enemy C4 5 m ahead: the charge is
// deleted without going off (0x005d8ac0). Pointed past it, nothing happens.
TEST(ArmouredRounds_DefuserRemovesCharge)
{
	if (!std::getenv("BF2142_GAME_DIR")) {
		return;
	}
	for (const bool aimed : {false, true}) {
		Field field("mods/bf2142", true, nullptr, true);
		if (!field.loaded) {
			CHECK(field.loaded);
			return;
		}
		world::registerUntypedTemplateProperties(field.console, field.templates,
			{"collision.bouncing", "collision.maxStickAngle", "drag", "detonation.explosionForce",
				"detonation.detonateOnWaterCollision", "floaterMod", "hasOnTimeEffect", "itemIndex", "pickupEnabled",
				"ammo.replenishingType", "ammo.abilityRadius", "ammo.abilityStrength", "ammo.abilityMaterial",
				"ammo.abilityDrain", "ammo.onlyActiveWhileFiring", "ammo.reloadWithoutPlayer"});
		std::string out;
		field.console.run("Objects/Weapons/Handheld/Unl_c4/unl_c4.con", {}, out);
		field.console.run("Objects/Weapons/Handheld/Unl_Defuser/Unl_Defuser.con", {}, out);
		CHECK(field.templates.find("unl_defuser") && field.templates.find("Unl_c4_Projectile"));
		world::ObjectTemplate* kit = field.templates.create("Kit", "EU_Assault");
		CHECK(kit != nullptr);
		if (!kit) {
			return;
		}
		for (const char* item : {"unl_defuser", "Unl_c4"}) {
			kit->children().push_back({item, {}, {}});
		}
		const int enemy = field.soldier(1, 40, 40);
		CHECK(field.world->launchProjectile("Unl_c4_Projectile", {0, 0.3f, 5}, {0, -3, 0}, enemy));
		const int engineer = field.soldier(2, 0, 0);
		field.world->setBattery(engineer, 1);
		field.run(1);
		const auto charge = [&] {
			for (const auto& p : field.world->projectiles()) {
				if (p.alive && p.settings && p.settings->name == "Unl_c4_Projectile") {
					return true;
				}
			}
			return false;
		};
		CHECK(charge());
		game::SoldierInput input;
		input.selectItem = std::atoi(field.templates.find("unl_defuser")->settings()["itemIndex"].c_str());
		field.world->setInput(engineer, input);
		field.world->tick(1 / 30.0f);
		input.selectItem = 0;
		input.fire = true;
		input.aimPitch = aimed ? -17.0f : 20.0f;
		for (int i = 0; i < 30; ++i) {
			field.world->setInput(engineer, input);
			field.world->tick(1 / 30.0f);
		}
		std::fprintf(stderr, "  defuser %s: charge %s\n", aimed ? "on it" : "above it", charge() ? "left" : "gone");
		CHECK(charge() == !aimed);
		CHECK(field.alive(engineer)); // removed, not set off 5 m away
	}
}

// The defuser on an enemy motion mine (Unl_Mine_Motion: armour 1000 of
// 10000, material 90), lying 5 m ahead and caught in the air as it moves
// through the beam: deleted without going off, and the engineer, who
// carries motion mines and has laid one, gets that one back (0x005d8d70:
// each firearm firing the round's template, slot 0x84(1)).
TEST(ArmouredRounds_DefuserRemovesMotionMines)
{
	if (!std::getenv("BF2142_GAME_DIR")) {
		return;
	}
	for (const bool moving : {false, true}) {
		Field field("mods/bf2142", true, nullptr, true);
		if (!field.loaded) {
			CHECK(field.loaded);
			return;
		}
		world::registerUntypedTemplateProperties(field.console, field.templates,
			{"collision.bouncing", "collision.maxStickAngle", "drag", "detonation.explosionForce",
				"detonation.detonateOnWaterCollision", "floaterMod", "hasOnTimeEffect", "itemIndex", "altItemIndex",
				"pickupEnabled", "armingDelay", "ammo.replenishingType", "ammo.abilityRadius", "ammo.abilityStrength",
				"ammo.abilityMaterial", "ammo.abilityDrain", "ammo.onlyActiveWhileFiring", "ammo.reloadWithoutPlayer",
				"ammo.autoReload", "ammo.reloadTime", "fire.maxProjectilesInWorld"});
		std::string out;
		field.console.run("Objects/Weapons/Handheld/Unl_Mine_Motion/Unl_Mine_Motion.con", {}, out);
		field.console.run("Objects/Weapons/Handheld/Unl_Defuser/Unl_Defuser.con", {}, out);
		const world::ObjectTemplate* mine = field.templates.find("Unl_Mine_Motion_Projectile");
		CHECK(mine && field.templates.find("unl_defuser"));
		if (!mine) {
			return;
		}
		const game::ProjectileSettings settings = game::ProjectileSettings::fromTemplate(*mine);
		CHECK(settings.hitPoints == 1000 && settings.maxHitPoints == 10000 && settings.armourMaterial == 90);
		world::ObjectTemplate* kit = field.templates.create("Kit", "EU_Assault");
		for (const char* item : {"unl_defuser", "Unl_Mine_Motion"}) {
			kit->children().push_back({item, {}, {}});
		}
		const int enemy = field.soldier(1, 40, 40);
		const int engineer = field.soldier(2, 0, 0);
		field.world->setBattery(engineer, 1);
		const auto rounds = [&] {
			for (const auto& w : field.world->weaponAmmo(engineer)) {
				if (w.weapon == "unl_mine_motion") {
					return w.rounds;
				}
			}
			return -1;
		};
		const auto press = [&](game::SoldierInput input, int ticks) {
			for (int i = 0; i < ticks; ++i) {
				field.world->setInput(engineer, input);
				field.world->tick(1 / 30.0f);
				input.selectItem = 0;
				input.turn = 0;
			}
		};
		// One of his own laid behind him: the mines take the alternate slot 6.
		const int full = rounds();
		game::SoldierInput input;
		input.selectItem = 6;
		press(input, 10);
		input = {};
		input.turn = 180;
		press(input, 1);
		input = {};
		input.fire = true;
		press(input, 1);
		press({}, 20);
		// Back to the front: the turn undone exactly (standing, a turn past
		// the 40-degree twist runs on by the catch-up: 180 faces 224).
		input = {};
		input.turn = -180;
		press(input, 1);
		const int laid = rounds();
		std::fprintf(stderr, "  motion mines: %d, %d after laying one\n", full, laid);
		CHECK(full > 0 && laid == full - 1);
		input = {};
		input.selectItem = 5;
		press(input, 10);
		uint32_t mineSerial = 0;
		const auto enemyMine = [&]() -> const game::Projectile* {
			for (const auto& p : field.world->projectiles()) {
				if (p.alive && p.settings && p.settings->name == "Unl_Mine_Motion_Projectile" && p.player == enemy) {
					mineSerial = p.serial;
					return &p;
				}
			}
			return nullptr;
		};
		input = {};
		input.fire = true;
		if (!moving) {
			CHECK(field.world->launchProjectile("Unl_Mine_Motion_Projectile", {0, 0.3f, 5}, {0, -3, 0}, enemy));
			press({}, 30);
			const game::Projectile* lying = enemyMine();
			CHECK(lying && lying->resting);
			// At its centre from the eyes (the feet + 1.6 m).
			game::GameWorld::PlayerInfo me;
			field.world->playerInfo(engineer, me);
			if (lying) {
				input.aimPitch = -std::atan2(me.position.y + 1.6f - lying->position.y, lying->position.z - me.position.z) * 57.29578f;
			}
			press(input, 15);
		} else {
			// Thrown up 5 m ahead; the trigger pressed as the mine rises past
			// 0.7 m, aimed where it will be (the first scan at once, 0x005d84a0).
			CHECK(field.world->launchProjectile("Unl_Mine_Motion_Projectile", {0, 0.3f, 5}, {0, 6, 0}, enemy));
			bool fired = false;
			float top = 0;
			for (int i = 0; i < 60 && enemyMine(); ++i) {
				const game::Projectile* m = enemyMine();
				top = std::max(top, m->position.y);
				if (!fired && m->position.y > 0.7f && m->velocity.y > 0) {
					fired = true;
					CHECK(!m->resting && m->velocity.y > 1);
					// At where it will be next tick, from the eyes.
					game::GameWorld::PlayerInfo me;
					field.world->playerInfo(engineer, me);
					const physics::Vec3 next = m->position + m->velocity * (1 / 30.0f);
					input.aimPitch = -std::atan2(me.position.y + 1.6f - next.y, next.z - me.position.z) * 57.29578f;
				}
				press(fired ? input : game::SoldierInput {}, 1);
			}
			std::fprintf(stderr, "  thrown mine rose to %.2f m\n", top);
			CHECK(fired);
		}
		std::fprintf(stderr, "  %s mine: %s, motion mines %d\n", moving ? "moving" : "lying", enemyMine() ? "left" : "gone", rounds());
		CHECK(enemyMine() == nullptr);
		CHECK(field.alive(engineer));
		CHECK(rounds() == full);
		// AI event 3/0x15 with the round's id (0x005d8ac0): the AI forgets it.
		CHECK(mineSerial != 0 && field.world->takeAiForgotten() == std::vector<uint32_t> {mineSerial});
	}
}

TEST(Commander_ResultNotificationsReachApplicantsAndTeam)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	game::GameWorld world(templates, objects);
	const int winner = world.addPlayer("Winner", 2, false);
	const int loser = world.addPlayer("Loser", 2, false);
	const int mate = world.addPlayer("Mate", 2, false);
	const int enemy = world.addPlayer("Enemy", 1, false);
	const int bot = world.addPlayer("Bot", 2, true);
	world.setCommanderEnabled(true);
	world.applyForCommander(winner);
	world.applyForCommander(loser);
	CHECK(world.setCommander(2, winner));
	auto notices = world.takeSquadMessages(loser);
	CHECK(notices.size() == 2 && notices[0].id == 0x22 && notices[1].id == 0x21 && notices[1].value == winner);
	for (int player : {winner, mate}) {
		notices = world.takeSquadMessages(player);
		CHECK(notices.size() == 1 && notices[0].id == 0x21 && notices[0].value == winner);
	}
	CHECK(world.takeSquadMessages(enemy).empty() && world.takeSquadMessages(bot).empty());
	CHECK(world.setCommander(2, winner)); // same commander: no duplicate
	CHECK(world.takeSquadMessages(winner).empty());
	world.resignCommander(winner);
	notices = world.takeSquadMessages(winner);
	CHECK(notices.size() == 1 && notices[0].id == 0x25 && notices[0].value == winner);
	const auto heard = world.takeRadioHeard();
	for (int player : {winner, loser, mate}) {
		CHECK(std::count_if(heard.begin(), heard.end(), [&](const auto& voice) {
			return voice.receiver == player && voice.voice == "commander_resign" && voice.channel == 13 && voice.sender == -1;
		}) == 1);
	}
	CHECK(std::none_of(heard.begin(), heard.end(), [&](const auto& voice) { return voice.receiver == enemy || voice.receiver == bot; }));
	world.resignCommander(winner);
	CHECK(world.takeSquadMessages(winner).empty());
}

// The commander assets' dishes and antennas turn by themselves
// (continousRotationSpeed on a RotationalBundle under a stationary root: the
// part's own update, 0x00565a20, runs whatever the root's physics). They
// need a ticked part state; a stationary root with none keeps none.
TEST(Commander_ScanningDishesTurnAtTheirAuthoredSpeed)
{
	if (!std::getenv("BF2142_GAME_DIR")) {
		return;
	}
	struct Dish {
		const char* object;
		const char* part;
		float speed;
	};
	const Dish dishes[] = {{"commander_movement_analyst", "commander_movement_analyst_top", 150},
		{"commander_emp_strike", "commander_EMP_Strike_mount", 60},
		{"commander_orbital_strike", "commander_Orbital_Strike_Antenna", 4}};
	for (const std::string& mod : {std::string("mods/bf2142"), std::string("mods/Project_Remaster_v17_Final")}) {
		if (!test::runs(mod)) continue;
		for (const Dish& dish : dishes) {
			Field field(mod, true);
			if (!field.loaded) {
				CHECK(field.loaded);
				continue;
			}
			const world::Object* placed = field.objects.create(field.templates.find(dish.object), {0, 0, 0});
			field.world->tick(1 / 30.0f);
			world::Vec3 offset, rotation;
			CHECK(field.world->partPose(placed->id(), dish.part, offset, rotation));
			const float start = rotation.x;
			for (int i = 0; i < 90; ++i) {
				field.world->tick(1 / 30.0f);
			}
			CHECK(field.world->partPose(placed->id(), dish.part, offset, rotation));
			const float turned = std::remainder(rotation.x - start - dish.speed * 3.0f, 360.0f);
			std::fprintf(stderr, "  %s/%s: turned %.2f (authored %.1f deg/s x 3 s)\n", mod.c_str(), dish.part,
				rotation.x - start, dish.speed);
			CHECK(std::fabs(turned) < 0.5f);
		}
		// A stationary root without a turning part gets no part state.
		Field field(mod, true);
		if (!field.loaded) {
			continue;
		}
		world::ObjectTemplate* still = field.templates.find("commander_movement_analyst");
		world::ObjectTemplate* top = field.templates.find("commander_movement_analyst_top");
		CHECK(still && top);
		top->settings()["continousRotationSpeed"] = "0/0/0";
		const world::Object* placed = field.objects.create(still, {0, 0, 0});
		field.world->tick(1 / 30.0f);
		CHECK(field.world->vehicle(placed->id()) == nullptr);
	}
}
