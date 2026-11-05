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
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Game/Vehicle.h"
#include "Hud/Hud.h"
#include "IO/File/FileManager.h"
#include "Main/HudMenusVehicles.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
#include "Main/HudWorld.h"
#include "Render/PostProduction.h"
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
	input = {};
	input.fire = true;
	for (int i = 0; i < 4 && !g.world->projectileCamera(me, camera); ++i) {
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
	}
	CHECK(g.world->projectileCamera(me, camera) && camera.takeOverInput);
	host::roundCameraEffects(*g.world, me, effects);
	CHECK(effects.tv);
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
	// No input: the camera snaps straight (snapToZeroOnNoInput).
	g.world->setPlayerInput({});
	g.world->tick(1 / 30.0f);
	CHECK(g.world->projectileCamera(me, camera));
	for (const auto& p : g.world->projectiles()) {
		if (p.player == me) {
			roundYaw = std::atan2(p.velocity.x, p.velocity.z) * 57.29578f;
		}
	}
	CHECK(std::fabs(std::remainder(camera.yaw - roundYaw, 360.0f)) < 1);
	// The round's life (5 s) runs out: the seat's camera again.
	g.run(6);
	CHECK(!g.world->projectileCamera(me, camera));
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
	const uint32_t tank = g.start("eu_tank", {0, 2, 6});
	CHECK(g.enter(tank));
	CHECK(g.world->seatTurnsGun(tank, 0));
	g.run(2);
	float yaw = 0, pitch = 0;
	CHECK(g.world->gunAim(tank, 0, yaw, pitch));
	const float hullYaw = g.yaw(tank);
	game::SoldierInput look;
	look.turn = 3; // degrees a tick
	look.aimPitch = 4;
	g.drive(look, 0.5f);
	CHECK(g.world->gunAim(tank, 0, yaw, pitch));
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
	fire.aimPitch = 4;
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
	deploy(4);
	const uint32_t hub = placed("unl_hub_ammo_stationary_model");
	CHECK(shield != 0 && hub != 0);
	anim::Matrix early {}, late {};
	CHECK(g.world->partBone(shield, "portable_shield_arm1_anim", early));
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
		input = {};
		input.turn = 2;
		g.world->setPlayerInput(input);
		g.world->tick(1 / 30.0f);
		g.world->setPlayerInput({});
		g.world->gunAim(at, seat, yaw1, pitch);
		turned[zoomed ? 1 : 0] = yaw1 - yaw0;
	}
	std::fprintf(stderr, "  turned %.2f, zoomed %.2f\n", turned[0], turned[1]);
	CHECK(std::fabs(turned[0] - 2) < 0.01f && std::fabs(turned[1] - 1.2f) < 0.01f);
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
