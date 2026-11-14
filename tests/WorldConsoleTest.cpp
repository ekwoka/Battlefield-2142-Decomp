// World script methods: placing objects.
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameLogic.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/InertRows.h"
#include "World/WorldConsole.h"

#include <cmath>
#include <string>

using namespace dice::hfe;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

} // namespace

TEST(WorldConsole_TemplateLookupLifecycle)
{
	world::ObjectTemplateManager templates;
	CHECK(templates.find("LatePart") == nullptr);
	auto* first = templates.create("SimpleObject", "LatePart");
	CHECK(first && templates.find("latePART") == first);
	CHECK(templates.create("Bundle", "LATEpart") == first);
	const auto id = first->id();
	templates.destroy(first);
	CHECK(templates.find("LatePart") == nullptr && templates.findById(id) == nullptr);
	auto* replacement = templates.create("Bundle", "latepart");
	CHECK(replacement && replacement->id() != id && templates.find("LATEPART") == replacement);
	CHECK(templates.find("") == nullptr);
	const std::string highA = std::string("Part") + static_cast<char>(0xc0);
	const std::string highB = std::string("PART") + static_cast<char>(0xe0);
	auto* high = templates.create("SimpleObject", highA);
	CHECK(high && templates.find(highA) == high && templates.find(highB) == nullptr);
}

TEST(WorldConsole_AnimatedBundleSkeletonProperty)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "AnimatedBundle");
	world::registerWorldConsoleMethods(console, templates, objects);
	std::string out;
	CHECK(console.execute("ObjectTemplate.create AnimatedBundle flag_eu", out) == ConsoleResult::Ok);
	CHECK(console.execute("ObjectTemplate.skeleton Objects\\Common\\Flags\\flag_setup.ske", out) == ConsoleResult::Ok);
	const world::ObjectTemplate* flag = templates.find("flag_eu");
	CHECK(flag && flag->settings().at("skeleton") == "Objects\\Common\\Flags\\flag_setup.ske");
}

// Overgrowth collision objects (OvergrowthCollision.con) are placed by a
// matrix; the rotation must come back as the engine's yaw/pitch/roll.
TEST(WorldConsole_AbsoluteTransformation)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "SimpleObject");
	world::registerWorldConsoleMethods(console, templates, objects);
	std::string out;
	CHECK(console.execute("ObjectTemplate.create SimpleObject tree", out) == ConsoleResult::Ok);
	CHECK(console.execute("Object.create tree", out) == ConsoleResult::Ok);
	CHECK(console.execute("Object.absoluteTransformation "
						  "[-0.0725665/0/0.997364/0][0/1/0/0][-0.997364/0/-0.0725665/0][-45.0684/172.647/-919.156/1]",
			  out) == ConsoleResult::Ok);
	const world::Object* object = objects.active();
	CHECK(object != nullptr);
	CHECK(near(object->position().x, -45.0684f) && near(object->position().y, 172.647f));
	CHECK(near(object->position().z, -919.156f));
	// Row 2 is (sin yaw, 0, cos yaw) for a pure yaw.
	CHECK(near(object->rotation().x, std::atan2(-0.997364f, -0.0725665f) * 180 / 3.14159265f));
	CHECK(near(object->rotation().y, 0) && near(object->rotation().z, 0));

	// Any yaw/pitch/roll survives formatting and parsing back.
	CHECK(console.execute("Object.rotation 30/-20/10", out) == ConsoleResult::Ok);
	CHECK(console.execute("Object.absoluteTransformation", out) == ConsoleResult::Ok);
	const std::string matrix = out;
	CHECK(console.execute("Object.rotation 0/0/0", out) == ConsoleResult::Ok);
	CHECK(console.execute("Object.absoluteTransformation " + matrix, out) == ConsoleResult::Ok);
	CHECK(near(object->rotation().x, 30) && near(object->rotation().y, -20) && near(object->rotation().z, 10));
}

// ObjectTemplate.supplyGroupNeeded <team> <group> [optional] (0x00503dec:
// two or three arguments) keeps each pair in order.
TEST(WorldConsole_SupplyGroupNeeded)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "ControlPoint");
	world::registerWorldConsoleMethods(console, templates, objects);
	std::string out;
	CHECK(console.execute("ObjectTemplate.create ControlPoint cp", out) == ConsoleResult::Ok);
	CHECK(console.execute("ObjectTemplate.supplyGroupNeeded 2 3 0", out) == ConsoleResult::Ok);
	CHECK(console.execute("ObjectTemplate.supplyGroupNeeded 1 4", out) == ConsoleResult::Ok);
	CHECK(console.execute("ObjectTemplate.supplyGroupNeeded 1 5 1", out) == ConsoleResult::Ok);
	CHECK(templates.find("cp")->settings()["supplyGroupNeeded.list"] == "2 3 0;1 4 0;1 5 1;");
}

// GameLogic.setBeforeSpawnCamera <position> <rotation> (0x004278ef, the
// levels' Init.con) keeps the camera for the view before the first spawn.
TEST(WorldConsole_BeforeSpawnCamera)
{
	Console console;
	game::GameLogic logic;
	game::registerGameLogicConsoleMethods(console, logic);
	std::string out;
	CHECK(!logic.beforeSpawnCamera.set);
	CHECK(console.execute("gameLogic.setBeforeSpawnCamera -180/29/-451 -138/-19/0", out) == ConsoleResult::Ok);
	CHECK(logic.beforeSpawnCamera.set && logic.beforeSpawnCamera.position[2] == -451 && logic.beforeSpawnCamera.rotation[0] == -138);
}

// GameLogic.supplyDropHeight / supplyDropNumSecsToLive (+0x1b8 / +0x1bc)
// and radialLevelIndex (0..100, the IGA textures' key) as properties.
TEST(WorldConsole_GameLogicSupplyDropAndRadialLevel)
{
	Console console;
	game::GameLogic logic;
	game::registerGameLogicConsoleMethods(console, logic);
	std::string out;
	CHECK(logic.supplyDropHeight == 50 && logic.supplyDropNumSecsToLive == 300 && logic.radialLevelIndex == 100);
	CHECK(console.execute("GameLogic.supplyDropHeight 80", out) == ConsoleResult::Ok);
	CHECK(console.execute("GameLogic.supplyDropNumSecsToLive 120", out) == ConsoleResult::Ok);
	CHECK(console.execute("GameLogic.radialLevelIndex 250", out) == ConsoleResult::Ok);
	CHECK(logic.supplyDropHeight == 80 && logic.supplyDropNumSecsToLive == 120 && logic.radialLevelIndex == 100);
	CHECK(console.execute("GameLogic.supplyDropHeight", out) == ConsoleResult::Ok && std::stof(out) == 80);
}

// Template ids (0x0070a700): a template its class makes client-only (flag
// 0x2000, the sounds) is numbered from 0x1000000, apart from the
// network-shared ones, which go on counting as if it weren't there (a
// dedicated server, which makes no sounds, numbers them alike).
TEST(WorldConsole_ClientOnlyTemplateIds)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericFireArm");
	templates.registerType(2, "Sound", [](uint32_t classId, const std::string& type, const std::string& name, uint32_t id) {
		auto t = std::make_unique<world::ObjectTemplate>(classId, type, name, id);
		t->setFlags(t->flags() | world::TemplateFlags::ClientOnly);
		return t;
	});
	const uint32_t first = templates.create("GenericFireArm", "a")->id();
	world::ObjectTemplate* sound = templates.create("Sound", "s_a");
	world::ObjectTemplate* second = templates.create("GenericFireArm", "b");
	CHECK(sound->id() == 0x1000000 && (sound->flags() & world::TemplateFlags::ClientOnly) != 0);
	CHECK(second->id() == first + 1);
	CHECK(templates.findById(second->id()) == second && templates.findById(0x1000000) == sound);
}

// GameLogic.defaultTimeToNextAIWave (+0x28c, 8; read by nothing) as the
// co-op maps set it, and Object.notInAI (0x006ed7a4 -> 0x006f6ef0): the
// object goes in the single-player and co-op modes, stays in the others.
TEST(WorldConsole_AiWaveTimeAndNotInAI)
{
	Console console;
	game::GameLogic logic;
	game::registerGameLogicConsoleMethods(console, logic);
	std::string out;
	CHECK(logic.defaultTimeToNextAIWave == 8);
	CHECK(console.execute("gameLogic.setDefaultTimeToNextAIWave 0", out) == ConsoleResult::Ok);
	CHECK(logic.defaultTimeToNextAIWave == 0);
	for (const char* mode : {"gpm_cq", "gpm_coop", "gpm_sp"}) {
		world::ObjectTemplateManager templates;
		world::ObjectManager objects;
		templates.registerType(1, "SimpleObject");
		templates.create("SimpleObject", "crate");
		Console world;
		world::registerWorldConsoleMethods(world, templates, objects);
		objects.setGameMode(mode);
		CHECK(world.execute("Object.create crate", out) == ConsoleResult::Ok);
		CHECK(world.execute("Object.notInAI 1", out) == ConsoleResult::Ok);
		CHECK(objects.objects().size() == (std::string(mode) == "gpm_cq" ? 1u : 0u));
	}
}

// ObjectTemplate.addVehicleType: one argument is the SupplyObject's list of
// vehicle categories (0x00504752, +0x2d0), kept as their numbers in order;
// four the SupplyDepot's (0x004f98f9). setIfTitanChangeToWeapon <weapon>
// <distance> (0x004e20d4) keeps both.
TEST(WorldConsole_AddVehicleTypeAndTitanWeapon)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "SupplyObject");
	templates.registerType(2, "GenericFireArm");
	world::registerWorldConsoleMethods(console, templates, objects);
	std::string out;
	CHECK(console.execute("ObjectTemplate.create SupplyObject pad", out) == ConsoleResult::Ok);
	CHECK(console.execute("ObjectTemplate.addVehicleType 3", out) == ConsoleResult::Ok);
	CHECK(console.execute("ObjectTemplate.addVehicleType VCAir", out) == ConsoleResult::Ok);
	CHECK(console.execute("ObjectTemplate.addVehicleType depot 1 2.5 3", out) == ConsoleResult::Ok);
	const world::ObjectTemplate* pad = templates.find("pad");
	CHECK(pad->settings().at("addVehicleType.list") == "3 2");
	CHECK(pad->settings().at("addVehicleType.supplyDepot.list") == "depot 1 2.5 3;");
	CHECK(console.execute("ObjectTemplate.create GenericFireArm pod_launcher_cq", out) == ConsoleResult::Ok);
	CHECK(console.execute("ObjectTemplate.setIfTitanChangeToWeapon pod_launcher 250", out) == ConsoleResult::Ok);
	CHECK(templates.find("pod_launcher_cq")->settings().at("setIfTitanChangeToWeapon") == "pod_launcher 250");
}

// The inert rows take the original's argument counts and do nothing; a real
// binding registered afterwards replaces one. The level's texture suffix
// (texturemanager.customTextureSuffix) is kept.
TEST(WorldConsole_InertRows)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::registerWorldConsoleMethods(console, templates, objects);
	std::string out;
	CHECK(console.execute("Object.setLightSourceMask 6", out) == ConsoleResult::Ok);
	CHECK(console.execute("RagDoll.toSkeleton 1 2 3", out) == ConsoleResult::Ok);
	CHECK(console.execute("RagDoll.toSkeleton 1 2", out) == ConsoleResult::Error);
	CHECK(console.execute("GeometryTemplate.setLightmapSizeAll 0 0 64", out) == ConsoleResult::Ok);
	CHECK(console.execute("terrain.init 1", out) == ConsoleResult::Error);
	CHECK(console.execute("awards.setData 100_1 \"6,9, ,15\"", out) == ConsoleResult::Ok);
	bool replaced = false;
	bindMethod(console, "undergrowth.load", ConsoleAccess::Anyone, [&replaced](std::string) { replaced = true; });
	CHECK(console.execute("undergrowth.load Levels/Minsk/", out) == ConsoleResult::Ok);
	CHECK(replaced);
	CHECK(console.execute("texturemanager.customTextureSuffix \"Snow\"", out) == ConsoleResult::Ok);
	CHECK(world::customTextureSuffix() == "Snow");
	world::setCustomTextureSuffix("");
}
