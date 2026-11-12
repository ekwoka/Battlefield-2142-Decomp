// Steering a seat's gun through the look, as a person does. A person's look
// drives the seat's bundles at their own speeds (GameWorld::stepGunner,
// 0x00563bd0: input x maxSpeed reached at `acceleration`), so a gun does not
// jump to an absolute look; this leads it there in proportional steps until
// it settles, and also converges for a bot's degrees-a-tick seat.
#pragma once

#include "Game/SoldierControl.h"

#include <cmath>

namespace seataim {

// `tick(input)` applies one tick of input; `aim(yaw, pitch)` reads the gun
// (GameWorld::gunAim). `look` is the running aimPitch, carried on. A NAN
// target leaves that axis be. Returns the ticks used.
template <class Tick, class Aim>
int steerGun(Tick tick, Aim aim, float& look, float yaw, float pitch, int ticks = 240)
{
	int used = 0;
	for (int quiet = 0; used < ticks && quiet < 15; ++used) {
		float y = 0, p = 0;
		aim(y, p);
		const float dy = std::isnan(yaw) ? 0.0f : std::remainder(yaw - y, 360.0f);
		const float dp = std::isnan(pitch) ? 0.0f : pitch - p;
		dice::hfe::game::SoldierInput in;
		in.turn = 0.5f * dy;
		look += 0.5f * dp;
		in.aimPitch = look;
		tick(in);
		// Settled: no change worth a step (a limit holds it short).
		float y1 = 0, p1 = 0;
		aim(y1, p1);
		quiet = std::fabs(std::remainder(y1 - y, 360.0f)) < 0.005f && std::fabs(p1 - p) < 0.005f ? quiet + 1 : 0;
	}
	return used;
}

} // namespace seataim
