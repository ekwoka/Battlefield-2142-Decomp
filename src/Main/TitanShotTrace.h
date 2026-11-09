#pragma once

#include "Game/GameWorld.h"
#include "World/Object.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace dice::hfe::host {

// Bounded local diagnostics for a walker AA round near its own Titan.
class TitanShotTrace {
public:
	void update(const game::GameWorld& round, const world::ObjectManager& objects, std::FILE* out = stdout)
	{
			std::set<uint32_t> active;
			const auto titans = round.titans();
			for (const game::Projectile& p : round.projectiles()) {
				if (!p.alive || p.visual || p.player != round.localPlayer() || !p.settings) continue;
				const bool missile = p.settings->name == "us_heavy_mech_AA_missile";
				const bool cannon = p.settings->name == "us_heavy_mech_AAcannon_Projectile";
				if (!missile && !cannon) continue;
				auto it = m_tracked.find(p.serial);
				if (it == m_tracked.end()) {
					const bool nearTitan = std::any_of(titans.begin(), titans.end(), [&](const auto& t) {
						const auto d = p.position - t.position;
						const float reach = t.radius + 180.0f;
						return t.team == p.team && physics::lengthSquared(d) < reach * reach;
					});
					if (!nearTitan) continue;
					int& count = missile ? m_missiles : m_cannons;
					if (count >= 12) continue;
					++count;
					it = m_tracked.emplace(p.serial, p.position).first;
					std::fprintf(out, "titan-shot: start serial=%u weapon=%s team=%d origin=%.3f,%.3f,%.3f offset=%.3f,%.3f,%.3f\n",
						p.serial, p.settings->name.c_str(), p.team, p.origin.x, p.origin.y, p.origin.z,
						p.settings->collisionOffset.x, p.settings->collisionOffset.y, p.settings->collisionOffset.z);
				}
				active.insert(p.serial);
				for (const auto& titan : titans) {
					if (titan.team != p.team) continue;
					const world::Object* root = objects.find(titan.object);
					const world::Vec3 turn = root ? root->rotation() : world::Vec3 {};
					std::fprintf(out, "titan-shot: step serial=%u age=%.3f pos=%.3f,%.3f,%.3f vel=%.3f,%.3f,%.3f titan=%u at=%.3f,%.3f,%.3f rot=%.3f,%.3f,%.3f shield=%d mesh=%d\n",
						p.serial, p.age, p.position.x, p.position.y, p.position.z, p.velocity.x, p.velocity.y, p.velocity.z,
						titan.object, titan.position.x, titan.position.y, titan.position.z, turn.x, turn.y, turn.z, titan.shield, round.projectileCollision(titan.object) != nullptr);
				}
				it->second = p.position;
			}
			for (auto it = m_tracked.begin(); it != m_tracked.end();) {
				if (active.count(it->first)) { ++it; continue; }
				std::fprintf(out, "titan-shot: end serial=%u last=%.3f,%.3f,%.3f\n", it->first, it->second.x, it->second.y, it->second.z);
				const auto& impacts = round.impacts();
				const size_t first = impacts.size() > 4 ? impacts.size() - 4 : 0;
				for (size_t i = first; i < impacts.size(); ++i) {
					const auto& hit = impacts[i];
					std::fprintf(out, "titan-shot: recent-impact object=%u material=%u at=%.3f,%.3f,%.3f\n",
						hit.object, hit.material, hit.point.x, hit.point.y, hit.point.z);
				}
				it = m_tracked.erase(it);
			}
	}

private:
	std::map<uint32_t, physics::Vec3> m_tracked;
	int m_cannons = 0, m_missiles = 0;
};

} // namespace dice::hfe::host
