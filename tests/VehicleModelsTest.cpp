// The vehicle models other than newCar2, with the shipped templates on flat
// ground: the tracked tank, the hover tank, the walker, the transport and
// the gunship, driven by a player in the driver's seat.
#include "AI/Heli.h"
#include "AI/PathFollower.h"
#include "Animation/AnimationConsole.h"
#include "Animation/AnimationSystem.h"
#include "Animation/FirstPerson.h"
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameWorld.h"
#include "Game/Vehicle.h"
#include "IO/File/FileManager.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/TemplateFiles.h"
#include "World/WorldConsole.h"

#include <algorithm>
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

bool near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

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
	uint32_t start(const char* name, const physics::Vec3& at, float settle = 3, float ground = 0, float water = -10000)
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
			{[ground](float, float) { return ground; }, [ground](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return ground;
			 }},
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
}
