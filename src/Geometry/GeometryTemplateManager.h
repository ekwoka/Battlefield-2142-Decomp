// Geometry templates: the meshes object templates draw ("GeometryTemplate.create
// StaticMesh rock_01" followed by "ObjectTemplate.geometry rock_01").
//
// The mesh file lives next to the script that creates the template:
// <script directory>/Meshes/<name>.staticMesh (or .bundledMesh / .skinnedMesh).
#pragma once

#include "MeshFile.h"

#include <map>
#include <memory>
#include <string>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::geom {

struct GeometryTemplate {
	std::string name;
	MeshKind kind = MeshKind::Static;
	std::string path; // virtual file system path of the mesh (roads: of the settings .dat)
};

class GeometryTemplateManager {
public:
	// `type` is "StaticMesh", "BundledMesh", "SkinnedMesh" or "RoadCompiled" (any case).
	// `scriptDirectory` ends with '/' or is empty. Unknown types create nothing.
	GeometryTemplate* create(const std::string& type, const std::string& name, const std::string& scriptDirectory);
	GeometryTemplate* find(const std::string& name) const;
	GeometryTemplate* active() const { return m_active; }
	size_t count() const { return m_templates.size(); }

private:
	struct Less {
		bool operator()(const std::string& a, const std::string& b) const;
	};
	std::map<std::string, std::unique_ptr<GeometryTemplate>, Less> m_templates;
	GeometryTemplate* m_active = nullptr;
};

void registerGeometryConsoleMethods(Console& console, GeometryTemplateManager& geometries);

} // namespace dice::hfe::geom
