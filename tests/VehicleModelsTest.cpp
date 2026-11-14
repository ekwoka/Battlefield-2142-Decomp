// The vehicle models other than newCar2, with the shipped templates on flat
// ground: the tracked tank, the hover tank, the walker, the transport and
// the gunship, driven by a player in the driver's seat.
#include "AI/Heli.h"
#include "AI/PathFollower.h"
#include "Animation/AnimationConsole.h"
#include "Animation/AnimationSystem.h"
#include "Animation/FirstPerson.h"
#include "Console/Console.h"
#include "Effects/Effects.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Game/Bots.h"
#include "Game/Vehicle.h"
#include "Geometry/GeometryTemplateManager.h"
#include "Hud/Hud.h"
#include "IO/File/FileManager.h"
#include "Main/HudMenusVehicles.h"
#include "Main/GameAudio.h"
#include "Main/HudAlerts.h"
#include "Sound/Game/GameSounds.h"
#include "Sound/Game/SoundConsole.h"
#include "Sound/Game/SoundObjectTemplate.h"
#include "Sound/Game/SoundManager.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
#include "Main/HudWorld.h"
#include "Main/PlayerCamera.h"
#include "Main/RoundVisuals.h"
#include "Net/RemoteInput.h"
#include "Render/PostProduction.h"
#include "SeatAim.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/TemplateFiles.h"
#include "World/WorldConsole.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#ifdef BF2142_RENDER_TESTS
#include "Render/LevelRenderer.h"
#include "Render/PngWriter.h"
#include "Geometry/MeshFile.h"
#include "Terrain/Heightmap.h"
#include "Scene/LightSettings.h"
#include "TitanLevel.h"
#include <filesystem>
#endif

using namespace dice::hfe;

namespace {

bool near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

// Every object template and the materials, on flat ground at height 0; the
// EU spawn point at the origin facing +z.
struct Garage {
	io::FileManager files;
	Console console {&files};
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	geom::GeometryTemplateManager geometries;
	physics::CollisionTemplateManager collisionTemplates;
	physics::MaterialManager materials;
	std::unique_ptr<game::GameWorld> world;
	bool loaded = false;

	explicit Garage(const char* gameDir, const char* modPath = "mods/bf2142", bool clientAssets = false,
		const std::function<void(Garage&)>& beforeLoad = {})
	{
		files.setRootPath(gameDir);
		files.addSearchPath(modPath);
		if (std::string(modPath) != "mods/bf2142") files.addSearchPath("mods/bf2142");
		files.setMountingAllowed(true);
		bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
			[this](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
		std::string out;
		CHECK(console.run("ServerArchives.con", {}, out));
		if (clientAssets) CHECK(console.run("ClientArchives.con", {}, out));
		files.setMountingAllowed(false);
		world::registerWorldConsoleMethods(console, templates, objects);
		if (beforeLoad) {
			beforeLoad(*this); // e.g. the sound factory, the WarningHud methods: before the untyped properties
		}
		geom::registerGeometryConsoleMethods(console, geometries);
		templates.setCurrentFileProvider([this] { return console.currentFile(); });
		physics::registerCollisionConsoleMethods(console, collisionTemplates);
		physics::registerMaterialConsoleMethods(console, materials);
		std::vector<std::string> untyped;
		std::ifstream table(std::string(BF2142_SOURCE_DIR) + "/symbols/BF2142.exe.console.csv");
		std::string line;
		std::getline(table, line);
		while (std::getline(table, line)) {
			std::vector<std::string> cols;
			std::stringstream row(line);
			for (std::string col; std::getline(row, col, ',');) {
				cols.push_back(col);
			}
			if (cols.size() >= 6 && cols[0] == "ObjectTemplate" && !console.hasMethod("ObjectTemplate." + cols[1])
				&& (cols[2] == "property" || (cols[2] == "method" && cols[4] == "1" && cols[5] == "1"))) {
				untyped.push_back(cols[1]);
			}
		}
		world::registerUntypedTemplateProperties(console, templates, untyped);
		// The engine's constants (c_ETTank ...), as main registers them.
		std::ifstream constants(std::string(BF2142_SOURCE_DIR) + "/symbols/BF2142.exe.constants.csv");
		while (std::getline(constants, line)) {
			const size_t comma = line.find(',');
			if (comma != std::string::npos) {
				console.registerConstant(line.substr(0, comma), line.substr(comma + 1));
			}
		}
		templates.setLoading(true);
		world::loadObjectTemplates(console, files, "Objects/", false);
		templates.setLoading(false);
		console.run("Common/Material/MaterialManagerSettings.con", {}, out);
		world::ObjectTemplate* point = templates.create("ControlPoint", "garage_cp");
		point->settings()["controlPointId"] = "2";
		point->settings()["team"] = "2";
		point->settings()["unableToChangeTeam"] = "1";
		objects.create(point, {-200, 0, 0});
		world::ObjectTemplate* spawn = templates.create("SpawnPoint", "garage_sp");
		objects.create(spawn, {0, 0, 0}, {0, 0, 0})->setControlPointId(2);
		loaded = templates.find("eu_tank") != nullptr;
	}

	uint32_t groundMaterial = 0; // the terrain's material under every point
	float groundHeight = 0;      // start's ground, which a test may move after
	// A shape over it (x, z -> height above groundHeight), when a test sets one.
	std::function<float(float, float)> groundShape;
	std::optional<physics::Ground> terrain; // instead of that ground
	// A pit 100 m deep from z = 10 on: a parked gunship's TV missile leaves
	// its camera (CameraId 11, under the nose, 10 degrees down:
	// fireInCameraDof) a metre over the ground.
	void pitAhead()
	{
		const auto h = [](float, float z) { return z < 10 ? 0.0f : -100.0f; };
		terrain = physics::Ground {h,
			[h](float x, float z, physics::Vec3& normal) {
				normal = {0, 1, 0};
				return h(x, z);
			},
			[this](float, float) { return groundMaterial; }};
	}

	// The vehicle at `at` facing +z, the world started and settled, the
	// player spawned beside it.
	uint32_t start(const char* name, const physics::Vec3& at, float settle = 3, float ground = 0, float water = -10000, float yaw = 0)
	{
		world::ObjectTemplate* t = templates.find(name);
		CHECK(t != nullptr);
		if (!t) {
			return 0;
		}
		world::Object* made = objects.create(t, {at.x, at.y, at.z}, {yaw, 0, 0});
		made->setTeam(2);
		world = std::make_unique<game::GameWorld>(templates, objects);
		const uint32_t material = groundMaterial;
		groundHeight = ground;
		world->enablePhysics(files, collisionTemplates,
			terrain ? *terrain
					: physics::Ground {[this](float x, float z) { return groundHeight + (groundShape ? groundShape(x, z) : 0.0f); },
						  [this](float x, float z, physics::Vec3& normal) {
							  normal = {0, 1, 0};
							  return groundHeight + (groundShape ? groundShape(x, z) : 0.0f);
						  },
						  [material](float, float) { return material; }},
			&materials);
		if (water > -10000) {
			world->setWaterLevel(water);
		}
		world->startRound();
		run(settle);
		return made->id();
	}

	// Into the driver's seat: the player tried round the vehicle until an
	// entry point takes him.
	bool enter(uint32_t vehicle, bool spawn = true)
	{
		if (spawn && !world->spawnPlayer(2)) {
			return false;
		}
		game::GameWorld::PlayerInfo me;
		world->playerInfo(world->localPlayer(), me);
		const world::Vec3& p = objects.find(vehicle)->position();
		for (float r = 0; r <= 6; r += 1.5f) {
			for (int k = 0; k < 8; ++k) {
				const float a = static_cast<float>(k) * 0.785398f;
				for (float h : {0.0f, p.y - 1.0f}) {
					world->setSoldierPosition(me.soldier, {p.x + r * std::sin(a), h, p.z + r * std::cos(a)});
					game::SoldierInput use;
					use.use = true;
					world->setPlayerInput(use);
					world->tick(1 / 30.0f);
					world->setPlayerInput({});
					world->tick(1 / 30.0f);
					if (world->playerVehicle()) {
						return world->playerVehicle()->id() == vehicle;
					}
				}
			}
		}
		return false;
	}

	void run(float seconds)
	{
		for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
			world->tick(1 / 30.0f);
		}
	}

	void drive(const game::SoldierInput& input, float seconds)
	{
		for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
			world->setPlayerInput(input);
			world->tick(1 / 30.0f);
		}
	}

	// The soldiers' and seats' animation systems, as the walker test loads
	// them (call after start).
	std::unique_ptr<anim::AnimationSystems> systems;
	void animate()
	{
		files.setMountingAllowed(true);
		std::string out;
		console.run("ClientArchives.con", {}, out);
		files.setMountingAllowed(false);
		systems = std::make_unique<anim::AnimationSystems>(
			[this](const std::string& path, std::vector<uint8_t>& data) { return files.readFile(path, data); });
		anim::registerAnimationConsoleMethods(console, *systems);
		console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, out);
		world->setAnimationSystems(systems.get(), [this](const std::string& path) {
			return systems->get(path, [&] {
				std::string text;
				console.run(path, {}, text);
			});
		});
	}

	physics::Vec3 position(uint32_t id) const
	{
		const world::Vec3& p = objects.find(id)->position();
		return {p.x, p.y, p.z};
	}
	float yaw(uint32_t id) const { return objects.find(id)->rotation().x; }
};

const char* gameDir() { return std::getenv("BF2142_GAME_DIR"); }

} // namespace

// The tracked tank (eu_tank: Engine c_ETTank, EngineGrip wheels; the old
// wheel path 0x006b8230 and the tank's engine 0x0064d4f0): it settles on its
// springs, drives at its tracks' speed (rpm x 14 x 3.5 / 4: 12.25 m/s at
// most) and turns by its tracks and the trackTurn torque.
TEST(Vehicles_TrackedTankDrivesAndTurns)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t tank = g.start("eu_tank", {0, 2, 6});
	const physics::Vec3 rest = g.position(tank);
	CHECK(rest.y > 0 && rest.y < 2); // on its springs
	CHECK(g.enter(tank));
	const game::Vehicle* v = g.world->playerVehicleState();
	CHECK(v && !v->hasNewCar2() && v->engines().size() == 1 && (v->engines()[0].flags & 6) == 6);
	game::SoldierInput input;
	input.throttle = 1;
	g.drive(input, 4);
	const physics::Vec3 ahead = g.position(tank);
	std::fprintf(stderr, "  eu_tank: %.1f m ahead after 4 s, rpm %.2f\n", ahead.z - rest.z, v ? v->rpm() : 0.0f);
	CHECK(ahead.z - rest.z > 15 && ahead.z - rest.z < 60);
	CHECK(std::fabs(ahead.x - rest.x) < 3);
	const float before = g.yaw(tank);
	input.strafe = 1; // PIYaw: right
	g.drive(input, 2);
	const float turned = std::remainder(g.yaw(tank) - before, 360.0f);
	std::fprintf(stderr, "  eu_tank: turned %.1f degrees in 2 s\n", turned);
	CHECK(turned > 15);
	// Let go: it comes to a stop (the tracks hold still with no rpm).
	g.drive({}, 4);
	game::GameWorld::PlayerInfo me;
	g.world->playerInfo(g.world->localPlayer(), me);
	const float speed = [&] {
		for (const auto& info : g.world->vehicleList()) {
			if (info.object == tank) {
				return std::sqrt(physics::lengthSquared(info.velocity));
			}
		}
		return -1.0f;
	}();
	CHECK(speed >= 0 && speed < 1);
}

// The hover tank (as_tank: Springs straight under the root, plane and
// helicopter engines, no wheels driven): it floats on its antigravs; W
// levels the forward thruster and pushes it on, A/D strafe (PIYaw), the
// mouse's x (PIRoll) turns it.
TEST(Vehicles_HoverTankThrustsAndStrafes)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t tank = g.start("as_tank", {0, 3, 6});
	const physics::Vec3 rest = g.position(tank);
	CHECK(rest.y > 0.5f && rest.y < 3);
	CHECK(g.enter(tank));
	const game::Vehicle* v = g.world->playerVehicleState();
	CHECK(v && v->engines().size() == 4 && !v->flies());
	game::SoldierInput input;
	input.throttle = 1;
	g.drive(input, 3);
	const physics::Vec3 ahead = g.position(tank);
	std::fprintf(stderr, "  as_tank: %.1f m ahead after 3 s, height %.2f\n", ahead.z - rest.z, ahead.y);
	CHECK(ahead.z - rest.z > 5);
	input = {};
	input.strafe = 1;
	g.drive(input, 3);
	const physics::Vec3 aside = g.position(tank);
	std::fprintf(stderr, "  as_tank: %.1f m to the right after 3 s\n", aside.x - ahead.x);
	CHECK(aside.x - ahead.x > 2);
	const float before = g.yaw(tank);
	input = {};
	input.turn = 5; // a full PIRoll right
	g.drive(input, 2);
	const float turned = std::remainder(g.yaw(tank) - before, 360.0f);
	std::fprintf(stderr, "  as_tank: turned %.1f degrees in 2 s\n", turned);
	CHECK(turned > 10);
}

TEST(Vehicles_HoverTankGunFollowsHullAfterSteering)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		Garage g(gameDir(), mod);
		CHECK(g.loaded && g.templates.find("as_tank"));
		if (!g.loaded || !g.templates.find("as_tank")) continue;
		const uint32_t tank = g.start("as_tank", {0, 3, 6});
		CHECK(g.enter(tank));
		const auto seats = g.world->vehicleSeats(tank);
		CHECK(!seats.empty() && seats[0].yawBundle.empty() && !seats[0].pitchBundle.empty());
		const float before = g.yaw(tank);
		game::SoldierInput steer;
		steer.throttle = 1;
		steer.turn = 5;
		g.drive(steer, 2.0f);
		CHECK(std::fabs(std::remainder(g.yaw(tank) - before, 360.0f)) > 2);
		g.world->setPlayerInput({});
		g.run(4.0f); // let hull rotation and inherited velocity settle before firing
		float yaw = 0, pitch = 0;
		CHECK(g.world->gunAim(tank, 0, yaw, pitch));
		std::fprintf(stderr, "  %s: hover gun yaw relative to hull %.2f\n", mod, yaw);
		CHECK(std::fabs(yaw) < 0.01f);
		const size_t count = g.world->projectiles().size();
		game::SoldierInput fire;
		fire.fire = true;
		g.drive(fire, 0.1f);
		bool fired = false;
		for (size_t i = count; i < g.world->projectiles().size(); ++i) {
			const auto& p = g.world->projectiles()[i];
			if (p.shooter != tank) continue;
			fired = true;
			const float shotYaw = std::atan2(p.velocity.x, p.velocity.z) * 57.29578f;
			CHECK(std::fabs(std::remainder(shotYaw - g.yaw(tank), 360.0f)) < 4);
		}
		CHECK(fired);
	}
}

// The walker (as_heavy_mech: MechLegs, its response and its keepUp
// regulator): it stands on its legs (the legs' object 0.52 below the
// root, 2.8 long), walks at 7.5 m/s and turns on the spot.
TEST(Vehicles_WalkerStandsWalksAndTurns)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t mech = g.start("as_heavy_mech", {0, 4, 6});
	const physics::Vec3 rest = g.position(mech);
	std::fprintf(stderr, "  as_heavy_mech: stands at %.2f\n", rest.y);
	CHECK(rest.y > 3.0f && rest.y < 3.7f);
	CHECK(g.enter(mech));
	const game::Vehicle* v = g.world->playerVehicleState();
	CHECK(v && v->legs());
	game::SoldierInput input;
	input.throttle = 1;
	g.drive(input, 3);
	const physics::Vec3 ahead = g.position(mech);
	std::fprintf(stderr, "  as_heavy_mech: %.1f m ahead after 3 s, height %.2f\n", ahead.z - rest.z, ahead.y);
	CHECK(ahead.z - rest.z > 15 && ahead.z - rest.z < 24);
	CHECK(ahead.y > 3.0f && ahead.y < 3.7f);
	// Stopped, then turning right on the spot.
	g.drive({}, 1);
	const float before = g.yaw(mech);
	input = {};
	input.strafe = 1;
	g.drive(input, 2);
	const float turned = std::remainder(g.yaw(mech) - before, 360.0f);
	const physics::Vec3 after = g.position(mech);
	std::fprintf(stderr, "  as_heavy_mech: turned %.1f degrees in 2 s\n", turned);
	CHECK(turned > 30);
	CHECK(std::sqrt((after.x - ahead.x) * (after.x - ahead.x) + (after.z - ahead.z) * (after.z - ahead.z)) < 5);
	// Upright all along (the keepUp couple).
	CHECK(std::fabs(g.objects.find(mech)->rotation().y) < 10 && std::fabs(g.objects.find(mech)->rotation().z) < 10);
}

// A walker left standing falls asleep like any node; its legs' update
// (0x00650e90, run while the node sleeps) wakes it in any state but
// standing (+0x1bc, slot 0x160), so it turns on the spot from rest (the
// engines' wake, a throttle or rpm, never comes for a walker turning).
TEST(Vehicles_WalkerTurnsFromRest)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t mech = g.start("as_heavy_mech", {0, 4, 6});
	CHECK(g.enter(mech));
	g.drive({}, 5); // still for 5 s: asleep
	const float before = g.yaw(mech);
	game::SoldierInput input;
	input.strafe = -1;
	g.drive(input, 2);
	const float turned = std::remainder(g.yaw(mech) - before, 360.0f);
	std::fprintf(stderr, "  as_heavy_mech: turned %.1f degrees in 2 s from rest\n", turned);
	CHECK(turned < -30);
}

// A per-tick integration trace of the shared AI aim controller on the shipped
// tank and walker templates. Move a seen target across the +/-180 degree yaw
// seam, then kill it so the bot hands off to a second target. The trace keeps
// the controller's degree command, measured gun yaw, hull yaw and target id
// together; this catches a one-tick 360-degree swing that an end-angle test
// cannot see.
TEST(Vehicles_BotTankAndWalkerAimYawIsContinuous)
{
	if (!gameDir()) return;
	for (const auto& [vehicleName, equipment] : {std::pair<const char*, const char*> {"eu_tank", "Tank"},
													{"as_heavy_mech", "Mech"}}) {
		Garage g(gameDir());
		if (!g.loaded) return;
		game::BotSettings settings;
		settings.behaviours = {"Idle", "MoveTo", "Fire"};
		game::registerBotConsoleMethods(g.console, settings);
		const std::string aiName = std::string("Wave9") + equipment;
		const std::string plugin = aiName + "_Unit";
		const std::string vehicleBehaviour = std::string("aiSettings.setVehicleBehaviour ") + equipment
			+ " Fire BBFireInfantery BBPFireInfantery 4 UCUnion UnitWeights";
		const std::vector<std::string> commands {
			"aiTemplatePlugIn.create Unit " + plugin, std::string("aiTemplatePlugIn.equipmentTypeName ") + equipment,
			"aiTemplatePlugIn.create Unit Wave9SeatUnit", std::string("aiTemplatePlugIn.equipmentTypeName ") + equipment,
			"aiTemplatePlugIn.create ControlInfo Wave9Control", "aiTemplate.create Wave9Seat", "aiTemplate.addPlugIn Wave9Control",
			"aiTemplate.addPlugIn Wave9SeatUnit",
			"aiTemplatePlugIn.proportionalConstant -7", "aiTemplatePlugIn.derivativeConstant -0.01",
			"aiTemplate.create " + aiName, "aiTemplate.addType ITGround",
			"aiTemplate.addPlugIn " + plugin,
			"weaponTemplate.create Wave9Gun", "weaponTemplate.setStrength Infantry 5",
			"weaponTemplate.maxRange 150", "weaponTemplate.weaponType FireArm",
			"weaponTemplate.allowedDeviation 2", "aiSettings.createBehaviourModifiers UnitWeights",
			"aiSettings.setBehaviourModifier Idle 1.0", vehicleBehaviour,
			std::string("aiSettings.setVehicleDefaultBehaviour ") + equipment + " Fire", "aiSettings.setViewDistance 250"};
		std::string out;
		for (const std::string& command : commands) {
			out.clear();
			g.console.execute(command, out);
			if (!out.empty()) std::fprintf(stderr, "  config %s: %s\n", command.c_str(), out.c_str());
		}
		world::ObjectTemplate* root = g.templates.find(vehicleName);
		CHECK(root != nullptr);
		if (!root) continue;
		root->settings()["aiTemplate"] = aiName;
		const std::string gunName = std::string(vehicleName) == "eu_tank" ? "EU_TANK_Barrel" : "AS_heavy_mech_main_cannon";
		world::ObjectTemplate* mainGun = g.templates.find(gunName);
		CHECK(mainGun != nullptr);
		if (mainGun) mainGun->settings()["aiTemplate"] = "Wave9Gun";
		if (std::string(vehicleName) == "as_heavy_mech") {
			if (world::ObjectTemplate* turretGun = g.templates.find("as_heavy_mech_AAcannon_firearm")) {
				turretGun->settings()["aiTemplate"] = "Wave9Gun";
			}
		}
		const uint32_t vehicle = g.start(vehicleName, {0, vehicleName[0] == 'a' ? 4.0f : 2.0f, 6});
		const auto seats = g.world->vehicleSeats(vehicle);
		CHECK(!seats.empty());
		if (seats.empty()) continue;
		const int gunSeat = std::string(vehicleName) == "as_heavy_mech" ? 1 : 0;
		CHECK(static_cast<size_t>(gunSeat) < seats.size());
		CHECK(!seats[static_cast<size_t>(gunSeat)].yawBundle.empty());
		CHECK(seats[static_cast<size_t>(gunSeat)].yawSpeed > 0);
		std::fprintf(stderr, "  VEHICLEAI %s turretSeat=%d name=%s weapon=%s yawBundle=%s yawRate=%.1f\n", vehicleName,
			gunSeat, seats[static_cast<size_t>(gunSeat)].name.c_str(), seats[static_cast<size_t>(gunSeat)].weapon.c_str(),
			seats[static_cast<size_t>(gunSeat)].yawBundle.c_str(), seats[static_cast<size_t>(gunSeat)].yawSpeed);
		if (world::ObjectTemplate* seatWeaponTemplate = g.templates.find(seats[static_cast<size_t>(gunSeat)].weapon)) {
			seatWeaponTemplate->settings()["aiTemplate"] = "Wave9Gun";
		}
		for (const auto& seatWeapon : seats[static_cast<size_t>(gunSeat)].weapons) {
			if (world::ObjectTemplate* seatWeaponTemplate = g.templates.find(seatWeapon.weapon)) {
				seatWeaponTemplate->settings()["aiTemplate"] = "Wave9Gun";
			}
		}
		settings.vehicles[equipment].sensingPattern = "Turret";
		if (world::ObjectTemplate* seatTemplate = g.templates.find(seats[static_cast<size_t>(gunSeat)].name)) {
			seatTemplate->settings()["aiTemplate"] = "Wave9Seat";
		} else {
			CHECK(false);
		}
		const int bot = g.world->addPlayer("Wave9Bot", 2, true);
		CHECK(g.world->spawnSoldier(bot));
		game::GameWorld::PlayerInfo botInfo;
		g.world->playerInfo(bot, botInfo);
		const physics::Vec3 origin = g.position(vehicle);
		bool boarded = false;
		for (float radius = 0; radius <= 6 && !boarded; radius += 1.5f) {
			for (int k = 0; k < 8 && !boarded; ++k) {
				const float angle = k * 0.785398163f;
				g.world->setSoldierPosition(botInfo.soldier, {origin.x + radius * std::sin(angle), origin.y - 1.0f,
					origin.z + radius * std::cos(angle)});
				game::SoldierInput use; use.use = true;
				g.world->setInput(bot, use); g.world->tick(1 / 30.0f);
				g.world->setInput(bot, {}); g.world->tick(1 / 30.0f);
				g.world->playerInfo(bot, botInfo);
				boarded = botInfo.vehicle == vehicle && botInfo.seat == 0;
			}
		}
		CHECK(boarded);
		if (!boarded) continue;
		if (gunSeat > 0) {
			game::SoldierInput switchSeat;
			switchSeat.switchSeat = gunSeat + 1;
			g.world->setInput(bot, switchSeat);
			g.world->tick(1 / 30.0f);
			g.world->setInput(bot, {});
			g.world->tick(1 / 30.0f);
			g.world->playerInfo(bot, botInfo);
			CHECK(botInfo.vehicle == vehicle && botInfo.seat == gunSeat);
		}
		physics::Vec3 gunOrigin;
		physics::Mat3 gunFrame;
		CHECK(g.world->gunFrame(vehicle, gunSeat, gunOrigin, gunFrame));
		const physics::Vec3 rootEye = botInfo.position + physics::Vec3 {0, 1.6f, 0};
		const float muzzleOffset = std::sqrt(physics::lengthSquared(gunOrigin - rootEye));
		std::fprintf(stderr, "  AIMORIGIN %s seat=%d muzzle-root-eye=%.3fm yawBundle=%s yawRate=%.1f\n", vehicleName, gunSeat,
			muzzleOffset, seats[static_cast<size_t>(gunSeat)].yawBundle.c_str(), seats[static_cast<size_t>(gunSeat)].yawSpeed);
		CHECK(muzzleOffset > 1.0f);
		const int enemy = g.world->addPlayer("Wave9Target", 1, false);
		CHECK(g.world->spawnSoldier(enemy));
		game::GameWorld::PlayerInfo enemyInfo;
		g.world->playerInfo(enemy, enemyInfo);
		const int handoffEnemy = g.world->addPlayer("Wave9Handoff", 1, false);
		CHECK(g.world->spawnSoldier(handoffEnemy));
		game::GameWorld::PlayerInfo handoffInfo;
		g.world->playerInfo(handoffEnemy, handoffInfo);
		g.world->setSoldierPosition(handoffInfo.soldier, {origin.x + 300, 0, origin.z});
		game::BotManager bots(*g.world, std::move(settings));
		// Isolate turret tracking from the driver's pursuit steering. Keep
		// the actual AI degree command and seat-weapon update path intact.
		const auto stepAim = [&]() {
			bots.update(1 / 30.0f);
			game::SoldierInput command = bots.lastInputFor(bot);
			command.throttle = 0;
			command.strafe = 0;
			command.sprint = false;
			g.world->setInput(bot, command);
			g.world->tick(1 / 30.0f);
		};
		g.world->setSoldierPosition(enemyInfo.soldier, {origin.x + 1000, 0, origin.z});
		std::set<int> idleSectors;
		float idleMaxGunStep = 0, idlePreviousYaw = 0;
		float idleMaxCommand = 0;
		bool idleHavePrevious = false;
		for (int tick = 0; tick < 150; ++tick) {
			stepAim();
			const game::SoldierInput command = bots.lastInputFor(bot);
			idleMaxCommand = std::max(idleMaxCommand, std::fabs(command.turn));
			float yaw = 0, pitch = 0;
			g.world->gunAim(vehicle, gunSeat, yaw, pitch);
			const float step = idleHavePrevious ? std::remainder(yaw - idlePreviousYaw, 360.0f) : 0.0f;
			if (idleHavePrevious) idleMaxGunStep = std::max(idleMaxGunStep, std::fabs(step));
			idlePreviousYaw = yaw; idleHavePrevious = true;
			if (bots.lookPlanned(bot)) idleSectors.insert(bots.lookSector(bot));
			std::fprintf(stderr, "  IDLETRACE %s tick=%d target=%d sector=%d planned=%d cmd=%7.3f gun=%7.3f dgun=%7.3f\n",
				vehicleName, tick, bots.target(bot), bots.lookSector(bot), bots.lookPlanned(bot), command.turn, yaw, step);
		}
		std::fprintf(stderr, "  IDLESUMMARY %s seat=%d sectors=%zu max command %.3f max gun delta %.3f deg/tick (cap %.3f), target=%d\n",
			vehicleName, gunSeat, idleSectors.size(), idleMaxCommand, idleMaxGunStep,
			seats[static_cast<size_t>(gunSeat)].yawSpeed / 30.0f, bots.target(bot));
		CHECK(bots.target(bot) < 0);
		CHECK(idleSectors.size() >= 2);
		CHECK(idleMaxGunStep > 0.05f);
		CHECK(idleMaxCommand <= seats[static_cast<size_t>(gunSeat)].yawSpeed / 30.0f + 0.05f);
		CHECK(idleMaxGunStep <= seats[static_cast<size_t>(gunSeat)].yawSpeed / 30.0f + 0.05f);
		g.world->playerInfo(bot, botInfo);
		std::fprintf(stderr, "  IDLEPLAYER %s vehicle=%u expected=%u seat=%d driving=%d alive=%d\n", vehicleName,
			botInfo.vehicle, vehicle, botInfo.seat, botInfo.driving, botInfo.alive);
		float scanYaw = 0, scanPitch = 0;
		g.world->gunAim(vehicle, gunSeat, scanYaw, scanPitch);
		const float scanWorldYaw = (g.objects.find(vehicle)->rotation().x + scanYaw) * 0.0174532925f;
		const float trackStart = std::remainder(scanWorldYaw * 57.29578f, 360.0f);
		const float trackEnd = trackStart >= 0 ? 190.0f : -190.0f;
		for (int i = 0; i < 90; ++i) {
			const float a = trackStart * 0.0174532925f;
			g.world->setSoldierPosition(enemyInfo.soldier,
				{origin.x + 80.0f * std::sin(a), 0, origin.z + 80.0f * std::cos(a)});
			stepAim();
		}
		std::fprintf(stderr, "  AIMWARMUP %s seat=%d target=%d known=%d range=%.1f start=%.1f end=%.1f yawRate=%.1f limits=%.1f/%.1f\n", vehicleName,
			gunSeat, bots.target(bot), bots.knows(bot, enemy), bots.preferredRange(bot), trackStart, trackEnd, seats[static_cast<size_t>(gunSeat)].yawSpeed,
			seats[static_cast<size_t>(gunSeat)].minYaw, seats[static_cast<size_t>(gunSeat)].maxYaw);
		CHECK(bots.target(bot) == enemy);
		float settledError = 180;
		float previousGunYaw = 0;
		bool havePrevious = false;
		float maxGunStep = 0, maxCommand = 0;
		int seamSamples = 0;
		int handoffSeenAt = -1;
		const float yawRate = seats[static_cast<size_t>(gunSeat)].yawSpeed;
		CHECK(yawRate > 0);
		for (int tick = 0; tick < 240; ++tick) {
			// The target starts at the idle scan's bearing, then moves through the
			// rear seam to the other side while the gun tracks it.
			const float angle = (tick < 180 ? trackStart + tick * ((trackEnd - trackStart) / 179.0f) : trackEnd);
			const float radians = angle * 0.0174532925f;
			g.world->setSoldierPosition(enemyInfo.soldier,
				{origin.x + 80.0f * std::sin(radians), 0, origin.z + 80.0f * std::cos(radians)});
			if (tick == 200) {
				g.world->setHitPoints(enemyInfo.soldier, 0);
				float handoffYaw = 0, handoffPitch = 0;
				g.world->gunAim(vehicle, gunSeat, handoffYaw, handoffPitch);
				const float hull = (g.objects.find(vehicle)->rotation().x + handoffYaw) * 0.0174532925f;
				const physics::Vec3 current = g.position(vehicle);
				g.world->setSoldierPosition(handoffInfo.soldier,
					{current.x + 45.0f * std::sin(hull), 0, current.z + 45.0f * std::cos(hull)});
			}
			stepAim();
			const game::SoldierInput command = bots.lastInputFor(bot);
			if (tick == 0) std::fprintf(stderr, "  AIMSETUP %s equipment=%s behaviour=%s target=%d\n", vehicleName,
				bots.equipment(bot).c_str(), bots.behaviour(bot).c_str(), bots.target(bot));
			maxCommand = std::max(maxCommand, std::fabs(command.turn));
			float yaw = 0, pitch = 0;
			const bool hasGun = g.world->gunAim(vehicle, gunSeat, yaw, pitch);
			CHECK(hasGun);
			const float step = havePrevious ? std::remainder(yaw - previousGunYaw, 360.0f) : 0.0f;
			if (havePrevious) maxGunStep = std::max(maxGunStep, std::fabs(step));
			previousGunYaw = yaw; havePrevious = true;
			if (tick == 199) {
				physics::Vec3 muzzle; physics::Mat3 frame;
				CHECK(g.world->gunFrame(vehicle, gunSeat, muzzle, frame));
				g.world->playerInfo(enemy, enemyInfo);
				const auto local = frame.untransform(enemyInfo.position - muzzle);
				const float desiredYaw = std::atan2(local.x, local.z) * 57.29578f;
				settledError = std::fabs(std::remainder(desiredYaw - yaw, 360.0f));
			}
			const float hullYaw = g.objects.find(vehicle)->rotation().x;
			if (std::fabs(angle) >= 160 && std::fabs(angle) <= 200) ++seamSamples;
			if (tick >= 200 && handoffSeenAt < 0 && bots.target(bot) == handoffEnemy) handoffSeenAt = tick;
			std::fprintf(stderr, "  AIMTRACE %s tick=%d target=%d desired=%7.3f cmd=%7.3f gun=%7.3f dgun=%7.3f hull=%7.3f\n",
				vehicleName, tick, bots.target(bot), std::remainder(angle, 360.0f), command.turn, yaw, step, hullYaw);
		}
		std::fprintf(stderr, "  AIMSUMMARY %s max gun delta %.3f deg/tick, max command %.3f deg, rate cap %.3f, handoff tick %d, seam samples %d\n",
			vehicleName, maxGunStep, maxCommand, yawRate / 30.0f, handoffSeenAt, seamSamples);
		std::fprintf(stderr, "  SETTLED %s yaw error %.3f degrees\n", vehicleName, settledError);
		CHECK(settledError < 5.0f);
		CHECK(seamSamples >= 5);
		CHECK(maxGunStep <= yawRate / 30.0f + 0.05f);
		CHECK(maxCommand <= yawRate / 30.0f + 0.05f);
		CHECK(handoffSeenAt >= 200 && handoffSeenAt <= 225);
	}
}

// The walker's own animation system in the simulation (0x0063d2f0 with
// 0x0063ce30): occupied and walking it plays stand_walk, whose effect
// events 1 and 2 (the feet: FootStepEffectRight 1024, FootStepEffectLeft
// 2048) and 3 and 4 (the walk cycle's sound, 12288) reach the object's
// tree; the walk's fade-in speed key points (0.2 to 1) slow the legs'
// drive at the start (0x008ba420 into 0x00650e90), and while a bundle with
// blockInput plays the legs hold their state (0x00650680).
TEST(Vehicles_WalkerRunsItsAnimationSystem)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	g.files.setMountingAllowed(true);
	std::string out;
	g.console.run("ClientArchives.con", {}, out);
	g.files.setMountingAllowed(false);
	anim::AnimationSystems systems([&g](const std::string& path, std::vector<uint8_t>& data) {
		return g.files.readFile(path, data);
	});
	anim::registerAnimationConsoleMethods(g.console, systems);
	g.console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, out);
	const auto load = [&](const std::string& path) {
		return systems.get(path, [&] { g.console.run(path, {}, out); });
	};
	const uint32_t mech = g.start("as_heavy_mech", {0, 4, 6});
	g.world->setAnimationSystems(&systems, load);
	g.run(1);
	const auto playing = [&](const char* name) {
		const std::vector<std::string> names = g.world->objectAnimation(mech);
		return std::find(names.begin(), names.end(), name) != names.end();
	};
	CHECK(playing("unmanned_slope")); // the parked pose while nobody sits in it
	CHECK(g.enter(mech));
	g.drive({}, 2);
	CHECK(playing("stand_slope"));
	game::SoldierInput input;
	input.throttle = 1;
	const physics::Vec3 rest = g.position(mech);
	uint32_t fired = 0;
	float firstSecond = 0;
	for (int i = 0; i < 90; ++i) {
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
		const auto events = g.world->takeObjectEffectEvents();
		const auto it = events.find(mech);
		fired |= it != events.end() ? it->second : 0;
		if (i == 29) {
			firstSecond = g.position(mech).z - rest.z;
		}
	}
	CHECK(playing("stand_walk"));
	const float walked = g.position(mech).z - rest.z;
	std::fprintf(stderr, "  as_heavy_mech animated: %.1f m in the first second, %.1f m in 3 s, effects %#x\n",
		firstSecond, walked, fired);
	CHECK((fired & 1024) && (fired & 2048) && (fired & 4096) && (fired & 8192));
	CHECK(walked > 10 && walked < 24);
	// Let go: the walk plays on to its stop point holding the legs, then
	// the walker stands again.
	g.drive({}, 3);
	CHECK(!playing("stand_walk") && playing("stand_slope"));
	const game::Vehicle* v = g.world->playerVehicleState();
	CHECK(v && v->legs() && v->legs()->state == 0 && v->legs()->messages == 0);
}

// boneIndex (0x0063d340 / 0x0063d5e0 / 0x0063d470): the walker's
// bundleSkeleton (as_heavy_mech.ske, root bones) posed by its animation
// system gives each part with a boneIndex its local matrix: parked
// (unmanned) the torso sits lower than standing; the AT's landing legs
// (eu_at, boneIndex 0..6) take their bones' places.
TEST(Vehicles_BundleSkeletonPosesBoneParts)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	g.files.setMountingAllowed(true);
	std::string out;
	g.console.run("ClientArchives.con", {}, out);
	g.files.setMountingAllowed(false);
	anim::AnimationSystems systems([&g](const std::string& path, std::vector<uint8_t>& data) {
		return g.files.readFile(path, data);
	});
	anim::registerAnimationConsoleMethods(g.console, systems);
	g.console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, out);
	const auto load = [&](const std::string& path) {
		return systems.get(path, [&] { g.console.run(path, {}, out); });
	};
	const uint32_t mech = g.start("as_heavy_mech", {0, 4, 6});
	g.world->setAnimationSystems(&systems, load);
	g.run(2);
	anim::Matrix parked {}, standing {};
	CHECK(g.world->partBone(mech, "AS_heavy_mech_torsospring", parked));
	CHECK(!g.world->partBone(mech, "AS_heavy_mech_turret", standing)); // no boneIndex
	CHECK(g.enter(mech));
	g.drive({}, 3);
	CHECK(g.world->partBone(mech, "AS_heavy_mech_torsospring", standing));
	std::fprintf(stderr, "  as_heavy_mech torso: parked y %.2f, standing y %.2f\n", parked[13], standing[13]);
	CHECK(standing[13] > parked[13] + 0.2f);
	const uint32_t at = g.start("eu_at", {0, 4, 6});
	g.world->setAnimationSystems(&systems, load);
	g.run(2);
	anim::Matrix leg {};
	CHECK(g.world->partBone(at, "EU_AT_FLG_Top", leg));
	std::fprintf(stderr, "  eu_at front leg: %.2f %.2f %.2f\n", leg[12], leg[13], leg[14]);
	CHECK(std::fabs(leg[14] - 2.674f) < 1.5f);
	// The Talon's engine pods are bone parts: their jets (e_jetHelo) go
	// where the bones put the pods, not the pods' template places.
	const uint32_t ag = g.start("us_ag", {0, 4, 6});
	g.world->setAnimationSystems(&systems, load);
	CHECK(g.enter(ag));
	g.drive({}, 2);
	anim::Matrix pod {};
	CHECK(g.world->partBone(ag, "US_AG_L_Engine_anim_obj", pod));
	const world::ObjectTemplate* agTemplate = g.templates.find("us_ag");
	CHECK(agTemplate != nullptr);
	if (agTemplate) {
		const effects::ObjectEffects::PartBone bone = [&g](uint32_t object, const std::string& part,
														  std::array<float, 16>& local) {
			return g.world->partBone(object, part, local);
		};
		const auto plain = effects::ObjectEffects::bundlesOf(g.templates, *agTemplate, ag);
		const auto boned = effects::ObjectEffects::bundlesOf(g.templates, *agTemplate, ag, {}, nullptr, bone);
		int jets = 0;
		for (size_t i = 0; i < boned.size() && i < plain.size(); ++i) {
			if (boned[i].bundle->name() != "e_jetHelo") {
				continue;
			}
			++jets;
			const physics::Vec3 a = boned[i].rotation.transform({0, 0, 1}), b = plain[i].rotation.transform({0, 0, 1});
			std::fprintf(stderr, "  us_ag jet %d: bone z %.2f %.2f %.2f, template z %.2f %.2f %.2f\n", jets, a.x, a.y,
				a.z, b.x, b.y, b.z);
			CHECK(physics::lengthSquared(a - b) > 1e-4f || physics::lengthSquared(boned[i].position - plain[i].position) > 1e-4f);
		}
		CHECK(jets == 2);
	}
}

// The legs' object pass (0x006cec60): a contact under the legs' half-radius
// line is ground for the drive, its normal joining the drive's by axis
// (0x006b6b60: the larger of one sign, the sum of opposite signs).
TEST(Vehicles_WalkerLegsSphereContacts)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const world::ObjectTemplate* t = g.templates.find("as_heavy_mech");
	CHECK(t != nullptr);
	if (!t) {
		return;
	}
	std::unique_ptr<game::Vehicle> v = game::Vehicle::fromTemplate(*t, g.templates, nullptr);
	CHECK(v && v->legs());
	if (!v || !v->legs()) {
		return;
	}
	CHECK(near(v->legs()->length, 2.8f) && near(v->legs()->stepHeight, 1.4f) && near(v->legs()->radius, 1.5f));
	v->legsObjectContact({0.6f, 0.8f, 0}, {0, 1, 0}, false);
	CHECK(!v->legs()->landed && near(v->legs()->contactNormal.x, 0.6f) && near(v->legs()->contactNormal.y, 1));
	v->legsObjectContact({-0.2f, 0.9f, 0.3f}, {0, 0.95f, 0.1f}, true);
	CHECK(near(v->legs()->contactNormal.x, 0.4f) && near(v->legs()->contactNormal.y, 1)
		&& near(v->legs()->contactNormal.z, 0.3f));
	CHECK(v->legs()->landed && v->legs()->grounded && near(v->legs()->bestNormal.y, 0.95f));
}

// The transport (as_at: the rotor's collective on PIThrottle, the PCO's
// vertical regulator holding it when the throttle is let go): W takes it up,
// let go it hangs with little vertical speed.
TEST(Vehicles_TransportTakesOffAndHangs)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t at = g.start("as_at", {0, 3, 8});
	const physics::Vec3 rest = g.position(at);
	CHECK(g.enter(at));
	const game::Vehicle* v = g.world->playerVehicleState();
	CHECK(v && v->flies());
	game::SoldierInput input;
	input.throttle = 1;
	g.drive(input, 6);
	const physics::Vec3 up = g.position(at);
	std::fprintf(stderr, "  as_at: %.1f m up after 6 s\n", up.y - rest.y);
	CHECK(up.y - rest.y > 5);
	g.drive({}, 3);
	const physics::Vec3 a = g.position(at);
	g.drive({}, 2);
	const physics::Vec3 b = g.position(at);
	std::fprintf(stderr, "  as_at: let go, %.2f m/s vertically\n", (b.y - a.y) / 2);
	CHECK(std::fabs(b.y - a.y) / 2 < 3);
	CHECK(b.y > rest.y + 3);
}

// The gunship (as_ag: its take-off engine's lift, its forward engine on
// PIThrottle, the rotor): it climbs off the ground and flies forward.
TEST(Vehicles_GunshipFlies)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t ag = g.start("as_ag", {0, 3, 8});
	const physics::Vec3 rest = g.position(ag);
	CHECK(g.enter(ag));
	const game::Vehicle* v = g.world->playerVehicleState();
	CHECK(v && v->flies());
	game::SoldierInput input;
	input.throttle = 1;
	g.drive(input, 5);
	const physics::Vec3 p = g.position(ag);
	std::fprintf(stderr, "  as_ag: %.1f m up, %.1f m ahead after 5 s\n", p.y - rest.y, p.z - rest.z);
	CHECK(p.y - rest.y > 5);
	CHECK(p.z - rest.z > 10);
}

TEST(Vehicles_AircraftHudThrustFromPilot)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* name : {"as_ag", "us_ag", "as_at", "eu_at"}) {
			Garage g(gameDir(), mod);
			CHECK(g.loaded && g.templates.find(name));
			const uint32_t id = g.start(name, {0, 3, 8});
			CHECK(g.enter(id));
			game::GameLogic logic;
			host::LocalHudWorld world(*g.world, logic);
			menu::VehicleGuiInput gui;
			gui.controlled = g.templates.find(name);
			gui.weapon = world.seatGroupWeapon(0).weapon;
			gui.second = world.seatGroupWeapon(1).weapon;
			gui.cameraMode = 1; // Inside, as in the pilot HUD
			const int index = menu::vehicleGuiIndex(gui);
			CHECK(index == 7 || index == 15 || index == 64 || index == 66);
			game::SoldierInput input;
			input.throttle = 1;
			g.drive(input, 0.1f);
			CHECK(menu::aircraftHudThrust(index, world.pilotThrust(id)) > 0.9f);
			input.throttle = -1;
			g.drive(input, 0.1f);
			CHECK(menu::aircraftHudThrust(index, world.pilotThrust(id)) < -0.9f);
		}
	}
}

// A gunship's pilot pitches up and down at comparable rates (the report:
// "can pitch down, can't pitch up"): from a hover, held pitch keys and a
// sustained mouse stream each way.
TEST(Vehicles_GunshipPitchesBothWays)
{
	if (!gameDir()) {
		return;
	}
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* name : {"as_ag", "us_ag"}) {
			for (int mode = 0; mode < 4; ++mode) {
				Garage g(gameDir(), mod);
				if (!g.loaded || !g.templates.find(name)) {
					continue;
				}
				const uint32_t ag = g.start(name, {0, 3, 8});
				if (!g.enter(ag)) {
					continue;
				}
				game::SoldierInput lift;
				lift.throttle = 1;
				g.drive(lift, 4);
				g.drive({}, 3);
				// mode 0/1: ArrowDown (-1, nose up) / ArrowUp (+1); 2/3: mouse up / down.
				const float dir = mode % 2 == 0 ? 1.0f : -1.0f; // +1 = nose up
				float aim = 0;
				float peak = 0, peakRate = 0;
				const auto pitchOf = [&] {
					for (const auto& info : g.world->vehicleList()) {
						if (info.object == ag) {
							return std::pair<float, float> {std::asin(std::clamp(info.forward.y, -1.0f, 1.0f)) * 57.29578f,
								info.angularVelocity.x * info.right.x + info.angularVelocity.y * info.right.y + info.angularVelocity.z * info.right.z};
						}
					}
					return std::pair<float, float> {};
				};
				const float start = pitchOf().first;
				for (int i = 0; i < 60; ++i) {
					game::SoldierInput in;
					if (mode < 2) {
						in.pitchAxis = -dir;
					} else {
						aim += dir * 4.0f; // degrees a tick, up positive
						in.aimPitch = aim;
					}
					g.world->setPlayerInput(in);
					g.world->tick(1 / 30.0f);
					const auto [angle, rate] = pitchOf();
					if (std::fabs(angle - start) > std::fabs(peak)) peak = angle - start;
					peakRate = std::max(peakRate, std::fabs(rate));
					if (i % 10 == 9) {
						std::fprintf(stderr, "    %s %s mode %d t=%.1f pitch %.2f rate %.3f\n", mod, name, mode, (i + 1) / 30.0f, angle - start, rate);
					}
				}
				std::fprintf(stderr, "  %s %s mode %d: peak %.2f deg, peak rate %.3f rad/s\n", mod, name, mode, peak, peakRate);
				CHECK(peak * dir > 3);
			}
		}
	}
}

// The remote path (client -> PlayerMove -> server/prediction): a driver's
// PIPitch is the mouse's change, so a pilot who keeps pulling the mouse up
// (the hull fights back) must not run out of pitch input. The server's
// clamped aim pitch (+-89) swallowed every further pull up while pulls down
// still worked: "can pitch down, can't pitch up".
TEST(Vehicles_RemoteMouseKeepsPitchingUp)
{
	for (const float dir : {1.0f, -1.0f}) { // +1: mouse up (nose up)
		net::MoveInputState state;
		float aim = 0, last = 0, lastLook = 0, early = 0, late = 0;
		for (int i = 0; i < 120; ++i) {
			game::SoldierInput in;
			in.aimPitch = aim += dir * 2.0f;
			const game::SoldierInput seen = net::soldierInputFromMove(net::playerInputFrom(in, last).toMove(i), state);
			last = in.aimPitch;
			const float driven = game::GameWorld::driverInput(seen, 1, lastLook).pitch;
			lastLook = seen.aimPitch;
			(i == 5 ? early : late) = -driven * dir; // nose-up positive in the direction pulled
		}
		std::fprintf(stderr, "  remote mouse stream dir %+.0f: pitch input early %.3f, after 120 ticks %.3f\n", dir, early, late);
		CHECK(late > 0.9f * early && early > 0.2f);
	}
}

// A recoil graph with a large exponent (eu_sni's) stays finite: the
// curve's powers over their sum taken as (i / n)^e (pow(75, e) overflowed
// to inf, and inf x 0 made the bots' and soldiers' turns NaN).
TEST(Recoil_GraphWithALargeExponentStaysFinite)
{
	anim::RecoilSettings recoil;
	recoil.graphFrameCount = 75;
	recoil.graphExponent = 30;
	recoil.graphTotalMovement = 4;
	const std::vector<float> graph = recoil.graph();
	float sum = 0;
	for (const float v : graph) {
		CHECK(std::isfinite(v));
		sum += v;
	}
	recoil.goBackOnRecoil = false;
	float plain = 0;
	for (const float v : recoil.graph()) {
		plain += v;
	}
	CHECK(std::fabs(plain + 4) < 1e-3f);
	recoil.graphExponent = 2;
	recoil.graphFrameCount = 8;
	recoil.graphTotalMovement = 2;
	const std::vector<float> small = recoil.graph();
	// The same curve as before for ordinary settings: 64 / 204 of -2 last.
	CHECK(std::fabs(small[7] + 2.0f * 64.0f / 204.0f) < 1e-5f);
}

namespace {

// The bots' drivers (ai::driveControl: TankMoveTo, MechMoveTo) at the
// wheel of the walker and the tracked tank, as BotManager::drive feeds
// them: towards a goal 60 m off to the right ahead; how near they get.
float botDrives(Garage& g, uint32_t vehicle, ai::Driver driver, float maxSpeed, const physics::Vec3& goal, float seconds)
{
	ai::DriveSettings settings;
	settings.driver = driver;
	settings.maxSpeed = maxSpeed;
	settings.yawScale = 15;
	ai::DriveState state;
	float time = 0;
	for (int i = 0; i < static_cast<int>(seconds * 30); ++i) {
		time += 1 / 30.0f;
		const world::Object* o = g.objects.find(vehicle);
		const physics::Vec3 at {o->position().x, o->position().y, o->position().z};
		const float yaw = o->rotation().x * 3.14159265f / 180.0f;
		ai::DriveSense sense;
		sense.now = time;
		sense.forward = {std::sin(yaw), 0, std::cos(yaw)};
		sense.right = {std::cos(yaw), 0, -std::sin(yaw)};
		sense.position = {at.x, at.y, at.z};
		physics::Vec3 velocity;
		for (const auto& info : g.world->vehicleList()) {
			if (info.object == vehicle) {
				velocity = info.velocity;
			}
		}
		sense.velocity = {velocity.x, velocity.y, velocity.z};
		sense.speed = velocity.x * sense.forward.x + velocity.z * sense.forward.z;
		sense.goal = {goal.x, goal.y, goal.z};
		sense.goalDistance = std::sqrt((goal.x - at.x) * (goal.x - at.x) + (goal.z - at.z) * (goal.z - at.z));
		sense.remaining = sense.goalDistance;
		sense.error = std::remainder(std::atan2(goal.x - at.x, goal.z - at.z) - yaw, 6.2831853f);
		if (sense.goalDistance < 6) {
			return sense.goalDistance;
		}
		const ai::DriveOutput out = ai::driveControl(settings, state, sense);
		game::SoldierInput input;
		input.throttle = out.throttle;
		input.strafe = out.turn;
		input.sprint = out.sprint;
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
	}
	const physics::Vec3 at = g.position(vehicle);
	return std::sqrt((goal.x - at.x) * (goal.x - at.x) + (goal.z - at.z) * (goal.z - at.z));
}

} // namespace

TEST(Vehicles_BotsDriveTheWalkerAndTheTank)
{
	if (!gameDir()) {
		return;
	}
	for (const char* name : {"us_heavy_mech", "eu_tank"}) {
		Garage g(gameDir());
		if (!g.loaded) {
			return;
		}
		const uint32_t v = g.start(name, {0, 4, 6});
		CHECK(g.enter(v));
		const physics::Vec3 goal {45, 0, 50};
		const float left = botDrives(g, v, std::string(name) == "eu_tank" ? ai::Driver::Tank : ai::Driver::Mech,
			std::string(name) == "eu_tank" ? 13.0f : 8.0f, goal, 25);
		std::fprintf(stderr, "  %s driven by the bots' controller: %.1f m from the goal\n", name, left);
		CHECK(left < 8);
	}
}

// The renderer's turning parts: the transport's moving parts (its rotor
// Engine and RotationalBundles) report their axes' angles through partPose,
// so the drawn rotor spins as the vehicle's state has it.
TEST(Vehicles_PartPoseTurnsTheMovingParts)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t at = g.start("as_at", {0, 3, 8});
	CHECK(g.enter(at));
	game::SoldierInput input;
	input.throttle = 1;
	g.drive(input, 1.3f);
	const game::Vehicle* v = g.world->playerVehicleState();
	CHECK(v != nullptr);
	if (!v) {
		return;
	}
	int turned = 0;
	for (const game::Vehicle::Part& part : v->parts()) {
		world::Vec3 offset, rotation;
		if (!part.moves || part.angle == std::array<float, 3> {} || !g.world->partPose(at, part.name, offset, rotation)) {
			continue;
		}
		// A gunner's bundle by its seat comes first; otherwise the part's angles.
		if (rotation.x == part.angle[0] && rotation.y == part.angle[1] && rotation.z == part.angle[2]) {
			++turned;
		}
	}
	std::fprintf(stderr, "  as_at: %d moving parts turned\n", turned);
	CHECK(turned > 0);
}

// The *_NP APCs' floats (FloatingBundle, node 0x006ca860): over 30 m of
// water eu_apc_NP rides on its four floats near the surface; eu_apc,
// without them, lies lower, held by its hull's buoyancy alone (floaterMod,
// 0x006b9260).
TEST(Vehicles_FloatingBundlesHoldTheApcUp)
{
	if (!gameDir()) {
		return;
	}
	float heights[2] = {};
	for (int np = 0; np < 2; ++np) {
		Garage g(gameDir());
		if (!g.loaded) {
			return;
		}
		const uint32_t apc = g.start(np ? "eu_apc_NP" : "eu_apc", {0, 2, 6}, 10, -30, 0);
		const game::Vehicle* v = g.world->vehicle(apc);
		CHECK(v != nullptr);
		if (v) {
			CHECK(v->floaters().size() == (np ? 4u : 0u));
		}
		heights[np] = g.position(apc).y;
	}
	std::fprintf(stderr, "  after 10 s over 30 m of water: eu_apc at %.2f, eu_apc_NP at %.2f\n", heights[0], heights[1]);
	CHECK(heights[1] > heights[0] + 1.5f);
	CHECK(heights[1] > -2 && heights[1] < 3);
}

// regulateVector (0x005644b0, 0x00565180): eu_tank's cupola gun bundle has
// -0.5 on its yaw (maxSpeed 90): with the hull turning at 0.56 rad/s the
// regulated input is -0.28 (25 degrees a second against the hull), which
// replaces a zero or weaker same-way input.
TEST(Vehicles_TurretRegulationCounterTurns)
{
	physics::RigidBody body;
	body.angularVelocity = {0, 0.56f, 0};
	const float reg = game::GameWorld::regulation(-0.5f, body);
	CHECK(std::fabs(reg + 0.28f) < 1e-4f);
	CHECK(game::GameWorld::regulatedInput(0, reg) == reg);
	CHECK(game::GameWorld::regulatedInput(-0.1f, reg) == reg);
	CHECK(game::GameWorld::regulatedInput(-0.5f, reg) == -0.5f);
	CHECK(game::GameWorld::regulatedInput(0.3f, reg) == 0.3f);
	body.angularVelocity = {0, 10, 0};
	CHECK(game::GameWorld::regulation(-0.5f, body) == -1.0f);
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t tank = g.start("eu_tank", {0, 2, 6}, 1);
	bool found = false;
	for (const auto& seat : g.world->vehicleSeats(tank)) {
		if (seat.regulateYaw != 0) {
			std::fprintf(stderr, "  eu_tank %s: regulateVector.x %.2f, yaw speed %.0f\n", seat.yawBundle.c_str(),
				seat.regulateYaw, seat.yawSpeed);
			found = found || (seat.regulateYaw == -0.5f && seat.yawSpeed == 90);
		}
	}
	CHECK(found);
}

// LandingGear (0x00650120): eu_at's gear (down under 10 m and 10 m/s, up
// from 20 m and 20 m/s) starts down, goes up flying high and fast and down
// again low and slow. The shipped gears have no axes (only their sound
// reads the state).
TEST(Vehicles_LandingGearGoesUpAndDown)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const world::ObjectTemplate* at = g.templates.find("eu_at");
	CHECK(at != nullptr);
	if (!at) {
		return;
	}
	std::unique_ptr<game::Vehicle> v = game::Vehicle::fromTemplate(*at, g.templates, nullptr);
	CHECK(v && v->gears().size() == 1);
	if (!v || v->gears().empty()) {
		return;
	}
	const game::Vehicle::GearPart& gear = v->gears()[0];
	CHECK(gear.downHeight == 10 && gear.upHeight == 20 && gear.downSpeed == 10 && gear.upSpeed == 20);
	physics::Ground ground {[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& n) {
								n = {0, 1, 0};
								return 0.0f;
							}};
	physics::RigidBody body;
	body.position = {0, 2, 0};
	v->tick(body, ground, 1 / 30.0f, 0.8f);
	CHECK(!gear.up);
	body.position = {0, 15, 0}; // the ray (20 m) finds the ground, not up yet
	body.velocity = {0, 0, 30};
	v->tick(body, ground, 1 / 30.0f, 0.8f);
	CHECK(!gear.up);
	body.position = {0, 40, 0};
	v->tick(body, ground, 1 / 30.0f, 0.8f);
	CHECK(gear.up);
	body.position = {0, 5, 0};
	v->tick(body, ground, 1 / 30.0f, 0.8f);
	CHECK(gear.up); // still fast
	body.velocity = {0, 0, 5};
	v->tick(body, ground, 1 / 30.0f, 0.8f);
	CHECK(!gear.up);
}

namespace {

// The pilots' controller (src/AI/Heli: HeliMoveTo 0x100e50b0) flying the
// player's aircraft to `goal` at `margin` over the ground, the inputs
// mapped as the bots map them (PIThrottle, PIYaw strafe, PIRoll the turn x
// 5, PIPitch direct). Returns the closest it came (3-D) and the end.
struct Flight {
	float closest = 1e9f;
	physics::Vec3 end;
	float topSpeed = 0;
	float lowest = 1e9f; // the least height over the ground after take-off
};
Flight pilotFlies(Garage& g, uint32_t vehicle, const physics::Vec3& goal, float margin, float seconds)
{
	ai::HeliState state;
	Flight flight;
	physics::Vec3 last;
	for (int i = 0; i < static_cast<int>(seconds * 30); ++i) {
		game::GameWorld::VehicleInfo me {};
		for (const auto& info : g.world->vehicleList()) {
			if (info.object == vehicle) {
				me = info;
			}
		}
		ai::HeliSense sense;
		sense.position = {me.position.x, me.position.y, me.position.z};
		sense.velocity = {me.velocity.x, me.velocity.y, me.velocity.z};
		sense.angularVelocity = {me.angularVelocity.x, me.angularVelocity.y, me.angularVelocity.z};
		sense.forward = {me.forward.x, me.forward.y, me.forward.z};
		sense.right = {me.right.x, me.right.y, me.right.z};
		const physics::Vec3& f = me.forward;
		const physics::Vec3& r = me.right;
		sense.up = {f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x};
		const physics::Vec3 a = i > 0 ? (me.velocity - last) * 30.0f : physics::Vec3 {};
		sense.acceleration = {a.x, a.y, a.z};
		last = me.velocity;
		sense.terrain = [](float, float) { return 0.0f; };
		ai::HeliPilot pilot(state, sense);
		pilot.flyTo({goal.x, goal.y, goal.z}, margin);
		game::SoldierInput input;
		input.throttle = state.controls.throttle;
		input.strafe = state.controls.yaw;
		input.turn = state.controls.roll * 5;
		input.hasPitch = true;
		input.pitch = state.controls.pitch;
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
		const physics::Vec3 at = g.position(vehicle);
		flight.closest = std::min(flight.closest, std::sqrt(physics::lengthSquared(at - goal)));
		flight.topSpeed = std::max(flight.topSpeed, std::sqrt(physics::lengthSquared(me.velocity)));
		if (state.airborne) {
			flight.lowest = std::min(flight.lowest, at.y);
		}
	}
	flight.end = g.position(vehicle);
	return flight;
}

} // namespace

// The bots' flight (src/AI/Heli, AIDLL 0x100e50b0 and its primitives):
// the transport (as_at, eu_at) and the gunships (as_ag, us_ag) take off
// and fly 150 m to a point 25 m over the ground. The gunships stop and hover
// on it; the heavier transports overshoot (from 22 m/s, braking only within
// 20 m) and wander round it within some 35 m on the rewrite's flight model.
TEST(Vehicles_BotPilotsFlyToAPointAndHover)
{
	if (!gameDir()) {
		return;
	}
	for (const char* name : {"as_at", "eu_at", "as_ag", "us_ag"}) {
		Garage g(gameDir());
		if (!g.loaded) {
			return;
		}
		const uint32_t v = g.start(name, {0, 4, 6});
		CHECK(g.enter(v));
		const physics::Vec3 goal {100, 25, 120};
		const Flight flight = pilotFlies(g, v, goal, 25, 45);
		const float off = std::sqrt(physics::lengthSquared(flight.end - goal));
		std::fprintf(stderr, "  %s: closest %.1f m, %.1f m off after 45 s (at %.1f, %.1f, %.1f), top speed %.1f m/s\n",
			name, flight.closest, off, flight.end.x, flight.end.y, flight.end.z, flight.topSpeed);
		CHECK(flight.closest < 10);
		CHECK(off < (name[3] == 'a' && name[4] == 'g' ? 5.0f : 45.0f));
		CHECK(flight.end.y > 12 && flight.end.y < 40);
	}
}

// Spawning into a vehicle: the APC carries a SpawnPoint with
// setEnterOnSpawn 1 (eu_apc_SpawnPoint): chosen as a mobile spawn point,
// the soldier appears in its nearest free seat (0x005a98e0). A spawn
// point with setEnterOnSpawn on the ground beside it does the same; with
// no vehicle near, the point can't be used.
TEST(Vehicles_SpawnIntoTheApc)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("eu_apc_pods")) {
		return;
	}
	const uint32_t apc = g.start("eu_apc_pods", {0, 2, 30}, 2);
	CHECK(g.world->spawnVehicles(2) == std::vector<uint32_t> {apc});
	CHECK(g.world->spawnVehicles(1).empty());
	// A mobile spawn point needs a control point of its team (0x005b24f0):
	// with the garage's point lost, the APC is no spawn.
	g.world->setControlPointTeam(2, 1);
	CHECK(g.world->spawnVehicles(2).empty());
	const int early = g.world->addPlayer("Early", 2, false);
	g.world->setSpawnVehicle(early, apc);
	game::GameWorld::PlayerInfo earlyInfo;
	CHECK(!g.world->spawnSoldier(early) || (g.world->playerInfo(early, earlyInfo) && earlyInfo.vehicle != apc));
	g.world->setControlPointTeam(2, 2);
	const int me = g.world->addPlayer("Me", 2, false);
	g.world->setSpawnVehicle(me, apc);
	CHECK(g.world->spawnSoldier(me));
	game::GameWorld::PlayerInfo info;
	CHECK(g.world->playerInfo(me, info) && info.vehicle == apc && info.seat == 0);

	// The garage's ground point with setEnterOnSpawn: the APC is 30 m away
	// (out of 5 m), so a human can't spawn there...
	world::ObjectTemplate* sp = g.templates.find("garage_sp");
	sp->settings()["setEnterOnSpawn"] = "1";
	const int bot = g.world->addPlayer("Bot", 2, false);
	CHECK(!g.world->spawnSoldier(bot));
	// ...until it stands beside the APC.
	for (const auto& object : g.objects.objects()) {
		if (object->objectTemplate() == sp) {
			object->setPosition({3, 0, 30});
		}
	}
	CHECK(g.world->spawnSoldier(bot));
	CHECK(g.world->playerInfo(bot, info) && info.vehicle == apc && info.seat > 0);
	// setAIEnterOnSpawn alone seats only bots (0x005b18e0: the template's
	// +0x8a for an AI): a human spawns there standing.
	sp->settings()["setEnterOnSpawn"] = "0";
	sp->settings()["setAIEnterOnSpawn"] = "1";
	const int walker = g.world->addPlayer("Walker", 2, false);
	CHECK(g.world->spawnSoldier(walker));
	CHECK(g.world->playerInfo(walker, info) && info.vehicle == 0 && info.alive);
	const int rider = g.world->addPlayer("Rider", 2, true);
	CHECK(g.world->spawnSoldier(rider));
	CHECK(g.world->playerInfo(rider, info) && info.vehicle == apc);
}

// The gunship's TV-guided missile (as_ag's GuidedMissileWeapon: ToggleCameraComp
// zoom.useProjectileCamera 1, AAGuided_missile useProjectileCamera 1 with
// AAGuided_missile_Camera takeOverInput 1, wire guided with
// useCameraDirectionToTarget): fired, the round's camera is the gunner's
// view (0x005b38a0, the round's +0x240) and takes the input (0x005ca2e0):
// the mouse turns the camera within 18 degrees (snapping back without
// input) and the guidance (0x005e2e20, the active camera) turns the
// missile after it; the seat's gun doesn't turn. When the round goes the
// view is the seat's again (0x005b4b00).
TEST(Vehicles_GunshipMissileCamera)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_ag")) {
		return;
	}
	g.pitAhead();
	const uint32_t v = g.start("as_ag", {0, 4, 6});
	CHECK(g.enter(v));
	const int me = g.world->localPlayer();
	const auto seats = g.world->vehicleSeats(v);
	// The gunner's seat: the cannon (itemIndex 1) and the
	// GuidedMissileWeapon (itemIndex 2), both isSelectable 0, in the PIFire
	// group; the seat's specialToggleWeaponInput PIAltFire moves to the
	// next by item index (0x005c7f80, 0x004d2a50).
	const int seat = 1;
	CHECK(seats.size() == 2);
	if (seats.size() != 2) {
		return;
	}
	const auto lowerName = [](std::string n) {
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return n;
	};
	CHECK(seats[1].weapons.size() == 2);
	CHECK(lowerName(seats[1].weapon) == "as_ag_gunnercannon");
	CHECK(seats[1].specialToggleInput == 31);
	game::SoldierInput input;
	input.switchSeat = seat + 1;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(4); // past the draw and the first reload
	game::GameWorld::PlayerInfo info;
	CHECK(g.world->playerInfo(me, info) && info.seat == seat);
	CHECK(lowerName(g.world->vehicleSeats(v)[1].weapon) == "as_ag_gunnercannon");
	// PIAltFire: the missile; held, it fires nothing (0x005c69a0).
	input = {};
	input.altFire = true;
	for (int i = 0; i < 3; ++i) {
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
	}
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	CHECK(lowerName(g.world->vehicleSeats(v)[1].weapon) == "as_ag_guidedmissileweapon");
	CHECK(g.world->vehicleSeats(v)[1].startCameraId == 11);
	g.run(4); // its delay and reload
	game::GameWorld::ProjectileCamera camera;
	CHECK(!g.world->projectileCamera(me, camera));
	// The TVEffect (RendDX9 0x10048f80) only through a round's camera.
	render::PostEffects effects;
	host::roundCameraEffects(*g.world, me, effects);
	CHECK(!effects.tv);
	// High above the ground, as a gunship flies: the missile, launched at
	// rest, dives under its engine (acceleration 35) where the camera looks.
	g.groundHeight = -500;
	input = {};
	input.fire = true;
	for (int i = 0; i < 4 && !g.world->projectileCamera(me, camera); ++i) {
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
	}
	CHECK(g.world->projectileCamera(me, camera) && camera.takeOverInput);
	host::roundCameraEffects(*g.world, me, effects);
	CHECK(effects.tv);
	// Launched at rest along the active camera's facing (fireInCameraDof,
	// 0x005f1a20: zoom.startCameraId 11, as_ag_TAD_GuidedMissile_Camera,
	// set 10 degrees down under the seat), its nose that way.
	{
		const world::Object* ship = g.objects.find(v);
		const physics::Mat3 hull =
			physics::Mat3::fromYawPitchRoll({ship->rotation().x, ship->rotation().y, ship->rotation().z});
		physics::Vec3 nose;
		for (const auto& p : g.world->projectiles()) {
			if (p.player == me) {
				nose = game::noseOf(p);
			}
		}
		const physics::Vec3 local = hull.untransform(nose);
		std::fprintf(stderr, "  the missile's nose in the gunship's frame (%.3f, %.3f, %.3f)\n", local.x, local.y, local.z);
		CHECK(std::fabs(local.y + std::sin(10 / 57.29578f)) < 0.05f && local.z > 0.95f);
	}
	const float startYaw = camera.yaw;
	// The mouse to the right for a second: the camera turns at most 18
	// degrees off the round, and the round comes round after it.
	input = {};
	input.turn = 3;
	for (int i = 0; i < 30; ++i) {
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
	}
	CHECK(g.world->projectileCamera(me, camera));
	const auto& rounds = g.world->projectiles();
	float roundYaw = 0;
	for (const auto& p : rounds) {
		if (p.player == me) {
			roundYaw = std::atan2(p.velocity.x, p.velocity.z) * 57.29578f;
		}
	}
	std::fprintf(stderr, "  camera yaw %.1f -> %.1f, the round's %.1f\n", startYaw, camera.yaw, roundYaw);
	CHECK(std::remainder(camera.yaw - roundYaw, 360.0f) > 0 && std::remainder(camera.yaw - roundYaw, 360.0f) <= 18.01f);
	CHECK(std::remainder(roundYaw - startYaw, 360.0f) > 1);
	// No input: recenter at the camera's configured speed without a jump.
	const float beforeReturn = camera.yaw;
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	CHECK(g.world->projectileCamera(me, camera));
	for (const auto& p : g.world->projectiles()) {
		if (p.player == me) {
			roundYaw = std::atan2(p.velocity.x, p.velocity.z) * 57.29578f;
		}
	}
	const float offset = std::fabs(std::remainder(camera.yaw - roundYaw, 360.0f));
	CHECK(offset > 1 && offset < 18);
	CHECK(std::fabs(std::remainder(camera.yaw - beforeReturn, 360.0f)) < 5);
	// Aim down, then stop moving the mouse while retaining its absolute
	// input pitch. Recentring must not jump the view vertically either.
	input = {};
	for (int i = 0; i < 10; ++i) {
		input.aimPitch -= 2.0f;
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
	}
	CHECK(g.world->projectileCamera(me, camera));
	const float downPitch = camera.pitch;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	CHECK(g.world->projectileCamera(me, camera));
	CHECK(std::fabs(camera.pitch - downPitch) < 5.0f);
	// The round's life (5 s) runs out: the seat's camera again.
	g.run(6);
	CHECK(!g.world->projectileCamera(me, camera));
	// Steering the round must not leave the gunner's stored look pitched
	// down after returning to the seat. The viewer consumes this correction.
	const float restore = g.world->takeAimKick(me);
	CHECK(std::fabs(restore - 20.0f) < 0.1f);
	input.aimPitch += restore;
	input.altFire = true;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	input.altFire = false;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	float restoredYaw = 0, restoredPitch = 0;
	CHECK(g.world->gunAim(v, seat, restoredYaw, restoredPitch));
	CHECK(std::fabs(restoredPitch) < 1.0f);
}

// Exercise the shipped zero-muzzle-speed TV missile in both gunships and mods.
TEST(Vehicles_GunshipMissileMotorAndCamera)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* aircraft : {"as_ag", "us_ag"}) {
			Garage g(gameDir(), mod);
			CHECK(g.loaded && g.templates.find(aircraft));
			if (!g.loaded || !g.templates.find(aircraft)) continue;
			g.pitAhead();
			const uint32_t v = g.start(aircraft, {0, 4, 6});
			CHECK(g.enter(v));
			game::SoldierInput in;
			in.switchSeat = 2;
			g.world->setPlayerInput(in);
			g.world->tick(1 / 30.0f);
			g.world->setPlayerInput({});
			g.run(4);
			in = {}; in.altFire = true;
			g.world->setPlayerInput(in);
			g.world->tick(1 / 30.0f);
			g.world->setPlayerInput({});
			g.run(4);
			anim::AnimationSystems systems(nullptr);
			PlayerCamera playerCamera(*g.world, systems, nullptr);
			playerCamera.tick(1 / 30.0f);
			const auto selectedMode = playerCamera.mode();
			playerCamera.toggleMode();
			CHECK(playerCamera.mode() == selectedMode);
			CHECK(!playerCamera.selectView(2));
			in = {}; in.fire = true;
			game::GameWorld::ProjectileCamera camera;
			for (int i = 0; i < 12 && !g.world->projectileCamera(g.world->localPlayer(), camera); ++i) {
				g.world->setPlayerInput(in);
				g.world->tick(1 / 30.0f);
			}
			CHECK(g.world->projectileCamera(g.world->localPlayer(), camera));
			if (!g.world->projectileCamera(g.world->localPlayer(), camera)) continue;
			const uint32_t serial = camera.serial;
			CHECK(serial != 0 && camera.lifeRemaining > 0.9f && camera.rangeProgress < 0.05f);
			playerCamera.tick(1 / 30.0f);
			const auto mode = playerCamera.mode();
			playerCamera.toggleMode();
			CHECK(playerCamera.mode() == mode);
			CHECK(!playerCamera.selectView(2) && playerCamera.mode() == mode);
			const auto visible = game::projectileRenderStates(g.templates, g.world->projectiles());
			const auto hidden = game::projectileRenderStates(g.templates, g.world->projectiles(), serial);
			CHECK(visible.size() == hidden.size() + 1);
			g.world->setPlayerInput({});
			g.run(0.5f);
			CHECK(g.world->projectileCamera(g.world->localPlayer(), camera));
			CHECK(camera.lifeRemaining < 0.95f && camera.lifeRemaining > 0.7f);
			CHECK(camera.rangeProgress > 0 && camera.rangeProgress < 1.0f - camera.lifeRemaining);
			bool found = false;
			for (const auto& p : g.world->projectiles()) if (p.serial == serial && p.alive) {
				found = true;
				CHECK(physics::lengthSquared(p.velocity) > 100.0f);
				CHECK(physics::lengthSquared(p.position - p.origin) > 1.0f);
			}
			CHECK(found);
		}
	}
}

// A short-lived round simulates an immediate impact: it must not bypass
// the actual launcher's authored reload minimum.
TEST(Vehicles_GunshipMissileEarlyImpactReload)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* aircraft : {"as_ag", "us_ag"}) {
			Garage g(gameDir(), mod);
			auto* missile = g.templates.find("AAGuided_missile");
			CHECK(g.loaded && missile && g.templates.find(aircraft));
			if (!g.loaded || !missile || !g.templates.find(aircraft)) continue;
			missile->settings()["timeToLive"] = "CRD_NONE/0.4/0/0";
			const uint32_t v = g.start(aircraft, {0, 4, 6});
			CHECK(g.enter(v));
			game::SoldierInput in; in.switchSeat = 2;
			g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			g.world->setPlayerInput({}); g.run(4);
			in = {}; in.altFire = true;
			g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			g.world->setPlayerInput({}); g.run(4);
			const game::FireArm* launcher = g.world->gun(v, 1);
			CHECK(launcher && launcher->settings().minimumReloadTicks == 90 && launcher->settings().reloadTime == 2.0f);
			if (!launcher) continue;
			in = {}; in.fire = true;
			g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			CHECK(launcher->roundsInMag() == 0 && !launcher->reloading());
			g.world->setPlayerInput({}); g.run(0.6f);
			game::GameWorld::ProjectileCamera view;
			CHECK(!g.world->projectileCamera(g.world->localPlayer(), view));
			in.fire = true;
			for (int i = 0; i < 40; ++i) { g.world->setPlayerInput(in); g.world->tick(1 / 30.0f); }
			CHECK(!g.world->projectileCamera(g.world->localPlayer(), view));
			CHECK(!launcher->reloading());
			g.world->setPlayerInput({}); g.run(1.2f);
			CHECK(launcher->reloading() && launcher->roundsInMag() == 0);
			g.run(2.0f);
			CHECK(!launcher->reloading() && launcher->roundsInMag() == 1);
			in.fire = true; g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			CHECK(g.world->projectileCamera(g.world->localPlayer(), view));
		}
	}
}

// The missile starts with zero authored muzzle speed. A moving carrier
// must not make the TV view face sideways or backwards at launch.
TEST(Vehicles_GunshipTVMovingCarrierLaunchHeading)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* aircraft : {"as_ag", "us_ag"}) {
			for (const physics::Vec3 velocity : {physics::Vec3{25, 0, 0}, physics::Vec3{-25, 0, 0},
					 physics::Vec3{0, 0, -25}, physics::Vec3{0, 0, 25}}) {
				Garage g(gameDir(), mod);
				if (!g.loaded || !g.templates.find(aircraft)) continue;
				const auto v = g.start(aircraft, {0, 4, 6});
				CHECK(g.enter(v));
				game::SoldierInput in; in.switchSeat = 2;
				g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
				g.world->setPlayerInput({}); g.run(4);
				in = {}; in.altFire = true;
				g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
				g.world->setPlayerInput({}); g.run(4);
				// Up off the ground first: the missile leaves its camera
				// (CameraId 11, under the nose, 10 degrees down: fireInCameraDof).
				for (int i = 0; i < 30; ++i) {
					CHECK(g.world->setBodyVelocity(v, {0, 5, 0}));
					g.world->tick(1 / 30.0f);
				}
				// Moving for a few ticks first: a launch takes the carrier's
				// displacement over the last tick (0x005f31d0, node slot 0x64).
				for (int i = 0; i < 3; ++i) {
					CHECK(g.world->setBodyVelocity(v, velocity));
					g.world->tick(1 / 30.0f);
				}
				CHECK(g.world->setBodyVelocity(v, velocity));
				g.world->tick(1 / 30.0f); // the node reports its velocity from the step's displacement (0x005f31d0 reads slot 0x64)
				in = {}; in.fire = true;
				g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
				game::GameWorld::ProjectileCamera camera;
				CHECK(g.world->projectileCamera(g.world->localPlayer(), camera));
				if (!g.world->projectileCamera(g.world->localPlayer(), camera)) continue;
				std::fprintf(stderr, "  moving TV: %s %s velocity %.0f/%.0f camera %.1f/%.1f\n",
					mod, aircraft, velocity.x, velocity.z, camera.yaw, camera.pitch);
				CHECK(std::fabs(std::remainder(camera.yaw, 360.0f)) < 5);
				CHECK(std::fabs(camera.pitch + 10) < 5);
				bool found = false;
				for (const auto& p : g.world->projectiles()) if (p.serial == camera.serial) {
					found = true;
					std::fprintf(stderr, "    missile velocity %.2f/%.2f/%.2f\n", p.velocity.x, p.velocity.y, p.velocity.z);
					// Retail (0x005f31d0): the round inherits the node's reported velocity and faces along the
					// camera (its nose, noseOf); the engine pushes along the nose (0x005b3e60). The old
					// forced alignment of the velocity to the camera frame is gone.
					CHECK(p.heading.z > 0.9f && std::fabs(p.heading.x) < 0.1f);
					CHECK(std::fabs(p.velocity.x - velocity.x) < 3 && std::fabs(p.velocity.z - velocity.z) < 3);
				}
				CHECK(found);
				g.world->setPlayerInput({});
				g.run(0.3f);
				CHECK(g.world->projectileCamera(g.world->localPlayer(), camera));
				CHECK(std::fabs(std::remainder(camera.yaw, 360.0f)) < 8 && std::fabs(camera.pitch + 10) < 8);
			}
		}
	}
}

// The TV missile leaves along its camera (fireInCameraDof, 0x005f1a20: the
// player's camera, CameraId 11, fixed under the nose 10 degrees down), not
// along the cannon's mount, which can be left pointing down.
TEST(Vehicles_GunshipTVLaunchUsesCameraDirection)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* aircraft : {"as_ag", "us_ag"}) {
			Garage g(gameDir(), mod);
			if (!g.loaded || !g.templates.find(aircraft)) continue;
			g.pitAhead(); // as a gunship flies: the missile, launched at rest, dives clear of the ground
			const auto v = g.start(aircraft, {0, 4, 6});
			CHECK(g.enter(v));
			game::SoldierInput in; in.switchSeat = 2;
			g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			g.world->setPlayerInput({}); g.run(4);
			float look = 0; // the bundles turn at their own speeds (stepGunner): lead the gun down
			seataim::steerGun([&](const game::SoldierInput& i) { g.world->setPlayerInput(i); g.world->tick(1 / 30.0f); },
				[&](float& y, float& p) { g.world->gunAim(v, 1, y, p); }, look, NAN, -25);
			in = {}; in.aimPitch = look;
			float yaw = 0, pitch = 0;
			CHECK(g.world->gunAim(v, 1, yaw, pitch) && pitch < -10);
			in.altFire = true; g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			in.altFire = false; g.world->setPlayerInput(in); g.run(4);
			in.fire = true;
			game::GameWorld::ProjectileCamera camera;
			// The press waits for the missile's draw delay (as upstream's Vehicles_GunshipMissileCamera does).
			for (int i = 0; i < 6 && !g.world->projectileCamera(g.world->localPlayer(), camera); ++i) {
				g.world->setPlayerInput(in);
				g.world->tick(1 / 30.0f);
			}
			CHECK(g.world->projectileCamera(g.world->localPlayer(), camera));
			bool found = false;
			for (const auto& p : g.world->projectiles()) if (p.serial == camera.serial) {
				found = true;
				std::fprintf(stderr, "  %s %s TV launch %.3f/%.3f/%.3f\n", mod, aircraft, p.heading.x,
					p.heading.y, p.heading.z);
				CHECK(std::fabs(p.heading.y + 0.1736f) < 0.03f && std::fabs(p.heading.x) < 0.03f);
			}
			CHECK(found);
		}
	}
}

TEST(Vehicles_GunshipTVSelectionRestoresGunnerPitch)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* aircraft : {"as_ag", "us_ag"}) {
			Garage g(gameDir(), mod);
			if (!g.loaded || !g.templates.find(aircraft)) continue;
			g.pitAhead(); // as a gunship flies: the missile, launched at rest, dives clear of the ground
			const auto v = g.start(aircraft, {0, 4, 6});
			CHECK(g.enter(v));
			game::SoldierInput in; in.switchSeat = 2;
			g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			g.world->setPlayerInput({}); g.run(1);
			in = {}; in.altFire = true;
			g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			in.altFire = false; g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			in.aimPitch = -35;
			g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			for (int i = 0; i < 30; ++i) {
				g.world->setPlayerInput(in); g.world->tick(1 / 30.0f); // the bundle settles (stepGunner)
			}
			float yaw = 0, held = 0, pitch = 0;
			CHECK(g.world->gunAim(v, 1, yaw, held));
			in.altFire = true; g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			in.altFire = false; g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			const float correction = g.world->takeAimKick(g.world->localPlayer());
			CHECK(std::fabs(correction - 35) < 0.1f);
			in.aimPitch += correction;
			g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
			// The look's restore is no turn of the gun: the bundle takes the
			// look's changes only (stepGunner), so it stays where it was.
			CHECK(g.world->gunAim(v, 1, yaw, pitch));
			CHECK(std::fabs(pitch - held) < 1.0f);
		}
	}
}

TEST(Vehicles_GunshipMouseInputBetweenTicks)
{
	if (!gameDir()) return;
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_ag")) return;
	const auto v = g.start("as_ag", {0, 4, 6});
	CHECK(g.enter(v));
	game::SoldierInput in; in.switchSeat = 2;
	g.world->setPlayerInput(in); g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({}); g.run(1);
	float before = 0, pitch = 0, after = 0;
	CHECK(g.world->gunAim(v, 1, before, pitch));
	in = {}; in.turn = 1;
	for (int i = 0; i < 4; ++i) g.world->setPlayerInput(in);
	g.world->tick(1 / 30.0f);
	CHECK(g.world->gunAim(v, 1, after, pitch));
	// The four moves make one tick's 4 degrees, 0.8 PIMouseLook units: the
	// yaw bundle's speed goes towards 0.8 x maxSpeed (stepGunner, 0x00563bd0).
	const auto& axis = g.world->vehicleSeats(v)[1].yawAxis;
	const float speed = std::min(0.8f * axis.maxSpeed, axis.acceleration / 30);
	CHECK(speed > 0 && std::fabs(std::remainder(after - before, 360.0f) - speed / 30) < 0.05f);
}

// A seat's weapons by group (0x005cc5e0): the heavy mech's driver has its
// main cannon (PIFire), its dumb rockets (fire.fireInput PIAltFire,
// itemIndex 3) and ads_land (PIFlareFire), each its group's active one;
// its SeatWeapon part is the PIFire one. The keys (0x004d2a50): a
// c_PIWeaponSelect key takes the selectable weapon of that item index in
// each group; the key of the active one toggles its fire rate instead.
TEST(Vehicles_SeatWeaponGroupsAndKeys)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_heavy_mech") || !g.templates.find("as_ag")) {
		return;
	}
	const auto lowerName = [](std::string n) {
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return n;
	};
	{
		const uint32_t mech = g.start("as_heavy_mech", {0, 4, 6});
		const auto seats = g.world->vehicleSeats(mech);
		CHECK(!seats.empty());
		if (seats.empty()) {
			return;
		}
		int groups[3] = {0, 0, 0};
		for (const auto& w : seats[0].weapons) {
			if (w.group >= 0) {
				++groups[w.group];
			}
			if (w.group == 1) {
				CHECK(lowerName(w.weapon) == "as_heavy_mech_dumrockets_firearm" && w.itemIndex == 3);
			}
			if (w.group == 2) {
				CHECK(w.activeDefense && w.protectionRadius == 6 && w.detonateMaterial == 2142);
			}
		}
		CHECK(groups[0] == 1 && groups[1] == 1 && groups[2] == 1);
		CHECK(lowerName(seats[0].weapon) == "as_heavy_mech_main_cannon");
	}
	// The gunship's gunner with both weapons made selectable: key 2 the
	// missile, key 1 the cannon again.
	for (const char* name : {"as_ag_GunnerCannon", "as_ag_GuidedMissileWeapon"}) {
		if (world::ObjectTemplate* t = g.templates.find(name)) {
			t->settings()["isSelectable"] = "1";
		}
	}
	const uint32_t v = g.start("as_ag", {0, 4, 6});
	CHECK(g.enter(v));
	game::SoldierInput input;
	input.switchSeat = 2;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(1);
	const auto weapon = [&] { return lowerName(g.world->vehicleSeats(v)[1].weapon); };
	CHECK(weapon() == "as_ag_gunnercannon");
	const auto key = [&](int item) {
		game::SoldierInput press;
		press.selectItem = item;
		g.world->setPlayerInput(press);
		g.world->tick(1 / 30.0f);
		g.world->setPlayerInput({});
		g.world->tick(1 / 30.0f);
	};
	key(2);
	CHECK(weapon() == "as_ag_guidedmissileweapon");
	CHECK(g.world->seatWeaponIndex(v, 1) >= 0);
	key(1);
	CHECK(weapon() == "as_ag_gunnercannon");
	// c_PIToggleWeapon: the next by item index, round from the last.
	game::SoldierInput toggle;
	toggle.toggleWeapon = true;
	g.world->setPlayerInput(toggle);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	CHECK(weapon() == "as_ag_guidedmissileweapon");
}

// The second gun's HUD (0x007c2d50: the control object's slot 0x78(1), the
// PIAltFire group's active weapon): the heavy mech's driver's dumb rockets
// fill the Secondary* block beside the cannon's Primary* one.
TEST(Vehicles_SecondWeaponHud)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_heavy_mech")) {
		return;
	}
	const uint32_t mech = g.start("as_heavy_mech", {0, 4, 6});
	CHECK(g.enter(mech));
	g.run(1);
	const auto lowerName = [](std::string n) {
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return n;
	};
	game::GameLogic logic;
	host::LocalHudWorld world(*g.world, logic);
	const host::HudWorld::Weapon first = world.seatGroupWeapon(0), second = world.seatGroupWeapon(1);
	CHECK(first.weapon && lowerName(first.weapon->name()) == "as_heavy_mech_main_cannon");
	CHECK(second.weapon && lowerName(second.weapon->name()) == "as_heavy_mech_dumrockets_firearm");
	CHECK(second.gun != nullptr && second.rounds > 0);
	if (!second.weapon || !second.gun) {
		return;
	}
	hud::HudManager hud(nullptr);
	menu::WeaponHud secondary;
	menu::WeaponHudInput in;
	in.weapon = second.weapon;
	in.magSize = second.gun->settings().magSize;
	in.nrOfMags = second.gun->settings().nrOfMags;
	in.rounds = second.rounds;
	in.mags = second.mags;
	secondary.update(hud, in, true);
	CHECK(hud.number("SecondaryAmmoShow") == 1 && hud.string("SecondaryAmmoString") == std::to_string(second.rounds));
}

// The active defence (ads_land on the tank's driver, PIFlareFire): a press
// raises the shield on the hull for one round's interval (5 s at 12 a
// minute, 0x005d97f0) of min(protectionRadius 6, the hull's radius - 0.1);
// a round with activeDefenseEnabled entering it goes off on it with
// material 2142 (0x006c07a0), short of the hull.
TEST(Vehicles_ActiveDefenceShield)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("eu_tank")) {
		return;
	}
	const uint32_t tank = g.start("eu_tank", {0, 2, 6});
	CHECK(g.enter(tank));
	g.run(2);
	CHECK(g.world->shieldRadius(tank) == 0);
	game::SoldierInput flare;
	flare.flareFire = true;
	g.world->setPlayerInput(flare);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	// The press is a fire event to the ads' children (e_ActiveDef_shield).
	const auto fires = g.world->takeObjectWeaponFires();
	const auto fired = fires.find(tank);
	CHECK(fired != fires.end() && std::any_of(fired->second.begin(), fired->second.end(), [](const auto& fire) { return fire.weapon == "ads_land"; }));
	const float radius = g.world->shieldRadius(tank);
	std::fprintf(stderr, "  shield radius %.2f\n", radius);
	CHECK(radius > 2 && radius <= 6);
	const physics::Vec3 at = g.position(tank);
	const size_t before = g.world->impacts().size();
	g.world->launchRemoteProjectile(987654, "AAGuided_missile", at + physics::Vec3 {0, 0.5f, 40}, {0, 0, -100});
	g.run(1);
	bool onShield = false;
	for (size_t i = before; i < g.world->impacts().size(); ++i) {
		const auto& impact = g.world->impacts()[i];
		const float d = std::sqrt(physics::lengthSquared(impact.point - at));
		std::fprintf(stderr, "  impact material %u at %.2f m\n", impact.material, d);
		onShield = onShield || (impact.material == 2142 && std::fabs(d - radius) < 0.6f);
	}
	CHECK(onShield);
	g.run(5);
	CHECK(g.world->shieldRadius(tank) == 0);
}

// ignoreParentRotation's pitch (0x00563180): the parent's turn measured at
// the child's aim; eu_tank's cupola gun (0/0.5/0) takes back half the
// hull's pitch as the tank settles onto a slope, the look held still; the
// FAV driver's camera (0/0.6/0) its share too.
TEST(Vehicles_IgnoreParentRotationPitch)
{
	physics::Mat3 level;
	const physics::Mat3 noseUp = physics::Mat3::fromYawPitchRoll({0, -10, 0});
	float yawTurn = 0, pitchTurn = 0;
	game::GameWorld::parentTurn(level, noseUp, 0, 0, yawTurn, pitchTurn);
	std::fprintf(stderr, "  hull 10 degrees about x: the aim's pitch turn %.2f, yaw %.2f\n", pitchTurn, yawTurn);
	CHECK(std::fabs(std::fabs(pitchTurn) - 10) < 0.05f && std::fabs(yawTurn) < 0.05f);
	game::GameWorld::parentTurn(level, level, 30, 5, yawTurn, pitchTurn);
	CHECK(yawTurn == 0 && pitchTurn == 0);
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("eu_tank")) {
		return;
	}
	const uint32_t tank = g.start("eu_tank", {0, 2, 6});
	const auto seats = g.world->vehicleSeats(tank);
	CHECK(seats.size() >= 2);
	if (seats.size() < 2) {
		return;
	}
	CHECK(seats[1].ignoreParentPitch == 0.5f);
	CHECK(g.enter(tank));
	// The ground drops a metre and tilts (0.3 up along +z); the driver's
	// throttle wakes the hull, and the player moves to the cupola as it
	// falls onto the slope.
	g.world->enablePhysics(g.files, g.collisionTemplates,
		{[](float, float z) { return 0.3f * (z - 6) - 1; }, [](float, float z, physics::Vec3& normal) {
			 normal = physics::Vec3 {0, 1, -0.3f} * (1 / std::sqrt(1.09f));
			 return 0.3f * (z - 6) - 1;
		 }},
		&g.materials);
	game::SoldierInput input;
	input.throttle = 1;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	input = {};
	input.switchSeat = 2;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	float yaw = 0, pitch = 0;
	CHECK(g.world->gunAim(tank, 1, yaw, pitch));
	const float flatPitch = pitch;
	const float hullBefore = g.objects.find(tank)->rotation().y;
	g.drive({}, 4);
	CHECK(g.world->gunAim(tank, 1, yaw, pitch));
	const float hullAfter = g.objects.find(tank)->rotation().y;
	std::fprintf(stderr, "  hull pitch %.1f -> %.1f, cupola gun pitch %.1f -> %.1f\n", hullBefore, hullAfter, flatPitch, pitch);
	const float hullTurn = std::remainder(hullAfter - hullBefore, 360.0f);
	CHECK(std::fabs(hullTurn) > 5);
	// Half the hull's turn taken back (the hull's rotation y is down positive; the aim's pitch up positive).
	CHECK(std::fabs((pitch - flatPitch) - 0.5f * hullTurn) < 0.25f * std::fabs(hullTurn));
	if (g.templates.find("eu_fav")) {
		const uint32_t fav = g.start("eu_fav", {0, 2, 6});
		const auto favSeats = g.world->vehicleSeats(fav);
		CHECK(!favSeats.empty() && favSeats[0].cameraIgnorePitch == 0.6f);
	}
}

namespace {

// The animation systems for a Garage (the client archives' .inc and .baf
// files), as Vehicles_BundleSkeletonPosesBoneParts sets them up.
struct Animations {
	std::string out;
	anim::AnimationSystems systems;
	explicit Animations(Garage& g)
		: systems([&g](const std::string& path, std::vector<uint8_t>& data) { return g.files.readFile(path, data); })
	{
		g.files.setMountingAllowed(true);
		g.console.run("ClientArchives.con", {}, out);
		g.files.setMountingAllowed(false);
		anim::registerAnimationConsoleMethods(g.console, systems);
		g.console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, out);
	}
	void attach(Garage& g)
	{
		g.world->setAnimationSystems(&systems, [this, &g](const std::string& path) {
			return systems.get(path, [&] { g.console.run(path, {}, out); });
		});
	}
};

float yawOf(const physics::Vec3& v) { return std::atan2(v.x, v.z) * 57.29578f; }

} // namespace

// A ground driver's guns (0x004bda00 -> 0x004cc7a0 -> 0x005cacf0, case 6:
// the control object the player drives gets the fire event as any seat's):
// eu_tank's driver turns the turret (EU_TANK_Turret, PIMouseLookX) and the
// barrel (PIMouseLookY) with the look and fires the main gun along them;
// the cupola (EU_TANK_CupolaBase under the turret) rides the turret. The
// hover tank's barrel is its driver's on PIPitch (a firearm under it), its
// gunner's pitch bundle the mouse's (AS_TANK_Cupola4), not the PICameraY
// hatch; the FAV's driver has no gun to turn.
TEST(Vehicles_GroundDriverAimsAndFires)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_tank") || !g.templates.find("eu_fav")) {
		return;
	}
	const auto lowerName = [](std::string n) {
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return n;
	};
	{
		const uint32_t hover = g.start("as_tank", {0, 3, 6});
		const auto seats = g.world->vehicleSeats(hover);
		CHECK(seats.size() >= 2);
		if (seats.size() >= 2) {
			CHECK(seats[0].yawBundle.empty() && lowerName(seats[0].pitchBundle) == "as_tank_barrelbase");
			CHECK(lowerName(seats[1].pitchBundle) == "as_tank_cupola4" && seats[1].minPitch == -25 && seats[1].maxPitch == 17);
		}
		const uint32_t fav = g.start("eu_fav", {0, 2, 6});
		const auto favSeats = g.world->vehicleSeats(fav);
		CHECK(!favSeats.empty() && favSeats[0].yawBundle.empty() && favSeats[0].pitchBundle.empty());
		CHECK(!g.world->seatTurnsGun(fav, 0));
	}
	// Away from the hover tank and the FAV left standing at its place (they
	// would shove it: their parts meet its hull, MeshCollision.cpp).
	const uint32_t tank = g.start("eu_tank", {40, 2, 6});
	CHECK(g.enter(tank));
	CHECK(g.world->seatTurnsGun(tank, 0));
	g.run(2);
	float yaw = 0, pitch = 0;
	CHECK(g.world->gunAim(tank, 0, yaw, pitch));
	const float hullYaw = g.yaw(tank);
	// The look drives the bundles' speeds, not their angles (stepGunner,
	// 0x00563bd0): EU_TANK_Turret 25 degrees a second a PIMouseLook unit
	// (5 degrees of look) reached at 500 a second, a second. A flick of 30
	// degrees a tick for 3 ticks winds it up 16.7 a second each tick; after
	// the mouse stops it keeps turning two more ticks as it slows.
	float start = 0;
	CHECK(g.world->gunAim(tank, 0, start, pitch));
	game::SoldierInput flick;
	flick.turn = 30;
	g.drive(flick, 0.1f);
	float stopped = 0;
	CHECK(g.world->gunAim(tank, 0, stopped, pitch));
	g.drive({}, 0.2f);
	CHECK(g.world->gunAim(tank, 0, yaw, pitch));
	const float driving = std::remainder(stopped - start, 360.0f), coasting = std::remainder(yaw - stopped, 360.0f);
	std::fprintf(stderr, "  eu_tank flick: %.2f turning, %.2f coasting\n", driving, coasting);
	CHECK(std::fabs(driving - 100.0f / 30) < 0.2f); // 16.7 + 33.3 + 50 degrees a second, a tick each
	CHECK(std::fabs(coasting - 50.0f / 30) < 0.2f);  // 33.3 + 16.7, then still
	float look = 0;
	seataim::steerGun([&](const game::SoldierInput& in) { g.world->setPlayerInput(in); g.world->tick(1 / 30.0f); },
		[&](float& y, float& p) { g.world->gunAim(tank, 0, y, p); }, look, std::remainder(start + 45, 360.0f), 4);
	CHECK(g.world->gunAim(tank, 0, yaw, pitch));
	yaw = std::remainder(yaw - start, 360.0f);
	std::fprintf(stderr, "  eu_tank turret %.1f, barrel %.1f (the hull turned %.1f)\n", yaw, pitch,
		std::remainder(g.yaw(tank) - hullYaw, 360.0f));
	CHECK(std::fabs(std::remainder(yaw - 45, 360.0f)) < 3);
	CHECK(std::fabs(pitch - 4) < 1.5f); // the barrel's ignoreParentRotation (0/0.7/0) takes back the hull's settling
	// The cupola's gun turns in the turret's frame.
	physics::Vec3 origin;
	physics::Mat3 frame;
	CHECK(g.world->gunFrame(tank, 0, origin, frame));
	const size_t before = g.world->projectiles().size();
	game::SoldierInput fire;
	fire.turn = 0;
	fire.aimPitch = look;
	fire.fire = true;
	g.drive(fire, 0.1f);
	g.world->setPlayerInput({});
	bool fired = false;
	for (size_t i = before; i < g.world->projectiles().size(); ++i) {
		const game::Projectile& p = g.world->projectiles()[i];
		if (p.shooter != tank) {
			continue;
		}
		fired = true;
		const float along = std::remainder(yawOf(p.velocity) - g.yaw(tank), 360.0f);
		std::fprintf(stderr, "  the main gun's round heads %.1f off the hull\n", along);
		CHECK(std::fabs(std::remainder(along - 45, 360.0f)) < 4);
	}
	CHECK(fired);
	// Up into the cupola: its gun's frame is the turret's.
	game::SoldierInput move;
	move.switchSeat = 2;
	g.world->setPlayerInput(move);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	CHECK(g.world->gunFrame(tank, 1, origin, frame));
	const float cupolaFrame = std::remainder(frame.yawPitchRoll().x - g.yaw(tank), 360.0f);
	std::fprintf(stderr, "  the cupola's frame %.1f off the hull\n", cupolaFrame);
	CHECK(std::fabs(std::remainder(cupolaFrame - 45, 360.0f)) < 3);
}

// fire.fireInCameraDof (the fire frame 0x005f1a20): a seat gun's round
// leaves along the seat camera's frame (the player's camera's Camera
// object, its own look included), not the gun's. eu_tank's cupola camera
// leans with the mouse (eu_tank_cupola_Camera: +-1 yaw), so a turning
// gunner's rounds go off the barrel by the lean.
TEST(Vehicles_CameraDofRoundsFollowTheCamera)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t tank = g.start("eu_tank", {0, 2, 6});
	CHECK(g.enter(tank));
	game::SoldierInput move;
	move.switchSeat = 2;
	g.world->setPlayerInput(move);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(1);
	g.world->setZoomed(g.world->localPlayer(), true); // no deviation
	game::SoldierInput turn;
	turn.turn = 3;
	g.drive(turn, 0.5f);
	const size_t before = g.world->projectiles().size();
	turn.fire = true;
	g.world->setPlayerInput(turn);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	CHECK(g.world->projectiles().size() > before);
	if (g.world->projectiles().size() <= before) {
		return;
	}
	const game::Projectile& p = g.world->projectiles()[before];
	world::Vec3 offset, look;
	CHECK(g.world->partPose(tank, "eu_tank_cupola_Camera", offset, look, 1));
	physics::Vec3 origin;
	physics::Mat3 hull;
	float yaw = 0, pitch = 0;
	CHECK(g.world->gunFrame(tank, 1, origin, hull));
	CHECK(g.world->gunAim(tank, 1, yaw, pitch));
	const float y = yaw * 3.14159265f / 180.0f, x = pitch * 3.14159265f / 180.0f;
	const physics::Vec3 barrel = hull.transform({std::sin(y) * std::cos(x), std::sin(x), std::cos(y) * std::cos(x)});
	const physics::Vec3 way = p.velocity * (1 / std::sqrt(physics::lengthSquared(p.velocity)));
	const physics::Vec3 local = hull.untransform(way);
	const float roundYaw = std::atan2(local.x, local.z) * 57.29578f;
	const float off = std::acos(std::clamp(static_cast<double>(physics::dot(way, barrel)), -1.0, 1.0)) * 57.29578;
	std::fprintf(stderr, "  cupola gun %.2f, camera lean %.2f/%.2f, round %.2f (off the barrel %.2f)\n", yaw, look.x, look.y,
		roundYaw, off);
	CHECK(std::fabs(look.x) > 0.5f);
	CHECK(std::fabs(std::remainder(roundYaw - (yaw + look.x), 360.0f)) < 0.15f);
}

// The heavy mech's guns ride its torso bone (AS_heavy_mech_torsospring,
// boneIndex 1: 0x0063d470 -> its slot 0xe8): the main cannon's rounds start
// where the posed torso holds it, so they rise and fall with it; parked
// (unmanned) the torso sinks and the gun with it. The secondary position
// sits on the torso's turret (the driver's yaw bundle).
TEST(Vehicles_MechGunsRideTheTorsoBone)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_heavy_mech")) {
		return;
	}
	Animations a(g);
	const uint32_t mech = g.start("as_heavy_mech", {0, 4, 6});
	a.attach(g);
	const auto seats = g.world->vehicleSeats(mech);
	CHECK(seats.size() >= 2);
	if (seats.size() < 2) {
		return;
	}
	const auto lowerName = [](std::string n) {
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return n;
	};
	CHECK(lowerName(seats[0].gunAnchor.bone) == "as_heavy_mech_torsospring" && seats[0].gunAnchor.yawSeat == -1);
	CHECK(lowerName(seats[1].gunAnchor.bone) == "as_heavy_mech_torsospring" && seats[1].gunAnchor.yawSeat == 0);
	CHECK(seats[0].morePitchBundles.size() >= 2); // the rocket rack and the cannon mount beside the camera mount
	CHECK(g.enter(mech));
	g.drive({}, 3);
	const auto gunHeight = [&] {
		physics::Vec3 origin;
		physics::Mat3 frame;
		CHECK(g.world->gunFrame(mech, 0, origin, frame));
		const world::Object* o = g.objects.find(mech);
		const physics::Mat3 hull = physics::Mat3::fromYawPitchRoll({o->rotation().x, o->rotation().y, o->rotation().z});
		return hull.untransform(origin - g.position(mech)).y;
	};
	anim::Matrix standingBone {}, parkedBone {};
	CHECK(g.world->partBone(mech, "AS_heavy_mech_torsospring", standingBone));
	const float standing = gunHeight();
	game::SoldierInput leave;
	leave.use = true;
	g.world->setPlayerInput(leave);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	CHECK(!g.world->playerVehicle());
	g.run(4);
	CHECK(g.world->partBone(mech, "AS_heavy_mech_torsospring", parkedBone));
	const float parked = gunHeight();
	std::fprintf(stderr, "  main cannon over the hull: standing %.2f, parked %.2f (the torso bone %.2f -> %.2f)\n", standing,
		parked, standingBone[13], parkedBone[13]);
	// Parked the torso also tips forward (its bone's rows turn by about 13
	// degrees about x), so the cannon ahead of it drops further than the
	// bone's own drop.
	std::fprintf(stderr, "  parked torso's tip: %.2f\n", parkedBone[6]);
	const float drop = standingBone[13] - parkedBone[13];
	CHECK(standing - parked > drop - 0.05f && standing - parked < drop + 1.5f);
	CHECK(std::fabs(parkedBone[6]) > 0.1f);
}

// The deployables' own systems (0x0063d530 builds the skeleton helper for
// any template with a bundleSkeleton; 0x0063d5b0 -> 0x0063d470): the
// portable shield's and the ammo hub's "idle" CountTrigger plays "open"
// once, then stays in "stand"/"idle", and their boneIndex parts take the
// bones' places (the shield's arms spread, the hub's lids open).
TEST(Vehicles_DeployablesPoseTheirParts)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	world::ObjectTemplate* kit = g.loaded ? g.templates.find("EU_Assault") : nullptr;
	if (!kit || !g.templates.find("unl_port_shield") || !g.templates.find("Unl_Hub_Ammo")) {
		return;
	}
	kit->children() = {{"eu_ar_rifle", {}, {}}, {"Unl_Hub_Ammo", {}, {}}, {"unl_port_shield", {}, {}}};
	Animations a(g);
	g.start("eu_fav", {30, 2, 30});
	a.attach(g);
	CHECK(g.world->spawnPlayer(2) != nullptr);
	g.run(1);
	const auto placed = [&](const char* name) -> uint32_t {
		for (const auto& object : g.objects.objects()) {
			if (object->objectTemplate() && object->objectTemplate()->name() == name) {
				return object->id();
			}
		}
		return 0;
	};
	const auto deploy = [&](int item) {
		game::SoldierInput take;
		take.selectItem = item;
		g.drive(take, 1.5f);
		game::SoldierInput fire;
		fire.fire = true;
		g.drive(fire, 0.1f);
		g.drive({}, 0.1f);
	};
	deploy(5);
	const uint32_t shield = placed("portable_shield");
	// The arms as it is put down (posed with each update of its system).
	anim::Matrix early {}, late {};
	CHECK(g.world->partBone(shield, "portable_shield_arm1_anim", early));
	deploy(4);
	const uint32_t hub = placed("unl_hub_ammo_stationary_model");
	CHECK(shield != 0 && hub != 0);
	g.run(4);
	CHECK(g.world->partBone(shield, "portable_shield_arm1_anim", late));
	float moved = 0;
	for (size_t i = 0; i < 16; ++i) {
		moved = std::max(moved, std::fabs(late[i] - early[i]));
	}
	std::fprintf(stderr, "  the shield's arm moved by %.2f (matrix entries) as it opened\n", moved);
	CHECK(moved > 0.05f);
	const auto playing = g.world->objectAnimation(shield);
	CHECK(std::find(playing.begin(), playing.end(), "stand") != playing.end());
	anim::Matrix lid {};
	CHECK(hub == 0 || g.world->partBone(hub, "UNL_HUB_Ammo_lid1", lid));
}

// The deployables as the rounds meet them: each is a mobile mesh body (its
// root's instance and its collisionPart children's, posed by its bones,
// 0x0063ccc0 / 0x0063d470). The portable shield's ThrownFireComp turns it
// by fire.projectileStartRotation 90/0/0 in its own frame (0x005e5720), so
// its panel (geometry 0, in the root's y-z plane, about 0.94 m across)
// faces the deployer: rounds from in front, 0.3 m off its middle, meet it
// (set down along the view they passed its 4 cm edge). The sentry gun, its
// shape all parts, takes rounds and is destroyed (120 hit points).
TEST(Vehicles_DeployablesStopRounds)
{
	if (!gameDir()) {
		return;
	}
	for (const char* gadget : {"unl_port_shield", "Unl_Deployer_Sentrygun"}) {
		Garage g(gameDir());
		world::ObjectTemplate* kit = g.loaded ? g.templates.find("EU_Assault") : nullptr;
		if (!kit || !g.templates.find(gadget)) {
			return;
		}
		const bool shield = std::string(gadget) == "unl_port_shield";
		kit->children() = {{"eu_ar_rifle", {}, {}}, {gadget, {}, {}}};
		Animations a(g);
		g.start("eu_fav", {30, 2, 30});
		a.attach(g);
		CHECK(g.world->spawnPlayer(2) != nullptr);
		game::GameWorld::PlayerInfo me;
		CHECK(g.world->playerInfo(g.world->localPlayer(), me));
		g.world->setSoldierPosition(me.soldier, {0, 0, 0});
		g.run(1);
		game::SoldierInput take;
		take.selectItem = 5;
		g.drive(take, 1.5f);
		game::SoldierInput fire;
		fire.fire = true;
		g.drive(fire, 0.1f);
		g.drive({}, 2.5f); // unfolded
		uint32_t placed = 0;
		for (const auto& object : g.objects.objects()) {
			const world::ObjectTemplate* t = object->objectTemplate();
			if (t && t->name() == (shield ? "portable_shield" : "eu_sentrygun")) {
				placed = object->id();
			}
		}
		CHECK(placed != 0);
		if (!placed) {
			continue;
		}
		const physics::Vec3 at = g.position(placed);
		const float yaw = g.yaw(placed);
		std::fprintf(stderr, "  %s at %.2f %.2f %.2f, yaw %.1f, %.0f hit points\n", gadget, at.x, at.y, at.z, yaw,
			g.world->hitPoints(placed));
		if (shield) {
			// The deployer looked along +z (yaw 0): the shield turned by 90.
			CHECK(std::fabs(std::remainder(yaw - 90.0f, 360.0f)) < 1.0f);
		}
		// Six metres back on the deployer's side (the shield's 0.3 m off its
		// middle), still looking along +z, the rifle aimed at its middle.
		g.world->setSoldierPosition(me.soldier, {at.x + (shield ? 0.3f : 0.0f), 0, at.z - 6});
		take.selectItem = 3;
		g.drive(take, 1.5f);
		const float before = g.world->hitPoints(placed);
		game::SoldierInput shoot;
		shoot.aimPitch = shield ? -12 : -10;
		g.drive(shoot, 0.5f);
		bool destroyed = false;
		for (int burst = 0; burst < 40 && !destroyed; ++burst) {
			shoot.fire = true;
			g.drive(shoot, 1 / 30.0f);
			shoot.fire = false;
			g.drive(shoot, 0.4f);
			destroyed = !g.objects.find(placed) || g.world->hitPoints(placed) <= 0;
			if (shield && g.world->hitPoints(placed) < before) {
				break;
			}
		}
		const float after = g.objects.find(placed) ? g.world->hitPoints(placed) : 0;
		std::fprintf(stderr, "  %s: %.0f -> %.0f hit points%s\n", gadget, before, after, destroyed ? ", destroyed" : "");
		CHECK(after < before);
		if (!shield) {
			CHECK(destroyed);
		}
	}
}

// The player's may-use test (0x005921e0) and a weapon key's item selection
// in a seat (0x004d2a50 -> 0x0079c190): the heavy mech's driver presses 3,
// the rockets' key (PIAltFire group, already active): no new pick, no
// popup; the gunship's gunner with both weapons made selectable: key 2
// takes the missile and opens the selection on it. Each entry starts every
// group at its first weapon again (0x005cdd60). The selection's flash: six
// toggles of the key's slot 0.1 s apart, the list held open meanwhile.
TEST(Vehicles_SeatItemSelectionAndFirstWeapons)
{
	{
		hud::HudManager hud(nullptr);
		menu::ItemSelection items;
		items.open({{1, "cannon.tga"}, {2, "missile.tga"}}, 2, 0.4f);
		items.flash(2);
		CHECK(items.flashing() == 1);
		int toggles = 0;
		float last = 1;
		for (int i = 0; i < 14; ++i) {
			items.update(hud, 0.05f);
			const float now = hud.number("ItemSlot1Active");
			toggles += now != last;
			last = now;
		}
		CHECK(toggles == 6 && last == 1);
		CHECK(hud.number("ShowItemSelect") == 1); // 0.4 s after the last toggle at 0.6 s
		for (int i = 0; i < 10; ++i) {
			items.update(hud, 0.05f);
		}
		CHECK(hud.number("ShowItemSelect") == 0);
	}
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_ag")) {
		return;
	}
	for (const char* name : {"as_ag_GunnerCannon", "as_ag_GuidedMissileWeapon"}) {
		if (world::ObjectTemplate* t = g.templates.find(name)) {
			t->settings()["isSelectable"] = "1";
		}
	}
	const uint32_t v = g.start("as_ag", {0, 4, 6});
	CHECK(g.enter(v));
	const int me = g.world->localPlayer();
	game::SoldierInput input;
	input.switchSeat = 2;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(1);
	g.world->takeSeatItemSelect(me);
	const int first = g.world->seatWeaponIndex(v, 1);
	game::SoldierInput press;
	press.selectItem = 2;
	g.world->setPlayerInput(press);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	CHECK(g.world->seatWeaponIndex(v, 1) != first);
	CHECK(g.world->takeSeatItemSelect(me) == 2);
	CHECK(g.world->takeSeatItemSelect(me) == 0); // once
	// Out and back in: the group's first weapon again.
	input = {};
	input.switchSeat = 1;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	input.switchSeat = 2;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	CHECK(g.world->seatWeaponIndex(v, 1) == first);
}

// surpressPlayerInputVector (0x00565700): the air transports' gun mounts
// (0.6 on the yaw) turn at 0.6 of the gunner's input while zoomed, at the
// whole of it otherwise.
TEST(VehicleModels_SuppressedInputWhileZoomed)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_at")) {
		return;
	}
	const uint32_t at = g.start("as_at", {0, 2, 6});
	const auto seats = g.world->vehicleSeats(at);
	int seat = -1;
	for (size_t i = 0; i < seats.size(); ++i) {
		if (seats[i].suppressYaw != 0 && seat < 0) {
			seat = static_cast<int>(i);
		}
	}
	CHECK(seat >= 0);
	if (seat < 0) {
		return;
	}
	std::fprintf(stderr, "  as_at seat %d (%s): %.2f\n", seat, seats[static_cast<size_t>(seat)].yawBundle.c_str(),
		seats[static_cast<size_t>(seat)].suppressYaw);
	CHECK(seats[static_cast<size_t>(seat)].suppressYaw == 0.6f);
	CHECK(g.enter(at));
	game::SoldierInput input;
	input.switchSeat = seat + 1;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	const int me = g.world->localPlayer();
	float turned[2] = {};
	for (const bool zoomed : {false, true}) {
		g.world->setZoomed(me, zoomed);
		float yaw0 = 0, yaw1 = 0, pitch = 0;
		g.world->gunAim(at, seat, yaw0, pitch);
		// The bundle's speed follows the input (stepGunner): held, the turn
		// is its steady speed, which the suppression scales.
		input = {};
		input.turn = 2;
		for (int tick = 0; tick < 10; ++tick) {
			g.world->setPlayerInput(input);
			g.world->tick(1 / 30.0f);
		}
		g.world->setPlayerInput({});
		for (int tick = 0; tick < 5; ++tick) {
			g.world->tick(1 / 30.0f);
		}
		g.world->gunAim(at, seat, yaw1, pitch);
		turned[zoomed ? 1 : 0] = std::remainder(yaw1 - yaw0, 360.0f);
	}
	std::fprintf(stderr, "  turned %.2f, zoomed %.2f\n", turned[0], turned[1]);
	CHECK(std::fabs(turned[0]) > 1 && std::fabs(turned[1] / turned[0] - 0.6f) < 0.02f);
}

// A RotationalBundle turns by angle x multiply + offset (0x00564940). The
// transport's door-gun camera arm (rest roll 90, multiply -1) swings from 90
// to -10 as the gunner settles in; unscaled it ended at 190, upside down.
TEST(VehicleModels_DoorGunCameraArmUsesMultiply)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_at")) {
		return;
	}
	const uint32_t at = g.start("as_at", {0, 2, 6});
	CHECK(g.enter(at));
	g.run(3); // startAutoRotateOnEnter: the arms roll to their +100 limit
	world::Vec3 offset, rotation;
	// The left arm's roll runs 0 to +100 on its axis, the right's 0 to -100;
	// multiply -1 turns each frame the other way, to -100 and +100.
	for (const bool left : {true, false}) {
		const char* arm = left ? "as_at_L_gun_cameraArm" : "as_at_R_gun_cameraArm";
		CHECK(g.world->partPose(at, arm, offset, rotation));
		std::fprintf(stderr, "  %s roll %.1f\n", arm, rotation.z);
		CHECK(std::fabs(rotation.z - (left ? -100.0f : 100.0f)) < 1);
	}
}

// The transport's passenger seats (positions 4 to 7) leave the primary fire to
// the soldier's own weapon; the pod launcher there is the exit key's alone.
TEST(VehicleModels_TransportPassengerFiresHisOwnWeapon)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_at")) {
		return;
	}
	const uint32_t at = g.start("as_at", {0, 2, 6});
	const auto seats = g.world->vehicleSeats(at);
	CHECK(seats.size() >= 7);
	if (seats.size() < 7) {
		return;
	}
	for (size_t i = 0; i < seats.size(); ++i) {
		CHECK(seats[i].kitWeapon() == (i >= 3)); // the pilot and the door guns keep theirs
	}
	CHECK(g.enter(at));
	game::SoldierInput input;
	input.switchSeat = 4;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(1);
	game::GameWorld::PlayerInfo me;
	CHECK(g.world->playerInfo(g.world->localPlayer(), me) && me.vehicle == at && me.seat == 3);
	const auto before = g.world->playerRounds(g.world->localPlayer(), false, false);
	input = {};
	input.fire = true;
	for (int tick = 0; tick < 20; ++tick) {
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
	}
	g.world->setPlayerInput({});
	const auto after = g.world->playerRounds(g.world->localPlayer(), false, false);
	CHECK(g.world->playerInfo(g.world->localPlayer(), me) && me.vehicle == at); // not launched in a pod
	std::fprintf(stderr, "  rounds %d -> %d\n", before.empty() ? 0 : before[0].count, after.empty() ? 0 : after[0].count);
	CHECK(!after.empty() && after[0].count > (before.empty() ? 0 : before[0].count));
}

TEST(VehicleModels_TransportPassengerRecoilAndHudAfterPilot)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		Garage g(gameDir(), mod);
		CHECK(g.loaded);
		if (!g.loaded) return;
		const uint32_t at = g.start("as_at", {0, 2, 6});
		CHECK(g.enter(at));
		game::GameWorld::PlayerInfo me;
		CHECK(g.world->playerInfo(g.world->localPlayer(), me) && me.seat == 0);
		const auto seats = g.world->vehicleSeats(at);
		CHECK(seats.size() > 3 && !seats[0].kitWeapon() && seats[3].kitWeapon());
		if (seats.size() <= 3) return;
		game::GameLogic logic;
		host::LocalHudWorld world(*g.world, logic);
		hud::HudManager hud(nullptr);
		menu::VehicleGuiInput pilot;
		pilot.controlled = g.templates.find(seats[0].name);
		pilot.weapon = world.seatGroupWeapon(0).weapon;
		hud.set("GuiIndex", static_cast<float>(menu::vehicleGuiIndex(pilot)));
		hud.set("CrosshairIconShow", 0);
		game::SoldierInput input;
		input.switchSeat = 4;
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
		g.world->setPlayerInput({});
		g.run(1);
		CHECK(g.world->playerInfo(g.world->localPlayer(), me) && me.seat == 3);
		const auto held = world.heldWeapon();
		CHECK(held.weapon && held.gun);
		if (!held.weapon || !held.gun) return;
		const auto refreshHud = [&](bool active) {
			return menu::updateSoldierWeaponHud(hud, held.weapon, g.world->weaponDeviation(me.id), false, false, active);
		};
		CHECK(refreshHud(true) == menu::weaponGuiIndex(held.weapon, false));
		CHECK(hud.number("GuiIndex") > 0);
		CHECK(hud.number("GuiIndex") == menu::weaponGuiIndex(held.weapon, false));
		CHECK(hud.number("CrosshairIconShow") == 1);
		const float settledCrosshair = hud.number("CrossRightPos");
		refreshHud(false);
		CHECK(hud.number("CrosshairIconShow") == 0);
		refreshHud(true);
		CHECK(hud.number("CrosshairIconShow") == 1);

		anim::AnimationSystems systems(nullptr);
		PlayerCamera camera(*g.world, systems, nullptr);
		camera.tick(1 / 30.0f);
		game::GameWorld::LadderView before;
		CHECK(g.world->seatedSoldierView(me.id, before));
		const float hullYaw = g.objects.find(at)->rotation().x;
		float highest = 0, sideways = 0, modelRecoil = 0;
		input = {};
		input.fire = true;
		for (int tick = 0; tick < 20; ++tick) {
			g.world->setPlayerInput(input);
			g.world->tick(1 / 30.0f);
			camera.tick(1 / 30.0f);
			game::GameWorld::LadderView view;
			PlayerCamera::View eye;
			CHECK(g.world->seatedSoldierView(me.id, view));
			CHECK(camera.view(eye, 4.0f / 3.0f));
			CHECK(std::fabs(view.pitch - g.world->aimPitchOf(me.id)) < 0.001f);
			CHECK(std::fabs(eye.pitch * 57.29578f - view.pitch) < 0.001f);
			highest = std::max(highest, view.pitch - before.pitch);
			modelRecoil = std::max(modelRecoil, std::fabs(camera.recoil()));
			sideways = std::max(sideways, std::fabs(std::remainder(view.yaw - before.yaw
				- (g.objects.find(at)->rotation().x - hullYaw), 360.0f)));
		}
		std::fprintf(stderr, "  %s: passenger recoil %.3f up, %.3f sideways\n", mod, highest, sideways);
		CHECK(highest > 0.1f);
		CHECK(sideways > 0.001f);
		CHECK(modelRecoil > 0.01f);
		CHECK(std::fabs(g.world->takeAimKick(me.id)) > 0.01f);
		refreshHud(true);
		CHECK(hud.number("CrossRightPos") > settledCrosshair);
	}
}

// A passenger reloads his weapon from the seat.
TEST(VehicleModels_TransportPassengerReloads)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_at")) {
		return;
	}
	const uint32_t at = g.start("as_at", {0, 40, 6});
	CHECK(g.enter(at));
	game::SoldierInput input;
	input.switchSeat = 4;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(1);
	const game::FireArm* gun = g.world->playerWeapon();
	CHECK(gun != nullptr);
	if (!gun) {
		return;
	}
	input = {};
	input.fire = true;
	for (int tick = 0; tick < 12; ++tick) {
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
	}
	g.world->setPlayerInput({});
	const int spent = gun->roundsInMag();
	input = {};
	input.reload = true;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	const bool started = g.world->playerWeapon()->reloading();
	g.run(6);
	std::fprintf(stderr, "  magazine %d, reloading %d, then %d\n", spent, started ? 1 : 0, g.world->playerWeapon()->roundsInMag());
	CHECK(started);
	CHECK(!g.world->playerWeapon()->reloading() && g.world->playerWeapon()->roundsInMag() > spent);
	game::GameWorld::PlayerInfo me;
	CHECK(g.world->playerInfo(g.world->localPlayer(), me) && me.vehicle == at);
}

// A passenger's rounds leave the transport that carries him: they start inside
// its hull and must not meet it.
TEST(VehicleModels_TransportPassengerRoundsClearTheHull)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_at")) {
		return;
	}
	const uint32_t at = g.start("as_at", {0, 40, 6});
	CHECK(g.enter(at));
	game::SoldierInput input;
	input.switchSeat = 4;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(1);
	const float hp = g.world->hitPoints(at);
	input = {};
	input.fire = true;
	float farthest = 0;
	int seen = 0;
	for (int tick = 0; tick < 30; ++tick) {
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
		input.fire = tick < 6;
		const physics::Vec3 hull = g.position(at);
		for (const auto& p : g.world->projectiles()) {
			if (p.alive && p.player == g.world->localPlayer()) {
				++seen;
				farthest = std::max(farthest, std::sqrt(physics::lengthSquared(p.position - hull)));
			}
		}
	}
	std::fprintf(stderr, "  rounds seen %d, farthest %.1f m from the hull, hull %.0f -> %.0f\n", seen, farthest, hp,
		g.world->hitPoints(at));
	CHECK(seen > 0 && farthest > 20);
	CHECK(g.world->hitPoints(at) == hp);
}

// A passenger's round goes where his camera looks: from the seat's eye along
// the seated view (the head's twist on foot is left behind), carried along by
// the flying transport as a door gun's rounds are.
TEST(VehicleModels_TransportPassengerFiresWhereHeLooks)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_at")) {
		return;
	}
	const uint32_t at = g.start("as_at", {0, 40, 6}, 3, 0, -10000, 30);
	CHECK(g.world->spawnPlayer(2));
	game::SoldierInput input;
	input.turn = 30; // standing still: the head twists, the body stays
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	CHECK(g.enter(at, false));
	input = {};
	input.switchSeat = 4;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(1);
	input = {};
	input.turn = 20;
	input.aimPitch = 12; // over the ground the transport stands on
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	input.turn = 0;
	g.drive(input, 1); // the turn's deviation settles
	for (int tick = 0; tick < 3; ++tick) { // under way, as in flight
		CHECK(g.world->setBodyVelocity(at, {25, 0, -10}));
		g.world->tick(1 / 30.0f);
	}
	input.fire = true;
	CHECK(g.world->setBodyVelocity(at, {25, 0, -10}));
	const physics::Vec3 hull = g.position(at);
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	const physics::Vec3 carried = (g.position(at) - hull) * 30.0f;
	game::GameWorld::LadderView view;
	CHECK(g.world->seatedSoldierView(g.world->localPlayer(), view));
	const game::Projectile* round = nullptr;
	for (const auto& p : g.world->projectiles()) {
		if (p.alive && p.player == g.world->localPlayer()) {
			round = &p;
		}
	}
	CHECK(round != nullptr);
	if (!round) {
		return;
	}
	const float yaw = std::atan2(round->heading.x, round->heading.z) * 57.2958f;
	const float pitch = std::asin(std::clamp(round->heading.y, -1.0f, 1.0f)) * 57.2958f;
	const physics::Vec3 off = round->origin - view.position;
	const physics::Vec3 own = round->velocity - carried;
	const physics::Vec3 side = physics::cross(own, round->heading);
	std::fprintf(stderr, "  view %.1f/%.1f, round %.1f/%.1f, origin off %.2f %.2f %.2f, carried %.1f %.1f %.1f, side %.2f\n",
		view.yaw, view.pitch, yaw, pitch, off.x, off.y, off.z, carried.x, carried.y, carried.z,
		std::sqrt(physics::lengthSquared(side)));
	CHECK(std::fabs(std::remainder(yaw - view.yaw, 360.0f)) < 1);
	CHECK(std::fabs(pitch - view.pitch) < 1);
	CHECK(std::fabs(off.y) < 0.3f); // at the eye, not over the hull
	CHECK(std::sqrt(physics::lengthSquared(side)) < 0.01f * std::sqrt(physics::lengthSquared(own)));
}

// StickyCollisionComp (0x005e83d0): C4 (maxStickAngle 180, every stick
// flag 1) dropped on the tank sticks to it and rides it; the AP mine
// (stickToVehicles 0, maxStickAngle 45) bounces off the tank and sticks
// to the flat ground beside it.
TEST(Vehicles_StickyProjectilesRideVehicles)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t tank = g.start("eu_tank", {0, 2, 6});
	CHECK(g.enter(tank));
	const physics::Vec3 top = g.position(tank) + physics::Vec3 {0, 6, 0};
	CHECK(g.world->launchProjectile("Unl_c4_Projectile", top, {0, -4, 0}));
	g.run(2);
	const game::Projectile* c4 = nullptr;
	for (const auto& p : g.world->projectiles()) {
		c4 = p.settings && p.settings->name == "Unl_c4_Projectile" ? &p : c4;
	}
	CHECK(c4 && c4->settings->sticky && c4->settings->maxStickCos < -0.99f);
	CHECK(c4 && c4->stuck && c4->stuckTo == tank);
	if (!c4) {
		return;
	}
	const physics::Vec3 before = c4->position;
	const physics::Vec3 offset = before - g.position(tank);
	std::fprintf(stderr, "  c4 stuck at %.2f %.2f %.2f, %.2f m over the tank's origin\n", before.x, before.y, before.z,
		offset.y);
	CHECK(offset.y > 0.3f && offset.y < 4);
	game::SoldierInput input;
	input.throttle = 1;
	g.drive(input, 3);
	const physics::Vec3 after = c4->position;
	const physics::Vec3 moved = after - before;
	std::fprintf(stderr, "  the tank drove: c4 moved %.1f m, now %.2f m off its place on the tank\n",
		std::sqrt(physics::lengthSquared(moved)),
		std::sqrt(physics::lengthSquared(after - g.position(tank) - offset)));
	CHECK(physics::lengthSquared(moved) > 4 * 4);
	CHECK(c4->stuck && std::sqrt(physics::lengthSquared(after - g.position(tank))) < 4);

	// The AP mine on the same tank: stickToVehicles 0, so it ends on the ground.
	// Its trigger (MTYPco: vehicles too, 0x005e8aa0) is taken off here, or
	// the tank driving off would set it off.
	if (world::ObjectTemplate* apMine = g.templates.find("Unl_Mine_AntiPersonnel_Projectile")) {
		apMine->settings()["detonation.triggerType"] = "MTYNone";
	}
	g.drive({}, 3);
	const physics::Vec3 roof = g.position(tank) + physics::Vec3 {0, 6, 0};
	CHECK(g.world->launchProjectile("Unl_Mine_AntiPersonnel_Projectile", roof, {0, -4, 0}));
	g.run(4);
	const game::Projectile* mine = nullptr;
	for (const auto& p : g.world->projectiles()) {
		mine = p.settings && p.settings->name == "Unl_Mine_AntiPersonnel_Projectile" ? &p : mine;
	}
	CHECK(mine && mine->settings->sticky && !mine->settings->stickToVehicles
		&& std::fabs(mine->settings->maxStickCos - 0.7071f) < 0.001f);
	if (mine) {
		std::fprintf(stderr, "  ap mine at %.2f %.2f %.2f, stuck %d to %u\n", mine->position.x, mine->position.y,
			mine->position.z, mine->stuck, mine->stuckTo);
		CHECK(!mine->stuck && mine->stuckTo == 0);
		// Lying on the roof, it falls once the tank drives off, and sticks
		// to the flat ground (45 degrees at most).
		g.drive(input, 2);
		g.run(2);
		std::fprintf(stderr, "  the tank gone: ap mine at %.2f %.2f %.2f, stuck %d\n", mine->position.x, mine->position.y,
			mine->position.z, mine->stuck);
		CHECK(mine->stuck && mine->stuckTo == 0 && mine->position.y < 0.3f);
	}
}

// The seat's radio filter (soundFilter, the radio handler 0x0042e0c0) and
// neverDrawAs1p's first-person effects (0x006187c0): as_tank's driver
// talks through "vehicle"; its effects may take their first-person form,
// a Titan's (neverDrawAs1p) not.
TEST(Vehicles_RadioFilterAndFirstPersonEffects)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t tank = g.start("as_tank", {0, 2, 6});
	CHECK(g.enter(tank));
	const int me = g.world->localPlayer();
	CHECK(g.world->radioFilter(me) == "vehicle");
	CHECK(g.world->firstPersonEffects(tank));
	world::ObjectTemplate* titan = g.templates.find("eu_titan_wake");
	CHECK(titan && titan->settings().count("neverDrawAs1p"));
	g.templates.find("as_tank")->settings()["neverDrawAs1p"] = "1";
	CHECK(!g.world->firstPersonEffects(tank));
}

// c_PIReload in a seat: the player's update (0x00590e10) sends event 10 to
// his control object, the seat's PCO hands it down its tree (0x005cacf0 ->
// 0x0063ca60) and each active firearm reloads (0x005c62c0 -> 0x005c50e0
// case 10): eu_apc's second turret's grenade launcher (3 a magazine,
// 2.1 s) reloads a part-used magazine.
TEST(Vehicles_SeatReloadPress)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t apc = g.start("eu_apc", {0, 2, 6});
	CHECK(g.enter(apc));
	game::SoldierInput input;
	input.switchSeat = 2;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(3);
	game::GameWorld::PlayerInfo info;
	CHECK(g.world->playerInfo(g.world->localPlayer(), info) && info.seat == 1);
	const game::FireArm* launcher = g.world->gun(apc, 1);
	CHECK(launcher && launcher->settings().magSize == 3);
	if (!launcher) {
		return;
	}
	// Its fire mode is the 3-round burst (225 a minute: 8 ticks a round):
	// the first round out, the reload's press drops the rest of the burst.
	input = {};
	input.fire = true;
	for (int i = 0; i < 4 && launcher->roundsInMag() == 3; ++i) {
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
	}
	std::fprintf(stderr, "  launcher: %d in the magazine, reloading %d\n", launcher->roundsInMag(), launcher->reloading());
	CHECK(launcher->roundsInMag() == 2 && !launcher->reloading());
	input = {};
	input.reload = true;
	g.world->setPlayerInput(input);
	g.world->tick(1 / 30.0f);
	CHECK(launcher->reloading());
	g.world->setPlayerInput({});
	g.run(2.5f);
	CHECK(!launcher->reloading() && launcher->roundsInMag() == 3);
}

// A joined client's mech ghost (GameWorld::animateRemoteVehicle): the
// client's round has no occupants for it, so its bundleAnimationSystem is
// fed from the ghost (its movement and taken seats) as the exe's client
// runs every object's skeleton helper (0x0063d5b0 -> 0x0063d2f0 ->
// 0x0063d470): parked the torso sits low, manned (0x800) it stands up, as
// the round's own mech's does; the seats on the torso ride it (seatMount's
// bone, animateRemoteSeated).
TEST(Vehicles_RemoteMechPosesItsBoneParts)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_heavy_mech")) {
		return;
	}
	Animations a(g);
	g.start("eu_fav", {0, 4, 6}, 0.1f);
	a.attach(g);
	world::Object* ghost = g.objects.create(g.templates.find("as_heavy_mech"), {20, 0, 20}, {30, 0, 0});
	const uint32_t mech = ghost->id();
	anim::Matrix parked {}, standing {};
	CHECK(!g.world->partBone(mech, "AS_heavy_mech_torsospring", parked));
	for (int i = 0; i < 60; ++i) {
		g.world->animateRemoteVehicle(mech, {}, false, 1 / 30.0f);
		g.world->tick(1 / 30.0f);
	}
	CHECK(g.world->partBone(mech, "AS_heavy_mech_torsospring", parked));
	for (int i = 0; i < 90; ++i) {
		g.world->animateRemoteVehicle(mech, {}, true, 1 / 30.0f);
		g.world->tick(1 / 30.0f);
	}
	CHECK(g.world->partBone(mech, "AS_heavy_mech_torsospring", standing));
	std::fprintf(stderr, "  remote as_heavy_mech torso: parked y %.2f, manned y %.2f\n", parked[13], standing[13]);
	CHECK(standing[13] > parked[13] + 0.2f);
	// Gone with its object.
	g.objects.remove(mech);
	g.world->tick(1 / 30.0f);
	CHECK(!g.world->partBone(mech, "AS_heavy_mech_torsospring", parked));
}

// The rolling-surface loop (groundContactVolume 1; 0x005c8e70 with the
// contact sound object 0x005c65d0): the tank's tracks' web_tracked
// (material 61) on ground of material 5 loop the cell's
// S_GroundContact_Dirt, its envelopes on the speed eased by 0.08 a tick;
// at rest it eases out and pauses.
TEST(Vehicles_RollingSurfaceLoop)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	g.groundMaterial = 5;
	const uint32_t tank = g.start("eu_tank", {0, 2, 0});
	// Settled: what its drop onto the ground gave has eased away (or nearly).
	for (const auto& r : g.world->rollingSounds()) {
		CHECK(r.input < 0.01f);
	}
	CHECK(g.enter(tank));
	game::SoldierInput go;
	go.throttle = 1;
	g.drive(go, 3);
	const auto rolling = g.world->rollingSounds();
	CHECK(rolling.size() == 1);
	if (rolling.size() == 1) {
		std::fprintf(stderr, "  %s at %.3f\n", rolling[0].sound.c_str(), rolling[0].input);
		CHECK(rolling[0].vehicle == tank && rolling[0].sound == "S_GroundContact_Dirt");
		CHECK(rolling[0].input > 0.05f && rolling[0].input <= 1);
	}
	g.drive({}, 12);
	CHECK(g.world->rollingSounds().empty()); // eased to 0: paused
}

// The heavy IFV (eu_ifv): its seven ifv_shield_the_generator children are
// its shield cells (hudRepairCellNo 1, mirrorParentDamageLevel 1): all up
// at full armour, all down once the hull is a wreck, each one's
// VehicleShieldDestroyed with it.
TEST(Vehicles_HeavyIfvShieldCells)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("eu_ifv")) {
		return;
	}
	const uint32_t ifv = g.start("eu_ifv", {0, 2, 0});
	auto cells = g.world->repairCells(ifv);
	CHECK(cells.size() == 7 && std::all_of(cells.begin(), cells.end(), [](bool up) { return up; }));
	g.world->applyDamage(ifv, 600); // half
	g.run(0.5f);
	cells = g.world->repairCells(ifv);
	CHECK(cells.size() == 7 && std::all_of(cells.begin(), cells.end(), [](bool up) { return up; }));
	g.world->applyDamage(ifv, 1000);
	g.run(0.5f);
	cells = g.world->repairCells(ifv);
	CHECK(cells.size() == 7 && std::none_of(cells.begin(), cells.end(), [](bool up) { return up; }));
}

// The commander's supply crate (supply_crate, a Mesh body, under its
// supply_parachute Bundle: supply_parachuteWingMiddle's lift 200 and
// supply_ParachuteForceDown's constantForce 5, straight down,
// useAbsoluteDirection): its parachute's Wing holds the fall to a few
// metres a second (free fall would be past 30 m/s after 4 s) until it
// lands.
TEST(Vehicles_SupplyCrateComesDownUnderItsParachute)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t crate = g.start("supply_crate", {0, 60, 0}, 4);
	const physics::Vec3 a = g.position(crate);
	g.run(1);
	const physics::Vec3 b = g.position(crate);
	const float speed = a.y - b.y;
	std::fprintf(stderr, "  supply_crate: %.1f m up after 4 s, falling %.2f m/s\n", a.y, speed);
	CHECK(a.y > 20);
	CHECK(speed > 0.5f && speed < 15);
	g.run(30);
	CHECK(g.position(crate).y < 2);
}

// The soldier's parachute (Parachutes.cpp): falling faster than 10 m/s and
// 2 m clear of the ground, key 9 opens it (0x004d2a50, 0x005e4750); he
// rides it visible, posed by 3p_parachute_animSys, its wing and ForceDown
// slowing him; on the ground (0x005cf750) he is out 0.05 s later and the
// parachute goes 0.1 s after that (0x005cff20, 0x005cff90).
TEST(Vehicles_SoldierParachuteOpensFallsAndLands)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	g.start("supply_crate", {50, 0.5f, 50}, 0.5f);
	g.animate();
	CHECK(g.world->spawnPlayer(2));
	game::GameWorld::PlayerInfo me;
	g.world->playerInfo(g.world->localPlayer(), me);
	g.world->setSoldierPosition(me.soldier, {0, 150, 0});
	game::SoldierInput nine;
	nine.selectItem = 9;
	// Too slow at first: refused.
	g.drive({}, 0.3f);
	g.drive(nine, 1 / 30.0f);
	CHECK(g.world->parachutes().empty());
	g.drive({}, 1.2f); // past 10 m/s
	g.drive(nine, 1 / 30.0f);
	const auto open = g.world->parachutes();
	CHECK(open.size() == 1);
	if (open.size() != 1) {
		return;
	}
	g.world->playerInfo(g.world->localPlayer(), me);
	CHECK(me.vehicle == open[0].object && open[0].player == me.id);
	CHECK(g.world->ridingParachute(me.id));
	g.run(0.5f);
	CHECK(!g.objects.find(me.soldier)->hidden()); // he hangs under it, seen
	const auto playing = [&](const char* bundle) {
		const std::vector<std::string> names = g.world->playingAnimations(me.soldier);
		return std::find(names.begin(), names.end(), bundle) != names.end();
	};
	CHECK(playing("still")); // 3p_parachute_animSys's message 1
	// The use key doesn't let him out (dontAllowExit).
	game::SoldierInput use;
	use.use = true;
	g.drive(use, 2 / 30.0f);
	CHECK(g.world->ridingParachute(me.id));
	g.drive({}, 4);
	const physics::Vec3 a = g.position(open[0].object);
	g.run(1);
	const physics::Vec3 b = g.position(open[0].object);
	std::fprintf(stderr, "  parachute: %.1f m up, falling %.2f m/s\n", b.y, a.y - b.y);
	CHECK(a.y - b.y > 0.5f && a.y - b.y < 15);
	CHECK(g.world->parachutes()[0].state == 1); // high
	bool landing = false;
	for (int i = 0; i < 60 * 30 && !g.world->parachutes().empty(); ++i) {
		g.world->tick(1 / 30.0f);
		landing = landing || (!g.world->parachutes().empty() && g.world->parachutes()[0].state == 2 && playing("landing"));
	}
	CHECK(landing); // under 8 m: message 2
	CHECK(g.world->parachutes().empty());
	CHECK(g.objects.find(open[0].object) == nullptr);
	g.world->playerInfo(g.world->localPlayer(), me);
	CHECK(me.alive && me.vehicle == 0);
	CHECK(g.position(me.soldier).y < 3);
}
// The canopy (animatedparachute, an AnimatedBundle: 0x005c06a0 / 0x005c00e0)
// runs parachute/AnimationSystem.inc on the parachute's messages
// (0x005cfbb0, every 1/3 s): "startup" deploys it once, "root" loops the
// idle, and message 4 collapses it: the rider's death sets it at once and
// starts the landing timer (0x005cfe40), so the parachute is gone 0.15 s
// later (0x005cff20, then the event 0x21).
TEST(Vehicles_ParachuteCanopyDeploysIdlesAndCollapses)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	g.start("supply_crate", {50, 0.5f, 50}, 0.5f);
	g.animate();
	CHECK(g.world->spawnPlayer(2));
	game::GameWorld::PlayerInfo me;
	g.world->playerInfo(g.world->localPlayer(), me);
	g.world->setSoldierPosition(me.soldier, {0, 60, 0});
	game::SoldierInput nine;
	nine.selectItem = 9;
	g.drive({}, 1.5f);
	g.drive(nine, 1 / 30.0f);
	const auto open = g.world->parachutes();
	CHECK(open.size() == 1);
	if (open.size() != 1) {
		return;
	}
	const uint32_t chute = open[0].object;
	const auto playing = [&](const char* bundle) {
		const std::vector<std::string> names = g.world->playingAnimations(chute);
		return std::find(names.begin(), names.end(), bundle) != names.end();
	};
	g.world->tick(1 / 30.0f);
	CHECK(playing("parachute_deploy"));
	g.run(2);
	CHECK(playing("parachute_idle"));
	CHECK(!playing("parachute_collapse"));
	CHECK(g.world->parachutes()[0].state == 1);
	g.world->setHitPoints(me.soldier, 0);
	bool collapsed = false;
	int ticks = 0;
	for (; ticks < 30 && !g.world->parachutes().empty(); ++ticks) {
		g.world->tick(1 / 30.0f);
		collapsed = collapsed || playing("parachute_collapse");
	}
	std::fprintf(stderr, "  the canopy collapsed: %d, gone after %d ticks\n", collapsed, ticks);
	CHECK(collapsed);
	CHECK(g.world->parachutes().empty() && ticks <= 7);
	CHECK(g.objects.find(chute) == nullptr);
}

// The parachute's room (0x005e45e0 / 0x006f2760): the round's collision
// mesh (parachute.collisionmesh's vehicle LOD) placed at its start, 2 m over
// the feet; a ceiling a metre over the head crosses the segments to the
// canopy's top, one 4 m up doesn't.
TEST(Vehicles_ParachuteNeedsRoomForItsCanopy)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	g.start("supply_crate", {50, 0.5f, 50}, 0.5f);
	const world::ObjectTemplate* round = g.templates.find("parachute");
	CHECK(round != nullptr);
	if (!round) {
		return;
	}
	const std::vector<physics::Vec3> vertices = g.world->vehicleLodVertices(*round);
	float low = 1e9f, high = -1e9f;
	for (const physics::Vec3& v : vertices) {
		low = std::min(low, v.y);
		high = std::max(high, v.y);
	}
	std::fprintf(stderr, "  parachute mesh: %zu vertices, y %.2f..%.2f\n", vertices.size(), low, high);
	CHECK(!vertices.empty() && high > 0.5f && low < -2);
	const physics::Vec3 start {0, 102, 0}; // feet at 100
	const auto ceiling = [](float y) {
		return [y](const physics::Vec3& a, const physics::Vec3& b) { return (a.y - y) * (b.y - y) <= 0; };
	};
	CHECK(!game::canopyClear(start, {}, vertices, ceiling(102.8f)));
	CHECK(game::canopyClear(start, {}, vertices, ceiling(104)));
	CHECK(game::canopyClear(start, {}, vertices, ceiling(98.5f))); // below the feet: the 2 m ray's business
}

// The stationary guns (eu_bipod: a PlayerControlObject whose
// hasMobilePhysics is 0; Body::fixed): a soldier takes the seat at its
// entry point, sits there seen (its seatInformation names the bipod,
// 0x005cd4f0 / 0x005cdd60), turns its gun with the mouse and fires it; the
// bipod doesn't move, and its radius has boundingRadiusModifierWhenOccupied's
// 2 in it (0x005c6be0).
TEST(Vehicles_BipodIsAStationarySeat)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t bipod = g.start("eu_bipod", {0, 1.2f, 8}, 1); // its anchor 0/-1.109/0 on the ground
	g.animate();
	const physics::Vec3 at = g.position(bipod);
	CHECK(g.world->objectRadius(bipod) > 2);
	CHECK(g.enter(bipod));
	game::GameWorld::PlayerInfo me;
	g.world->playerInfo(g.world->localPlayer(), me);
	CHECK(me.vehicle == bipod && me.seat == 0);
	g.run(0.5f);
	CHECK(!g.objects.find(me.soldier)->hidden());
	float yaw0 = 0, pitch0 = 0, yaw1 = 0, pitch1 = 0;
	CHECK(g.world->gunAim(bipod, 0, yaw0, pitch0));
	game::SoldierInput turn;
	turn.turn = 2;
	g.drive(turn, 0.5f);
	CHECK(g.world->gunAim(bipod, 0, yaw1, pitch1));
	std::fprintf(stderr, "  eu_bipod: gun yaw %.1f -> %.1f\n", yaw0, yaw1);
	CHECK(std::fabs(yaw1 - yaw0) > 5);
	// The barrel rests 40 degrees up from eu_down's angle (restPitch): the
	// bundle (20..60 down) led to -40 by the look (stepGunner turns it at its
	// own speed) points the barrel level.
	float look = 0;
	seataim::steerGun([&](const game::SoldierInput& in) { g.world->setPlayerInput(in); g.world->tick(1 / 30.0f); },
		[&](float& y, float& p) { g.world->gunAim(bipod, 0, y, p); }, look, NAN, -40);
	game::SoldierInput level;
	level.aimPitch = look;
	g.drive(level, 0.2f);
	g.world->gunAim(bipod, 0, yaw1, pitch1);
	CHECK(std::fabs(pitch1 + 40) < 0.5f);
	// eu_down takes both inputs: its pose (what the drawing and the seat
	// camera's walk, PlayerCamera::findSeatCamera, put on it) holds the yaw
	// and the 40 down, which eu_bipod_toprotation's setRotation 0/-40/0
	// under it takes back: the camera under the barrel looks level.
	world::Vec3 offset, rotation;
	CHECK(g.world->partPose(bipod, "eu_down", offset, rotation));
	CHECK(std::fabs(rotation.y - 40) < 0.5f && std::fabs(std::remainder(rotation.x - yaw1, 360.0f)) < 0.5f);
	const size_t before = g.world->projectilesInFlight();
	game::SoldierInput fire = level;
	fire.fire = true;
	g.drive(fire, 0.2f);
	CHECK(g.world->projectilesInFlight() > before);
	const physics::Vec3 still = g.position(bipod);
	CHECK(std::fabs(still.x - at.x) < 1e-3f && std::fabs(still.y - at.y) < 1e-3f && std::fabs(still.z - at.z) < 1e-3f);
}

// The stationary AA cannon's muzzle ray must follow its live camera after
// the nested yaw and pitch mounts (static_aa's installed template tree).
TEST(Vehicles_StaticAAFireFollowsItsCamera)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!std::ifstream(std::string(gameDir()) + "/" + mod + "/ServerArchives.con")) continue;
		Garage g(gameDir(), mod);
		if (!g.loaded || !g.templates.find("static_aa")) continue;
		const uint32_t aa = g.start("static_aa", {0, 2, 8});
		g.animate();
		CHECK(g.enter(aa));
		const auto seats = g.world->vehicleSeats(aa);
		if (!seats.empty()) {
			menu::VehicleGuiInput gui;
			gui.controlled = g.templates.find(seats[0].name);
			gui.weapon = g.templates.find(seats[0].weapon);
			CHECK(menu::weaponGuiIndex(gui.weapon, false) == 27);
			gui.cameraMode = 1; // Inside
			CHECK(menu::vehicleGuiIndex(gui) == 27);
			gui.cameraMode = 2; // Nose
			CHECK(menu::vehicleGuiIndex(gui) == 27);
			gui.cameraMode = 3; // Chase
			CHECK(menu::vehicleGuiIndex(gui) == 0);
		}
		PlayerCamera camera(*g.world, *g.systems, nullptr);
		for (int i = 0; i < 60; ++i) {
			g.world->setPlayerInput({});
			g.world->tick(1 / 30.0f);
			camera.tick(1 / 30.0f);
		}
		const size_t before = g.world->projectiles().size();
		PlayerCamera::View sight;
		bool fired = false;
		for (int i = 0; i < 90 && !fired; ++i) {
			camera.tick(1 / 30.0f);
			CHECK(camera.view(sight, 16.0f / 9.0f));
			game::SoldierInput fire;
			fire.fire = true;
			g.world->setPlayerInput(fire);
			g.world->tick(1 / 30.0f);
			for (size_t n = before; n < g.world->projectiles().size(); ++n) {
				const game::Projectile& p = g.world->projectiles()[n];
				if (p.shooter != aa) continue;
				const float yaw = std::atan2(p.velocity.x, p.velocity.z);
				const float pitch = std::atan2(p.velocity.y, std::sqrt(p.velocity.x * p.velocity.x + p.velocity.z * p.velocity.z));
				const float error = std::acos(std::clamp(std::sin(sight.pitch) * std::sin(pitch)
					+ std::cos(sight.pitch) * std::cos(pitch) * std::cos(yaw - sight.yaw), -1.0f, 1.0f));
				CHECK(error < 5.0f * 3.14159265f / 180.0f);
				fired = true;
				break;
			}
		}
		CHECK(fired);
	}
}

// The stationary guns (static_av/aa/ap: hasRestrictedExit, hasMobilePhysics 0) are
// also in the static world; their own mesh there must not block their
// gunner's exit (the places' ray and room test, 0x005c7840).
TEST(Vehicles_StaticGunsLetTheirGunnerExit)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!std::ifstream(std::string(gameDir()) + "/" + mod + "/ServerArchives.con")) continue;
		for (const char* name : {"static_av", "static_aa", "static_ap"}) {
			Garage g(gameDir(), mod);
			if (!g.loaded || !g.templates.find(name)) continue;
			const uint32_t gun = g.start(name, {0, 0.5f, 8});
			CHECK(g.enter(gun));
			game::SoldierInput use;
			use.use = true;
			g.world->setPlayerInput(use);
			g.world->tick(1 / 30.0f);
			g.world->setPlayerInput({});
			g.run(0.5f);
			std::fprintf(stderr, "  %s %s: %s\n", mod, name, g.world->playerVehicle() ? "still in" : "out");
			CHECK(g.world->playerVehicle() == nullptr);
		}
	}
}

// A seated soldier's worn kit (helmet, vest, pack) is his child (0x00571d10
// addChild): it stands where the seat put him this tick, not a tick behind
// on a moving vehicle.
TEST(Vehicles_SeatedKitMovesWithItsWearer)
{
	if (!gameDir()) return;
	Garage g(gameDir(), "mods/bf2142");
	if (!g.loaded || !g.templates.find("eu_fav")) { CHECK(false); return; }
	const uint32_t fav = g.start("eu_fav", {0, 2, 6});
	g.animate();
	CHECK(g.enter(fav));
	game::SoldierInput go;
	go.throttle = 1;
	g.drive(go, 2);
	game::GameWorld::PlayerInfo me;
	CHECK(g.world->playerInfo(g.world->localPlayer(), me) && me.kit != 0);
	const world::Object* soldier = g.objects.find(me.soldier);
	const world::Object* kit = g.objects.find(me.kit);
	CHECK(soldier && kit);
	if (!soldier || !kit) return;
	const world::Vec3 a = soldier->position(), b = kit->position();
	const float gap = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
	const float speed = std::sqrt(me.velocity.x * me.velocity.x + me.velocity.z * me.velocity.z);
	std::fprintf(stderr, "  driver at %.2f m/s, kit %.3f m from him, hidden %d/%d\n", speed, gap, soldier->hidden(), kit->hidden());
	CHECK(speed > 3);
	CHECK(gap < 1e-3f);
	CHECK(kit->hidden() == soldier->hidden());
}

// Check the real stationary AA at multiple headings, including both fire groups.
TEST(Vehicles_StaticAAHeadingAndEmpMissileFlight)
{
    if (!gameDir()) return;
    for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
        for (float heading : {0.0f, 90.0f, 180.0f, -90.0f}) {
            for (bool missile : {false, true}) {
              for (float hullYaw : {0.0f, 90.0f}) {
               for (bool authoredSpread : {false, true}) {
                for (float aimPitch : {-30.0f, 0.0f, 20.0f}) {
                Garage g(gameDir(), mod);
                CHECK(g.loaded && g.templates.find("static_aa"));
                if (!g.loaded || !g.templates.find("static_aa")) continue;
                // Test both the authored spread (EMP missile minDev 10) and a
                // zero-spread fixture that isolates the underlying launch frame.
                auto* weapon = g.templates.find(missile ? "static_aa_missile_firearm" : "static_aa_cannon_firearm");
                CHECK(weapon != nullptr);
                if (!weapon) continue;
                if (!authoredSpread) weapon->settings()["deviation.minDev"] = "0";
                const uint32_t aa = g.start("static_aa", {0, 2, 8}, 3, 0, -10000, hullYaw);
                g.animate();
                CHECK(g.enter(aa));
                PlayerCamera camera(*g.world, *g.systems, nullptr);
                game::SoldierInput aim;
                aim.turn = heading;
                aim.aimPitch = aimPitch;
                g.world->setPlayerInput(aim);
                g.world->tick(1 / 30.0f);
                aim.turn = 0;
                for (int i = 0; i < 60; ++i) {
                    g.world->setPlayerInput(aim);
                    g.world->tick(1 / 30.0f);
                    camera.tick(1 / 30.0f);
                }
                PlayerCamera::View sight;
                CHECK(camera.view(sight, 16.0f / 9.0f));
                const size_t before = g.world->projectiles().size();
                bool fired = false;
                for (int i = 0; i < 90 && !fired; ++i) {
                    aim.fire = !missile;
                    aim.altFire = missile;
                    g.world->setPlayerInput(aim);
                    g.world->tick(1 / 30.0f);
                    for (size_t n = before; n < g.world->projectiles().size(); ++n) {
                        const auto& p = g.world->projectiles()[n];
                        if (p.shooter != aa) continue;
                        const float yaw = std::atan2(p.velocity.x, p.velocity.z);
                        const float pitch = std::atan2(p.velocity.y, std::hypot(p.velocity.x, p.velocity.z));
                        const float cosine = std::sin(sight.pitch) * std::sin(pitch)
                            + std::cos(sight.pitch) * std::cos(pitch) * std::cos(yaw - sight.yaw);
                        const float error = std::acos(std::clamp(cosine, -1.0f, 1.0f)) * 180.0f / 3.14159265f;
                        std::fprintf(stderr, "  AA %s turret %.0f hull %.0f %s spread=%d: sight yaw/pitch %.1f/%.1f projectile %.1f/%.1f error %.1f alive %d\n",
                            mod, heading, hullYaw, missile ? "missile" : "cannon", authoredSpread, sight.yaw * 180 / 3.14159265f,
                            sight.pitch * 180 / 3.14159265f, yaw * 180 / 3.14159265f, pitch * 180 / 3.14159265f, error, p.alive);
                        CHECK(error < (authoredSpread ? 30.0f : 2.0f));
                        CHECK(p.settings != nullptr);
                        if (p.settings) CHECK(p.settings->name == (missile ? "static_aa_missile_projectile" : "static_aa_cannon_Projectile"));
                        CHECK(p.alive);
                        const uint64_t serial = p.serial;
                        const physics::Vec3 launch = p.origin;
                        for (int step = 0; step < 12; ++step) {
                            g.world->setPlayerInput({});
                            g.world->tick(1 / 30.0f);
                        }
                        bool found = false;
                        for (const auto& later : g.world->projectiles()) {
                            if (later.serial != serial) continue;
                            found = true;
                            CHECK(later.alive);
                            CHECK(physics::lengthSquared(later.position - launch) > 100.0f);
                        }
                        CHECK(found);
                        CHECK(g.world->empLevel(aa) == 0);
                        fired = true;
                        break;
                    }
                }
                CHECK(fired);
                }
               }
              }
            }
        }
    }
}


// The walker's main cannon: the installed SimpleDeviationComp is low but
// nonzero (minDev 0.25), and each actual shot must follow the posed camera
// frame while retaining that configured spread.
TEST(Vehicles_WalkerMainGunUsesItsAimAndConfiguredDeviation)
{
	if (!gameDir()) return;
	const auto lowerName = [](std::string n) {
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return n;
	};
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!std::ifstream(std::string(gameDir()) + "/" + mod + "/ServerArchives.con")) continue;
		Garage g(gameDir(), mod);
		if (!g.loaded || !g.templates.find("as_heavy_mech")) continue;
		const uint32_t mech = g.start("as_heavy_mech", {0, 4, 8});
		g.animate();
		CHECK(g.enter(mech));
		PlayerCamera camera(*g.world, *g.systems, nullptr);
		constexpr float dt = 1 / 30.0f;
		for (int i = 0; i < 60; ++i) {
			g.world->setPlayerInput({});
			g.world->tick(dt);
			camera.tick(dt);
		}
		const auto seats = g.world->vehicleSeats(mech);
		CHECK(!seats.empty() && lowerName(seats[0].weapon) == "as_heavy_mech_main_cannon");
		if (!seats.empty()) {
			menu::VehicleGuiInput gui;
			gui.controlled = g.templates.find(seats[0].name);
			gui.weapon = g.templates.find(seats[0].weapon);
			CHECK(menu::weaponGuiIndex(gui.weapon, false) == 60);
			gui.cameraMode = 1; // Inside
			CHECK(menu::vehicleGuiIndex(gui) == 60);
			gui.cameraMode = 2; // Nose
			CHECK(menu::vehicleGuiIndex(gui) == 0);
			gui.cameraMode = 3; // Chase
			CHECK(menu::vehicleGuiIndex(gui) == 0);
		}
		const world::ObjectTemplate* weapon = g.templates.find("as_heavy_mech_main_cannon");
		CHECK(weapon && weapon->settings().count("deviation.minDev")
			&& weapon->settings().at("deviation.minDev") == "0.25");
		if (!weapon) continue;
		const size_t before = g.world->projectiles().size();
		size_t observed = before;
		bool checkedRay = false;
		std::vector<std::pair<float, float>> spread;
		for (int i = 0; i < 900; ++i) {
			camera.tick(dt);
			PlayerCamera::View sight;
			CHECK(camera.view(sight, 16.0f / 9.0f));
			game::SoldierInput fire;
			fire.fire = true;
			g.world->setPlayerInput(fire);
			g.world->tick(dt);
			const float cp = std::cos(sight.pitch), sp = std::sin(sight.pitch);
			const physics::Vec3 forward {std::sin(sight.yaw) * cp, sp, std::cos(sight.yaw) * cp};
			const physics::Vec3 right {std::cos(sight.yaw), 0, -std::sin(sight.yaw)};
			const physics::Vec3 up = physics::cross(forward, right);
			for (size_t n = observed; n < g.world->projectiles().size(); ++n) {
				const game::Projectile& p = g.world->projectiles()[n];
				if (p.shooter != mech) continue;
				const float speed = std::sqrt(physics::lengthSquared(p.velocity));
				if (speed < 1e-4f) continue;
				const physics::Vec3 ray = p.velocity * (1 / speed);
				const float along = physics::dot(ray, forward);
				if (!checkedRay) {
					const float error = std::acos(std::clamp(along, -1.0f, 1.0f));
					CHECK(error < 5.0f * 3.14159265f / 180.0f);
					checkedRay = true;
				}
				spread.emplace_back(std::atan2(physics::dot(ray, right), along), std::atan2(physics::dot(ray, up), along));
			}
			observed = g.world->projectiles().size();
		}
		CHECK(checkedRay && spread.size() >= 5);
		if (!spread.empty()) {
			double squares = 0;
			for (const auto& [right, up] : spread) squares += right * right + up * up;
			const float rmsDegrees = static_cast<float>(std::sqrt(squares / spread.size()) * 57.29578);
			CHECK(rmsDegrees > 0.01f && rmsDegrees < 1.0f);
		}
		// Zoomed, the component's deviation is 0 (0x005dedb0's +0x2c): the
		// rounds leave along the gun, one after another the same way.
		g.world->setZoomed(g.world->localPlayer(), true);
		std::vector<physics::Vec3> rays;
		for (int i = 0; i < 300; ++i) {
			game::SoldierInput fire;
			fire.fire = true;
			g.world->setPlayerInput(fire);
			g.world->tick(dt);
			for (size_t n = observed; n < g.world->projectiles().size(); ++n) {
				const game::Projectile& p = g.world->projectiles()[n];
				const float speed = std::sqrt(physics::lengthSquared(p.velocity));
				if (p.shooter == mech && p.age < 0.05f && speed > 1e-4f) rays.push_back(p.velocity * (1 / speed));
			}
			observed = g.world->projectiles().size();
		}
		CHECK(rays.size() >= 3); // the magazine is near its end after the volley above
		float widest = 0;
		for (size_t n = 1; n < rays.size(); ++n) {
			widest = std::max(widest, std::acos(std::clamp(physics::dot(rays[n], rays[n - 1]), -1.0f, 1.0f)) * 57.29578f);
		}
		std::fprintf(stderr, "  %s zoomed: %zu rounds, widest step %.4f degrees\n", mod, rays.size(), widest);
		CHECK(widest < 0.01f);
	}
}

// The heavy IFV's shield cells (SeatBehaviours.cpp): each
// ifv_shield_the_generator's mesh is the vehicle's for the rounds, so a
// round strikes the cell and hurts its armour (110 hit points), not the
// hull; the hit that brings it down raises VehicleShieldDestroyed (56)
// with the attacker. A teammate's hit (the IFV is team 2's, unmanned)
// is team damage on that hit only (damagePointOnDestroy: TeamDamage.cpp,
// 0x004c0e40): PlayerTeamDamagePoint for the scripts and event 0/0xb
// {1, no driver, the shooter} for him; an enemy's is none.
TEST(Vehicles_HeavyIfvShieldCellsTakeRounds)
{
	if (!gameDir()) {
		return;
	}
	for (const int team : {1, 2}) {
		Garage g(gameDir());
		if (!g.loaded || !g.templates.find("eu_ifv")) {
			return;
		}
		const uint32_t ifv = g.start("eu_ifv", {0, 2, 0}, 3);
		std::vector<std::string> events;
		g.world->setEventSink([&](const std::string& name, const game::ScriptArgs&) { events.push_back(name); });
		const int shooter = g.world->addPlayer("Shooter", team, true);
		const uint32_t cell = g.world->shieldCellId(ifv, 0);
		CHECK(cell != 0 && std::fabs(g.world->hitPoints(cell) - 110) < 1e-3f);
		const float hull = g.world->hitPoints(ifv);
		// The first cell: 3.0/-0.6/2.0 on its right side, facing out.
		const physics::Vec3 at = g.position(ifv);
		const physics::Vec3 aim = at + physics::Vec3 {3.0f, -0.6f, 2.0f};
		const world::ObjectTemplate* rifle = g.templates.find("Unl_AV_Rifle");
		const std::string round = rifle && rifle->settings().count("projectileTemplate") ? rifle->settings().at("projectileTemplate") : "";
		CHECK(!round.empty());
		bool down = false;
		int shots = 0;
		for (; shots < 200 && !down; ++shots) {
			g.world->launchProjectile(round, aim + physics::Vec3 {6, 0, 0}, {-300, 0, 0}, shooter);
			g.run(0.1f);
			down = g.world->hitPoints(cell) <= 0;
		}
		CHECK(down);
		CHECK(g.world->hitPoints(cell) <= 0 && g.world->hitPoints(ifv) > hull - 1);
		CHECK(std::find(events.begin(), events.end(), "VehicleShieldDestroyed") != events.end());
		const auto damage = g.world->takeTeamDamage(shooter);
		const size_t points = static_cast<size_t>(std::count(events.begin(), events.end(), "PlayerTeamDamagePoint"));
		std::fprintf(stderr, "  team %d: the cell down in %d shots, %zu team damage\n", team, shots, damage.size());
		if (team == 2) {
			CHECK(damage.size() == 1 && points == 1);
			CHECK(damage.size() == 1 && damage[0].kind == 1 && damage[0].attacker == shooter && damage[0].victim == -1
				&& damage[0].object == ifv);
			CHECK(g.world->takeTeamDamage(shooter).empty()); // taken once
		} else {
			CHECK(damage.empty() && points == 0);
		}
		const std::vector<bool> cells = g.world->repairCells(ifv);
		CHECK(!cells.empty() && !cells[0]);
	}
}

// The cells' armour effects (0x006549a0 / 0x00654730 on each cell's
// armour): addArmorEffect 99 e_healing_node (its SimpleObject
// ifv_shield_the_generatoreffect, a glow on the cell) while a cell is under
// 100 %, through the mirror of the hull's damage; gone when it is full
// again; a wreck takes -1's e_bluespark_loop, which no archive ships
// (e_bluesparks_loop is), so nothing.
TEST(Vehicles_HeavyIfvShieldCellsShowArmourEffects)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("eu_ifv")) {
		return;
	}
	const uint32_t ifv = g.start("eu_ifv", {0, 2, 0}, 3);
	const auto glows = [&] {
		std::vector<game::GameWorld::PlacedMesh> meshes;
		g.world->armorEffectMeshes(meshes);
		std::vector<physics::Vec3> out;
		for (const auto& m : meshes) {
			if (m.objectTemplate && m.objectTemplate->name() == "ifv_shield_the_generatoreffect") {
				out.push_back(m.position);
			}
		}
		return out;
	};
	CHECK(glows().empty());
	const float hull = g.world->hitPoints(ifv);
	g.world->setHitPoints(ifv, hull * 0.9f);
	g.run(0.5f);
	const uint32_t cell = g.world->shieldCellId(ifv, 0);
	const auto damaged = glows();
	std::fprintf(stderr, "  cell 0 at %.1f hit points, %zu glows\n", g.world->hitPoints(cell), damaged.size());
	CHECK(g.world->hitPoints(cell) < 110 && g.world->hitPoints(cell) > 90);
	CHECK(damaged.size() == 7);
	const physics::Vec3 at = g.position(ifv);
	for (const physics::Vec3& p : damaged) {
		CHECK(physics::lengthSquared(p - at) < 36);
	}
	g.world->setHitPoints(ifv, hull);
	g.run(0.5f);
	CHECK(std::fabs(g.world->hitPoints(cell) - 110) < 1e-2f);
	CHECK(glows().empty());
	// Down to a wreck: no glow, and no spark either.
	g.world->setHitPoints(ifv, hull * 0.9f);
	g.run(0.5f);
	const int shooter = g.world->addPlayer("Shooter", 1, true);
	const world::ObjectTemplate* rifle = g.templates.find("Unl_AV_Rifle");
	const std::string round = rifle && rifle->settings().count("projectileTemplate") ? rifle->settings().at("projectileTemplate") : "";
	const physics::Vec3 aim = at + physics::Vec3 {3.0f, -0.6f, 2.0f};
	for (int shots = 0; shots < 50 && g.world->hitPoints(cell) > 0; ++shots) {
		g.world->launchProjectile(round, aim + physics::Vec3 {6, 0, 0}, {-300, 0, 0}, shooter);
		g.run(0.1f);
	}
	g.run(0.2f);
	std::fprintf(stderr, "  the wreck at %.1f hit points, %zu glows\n", g.world->hitPoints(cell), glows().size());
	CHECK(g.world->hitPoints(cell) < 0 && g.world->hitPoints(cell) >= -10);
	CHECK(glows().size() == 6);
	CHECK(!g.world->effects().emitting(cell));
}

// A joined client's cells (0x005ffec0): the host captures each cell's hit
// points in eighths of its maximum (floor, at least 1 while any are left,
// 0 at none) and the client sets them back (e x max / 8, a wreck's
// -wreckHitPoints at 0) through the armour's setter, whose effects then
// show there as on the host. Here the second IFV stands for the client's
// copy of the first: its own hull is whole, its cells take the first's.
TEST(Vehicles_JoinedShieldCellsShowArmourEffects)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("eu_ifv")) {
		return;
	}
	world::Object* copy = g.objects.create(g.templates.find("eu_ifv"), {30, 2, 0}, {0, 0, 0});
	copy->setTeam(2);
	const uint32_t remote = copy->id();
	const uint32_t ifv = g.start("eu_ifv", {0, 2, 0}, 3);
	const auto glowsNear = [&](uint32_t vehicle) {
		std::vector<game::GameWorld::PlacedMesh> meshes;
		g.world->armorEffectMeshes(meshes);
		size_t n = 0;
		for (const auto& m : meshes) {
			if (m.objectTemplate && m.objectTemplate->name() == "ifv_shield_the_generatoreffect"
				&& physics::lengthSquared(m.position - g.position(vehicle)) < 36) {
				++n;
			}
		}
		return n;
	};
	CHECK(g.world->repairCellEighths(ifv) == std::vector<int>(7, 8));
	g.world->setHitPoints(ifv, g.world->hitPoints(ifv) * 0.9f);
	g.run(0.5f);
	const std::vector<int> eighths = g.world->repairCellEighths(ifv);
	std::fprintf(stderr, "  cell 0 at %.1f of 110 hit points: %d eighths\n", g.world->hitPoints(g.world->shieldCellId(ifv, 0)),
		eighths.empty() ? -1 : eighths[0]);
	CHECK(eighths == std::vector<int>(7, 7));
	CHECK(glowsNear(remote) == 0);
	g.world->setRemoteRepairCells(remote, eighths);
	g.run(0.5f);
	const uint32_t cell = g.world->shieldCellId(remote, 0);
	CHECK(std::fabs(g.world->hitPoints(cell) - 110 * 7 / 8.0f) < 1e-3f); // not mirrored back to its whole hull
	CHECK(glowsNear(remote) == 7);
	// Whole again: the glow goes; a wreck (0 eighths): the wreck's hit points.
	g.world->setRemoteRepairCells(remote, std::vector<int>(7, 8));
	g.run(0.2f);
	CHECK(glowsNear(remote) == 0);
	g.world->setRemoteRepairCells(remote, {0, 8, 8, 8, 8, 8, 8});
	g.run(0.2f);
	CHECK(std::fabs(g.world->hitPoints(cell) + 10) < 1e-3f);
	CHECK(g.world->repairCells(remote).size() == 7 && !g.world->repairCells(remote)[0]);
	// A sliver left is still one eighth.
	g.world->setRemoteRepairCells(remote, {1, 8, 8, 8, 8, 8, 8});
	CHECK(g.world->repairCellEighths(remote)[0] == 1);
}

namespace {

// Horizontal rays across a vehicle (along its x, every half metre of its
// z from -2 to 2) at heights over its origin from `low` to `high`: at how
// many heights one meets it, and the first and last.
struct Probe {
	int hits = 0;
	float lowest = 1e9f, highest = -1e9f;
};
Probe probeAcross(Garage& g, uint32_t vehicle, float low, float high)
{
	Probe probe;
	const world::Object* o = g.objects.find(vehicle);
	const physics::Mat3 hull = physics::Mat3::fromYawPitchRoll({o->rotation().x, o->rotation().y, o->rotation().z});
	const physics::Vec3 at = g.position(vehicle);
	for (float h = low; h <= high; h += 0.25f) {
		for (float z = -2; z <= 2; z += 0.5f) {
			const physics::Vec3 from = at + hull.transform({-12, h, z});
			const physics::Vec3 to = at + hull.transform({12, h, z});
			float fraction = 0;
			uint32_t object = 0;
			if (g.world->segmentHit(from, to, 0, fraction, object) && object == vehicle) {
				++probe.hits;
				probe.lowest = std::min(probe.lowest, h);
				probe.highest = std::max(probe.highest, h);
				break;
			}
		}
	}
	return probe;
}

} // namespace

// The walker's parts (0x0063ccc0): its mesh's geometry 0 is one small
// face and the parts' collisionPart geometries are its shape (the legs'
// bones, the torso on its spring, the turret and guns on their bundles).
// Rays across it meet the legs and the torso standing, and again after it
// has walked (the parts posed where they stand, PartCollision.cpp); the
// tank's hull and turret as before.
TEST(Vehicles_RoundsMeetTheWalkersParts)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t mech = g.start("us_heavy_mech", {0, 4, 6});
	g.animate();
	g.run(1);
	// The feet stand about 3.2 m under the origin, the torso above it.
	const Probe legs = probeAcross(g, mech, -2.75f, -0.75f);
	const Probe torso = probeAcross(g, mech, 0.0f, 2.0f);
	std::fprintf(stderr, "  us_heavy_mech parked: legs %d rays (%.2f..%.2f), torso %d rays (%.2f..%.2f)\n", legs.hits,
		legs.lowest, legs.highest, torso.hits, torso.lowest, torso.highest);
	CHECK(legs.hits >= 8 && torso.hits >= 8);
	CHECK(g.enter(mech));
	const physics::Vec3 start = g.position(mech);
	game::SoldierInput walk;
	walk.throttle = 1;
	g.drive(walk, 4);
	const physics::Vec3 now = g.position(mech);
	const float walked = std::sqrt(physics::lengthSquared(now - start));
	const Probe walkingLegs = probeAcross(g, mech, -2.75f, -0.75f);
	const Probe walkingTorso = probeAcross(g, mech, 0.0f, 2.0f);
	std::fprintf(stderr, "  walked %.1f m: legs %d rays, torso %d rays\n", walked, walkingLegs.hits, walkingTorso.hits);
	CHECK(walked > 5);
	CHECK(walkingLegs.hits >= 8 && walkingTorso.hits >= 8);
	// A ray at the knees' height (the legs reach back from the hips) meets
	// a leg out at its side (the left or, mid-stride, the right), not the
	// middle (where the root's one face lies).
	const world::Object* o = g.objects.find(mech);
	const physics::Mat3 hull = physics::Mat3::fromYawPitchRoll({o->rotation().x, o->rotation().y, o->rotation().z});
	float fraction = 0;
	uint32_t object = 0;
	CHECK(g.world->segmentHit(now + hull.transform({-12, -1.5f, -0.75f}), now + hull.transform({12, -1.5f, -0.75f}), 0,
		fraction, object));
	const float x = -12 + 24 * fraction;
	std::fprintf(stderr, "  the knee-high ray meets it at x %.2f\n", x);
	CHECK(object == mech && std::fabs(x) > 1.0f);

	Garage tank(gameDir());
	const uint32_t t = tank.start("eu_tank", {0, 2, 6});
	const Probe hull2 = probeAcross(tank, t, -0.5f, 2.0f);
	CHECK(hull2.hits >= 6 && hull2.highest > 0.9f); // the turret over the hull
}

// An anti-vehicle rocket (eu_av's) at the walker's leg (us_heavy_mech:
// the root's armour, 1000 hit points; the leg's faces are material "legs"
// 4201 through the root's mapMaterial) hurts the walker: a part's hit is its root's (0x004c0e40,
// the nearest armoured ancestor).
// The parts in the mesh physics (each collisionPart child's response in
// its root's chain, 0x005f6a60 / 0x006c0090): a soldier meets a part's
// soldier LOD where the part is now (0x006c3890). Dropped 2 m to the
// tank's left he lands on the hull's edge (0.7 m) while the turret looks
// ahead, and on the turret's roof (1.8 m) once it has turned 90 degrees
// over that side; dropped on the turret at rest he stands on its roof, not
// on the hull under it.
TEST(Vehicles_SoldierStandsOnTheTurnedTurret)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("eu_tank")) {
		return;
	}
	const uint32_t tank = g.start("eu_tank", {0, 2, 0});
	CHECK(g.enter(tank));
	const physics::Vec3 at = g.position(tank);
	const int id = g.world->addPlayer("Dropped", 2, true);
	CHECK(g.world->spawnSoldier(id));
	game::GameWorld::PlayerInfo info;
	const auto drop = [&](float x, float z) {
		g.world->playerInfo(id, info);
		g.world->setSoldierPosition(info.soldier, at + physics::Vec3 {x, 4, z});
		g.run(1.5f);
		g.world->playerInfo(id, info);
		return info.position.y - at.y;
	};
	const float roof = drop(0, -3);
	const float side = drop(-2, 0);
	game::SoldierInput look;
	look.turn = 3; // degrees a tick
	g.drive(look, 1.0f);
	g.drive({}, 0.5f);
	float yaw = 0, pitch = 0;
	CHECK(g.world->gunAim(tank, 0, yaw, pitch));
	const float turned = drop(-2, 0);
	std::fprintf(stderr, "  on the turret at rest %.2f; 2 m to the left %.2f, after it turned %.0f degrees %.2f\n", roof, side,
		yaw, turned);
	CHECK(roof > 1.4f);
	CHECK(side < 1.0f);
	CHECK(turned > 1.4f);
}

// A soldier standing by a tank carries the tank's soldier LOD into the next
// tick (carry() reads the movers the last gather left); the tank leaving
// between the two ticks must not leave the soldier a freed mesh to ray.
// (Under DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib it crashes without the movers' keepalive.)
TEST(Vehicles_SoldierSurvivesTheObjectUnderItLeaving)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("eu_tank")) {
		return;
	}
	const uint32_t tank = g.start("eu_tank", {0, 2, 0});
	const physics::Vec3 at = g.position(tank);
	const int id = g.world->addPlayer("Standing", 2, true);
	CHECK(g.world->spawnSoldier(id));
	game::GameWorld::PlayerInfo info;
	g.world->playerInfo(id, info);
	g.world->setSoldierPosition(info.soldier, at + physics::Vec3 {0, 4, -3});
	g.run(1.5f); // on the roof: the movers hold the tank's mesh
	g.objects.remove(tank);
	g.run(0.5f);
	g.world->playerInfo(id, info);
	CHECK(info.alive);
}

TEST(Vehicles_RocketHurtsTheWalker)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t mech = g.start("us_heavy_mech", {0, 4, 6});
	const int shooter = g.world->addPlayer("Shooter", 1, true);
	const world::ObjectTemplate* launcher = g.templates.find("eu_av");
	const std::string round = launcher && launcher->settings().count("projectileTemplate")
		? launcher->settings().at("projectileTemplate")
		: "";
	CHECK(!round.empty());
	const float before = g.world->hitPoints(mech);
	const world::Object* o = g.objects.find(mech);
	const physics::Mat3 hull = physics::Mat3::fromYawPitchRoll({o->rotation().x, o->rotation().y, o->rotation().z});
	// From 20 m off its left side at the knee's height, flying across.
	const physics::Vec3 at = g.position(mech) + hull.transform({-20, -1.5f, -0.75f});
	CHECK(g.world->launchProjectile(round, at, hull.transform({60, 0, 0}), shooter));
	g.run(1);
	const float after = g.world->hitPoints(mech);
	std::fprintf(stderr, "  us_heavy_mech: %.0f -> %.0f hit points\n", before, after);
	CHECK(after < before - 10);
}

// A wreck's mesh instances (0x006539f0(object, 1)): the root's turns to
// its wreck sub-geometry (us_heavy_mech's whole wreck shape) and the
// parts, with one sub-geometry, leave the rays.
TEST(Vehicles_WalkerWreckMeetsRaysAsItsWreckShape)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t mech = g.start("us_heavy_mech", {0, 4, 6});
	g.run(0.5f);
	g.world->applyDamage(mech, 1100);
	g.run(0.2f);
	CHECK(g.world->hitPoints(mech) <= 0);
	const Probe wreck = probeAcross(g, mech, -3.5f, 3.0f);
	std::fprintf(stderr, "  us_heavy_mech wreck: %d rays (%.2f..%.2f)\n", wreck.hits, wreck.lowest, wreck.highest);
	CHECK(wreck.hits >= 4);
}

// A decal on a walker's leg (0x005a36d0: the hit object's own decal set
// and mesh instance; the leg, a collisionPart child, is an object of its
// own with both): it lies in the leg's space, clipped to the leg's faces,
// and goes where the leg goes as the walker walks.
TEST(Vehicles_DecalOnTheWalkersLegRidesIt)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t mech = g.start("us_heavy_mech", {0, 4, 6});
	g.animate();
	g.run(1);
	CHECK(g.world->collisionPartCount(mech) > 10);
	const int shooter = g.world->addPlayer("Shooter", 1, true);
	const world::ObjectTemplate* rifle = g.templates.find("eu_ar_rifle");
	const std::string round = rifle && rifle->settings().count("projectileTemplate")
		? rifle->settings().at("projectileTemplate")
		: "";
	CHECK(!round.empty());
	const world::Object* o = g.objects.find(mech);
	const physics::Mat3 hull = physics::Mat3::fromYawPitchRoll({o->rotation().x, o->rotation().y, o->rotation().z});
	const physics::Vec3 at = g.position(mech) + hull.transform({-20, -1.5f, -0.75f});
	CHECK(g.world->launchProjectile(round, at, hull.transform({600, 0, 0}), shooter));
	g.run(0.2f);
	const effects::Decal* decal = nullptr;
	for (const effects::Decal& d : g.world->effects().decals()) {
		if (d.object == mech && d.part >= 0) {
			decal = &d;
		}
	}
	CHECK(decal != nullptr);
	if (!decal) {
		return;
	}
	const int part = decal->part;
	const effects::Vec3 local = decal->position;
	// On the leg's own faces, in the leg's space.
	const physics::CollisionWorld* faces = g.world->partCollision(mech, part);
	int near = 0;
	if (faces) {
		faces->forEachFaceNear(local, 0.25f,
			[&](const physics::Vec3&, const physics::Vec3&, const physics::Vec3&, uint32_t, uint32_t) { ++near; });
	}
	CHECK(near > 0);
	physics::Mat3 r0, r1;
	physics::Vec3 t0, t1;
	CHECK(g.world->collisionPartFrame(mech, part, r0, t0));
	CHECK(g.enter(mech));
	game::SoldierInput walk;
	walk.throttle = 1;
	g.drive(walk, 2);
	CHECK(g.world->collisionPartFrame(mech, part, r1, t1));
	const float moved = std::sqrt(physics::lengthSquared(t1 - t0)) + std::fabs(r1.rows[2].y - r0.rows[2].y)
		+ std::fabs(r1.rows[1].z - r0.rows[1].z);
	std::fprintf(stderr, "  the decal on part %d (%.2f, %.2f, %.2f), %d faces near; the part moved %.2f\n", part, local.x,
		local.y, local.z, near, moved);
	CHECK(moved > 0.05f);
	bool kept = false;
	for (const effects::Decal& d : g.world->effects().decals()) {
		kept = kept || (d.object == mech && d.part == part);
	}
	CHECK(kept);
}

// The wreck's instance (0x006539f0 -> 0x006e5df0): every LOD query goes
// through sub-geometry 1, so the wrecked tank's vehicle LOD (what the
// ground pass and the other movers meet) is the wreck's shape (eu_tank:
// 34 faces whole, 20 wrecked).
TEST(Vehicles_WreckMeetsMoversAsItsWreckShape)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t tank = g.start("eu_tank", {0, 2, 6});
	const physics::CollisionWorld* whole = g.world->vehicleCollision(tank);
	CHECK(whole != nullptr);
	const size_t before = whole ? whole->triangles() : 0;
	g.world->applyDamage(tank, 1e5f);
	g.run(0.5f);
	const physics::CollisionWorld* wreck = g.world->vehicleCollision(tank);
	const size_t after = wreck ? wreck->triangles() : 0;
	std::fprintf(stderr, "  eu_tank vehicle LOD: %zu faces, wrecked %zu, at %.2f m\n", before, after, g.position(tank).y);
	CHECK(g.world->hitPoints(tank) <= 0);
	CHECK(before > 0 && after > 0 && after != before);
	g.run(2);
	CHECK(g.position(tank).y > -1); // it still stands on the ground
}

// A static object's collisionPart children (0x0063d720 -> 0x0063ccc0 with
// its mesh): missile_silo_v2's base and barrel are geometries 1 and 2 of
// its mesh, in the static world where their templates place them.
TEST(Vehicles_StaticObjectsCollisionParts)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("missile_silo_v2")) {
		return;
	}
	world::ObjectTemplate* silo = g.templates.find("missile_silo_v2");
	const uint32_t id = g.objects.create(silo, {40, 0, 40}, {0, 0, 0})->id();
	g.start("eu_tank", {0, 2, 6}, 0.1f);
	const auto name = silo->settings().find("collisionMesh");
	const physics::CollisionMeshFile* mesh = name != silo->settings().end() ? g.collisionTemplates.mesh(name->second, g.files) : nullptr;
	CHECK(mesh && mesh->geometries.size() > 2);
	if (!mesh) {
		return;
	}
	physics::CollisionWorld rootOnly;
	rootOnly.addMesh(*mesh, physics::CollisionUser::Projectile, {}, {40, 0, 40}, id);
	size_t inWorld = 0;
	g.world->staticCollision(); // (soldier LOD)
	const physics::CollisionWorld* statics = g.world->projectileCollision(0);
	CHECK(statics != nullptr);
	if (statics) {
		statics->forEachFaceNear({40, 0, 40}, 60,
			[&](const physics::Vec3&, const physics::Vec3&, const physics::Vec3&, uint32_t, uint32_t owner) {
				inWorld += owner == id ? 1 : 0;
			});
	}
	std::fprintf(stderr, "  missile_silo_v2: %zu projectile faces, the root's own %zu\n", inWorld, rootOnly.triangles());
	CHECK(inWorld > rootOnly.triangles());
}

// The static AV gun (static_av: stationary, Body::fixed): its
// seatInformation names Static_AV_Cockpit, so its gunner is shown sitting
// on the cockpit (0x005cd4f0 / 0x005c6670 / 0x005cdd60) and turns with it
// as the gun yaws.
TEST(Vehicles_StaticGunnerSitsOnTheCockpit)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t gun = g.start("static_av", {0, 0.5f, 8}, 1);
	g.animate();
	const auto seats = g.world->vehicleSeats(gun);
	CHECK(!seats.empty() && seats[0].seatShown && seats[0].seatAnchor.yawSeat == 0);
	CHECK(g.enter(gun));
	game::GameWorld::PlayerInfo me;
	g.world->playerInfo(g.world->localPlayer(), me);
	g.run(0.5f);
	CHECK(!g.objects.find(me.soldier)->hidden());
	const float before = g.yaw(me.soldier);
	// Static_AV_Cockpit turns 15 degrees a second a PIMouseLook unit
	// (stepGunner): a unit a tick for a second, then it settles.
	game::SoldierInput turn;
	turn.turn = 5;
	g.drive(turn, 1.0f);
	g.drive({}, 0.2f);
	float yaw = 0, pitch = 0;
	CHECK(g.world->gunAim(gun, 0, yaw, pitch));
	const float after = g.yaw(me.soldier);
	std::fprintf(stderr, "  static_av: gun yaw %.1f, gunner %.1f -> %.1f\n", yaw, before, after);
	CHECK(std::fabs(yaw) > 5 && std::fabs(std::remainder(after - before - yaw, 360.0f)) < 1);
}

TEST(PlayerCamera_ReloadAndWalkerWithGameAssets)
{
	if (!gameDir()) return;
	Garage g(gameDir());
	if (!g.loaded) return;
	g.files.setMountingAllowed(true);
	std::string out;
	g.console.run("ClientArchives.con", {}, out);
	g.files.setMountingAllowed(false);
	anim::AnimationSystems systems([&](const std::string& path, std::vector<uint8_t>& bytes) { return g.files.readFile(path, bytes); });
	anim::registerAnimationConsoleMethods(g.console, systems);
	g.console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, out);
	const auto load = [&](const std::string& path) { return systems.get(path, [&] { g.console.run(path, {}, out); }); };
	const uint32_t walker = g.start("as_heavy_mech", {0, 4, 30});
	g.world->setAnimationSystems(&systems, load);
	CHECK(g.world->spawnPlayer(2));
	PlayerCamera camera(*g.world, systems, load);
	const auto step = [&](game::SoldierInput input = {}) {
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
		camera.tick(1 / 30.0f);
	};
	for (int i = 0; i < 90; ++i) step();
	camera.setAltFire(true);
	step();
	camera.setAltFire(false);
	for (int i = 0; i < 30; ++i) step();
	CHECK(camera.zoomLevel() > 0);
	game::SoldierInput fire;
	fire.fire = true;
	for (int i = 0; i < 10; ++i) step(fire);
	game::SoldierInput reload;
	reload.reload = true;
	step(reload);
	CHECK(g.world->playerWeapon()->reloading());
	CHECK(camera.zoomLevel() == 0);
	for (int i = 0; i < 15; ++i) step();
	const auto playing = camera.playing();
	CHECK(std::find(playing.begin(), playing.end(), "reload") != playing.end());
	for (int i = 0; i < 150; ++i) step();
	CHECK(!g.world->playerWeapon()->reloading());
	const auto finished = camera.playing();
	CHECK(std::find(finished.begin(), finished.end(), "reload") == finished.end());
	CHECK(g.enter(walker));
	for (int i = 0; i < 90; ++i) step();
	PlayerCamera::View inside;
	CHECK(camera.view(inside, 16.0f / 9.0f));
	PlayerCamera::Cockpit cockpit;
	CHECK(camera.cockpit(cockpit) && cockpit.vehicle == walker && cockpit.subGeometry == 0);
	CHECK(inside.position[1] > g.objects.find(walker)->position().y);
	camera.toggleMode();
	CHECK(!camera.insideView() && camera.mode() != game::camera::Mode::Inside);
	CHECK(camera.selectView(1));
	CHECK(camera.insideView());
}

TEST(PlayerCamera_TankZoomRetainsShortClicks)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!std::ifstream(std::string(gameDir()) + "/" + mod + "/ServerArchives.con")) continue;
		for (const char* name : {"eu_tank", "as_tank"}) {
			Garage g(gameDir(), mod);
			if (!g.loaded) return;
			const auto tank = g.start(name, {0, 2, 6});
			CHECK(g.enter(tank));
			anim::AnimationSystems systems(nullptr);
			PlayerCamera camera(*g.world, systems, nullptr);
			const auto step = [&] {
				g.world->setPlayerInput({});
				g.world->tick(1 / 30.0f);
				camera.tick(1 / 30.0f);
			};
			for (int i = 0; i < 60; ++i) step();
			const float wide = camera.fieldOfView();
			camera.setAltFire(true);
			step();
			camera.setAltFire(false);
			for (int i = 0; i < 30; ++i) step();
			const float zoomed = camera.fieldOfView();
			std::fprintf(stderr, "  %s %s zoom level %d: fov %.4f -> %.4f\n", mod, name, camera.seatZoomLevel(), wide, zoomed);
			CHECK(camera.seatZoomLevel() == 1);
			CHECK(zoomed < wide * 0.75f);
			camera.setAltFire(true);
			step();
			camera.setAltFire(false);
			for (int i = 0; i < 30; ++i) step();
			CHECK(camera.seatZoomLevel() == 0);
			CHECK(near(camera.fieldOfView(), wide));
			// A mouse press/release can occur between 30 Hz camera ticks.
			camera.setAltFire(true);
			camera.setAltFire(false);
			for (int i = 0; i < 30; ++i) step();
			CHECK(camera.seatZoomLevel() == 1);
			CHECK(camera.fieldOfView() < wide * 0.75f);
		}
	}
}

// The gunships' pilot gun (MultiFireComp, fire.addFireRate 0 = single fire, mag 10, ammo.autoReload),
// vanilla and Remaster alike (Remaster adds only a DefaultZoomComp, which the world never sees): a held
// trigger fires one round, each fresh press one more, an emptied magazine reloads itself and the
// next press fires again. Nothing else locks the gun.
TEST(Vehicles_GunshipPilotGunFiresPerPressAndReloads)
{
	if (!gameDir()) {
		return;
	}
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* name : {"as_ag", "us_ag"}) {
			Garage g(gameDir(), mod);
			if (!g.loaded || !g.templates.find(name)) {
				continue;
			}
			const uint32_t v = g.start(name, {0, 4, 6});
			CHECK(g.enter(v));
			const int me = g.world->localPlayer();
			const auto* gun = g.world->gun(v, 0);
			CHECK(gun != nullptr);
			if (!gun) {
				continue;
			}
			CHECK(gun->fireMode() == 0);
			const auto rounds = [&] {
				game::GameWorld::PlayerInfo info;
				g.world->playerInfo(me, info);
				return static_cast<int>(info.gunShots);
			};
			g.run(8); // the weapon's draw
			const int first = rounds();
			game::SoldierInput hold;
			hold.fire = true;
			for (int i = 0; i < 60; ++i) {
				g.world->setPlayerInput(hold);
				g.world->tick(1 / 30.0f);
			}
			CHECK(rounds() == first + 1); // held: one round
			g.world->setPlayerInput({});
			g.run(1);
			// Tapped through the whole magazine, its reload and on: a round every press.
			int fired = 0;
			for (int i = 0; i < 40; ++i) {
				game::SoldierInput tap;
				tap.fire = true;
				const int before = rounds();
				g.world->setPlayerInput(tap);
				g.world->tick(1 / 30.0f);
				fired += rounds() > before;
				g.world->setPlayerInput({});
				g.run(1.2f);
				const int reloadWait = gun->settings().minimumReloadTicks
					+ static_cast<int>(std::ceil(gun->settings().reloadTime * 30)) + 10;
				for (int tick = 0; tick < reloadWait
					&& (gun->reloading() || (gun->roundsInMag() == 0 && gun->magsLeft() != 0)); ++tick) {
					g.world->tick(1 / 30.0f);
				}
			}
			CHECK(fired == 40);
		}
	}
}

// The helipad (helipad_m's SupplyObject child at 0/5/0: radius 5,
// refillAmmoSpeed 12, addVehicleType 3) rearms a parked gunship's pilot
// rockets as retail does (0x005d0b20 -> 0x005f5110): 12 x 0.2 per cent of
// the whole load (6 magazines of 10) every 0.2 s, a magazine within seven
// updates (1.4 s; one magazine's 2.4 % would take 8.3 s), staying put, and
// on to full. The pilot's ammo tick is stamped with the flag clear (player
// slot 0x260(now, 0)): no ability-bar icon or S_Resupply for the receiver.
TEST(Vehicles_HelipadRearmsAParkedGunshipByItsWholeLoad)
{
	if (!gameDir()) {
		return;
	}
	int tested = 0;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* name : {"as_ag", "us_ag"}) {
			Garage g(gameDir(), mod);
			if (!g.loaded || !g.templates.find(name) || !g.templates.find("helipad_m")) {
				continue;
			}
			const uint32_t v = g.start(name, {0, 4, 6});
			CHECK(g.enter(v));
			const int me = g.world->localPlayer();
			const game::FireArm* gun = g.world->gun(v, 0);
			CHECK(gun != nullptr && gun->settings().magSize == 10 && gun->settings().nrOfMags == 6);
			if (!gun) {
				continue;
			}
			g.run(8); // the weapon's draw
			// A magazine tapped out and reloaded, away from any pad.
			for (int i = 0; i < 40 && gun->magsLeft() == 6; ++i) {
				game::SoldierInput tap;
				tap.fire = true;
				g.world->setPlayerInput(tap);
				g.world->tick(1 / 30.0f);
				g.world->setPlayerInput({});
				g.run(1.2f);
			}
			g.run(2);
			const int spent = gun->magsLeft();
			CHECK(spent == 5);
			const world::Vec3 at = g.objects.find(v)->position();
			g.objects.create(g.templates.find("helipad_m"), {at.x, 0, at.z});
			int ticks = 0;
			while (ticks < 30 * 20 && gun->magsLeft() == spent) {
				g.world->tick(1 / 30.0f);
				++ticks;
			}
			CHECK(gun->magsLeft() == spent + 1);
			CHECK(ticks <= 8 * 6); // 7 updates after the pad's first
			game::GameWorld::AbilityBarInfo bar;
			CHECK(g.world->abilityBar(me, bar) && !bar.ammoFlag && bar.sinceAmmo >= 0 && bar.sinceAmmo < 6);
			g.run(2);
			CHECK(gun->magsLeft() == 6);
			++tested;
		}
	}
	CHECK(tested > 0);
}

// The gunships' gunner (as_ag / us_ag seat 2, vanilla and Remaster): the
// seat camera hangs under the turret bundles, so the view turns with the
// mouse and follows the gun's yaw and pitch; and it draws geometry 1 of
// the mesh (cockpitSubGeom 1, the whole exterior hull, seen from inside:
// the renderer culls its back faces), where the pilot's is geometry 0.
TEST(PlayerCamera_GunshipGunnerTurnsWithTheGun)
{
	if (!gameDir()) return;
	constexpr float kDeg = 3.14159265f / 180;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!std::ifstream(std::string(gameDir()) + "/" + mod + "/ServerArchives.con")) continue;
		for (const char* name : {"as_ag", "us_ag"}) {
			Garage g(gameDir(), mod);
			if (!g.loaded || !g.templates.find(name)) continue;
			const auto ship = g.start(name, {0, 2, 6});
			CHECK(g.enter(ship));
			anim::AnimationSystems systems(nullptr);
			PlayerCamera camera(*g.world, systems, nullptr);
			PlayerCamera::Cockpit cockpit;
			camera.tick(1 / 30.0f);
			CHECK(camera.cockpit(cockpit) && cockpit.subGeometry == 0);
			game::SoldierInput sw;
			sw.switchSeat = 2;
			g.world->setPlayerInput(sw);
			g.world->tick(1 / 30.0f);
			for (int i = 0; i < 20; ++i) { g.world->setPlayerInput({}); g.world->tick(1 / 30.0f); camera.tick(1 / 30.0f); }
			CHECK(camera.cockpit(cockpit) && cockpit.vehicle == ship && cockpit.subGeometry == 1);
			PlayerCamera::View rest, turned;
			CHECK(camera.view(rest, 16.0f / 9.0f));
			float gunYaw = 0, gunPitch = 0;
			game::GameWorld::PlayerInfo me;
			g.world->playerInfo(g.world->localPlayer(), me);
			// The gun's bundles turn at their own speeds (stepGunner): lead it.
			float look = 0;
			seataim::steerGun([&](const game::SoldierInput& in) { g.world->setPlayerInput(in); g.world->tick(1 / 30.0f); camera.tick(1 / 30.0f); },
				[&](float& y, float& p) { g.world->gunAim(ship, me.seat, y, p); }, look, 20, 25);
			CHECK(camera.view(turned, 16.0f / 9.0f));
			CHECK(g.world->gunAim(ship, me.seat, gunYaw, gunPitch));
			std::fprintf(stderr, "  %s %s gunner: gun %.1f/%.1f, view yaw %.1f -> %.1f, pitch %.1f -> %.1f\n", mod, name, gunYaw, gunPitch,
				rest.yaw / kDeg, turned.yaw / kDeg, rest.pitch / kDeg, turned.pitch / kDeg);
			CHECK(std::fabs(gunYaw - 20) < 1 && gunPitch > 10);
			CHECK(std::fabs((turned.yaw - rest.yaw) / kDeg - gunYaw) < 1);
			CHECK(std::fabs((turned.pitch - rest.pitch) / kDeg - gunPitch) < 1);
		}
	}
}

// The cockpit geometry selector is asked about every multi-geometry object
// during a scene sync. Only the occupied vehicle can use the seat camera;
// unrelated objects must not trigger its recursive template-tree lookup.
TEST(PlayerCamera_GunshipCockpitSelectionIsScopedToTheOccupiedHull)
{
	if (!gameDir()) return;
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_ag")) return;
	const uint32_t ship = g.start("as_ag", {0, 3, 8});
	const uint32_t other = g.objects.create(g.templates.find("as_ag"), {40, 3, 8})->id();
	CHECK(g.enter(ship));
	anim::AnimationSystems systems(nullptr);
	PlayerCamera camera(*g.world, systems, nullptr);
	camera.tick(1 / 30.0f);
	PlayerCamera::Cockpit cockpit;
	CHECK(camera.cockpit(cockpit, ship) && cockpit.vehicle == ship && cockpit.subGeometry == 0);
	CHECK(!camera.cockpit(cockpit, other));
	CHECK(camera.cockpit(cockpit) && cockpit.vehicle == ship); // frame-level query remains available
	// CPU-only actual-template probe: compare the old query shape (resolve the
	// active camera tree for every geometry) with object-scoped misses (the
	// renderer's many non-player objects). This is not a frame-time/fps claim.
	volatile int resolved = 0;
	constexpr int queries = 500;
	const auto wholeTreeStart = std::chrono::steady_clock::now();
	for (int i = 0; i < queries; ++i) resolved += camera.cockpit(cockpit) ? 1 : 0;
	const auto wholeTreeEnd = std::chrono::steady_clock::now();
	const auto scopedStart = std::chrono::steady_clock::now();
	for (int i = 0; i < queries; ++i) resolved += camera.cockpit(cockpit, other) ? 1 : 0;
	const auto scopedEnd = std::chrono::steady_clock::now();
	const auto wholeUs = std::chrono::duration_cast<std::chrono::microseconds>(wholeTreeEnd - wholeTreeStart).count();
	const auto scopedUs = std::chrono::duration_cast<std::chrono::microseconds>(scopedEnd - scopedStart).count();
	std::fprintf(stderr, "  as_ag CPU probe: %d full camera-tree queries %lld us; %d unrelated-object queries %lld us\n",
		queries, static_cast<long long>(wholeUs), queries, static_cast<long long>(scopedUs));
	CHECK(resolved == queries);
}

// C selects the gunship's Chase camera. That seat-local choice must not
// survive leaving the Raider as a third-person infantry camera. The explicit
// --camera-mode third setting remains honored on foot.
TEST(PlayerCamera_GunshipExitRestoresConfiguredInfantryView)
{
	if (!gameDir()) return;
	for (const bool diagnosticThird : {false, true}) {
		Garage g(gameDir());
		if (!g.loaded || !g.templates.find("as_ag")) continue;
		const uint32_t ship = g.start("as_ag", {0, 3, 8});
		CHECK(g.enter(ship));
		anim::AnimationSystems systems(nullptr);
		PlayerCamera camera(*g.world, systems, nullptr);
		camera.spawnFirstPerson = !diagnosticThird;
		camera.firstPerson = !diagnosticThird;
		camera.tick(1 / 30.0f);
		CHECK(camera.selectView(2));
		CHECK(!camera.insideView());
		game::SoldierInput leave;
		leave.use = true;
		g.world->setPlayerInput(leave);
		g.world->tick(1 / 30.0f);
		g.world->setPlayerInput({});
		camera.tick(1 / 30.0f);
		CHECK(!g.world->playerVehicle());
		CHECK(camera.firstPerson == !diagnosticThird);
		CHECK(camera.insideView() == !diagnosticThird);
	}
}

// A gunship's pilot view is mounted to the hull and should carry the hull's
// bank, and the outside views follow it too (a requested deviation).
TEST(PlayerCamera_GunshipBankingInsideAndOutside)
{
	if (!gameDir()) return;
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("as_ag")) return;
	const uint32_t ship = g.start("as_ag", {0, 3, 8});
	CHECK(g.enter(ship));
	anim::AnimationSystems systems(nullptr);
	PlayerCamera camera(*g.world, systems, nullptr);
	camera.tick(1 / 30.0f);
	game::SoldierInput bank;
	bank.throttle = 1;
	bank.rollAxis = 1;
	g.drive(bank, 1.5f);
	camera.tick(1 / 30.0f);
	const float hullBank = g.objects.find(ship)->rotation().z;
	std::fprintf(stderr, "  as_ag pilot hull bank %.2f deg\n", hullBank);
	CHECK(std::fabs(hullBank) > 0.02f);
	PlayerCamera::View inside;
	CHECK(camera.view(inside, 16.0f / 9.0f));
	CHECK(std::fabs(inside.roll - hullBank * 3.14159265f / 180) < 0.03f);
	// The real C cycle: whichever outside views the gunship allows after Inside. Nose hangs on the hull and the
	// chase views follow an aircraft's bank; a fly-by camera is a fixed outside point and stays level.
	int outsideViews = 0;
	for (int press = 0; press < 6; ++press) {
		camera.toggleMode(false);
		camera.tick(1 / 30.0f);
		PlayerCamera::View outside;
		if (camera.insideView() || !camera.externalView(outside, 16.0f / 9.0f)) continue;
		const bool flyBy = camera.mode() == game::camera::Mode::FlyBy;
		const float expected = flyBy ? 0.0f : hullBank * 3.14159265f / 180;
		std::fprintf(stderr, "  as_ag C press %d mode %d roll %.3f expected %.3f\n", press, static_cast<int>(camera.mode()), outside.roll, expected);
		CHECK(std::fabs(outside.roll - expected) < 0.05f);
		++outsideViews;
	}
	CHECK(outsideViews >= 2);
	CHECK(std::fabs(g.objects.find(ship)->rotation().z) > 0.02f);
}


// The PAC FAV authors its centre of mass 0.5 m above its origin. With tyre
// friction torque taken about that centre it rolled to 87 degrees and was
// destroyed in 11 s under modest steering plus boost; retail's friction
// (0x006c7980) takes it about the body's position. Drive the actual vehicles
// for 20 s and require stable, mostly-grounded running.
TEST(Vehicles_FavStaysUprightUnderSteeringAndBoost)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* name : {"as_fav", "eu_fav"}) {
			for (float steer : {0.0f, 0.2f}) {
				for (bool boost : {false, true}) {
					Garage g(gameDir(), mod);
					if (!g.loaded || !g.templates.find(name)) { CHECK(false); continue; }
					const uint32_t id = g.start(name, {0, 2, 6});
					if (!g.enter(id)) { CHECK(false); continue; }
					game::SoldierInput input;
					input.throttle = 1;
					input.strafe = steer;
					input.sprint = boost;
					float maxSpeed = 0, maxRoll = 0, maxPitch = 0;
					int airborne = 0, ticks = 0;
					bool alive = true;
					for (; ticks < 20 * 30; ++ticks) {
						g.world->setPlayerInput(input);
						g.world->tick(1 / 30.0f);
						const game::Vehicle* v = g.world->playerVehicleState();
						if (!v) { alive = false; break; }
						airborne += std::none_of(v->wheels().begin(), v->wheels().end(), [](const auto& w) { return w.contact; });
						for (const auto& state : g.world->vehicleList()) if (state.object == id) {
							maxSpeed = std::max(maxSpeed, std::sqrt(physics::lengthSquared(state.velocity)));
							maxRoll = std::max(maxRoll, std::fabs(std::asin(std::clamp(state.right.y, -1.0f, 1.0f)) * 57.29578f));
							maxPitch = std::max(maxPitch, std::fabs(std::asin(std::clamp(state.forward.y, -1.0f, 1.0f)) * 57.29578f));
						}
					}
					std::fprintf(stderr, "  %s %s steer=%.1f boost=%d: speed %.1f roll %.1f pitch %.1f airborne ticks %d alive %d\n", mod, name,
						steer, boost, maxSpeed, maxRoll, maxPitch, airborne, alive);
					CHECK(alive);
					CHECK(maxRoll < 15.0f);
					CHECK(maxPitch < 15.0f);
					CHECK(airborne < 15);                       // never off the ground for half a second
					CHECK(maxSpeed > (boost ? 30.0f : 24.0f));  // still the quick car, not capped
				}
			}
		}
	}
}

// Real Suez hills under a buggy and a tank, with the physics' terrain query
// as main.cpp wires it (0x006d2fc0: the cell triangle's height and face
// normal), against the port's former one (bilinear height, the normal from
// heights a cell either side): no more time off the ground, no much rougher ride.
TEST(Vehicles_DriveOverSuezHills)
{
	if (!gameDir()) return;
	struct Drive {
		int airborne = 0;
		float jolt = 0, meanJolt = 0, roll = 0, pitch = 0, distance = 0;
		int joltTick = 0;
		bool alive = true;
	};
	const auto drive = [](const char* name, const physics::Vec3& from, bool faceted) {
		Drive r;
		Garage g(gameDir());
		terrain::HeightmapCluster heightmaps;
		terrain::registerTerrainConsoleMethods(g.console, heightmaps, g.files);
		g.files.setMountingAllowed(true);
		CHECK(g.files.mountArchive("Levels/Suez_Canal/server.zip", "Levels/Suez_Canal/"));
		g.files.setMountingAllowed(false);
		std::string out;
		g.console.run("Levels/Suez_Canal/Heightdata.con", {}, out);
		const terrain::Heightmap* map = heightmaps.primary();
		if (!g.loaded || !map || !map->loaded()) { r.alive = false; return r; }
		physics::Ground ground;
		ground.height = [map](float x, float z) { return map->heightAt(x, z); };
		ground.material = [&heightmaps](float x, float z) { return heightmaps.materialAt(x, z); };
		ground.raycast = [map](const physics::Vec3& a, const physics::Vec3& b, float& fraction, physics::Vec3& point, physics::Vec3& normal) {
			std::array<float, 3> n {}, p {};
			if (!map->raycast({a.x, a.y, a.z}, {b.x, b.y, b.z}, fraction, n, &p)) return false;
			point = {p[0], p[1], p[2]};
			normal = {n[0], n[1], n[2]};
			return true;
		};
		if (faceted) {
			ground.heightAndNormal = [map](float x, float z, physics::Vec3& normal) {
				std::array<float, 3> n {};
				const float height = map->heightAndNormal(x, z, n);
				normal = {n[0], n[1], n[2]};
				return height;
			};
		} else {
			ground.heightAndNormal = [map](float x, float z, physics::Vec3& normal) {
				const float step = map->scale()[0];
				const float dx = map->heightAt(x + step, z) - map->heightAt(x - step, z);
				const float dz = map->heightAt(x, z + step) - map->heightAt(x, z - step);
				const float length = std::sqrt(dx * dx + 4 * step * step + dz * dz);
				normal = {-dx / length, 2 * step / length, -dz / length};
				return map->heightAt(x, z);
			};
		}
		g.terrain = ground;
		const uint32_t id = g.start(name, from);
		if (!g.enter(id)) { r.alive = false; return r; }
		game::SoldierInput input;
		input.throttle = 1;
		float lastVy = 0;
		bool first = true;
		for (int tick = 0; tick < 8 * 30; ++tick) {
			g.world->setPlayerInput(input);
			g.world->tick(1 / 30.0f);
			const game::Vehicle* v = g.world->playerVehicleState();
			if (!v) { r.alive = false; break; }
			r.airborne += std::none_of(v->wheels().begin(), v->wheels().end(), [](const auto& w) { return w.contact; });
			for (const auto& state : g.world->vehicleList()) if (state.object == id) {
				const float dv = first ? 0 : std::fabs(state.velocity.y - lastVy);
				r.meanJolt += dv / (8 * 30);
				if (dv > r.jolt) { r.jolt = dv; r.joltTick = tick; }
				lastVy = state.velocity.y;
				first = false;
				r.roll = std::max(r.roll, std::fabs(std::asin(std::clamp(state.right.y, -1.0f, 1.0f)) * 57.29578f));
				r.pitch = std::max(r.pitch, std::fabs(std::asin(std::clamp(state.forward.y, -1.0f, 1.0f)) * 57.29578f));
			}
		}
		if (const world::Object* o = g.objects.find(id)) r.distance = o->position().z - from.z;
		return r;
	};

	// The drivable stretches along +z (no slope over 0.35 ahead or across)
	// whose slope changes most, from the primary heightmap.
	std::vector<std::pair<float, physics::Vec3>> stretches;
	{
		Garage g(gameDir());
		terrain::HeightmapCluster heightmaps;
		terrain::registerTerrainConsoleMethods(g.console, heightmaps, g.files);
		g.files.setMountingAllowed(true);
		g.files.mountArchive("Levels/Suez_Canal/server.zip", "Levels/Suez_Canal/");
		g.files.setMountingAllowed(false);
		std::string out;
		g.console.run("Levels/Suez_Canal/Heightdata.con", {}, out);
		const terrain::Heightmap* map = heightmaps.primary();
		CHECK(map && map->loaded());
		if (!map || !map->loaded()) return;
		for (float x = -800; x <= 800; x += 40) {
			for (float z = -800; z <= 600; z += 40) {
				float rough = 0, steep = 0;
				for (float d = 2; d < 160; d += 2) {
					const float a = map->heightAt(x, z + d - 2), b = map->heightAt(x, z + d), c = map->heightAt(x, z + d + 2);
					rough += std::fabs(a - 2 * b + c);
					steep = std::max({steep, std::fabs(c - b) / 2, std::fabs(map->heightAt(x + 2, z + d) - map->heightAt(x - 2, z + d)) / 4});
				}
				if (steep < 0.35f) stretches.push_back({rough, {x, map->heightAt(x, z) + 2, z}});
			}
		}
	}
	std::sort(stretches.begin(), stretches.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	stretches.resize(std::min<size_t>(stretches.size(), 4));
	CHECK(stretches.size() == 4);
	for (const auto& [rough, from] : stretches) {
		for (const char* name : {"eu_fav", "eu_tank"}) {
			const Drive was = drive(name, from, false), now = drive(name, from, true);
			std::fprintf(stderr, "  %s at %.0f/%.0f (roughness %.1f): airborne %d -> %d, jolt %.2f (tick %d) -> %.2f (tick %d) m/s, mean %.3f -> %.3f, roll %.1f -> %.1f, pitch %.1f -> %.1f, distance %.0f -> %.0f m\n",
				name, from.x, from.z, rough, was.airborne, now.airborne, was.jolt, was.joltTick, now.jolt, now.joltTick, was.meanJolt, now.meanJolt, was.roll, now.roll, was.pitch, now.pitch, was.distance, now.distance);
			CHECK(was.alive && now.alive);
			CHECK(now.roll < 45.0f);
			CHECK(now.airborne <= was.airborne + 3);
			// Over the whole drive; the single worst tick isn't compared: once the
			// two drives part, one crosses folds (a 2 m dip at 0/78) the other misses.
			CHECK(now.meanJolt <= was.meanJolt * 1.35f);
			CHECK(now.distance > was.distance * 0.8f);
		}
	}
}

#ifdef BF2142_RENDER_TESTS
// The bundled walker's visual bounds understate its collision-based radius.
// Isolate the body so firing effects cannot make an invisible walker pass.
TEST(Vehicles_WalkerDistanceVisibilityPixelsOptIn)
{
	const char* output = std::getenv("BF2142_RENDER_TEST_DIR");
	if (!output || !gameDir()) return;
	std::filesystem::create_directories(output);
	CHECK(SDL_Init(SDL_INIT_VIDEO));
	{
		std::string error;
		auto gpu = render::GpuContext::create(nullptr, error);
		if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); CHECK(gpu); return; }
		for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
			for (const char* name : {"us_heavy_mech", "as_heavy_mech"}) {
				Garage g(gameDir(), mod, true);
				CHECK(g.loaded);
				if (!g.loaded) continue;
				const uint32_t id = g.start(name, {0, 5, 0});
				const auto* object = g.objects.find(id);
				CHECK(object);
				if (!object) continue;
				const auto* geometry = g.geometries.find(object->objectTemplate()->geometry());
				CHECK(geometry);
				if (!geometry) continue;
				std::vector<uint8_t> data;
				CHECK(g.files.readFile(geometry->path, data));
				const auto mesh = geom::parseMesh(data, geometry->kind);
				CHECK(mesh && mesh->geometries.size() > 1);
				if (!mesh || mesh->geometries.size() < 2) continue;
				const auto& bounds = mesh->geometries[1].lods[0].bounds;
				std::fprintf(stderr, "  %s %s: object radius %.3f, mesh box %.2f/%.2f/%.2f .. %.2f/%.2f/%.2f\n",
					mod, name, g.world->objectRadius(id), bounds.min[0], bounds.min[1], bounds.min[2],
					bounds.max[0], bounds.max[1], bounds.max[2]);
				terrain::HeightmapCluster terrain;
				scene::LightSettings lights;
				lights.fogStartEndAndBase = {1000, 2000, 1, 0};
				scene::SkySettings sky;
				scene::WaterSettings water;
				scene::RendererSettings settings;
				settings.drawSkyDome = settings.drawTerrain = false;
				render::LevelSources sources {g.files, g.templates, g.objects, g.geometries, terrain,
					nullptr, lights, sky, water, "", ""};
				sources.renderer = &settings;
				sources.objectRadius = [&](uint32_t object) { return g.world->objectRadius(object); };
				sources.partPose = [&](uint32_t object, const std::string& part, world::Vec3& offset, world::Vec3& rotation) {
					return g.world->partPose(object, part, offset, rotation);
				};
				sources.partBone = [&](uint32_t object, const std::string& part, anim::Matrix& local) {
					return g.world->partBone(object, part, local);
				};
				render::LevelRenderer renderer(*gpu);
				renderer.build(sources);
				auto target = gpu->createOffscreenTarget(960, 540);
				render::Camera camera;
				const auto origin = object->position();
				camera.yaw = 3.14159265f;
				const auto frame = [&]() {
					renderer.syncObjects();
					auto* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
					renderer.draw(commands, target.color, target, camera, lights);
					CHECK(SDL_SubmitGPUCommandBuffer(commands));
					std::vector<uint8_t> pixels;
					CHECK(gpu->readPixels(target, pixels));
					return pixels;
				};
				for (float distance : {40.0f, 100.0f, 180.0f, 250.0f, 550.0f, 40.0f}) {
					camera.position = {origin.x, origin.y, origin.z + distance};
					settings.drawObjects = false;
					const auto blank = frame();
					settings.drawObjects = true;
					const auto body = frame();
					size_t changed = 0;
					for (size_t i = 0; i + 3 < body.size() && i + 3 < blank.size(); i += 4)
						if (body[i] != blank[i] || body[i + 1] != blank[i + 1] || body[i + 2] != blank[i + 2]) ++changed;
					std::fprintf(stderr, "  %s %s at %.0f m: %zu body pixels\n", mod, name, distance, changed);
					if (distance > 500) {
						CHECK(changed == 0);
					} else {
						CHECK(changed > 20);
					}
					const std::string label = std::string(mod).find("Remaster") != std::string::npos ? "remaster-" : "vanilla-";
					CHECK(render::writePng(std::string(output) + "/" + label + name + "-" + std::to_string(static_cast<int>(distance)) + ".png",
						target.width, target.height, body));
				}
			}
		}
	}
	SDL_Quit();
}

// Compare independently drawn particles with a pre-batched, multi-material
// mesh whose triangles are interleaved and whose slots alternate geometry.
TEST(Vehicles_MeshParticleBatchPixelsOptIn)
{
	if (!std::getenv("BF2142_RENDER_TEST_DIR")) return;
	enum class Fault { None, MissingIndices, MixedSlots, InvalidIndex, InvalidSlot };
	const auto meshBytes = [](uint32_t slots, uint32_t singleVariant, Fault fault) {
		std::vector<uint8_t> bytes;
		const auto write = [&bytes](auto value) {
			const auto* data = reinterpret_cast<const uint8_t*>(&value);
			bytes.insert(bytes.end(), data, data + sizeof(value));
		};
		const auto u32 = [&](uint32_t value) { write(value); };
		const auto element = [&](uint16_t stream, uint16_t offset, uint16_t type, uint16_t usage) {
			write(stream); write(offset); write(type); write(usage);
		};
		const auto text = [&](const std::string& value) {
			u32(static_cast<uint32_t>(value.size()));
			bytes.insert(bytes.end(), value.begin(), value.end());
		};
		u32(0); u32(10); u32(0); u32(0); u32(0); write(uint8_t {0});
		u32(1); u32(1);
		u32(5);
		element(0, 0, 2, 0); element(0, 12, 2, 3);
		element(0, 24, 4, fault == Fault::MissingIndices ? 7 : 2);
		element(0, 28, 1, 5); element(255, 0, 17, 0);
		u32(4); u32(36); u32(slots * 8);
		for (uint32_t material = 0; material < 2; ++material) {
			for (uint32_t slot = 0; slot < slots; ++slot) {
				const uint32_t variant = slots == 1 ? singleVariant : slot % 2;
				const float x = variant ? 0.75f : -0.75f;
				const float y = material == 0 ? -0.75f : 0.75f;
				const float vertices[4][2] = {{x - 0.5f, y - 0.5f}, {x + 0.5f, y - 0.5f},
					{x + 0.5f, y + 0.5f}, {x - 0.5f, y + 0.5f}};
				for (uint32_t v = 0; v < 4; ++v) {
					write(vertices[v][0]); write(vertices[v][1]); write(0.0f);
					write(0.0f); write(0.0f); write(-1.0f);
					const uint32_t blend = material == 0 && slot == 0 && fault == Fault::InvalidSlot ? slots
						: material == 0 && slot == 0 && v == 1 && fault == Fault::MixedSlots ? 1 : slot;
					u32(blend); write(0.0f); write(0.0f);
				}
			}
		}
		u32(slots * 12);
		for (uint32_t material = 0; material < 2; ++material) {
			for (int triangle = 0; triangle < 2; ++triangle) {
				for (uint32_t i = 0; i < slots; ++i) {
					const uint16_t base = static_cast<uint16_t>((slots - i - 1) * 4);
					const bool invalid = material == 0 && triangle == 0 && i == slots - 1 && fault == Fault::InvalidIndex;
					write(invalid ? static_cast<uint16_t>(slots * 4) : base);
					write(static_cast<uint16_t>(base + (triangle == 0 ? 1 : 2)));
					write(static_cast<uint16_t>(base + (triangle == 0 ? 2 : 3)));
				}
			}
		}
		u32(0);
		for (float value : {-1.25f, -1.25f, 0.0f, 1.25f, 1.25f, 0.0f}) write(value);
		u32(slots);
		u32(2);
		for (uint32_t material = 0; material < 2; ++material) {
			u32(0); text("BundledMesh.fx"); text(""); u32(0);
			u32(material * slots * 4); u32(material * slots * 6);
			u32(slots * 6); u32(slots * 4); u32(0); u32(0);
		}
		return bytes;
	};
	CHECK(SDL_Init(SDL_INIT_VIDEO));
	{
		std::string error;
		auto gpu = render::GpuContext::create(nullptr, error);
		if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); CHECK(gpu); return; }
		io::FileManager files;
		world::ObjectTemplateManager templates;
		world::ObjectManager objects;
		geom::GeometryTemplateManager geometries;
		for (const auto& item : {std::pair<const char*, uint32_t> {"single-odd", 1},
				 std::pair<const char*, uint32_t> {"single-even", 0},
				 std::pair<const char*, uint32_t> {"batched", 0}}) {
			auto* geometry = geometries.create("MeshParticleMesh", item.first, "test/");
			CHECK(geometry);
			if (!geometry) return;
			files.addMemoryFile(geometry->path, meshBytes(std::string(item.first) == "batched" ? 26 : 1, item.second, Fault::None));
		}
		for (const auto& item : {std::pair<const char*, Fault> {"missing-indices", Fault::MissingIndices},
				 std::pair<const char*, Fault> {"mixed-slots", Fault::MixedSlots},
				 std::pair<const char*, Fault> {"invalid-index", Fault::InvalidIndex},
				 std::pair<const char*, Fault> {"invalid-slot", Fault::InvalidSlot}}) {
			auto* geometry = geometries.create("MeshParticleMesh", item.first, "test/");
			CHECK(geometry);
			if (!geometry) return;
			files.addMemoryFile(geometry->path, meshBytes(2, 0, item.second));
		}
		terrain::HeightmapCluster terrain;
		scene::LightSettings lights;
		lights.fogStartEndAndBase = {1000, 2000, 1, 0};
		scene::SkySettings sky;
		scene::WaterSettings water;
		scene::RendererSettings settings;
		settings.drawSkyDome = settings.drawTerrain = false;
		effects::EffectManager manager(templates);
		render::LevelSources sources {files, templates, objects, geometries, terrain,
			nullptr, lights, sky, water, "", ""};
		sources.renderer = &settings;
		sources.objectEffects = &manager;
		render::LevelRenderer renderer(*gpu);
		renderer.build(sources);
		render::Camera camera;
		camera.position = {0, 0, -5};
		auto target = gpu->createOffscreenTarget(320, 180);
		const auto frame = [&]() {
			renderer.syncObjects();
			auto* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
			renderer.draw(commands, target.color, target, camera, lights);
			CHECK(SDL_SubmitGPUCommandBuffer(commands));
			std::vector<uint8_t> pixels;
			CHECK(gpu->readPixels(target, pixels));
			return pixels;
		};
		const auto blank = frame();
		const effects::MeshSprite particle {{}, {}, 1, {0.25f, 0.75f, 0.3f, 0.12f}, 1};
		manager.setPlacedMeshes({{"single-odd", {particle}}, {"single-even", {particle}}});
		const auto reference = frame();
		manager.setPlacedMeshes({{"batched", {particle, particle}}});
		const auto batched = frame();
		CHECK(reference != blank);
		CHECK(batched == reference);
		std::vector<std::pair<std::string, std::vector<effects::MeshSprite>>> singles;
		std::vector<effects::MeshSprite> many;
		for (int i = 0; i < 27; ++i) {
			auto sprite = particle;
			sprite.position.x = static_cast<float>(i % 9 - 4) * 0.6f;
			sprite.position.y = static_cast<float>(i / 9 - 1) * 0.6f;
			sprite.size = 0.3f;
			singles.push_back({i % 2 == 0 ? "single-odd" : "single-even", {sprite}});
			many.push_back(sprite);
		}
		manager.setPlacedMeshes(std::move(singles));
		const auto referenceMany = frame();
		manager.setPlacedMeshes({{"batched", many}});
		CHECK(frame() == referenceMany);
		for (const auto& item : {std::pair<const char*, const char*> {"missing-indices", "blend indices"},
				 std::pair<const char*, const char*> {"mixed-slots", "mixed particle slots"},
				 std::pair<const char*, const char*> {"invalid-index", "index outside material"},
				 std::pair<const char*, const char*> {"invalid-slot", "slot outside batch"}}) {
			const size_t previous = renderer.stats().missingMeshNames.size();
			manager.setPlacedMeshes({{item.first, {particle}}});
			CHECK(frame() == blank);
			const auto& missing = renderer.stats().missingMeshNames;
			CHECK(missing.size() == previous + 1);
			if (missing.size() == previous + 1) CHECK(missing.back().find(item.second) != std::string::npos);
		}
		gpu->releaseTarget(target);
	}
	SDL_Quit();
}

// Explicit opt-in: real SDL GPU offscreen draw/readback, never a window.
TEST(Vehicles_AdsSpikesPixelsOptIn)
{
	const char* output = std::getenv("BF2142_RENDER_TEST_DIR");
	if (!output || !gameDir()) return;
	std::filesystem::create_directories(output);
	CHECK(SDL_Init(SDL_INIT_VIDEO));
	{
		std::string error;
		auto gpu = render::GpuContext::create(nullptr, error);
		if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); CHECK(gpu); return; }
		for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
			Garage g(gameDir(), mod, true);
			CHECK(g.loaded);
			auto* spike = g.templates.find("em_ActiveDef_shield_Spikes");
			CHECK(spike);
			if (!spike) continue;
			const auto authored = effects::ParticleTemplate::fromTemplate(*spike);
			CHECK(authored.particleMaxSize == -1 && authored.directionalScale == 3);
			effects::EffectManager manager(g.templates);
			CHECK(manager.spawn(spike->name(), {}));
			for (int i = 0; i < 30; ++i) manager.update(1 / 30.0f);
			size_t meshCount = 0;
			manager.forEachMeshGroup([&](const std::string& name, bool additive,
				const std::vector<effects::MeshSprite>& list) {
				CHECK(name == "mp_smokespike" && additive);
				for (const auto& mesh : list) {
					CHECK(mesh.size == -1 && mesh.stretch > 0 && mesh.color[3] > 0);
					++meshCount;
				}
			});
			CHECK(meshCount > 20);
			world::ObjectManager noObjects;
			terrain::HeightmapCluster noTerrain;
			scene::LightSettings lights;
			lights.fogStartEndAndBase = {1000, 2000, 1, 0};
			scene::SkySettings sky;
			scene::WaterSettings water;
			scene::RendererSettings settings;
			render::LevelSources sources {g.files, g.templates, noObjects, g.geometries, noTerrain,
				nullptr, lights, sky, water, "", ""};
			sources.renderer = &settings;
			render::LevelRenderer renderer(*gpu);
			renderer.build(sources);
			render::Camera camera;
			camera.position = {0, 4, -18};
			camera.pitch = -std::atan2(4.0f, 18.0f);
			auto target = gpu->createOffscreenTarget(960, 540);
			const auto frame = [&]() {
				renderer.syncObjects();
				auto* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
				renderer.draw(commands, target.color, target, camera, lights);
				CHECK(SDL_SubmitGPUCommandBuffer(commands));
				std::vector<uint8_t> pixels;
				CHECK(gpu->readPixels(target, pixels));
				return pixels;
			};
			const auto blank = frame();
			sources.objectEffects = &manager;
			const auto active = frame();
			size_t greenPixels = 0;
			for (size_t i = 0; i + 3 < active.size(); i += 4) {
				const int red = int(active[i]) - int(blank[i]);
				const int green = int(active[i + 1]) - int(blank[i + 1]);
				const int blue = int(active[i + 2]) - int(blank[i + 2]);
				if (green > 8 && green > red && green > blue) ++greenPixels;
			}
			const std::string label = std::string(mod) == "mods/bf2142" ? "vanilla" : "remaster";
			std::fprintf(stderr, "  %s ADS: %zu mesh particles, %zu green spike pixels\n",
				label.c_str(), meshCount, greenPixels);
			CHECK(greenPixels > 100);
			CHECK(render::writePng(std::string(output) + "/" + label + "-ads-spikes.png",
				target.width, target.height, active));
			sources.objectEffects = nullptr;
			CHECK(frame() == blank);
			gpu->releaseTarget(target);
		}
	}
	SDL_Quit();
}

TEST(Vehicles_ProjectilePixelsOptIn)
{
	const char* output = std::getenv("BF2142_RENDER_TEST_DIR");
	if (!output || !gameDir()) return;
	std::filesystem::create_directories(output);
	CHECK(SDL_Init(SDL_INIT_VIDEO));
	{
		std::string error;
		auto gpu = render::GpuContext::create(nullptr, error);
		if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); CHECK(gpu); return; }
		const auto draw = [&](io::FileManager& files, world::ObjectTemplateManager& templates,
			geom::GeometryTemplateManager& geometries, game::GameWorld& round,
			const std::string& name, const std::string& label, bool tracers = false,
			effects::EffectManager* objectEffects = nullptr, float distance = 8, bool insideTitan = false) {
			const auto live = std::find_if(round.projectiles().begin(), round.projectiles().end(),
				[&](const game::Projectile& p) { return p.alive && p.settings && p.settings->name == name; });
			CHECK(live != round.projectiles().end());
			if (live == round.projectiles().end()) return;
			std::fprintf(stderr, "  draw %s: %s at %.2f/%.2f/%.2f velocity %.2f/%.2f/%.2f\n", label.c_str(),
				name.c_str(), live->position.x, live->position.y, live->position.z,
				live->velocity.x, live->velocity.y, live->velocity.z);
			// Isolate the actual in-flight mesh: no launcher, particles, trails,
			// HUD or terrain can cause a false-positive pixel difference.
			world::ObjectManager noPlacedObjects;
			if (insideTitan) {
				// The same mesh as an ordinary placed object must remain
				// subject to indoor culling. Only the transient flight model
				// should add pixels over this baseline.
				auto* t = templates.find(name);
				const auto rotation = physics::lookAtRotation(live->velocity, physics::Mat3 {}).yawPitchRoll();
				noPlacedObjects.create(t, {live->position.x, live->position.y, live->position.z},
					{rotation.x, rotation.y, rotation.z});
			}
			terrain::HeightmapCluster noTerrain;
			scene::LightSettings lights;
			lights.fogStartEndAndBase = {1000, 2000, 1, 0};
			scene::SkySettings sky;
			scene::WaterSettings water;
			scene::RendererSettings renderSettings;
			renderSettings.insideTitanOverride = insideTitan;
			render::LevelSources sources {files, templates, noPlacedObjects, geometries, noTerrain,
				nullptr, lights, sky, water, "", ""};
			sources.renderer = &renderSettings;
			bool shown = false;
			client::RoundVisuals visuals(templates);
			std::vector<client::RoundVisuals::Mesh> roundMeshes;
			sources.placedTemplates = [&](std::vector<render::LevelSources::PlacedTemplate>& out) {
				if (!shown) return;
				roundMeshes.clear();
				visuals.meshes(round.projectiles(), roundMeshes, 0);
				for (const auto& mesh : roundMeshes) {
					if (mesh.objectTemplate->name() != name) continue;
					out.push_back({mesh.objectTemplate, mesh.position, mesh.rotation});
				}
			};
			render::LevelRenderer renderer(*gpu);
			renderer.build(sources);
			render::Camera camera;
			camera.position = {live->position.x - distance, live->position.y + distance * 0.25f, live->position.z};
			camera.yaw = 1.57079632679f;
			camera.pitch = -std::atan2(2.0f, 8.0f);
			auto target = gpu->createOffscreenTarget(960, 540);
			const auto frame = [&]() {
				sources.objectEffects = shown ? objectEffects : nullptr;
				sources.effects = shown && tracers ? &round.effects() : nullptr;
				renderer.syncObjects();
				auto* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
				renderer.draw(commands, target.color, target, camera, lights);
				CHECK(SDL_SubmitGPUCommandBuffer(commands));
				std::vector<uint8_t> pixels;
				CHECK(gpu->readPixels(target, pixels));
				return pixels;
			};
			const auto blank = frame();
			shown = true;
			const auto missile = frame();
			size_t changed = 0;
			for (size_t i = 0; i + 3 < missile.size() && i + 3 < blank.size(); i += 4) {
				if (std::abs(int(missile[i]) - int(blank[i])) > 3 ||
					std::abs(int(missile[i + 1]) - int(blank[i + 1])) > 3 ||
					std::abs(int(missile[i + 2]) - int(blank[i + 2])) > 3) ++changed;
			}
			std::fprintf(stderr, "  %s: %zu visible projectile pixels; %zu meshes, %zu missing\n", label.c_str(), changed,
				renderer.stats().meshes, renderer.stats().missingMeshes);
			for (const auto& missing : renderer.stats().missingMeshNames) std::fprintf(stderr, "    %s\n", missing.c_str());
			CHECK(changed > (distance < 20 ? 20u : 0u));
			CHECK(render::writePng(std::string(output) + "/" + label + ".png", target.width, target.height, missile));
			shown = false;
			CHECK(frame() == blank);
			gpu->releaseTarget(target);
		};
		for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
			const std::string prefix = std::string(mod) == "mods/bf2142" ? "vanilla" : "remaster";
			if (prefix == "remaster") {
				Garage m(gameDir(), mod, true);
				m.start("eu_tank", {100, 0, 100}, 0);
				const int me = m.world->joinPlayer(2);
				m.world->setKit(me, "EU_Anti-Vehicle", "us_light_soldier");
				m.world->setUnlocks(me, {"UnlockEU_Anti-VehiclePlayer1"});
				CHECK(m.world->spawnSoldier(me));
				game::SoldierInput input; input.selectItem = 3;
				m.world->setPlayerInput(input); m.world->tick(1 / 30.0f);
				m.world->setPlayerInput({}); m.run(3);
				input = {}; input.fire = true;
				m.world->setPlayerInput(input); m.world->tick(1 / 30.0f);
				m.world->setPlayerInput({});
				draw(m.files, m.templates, m.geometries, *m.world, "magnetic_ammo", "remaster-madlock-flight", false, nullptr, 2);
				m.run(2);
				draw(m.files, m.templates, m.geometries, *m.world, "magnetic_ammo", "remaster-madlock-ground", false, nullptr, 2);
			}

			{
				Garage g(gameDir(), mod, true);
				CHECK(g.loaded);
				const auto aa = g.start("static_aa", {0, 2, 8});
				CHECK(g.enter(aa));
				for (int i = 0; i < 90 && g.world->projectilesInFlight() == 0; ++i) {
					game::SoldierInput fire; fire.fire = true;
					g.world->setPlayerInput(fire); g.world->tick(1 / 30.0f);
				}
				g.world->setPlayerInput({}); g.run(0.1f);
				draw(g.files, g.templates, g.geometries, *g.world, "static_aa_cannon_Projectile", prefix + "-aa-tracer", true);
				game::SoldierInput rocket; rocket.altFire = true;
				g.world->setPlayerInput(rocket); g.world->tick(1 / 30.0f);
				g.world->setPlayerInput({});
				effects::EffectManager effectManager(g.templates);
				effects::ObjectEffects attachments(g.templates);
				for (int i = 0; i < 10; ++i) {
					g.world->tick(1 / 30.0f);
					std::vector<effects::ObjectEffects::ObjectState> states;
					for (const auto& p : g.world->projectiles()) {
						if (!p.alive || !p.settings || p.settings->name != "static_aa_missile_projectile") continue;
						effects::ObjectEffects::ObjectState state;
						state.id = p.serial;
						state.objectTemplate = g.templates.find(p.settings->name);
						state.position = p.position; state.velocity = p.velocity;
						state.rotation = physics::lookAtRotation(p.velocity, physics::Mat3 {});
						states.push_back(state);
					}
					attachments.update(states, effectManager);
					effectManager.update(1 / 30.0f);
				}
				draw(g.files, g.templates, g.geometries, *g.world, "static_aa_missile_projectile",
					prefix + "-aa-rocket-effects", false, &effectManager);
			}
			{
				geom::GeometryTemplateManager geometries;
				titantest::TitanLevel level(gameDir(), "Suez_Canal", 48, true, mod, false, &geometries);
				CHECK(level.loaded);
				const auto silos = level.world->missileSilos();
				CHECK(!silos.empty());
				if (silos.empty()) continue;
				level.world->setControlPointTeam(silos.front().controlPoint, 1);
				for (int tick = 0; tick < 30 * 150; ++tick) {
					level.run(1 / 30.0f);
					const auto& rounds = level.world->projectiles();
					if (std::any_of(rounds.begin(), rounds.end(), [](const auto& p) {
						return p.alive && p.settings && p.settings->name == "msilo_missile";
					})) break;
				}
				level.run(0.1f);
				draw(level.files, level.templates, geometries, *level.world, "msilo_missile", prefix + "-anti-titan");
				for (float distance : {80.0f, 160.0f}) {
					draw(level.files, level.templates, geometries, *level.world, "msilo_missile",
						prefix + "-anti-titan-" + std::to_string(static_cast<int>(distance)) + "m", false, nullptr, distance);
				}
				draw(level.files, level.templates, geometries, *level.world, "msilo_missile",
					prefix + "-anti-titan-from-interior", false, nullptr, 80, true);
			}
		}
	}
	SDL_Quit();
}
#endif

#include "TitanLevel.h"
// A silo's launch: the missile leaves the barrel's muzzle (not the ground at the silo's foot), its trail rides the
// missile, and once the missile has left nothing of the launch still glows or smokes at the silo (the report: an
// orange glow at the base long after a launch, drawn too low). The trail's anchor is the round, as main.cpp feeds it
// (RoundVisuals::appendEffectStates -> ObjectEffects -> EffectManager), with the silo object's own bundles beside it.
TEST(Titan_SiloLaunchLeavesNoLiveEffectAtItsBase)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		geom::GeometryTemplateManager geometries;
		titantest::TitanLevel level(gameDir(), "Suez_Canal", 48, true, mod, true, &geometries);
		CHECK(level.loaded);
		const auto silos = level.world->missileSilos();
		CHECK(!silos.empty());
		if (silos.empty()) continue;
		level.world->setControlPointTeam(silos.front().controlPoint, 1);
		effects::EffectManager effectManager(level.templates);
		effects::ObjectEffects attachments(level.templates);
		client::RoundVisuals visuals(level.templates);
		const auto step = [&]() {
			level.run(1 / 30.0f);
			std::vector<effects::ObjectEffects::ObjectState> states;
			for (const auto& silo : level.world->missileSilos()) {
				const world::Object* object = level.objects.find(silo.object);
				if (!object) continue;
				effects::ObjectEffects::ObjectState state;
				state.id = object->id();
				state.objectTemplate = object->objectTemplate();
				state.position = {object->position().x, object->position().y, object->position().z};
				state.rotation = physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
				states.push_back(state);
			}
			visuals.appendEffectStates(level.world->projectiles(), states);
			attachments.update(states, effectManager, [&](uint32_t object, const std::string& part, physics::Vec3& offset, physics::Vec3& rotation) {
				world::Vec3 o, r;
				if (!level.world->partPose(object, part, o, r)) return false;
				offset = {o.x, o.y, o.z};
				rotation = {r.x, r.y, r.z};
				return true;
			});
			effectManager.update(1 / 30.0f);
		};
		const auto missile = [&]() -> const game::Projectile* {
			for (const auto& p : level.world->projectiles()) {
				if (p.alive && p.settings && p.settings->name == "msilo_missile") return &p;
			}
			return nullptr;
		};
		int guard = 0;
		while (!missile() && guard++ < 30 * 150) step();
		CHECK(missile() != nullptr);
		if (!missile()) continue;
		physics::Vec3 base;
		for (const auto& s : level.world->missileSilos()) {
			if (s.launched > 0) base = s.position;
		}
		// It leaves the barrel's muzzle, well above the silo's origin at the ground.
		CHECK(missile()->position.y - base.y > 3.0f);
		const auto nearBase = [&]() {
			int near = 0;
			for (const auto& e : effectManager.emitterList()) {
				const physics::Vec3 d = e->position() - base;
				if (physics::lengthSquared(d) < 15.0f * 15.0f) ++near;
			}
			return near;
		};
		// The missile climbs away at 4 m/s and then turns for the Titan: after a few seconds its trail is far from the base
		// and the silo's own smoke (2.5 s) is spent.
		int goneTicks = -1;
		for (int tick = 0; tick < 30 * 40; ++tick) {
			step();
			if (tick == 30 * 6) CHECK(nearBase() == 0);
			if (!missile() && goneTicks < 0) goneTicks = tick;
			// Seconds after the missile is gone (it flew its 14 s out): nothing of it is left anywhere.
			if (goneTicks >= 0 && tick == goneTicks + 30 * 8) CHECK(effectManager.emitterList().empty());
		}
		CHECK(goneTicks >= 0);
	}
}

// Titans are not in vehicleList(): the floor radio ("The enemy has boarded our Titan!") once found none and took every
// soldier on a Titan floor, the player's own team's included, for a boarder. Its holder is now the hull test's.
TEST(Titan_HullHoldsItsOwnSoldiersNotOthers)
{
	if (!gameDir()) return;
	titantest::TitanLevel level(gameDir(), "Suez_Canal", 48);
	CHECK(level.world->spawnPlayer(2) != nullptr);
	level.run(1.0f);
	game::GameWorld::PlayerInfo info;
	CHECK(level.world->playerInfo(level.world->localPlayer(), info));
	const physics::Vec3 at = info.position + physics::Vec3 {0, 1.0f, 0};
	const uint32_t held = level.world->titanHolding(at, true);
	CHECK(held != 0);
	const world::Object* titan = level.objects.find(held);
	CHECK(titan && titan->team() == info.team);
	CHECK(level.world->titanHolding({0, 500, 0}, true) == 0);
}

// A round used to leave from 1.6 m above the feet whatever the pose. It follows the soldier's eye down when crouched
// (about 1.08) and prone (about 0.5).
TEST(Soldier_FiresFromTheEyeOfItsPose)
{
	if (!gameDir()) return;
	Garage g(gameDir());
	if (!g.loaded) return;
	g.start("eu_tank", {50, 0, 50}, 1);
	CHECK(g.world->spawnPlayer(2) != nullptr);
	const int player = g.world->localPlayer();
	game::GameWorld::PlayerInfo info;
	float heights[3] = {};
	int index = 0;
	for (auto pose : {game::SoldierPose::Stand, game::SoldierPose::Crouch, game::SoldierPose::Prone}) {
		game::SoldierInput in;
		in.pose = pose;
		for (int i = 0; i < 90; ++i) { g.world->setInput(player, in); g.world->tick(1 / 30.0f); }
		g.world->playerInfo(player, info);
		in.fire = true;
		g.world->setInput(player, in);
		g.world->tick(1 / 30.0f);
		in.fire = false;
		g.world->setInput(player, in);
		float originY = -1000;
		for (const auto& p : g.world->projectiles()) if (p.player == player) originY = p.origin.y;
		CHECK(originY > -999); // a round was fired
		heights[index++] = originY - info.position.y;
	}
	std::fprintf(stderr, "  firing height above the feet: stand %.2f crouch %.2f prone %.2f\n", heights[0], heights[1], heights[2]);
	CHECK(std::fabs(heights[0] - 1.6f) < 0.05f);
	CHECK(std::fabs(heights[1] - 1.08f) < 0.05f);
	CHECK(std::fabs(heights[2] - 0.5f) < 0.05f);
}

// Rounds sent at a standing enemy PAC walker from every side must damage
// it ("enemy units noclip" report: an EU APC's rounds passing through it):
// only geometry 0 of its collision mesh was in the projectile world, which
// holds next to nothing; the pelvis, legs and turret are children's
// geometries.
TEST(Vehicles_RoundsHitAnEnemyWalker)
{
	if (!gameDir()) {
		return;
	}
	for (int side = 0; side < 4; ++side) {
		// A fresh walker per side: the anti-vehicle round (500) wrecks it.
		Garage g(gameDir());
		if (!g.loaded || !g.templates.find("as_heavy_mech")) {
			return;
		}
		const uint32_t walker = g.start("as_heavy_mech", {0, 4, 0}, 3);
		const int shooter = g.world->addPlayer("Shooter", 1, true);
		const world::ObjectTemplate* rifle = g.templates.find("Unl_AV_Rifle");
		const std::string round = rifle && rifle->settings().count("projectileTemplate") ? rifle->settings().at("projectileTemplate") : "";
		CHECK(!round.empty());
		const physics::Vec3 at = g.position(walker);
		const physics::Vec3 dir = side == 0 ? physics::Vec3 {1, 0, 0} : side == 1 ? physics::Vec3 {-1, 0, 0} : side == 2 ? physics::Vec3 {0, 0, 1} : physics::Vec3 {0, 0, -1};
		const physics::Vec3 across {dir.z, 0, dir.x};
		const float before = g.world->hitPoints(walker);
		g.world->launchProjectile(round, at + dir * 20.0f + across * 0.5f + physics::Vec3 {0, 1, 0}, dir * -300.0f, shooter);
		g.run(0.3f);
		std::fprintf(stderr, "  side %d: walker %.0f -> %.0f\n", side, before, g.world->hitPoints(walker));
		CHECK(g.world->hitPoints(walker) < before);
	}
}

// A/D alone turn a tracked tank on the spot (no W needed).
// The hover tank's turn is the engine's (0x0064d9a2 in BF2142.exe): a torque
// about the up axis only while the drive value is over 0.01, which the
// throttle gives; A/D alone, from rest, turn nothing (retail's rule).
TEST(Vehicles_HoverTankTurnsOnlyWithDrive)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t tank = g.start("eu_tank", {0, 2, 6});
	CHECK(g.enter(tank));
	const physics::Vec3 rest = g.position(tank);
	const float before = g.yaw(tank);
	game::SoldierInput input;
	input.strafe = 1;
	g.drive(input, 3);
	const float turned = std::remainder(g.yaw(tank) - before, 360.0f);
	std::fprintf(stderr, "  eu_tank: A/D alone turned %.1f degrees in 3 s, moved %.1f m\n", turned,
		std::sqrt(physics::lengthSquared(g.position(tank) - rest)));
	CHECK(std::fabs(turned) < 1);
}

// The air transport's side gunner: turning on the mouse turns the gun and
// its camera chain together.
TEST(Vehicles_TransportGunnerCameraTurnsWithTheGun)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("eu_at")) {
		return;
	}
	const uint32_t at = g.start("eu_at", {0, 2, 6});
	CHECK(g.enter(at));
	const auto seats = g.world->vehicleSeats(at);
	for (size_t i = 0; i < seats.size(); ++i) {
		std::fprintf(stderr, "  seat %zu: camera '%s'\n", i, seats[i].camera.c_str());
	}
	game::SoldierInput seat;
	seat.switchSeat = 2;
	g.world->setPlayerInput(seat);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(0.5f);
	game::SoldierInput look;
	look.turn = 10;
	for (int i = 0; i < 30; ++i) {
		g.world->setPlayerInput(look);
		g.world->tick(1 / 30.0f);
	}
	float gunYaw = 0, cameraYaw = 0;
	for (const char* part : {"EU_AT_minigun_left_base", "eu_at_L_gun_cameraBase", "eu_at_L_gun_cameraMount", "eu_at_L_gun_camera",
			 "EU_AT_minigun_right_base", "eu_at_R_gun_cameraBase"}) {
		world::Vec3 offset, rotation;
		const bool ok = g.world->partPose(at, part, offset, rotation);
		std::fprintf(stderr, "  %s: %d rot %.1f %.1f %.1f\n", part, ok, rotation.x, rotation.y, rotation.z);
		if (std::string(part) == "EU_AT_minigun_left_base") {
			gunYaw = rotation.x;
		}
		if (std::string(part) == "eu_at_L_gun_cameraBase") {
			cameraYaw = rotation.x;
		}
	}
	CHECK(std::fabs(gunYaw) > 10 && std::fabs(cameraYaw - gunYaw) < 0.01f);
}

// The Shepherd's side gunners (eu_at seats F2 and F3): the minigun's rounds
// leave along the barrel the mouse aims, wherever the gun is turned
// (fire.fireInCameraDof's frame is the seat camera's, 0x005f1a20, and that
// camera rides the gun's bundles).
TEST(Vehicles_ShepherdGunnersFireWhereTheyAim)
{
	if (!gameDir()) {
		return;
	}
	for (const int seatKey : {2, 3}) {
		Garage g(gameDir());
		if (!g.loaded || !g.templates.find("eu_at")) {
			return;
		}
		const uint32_t at = g.start("eu_at", {0, 2, 6});
		CHECK(g.enter(at));
		game::SoldierInput seat;
		seat.switchSeat = seatKey;
		g.world->setPlayerInput(seat);
		g.world->tick(1 / 30.0f);
		g.world->setPlayerInput({});
		g.run(0.5f);
		g.world->setZoomed(g.world->localPlayer(), true); // no deviation
		const int gunSeat = seatKey - 1;
		float yaw = 0, pitch = 0;
		CHECK(g.world->gunAim(at, gunSeat, yaw, pitch));
		float look = 0;
		seataim::steerGun([&](const game::SoldierInput& i) { g.world->setPlayerInput(i); g.world->tick(1 / 30.0f); },
			[&](float& y, float& p) { g.world->gunAim(at, gunSeat, y, p); }, look, std::remainder(yaw + 25, 360.0f), pitch + 8); // up: a round into the ground ends within its tick
		game::SoldierInput hold;
		hold.aimPitch = look;
		g.world->setPlayerInput(hold);
		g.world->tick(1 / 30.0f);
		const size_t before = g.world->projectiles().size();
		hold.fire = true;
		for (int i = 0; i < 90 && g.world->projectiles().size() <= before; ++i) {
			g.world->setPlayerInput(hold);
			g.world->tick(1 / 30.0f);
		}
		CHECK(g.world->projectiles().size() > before);
		if (g.world->projectiles().size() <= before) {
			return;
		}
		const game::Projectile& p = g.world->projectiles()[before];
		physics::Vec3 origin;
		physics::Mat3 hull;
		float gunYaw = 0, gunPitch = 0;
		CHECK(g.world->gunFrame(at, gunSeat, origin, hull));
		CHECK(g.world->gunAim(at, gunSeat, gunYaw, gunPitch));
		const float y = gunYaw * 3.14159265f / 180.0f, x = gunPitch * 3.14159265f / 180.0f;
		const physics::Vec3 barrel = hull.transform({std::sin(y) * std::cos(x), std::sin(x), std::cos(y) * std::cos(x)});
		const physics::Vec3 way = p.velocity * (1 / std::sqrt(physics::lengthSquared(p.velocity)));
		const float off = std::acos(std::clamp(static_cast<double>(physics::dot(way, barrel)), -1.0, 1.0)) * 57.29578;
		std::fprintf(stderr, "  Shepherd seat %d: gun %.1f/%.1f, round off the barrel by %.2f degrees\n", seatKey, gunYaw, gunPitch, off);
		CHECK(std::fabs(gunYaw - yaw) > 10); // the gun did turn
		CHECK(off < 2.0f);
	}
}

// A pod seat of the APC aiming up: how high and far the pod goes.
TEST(Vehicles_ApcPodFlightWhenAimedUp)
{
	if (!gameDir()) {
		return;
	}
	for (const float aim : {0.0f, 30.0f, 60.0f, 80.0f}) {
		Garage g(gameDir());
		if (!g.loaded || !g.templates.find("eu_apc_pods")) {
			return;
		}
		const uint32_t apc = g.start("eu_apc_pods", {0, 2, 6}, 3);
		CHECK(g.enter(apc));
		game::SoldierInput seat;
		seat.switchSeat = 4; // a pod seat (F3)
		g.world->setPlayerInput(seat);
		g.world->tick(1 / 30.0f);
		g.world->setPlayerInput({});
		g.run(0.5f);
		game::GameWorld::PlayerInfo me;
		g.world->playerInfo(g.world->localPlayer(), me);
		const physics::Vec3 start = g.position(apc);
		game::SoldierInput fire;
		fire.altFire = true;
		fire.aimPitch = aim;
		g.world->setPlayerInput(fire);
		g.world->tick(1 / 30.0f);
		g.world->setPlayerInput({});
		float top = -1e9f, far = 0;
		bool launched = false;
		for (int i = 0; i < 30 * 12; ++i) {
			game::SoldierInput hold;
			hold.aimPitch = aim;
			g.world->setPlayerInput(hold);
			g.world->tick(1 / 30.0f);
			g.world->playerInfo(g.world->localPlayer(), me);
			if (me.vehicle != 0 && me.vehicle != apc) {
				launched = true;
				const physics::Vec3 p = g.position(me.vehicle);
				top = std::max(top, p.y - start.y);
				far = std::max(far, std::hypot(p.x - start.x, p.z - start.z));
			}
		}
		std::fprintf(stderr, "  aim %.0f: pod launched %d, peak %.1f m above the APC, %.1f m out\n", aim, launched, top, far);
	}
}

// The actual APC pod carries a mouse-look ForceObject behind its centre.
// Opposite mouse input must turn the pod in opposite directions; no input
// is the control. This covers the launch and the authored child template.
TEST(Vehicles_ApcPodMouseTurns)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		float turns[3] = {};
		for (int sample = 0; sample < 3; ++sample) {
			Garage g(gameDir(), mod);
			CHECK(g.loaded && g.templates.find("eu_apc_pods"));
			if (!g.loaded || !g.templates.find("eu_apc_pods")) return;
			const uint32_t apc = g.start("eu_apc_pods", {0, 2, 6}, 3);
			CHECK(g.enter(apc));
			game::SoldierInput seat; seat.switchSeat = 4;
			g.world->setPlayerInput(seat); g.world->tick(1 / 30.0f);
			g.world->setPlayerInput({}); g.run(0.5f);
			game::SoldierInput fire; fire.altFire = true;
			g.world->setPlayerInput(fire); g.world->tick(1 / 30.0f);
			CHECK(g.world->ridingPod(g.world->localPlayer()));
			if (g.world->pods().empty()) return;
			const uint32_t id = g.world->pods()[0].object;
			const world::Object* pod = g.objects.find(id);
			CHECK(pod != nullptr);
			if (!pod) return;
			const float initial = pod->rotation().x;
			game::SoldierInput input; input.turn = (sample - 1) * 5.0f;
			for (int tick = 0; tick < 90; ++tick) {
				g.world->setPlayerInput(input); g.world->tick(1 / 30.0f);
			}
			pod = g.objects.find(id);
			CHECK(pod != nullptr);
			if (!pod) return;
			turns[sample] = std::remainder(pod->rotation().x - initial, 360.0f);
		}
		std::fprintf(stderr, "  %s mouse yaw changes: %.2f %.2f %.2f degrees\n", mod, turns[0], turns[1], turns[2]);
		CHECK(std::fabs(turns[0] - turns[1]) > 1.0f);
		CHECK(std::fabs(turns[2] - turns[1]) > 1.0f);
		CHECK((turns[0] - turns[1]) * (turns[2] - turns[1]) < 0);
	}
}

// Actual APC Conquest pods: compare two seconds of throttle and yaw input
// with an unsteered launch. The authored offset forces must also tilt the pod.
TEST(Vehicles_ApcPodSteeringForces)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) for (const bool titanPod : {false, true}) {
		physics::Vec3 end[3] = {};
		float maxTilt[3] = {};
		for (int sample = 0; sample < 3; ++sample) {
			Garage g(gameDir(), mod);
			CHECK(g.loaded && g.templates.find("eu_apc_pods"));
			if (!g.loaded || !g.templates.find("eu_apc_pods")) return;
			// The garage has no Titan: choose the boarding projectile directly
			// while retaining the real pod template and the real APC launcher.
			CHECK(g.templates.find("pod_launcher_cq") != nullptr);
			if (!g.templates.find("pod_launcher_cq")) return;
			if (titanPod) g.templates.find("pod_launcher_cq")->settings()["projectileTemplate"] = "eu_pod";
			const uint32_t apc = g.start("eu_apc_pods", {0, 2, 6}, 3);
			CHECK(g.enter(apc));
			game::SoldierInput seat; seat.switchSeat = 4;
			g.world->setPlayerInput(seat); g.world->tick(1 / 30.0f);
			g.world->setPlayerInput({}); g.run(0.5f);
			game::SoldierInput fire; fire.altFire = true;
			g.world->setPlayerInput(fire); g.world->tick(1 / 30.0f);
			CHECK(g.world->ridingPod(g.world->localPlayer()));
			if (g.world->pods().empty()) return;
			const uint32_t id = g.world->pods()[0].object;
			game::SoldierInput input;
			input.throttle = sample == 1 ? 1 : 0;
			input.strafe = sample == 2 ? 1 : 0;
			for (int tick = 0; tick < 60; ++tick) {
				g.world->setPlayerInput(input); g.world->tick(1 / 30.0f);
				const world::Object* pod = g.objects.find(id);
				if (!pod) return;
				maxTilt[sample] = std::max(maxTilt[sample], std::max(std::fabs(pod->rotation().y), std::fabs(pod->rotation().z)));
			}
			end[sample] = g.position(id);
		}
		const auto forward = end[1] - end[0], sideways = end[2] - end[0];
		std::fprintf(stderr, "  %s %s forward delta %.2f/%.2f/%.2f tilt %.2f; sideways delta %.2f/%.2f/%.2f tilt %.2f\n", mod, titanPod ? "Titan" : "Conquest",
			forward.x, forward.y, forward.z, maxTilt[1], sideways.x, sideways.y, sideways.z, maxTilt[2]);
		CHECK(std::hypot(forward.x, forward.z) > (titanPod ? 20.0f : 8.0f));
		CHECK(std::hypot(sideways.x, sideways.z) > (titanPod ? 12.0f : 8.0f));
		CHECK(maxTilt[1] > maxTilt[0] + 0.1f);
		CHECK(maxTilt[2] > maxTilt[0] + 0.1f);
	}
}

// E leaves the air transport (each seat) while it sits on the ground.
TEST(Vehicles_TransportCanBeLeftWithUse)
{
	if (!gameDir()) {
		return;
	}
	for (const int seatNo : {1, 2, 3, 4}) {
		Garage g(gameDir());
		if (!g.loaded || !g.templates.find("eu_at")) {
			return;
		}
		const uint32_t at = g.start("eu_at", {0, 2, 6}, 3);
		CHECK(g.enter(at));
		game::SoldierInput go;
		go.switchSeat = seatNo;
		g.world->setPlayerInput(go);
		g.world->tick(1 / 30.0f);
		g.world->setPlayerInput({});
		g.run(1);
		game::GameWorld::PlayerInfo me;
		g.world->playerInfo(g.world->localPlayer(), me);
		const int before = me.seat;
		game::SoldierInput use;
		use.use = true;
		g.world->setPlayerInput(use);
		g.world->tick(1 / 30.0f);
		g.world->setPlayerInput({});
		g.run(1);
		g.world->playerInfo(g.world->localPlayer(), me);
		std::fprintf(stderr, "  seat %d (was %d): after E vehicle %u (transport %u) pods %zu\n", seatNo, before, me.vehicle, at, g.world->pods().size());
		CHECK(me.vehicle != at);
	}
}

// Spawned straight into a vehicle (the spawn menu's vehicle), E leaves it.
TEST(Vehicles_SpawnedIntoTheTransportCanLeave)
{
	if (!gameDir()) {
		return;
	}
	for (const char* name : {"eu_at_spawn", "eu_apc_pods", "eu_transport"}) {
		Garage g(gameDir());
		if (!g.loaded || !g.templates.find(name)) {
			continue;
		}
		const uint32_t vehicle = g.start(name, {0, 2, 6}, 3);
		const int me = g.world->addPlayer("Me", 2, false);
		g.world->setSpawnVehicle(me, vehicle);
		const bool spawned = g.world->spawnSoldier(me);
		game::GameWorld::PlayerInfo info;
		g.world->playerInfo(me, info);
		const uint32_t in = info.vehicle;
		g.run(1);
		game::SoldierInput use;
		use.use = true;
		g.world->setInput(me, use);
		g.run(0.1f);
		g.world->setInput(me, {});
		g.run(1);
		g.world->playerInfo(me, info);
		std::fprintf(stderr, "  %s: spawned %d in %u seat, after E in %u\n", name, spawned, in, info.vehicle);
		CHECK(spawned && in == vehicle && info.vehicle == 0);
	}
}

// The Shepherd flown off and left (the pilot dies in it), then spawned
// into again: E must leave it, parked wherever it came down.
TEST(Vehicles_ShepherdFlownOffThenSpawnedIntoCanBeLeft)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("eu_at_spawn")) {
		return;
	}
	const uint32_t at = g.start("eu_at_spawn", {0, 2, 6}, 3);
	CHECK(g.enter(at));
	game::SoldierInput fly;
	fly.throttle = 1;
	fly.jump = true;
	g.drive(fly, 6);
	game::SoldierInput down;
	down.throttle = 0.3f;
	down.pose = game::SoldierPose::Crouch;
	for (int i = 0; i < 100 && g.position(at).y > 3; ++i) {
		g.drive(down, 3);
	}
	g.drive({}, 5);
	const physics::Vec3 where = g.position(at);
	const int me = g.world->localPlayer();
	game::GameWorld::PlayerInfo info;
	g.world->playerInfo(me, info);
	std::fprintf(stderr, "  landed at %.0f %.1f %.0f, pilot in %u\n", where.x, where.y, where.z, info.vehicle);
	g.world->applyDamage(g.world->playerObject(), 1e6f);
	g.run(8);
	g.world->setSpawnVehicle(me, at);
	const bool spawned = g.world->spawnSoldier(me);
	g.world->playerInfo(me, info);
	const uint32_t in = info.vehicle;
	game::SoldierInput use;
	use.use = true;
	int firstTick = -1;
	for (int tick = 0; tick < 90 && firstTick < 0; ++tick) {
		g.world->setInput(me, tick % 2 ? use : game::SoldierInput {}); // a press every other tick
		g.run(1 / 30.0f);
		g.world->playerInfo(me, info);
		firstTick = info.vehicle == 0 ? tick : -1;
	}
	std::fprintf(stderr, "  spawned %d in %u (vehicle %u), left after tick %d\n", spawned, in, at, firstTick);
	CHECK(spawned && in == at && info.vehicle == 0);
}

// Leaving the Shepherd in flight: the exit key drops the soldier in a pod from
// the air_pod_launcher under the hull (exitThroughPodLauncher); on the
// ground it is a walk out.
TEST(Vehicles_ShepherdExitDropsAPodInFlight)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("eu_at")) {
		return;
	}
	const uint32_t at = g.start("eu_at", {0, 2, 6}, 3);
	CHECK(g.enter(at));
	game::SoldierInput use;
	use.use = true;
	// On the ground: out on foot, no pod.
	g.world->setPlayerInput(use);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(0.5f);
	std::fprintf(stderr, "  ground E: pods %zu, vehicle %u\n", g.world->pods().size(), g.world->playerVehicle() ? g.world->playerVehicle()->id() : 0u);
	CHECK(g.world->pods().empty());
	CHECK(g.world->playerVehicle() == nullptr);
	CHECK(g.enter(at));
	game::SoldierInput fly;
	fly.throttle = 1;
	fly.jump = true;
	g.drive(fly, 6);
	const float height = g.position(at).y;
	g.world->setPlayerInput(use);
	g.world->tick(1 / 30.0f);
	g.world->setPlayerInput({});
	g.run(0.2f);
	const bool podded = g.world->pods().size() == 1;
	std::fprintf(stderr, "  left at %.1f m: pods %zu, in vehicle %u\n", height, g.world->pods().size(),
		g.world->playerVehicle() ? g.world->playerVehicle()->id() : 0u);
	CHECK(podded);
	g.run(60);
	CHECK(g.world->pods().empty());
	CHECK(g.world->playerVehicle() == nullptr);
}

// Missiles at each enemy vehicle from four sides: any that passes through.
TEST(Vehicles_MissilesHitEveryEnemyVehicle)
{
	if (!gameDir()) {
		return;
	}
	int through = 0;
	for (const char* name : {"as_fav", "eu_fav", "as_tank", "eu_tank", "as_apc_pods", "eu_apc_pods", "as_heavy_mech", "us_heavy_mech", "eu_ifv", "as_ag", "us_ag", "as_hov_light"}) {
		for (const char* missile : {"unl_av_missile", "eu_av_missile", "as_av_missile"}) {
			for (int side = 0; side < 4; ++side) {
				Garage g(gameDir());
				if (!g.loaded || !g.templates.find(name) || !g.templates.find(missile)) {
					continue;
				}
				const uint32_t vehicle = g.start(name, {0, 4, 0}, 3);
				const int shooter = g.world->addPlayer("Shooter", name[0] == 'a' ? 2 : 1, true);
				const physics::Vec3 at = g.position(vehicle);
				const physics::Vec3 dir = side == 0 ? physics::Vec3 {1, 0, 0} : side == 1 ? physics::Vec3 {-1, 0, 0} : side == 2 ? physics::Vec3 {0, 0, 1} : physics::Vec3 {0, 0, -1};
				const float before = g.world->hitPoints(vehicle);
				g.world->launchProjectile(missile, at + dir * 120.0f + physics::Vec3 {0, 0.5f, 0} + physics::Vec3 {dir.z, 0, dir.x} * 3.0f, dir * -60.0f, shooter);
				g.run(5.0f);
				const bool hit = !g.objects.find(vehicle) || g.world->hitPoints(vehicle) < before;
				if (!hit) {
					++through;
					std::fprintf(stderr, "  %s: %s passed through from side %d\n", name, missile, side);
				}
			}
		}
	}
	std::fprintf(stderr, "  %d passes\n", through);
}

// Where a round from the side meets the EU FAV: a grid of heights and offsets.
TEST(Vehicles_EuFavSideGrid)
{
	if (!gameDir()) {
		return;
	}
	for (const char* name : {"eu_fav", "as_fav"}) {
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find(name)) {
		return;
	}
	std::fprintf(stderr, "  %s\n", name);
	const uint32_t fav = g.start(name, {0, 4, 0}, 3);
	const physics::Vec3 at = g.position(fav);
	std::fprintf(stderr, "  FAV at %.2f %.2f %.2f\n", at.x, at.y, at.z);
	const int shooter = g.world->addPlayer("Shooter", 1, true);
	for (float y = -0.5f; y <= 2.01f; y += 0.5f) {
		std::string row;
		for (float z = -2.5f; z <= 2.51f; z += 0.5f) {
			const physics::Vec3 start = at + physics::Vec3 {25.0f, y, z};
			float fraction = 1;
			uint32_t object = 0;
			const bool hit = g.world->segmentHit(start, at + physics::Vec3 {-25.0f, y, z}, shooter, fraction, object);
			row += hit && object == fav ? '#' : (hit ? 'o' : '.');
		}
		std::fprintf(stderr, "   y %+.1f %s\n", y, row.c_str());
	}
	}
}

TEST(Vehicles_FavMeshLods)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	for (const char* name : {"eu_fav", "as_fav"}) {
		const world::ObjectTemplate* t = g.templates.find(name);
		if (!t) {
			continue;
		}
		const auto mesh = t->settings().find("collisionMesh");
		std::fprintf(stderr, "  %s: collisionMesh %s\n", name, mesh == t->settings().end() ? "(none)" : mesh->second.c_str());
		if (mesh == t->settings().end()) {
			continue;
		}
		const physics::CollisionMeshFile* file = g.collisionTemplates.mesh(mesh->second, g.files);
		if (!file) {
			continue;
		}
		for (size_t gi = 0; gi < file->geometries.size(); ++gi) {
			for (size_t si = 0; si < file->geometries[gi].subGeometries.size(); ++si) {
				for (const physics::CollisionLod& lod : file->geometries[gi].subGeometries[si].lods) {
					std::fprintf(stderr, "    geom %zu sub %zu: type %u faces %zu box %.1f %.1f %.1f .. %.1f %.1f %.1f\n", gi, si, lod.collisionType, lod.faces.size(),
						lod.boundsMin[0], lod.boundsMin[1], lod.boundsMin[2], lod.boundsMax[0], lod.boundsMax[1], lod.boundsMax[2]);
				}
			}
		}
	}
}

// How much of each vehicle's solid hull (its vehicle LOD) a projectile ray
// meets at the projectile LOD, geometry 0 alone (the children's parts are
// not counted): rays through a grid from the four sides and from above.
TEST(Vehicles_ProjectileCoverageSurvey)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	for (const char* name : {"eu_fav", "as_fav", "eu_tank", "as_tank", "us_heavy_mech", "as_heavy_mech", "eu_apc", "as_apc", "eu_apc_pods", "as_apc_pods", "as_ag",
			 "us_ag", "eu_bipod", "as_bipod", "eu_at", "as_at", "eu_ifv", "eu_apc_alps", "eu_apc_np", "as_apc_np", "as_hov_light", "eu_hov_light", "eu_mech",
			 "us_bipod", "static_av", "static_aa", "static_ap"}) {
		const world::ObjectTemplate* t = g.templates.find(name);
		if (!t) {
			continue;
		}
		const auto setting = t->settings().find("collisionMesh");
		if (setting == t->settings().end()) {
			continue;
		}
		const physics::CollisionMeshFile* file = g.collisionTemplates.mesh(setting->second, g.files);
		if (!file || file->geometries.empty()) {
			continue;
		}
		physics::CollisionWorld vehicle, projectile;
		vehicle.addMesh(*file, physics::CollisionUser::Vehicle, {}, {}, 1, {});
		projectile.addMesh(*file, physics::CollisionUser::Projectile, {}, {}, 1, {});
		int onHull = 0, covered = 0;
		for (int axis = 0; axis < 3; ++axis) {
			for (int sign = -1; sign <= 1; sign += 2) {
				for (float a = -3; a <= 3.01f; a += 0.5f) {
					for (float b = -3; b <= 3.01f; b += 0.5f) {
						physics::Vec3 from {}, to {};
						const float far = 30.0f * sign;
						if (axis == 0) {
							from = {far, a, b};
							to = {-far, a, b};
						} else if (axis == 1) {
							from = {a, far, b};
							to = {a, -far, b};
						} else {
							from = {a, b, far};
							to = {a, b, -far};
						}
						physics::CollisionWorld::Hit hit;
						if (!vehicle.raycast(from, to, hit)) {
							continue;
						}
						++onHull;
						covered += projectile.raycast(from, to, hit) ? 1 : 0;
					}
				}
			}
		}
		std::fprintf(stderr, "  %-14s hull rays %4d, met by the projectile LOD %4d (%3d%%)\n", name, onHull, covered, onHull ? covered * 100 / onHull : 0);
	}
}

// An AV missile fired a little off a parked enemy vehicle from 120 m: does it
// home on it, and where does it end.
TEST(Vehicles_MissileHomingOnAParkedVehicle)
{
	if (!gameDir()) {
		return;
	}
	for (const char* name : {"eu_apc", "eu_fav", "as_tank"}) {
		Garage g(gameDir());
		if (!g.loaded || !g.templates.find(name) || !g.templates.find("unl_av_missile")) {
			continue;
		}
		const uint32_t vehicle = g.start(name, {0, 4, 0}, 3);
		const int shooter = g.world->addPlayer("Shooter", name[0] == 'a' ? 2 : 1, true);
		const physics::Vec3 at = g.position(vehicle);
		const float before = g.world->hitPoints(vehicle);
		g.world->launchProjectile("unl_av_missile", at + physics::Vec3 {120, 0.5f, 6}, {-60, 0, 0}, shooter);
		float closest = 1e9f;
		bool gone = false;
		for (int tick = 0; tick < 150; ++tick) {
			g.run(1 / 30.0f);
			if (g.world->projectiles().empty()) {
				gone = true;
				break;
			}
			const physics::Vec3 p = g.world->projectiles().front().position;
			closest = std::min(closest, std::sqrt(physics::lengthSquared(p - at)));
		}
		std::fprintf(stderr, "  %s: closest %.1f m, round %s, hit points %.0f -> %.0f\n", name, closest, gone ? "gone" : "still flying", before, g.world->hitPoints(vehicle));
	}
}

#ifdef BF2142_RENDER_TESTS
// The actual bridge glow is a black-background quad using StaticMesh.fx's
// alpha_one technique. Its black texels must not darken the scene behind it.
TEST(Render_Glow3AdditivePixelsOptIn)
{
    const char* output = std::getenv("BF2142_RENDER_TEST_DIR");
    if (!output || !gameDir()) return;
    std::filesystem::create_directories(output);
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    {
        std::string error;
        auto gpu = render::GpuContext::create(nullptr, error);
        if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); CHECK(gpu); return; }
        for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
            Garage g(gameDir(), mod, true);
            auto* glow = g.templates.find("glow3");
            CHECK(g.loaded && glow);
            if (!g.loaded || !glow) continue;
            // The authored glow mesh is visual: its .con comments out
            // collisionMesh, so it is not a road obstruction.
            CHECK(glow->settings().count("collisionMesh") == 0);
            world::ObjectManager objects;
            auto* object = objects.create(glow, {0, 0, 0});
            terrain::HeightmapCluster noTerrain;
            scene::LightSettings lights;
            lights.fogColor = {160, 170, 180};
            lights.fogStartEndAndBase = {1000, 2000, 1, 0};
            scene::SkySettings sky;
            scene::WaterSettings water;
            scene::RendererSettings settings;
            settings.drawSkyDome = false;
            settings.drawTerrain = false;
            render::LevelSources sources {g.files, g.templates, objects, g.geometries, noTerrain, nullptr, lights, sky, water, "", ""};
            sources.renderer = &settings;
            render::LevelRenderer renderer(*gpu);
            renderer.build(sources);
            auto target = gpu->createOffscreenTarget(640, 360);
            render::Camera camera;
            camera.position = {0, 0, -8};
            camera.yaw = 0;
            const auto frame = [&]() {
                renderer.syncObjects();
                auto* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
                renderer.draw(commands, target.color, target, camera, lights);
                CHECK(SDL_SubmitGPUCommandBuffer(commands));
                std::vector<uint8_t> pixels;
                CHECK(gpu->readPixels(target, pixels));
                return pixels;
            };
            object->setHidden(true);
            const auto background = frame();
            object->setHidden(false);
            const auto visible = frame();
            size_t dark = 0, bright = 0;
            for (size_t i = 0; i + 3 < visible.size() && i + 3 < background.size(); i += 4) {
                int delta = 0;
                for (int c = 0; c < 3; ++c) delta += int(visible[i+c]) - int(background[i+c]);
                if (delta < -9) ++dark;
                if (delta > 9) ++bright;
            }
            const char* label = std::string(mod).find("Remaster") == std::string::npos ? "vanilla" : "remaster";
            std::fprintf(stderr, "  %s glow3: %zu darkened, %zu brightened pixels\n", label, dark, bright);
            CHECK(render::writePng(std::string(output) + "/" + label + "-glow3.png", target.width, target.height, visible));
            CHECK(dark == 0);
            CHECK(bright > 100);
            gpu->releaseTarget(target);
        }
    }
    SDL_Quit();
}

// Actual Suez bridge, using the map's placed glow3 and bridge meshes.
TEST(Render_SuezBridgeGlowPixelsOptIn)
{
    const char* output = std::getenv("BF2142_RENDER_TEST_DIR");
    if (!output || !gameDir()) return;
    std::filesystem::create_directories(output);
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    {
        std::string error;
        auto gpu = render::GpuContext::create(nullptr, error);
        if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); CHECK(gpu); return; }
        for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
            geom::GeometryTemplateManager geometries;
            titantest::TitanLevel level(gameDir(), "Suez_Canal", 48, true, mod, true, &geometries);
            CHECK(level.loaded);
            if (!level.loaded) continue;
            world::Object* glow = nullptr;
            for (const auto& o : level.objects.objects()) {
                if (o->objectTemplate() && o->objectTemplate()->name() == "glow3"
                    && std::fabs(o->position().x + 260.894f) < 0.1f && std::fabs(o->position().z + 31.995f) < 0.1f)
                    glow = o.get();
            }
            CHECK(glow);
            if (!glow) continue;
            scene::LightSettings lights;
            lights.fogStartEndAndBase = {1000, 2000, 1, 0};
            scene::SkySettings sky; scene::WaterSettings water;
            scene::RendererSettings settings;
            settings.drawSkyDome = false; settings.drawTerrain = false;
            render::LevelSources sources {level.files, level.templates, level.objects, geometries, level.heightmaps,
                nullptr, lights, sky, water, "", ""};
            sources.renderer = &settings;
            render::LevelRenderer renderer(*gpu); renderer.build(sources);
            auto target = gpu->createOffscreenTarget(640, 360);
            render::Camera camera; camera.position = {-270, 52.5f, -39};
            camera.yaw = std::atan2(9.106f, 7.005f);
            camera.pitch = std::atan2(-0.8f, std::hypot(9.106f, 7.005f));
            const auto frame = [&]() {
                renderer.syncObjects();
                auto* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
                renderer.draw(commands, target.color, target, camera, lights);
                CHECK(SDL_SubmitGPUCommandBuffer(commands));
                std::vector<uint8_t> pixels; CHECK(gpu->readPixels(target, pixels)); return pixels;
            };
            glow->setHidden(true); const auto background = frame();
            const auto backgroundAgain = frame();
            glow->setHidden(false); const auto visible = frame();
            size_t dark = 0, bright = 0, noise = 0;
            for (size_t i = 0; i + 3 < visible.size() && i + 3 < background.size(); i += 4) {
                int delta = 0, baseDelta = 0;
                for (int c = 0; c < 3; ++c) {
                    delta += int(visible[i+c]) - int(background[i+c]);
                    baseDelta += std::abs(int(backgroundAgain[i+c]) - int(background[i+c]));
                }
                if (baseDelta > 9) ++noise;
                if (delta < -9) ++dark;
                if (delta > 9) ++bright;
            }
            const char* label = std::string(mod).find("Remaster") == std::string::npos ? "vanilla" : "remaster";
            std::fprintf(stderr, "  %s Suez bridge glow3: %zu darkened, %zu brightened pixels, baseline noise %zu\n", label, dark, bright, noise);
            CHECK(render::writePng(std::string(output) + "/" + label + "-suez.png", target.width, target.height, visible));
            // The full scene changes slightly between identical frames;
            // bound darkening by that measured baseline noise. The isolated
            // asset check above requires exactly zero darkened pixels.
            CHECK(dark <= noise); CHECK(bright > 100); CHECK(bright > noise * 3);
            gpu->releaseTarget(target);
        }
    }
    SDL_Quit();
}

// Exercise the shipped Titan scene with and without the retail static-part
// switch. Camera membership in the native renderer is only an approximation;
// changing that switch must not remove static geometry from the same view.
TEST(Titan_StaticVisibilityPixelsOptIn)
{
	const char* output = std::getenv("BF2142_RENDER_TEST_DIR");
	if (!output || !gameDir()) return;
	std::filesystem::create_directories(output);
	CHECK(SDL_Init(SDL_INIT_VIDEO));
	{
		std::string error;
		auto gpu = render::GpuContext::create(nullptr, error);
		if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); CHECK(gpu); return; }
		for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
			geom::GeometryTemplateManager geometries;
			titantest::TitanLevel level(gameDir(), "Suez_Canal", 48, true, mod, false, &geometries);
			CHECK(level.loaded);
			if (!level.loaded) continue;
			const auto* info = level.titan(2);
			CHECK(info);
			if (!info) continue;
			const auto* ship = level.objects.find(info->object);
			CHECK(ship);
			if (!ship) continue;
			const physics::Mat3 rotation = physics::Mat3::fromYawPitchRoll({ship->rotation().x, ship->rotation().y, ship->rotation().z});
			scene::LightSettings lights;
			lights.fogStartEndAndBase = {1000, 2000, 1, 0};
			scene::SkySettings sky;
			scene::WaterSettings water;
			scene::RendererSettings settings;
			settings.drawBundledMeshes = false;
			settings.drawSkinnedMeshes = false;
			settings.drawSkyDome = false;
			settings.drawTerrain = false;
			settings.insideTitanOverride = true;
			render::LevelSources sources {level.files, level.templates, level.objects, geometries, level.heightmaps,
				nullptr, lights, sky, water, "", ""};
			sources.renderer = &settings;
			render::LevelRenderer renderer(*gpu);
			renderer.build(sources);
			auto target = gpu->createOffscreenTarget(640, 360);
			for (const physics::Vec3 local : {physics::Vec3 {6, -1, 11}, physics::Vec3 {0, 12, -30}}) {
				const auto p = rotation.transform(local) + info->position;
				render::Camera camera;
				camera.position = {p.x, p.y, p.z};
				camera.yaw = ship->rotation().x * 3.14159265f / 180.0f;
				const auto frame = [&]() {
					renderer.syncObjects();
					auto* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
					renderer.draw(commands, target.color, target, camera, lights);
					CHECK(SDL_SubmitGPUCommandBuffer(commands));
					std::vector<uint8_t> pixels;
					CHECK(gpu->readPixels(target, pixels));
					return pixels;
				};
				settings.showAllTitanParts = false;
				const auto normal = frame();
				settings.showAllTitanParts = true;
				const auto all = frame();
				settings.drawStaticMeshes = false;
				const auto blank = frame();
				settings.drawStaticMeshes = true;
				const std::string label = std::string(mod).find("Remaster") != std::string::npos ? "remaster" : "vanilla";
				CHECK(render::writePng(std::string(output) + "/" + label + (local.y < 0 ? "-view-a.png" : "-view-b.png"), target.width, target.height, all));
				size_t different = 0, lit = 0, geometryPixels = 0;
				for (size_t i = 0; i + 3 < normal.size() && i + 3 < all.size(); i += 4) {
					if (normal[i] != all[i] || normal[i+1] != all[i+1] || normal[i+2] != all[i+2]) ++different;
					if (all[i] || all[i+1] || all[i+2]) ++lit;
					if (all[i] != blank[i] || all[i+1] != blank[i+1] || all[i+2] != blank[i+2]) ++geometryPixels;
				}
				std::fprintf(stderr, "  %s local %.0f/%.0f/%.0f: %zu changed, %zu lit, %zu geometry pixels\n", mod, local.x, local.y, local.z, different, lit, geometryPixels);
				CHECK(lit > 1000);
				CHECK(geometryPixels > 1000);
				CHECK(different == 0);
			}
		}
	}
	SDL_Quit();
}
#endif

// The native projectile-LOD rays for the retail comparison (work/fav-retail-audit/emu.py's grid):
// writes one '0'/'1' per ray to $BF2142_RAY_OUT.
TEST(Vehicles_NativeRaysForTheRetailComparison)
{
	const char* out = std::getenv("BF2142_RAY_OUT");
	const char* meshName = std::getenv("BF2142_RAY_MESH");
	if (!gameDir() || !out || !meshName) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const physics::CollisionMeshFile* file = g.collisionTemplates.mesh(meshName, g.files);
	CHECK(file != nullptr);
	if (!file) {
		return;
	}
	physics::CollisionWorld world;
	world.addMesh(*file, physics::CollisionUser::Projectile, {}, {}, 1, {});
	std::string bits;
	for (const int axis : {0, 2, 1}) {
		for (const int sign : {1, -1}) {
			for (int ia = -12; ia <= 12; ++ia) {
				for (int ib = -12; ib <= 12; ++ib) {
					float start[3] = {0, 0, 0}, delta[3] = {0, 0, 0};
					int other[2], n = 0;
					for (int i = 0; i < 3; ++i) {
						if (i != axis) {
							other[n++] = i;
						}
					}
					start[axis] = 30.0f * sign;
					delta[axis] = -60.0f * sign;
					start[other[0]] = ia * 0.25f;
					start[other[1]] = ib * 0.25f;
					physics::CollisionWorld::Hit hit;
					const physics::Vec3 from {start[0], start[1], start[2]};
					const physics::Vec3 to {start[0] + delta[0], start[1] + delta[1], start[2] + delta[2]};
					bits += world.raycast(from, to, hit) ? '1' : '0';
				}
			}
		}
	}
	std::ofstream(out) << bits;
}

// The driver of an open FAV is hit by a round from the side through the open frame (the
// original lets occupants of open vehicles be shot); the shooter's own vehicle's seats are not.
TEST(Vehicles_OpenVehicleOccupantCanBeShot)
{
	if (!gameDir()) {
		return;
	}
	for (const char* name : {"eu_fav", "as_fav"}) {
		Garage g(gameDir());
		if (!g.loaded || !g.templates.find(name)) {
			continue;
		}
		const uint32_t fav = g.start(name, {0, 2, 6}, 3);
		g.animate();
		CHECK(g.enter(fav));
		g.run(1);
		const int me = g.world->localPlayer();
		const int shooter = g.world->addPlayer("Shooter", 1, false);
		const world::ObjectTemplate* rifle = g.templates.find("Unl_Carbine");
		const std::string round = rifle && rifle->settings().count("projectileTemplate") ? rifle->settings().at("projectileTemplate") : "";
		CHECK(!round.empty());
		game::GameWorld::PlayerInfo info;
		g.world->playerInfo(me, info);
		const world::Object* sitter = g.objects.find(info.soldier);
		CHECK(sitter != nullptr);
		if (!sitter) {
			continue;
		}
		const physics::Vec3 seat {sitter->position().x, sitter->position().y, sitter->position().z};
		// A round per height and side; which of the driver and the vehicle it hurt.
		int driverHits = 0, vehicleHits = 0;
		for (int side = 0; side < 4; ++side) {
			const physics::Vec3 dir = side == 0 ? physics::Vec3 {1, 0, 0} : side == 1 ? physics::Vec3 {-1, 0, 0} : side == 2 ? physics::Vec3 {0, 0, 1} : physics::Vec3 {0, 0, -1};
			for (float y = 0.3f; y <= 1.7f; y += 0.1f) {
				game::GameWorld::PlayerInfo now;
				g.world->playerInfo(me, now);
				if (!now.alive) {
					break;
				}
				const float driverBefore = g.world->hitPoints(now.soldier);
				const float vehicleBefore = g.world->hitPoints(fav);
				g.world->launchProjectile(round, seat + dir * 25.0f + physics::Vec3 {0, y, 0}, dir * -300.0f, shooter);
				g.run(0.15f);
				game::GameWorld::PlayerInfo then;
				g.world->playerInfo(me, then);
				driverHits += (!then.alive || g.world->hitPoints(now.soldier) < driverBefore) ? 1 : 0;
				vehicleHits += g.world->hitPoints(fav) < vehicleBefore ? 1 : 0;
			}
		}
		std::fprintf(stderr, "  %s: %d rounds hurt the driver, %d the vehicle (of 60)\n", name, driverHits, vehicleHits);
		CHECK(driverHits > 0);
	}
}

#ifdef BF2142_RENDER_TESTS
// The shipped Titan signs are bundled meshes marked isTitanExterior. Isolate
// each real sign as a Titan child so the pixels cannot come from the hull.
TEST(Titan_ArrowSignVisibilityPixelsOptIn)
{
	const char* output = std::getenv("BF2142_RENDER_TEST_DIR");
	if (!output || !gameDir()) return;
	std::filesystem::create_directories(output);
	CHECK(SDL_Init(SDL_INIT_VIDEO));
	{
		std::string error;
		auto gpu = render::GpuContext::create(nullptr, error);
		if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); CHECK(gpu); return; }
		for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
			geom::GeometryTemplateManager geometries;
			titantest::TitanLevel level(gameDir(), "Suez_Canal", 48, true, mod, false, &geometries);
			CHECK(level.loaded);
			if (!level.loaded) continue;
			for (const char* name : {"eu_sign_arrow", "as_sign_arrow"}) {
				const auto* sign = level.templates.find(name);
				const auto* geometry = geometries.find(name);
				CHECK(sign && geometry && geometry->titanExterior);
				if (!sign || !geometry) continue;
				const auto* actualTitan = level.titan(name[0] == 'e' ? 2 : 1);
				CHECK(actualTitan);
				const auto* actualRoot = actualTitan ? level.objects.find(actualTitan->object) : nullptr;
				CHECK(actualRoot && actualRoot->objectTemplate());
				if (!actualRoot || !actualRoot->objectTemplate()) continue;
				const auto& children = actualRoot->objectTemplate()->children();
				CHECK(std::any_of(children.begin(), children.end(), [&](const auto& child) { return child.name == name; }));
				world::ObjectTemplate root(1, "SimpleObject", "sign_test_root", 1);
				root.children().push_back({name, {0, 0, 0}, {0, 0, 0}});
				world::ObjectManager objects;
				const uint32_t rootId = objects.create(&root, {0, 0, 0})->id();
				terrain::HeightmapCluster terrain;
				scene::LightSettings lights;
				lights.fogStartEndAndBase = {1000, 2000, 1, 0};
				scene::SkySettings sky;
				scene::WaterSettings water;
				scene::RendererSettings settings;
				settings.drawSkyDome = false;
				settings.drawTerrain = false;
				render::LevelSources sources {level.files, level.templates, objects, geometries, terrain,
					nullptr, lights, sky, water, "", ""};
				sources.renderer = &settings;
				bool ownerTitan = true;
				sources.titanObject = [&](int team) { return ownerTitan && team == 2 ? rootId : 0u; };
				bool destroyed = false;
				sources.partLook = [&](uint32_t, const std::string&) { return destroyed ? 2 : 0; };
				render::LevelRenderer renderer(*gpu);
				renderer.build(sources);
				auto target = gpu->createOffscreenTarget(640, 360);
				render::Camera camera;
				camera.position = {0, 0, 4};
				camera.yaw = 3.14159265f;
				const auto frame = [&]() {
					renderer.syncObjects();
					auto* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
					renderer.draw(commands, target.color, target, camera, lights);
					CHECK(SDL_SubmitGPUCommandBuffer(commands));
					std::vector<uint8_t> pixels;
					CHECK(gpu->readPixels(target, pixels));
					return pixels;
				};
				settings.drawBundledMeshes = false;
				const auto blank = frame();
				settings.drawBundledMeshes = true;
				settings.insideTitanOverride = false;
				const auto outside = frame();
				settings.insideTitanOverride = true;
				const auto inside = frame();
				ownerTitan = false;
				const auto other = frame();
				ownerTitan = true;
				destroyed = true;
				const auto hidden = frame();
				destroyed = false;
				camera.position = {0, 0, 60};
				const auto far = frame();
				const auto changed = [&](const std::vector<uint8_t>& pixels) {
					size_t count = 0;
					for (size_t i = 0; i + 3 < pixels.size() && i + 3 < blank.size(); i += 4)
						if (pixels[i] != blank[i] || pixels[i+1] != blank[i+1] || pixels[i+2] != blank[i+2]) ++count;
					return count;
				};
				const size_t outPixels = changed(outside), inPixels = changed(inside), otherPixels = changed(other);
				std::fprintf(stderr, "  %s %s: outside %zu, inside %zu, non-Titan %zu, destroyed %zu, far %zu pixels\n", mod, name, outPixels, inPixels, otherPixels, changed(hidden), changed(far));
				CHECK(outPixels > 100);
				CHECK(inPixels > 100);
				CHECK(otherPixels == 0);
				CHECK(changed(hidden) == 0);
				CHECK(changed(far) == 0);
				const std::string label = std::string(mod).find("Remaster") != std::string::npos ? "remaster-" : "vanilla-";
				CHECK(render::writePng(std::string(output) + "/" + label + name + ".png", target.width, target.height, inside));
			}
		}
	}
	SDL_Quit();
}
#endif

#ifdef BF2142_RENDER_TESTS
// Full Suez Titan tree at the authored arrow, with the gameplay callbacks.
TEST(Titan_AuthoredArrowScenePixelsOptIn)
{
	const char* output = std::getenv("BF2142_RENDER_TEST_DIR");
	if (!output || !gameDir()) return;
	std::filesystem::create_directories(output);
	CHECK(SDL_Init(SDL_INIT_VIDEO));
	{
		std::string error;
		auto gpu = render::GpuContext::create(nullptr, error);
		if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); CHECK(gpu); return; }
		for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
			geom::GeometryTemplateManager geometries;
			titantest::TitanLevel level(gameDir(), "Suez_Canal", 48, true, mod, false, &geometries);
			CHECK(level.loaded);
			if (!level.loaded) continue;
			const auto* info = level.titan(2);
			const auto* ship = info ? level.objects.find(info->object) : nullptr;
			CHECK(ship && ship->objectTemplate());
			if (!ship || !ship->objectTemplate()) continue;
			const auto& children = ship->objectTemplate()->children();
			const auto found = std::find_if(children.begin(), children.end(), [](const auto& c) {
				return c.name == "eu_sign_arrow" && std::fabs(c.position.x - 6.087f) < 0.01f;
			});
			CHECK(found != children.end());
			if (found == children.end()) continue;
			const auto hullRotation = physics::Mat3::fromYawPitchRoll({ship->rotation().x, ship->rotation().y, ship->rotation().z});
			const auto signRotation = physics::Mat3::fromYawPitchRoll({found->rotation.x, found->rotation.y, found->rotation.z});
			const physics::Vec3 origin {ship->position().x, ship->position().y, ship->position().z};
			const auto signPos = origin + hullRotation.transform({found->position.x, found->position.y, found->position.z});
			scene::LightSettings lights;
			lights.fogStartEndAndBase = {1000, 2000, 1, 0};
			scene::SkySettings sky;
			scene::WaterSettings water;
			scene::RendererSettings settings;
			settings.drawSkyDome = false;
			settings.drawTerrain = false;
			render::LevelSources sources {level.files, level.templates, level.objects, geometries, level.heightmaps,
				nullptr, lights, sky, water, "", ""};
			sources.renderer = &settings;
			sources.titanObject = [&](int team) { return level.world->titanOf(team); };
			sources.partTransforms = [&](uint32_t obj, std::vector<anim::Matrix>& parts) { return level.world->partTransforms(obj, parts); };
			sources.partBone = [&](uint32_t obj, const std::string& part, anim::Matrix& m) { return level.world->partBone(obj, part, m); };
			sources.partPose = [&](uint32_t obj, const std::string& part, world::Vec3& offset, world::Vec3& rot) { return level.world->partPose(obj, part, offset, rot); };
			bool hideSigns = false;
			sources.partLook = [&](uint32_t obj, const std::string& path) {
				return hideSigns && path == "eu_sign_arrow" ? 2 : level.world->titanPartLook(obj, path);
			};
			render::LevelRenderer renderer(*gpu);
			renderer.build(sources);
			auto target = gpu->createOffscreenTarget(960, 540);
			for (float distance : {2.0f, 4.0f, -1.0f}) {
				const auto eye = distance < 0 ? origin + physics::Vec3 {70.4f, 8.0f, 4.1f} : signPos + hullRotation.transform(signRotation.transform({0, 0, distance}));
				const auto offset = eye - signPos;
				render::Camera camera;
				camera.position = {eye.x, eye.y, eye.z};
				camera.yaw = std::atan2(-offset.x, -offset.z);
				camera.pitch = std::atan2(-offset.y, std::hypot(offset.x, offset.z));
				const auto frame = [&]() {
					renderer.syncObjects();
					auto* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
					renderer.draw(commands, target.color, target, camera, lights);
					CHECK(SDL_SubmitGPUCommandBuffer(commands));
					std::vector<uint8_t> pixels;
					CHECK(gpu->readPixels(target, pixels));
					return pixels;
				};
				if (distance == 2.0f) {
					camera.fovY = 0.55f; // an earlier view established a narrower base FOV
					frame();
					camera.fovY = 1.0f;
				}
				hideSigns = false;
				const auto shown = frame();
				hideSigns = true;
				const auto hidden = frame();
				size_t changed = 0;
				for (size_t i = 0; i + 3 < shown.size() && i + 3 < hidden.size(); i += 4)
					if (std::abs(int(shown[i])-int(hidden[i])) > 3 || std::abs(int(shown[i+1])-int(hidden[i+1])) > 3 || std::abs(int(shown[i+2])-int(hidden[i+2])) > 3) ++changed;
				std::fprintf(stderr, "  %s authored sign %.1fm: %zu pixels; titan-relative eye %.2f/%.2f/%.2f\n", mod, distance, changed, eye.x-origin.x, eye.y-origin.y, eye.z-origin.z);
				const std::string label = std::string(mod).find("Remaster") != std::string::npos ? "remaster" : "vanilla";
				CHECK(render::writePng(std::string(output) + "/" + label + "-" + std::to_string(int(distance)) + ".png", target.width, target.height, shown));
				CHECK(changed > 20);
			}
		}
	}
	SDL_Quit();
}
#endif

// Fire actual installed projectiles vertically into flat terrain alongside
// soldiers. This exercises flight, impact, detonation and material damage,
// not just a direct call to the blast helper.
TEST(Vehicles_InstalledMissileSplashHeadless)
{
	CHECK(gameDir() != nullptr);
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* projectile : {"us_heavy_mech_dumrockets_projectile", "as_heavy_mech_dumrockets_projectile",
				"US_AG_PilotWeapon_Projectile", "eu_av_missile", "as_av_missile"}) {
			Garage g(gameDir(), mod);
			CHECK(g.loaded && g.templates.find(projectile));
			if (!g.loaded || !g.templates.find(projectile)) continue;
			CHECK(g.start("eu_tank", {100, 2, 100}, 0) != 0);
			g.animate();
			const float ranges[] = {0.65f, 1.5f, 2.5f, 3.5f, 5.0f};
			uint32_t soldiers[5] {};
			int players[5] {};
			for (int i = 0; i < 5; ++i) {
				const int id = g.world->addPlayer("SplashTarget", 2, true);
				players[i] = id;
				CHECK(g.world->spawnSoldier(id));
				game::GameWorld::PlayerInfo info;
				CHECK(g.world->playerInfo(id, info));
				soldiers[i] = info.soldier;
				CHECK(g.world->setSoldierPosition(info.soldier, {ranges[i], 0, 0}));
			}
			g.run(0.1f);
			for (int i = 0; i < 5; ++i) {
				const auto* body = g.world->soldierBody(players[i]);
				CHECK(body != nullptr);
				if (body) CHECK(std::fabs(body->position.x - ranges[i]) < 0.05f && std::fabs(body->position.z) < 0.05f);
			}
			CHECK(g.world->launchProjectile(projectile, {0, 8, 0}, {0, -100, 0}));
			g.run(0.5f);
			const auto& impacts = g.world->impacts();
			CHECK(!impacts.empty());
			if (impacts.empty()) continue;
			const auto& hit = impacts.back();
			std::fprintf(stderr, "  splash %s %s impact=(%.2f,%.2f,%.2f) target=%u health:",
				mod, projectile, hit.point.x, hit.point.y, hit.point.z, hit.object);
			CHECK(hit.object == 0 && std::fabs(hit.point.y) < 0.05f);
			for (int i = 0; i < 5; ++i) std::fprintf(stderr, " %.2fm=%.2f", ranges[i], g.world->hitPoints(soldiers[i]));
			std::fprintf(stderr, "\n");
			CHECK(g.world->hitPoints(soldiers[4]) >= 99.99f);
			if (std::string(projectile).find("dumrockets") != std::string::npos)
				CHECK(g.world->hitPoints(soldiers[0]) >= 99.99f);
			if (std::string(projectile) == "US_AG_PilotWeapon_Projectile")
				CHECK(g.world->hitPoints(soldiers[0]) < 99.0f);
		}
	}
}

// The walker's driver primary rounds use a two metre blast, unlike its
// dumrockets. Keep the installed template values and soldier-origin
// distance/falloff covered for both factions and both shipped mods.
TEST(Vehicles_WalkerPrimarySplashAgainstInfantry)
{
	CHECK(gameDir() != nullptr);
	if (!gameDir()) return;
	struct Case { const char* vehicle; const char* projectile; uint32_t material; float damage; };
	const Case cases[] = {
		{"us_heavy_mech", "us_heavy_mech_minigun_firearm_Projectile", 40, 100},
		{"as_heavy_mech", "as_heavy_Mech_Cannon_Projectile", 42, 200},
	};
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const Case& item : cases) {
			Garage g(gameDir(), mod);
			CHECK(g.loaded && g.templates.find(item.vehicle) && g.templates.find(item.projectile));
			if (!g.loaded || !g.templates.find(item.vehicle) || !g.templates.find(item.projectile)) continue;
			const uint32_t walker = g.start(item.vehicle, {100, 3, 100});
			CHECK(walker != 0);
			const world::ObjectTemplate* projectile = g.templates.find(item.projectile);
			const auto setting = [projectile](const char* key) {
				const auto it = projectile->settings().find(key);
				return it == projectile->settings().end() ? std::string{} : it->second;
			};
			CHECK(setting("detonation.explosionRadius") == "2");
			CHECK(setting("detonation.explosionDamage") == (item.damage == 100 ? "100" : "200"));
			CHECK(setting("detonation.explosionMaterial") == std::to_string(item.material));
			CHECK(setting("detonation.detectionRadius") == "1.5");
			const int attacker = g.world->addPlayer("Walker driver", 2, false);
			const int targetTeam = 1;
			const float horizontal[] = {0.5f, 1.0f, 1.5f, 2.0f};
			int targets[4] {};
			uint32_t soldiers[4] {};
			for (int i = 0; i < 4; ++i) {
				targets[i] = g.world->addPlayer("Splash infantry", targetTeam, false);
				CHECK(g.world->spawnSoldier(targets[i]));
				game::GameWorld::PlayerInfo info;
				CHECK(g.world->playerInfo(targets[i], info));
				soldiers[i] = info.soldier;
				const float angle = static_cast<float>(i) * 1.57079632679f;
				CHECK(g.world->setSoldierPosition(info.soldier,
					{horizontal[i] * std::cos(angle), 0, horizontal[i] * std::sin(angle)}));
			}
			const uint32_t soldierMaterial = static_cast<uint32_t>(std::stoi(
				g.objects.find(soldiers[0])->objectTemplate()->settings().at("armor.defaultMaterial")));
			const float materialMod = g.materials.damageMod(item.material, soldierMaterial);
			CHECK(materialMod > 0);
			const physics::Vec3 blast {0, 0, 0};
			float before[4] {};
			float distance[4] {};
			float falloff[4] {};
			std::vector<game::GameWorld::BlastRayTrace> rayTraces;
			g.world->setBlastRayTraceSink([&rayTraces](const auto& ray) { rayTraces.push_back(ray); });
			for (int i = 0; i < 4; ++i) {
				const auto* body = g.world->soldierBody(targets[i]);
				CHECK(body != nullptr);
				if (!body) continue;
				distance[i] = std::sqrt((body->position.x - blast.x) * (body->position.x - blast.x)
					+ (body->position.y - blast.y) * (body->position.y - blast.y)
					+ (body->position.z - blast.z) * (body->position.z - blast.z));
				falloff[i] = distance[i] < 1.8f ? 1.0f : std::max(0.0f, (2.0f - distance[i]) / 0.2f);
				before[i] = g.world->hitPoints(soldiers[i]);
			}
			CHECK(g.world->detonate(item.projectile, blast, attacker));
			for (int i = 0; i < 4; ++i) {
				const float after = g.world->hitPoints(soldiers[i]);
				const float dealt = before[i] - after;
				const float visibleShare = falloff[i] * materialMod * item.damage > 0
					? dealt / (falloff[i] * materialMod * item.damage) : 0;
				const auto* body = g.world->soldierBody(targets[i]);
				const bool directRayClear = body && g.world->soldierPathClear(2, blast, body->position);
				std::fprintf(stderr,
					"  walker splash %s %s horiz=%.2f distance=%.3f falloff=%.3f material=%u vs soldier=%u mod=%.4f direct-ray=%s visible-share=%.3f final-damage=%.2f hp=%.2f->%.2f\n",
					mod, item.projectile, horizontal[i], distance[i], falloff[i], item.material, soldierMaterial, materialMod,
					directRayClear ? "clear" : "blocked", visibleShare, dealt, before[i], after);
				if (distance[i] < 2.0f) CHECK(after < before[i]);
				else CHECK(std::fabs(after - before[i]) < 1e-3f);
			}
			g.world->setBlastRayTraceSink({});
			for (const auto& ray : rayTraces) {
				std::fprintf(stderr, "    cover-ray soldier=%u bone=%d offset=%d %s blocker=%u from=(%.3f,%.3f,%.3f) to=(%.3f,%.3f,%.3f)\n",
					ray.soldier, ray.bone, ray.offset, ray.clear ? "clear" : "blocked", ray.blocker,
					ray.from.x, ray.from.y, ray.from.z, ray.to.x, ray.to.y, ray.to.z);
			}
		}
	}
}

// detectionRadius is a saveable detonation property, not a proximity fuse.
// A configured round that passes close to an enemy soldier without collision
// must keep flying until its ordinary TTL/impact rule fires.
TEST(Vehicles_DetectionRadiusDoesNotProximityDetonateOnInfantry)
{
	CHECK(gameDir() != nullptr);
	if (!gameDir()) return;
	Garage g(gameDir());
	if (!g.loaded) return;
	const uint32_t vehicle = g.start("eu_tank", {0, 2, 30});
	CHECK(vehicle != 0);
	auto* round = g.templates.create("GenericProjectile", "test_detection_radius_round");
	round->components().push_back("DefaultDetonationComp");
	round->settings()["detonation.detectionRadius"] = "1.5";
	round->settings()["detonation.explosionRadius"] = "2";
	round->settings()["detonation.explosionDamage"] = "100";
	round->settings()["detonation.explosionMaterial"] = "40";
	round->settings()["timeToLive"] = "10";
	const int enemy = g.world->addPlayer("Enemy infantry", 1, false);
	CHECK(g.world->spawnSoldier(enemy));
	game::GameWorld::PlayerInfo info;
	CHECK(g.world->playerInfo(enemy, info));
	CHECK(g.world->setSoldierPosition(info.soldier, {0, 0, 0}));
	g.run(0.1f);
	// This path is 1.0 m above the soldier origin, inside 1.5 m, but is
	// offset beyond the soldier's collision capsule.
	const float before = g.world->hitPoints(info.soldier);
	CHECK(g.world->launchProjectile("test_detection_radius_round", {1.25f, 2.0f, -4.0f}, {0, 0, 20}));
	g.run(0.2f);
	CHECK(g.world->projectilesInFlight() > 0);
	CHECK(std::fabs(g.world->hitPoints(info.soldier) - before) < 1e-3f);
}

TEST(Vehicles_EUWalkerAAGunnerSightAndShot)
{
	if (!gameDir()) return;
	for (float heading : {0.f, 90.f, 180.f, -90.f}) for (bool alt : {false, true}) for (bool authored : {false, true}) {
		Garage g(gameDir(), "mods/Project_Remaster_v17_Final");
		CHECK(g.loaded && g.templates.find("us_heavy_mech"));
		if (!g.loaded || !g.templates.find("us_heavy_mech")) continue;
		auto* weapon = g.templates.find(alt ? "us_heavy_mech_AA_Launcher" : "US_heavy_mech_AAcannon_barrel");
		CHECK(weapon != nullptr);
		if (!weapon) continue;
		if (!authored) weapon->settings()["deviation.minDev"] = "0";
		const auto walker = g.start("us_heavy_mech", {0, 4, 8});
		g.animate();
		CHECK(g.enter(walker));
		game::SoldierInput input;
		input.switchSeat = 2;
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
		input = {};
		input.turn = heading;
		input.aimPitch = 40;
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
		input.turn = 0;
		PlayerCamera camera(*g.world, *g.systems, nullptr);
		for (int i=0; i<60; ++i) { g.world->setPlayerInput(input); g.world->tick(1 / 30.0f); camera.tick(1 / 30.0f); }
		PlayerCamera::View sight;
		CHECK(camera.view(sight, 16.f / 9));
		input.fire = !alt;
		input.altFire = alt;
		g.world->setPlayerInput(input);
		bool fired = false;
		for (int i=0; i<90 && !fired; ++i) {
			g.world->tick(1 / 30.0f);
			for (const auto& p : g.world->projectiles()) {
				if (p.shooter != walker || !p.settings) continue;
				const float yaw = std::atan2(p.velocity.x, p.velocity.z);
				const float pitch = std::atan2(p.velocity.y, std::hypot(p.velocity.x, p.velocity.z));
				const float cosine = std::sin(sight.pitch)*std::sin(pitch) + std::cos(sight.pitch)*std::cos(pitch)*std::cos(yaw-sight.yaw);
				const float error = std::acos(std::clamp(cosine, -1.0f, 1.0f)) * 57.29578f;
				std::fprintf(stderr, " walker heading %.0f %s authored=%d: sight %.1f %.1f shot %.1f %.1f error %.1f %s\n", heading, alt ? "alt" : "primary", authored, sight.yaw*57.29578f, sight.pitch*57.29578f, yaw*57.29578f, pitch*57.29578f, error, p.settings->name.c_str());
				CHECK(error < (authored ? 15 : 2));
				fired = true;
				break;
			}
		}
		CHECK(fired);
	}
}

// The retail blast enumerator visits a vehicle's occupants after the vehicle.
TEST(Vehicles_BlastCanReachVehicleOccupant)
{
	CHECK(gameDir() != nullptr);
	if (!gameDir()) return;
	Garage g(gameDir());
	CHECK(g.loaded);
	if (!g.loaded) return;
	auto* blast = g.templates.create("GenericProjectile", "test_occupant_blast");
	blast->components().push_back("DefaultDetonationComp");
	blast->settings()["detonation.explosionDamage"] = "100";
	blast->settings()["detonation.explosionRadius"] = "5";
	blast->settings()["detonation.explosionMaterial"] = "42";
	auto* narrow = g.templates.create("GenericProjectile", "test_occupant_narrow_blast");
	narrow->components().push_back("DefaultDetonationComp");
	narrow->settings()["detonation.explosionDamage"] = "50";
	narrow->settings()["detonation.explosionRadius"] = "0.3";
	narrow->settings()["detonation.explosionMaterial"] = "42";
	const uint32_t vehicle = g.start("eu_fav", {0, 2, 6}, 1);
	CHECK(vehicle != 0 && g.enter(vehicle));
	g.run(0.1f);
	game::GameWorld::PlayerInfo info;
	CHECK(g.world->playerInfo(g.world->localPlayer(), info));
	CHECK(info.vehicle == vehicle);
	const auto* soldier = g.objects.find(info.soldier);
	CHECK(soldier != nullptr);
	if (!soldier) return;
	const auto p = soldier->position();
	const float before = g.world->hitPoints(info.soldier);
	CHECK(g.world->detonate("test_occupant_blast", {p.x + 20, p.y + 1, p.z}));
	CHECK(g.world->hitPoints(info.soldier) == before);
	const auto v = g.position(vehicle);
	const float fromCenter = std::sqrt((p.x-v.x)*(p.x-v.x) + (p.y+1-v.y)*(p.y+1-v.y) + (p.z-v.z)*(p.z-v.z));
	std::fprintf(stderr, "  occupant is %.2f m from vehicle center\n", fromCenter);
	CHECK(fromCenter > 0.3f);
	CHECK(g.world->detonate("test_occupant_narrow_blast", {p.x, p.y + 1, p.z}));
	const float afterNarrow = g.world->hitPoints(info.soldier);
	std::fprintf(stderr, "  narrow blast occupant health %.2f -> %.2f\n", before, afterNarrow);
	CHECK(afterNarrow < before - 1.0f);
	CHECK(g.world->detonate("test_occupant_blast", {p.x, p.y + 1, p.z}));
	const float after = g.world->hitPoints(info.soldier);
	std::fprintf(stderr, "  vehicle occupant splash health %.2f -> %.2f\n", before, after);
	CHECK(after < afterNarrow - 1.0f);
}

namespace {

// The shipped AI scripts on a garage: the behaviours and strategies
// (AI/AIBehaviours.ai, AI/AIDefault.ai, as main runs them) and every
// object's AI templates (Objects/**/ai/*.ai: the soldiers' SoldierCtrl, the
// vehicles' Unit and Mobile parts).
void loadShippedAi(Garage& g, game::BotSettings& settings)
{
	game::registerBotConsoleMethods(g.console, settings);
	std::string out;
	CHECK(g.console.run("AI/AIBehaviours.ai", {}, out));
	CHECK(g.console.run("AI/AIDefault.ai", {}, out));
	for (const std::string& script : world::templateScripts(g.files, "Objects/", true)) {
		if (script.size() > 3 && script.compare(script.size() - 3, 3, ".ai") == 0) {
			g.console.run(script, {}, out);
		}
	}
}

} // namespace

// A walking bot's look (BotsLook.cpp's look-around through BotManager::aim)
// on the shipped soldier and its SoldierCtrl. One bot is run every other
// frame (the round robin, 0x10098c00); the skipped frame holds its input
// while it moves. The executed frame's turn covers both ticks, so the held
// turn must not be applied again: that sent the view past its point and
// back each frame, a left-right swing many times a second while walking.
TEST(Vehicles_BotWalkingLookDoesNotSwing)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		Garage g(gameDir(), mod);
		if (!g.loaded) return;
		world::ObjectTemplate* enemyPoint = g.templates.create("ControlPoint", "swing_cp");
		enemyPoint->settings()["controlPointId"] = "1";
		enemyPoint->settings()["team"] = "1";
		enemyPoint->settings()["unableToChangeTeam"] = "1";
		g.objects.create(enemyPoint, {300, 0, 0});
		game::BotSettings settings;
		loadShippedAi(g, settings);
		g.start("eu_tank", {0, 2, -400});
		const int bot = g.world->addPlayer("SwingBot", 2, true);
		CHECK(g.world->spawnSoldier(bot));
		game::GameWorld::PlayerInfo info;
		g.world->playerInfo(bot, info);
		const world::Object* soldier = g.objects.find(info.soldier);
		CHECK(soldier && soldier->objectTemplate());
		const auto ai = soldier && soldier->objectTemplate() ? soldier->objectTemplate()->settings().find("aiTemplate")
															  : decltype(soldier->objectTemplate()->settings().end()) {};
		const bool shippedCtrl = soldier && ai != soldier->objectTemplate()->settings().end()
			&& settings.plugin(ai->second, "ControlInfo") != nullptr;
		CHECK(shippedCtrl); // the gains are the mod's, not aim's fallback
		game::BotManager bots(*g.world, settings);
		float lastYaw = info.yaw, lastStep = 0;
		int walking = 0, flips = 0;
		for (int tick = 0; tick < 30 * 30; ++tick) {
			bots.update(1 / 30.0f);
			g.world->tick(1 / 30.0f);
			g.world->playerInfo(bot, info);
			const float step = std::remainder(info.yaw - lastYaw, 360.0f);
			lastYaw = info.yaw;
			if (info.alive && !info.vehicle && info.velocity.x * info.velocity.x + info.velocity.z * info.velocity.z > 1) {
				++walking;
				// A swing: a turn of over half a degree undone the next tick.
				flips += std::fabs(step) > 0.5f && std::fabs(lastStep) > 0.5f && (step > 0) != (lastStep > 0);
			}
			lastStep = step;
		}
		std::fprintf(stderr, "  %s: walking %d ticks, %d swings\n", mod, walking, flips);
		CHECK(walking > 30 * 5);
		CHECK(flips * 20 < walking);
	}
}

// Getting into a tank on foot (BBPChange's plan, AIDLL 0x1006dac0 from
// 0x1006e9b9): the walk goes at the vehicle and beside it the use press runs
// while the vehicle lies within the two AI radii and a metre (0x1006ed13).
// From each side the bot walks up to the hull and gets in from the ground;
// it never jumps and never climbs on (a press only within 2.5 m of the
// vehicle's middle, which a tank's hull keeps a soldier from, left it
// pushing until the path follower's stuck check jumped it onto the hull).
TEST(Vehicles_BotWalksUpToATankAndGetsIn)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* tank : {"eu_tank", "as_tank"}) {
			for (int side = 0; side < 4; ++side) {
				Garage g(gameDir(), mod);
				if (!g.loaded) return;
				game::BotSettings settings;
				loadShippedAi(g, settings);
				const uint32_t vehicle = g.start(tank, {0, 2, 0});
				CHECK(vehicle != 0);
				if (!vehicle) continue;
				const physics::Vec3 at = g.position(vehicle);
				const int bot = g.world->addPlayer("GetInBot", 2, true);
				CHECK(g.world->spawnSoldier(bot));
				game::GameWorld::PlayerInfo info;
				g.world->playerInfo(bot, info);
				const float angle = side * 1.57079633f + 0.3f; // front, right, back, left, off the axes
				g.world->setSoldierPosition(info.soldier, {at.x + 12 * std::sin(angle), 0, at.z + 12 * std::cos(angle)});
				game::BotManager bots(*g.world, settings);
				bool seated = false, jumped = false, climbed = false;
				int tick = 0;
				for (; tick < 30 * 20 && !seated; ++tick) {
					bots.update(1 / 30.0f);
					jumped = jumped || bots.lastInputFor(bot).jump;
					g.world->tick(1 / 30.0f);
					g.world->playerInfo(bot, info);
					seated = info.vehicle == vehicle;
					climbed = climbed || (!seated && info.position.y > 0.5f);
				}
				std::fprintf(stderr, "  %s %s side %d: %s after %.1f s (%s)\n", mod, tank, side,
					seated ? "in" : "not in", tick / 30.0f, bots.behaviour(bot).c_str());
				CHECK(seated);
				CHECK(!jumped);
				CHECK(!climbed);
			}
		}
	}
}

// A soldier bot closing on a target beyond its preferred range walks
// (steer) while the fire plan aims. The walk's look runs the controller
// with its own last errors (the steering statement 0x100cd3b0, built by the
// fire plan at 0x10079489, calls 0x100cd4f3 with no lead); sharing the
// aim's errors, aim point and pitch history kept the aim's pitch from
// settling, and the bot never fired while it walked.
TEST(Vehicles_BotFiresWhileClosingIn)
{
	if (!gameDir()) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		Garage g(gameDir(), mod);
		if (!g.loaded) return;
		game::BotSettings settings;
		loadShippedAi(g, settings);
		g.start("eu_tank", {0, 2, -400});
		const int bot = g.world->addPlayer("CloserBot", 2, true);
		g.world->setKit(bot, "EU_Assault", "us_light_soldier"); // the rifle kit in both mods
		CHECK(g.world->spawnSoldier(bot));
		const int enemy = g.world->addPlayer("Kept", 1, false);
		CHECK(g.world->spawnSoldier(enemy));
		game::GameWorld::PlayerInfo self, other;
		g.world->playerInfo(enemy, other);
		g.world->setHitPoints(other.soldier, 1e6f); // stays up to be kept ahead
		game::BotManager bots(*g.world, settings);
		int walkingTicks = 0, walkingRounds = 0;
		for (int tick = 0; tick < 30 * 20; ++tick) {
			g.world->playerInfo(bot, self);
			// 15 m ahead of the bot (+z): past a short preferred range the
			// plan walks after it, or the order's walk takes it on.
			const float keep = 15.0f;
			g.world->playerInfo(enemy, other);
			g.world->setSoldierPosition(other.soldier, {self.position.x, 0, self.position.z + keep});
			const int before = g.world->playerRounds(bot, false, false).empty() ? 0 : g.world->playerRounds(bot, false, false)[0].count;
			bots.update(1 / 30.0f);
			g.world->tick(1 / 30.0f);
			g.world->playerInfo(bot, self);
			const auto rounds = g.world->playerRounds(bot, false, false);
			const int after = rounds.empty() ? 0 : rounds[0].count;
			if (bots.behaviour(bot) == "Fire" && self.velocity.x * self.velocity.x + self.velocity.z * self.velocity.z > 1) {
				++walkingTicks;
				walkingRounds += after - before;
			}
		}
		std::fprintf(stderr, "  %s: %d ticks walking in Fire, %d rounds then\n", mod, walkingTicks, walkingRounds);
		CHECK(walkingTicks > 30);
		CHECK(walkingRounds > 0);
	}
}

// A round's motor (0x005b3e60 from 0x005b4370; started at the launch or
// startDelay after it, 0x005b3990 -> 0x005b2c10): the installed walkers'
// rockets leave at the firearm's velocity 35 and gain acceleration 100 along
// their heading up to maxSpeed 125. Fired from the driver's seat with
// PIAltFire.
TEST(Vehicles_WalkerRocketsAccelerateToMaxSpeed)
{
	if (!gameDir()) return;
	constexpr float dt = 1 / 30.0f;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* walker : {"us_heavy_mech", "as_heavy_mech"}) {
			Garage g(gameDir(), mod);
			CHECK(g.loaded && g.templates.find(walker));
			if (!g.loaded || !g.templates.find(walker)) continue;
			const uint32_t mech = g.start(walker, {0, 4, 8});
			g.animate();
			CHECK(g.enter(mech));
			game::SoldierInput fire;
			fire.aimPitch = 10;
			g.drive(fire, 1);
			fire.altFire = true;
			const std::string rocket = std::string(walker) + "_dumrockets_projectile";
			float first = 0, half = 0, top = 0;
			int age = -1;
			for (int i = 0; i < 150 && age < 45; ++i) {
				g.world->setPlayerInput(fire);
				g.world->tick(dt);
				for (const game::Projectile& p : g.world->projectiles()) {
					if (p.shooter != mech || !p.alive || !p.settings || p.settings->name != rocket) continue;
					const float speed = std::sqrt(physics::lengthSquared(p.velocity));
					if (++age == 0) first = speed;
					if (age == 15) half = speed;
					top = std::max(top, speed);
					break; // the oldest one
				}
			}
			std::fprintf(stderr, "  %s %s rocket: %.1f m/s at the muzzle, %.1f after 0.5 s, %.1f at most\n", mod, walker, first, half, top);
			CHECK(age >= 45);
			CHECK(first > 34 && first < 40);
			CHECK(half > 80 && half < 90); // 35 + 100 x 0.5
			CHECK(top >= 125 && top < 128.4f); // the last push starts just under maxSpeed
		}
	}
}

// The heat seekers' motor (0x005b3e60): with follow.targetActivation it runs
// only while the seek holds a target (0x005b2c10 / 0x005b2ca0), and with
// follow.useTargetSpeed its cap is the target's top speed x speedModifier
// (0x005ec770), not maxSpeed; at the cap it brakes by deAcceleration.
// static_aa_missile_projectile: acceleration 60, deAcceleration 0.5,
// speedModifier 1.1, maxSpeed 300, drag 10.
TEST(Vehicles_HeatSeekersThrustToTheirTargetsSpeed)
{
	if (!gameDir()) return;
	constexpr float dt = 1 / 30.0f;
	const physics::Vec3 sight {0, std::sin(20 * 3.14159265f / 180), std::cos(20 * 3.14159265f / 180)};
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		// From the gun's own seat with no lock (nothing in the sky): it has no
		// target, so no motor.
		{
			Garage g(gameDir(), mod);
			world::ObjectTemplate* round = g.templates.find("static_aa_missile_projectile");
			CHECK(g.loaded && g.templates.find("static_aa") && round);
			if (!g.loaded || !g.templates.find("static_aa") || !round) continue;
			round->settings()["gravityModifier"] = "0"; // fixture (shipped -1): the speed is the motor's and the drag's
			const uint32_t aa = g.start("static_aa", {0, 2, 8});
			g.animate();
			CHECK(g.enter(aa));
			game::SoldierInput fire;
			fire.altFire = true;
			float launch = 0, top = 0;
			int age = -1;
			for (int i = 0; i < 200 && age < 45; ++i) {
				g.world->setPlayerInput(fire);
				g.world->tick(dt);
				for (const game::Projectile& p : g.world->projectiles()) {
					if (p.shooter != aa || !p.alive || !p.settings || p.settings->name != "static_aa_missile_projectile") continue;
					fire.altFire = false;
					const float speed = std::sqrt(physics::lengthSquared(p.velocity));
					if (++age == 0) launch = speed;
					top = std::max(top, speed);
					CHECK(p.lockedOn == 0);
					break; // the oldest one
				}
			}
			std::fprintf(stderr, "  %s static AA missile from its seat, no lock: launch %.1f, top %.1f m/s\n", mod, launch, top);
			CHECK(age >= 45);
			CHECK(launch > 55 && top < launch + 0.5f);
		}
		// The same round at a gunship 150 m up its way, held still or flown
		// away at 80 m/s. The first run, with no gunship, is the coasting
		// round the others are compared with.
		float coasted = 0;
		for (float targetSpeed : {-1.0f, 0.0f, 80.0f}) {
			Garage g(gameDir(), mod);
			world::ObjectTemplate* round = g.templates.find("static_aa_missile_projectile");
			CHECK(g.loaded && g.templates.find("as_ag") && round);
			if (!g.loaded || !g.templates.find("as_ag") || !round) continue;
			// Fixtures, not the shipped values: nobody flies the target, so its
			// heat stays on; no gravity (shipped -1), so the speed is the
			// motor's and the drag's.
			if (world::ObjectTemplate* heat = g.templates.find("HeatObject")) heat->settings()["disableWhenEmptyVehicle"] = "0";
			round->settings()["gravityModifier"] = "0";
			const physics::Vec3 from {0, 20, 8};
			const uint32_t jet = g.start("as_ag", from + sight * 150, 0);
			g.objects.find(jet)->setTeam(1);
			if (targetSpeed < 0) g.objects.find(jet)->setTeam(0), g.world->setBodyVelocity(jet, {0, 0, 0}), g.objects.find(jet)->setPosition({0, 20, -5000});
			float launch = 0, top = 0, last = 0, at1 = 0;
			int locked = 0;
			CHECK(g.world->launchProjectile("static_aa_missile_projectile", from, sight * 60));
			for (int age = 0; age <= 60; ++age) {
				if (targetSpeed >= 0) g.world->setBodyVelocity(jet, sight * targetSpeed);
				g.world->tick(dt);
				CHECK(g.world->projectiles().size() == 1 && g.world->projectiles().front().alive);
				if (g.world->projectiles().empty()) break;
				const game::Projectile& p = g.world->projectiles().front();
				last = std::sqrt(physics::lengthSquared(p.velocity));
				if (age == 0) launch = last;
				if (age == 30) at1 = last;
				top = std::max(top, last);
				locked += p.lockedOn == jet;
			}
			std::fprintf(stderr, "  %s static AA missile, target %.0f m/s: launch %.1f, 1 s %.1f, 2 s %.1f, top %.1f m/s, locked %d ticks\n",
				mod, targetSpeed, launch, at1, last, top, locked);
			if (targetSpeed < 0) {
				CHECK(locked == 0);
				CHECK(top < launch + 0.5f); // no target, no motor
				coasted = last;
			} else if (targetSpeed == 0) {
				CHECK(locked > 50);
				CHECK(top < launch + 0.5f); // cap 0: it only brakes
				CHECK(last < coasted - 0.5f && last > coasted - 1.5f); // 0.5 m/s^2 for 2 s
			} else {
				CHECK(locked > 50);
				CHECK(at1 > 85.5f && at1 < 90); // about 1.1 x 80, reached in half a second
				CHECK(top < 90.5f);
			}
		}
	}
}

// The launcher's lock runs for a firearm that is its seat's weapon in any
// of the three fire slots (0x005e1630 in the target component's update
// 0x005e1b30), not just the first: the static AA's missiles are its
// alt-fire. Locked from the seat on a gunship 150 m up the sight line, the
// missile leaves with that target and its motor takes it to the target's
// speed x 1.1.
TEST(Vehicles_AltFireLauncherLocksAndItsMissileChases)
{
	if (!gameDir()) return;
	constexpr float dt = 1 / 30.0f;
	const physics::Vec3 sight {0, std::sin(20 * 3.14159265f / 180), std::cos(20 * 3.14159265f / 180)};
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		Garage g(gameDir(), mod);
		world::ObjectTemplate* round = g.templates.find("static_aa_missile_projectile");
		CHECK(g.loaded && g.templates.find("static_aa") && g.templates.find("as_ag") && round);
		if (!g.loaded || !g.templates.find("static_aa") || !g.templates.find("as_ag") || !round) continue;
		// Fixtures, not the shipped values: nobody flies the target, so its
		// heat stays on; no gravity (shipped -1), so the speed is the motor's
		// and the drag's.
		if (world::ObjectTemplate* heat = g.templates.find("HeatObject")) heat->settings()["disableWhenEmptyVehicle"] = "0";
		round->settings()["gravityModifier"] = "0";
		world::Object* target = g.objects.create(g.templates.find("as_ag"), {0, 4 + 150 * sight.y, 8 + 150 * sight.z});
		target->setTeam(1);
		const uint32_t jet = target->id();
		const uint32_t aa = g.start("static_aa", {0, 2, 8}, 0);
		float away = 0;
		const auto tick = [&](const game::SoldierInput& input) {
			g.world->setBodyVelocity(jet, sight * away); // held, then flown up the sight line
			g.world->setPlayerInput(input);
			g.world->tick(dt);
		};
		for (int i = 0; i < 60; ++i) tick({});
		g.animate();
		CHECK(g.enter(aa));
		game::SoldierInput aim;
		aim.aimPitch = 20;
		for (int i = 0; i < 60; ++i) tick(aim); // lockDelay 0.66 s
		const auto lock = g.world->weaponLock(g.world->localPlayer());
		CHECK(lock.state == 2 && lock.target == jet && lock.maxDistance == 375);
		aim.altFire = true;
		float at1 = 0, top = 0;
		int age = -1, locked = 0;
		for (int i = 0; i < 200 && age < 45; ++i) {
			tick(aim);
			for (const game::Projectile& p : g.world->projectiles()) {
				if (p.shooter != aa || !p.alive || !p.settings || p.settings->name != "static_aa_missile_projectile") continue;
				aim.altFire = false;
				away = 80;
				const float speed = std::sqrt(physics::lengthSquared(p.velocity));
				if (++age == 30) at1 = speed;
				top = std::max(top, speed);
				locked += p.lockedOn == jet;
				break; // the oldest one
			}
		}
		std::fprintf(stderr, "  %s static AA from its seat: lock state %d, missile locked %d of %d ticks, 1 s %.1f, top %.1f m/s\n",
			mod, lock.state, locked, age + 1, at1, top);
		CHECK(age >= 45 && locked > 40);
		CHECK(at1 > 85.5f && at1 < 90 && top < 90.5f);
	}
}

TEST(Vehicles_RemasterMadlockProjectile)
{
	if (!gameDir()) return;
	for (bool wall : {false, true}) {
		Garage g(gameDir(), "mods/Project_Remaster_v17_Final", true);
		if (wall) {
			physics::CollisionLod lod;
			lod.collisionType = 0;
			lod.boundsMin = {-5, 0, -0.01f}; lod.boundsMax = {5, 5, 0.01f};
			lod.vertices = {{-5,0,0},{5,0,0},{5,5,0},{-5,5,0}};
			lod.faces = {{{0,1,2},0},{{0,2,3},0},{{0,2,1},0},{{0,3,2},0}};
			physics::CollisionMeshFile mesh;
			mesh.version = 10;
			mesh.geometries.resize(1); mesh.geometries[0].subGeometries.resize(1);
			mesh.geometries[0].subGeometries[0].lods.push_back(lod);
			g.collisionTemplates.add("mgl_wall_mesh", mesh);
			auto* object = g.templates.create("SimpleObject", "mgl_wall");
			object->settings()["collisionMesh"] = "mgl_wall_mesh";
			g.objects.create(object, {0, 0, 8});
		}
		g.start("eu_tank", {100, 0, 100}, 0);
		const int me = g.world->joinPlayer(2);
		g.world->setKit(me, "EU_Anti-Vehicle", "us_light_soldier");
		g.world->setUnlocks(me, {"UnlockEU_Anti-VehiclePlayer1"});
		CHECK(g.world->spawnSoldier(me));
		game::SoldierInput input; input.selectItem = 3;
		g.world->setPlayerInput(input); g.world->tick(1 / 30.0f);
		g.world->setPlayerInput({}); g.run(3);
		CHECK(g.objects.find(g.world->playerWeaponObject())->objectTemplate());
		std::fprintf(stderr, "MGL weapon=%s\n", g.objects.find(g.world->playerWeaponObject())->objectTemplate()->name().c_str());
		CHECK(g.objects.find(g.world->playerWeaponObject())->objectTemplate()->name() == "UNL_Auto_GL");
		game::GameWorld::PlayerInfo info;
		g.world->playerInfo(me, info); std::fprintf(stderr, "before ammo %d alive %d\n", info.roundsInMag, info.alive);
		for (const auto& kv : g.objects.find(g.world->playerWeaponObject())->objectTemplate()->settings())
			if (kv.first == "projectileTemplate") std::fprintf(stderr,"projectile='%s'\n",kv.second.c_str());
		input = {}; input.fire = true;
		g.world->setPlayerInput(input); g.world->tick(1 / 30.0f);
		g.world->playerInfo(me, info); std::fprintf(stderr, "after ammo %d\n", info.roundsInMag);
		CHECK(info.roundsInMag == 5);
		g.world->setPlayerInput({});
		bool contacted = false, wallContact = false;
		const int enemy = g.world->addPlayer("BlastTarget", 1, false);
		CHECK(g.world->spawnSoldier(enemy));
		game::GameWorld::PlayerInfo target;
		CHECK(g.world->playerInfo(enemy, target));
		g.world->setSoldierPosition(target.soldier, {1000, 0, 1000});
		for (int i = 0; i < 155; ++i) {
			for (const auto& p : g.world->projectiles()) if (p.alive && p.settings && p.settings->name == "magnetic_ammo") {
				contacted |= p.tracerHit;
				wallContact |= p.tracerHit && p.position.y > 0.5f && p.position.z > 7.9f && p.position.z <= 8;
				if (i == 90) g.world->setSoldierPosition(target.soldier, {p.position.x + 1, 0, p.position.z});
			}
			if (i == 0) CHECK(game::projectileRenderStates(g.templates, g.world->projectiles()).size() == 1);
			if (i == 120) CHECK(g.world->projectilesInFlight() == 1);
			if (i % 15 == 0) {
				std::fprintf(stderr, "MGL tick %d live=%zu\n", i, g.world->projectilesInFlight());
				for (const auto& p : g.world->projectiles()) if (p.settings)
					std::fprintf(stderr, "  %s alive=%d t=%.2f pos=%.2f/%.2f/%.2f v=%.2f/%.2f/%.2f bounce=%d\n", p.settings->name.c_str(), p.alive, p.timeLeft, p.position.x,p.position.y,p.position.z,p.velocity.x,p.velocity.y,p.velocity.z,p.settings->bounces);
			}
			if (i == 0) CHECK(g.world->projectilesInFlight() == 1);
			g.world->tick(1 / 30.0f);
		}
		CHECK(contacted);
		if (wall) CHECK(wallContact);
		CHECK(g.world->projectilesInFlight() == 0);
		CHECK(g.world->playerInfo(enemy, target));
		std::fprintf(stderr, "MGL blast victim alive=%d health=%.1f\n", target.alive, g.world->hitPoints(target.soldier));
		CHECK(!target.alive || g.world->hitPoints(target.soldier) < 100);
	}
}

// End to end on the shipped Remaster data: the tank's damaged-cabin alarm
// (S_<seat>_AmbientDamaged under damagedAmbientSoundLimit, 0x005c8a90) and
// the motion mine's lock warning (WarningHud TTMagnet -> HUD_MotionMine_*).
TEST(Vehicles_TankDamageAlarmAndMotionMineWarning)
{
	if (!gameDir()) return;
	std::unique_ptr<sound::SoundManager> manager;
	Garage g(gameDir(), "mods/Project_Remaster_v17_Final", true, [&manager](Garage& garage) { // the client archives: the sounds' files
		manager = std::make_unique<sound::SoundManager>(nullptr, garage.templates);
		manager->registerTemplateType();
		sound::registerSoundConsoleMethods(garage.console, *manager); // as main does (soundFilename ...)
		menu::registerWarningHudMethods(garage.console, garage.templates);
	});
	if (!g.loaded) return;
	const uint32_t tank = g.start("eu_tank", {0, 1, 0});
	CHECK(g.enter(tank));
	sound::GameSounds sounds {*manager};
	GameAudio audio(*g.world, g.objects, g.templates, *manager, sounds, nullptr);
	GameAudio::Camera camera;
	camera.firstPerson = true;
	audio.tick(camera);
	const std::string healthy = audio.listenerState().ambient;
	const world::ObjectTemplate* t = g.objects.find(tank)->objectTemplate();
	const float maxHp = static_cast<float>(std::atof(t->settings().at("armor.maxHitPoints").c_str()));
	g.world->setHitPoints(tank, maxHp * 0.25f); // over eu_tank's damagedAmbientSoundLimit 0.20
	audio.tick(camera);
	const std::string quarter = audio.listenerState().ambient;
	g.world->setHitPoints(tank, maxHp * 0.1f);
	audio.tick(camera);
	const std::string damaged = audio.listenerState().ambient;
	const auto* alarm = dynamic_cast<const sound::SoundObjectTemplate*>(g.templates.find(damaged));
	const std::string file = alarm ? alarm->soundFilenames() : std::string();
	std::fprintf(stderr, "  cabin: healthy %s, at 25 %% %s, at 10 %% %s (%s)\n", healthy.c_str(), quarter.c_str(), damaged.c_str(), file.c_str());
	CHECK(healthy == "S_eu_tank_Ambient" && quarter == healthy);
	CHECK(damaged == "S_eu_tank_AmbientDamaged");
	CHECK(file.find("damagealarm") != std::string::npos);
	g.world->setHitPoints(tank, maxHp);
	// A motion mine 8 m off: armed (3.5 s) and locked (TTMagnet, 20 m) on the tank.
	const auto pos = g.objects.find(tank)->position();
	const physics::Vec3 at {pos.x, pos.y, pos.z};
	CHECK(g.world->launchProjectile("Unl_Mine_Motion_Projectile", at + physics::Vec3 {8, 0.5f, 0}, {0, 0, 0}));
	bool locked = false;
	for (int i = 0; i < 30 * 8 && !locked; ++i) {
		g.world->tick(1 / 30.0f);
		for (const auto& p : g.world->projectiles()) {
			locked = locked || (p.alive && p.lockedOn == tank);
		}
	}
	std::fprintf(stderr, "  motion mine locked on the tank: %d\n", locked);
	CHECK(locked);
	hud::HudManager hudManager(nullptr);
	menu::WarningHud warnings;
	std::vector<std::string> played;
	warnings.setSoundHook([&played](const std::string& sound, bool start) { played.push_back((start ? "+" : "-") + sound); });
	warnings.update(hudManager, t, {{1, 3, false}}); // as main feeds it: the mine (TTMagnet, 3) locked on
	std::fprintf(stderr, "  warning: %s, icon %.0f\n", played.empty() ? "-" : played.front().c_str(), hudManager.number("WarningIconShow"));
	CHECK(!played.empty() && played.front() == "+HUD_MotionMine_Warning" && hudManager.number("WarningIconShow") == 1);
	// The tone itself: GameSounds.con's Sound.addSound (Missile_Warning.wav at pitch 0.45), as main runs it.
	std::string out;
	g.console.run("Sound/GameSounds.con", {}, out);
	const auto* tone = dynamic_cast<const sound::SoundObjectTemplate*>(g.templates.find("S_Game_HUD_MotionMine_Warning"));
	std::fprintf(stderr, "  tone: %s\n", tone ? tone->soundFilenames().c_str() : "-");
	CHECK(tone && tone->soundFilenames().find("Missile_Warning") != std::string::npos);
}

// A bipod's ducking plays its seat system's duck (3p_eu_bipod_animSys.inc:
// PoseTrigger root over stand and duck): the seat's setDucking (0x005c7cf0)
// sets the seat system's pose (+0x284 slot 0x28) to 1 while ducking, 0
// once risen, so the sitter's head goes down with lookAroundDuck and comes
// back; a joined client's seated soldier as its ghost's ducking says.
TEST(Vehicles_BipodDuckPlaysTheDuckPose)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t bipod = g.start("eu_bipod", {0, 1.2f, 8}, 1);
	g.animate();
	CHECK(g.enter(bipod));
	game::GameWorld::PlayerInfo me;
	g.world->playerInfo(g.world->localPlayer(), me);
	const anim::Skeleton* skeleton = g.systems->skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	const int head = skeleton ? skeleton->find("head") : -1;
	CHECK(head >= 0);
	if (head < 0) {
		return;
	}
	const auto headHeight = [&](uint32_t soldier) {
		std::vector<anim::Matrix> pose;
		if (!g.world->skeletonPose(soldier, *skeleton, pose) || pose.size() <= static_cast<size_t>(head)) {
			return 0.0f;
		}
		return pose[static_cast<size_t>(head)][13];
	};
	game::SoldierInput still;
	g.drive(still, 1);
	const float sitting = headHeight(me.soldier);
	game::SoldierInput duck;
	duck.pose = game::SoldierPose::Crouch;
	g.drive(duck, 1);
	CHECK(g.world->seatDucking(bipod, 0));
	const float ducked = headHeight(me.soldier);
	g.drive(still, 1);
	const float risen = headHeight(me.soldier);
	std::fprintf(stderr, "  eu_bipod: head %.3f sitting, %.3f ducking, %.3f risen\n", sitting, ducked, risen);
	CHECK(ducked < sitting - 0.1f);
	CHECK(std::fabs(risen - sitting) < 0.02f);
	// A joined client's seated soldier, by its ghost's ducking.
	const uint32_t other = g.objects.create(g.templates.find("us_light_soldier"), {3, 1.2f, 8})->id();
	const auto remoteHead = [&](bool ducking) {
		for (int i = 0; i < 30; ++i) {
			g.world->animateRemoteSeated(other, bipod, 0, 0, 1 / 30.0f, ducking);
		}
		return headHeight(other);
	};
	const float remoteSitting = remoteHead(false), remoteDucked = remoteHead(true);
	CHECK(remoteDucked < remoteSitting - 0.1f);
	CHECK(std::fabs(remoteHead(false) - remoteSitting) < 0.02f);
}

// The motion mine (Unl_Mine_Motion_Projectile: physicsType Mesh, drag 10,
// mass 5) is a Basic node whose drag is its collision mesh's vehicle LOD
// box's (0x006c9060 through the mesh's slot 0x54(1)).
TEST(Weapons_MotionMineHasItsBox)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	g.start("eu_bipod", {0, 1.2f, 8}, 1);
	const auto settingsOf = [&](const std::string& name) -> const game::ProjectileSettings* {
		if (!g.world->launchProjectile(name, {0, 30, 0}, {0, 0, 1})) {
			return nullptr;
		}
		return g.world->projectiles().empty() ? nullptr : g.world->projectiles().back().settings;
	};
	const game::ProjectileSettings* mine = settingsOf("Unl_Mine_Motion_Projectile");
	CHECK(mine && mine->basicNode && mine->drag == 10 && mine->mass == 5);
	if (mine) {
		std::fprintf(stderr, "  motion mine box %.3f x %.3f x %.3f\n", mine->boxSize.x, mine->boxSize.y, mine->boxSize.z);
		CHECK(mine->boxSize.x > 0.05f && mine->boxSize.y > 0.05f && mine->boxSize.z > 0.05f);
		CHECK(mine->boxSize.x < 2 && mine->boxSize.y < 2 && mine->boxSize.z < 2);
	}
	// A grenade stays a RotationalPoint.
	const game::ProjectileSettings* frag = settingsOf("Unl_Grenade_Frag_Projectile");
	CHECK(frag && frag->rotationalPoint && !frag->basicNode);
}

// A gunship parked on a helipad laid on the terrain (Suez_Canal's EU pad:
// helipad_m, its deck 0.29 m over the flattened ground). Its landing gear
// probes the pad from where the Spring's step (0x006caef0) left the parts,
// at rest; probing from the pushed-out place missed the deck every third
// tick, so the springs lost and found it in turn: the body never slept,
// bounced at 2, 4, -6 m/s, lost hit points and, with throttle, beat itself
// to pieces. It settles and sleeps undamaged, and lifts off smoothly.
TEST(Vehicles_GunshipRestsOnAHelipad)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("helipad_m")) {
		return;
	}
	g.objects.create(g.templates.find("helipad_m"), {0, 0, 8}, {0, 0, 0});
	const uint32_t ag = g.start("us_ag", {0, 2.4f, 8}, 0.1f);
	float fastest = 0;
	for (int tick = 0; tick < 4 * 30; ++tick) {
		const physics::Vec3 before = g.position(ag);
		g.run(1 / 30.0f);
		if (tick >= 60) {
			fastest = std::max(fastest, std::fabs(g.position(ag).y - before.y) * 30);
		}
	}
	const physics::Vec3 rest = g.position(ag);
	std::fprintf(stderr, "  us_ag on the pad: %.2f m up, %.2f m/s at most after 2 s, %s, %.0f hit points\n", rest.y,
		fastest, g.world->bodyAsleep(ag) ? "asleep" : "awake", g.world->hitPoints(ag));
	CHECK(g.world->bodyAsleep(ag));
	CHECK(fastest < 1);
	CHECK(g.world->hitPoints(ag) == 1000);
	CHECK(rest.y > 1.5f && rest.y < 4);
	CHECK(g.enter(ag));
	game::SoldierInput input;
	input.throttle = 1;
	float lowest = rest.y, sharpest = 0;
	physics::Vec3 last = g.position(ag);
	for (int tick = 0; tick < 3 * 30; ++tick) {
		g.drive(input, 1 / 30.0f);
		const physics::Vec3 p = g.position(ag);
		sharpest = std::max(sharpest, std::fabs(p.y - last.y) * 30);
		lowest = std::min(lowest, p.y);
		last = p;
	}
	std::fprintf(stderr, "  with throttle: %.1f m up after 3 s, %.1f m/s at most vertically, %.0f hit points\n", last.y,
		sharpest, g.world->hitPoints(ag));
	CHECK(last.y > rest.y + 2);
	CHECK(sharpest < 8);
	CHECK(g.world->hitPoints(ag) == 1000);
}

// A walker on a bridge high over the terrain: its feet find the
// deck (0x00650e90's sphere at each foot against the objects' meshes, not
// only the heightmap's segment), so it isn't pushed sideways as over a
// drop (30 a tick to the side of the foot in the air) and comes to rest.
TEST(Vehicles_WalkerStandsOnADeck)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	// Suez_Canal's big bridge, its deck 20 m over the ground.
	if (!g.templates.find("bc_big_bridge_a")) {
		return;
	}
	g.objects.create(g.templates.find("bc_big_bridge_a"), {0, 20, 0}, {180, 0, 0});
	const uint32_t mech = g.start("us_heavy_mech", {0, 23.5f, 0}, 4);
	const physics::Vec3 a = g.position(mech);
	g.run(4);
	const physics::Vec3 b = g.position(mech);
	const float drift = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
	std::fprintf(stderr, "  us_heavy_mech on the deck: %.2f m up, %.2f m drift in 4 s, %s\n", b.y, drift,
		g.world->bodyAsleep(mech) ? "asleep" : "awake");
	CHECK(b.y > 20 && b.y < 26);
	CHECK(drift < 0.5f);
	CHECK(g.world->bodyAsleep(mech));
}

// Suez_Canal's industrial bridge (-512/47/-36): a walker stands on its deck
// on the legs' sphere alone, its hull's vehicle LOD box (1.9 m under its
// origin) above the deck's box. The legs object is a mover of its own with
// the dispatch's radius 5 and no box tests (0x006dc7b0 without a mesh
// instance), so the sphere meets the deck every tick and the walker sleeps;
// tied to the hull's box tests it met it every other tick and bounced
// 0.16 m for ever.
TEST(Vehicles_WalkerSleepsOnTheIndustrialBridge)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("bridge_industrial_01_c")) {
		return;
	}
	g.objects.create(g.templates.find("bridge_industrial_01_c"), {0, 20, 0}, {0, 0, 0});
	const uint32_t mech = g.start("us_heavy_mech", {0, 26.5f, 0}, 2);
	const physics::Vec3 a = g.position(mech);
	float lowest = a.y, highest = a.y;
	for (int i = 0; i < 60; ++i) {
		g.run(1 / 30.0f);
		lowest = std::min(lowest, g.position(mech).y);
		highest = std::max(highest, g.position(mech).y);
	}
	const physics::Vec3 b = g.position(mech);
	std::fprintf(stderr, "  us_heavy_mech on the industrial bridge: %.3f m up, %.3f m bounce, %s\n", b.y,
		highest - lowest, g.world->bodyAsleep(mech) ? "asleep" : "awake");
	CHECK(b.y > 25 && b.y < 28); // on the deck (23.15), the legs 2.8 + 0.57 under the origin
	CHECK(highest - lowest < 0.01f);
	CHECK(g.world->bodyAsleep(mech));
}

// The parts as movers of their own (0x005f6ba0's Basic node and the
// mobile response, run down the root's chain by the dispatch 0x006dd1b0):
// a part's ground pass (0x006b9260) puts its vehicle LOD where the part is,
// and its solve (0x006ba7d0) moves the root. eu_ifv's tracks (geometries 33
// and 34, six vertices each, 1.24 m under the hull's origin) hang between
// its road wheels (z 0.77 and -1.35): a 1 m bump there meets the tracks
// and lifts the IFV by about as far as it reaches into them. Before, the
// parts moved with the hull and the bump passed through them.
TEST(Vehicles_PartsMeetTheGroundAsMovers)
{
	if (!gameDir()) {
		return;
	}
	Garage flat(gameDir());
	if (!flat.loaded || !flat.templates.find("eu_ifv")) {
		return;
	}
	const uint32_t a = flat.start("eu_ifv", {0, 3, 0}, 4);
	Garage bump(gameDir());
	bump.groundShape = [](float, float z) { return z > -0.6f && z < 0.0f ? 1.0f : 0.0f; };
	const uint32_t b = bump.start("eu_ifv", {0, 3, 0}, 4);
	const float rest = flat.position(a).y, lifted = bump.position(b).y;
	std::fprintf(stderr, "  eu_ifv at rest %.3f, over the bump %.3f\n", rest, lifted);
	CHECK(lifted > rest + 0.2f);
	CHECK(lifted < rest + 0.5f);
}

// A part as a mover meets the other bodies (0x006dc7b0 with the part as
// param_3, the roots' velocities, its contacts solved onto its root):
// us_ag's TAD base (geometry 13, 30 vertices under the nose, 0.6 m over
// the ground on its gear) settles onto a tank parked across its nose and
// pushes it away (the tank ends past 5.9 m; without the part as a mover
// the gunship's hull alone met it, 5.73 m).
TEST(Vehicles_PartMoverPushesAnotherBody)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	if (!g.loaded || !g.templates.find("us_ag")) {
		return;
	}
	world::Object* parked = g.objects.create(g.templates.find("eu_tank"), {0, 1.2f, 5}, {90, 0, 0});
	parked->setTeam(2);
	const uint32_t tank = parked->id();
	g.start("us_ag", {0, 3.5f, 0}, 5);
	const physics::Vec3 at = g.position(tank);
	std::fprintf(stderr, "  the tank under us_ag's nose ends at z %.2f\n", at.z);
	CHECK(at.z > 5.9f);
}

// A deployed sentry gun (Unl_Deployer_Sentrygun's eu_sentrygun) is run by
// its SentryGun auto controller: an enemy on foot in its 30-degree view
// within 63 m is taken, fired at after the reactionTime (1 s), and killed,
// the kill its deployer's; its gun is the PIFire GenericFireArm, not the
// PIAltFire siren (Car_horn_Alt) beside it. Its gun's DefaultAnimationComp (loopingFire 1)
// holds message 1 from the first round to the trigger's release, so the
// "still" switch plays "fire" (0x005e0f30, 0x005e0f60, 0x0063ce30).
TEST(Vehicles_DeployedSentryGunFires)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	world::ObjectTemplate* kit = g.loaded ? g.templates.find("EU_Assault") : nullptr;
	if (!kit || !g.templates.find("Unl_Deployer_Sentrygun") || !g.templates.find("eu_sentrygun")) {
		return;
	}
	kit->children() = {{"eu_ar_rifle", {}, {}}, {"Unl_Deployer_Sentrygun", {}, {}}};
	world::ObjectTemplate* point = g.templates.create("ControlPoint", "garage_cp_pac");
	point->settings()["controlPointId"] = "1";
	point->settings()["team"] = "1";
	point->settings()["unableToChangeTeam"] = "1";
	g.objects.create(point, {200, 0, 0});
	g.objects.create(g.templates.find("garage_sp"), {0, 0, 25}, {180, 0, 0})->setControlPointId(1);
	Animations a(g);
	g.start("eu_fav", {30, 2, 30});
	a.attach(g);
	// Both sides in before the deployment (a side's first player starts
	// the round over), the enemy kept far off meanwhile.
	const int enemy = g.world->addPlayer("Pac", 1, true);
	CHECK(g.world->spawnPlayer(2) != nullptr);
	const int owner = g.world->localPlayer();
	g.run(1);
	CHECK(g.world->spawnSoldier(enemy));
	game::GameWorld::PlayerInfo info;
	CHECK(g.world->playerInfo(enemy, info) && info.alive);
	g.world->setSoldierPosition(info.soldier, {0, 0, -400});
	g.run(1);
	game::SoldierInput take;
	take.selectItem = 5;
	g.drive(take, 1.5f);
	game::SoldierInput fire;
	fire.fire = true;
	g.drive(fire, 0.1f);
	g.drive({}, 2.5f);
	uint32_t sentry = 0;
	for (const auto& object : g.objects.objects()) {
		if (object->objectTemplate() && object->objectTemplate()->name() == "eu_sentrygun") {
			sentry = object->id();
		}
	}
	CHECK(sentry != 0);
	CHECK(g.world->playerInfo(enemy, info) && info.alive);
	g.world->setSoldierPosition(info.soldier, {0, 0, 25});
	// Its scan (0.9 rad/s) comes round to him, then the reaction time.
	bool firing = false, killed = false;
	for (int i = 0; i < 450 && !killed; ++i) {
		g.run(1 / 30.0f);
		const auto playing = g.world->objectAnimation(sentry);
		firing = firing || std::find(playing.begin(), playing.end(), "fire") != playing.end();
		game::GameWorld::PlayerInfo me;
		killed = g.world->playerInfo(owner, me) && me.kills == 1;
	}
	CHECK(killed);
	CHECK(firing);
	// The target gone, the trigger lets go and the switch goes back.
	g.run(1.5f);
	const auto after = g.world->objectAnimation(sentry);
	CHECK(std::find(after.begin(), after.end(), "stand") != after.end());
}

// checkLineOfFire (AutoControllers.ai: 1 for SentryGun and UAVDrone): the
// fire plan's condition (AIDLL 0x100db150 -> 0x100dad90) holds the
// trigger while a teammate on foot stands within 0.5 + 0.02 t of the line
// from the gun to the target; he steps aside and the gun fires.
TEST(Vehicles_SentryGunHoldsFireOverATeammate)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	world::ObjectTemplate* kit = g.loaded ? g.templates.find("EU_Assault") : nullptr;
	if (!kit || !g.templates.find("Unl_Deployer_Sentrygun") || !g.templates.find("eu_sentrygun")) {
		return;
	}
	kit->children() = {{"eu_ar_rifle", {}, {}}, {"Unl_Deployer_Sentrygun", {}, {}}};
	world::ObjectTemplate* point = g.templates.create("ControlPoint", "garage_cp_pac");
	point->settings()["controlPointId"] = "1";
	point->settings()["team"] = "1";
	point->settings()["unableToChangeTeam"] = "1";
	g.objects.create(point, {200, 0, 0});
	g.objects.create(g.templates.find("garage_sp"), {0, 0, 25}, {180, 0, 0})->setControlPointId(1);
	g.start("eu_fav", {30, 2, 30});
	const int enemy = g.world->addPlayer("Pac", 1, true);
	const int mate = g.world->addPlayer("Mate", 2, true);
	CHECK(g.world->spawnPlayer(2) != nullptr);
	const int owner = g.world->localPlayer();
	g.run(1);
	CHECK(g.world->spawnSoldier(enemy) && g.world->spawnSoldier(mate));
	game::GameWorld::PlayerInfo info, mateInfo;
	CHECK(g.world->playerInfo(enemy, info) && info.alive && g.world->playerInfo(mate, mateInfo));
	g.world->setSoldierPosition(info.soldier, {0, 0, -400});
	g.world->setSoldierPosition(mateInfo.soldier, {0, 0, -420});
	g.run(1);
	game::SoldierInput take;
	take.selectItem = 5;
	g.drive(take, 1.5f);
	game::SoldierInput fire;
	fire.fire = true;
	g.drive(fire, 0.1f);
	g.drive({}, 2.5f);
	const world::Object* sentry = nullptr;
	for (const auto& object : g.objects.objects()) {
		if (object->objectTemplate() && object->objectTemplate()->name() == "eu_sentrygun") {
			sentry = object.get();
		}
	}
	CHECK(sentry != nullptr);
	if (!sentry) {
		return;
	}
	const auto at = sentry->position();
	// The enemy 20 m off along +z, the teammate halfway, on the line.
	CHECK(g.world->playerInfo(enemy, info) && info.alive);
	g.world->setSoldierPosition(info.soldier, {at.x, at.y, at.z + 20});
	g.world->setSoldierPosition(mateInfo.soldier, {at.x, at.y, at.z + 10});
	game::GameWorld::PlayerInfo me;
	for (int i = 0; i < 300; ++i) {
		g.world->setSoldierPosition(mateInfo.soldier, {at.x, at.y, at.z + 10});
		g.run(1 / 30.0f);
	}
	CHECK(g.world->playerInfo(owner, me) && me.kills == 0);
	CHECK(g.world->playerInfo(enemy, info) && info.alive);
	CHECK(g.world->playerInfo(mate, mateInfo) && mateInfo.alive);
	g.world->setSoldierPosition(mateInfo.soldier, {at.x + 8, at.y, at.z + 10});
	bool killed = false;
	for (int i = 0; i < 300 && !killed; ++i) {
		g.run(1 / 30.0f);
		killed = g.world->playerInfo(owner, me) && me.kills == 1;
	}
	CHECK(killed);
}

// flipOutAtHealth (AutoControllers.ai: the sentry gun's 0.25, of its 120
// hit points): under it the auto controller takes no target and holds
// PIFire with the yaw at its full speed (AIDLL 0x10097d60, 0x100f9cf0's
// flipping branch): with no enemy near, the gun spins and fires.
TEST(Vehicles_SentryGunFlipsOut)
{
	if (!gameDir()) {
		return;
	}
	Garage g(gameDir());
	world::ObjectTemplate* kit = g.loaded ? g.templates.find("EU_Assault") : nullptr;
	if (!kit || !g.templates.find("Unl_Deployer_Sentrygun") || !g.templates.find("eu_sentrygun")) {
		return;
	}
	kit->children() = {{"eu_ar_rifle", {}, {}}, {"Unl_Deployer_Sentrygun", {}, {}}};
	world::ObjectTemplate* point = g.templates.create("ControlPoint", "garage_cp_pac");
	point->settings()["controlPointId"] = "1";
	point->settings()["team"] = "1";
	point->settings()["unableToChangeTeam"] = "1";
	g.objects.create(point, {200, 0, 0});
	g.objects.create(g.templates.find("garage_sp"), {0, 0, 25}, {180, 0, 0})->setControlPointId(1);
	Animations a(g);
	g.start("eu_fav", {30, 2, 30});
	a.attach(g);
	const int enemy = g.world->addPlayer("Pac", 1, true);
	CHECK(g.world->spawnPlayer(2) != nullptr);
	g.run(1);
	CHECK(g.world->spawnSoldier(enemy));
	game::GameWorld::PlayerInfo info;
	CHECK(g.world->playerInfo(enemy, info));
	g.world->setSoldierPosition(info.soldier, {0, 0, -400});
	g.run(1);
	game::SoldierInput take;
	take.selectItem = 5;
	g.drive(take, 1.5f);
	game::SoldierInput fire;
	fire.fire = true;
	g.drive(fire, 0.1f);
	g.drive({}, 2.5f);
	uint32_t sentry = 0;
	for (const auto& object : g.objects.objects()) {
		if (object->objectTemplate() && object->objectTemplate()->name() == "eu_sentrygun") {
			sentry = object->id();
		}
	}
	CHECK(sentry != 0);
	const auto firing = [&] {
		const auto playing = g.world->objectAnimation(sentry);
		return std::find(playing.begin(), playing.end(), "fire") != playing.end();
	};
	g.run(1);
	CHECK(!firing()); // healthy: nothing to shoot at
	g.world->setHitPoints(sentry, 50); // over 30: still calm
	g.run(0.5f);
	CHECK(!firing());
	g.world->setHitPoints(sentry, 29);
	bool flipped = false;
	for (int i = 0; i < 30 && !flipped; ++i) {
		g.run(1 / 30.0f);
		flipped = firing();
	}
	CHECK(flipped);
}

// A gunship's pilot weapon (as_ag / us_ag PilotWeapon: ammo.autoReload, 10 x 6)
// spent to nothing is rearmed by a pad's SupplyObject (the helipad's
// dummy_helipad_repair_SupplyObject and the Titans' heliRepairRearm: radius,
// workOnVehicles, addVehicleType 3, refillAmmoSpeed 12; 0x005d0b20 -> the
// ammo's slot 0xa8, 0x005f5110). The ammo's update starts the reload of an
// empty magazine by itself once a magazine can be taken (0x005f3c40, which a
// resupply makes true), and the sound component's reload event (0x005dbfe0:
// S_..._PilotWeapon_Reload1P, vehicle_reload.wav) plays for the pilot.
TEST(Vehicles_GunshipRearmedOnAPadReloadsWithItsSound)
{
	if (!gameDir()) {
		return;
	}
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* aircraft : {"as_ag", "us_ag"}) {
			for (const char* padName : {"dummy_helipad_repair", "eu_titan_heliRepairRearm"}) {
				Garage g(gameDir(), mod);
				if (!g.loaded || !g.templates.find(aircraft) || !g.templates.find(padName)) {
					continue;
				}
				const uint32_t v = g.start(aircraft, {0, 4, 6});
				CHECK(g.enter(v));
				if (!g.world->playerVehicle()) {
					continue;
				}
				std::fprintf(stderr, "  %s %s %s\n", mod, aircraft, padName);
				// Fire the pilot weapon dry (it stays busy until the trigger is let
				// go, busyUntilButtonRelease: press and release): no round for 5 s.
				game::SoldierInput fire;
				const auto* gun = g.world->gun(v, 0);
				CHECK(gun);
				if (!gun) continue;
				const int dryWait = gun->settings().minimumReloadTicks
					+ static_cast<int>(std::ceil(gun->settings().reloadTime * 30)) + 30 * 5;
				int shots = 0, idle = 0;
				for (int i = 0; i < 30 * 240 && idle < dryWait; ++i) {
					fire.fire = i % 2 == 0;
					g.world->setPlayerInput(fire);
					g.world->tick(1 / 30.0f);
					bool shot = false;
					for (const auto& sound : g.world->takeWeaponSounds()) {
						shot = shot || sound.kind == game::GameWorld::WeaponSoundEvent::Fire;
					}
					shots += shot;
					idle = shot ? 0 : idle + 1;
				}
				g.world->setPlayerInput({});
				std::fprintf(stderr, "    shots %d\n", shots);
				CHECK(shots >= 60); // the 6 spare magazines and the loaded one
				// Without a pad nothing reloads an empty gun with no magazine left.
				g.run(3);
				int reloads = 0;
				for (const auto& sound : g.world->takeWeaponSounds()) {
					reloads += sound.kind == game::GameWorld::WeaponSoundEvent::Reload;
				}
				CHECK(reloads == 0);
				// The pad under it (a SupplyObject of a Bundle at the gunship; the
				// Titans' own is a child of the Titan).
				world::ObjectTemplate* pad = g.templates.find(padName);
				if (std::string(padName) == "eu_titan_heliRepairRearm") {
					world::ObjectTemplate* holder = g.templates.create("Bundle", "test_titan_pad_holder");
					holder->children().push_back({padName, {0, 0, 0}, {}});
					pad = holder;
				}
				const world::Vec3 at = g.objects.find(v)->position();
				g.objects.create(pad, {at.x, at.y - 5, at.z});
				for (int i = 0; i < 30 * 8; ++i) {
					g.world->tick(1 / 30.0f);
					for (const auto& sound : g.world->takeWeaponSounds()) {
						reloads += sound.kind == game::GameWorld::WeaponSoundEvent::Reload;
					}
				}
				CHECK(reloads >= 1);
			}
		}
	}
}

// The same end to end through the sound templates of the shipped data: the
// pilot weapon's Reload1P (vehicle_reload.wav) triggers for the seated
// pilot when the pad's refill makes the dry gun take a magazine in.
TEST(Vehicles_GunshipRearmedOnAPadTriggersThePilotsReloadSound)
{
	if (!gameDir()) {
		return;
	}
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* aircraft : {"as_ag", "us_ag"}) {
			std::unique_ptr<sound::SoundManager> manager;
			Garage g(gameDir(), mod, true, [&manager](Garage& garage) {
				manager = std::make_unique<sound::SoundManager>(nullptr, garage.templates);
				manager->registerTemplateType();
				sound::registerSoundConsoleMethods(garage.console, *manager);
			});
			if (!g.loaded || !g.templates.find(aircraft) || !g.templates.find("dummy_helipad_repair")) {
				continue;
			}
			const uint32_t v = g.start(aircraft, {0, 4, 6});
			CHECK(g.enter(v));
			if (!g.world->playerVehicle()) {
				continue;
			}
			sound::GameSounds sounds {*manager};
			std::vector<std::string> triggered;
			sounds.onTrigger = [&triggered](const sound::GameSounds::Triggered& t) { triggered.push_back(t.name); };
			GameAudio audio(*g.world, g.objects, g.templates, *manager, sounds, nullptr);
			GameAudio::Camera camera;
			camera.firstPerson = true;
			const auto count = [&triggered](const char* part) {
				int n = 0;
				for (const std::string& name : triggered) {
					n += name.find(part) != std::string::npos;
				}
				return n;
			};
			game::SoldierInput fire;
			const auto* gun = g.world->gun(v, 0);
			CHECK(gun);
			if (!gun) continue;
			const int dryWait = gun->settings().minimumReloadTicks
				+ static_cast<int>(std::ceil(gun->settings().reloadTime * 30)) + 30 * 5;
			int idle = 0;
			for (int i = 0; i < 30 * 240 && idle < dryWait; ++i) {
				fire.fire = i % 2 == 0;
				g.world->setPlayerInput(fire);
				g.world->tick(1 / 30.0f);
				const int before = count("PilotWeapon_Fire");
				audio.tick(camera);
				idle = count("PilotWeapon_Fire") > before ? 0 : idle + 1;
			}
			g.world->setPlayerInput({});
			const int fired = count("PilotWeapon_Fire");
			std::fprintf(stderr, "  %s %s: %d fire sounds, %d reload sounds when spent\n", mod, aircraft, fired, count("PilotWeapon_Reload"));
			CHECK(fired >= 60);
			const int spentReloads = count("PilotWeapon_Reload1P");
			g.objects.create(g.templates.find("dummy_helipad_repair"), {g.objects.find(v)->position().x,
				g.objects.find(v)->position().y - 5, g.objects.find(v)->position().z});
			for (int i = 0; i < 30 * 8; ++i) {
				g.world->tick(1 / 30.0f);
				audio.tick(camera);
			}
			std::fprintf(stderr, "    after the pad: %d reload sounds (Reload1P %d)\n", count("PilotWeapon_Reload"), count("PilotWeapon_Reload1P"));
			CHECK(count("PilotWeapon_Reload1P") > spentReloads);
		}
	}
}

// The gunner's seat: the guided missile (1 x 8, ammo.autoReload) the seat's
// special toggle selects, spent, takes a magazine in by itself when the pad
// rearms it, and its Reload1P triggers.
TEST(Vehicles_GunshipGunnerMissileRearmedOnAPadReloads)
{
	if (!gameDir()) {
		return;
	}
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const char* aircraft : {"as_ag", "us_ag"}) {
			std::unique_ptr<sound::SoundManager> manager;
			Garage g(gameDir(), mod, true, [&manager](Garage& garage) {
				manager = std::make_unique<sound::SoundManager>(nullptr, garage.templates);
				manager->registerTemplateType();
				sound::registerSoundConsoleMethods(garage.console, *manager);
			});
			if (!g.loaded || !g.templates.find(aircraft) || !g.templates.find("dummy_helipad_repair")) {
				continue;
			}
			g.groundHeight = 0;
			const uint32_t v = g.start(aircraft, {0, 4, 6});
			CHECK(g.enter(v));
			if (!g.world->playerVehicle()) {
				continue;
			}
			sound::GameSounds sounds {*manager};
			std::vector<std::string> triggered;
			sounds.onTrigger = [&triggered](const sound::GameSounds::Triggered& t) { triggered.push_back(t.name); };
			GameAudio audio(*g.world, g.objects, g.templates, *manager, sounds, nullptr);
			GameAudio::Camera camera;
			camera.firstPerson = true;
			const auto count = [&triggered](const char* part) {
				int n = 0;
				for (const std::string& name : triggered) {
					n += name.find(part) != std::string::npos;
				}
				return n;
			};
			const auto step = [&](const game::SoldierInput& input) {
				g.world->setPlayerInput(input);
				g.world->tick(1 / 30.0f);
				audio.tick(camera);
			};
			game::SoldierInput input;
			input.switchSeat = 2;
			step(input);
			input = {};
			for (int i = 0; i < 30 * 4; ++i) step(input);
			input.altFire = true; // the toggle: the cannon -> the guided missile
			for (int i = 0; i < 3; ++i) step(input);
			input = {};
			for (int i = 0; i < 30 * 4; ++i) step(input);
			g.groundHeight = -500; // the missile dives for a while: its press is free again
			int idle = 0;
			for (int i = 0; i < 30 * 400 && idle < 30 * 10; ++i) {
				input = {};
				input.fire = i % 4 == 0;
				const int before = count("GuidedMissile");
				step(input);
				idle = count("GuidedMissile") > before ? 0 : idle + 1;
			}
			g.groundHeight = 0;
			std::fprintf(stderr, "  %s %s: missile sounds %d (Fire1P %d), reloads %d\n", mod, aircraft, count("GuidedMissile"),
				count("GuidedMissile_firearm_Fire1P") + count("GuidedMissileWeapon_Fire1P"), count("GuidedMissile") - 0);
			const int spent = count("Reload1P");
			g.objects.create(g.templates.find("dummy_helipad_repair"), {g.objects.find(v)->position().x,
				g.objects.find(v)->position().y - 5, g.objects.find(v)->position().z});
			for (int i = 0; i < 30 * 12; ++i) step({});
			std::fprintf(stderr, "    after the pad: Reload1P %d (was %d)\n", count("Reload1P"), spent);
			CHECK(count("Reload1P") > spent);
		}
	}
}

#ifdef BF2142_RENDER_TESTS
// Explicit opt-in (a real GPU device, no window): every enhanced lit pipeline
// must compile. A shader sampler binding that clashes with a later addition to
// its base shader (terrain.frag's envmap at 13 against the AO map) makes
// Metal refuse the library, and the world silently loses the cascaded shadows
// and the ambient occlusion.
TEST(Render_EnhancedLitPipelinesCompileOptIn)
{
	const char* output = std::getenv("BF2142_RENDER_TEST_DIR");
	if (!output) return;
	CHECK(SDL_Init(SDL_INIT_VIDEO));
	{
		std::string error;
		auto gpu = render::GpuContext::create(nullptr, error);
		if (!gpu) { std::fprintf(stderr, "%s\n", error.c_str()); CHECK(gpu); return; }
		CHECK(gpu->enhancedPipelines().terrain != nullptr);
		CHECK(gpu->enhancedLitAvailable());
		// The prepass's and the motion's culled variants (the world pass's meshCulled).
		CHECK(gpu->enhancedPipelines().depthCulled != nullptr);
		CHECK(gpu->enhancedPipelines().motionCulled != nullptr);
	}
	SDL_Quit();
}
#endif

// The Hachimoto's camera mount rides PIPitch with the same limits as its driver's gun, so the view
// tilts with the barrel: also parked, when the node sleeps (the bundles keep turning on the input).
TEST(PlayerCamera_HovDriverViewTiltsWithTheGunWhileParked)
{
	if (!gameDir()) return;
	constexpr float kDeg = 3.14159265f / 180;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!std::ifstream(std::string(gameDir()) + "/" + mod + "/ServerArchives.con")) continue;
		Garage g(gameDir(), mod);
		if (!g.loaded || !g.templates.find("as_hov_light")) continue;
		const auto car = g.start("as_hov_light", {0, 2, 6});
		CHECK(g.enter(car));
		anim::AnimationSystems systems(nullptr);
		PlayerCamera camera(*g.world, systems, nullptr);
		for (int i = 0; i < 60; ++i) { g.world->setPlayerInput({}); g.world->tick(1 / 30.0f); camera.tick(1 / 30.0f); }
		PlayerCamera::View rest;
		CHECK(camera.view(rest, 16.0f / 9.0f));
		for (int step = 1; step <= 30; ++step) {
			game::SoldierInput in;
			in.aimPitch = -0.5f * std::min(step, 20);
			g.world->setPlayerInput(in);
			g.world->tick(1 / 30.0f);
			camera.tick(1 / 30.0f);
		}
		float yaw = 0, gun = 0;
		CHECK(g.world->gunAim(car, 0, yaw, gun));
		PlayerCamera::View aimed;
		CHECK(camera.view(aimed, 16.0f / 9.0f));
		CHECK(std::fabs(gun) > 4.0f);
		CHECK(std::fabs((aimed.pitch - rest.pitch) / kDeg - gun) < 0.1f);
	}
}

// The Shepherd with its pilot gone but a gunner aboard (retail: the engines' thrust and the
// ForceObject's keepUp follow the root PCO's own seat, 0x0064df20 / 0x0062c920 through 0x005c87b0
// with 0, not the vehicle's other seats).
namespace {
void shepherdProbe(const char* label, int seat, bool aboard, std::vector<physics::Vec3>& track)
{
	Garage g(gameDir());
	if (!g.loaded) {
		return;
	}
	const uint32_t at = g.start("eu_at", {0, 80, 6}, 0.5f);
	if (aboard) {
		CHECK(g.enter(at));
		game::SoldierInput hold;
		g.drive(hold, 1);
		if (seat > 1) {
			game::SoldierInput move;
			move.switchSeat = seat;
			g.world->setPlayerInput(move);
			g.world->tick(1 / 30.0f);
		}
	}
	game::GameWorld::PlayerInfo me;
	g.world->playerInfo(g.world->localPlayer(), me);
	std::fprintf(stderr, "  %s: seat %d vehicle %u\n", label, me.seat + 1, me.vehicle);
	for (int s = 0; s <= 10; ++s) {
		const physics::Vec3 p = g.position(at);
		const world::Vec3& r = g.objects.find(at)->rotation();
		std::fprintf(stderr, "  %s t=%2d y %.2f z %.2f pitch %.1f roll %.1f\n", label, s, p.y, p.z, r.y, r.z);
		track.push_back(p);
		g.drive({}, 1);
	}
}
}

TEST(Vehicles_ShepherdWithOnlyAGunnerAboard)
{
	if (!gameDir()) {
		return;
	}
	std::vector<physics::Vec3> pilot, gunner, nobody;
	shepherdProbe("pilot", 1, true, pilot);
	shepherdProbe("gunner", 2, true, gunner);
	shepherdProbe("nobody", 1, false, nobody);
	if (pilot.size() == 11 && gunner.size() == 11 && nobody.size() == 11) {
		CHECK(pilot.back().y > 30);                  // the pilot's hold keeps it flying
		CHECK(gunner.back().y < pilot.back().y - 15); // a gunner alone: no thrust, no hover regulator
	}
}

// The pilot leaving the Shepherd in the air: the rotor engines keep running
// (message 5, 0x0064c9bb: a propeller or rotor stops only within the root's
// radius of the ground, 0x006f2a30), while the lift and the keepUp still
// end with the root seat at once (0x0064df20 x 0x005c87b0(0), 0x0062cc2e):
// it falls with the rotor turning. Left on the ground, the engines stop.
TEST(Vehicles_ShepherdRotorRunsOnAfterThePilotBails)
{
	if (!gameDir()) {
		return;
	}
	for (const bool air : {true, false}) {
		Garage g(gameDir());
		if (!g.loaded || !g.templates.find("eu_at")) {
			return;
		}
		const uint32_t at = g.start("eu_at", air ? physics::Vec3 {0, 80, 6} : physics::Vec3 {0, 2, 6}, air ? 0.5f : 3);
		CHECK(g.enter(at));
		g.drive({}, 3);
		const float left = g.position(at).y;
		const auto rotorRpm = [](const game::Vehicle* v, int& rotors, int& running) {
			float rpm = 0;
			rotors = running = 0;
			for (const auto& e : v->engines()) {
				if ((e.flags & 0x20) && !e.pureRotational) {
					++rotors;
					running += e.running;
					rpm = std::max(rpm, std::fabs(e.rpm));
				}
			}
			return rpm;
		};
		int rotors = 0, running = 0;
		const float flying = rotorRpm(g.world->vehicle(at), rotors, running);
		game::SoldierInput use;
		use.use = true;
		g.world->setPlayerInput(use);
		g.world->tick(1 / 30.0f);
		g.world->setPlayerInput({});
		CHECK(g.world->playerVehicle() == nullptr || g.world->playerVehicle()->id() != at);
		g.run(1.5f);
		const game::Vehicle* v = g.world->vehicle(at);
		CHECK(v != nullptr);
		if (!v) {
			return;
		}
		const float rpm = rotorRpm(v, rotors, running);
		const float fell = left - g.position(at).y;
		std::fprintf(stderr, "  %s: left at %.1f m (rpm %.2f), 1.5 s later fell %.1f m, rotors %d running %d rpm %.2f\n",
			air ? "air" : "ground", left, flying, fell, rotors, running, rpm);
		CHECK(rotors > 0);
		if (air) {
			CHECK(running == rotors && rpm > 0.3f);
			CHECK(fell > 20); // no lift without the pilot
		} else {
			CHECK(running == 0 && rpm == 0);
		}
	}
}

// A helicopter engine's sound follows its collective, not its rotor's steady rpm
// (0x0064cb80: clamp(2 aoa, -0.5, 0.5) + 0.5): the gunships' and transports'
// pitch and volume envelopes (authored over 0.42..0.75) move with the throttle.
TEST(Vehicles_HelicopterEngineSoundFollowsTheCollective)
{
	if (!gameDir()) return;
	for (const char* name : {"us_ag", "as_ag", "eu_at", "as_at"}) {
		Garage g(gameDir());
		if (!g.loaded) return;
		const uint32_t ag = g.start(name, {0, 3, 8});
		CHECK(g.enter(ag));
		const game::Vehicle* v = g.world->playerVehicleState();
		CHECK(v && !v->engines().empty());
		const auto& rotor = v->engines().front(); // the one with the Idle / Rpm2 sounds
		game::SoldierInput input;
		input.throttle = 1;
		g.drive(input, 4);
		const float up = rotor.audioValue();
		input.throttle = -1;
		g.drive(input, 2);
		const float down = rotor.audioValue();
		std::fprintf(stderr, "  %s: audio value %.3f climbing, %.3f descending\n", name, up, down);
		CHECK(up >= 0 && up <= 1 && down >= 0 && down <= 1);
		CHECK(std::fabs(up - down) > 0.1f); // the gunships' main engine (negative axis, canReverse 0) opens on throttle down
	}
}
