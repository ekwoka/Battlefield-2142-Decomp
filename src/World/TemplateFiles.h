// Loading the object templates of the mod ("Loading Templates..").
#pragma once

#include <string>
#include <vector>

namespace dice::hfe {
class Console;
namespace io {
class FileManager;
}
} // namespace dice::hfe

namespace dice::hfe::world {

// The script files that define templates below `directory` ("Objects/"),
// in the order the game runs them: every file whose name contains ".con"
// (and ".ai" when AI is on), sorted by lower-case path with backslash
// separators, except that "\common\" directories sort first.
std::vector<std::string> templateScripts(const io::FileManager& files, const std::string& directory, bool withAi);

// Runs them all (FUN_004d5440). Returns the number of scripts run.
size_t loadObjectTemplates(Console& console, const io::FileManager& files, const std::string& directory, bool withAi);

} // namespace dice::hfe::world
