// Materials: what surfaces are made of, for physics, damage, impact effects
// and sounds (dice.hfe.world.MaterialManager, class 0xc4af, vtable
// 0x00978f58; the "MaterialManager" singleton).
//
// Common/Material/materialManagerDefine.con defines the materials
//     Material.active <id>      (creates or selects)
//     Material.name "Sand"   Material.friction 0.8   ...
// and materialManagerSettings.con the cells for pairs of materials
//     MaterialManager.createCell <a> <b>
//     MaterialManager.damageMod 0   (and decal/effect/sound templates)
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::physics {

struct Material {
	std::string name;
	int type = 0;
	float friction = 1;
	float elasticity = 0;
	float resistance = 0;
	bool multiplyFriction = false;
	float projectileCollisionHardness = 0;
	float damageLoss = 0;
	float minDamageLoss = 0;
	float maxDamageLoss = 100000; // 0x50's default
	float penetrationDeviation = 0;
	bool overrideNeverPenetrate = false;
	bool hasWaterPhysics = false;
	bool isOneSided = false;
	bool isSeeThrough = false;
};

// A pair of materials meeting (a projectile's and a surface's, ...).
struct MaterialCell {
	float damageMod = -1; // unset (the cell's constructor 0x0060e770: +0x34 -1)
	// By index ("setEffectTemplate <index> <template>"): index 0 is the
	// usual one; others are variants (e.g. 1 for small projectiles).
	std::vector<std::string> decalTemplates;
	std::vector<std::string> effectTemplates;
	std::vector<std::string> soundTemplates;
};

class MaterialManager {
public:
	Material& define(uint32_t id); // creates or returns
	const Material* find(uint32_t id) const;
	MaterialCell& createCell(uint32_t a, uint32_t b);
	const MaterialCell* cell(uint32_t a, uint32_t b) const;
	size_t materialCount() const { return m_materials.size(); }
	size_t cellCount() const { return m_cells.size(); }

	// Slot 0x88 (0x0060a850): the mean of the two materials' friction, or
	// the product if either multiplies (unknown materials use material 0,
	// else 1).
	float friction(uint32_t a, uint32_t b) const;
	float elasticity(uint32_t id) const; // slot 0x80
	float resistance(uint32_t id) const; // slot 0x84
	// The damage a projectile of material `projectile` does to `surface`
	// is multiplied by this (slot 0x64, 0x0060a540): the cell's damageMod,
	// else the cell of `projectile` against material 0's, else 1 (the
	// manager's +0x20); a cell's -1 is unset.
	float damageMod(uint32_t projectile, uint32_t surface) const;

	// The material and cell scripts work on.
	Material* activeMaterial() { return m_active; }
	MaterialCell* activeCell() { return m_activeCell; }

private:
	const Material* lookup(uint32_t id) const;

	std::map<uint32_t, Material> m_materials;
	std::map<std::pair<uint32_t, uint32_t>, MaterialCell> m_cells;
	Material* m_active = nullptr;
	MaterialCell* m_activeCell = nullptr;
};

void registerMaterialConsoleMethods(Console& console, MaterialManager& materials);

} // namespace dice::hfe::physics
