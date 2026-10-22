// The local game's soldier input as the player inputs a server expects
// (PlayerInput, GameProtocol.h). Header-only so the Net library doesn't
// link the game.
//
// The original fills PlayerInput from the control maps (0x00696db0, run
// for each player map with the last winning): a mouse axis is the device's
// delta times the map's mouse sensitivity, with the aspect correction and
// Y invert (0x006802c0). The server's soldier turns 5 degrees per
// PIMouseLook unit and tick (0x0058a6d0: *0x00a68e4c and *0x00a68f24 are
// both 5.0), so the local turn and the change of pitch are sent in those
// units; up is negative.
#pragma once

#include "Game/SoldierControl.h"
#include "Net/GameProtocol.h"

namespace dice::hfe::net {

constexpr float kDegreesPerMouseLookUnit = 5.0f;

// `previousAimPitch`: the aim pitch the last move carried (degrees).
inline PlayerInput playerInputFrom(const game::SoldierInput& in, float previousAimPitch = 0)
{
	PlayerInput out;
	out.set(kPIThrottle, in.throttle);
	out.set(kPIYaw, in.strafe); // on foot PIYaw strafes
	out.set(kPIMouseLookX, in.turn / kDegreesPerMouseLookUnit);
	out.set(kPIMouseLookY, -(in.aimPitch - previousAimPitch) / kDegreesPerMouseLookUnit);
	out.set(kPISprint, in.sprint ? 1.0f : 0.0f);
	out.set(kPICrouch, in.pose == game::SoldierPose::Crouch ? 1.0f : 0.0f);
	out.set(kPILie, in.pose == game::SoldierPose::Prone ? 1.0f : 0.0f);
	out.set(kPIFire, in.fire ? 1.0f : 0.0f);
	out.set(kPIAltFire, in.altFire ? 1.0f : 0.0f);
	out.set(kPIUse, in.use ? 1.0f : 0.0f);
	if (in.switchSeat >= 1 && in.switchSeat <= 8) {
		out.set(kPIPositionSelect1 + in.switchSeat - 1, 1.0f);
	}
	if (in.selectItem >= 1 && in.selectItem <= 9) {
		out.set(kPIWeaponSelect1 + in.selectItem - 1, 1.0f);
	}
	return out;
}

} // namespace dice::hfe::net
