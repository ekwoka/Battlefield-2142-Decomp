// AIDLL.dll's sensing agent's look-around (the agent at bot +0xdc:
// constructor 0x100d7210, destructor 0x100d72d0, reset 0x100d6b90; its
// update 0x100d7840, run by 0x1009a4c0 after the main plan for a bot that
// isn't scripted). It is where a soldier looks when no plan aims: eight
// world sectors (0x100d50b0: +z, +x, -z, -x, then the diagonals +x+z,
// +x-z, -x-z, -x+z), each with a weight that grows at each choice (the
// way it walks faster) and drops to 0 once it is faced; the memory's
// items and the rounds that passed it add to the sectors they lie in;
// the heaviest is looked at for a while, the walk's way for longest.
//
// The plan is the Soldier sensing pattern's (AIBehaviours.ai's
// addSensingAgentPattern Infantery Soldier; vtable 0x101175e8): a look
// statement along an offset (0x100de2c0) beside the steering statement
// (0x100cd3b0) that turns the walk (0x1008f8e0's way) into throttle and
// strafe against the look (BotManager::steer). The Turret pattern
// (vtable 0x10117660) builds the same look statement (0x1008c120: offset;
// 0x1008c220: at an object, 0x100d4f10) for the seat's controlled object
// when that has its ControlInfo (its AI part's +0x38), with no steering:
// the gun turns, run by the seat's MouseTurretLookAt as a soldier's look
// is. Its statement carries 5 degrees (+0x1c, DAT_10127df0 x 5) where the
// Soldier's carries 0; the controller 0x100e5820 takes it as its fifth
// argument and never reads it. The Null pattern (0x101175b4, 0x1008b9f0)
// builds a statement that does nothing.
// Any plan's aim (MouseTurretLookAt 0x1008e6e0, MouseTurretAimAt
// 0x1008e940 -> 0x100d51c0) pauses the look-around for 2 s; a step of the
// walk clears the pause's stamp (0x1008f8e0).
#include "Bots.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::game {

namespace {

constexpr float kEye = 1.6f; // Bots.cpp's kEyeHeight
constexpr float kRadiansPerDegree = 3.14159265f / 180;

const float kSectorX[8] = {0, 1, 0, -1, 0.70710677f, 0.70710677f, -0.70710677f, -0.70710677f};
const float kSectorZ[8] = {1, 0, -1, 0, 0.70710677f, -0.70710677f, -0.70710677f, 0.70710677f};

// 0x100d5280 / 0x100d5360: sector i within `degrees` of the way (x, z);
// false for a way under 0.1 long.
bool within(int i, float x, float z, float cosine)
{
	const float l2 = x * x + z * z;
	if (i < 0 || i > 7 || l2 < 0.01f) {
		return false;
	}
	const float l = std::sqrt(l2);
	return (kSectorX[i] * x + kSectorZ[i] * z) / l >= cosine;
}
constexpr float kCos20 = 0.93969262f;
constexpr float kCos90 = 0.0f;

// 0x100d5700: the cardinal and the diagonal sector holding a way.
void sectorsOf(float x, float z, int& cardinal, int& diagonal)
{
	cardinal = (x + z >= 0) ? (x >= z ? 1 : 0) : (x >= z ? 2 : 3);
	diagonal = z < 0 ? (x <= 0 ? 6 : 5) : (x <= 0 ? 7 : 4);
}

} // namespace

// 0x100d6d60 (bot vt+0x308, from the sensing pass 0x100ed700): the sectors
// the soldier faces (its view within about 10 degrees of them, 0.9848)
// weigh 0 again; the chosen one is marked reached.
void BotManager::faceSectors(Bot& bot, float yaw)
{
	const float fx = std::sin(yaw * kRadiansPerDegree), fz = std::cos(yaw * kRadiansPerDegree);
	int c = 0, g = 0;
	sectorsOf(fx, fz, c, g);
	for (int s : {c, g}) {
		if (kSectorX[s] * fx + kSectorZ[s] * fz > 0.9848f) {
			bot.look.sectorW[static_cast<size_t>(s)] = 0;
			if (bot.look.plan && bot.look.sectorPlan && s == bot.look.sector) {
				bot.look.reached = true;
			}
		}
	}
}

// 0x100d7840 with its parts: the weights' growth 0x100d6630, the
// stimuli 0x100d7400 (memory items) and 0x100d7590 (rounds), the choice
// 0x100d77c0 -> 0x100d67b0 / 0x100d60f0, the look's duration 0x100d5ad0,
// the cancel test 0x100d6300, a new plan 0x100d5ca0 and the stimuli
// cleared 0x100d6440. The soldier's control info allows every sector
// (0x100d5a40 over 0x100d5440: no limits). A turret's would hold the
// sectors to its yaw limits (0x100d5440 over the ControlInfo's slots
// 0x4c / 0x54 / 0x60 in BF2142.exe); the rewrite lets every sector
// through (the shipped tanks' and AA guns' turrets turn all round; a
// limited gun stops at its limit).
void BotManager::lookAround(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	Bot::Look& L = bot.look;
	if (!L.started) {
		// The reset (0x100d6b90): a decision now, the first look 0.5..1 s.
		L = Bot::Look();
		L.started = true;
		L.decidedAt = m_time;
		L.duration = 0.5f + random() * 0.5f;
	}
	if (L.paused) {
		// 0x100d51f0: the plan cancelled; the pause lasts 2 s from its
		// stamp (DAT_10129fd0).
		L.plan = false;
		L.needPlan = true;
		if (m_time - L.pausedAt > 2.0f) {
			L.paused = false;
		}
		return;
	}
	L.decidedAt = std::min(L.decidedAt, m_time);
	const float elapsed = m_time - L.decidedAt;
	// A soldier's look chosen ahead of its walk is dropped once the walk
	// turns from it (the override).
	const bool override = L.aheadOfMove && !within(L.sector, L.moveX, L.moveZ, kCos20);
	if (!(elapsed >= L.duration || L.force || L.needPlan) && !override) {
		// The plan keeps its look.
	} else {
		if (override) {
			L.aheadOfMove = false;
		}
		// The pose (0x100a8dd0): prone looks about only when forced.
		const bool skip = self.pose == SoldierPose::Prone ? !L.force : false;
		if (bot.mayAdvance && !skip) {
			float backX = 0, backZ = 0;
			if (!L.force) {
				// 0x100d6630: the walk's sectors grow 0.05 (0.25 on the
				// override), the others 0.0125; nothing without a way.
				const float fx = L.moveX, fz = L.moveZ;
				if (fx * fx + fz * fz >= 0.01f) {
					backX = -fx;
					backZ = -fz;
					for (int i = 0; i < 8; ++i) {
						L.sectorW[static_cast<size_t>(i)] += !within(i, fx, fz, kCos20) ? 0.0125f : (override ? 0.25f : 0.05f);
					}
				}
			}
			// The stimuli: the memory's items (0x100d7400) and the rounds
			// (0x100d7590), each into its cardinal and diagonal sector.
			struct Acc {
				float w = 0;
				physics::Vec3 sum;
				int n = 0;
			} acc[8];
			const auto add = [&](float w, const physics::Vec3& at) {
				const physics::Vec3 d = at - self.position;
				const float l2 = d.x * d.x + d.z * d.z;
				if (l2 < 1e-6f) {
					return;
				}
				int c = 0, g = 0;
				sectorsOf(d.x, d.z, c, g);
				for (int s : {c, g}) {
					acc[s].w += w;
					acc[s].sum = acc[s].sum + at;
					acc[s].n += 1;
				}
			};
			for (const Known& k : bot.known) {
				const GameWorld::PlayerInfo* other = playerOf(k.player);
				if (!other || !other->alive) {
					continue;
				}
				const bool heard = false; // flag 1: the bots hear vehicles only (Bots.cpp's hear)
				const bool targeted = k.player == bot.planTarget; // flag 0x20 (0x1007cf10)
				float w = 0;
				if (!k.everSeen) {
					w = (heard ? 0.3f : 0) + (targeted ? 0.25f : 0);
				} else if (!k.secondHand) {
					w = 0.5f;
				} else {
					w = targeted ? 0.25f : (heard ? 0.25f : 0);
				}
				w *= certainty(self.team, k.player);
				add(w, k.position);
			}
			for (const Bot::RoundSeen& r : bot.roundsSeen) {
				const GameWorld::PlayerInfo* shooter = playerOf(r.shooter);
				if (!shooter) {
					continue;
				}
				float w = 0;
				physics::Vec3 at;
				if (r.friendly) {
					w = 0.5f;
					at = r.impact;
				} else {
					if (!L.forceLatch) {
						L.force = L.forceLatch = true; // one forced re-decision
					}
					w = m_time - r.at >= 5 ? 0.5f : 0.7f;
					at = shooter->position;
				}
				add(w * certainty(self.team, r.shooter), at);
			}
			// 0x100d67b0: the heaviest sector with its stimuli's mean point;
			// none above 0, a random one (round(U * 7): 0 and 7 half as
			// likely), weight 0.1, no point.
			int best = -1;
			float bestW = 0;
			physics::Vec3 bestAt;
			bool hasAt = false;
			for (int i = 0; i < 8; ++i) {
				const float w = acc[i].w + L.sectorW[static_cast<size_t>(i)];
				if (w > bestW) {
					best = i;
					bestW = w;
					hasAt = acc[i].n > 0;
					bestAt = hasAt ? acc[i].sum * (1.0f / static_cast<float>(acc[i].n)) : physics::Vec3 {};
				}
			}
			if (best < 0) {
				best = static_cast<int>(std::floor(random() * 7.0f + 0.5f)); // 0x100d60f0
				bestW = 0.1f;
				hasAt = false;
			}
			// 0x100d5ad0: how long the look lasts. Moving: the input's
			// throttle or strafe at 0.01 or more.
			const bool moving = std::fabs(input.throttle) >= 0.01f || std::fabs(input.strafe) >= 0.01f;
			const auto u = [this](float a, float b) { return a + (b - a) * random(); };
			if (backX * backX + backZ * backZ < 0.01f) {
				L.duration = u(1.5f, 2.0f);
			} else if (moving) {
				L.duration = within(best, -backX, -backZ, kCos20) ? u(7, 10) : u(1.5f, 2.0f);
			} else {
				L.duration = u(3, 4);
			}
			if (L.force) {
				L.plan = false; // 0x100d51f0
				L.needPlan = true;
			} else {
				L.forceLatch = false;
			}
			L.decidedAt = m_time;
			L.force = false;
			// 0x100d6300: a plan whose sector isn't chosen again goes.
			bool bad = false;
			if (L.sectorPlan) {
				if (best != L.sector) {
					L.reached = false;
					bad = true;
				}
			}
			bad = bad || override;
			if (L.plan && bad) {
				L.plan = false;
				L.needPlan = true;
			}
			if (L.needPlan) {
				// 0x100d5ca0: the walk's way known, or the choice outweighs
				// the Turn's request: the sector's look; else the request's.
				if (L.moveX * L.moveX + L.moveZ * L.moveZ >= 0.01f || !L.requested || L.requestWeight <= bestW) {
					float gunYaw = 0, gunPitch = bot.pitch;
					if (self.vehicle) {
						m_world.gunAim(self.vehicle, self.seat, gunYaw, gunPitch);
					}
					const float pitchY = std::sin(gunPitch * kRadiansPerDegree);
					// A point at y exactly 0 reads as a plain sector (the
					// original's test of the point's y).
					if (hasAt && bestAt.y != 0) {
						L.offset = bestAt - self.position;
						L.offsetFromRoot = true;
					} else {
						L.offset = {kSectorX[best], pitchY, kSectorZ[best]};
						L.offsetFromRoot = false;
					}
					L.sectorPlan = true;
					L.sector = best;
					L.sectorWeight = bestW;
					L.sectorPoint = bestAt;
					L.aheadOfMove = within(best, L.moveX, L.moveZ, kCos20);
				} else {
					L.offset = L.request;
					L.offsetFromRoot = false;
					L.sectorPlan = false;
					L.aheadOfMove = false;
				}
				L.plan = true;
				L.needPlan = false;
			}
			// 0x100d6440: the stimuli and the request spent.
			bot.roundsSeen.clear();
			L.requested = false;
			L.requestWeight = 0;
		} else if (!L.plan) {
			// 0x100d58f0: along the walk (x 5), the look's height kept.
			L.offset = {L.moveX, std::sin(bot.pitch * kRadiansPerDegree), L.moveZ};
			L.offsetFromRoot = false;
			L.plan = L.moveX * L.moveX + L.moveZ * L.moveZ >= 0.01f;
			L.sectorPlan = false;
		}
	}
	if (!L.plan) {
		return;
	}
	// The look statement: the aim turns the view along the offset.
	physics::Vec3 eye = self.position + physics::Vec3 {0, kEye, 0};
	// Turret LookAt is evaluated from the gun's round origin in aim(). Keep
	// the idle sector's short look point in that same frame: a root-eye point
	// can fall inside a long barrel's muzzle sweep and its apparent bearing
	// then changes as the muzzle moves around the yaw pivot.
	if (!L.offsetFromRoot && self.vehicle) {
		physics::Mat3 hull;
		m_world.gunFrame(self.vehicle, self.seat, eye, hull);
	}
	const physics::Vec3 point = L.offsetFromRoot ? self.position + L.offset : eye + L.offset * 5.0f;
	L.quiet = true;
	const float throttle = input.throttle, strafe = input.strafe;
	aim(bot, self, point, {}, false, {}, input, seconds);
	input.throttle = throttle;
	input.strafe = strafe;
	L.quiet = false;
}

bool BotManager::turretLooks(const GameWorld::PlayerInfo& self) const
{
	if (!self.vehicle || m_settings.vehicle(equipmentOf(self)).sensingPattern != "Turret") {
		return false;
	}
	float yaw = 0, pitch = 0;
	if (!(self.seat > 0 || drivesGun(self)) || !m_world.gunAim(self.vehicle, self.seat, yaw, pitch)) {
		return false;
	}
	const std::vector<GameWorld::SeatLayout> seats = const_cast<GameWorld&>(m_world).vehicleSeats(self.vehicle);
	if (self.seat < 0 || static_cast<size_t>(self.seat) >= seats.size()) {
		return false;
	}
	return m_settings.plugin(aiTemplateOf(seats[static_cast<size_t>(self.seat)].name), "ControlInfo") != nullptr;
}

int BotManager::lookSector(int player) const
{
	const auto it = m_bots.find(player);
	return it == m_bots.end() ? -1 : it->second.look.sector;
}

bool BotManager::lookPlanned(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() && it->second.look.plan;
}

} // namespace dice::hfe::game
