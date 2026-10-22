#include "MaterialManager.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

#include <cstdlib>
#include <functional>

namespace dice::hfe::physics {

Material& MaterialManager::define(uint32_t id)
{
	m_active = &m_materials[id];
	return *m_active;
}

const Material* MaterialManager::find(uint32_t id) const
{
	const auto it = m_materials.find(id);
	return it == m_materials.end() ? nullptr : &it->second;
}

MaterialCell& MaterialManager::createCell(uint32_t a, uint32_t b)
{
	m_activeCell = &m_cells[{a, b}];
	return *m_activeCell;
}

const MaterialCell* MaterialManager::cell(uint32_t a, uint32_t b) const
{
	const auto it = m_cells.find({a, b});
	return it == m_cells.end() ? nullptr : &it->second;
}

const Material* MaterialManager::lookup(uint32_t id) const
{
	const Material* material = find(id);
	return material ? material : find(0);
}

float MaterialManager::friction(uint32_t a, uint32_t b) const
{
	const Material* first = lookup(a);
	const Material* second = lookup(b);
	const float fa = first ? first->friction : 1.0f;
	const float fb = second ? second->friction : 1.0f;
	const bool multiply = (first && first->multiplyFriction) || (second && second->multiplyFriction);
	return multiply ? fa * fb : (fa + fb) * 0.5f;
}

float MaterialManager::elasticity(uint32_t id) const
{
	const Material* material = lookup(id);
	return material ? material->elasticity : 1.0f;
}

float MaterialManager::resistance(uint32_t id) const
{
	const Material* material = lookup(id);
	return material ? material->resistance : 1.0f;
}

float MaterialManager::damageMod(uint32_t projectile, uint32_t surface) const
{
	const MaterialCell* found = cell(projectile, surface);
	return found ? found->damageMod : 1.0f;
}

void registerMaterialConsoleMethods(Console& console, MaterialManager& materials)
{
	const uint32_t access = ConsoleAccess::Anyone;
	bindMethod(console, "Material.active", access, [&materials](int id) {
		materials.define(static_cast<uint32_t>(id));
	});
	// Properties of the active material.
	const auto property = [&console, &materials, access](const char* name, auto member) {
		using Value = std::remove_reference_t<decltype(std::declval<Material&>().*member)>;
		bindProperty<Value>(console, std::string("Material.") + name, access,
			[&materials, member] {
				Material* active = materials.activeMaterial();
				return active ? active->*member : Value {};
			},
			[&materials, member](const Value& value) {
				if (Material* active = materials.activeMaterial()) {
					active->*member = value;
				}
			});
	};
	property("name", &Material::name);
	property("type", &Material::type);
	property("friction", &Material::friction);
	property("elasticity", &Material::elasticity);
	property("resistance", &Material::resistance);
	property("multiplyFriction", &Material::multiplyFriction);
	property("projectileCollisionHardness", &Material::projectileCollisionHardness);
	property("damageLoss", &Material::damageLoss);
	property("minDamageLoss", &Material::minDamageLoss);
	property("maxDamageLoss", &Material::maxDamageLoss);
	property("penetrationDeviation", &Material::penetrationDeviation);
	property("overrideNeverPenetrate", &Material::overrideNeverPenetrate);
	property("hasWaterPhysics", &Material::hasWaterPhysics);
	property("isOneSided", &Material::isOneSided);
	property("isSeeThrough", &Material::isSeeThrough);

	bindMethod(console, "MaterialManager.createCell", access, [&materials](int a, int b) {
		materials.createCell(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
	});
	bindProperty<float>(console, "MaterialManager.damageMod", access,
		[&materials] { return materials.activeCell() ? materials.activeCell()->damageMod : 1.0f; },
		[&materials](const float& value) {
			if (MaterialCell* cell = materials.activeCell()) {
				cell->damageMod = value;
			}
		});
	const auto templateSetter = [&console, &materials, access](const char* name,
									std::vector<std::string> MaterialCell::*member) {
		ConsoleMethod method;
		method.name = std::string("MaterialManager.") + name;
		method.access = access;
		method.minArgs = 2; // <index> <template>
		method.maxArgs = 2;
		method.invoke = [&materials, member](const std::vector<std::string>& args) {
			MaterialCell* cell = materials.activeCell();
			const int index = std::atoi(args[0].c_str());
			if (cell && index >= 0 && index < 16) {
				auto& list = cell->*member;
				if (list.size() <= static_cast<size_t>(index)) {
					list.resize(static_cast<size_t>(index) + 1);
				}
				list[static_cast<size_t>(index)] = args[1];
			}
			return std::string();
		};
		console.registerMethod(std::move(method));
	};
	templateSetter("setDecalTemplate", &MaterialCell::decalTemplates);
	templateSetter("setEffectTemplate", &MaterialCell::effectTemplates);
	templateSetter("setSoundTemplate", &MaterialCell::soundTemplates);
}

} // namespace dice::hfe::physics
