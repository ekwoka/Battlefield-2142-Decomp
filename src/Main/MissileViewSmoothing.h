#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace dice::hfe {
// The drawn camera between fixed simulation samples. A new serial (a
// projectile, or another kind of view) starts its own
// history so a launch, an impact or a switch of view cannot blend with a
// different camera. With a jump limit (the objects' 4 m height rule,
// 0x0043ef30 with *0x00920ca0, Render/TickBlend.h) a bigger height change is
// not blended. The alpha is retail's (0x0043f170: -0.1 .. 1.1).
class MissileViewSmoothing {
public:
	struct Pose { std::array<float, 3> position {}; float yaw = 0, pitch = 0, roll = 0; };
	Pose sample(uint32_t serial, bool ticked, float alpha, const Pose& current, float jump = 0)
	{
		if (!serial) { m_serial = 0; return current; }
		if (serial != m_serial) { m_serial = serial; m_previous = m_current = current; }
		else if (ticked) { m_previous = m_current; m_current = current; }
		alpha = std::clamp(alpha, -0.1f, 1.1f);
		if (jump > 0 && std::fabs(m_current.position[1] - m_previous.position[1]) > jump) return m_current;
		Pose result;
		for (int i = 0; i < 3; ++i) result.position[i] = m_previous.position[i] + alpha * (m_current.position[i] - m_previous.position[i]);
		result.yaw = m_previous.yaw + alpha * std::remainder(m_current.yaw - m_previous.yaw, 6.28318530718f);
		result.pitch = m_previous.pitch + alpha * (m_current.pitch - m_previous.pitch);
		result.roll = m_previous.roll + alpha * std::remainder(m_current.roll - m_previous.roll, 6.28318530718f);
		return result;
	}
private:
	uint32_t m_serial = 0;
	Pose m_previous, m_current;
};
}
