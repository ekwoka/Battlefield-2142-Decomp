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
#include "Game/StanceKeys.h"
#include "Net/GameProtocol.h"

#include <algorithm>

namespace dice::hfe::net {

constexpr float kDegreesPerMouseLookUnit = 5.0f;

// `previousAimPitch`: the aim pitch the last move carried (degrees).
inline PlayerInput playerInputFrom(const game::SoldierInput& in, float previousAimPitch = 0)
{
	PlayerInput out;
	out.set(kPIThrottle, in.throttle);
	out.set(kPIYaw, in.strafe); // on foot PIYaw strafes
	out.set(kPIPitch, in.pitchAxis); // a driver's keys and sticks (the mouse's go as PIMouseLook)
	out.set(kPIRoll, in.rollAxis);
	out.set(kPIMouseLookX, in.turn / kDegreesPerMouseLookUnit);
	out.set(kPIMouseLookY, -(in.aimPitch - previousAimPitch) / kDegreesPerMouseLookUnit);
	out.set(kPISprint, in.sprint ? 1.0f : 0.0f);
	// The raw keys, as retail sends them: the server toggles prone from the lie key (0x005889d0).
	out.set(kPICrouch, in.pose == game::SoldierPose::Crouch ? 1.0f : 0.0f);
	out.set(kPILie, in.lieKey ? 1.0f : 0.0f);
	out.set(kPIFire, in.fire ? 1.0f : 0.0f);
	out.set(kPIAltFire, in.altFire ? 1.0f : 0.0f);
	out.set(kPIFlareFire, in.flareFire ? 1.0f : 0.0f);
	out.set(kPIToggleWeapon, in.toggleWeapon ? 1.0f : 0.0f);
	out.set(kPIUse, in.use ? 1.0f : 0.0f);
	out.set(kPIToggleFireRate, in.toggleFireRate ? 1.0f : 0.0f);
	out.set(kPIReload, in.reload ? 1.0f : 0.0f);
	out.set(kPIMouseLook, in.mouseLook ? 1.0f : 0.0f);
	out.set(kPIAction, in.jump || in.jumpKey ? 1.0f : 0.0f);
	out.set(kPINextItem, in.itemStep > 0 ? 1.0f : 0.0f);
	out.set(kPIPrevItem, in.itemStep < 0 ? 1.0f : 0.0f);
	if (in.switchSeat >= 1 && in.switchSeat <= 8) {
		out.set(kPIPositionSelect1 + in.switchSeat - 1, 1.0f);
	}
	if (in.selectItem >= 1 && in.selectItem <= 9) {
		out.set(kPIWeaponSelect1 + in.selectItem - 1, 1.0f);
	}
	return out;
}

// What one move asks of a soldier, as the server takes it: 0x005838d0
// copies the move into the player's PlayerInput, and the soldier's input
// handler (0x0058a6d0) turns it 5 degrees a PIMouseLook unit, the throttle
// and PIYaw move it, the triggers act on a press. The server (HostedGame)
// and the client's prediction (Prediction.h) both go through this, so the
// prediction steps exactly what the server steps. `state` carries what
// lasts from move to move: the aim's pitch, and the keys held (presses
// act once).
struct MoveInputState {
	float pitch = 0;
	// In a vehicle's seat (set by the caller before each move: the server
	// from the player's vehicle, the vehicle prediction always): the aim's
	// pitch then runs on past the soldier's +-85.
	bool seated = false;
	bool useWas = false;
	int seatWas = 0, itemWas = 0;
	// The stance keys' state (StanceKeys.h): the caller sets `stance.prone` to the soldier's
	// last pose before each move, as a refused press is spent.
	ProneToggle stance;
};
inline game::SoldierInput soldierInputFromMove(const PlayerMove& move, MoveInputState& state)
{
	game::SoldierInput input;
	input.throttle = move.axis(kPIThrottle);
	input.strafe = move.axis(kPIYaw);
	input.pitchAxis = move.axis(kPIPitch);
	input.rollAxis = move.axis(kPIRoll);
	input.turn = move.axis(kPIMouseLookX) * kDegreesPerMouseLookUnit;
	// On foot the soldier's +-85 (game::kSoldierLookPitchLimit, 0x005890a0
	// clamping the pitch +0x270 the input handling 0x0058a6d0 adds the
	// PIMouseLookY change to), so looking past the top or bottom isn't kept
	// to be undone. In a seat the original keeps no pitch of the soldier's:
	// the seat's bundles take the PIMouseLookY change itself and clamp their
	// own rotation (rotationAxisStep). Here a driver's PIPitch and the
	// seat's bundles take this value's change, so it runs on (a clamp pinned
	// at one end swallows the mouse that way); the client's look (Viewer,
	// driving: +-1000 rad, about +-57000 degrees) is the same sum. Whatever
	// aims along it bounds it first (game::soldierLookPitch).
	const float limit = state.seated ? 57000.0f : game::kSoldierLookPitchLimit;
	state.pitch = std::clamp(state.pitch - move.axis(kPIMouseLookY) * kDegreesPerMouseLookUnit, -limit, limit);
	input.aimPitch = state.pitch;
	input.sprint = move.trigger(kPISprint);
	input.fire = move.trigger(kPIFire);
	input.altFire = move.trigger(kPIAltFire);
	input.flareFire = move.trigger(kPIFlareFire);
	input.toggleWeapon = move.trigger(kPIToggleWeapon); // held: the world acts on the press
	input.toggleFireRate = move.trigger(kPIToggleFireRate); // held: the world acts on the press
	input.reload = move.trigger(kPIReload);                 // the same
	input.mouseLook = move.trigger(kPIMouseLook);           // held (0x00611c80 tests its bit 0x800 and > 0.5)
	input.itemStep = move.trigger(kPINextItem) ? 1 : move.trigger(kPIPrevItem) ? -1 : 0; // the wheel, each move
	// The stance as the server's soldier takes it (0x005889d0): a lie press toggles prone, a
	// crouch press or the jump key gets up (that jump isn't one).
	const bool jumpKey = move.trigger(kPIAction), lieKey = move.trigger(kPILie), crouch = move.trigger(kPICrouch);
	if (state.seated) {
		state.stance.prone = false;
	}
	const bool wasProne = state.stance.prone;
	state.stance.update(lieKey, crouch, jumpKey);
	input.jumpKey = jumpKey;
	input.lieKey = lieKey;
	input.jump = jumpKey && !wasProne && !state.stance.prone;
	input.pose = state.stance.prone ? game::SoldierPose::Prone : crouch ? game::SoldierPose::Crouch : game::SoldierPose::Stand;
	const bool use = move.trigger(kPIUse);
	input.use = use && !state.useWas;
	state.useWas = use;
	int seat = 0, item = 0;
	for (int i = 0; i < 8; ++i) {
		seat = move.trigger(kPIPositionSelect1 + i) ? i + 1 : seat;
	}
	for (int i = 0; i < 9; ++i) {
		item = move.trigger(kPIWeaponSelect1 + i) ? i + 1 : item;
	}
	if (seat && seat != state.seatWas) {
		input.switchSeat = seat;
	}
	if (item && item != state.itemWas) {
		input.selectItem = item;
	}
	state.seatWas = seat;
	state.itemWas = item;
	return input;
}

} // namespace dice::hfe::net
