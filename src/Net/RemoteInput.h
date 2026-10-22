// The local game's soldier input as the player inputs a server expects
// (PlayerInput, GameProtocol.h). Header-only so the Net library doesn't
// link the game.
//
// Provisional: the original fills PlayerInput from the control map (the
// keys' and mouse's values per c_PI input) and the server's soldier turns
// by the mouse-look inputs; the scale of those (degrees per unit) is not
// worked out, so `turn` and `aimPitch` are passed as they are.
#pragma once

#include "Game/SoldierControl.h"
#include "Net/GameProtocol.h"

namespace dice::hfe::net {

inline PlayerInput playerInputFrom(const game::SoldierInput& in)
{
	PlayerInput out;
	out.set(kPIThrottle, in.throttle);
	out.set(kPIYaw, in.strafe); // on foot PIYaw strafes
	out.set(kPIMouseLookX, in.turn);
	out.set(kPIMouseLookY, in.aimPitch);
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
