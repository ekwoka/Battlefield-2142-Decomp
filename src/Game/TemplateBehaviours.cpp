// Template behaviours with a game side of their own: a soldier's 3P hit
// reaction (BF2142.exe 0x005eda90; GameWorld::hitReaction) and the seats'
// guns nobody uses (ammo.reloadWithoutPlayer, 0x005f4bb0 / 0x005f3a40).
#include "GameWorld.h"

#include <cmath>

namespace dice::hfe::game {

const char* GameWorld::hitReaction(int node, const physics::Vec3& direction, float soldierYawDegrees)
{
	const float yaw = soldierYawDegrees * 3.14159265f / 180.0f;
	const physics::Vec3 forward {std::sin(yaw), 0, std::cos(yaw)};
	const bool back = physics::dot(direction, forward) >= 0.0f;
	switch (node) {
	case 0xf:
	case 0x11:
		return back ? "hitBackLeft" : "hitFrontLeft";
	case 0x1f:
	case 0x21:
		return back ? "hitBackRight" : "hitFrontRight";
	case 0x2f:
		return back ? "hitBackHead" : "hitFrontHead";
	default:
		return back ? "hitBackBody" : "hitFrontBody";
	}
}

void GameWorld::playHitReaction(Player& victim, int node, const physics::Vec3& direction)
{
	// 0x005eda90: a living soldier on foot (+0x28c, +0x34) with its 3P
	// animation system (+0x2f0 == 1), above 0 hit points after the hit.
	if (!victim.alive || victim.vehicle != 0 || !victim.bodyAnimation || hitPoints(victim.object) <= 0) {
		return;
	}
	victim.bodyAnimation->play(hitReaction(node, direction, victim.body.yawDegrees));
}

void GameWorld::stepUnmannedGuns(float seconds)
{
	for (Body& body : m_bodies) {
		for (size_t seat = 0; seat < body.guns.size(); ++seat) {
			Body::Gun& gun = body.guns[seat];
			const bool manned = seat < body.occupants.size() && body.occupants[seat] >= 0;
			if (!gun.weapon || manned) {
				gun.manned = manned;
				continue;
			}
			if (gun.weapon->settings().reloadWithoutPlayer) {
				gun.weapon->runReload(seconds); // 0x005f4bb0: the reload runs on
			} else if (gun.manned) {
				gun.weapon->dropReload(); // 0x005f3a40: its user gone
			}
			gun.manned = false;
		}
	}
}

} // namespace dice::hfe::game
