// World script methods: placing objects.
#include "Console/Console.h"
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
