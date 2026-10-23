// A soldier's movement from player input (Soldier.cpp: input handling
// 0x0058a6d0, ground and air movement 0x00588420, speed choice 0x00586020,
// the jump in 0x0058a6d0, the sprint's stamina 0x006457f0).
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
	bool jump = false;  // PIAction (input 9, c_PIAction: Space), held: jumps on landing
};

// The sprint's stamina (the Soldier's +0x360; set up by 0x00645790 from
// the soldier template's sprintDissipationTime (+0x300), sprintRecoverTime
// (+0x2fc) and sprintLimit (+0x304) and the player's ability modifiers).
class SprintStamina {
public:
	void configure(float dissipationTime, float recoverTime, float limit, float useModifier = 1,
		float rechargeModifier = 1);
	bool configured() const { return m_configured; }
	// 0x00645760 (the sprint key's events): asks to sprint; `held` (event
	// 0x29) only keeps a sprint going.
	void request(bool held);
	// 0x006457f0: sprinting, the stamina drains by the time over the
	// dissipation time (x the use modifier) and the sprint ends when it is
	// out, not asked for any more or `blocked`; otherwise it recovers by the
	// time over the recovery time (x the recharge modifier; full at once
	// without one) unless `recoverDelay` is still running, and a sprint
	// asked for starts at the limit or more. Without a dissipation time it
	// never sprints.
	void update(bool blocked, float recoverDelay, float seconds);
	// 0x006c54e0 with the jump's max(0, stamina - sprintLossAtJump).
	void lose(float amount) { m_stamina = amount < m_stamina ? m_stamina - amount : 0.0f; }
	bool sprinting() const { return m_sprinting; }
	float stamina() const { return m_stamina; }

private:
	bool m_configured = false;
	float m_dissipationTime = 0; // +0x0
	float m_recoverTime = 0;     // +0x4
	float m_limit = 0;           // +0x8
	float m_useModifier = 1;     // +0xc
	float m_rechargeModifier = 1; // +0x10
	float m_drain = 1;           // +0x14
	float m_stamina = 1;         // +0x18
	bool m_requested = false;    // +0x1c
	bool m_sprinting = false;    // +0x1e
};

// The jump's tuning (0x0058a6d0; static_init 0x008d.../0x008e...).
namespace jump {
constexpr float kUpSpeed = 1.0f * 6.0f;    // *0x00a68f18 x 6: m/s up
constexpr float kCarry = 0.98f;            // *0x00a68f14: the ground speed kept (at most the pose's)
constexpr float kAirTime = 2.0f;           // soldier +0x2e4: the air control's time
constexpr float kSlopeDelay = 0.5f;        // +0x2e0 from ground steeper than kSteep
constexpr float kSteep = 0.8f;             // the ground normal's y (0x00965e44)
constexpr float kNoProneTime = 0.2f;       // +0x3f8 (*0x00a68ee8): no lying down
constexpr float kJumpTime = 1.0f;          // +0x3f4 (*0x00a68eec)
constexpr float kRecoverDelay = 1.0f;      // +0x408 (*0x00a68ed0): the stamina waits
constexpr float kGetUpTime = 1.0f;         // +0x3fc (*0x00a68e30): no jump after getting up from prone
constexpr float kAirSpeed = 2.0f;          // *0x00a68eb4: the air control's speed x time left x 0.5
} // namespace jump

// Speeds in m/s by speed index (0x00a68f4c, filled from 0x00a689a8...).
float soldierSpeed(int index);
// The speed index of a pose (0x00586020).
int soldierSpeedIndex(SoldierPose pose, bool sprinting);

// The turn: the body (the soldier object's yaw, +0x25c) follows the look
// with a lag: the look is the body plus the head's twist (+0x278); see
// update. Firing, the camera and what the soldier sets down go along the
// look; walking along the body.
class SoldierControl {
public:
	// One tick of input (0x0058a6d0). A jump (PIAction) needs the soldier
	// standing (pose 0), on the ground (the response's +0xf9), not
	// swimming and no get-up time left (+0x3fc, a second after rising from
	// prone): the velocity along the ground x 0.98 (at most the pose's
	// speed x 0.98) and 6 m/s up; the stamina loses the template's
	// sprintLossAtJump (`jumpLoss`) and waits a second to recover. In the
	// air, for two seconds after a jump (half a second later from ground
	// steeper than 0.8), the input steers (0x00588420 with its second
	// argument set).
	void update(const SoldierInput& input, physics::SoldierBody& body, float seconds = 1 / 30.0f);
	SprintStamina& stamina() { return m_stamina; }
	const SprintStamina& stamina() const { return m_stamina; }
	float jumpLoss = 0;     // the soldier template's sprintLossAtJump (+0x308)
	bool jumped() const { return m_jumped; } // this tick
	float airTime() const { return m_airTime; }

	float throttle() const { return m_throttle; }
	float strafe() const { return m_strafe; }
	// Degrees the look is ahead of the body (+0x278).
	float twist() const { return m_twist; }
	void reset()
	{
		m_twist = m_catchUp = 0;
		m_slopeDelay = m_airTime = -1;
		m_jumpTime = m_noProne = m_getUp = m_recoverDelay = 0;
		m_lastPose = SoldierPose::Stand;
		m_sprintHeld = false;
	}

private:
	float m_throttle = 0; // 0x264
	float m_strafe = 0;   // 0x260
	float m_twist = 0;    // 0x278
	float m_catchUp = 0;  // 0x280: the twist the body still has to catch up
	// Timers (0x0058a6d0's start: -1 once out; 0x00586710: down to 0).
	float m_slopeDelay = -1; // +0x2e0
	float m_airTime = -1;    // +0x2e4
	float m_jumpTime = 0;    // +0x3f4
	float m_noProne = 0;     // +0x3f8
	float m_getUp = 0;       // +0x3fc
	float m_recoverDelay = 0; // +0x408
	SoldierPose m_lastPose = SoldierPose::Stand;
	bool m_sprintHeld = false;
	bool m_jumped = false;
	SprintStamina m_stamina;
};

} // namespace dice::hfe::game
