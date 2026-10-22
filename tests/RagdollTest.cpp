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
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
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
	void start()
	{
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->enablePhysics(files, collisionTemplates,
			{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return 0.0f;
			 }});
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

	// The medic shocks it back: up where it lay, the get-up animation playing.
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
	CHECK(gettingUp);
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
