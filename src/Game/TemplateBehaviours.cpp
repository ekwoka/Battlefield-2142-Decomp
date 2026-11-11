// Template behaviours with a game side of their own: a soldier's 3P hit
// reaction (BF2142.exe 0x005eda90; GameWorld::hitReaction) and the seats'
// guns nobody uses (ammo.reloadWithoutPlayer, 0x005f4bb0 / 0x005f3a40),
// and the radio messages' emotes (0x005edbf0, allowEmoteAnimation).
#include "GameWorld.h"

#include "World/ObjectTemplateManager.h"

#include <cmath>
#include <cstdlib>

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

bool GameWorld::emoteAllowed(const std::string& emote, bool sprinting, SoldierPose pose,
	const world::ObjectTemplate* inHand)
{
	// 0x005edbf0: the name's length (+0x14), the sprint (slot 0x100 of the
	// +0x184 interface, 0x00585e80), the pose (+0xdc of the +0x18c
	// interface: soldier +0x268, 2 prone), then the item in hand
	// (0x005896a0: none for index 0) by its template's class and
	// allowEmoteAnimation.
	if (emote.empty() || sprinting || pose == SoldierPose::Prone) {
		return false;
	}
	if (inHand && inHand->typeName() == "GenericFireArm") {
		const auto allow = inHand->settings().find("allowEmoteAnimation");
		return allow != inHand->settings().end() && std::atoi(allow->second.c_str()) != 0;
	}
	return true;
}

bool GameWorld::playEmote(int player, const std::string& emote)
{
	Player* sender = findPlayer(player);
	// 0x0042e0c0: the sender's controlled object a soldier (class 0x9493:
	// not seated, not in a ladder's container); 0x005edbf0: its 3P system.
	if (!sender || !sender->alive || sender->manDown || sender->vehicle != 0 || sender->ladder >= 0
		|| !sender->bodyAnimation) {
		return false;
	}
	const world::ObjectTemplate* inHand = sender->weaponTemplate.empty() ? nullptr : m_templates.find(sender->weaponTemplate);
	if (!emoteAllowed(emote, sender->control.stamina().sprinting(), sender->input.pose, inHand)) {
		return false;
	}
	return sender->bodyAnimation->play(emote); // slot 0x1c, 0x008b97f0
}

bool GameWorld::playRemoteEmote(uint32_t object, const std::string& emote, bool sprinting)
{
	const auto found = m_remoteSoldiers.find(object);
	if (found == m_remoteSoldiers.end() || !found->second.body || found->second.seat) {
		return false;
	}
	RemoteSoldier& remote = found->second;
	const world::ObjectTemplate* inHand = remote.weaponName.empty() ? nullptr : m_templates.find(remote.weaponName);
	if (!emoteAllowed(emote, sprinting, remote.posture, inHand)) {
		return false;
	}
	return remote.body->play(emote);
}

void GameWorld::stepUnmannedGuns(float seconds)
{
	for (Body& body : m_bodies) {
		// The active defence's shield lasts the ADS weapon's fire state 4
		// (raiseShield); out of it the root's shield is cleared.
		body.shieldLeft = std::max(0.0f, body.shieldLeft - seconds);
		for (size_t seat = 0; seat < body.guns.size(); ++seat) {
			Body::Gun& gun = body.guns[seat];
			const bool manned = seat < body.occupants.size() && body.occupants[seat] >= 0;
			if (manned) {
				gun.manned = manned;
				continue;
			}
			// Each of the seat's firearms (the active one in the Gun's Arm part).
			const auto unmanned = [&](FireArm* weapon) {
				if (!weapon) {
					return;
				}
				if (weapon->settings().reloadWithoutPlayer) {
					weapon->runReload(seconds); // 0x005f4bb0: the reload runs on
				} else if (gun.manned) {
					weapon->dropReload(); // 0x005f3a40: its user gone
				}
			};
			unmanned(gun.weapon.get());
			for (Body::Arm& arm : gun.arms) {
				unmanned(arm.weapon.get());
			}
			gun.manned = false;
		}
	}
}

} // namespace dice::hfe::game
