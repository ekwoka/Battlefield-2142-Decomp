// AIDLL.dll's path follower (the bot's +0xd8 object, 0x178 bytes,
// constructor 0x10094f10) and the drivers that turn its steering point into
// input (the interpreter entries InfanteryMoveTo 0x1008fc90, CarMoveTo
// 0x10091050, TankMoveTo 0x10091b40), rewritten.
//
// The BBPGotoWaypoint patterns plan Turn(0.5) then MoveToFinding (0x100d11c0)
// to the goal (radius: the goal's; half of it at the wheel, 0x10068980).
// MoveToFinding asks the follower for a path when the goal changes
// (0x100966c0 -> 0x10095fc0):
//   - a straight walk over the mesh first, under 200 m and at most five
//     polygons (0x100952c0); otherwise the cluster and polygon searches;
//   - the polygon path's portals become waypoints (0x10095b20), smoothed
//     onto the portals keeping max(1, 0.8 radius) from their ends
//     (0x10093610, below).
// Each tick (getSteer 0x10095720):
//   - off the path: back onto the waypoint whose polygon the bot is in,
//     up to 20 ahead (0x10093a20);
//   - a waypoint is reached within 3 m (soldier) / 5 m (vehicle) of height
//     and 0.5 m (soldier) / max(1, approach) m (vehicle; the last one also
//     within the vehicle's radius) horizontally (0x10094050);
//   - look-ahead (0x10093420): within 10 m (20 m) of the waypoint the
//     follower skips to the next while the mesh ray to it is clear (one
//     waypoint a tick on foot, up to four at the wheel); when not even the
//     waypoint itself can be seen, it is blocked, and blocked for 2 s
//     (0.5 s) the path is planned again from where the bot is (0x10095470);
//   - progress (0x10092bc0): a bot that pushes its controls and doesn't get
//     2 m (4 m) further for 10 s (20 s) fails; the move ends and the
//     behaviour plans again. A soldier standing still half a second jumps.
// Not rewritten: the search windows of three clusters (the whole path is
// searched at once), the dynamic-obstacle sidestep (0x100e63c0), and the
// jump (the rewrite's soldiers can't jump).
#pragma once

#include "NavMesh.h"

#include <vector>

namespace dice::hfe::ai {

// A ray over the mesh (the pathfinder's vslot 0x84): walks the polygons the
// segment crosses seen from above, at most `maxPolygons` of them. False
// when it leaves the mesh, enters a polygon the profile can't walk on or
// runs out of polygons; `reached` gets how far it got (metres, x/z).
bool meshRaycast(const NavMesh& mesh, const Vec3& from, const Vec3& to, const CostProfile* profile, int maxPolygons,
	float* reached = nullptr, int32_t startPolygon = -1);

// The polygon under a point, else the nearest within `reach` metres
// (rings a metre apart, 16 directions; the original's lookup through the
// pathfinder's vslot 0x38 is not decoded: provisional), -1 for none.
// `at` gets the point on it.
int32_t nearestPolygon(const NavMesh& mesh, const Vec3& point, float reach, const CostProfile* profile, Vec3* at = nullptr);

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
	bool resync(const NavMesh& mesh, const Vec3& position);
	void lookAhead(const NavMesh& mesh, const Vec3& position, float now);
	int rayPolygons() const { return m_settings.soldier ? 8 : 30; }

	FollowerSettings m_settings;
	bool m_planned = false;
	bool m_failed = false;  // +0xd8
	Vec3 m_goal;            // +0x2c
	std::vector<Waypoint> m_waypoints; // +0x78
	int32_t m_startPolygon = -1;
	size_t m_current = 0;              // +0x88
	bool m_blocked = false;            // +0xda
	float m_blockedSince = 0;
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

// The ground vehicles' driving (CarMoveTo 0x10091050 / TankMoveTo
// 0x10091b40, their controllers 0x100e9d40 / 0x100e0540 and the state
// machine 0x100e8170) from the Mobile and ControlInfo plug-ins.
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
	float radius = 2.5f; // the vehicle's (provisional: its AI object's radius isn't read)
	bool tank = false;   // TankMoveTo's controller
};

// What the driver sees this tick.
struct DriveSense {
	float error = 0;        // radians from the heading to the steering point, + to the right
	float goalDistance = 0; // to the steering point
	float speed = 0;        // along the heading (m/s, backwards negative)
	float yawRate = 0;      // radians per second, + to the right
	float tilt = 0;         // max(|pitch rate|, |roll rate|)
	float remaining = 1e8f; // of the path
	float maxBend = 0;      // ahead (PathFollower::maxBend)
	// Free mesh ahead and behind (the forward and diagonal rays, the
	// backward ones) and to the sides, out to radius + 10 m.
	float front = 1e9f, back = 1e9f, left = 1e9f, right = 1e9f;
};

struct DriveState {
	int state = 0; // 0 forward, 8 backing towards the goal, 9/11 turning on the spot
};

struct DriveOutput {
	float throttle = 0;
	float turn = 0; // PIYaw, + right
	int direction = 1; // +1 ahead, -1 backing
	float targetSpeed = 0;
};

DriveOutput driveControl(const DriveSettings& settings, DriveState& state, const DriveSense& sense);

} // namespace dice::hfe::ai
