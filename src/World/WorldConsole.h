// Console methods of the world: "ObjectTemplate.*" and "Object.*".
#pragma once

#include <string>
#include <vector>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::world {

class ObjectManager;
class ObjectTemplateManager;

void registerWorldConsoleMethods(Console& console, ObjectTemplateManager& templates, ObjectManager& objects);

// Registers "ObjectTemplate.<name>" properties that aren't typed yet: they
// apply to the active template and keep the value as text in its settings.
void registerUntypedTemplateProperties(Console& console, ObjectTemplateManager& templates,
	const std::vector<std::string>& names);

// ObjectTemplate.grip's PhysicsGripFlags (template +0xfc; the property's
// reader 0x0051ae29, its writer 0x0051aaa9): PGFNoGrip 1, PGFContactGrip
// 2, PGFRollGrip 4, PGFEngineGrip 8, PGFRollGripWhenOccupied 0x10,
// PGFDummyGrip 0x20, PGFStaticFriction 0x40, PGFEngineDummyGrip 0x80. A
// name is taken with or without its c_ prefix; a word under four
// characters is a number, taken by its name, so one that isn't a single
// flag (0, 3, ...) is PGFNoGrip; anything else is PGFNoGrip too.
int physicsGripFlags(const std::string& word);
const char* physicsGripFlagName(int flag); // 0x0051aaa9: PGFNoGrip for any other value

} // namespace dice::hfe::world
