// The gunship pilot's gun and strike (BBPHeliFire3d, AIDLL 0x1007f180 /
// 0x1007ec50), shared by the retail plan in BotManager::fly and by
// enhanced.botPilots' own hunt (not retail).
#include "Bots.h"

#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dice::hfe::game {

namespace {

ai::Vec3 aiVec(const physics::Vec3& v) { return {v.x, v.y, v.z}; }

physics::Vec3 flat(const physics::Vec3& v)
{
	const float l = std::sqrt(v.x * v.x + v.z * v.z);
	return l > 1e-5f ? physics::Vec3 {v.x / l, 0, v.z / l} : physics::Vec3 {0, 0, 1};
}

float horizontal(const physics::Vec3& v) { return std::sqrt(v.x * v.x + v.z * v.z); }

// As Bots.cpp's (file-local there).
std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

// A firearm's fire mode as it is picked up: the first fire.addFireRate, 2
// (automatic) without one (0x005f28a0; Bots.cpp's fireMode).
int fireMode(const world::ObjectTemplate* t)
{
	if (!t) {
		return 2;
	}
	const auto it = t->settings().find("fire.addFireRate.list");
	return it == t->settings().end() ? 2 : std::atoi(it->second.c_str());
}

} // namespace

// The pilot's weapon (0x100dd4a0, flags 7, from BBPHeliFire3d's plan
// 0x1007f9d0): of the seat's weapons with a strength above 0 against the
// target's CombatStrengthType, the one scoring most (strictly), the one in
// use counting twice (the plan's per-bot +0x14). None: the seat's weapon.
BotManager::PilotGun BotManager::pilotGunFor(Bot& bot, const GameWorld::PlayerInfo& self, int type)
{
	const auto seats = m_world.vehicleSeats(self.vehicle);
	PilotGun out;
	out.gun = seats.empty() ? std::string() : seats[0].weapon;
	if (!seats.empty() && !seats[0].weapons.empty()) {
		int best = -1;
		float bestScore = 0;
		for (size_t i = 0; i < seats[0].weapons.size(); ++i) {
			const GameWorld::SeatWeapon& w = seats[0].weapons[i];
			// Only the Armament's AI weapons (0x10031d80 skips a firearm
			// without a weaponTemplate): ads_air would score as a rifle.
			if (w.weapon.empty() || (!m_settings.weapons.empty() && !m_settings.weapons.count(lower(w.aiTemplate)))) {
				continue;
			}
			const WeaponAi ai = weaponAi(w.weapon);
			const float strength = type >= 0 && type < 8 ? ai.strength[static_cast<size_t>(type)] : 0;
			const float score = strength * (static_cast<int>(i) == bot.pilotWeapon ? 2.0f : 1.0f);
			if (strength > 0 && score > bestScore) {
				best = static_cast<int>(i);
				bestScore = score;
			}
		}
		if (best >= 0) {
			bot.pilotWeapon = best;
			out.gun = seats[0].weapons[static_cast<size_t>(best)].weapon;
			out.group = seats[0].weapons[static_cast<size_t>(best)].group;
		}
	}
	out.weapon = weaponAi(out.gun);
	return out;
}

bool BotManager::pilotStrike(Bot& bot, const GameWorld::VehicleInfo& vehicle, ai::HeliPilot& pilot, const PilotGun& pg,
	const physics::Vec3& point, const physics::Vec3& velocity, bool ground, float margin, float dt, float seconds,
	SoldierInput& input, PilotStats* stats)
{
	const physics::Vec3 at = vehicle.position;
	const physics::Vec3 to = point - at;
	const float length = std::sqrt(physics::lengthSquared(to));
	const physics::Vec3 dir = length > 0.01f ? to * (1 / length) : vehicle.forward;
	const WeaponAi& weapon = pg.weapon;
	const float maxRange = weapon.maxRange > 0 ? weapon.maxRange : 300.0f;
	// HeliAimAt's aim (0x100dcf10): the lead from the gun (the hull's
	// frame) for the round, the hull's motion inherited; an indirect
	// weapon's with precision 0.75, no inherited motion and the direction
	// laid flat. No solution: straight at the target (slot 8).
	bool indirect = false;
	const ai::RoundModel round = roundModel(pg.gun, &indirect);
	physics::Vec3 aimDir = dir;
	ai::Vec3 solved = aiVec(dir), impact;
	double impactTime = 0;
	bool hasSolution = false;
	if (round.speed > 0) {
		bot.heliLead.setup(aiVec(to), aiVec(velocity), indirect ? ai::Vec3 {} : aiVec(vehicle.velocity), aiVec(vehicle.forward),
			m_time, round);
		hasSolution = bot.heliLead.solve(m_time, indirect ? 0.75f : 0.5f, false, solved, impact, &impactTime);
		if (hasSolution) {
			aimDir = {solved.x, indirect ? 0.0f : solved.y, solved.z};
			aimDir = aimDir * (1 / std::max(std::sqrt(physics::lengthSquared(aimDir)), 1e-4f));
		}
	}
	pilot.aimAt(aiVec(aimDir), margin, bot.heliLastDirY, dt > 0 ? dt : seconds);
	bot.heliLastDirY = aimDir.y;
	// The trigger's condition (0x1007ec50), an And of: the target within
	// maxRange (0x100d4480, 3D); on target (0x100dcab0: the AI weapon's
	// allowedDeviation, slot 0x94, as the reach), waiting for the closest
	// approach only for a weapon that isn't automatic (slot 0x104) at a
	// target that isn't ITGround (its AI object's flag bit 3); and, for a
	// weapon with a fireRate (slot 0xc8), a timer. Not ported: an indirect
	// weapon (slot 0x108) at an ITGround target takes another test
	// (0x100dc850); no shipped pilot weapon is indirect.
	const world::ObjectTemplate* g = m_world.templates().find(pg.gun);
	const bool automatic = fireMode(g) == 2;
	const bool closest = !automatic && !ground;
	if (!bot.heliOnTarget || bot.heliOnTarget->closestApproach() != closest) {
		bot.heliOnTarget.emplace(weapon.allowedDeviation, closest);
	}
	const bool onTarget = length <= maxRange && bot.heliOnTarget->test(hasSolution, bot.heliLead, solved, impact, impactTime, m_time);
	if (stats) {
		stats->pass += seconds;
		stats->onTarget += onTarget ? seconds : 0.0f;
	}
	// A press (BAPATrigger 0x100d3110) for a weapon that isn't automatic, so
	// let go between taps; an automatic one is held.
	float spacing = 0;
	if (weapon.fireRate != -1) {
		const float taps = std::round((1 - (1 - m_settings.skill) * 0.8f) * std::round(1.5f * weapon.fireRate));
		spacing = taps > 0 ? 60.0f / taps : 1e9f;
	}
	const bool released = automatic || m_time - bot.heliTapAt > seconds * 1.5f;
	if (!onTarget || !released || m_time - bot.heliTapAt < spacing) {
		return false;
	}
	(pg.group == 1 ? input.altFire : input.fire) = true;
	bot.heliTapAt = m_time;
	if (stats) {
		++stats->presses;
		if (bot.pilotFirstPressFor != bot.seatedAt) {
			bot.pilotFirstPressFor = bot.seatedAt;
			stats->firstAttack += m_time - bot.seatedAt;
			++stats->firstAttacks;
		}
	}
	return true;
}

// enhanced.botPilots (not retail: BBPHeliFire3d's plan, 0x1007f180, flies its
// away-and-back moves whatever comes at it, and only UseFlareOnMissileLock,
// 0x100d1e80, answers a lock). An airborne gunship pilot
//   - with a round locked on its aircraft within 500 m, or 8 % of its hit
//     points lost within a second, breaks for 3.5 s: 200 m out, across the
//     threat (the round, else the nearest enemy that hit it in the last 2 s)
//     and away from it, down to 30 m under its height (25 m over the
//     ground at least); the flares are UseFlareOnMissileLock's as before;
//   - under 35 % of its hit points flies to the nearest supply object within
//     2500 m that mends it (the Titan's deck point, a helipad), and stays
//     there until it is back over 90 %; with none it goes on.
// Then the hunt sets up a run afresh.
bool BotManager::evade(Bot& bot, const GameWorld::PlayerInfo& self, const GameWorld::VehicleInfo& vehicle, ai::HeliPilot& pilot)
{
	const physics::Vec3 at = vehicle.position;
	const float health = healthOf(vehicle.object);
	bool hit = false;
	if (m_time - bot.lastHealthAt >= 1) {
		hit = bot.lastHealth >= 0 && bot.lastHealth - health >= 0.08f && m_time - bot.lastHealthAt < 2;
		bot.lastHealth = health;
		bot.lastHealthAt = m_time;
	}
	bool locked = false;
	physics::Vec3 from;
	float nearest = 500 * 500;
	for (const Projectile& p : m_world.projectiles()) {
		const float d2 = physics::lengthSquared(p.position - at);
		if (p.alive && p.lockedOn == vehicle.object && d2 < nearest) {
			nearest = d2;
			from = p.position;
			locked = true;
		}
	}
	if (!locked && hit) {
		float best = 1e18f;
		for (const Known& k : bot.known) {
			const GameWorld::PlayerInfo* p = m_time - k.alertAt < 2 ? playerOf(k.player) : nullptr;
			const physics::Vec3 where = p && k.lostAt < 0 ? p->position : k.position;
			if (p && p->alive && p->team != self.team && physics::lengthSquared(where - at) < best) {
				best = physics::lengthSquared(where - at);
				from = where;
			}
		}
		hit = best < 1e18f;
	}
	if (locked || hit) {
		if (m_time >= bot.evadeUntil) {
			bot.evadeSide = random() < 0.5f ? -1.0f : 1.0f;
			bot.evadeSince = m_time;
		}
		bot.evadeUntil = m_time + 3.5f;
		bot.evadeFrom = from;
	}
	bot.retreating = health < 0.35f || (bot.retreating && health < 0.9f);
	const auto terrain = [this](const physics::Vec3& p) { return m_world.terrainHeight(p.x, p.z); };
	const auto restart = [&] {
		bot.huntLeg = 0; // a run afresh after it
		bot.huntAway = false;
		bot.heliOnTarget.reset();
	};
	if (m_time < bot.evadeUntil) {
		const physics::Vec3 away = flat(at - bot.evadeFrom);
		const physics::Vec3 across {away.z * bot.evadeSide, 0, -away.x * bot.evadeSide};
		physics::Vec3 to = at + flat(away * 0.6f + across) * 200.0f;
		to.y = std::max(at.y - 30, terrain(to) + 25);
		pilot.flyTo(aiVec(to), 25);
		bot.flyState = "evade";
		restart();
		return true;
	}
	if (bot.retreating) {
		const GameWorld::VehicleSupply* best = nullptr;
		std::vector<GameWorld::VehicleSupply> supplies = m_world.vehicleSupplies(vehicle.object);
		for (const GameWorld::VehicleSupply& s : supplies) {
			if (horizontal(s.at - at) < 2500 && (!best || horizontal(s.at - at) < horizontal(best->at - at))) {
				best = &s;
			}
		}
		if (best) {
			// In at 50 m over it (up to that height 150 m off, outside a Titan's hull), then down onto it the
			// last 20 m.
			const float off = horizontal(best->at - at);
			physics::Vec3 to = best->at + physics::Vec3 {0, 50, 0};
			bot.retreatDown = off < 20 || (bot.retreatDown && off < 60);
			if (!bot.retreatDown && at.y < best->at.y + 35) {
				to = best->at + flat(at - best->at) * 150.0f + physics::Vec3 {0, 60, 0};
			} else if (bot.retreatDown) {
				to = best->at + physics::Vec3 {0, std::max(0.5f, 0.3f * best->reach), 0};
			}
			pilot.flyTo(aiVec(to), bot.retreatDown ? 0.0f : 30.0f);
			bot.flyState = std::sqrt(physics::lengthSquared(best->at - at)) < best->reach ? "retreat (mending)" : "retreat";
			restart();
			return true;
		}
	}
	return false;
}

// enhanced.botPilots (not retail). The original's pilot attacks only what
// BBFire3d hands it, and BBFire3d scores everything that can't fight a
// helicopter (most soldiers, every shipped vehicle) at a hundredth
// (0x1007d6c0), so it mostly flies its waypoints; and its attack runs
// (0x1007f180) close to 30 m, where the target lies too steeply below for
// HeliAimAt (dir.y -0.6 at most, 0x100e5590) and its gentle pitch hold
// (0x100e4440, scale 0.2) to line the hull up within atan(0.0125 x 6).
// Here the pilot hunts:
//   - every 2 s, of the enemies its side knows within 700 m and its guns
//     are rated against (not a Titan's gunners), the best strength over
//     1 + d / 250, x (1 + 0.5 per other enemy on foot within 20 m) for a
//     soldier, x 0.5 for an aircraft; in gpm_ti an enemy Titan's gun hubs
//     (their collision hangs below the force field, Titans.cpp) at 4, x 0.4
//     for one on the Titan's far side, x 0.1 for 20 s after a run on it
//     wasted five presses; every score x the target's threat (its kills of
//     the last minute, enhanced.botThreat) and x 1.5 by the team's fight
//     (within 100 m of the pilot's goal or a silo; aboard the enemy's Titan,
//     and its guns, while its shield is down); the current target kept unless
//     another scores 1.3 times more;
//   - the run: set up 0.9 x maxRange (120..280 m) off (or anywhere 0.5..1.2
//     of that with the nose on the target), at 0.2 of it above a ground
//     target (11 degrees down), level with an aircraft; for a hub straight
//     out from its Titan's middle (the hull behind it), 0.05 of it below
//     (at 0.2 below the nose, held by HeliAimAt's gentle pitch, lagged the
//     climbing line until the break-off: 9 hits in 600 s on Verdun), going
//     round the Titan rather than under it; then in at that height under
//     HeliAimAt and the retail trigger (pilotStrike), breaking off past
//     50 m (a hub: 0.35 stand-offs, then away from its Titan), a dive over
//     dir.y -0.55, a climb over 0.33, an overshoot, 30 s, 1.5 s crawling
//     under 4 m/s (the nose up, the hull stops under the hub) or five
//     presses on a hub with no loss, and setting up again.
bool BotManager::hunt(Bot& bot, const GameWorld::PlayerInfo& self, const GameWorld::VehicleInfo& vehicle, ai::HeliPilot& pilot,
	const std::string& rootAi, float dt, float seconds, SoldierInput& input)
{
	const physics::Vec3 at = vehicle.position;
	const int enemy = self.team == 1 ? 2 : 1;
	const auto seats = m_world.vehicleSeats(self.vehicle);
	if (seats.empty() || !m_world.insideCombatArea(self.id)) {
		return false;
	}
	// A new seat: a new hunt (no target, run or break-off carried over from the last).
	if (bot.huntSeating != bot.seatedAt) {
		bot.huntSeating = bot.seatedAt;
		bot.huntPlayer = -1;
		bot.huntPart = 0;
		bot.huntPickAt = bot.huntLegAt = -1e9f;
		bot.huntLeg = 0;
		bot.huntAway = false;
	}
	float bestStrength[8] = {};
	for (const GameWorld::SeatWeapon& w : seats[0].weapons) {
		if (w.weapon.empty() || (!m_settings.weapons.empty() && !m_settings.weapons.count(lower(w.aiTemplate)))) {
			continue;
		}
		const WeaponAi ai = weaponAi(w.weapon);
		for (size_t i = 0; i < 8; ++i) {
			bestStrength[i] = std::max(bestStrength[i], ai.strength[i]);
		}
	}
	// The candidates: the enemies the bot or its side knows, where it knows them.
	struct Candidate {
		int player = -1;
		uint32_t part = 0;
		physics::Vec3 position, velocity;
		int type = 0;
		bool ground = true, air = false;
		uint32_t vehicle = 0;
		float score = 0;
		physics::Vec3 titanAt; // a hub's: its Titan's middle
		float titanRadius = 0;
		float hitPoints = 0;
	};
	const auto knownAt = [&](const GameWorld::PlayerInfo& p, physics::Vec3& where) {
		for (const Known& k : bot.known) {
			if (k.player == p.id && k.everSeen && (k.lostAt < 0 || m_time - k.lostAt < 30)) {
				where = k.lostAt < 0 ? p.position : k.position;
				return true;
			}
		}
		return m_knowledge[self.team].count(p.id) && knownPosition(self.team, p.id, where);
	};
	const auto describe = [&](const GameWorld::PlayerInfo& p, Candidate& c) {
		c.player = p.id;
		c.velocity = p.velocity;
		c.vehicle = p.vehicle;
		c.type = std::clamp(strengthTypeOf(p), 0, 7);
		uint32_t types = 0;
		if (const world::Object* o = m_world.objects().find(p.vehicle ? p.vehicle : p.soldier); o && o->objectTemplate()) {
			types = m_settings.typeFlags(aiTemplateOf(o->objectTemplate()->name()));
		}
		c.ground = types ? (types & kITGround) != 0 : !p.vehicle;
		c.air = (types & kITAir) != 0;
	};
	// The scan: every candidate when the target is chosen (every 2 s), else the held one alone, kept up to date
	// (scored only for the choice).
	std::vector<Candidate> candidates;
	// The team's fight: the bot's own goal (its order's area, the commander's), a silo, the enemy's Titan while
	// its shield is down (the assault). An enemy within 100 m of one counts 1.5 times (one aboard the enemy's
	// Titan, and its guns, which shoot at the boarders' transports, too).
	std::vector<physics::Vec3> fronts;
	if (bot.hasGoal) {
		fronts.push_back(bot.goal);
	}
	for (const GameWorld::SiloInfo& silo : m_world.missileSilos()) {
		fronts.push_back(silo.position);
	}
	const GameWorld::TitanInfo* assault = nullptr;
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	for (const GameWorld::TitanInfo& t : titans) {
		assault = t.team == enemy && !t.wreck && !t.shield ? &t : assault;
	}
	const auto front = [&](const physics::Vec3& p) {
		if (assault && horizontal(p - assault->position) < 160) {
			return 1.5f;
		}
		for (const physics::Vec3& f : fronts) {
			if (horizontal(p - f) < 100) {
				return 1.5f;
			}
		}
		return 1.0f;
	};
	const auto scan = [&](bool all) {
		candidates.clear();
		for (const GameWorld::PlayerInfo& p : m_players) {
			physics::Vec3 where;
			if ((!all && p.id != bot.huntPlayer) || !p.alive || p.team != enemy || !knownAt(p, where) || horizontal(where - at) > 700) {
				continue;
			}
			Candidate c;
			describe(p, c);
			c.position = where + physics::Vec3 {0, p.vehicle ? 1.0f : 0.9f, 0};
			const float strength = bestStrength[c.type];
			if (strength <= 0 || c.type == 6) {
				continue; // nothing to hurt it with, or a Titan's gunner
			}
			float score = strength / (1 + horizontal(where - at) / 250);
			if (all && !p.vehicle) {
				for (const GameWorld::PlayerInfo& q : m_players) {
					score += q.id != p.id && q.alive && q.team == enemy && !q.vehicle
							&& physics::lengthSquared(q.position - p.position) < 20 * 20
						? 0.5f * strength / (1 + horizontal(where - at) / 250)
						: 0.0f;
				}
			}
			c.score = (c.air ? score * 0.5f : score) * threat(p.id) * front(where);
			candidates.push_back(c);
		}
		if (!all && !bot.huntPart) {
			return;
		}
		for (const GameWorld::TitanInfo& titan : titans) {
			if (titan.team != enemy || titan.wreck) {
				continue;
			}
			for (const GameWorld::TitanPartInfo& part : m_world.titanParts(enemy, titan.object)) {
				if ((!all && part.id != bot.huntPart) || part.kind != GameWorld::TitanPartInfo::Gun || part.wreck
					|| part.hitPoints <= 0 || horizontal(part.position - at) > 700) {
					continue;
				}
				Candidate c;
				c.part = part.id;
				c.position = part.position;
				c.velocity = titan.velocity;
				c.type = 6;
				c.titanAt = titan.position;
				c.titanRadius = titan.radius;
				c.hitPoints = part.hitPoints;
				c.ground = false; // hits it from below: no closest-approach wait wanted, but it is no ITGround object
				c.score = 4 / (1 + horizontal(part.position - at) / 250);
				// Its gunner's kills; a hub on the Titan's far side (the run would cross the hull) or one whose
				// last run found the line blocked counts for less.
				for (const auto& [gunner, hub] : m_titanGunners) {
					c.score *= hub == part.id ? threat(gunner) : 1.0f;
				}
				if (physics::dot(flat(part.position - titan.position), flat(at - titan.position)) < 0) {
					c.score *= 0.4f;
				}
				if (part.id == bot.huntSkipPart && m_time < bot.huntSkipUntil) {
					c.score *= 0.1f;
				}
				c.score *= assault ? 1.5f : 1.0f;
				candidates.push_back(c);
			}
		}
	};
	const Candidate* current = nullptr;
	const Candidate* best = nullptr;
	const auto find = [&] {
		current = best = nullptr;
		for (const Candidate& c : candidates) {
			if ((bot.huntPlayer >= 0 && c.player == bot.huntPlayer) || (bot.huntPart && c.part == bot.huntPart)) {
				current = &c;
			}
			if (!best || c.score > best->score) {
				best = &c;
			}
		}
	};
	bool choose = m_time >= bot.huntPickAt || (bot.huntPlayer < 0 && !bot.huntPart);
	scan(choose);
	find();
	if (!choose && !current) {
		choose = true; // the held target lost: the choice now
		scan(true);
		find();
	}
	if (choose) {
		bot.huntPickAt = m_time + 2;
		const Candidate* pick = current && best && best->score <= current->score * 1.3f ? current : best;
		if (pick != current) {
			bot.huntPlayer = pick ? pick->player : -1;
			bot.huntPart = pick ? pick->part : 0;
			bot.huntLeg = 0;
			bot.huntAway = false;
			bot.heliOnTarget.reset();
		}
		current = pick;
	}
	if (!current) {
		bot.huntPlayer = -1;
		bot.huntPart = 0;
		return false;
	}
	const Candidate& c = *current;
	PilotStats* stats = m_stats ? &m_pilotStats[rootAi] : nullptr;
	const char* kind = c.part ? "Titan gun" : c.air ? "aircraft" : c.type == 0 ? "infantry" : "vehicle";
	if (stats) {
		stats->hunting += seconds;
		stats->hunted[kind].first += seconds;
	}
	const PilotGun pg = pilotGunFor(bot, self, c.type);
	const float maxRange = pg.weapon.maxRange > 0 ? pg.weapon.maxRange : 200.0f;
	const float standOff = std::clamp(0.9f * maxRange, 120.0f, 280.0f);
	const float rise = c.part ? -0.05f * standOff : c.air ? 0.0f : 0.2f * standOff;
	const physics::Vec3 to = c.position - at;
	const float dist = horizontal(to);
	const auto terrain = [this](const physics::Vec3& p) { return m_world.terrainHeight(p.x, p.z); };
	if (bot.huntLeg == 0) {
		// Set up: the far side's stand-off point (seen from here), at the run's height.
		// A hub's run comes straight out from its Titan's middle through it, so the hull lies behind the hub.
		const physics::Vec3 out = c.part ? flat(c.position - c.titanAt) : flat(at - c.position);
		physics::Vec3 start = c.position + out * standOff;
		start.y = std::max(c.position.y + rise, terrain(start) + 20);
		// A hub's: round its Titan, not under it, when the way to the start passes its hull (some 40 m round).
		physics::Vec3 way = start;
		if (c.part) {
			const physics::Vec3 seg = start - at;
			const float l2 = std::max(seg.x * seg.x + seg.z * seg.z, 1.0f);
			const float k = std::clamp(((c.titanAt.x - at.x) * seg.x + (c.titanAt.z - at.z) * seg.z) / l2, 0.0f, 1.0f);
			const physics::Vec3 near = at + seg * k;
			if (horizontal(near - c.titanAt) < c.titanRadius + 40) {
				physics::Vec3 side = flat(near - c.titanAt);
				if (horizontal(near - c.titanAt) < 1) {
					side = flat(physics::Vec3 {seg.z, 0, -seg.x});
				}
				way = c.titanAt + side * (c.titanRadius + 80);
				way.y = start.y;
			}
		}
		pilot.flyTo(aiVec(way), 20);
		bot.flyState = "hunt set up";
		if (stats) {
			stats->huntLegs[0] += seconds;
		}
		const physics::Vec3 ahead = flat(vehicle.forward);
		// Placed at the start, or anywhere 0.5..1.2 stand-offs off with the
		// nose on it (the run's height hold makes up the height). After a
		// break-off, not before it has been out of that band or turned its
		// nose away, or 10 s on: a run timed out there went straight back
		// in, never breaking off.
		const bool lined = dist > 0.5f * standOff && dist < 1.2f * standOff && physics::dot(flat(to), ahead) > 0.85f
			&& (!c.part || physics::dot(flat(at - c.position), out) > 0.8f);
		bot.huntAway = bot.huntAway && lined && m_time - bot.huntLegAt < 10;
		const bool placed = !bot.huntAway && (horizontal(start - at) < 40 || lined);
		if (placed) {
			bot.huntLeg = 1;
			bot.huntLegAt = m_time;
			bot.huntHeight = start.y;
			bot.heliOnTarget.reset();
			bot.huntSlowSince = -1;
			bot.huntPartHitPoints = c.hitPoints;
			bot.huntPartPresses = 0;
		}
		return true;
	}
	const float length = std::sqrt(physics::lengthSquared(to));
	const physics::Vec3 dir = length > 0.01f ? to * (1 / length) : vehicle.forward;
	// A run in that crawls (HeliAimAt holds the height and the nose, not the speed: against a hub above, the
	// nose comes up and the hull stops) breaks off after 1.5 s under 4 m/s, as does one within 0.35 stand-offs
	// of a hub, where it would hang under the Titan's hull. At a hub, five presses with no loss of its hit
	// points (the rounds meet the hull, another hub, the forcefield) break it off too, and the hub is passed
	// over for 20 s.
	bot.huntSlowSince = horizontal(vehicle.velocity) >= 4 ? -1.0f : bot.huntSlowSince < 0 ? m_time : bot.huntSlowSince;
	const bool stalled = bot.huntSlowSince >= 0 && m_time - bot.huntSlowSince > 1.5f;
	if (c.part && c.hitPoints < bot.huntPartHitPoints) {
		bot.huntPartHitPoints = c.hitPoints;
		bot.huntPartPresses = 0;
	}
	const bool wasted = c.part && bot.huntPartPresses >= 5;
	if (wasted) {
		bot.huntSkipPart = c.part;
		bot.huntSkipUntil = m_time + 20;
		bot.huntPickAt = m_time; // choose again
	}
	if (dist < (c.part ? std::max(50.0f, 0.35f * standOff) : 50.0f) || dir.y < -0.55f || dir.y > 0.33f
		|| physics::dot(flat(to), flat(vehicle.forward)) < 0.2f || m_time - bot.huntLegAt > 30 || stalled || wasted) {
		bot.huntSlowSince = -1;
		bot.huntLeg = 0; // break off: on and away, then set up again
		bot.huntLegAt = m_time;
		bot.huntAway = true;
		// Away from a hub's Titan, not on under its hull.
		const physics::Vec3 away = c.part ? flat(at - c.titanAt) : flat(vehicle.forward);
		pilot.flyTo(aiVec(at + away * 150.0f + physics::Vec3 {0, c.part ? -10.0f : 15.0f, 0}), 25);
		bot.flyState = "hunt break off";
		return true;
	}
	// In at the run's height: HeliAimAt holds it over the terrain ahead.
	const float margin = std::max(10.0f, bot.huntHeight - terrain(at));
	bool seenThrough = false;
	const bool clear = c.part
		|| (m_world.lineOfSight(at + vehicle.forward * 4.0f, c.position, vehicle.object, seenThrough, c.vehicle) && !seenThrough);
	if (stats) {
		stats->huntLegs[clear ? 1 : 2] += seconds;
	}
	if (!clear) {
		bot.flyState = "hunt in (blocked)";
		pilot.flyTo(aiVec(physics::Vec3 {c.position.x, bot.huntHeight, c.position.z}), margin);
		return true;
	}
	bot.flyState = "hunt in";
	if (pilotStrike(bot, vehicle, pilot, pg, c.position, c.velocity, c.ground, margin, dt, seconds, input, stats)) {
		bot.huntPartPresses += c.part ? 1 : 0;
		if (stats) {
			++stats->hunted[kind].second;
		}
	}
	return true;
}

// $BF2142_BOT_STATS's fight counts (not retail; see FightStats).
void BotManager::fightStats(float seconds)
{
	FightStats& f = m_fight;
	m_world.noteDamage(true);
	const auto gunship = [&](uint32_t vehicle) {
		const world::Object* o = m_world.objects().find(vehicle);
		if (!o || !o->objectTemplate() || m_world.isTitan(vehicle)) {
			return false;
		}
		const std::string rootAi = aiTemplateOf(o->objectTemplate()->name());
		if (!(m_settings.typeFlags(rootAi) & kITAir) || !m_settings.plugin(rootAi, "Armament")) {
			return false;
		}
		const auto seats = m_world.vehicleSeats(vehicle);
		return !seats.empty() && !seats[0].podLauncher
			&& std::none_of(seats[0].weapons.begin(), seats[0].weapons.end(), [](const auto& w) { return w.podLauncher; });
	};
	const auto who = [&](const GameWorld::PlayerInfo& p) -> std::string {
		if (!p.vehicle) {
			return "soldier";
		}
		if (m_world.isTitan(p.vehicle)) {
			return "Titan gun";
		}
		if (gunship(p.vehicle)) {
			return p.seat == 0 ? "gunship pilot" : "gunship gunner";
		}
		return "other vehicle";
	};
	// The Titan guns: down, back, and who aims at them.
	std::map<std::pair<uint32_t, int>, uint32_t> seatHub;
	std::map<uint32_t, uint32_t> hubTitan;
	std::map<uint32_t, int> partKind;
	for (const GameWorld::TitanInfo& titan : m_world.titans()) {
		for (const GameWorld::TitanPartInfo& part : m_world.titanParts(titan.team, titan.object)) {
			partKind[part.id] = part.kind;
			if (part.kind != GameWorld::TitanPartInfo::Gun) {
				continue;
			}
			hubTitan[part.id] = titan.object;
			if (part.seat >= 0) {
				seatHub[{titan.object, part.seat}] = part.id;
			}
			const bool down = part.wreck || part.hitPoints <= 0;
			const auto it = f.gunDownAt.find(part.id);
			if (down && it == f.gunDownAt.end() && !titan.wreck) {
				f.gunDownAt[part.id] = m_time;
				++f.gunsDown;
			} else if (!down && it != f.gunDownAt.end()) {
				f.downtime += m_time - it->second;
				++f.gunsRepaired;
				f.gunDownAt.erase(it);
			}
		}
	}
	// Kills, by where the killer sits.
	std::map<int, const GameWorld::PlayerInfo*> byId;
	std::map<uint32_t, const GameWorld::PlayerInfo*> bySoldier;
	for (const GameWorld::PlayerInfo& p : m_players) {
		byId[p.id] = &p;
		bySoldier[p.soldier] = &p;
		int& last = f.lastKills[p.id];
		if (p.kills > last) {
			f.kills[who(p)] += p.kills - last;
			if (p.vehicle && m_world.isTitan(p.vehicle)) {
				for (int k = last; k < p.kills; ++k) {
					f.gunKillTimes[{p.vehicle, p.seat}].push_back(m_time);
				}
			}
		}
		last = p.kills;
	}
	// Hits, by who on what.
	std::map<uint32_t, bool> vehicles; // object -> an aircraft
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		const world::Object* o = m_world.objects().find(v.object);
		vehicles[v.object] = o && o->objectTemplate() && (m_settings.typeFlags(aiTemplateOf(o->objectTemplate()->name())) & kITAir);
	}
	for (const GameWorld::DamageNote& n : m_world.takeDamageNotes()) {
		const auto by = byId.find(n.attacker);
		if (by == byId.end()) {
			continue;
		}
		std::string what = "other";
		if (const auto k = partKind.find(n.object); k != partKind.end()) {
			what = k->second == GameWorld::TitanPartInfo::Gun ? "Titan gun" : "Titan (other part)";
		} else if (m_world.isTitan(n.object)) {
			what = "Titan (other part)";
		} else if (const auto v = vehicles.find(n.object); v != vehicles.end()) {
			what = v->second ? "aircraft" : "vehicle";
		} else if (bySoldier.count(n.object)) {
			what = "infantry";
		}
		auto& h = f.hits[who(*by->second) + " > " + what];
		++h.first;
		h.second += n.amount;
	}
	// The gunships' flight, their gunners; the bots aiming at Titan guns.
	std::map<std::pair<uint32_t, int>, int> aimers; // (Titan, seat) -> enemy bots aiming at it
	for (auto& [id, bot] : m_bots) {
		const auto self = byId.find(id);
		if (self == byId.end() || !self->second->alive) {
			bot.flyState = "";
			continue;
		}
		const GameWorld::PlayerInfo& p = *self->second;
		if (p.vehicle && gunship(p.vehicle)) {
			if (p.seat == 0) {
				f.gunshipFlight[*bot.flyState ? std::string(bot.flyState) : "not flying: " + bot.behaviour] += seconds;
			} else {
				f.gunnerSeated += seconds;
				f.gunnerFiring += bot.lastInput.fire || bot.lastInput.altFire ? seconds : 0.0f;
			}
		}
		if (!p.vehicle || p.seat != 0) {
			bot.flyState = ""; // (a pilot not run this tick keeps its last)
		}
		if (bot.huntPart && hubTitan.count(bot.huntPart)) {
			for (const auto& [key, hub] : seatHub) {
				aimers[key] += hub == bot.huntPart;
			}
		}
		if (bot.target >= 0) {
			if (const auto t = byId.find(bot.target); t != byId.end() && t->second->alive && t->second->vehicle
				&& m_world.isTitan(t->second->vehicle) && t->second->team != p.team) {
				++aimers[{t->second->vehicle, t->second->seat}];
			}
		}
	}
	for (const GameWorld::PlayerInfo& p : m_players) {
		if (!p.alive || !p.vehicle || !m_world.isTitan(p.vehicle) || p.seat < 0) {
			continue;
		}
		const std::pair<uint32_t, int> key {p.vehicle, p.seat};
		if (const auto hub = seatHub.find(key); hub != seatHub.end() && f.gunDownAt.count(hub->second)) {
			continue; // a wreck's gunner fires nothing
		}
		const auto times = f.gunKillTimes.find(key);
		const long recent = times == f.gunKillTimes.end()
			? 0
			: std::count_if(times->second.begin(), times->second.end(), [&](float t) { return m_time - t < 60; });
		const auto a = aimers.find(key);
		const int n = a == aimers.end() ? 0 : a->second;
		f.gunSeconds += seconds;
		f.gunTargeted += static_cast<float>(n) * seconds;
		if (recent >= 2) {
			f.busyGunSeconds += seconds;
			f.busyTargeted += static_cast<float>(n) * seconds;
		}
	}
}

std::string BotManager::fightStatsReport() const
{
	const FightStats& f = m_fight;
	std::string out = "fight stats:\n";
	char line[256];
	float flown = 0;
	for (const auto& [state, s] : f.gunshipFlight) {
		flown += s;
	}
	for (const auto& [state, s] : f.gunshipFlight) {
		std::snprintf(line, sizeof line, "  gunship pilot %-34s %7.1f s (%4.1f %%)\n", state.c_str(), s, flown > 0 ? 100 * s / flown : 0.0f);
		out += line;
	}
	std::snprintf(line, sizeof line, "  gunship gunners: seated %.1f s, firing %.1f s\n", f.gunnerSeated, f.gunnerFiring);
	out += line;
	for (const auto& [key, h] : f.hits) {
		std::snprintf(line, sizeof line, "  hits %-44s %6d, %9.0f damage\n", key.c_str(), h.first, h.second);
		out += line;
	}
	for (const auto& [key, k] : f.kills) {
		std::snprintf(line, sizeof line, "  kills by %-20s %5d\n", key.c_str(), k);
		out += line;
	}
	float open = 0;
	for (const auto& [part, at] : f.gunDownAt) {
		open += m_time - at;
	}
	std::snprintf(line, sizeof line,
		"  Titan guns: %d wrecked, %d repaired (mean %.1f s down), %zu still down at the end (%.1f s so far)\n", f.gunsDown,
		f.gunsRepaired, f.gunsRepaired ? f.downtime / static_cast<float>(f.gunsRepaired) : -1.0f, f.gunDownAt.size(), open);
	out += line;
	std::snprintf(line, sizeof line,
		"  Titan guns manned %.1f s, %.2f enemy bots aiming at one on average; busy (2+ kills in 60 s) %.1f s, %.2f aiming\n",
		f.gunSeconds, f.gunSeconds > 0 ? f.gunTargeted / f.gunSeconds : 0.0f, f.busyGunSeconds,
		f.busyGunSeconds > 0 ? f.busyTargeted / f.busyGunSeconds : 0.0f);
	out += line;
	return out;
}

} // namespace dice::hfe::game
