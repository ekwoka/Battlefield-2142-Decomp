// The stance keys as the original soldier reads them (0x005889d0, each tick
// from 0x0058b4a2): a c_PILie press toggles prone (the press latched in
// +0x284 bit 2), while crouch lasts as long as c_PICrouch is held. A crouch
// press or a jump gets up from prone. Both keys are c_CMPushAndHold; the
// toggle is the soldier's, not the control map's. Free of SDL for the tests.
#pragma once

namespace dice::hfe {

struct ProneToggle {
	bool prone = false;
	bool lieWas = false, crouchWas = false, jumpWas = false;

	void update(bool lie, bool crouch, bool jump)
	{
		if (lie && !lieWas) {
			prone = !prone;
		} else if (prone && ((crouch && !crouchWas) || (jump && !jumpWas))) {
			prone = false;
		}
		lieWas = lie;
		crouchWas = crouch;
		jumpWas = jump;
	}
};

} // namespace dice::hfe
