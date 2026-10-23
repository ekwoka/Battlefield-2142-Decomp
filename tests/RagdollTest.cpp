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
