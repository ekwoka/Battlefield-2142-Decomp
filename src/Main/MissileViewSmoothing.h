#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace dice::hfe {
// Render between fixed simulation samples; a new projectile starts its own
// history so a launch or impact cannot blend with a different camera.
class MissileViewSmoothing {
public:
	struct Pose { std::array<float, 3> position {}; float yaw = 0, pitch = 0; };
	Pose sample(uint32_t serial, bool ticked, float alpha, const Pose& current)
	{
		if (!serial) { m_serial = 0; return current; }
		if (serial != m_serial) { m_serial = serial; m_previous = m_current = current; }
		else if (ticked) { m_previous = m_current; m_current = current; }
		alpha = std::clamp(alpha, 0.0f, 1.0f);
		Pose result;
		for (int i = 0; i < 3; ++i) result.position[i] = m_previous.position[i] + alpha * (m_current.position[i] - m_previous.position[i]);
		result.yaw = m_previous.yaw + alpha * std::remainder(m_current.yaw - m_previous.yaw, 6.28318530718f);
		result.pitch = m_previous.pitch + alpha * (m_current.pitch - m_previous.pitch);
		return result;
	}
private:
	uint32_t m_serial = 0;
	Pose m_previous, m_current;
};
}
