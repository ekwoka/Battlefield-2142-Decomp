// Collision mesh templates ("collisionManager.createTemplate <name>",
// CollisionManager 0x006b3c93): like geometry templates, the mesh lives next
// to the creating script, <script directory>/Meshes/<name>.collisionmesh.
// Object templates name theirs with "ObjectTemplate.collisionMesh".
#pragma once

#include "CollisionMeshFile.h"

#include <map>
#include <memory>
#include <string>

namespace dice::hfe {
class Console;
namespace io {
class FileManager;
}
} // namespace dice::hfe

namespace dice::hfe::physics {

class CollisionTemplateManager {
public:
	void create(const std::string& name, const std::string& scriptDirectory);
	// The template's mesh, loaded on first use; null if missing or unreadable.
	const CollisionMeshFile* mesh(const std::string& name, const io::FileManager& files);
	size_t count() const { return m_paths.size(); }

private:
	struct Less {
		bool operator()(const std::string& a, const std::string& b) const;
	};
	std::map<std::string, std::string, Less> m_paths;
	std::map<std::string, std::unique_ptr<CollisionMeshFile>, Less> m_meshes;
};

void registerCollisionConsoleMethods(Console& console, CollisionTemplateManager& templates);

} // namespace dice::hfe::physics
