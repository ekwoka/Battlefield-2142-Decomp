#include "GhostPriority.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace dice::hfe::net {

float ghostScopeRadius(float viewDistance) { return std::max(viewDistance, 500.0f); }

float ghostScore(const GhostEye& eye, const GhostVec3& position, const GhostVec3& velocity, bool largeProjectile)
{
	const float radius = largeProjectile ? 1000.0f : eye.radius; // LargerProjectileRadius
	const float dx = position.x - eye.position.x, dy = position.y - eye.position.y, dz = position.z - eye.position.z;
	const float d2 = dx * dx + dy * dy + dz * dz;
	if (d2 >= radius * radius) {
		return 0;
	}
	float d = std::sqrt(d2);
	if (d < FLT_EPSILON) {
		d = 0.001f;
	}
	const float speed2 = velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z;
	const float moving = std::min(1.0f, speed2 * 0.000625f);
	const float facing = (dx * eye.forward.x + dy * eye.forward.y + dz * eye.forward.z) / d;
	const float inView = facing >= eye.fov * 0.5f * 1.3333334f ? 1.0f : 0.0f;
	return moving + inView + (1 - d / radius);
}

void GhostScheduler::setPriority(uint16_t ghost, float p, bool keepMax)
{
	p = std::min(p, kGhostNewPriority);
	const auto it = m_states.find(ghost);
	if (it == m_states.end()) {
		State s;
		s.always = p < 0;
		s.priority = kGhostNewPriority;
		m_states[ghost] = s;
		return;
	}
	State& s = it->second;
	s.status = Status::InScope;
	if (p < 0) {
		s.always = true;
		s.dirty = true;
		s.priority = kGhostAlwaysPriority;
		return;
	}
	if (keepMax && s.priority < kGhostNewPriority && s.priority >= p) {
		return;
	}
	s.priority = p;
}

void GhostScheduler::beginRescore()
{
	for (auto& [ghost, s] : m_states) {
		if (s.always) {
			s.status = Status::InScope;
			s.priority = kGhostAlwaysPriority;
		} else {
			s.status = Status::OutOfScope;
			s.priority = kGhostNewPriority;
		}
	}
}

void GhostScheduler::markDirty(uint16_t ghost, bool urgent)
{
	const auto it = m_states.find(ghost);
	if (it == m_states.end()) {
		return;
	}
	it->second.dirty = true;
	if (urgent) {
		it->second.priority = kGhostNewPriority;
	}
}

void GhostScheduler::forget(uint16_t ghost) { m_states.erase(ghost); }

std::vector<uint16_t> GhostScheduler::queue()
{
	std::vector<std::pair<float, uint16_t>> heap;
	for (auto& [ghost, s] : m_states) {
		if (s.status != Status::InScope || s.dirty) {
			s.accumulated += s.priority;
			heap.emplace_back(s.accumulated, ghost);
		}
	}
	// Highest first; ties by ghost id, so the order is stable.
	std::sort(heap.begin(), heap.end(), [](const auto& a, const auto& b) {
		return a.first != b.first ? a.first > b.first : a.second < b.second;
	});
	std::vector<uint16_t> out;
	out.reserve(heap.size());
	for (const auto& [p, ghost] : heap) {
		out.push_back(ghost);
	}
	return out;
}

void GhostScheduler::written(uint16_t ghost)
{
	const auto it = m_states.find(ghost);
	if (it == m_states.end()) {
		return;
	}
	if (it->second.status == Status::OutOfScope) {
		m_states.erase(it); // 0x00581dc0
		return;
	}
	it->second.accumulated = 0;
	it->second.dirty = false;
}

const GhostScheduler::State* GhostScheduler::state(uint16_t ghost) const
{
	const auto it = m_states.find(ghost);
	return it != m_states.end() ? &it->second : nullptr;
}

} // namespace dice::hfe::net
