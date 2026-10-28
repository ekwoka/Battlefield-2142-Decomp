#include "CombatAreas.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dice::hfe::game {

namespace {

bool parseVec2(const std::string& text, float& x, float& z)
{
	return std::sscanf(text.c_str(), "%f/%f", &x, &z) == 2;
}

// The nearest point of segment a-b to p.
void nearestOnSegment(float px, float pz, float ax, float az, float bx, float bz, float& qx, float& qz)
{
	const float dx = bx - ax, dz = bz - az;
	const float length = dx * dx + dz * dz;
	const float t = length > 0 ? std::clamp(((px - ax) * dx + (pz - az) * dz) / length, 0.0f, 1.0f) : 0.0f;
	qx = ax + dx * t;
	qz = az + dz * t;
}

} // namespace

bool CombatArea::contains(float x, float z) const
{
	if (!polygon) {
		return x >= minX && x <= maxX && z >= minZ && z <= maxZ;
	}
	// Even-odd rule over the polygon's edges.
	bool in = false;
	for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
		const auto& a = points[i];
		const auto& b = points[j];
		if ((a.second > z) != (b.second > z) && x < (b.first - a.first) * (z - a.second) / (b.second - a.second) + a.first) {
			in = !in;
		}
	}
	return in;
}

float CombatArea::borderDistance(float x, float z, float& dirX, float& dirZ) const
{
	std::vector<std::pair<float, float>> corners = points;
	if (!polygon) {
		corners = {{minX, minZ}, {maxX, minZ}, {maxX, maxZ}, {minX, maxZ}};
	}
	float best = 1e30f, bestX = x, bestZ = z;
	for (size_t i = 0, j = corners.size() - 1; i < corners.size(); j = i++) {
		float qx = 0, qz = 0;
		nearestOnSegment(x, z, corners[j].first, corners[j].second, corners[i].first, corners[i].second, qx, qz);
		const float d = (qx - x) * (qx - x) + (qz - z) * (qz - z);
		if (d < best) {
			best = d;
			bestX = qx;
			bestZ = qz;
		}
	}
	const float distance = std::sqrt(best);
	dirX = distance > 0 ? (bestX - x) / distance : 0;
	dirZ = distance > 0 ? (bestZ - z) / distance : 0;
	return distance;
}

float CombatAreas::timerFor(int category, bool soldier) const
{
	const auto it = vehicleTimers.find(category);
	return !soldier && it != vehicleTimers.end() ? it->second : timeAllowedOutside;
}

float CombatAreas::damageFor(int category) const
{
	const auto it = vehicleDamages.find(category);
	return it != vehicleDamages.end() ? it->second : damage;
}

void CombatAreas::reset()
{
	*this = CombatAreas {};
}

bool CombatAreas::any(int category) const
{
	return use && std::any_of(areas.begin(), areas.end(), [category](const CombatArea& a) {
		return a.binds(category) && (!a.polygon ? a.maxX > a.minX && a.maxZ > a.minZ : a.points.size() >= 3);
	});
}

bool CombatAreas::inside(float x, float z, int category) const
{
	bool normal = false, in = false;
	for (const CombatArea& area : areas) {
		if (!area.binds(category) || (area.polygon && area.points.size() < 3) || area.inverted) {
			continue;
		}
		normal = true;
		in = in || area.contains(x, z);
	}
	if (!normal) {
		in = true; // no area binds it: the whole world (0x004ca7d0's heightmap test)
	}
	for (const CombatArea& area : areas) {
		if (area.binds(category) && area.inverted && area.contains(x, z)) {
			in = false;
		}
	}
	return in;
}

bool CombatAreas::nearestBorder(float x, float z, float radius, int category, float& distance, float& dirX,
	float& dirZ) const
{
	bool found = false;
	distance = radius;
	for (const CombatArea& area : areas) {
		if (!area.binds(category) || (area.polygon && area.points.size() < 3)
			|| (!area.polygon && (area.maxX <= area.minX || area.maxZ <= area.minZ))) {
			continue;
		}
		float dx = 0, dz = 0;
		const float d = area.borderDistance(x, z, dx, dz);
		if (d < distance) {
			distance = d;
			dirX = dx;
			dirZ = dz;
			found = true;
		}
	}
	return found;
}

void registerCombatAreaConsoleMethods(Console& console, CombatAreas& areas)
{
	const uint32_t access = ConsoleAccess::Anyone;
	CombatAreas* a = &areas;
	const auto active = [a]() -> CombatArea* {
		return a->active >= 0 && static_cast<size_t>(a->active) < a->areas.size() ? &a->areas[static_cast<size_t>(a->active)]
																				: nullptr;
	};
	bindMethod(console, "CombatAreaManager.use", access, [a](int use) { a->use = use != 0; });
	// The vehicle categories by name (world::VehicleCategory).
	const auto category = [](const std::string& name) {
		static const char* const names[] = {"VCLand", "VCSea", "VCAir", "VCHelicopter", "VCNoVehicleCategory"};
		for (int i = 0; i < 5; ++i) {
			if (name == names[i]) {
				return i;
			}
		}
		return std::atoi(name.c_str());
	};
	bindMethod(console, "CombatAreaManager.addVehicleDamage", access, [a, category](std::string c, float damage) {
		a->vehicleDamages[category(c)] = damage; // 0x004cb010
	});
	bindMethod(console, "CombatAreaManager.addVehicleTimer", access, [a, category](std::string c, float seconds) {
		a->vehicleTimers[category(c)] = seconds; // 0x004cafb0
	});
	{
		ConsoleMethod m;
		m.name = "CombatAreaManager.damage";
		m.kind = ConsoleMethodKind::ReadWriteProperty;
		m.access = access;
		m.maxArgs = 1;
		m.invoke = [a](const std::vector<std::string>& args) {
			if (!args.empty()) {
				a->damage = static_cast<float>(std::atof(args[0].c_str()));
			}
			char text[32];
			std::snprintf(text, sizeof(text), "%g", a->damage);
			return std::string(text);
		};
		console.registerMethod(m);
		m.name = "CombatAreaManager.timeAllowedOutside";
		m.invoke = [a](const std::vector<std::string>& args) {
			if (!args.empty()) {
				a->timeAllowedOutside = static_cast<float>(std::atof(args[0].c_str()));
			}
			char text[32];
			std::snprintf(text, sizeof(text), "%g", a->timeAllowedOutside);
			return std::string(text);
		};
		console.registerMethod(m);
	}
	// create (0x004cb230): an area of that name again, else a new one.
	bindMethod(console, "CombatArea.create", access, [a](std::string name) {
		for (size_t i = 0; i < a->areas.size(); ++i) {
			if (a->areas[i].name == name) {
				a->active = static_cast<int>(i);
				return;
			}
		}
		CombatArea area;
		area.name = std::move(name);
		a->areas.push_back(std::move(area));
		a->active = static_cast<int>(a->areas.size()) - 1;
	});
	bindMethod(console, "CombatArea.active", access, [a](std::string name) {
		for (size_t i = 0; i < a->areas.size(); ++i) {
			if (a->areas[i].name == name) {
				a->active = static_cast<int>(i);
			}
		}
	});
	bindMethod(console, "CombatArea.deleteActiveCombatArea", access, [a] {
		if (a->active >= 0 && static_cast<size_t>(a->active) < a->areas.size()) {
			a->areas.erase(a->areas.begin() + a->active);
		}
		a->active = -1;
	});
	{
		ConsoleMethod m;
		m.name = "CombatArea.getActiveCombatAreaName";
		m.access = access;
		m.invoke = [active](const std::vector<std::string>&) {
			const CombatArea* area = active();
			return area ? area->name : std::string();
		};
		console.registerMethod(m);
	}
	bindMethod(console, "CombatArea.min", access, [active](std::string v) {
		if (CombatArea* area = active()) {
			parseVec2(v, area->minX, area->minZ);
		}
	});
	bindMethod(console, "CombatArea.max", access, [active](std::string v) {
		if (CombatArea* area = active()) {
			parseVec2(v, area->maxX, area->maxZ);
		}
	});
	bindMethod(console, "CombatArea.team", access, [active](int team) {
		if (CombatArea* area = active()) {
			area->team = team;
		}
	});
	bindMethod(console, "CombatArea.vehicles", access, [active](int vehicles) {
		if (CombatArea* area = active()) {
			area->vehicles = vehicles;
		}
	});
	bindMethod(console, "CombatArea.layer", access, [active](int layer) {
		if (CombatArea* area = active()) {
			area->layer = layer;
		}
	});
	bindMethod(console, "CombatArea.usedByPathFinding", access, [active](int used) {
		if (CombatArea* area = active()) {
			area->usedByPathFinding = used != 0;
		}
	});
	bindMethod(console, "CombatArea.addAreaPoint", access, [active](std::string v) {
		float x = 0, z = 0;
		// 0x004da580: a point on the last one (within 1.19e-7 either way)
		// or, from two points on, on the first (0x004d9c30) is dropped; the
		// area is a polygon from its third point.
		CombatArea* area = active();
		if (!area || !parseVec2(v, x, z)) {
			return;
		}
		const auto same = [x, z](const std::pair<float, float>& p) {
			return std::abs(p.first - x) + std::abs(p.second - z) <= 1.1920929e-7f;
		};
		if ((!area->points.empty() && same(area->points.back())) || (area->points.size() >= 2 && same(area->points.front()))) {
			return;
		}
		area->points.push_back({x, z});
		area->polygon = area->points.size() >= 3;
	});
}

} // namespace dice::hfe::game
