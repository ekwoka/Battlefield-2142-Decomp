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
	bindProperty<std::string>(console, "Overgrowth.path", kAccess, [&s] { return s.path; }, // 0x1008699d, 0x10086a07
		[&s](const std::string& v) { s.path = v; });
	bindMethod(console, "Overgrowth.preLoad", kAccess, [&s] { s.preLoaded = true; }); // 0x10086a7c
	bindDistance("Overgrowth.viewDistance", s.viewDistance); // 0x1008742b, 0x100874e9
	bindDistance("Overgrowth.viewDistanceScale", s.viewDistanceScale); // 0x1008757f, 0x100875f0
	bindDistance("Overgrowth.closeDistPercentage", s.closeDistPercentage); // 0x10087ce8, 0x10087d59, 0x10087da6
	bindFloat("Overgrowth.closeDistFade", s.closeDistFade); // 0x10087f2e, 0x10087f9f, 0x10087fec
	bindFloat("Overgrowth.lodTimer", s.lodTimer); // 0x10088032, 0x100880a3, 0x100880f0
	bindFloat("Overgrowth.editorEnable", s.editorEnable); // 0x100881a7, 0x100881f4
	bindMethod(console, "Overgrowth.addMaterial", kAccess, // 0x10086c04, 0x10086ce7
		[&s](std::string name, unsigned index) { return s.addMaterial(name, index); });
	bindMethod(console, "Overgrowth.setActiveMaterial", kAccess, [&s](std::string name) { // 0x10086e0f, 0x10086e80, 0x10086ebf
		for (size_t i = 0; i < s.materials.size(); ++i) {
			if (sameName(s.materials[i].name, name)) {
				s.activeMaterial = static_cast<int>(i);
			}
		}
	});
	bindMethod(console, "Overgrowth.getActiveMaterialName", kAccess, [&s] { // 0x10086f34, 0x10086f93
		return s.activeMaterial >= 0 ? s.materials[static_cast<size_t>(s.activeMaterial)].name : std::string();
	});
	bindMethod(console, "Overgrowth.removeActiveMaterial", kAccess, [&s] { // 0x10086d94, 0x10086de7
		if (s.activeMaterial < 0) {
			return false;
		}
		s.materials.erase(s.materials.begin() + s.activeMaterial);
		s.activeMaterial = -1;
		return true;
	});
	bindMethod(console, "Overgrowth.addType", kAccess, [&s](std::string name) { return s.addType(name); }); // 0x1008707a, 0x100870ed, 0x10087138
	bindMethod(console, "Overgrowth.setActiveType", kAccess, [&s](std::string name) { // 0x10087228, 0x10087299, 0x100872d8
		for (size_t i = 0; i < s.types.size(); ++i) {
			if (sameName(s.types[i].name, name)) {
				s.activeType = static_cast<int>(i);
			}
		}
	});
	bindMethod(console, "Overgrowth.removeActiveType", kAccess, [&s] { // 0x100871ad, 0x10087200
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
	bindProperty<std::string>(console, "OvergrowthType.geometry", kAccess, // 0x100882df, 0x10088356
		[&s] {
			const OvergrowthSettings::Type* t = s.active();
			return t ? t->geometry : std::string();
		},
		[&s](const std::string& v) {
			if (OvergrowthSettings::Type* t = s.active()) {
				t->geometry = v;
			}
		});
	bindType("OvergrowthType.density", &OvergrowthSettings::Type::density); // 0x100883cb, 0x1008843c, 0x10088489
	bindType("OvergrowthType.normalScale", &OvergrowthSettings::Type::normalScale); // 0x100884d2, 0x10088543, 0x10088590
	bindType("OvergrowthType.rotationScale", &OvergrowthSettings::Type::rotationScale); // 0x100885d9, 0x10088656, 0x100886a3
	bindType("OvergrowthType.minRadiusToSame", &OvergrowthSettings::Type::minRadiusToSame); // 0x100886ec, 0x1008875d, 0x100887aa
	bindType("OvergrowthType.minRadiusToOthers", &OvergrowthSettings::Type::minRadiusToOthers); // 0x100887f3, 0x10088864, 0x100888b1
	bindProperty<ConsoleVec2>(console, "OvergrowthType.randomScale", kAccess, // 0x1008897a, 0x100889e5
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
