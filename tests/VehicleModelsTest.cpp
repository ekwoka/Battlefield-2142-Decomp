// The vehicle models other than newCar2, with the shipped templates on flat
// ground: the tracked tank, the hover tank, the walker, the transport and
// the gunship, driven by a player in the driver's seat.
#include "AI/PathFollower.h"
#include "Animation/FirstPerson.h"
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/TemplateFiles.h"
#include "World/WorldConsole.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

// Every object template and the materials, on flat ground at height 0; the
// EU spawn point at the origin facing +z.
struct Garage {
	io::FileManager files;
	Console console {&files};
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	physics::CollisionTemplateManager collisionTemplates;
	physics::MaterialManager materials;
	std::unique_ptr<game::GameWorld> world;
	bool loaded = false;

	explicit Garage(const char* gameDir)
	{
		files.setRootPath(gameDir);
		files.addSearchPath("mods/bf2142");
		files.setMountingAllowed(true);
		bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
			[this](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
		std::string out;
		CHECK(console.run("ServerArchives.con", {}, out));
		files.setMountingAllowed(false);
		world::registerWorldConsoleMethods(console, templates, objects);
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

	// The vehicle at `at` facing +z, the world started and settled, the
	// player spawned beside it.
	uint32_t start(const char* name, const physics::Vec3& at, float settle = 3)
	{
		world::ObjectTemplate* t = templates.find(name);
		CHECK(t != nullptr);
		if (!t) {
			return 0;
		}
		world::Object* made = objects.create(t, {at.x, at.y, at.z}, {0, 0, 0});
		made->setTeam(2);
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->enablePhysics(files, collisionTemplates,
			{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return 0.0f;
			 }},
			&materials);
		world->startRound();
		run(settle);
		return made->id();
	}

	// Into the driver's seat: the player tried round the vehicle until an
	// entry point takes him.
	bool enter(uint32_t vehicle)
	{
		if (!world->spawnPlayer(2)) {
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
