// Fall damage on the Titans' decks and out of the drop pods (needs
// $BF2142_GAME_DIR): backlog P-1 and P-2.
#include "TitanLevel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <tuple>
#include <utility>

using namespace dice::hfe;
using titantest::TitanLevel;

namespace {

std::pair<physics::Vec3, physics::Mat3> frameOf(TitanLevel& level, uint32_t object)
{
	const world::Object* o = level.objects.find(object);
	return {physics::Vec3 {o->position().x, o->position().y, o->position().z},
		physics::Mat3::fromYawPitchRoll({o->rotation().x, o->rotation().y, o->rotation().z})};
}

struct Fall {
	float speed = 0, hurt = 0;
	bool alive = true;
};

// The soldier of `me` let go `height` over `feet` (world), at rest: his
// fastest fall (his own velocity, relative to a Titan carrying him) and
// what the landing took.
Fall dropAt(TitanLevel& level, int me, const physics::Vec3& feet, float height)
{
	Fall f;
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(me, info);
	if (!info.alive) {
		level.world->spawnSoldier(me);
		level.world->playerInfo(me, info);
	}
	level.world->setHitPoints(info.soldier, 100);
	level.world->setSoldierPosition(info.soldier, feet + physics::Vec3 {0, height, 0});
	for (int tick = 0; tick < 75; ++tick) {
		level.run(1 / 30.0f);
		level.world->playerInfo(me, info);
		if (!info.alive) {
			break;
		}
		f.speed = std::max(f.speed, -level.world->soldierBody(me)->velocity.y);
	}
	f.alive = info.alive;
	f.hurt = 100 - (info.alive ? level.world->hitPoints(info.soldier) : 0.0f);
	return f;
}

} // namespace

// P-2: a drop onto a Titan's deck is the road kill (0x005a4690 ->
// 0x005a43c0: the decks are the root's mesh, exterior_back, whose node is
// mobile), not the ground's rule (0x005a3db0). Under 10 m/s nothing (3 m/s
// only for an object faster than the soldier, and a Titan does 1.1 m/s at
// most: kPush 0.33 under 0.99 damping), then (v - 10) x v x the mass
// factor (0x005a2ca0: 1e6 over 200, at most 3), with no angle and no
// material cell; the ground's is (v - 9.1) x v x the cell's damageMod x
// the angle. The speed is the soldier's own (his node's +0x20, 0x006c99a0),
// which is relative to a Titan carrying him (0x006ca3b0 adds the carry
// apart), and the contact's (0x006c3890 leaves the carrier's point velocity
// out), so a moving Titan's drop does what a still one's does. With gravity
// 14.73 the 4.5 m from the rear's upper deck to its lower one meets the
// deck at about 11 m/s: about 40 on the deck, 18 on the ground. The
// enhanced.softLanding option gives the deck the ground's rule.
TEST(Titan_DeckDropIsTheRoadKillRule)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	float stillHurt[2] = {};
	for (const bool move : {false, true}) {
		for (const bool soft : {false, true}) {
			TitanLevel level(dir, "Minsk", 48);
			if (!level.loaded) {
				return;
			}
			bool on = true;
			level.world->setSoftLanding(&soft, &on);
			if (move) {
				CHECK(level.world->moveTitan(1, 300, -200) == game::GameWorld::TitanMove::Accepted);
				level.run(10);
				CHECK(physics::lengthSquared(level.titan(1)->velocity) > 0.3f * 0.3f);
			}
			const uint32_t pac = level.titan(1)->object;
			const int me = level.world->addPlayer("Me", 1, false);
			// The upper rear deck (11 m over the origin, 52-68 m aft) and
			// flat ground far off.
			auto [at, turn] = frameOf(level, pac);
			const Fall deck = dropAt(level, me, at + turn.transform({0, 11.0f, -58}), 4.5f);
			const physics::Vec3 g {at.x + 200, 0, at.z + 200};
			const Fall ground = dropAt(level, me, {g.x, level.world->terrainHeight(g.x, g.z), g.z}, 4.5f);
			const float roadKill = 3 * (deck.speed - 10) * deck.speed;
			const float groundRule = (ground.speed - 9.1f) * ground.speed;
			std::fprintf(stderr, "  Titan %s (%.1f m/s), softLanding %d: 4.5 m onto the deck %.1f m/s, hurt %.0f (road kill %.0f); ground %.1f m/s, hurt %.0f ((v - 9.1) v %.0f)\n",
				move ? "moving" : "still", std::sqrt(physics::lengthSquared(level.titan(1)->velocity)), soft, deck.speed, deck.hurt, roadKill,
				ground.speed, ground.hurt, groundRule);
			CHECK(deck.speed > 10.5f && deck.speed < 12);
			CHECK(std::fabs(ground.hurt - groundRule) < 2);
			if (!soft) {
				CHECK(std::fabs(deck.hurt - roadKill) < 2);
				CHECK(deck.hurt > ground.hurt * 1.5f);
				stillHurt[move ? 1 : 0] = deck.hurt;
			} else {
				CHECK(deck.hurt < ground.hurt); // the ground's rule with the deck's (metal) cell
			}
		}
	}
	CHECK(std::fabs(stillHurt[0] - stillHurt[1]) < 2); // the Titan's motion adds nothing
}

namespace {

struct PodRide {
	bool rode = false, left = false, alive = false;
	float exitHurt = 0, hurt = 0;
	physics::Vec3 exitVelocity;    // the rider's own the tick he is out (the Titan's frame)
	physics::Vec3 exitFromPod;     // his feet from where the pod was, the world's
	physics::Vec3 exitAt, restAt;  // his feet out of the pod and 3 s on (the Titan's frame)
	float titanSpeed = 0;
};

// An EU APC's pod seat 40 m over `spot` (the Titan's frame): the pod
// launched, pushed at `velocity` (the Titan's frame) and ridden down.
PodRide podRide(TitanLevel& level, uint32_t titan, const physics::Vec3& spot, const physics::Vec3& velocity)
{
	PodRide r;
	auto [p, turn] = frameOf(level, titan);
	const physics::Vec3 at = p + turn.transform(spot);
	world::Object* apc = level.objects.create(level.templates.find("eu_apc_pods"), {at.x, at.y, at.z});
	apc->setTeam(2);
	const uint32_t apcId = apc->id();
	level.run(1 / 30.0f);
	const int player = level.world->addPlayer("Pod rider", 2, false);
	level.world->setSpawnVehicle(player, apcId);
	if (!level.world->spawnSoldier(player)) {
		return r;
	}
	const auto seats = level.world->vehicleSeats(apcId);
	int seat = -1;
	for (size_t i = 0; i < seats.size(); ++i) {
		std::string name = seats[i].name;
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (name.find("podcannon") != std::string::npos) {
			seat = static_cast<int>(i);
			break;
		}
	}
	game::SoldierInput input;
	input.switchSeat = seat + 1;
	level.world->setInput(player, input);
	level.run(1 / 30.0f);
	level.world->setInput(player, {});
	level.run(1 / 30.0f);
	if (seat < 0 || !level.world->launchPod(player) || level.world->pods().empty()) {
		return r;
	}
	r.rode = true;
	const uint32_t pod = level.world->pods().back().object;
	level.world->setBodyVelocity(pod, turn.transform(velocity));
	level.objects.remove(apcId);
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(player, info);
	const float hp = level.world->hitPoints(info.soldier);
	physics::Vec3 podAt;
	for (int tick = 0; tick < 300 && level.world->ridingPod(player); ++tick) {
		for (const auto& pi : level.world->pods()) {
			if (pi.object == pod) {
				podAt = pi.position;
			}
		}
		r.titanSpeed = std::sqrt(physics::lengthSquared(level.titan(1)->velocity));
		level.run(1 / 30.0f);
	}
	r.left = !level.world->ridingPod(player);
	level.world->playerInfo(player, info);
	std::tie(p, turn) = frameOf(level, titan);
	if (const physics::SoldierBody* body = info.alive ? level.world->soldierBody(player) : nullptr) {
		r.exitVelocity = turn.untransform(body->velocity);
		r.exitFromPod = info.position - podAt;
		r.exitAt = turn.untransform(info.position - p);
	}
	r.exitHurt = hp - (info.alive ? level.world->hitPoints(info.soldier) : 0.0f);
	level.run(3);
	level.world->playerInfo(player, info);
	std::tie(p, turn) = frameOf(level, titan);
	r.alive = info.alive;
	r.restAt = turn.untransform(info.position - p);
	r.hurt = hp - (info.alive ? level.world->hitPoints(info.soldier) : 0.0f);
	level.world->removePlayer(player);
	return r;
}

} // namespace

// P-1 and P-2's pod exit: drop pods landing on the enemy PAC Titan's roof
// (its shield just down, the Titan coasting to a stop: 0x005aca70 stops
// the push under 60 %) from several directions. The rider comes out where
// the pod was, 0.5 m up (0x005cea00's place: 0x005cc9a0 -> 0x005c71a0 with
// the pod's soldierExitLocation, none), taking nothing at the exit. Retail
// hands him the pod's own velocity (0x005cfd70: the pod node's slot 0x58
// into his node's slot 0x5c, 0x006c5a90, after the 0.05 s landing timer,
// 0x005d01c0), and a pod that came in at a slant is still sliding over the
// deck then, so its rider goes on along the deck (here up to 5 m/s, 12-17
// m/s for a pod sliding down the rear deck): off the edge
// from near it. With enhanced.podExitAtRest he gets out at rest on the deck
// and every rider stays there, unhurt.
TEST(Titan_PodRidersStayOnTheDeck)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	for (const bool rest : {false, true}) {
		TitanLevel level(dir, "Minsk", 48);
		if (!level.loaded || !level.templates.find("eu_apc_pods")) {
			return;
		}
		const bool yes = true;
		level.world->setPodExitAtRest(&rest, &yes);
		CHECK(level.world->moveTitan(1, 300, -200) == game::GameWorld::TitanMove::Accepted);
		level.run(10);
		const auto pac = *level.titan(1);
		level.world->setHitPoints(pac.object, pac.hitPoints * 0.55f);
		level.run(0.2f);
		int flung = 0, fell = 0, rides = 0;
		for (const physics::Vec3 spot : {physics::Vec3 {0, 40, -30}, {8, 40, -20}}) {
			for (const physics::Vec3 v : {physics::Vec3 {0, -30, 0}, {0, -20, 20}, {0, -20, -20}, {20, -20, 0}, {-20, -20, 0}, {0, -40, 30}}) {
				const PodRide r = podRide(level, pac.object, spot, v);
				CHECK(r.rode && r.left);
				if (!r.rode || !r.left) {
					continue;
				}
				++rides;
				const float along = std::sqrt(r.exitVelocity.x * r.exitVelocity.x + r.exitVelocity.z * r.exitVelocity.z);
				// He stays: alive, unhurt, on the Titan (its lowest ledges are
				// 6.5 m over its origin) within a metre and a half of where he
				// got out.
				const physics::Vec3 moved = r.restAt - r.exitAt;
				const bool stayed = r.alive && r.hurt == 0 && r.restAt.y > 5 && moved.x * moved.x + moved.z * moved.z < 1.5f * 1.5f;
				std::fprintf(stderr,
					"  podExitAtRest %d, over %2.0f/%3.0f at %3.0f/%3.0f/%3.0f (Titan %.2f m/s): out %.2f/%.2f/%.2f from the pod at %4.1f m/s along the deck, "
					"at %5.1f/%5.1f/%5.1f, 3 s on at %5.1f/%5.1f/%5.1f, hurt %3.0f%s\n",
					rest, spot.x, spot.z, v.x, v.y, v.z, r.titanSpeed, r.exitFromPod.x, r.exitFromPod.y, r.exitFromPod.z, along, r.exitAt.x, r.exitAt.y,
					r.exitAt.z, r.restAt.x, r.restAt.y, r.restAt.z, r.hurt, stayed ? "" : r.alive ? " MOVED OFF" : " DEAD");
				// Where the pod was (sampled a tick before it went), 0.5 m up:
				// the shipped pods' place. Nothing taken at the exit.
				CHECK(std::fabs(r.exitFromPod.x) < 0.2f && std::fabs(r.exitFromPod.z) < 0.2f && std::fabs(r.exitFromPod.y - 0.5f) < 0.15f);
				CHECK(r.exitHurt == 0);
				flung += along > 4 ? 1 : 0;
				fell += stayed ? 0 : 1;
				if (rest) {
					CHECK(along < 0.01f);
					CHECK(stayed);
				}
			}
		}
		std::fprintf(stderr, "  podExitAtRest %d: %d of %d riders left their pod faster than 4 m/s along the deck, %d didn't stay on it\n", rest, flung,
			rides, fell);
		CHECK(rides == 12);
		if (!rest) {
			CHECK(flung >= 2 && fell >= 1); // retail's: the pod's slide carries over
		}
	}
}
