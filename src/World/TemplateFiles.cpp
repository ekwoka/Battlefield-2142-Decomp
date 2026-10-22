#include "TemplateFiles.h"

#include "Console/Console.h"
#include "IO/File/FileManager.h"

#include <algorithm>
#include <map>

namespace dice::hfe::world {

namespace {

void replaceAll(std::string& text, const std::string& from, const std::string& to)
{
	for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size())) {
		text.replace(pos, from.size(), to);
	}
}

} // namespace

std::vector<std::string> templateScripts(const io::FileManager& files, const std::string& directory, bool withAi)
{
	// The original keeps the paths in a std::set keyed by the Windows path
	// with "\common\" renamed to "\aaaaaaaaaaaaaaa\", so shared templates
	// load before the ones that use them.
	std::map<std::string, std::string> ordered; // sort key -> path
	for (const std::string& path : files.listFilesRecursive(directory)) {
		if (path.find(".con") == std::string::npos && !(withAi && path.find(".ai") != std::string::npos)) {
			continue;
		}
		std::string key = path;
		std::replace(key.begin(), key.end(), '/', '\\');
		key = "\\" + key;
		replaceAll(key, "\\common\\", "\\aaaaaaaaaaaaaaa\\");
		ordered.emplace(key, path);
	}
	std::vector<std::string> scripts;
	scripts.reserve(ordered.size());
	std::string prefix = directory;
	if (!prefix.empty() && prefix.back() != '/') {
		prefix.push_back('/');
	}
	for (const auto& [key, path] : ordered) {
		scripts.push_back(prefix + path);
	}
	return scripts;
}

size_t loadObjectTemplates(Console& console, const io::FileManager& files, const std::string& directory, bool withAi)
{
	const std::vector<std::string> scripts = templateScripts(files, directory, withAi);
	std::string output;
	for (const std::string& script : scripts) {
		console.run(script, {}, output);
	}
	return scripts.size();
}

} // namespace dice::hfe::world
