// World script methods: placing objects.
#include "Console/Console.h"
#include "Game/GameLogic.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <cmath>
#include <string>

using namespace dice::hfe;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

} // namespace

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
