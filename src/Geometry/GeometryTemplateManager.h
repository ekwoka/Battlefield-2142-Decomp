// Geometry templates: the meshes object templates draw ("GeometryTemplate.create
// StaticMesh rock_01" followed by "ObjectTemplate.geometry rock_01").
//
// The mesh file lives next to the script that creates the template:
// <script directory>/Meshes/<name>.staticMesh (or .bundledMesh / .skinnedMesh).
#pragma once

#include "MeshFile.h"

#include <cstdint>
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
	// GeometryTemplate.isTitanInterior / isTitanExterior (RendDX9 property
	// objects 0x1007352b / 0x10073412 -> template slots 0xd0 / 0xc8,
	// 0x10148690 / 0x10148670: +0xc9 / +0xc8, setting one clears the
	// other): the Titans' rooms, drawn only with the camera inside a Titan,
	// and the hull parts and fittings left out from inside (TitanParts.h).
	bool titanInterior = false;
	bool titanExterior = false;
	// GeometryTemplate.titanExteriorPartIndex (0x10074341, +0x2bc): a hull
	// part's slot (1..4) in the static mesh renderer's LOD pairs (0x100baf40).
	int titanExteriorPart = 0;
	// Per material, keyed (geometry << 16 | lod << 8 | material):
	// GeometryTemplate.setMaterialReflectionScale and setSpecularStaticGloss
	// (0x10073caf / 0x10073eb9: bundled templates only, slots 0x40 / 0x48,
	// 0x100a25d0 / 0x100a26b0: maps at +0xec / +0xf8) and
	// setMaterialParallaxHeightScale (0x10073208, any template's slot 0x38;
	// empty for bundled ones, 0x100a7d70).
	std::map<uint32_t, float> reflectionScale;
	std::map<uint32_t, float> staticGloss;
	std::map<uint32_t, float> parallaxHeightScale;
	static uint32_t materialKey(int geometry, int lod, int material)
	{
		return (static_cast<uint32_t>(geometry & 0xff) << 16) | (static_cast<uint32_t>(lod & 0xff) << 8) |
			static_cast<uint32_t>(material & 0xff);
	}
	// GeometryTemplate.nrOfAnimatedUVMatrix (0x10073ba7, bundled +0x110).
	int animatedUVMatrices = 0;
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
