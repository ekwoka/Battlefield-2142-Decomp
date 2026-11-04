#include "OvergrowthSettings.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

#include <algorithm>
#include <cctype>

namespace dice::hfe::scene {

namespace {

constexpr uint32_t kAccess = ConsoleAccess::Anyone;

bool sameName(const std::string& a, const std::string& b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
	});
}

} // namespace

bool OvergrowthSettings::addMaterial(const std::string& name, unsigned index)
{
	for (const Material& m : materials) {
		if (m.index == index) {
			return false;
		}
	}
	materials.push_back({name, index});
	return true;
}

bool OvergrowthSettings::addType(const std::string& name)
{
	for (const Type& t : types) {
		if (sameName(t.name, name)) {
			return false;
		}
	}
	types.push_back({});
	types.back().name = name;
	return true;
}

const OvergrowthSettings::Type* OvergrowthSettings::typeForGeometry(const std::string& geometry) const
{
	for (const Type& t : types) {
		if (sameName(t.geometry, geometry)) {
			return &t;
		}
	}
	return nullptr;
}

void registerOvergrowthConsoleMethods(Console& console, OvergrowthSettings& s)
{
	const auto bindDistance = [&console, &s](const char* name, float& value) {
		bindProperty<float>(console, name, kAccess, [&value] { return value; },
			[&value, &s](const float& v) {
				value = v;
				s.configured = true;
			});
	};
	const auto bindFloat = [&console](const char* name, float& value) {
		bindProperty<float>(console, name, kAccess, [&value] { return value; }, [&value](const float& v) { value = v; });
	};
	// The object's own rows (constructors 0x10086920..0x10088136).
	bindProperty<std::string>(console, "Overgrowth.path", kAccess, [&s] { return s.path; },
		[&s](const std::string& v) { s.path = v; });
	bindMethod(console, "Overgrowth.preLoad", kAccess, [&s] { s.preLoaded = true; });
	bindDistance("Overgrowth.viewDistance", s.viewDistance);
	bindDistance("Overgrowth.viewDistanceScale", s.viewDistanceScale);
	bindDistance("Overgrowth.closeDistPercentage", s.closeDistPercentage);
	bindFloat("Overgrowth.closeDistFade", s.closeDistFade);
	bindFloat("Overgrowth.lodTimer", s.lodTimer);
	bindFloat("Overgrowth.editorEnable", s.editorEnable);
	bindMethod(console, "Overgrowth.addMaterial", kAccess,
		[&s](std::string name, unsigned index) { return s.addMaterial(name, index); });
	bindMethod(console, "Overgrowth.setActiveMaterial", kAccess, [&s](std::string name) {
		for (size_t i = 0; i < s.materials.size(); ++i) {
			if (sameName(s.materials[i].name, name)) {
				s.activeMaterial = static_cast<int>(i);
			}
		}
	});
	bindMethod(console, "Overgrowth.getActiveMaterialName", kAccess, [&s] {
		return s.activeMaterial >= 0 ? s.materials[static_cast<size_t>(s.activeMaterial)].name : std::string();
	});
	bindMethod(console, "Overgrowth.removeActiveMaterial", kAccess, [&s] {
		if (s.activeMaterial < 0) {
			return false;
		}
		s.materials.erase(s.materials.begin() + s.activeMaterial);
		s.activeMaterial = -1;
		return true;
	});
	bindMethod(console, "Overgrowth.addType", kAccess, [&s](std::string name) { return s.addType(name); });
	bindMethod(console, "Overgrowth.setActiveType", kAccess, [&s](std::string name) {
		for (size_t i = 0; i < s.types.size(); ++i) {
			if (sameName(s.types[i].name, name)) {
				s.activeType = static_cast<int>(i);
			}
		}
	});
	bindMethod(console, "Overgrowth.removeActiveType", kAccess, [&s] {
		if (s.activeType < 0) {
			return false;
		}
		s.types.erase(s.types.begin() + s.activeType);
		s.activeType = -1;
		return true;
	});
	// OvergrowthType.* (constructors 0x10088262..0x100888fa): the active
	// type's; reads 0 (randomScale 1 1) without one.
	const auto bindType = [&console, &s](const char* name, float OvergrowthSettings::Type::*field) {
		bindProperty<float>(console, name, kAccess,
			[&s, field] {
				const OvergrowthSettings::Type* t = s.active();
				return t ? t->*field : 0.0f;
			},
			[&s, field](const float& v) {
				if (OvergrowthSettings::Type* t = s.active()) {
					t->*field = v;
				}
			});
	};
	bindProperty<std::string>(console, "OvergrowthType.geometry", kAccess,
		[&s] {
			const OvergrowthSettings::Type* t = s.active();
			return t ? t->geometry : std::string();
		},
		[&s](const std::string& v) {
			if (OvergrowthSettings::Type* t = s.active()) {
				t->geometry = v;
			}
		});
	bindType("OvergrowthType.density", &OvergrowthSettings::Type::density);
	bindType("OvergrowthType.normalScale", &OvergrowthSettings::Type::normalScale);
	bindType("OvergrowthType.rotationScale", &OvergrowthSettings::Type::rotationScale);
	bindType("OvergrowthType.minRadiusToSame", &OvergrowthSettings::Type::minRadiusToSame);
	bindType("OvergrowthType.minRadiusToOthers", &OvergrowthSettings::Type::minRadiusToOthers);
	bindProperty<ConsoleVec2>(console, "OvergrowthType.randomScale", kAccess,
		[&s] {
			const OvergrowthSettings::Type* t = s.active();
			return t ? ConsoleVec2 {t->randomScale[0], t->randomScale[1]} : ConsoleVec2 {1, 1};
		},
		[&s](const ConsoleVec2& v) {
			if (OvergrowthSettings::Type* t = s.active()) {
				t->randomScale = {v.x, v.y};
			}
		});
}

} // namespace dice::hfe::scene
