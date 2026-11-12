// Ragdolls and seat poses with the game's own data (needs
// $BF2142_GAME_DIR): ragDollInit.con's template, a killed soldier's body
// falling to rest on flat ground in one piece and getting up when revived,
// and a soldier seated in a car posed by the seat's animation system.
#include "Animation/AnimationConsole.h"
#include "Animation/AnimationSystem.h"
#include "Animation/Ik.h"
#include "Animation/RagDoll.h"
#include "Game/CombatAreas.h"
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Main/HudWorld.h"
#include "Main/GameAudio.h"
#include "Main/HudMenusComm.h"
#include "IO/File/FileManager.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/CollisionWorld.h"
#include "Physics/MaterialManager.h"
#include "Terrain/Heightmap.h"
#include "Sound/Game/GameSounds.h"
#include "Sound/Game/SoundConsole.h"
#include "Sound/Game/SoundManager.h"
#include "Sound/Game/SoundObjectTemplate.h"
#include "Sound/Game/VoiceMessages.h"
#include "Sound/SoundEngine/OggStream.h"
#include "Hud/Hud.h"
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
	physics::MaterialManager materials;
	std::unique_ptr<game::GameWorld> world;
	std::vector<std::string> events;
	bool valueHolders = false;

	explicit Game(const char* gameDir, const char* modPath = "mods/bf2142", bool clientArchives = false)
	{
		files.setRootPath(gameDir);
		files.addSearchPath(modPath);
		if (std::string(modPath) != "mods/bf2142") {
			files.addSearchPath("mods/bf2142");
		}
		bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
			[this](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
		std::string out;
		console.run("ServerArchives.con", {}, out);
		if (clientArchives) {
			files.setMountingAllowed(true);
			console.run("ClientArchives.con", {}, out);
			files.setMountingAllowed(false);
		}
		anim::registerAnimationConsoleMethods(console, systems);
		physics::registerMaterialConsoleMethods(console, materials);
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
		console.run("Common/Material/MaterialManagerSettings.con", {}, out);
	}

	// The world, once the test's templates are in place.
	// `slope`: the ground rises by that much a metre along x (0: flat).
	void start(const physics::MaterialManager* materials = nullptr, uint32_t surface = 0, float slope = 0)
	{
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->enablePhysics(files, collisionTemplates,
			{[slope](float x, float) { return x * slope; }, [slope](float x, float, physics::Vec3& normal) {
				 const float length = std::sqrt(slope * slope + 1);
				 normal = {-slope / length, 1 / length, 0};
				 return x * slope;
			 }, [surface](float, float) { return surface; }},
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
	defib->settings()["ammo.autoReload"] = "1"; // as Unl_Defibrillator
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
	// The kill's own generated "S_<soldier>_Death" cue is not an impact; only impacts must come from material 5500.
	heard.erase(std::remove_if(heard.begin(), heard.end(), [](const std::string& s) { return s.size() > 6 && s.compare(s.size() - 6, 6, "_Death") == 0; }), heard.end());
	CHECK(!heard.empty());
	CHECK(std::all_of(heard.begin(), heard.end(), [](const std::string& s) { return s == "S_Soldier_RagDoll_Impact"; }));
}

// Sleeping (0x006c9060): a parked car stops being stepped once it has been
// still for 30 ticks; the engine wakes it (0x0064e920) when driven.
TEST(Physics_ParkedCarSleepsAndWakesWhenDriven)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	const uint32_t car = makeCar(game, {3, 1.2f, 0});
	game.start();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	game.run(4);
	CHECK(game.world->bodyAsleep(car));
	const float y = game.objects.find(car)->position().y;
	game.run(1);
	CHECK(game.objects.find(car)->position().y == y);
	const float end = driveCar(game, car, 3);
	CHECK(!game.world->bodyAsleep(car));
	CHECK(end > 3);
}

// The push of a blast on an object's node (0x005a66e0: slot 0xa4 at a point
// between it and the blast, the armour's explosionForceMod and Max): a car
// beside a blast is shoved away from it and woken; a major actor is not
// pushed (node slot 0x1c4).
TEST(Physics_BlastPushesACar)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	float moved[2] = {};
	for (int major = 0; major < 2; ++major) {
		Game game(gameDir);
		const uint32_t car = makeCar(game, {3, 1.2f, 0});
		world::ObjectTemplate* t = game.templates.find("car");
		t->settings()["armor.explosionForceMod"] = "16";
		t->settings()["armor.explosionForceMax"] = "5000";
		if (major) {
			t->settings()["isMajorActor"] = "1";
		}
		world::ObjectTemplate* blast = game.templates.create("GenericProjectile", "blast");
		blast->components().push_back("DefaultDetonationComp");
		blast->settings()["detonation.explosionDamage"] = "1";
		blast->settings()["detonation.explosionRadius"] = "10";
		blast->settings()["detonation.explosionForce"] = "40";
		blast->settings()["detonation.explosionMaterial"] = "69";
		game.start();
		game.run(4);
		CHECK(game.world->bodyAsleep(car));
		const float x = game.objects.find(car)->position().x;
		CHECK(game.world->detonate("blast", {0.5f, 1.0f, 0}));
		game.run(2);
		moved[major] = game.objects.find(car)->position().x - x;
	}
	std::fprintf(stderr, "  pushed %.2f m; a major actor %.2f m\n", moved[0], moved[1]);
	CHECK(moved[0] > 0.2f);
	CHECK(std::fabs(moved[1]) < 0.01f);
}

// collisionGroups (template +0x104, response slot 0x50): two objects
// sharing a bit don't meet (0x006dc7b0); a team's PlayerControlObject
// passes through what has its team's bit (0x100 team 1, 0x200 team 2,
// 0x006bb050's start). A car driven at a wall stops; with the wall in the
// car's group, or the wall the EU's (0x200) and the car EU's, it drives on.
TEST(Physics_CollisionGroups)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	float ends[3] = {};
	for (int c = 0; c < 3; ++c) {
		Game game(gameDir);
		const uint32_t car = makeCar(game, {3, 1.2f, 0});
		world::ObjectTemplate* wall = addStatic(game, "wall", {wallLod(6)}, {3, -1, 12});
		if (c == 1) {
			wall->settings()["collisionGroups"] = "4";
			game.templates.find("car")->settings()["collisionGroups"] = "4";
		} else if (c == 2) {
			wall->settings()["collisionGroups"] = "512";
			game.objects.find(car)->setTeam(2);
		}
		game.start();
		CHECK(game.world->spawnPlayer(2) != nullptr);
		game.run(3);
		ends[c] = driveCar(game, car, 6);
	}
	std::fprintf(stderr, "  the car ends at z %.2f, %.2f in the wall's group, %.2f through its team's wall\n", ends[0],
		ends[1], ends[2]);
	CHECK(ends[0] < 12);
	CHECK(ends[1] > 14);
	CHECK(ends[2] > 14);
}

// The collision events (object slot 0xac -> 0x005a4690): a car driven into
// a wall takes (speed - 4)^2 x cos^2 x the material cell's damageMod (1
// here) once for the pair within a second; a soldier standing in its way is
// run over (0x005a43c0: (speed - 3) x speed x the mass factor, at most 3).
TEST(Physics_CollisionDamageAndRoadKill)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	{
		Game game(gameDir);
		const uint32_t car = makeCar(game, {3, 1.2f, 0});
		addStatic(game, "wall", {wallLod(6)}, {3, -1, 25});
		game.start();
		CHECK(game.world->spawnPlayer(2) != nullptr);
		game.run(3);
		CHECK(game.world->hitPoints(car) == 500);
		driveCar(game, car, 6);
		std::fprintf(stderr, "  the car has %.1f of 500 hit points after the wall\n", game.world->hitPoints(car));
		CHECK(game.world->hitPoints(car) < 500);
	}
	{
		Game game(gameDir);
		const uint32_t car = makeCar(game, {3, 1.2f, 0});
		// newCar2's torque follows the engine's torque curve at the rpm
		// (0x0064f2a0: 0.7 of it at full rpm); a stronger engine than the
		// default 60 brings the car over the run-over speed.
		game.templates.find("car_engine")->settings()["torque"] = "90";
		game.start();
		CHECK(game.world->spawnPlayer(2) != nullptr);
		const int victim = game.world->addPlayer("Victim", 1, false);
		CHECK(game.world->spawnSoldier(victim));
		game::GameWorld::PlayerInfo info;
		game.world->playerInfo(victim, info);
		CHECK(game.world->setSoldierPosition(info.soldier, {3, 0, 22}));
		game.run(3);
		driveCar(game, car, 6);
		game.world->playerInfo(victim, info);
		game::GameWorld::PlayerInfo driver;
		game.world->playerInfo(game.world->localPlayer(), driver);
		std::fprintf(stderr, "  the soldier in the way: %s, the driver's kills %d\n", info.alive ? "alive" : "dead",
			driver.kills);
		CHECK(!info.alive && info.deaths == 1);
		CHECK(driver.kills == 1);
	}
}

// The contact effects (0x006b75d0 -> 0x006ba7d0's end): a car falling on
// its roof plays its material cell's sound (index 0) once, and again
// only after the slot's 0.5-1.5 s timer has run out.
TEST(Physics_ContactSound)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	physics::MaterialManager materials;
	for (uint32_t m = 0; m < 256; ++m) { // whatever its vertices' materials are, against the terrain's 0
		materials.createCell(m, 0).soundTemplates = {"S_Crash"};
	}
	const uint32_t car = makeCar(game, {3, 3.0f, 0});
	game.objects.find(car)->setRotation({0, 0, 180}); // landing on its roof, not its springs
	game.start(&materials);
	int sounds = 0;
	for (int i = 0; i < 30; ++i) {
		game.world->tick(1 / 30.0f);
		for (const auto& sound : game.world->takeSounds()) {
			sounds += sound.sound == "S_Crash";
		}
	}
	std::fprintf(stderr, "  %d contact sounds in the first second\n", sounds);
	CHECK(sounds >= 1 && sounds <= 2);
}

// Buoyancy (0x006b9260): a box all under the water is lifted as gravity
// pulls it times floaterMod (1 by default): a car let go 3 m under the
// surface hangs there, and with floaterMod 0 sinks to the bottom. Its node
// knows how deep it is (+0x5c, at most 1).
TEST(Physics_CarFloats)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	float ends[2] = {};
	for (int sinks = 0; sinks < 2; ++sinks) {
		Game game(gameDir);
		const uint32_t car = makeCar(game, {3, 5, 0});
		if (sinks) {
			game.templates.find("car")->settings()["floaterMod"] = "0";
		}
		game.start();
		game.world->setWaterLevel(8);
		game.run(3);
		ends[sinks] = game.objects.find(car)->position().y;
		CHECK(game.world->bodySubmerged(car) == 1);
	}
	std::fprintf(stderr, "  let go at 5 under water at 8: at %.2f; with floaterMod 0 at %.2f\n", ends[0], ends[1]);
	CHECK(ends[0] > 4.5f);
	CHECK(ends[1] < 1.5f);
}

// Ladders (0x0059fd40, 0x0059f6b0, 0x0059e820): a soldier walking at a
// ladder looking up gets on, climbs 1.35 m/s by the throttle and is put
// off over the top.
TEST(Physics_SoldierClimbsALadder)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	game.templates.registerType(12, "Ladder");
	{
		// A ladder 6 m tall: its soldier LOD a quad across x at z 0.
		const physics::CollisionLod lod = boxedLod(2, {{-0.4f, 0, 0}, {0.4f, 0, 0}, {0.4f, 6, 0}, {-0.4f, 6, 0}},
			{{{0, 1, 2}, 0}, {{0, 2, 3}, 0}, {{0, 2, 1}, 0}, {{0, 3, 2}, 0}});
		physics::CollisionSubGeometry part;
		part.lods = {lod, lod, lod};
		physics::CollisionGeometry geometry;
		geometry.subGeometries = {part};
		physics::CollisionMeshFile mesh;
		mesh.version = 10;
		mesh.geometries = {geometry};
		game.collisionTemplates.add("ladder_mesh", mesh);
		world::ObjectTemplate* ladder = game.templates.create("Ladder", "ladder");
		ladder->settings()["collisionMesh"] = "ladder_mesh";
		game.objects.create(ladder, {0, 0, 4}, {0, 0, 0});
	}
	game.start();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	game.run(1);
	game::SoldierInput climb;
	climb.throttle = 1;
	climb.aimPitch = 30; // looking up
	game.world->setPlayerInput(climb);
	bool climbed = false;
	float highest = 0;
	for (int i = 0; i < 30 * 10; ++i) {
		game.world->tick(1 / 30.0f);
		climbed = climbed || game.world->onLadder(game.world->localPlayer());
		highest = std::max(highest, game.world->player()->position().y);
	}
	std::fprintf(stderr, "  on the ladder: %s; highest feet %.2f; off: %s\n", climbed ? "yes" : "no", highest,
		game.world->onLadder(game.world->localPlayer()) ? "no" : "yes");
	CHECK(climbed);
	CHECK(highest > 5.5f);
	CHECK(!game.world->onLadder(game.world->localPlayer()));
}

// The landing's sound (0x005ef250 -> 0x007fdcd0): the footstep set's
// sound, through the player check (player slot 0x1bc, 0x00591510: the
// local player alone); the take-off's (0x007fd970) for everyone.
TEST(Physics_LandSoundForTheLocalPlayerOnly)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	int heardBy[2] = {};
	for (int local = 0; local < 2; ++local) {
		Game game(gameDir);
		physics::MaterialManager materials;
		materials.createCell(5500, 0).soundTemplates = {"S_Step", "S_Step", "S_Step", "S_Step"};
		game.start(&materials);
		int id = -1;
		if (local) {
			CHECK(game.world->spawnPlayer(2) != nullptr);
			id = game.world->localPlayer();
		} else {
			id = game.world->addPlayer("bot", 2, true);
			CHECK(game.world->spawnSoldier(id));
		}
		game::SoldierInput walk;
		walk.throttle = 1;
		game.world->setInput(id, walk);
		game.run(1); // the footstep set: the terrain's
		(void)game.world->takeSounds();
		game::SoldierInput jump;
		jump.jump = true;
		game.world->setInput(id, jump);
		game.world->tick(1 / 30.0f);
		game.world->setInput(id, {});
		int heard = 0;
		for (int i = 0; i < 60; ++i) {
			game.world->tick(1 / 30.0f);
			for (const auto& sound : game.world->takeSounds()) {
				heard += sound.sound == "S_Step";
			}
		}
		std::fprintf(stderr, "  %s: %d footstep-set sounds after the jump\n", local ? "local" : "bot", heard);
		heardBy[local] = heard;
	}
	CHECK(heardBy[1] == heardBy[0] + 1); // the same steps and take-off, and the landing
}

// Diagnostic against the installed soldier BAFs and material cells: ordinary
// movement must send animation sound events through the round's queued sound
// path. Run both archive sets because Remaster replaces the soldier data.
TEST(Physics_InstalledSoldierWalkingQueuesFootsteps)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		Game game(gameDir, mod, true);
		if (const auto* configured = game.materials.cell(5500, 0)) {
			std::fprintf(stderr, "  %s installed material 5500/0:", mod);
			for (const std::string& sound : configured->soundTemplates) {
				std::fprintf(stderr, " %s", sound.c_str());
			}
			std::fprintf(stderr, "\n");
		}
		// Route the installed material cell through a named probe so the test
		// can observe its queued GameAudio dispatch without an audio device.
		game.materials.createCell(5500, 0).soundTemplates = {"ProbeStep", "ProbeStep", "ProbeStep", "ProbeStep"};
		game.start(&game.materials);
		CHECK(game.world->spawnPlayer(2) != nullptr);
		const int local = game.world->localPlayer();
		const anim::AnimationSystemTemplate* animationSystem =
			game.systems.get("Objects/Soldiers/Common/Animations/AnimationSystem3p.inc", [] {});
		for (const char* name : {"stand_walk", "stand_run"}) {
			const int index = animationSystem ? animationSystem->findBundle(name) : -1;
			if (index >= 0) {
				const anim::Bundle& bundle = animationSystem->bundles()[static_cast<size_t>(index)];
				CHECK(bundle.events.empty()); // shipped movement bundles lack footstep event markers
				std::fprintf(stderr, "  %s %s bundle length %.3f events %zu clips %zu\n", mod, name, bundle.length,
					bundle.events.size(), bundle.clips.size());
			}
		}
		sound::SoundManager manager(nullptr, game.templates);
		manager.registerTemplateType();
		auto* probe = dynamic_cast<sound::SoundObjectTemplate*>(game.templates.create(
			sound::SoundObjectTemplate::kClassId, "ProbeStep"));
		CHECK(probe != nullptr);
		if (probe) {
			probe->setSoundFilenames("probe.wav");
		}
		sound::GameSounds gameSounds(manager);
		std::vector<std::string> played;
		gameSounds.onTrigger = [&](const sound::GameSounds::Triggered& event) { played.push_back(event.name); };
		GameAudio audio(*game.world, game.objects, game.templates, manager, gameSounds, nullptr);
		game::SoldierInput walk;
		walk.throttle = 0.2f;
		game.world->setInput(local, walk);
		GameAudio::Camera camera;
		camera.firstPerson = true;
		const auto advance = [&](int ticks) {
			for (int i = 0; i < ticks; ++i) {
				game.world->tick(1 / 30.0f);
				audio.tick(camera);
			}
		};
		advance(120);
		const size_t walkCount = played.size();
		walk.throttle = 1;
		game.world->setInput(local, walk);
		advance(120);
		game::GameWorld::PlayerInfo info;
		CHECK(game.world->playerInfo(local, info));
		const size_t walkSteps = static_cast<size_t>(std::count(played.begin(), played.begin() + walkCount, "ProbeStep"));
		const size_t runSteps = static_cast<size_t>(std::count(played.begin() + walkCount, played.end(), "ProbeStep"));
		std::fprintf(stderr, "  %s: material=%u pos=(%.2f %.2f %.2f), walking=%zu running=%zu GameAudio samples\n",
			mod, game.world->footstepMaterial(local), info.position.x, info.position.y, info.position.z, walkSteps, runSteps);
		CHECK(walkSteps >= 2 && runSteps >= 4);
	}
}

// The original movement selector chooses slots by stance/sprint, then the
// material manager tries the default surface for a missing slot or cell.
// Standing still on a 16 degree slope leaves the body a small residual
// integrated velocity into it (0.13 m/s across, over the 0.1 walking gate),
// but it does not move: the gate reads the measured displacement as retail's
// 0x007fda00 does (slot 0x64), so no footsteps play (seen on a Suez stair).
TEST(Physics_StandingOnASlopeMakesNoFootsteps)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	Game game(gameDir, "mods/Project_Remaster_v17_Final", true);
	game.materials.createCell(5500, 0).soundTemplates = {"WalkDefault", "CrouchDefault", "ProneDefault", "SprintDefault"};
	game.start(&game.materials, 0, 0.287f);
	CHECK(game.world->spawnPlayer(2) != nullptr);
	const int player = game.world->localPlayer();
	game.run(2);
	(void)game.world->takeSounds();
	float drift = 0, measured = 0;
	for (int i = 0; i < 90; ++i) {
		game.run(1 / 30.0f);
		const physics::SoldierBody* body = game.world->soldierBody(player);
		CHECK(body != nullptr);
		if (!body) break;
		drift = std::max(drift, std::sqrt(body->velocity.x * body->velocity.x + body->velocity.z * body->velocity.z));
		const physics::Vec3& m = body->measuredVelocity;
		measured = std::max(measured, std::sqrt(m.x * m.x + m.y * m.y + m.z * m.z));
	}
	CHECK(drift > 0.1f);   // the integrator's pending push into the slope, as retail's +0x20
	CHECK(measured < 0.1f); // but the reported (slot 0x64) velocity, the footsteps' and animation's: no motion
	const auto standing = game.world->takeSounds();
	std::fprintf(stderr, "  standing on the slope: integrated %.3f m/s, measured %.6f m/s\n", drift, measured);
	CHECK(std::none_of(standing.begin(), standing.end(), [](const auto& s) { return s.sound == "WalkDefault"; }));
	// Walking on it still steps.
	game::SoldierInput input;
	input.throttle = 1;
	game.world->setInput(player, input);
	game.run(3);
	const auto walking = game.world->takeSounds();
	CHECK(std::count_if(walking.begin(), walking.end(), [](const auto& s) { return s.sound == "WalkDefault"; }) >= 2);
}

// Running, a hop or a jump keeps the wanted velocity through the air
// (0x006c2230 returns early without clearing it, 0x00588420's air branch
// never sets it): landing carries on at running speed instead of braking
// to a stop for a tick (the judder walking over a Titan's deck seams).
TEST(Physics_LandingWhileRunningDoesNotBrake)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	Game game(gameDir, "mods/Project_Remaster_v17_Final", true);
	game.start();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	const int player = game.world->localPlayer();
	game.run(1);
	game::SoldierInput input;
	input.throttle = 1;
	game.world->setInput(player, input);
	game.run(1.5f);
	input.jump = true;
	game.world->setInput(player, input);
	game.run(1 / 30.0f);
	input.jump = false;
	game.world->setInput(player, input);
	bool left = false, landed = false;
	float slowest = 1e9f;
	int after = 0;
	for (int i = 0; i < 90 && after < 6; ++i) {
		game.run(1 / 30.0f);
		const physics::SoldierBody* body = game.world->soldierBody(player);
		CHECK(body != nullptr);
		if (!body) break;
		left = left || !body->onGround();
		if (left && body->onGround()) landed = true;
		if (landed) {
			slowest = std::min(slowest, std::sqrt(body->velocity.x * body->velocity.x + body->velocity.z * body->velocity.z));
			++after;
		}
	}
	std::fprintf(stderr, "  slowest ground speed in the 6 ticks after landing: %.2f m/s\n", slowest);
	CHECK(left && landed);
	CHECK(slowest > 1.5f);
}

TEST(Physics_FootstepsChooseMovementSlotAndSurfaceFallback)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
	for (uint32_t surface : {0u, 7u, 12345u}) {
		Game game(gameDir, mod, true);
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			auto* soldier = game.templates.find(name);
			soldier->settings()["sprintDissipationTime"] = "20";
			soldier->settings()["sprintRecoverTime"] = "5";
		}
		game.materials.createCell(5500, 0).soundTemplates = {"WalkDefault", "CrouchDefault", "ProneDefault", "SprintDefault"};
		game.materials.createCell(5500, 7).soundTemplates = {"WalkSeven"};
		game.start(&game.materials, surface);
		CHECK(game.world->spawnPlayer(2) != nullptr);
		const int player = game.world->localPlayer();
		game.run(1);
		(void)game.world->takeSounds();
		game::SoldierInput input;
		input.throttle = 1;
		game.world->setInput(player, input);
		game.run(3);
		const auto walking = game.world->takeSounds();
		const std::string walk = surface == 7 ? "WalkSeven" : "WalkDefault";
		CHECK(std::count_if(walking.begin(), walking.end(), [&](const auto& s) { return s.sound == walk; }) >= 2);
		CHECK(std::none_of(walking.begin(), walking.end(), [](const auto& s) { return s.sound == "SprintDefault"; }));
		input.sprint = true;
		game.world->setInput(player, input);
		game.run(3);
		const auto sprint = game.world->takeSounds();
		CHECK(std::count_if(sprint.begin(), sprint.end(), [](const auto& s) { return s.sound == "SprintDefault"; }) >= 2);
		input.sprint = false;
		input.pose = game::SoldierPose::Crouch;
		game.world->setInput(player, input);
		game.run(3);
		const auto crouch = game.world->takeSounds();
		CHECK(std::count_if(crouch.begin(), crouch.end(), [](const auto& s) { return s.sound == "CrouchDefault"; }) >= 2);
		input.pose = game::SoldierPose::Prone;
		game.world->setInput(player, input);
		game.run(4);
		const auto prone = game.world->takeSounds();
		CHECK(std::count_if(prone.begin(), prone.end(), [](const auto& s) { return s.sound == "ProneDefault"; }) >= 2);
	}
	}
}

TEST(Physics_InstalledFootstepMaterialsSelectWalkAndSprint)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (uint32_t surface : {0u, 4u, 7u, 4101u}) {
			Game game(gameDir, mod, true);
			for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
				auto* soldier = game.templates.find(name);
				soldier->settings()["sprintDissipationTime"] = "20";
				soldier->settings()["sprintRecoverTime"] = "5";
			}
			game.start(&game.materials, surface);
			CHECK(game.world->spawnPlayer(2) != nullptr);
			const int player = game.world->localPlayer();
			game.run(1);
			(void)game.world->takeSounds();
			game::SoldierInput input;
			input.throttle = 1;
			game.world->setInput(player, input);
			game.run(3);
			const auto walking = game.world->takeSounds();
			const char* walk = surface == 4 ? "S_Walk_Tarmac" : surface == 4101 ? "S_Walk_Titan" : "S_Walk_Dirt";
			CHECK(std::count_if(walking.begin(), walking.end(), [&](const auto& event) { return event.sound == walk; }) >= 2);
			input.sprint = true;
			game.world->setInput(player, input);
			game.run(3);
			const auto sprinting = game.world->takeSounds();
			const char* sprint = surface == 4 ? "S_Sprint_Tarmac" : surface == 4101 ? "S_Sprint_Titan" : "S_Sprint_Dirt";
			CHECK(std::count_if(sprinting.begin(), sprinting.end(), [&](const auto& event) { return event.sound == sprint; }) >= 2);
			std::fprintf(stderr, "  %s surface %u: walk=%s sprint=%s\n", mod, surface, walk, sprint);
		}
	}
}

// Q's shipped Radio rose maps Request Medic to Radio.callForMedic. That
// command uses the local radio echo, which GameAudio resolves through the
// installed VoiceMessages table and voice samples.
TEST(GameAudio_InstalledQCommoRosePlaysLocalVoice)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir, "mods/bf2142", true);
	game.start(&game.materials);
	CHECK(game.world->spawnPlayer(2) != nullptr);
	const int local = game.world->localPlayer();
	sound::SoundManager manager(nullptr, game.templates);
	manager.registerTemplateType();
	sound::GameSounds gameSounds(manager);
	sound::VoiceMessages voices(manager);
	sound::registerVoiceMessageConsoleMethods(game.console, voices);
	voices.reset("English", "English");
	std::string output;
	CHECK(game.console.run("Sound/VoiceMessages.con", {}, output));
	voices.build();
	const auto validateVoiceSamples = [&](const char* message) {
		const int messageId = voices.message(message), voiceId = voices.voice("grunt");
		const auto samples = voices.samples(messageId, voiceId);
		CHECK(samples.second > 0);
		for (int index = samples.first; index < samples.first + samples.second; ++index) {
			const std::string path = voices.radioFile(2, index);
			std::vector<uint8_t> encoded;
			CHECK(game.files.readFile(path, encoded));
			sound::OggInfo info;
			const bool valid = sound::probeOggVorbis(encoded, path.c_str(), info);
			std::fprintf(stderr, "  installed Q sample %s: valid=%d %uHz channels=%d frames=%u\n", path.c_str(), valid,
				info.sampleRate, info.channels, info.frames);
			CHECK(valid && info.frames > 0);
		}
	};
	validateVoiceSamples("medic");
	validateVoiceSamples("roger_that");
	std::vector<std::pair<std::string, bool>> played;
	voices.onPlay = [&](const std::string& file, bool localVoice) { played.emplace_back(file, localVoice); };
	GameAudio audio(*game.world, game.objects, game.templates, manager, gameSounds, &voices);
	const std::string command = "Radio.callForMedic";
	bindMethod(game.console, command, ConsoleAccess::Anyone, [&, local] {
		game.world->radioMessage(local, 6, "medic");
		game.world->hearRadio(local, 6, "medic", local); // main.cpp's local sender echo
	});
	bindMethod(game.console, "game.radioMessage", ConsoleAccess::Anyone,
		[&, local](std::string channel, std::string voice) {
			const int target = game::GameWorld::targetChannel(channel);
			game.world->radioMessage(local, target, voice);
			game.world->hearRadio(local, target, voice, local);
		});
	hud::HudManager hud([](const std::string&, std::string&) { return false; });
	menu::CommRoses roses(hud);
	roses.bind(game.console, [&](const std::string& line) {
		game.console.execute(line, output, ConsoleAccess::Anyone);
	});
	CHECK(game.console.run("Menu/HUD/HudSetup/Communication/Radio.con", {}, output));
	CHECK(roses.find("Radio") && roses.find("Radio")->command(7) == command);
	roses.openRadio(false);
	CHECK(game.console.execute("RadioInterface.selectOrder 7", output, ConsoleAccess::Anyone) != ConsoleResult::Error);
	GameAudio::Camera camera;
	camera.firstPerson = true;
	audio.tick(camera);
	roses.openRadio(false);
	CHECK(game.console.execute("RadioInterface.selectOrder 8", output, ConsoleAccess::Anyone) != ConsoleResult::Error);
	audio.tick(camera);
	CHECK(audio.voiceOvers == 2 && played.size() == 2 && !played[0].second && !played[1].second);
	for (const auto& sample : played) {
		CHECK(game.files.exists(sample.first));
		std::fprintf(stderr, "  Q Radio sample: %s (exists=%d)\n", sample.first.c_str(), game.files.exists(sample.first));
	}
}

// The third-person muzzle flash follows the gun: the weapon's e_muzz child
// is spawned against the weapon object's anchor at its place on the weapon
// (FireComp 0x005f25f0 places the muzzle bundles by the weapon's matrix),
// the anchor kept on the soldier's skeleton's weapon root (mesh1) while it
// burns.
TEST(RagDoll_MuzzleFlashFollowsTheGun)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	game.templates.registerType(20, "EffectBundle");
	game.templates.registerType(21, "SpriteParticleSystem");
	world::ObjectTemplate* glow = game.templates.create("SpriteParticleSystem", "test_glow3p");
	glow->settings()["emitFrequency"] = "60";
	glow->settings()["emitTime"] = "0.5";
	glow->settings()["timeToLive"] = "0.1";
	glow->settings()["isLocalSystem"] = "1";
	glow->settings()["textureName"] = "objects\\effects\\textures\\spark";
	world::ObjectTemplate* muzzle = game.templates.create("EffectBundle", "e_muzz_test");
	muzzle->children().push_back({"test_glow3p", {}, {}});
	game.templates.find("eu_ar_rifle")->children().push_back({"e_muzz_test", {0, 0.05f, 0.6f}, {}});
	game.start();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	const int me = game.world->localPlayer();
	game.world->localMuzzleEffects = true;
	game.run(0.5f);
	game::GameWorld::PlayerInfo info;
	game.world->playerInfo(me, info);
	game::SoldierInput input;
	input.fire = true;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	game.world->setPlayerInput({});
	game.world->tick(1 / 30.0f);
	const auto flash = [&]() -> const effects::Emitter* {
		for (const auto& emitter : game.world->effects().emitterList()) {
			if (emitter->anchor == info.weapon && emitter->anchor != 0) {
				return emitter.get();
			}
		}
		return nullptr;
	};
	CHECK(flash() != nullptr);
	if (!flash()) {
		std::fprintf(stderr, "  weapon %u, %zu emitters, %zu projectiles\n", info.weapon, game.world->effects().emitterList().size(), game.world->projectiles().size());
		for (const auto& e : game.world->effects().emitterList()) {
			std::fprintf(stderr, "   anchor %u at %.1f/%.1f/%.1f\n", e->anchor, e->position().x, e->position().y, e->position().z);
		}
		return;
	}
	const physics::Vec3 first = flash()->position();
	std::fprintf(stderr, "  flash at %.2f/%.2f/%.2f, the soldier at %.2f/%.2f/%.2f\n", first.x, first.y, first.z,
		info.position.x, info.position.y, info.position.z);
	CHECK(std::fabs(first.y - info.position.y - 1.3f) < 0.6f);
	CHECK(physics::lengthSquared(first - info.position) < 2.5f * 2.5f);
	// The soldier moved 3 m: the flash goes with the gun.
	CHECK(game.world->setSoldierPosition(info.soldier, info.position + physics::Vec3 {3, 0, 0}));
	game.world->tick(1 / 30.0f);
	CHECK(flash() != nullptr);
	if (flash()) {
		const physics::Vec3 moved = flash()->position() - first;
		std::fprintf(stderr, "  moved by %.2f/%.2f/%.2f\n", moved.x, moved.y, moved.z);
		CHECK(std::fabs(moved.x - 3) < 0.3f && std::fabs(moved.z) < 0.3f);
	}
}

// A shot starts every child bundle of the weapon with startOnEffects 0,
// whatever its name or place in the list (game::fireBundles: FireComp's
// list 0x005f25f0, event 0 at 0x0061a5e2), and not one that a first-person
// event starts (512): eu_av lists its reload puff e_muzz_eu_av_reload
// before its launch flash e_muzz_eu_av, and the flash never showed.
TEST(RagDoll_ShotStartsItsStartOnEffectsZeroBundles)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	game.templates.registerType(20, "EffectBundle");
	game.templates.registerType(21, "SpriteParticleSystem");
	for (const char* name : {"test_reload_puff", "test_shot_flash"}) {
		world::ObjectTemplate* system = game.templates.create("SpriteParticleSystem", name);
		system->settings()["emitFrequency"] = "60";
		system->settings()["emitTime"] = "0.5";
		system->settings()["timeToLive"] = "0.1";
		system->settings()["isLocalSystem"] = "1";
		system->settings()["textureName"] = std::string("objects\\effects\\textures\\") + name;
	}
	world::ObjectTemplate* reload = game.templates.create("EffectBundle", "e_muzz_test_reload");
	reload->settings()["setStartOnEffects"] = "512";
	reload->children().push_back({"test_reload_puff", {}, {}});
	game.templates.create("EffectBundle", "e_test_shot")->children().push_back({"test_shot_flash", {}, {}});
	auto& children = game.templates.find("eu_ar_rifle")->children();
	children.insert(children.begin(), {"e_muzz_test_reload", {0, 0.05f, 0.3f}, {}});
	children.push_back({"e_test_shot", {0, 0.05f, 0.6f}, {}});
	game.start();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	const int me = game.world->localPlayer();
	game.world->localMuzzleEffects = true;
	game.run(0.5f);
	game::GameWorld::PlayerInfo info;
	game.world->playerInfo(me, info);
	game::SoldierInput input;
	input.fire = true;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	game.world->setPlayerInput({});
	game.world->tick(1 / 30.0f);
	const auto started = [&](const char* system) {
		for (const auto& emitter : game.world->effects().emitterList()) {
			if (emitter->anchor == info.weapon && emitter->anchor != 0
				&& emitter->particleTemplate().texture.find(system) != std::string::npos) {
				return true;
			}
		}
		return false;
	};
	CHECK(started("test_shot_flash"));
	CHECK(!started("test_reload_puff"));
}

// Seat cameras on the mouse (0x00563bd0 on a Camera bundle with inputToYaw
// PIMouseLookX / inputToPitch PIMouseLookY): the driver's camera
// (eu_fav_driver_camera's settings: -125..120 across at 90 a second) turns
// and stays; a gunner's (eu_fav_MachineGun_Camera: +-1 / +-0.5,
// automaticReset) leans with the mouse and settles back. partPose gives
// the camera node's turn, which PlayerCamera applies.
TEST(RagDoll_SeatCamerasTurnOnTheMouse)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	game.collisionTemplates.create("as_fav", "Objects/Vehicles/as_fav/");
	game.templates.registerType(9, "Engine");
	game.templates.registerType(10, "Spring");
	game.templates.registerType(11, "Camera");
	world::ObjectTemplate* car = game.templates.create("PlayerControlObject", "car");
	car->settings()["hasMobilePhysics"] = "1";
	car->settings()["physicsType"] = "Mesh";
	car->settings()["collisionMesh"] = "as_fav";
	car->settings()["seatInformation"] = "car 0/0.5/0.4 0/0/0";
	car->settings()["mass"] = "1800";
	car->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
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
	world::ObjectTemplate* driverCamera = game.templates.create("Camera", "car_driver_camera");
	driverCamera->settings()["minRotation"] = "-125/-20/0";
	driverCamera->settings()["maxRotation"] = "120/40/0";
	driverCamera->settings()["maxSpeed"] = "90/90/0";
	driverCamera->settings()["acceleration"] = "5000/5000/0";
	driverCamera->settings()["inputToYaw"] = "PIMouseLookX";
	driverCamera->settings()["inputToPitch"] = "PIMouseLookY";
	car->children().push_back({"car_driver_camera", {0, 1.2f, 0.4f}, {}});
	// The gunner's seat with the leaning camera.
	world::ObjectTemplate* gunner = game.templates.create("PlayerControlObject", "car_gunner");
	gunner->settings()["seatInformation"] = "car_gunner 0/1/-1 0/0/0";
	world::ObjectTemplate* gunnerCamera = game.templates.create("Camera", "car_gunner_camera");
	gunnerCamera->settings()["minRotation"] = "-1/-0.5/0";
	gunnerCamera->settings()["maxRotation"] = "1/0.5/0";
	gunnerCamera->settings()["pivotPosition"] = "0/0/-2";
	gunnerCamera->settings()["maxSpeed"] = "5/5/0";
	gunnerCamera->settings()["acceleration"] = "1/0.5/0";
	gunnerCamera->settings()["inputToYaw"] = "PIMouseLookX";
	gunnerCamera->settings()["inputToPitch"] = "PIMouseLookY";
	gunnerCamera->settings()["automaticReset"] = "1";
	gunner->children().push_back({"car_gunner_camera", {0, 1, 0}, {}});
	car->children().push_back({"car_gunner", {0, 0.5f, -1}, {}});
	game.templates.create("EntryPoint", "car_entry")->settings()["entryRadius"] = "3";
	game.start();
	const uint32_t vehicle = game.objects.create(car, {3, 1.2f, 0}, {0, 0, 0})->id();
	CHECK(game.world->spawnPlayer(2) != nullptr);
	game.run(2);
	const std::vector<game::GameWorld::SeatLayout> seats = game.world->vehicleSeats(vehicle);
	CHECK(seats.size() == 2 && seats[0].camera == "car_driver_camera" && seats[1].camera == "car_gunner_camera");
	game::SoldierInput input;
	input.use = true;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	// The driver looks right: 10 degrees a tick is PIMouseLookX 2, the
	// axis' 1 at most: 90 degrees a second (maxSpeed).
	input = {};
	input.turn = 10;
	for (int i = 0; i < 15; ++i) {
		game.world->setPlayerInput(input);
		game.world->tick(1 / 30.0f);
	}
	world::Vec3 offset, rotation;
	CHECK(game.world->partPose(vehicle, "car_driver_camera", offset, rotation));
	std::fprintf(stderr, "  the driver's camera after half a second: %.1f across, %.1f down\n", rotation.x, rotation.y);
	CHECK(rotation.x > 30 && rotation.x <= 46);
	// It stays where it was turned.
	game.world->setPlayerInput({});
	game.world->tick(1 / 30.0f);
	CHECK(game.world->partPose(vehicle, "car_driver_camera", offset, rotation));
	game.run(1);
	world::Vec3 still;
	CHECK(game.world->partPose(vehicle, "car_driver_camera", offset, still) && std::fabs(still.x - rotation.x) < 0.01f);
	// The gunner's seat: a nudge leans the camera by at most its limit,
	// then it settles back.
	input = {};
	input.switchSeat = 2;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	input = {};
	input.turn = 10;
	for (int i = 0; i < 30; ++i) {
		game.world->setPlayerInput(input);
		game.world->tick(1 / 30.0f);
	}
	CHECK(game.world->partPose(vehicle, "car_gunner_camera", offset, rotation));
	std::fprintf(stderr, "  the gunner's camera leaning: %.2f across (offset %.2f/%.2f/%.2f)\n", rotation.x, offset.x, offset.y,
		offset.z);
	CHECK(rotation.x > 0.5f && rotation.x <= 1.0f);
	game.world->setPlayerInput({});
	game.run(3);
	CHECK(game.world->partPose(vehicle, "car_gunner_camera", offset, rotation) && std::fabs(rotation.x) < 0.05f);
}

// The hands' IK (Animation/Ik.h, 0x008b6760): a three-part arm reaches a
// target it can reach, its parts keep their lengths and the hand takes the
// target whole.
TEST(Ik_ArmReachesTheTarget)
{
	anim::Skeleton skeleton;
	const auto node = [&skeleton](const char* name, int parent, float z) {
		skeleton.nodes.push_back({name, parent, {0, 0, 0, 1}, {0, 0, z}});
	};
	node("root", -1, 0);
	node("shoulder", 0, 0);
	node("elbow", 1, 0.3f);
	node("low_arm", 2, 0.06f);
	node("joint", 3, 0.11f);
	node("wrist", 4, 0.1f);
	// The elbow a little bent, as the seats' poses have it (a straight arm
	// gives the solve no plane to bend in: v1 x v2 is zero).
	skeleton.nodes[2].rotation = {0.2588f, 0, 0, 0.9659f};
	const anim::LocalPose pose = anim::setupPose(skeleton);
	anim::IkTarget target;
	target.bone = 4;
	target.end = 5;
	target.target = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0.25f, 0.1f, 0.3f, 1};
	target.pole = {0, 1, 0};
	const std::vector<anim::Matrix> world = anim::worldMatricesWithIk(skeleton, pose, {target});
	const auto at = [&world](int i) { return physics::Vec3 {world[i][12], world[i][13], world[i][14]}; };
	const auto distance = [](const physics::Vec3& a, const physics::Vec3& b) { return std::sqrt(physics::lengthSquared(a - b)); };
	CHECK(world[5] == target.target);
	CHECK(std::abs(distance(at(1), at(2)) - 0.3f) < 1e-4f);
	CHECK(std::abs(distance(at(2), at(3)) - 0.06f) < 1e-4f);
	CHECK(std::abs(distance(at(3), at(4)) - 0.11f) < 1e-4f);
	// The wrist's own place from the lower arm lands near the target (the
	// solve aims the wrist through the handle offset by the elbow part).
	const physics::Vec3 wrist = at(4) + physics::Vec3 {world[4][8], world[4][9], world[4][10]} * 0.1f;
	std::fprintf(stderr, "  ik: wrist %.3f from the target, elbow at %.3f/%.3f/%.3f\n", distance(wrist, at(5)), at(2).x,
		at(2).y, at(2).z);
	CHECK(distance(wrist, at(5)) < 0.05f);
	// The pole bends the elbow its way (up).
	CHECK(at(2).y > 0.05f);
	// Out of reach: straight towards it.
	target.target[12] = 2;
	target.target[13] = 0;
	target.target[14] = 0;
	const std::vector<anim::Matrix> far = anim::worldMatricesWithIk(skeleton, pose, {target});
	const physics::Vec3 elbow {far[2][12], far[2][13], far[2][14]};
	CHECK(elbow.x > 0.29f && std::abs(elbow.y) < 0.01f && std::abs(elbow.z) < 0.01f);
}

// A seated driver's hands on the steering wheel: the seat's tree holds a
// bundle with the Ik component (as_fav's AS_FAV_SteeringWheel, its ik.*),
// so the 3P skeleton's wrists (left_wrist1, right_ullna) sit on its grips
// (0x005ee910, 0x005efc00).
TEST(Ik_DriverHoldsTheWheel)
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
	car->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
	game.templates.registerType(9, "Engine");
	game.templates.registerType(10, "Spring");
	game.templates.registerType(12, "RotationalBundle");
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
	world::ObjectTemplate* steering = game.templates.create("RotationalBundle", "car_steering");
	game.templates.addComponent(*steering, "Ik");
	steering->settings()["ik.positionLeft"] = "-0.145199/-0.0447125/-0.186665";
	steering->settings()["ik.rotationLeft"] = "-94.775/-2.18003/-58.2225";
	steering->settings()["ik.poleVectorLeft"] = "-0.294253/-0.339534/-3.01e-6";
	steering->settings()["ik.positionRight"] = "0.17838/-0.0352049/-0.155193";
	steering->settings()["ik.rotationRight"] = "-114.531/21.6388/120.12";
	steering->settings()["ik.poleVectorRight"] = "0.148879/-0.231249/0.143271";
	car->children().push_back({"car_steering", {0, 0.85f, 0.85f}, {0, 20, 0}});
	game.templates.create("EntryPoint", "car_entry")->settings()["entryRadius"] = "3";
	game.start();
	const uint32_t vehicle = game.objects.create(car, {3, 1.2f, 0}, {30, 0, 0})->id();
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
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	std::vector<anim::Matrix> seated;
	CHECK(skeleton && game.world->skeletonPose(soldier, *skeleton, seated));
	const world::Object* object = game.objects.find(soldier);
	const world::Object* carObject = game.objects.find(vehicle);
	if (!skeleton || !object || !carObject || seated.size() != skeleton->nodes.size()) {
		return;
	}
	// The grips in the world: the wheel's frame (its place in the car, the
	// car's turn), the ik positions in it.
	const physics::Mat3 carTurn = physics::Mat3::fromYawPitchRoll({carObject->rotation().x, carObject->rotation().y, carObject->rotation().z});
	const physics::Mat3 wheelTurn = physics::Mat3::fromYawPitchRoll({0, 20, 0}) * carTurn;
	const physics::Vec3 wheelAt = carTurn.transform({0, 0.85f, 0.85f})
		+ physics::Vec3 {carObject->position().x, carObject->position().y, carObject->position().z};
	const physics::Mat3 soldierTurn = physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
	const physics::Vec3 soldierAt {object->position().x, object->position().y, object->position().z};
	const auto grip = [&](const physics::Vec3& local) {
		return soldierTurn.untransform(wheelTurn.transform(local) + wheelAt - soldierAt);
	};
	const physics::Vec3 left = grip({-0.145199f, -0.0447125f, -0.186665f});
	const physics::Vec3 right = grip({0.17838f, -0.0352049f, -0.155193f});
	const int leftWrist = skeleton->find("left_wrist1"), ulna = skeleton->find("right_ullna");
	CHECK(leftWrist >= 0 && ulna >= 0);
	if (leftWrist < 0 || ulna < 0) {
		return;
	}
	const auto at = [&seated](int bone) {
		const anim::Matrix& m = seated[static_cast<size_t>(bone)];
		return physics::Vec3 {m[12], m[13], m[14]};
	};
	const auto distance = [](const physics::Vec3& a, const physics::Vec3& b) { return std::sqrt(physics::lengthSquared(a - b)); };
	std::fprintf(stderr, "  ik: left wrist %.3f from its grip, right %.3f; shoulder to grip %.2f\n",
		distance(at(leftWrist), left), distance(at(ulna), right), distance(at(skeleton->find("left_shoulder")), left));
	CHECK(distance(at(leftWrist), left) < 1e-3f);
	CHECK(distance(at(ulna), right) < 1e-3f);
	// The elbow stays at the upper arm's length from the shoulder.
	const int shoulder = skeleton->find("left_shoulder"), elbow = skeleton->find("left_elbow");
	CHECK(std::abs(distance(at(shoulder), at(elbow)) - std::sqrt(physics::lengthSquared({skeleton->nodes[elbow].position[0],
		skeleton->nodes[elbow].position[1], skeleton->nodes[elbow].position[2]}))) < 1e-3f);
}

// The combat areas (CombatAreaManager / CombatArea, 0x004bbf60): outside
// the areas that apply to the player the time outside runs; past the
// manager's timeAllowedOutside the soldier takes `damage` a tick (50: dead
// on the second). The commander warns the local player as it starts.
TEST(CombatArea_OutsideTimerThenDamage)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	game::CombatAreas areas;
	game::registerCombatAreaConsoleMethods(game.console, areas);
	std::string out;
	for (const char* line : {"CombatAreaManager.use 1", "CombatAreaManager.timeAllowedOutside 2",
			 "CombatAreaManager.damage 50", "CombatAreaManager.addVehicleTimer VCLand 3", "CombatArea.create far",
			 "CombatArea.addAreaPoint 100/100", "CombatArea.addAreaPoint 100/100", "CombatArea.addAreaPoint 200/100",
			 "CombatArea.create other", "CombatArea.create far", "CombatArea.addAreaPoint 200/200",
			 "CombatArea.addAreaPoint 100/200"}) {
		game.console.execute(line, out);
	}
	CHECK(areas.areas.size() == 2);
	CHECK(areas.areas[0].points.size() == 4 && areas.areas[0].polygon); // the repeat dropped, re-selected by name
	CHECK(areas.areas[1].vehicles == 4 && areas.areas[1].layer == 1 && !areas.areas[1].polygon);
	CHECK(areas.timerFor(0, false) == 3 && areas.timerFor(2, false) == 15 && areas.timerFor(0, true) == 2);
	game.console.execute("CombatArea.create other", out);
	game.console.execute("CombatArea.deleteActiveCombatArea", out);
	CHECK(areas.areas.size() == 1);
	game.start();
	game.world->setCombatAreas(&areas);
	CHECK(game.world->spawnPlayer(2) != nullptr); // at the origin: outside "far"
	const int me = game.world->localPlayer();
	game.run(1);
	CHECK(game.world->outsideCombatArea(me) > 0.9f);
	CHECK(std::abs(game.world->combatAreaTimeLeft(me) - 1) < 0.1f);
	// The HUD's countdown (0x0044c7b0): the round's while the game runs.
	game::GameLogic logic;
	host::LocalHudWorld hudWorld(*game.world, logic);
	CHECK(std::abs(hudWorld.combatAreaTimeLeft() - 1) < 0.1f);
	logic.endGame(1, 3);
	CHECK(hudWorld.combatAreaTimeLeft() < 0);
	const auto heard = game.world->takeRadioHeard();
	CHECK(std::count_if(heard.begin(), heard.end(), [](const auto& h) { return h.voice == "leaving_the_combat_area"; }) == 1);
	game::GameWorld::PlayerInfo info;
	game.run(1.0f);
	CHECK(game.world->playerInfo(me, info) && info.alive); // 2 s and a tick at most
	game.run(0.1f);
	CHECK(game.world->playerInfo(me, info) && !info.alive);
	CHECK(game.world->outsideCombatArea(me) < 0);
	// Inside, nothing runs.
	areas.areas[0].points = {{-50, -50}, {50, -50}, {50, 50}, {-50, 50}};
	game.run(20);
	CHECK(game.world->playerInfo(me, info) && (!info.alive || game.world->outsideCombatArea(me) < 0));
}

// A neutral APC (no teamOnVehicle) carrying a SpawnPoint: entered by a
// team-2 soldier it is team 2's (0x005c9500: the first occupant's team),
// so it is one of team 2's mobile spawn points; left empty it is its own
// (neutral) again.
TEST(Vehicle_TeamOfItsOccupant)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	game.collisionTemplates.create("as_fav", "Objects/Vehicles/as_fav/");
	world::ObjectTemplate* car = game.templates.create("PlayerControlObject", "apc");
	car->settings()["hasMobilePhysics"] = "1";
	car->settings()["physicsType"] = "Mesh";
	car->settings()["collisionMesh"] = "as_fav";
	car->settings()["seatInformation"] = "apc 0/0.5/0.4 0/0/0";
	car->settings()["mass"] = "1800";
	car->children().push_back({"apc_entry", {-1.5f, 0, 0}, {}});
	game.templates.registerType(9, "Engine");
	game.templates.registerType(10, "Spring");
	world::ObjectTemplate* engine = game.templates.create("Engine", "apc_engine");
	engine->settings()["engineType"] = "194";
	world::ObjectTemplate* wheel = game.templates.create("Spring", "apc_wheel");
	wheel->settings()["strength"] = "40";
	wheel->settings()["damping"] = "8";
	car->children().push_back({"apc_engine", {0, -0.44f, 0}, {}});
	for (float x : {-0.95f, 0.95f}) {
		for (float z : {-1.4f, 1.6f}) {
			engine->children().push_back({"apc_wheel", {x, -0.15f, z}, {}});
		}
	}
	game.templates.create("SpawnPoint", "apc_SpawnPoint");
	car->children().push_back({"apc_SpawnPoint", {0, 0, -3}, {}});
	game.templates.create("EntryPoint", "apc_entry")->settings()["entryRadius"] = "3";
	game.start();
	world::Object* apc = game.objects.create(car, {3, 1.2f, 0}, {0, 0, 0});
	const uint32_t vehicle = apc->id();
	CHECK(apc->team() == 0);
	CHECK(game.world->spawnPlayer(2) != nullptr);
	game.run(2);
	CHECK(game.world->spawnVehicles(2).empty());
	game::SoldierInput input;
	input.use = true;
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	game.world->setPlayerInput({});
	game.run(0.2f);
	CHECK(game.world->playerVehicle() && game.world->playerVehicle()->id() == vehicle);
	const auto mine = game.world->spawnVehicles(2);
	CHECK(mine.size() == 1 && mine[0] == vehicle);
	CHECK(game.world->spawnVehicles(1).empty());
	// Out: empty, its own team (none) again.
	game.world->setPlayerInput(input);
	game.world->tick(1 / 30.0f);
	game.world->setPlayerInput({});
	game.run(0.2f);
	CHECK(game.world->playerVehicle() == nullptr);
	CHECK(game.objects.find(vehicle)->team() == 0 && game.world->spawnVehicles(2).empty());
}

// A joined game's soldier ghost seated (EnterVehicle) or on a ladder (its
// networkable's 0x20000): posed by the seat's system with its own look
// (0x00587a30: Ry(+0x25c) x the seat), hidden in a seat without one; on a
// ladder by the container's climb at the networked height (0x0059ec80,
// 0x0059f380), its weapon put away. Back on foot, the body's system again.
TEST(RagDoll_RemoteSoldierSeatedAndOnLadder)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	std::string out;
	game.console.run("ClientArchives.con", {}, out); // the ladder's clips
	// A 5 m ladder panel facing +z at 0/0/40.
	physics::CollisionSubGeometry part;
	for (uint32_t type = 0; type < 3; ++type) {
		physics::CollisionLod lod;
		lod.collisionType = type;
		lod.vertices = {{-0.3f, 0, -0.05f}, {0.3f, 0, -0.05f}, {0.3f, 5, -0.05f}, {-0.3f, 5, -0.05f}};
		lod.faces = {{{0, 1, 2}, 0}, {{0, 2, 3}, 0}};
		lod.boundsMin = {-0.3f, 0, -0.05f};
		lod.boundsMax = {0.3f, 5, -0.05f};
		part.lods.push_back(lod);
	}
	physics::CollisionMeshFile panel;
	panel.version = 10;
	panel.geometries.resize(1);
	panel.geometries[0].subGeometries = {part};
	game.collisionTemplates.add("ladder_mesh", panel);
	game.templates.registerType(40, "Ladder");
	world::ObjectTemplate* ladder = game.templates.create("Ladder", "ladder_5m");
	ladder->settings()["collisionMesh"] = "ladder_mesh";
	game.objects.create(ladder, {0, 0, 40});
	// A car: the driver's seat with the FAV's seat system, a back seat without one.
	world::ObjectTemplate* car = game.templates.create("PlayerControlObject", "car");
	car->settings()["seatInformation"] = "car 0/0.5/0.4 0/0/0";
	car->settings()["seatAnimationSystem"] = "Objects/Vehicles/AS_FAV/3p_as_fav_driver_animSys.inc";
	world::ObjectTemplate* back = game.templates.create("PlayerControlObject", "car_back");
	back->settings()["seatInformation"] = "car_back 0/0.5/-1 0/0/0";
	car->children().push_back({"car_back", {0, 0, -1}, {}});
	game.start();
	const uint32_t vehicle = game.objects.create(car, {3, 1, 0}, {30, 0, 0})->id();
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	CHECK(skeleton != nullptr);
	if (!skeleton) {
		return;
	}
	world::Object* ghost = game.objects.create(game.templates.find("us_light_soldier"), {3, 1, 0});
	const uint32_t id = ghost->id();
	const auto height = [](const std::vector<anim::Matrix>& pose, int bone) { return pose[static_cast<size_t>(bone)][13]; };
	std::vector<anim::Matrix> standing, seated, turned, climbing, gettingOff;
	game::GameWorld::RemoteSoldierState foot;
	foot.weapon = "eu_ar_rifle";
	for (int i = 0; i < 10; ++i) {
		game.world->animateRemoteSoldier(id, foot, 1 / 30.0f);
	}
	CHECK(game.world->skeletonPose(id, *skeleton, standing) && standing.size() > 9);
	CHECK(game.world->remoteSoldierWeapon(id) != 0);

	// Seated, looking ahead: the seat's pose, turned with the car and seat.
	for (int i = 0; i < 10; ++i) {
		game.world->animateRemoteSeated(id, vehicle, 0, 0, 1 / 30.0f);
	}
	CHECK(game.world->skeletonPose(id, *skeleton, seated) && seated.size() > 9);
	CHECK(game.world->remoteSoldierWeapon(id) == 0); // put away
	CHECK(!ghost->hidden());
	CHECK(std::abs(std::remainder(ghost->rotation().x - 30, 360.0f)) < 0.5f);
	if (seated.size() > 9 && standing.size() > 9) {
		std::fprintf(stderr, "  remote seated: hips %.2f knees %.2f (standing %.2f %.2f)\n", height(seated, 1),
			height(seated, 2), height(standing, 1), height(standing, 2));
		CHECK(height(standing, 1) - height(standing, 2) > 0.2f);
		CHECK(height(seated, 1) - height(seated, 2) < 0.2f);
	}
	// The body turned 60 from the seat: the soldier with it.
	for (int i = 0; i < 10; ++i) {
		game.world->animateRemoteSeated(id, vehicle, 0, 60, 1 / 30.0f);
	}
	CHECK(std::abs(std::remainder(ghost->rotation().x - 90, 360.0f)) < 0.5f);
	CHECK(game.world->skeletonPose(id, *skeleton, turned));
	// The back seat has no seat system: not shown.
	game.world->animateRemoteSeated(id, vehicle, 1, 0, 1 / 30.0f);
	CHECK(ghost->hidden());
	// Out: shown again, the rifle back in its hands.
	game.world->animateRemoteSoldier(id, foot, 1 / 30.0f);
	CHECK(!ghost->hidden());
	CHECK(game.world->remoteSoldierWeapon(id) != 0);

	// On the ladder (id 1) low down, then near the top: the climb's and the
	// get-off's poses, not the standing one; the rifle put away.
	ghost->setPosition({0, 1, 39.4f});
	game::GameWorld::RemoteSoldierState onLadder = foot;
	onLadder.ladder = 1;
	onLadder.ladderPosition = 0.3f;
	for (int i = 0; i < 10; ++i) {
		game.world->animateRemoteSoldier(id, onLadder, 1 / 30.0f);
	}
	CHECK(game.world->skeletonPose(id, *skeleton, climbing) && climbing.size() == standing.size());
	const world::Object* rifle = game.objects.find(game.world->remoteSoldierWeapon(id));
	CHECK(rifle && rifle->hidden());
	onLadder.ladderPosition = 0.95f;
	for (int i = 0; i < 10; ++i) {
		game.world->animateRemoteSoldier(id, onLadder, 1 / 30.0f);
	}
	CHECK(game.world->skeletonPose(id, *skeleton, gettingOff) && gettingOff.size() == standing.size());
	float fromStanding = 0, fromClimb = 0;
	for (size_t i = 0; i < standing.size() && i < climbing.size() && i < gettingOff.size(); ++i) {
		if (skeleton->nodes[i].name.compare(0, 4, "mesh") == 0) {
			continue; // the weapon's nodes (the ladder clips leave them unset; nothing is held)
		}
		for (size_t k = 12; k < 15; ++k) {
			fromStanding = std::max(fromStanding, std::abs(climbing[i][k] - standing[i][k]));
			fromClimb = std::max(fromClimb, std::abs(gettingOff[i][k] - climbing[i][k]));
		}
	}
	std::fprintf(stderr, "  remote ladder: the climb %.2f m from standing, the get-off %.2f m from the climb\n",
		fromStanding, fromClimb);
	CHECK(fromStanding > 0.05f);
	CHECK(fromClimb > 0.05f);
	// Off the ladder (0): standing again, the rifle shown.
	game.world->animateRemoteSoldier(id, foot, 1 / 30.0f);
	rifle = game.objects.find(game.world->remoteSoldierWeapon(id));
	CHECK(rifle && !rifle->hidden());
}

// A joined game's soldier ghost (no player of the client's round) is
// posed by its animation and holds its weapon, as the round's own
// soldiers are; it goes with its ghost.
TEST(RagDoll_RemoteSoldierAnimated)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	game.start();
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	CHECK(skeleton != nullptr);
	if (!skeleton) {
		return;
	}
	world::Object* ghost = game.objects.create(game.templates.find("us_light_soldier"), {5, 0, 5});
	const uint32_t id = ghost->id();
	std::vector<anim::Matrix> pose;
	CHECK(!game.world->skeletonPose(id, *skeleton, pose)); // no player, no pose
	game::GameWorld::RemoteSoldierState state;
	state.weapon = "eu_ar_rifle";
	state.velocity = {0, 0, 3}; // running forward
	for (int i = 0; i < 30; ++i) {
		game.world->animateRemoteSoldier(id, state, 1 / 30.0f);
	}
	CHECK(game.world->isRemoteSoldier(id));
	CHECK(game.world->skeletonPose(id, *skeleton, pose) && pose.size() == skeleton->nodes.size());
	// Not the setup pose: the run moved the bones.
	const std::vector<anim::Matrix> setup = anim::worldMatrices(*skeleton, anim::setupPose(*skeleton));
	float moved = 0;
	for (size_t i = 0; i < pose.size() && i < setup.size(); ++i) {
		for (int k = 12; k < 15; ++k) {
			moved = std::max(moved, std::abs(pose[i][static_cast<size_t>(k)] - setup[i][static_cast<size_t>(k)]));
		}
	}
	CHECK(moved > 0.01f);
	// Its rifle: an object at the soldier, its parts on the mesh nodes.
	const uint32_t rifle = game.world->remoteSoldierWeapon(id);
	const world::Object* held = game.objects.find(rifle);
	CHECK(held && held->objectTemplate() && held->objectTemplate()->name() == "eu_ar_rifle");
	std::vector<anim::Matrix> parts(2);
	CHECK(game.world->partTransforms(rifle, parts));
	// Crouched: another pose than the standing run's.
	std::vector<anim::Matrix> standing = pose;
	state.velocity = {};
	state.pose = game::SoldierPose::Crouch;
	for (int i = 0; i < 30; ++i) {
		game.world->animateRemoteSoldier(id, state, 1 / 30.0f);
	}
	CHECK(game.world->skeletonPose(id, *skeleton, pose));
	float crouched = 0;
	for (size_t i = 0; i < pose.size() && i < standing.size(); ++i) {
		crouched = std::max(crouched, std::abs(pose[i][13] - standing[i][13]));
	}
	CHECK(crouched > 0.05f);
	// Its fire state on (player +0x19c): the rifle fires at its rate, a Fire
	// sound each round; the magazine empty, the Reload; the server's rounds
	// while it fires set the magazine.
	const game::FireArmSettings arm = game::FireArmSettings::fromTemplate(*game.templates.find("eu_ar_rifle"));
	game.world->takeWeaponSounds();
	state.remoteFire = true;
	state.firing = true;
	state.firePresses = 1;
	state.rounds = 3;
	state.fireMode = game::kFireAutomatic;
	for (int i = 0; i < 30; ++i) {
		game.world->animateRemoteSoldier(id, state, 1 / 30.0f);
		state.rounds = -1; // the server said it once
	}
	int fires = 0, reloads = 0;
	for (const auto& sound : game.world->takeWeaponSounds()) {
		fires += sound.kind == game::GameWorld::WeaponSoundEvent::Fire && sound.weapon == "eu_ar_rifle";
		reloads += sound.kind == game::GameWorld::WeaponSoundEvent::Reload;
	}
	// The rifle has no ammo.autoReload: the magazine stays empty until the
	// server's reload (the snapshot's mask 0x10000, reloadRemoteSoldier).
	CHECK(fires == 3 && reloads == 0);
	game.world->reloadRemoteSoldier(id, 0);
	for (const auto& sound : game.world->takeWeaponSounds()) {
		reloads += sound.kind == game::GameWorld::WeaponSoundEvent::Reload;
	}
	CHECK(reloads == 1);
	// Reloaded after reloadTime: it fires on at roundsPerMinute.
	state.firing = false;
	for (int i = 0; i < static_cast<int>(arm.reloadTime * 30) + 2; ++i) {
		game.world->animateRemoteSoldier(id, state, 1 / 30.0f);
	}
	game.world->takeWeaponSounds();
	state.firing = true;
	for (int i = 0; i < 30; ++i) {
		game.world->animateRemoteSoldier(id, state, 1 / 30.0f);
	}
	fires = 0;
	for (const auto& sound : game.world->takeWeaponSounds()) {
		fires += sound.kind == game::GameWorld::WeaponSoundEvent::Fire;
	}
	const int expected = std::min(arm.magSize, arm.roundsPerMinute / 60);
	CHECK(std::abs(fires - expected) <= 1);
	// Single fire: one round a press (the presses' count).
	state.firing = false;
	state.fireMode = game::kFireSingle;
	for (int i = 0; i < 10; ++i) {
		game.world->animateRemoteSoldier(id, state, 1 / 30.0f);
	}
	game.world->takeWeaponSounds();
	state.firePresses = 2; // a tap between two updates
	for (int i = 0; i < 10; ++i) {
		game.world->animateRemoteSoldier(id, state, 1 / 30.0f);
	}
	fires = 0;
	for (const auto& sound : game.world->takeWeaponSounds()) {
		fires += sound.kind == game::GameWorld::WeaponSoundEvent::Fire;
	}
	CHECK(fires == 1);
	state.remoteFire = false;
	state.pose = game::SoldierPose::Stand;
	// Another weapon in hand replaces it; the ghost gone, both go.
	state.weapon = "as_ar_rifle";
	game.world->animateRemoteSoldier(id, state, 1 / 30.0f);
	CHECK(!game.objects.find(rifle) && game.world->remoteSoldierWeapon(id) != 0);
	const uint32_t second = game.world->remoteSoldierWeapon(id);
	game.world->removeRemoteSoldier(id);
	CHECK(!game.world->isRemoteSoldier(id) && !game.objects.find(second));
}

// A joined client's rounds (GameWorld::fireRemoteRound): the remote
// soldier's firearm makes its bullets here (a projectile template without a
// networkable, 0x005f31d0), with tracers at the interval, which hit the
// other soldiers' capsules for their impact and damage nothing; a reload
// seen (mask 0x10000) reloads its weapon once; a server projectile's ghost
// flies as a visual round and goes off when the ghost goes.
TEST(RagDoll_RemoteSoldierRounds)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	world::ObjectTemplate* tracer = game.templates.create("GenericProjectile", "bullet_tracer");
	tracer->setGeometry("tracer_mesh");
	game.templates.find("bullet")->settings()["tracerTemplate"] = "bullet_tracer";
	game.templates.find("bullet")->settings()["tracerInterval"] = "3";
	world::ObjectTemplate* grenade = game.templates.create("GenericProjectile", "grenade");
	grenade->settings()["networkableInfo"] = "BasicInfo";
	grenade->settings()["detonation.explosionRadius"] = "5";
	game.start();
	world::Object* shooter = game.objects.create(game.templates.find("us_light_soldier"), {5, 0, 5});
	world::Object* target = game.objects.create(game.templates.find("pac_light_soldier"), {5, 0, 25});
	const uint32_t id = shooter->id();
	const float before = game.world->hitPoints(target->id());
	game::GameWorld::RemoteSoldierState idle;
	idle.weapon = "eu_ar_rifle";
	game.world->animateRemoteSoldier(target->id(), idle, 1 / 30.0f);
	game::GameWorld::RemoteSoldierState state;
	state.weapon = "eu_ar_rifle";
	state.remoteFire = true;
	state.firing = true;
	state.firePresses = 1;
	state.fireMode = game::kFireAutomatic;
	size_t made = 0, tracers = 0;
	bool allVisual = true;
	for (int i = 0; i < 20; ++i) {
		const size_t had = game.world->projectiles().size();
		game.world->animateRemoteSoldier(id, state, 1 / 30.0f);
		for (size_t k = had; k < game.world->projectiles().size(); ++k) {
			const game::Projectile& p = game.world->projectiles()[k];
			++made;
			tracers += p.tracer;
			allVisual = allVisual && p.visual && p.ghostObject == 0;
		}
		game.world->tick(1 / 30.0f);
	}
	CHECK(made >= 3 && allVisual);
	CHECK(tracers >= 1 && tracers <= (made + 2) / 3); // the first, then every third
	bool hitTarget = false;
	for (const auto& impact : game.world->impacts()) {
		hitTarget = hitTarget || impact.object == target->id();
	}
	CHECK(hitTarget);
	CHECK(game.world->hitPoints(target->id()) == before);
	// A reload seen: once (the second finds it reloading).
	state.firing = false;
	game.world->animateRemoteSoldier(id, state, 1 / 30.0f);
	game.world->takeWeaponSounds();
	game.world->reloadRemoteSoldier(id, 5);
	game.world->reloadRemoteSoldier(id, 5);
	int reloads = 0;
	for (const auto& sound : game.world->takeWeaponSounds()) {
		reloads += sound.kind == game::GameWorld::WeaponSoundEvent::Reload;
	}
	CHECK(reloads == 1);
	// A server grenade's ghost: it flies here, moving its object, and goes
	// when the ghost goes.
	world::Object* thrown = game.objects.create(grenade, {0, 10, 0});
	game.world->launchRemoteProjectile(thrown->id(), "grenade", {0, 10, 0}, {0, 0, 5});
	CHECK(game.world->hasRemoteProjectile(thrown->id()));
	game.world->tick(1 / 30.0f);
	CHECK(thrown->position().z > 0.1f);
	game.world->moveRemoteProjectile(thrown->id(), {1, 10, 1}, nullptr);
	game.world->endRemoteProjectile(thrown->id());
	CHECK(!game.world->hasRemoteProjectile(thrown->id()));
}

// A joined client's StickyProjectile event (0x0050f5d0): the server's C4
// ghost stops where the event puts it (the ghost's updates move it no
// more) and rides the client's object of the networkable it stuck to, at
// its place in that object's frame (the object as its ghost places it).
TEST(RagDoll_RemoteStickyProjectileRides)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	world::ObjectTemplate* c4 = game.templates.create("GenericProjectile", "remote_c4");
	c4->settings()["networkableInfo"] = "BasicInfo";
	world::ObjectTemplate* car = game.templates.create("PlayerControlObject", "remote_car");
	game.start();
	world::Object* vehicle = game.objects.create(car, {20, 0, 20});
	world::Object* thrown = game.objects.create(c4, {20, 3, 18});
	game.world->launchRemoteProjectile(thrown->id(), "remote_c4", {20, 3, 18}, {0, 0, 5});
	CHECK(!game.world->remoteProjectileStuck(thrown->id()));
	CHECK(game.world->stickRemoteProjectile(thrown->id(), vehicle->id(), {0, 1.5f, 1}, {0, 1, 0}));
	CHECK(game.world->remoteProjectileStuck(thrown->id()));
	game.world->tick(1 / 30.0f);
	CHECK(std::fabs(thrown->position().x - 20) < 1e-3f && std::fabs(thrown->position().y - 1.5f) < 1e-3f
		&& std::fabs(thrown->position().z - 21) < 1e-3f);
	// The vehicle's ghost drives off and turns: the round goes with it.
	vehicle->setPosition({30, 0.5f, 20});
	vehicle->setRotation({90, 0, 0});
	game.world->moveRemoteProjectile(thrown->id(), {0, 50, 0}, nullptr); // ignored once stuck
	game.world->tick(1 / 30.0f);
	const physics::Vec3 expected = physics::Vec3 {30, 0.5f, 20}
		+ physics::Mat3::fromYawPitchRoll({90, 0, 0}).transform({0, 1.5f, 1});
	std::fprintf(stderr, "  the round on the turned vehicle: %.2f %.2f %.2f\n", thrown->position().x, thrown->position().y,
		thrown->position().z);
	CHECK(std::fabs(thrown->position().x - expected.x) < 1e-3f && std::fabs(thrown->position().y - expected.y) < 1e-3f
		&& std::fabs(thrown->position().z - expected.z) < 1e-3f && std::fabs(expected.x - 31) < 1e-3f);
	// Stuck in the world (no parent): at the event's place.
	world::Object* other = game.objects.create(c4, {5, 3, 5});
	game.world->launchRemoteProjectile(other->id(), "remote_c4", {5, 3, 5}, {0, -5, 0});
	CHECK(game.world->stickRemoteProjectile(other->id(), 0, {5, 0.01f, 6}, {0, 1, 0}));
	game.world->tick(1 / 30.0f);
	CHECK(std::fabs(other->position().z - 6) < 1e-3f && std::fabs(other->position().y - 0.01f) < 1e-3f);
}

// A joined client's seated players on a turret (GameWorld::
// animateRemoteSeated with setRemotePartPose): a seat that rides another
// seat's turret turns with that turret's yaw bundle as its ghost's
// RotationalBundle networkable has it (read 0x00602070; the seatMount the
// server's 0x00587a30 puts under the seat's matrix), and the hands take the
// grips of the seat's IK object (0x005ee910, 0x005efc00) where the ghosts'
// bundles put it.
TEST(RagDoll_RemoteSeatedTurretAndGrips)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	game.templates.registerType(12, "RotationalBundle");
	world::ObjectTemplate* tank = game.templates.create("PlayerControlObject", "remote_tank");
	tank->settings()["seatInformation"] = "remote_tank 0/0.5/0.4 0/0/0";
	world::ObjectTemplate* gunner = game.templates.create("PlayerControlObject", "remote_tank_gunner");
	gunner->settings()["seatInformation"] = "remote_tank_gunner 0/1/0 0/0/0";
	tank->children().push_back({"remote_tank_gunner", {0, 1, 0}, {}});
	world::ObjectTemplate* turret = game.templates.create("RotationalBundle", "remote_tank_turret");
	turret->settings()["inputToYaw"] = "PIMouseLookX";
	gunner->children().push_back({"remote_tank_turret", {}, {}});
	// The cupola rides the turret, its gun's grips on a bundle of its own.
	world::ObjectTemplate* cupola = game.templates.create("PlayerControlObject", "remote_tank_cupola");
	cupola->settings()["seatInformation"] = "remote_tank_cupola 0.5/1.5/-0.5 0/0/0";
	cupola->settings()["seatAnimationSystem"] = "Objects/Vehicles/AS_FAV/3p_as_fav_driver_animSys.inc";
	turret->children().push_back({"remote_tank_cupola", {0.5f, 0.5f, -0.5f}, {}});
	world::ObjectTemplate* mount = game.templates.create("RotationalBundle", "remote_tank_cupola_mount");
	mount->settings()["inputToYaw"] = "PIMouseLookX";
	game.templates.addComponent(*mount, "Ik");
	mount->settings()["ik.positionLeft"] = "-0.145199/-0.0447125/-0.186665";
	mount->settings()["ik.rotationLeft"] = "-94.775/-2.18003/-58.2225";
	mount->settings()["ik.poleVectorLeft"] = "-0.294253/-0.339534/-3.01e-6";
	mount->settings()["ik.positionRight"] = "0.17838/-0.0352049/-0.155193";
	mount->settings()["ik.rotationRight"] = "-114.531/21.6388/120.12";
	mount->settings()["ik.poleVectorRight"] = "0.148879/-0.231249/0.143271";
	cupola->children().push_back({"remote_tank_cupola_mount", {0, 0.35f, 0.45f}, {}});
	game.start();
	const uint32_t vehicle = game.objects.create(tank, {10, 1, 10}, {30, 0, 0})->id();
	world::Object* ghost = game.objects.create(game.templates.find("us_light_soldier"), {10.5f, 2.5f, 9.5f});
	const uint32_t id = ghost->id();
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	CHECK(skeleton != nullptr);
	if (!skeleton) {
		return;
	}
	const auto seats = game.world->vehicleSeats(vehicle);
	CHECK(seats.size() == 3 && seats[2].name == "remote_tank_cupola");
	// The ghosts' bundles: the turret 40 degrees round, the cupola's gun 0 then 50.
	float turretYaw = 40, gunYaw = 0;
	game.world->setRemotePartPose([&](uint32_t object, const std::string& part, world::Vec3& offset, world::Vec3& rotation) {
		if (object != vehicle) {
			return false;
		}
		offset = {};
		if (part == "remote_tank_turret") {
			rotation = {turretYaw, 0, 0};
			return true;
		}
		if (part == "remote_tank_cupola_mount") {
			rotation = {gunYaw, 0, 0};
			return true;
		}
		return false;
	});
	for (int i = 0; i < 5; ++i) {
		game.world->animateRemoteSeated(id, vehicle, 2, 0, 1 / 30.0f);
	}
	std::fprintf(stderr, "  the cupola's sitter faces %.1f (the tank 30, its turret 40)\n", ghost->rotation().x);
	CHECK(std::abs(std::remainder(ghost->rotation().x - 70, 360.0f)) < 0.5f);
	const int leftWrist = skeleton->find("left_wrist1");
	CHECK(leftWrist >= 0);
	std::vector<anim::Matrix> ahead, aside;
	CHECK(game.world->skeletonPose(id, *skeleton, ahead));
	// The grip's bundle turned: the left hand moves with it, and lies on the
	// grip (the IK object's frame through the soldier's).
	gunYaw = 50;
	game.world->animateRemoteSeated(id, vehicle, 2, 0, 1 / 30.0f);
	CHECK(game.world->skeletonPose(id, *skeleton, aside));
	if (leftWrist < 0 || ahead.size() != skeleton->nodes.size() || aside.size() != ahead.size()) {
		return;
	}
	const auto at = [](const std::vector<anim::Matrix>& pose, int bone) {
		const anim::Matrix& m = pose[static_cast<size_t>(bone)];
		return physics::Vec3 {m[12], m[13], m[14]};
	};
	const float moved = std::sqrt(physics::lengthSquared(at(aside, leftWrist) - at(ahead, leftWrist)));
	// Where the grip is in the soldier's space.
	const world::Object* tankObject = game.objects.find(vehicle);
	const physics::Mat3 tankTurn = physics::Mat3::fromYawPitchRoll({30, 0, 0});
	const physics::Mat3 turretTurn = physics::Mat3::fromYawPitchRoll({turretYaw, 0, 0}) * tankTurn;
	const physics::Vec3 cupolaAt = tankTurn.transform({0, 1, 0}) + turretTurn.transform({0.5f, 0.5f, -0.5f})
		+ physics::Vec3 {tankObject->position().x, tankObject->position().y, tankObject->position().z};
	const physics::Mat3 mountTurn = physics::Mat3::fromYawPitchRoll({gunYaw, 0, 0}) * turretTurn;
	const physics::Vec3 mountAt = cupolaAt + turretTurn.transform({0, 0.35f, 0.45f});
	const physics::Mat3 soldierTurn = physics::Mat3::fromYawPitchRoll({ghost->rotation().x, ghost->rotation().y, ghost->rotation().z});
	const physics::Vec3 grip = soldierTurn.untransform(mountTurn.transform({-0.145199f, -0.0447125f, -0.186665f}) + mountAt
		- physics::Vec3 {ghost->position().x, ghost->position().y, ghost->position().z});
	const float off = std::sqrt(physics::lengthSquared(at(aside, leftWrist) - grip));
	std::fprintf(stderr, "  the left wrist moved %.3f m with the grip, %.3f m from it\n", moved, off);
	CHECK(moved > 0.05f);
	CHECK(off < 0.25f);
}

// A joined game's worn kit (helmet, vest, pack) is its soldier's child
// (0x005718d0 -> 0x00571d10 addChild): skinned to the remote soldier's pose.
// A kit ghost no one wears lies on the ground (geometry.dropGeom).
TEST(RagDoll_RemoteWornKitTakesItsSoldiersPose)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Game game(gameDir);
	game.start();
	const anim::Skeleton* skeleton = game.systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	CHECK(skeleton != nullptr);
	if (!skeleton) {
		return;
	}
	const uint32_t soldier = game.objects.create(game.templates.find("us_light_soldier"), {3, 1, 0})->id();
	const uint32_t worn = game.objects.create(game.templates.find("us_light_soldier"), {3, 1, 0})->id();
	const uint32_t lying = game.objects.create(game.templates.find("us_light_soldier"), {9, 0, 0})->id();
	game::GameWorld::RemoteSoldierState foot;
	for (int i = 0; i < 10; ++i) {
		game.world->animateRemoteSoldier(soldier, foot, 1 / 30.0f);
	}
	std::vector<anim::Matrix> body, kit;
	CHECK(game.world->skeletonPose(soldier, *skeleton, body) && body.size() > 9);
	CHECK(!game.world->skeletonPose(worn, *skeleton, kit)); // not his yet
	game.world->setRemoteKits({{worn, soldier}}, {lying});
	CHECK(game.world->skeletonPose(worn, *skeleton, kit) && kit.size() == body.size());
	CHECK(kit.size() > 9 && kit[9][13] == body[9][13]);
	CHECK(!game.world->isDroppedKit(worn));
	CHECK(game.world->isDroppedKit(lying));
}
