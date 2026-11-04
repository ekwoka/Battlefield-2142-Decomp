#include "AI/Goals.h"

#include "AI/Behaviour.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::ai {

namespace {

float horizontal2(const Vec3& a, const Vec3& b)
{
	return (a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z);
}

// The head to the back (0x10028030 then 0x1008aa40): a ring.
void rotate(Goal& goal)
{
	goal.points.push_back(goal.points.front());
	goal.points.pop_front();
	goal.newPoint = true;
}

} // namespace

Goal moveToGoal(float radius, const Vec3& at)
{
	Goal goal;
	goal.kind = Goal::Kind::Waypoints;
	goal.radius = radius;
	goal.areaRadius = 60; // the caller's (0x1008ae90 raises it to 2 at least)
	goal.points.push_back(at);
	return goal;
}

Goal flyToGoal(float radius, float height, const Vec3& at)
{
	Goal goal;
	goal.kind = Goal::Kind::Fly;
	goal.radius = radius;
	goal.height = height;
	goal.points.push_back(at);
	return goal;
}

Goal patrolGoal(float radius, const std::vector<Vec3>& points)
{
	Goal goal;
	goal.kind = Goal::Kind::Patrol;
	goal.radius = std::max(radius, 5.0f);
	goal.points.assign(points.begin(), points.end());
	return goal;
}

Goal patrol3dGoal(float radius, const std::vector<Vec3>& points)
{
	Goal goal;
	goal.kind = Goal::Kind::Patrol3d;
	goal.radius = radius;
	goal.height = 10;
	goal.points.assign(points.begin(), points.end());
	return goal;
}

float goalUrgency(Goal& goal, const Vec3& position, float d2, float botRadius, bool holding)
{
	if (goal.suspended) {
		return 0;
	}
	if (goal.points.empty()) {
		goal.done = true;
		return 0;
	}
	const Vec3& head = goal.points.front();
	switch (goal.kind) {
	case Goal::Kind::Waypoints: {
		bool arrived = false;
		const float u = gotoUrgency(d2, goal.radius, botRadius, GoalArea {}, arrived);
		if (arrived) {
			goal.done = true;
		}
		return u;
	}
	case Goal::Kind::Patrol:
		if (horizontal2(position, head) < goal.radius * goal.radius) {
			rotate(goal);
		}
		return 1;
	case Goal::Kind::Patrol3d:
		// [esi+0x24]'s head: the bot's y against the point's x (0x1008b96c).
		if (horizontal2(position, head) < goal.radius * goal.radius && std::fabs(position.y - head.x) < goal.height) {
			rotate(goal);
		}
		return 1;
	case Goal::Kind::Fly: {
		const float reach2 = goal.radius * goal.radius;
		const float height2 = goal.height * goal.height;
		const float d = horizontal2(position, head);
		const float dy2 = (position.y - head.y) * (position.y - head.y);
		if (goal.points.size() != 1) {
			if ((d < reach2 && dy2 < height2) || goal.lastDistance < d) {
				goal.points.pop_front();
				goal.newPoint = true;
			}
			goal.lastDistance = d;
			if (d < reach2 && height2 < dy2) {
				return (dy2 + d) / (height2 + reach2);
			}
			return 1;
		}
		if (d < 2500 && holding) {
			return 0;
		}
		if (reach2 <= d || height2 <= dy2) {
			return 1;
		}
		return (dy2 + d) / (height2 + reach2);
	}
	}
	return 0;
}

bool goalPoint(const Goal& goal, Vec3& at)
{
	if (goal.points.empty()) {
		return false;
	}
	at = goal.points.front();
	return true;
}

} // namespace dice::hfe::ai
