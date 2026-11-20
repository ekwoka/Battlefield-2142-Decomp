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

// The soldier's look pitch's bound, degrees either way: Soldier.cpp's
// camera update (0x005890a0, from the input handling 0x0058a6d0) clamps
// the pitch (+0x270) to +-(*0x00a68eb0)[1], 85 (static_init 0x008e05e0:
// {40 the twist, 85}), each time the PIMouseLookY change is added.
constexpr float kSoldierLookPitchLimit = 85;

// A look pitch fit to aim along (sin / cos): a seated player's input
// aimPitch is the sum of the mouse's changes and runs far past a quarter
// turn (net::MoveInputState), so whatever turns it into a direction takes
// the soldier's bound first.
constexpr float soldierLookPitch(float aimPitch)
{
	return aimPitch < -kSoldierLookPitchLimit ? -kSoldierLookPitchLimit
		: aimPitch > kSoldierLookPitchLimit   ? kSoldierLookPitchLimit
		: aimPitch == aimPitch                ? aimPitch
											  : 0.0f; // NaN
}

struct SoldierInput {
	float throttle = 0; // PIThrottle: forward positive
	float strafe = 0;   // PIYaw on foot: right positive
	float turn = 0;     // degrees this tick (mouse look)
	bool sprint = false;
	SoldierPose pose = SoldierPose::Stand;
	bool fire = false;  // PIFire
	bool altFire = false; // PIAltFire: sets off the soldier's C4
	bool flareFire = false; // PIFlareFire (input 47, X): a seat's PIFlareFire weapon (the active defence)
	bool toggleWeapon = false; // c_PIToggleWeapon (input 45, F), held: next weapon on a press
	float aimPitch = 0; // degrees, up positive
	bool use = false;   // PIUse: enter or leave a vehicle (on press)
	bool toggleFireRate = false; // c_PIToggleFireRate (input 46), held: the weapon's next fire mode on a press
	bool reload = false; // c_PIReload (input 32), held: a new magazine on a press (the firearm's action 10)
	bool mouseLook = false; // c_PIMouseLook (input 11, left Ctrl), held: a toggleMouseLook camera's free look
	int switchSeat = 0; // PIPosition1..8: move to that seat (1-based; on press)
	int selectItem = 0; // PIWeaponSelect1..9: the weapon in that kit slot (on press)
	bool jump = false;  // PIAction (input 9, c_PIAction: Space), held: jumps on landing
	// The mouse wheel's c_PINextItem (+1, input 48) or c_PIPrevItem (-1,
	// input 49) this tick: a zoomed rocket launcher's airburst distance
	// (0x005d5440).
	int itemStep = 0;
	// A driving bot's PIPitch as the AI writes it (the ControlInfo adaptor's
	// drivePitch, BF2142.exe 0x009d6b98 slot 0x88), in place of the mouse's
	// y (aimPitch's change) when set.
	bool hasPitch = false;
	float pitch = 0;
	// A driver's c_PIPitch and c_PIRoll from keys, joysticks and gamepads
	// (-1..1), added to what the mouse gives (aimPitch's change, turn).
	float pitchAxis = 0, rollAxis = 0;
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
	// A client's prediction taking the server's values (the soldier's
	// mode-3 state, 0x005fcdb0: PCO vtable 0x10c and 0x104).
	void restore(float stamina, bool sprinting)
	{
		m_stamina = stamina;
		m_sprinting = sprinting;
	}

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
constexpr float kRiseTime = 0.6f;          // +0x40c (*0x00a68dfc): set with +0x3fc (0x005889d0, 0x0058a6d0)
constexpr float kAirSpeed = 2.0f;          // *0x00a68eb4: the air control's speed x time left x 0.5
} // namespace jump

// The stance keys' limits (0x005889d0, 0x00586f40; static_init 0x008e0710...).
namespace stance {
constexpr float kMinProneTime = 1.0f;      // +0x400 (*0x00a68f44): lying down from standing, the lie key and a jump don't get up
constexpr float kReProneTime = 1.0f;       // +0x404 (*0x00a68f30): after getting up, no lying down
constexpr float kOriginRaise = 0.25f;      // *0x00a68e34 (the +0x18c interface's slot 0x6c, 0x00586360)
constexpr float kWaterMargin = 1.1f;       // *0x00a68ea0
// 0x00586f40's water test: no lying down with the water over the origin + 0.25 - 1.1.
inline bool waterTooDeep(const physics::Vec3& origin, const physics::Ground& ground)
{
	return ground.waterLevel && ground.waterLevel(origin.x, origin.z) > origin.y + kOriginRaise - kWaterMargin;
}
} // namespace stance

// Speeds in m/s by speed index (0x00a68f4c, filled from 0x00a689a8...).
float soldierSpeed(int index);
// The speed index of a pose (0x00586020).
int soldierSpeedIndex(SoldierPose pose, bool sprinting, bool zoomed = false);
// The speed the soldier's third-person animation system sees (0x005ef250,
// the 3p system, +0x2f0 == 0): crouched (pose +0x268 == 1) the horizontal
// speed x anim-soldier-1p-speed-crouch (1.7, *0x00a71a08, static_init
// 0x008e7d60), lying (2) x anim-soldier-1p-speed-prone (3.0, *0x00a719e0,
// static_init 0x008e7d80), either held within 0 and 0x00586df0(3): speed
// index 3's 3.9 m/s, crouched times the look's scale (0x00586420: zoomed
// in, the field of view over the camera's times mouseSpeedMod; 1 else).
// Standing and swimming it stays.
float soldierAnimationSpeed(float speed, SoldierPose pose, float lookScale = 1);

// The walk cycle the footsteps follow (0x005ef7f0): the 3p animation's
// (slot 0x40, anim::AnimationState::walkCycle) length and time, -1 and -1
// without a cycle. Without one (length under 0) on a ladder a 0.9 s cycle
// on the game clock (DAT_00a7d060 slot 0x14, fmod); otherwise 0 and 0: no
// steps.
void soldierStepCycle(float& length, float& time, bool onLadder, double clock);

// The movement sound's rule (0x007fda00, twice from the soldier's update
// 0x0058d350): each tick it may play, the cycle's half (time < length x
// 0.5, *0x0090dc48) against the one kept (+0x38); a step when it changed
// and the soldier moves at 0.1 m/s or more (|slot 0x64|, *0x00920bd8, all
// three axes), so twice a cycle. The half is kept moving or not.
struct SoldierFootsteps {
	static constexpr float kMinSpeed = 0.1f;
	bool half = false; // +0x38
	bool step(float length, float time, float speed)
	{
		const bool now = time < length * 0.5f;
		const bool play = speed >= kMinSpeed && now != half;
		half = now;
		return play;
	}
};

struct SoldierAnimationDirection {
	float forward = 0;
	float side = 0;
};

// Project horizontal movement into the soldier's animation frame. Infantry
// movement follows the look direction (body yaw plus head twist), which can
// differ from the body's yaw while it catches up.
SoldierAnimationDirection soldierAnimationDirection(const physics::Vec3& velocity, float lookYawDegrees);

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
	// The pose a requested one gives before update (0x005889d0): lying down
	// needs 0x00586f40 (no jump's no-prone time, no second after getting
	// up, the water not too deep), else the pose stays; lying down from
	// standing, only a crouch gets up within a second.
	SoldierPose stancePose(SoldierPose requested, bool waterTooDeep, bool jump = false)
	{
		if (m_holdProne) {
			if (requested == SoldierPose::Stand && !jump) {
				return SoldierPose::Prone;
			}
			m_holdProne = false;
		}
		if (requested == SoldierPose::Prone && m_lastPose != SoldierPose::Prone) {
			const bool canLie = m_noProne <= 0 && m_reProne <= 0 && !waterTooDeep;
			return canLie ? requested : m_lastPose == SoldierPose::Crouch ? SoldierPose::Crouch : SoldierPose::Stand;
		}
		if (m_lastPose == SoldierPose::Prone && requested == SoldierPose::Stand && m_minProne > 0) {
			return SoldierPose::Prone;
		}
		return requested;
	}
	SoldierPose pose() const { return m_lastPose; } // the last update's
	// Lying from the start (a squad spawn where only lying fits, 0x006c3340,
	// or a client's prediction started from a lying soldier): no lying-down
	// timers, and down while the input asks only to stand (a joined client
	// does until its toggle takes the pose).
	void holdProne()
	{
		m_lastPose = SoldierPose::Prone;
		m_holdProne = true;
	}
	// The weapon in hand zoomed (its zoom index not 0): standing, not
	// sprinting, the soldier walks at speed index 2.
	void setZoomed(bool zoomed) { m_zoomed = zoomed; }
	SprintStamina& stamina() { return m_stamina; }
	const SprintStamina& stamina() const { return m_stamina; }
	float jumpLoss = 0;     // the soldier template's sprintLossAtJump (+0x308)
	bool jumped() const { return m_jumped; } // this tick
	// The jump hold of the soldier's slot 0xa4 (0x00586580, on its +0x18c
	// interface): a jump's timer (+0x3f4) or the rise from prone's (+0x40c)
	// still running (above 0).
	bool jumpHold() const { return m_jumpTime > 0 || m_rise > 0; }
	float airTime() const { return m_airTime; }

	float throttle() const { return m_throttle; }
	float strafe() const { return m_strafe; }
	// Degrees the look is ahead of the body (+0x278).
	float twist() const { return m_twist; }
	float catchUp() const { return m_catchUp; } // +0x280
	// A client's prediction taking the server's values (the soldier's
	// mode-3 state: +0x264, +0x260, +0x278, +0x280; 0x005fcdb0).
	void restore(float throttle, float strafe, float twist, float catchUp)
	{
		m_throttle = throttle;
		m_strafe = strafe;
		m_twist = twist;
		m_catchUp = catchUp;
	}
	void reset()
	{
		m_twist = m_catchUp = 0;
		m_slopeDelay = m_airTime = -1;
		m_jumpTime = m_noProne = m_getUp = m_minProne = m_reProne = m_recoverDelay = m_rise = 0;
		m_lastPose = SoldierPose::Stand;
		m_holdProne = false;
		m_sprintHeld = false;
	}

private:
	bool m_zoomed = false;
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
	float m_minProne = 0;    // +0x400
	float m_reProne = 0;     // +0x404
	float m_recoverDelay = 0; // +0x408
	float m_rise = 0;         // +0x40c
	SoldierPose m_lastPose = SoldierPose::Stand;
	bool m_holdProne = false;
	bool m_sprintHeld = false;
	bool m_jumped = false;
	SprintStamina m_stamina;
};

} // namespace dice::hfe::game
