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
#include <vector>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::geom {

struct GeometryTemplate {
	std::string name;
	MeshKind kind = MeshKind::Static;
	std::string path; // virtual file system path of the mesh (roads: of the settings .dat)
	// GeometryTemplate.hasAdditiveAlpha: its blended materials (alpha mode
	// 1) are added (SrcBlend SRCALPHA, DestBlend ONE) rather than blended
	// over (RendDX9 0x100c3880 reads it at the bundled template's +0x268).
	bool hasAdditiveAlpha = false;
	// GeometryTemplate.simpleUVTranslation u/v: the additive materials'
	// texture coordinates scroll by this per second (RaShaderBMadditive.fx:
	// frac(GlobalTime * simpleUVTranslation); template +0x26c/+0x270).
	float simpleUVTranslation[2] = {0, 0};
	// A MeshParticleMesh (the meshes MeshParticleSystems emit; bundled-mesh
	// files).
	bool meshParticle = false;
	// GeometryTemplate.useRadiusForShadowDepth / useRadiusForShadowSpatial
	// (RendDX9 0x100a7e60..0x100a7e90, the bundled template's +0x204 /
	// +0x205; the heavy mechs set both): the light manager fits the
	// shadow's depth range (0x10125780) and light-space bounds (0x10123e70)
	// to the box pos +- radius * sqrt 2 instead of the mesh's oriented box,
	// steady while its parts move.
	bool useRadiusForShadowDepth = false;
	bool useRadiusForShadowSpatial = false;
	// GeometryTemplate.setSubGeometryLodDistance <geometry> <lod> <distance>
	// (RendDX9 0x1007303a): how far each LOD of a geometry is drawn; 0 or
	// missing for unset.
	std::vector<std::vector<float>> lodDistances;
	// GeometryTemplate.maxSkip3pLods / maxSkipWreckLods (RendDX9 0x10073920
	// / 0x10073a8a; the template's +0xc0 / +0xc4, both 1 by default,
	// 0x10148e50): the LODs the third-person and wreck geometries may drop
	// at load, at most the renderer's +0x910 (1 at the low geometry quality,
	// else 0) and one less than they have (bundled 0x100a31d0, skinned
	// 0x100d4b..: the first LODs go, the loaders 0x100a3b70 / 0x100d4160
	// skip their materials).
	unsigned maxSkip3pLods = 1;
	unsigned maxSkipWreckLods = 1;
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
