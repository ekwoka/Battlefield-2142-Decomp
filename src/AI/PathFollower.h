// AIDLL.dll's path follower (the bot's +0xd8 object, 0x178 bytes,
// constructor 0x10094f10) and the drivers that turn its steering point into
// input (the interpreter entries InfanteryMoveTo 0x1008fc90, CarMoveTo
// 0x10091050, TankMoveTo 0x10091b40, MechMoveTo 0x1008d250,
// FloatingTankMoveTo 0x1008c7c0), rewritten.
//
// The BBPGotoWaypoint patterns plan Turn(0.5) then MoveToFinding (0x100d11c0)
// to the goal (radius: the goal's; half of it at the wheel, 0x10068980).
// MoveToFinding asks the follower for a path when the goal changes
// (0x100966c0 -> 0x10095fc0):
//   - a straight walk over the mesh first, under 200 m, the ray not
//     entering cost class 5 (0x100952c0); otherwise over 100 m the cluster
//     search, then polygon searches in segments of three clusters
//     (0x100962e0, see planSegment); 100 m or less one polygon search in
//     the start's and the goal's clusters;
//   - the polygon path's portals become waypoints (0x10095b20), smoothed
//     onto the portals keeping max(1, 0.8 radius) from their ends
//     (0x10093610, below).
// Each tick (getSteer 0x10095720):
//   - off the path: back onto the waypoint whose polygon the bot is in,
//     up to 20 ahead (0x10093a20);
//   - a waypoint is reached within 3 m (soldier) / 5 m (vehicle) of height
//     and 0.5 m (soldier) / max(1, approach) m (vehicle; the last one also
//     within the vehicle's radius) horizontally (0x10094050);
//   - the next segment while the cluster path goes on (0x10095720);
//   - look-ahead (0x10093420): within 10 m (20 m) of the waypoint the
//     follower skips to the next while the mesh ray to it is clear (one
//     waypoint a tick on foot, up to four at the wheel); when none was
//     skipped and the waypoint itself can't be seen, it is blocked, and
//     blocked for 2 s (0.5 s) the path is planned again from where the bot
//     is (0x10095470);
//   - progress (0x10092bc0): a bot that pushes its controls and doesn't get
//     2 m (4 m) further for 10 s (20 s) fails; the move ends and the
//     behaviour plans again. A soldier standing still half a second jumps.
// Not rewritten: the time-slicing of the searches (a segment a tick here),
// the re-route from the bot's cluster when blocked (0x10095470 plans anew
// here), the dynamic-obstacle sidestep (0x100e63c0), and the jump (the
// rewrite's soldiers can't jump).
#pragma once

#include "NavMesh.h"

#include <functional>
#include <vector>

namespace dice::hfe::ai {

// A ray over the mesh (the pathfinder's vslot 0x84, 0x1003f930 ->
// 0x1003ce50): from the start polygon, polygon by polygon where the ray
// (x/z, from `from` towards `to`) leaves each one, at most `maxPolygons`
// (-1: 1000). True once the exit point is within 0.05 m of the end (3-D)
// or 0.16 m of it across; false when it leaves the mesh, enters a polygon
// the profile can't walk on or of the cost class `stopClass` (5 and 6 stop
// at 7 too; 0 for none), runs out of polygons, or (with a `width`) crosses
// an edge with less than `width` beside the crossing point on either side.
// `reached` gets how far it got (metres, x/z). The original also fails a
// climb steeper than the tree's slope limit (tree +0x24 -> +0xc) by more
// than the step height (0.35 on the infantry tree, the vehicle tree's own
// through vt+0x2c); those limits are not in the .qtr/.clb files the
// rewrite reads, so that test is left out.
bool meshRaycast(const NavMesh& mesh, const Vec3& from, const Vec3& to, const CostProfile* profile, int maxPolygons,
	float* reached = nullptr, int32_t startPolygon = -1, float width = 0, uint8_t stopClass = 0);

// A waypoint (0x28 bytes): the portal it crosses (a, b), where the bot
// steers (p) and the polygon it enters.
struct Waypoint {
	Vec3 a, b;
	Vec3 p;
	int32_t polygon = -1;
	bool portal = false; // the last one (the goal) has none
};

// The waypoints of a polygon path (0x10095b20): one per polygon entered,
// at the step's point (the edge's midpoint) with the edge as its portal,
// then the goal.
std::vector<Waypoint> pathWaypoints(const NavMesh& mesh, const std::vector<PathStep>& steps, const Vec3& to);

// The string-pull (0x10093610). Twice, for waypoints 5 m or more from the
// bot: where the line from the previous waypoint to the next crosses the
// portal, kept max(1, 0.8 radius) (at most half the portal) from its ends.
// Then once more without the margin where the line crosses the portal
// itself. The previous point carries over between the first two passes
// (as the original).
void smoothWaypoints(std::vector<Waypoint>& waypoints, const Vec3& bot, float radius);

struct FollowerSettings {
	bool soldier = true;
	float radius = 0.5f;   // the bot's (+0x100)
	float approach = 0;    // +4: 0 for MoveTo, 1.1 x the target's radius for MoveToObject
	PathOptions options;
};

class PathFollower {
public:
	struct Steer {
		bool ok = false;      // false: failed (no path, or stuck)
		bool wait = false;    // a search is under way
		bool arrived = false; // the last waypoint is reached
		bool jump = false;    // the soldier would jump here
		Vec3 point;           // where to steer
	};

	void reset();
	// Plans a path to `to` unless it already follows one there (0x10095fc0).
	void request(const NavMesh& mesh, const Vec3& from, const Vec3& to, const FollowerSettings& settings, float now);
	// The steering point this tick (0x10095720 with the update 0x10095ac0).
	Steer steer(const NavMesh& mesh, const Vec3& position, float now);
	// The progress check (0x10092bc0): `input` is the largest of |throttle|,
	// sprint and |yaw| this tick.
	void progress(const Vec3& position, float input, float now);
	// What is left of the path (0x10093e50).
	float remaining(const Vec3& position) const;
	// The sharpest bend (radians, signed) of the path within max(3,
	// speed x lookAheadTime) metres ahead against `forward` (0x10093c00,
	// kept for 0.5 s).
	float maxBend(const Vec3& position, const Vec3& forward, float speed, float lookAheadTime, float now);

	bool planned() const { return m_planned; }
	bool failed() const { return m_failed; }
	const Vec3& goal() const { return m_goal; }
	const std::vector<Waypoint>& waypoints() const { return m_waypoints; }
	size_t current() const { return m_current; }

private:
	void plan(const NavMesh& mesh, const Vec3& from, float now);
	bool planSegment(const NavMesh& mesh, const Vec3& position, float now);
	bool resync(const NavMesh& mesh, const Vec3& position);
	void lookAhead(const NavMesh& mesh, const Vec3& position, float now);
	int rayPolygons() const { return m_settings.soldier ? 8 : 30; }

	FollowerSettings m_settings;
	bool m_planned = false;
	bool m_failed = false;  // +0xd8
	Vec3 m_goal;            // +0x2c
	std::vector<Waypoint> m_waypoints; // +0x78
	int32_t m_startPolygon = -1;       // +0x14
	int32_t m_goalPolygon = -1;        // +0x24
	Vec3 m_requestFrom;                // +0x18
	std::vector<ClusterStep> m_clusterPath; // +0x90 +0x14
	size_t m_cursor = 0;                    // +0x90 +0
	size_t m_current = 0;              // +0x88
	bool m_blocked = false;            // +0xda
	float m_lastClear = 0;
	// Progress.
	float m_lastInput = 0;    // +0xb0
	float m_lastProgress = 0; // +0xa8
	Vec3 m_progressAt;        // +0x130
	float m_jumpAt = -100;    // +0xc0
	Vec3 m_sample;            // +0xc8
	bool m_jump = false;
	// The bend look-ahead's cache.
	float m_bendAt = -100;
	float m_bend = 0;
};

// The ground vehicles' driving from the Mobile and ControlInfo plug-ins:
// CarMoveTo 0x10091050 (state machine 0x100e8170, controller 0x100e9d40),
// TankMoveTo 0x10091b40 (0x100de8a0, 0x100e0540) and MechMoveTo 0x1008d250
// (0x100e1f20, 0x100e3c00). The tank's and the mech's machines are the
// car's with differences (see driveControl). FloatingTankMoveTo has no
// machine: floatingTankControl.
enum class Driver { Car, Tank, Mech };

struct DriveSettings {
	float maxSpeed = 15;
	float spaceNeededTweak = 0.94f;
	float speedAtZeroSpace = 3;
	float backingSpeed = 7;
	float brakeDistance = 25;
	float maxBrakePercentage = 80;
	float lookAheadTime = 1.5f;
	float minSpeedForLookAhead = 1;
	float minAngleDiffForSlowDown = 0.2f;
	float angleSlowDownScale = 1.8f;
	float maxAngleDiffSlowDown = 0.8f;
	float yawScale = 1;
	// The AI object's box (its vt+0x24, 0x100fe990): the farthest corner
	// from the object's origin (vt+0x28), from the box's centre (vt+0x30),
	// and the box's width (x) and length (z), which give the diagonal rays.
	float radius = 2.5f;
	float centreRadius = 2.5f;
	float width = 2, length = 4;
	Driver driver = Driver::Car;
};

// Another object in the bots' world list (the AI objects of the driver's
// world, its +0xd8 object's vector at +0x64) as the driver sees it.
// Soldiers (objects flagged 0x200) are not steered round, so the list
// holds the vehicles.
struct DriveObstacle {
	int id = -1;
	Vec3 position;          // its origin (vt+0x2c)
	Vec3 centre;            // its box's centre in the world
	Vec3 velocity;
	float radius = 0;       // vt+0x28
	float centreRadius = 0; // vt+0x30
};

// A mesh ray for the driver: whether it is clear to the end, else how far
// it got.
struct DriveRay {
	bool clear = true;
	float distance = 0;
};

// What the driver sees this tick.
struct DriveSense {
	float now = 0;          // the game time (the state changes at most once per time stamp)
	float error = 0;        // radians from the heading to the steering point, + to the right
	float goalDistance = 0; // to the steering point
	float speed = 0;        // along the heading (m/s, backwards negative)
	float yawRate = 0;      // radians per second, + to the right
	float tilt = 0;         // max(|pitch rate|, |roll rate|)
	float remaining = 1e8f; // of the path
	float maxBend = 0;      // ahead (PathFollower::maxBend)
	// Where the vehicle is and goes: its origin, its forward and right
	// axes across the ground (unit, y = 0), its velocity, the move's goal.
	Vec3 position;
	Vec3 forward {0, 0, 1}, right {1, 0, 0};
	Vec3 velocity;
	Vec3 goal;
	std::vector<DriveObstacle> obstacles;
	// The mesh rays (the pathfinder's vslot 0x84) from the vehicle and, for
	// choosing a side, from an obstacle. Without it the free space below
	// is given.
	std::function<DriveRay(const Vec3& from, const Vec3& to)> ray;
	// Free mesh ahead and behind (the forward and diagonal rays, the
	// backward ones) and to the sides, out to radius + 10 m.
	float freeFront = 1e9f, freeBack = 1e9f, freeLeft = 1e9f, freeRight = 1e9f;
};

struct DriveState {
	// 0 ahead; 2/3 ahead at full lock right/left of the goal; 4/5, 13/14
	// round an obstacle (back at 13/14); 6 passing an obstacle; 8 backing
	// towards a goal close behind; 9/11 backing, then turning; 12 backing
	// out of a narrow spot. The bot's +0x20c.
	int state = 0;
	float changedAt = -1;   // +0x218: one change per time stamp
	int side = 0;           // +0x210: the side an obstacle is passed on (1, 2)
	bool sideFlipped = false; // +0x214
	bool avoiding = false;  // +0x230: the side flipped: drive on, the speed capped
	int avoided = -1;       // +0x220: the obstacle in the way
	Vec3 avoidedAt;         // +0x224: where it was
	int brakingFor = -1;    // +0x234: the object ahead the speed is capped for
};

struct DriveOutput {
	float throttle = 0;
	float turn = 0; // PIYaw, + right
	int direction = 1; // +1 ahead, -1 backing
	float targetSpeed = 0;
	bool sprint = false; // PISprint (input 13): flat out near the top speed
	bool stop = false; // an obstacle sits on the goal: brake, the move is over
};

DriveOutput driveControl(const DriveSettings& settings, DriveState& state, const DriveSense& sense);

// The tank's reset (0x100e0930, TankMoveTo's and FloatingTankMoveTo's stop):
// a throttle against the speed along the heading, t = -clamp(speed, +-0.7):
// none within 0.05, 0.4 within 0.4, else t; no turning.
float tankStopThrottle(float forwardSpeed);

// FloatingTankMoveTo (0x1008c7c0): the hover tank strafes to its steering
// point with a velocity loop (0x100de350) and never turns its hull (its
// turn control, PIRoll, is the look's). `direction` is the unit 2-D way to
// the steering point (the infantry steering 0x100ce860, without the
// dynamic-obstacle sidestep 0x100e63c0, which is not rewritten),
// `acceleration` the change of velocity since the last call over its time.
struct FloatingTankOutput {
	float throttle = 0; // PIThrottle
	float strafe = 0;   // PIYaw (input 0, hard-coded): sideways on the hover tank
	float targetSpeed = 0;
};
FloatingTankOutput floatingTankControl(const DriveSettings& settings, const DriveSense& sense, const Vec3& direction,
	const Vec3& acceleration);

} // namespace dice::hfe::ai
