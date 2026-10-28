#include "CollisionTemplateManager.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "IO/File/FileManager.h"

#include <algorithm>
#include <cctype>

namespace dice::hfe::physics {

bool CollisionTemplateManager::Less::operator()(const std::string& a, const std::string& b) const
{
	return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
	});
}

void CollisionTemplateManager::create(const std::string& name, const std::string& scriptDirectory)
{
	m_paths.emplace(name, scriptDirectory + "Meshes/" + name + ".collisionmesh");
}

const CollisionMeshFile* CollisionTemplateManager::mesh(const std::string& name, const io::FileManager& files)
{
	const auto loaded = m_meshes.find(name);
	if (loaded != m_meshes.end()) {
		return loaded->second.get();
	}
	auto& slot = m_meshes[name];
	const auto path = m_paths.find(name);
	std::vector<uint8_t> data;
	if (path != m_paths.end() && files.readFile(path->second, data)) {
		if (auto parsed = parseCollisionMesh(data)) {
			slot = std::make_unique<CollisionMeshFile>(std::move(*parsed));
		}
	}
	return slot.get();
}

const CollisionMeshFile* CollisionTemplateManager::mesh(const std::string& name, const io::FileManager& files,
	const std::string& scriptDirectory)
{
	if (m_paths.find(name) == m_paths.end() && !scriptDirectory.empty()) {
		m_meshes.erase(name);
		create(name, scriptDirectory);
	}
	return mesh(name, files);
}

void registerCollisionConsoleMethods(Console& console, CollisionTemplateManager& templates)
{
	bindMethod(console, "collisionManager.createTemplate", ConsoleAccess::Anyone,
		[&console, &templates](std::string name) {
			const std::string& file = console.currentFile();
			const size_t slash = file.rfind('/');
			templates.create(name, slash == std::string::npos ? std::string() : file.substr(0, slash + 1));
		});
}

} // namespace dice::hfe::physics
