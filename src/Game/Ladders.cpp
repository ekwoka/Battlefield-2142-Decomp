// Ladders (BF2/Game/Objects: the Ladder object 0x0059fb00, vtable
// 0x009678c8, and the LadderContainer 0x0059dec0, class 0x9c46, vtable
// 0x009673d8). The soldier response knows nothing of ladders: a soldier who
// touches one (the ladder's collision callback, slot 43, 0x0059fd40) facing
// it is seated in a LadderContainer the game spawns at the ladder, which
// then moves itself up and down by the throttle (slot 42, 0x0059f6b0) and
// puts the soldier off at the ends (0x0059e210, 0x0059e820).
//
// The tweakables (named by 0x004238a0's registrations, 0x008e1607 on):
// offset-top 0.36, soldier-start-getoff 1.8, top-exit-offset and
// exit-offset 0.75, bottom-marginal 0.05, skid-speed-mod 4, climb-speed-mod
// 0.045, water-limit 1.2, getoff-speed-mod 1.15, soldier-dist 0.6,
// down-angle-limit 35.
//
// The rays (0x0059ed10) that stop the container or put the soldier off on
// the way, the bottom's ray down (0x0059d550), the jump key (PIAction)
// that leaves the ladder anywhere and the soldier's place in the container
// (the LadderContainer template's seat, 0/1/0) are rewritten; not the
// nudge (slot 103), the climbing animation and camera (0x0059f380,
// 0x0059e730).
#include "GameWorld.h"

#include "Physics/MaterialManager.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dice::hfe::game {

using physics::Vec3;

namespace {

constexpr float kOffsetTop = 0.36f;      // offset-top (0x00a69c1c)
constexpr float kStartGetOff = 1.8f;     // soldier-start-getoff (0x00a69c04)
constexpr float kExitOffset = 0.75f;     // top-exit-offset, exit-offset (0x00a69c10, 0x00a69c14)
constexpr float kBottomMarginal = 0.05f; // bottom-marginal (0x00a69bfc)
constexpr float kSkidSpeed = 4.0f;       // skid-speed-mod (0x00a69c08)
constexpr float kClimbSpeed = 0.045f;    // climb-speed-mod (0x00a69c4c): per tick at 30 Hz
constexpr float kWaterLimit = 1.2f;      // water-limit (0x00a69c20)
constexpr float kGetOffSpeed = 1.15f;    // getoff-speed-mod (0x00a69c48)
constexpr float kSoldierDistance = 0.6f; // soldier-dist (0x00a69c34)
constexpr float kDownAngle = 35.0f;      // down-angle-limit (0x00a69c00), degrees
constexpr float kOriginOverFeet = 1.01f; // 0x0059e820's exit height
constexpr float kDownRay = 0.2f;         // ladder-down-ray-mag (0x00a69c50)
constexpr float kDownRaySkid = 0.3f;     // ladder-down-ray-mag-skid (0x00a69c28)
constexpr float kUpRay = 1.7f;           // ladder-up-ray-mag-skid (0x00a69c18)
constexpr float kGetOffRayMin = 0.3f;    // ladder-getoff-ray-min (0x00a69c38)

float length(const Vec3& v) { return std::sqrt(physics::lengthSquared(v)); }

} // namespace

// 0x0059d550 (once): the top at the soldier box's highest point; the bottom
// at its lowest, but no lower than the terrain or 1.2 m under the water.
void GameWorld::measureLadder(Ladder& ladder) const
{
	if (ladder.measured) {
		return;
	}
	ladder.measured = true;
	ladder.top = ladder.position.y + ladder.boxMax.y;
	ladder.bottom = ladder.position.y + ladder.boxMin.y;
	if (m_hasWater) {
		ladder.bottom = std::max(ladder.bottom, m_waterLevel - kWaterLimit);
	}
	if (m_ground.height) {
		ladder.bottom = std::max(ladder.bottom, m_ground.height(ladder.position.x, ladder.position.z));
	}
	// The ray down where the soldier hangs (soldier-dist behind the
	// ladder's axis, along -z) from the top to the bottom, against the
	// objects (collision manager slot 0x68, filter 0x100 / 8; the ladder's
	// own faces left out): what it meets is the bottom, 0.01 over it; or,
	// for an object with a water material (its template's flag 2,
	// 0x005f7ce0), 1.2 under it if that is higher.
	const Vec3& forward = ladder.rotation.rows[2];
	const Vec3 at = ladder.position - forward * kSoldierDistance;
	physics::CollisionWorld::Hit hit {};
	if (m_collision.raycast({at.x, ladder.top, at.z}, {at.x, ladder.bottom, at.z}, hit, ladder.owner)) {
		const physics::Material* material = m_materials ? m_materials->find(hit.material) : nullptr;
		if (material && material->hasWaterPhysics) {
			ladder.bottom = std::max(ladder.bottom, hit.point.y - kWaterLimit);
		} else {
			ladder.bottom = hit.point.y + 0.01f;
		}
	}
}

// Where the soldier sits in the container: the LadderContainer template's
// seatInformation (0/1/0), in the container's frame (the ladder's
// rotation, at the container's height).
Vec3 GameWorld::ladderSeat() const
{
	if (const world::ObjectTemplate* container = m_templates.find("LadderContainer")) {
		const auto info = container->settings().find("seatInformation");
		if (info != container->settings().end()) {
			char name[128] = {};
			Vec3 at;
			if (std::sscanf(info->second.c_str(), "%127s %f/%f/%f", name, &at.x, &at.y, &at.z) == 4) {
				return at;
			}
		}
	}
	return {0, 1, 0};
}

// 0x0059ed10: rays from `p` (the container's new place, or a soldier
// getting on: its origin) against the objects (the ladder's own faces
// left out), `back` 0.75 m (ladder-exit-offset) against the way the ladder
// (or the soldier) faces. Going down or skidding (or without a
// container): a ray down 0.2 m (0.3 skidding) from p + back, else one from
// p along back; either meeting something gives 1 (put off there). Going
// up (or without a container): a ray up 0.2 m from p + back + 1.7 m
// (without a container from p + back, 1.9 m), else one from 1.7 m over p
// along back; either meeting something gives 2 (stay). Otherwise within
// 1.8 m of the top (soldier-start-getoff): the container's soldier
// fitting at the top's exit goes on (0); else a ray from the ladder's
// axis, s = max(0.3, 1 - the way left / 1.8) over the top, forward by
// 0.75 x s plus 0.75 meeting something gives 2.
int GameWorld::ladderRays(const Ladder& ladder, const Vec3& p, const Vec3& back, bool container, bool up, bool down,
	bool skid) const
{
	physics::CollisionWorld::Hit hit {};
	const auto ray = [&](const Vec3& from, const Vec3& way) { return m_collision.raycast(from, from + way, hit, ladder.owner); };
	int result = 0;
	if (!container || down || skid) {
		const float reach = skid ? kDownRaySkid : kDownRay;
		if (ray(p + back, {0, -reach, 0}) || ray(p, back)) {
			result = 1;
		}
	}
	if (!container || up) {
		const Vec3 start = p + back + Vec3 {0, container ? kUpRay : 0.0f, 0};
		const float reach = container ? 0.2f : kUpRay + 0.2f;
		const bool blocked = ray(start, {0, reach, 0}) || ray(p + Vec3 {0, kUpRay, 0}, back);
		if (blocked) {
			return 2;
		}
		const float left = ladder.top - p.y;
		if (kStartGetOff > 0 && left < kStartGetOff) {
			const Vec3& forward = ladder.rotation.rows[2];
			if (container) {
				Vec3 exit = ladder.position + forward * kExitOffset;
				exit.y = ladder.top + forward.y * kExitOffset;
				if (soldierFits(exit + Vec3 {0, kOriginOverFeet, 0}, SoldierPose::Stand, 0)) {
					return result;
				}
			}
			const float s = std::max(kGetOffRayMin, 1.0f - left / kStartGetOff);
			const Vec3 from {ladder.position.x, ladder.top + s, ladder.position.z};
			if (ray(from, forward * (s * kExitOffset) - back)) {
				result = 2;
			}
		}
	}
	return result;
}

// 0x0059fd40: a soldier touching the ladder's box gets on when, above its
// bottom, it faces the ladder within 60 degrees from either side, looks up
// (or, within 1.2 m of the top, down by 35 degrees or more) and fits where
// it would be put off sideways.
bool GameWorld::tryLadder(Player& player, SoldierInput& input)
{
	const Vec3 origin = player.body.position;
	const float yaw = player.body.yawDegrees * 3.14159265f / 180.0f;
	const Vec3 facing {std::sin(yaw), 0, std::cos(yaw)};
	const float pitch = -input.aimPitch; // looking down positive (0x004a6b80)
	for (size_t i = 0; i < m_ladders.size(); ++i) {
		Ladder& ladder = m_ladders[i];
		const Vec3 local = ladder.rotation.untransform(origin - ladder.position);
		const float r = physics::soldier::kRadius + 0.05f; // its spheres touching the ladder's mesh
		if (local.x < ladder.boxMin.x - r || local.x > ladder.boxMax.x + r || local.z < ladder.boxMin.z - r
			|| local.z > ladder.boxMax.z + r || local.y - 1.0f > ladder.boxMax.y
			|| local.y - 1.0f + physics::soldier::kHeight < ladder.boxMin.y) {
			continue;
		}
		measureLadder(ladder);
		if (origin.y < ladder.bottom) {
			continue;
		}
		const Vec3& forward = ladder.rotation.rows[2];
		const Vec3 towards = ladder.position - origin;
		const float l = length(towards);
		const bool front = l > 0 && physics::dot(towards * (1.0f / l), forward) > 0;
		const float along = physics::dot(facing, forward);
		if (!((front && along >= 0.5f) || (!front && along <= -0.5f))) {
			continue;
		}
		const float at = std::max(std::min(origin.y - 1.0f, ladder.top), ladder.bottom + kBottomMarginal);
		const bool nearTop = ladder.top - kWaterLimit < at;
		if (nearTop ? pitch < kDownAngle : pitch >= 0) {
			continue;
		}
		// Under the terrain or 1.2 m under the water there (0x0059e210 with
		// neither way; not near the top), or the rays (no container) meeting
		// anything: not on.
		if (!nearTop && m_ground.height) {
			float floor = m_ground.height(origin.x, origin.z);
			if (m_hasWater) {
				floor = std::max(floor, m_waterLevel - kWaterLimit);
			}
			if (origin.y < floor) {
				continue;
			}
		}
		if (ladderRays(ladder, origin, facing * -kExitOffset, false, false, false, false) != 0) {
			continue;
		}
		const Vec3 side = Vec3 {ladder.position.x, at, ladder.position.z} - forward * kExitOffset
			+ Vec3 {0, kOriginOverFeet, 0};
		if (!soldierFits(side, SoldierPose::Stand, 0)) {
			continue;
		}
		// 0x0059fbf0: seated in the container at the ladder.
		player.ladder = static_cast<int>(i);
		player.ladderY = at;
		player.body.velocity = {};
		return true;
	}
	return false;
}

// 0x0059f6b0: the container climbs by the throttle, 0.045 m a tick (1.35
// m/s), 1.15 times as fast within 1.8 m of the top; looking down by 35
// degrees or more turns the throttle round, and sprinting down skids at 4
// times. Past the top less 0.36 going up, under the bottom going down, or
// under the terrain or 1.2 m under the water, the soldier is put off
// (if it fits there; else the container stays).
void GameWorld::climbLadder(Player& player, SoldierInput& input, float seconds)
{
	if (player.ladder < 0 || static_cast<size_t>(player.ladder) >= m_ladders.size()) {
		player.ladder = -1;
		return;
	}
	Ladder& ladder = m_ladders[static_cast<size_t>(player.ladder)];
	measureLadder(ladder);
	const Vec3& forward = ladder.rotation.rows[2];
	const auto seat = [&] {
		if (player.ladder >= 0) {
			player.body.position = Vec3 {ladder.position.x, player.ladderY, ladder.position.z}
				+ ladder.rotation.transform(ladderSeat());
			player.body.yawDegrees = std::atan2(forward.x, forward.z) * 57.29578f;
		}
	};
	// The jump key (the container's +0x460, PIAction): off anywhere, 0.75 m
	// back, if the soldier fits there.
	if (input.jump && leaveLadder(player, 3)) {
		return;
	}
	const float pitch = -input.aimPitch;
	float sign = pitch >= kDownAngle ? -1.0f : 1.0f;
	float in = std::clamp(input.throttle, -1.0f, 1.0f);
	// 0x0059f1f0: sprinting while going down, short of the top: a skid.
	if (input.sprint && player.ladderY <= ladder.top - kStartGetOff && sign * in < 0) {
		in = kSkidSpeed;
		sign = -1;
	}
	const bool skid = in == kSkidSpeed;
	const bool up = sign * in > 0, down = sign * in < 0;
	const float k = player.ladderY > ladder.top - kStartGetOff ? kGetOffSpeed : 1.0f;
	const float y = player.ladderY + in * sign * kClimbSpeed * seconds * 30.0f * k;
	// 0x0059e210.
	int mode = 0;
	float floor = m_ground.height ? m_ground.height(ladder.position.x, ladder.position.z) : -1e9f;
	if (m_hasWater) {
		floor = std::max(floor, m_waterLevel - kWaterLimit);
	}
	if (y > ladder.top - kOffsetTop && up) {
		mode = 1;
	} else if (y < ladder.bottom - kBottomMarginal && down) {
		mode = 2;
	} else if (y < floor) {
		mode = 3;
	}
	if (mode != 0) {
		leaveLadder(player, mode);
		seat();
		return;
	}
	// The rays (0x0059ed10): 0 the container moves (slot 0x80), 1 the
	// soldier is put off where it is (mode 3), 2 it stays.
	const int rays = ladderRays(ladder, {ladder.position.x, y, ladder.position.z}, forward * -kExitOffset, true, up,
		down, skid);
	if (rays == 0) {
		player.ladderY = y;
	} else if (rays == 1) {
		leaveLadder(player, 3);
	}
	seat();
}

// 0x0059e820 / 0x0059ea20: the ends' exits (1 the top: over it, 0.75 m on
// the ladder's front side; 2 the bottom: 0.75 m back; 3 anywhere: 0.75 m
// back from the container), the origin 1.01 m over the point, if the
// soldier fits there (then 0x0059d440 ends the container).
bool GameWorld::leaveLadder(Player& player, int mode)
{
	const Ladder& ladder = m_ladders[static_cast<size_t>(player.ladder)];
	const Vec3& forward = ladder.rotation.rows[2];
	Vec3 at;
	if (mode == 1) {
		at = ladder.position + forward * kExitOffset;
		at.y = ladder.top + forward.y * kExitOffset;
	} else if (mode == 2) {
		at = ladder.position - forward * kExitOffset;
		at.y = ladder.bottom - forward.y * kExitOffset;
	} else {
		at = Vec3 {ladder.position.x, player.ladderY, ladder.position.z} - forward * kExitOffset;
	}
	const Vec3 origin = at + Vec3 {0, kOriginOverFeet, 0};
	if (!soldierFits(origin, SoldierPose::Stand, 0)) {
		return false;
	}
	player.body.position = origin;
	player.body.velocity = {};
	player.ladder = -1;
	return true;
}

bool GameWorld::onLadder(int id) const
{
	const Player* player = findPlayer(id);
	return player && player->ladder >= 0;
}

} // namespace dice::hfe::game
