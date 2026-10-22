// A soldier's movement from player input (Soldier.cpp: input handling
// 0x0058a6d0, ground movement 0x00588420, speed choice 0x00586020).
//
// Throttle and strafe inputs (-1..1) are shaped (sign(x) * x^2) and eased
// towards (0.1 of the way per tick while pressed, 0.2 when released). Their
// direction along the soldier's heading, at most 1 long, times the speed of
// the soldier's pose, is the velocity the physics response is asked for.
#pragma once

#include "Physics/SoldierPhysics.h"

namespace dice::hfe::game {

enum class SoldierPose { Stand = 0, Crouch = 1, Prone = 2, Swim = 3 };

struct SoldierInput {
	float throttle = 0; // PIThrottle: forward positive
	float strafe = 0;   // PIYaw on foot: right positive
	float turn = 0;     // degrees this tick (mouse look)
	bool sprint = false;
	SoldierPose pose = SoldierPose::Stand;
	bool fire = false;  // PIFire
	bool altFire = false; // PIAltFire: sets off the soldier's C4
	float aimPitch = 0; // degrees, up positive
	bool use = false;   // PIUse: enter or leave a vehicle (on press)
	int switchSeat = 0; // PIPosition1..8: move to that seat (1-based; on press)
	int selectItem = 0; // PIWeaponSelect1..9: the weapon in that kit slot (on press)
};

// Speeds in m/s by speed index (0x00a68f4c, filled from 0x00a689a8...).
float soldierSpeed(int index);
// The speed index of a pose (0x00586020).
int soldierSpeedIndex(SoldierPose pose, bool sprinting);

class SoldierControl {
public:
	void update(const SoldierInput& input, physics::SoldierBody& body);

	float throttle() const { return m_throttle; }
	float strafe() const { return m_strafe; }

private:
	float m_throttle = 0; // 0x264
	float m_strafe = 0;   // 0x260
};

} // namespace dice::hfe::game
