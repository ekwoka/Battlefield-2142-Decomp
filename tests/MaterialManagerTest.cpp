// Materials defined by script, and how two of them combine.
#include "Console/Console.h"
#include "Physics/MaterialManager.h"
#include "TestMain.h"

#include <cmath>
#include <string>

using namespace dice::hfe;

TEST(MaterialManager_ScriptsAndFriction)
{
	Console console;
	physics::MaterialManager materials;
	physics::registerMaterialConsoleMethods(console, materials);
	std::string out;
	for (const char* line : {"Material.active 0", "Material.name \"Default\"", "Material.friction 0.75",
			 "Material.active 2", "Material.name \"Sand\"", "Material.friction 0.8", "Material.elasticity 0.1",
			 "Material.active 7", "Material.friction 0.5", "Material.multiplyFriction 1",
			 "MaterialManager.createCell 38 2", "MaterialManager.damageMod 0.5",
			 "MaterialManager.setSoundTemplate 0 S_Bullet_Sand"}) {
		CHECK(console.execute(line, out) == ConsoleResult::Ok);
	}
	CHECK(materials.materialCount() == 3);
	CHECK(materials.find(2)->name == "Sand");
	// The mean, unless one multiplies; unknown materials fall back to 0.
	CHECK(std::fabs(materials.friction(0, 2) - 0.775f) < 1e-6f);
	CHECK(std::fabs(materials.friction(2, 7) - 0.4f) < 1e-6f);
	CHECK(std::fabs(materials.friction(99, 2) - 0.775f) < 1e-6f);
	CHECK(materials.elasticity(2) == 0.1f);
	CHECK(materials.damageMod(38, 2) == 0.5f && materials.damageMod(2, 38) == 1.0f);
	CHECK(materials.cell(38, 2)->soundTemplates.at(0) == "S_Bullet_Sand");
}
