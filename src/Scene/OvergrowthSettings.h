// The overgrowth's console rows (RendDX9's Overgrowth object DAT_1020ded8,
// vtable 0x101d3790, constructor 0x1008f3b0 over the Growth base
// 0x10122130), as a level's overgrowth/Overgrowth.con sets them: the view
// distances, the materials and the types with their placement settings.
// The level renderer draws the trees from these (LevelRenderer::
// loadOvergrowth); the editor's rows (painting, saving, generating, the
// atlas source) stay unregistered.
#pragma once

#include <array>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::scene {

struct OvergrowthSettings {
	// Overgrowth.path (+0x88): the level's overgrowth directory; set by
	// every shipped Overgrowth.con before preLoad.
	std::string path;
	// Overgrowth.viewDistance (+0x24, 500; both rows 0x100873ba / 0x10087478
	// call slot 3 0x1008f510, which also sizes the cells: viewDistance /
	// (10 / 2 - 1), then fitted to the terrain, 0x10120420).
	float viewDistance = 500;
	float viewDistanceScale = 1;    // +0x28 (0x1008763d)
	float closeDistPercentage = 0.15f; // +0xc8
	float closeDistFade = 0.3f;     // +0xcc
	float lodTimer = 0;             // +0x114: the cross-fade's seconds, 0 at once
	float editorEnable = 0;         // set only by the levels' BF2Editor branch
	struct Material {
		std::string name;
		unsigned index = 0;
	};
	// OvergrowthType's rows act on the active type (the object's +0x108;
	// density its +0x3c, 0x1008a2b0, randomScale +0x44, 0x1008ba90, which
	// reads 1 1 without a type).
	struct Type {
		std::string name;
		std::string geometry;
		float density = 0;
		float normalScale = 0;
		float rotationScale = 0;
		float minRadiusToSame = 0;
		float minRadiusToOthers = 0;
		std::array<float, 2> randomScale {1, 1};
	};
	// Overgrowth.addMaterial (0x10086c96 -> 0x10092840: refused when a
	// material has the index already) and addType (refused for a name
	// already there); the scripts make each active next.
	std::vector<Material> materials;
	std::vector<Type> types;
	int activeMaterial = -1;
	int activeType = -1;
	// Overgrowth.preLoad (0x10086ace -> 0x1008c7e0) loads the level's data;
	// the renderer does at its level load once this is set.
	bool preLoaded = false;
	// True once a script set any of the view distances.
	bool configured = false;

	bool addMaterial(const std::string& name, unsigned index);
	bool addType(const std::string& name);
	Type* active() { return activeType >= 0 && activeType < static_cast<int>(types.size()) ? &types[activeType] : nullptr; }
	// A type's geometry by name (the trees' templates are their types' geometries).
	const Type* typeForGeometry(const std::string& geometry) const;
};

void registerOvergrowthConsoleMethods(Console& console, OvergrowthSettings& settings);

} // namespace dice::hfe::scene
