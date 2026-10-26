#include "CombatAreas.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
	const auto active = [a]() -> CombatArea* { return a->areas.empty() ? nullptr : &a->areas.back(); };
	bindMethod(console, "CombatAreaManager.use", access, [a](int use) { a->use = use != 0; });
	bindMethod(console, "CombatArea.create", access, [a](std::string name) {
		CombatArea area;
		area.name = std::move(name);
		a->areas.push_back(std::move(area));
	});
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
		if (CombatArea* area = active(); area && parseVec2(v, x, z)) {
			area->polygon = true;
			area->points.push_back({x, z});
		}
	});
}

} // namespace dice::hfe::game
