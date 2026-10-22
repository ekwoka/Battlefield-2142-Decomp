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

} // namespace dice::hfe::world
