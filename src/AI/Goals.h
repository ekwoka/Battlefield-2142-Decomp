// The goals AIDLL.dll's bots move to: the objects a bot holds as its goal
// (bot vt+0x38 sets one; BBMoveTo 0x10066a70 and the 3d patterns read its
// urgency, slot 1, and its next point, slot 2). The SAI's orders make
// waypoint goals (kind 1, 0x1008ae90); the scripted-bot console commands
// make all four kinds (ai.botMoveTo, ai.botFlyTo, ai.botSetWaypoints_Patrol,
// ai.botSetWaypoints_Patrol3d: 0x100284a0, 0x100285a0, 0x10028680,
// 0x10028790).
//
// Every goal keeps a list of points, the head being the one moved to
// (slot 2: 0x1008aa00, 0x1008b120, 0x1008b530, 0x1008b7a0), and two flags:
// +4 "a new point" (set when the head changes) and +5 "done".
#pragma once

#include "AI/NavMesh.h"

#include <deque>
#include <vector>

namespace dice::hfe::ai {

struct Goal {
	// Slot 3, the goal's kind (what the behaviours check).
	enum class Kind {
		Waypoints = 1, // vtbl 0x1011747c (0x100d9080)
		Patrol = 2,    // vtbl 0x10117520 (0x1007eaf0)
		Fly = 6,       // vtbl 0x101174b8 (0x100674a0)
		Patrol3d = 7,  // vtbl 0x10117560 (0x100d4a80)
	};
	Kind kind = Kind::Waypoints;
	std::deque<Vec3> points;
	float radius = 0;           // +0x10: the reach (horizontal for Fly and both patrols)
	float height = 0;           // Fly +0x14: the vertical reach; Patrol3d +0x14: 10
	float areaRadius = 0;       // Waypoints +0x14 (60 for a scripted move, at least 2)
	float lastDistance = 1e9f;  // Fly +0x18: the horizontal distance² at the last look
	bool newPoint = true;       // +4
	bool done = false;          // +5
	bool suspended = false;     // +6 (slot 4, 0x1008a9a0): the urgency is 0
};

// ai.botMoveTo on the ground (0x100284a0 -> 0x1008ae90): a waypoint goal
// of one point, reach `radius`, area radius 60 (clamped to 2 or more).
Goal moveToGoal(float radius, const Vec3& at);
// ai.botFlyTo, and ai.botMoveTo for a bot whose vehicle is ITAir
// (0x1008b450): one point, horizontal reach `radius`, vertical `height`.
Goal flyToGoal(float radius, float height, const Vec3& at);
// ai.botSetWaypoints_Patrol (0x1008b560): the points in a ring, reach at
// least 5.
Goal patrolGoal(float radius, const std::vector<Vec3>& points);
// ai.botSetWaypoints_Patrol3d (0x1008b7d0): the points in a ring, reach
// `radius`, the vertical test's 10.
Goal patrol3dGoal(float radius, const std::vector<Vec3>& points);

// The goal's urgency for a bot at `position` (slot 1), moving on to the
// next point where the kind does:
//   Waypoints (0x1008aaa0): gotoUrgency over d2 with the reach; done on
//     arriving.
//   Patrol (0x1008b6c0): 1; the head goes to the back within the reach
//     (horizontally).
//   Patrol3d (0x1008b920): 1; the head goes to the back within the reach
//     horizontally and when |y - head.x| is under 10 (the original
//     compares the bot's height with the point's x, kept).
//   Fly (0x1008b230): with more than one point, the head is passed within
//     both reaches or once the bot gets farther from it than at the last
//     look; 1, or (dy² + d²) / (height² + radius²) inside the horizontal
//     reach but above the vertical one. At the last point: 0 within 50 m
//     while `holding` (the bot's vt+0x180), else 1 outside either reach and
//     (dy² + d²) / (height² + radius²) inside.
// `d2` is the bot's squared distance to the head as the caller measures
// it (the Waypoints kind's, see BotManager::moveUrgency).
float goalUrgency(Goal& goal, const Vec3& position, float d2, float botRadius, bool holding = false);
// Slot 2: the head, false for none.
bool goalPoint(const Goal& goal, Vec3& at);

} // namespace dice::hfe::ai
