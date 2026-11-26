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
	struct Pose { std::array<float, 3> position {}; float yaw = 0, pitch = 0, roll = 0, eye = 0; };
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
		result.eye = m_previous.eye + alpha * (m_current.eye - m_previous.eye);
		result.roll = m_previous.roll + alpha * std::remainder(m_current.roll - m_previous.roll, 6.28318530718f);
		return result;
	}
private:
	uint32_t m_serial = 0;
	Pose m_previous, m_current;
};

// The soldier's own eye (0x0043f170's local player). What is smoothed is
// its carrier: the camera less the eye's height, turned by the carrier's
// yaw (on foot the soldier's, seated the seat's), the eye's height kept
// alongside.
inline MissileViewSmoothing::Pose soldierEyeCarrier(MissileViewSmoothing::Pose camera, float eyeHeight, float carrierYaw)
{
	camera.position[1] -= eyeHeight;
	camera.yaw = carrierYaw;
	camera.eye = eyeHeight;
	return camera;
}

// The drawn eye from the smoothed carrier: the eye's height between the
// ticks, then a step on towards the pose's (0x005890a0, 0.2 a call,
// *0x00a68e14), undone after the frame (0x0043f610) like every blended
// matrix; the carrier's blended turn in place of its tick's; the look the
// frame's, the tick's plus what the next tick will take (0x0058b5e0 ->
// 0x005890a0), the pitch within 85 degrees. Radians, the look in degrees.
struct SoldierEyeView {
	float y = 0, yaw = 0, pitch = 0;
};
inline SoldierEyeView composeSoldierEye(const MissileViewSmoothing::Pose& smooth, float cameraYaw, float cameraPitch,
	float carrierYaw, float eyeTarget, float lookTurnDegrees, float lookPitchDegrees)
{
	constexpr float kRadians = 3.14159265f / 180.0f;
	SoldierEyeView eye;
	eye.y = smooth.position[1];
	eye.y += smooth.eye + (eyeTarget - smooth.eye) * 0.2f;
	eye.yaw = cameraYaw;
	eye.yaw += smooth.yaw - carrierYaw + lookTurnDegrees * kRadians;
	eye.pitch = std::clamp(cameraPitch + lookPitchDegrees * kRadians, -85 * kRadians, 85 * kRadians);
	return eye;
}
}
