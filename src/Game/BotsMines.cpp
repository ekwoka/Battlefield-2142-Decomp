// The mines on the bots' navigation meshes (AIDLL.dll's IAIMineManager;
// the zones and marks are src/AI/MineZones). Every mine of both teams is
// registered as the game makes it (BF2142.exe's AI event adaptor 0x0089f8d0
// / 0x0089fdd0 -> slot 0x14 0x100496f0: a projectile whose detonation's
// template +0x5d holds; the rewrite takes the trigger-armed mines, a
// trigger type and radius, not the remote C4) and taken off as it goes
// (slot 0x10 0x1004b880). No team, no sighting: every bot's mesh gets them.
#include "Bots.h"

#include "AI/MineZones.h"

#include <cmath>

namespace dice::hfe::game {

namespace {

ai::Vec3 aiAt(const physics::Vec3& v) { return {v.x, v.y, v.z}; }

} // namespace

// Slot 3 (0x1004b640): every 0.5 s the waiting mines that rest (the object's
// physics node at rest, IAIObjectEnvironment slot 0x70 -> BF2142.exe
// 0x00897c10) get their zones (0x1004ae90); gone ones are taken off.
void BotManager::updateMines()
{
	if (m_time < m_minesNext) {
		return;
	}
	m_minesNext = m_time + 0.5f;
	std::set<uint32_t> present;
	for (const Projectile& p : m_world.projectiles()) {
		if (!p.alive || !p.settings || p.settings->remote || p.settings->triggerType == 0 || p.settings->triggerRadius <= 0) {
			continue;
		}
		present.insert(p.serial);
		MineZone& zone = m_mines[p.serial]; // an id already held isn't added again
		if (zone.active || !p.resting) {
			continue;
		}
		zone.active = true;
		zone.type = p.settings->triggerType == 2 ? 1 : 0; // MTYVehicle: the vehicles' zone
		const float yaw = p.yaw * 3.14159265f / 180;
		const physics::Vec3 forward {std::sin(yaw), 0, std::cos(yaw)}, right {std::cos(yaw), 0, -std::sin(yaw)};
		const auto start = [&](const ai::NavMesh& mesh, int type, float radius) {
			int32_t polygon = mesh.findPolygon(aiAt(p.position));
			if (type == 0) {
				// 0x10046a80: along the facing, a metre at a time.
				for (int k = 1; k <= 6 && polygon < 0; ++k) {
					polygon = mesh.findPolygon(aiAt(p.position + forward * static_cast<float>(k)));
				}
			} else {
				// 0x10046b10: 15 tries at U * 2pi round, 2 + (R - 2) U out.
				for (int k = 0; k < 15 && polygon < 0; ++k) {
					const float a = random() * 6.2831853f, d = 2 + (radius - 2) * random();
					polygon = mesh.findPolygon(aiAt(p.position + physics::Vec3 {std::sin(a), 0, std::cos(a)} * d));
				}
			}
			return polygon;
		};
		const auto markAll = [&](const ai::NavMesh* mesh, int meshIndex, const std::vector<uint32_t>& polygons, bool counterB) {
			for (uint32_t polygon : polygons) {
				m_mineMarks.mark(const_cast<ai::NavMesh&>(*mesh), meshIndex, polygon, counterB);
				zone.marks.push_back({meshIndex, polygon, counterB});
			}
		};
		if (zone.type == 0 && m_navMesh) {
			// The cone (0x10048160): 7.5 m, 22.5 degrees either side.
			const float a = 22.5f * 3.14159265f / 180;
			const physics::Vec3 p1 = p.position + (forward * std::cos(a) + right * std::sin(a)) * 7.5f;
			const physics::Vec3 p2 = p.position + (forward * std::cos(a) - right * std::sin(a)) * 7.5f;
			const int32_t polygon = start(*m_navMesh, 0, 7.5f);
			if (polygon >= 0) {
				markAll(m_navMesh, 0, ai::floodTriangle(*m_navMesh, static_cast<uint32_t>(polygon), aiAt(p.position), aiAt(p1), aiAt(p2)), false);
			}
		} else if (zone.type == 1) {
			// 9 m round, on the vehicle mesh (A), then on the infantry
			// mesh (B).
			if (m_vehicleNavMesh) {
				const int32_t polygon = start(*m_vehicleNavMesh, 1, 9.0f);
				if (polygon >= 0) {
					markAll(m_vehicleNavMesh, 1, ai::floodCircle(*m_vehicleNavMesh, static_cast<uint32_t>(polygon), aiAt(p.position), 9.0f), false);
				}
			}
			if (m_navMesh) {
				const int32_t polygon = start(*m_navMesh, 1, 9.0f);
				if (polygon >= 0) {
					markAll(m_navMesh, 0, ai::floodCircle(*m_navMesh, static_cast<uint32_t>(polygon), aiAt(p.position), 9.0f), true);
				}
			}
		}
	}
	for (auto it = m_mines.begin(); it != m_mines.end();) {
		if (present.count(it->first)) {
			++it;
			continue;
		}
		for (const MineZone::Mark& m : it->second.marks) {
			m_mineMarks.unmark(m.mesh, m.polygon, m.counterB);
		}
		it = m_mines.erase(it);
	}
}

bool BotManager::mineMarked(int mesh, uint32_t polygon) const
{
	return m_mineMarks.marked(mesh, polygon);
}

} // namespace dice::hfe::game
