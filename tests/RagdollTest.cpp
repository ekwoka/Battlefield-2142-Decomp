// Ragdolls and seat poses with the game's own data (needs
// $BF2142_GAME_DIR): ragDollInit.con's template, a killed soldier's body
// falling to rest on flat ground in one piece and getting up when revived,
// and a soldier seated in a car posed by the seat's animation system.
#include "Animation/AnimationConsole.h"
#include "Animation/AnimationSystem.h"
#include "Animation/RagDoll.h"
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/CollisionWorld.h"
#include "Physics/MaterialManager.h"
#include "Terrain/Heightmap.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

constexpr const char* kRagDollScript = "Objects/Soldiers/Common/Animations/ragDollInit.con";

// The game's archives, animation systems and soldier templates on flat
// ground at height 0; EU's spawn point at the origin, PAC's 20 m away.
struct Game {
	io::FileManager files;
	Console console {&files};
	anim::AnimationSystems systems {[this](const std::string& path, std::vector<uint8_t>& data) {
		return files.readFile(path, data);
	}};
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	physics::CollisionTemplateManager collisionTemplates;
	std::unique_ptr<game::GameWorld> world;
	std::vector<std::string> events;
	bool valueHolders = false;

	explicit Game(const char* gameDir)
	{
		files.setRootPath(gameDir);
		files.addSearchPath("mods/bf2142");
		bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
			[this](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
		std::string out;
		console.run("ServerArchives.con", {}, out);
		anim::registerAnimationConsoleMethods(console, systems);
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
		templates.registerType(4, "GenericFireArm");
		templates.registerType(5, "GenericProjectile");
		templates.registerType(6, "Kit");
		templates.registerType(7, "PlayerControlObject");
		templates.registerType(8, "EntryPoint");
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			world::ObjectTemplate* soldier = templates.create("Soldier", name);
			soldier->settings()["armor.maxHitPoints"] = "100";
			soldier->settings()["skeleton3P"] = "Objects/Soldiers/Common/Animations/3p_setup.ske";
			soldier->settings()["animationSystem3P"] = "Objects/Soldiers/Common/Animations/AnimationSystem3p.inc";
		}
		for (const char* name : {"eu_ar_rifle", "as_ar_rifle"}) {
			world::ObjectTemplate* rifle = templates.create("GenericFireArm", name);
			rifle->settings()["projectileTemplate"] = "bullet";
			rifle->settings()["velocity"] = "300";
			rifle->settings()["itemIndex"] = "3";
			rifle->settings()["animationSystem3P"] = "Objects/Weapons/Handheld/eu_ar_rifle/AnimationSystem3p.inc";
		}
		templates.create("GenericProjectile", "bullet")->settings()["damage"] = "40";
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
	}

	// The world, once the test's templates are in place.
	void start(const physics::MaterialManager* materials = nullptr)
	{
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->enablePhysics(files, collisionTemplates,
			{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return 0.0f;
			 }},
			materials);
		world->setAnimationSystems(&systems, [this](const std::string& path) {
			std::string out;
			if (!valueHolders) {
				valueHolders = true;
				console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, out);
			}
			return systems.get(path, [&] { console.run(path, {}, out); });
		});
		world->setEventSink([this](const std::string& event, const game::ScriptArgs&) { events.push_back(event); });
		world->startRound();
	}

	void run(float seconds)
	{
		for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
			world->tick(1 / 30.0f);
		}
	}
	long count(const char* event) const { return std::count(events.begin(), events.end(), event); }
};

float distance(const anim::Vec& a, const anim::Vec& b)
{
	const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
	return std::sqrt(x * x + y * y + z * z);
}

} // namespace

TEST(RagDoll_TemplateFromTheGamesScript)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	const anim::RagDollTemplate* ragDoll = game.systems.ragDoll(kRagDollScript);
	CHECK(ragDoll != nullptr);
	if (!ragDoll) {
		return;
	}
	// 13 particles; ragDollConstraints.inc's lines, a repeated angle triple
	// replacing the first limit.
	CHECK(ragDoll->particles().size() == 13);
	CHECK(ragDoll->constraints().size() == 18);
	CHECK(ragDoll->angular().size() == 12);
	CHECK(ragDoll->dihedral().size() == 4);
	CHECK(ragDoll->lessThan().size() == 2);
	CHECK(ragDoll->capsules().size() == 12);
	CHECK(ragDoll->links().size() == 30);
	CHECK(ragDoll->midParticle() == ragDoll->particleOf(6));
	CHECK(ragDoll->locked(5) && ragDoll->locked(64) && !ragDoll->locked(47));
	// vars.set: slowed to 0.75, gravity in over 0.1 s.
	CHECK(ragDoll->settings.slowMotion == 0.75f && ragDoll->settings.gravityOverTime == 0.1f);
	CHECK(ragDoll->settings.iterations == 2 && std::abs(ragDoll->settings.dT - 1 / 90.0f) < 1e-6f);
	// Lengths from the setup skeleton: the left thigh (hip 1 to knee 2) 0.375 m.
	CHECK(std::abs(ragDoll->referenceDistance(1, 2) - 0.375f) < 1e-3f);
	// Knees small, hips and shoulders big.
	CHECK(ragDoll->particles()[static_cast<size_t>(ragDoll->particleOf(2))].radius == 0.05f);
	CHECK(ragDoll->particles()[static_cast<size_t>(ragDoll->particleOf(31))].radius == 0.12f);
	CHECK(ragDoll->particles()[static_cast<size_t>(ragDoll->particleOf(47))].radius == 0.1f);
}

TEST(RagDoll_KilledSoldierFallsToRestAndGetsUpWhenRevived)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	// A medic's defibrillator next to the rifle (as GameWorld_DefibrillatorRevives).
	world::ObjectTemplate* defib = game.templates.create("GenericFireArm", "unl_defibrillator");
	defib->settings()["itemIndex"] = "5";
	defib->settings()["projectileTemplate"] = "shock";
	defib->settings()["velocity"] = "60";
	defib->settings()["ammo.magSize"] = "1";
	defib->settings()["ammo.reloadTime"] = "0.6";
	world::ObjectTemplate* shock = game.templates.create("GenericProjectile", "shock");
	shock->settings()["damage"] = "1000";
	shock->settings()["damageMandownSoldiers"] = "1";
	shock->settings()["collision.resurrectDamage"] = "20000";
	shock->settings()["collision.restoreHP"] = "120";
	shock->settings()["timeToLive"] = "CRD_NONE/0.05/0/0";
	world::ObjectTemplate* kit = game.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"unl_defibrillator", {}, {}});
	game.start();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	const int bot = game.world->addPlayer("Mate", 2, true);
	CHECK(game.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info, me;
	game.world->playerInfo(game.world->localPlayer(), me);
	game.world->playerInfo(bot, info);
	game.world->setSoldierPosition(info.soldier, me.position + physics::Vec3 {0, 0, 1.2f});
	game.world->setSpawnTime(30);
	game.world->setManDownTime(30);
	game.run(0.5f); // posed by its animation systems
	const uint32_t soldier = info.soldier;
	game.world->setHitPoints(soldier, 0);
	const anim::RagDoll* ragDoll = game.world->ragDoll(soldier);
	CHECK(ragDoll != nullptr);
	CHECK(!game.world->bodyLanded(bot)); // ragdollLandedDelay (4 s) to go
	if (!ragDoll) {
		return;
	}
	const anim::RagDollTemplate& t = *ragDoll->ragDollTemplate();
	const auto connected = [&](float tolerance) {
		float worst = 0;
		for (const anim::RagDollTemplate::Distance& c : t.constraints()) {
			const float d = distance(ragDoll->particles()[static_cast<size_t>(c.a)].position,
				ragDoll->particles()[static_cast<size_t>(c.b)].position);
			worst = std::max(worst, std::abs(d - c.length) / c.length);
		}
		return worst <= tolerance ? 0.0f : worst;
	};
	CHECK(connected(0.15f) == 0); // starts from the animated pose
	float highest = 0;
	for (const anim::RagDoll::Particle& p : ragDoll->particles()) {
		highest = std::max(highest, p.position[1]);
	}
	CHECK(highest > 1.4f); // the head

	// Falls, never into the ground, and comes to rest lying down in one piece.
	float lowest = 1e9f, worstStretch = 0;
	int ticks = 0;
	while (!ragDoll->sleeping() && ticks < 30 * 10) {
		game.world->tick(1 / 30.0f);
		++ticks;
		for (const anim::RagDoll::Particle& p : ragDoll->particles()) {
			lowest = std::min(lowest, p.position[1]);
		}
		worstStretch = std::max(worstStretch, connected(0));
	}
	std::fprintf(stderr, "  ragdoll asleep after %.2f s; lowest particle %.3f m, worst stretch %.1f%%\n", ticks / 30.0f,
		lowest, worstStretch * 100);
	CHECK(ragDoll->sleeping());
	game.run(1);
	CHECK(ragDoll->touched() && game.world->bodyLanded(bot));
	CHECK(lowest > -0.02f);
	CHECK(worstStretch < 0.25f); // landing jolts it (two passes a step, as the game)
	CHECK(connected(0.05f) == 0);
	const anim::Vec mid = ragDoll->middle();
	float top = 0;
	for (const anim::RagDoll::Particle& p : ragDoll->particles()) {
		top = std::max(top, p.position[1]);
		CHECK(distance(p.position, mid) < 1.2f); // no limb flung off
	}
	CHECK(top < 0.5f); // lying
	// The soldier's object sits at the middle particle and its skeleton's
	// pose follows the particles: the head bone where the head particle is.
	const world::Object* body = game.objects.find(soldier);
	CHECK(body && std::abs(body->position().x - mid[0]) < 1e-4f && std::abs(body->position().y - mid[1]) < 1e-4f);
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	std::vector<anim::Matrix> pose;
	CHECK(skeleton && game.world->skeletonPose(soldier, *skeleton, pose));
	if (pose.size() > 47) {
		const anim::Vec head = ragDoll->particles()[static_cast<size_t>(t.particleOf(47))].position;
		CHECK(distance({pose[47][12] + mid[0], pose[47][13] + mid[1], pose[47][14] + mid[2]}, head) < 1e-3f);
		// The neck stays on the shoulders: the bones next to each other stay
		// as far apart as in the setup pose.
		const float neck = distance({pose[46][12], pose[46][13], pose[46][14]}, {pose[13][12], pose[13][13], pose[13][14]});
		CHECK(neck < 0.3f);
	}
	CHECK(game.world->soldierCapsules(soldier).size() > 0); // hit where it lies

	// The medic shocks it back: up where it lay, out of the ragdoll (no
	// get-up animation: BF2142.exe never plays reviveOnBack/OnStomach).
	game::SoldierInput input;
	input.selectItem = 5;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	game.world->playerInfo(game.world->localPlayer(), me);
	const float yaw = std::atan2(mid[0] - me.position.x, mid[2] - me.position.z) * 57.29578f;
	const float reach = std::sqrt((mid[0] - me.position.x) * (mid[0] - me.position.x) + (mid[2] - me.position.z) * (mid[2] - me.position.z));
	const float pitch = -std::atan2(1.6f - mid[1], reach) * 57.29578f;
	input = {};
	input.turn = yaw - me.yaw;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	for (int i = 0; i < 20; ++i) {
		input = {};
		input.fire = true;
		input.aimPitch = pitch + static_cast<float>(i % 5 - 2) * 4;
		game.world->setPlayerInput(input);
		game.run(0.7f);
		game.world->playerInfo(bot, info);
		if (info.alive) {
			break;
		}
	}
	game.world->playerInfo(bot, info);
	CHECK(info.alive && !info.manDown && game.count("PlayerRevived") == 1);
	CHECK(game.world->ragDoll(soldier) == nullptr);
	CHECK(std::abs(info.position.x - mid[0]) < 0.5f && std::abs(info.position.z - mid[2]) < 0.5f);
	const auto playing = game.world->playingAnimations(soldier);
	bool gettingUp = false;
	for (const std::string& name : playing) {
		gettingUp = gettingUp || name == "reviveOnBack" || name == "reviveOnStomach";
	}
	CHECK(!gettingUp);
	CHECK(std::abs(game.world->hitPoints(soldier) - 100) < 1e-3f);
}

TEST(RagDoll_SeatedSoldierTakesTheSeatsPose)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	// A car with as_fav's collision mesh and driver's seat animation.
	game.collisionTemplates.create("as_fav", "Objects/Vehicles/as_fav/");
	world::ObjectTemplate* car = game.templates.create("PlayerControlObject", "car");
	car->settings()["hasMobilePhysics"] = "1";
	car->settings()["physicsType"] = "Mesh";
	car->settings()["collisionMesh"] = "as_fav";
	car->settings()["seatInformation"] = "car 0/0.5/0.4 0/0/0";
	car->settings()["seatAnimationSystem"] = "Objects/Vehicles/AS_FAV/3p_as_fav_driver_animSys.inc";
	car->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
	// An engine and four sprung wheels (a vehicle model needs both).
	game.templates.registerType(9, "Engine");
	game.templates.registerType(10, "Spring");
	car->settings()["mass"] = "1800";
	world::ObjectTemplate* engine = game.templates.create("Engine", "car_engine");
	engine->settings()["engineType"] = "194";
	world::ObjectTemplate* wheel = game.templates.create("Spring", "car_wheel");
	wheel->settings()["strength"] = "40";
	wheel->settings()["damping"] = "8";
	car->children().push_back({"car_engine", {0, -0.44f, 0}, {}});
	for (float x : {-0.95f, 0.95f}) {
		for (float z : {-1.4f, 1.6f}) {
			engine->children().push_back({"car_wheel", {x, -0.15f, z}, {}});
		}
	}
	game.templates.create("EntryPoint", "car_entry")->settings()["entryRadius"] = "3";
	game.start();
	const uint32_t vehicle = game.objects.create(car, {3, 1.2f, 0}, {30, 0, 0})->id();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	game.run(3); // the car settles
	const uint32_t soldier = game.world->playerObject();
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	std::vector<anim::Matrix> standing, seated;
	CHECK(skeleton && game.world->skeletonPose(soldier, *skeleton, standing));
	game::SoldierInput input;
	input.use = true;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	game.world->setPlayerInput({});
	game.run(0.5f);
	CHECK(game.world->playerVehicle() && game.world->playerVehicle()->id() == vehicle);
	const world::Object* object = game.objects.find(soldier);
	CHECK(object && !object->hidden()); // seen in its seat
	const auto playing = game.world->playingAnimations(soldier);
	CHECK(std::find(playing.begin(), playing.end(), "still") != playing.end());
	CHECK(skeleton && game.world->skeletonPose(soldier, *skeleton, seated));
	if (object && standing.size() > 9 && seated.size() > 9) {
		// At the seat, turned with the car.
		const world::Object* carObject = game.objects.find(vehicle);
		const float yaw = carObject->rotation().x * 3.14159265f / 180.0f;
		const physics::Vec3 seat {carObject->position().x + 0.4f * std::sin(yaw), carObject->position().y + 0.5f,
			carObject->position().z + 0.4f * std::cos(yaw)};
		CHECK(std::abs(object->position().x - seat.x) < 0.05f && std::abs(object->position().z - seat.z) < 0.05f);
		CHECK(std::abs(object->rotation().x - carObject->rotation().x) < 1.0f);
		// Sitting: the knees bent, as high as the hips or nearly.
		const auto height = [](const std::vector<anim::Matrix>& pose, int bone) { return pose[static_cast<size_t>(bone)][13]; };
		std::fprintf(stderr, "  seated: hips %.2f knees %.2f feet %.2f (standing %.2f %.2f %.2f)\n", height(seated, 1),
			height(seated, 2), height(seated, 4), height(standing, 1), height(standing, 2), height(standing, 4));
		CHECK(height(standing, 1) - height(standing, 2) > 0.2f);
		CHECK(height(seated, 1) - height(seated, 2) < 0.2f);
	}
	// Out again: standing, and the seat's pose gone.
	input.use = true;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	game.world->setPlayerInput({});
	game.run(0.5f);
	CHECK(game.world->playerVehicle() == nullptr);
	const auto after = game.world->playingAnimations(soldier);
	CHECK(std::find(after.begin(), after.end(), "still") == after.end());
}

// Killed in a seat with a die bundle (0x005cea00 with died = 1): the
// seat's system plays "die" and the body stays in the seat, posed by it
// (no ragdoll), until someone takes the seat or the vehicle goes.
TEST(RagDoll_KilledInASeatSlumpsThere)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	game.collisionTemplates.create("as_fav", "Objects/Vehicles/as_fav/");
	world::ObjectTemplate* car = game.templates.create("PlayerControlObject", "car");
	car->settings()["hasMobilePhysics"] = "1";
	car->settings()["physicsType"] = "Mesh";
	car->settings()["collisionMesh"] = "as_fav";
	car->settings()["seatInformation"] = "car 0/0.5/0.4 0/0/0";
	car->settings()["seatAnimationSystem"] = "Objects/Vehicles/AS_FAV/3p_as_fav_driver_animSys.inc";
	car->settings()["armor.maxHitPoints"] = "500";
	car->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
	game.templates.registerType(9, "Engine");
	game.templates.registerType(10, "Spring");
	car->settings()["mass"] = "1800";
	world::ObjectTemplate* engine = game.templates.create("Engine", "car_engine");
	engine->settings()["engineType"] = "194";
	world::ObjectTemplate* wheel = game.templates.create("Spring", "car_wheel");
	wheel->settings()["strength"] = "40";
	wheel->settings()["damping"] = "8";
	car->children().push_back({"car_engine", {0, -0.44f, 0}, {}});
	for (float x : {-0.95f, 0.95f}) {
		for (float z : {-1.4f, 1.6f}) {
			engine->children().push_back({"car_wheel", {x, -0.15f, z}, {}});
		}
	}
	game.templates.create("EntryPoint", "car_entry")->settings()["entryRadius"] = "3";
	game.start();
	const uint32_t vehicle = game.objects.create(car, {3, 1.2f, 0}, {30, 0, 0})->id();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	game.run(3);
	const uint32_t soldier = game.world->playerObject();
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	game::SoldierInput input;
	input.use = true;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	game.world->setPlayerInput({});
	game.run(0.5f);
	CHECK(game.world->playerVehicle() && game.world->playerVehicle()->id() == vehicle);
	std::vector<anim::Matrix> seated, slumped;
	CHECK(skeleton && game.world->skeletonPose(soldier, *skeleton, seated));
	game.world->setHitPoints(soldier, 0);
	CHECK(game.world->seatCorpse(vehicle, 0) == soldier);
	game.run(1.0f);
	const world::Object* body = game.objects.find(soldier);
	CHECK(body && !body->hidden());
	CHECK(skeleton && game.world->skeletonPose(soldier, *skeleton, slumped));
	if (seated.size() > 47 && slumped.size() > 47) {
		const float moved = std::fabs(seated[47][12] - slumped[47][12]) + std::fabs(seated[47][13] - slumped[47][13])
			+ std::fabs(seated[47][14] - slumped[47][14]);
		std::fprintf(stderr, "  the head moved %.2f m slumping\n", moved);
		CHECK(moved > 0.05f); // the die bundle's pose, not the seat's still one
	}
	// Still there a while on, gone with the vehicle.
	game.run(5.0f);
	CHECK(game.world->seatCorpse(vehicle, 0) == soldier && game.objects.find(soldier));
	game.objects.remove(vehicle);
	game.run(0.1f);
	CHECK(game.world->seatCorpse(vehicle, 0) == 0 && !game.objects.find(soldier));
}

// A body dropped on flat ground (the setup pose 0.5 m up): its impacts
// meet ragDollInit.con's setParticleCollisionCriteria (0.05 m a step, 1 s
// apart: 0x008a7a20, 0x008a79f0) and it touches down (+0x63); asleep, it
// wakes when something near it moves (every 1/30 s, 0x008a79d0) and rides
// along with what it lies on (0x008add10: velocity x 1/30 s a frame).
// Its capsules are pushed out of an edge they lie across (0x006e2ee0).
TEST(RagDoll_ImpactsWakingAndRidingAlong)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	const anim::RagDollTemplate* t = game.systems.ragDoll(kRagDollScript);
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	CHECK(t && skeleton);
	if (!t || !skeleton) {
		return;
	}
	CHECK(std::abs(t->collisionMinSpeed() - 0.05f) < 1e-6f && std::abs(t->collisionMinDelay() - 1) < 1e-6f);
	std::vector<anim::Matrix> pose = anim::worldMatrices(*skeleton, anim::setupPose(*skeleton));
	for (anim::Matrix& m : pose) {
		m[13] += 0.5f;
	}
	anim::RagDoll body;
	body.start(*t, pose, {0, -3, 0});
	anim::RagDollWorld world;
	world.ground = [](float, float, anim::Vec& normal) {
		normal = {0, 1, 0};
		return 0.0f;
	};
	CHECK(!body.touched());
	int impacts = 0;
	float hardest = 0;
	for (int i = 0; i < 30 * 8 && !body.sleeping(); ++i) {
		body.update(1 / 30.0f, world);
		float speed = 0;
		uint32_t material = 1;
		if (body.takeImpact(speed, material)) {
			++impacts;
			hardest = std::max(hardest, speed);
			CHECK(material == 0 && speed >= 0.05f);
		}
	}
	std::fprintf(stderr, "  %d impact sounds, the hardest %.3f m a step\n", impacts, hardest);
	CHECK(body.touched() && impacts >= 1 && impacts <= 9);
	CHECK(body.sleeping());
	// Nothing moving: asleep. Something moving near: awake.
	world.movingNear = [](const anim::Vec&, float) { return false; };
	body.update(1 / 30.0f, world);
	CHECK(body.sleeping());
	world.movingNear = [](const anim::Vec&, float) { return true; };
	body.update(1 / 30.0f, world);
	CHECK(!body.sleeping());
	// Carried at 3 m/s along x: 0.1 m a frame.
	world.movingNear = {};
	world.carriedBy = [](const anim::Vec&, anim::Vec& velocity) {
		velocity = {3, 0, 0};
		return true;
	};
	const float before = body.middle()[0];
	for (int i = 0; i < 10; ++i) {
		body.update(1 / 30.0f, world);
	}
	CHECK(std::abs(body.middle()[0] - before - 1.0f) < 0.05f);
}

// The level's terrain material map (heightmap.loadMaterialData,
// HeightmapPrimary.mat: a byte per sample, the material id, read at the
// cell's lower corner, 0x006cfe40 / 0x006d2fc0), and a body landing on it
// takes its material for the impact sound.
TEST(RagDoll_TerrainMaterialMap)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	files.setMountingAllowed(true);
	const std::string level = "Levels/Camp_Gibraltar/";
	CHECK(files.mountArchive(level + "server.zip", level));
	files.setMountingAllowed(false);
	Console console {&files};
	terrain::HeightmapCluster cluster;
	terrain::registerTerrainConsoleMethods(console, cluster, files);
	std::string out;
	CHECK(console.run(level + "Heightdata.con", {}, out));
	const terrain::Heightmap* primary = cluster.primary();
	CHECK(primary && primary->loaded() && primary->hasMaterials());
	if (!primary || !primary->hasMaterials()) {
		return;
	}
	// Materials (materialManagerDefine.con): 8 Dirt, 12 rock; the painted
	// map is 1024 square, its last row and column 0.
	CHECK(cluster.materialAt(0, 0) == 8);
	CHECK(cluster.materialAt(250, -400) == 12);
	CHECK(primary->material(512, 0) == 0 && primary->material(0, 512) == 0 && primary->material(511, 0) != 0);
	CHECK(primary->material(-513, 0) == 0 && primary->material(0, 600) == 0);
	CHECK(cluster.materialAt(0.0f, -1.0f) == primary->material(0, -1)); // floor, not truncation
	// The secondary heightmaps have no map: 0 beyond the primary.
	CHECK(cluster.materialAt(3000, 0) == 0);

	// A body falling on rock reports rock.
	Game game(gameDir);
	const anim::RagDollTemplate* t = game.systems.ragDoll(kRagDollScript);
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	CHECK(t && skeleton);
	if (!t || !skeleton) {
		return;
	}
	std::vector<anim::Matrix> pose = anim::worldMatrices(*skeleton, anim::setupPose(*skeleton));
	anim::RagDoll body;
	body.start(*t, pose, {0, -3, 0});
	anim::RagDollWorld world;
	world.ground = [](float, float, anim::Vec& normal) {
		normal = {0, 1, 0};
		return -0.5f;
	};
	world.groundMaterial = [](float, float) { return 12u; };
	int impacts = 0;
	for (int i = 0; i < 30 * 3; ++i) {
		body.update(1 / 30.0f, world);
		float speed = 0;
		uint32_t material = 0;
		if (body.takeImpact(speed, material)) {
			++impacts;
			CHECK(material == 12);
		}
	}
	CHECK(impacts >= 1);
}

// The capsule edge test (collision-mesh slot 25, per face 0x006be990): a
// capsule lying across a box's top edge is pushed up and out of it; one
// below a face's edge with the normal pointing down is turned up without a
// push.
TEST(CollisionWorld_CapsuleAgainstEdges)
{
	physics::CollisionMeshFile mesh;
	physics::CollisionLod lod;
	const auto add = [&](std::array<float, 3> a, std::array<float, 3> b, std::array<float, 3> c) {
		const uint16_t i = static_cast<uint16_t>(lod.vertices.size());
		lod.vertices.push_back(a);
		lod.vertices.push_back(b);
		lod.vertices.push_back(c);
		lod.faces.push_back({{i, static_cast<uint16_t>(i + 1), static_cast<uint16_t>(i + 2)}, 0});
	};
	add({0, 1, 0}, {2, 1, 2}, {0, 1, 2}); // the top at y = 1, x 0..2, z 0..2
	physics::CollisionSubGeometry part;
	part.lods = {lod, lod, lod};
	physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	mesh.geometries = {geometry};
	physics::CollisionWorld world;
	world.addMesh(mesh, physics::CollisionUser::Soldier, {}, {});
	std::vector<physics::SphereContact> contacts;
	// Across the edge x = 0 (from (0,1,0) to (0,1,2)), 0.05 m over it, radius 0.1.
	world.capsuleEdgeContacts({-0.5f, 1.05f, 1}, {0.5f, 1.05f, 1}, 0.1f, contacts);
	CHECK(contacts.size() == 1);
	if (!contacts.empty()) {
		CHECK(contacts[0].normal.y > 0.99f && std::abs(contacts[0].depth + 0.05f) < 1e-3f);
	}
	contacts.clear();
	world.capsuleEdgeContacts({-0.5f, 0.95f, 1}, {0.5f, 0.95f, 1}, 0.1f, contacts);
	CHECK(contacts.size() == 1 && contacts[0].normal.y > 0.99f && contacts[0].depth > 0);
	contacts.clear();
	world.capsuleEdgeContacts({-0.5f, 1.5f, 1}, {0.5f, 1.5f, 1}, 0.1f, contacts);
	CHECK(contacts.empty());
}

// The manager's levels of detail (0x008ae120): by distance to the camera
// (tripled behind it, done by the caller), level 1 for every body with
// three awake ones nearer; a level-1 body steps 1.5 dT at a time
// (0x008add10) and a level-2 one lets its knees', elbows', a spine bone's
// and head's particles through the objects (0x008ac200).
TEST(RagDoll_LevelsOfDetail)
{
	CHECK((anim::ragDollLevels({5, 1, 3, 2, 4}, {true, true, true, true, true}) == std::vector<int> {1, 0, 0, 0, 1}));
	// Asleep ones don't count among the three.
	CHECK((anim::ragDollLevels({5, 1, 3, 2, 4}, {true, false, true, true, true}) == std::vector<int> {1, 0, 0, 0, 0}));
	CHECK((anim::ragDollLevels({}, {}).empty()));
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	const anim::RagDollTemplate* t = game.systems.ragDoll(kRagDollScript);
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	CHECK(t && skeleton);
	if (!t || !skeleton) {
		return;
	}
	std::vector<anim::Matrix> pose = anim::worldMatrices(*skeleton, anim::setupPose(*skeleton));
	for (anim::Matrix& m : pose) {
		m[13] += 5;
	}
	anim::RagDollWorld air;
	anim::RagDoll fine, coarse;
	fine.start(*t, pose, {0, 0, 0});
	coarse.start(*t, pose, {0, 0, 0});
	coarse.setLevelOfDetail(1);
	// 1/30 s: ragdoll_dT (1/90 s) x slowMotion 0.75 fits 2 steps (round
	// 0.025 / 0.0111) and 1.5 dT 2 steps of their own length (round 0.025 /
	// 0.0167 = 1.5): they fall differently far.
	for (int i = 0; i < 6; ++i) {
		fine.update(1 / 30.0f, air);
		coarse.update(1 / 30.0f, air);
	}
	CHECK(std::fabs(fine.middle()[1] - coarse.middle()[1]) > 1e-4f);
	// Level 2: an object below everything stops all but those five.
	anim::RagDollWorld floor;
	floor.raycast = [](const anim::Vec& from, const anim::Vec& to, anim::Vec& point, anim::Vec& normal, uint32_t& material) {
		if (from[1] < 4.0f || to[1] >= 4.0f) {
			return false;
		}
		const float k = (from[1] - 4.0f) / (from[1] - to[1]);
		point = {from[0] + (to[0] - from[0]) * k, 4.0f, from[2] + (to[2] - from[2]) * k};
		normal = {0, 1, 0};
		material = 0;
		return true;
	};
	// The constraints drag the others along a little, but the five fall
	// through the floor first; at level 0 they don't.
	const auto gap = [&](int lod) {
		anim::RagDoll body;
		body.start(*t, pose, {0, -10, 0});
		body.setLevelOfDetail(lod);
		for (int i = 0; i < 15; ++i) {
			body.update(1 / 30.0f, floor);
		}
		float skipped = 0, held = 0;
		int nSkipped = 0, nHeld = 0;
		for (const anim::RagDoll::Particle& p : body.particles()) {
			if (p.bone == 2 || p.bone == 7 || p.bone == 16 || p.bone == 32 || p.bone == 47) {
				skipped += p.position[1];
				++nSkipped;
			} else {
				held += p.position[1];
				++nHeld;
			}
		}
		return nSkipped > 0 && nHeld > 0 ? held / nHeld - skipped / nSkipped : 0.0f;
	};
	CHECK(gap(2) > 0.05f);
	CHECK(gap(0) < 0.05f);
}

// A soldier killed in a seat without a die bundle (0x005cea00 with
// died = 1, 0x005f0f50 with +0x2eb): thrown out at the seat's exit as a
// ragdoll pushed forward by rd_dieInVehicleForce; PlayerDeath at once,
// but the body stays (the armour's countdown 0x00652380): it goes once it
// has landed and armor.timeToStayAsWreck (20 by default, 0x00630040) has
// passed since the death, plus timeToStayAfterDestroyed (0).
TEST(RagDoll_ThrownOutOfASeatWithoutADieBundle)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	game.collisionTemplates.create("as_fav", "Objects/Vehicles/as_fav/");
	world::ObjectTemplate* car = game.templates.create("PlayerControlObject", "car");
	car->settings()["hasMobilePhysics"] = "1";
	car->settings()["physicsType"] = "Mesh";
	car->settings()["collisionMesh"] = "as_fav";
	car->settings()["seatInformation"] = "car 0/0.5/0.4 0/0/0";
	car->settings()["soldierExitLocation"] = "-2/0/0 0/0/0";
	car->settings()["armor.maxHitPoints"] = "500";
	car->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
	game.templates.registerType(9, "Engine");
	game.templates.registerType(10, "Spring");
	car->settings()["mass"] = "1800";
	world::ObjectTemplate* engine = game.templates.create("Engine", "car_engine");
	engine->settings()["engineType"] = "194";
	world::ObjectTemplate* wheel = game.templates.create("Spring", "car_wheel");
	wheel->settings()["strength"] = "40";
	wheel->settings()["damping"] = "8";
	car->children().push_back({"car_engine", {0, -0.44f, 0}, {}});
	for (float x : {-0.95f, 0.95f}) {
		for (float z : {-1.4f, 1.6f}) {
			engine->children().push_back({"car_wheel", {x, -0.15f, z}, {}});
		}
	}
	game.templates.create("EntryPoint", "car_entry")->settings()["entryRadius"] = "3";
	game.start();
	const uint32_t vehicle = game.objects.create(car, {3, 1.2f, 0}, {0, 0, 0})->id();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	game.run(3);
	const uint32_t soldier = game.world->playerObject();
	game::SoldierInput input;
	input.use = true;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	game.world->setPlayerInput({});
	game.run(0.5f);
	CHECK(game.world->playerVehicle() && game.world->playerVehicle()->id() == vehicle);
	game.world->setHitPoints(soldier, 0);
	CHECK(game.count("PlayerDeath") == 1);           // dead at once
	CHECK(game.world->seatCorpse(vehicle, 0) == 0);  // not slumped: no die bundle
	const world::Object* body = game.objects.find(soldier);
	CHECK(body && !body->hidden() && game.world->ragDoll(soldier) != nullptr);
	CHECK((game.world->corpses() == std::vector<uint32_t> {soldier}));
	if (!body) {
		return;
	}
	// Out at the exit, 2 m to the car's left (x - 2 at yaw 0), pushed
	// forward (+z).
	const world::Object* carObject = game.objects.find(vehicle);
	CHECK(carObject && body->position().x < carObject->position().x - 1);
	const float z0 = game.world->ragDoll(soldier)->middle()[2];
	game.run(2);
	CHECK(game.world->ragDoll(soldier) && game.world->ragDoll(soldier)->middle()[2] > z0 + 0.2f);
	game.run(15); // 17.5 s after the death: still there
	CHECK(game.objects.find(soldier) != nullptr);
	game.run(4);  // past 20 s, landed: gone
	CHECK(game.objects.find(soldier) == nullptr && game.world->corpses().empty());
}

// A body on foot after PlayerDeath with the soldiers' shipped armour
// (timeToStayAsWreck 6, timeToStayAfterDestroyed 1): once landed and six
// seconds dead, its countdown begins and it fades out linearly over a
// second (0x00652380: the geometry's alpha the time left over
// timeToStayAfterDestroyed), then it goes.
TEST(RagDoll_CorpseFadesOut)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
		game.templates.find(name)->settings()["armor.timeToStayAsWreck"] = "6";
		game.templates.find(name)->settings()["armor.timeToStayAfterDestroyed"] = "1";
	}
	game.start();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	game.run(1);
	const uint32_t soldier = game.world->playerObject();
	game.world->setHitPoints(soldier, 0);
	int ticks = 0;
	for (; ticks < 30 * 40 && game.count("PlayerDeath") == 0; ++ticks) {
		game.world->tick(1 / 30.0f);
	}
	CHECK(game.count("PlayerDeath") == 1);
	const world::Object* body = game.objects.find(soldier);
	CHECK(body != nullptr);
	if (!body) {
		return;
	}
	// Past six seconds and landed: the first corpse step begins the fade.
	game.world->tick(1 / 30.0f);
	CHECK(body->fade() < 1.0f && body->fade() > 0.9f);
	game.run(0.5f);
	CHECK(std::abs(body->fade() - 0.47f) < 0.05f);
	game.run(0.6f);
	CHECK(game.objects.find(soldier) == nullptr && game.world->corpses().empty());
}

// The network side: a server's body sends its networked particles (the
// hips and shoulders: bones 1, 6, 15, 31), a client's network-driven body
// follows them (0x008acce0: the first update puts them there, then each
// step pulls them ragdoll_net_interpolate of the way and the constraints
// hold them), and 0.3 s without an update leaves it to itself. A remote
// soldier's object in GameWorld gets such a body and its pose.
TEST(RagDoll_NetworkDrivenBody)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	const anim::RagDollTemplate* t = game.systems.ragDoll(kRagDollScript);
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	CHECK(t && skeleton);
	if (!t || !skeleton) {
		return;
	}
	CHECK(std::abs(t->settings.netInterpolate - 0.1f) < 1e-6f && t->settings.netBoneEnable == 15);
	std::vector<anim::Matrix> pose = anim::worldMatrices(*skeleton, anim::setupPose(*skeleton));
	for (anim::Matrix& m : pose) {
		m[13] += 1.0f;
	}
	anim::RagDollWorld world;
	world.ground = [](float, float, anim::Vec& normal) {
		normal = {0, 1, 0};
		return 0.0f;
	};
	anim::RagDoll server, client;
	server.start(*t, pose, {2, 0, 0});
	// The client's body starts elsewhere (a guess at the pose); the first
	// update snaps its networked particles.
	std::vector<anim::Matrix> guess = pose;
	for (anim::Matrix& m : guess) {
		m[12] += 3.0f;
	}
	client.start(*t, guess, {0, 0, 0});
	client.setNetworkDriven(true);
	CHECK(server.networkedPositions().size() == 4);
	CHECK(&server.particles()[3] && server.particles()[3].bone == 6 && server.particles()[3].networked);
	const anim::Vec before = client.middle();
	client.update(1 / 30.0f, world); // nothing until the first update
	CHECK(distance(client.middle(), before) < 1e-6f);
	// Falling, the client trails (a tenth of the way a step); at rest, it
	// is there.
	float worst = 0;
	for (int i = 0; i < 90; ++i) {
		server.update(1 / 30.0f, world);
		client.receive(server.networkedPositions());
		client.update(1 / 30.0f, world);
		if (i >= 60) {
			const std::vector<anim::Vec> a = server.networkedPositions(), b = client.networkedPositions();
			for (size_t k = 0; k < a.size(); ++k) {
				worst = std::max(worst, distance(a[k], b[k]));
			}
		}
	}
	std::fprintf(stderr, "  the client's networked particles at most %.3f m from the server's\n", worst);
	CHECK(client.networkDriven() && worst < 0.05f);
	CHECK(distance(client.middle(), server.middle()) < 0.05f);
	// No update for 0.3 s (of ragdoll time, slowMotion 0.75): its own.
	for (int i = 0; i < 15; ++i) {
		client.update(1 / 30.0f, world);
	}
	CHECK(!client.networkDriven());

	// A remote soldier's object: a body at its middle particle, posed.
	game.start();
	world::ObjectTemplate* soldier = game.templates.find("us_light_soldier");
	const uint32_t ghost = game.objects.create(soldier, {5, 0, 5}, {90, 0, 0})->id();
	std::vector<physics::Vec3> sent;
	for (const anim::Vec& p : server.networkedPositions()) {
		sent.push_back({p[0] + 5, p[1], p[2] + 5});
	}
	CHECK(game.world->driveRemoteRagDoll(ghost, sent));
	const anim::RagDoll* remote = game.world->remoteRagDoll(ghost);
	CHECK(remote && remote->networkDriven());
	game.run(0.2f);
	const world::Object* object = game.objects.find(ghost);
	CHECK(object && remote && std::abs(object->position().x - remote->middle()[0]) < 1e-4f);
	CHECK(object && std::abs(object->position().x - sent[1].x) < 0.3f);
	std::vector<anim::Matrix> posed;
	CHECK(game.world->skeletonPose(ghost, *skeleton, posed) && posed.size() == skeleton->nodes.size());
	game.world->removeRemoteRagDoll(ghost);
	CHECK(game.world->remoteRagDoll(ghost) == nullptr && !game.world->skeletonPose(ghost, *skeleton, posed));
}

namespace {

// A collision LOD of `type` with its box from its vertices (as the
// shipped meshes have it: the box gives the LOD's radius, slot 0x50).
physics::CollisionLod boxedLod(uint32_t type, std::vector<std::array<float, 3>> vertices,
	std::vector<physics::CollisionFace> faces)
{
	physics::CollisionLod lod;
	lod.collisionType = type;
	lod.boundsMin = {1e9f, 1e9f, 1e9f};
	lod.boundsMax = {-1e9f, -1e9f, -1e9f};
	for (const auto& v : vertices) {
		for (int k = 0; k < 3; ++k) {
			lod.boundsMin[static_cast<size_t>(k)] = std::min(lod.boundsMin[static_cast<size_t>(k)], v[static_cast<size_t>(k)]);
			lod.boundsMax[static_cast<size_t>(k)] = std::max(lod.boundsMax[static_cast<size_t>(k)], v[static_cast<size_t>(k)]);
		}
	}
	lod.vertices = std::move(vertices);
	lod.faces = std::move(faces);
	return lod;
}

// The FAV-like car of RagDoll_ThrownOutOfASeatWithoutADieBundle, at `at`.
uint32_t makeCar(Game& game, physics::Vec3 at)
{
	if (!game.templates.find("car")) {
		game.collisionTemplates.create("as_fav", "Objects/Vehicles/as_fav/");
		world::ObjectTemplate* car = game.templates.create("PlayerControlObject", "car");
		car->settings()["hasMobilePhysics"] = "1";
		car->settings()["physicsType"] = "Mesh";
		car->settings()["collisionMesh"] = "as_fav";
		car->settings()["seatInformation"] = "car 0/0.5/0.4 0/0/0";
		car->settings()["soldierExitLocation"] = "-2/0/0 0/0/0";
		car->settings()["armor.maxHitPoints"] = "500";
		car->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
		game.templates.registerType(9, "Engine");
		game.templates.registerType(10, "Spring");
		car->settings()["mass"] = "1800";
		world::ObjectTemplate* engine = game.templates.create("Engine", "car_engine");
		engine->settings()["engineType"] = "194";
		world::ObjectTemplate* wheel = game.templates.create("Spring", "car_wheel");
		wheel->settings()["strength"] = "40";
		wheel->settings()["damping"] = "8";
		car->children().push_back({"car_engine", {0, -0.44f, 0}, {}});
		for (float x : {-0.95f, 0.95f}) {
			for (float z : {-1.4f, 1.6f}) {
				engine->children().push_back({"car_wheel", {x, -0.15f, z}, {}});
			}
		}
		game.templates.create("EntryPoint", "car_entry")->settings()["entryRadius"] = "3";
	}
	return game.objects.create(game.templates.find("car"), {at.x, at.y, at.z}, {0, 0, 0})->id();
}

// A pole with a type-4 LOD (a trunk: two faces in the x-y plane), 0.6 m
// across (treeCollisionDiameter), and a vehicle LOD (as the shipped trees
// have: 0x006bb050 tests nothing of a mesh without one) of a thin post.
void makePole(Game& game, physics::Vec3 at)
{
	const physics::CollisionLod lod = boxedLod(4, {{-0.05f, -1, 0}, {0.05f, -1, 0}, {0, 4, 0}},
		{{{0, 2, 1}, 0}, {{0, 1, 2}, 0}});
	const physics::CollisionLod vehicle = boxedLod(1, {{-0.05f, -1, 0}, {0.05f, -1, 0}, {0, 4, 0}},
		{{{0, 2, 1}, 0}, {{0, 1, 2}, 0}});
	physics::CollisionSubGeometry part;
	part.lods = {vehicle, lod};
	physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	physics::CollisionMeshFile mesh;
	mesh.version = 10;
	mesh.geometries = {geometry};
	game.collisionTemplates.add("pole_mesh", mesh);
	game.templates.registerType(11, "SimpleObject");
	world::ObjectTemplate* pole = game.templates.create("SimpleObject", "pole");
	pole->settings()["collisionMesh"] = "pole_mesh";
	pole->settings()["treeCollisionDiameter"] = "0.6";
	game.objects.create(pole, {at.x, at.y, at.z}, {0, 0, 0});
}

// The car driven forward (+z) for `seconds`: where it ends up.
float driveCar(Game& game, uint32_t car, float seconds)
{
	game::SoldierInput input;
	input.use = true;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	game.world->setPlayerInput({});
	game.run(0.5f);
	CHECK(game.world->playerVehicle() && game.world->playerVehicle()->id() == car);
	game::SoldierInput drive;
	drive.throttle = 1;
	game.world->setPlayerInput(drive);
	game.run(seconds);
	return game.objects.find(car)->position().z;
}

} // namespace

// Trees and poles (collision type 4) against a vehicle (0x006bb050's
// third pass): a car driven at a pole stops at its trunk; without the pole
// it drives on past.
TEST(Hull_TreeStopsACar)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	float ends[2] = {};
	for (int withPole = 0; withPole < 2; ++withPole) {
		Game game(gameDir);
		const uint32_t car = makeCar(game, {3, 1.2f, 0});
		if (withPole) {
			makePole(game, {3, 0, 12});
		}
		game.start();
		CHECK(game.world->spawnPlayer(2) != nullptr);
		game.run(3);
		ends[withPole] = driveCar(game, car, 6);
	}
	std::fprintf(stderr, "  the car ends at z %.2f without the pole, %.2f with it\n", ends[0], ends[1]);
	CHECK(ends[0] > 14);
	CHECK(ends[1] < 12 && ends[1] > 8);
}

// A vehicle whose first Engine is a helicopter's (c_ETHelicopter) whose
// origin passes through a static object's vehicle LOD between ticks is
// destroyed (0x006bb050's early exit: material 0x29a, 1e10 damage); the
// same with a car's engine is not.
TEST(Hull_HelicopterHittingAStaticIsDestroyed)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	bool destroyed[2] = {};
	for (int helicopter = 0; helicopter < 2; ++helicopter) {
		Game game(gameDir);
		const uint32_t car = makeCar(game, {3, 3.0f, 0});
		if (helicopter) {
			game.templates.find("car_engine")->settings()["engineType"] = "c_ETHelicopter";
		}
		// A thin bar (both sides) across under the car's middle at 1.5 m,
		// longer than the car is wide: the car's hull falls round it (no
		// vertex of either crosses the other's faces), its origin through.
		// Not over four times smaller than the car (else only the bar's
		// vertices would go into the car's mesh, 0x006dc7b0).
		const physics::CollisionLod lod = boxedLod(1, {{-1.5f, 0, -0.05f}, {1.5f, 0, -0.05f}, {1.5f, 0, 0.05f}, {-1.5f, 0, 0.05f}},
			{{{0, 1, 2}, 0}, {{0, 2, 3}, 0}, {{0, 2, 1}, 0}, {{0, 3, 2}, 0}});
		physics::CollisionSubGeometry part;
		part.lods = {lod};
		physics::CollisionGeometry geometry;
		geometry.subGeometries = {part};
		physics::CollisionMeshFile mesh;
		mesh.version = 10;
		mesh.geometries = {geometry};
		game.collisionTemplates.add("plate_mesh", mesh);
		game.templates.registerType(11, "SimpleObject");
		world::ObjectTemplate* plate = game.templates.create("SimpleObject", "plate");
		plate->settings()["collisionMesh"] = "plate_mesh";
		game.objects.create(plate, {3, 1.5f, 0}, {0, 0, 0});
		game.start();
		game.run(2);
		destroyed[helicopter] = game.objects.find(car) && game.objects.find(car)->destroyed();
	}
	CHECK(!destroyed[0]);
	CHECK(destroyed[1]);
}

// A car on a static platform stands on its wheels' probes (0x006bb050's
// vertex pass for a part of under 4 vertices: a segment down through the
// wheel's bottom) as it stands on the terrain on its springs, not on its hull.
TEST(Hull_WheelsProbeStaticObjects)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	float rest[2] = {};
	for (int platform = 0; platform < 2; ++platform) {
		Game game(gameDir);
		const float base = platform ? 2.0f : 0.0f;
		const uint32_t car = makeCar(game, {3, base + 1.5f, 0});
		if (platform) {
			const physics::CollisionLod lod = boxedLod(1, {{-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, {-10, 0, 10}},
				{{{0, 1, 2}, 0}, {{0, 2, 3}, 0}, {{0, 2, 1}, 0}, {{0, 3, 2}, 0}});
			physics::CollisionSubGeometry part;
			part.lods = {lod};
			physics::CollisionGeometry geometry;
			geometry.subGeometries = {part};
			physics::CollisionMeshFile mesh;
			mesh.version = 10;
			mesh.geometries = {geometry};
			game.collisionTemplates.add("deck_mesh", mesh);
			game.templates.registerType(11, "SimpleObject");
			world::ObjectTemplate* deck = game.templates.create("SimpleObject", "deck");
			deck->settings()["collisionMesh"] = "deck_mesh";
			game.objects.create(deck, {3, base, 0}, {0, 0, 0});
		}
		game.start();
		game.run(4);
		rest[platform] = game.objects.find(car)->position().y - base;
	}
	std::fprintf(stderr, "  at rest %.3f m over the terrain, %.3f m over the platform\n", rest[0], rest[1]);
	CHECK(std::abs(rest[0] - rest[1]) < 0.25f);
}

namespace {

// A static SimpleObject `name` at `at` whose collision mesh is `lods`.
world::ObjectTemplate* addStatic(Game& game, const char* name, std::vector<physics::CollisionLod> lods, physics::Vec3 at,
	physics::Vec3 rotation = {})
{
	physics::CollisionSubGeometry part;
	part.lods = std::move(lods);
	physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	physics::CollisionMeshFile mesh;
	mesh.version = 10;
	mesh.geometries = {geometry};
	game.collisionTemplates.add(std::string(name) + "_mesh", mesh);
	if (!game.templates.find("SimpleObject_type")) {
		game.templates.registerType(11, "SimpleObject");
		game.templates.create("SimpleObject", "SimpleObject_type");
	}
	world::ObjectTemplate* t = game.templates.create("SimpleObject", name);
	t->settings()["collisionMesh"] = std::string(name) + "_mesh";
	game.objects.create(t, {at.x, at.y, at.z}, {rotation.x, rotation.y, rotation.z});
	return t;
}

// A wall in the x-y plane (both sides), `half` wide each way, 0 to 4 m up.
physics::CollisionLod wallLod(float half)
{
	return boxedLod(1, {{-half, 0, 0}, {half, 0, 0}, {half, 4, 0}, {-half, 4, 0}},
		{{{0, 1, 2}, 0}, {{0, 2, 3}, 0}, {{0, 2, 1}, 0}, {{0, 3, 2}, 0}});
}

} // namespace

// The vertex pass's ways (0x006bb050): a moving A's vertex runs from A's
// centre of mass a tick ago to where it is now, so a vertex already behind
// a face still meets it from the front. A car set down with its front
// through a wall is pushed back out of it (with ways from the vertices'
// last places, those behind the wall would never cross it).
TEST(Hull_VertexWaysFromTheCentreOfMass)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	const uint32_t car = makeCar(game, {3, 1.2f, 0});
	addStatic(game, "wall", {wallLod(6)}, {3, -1, 1.2f});
	game.start();
	game.run(2);
	const float z = game.objects.find(car)->position().z;
	std::fprintf(stderr, "  the car's origin ends at z %.2f (the wall at 1.2, its front 2.0 m ahead)\n", z);
	CHECK(z < 1.2f - 1.5f);
}

// A static with treeCollisionDiameter and a vehicle LOD but no type-4 LOD
// gets a trunk line up its vehicle LOD's box (0x006bb050's third pass:
// one face (cx, min y, 0), (cx, max y, 0), (cx + 0.05, min y, 0); the
// shipped brick_pillarjba). Here the vehicle LOD is three vertices in a
// line (no face of its own, and under 4 vertices a static's vertex pass
// does nothing): only the trunk line stops the car.
TEST(Hull_TrunkLineWithoutATypeFourLod)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	float ends[2] = {};
	for (int diameter = 0; diameter < 2; ++diameter) {
		Game game(gameDir);
		const uint32_t car = makeCar(game, {3, 1.2f, 0});
		world::ObjectTemplate* post =
			addStatic(game, "post", {boxedLod(1, {{0, -1, 0}, {0, 1.5f, 0}, {0, 4, 0}}, {{{0, 1, 2}, 0}})}, {3, 0, 12});
		if (diameter) {
			post->settings()["treeCollisionDiameter"] = "0.6";
		}
		game.start();
		CHECK(game.world->spawnPlayer(2) != nullptr);
		game.run(3);
		ends[diameter] = driveCar(game, car, 6);
	}
	std::fprintf(stderr, "  the car ends at z %.2f without a diameter, %.2f with one\n", ends[0], ends[1]);
	CHECK(ends[0] > 14);
	CHECK(ends[1] < 12 && ends[1] > 8);
}

// Object against object (0x006dc7b0 -> 0x006bb050 both ways, shares by
// mass 0x006b6cc0): a car driven into a parked one pushes it along and
// doesn't pass through it.
TEST(Hull_CarPushesAParkedCar)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	const uint32_t car = makeCar(game, {3, 1.2f, 0});
	const uint32_t parked = game.objects.create(game.templates.find("car"), {3, 1.2f, 9}, {0, 0, 0})->id();
	game.start();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	game.run(3);
	const float parkedAt = game.objects.find(parked)->position().z;
	const float end = driveCar(game, car, 6);
	const float pushedTo = game.objects.find(parked)->position().z;
	std::fprintf(stderr, "  the driven car ends at z %.2f; the parked one from %.2f to %.2f\n", end, parkedAt, pushedTo);
	CHECK(pushedTo > parkedAt + 1);
	CHECK(end < pushedTo);
}

// The small-object pre-trace (0x006bb050, LOD radius under 0.3): a small
// mobile object's way from its centre of mass a tick ago to its origin
// stops at the first face it crosses, and the object is put back there.
// Its vehicle LOD sits 0.15 to 0.25 m over its origin, so its vertices
// don't reach the deck yet: in the tick its origin falls through the deck
// (a third of a metre a tick by then) it is put back onto it.
TEST(Hull_SmallObjectPreTrace)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	std::vector<std::array<float, 3>> corners;
	for (int i = 0; i < 8; ++i) {
		corners.push_back({(i & 1) ? 0.05f : -0.05f, (i & 2) ? 0.25f : 0.15f, (i & 4) ? 0.05f : -0.05f});
	}
	// Its faces (outward): -y, +y, -x, +x, -z, +z.
	const std::vector<physics::CollisionFace> faces = {{{0, 1, 5}, 0}, {{0, 5, 4}, 0}, {{2, 6, 7}, 0}, {{2, 7, 3}, 0},
		{{0, 4, 6}, 0}, {{0, 6, 2}, 0}, {{1, 3, 7}, 0}, {{1, 7, 5}, 0}, {{0, 2, 3}, 0}, {{0, 3, 1}, 0}, {{4, 5, 7}, 0},
		{{4, 7, 6}, 0}};
	physics::CollisionSubGeometry part;
	part.lods = {boxedLod(1, corners, faces)};
	physics::CollisionMeshFile mesh;
	mesh.version = 10;
	mesh.geometries.resize(1);
	mesh.geometries[0].subGeometries = {part};
	game.collisionTemplates.add("can_mesh", mesh);
	game.templates.registerType(12, "SimpleObject");
	world::ObjectTemplate* can = game.templates.create("SimpleObject", "can");
	can->settings()["collisionMesh"] = "can_mesh";
	can->settings()["hasMobilePhysics"] = "1";
	can->settings()["physicsType"] = "Mesh";
	const uint32_t object = game.objects.create(can, {3, 6, 0}, {0, 0, 0})->id();
	addStatic(game, "deck", {boxedLod(1, {{-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, {-10, 0, 10}},
								 {{{0, 1, 2}, 0}, {{0, 2, 3}, 0}, {{0, 2, 1}, 0}, {{0, 3, 2}, 0}})},
		{3, 2, 0});
	game.start();
	float y = 6;
	for (int tick = 0; tick < 90 && y > 2.05f; ++tick) {
		game.world->tick(1 / 30.0f);
		y = game.objects.find(object)->position().y;
	}
	std::fprintf(stderr, "  the small object's origin first under 2.05 at %.3f (the deck at 2)\n", y);
	CHECK(y > 1.99f && y < 2.01f);
}

// The segment test of the vertex pass (0x006bd070 / 0x006e62c0): only the
// faces crossed from the front, the earliest of them.
TEST(CollisionWorld_SegmentMeetsFacesFromTheFront)
{
	physics::CollisionLod lod;
	lod.collisionType = 1;
	// Two floors at y 0 and y -1, the upper one facing down, the lower up.
	lod.vertices = {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}};
	lod.faces = {{{0, 2, 1}, 0}, {{3, 4, 5}, 0}};
	physics::CollisionSubGeometry part;
	part.lods = {lod};
	physics::CollisionMeshFile mesh;
	mesh.version = 10;
	mesh.geometries.resize(1);
	mesh.geometries[0].subGeometries = {part};
	physics::CollisionWorld world;
	world.addMesh(mesh, physics::CollisionUser::Vehicle, {}, {});
	physics::SphereContact contact;
	// Down through both: the upper face is met from behind, the lower one counts.
	CHECK(world.segmentContact({0.5f, 1, -0.5f}, {0.5f, -1.5f, -0.5f}, contact));
	CHECK(std::fabs(contact.point.y + 1) < 1e-5f && contact.normal.y > 0.99f);
	CHECK(std::fabs(contact.depth + 0.5f) < 1e-5f);
	// The same mesh turned a quarter and moved to (10, 0, 0): local
	// (0.5, y, -0.5) is world (9.5, y, -0.5); the contact comes back in
	// world space.
	const physics::Mat3 turned = physics::Mat3::fromYawPitchRoll({90, 0, 0});
	CHECK(world.segmentContact({9.5f, 1, -0.5f}, {9.5f, -1.5f, -0.5f}, turned, {10, 0, 0}, contact));
	CHECK(std::fabs(contact.point.x - 9.5f) < 1e-4f && std::fabs(contact.point.y + 1) < 1e-5f && contact.normal.y > 0.99f);
}

// Level 2 for network-driven bodies (0x008ae120): the manager updates the
// bodies nearest the camera first and gives level 2 to a network-driven
// one whose turn comes after ragdoll_clientMaxUpdateTime (0.002 ms) of the
// frame's updates by the performance counter. With a clock that moves
// 1.5 us a reading, the nearer remote body is in time, the farther not.
TEST(RagDoll_RemoteBodiesPastTheFrameBudget)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	const anim::RagDollTemplate* t = game.systems.ragDoll(kRagDollScript);
	CHECK(t && std::abs(t->settings.clientMaxUpdateTime - 0.002f) < 1e-7f);
	game.start();
	double now = 100;
	game.world->setClock([&now] { return now += 1.5e-6; });
	world::ObjectTemplate* soldier = game.templates.find("us_light_soldier");
	const uint32_t near = game.objects.create(soldier, {2, 0, 5}, {0, 0, 0})->id();
	const uint32_t far = game.objects.create(soldier, {2, 0, 40}, {0, 0, 0})->id();
	for (const uint32_t ghost : {far, near}) {
		const float z = ghost == near ? 5.0f : 40.0f;
		CHECK(game.world->driveRemoteRagDoll(ghost, {{2, 1, z}, {2.1f, 1, z}, {1.9f, 1.4f, z}, {2.2f, 1.4f, z}}));
	}
	// Without a camera (a server) every body is at level 0.
	game.world->tick(1 / 30.0f);
	CHECK(game.world->remoteRagDoll(near)->levelOfDetail() == 0 && game.world->remoteRagDoll(far)->levelOfDetail() == 0);
	game.world->setCamera({2, 1.5f, 0}, {0, 0, 1});
	game.world->tick(1 / 30.0f);
	const int nearLevel = game.world->remoteRagDoll(near)->levelOfDetail();
	const int farLevel = game.world->remoteRagDoll(far)->levelOfDetail();
	std::fprintf(stderr, "  levels: the nearer body %d, the farther %d\n", nearLevel, farLevel);
	CHECK(nearLevel == 0 && farLevel == 2);
	// A body left to itself (no update for 0.3 s) is not network-driven any
	// more: back to the distance levels.
	game.run(0.5f);
	CHECK(!game.world->remoteRagDoll(far)->networkDriven() && game.world->remoteRagDoll(far)->levelOfDetail() == 0);
}

// A client runs the armour's corpse countdown from the networked health
// (0x005fcdb0 -> the armour's setter -> 0x00654120: wreck at 0; 0x00654d80
// -> 0x00652380 fades the soldier, no server test before the removal):
// while a player is attached nothing happens; detached and landed,
// timeToStayAsWreck after the wreck the body fades over
// timeToStayAfterDestroyed, and stays (the server removes the ghost).
// Health back above 0 makes it whole.
TEST(RagDoll_RemoteCorpseFadesFromTheNetworkedHealth)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	world::ObjectTemplate* soldier = game.templates.find("us_light_soldier");
	soldier->settings()["armor.timeToStayAsWreck"] = "6";
	soldier->settings()["armor.timeToStayAfterDestroyed"] = "1";
	game.start();
	const uint32_t ghost = game.objects.create(soldier, {2, 0, 5}, {0, 0, 0})->id();
	CHECK(game.world->driveRemoteRagDoll(ghost, {{2, 1, 5}, {2.1f, 1, 5}, {1.9f, 1.4f, 5}, {2.2f, 1.4f, 5}}));
	game.world->setRemoteHealth(ghost, 0, true);
	game.run(8);
	CHECK(game.objects.find(ghost)->fade() == 1); // a player still on it
	game.world->setRemoteHealth(ghost, 0, false);
	game.run(0.5f);
	const float fading = game.objects.find(ghost)->fade();
	game.run(1);
	std::fprintf(stderr, "  fade %.2f half a second after the player left, then %.2f\n", fading, game.objects.find(ghost)->fade());
	CHECK(fading > 0.3f && fading < 0.7f);
	CHECK(game.objects.find(ghost) && game.objects.find(ghost)->fade() == 0);
	game.world->setRemoteHealth(ghost, 0.5f, false);
	CHECK(game.objects.find(ghost)->fade() == 1);
	// It waits 60 s at most, a player attached or not.
	const uint32_t other = game.objects.create(soldier, {6, 50, 5}, {0, 0, 0})->id();
	game.world->setRemoteHealth(other, 0, true);
	game.run(59);
	CHECK(game.objects.find(other)->fade() == 1);
	game.run(2.5f);
	CHECK(game.objects.find(other)->fade() == 0);
}

TEST(RagDoll_ImpactSoundFromTheFootStepsMaterial)
{
	// A falling body's impact: the cell of material 0x157c (5500,
	// Soldier_FootSteps) and the ground's, its sound template 4 (0x007fd780
	// -> 0x0060a6d0), the cell against material 0 when that one has none.
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	physics::MaterialManager materials;
	materials.createCell(5500, 0).soundTemplates = {"S_Walk_Dirt", "S_Walk_Dirt", "S_Prone_Dirt", "S_Sprint_Dirt",
		"S_Soldier_RagDoll_Impact"};
	materials.createCell(24, 0).soundTemplates = {"not_this_one"};
	game.start(&materials);
	CHECK(game.world->spawnPlayer(2) != nullptr);
	game.run(2);
	(void)game.world->takeSounds();
	game.world->setHitPoints(game.world->playerObject(), 0);
	std::vector<std::string> heard;
	for (int i = 0; i < 90; ++i) {
		game.world->tick(1 / 30.0f);
		for (const auto& sound : game.world->takeSounds()) {
			heard.push_back(sound.sound);
		}
	}
	CHECK(!heard.empty());
	CHECK(std::all_of(heard.begin(), heard.end(), [](const std::string& s) { return s == "S_Soldier_RagDoll_Impact"; }));
}
