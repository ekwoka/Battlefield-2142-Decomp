// --director (Game/Director.h): the interest scores, the shot director's
// rules (minimum and maximum shot length, the early cut off an idle
// subject, the hold on a death, hysteresis, variety) and the camera's
// smoothing, on hand-made samples. The bots' own round is in BotsTest.cpp
// (Director_CutsFromIdleBotToFightingOne).
#include "Game/Director.h"
#include "TestMain.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace dice::hfe;
using game::director::CameraMode;
using game::director::Cut;
using game::director::Sample;
using game::director::ShotDirector;

namespace {

Sample living(int player)
{
	Sample s;
	s.player = player;
	s.alive = true;
	s.team = 1 + player % 2;
	s.aliveFor = 60;
	s.speed = 3;
	return s;
}

Sample fighting(int player)
{
	Sample s = living(player);
	s.hasTarget = s.enemyInRange = true;
	s.sinceFired = 0.2f;
	s.sinceHit = 1;
	return s;
}

Sample idling(int player, float seconds)
{
	Sample s = living(player);
	s.speed = 0;
	s.idleBehaviour = true;
	s.idleFor = seconds;
	return s;
}

// The director run over the samples `at` gives for each time, every 0.5 s
// from 0 to `until`; the cuts made.
template <typename F>
std::vector<Cut> run(ShotDirector& shots, float until, F at, float from = 0)
{
	std::vector<Cut> cuts;
	for (float t = from; t <= until + 1e-4f; t += 0.5f) {
		if (auto cut = shots.update(t, at(t))) {
			cuts.push_back(*cut);
		}
	}
	return cuts;
}

} // namespace

TEST(Director_InterestRaisesFightsAndLowersIdling)
{
	using game::director::interest;
	const Sample base = living(1);
	const Sample fight = fighting(2);
	CHECK(interest(fight) > interest(base) + 5);
	CHECK(game::director::engaged(fight) && !game::director::engaged(base));
	// Each of the world's raisers.
	Sample kill = base;
	kill.sinceKill = 1;
	CHECK(interest(kill) > interest(base) + 3);
	Sample capture = base;
	capture.capturing = true;
	CHECK(interest(capture) > interest(base));
	Sample titan = base;
	titan.titanAssault = true;
	Sample core = titan;
	core.titanCore = true;
	CHECK(interest(titan) > interest(base) && interest(core) > interest(titan));
	Sample air = fight;
	air.vehicle = 7;
	air.flying = true;
	Sample tank = fight;
	tank.vehicle = 8;
	CHECK(interest(air) > interest(tank) && interest(tank) > interest(fight));
	// The lowerers: idling, a seat with nothing to shoot, the spawn, a gun of its own Titan.
	CHECK(interest(idling(3, 6)) < interest(base) - 1);
	CHECK(interest(idling(3, 9)) < interest(idling(3, 3)));
	Sample seat = base;
	seat.vehicle = 9;
	seat.seat = 2;
	seat.speed = 0;
	CHECK(interest(seat) < interest(base));
	Sample spawn = base;
	spawn.inSpawn = true;
	CHECK(interest(spawn) < interest(base));
	Sample gunner = tank;
	gunner.ownTitan = true;
	CHECK(interest(gunner) < interest(tank));
	Sample dead = fight;
	dead.alive = false;
	CHECK(interest(dead) == 0 && !game::director::engaged(dead));
	std::printf("  interest: base %.1f, fight %.1f, aircraft fight %.1f, idle 6 s %.2f, in spawn %.1f\n", interest(base),
		interest(fight), interest(air), interest(idling(3, 6)), interest(spawn));
}

// At least minShot (6 s) before a better subject takes over; at most maxShot (20 s) on one.
TEST(Director_ShotLengthsMinAndMax)
{
	ShotDirector shots;
	// Bot 1 fights first; bot 2 starts a far bigger fight at 1 s.
	auto cuts = run(shots, 12, [](float t) {
		Sample two = t >= 1 ? fighting(2) : living(2);
		if (t >= 1) {
			two.sinceKill = 0.5f;
			two.sinceDamage = 0.5f;
		}
		return std::vector<Sample> {fighting(1), two};
	});
	CHECK(cuts.size() == 2);
	CHECK(cuts.size() == 2 && cuts[0].subject == 1 && cuts[0].reason == "start");
	CHECK(cuts.size() == 2 && cuts[1].subject == 2 && cuts[1].reason == "better" && cuts[1].time == 6.0f);
	// Scores that never beat the subject by the margin: the shot ends at 20 s anyway.
	ShotDirector steady;
	cuts = run(steady, 45, [](float) { return std::vector<Sample> {fighting(1), fighting(3)}; });
	CHECK(cuts.size() == 3);
	CHECK(cuts.size() == 3 && cuts[1].reason == "max" && cuts[1].time == 20.0f && cuts[1].subject == 3);
	CHECK(cuts.size() == 3 && cuts[2].reason == "max" && cuts[2].time == 40.0f && cuts[2].subject == 1);
}

// A subject idle for a few seconds is left early (before the 6 s minimum) for a busier one.
TEST(Director_CutsEarlyOffAnIdleSubject)
{
	ShotDirector shots;
	auto cuts = run(shots, 10, [](float t) {
		// Bot 1 is the best at the start, then stands about; bot 2 starts shooting at 0.5 s.
		Sample one = t < 0.25f ? fighting(1) : idling(1, t);
		Sample two = t < 0.25f ? living(2) : fighting(2);
		two.sinceFired = 1.5f; // not quite bot 1's start
		return std::vector<Sample> {one, two};
	});
	CHECK(cuts.size() >= 2);
	CHECK(cuts.size() >= 2 && cuts[1].subject == 2 && cuts[1].reason == "idle");
	CHECK(cuts.size() >= 2 && cuts[1].time >= shots.settings().idleCut && cuts[1].time < shots.settings().minShot);
}

// The subject dies: its death view holds deathHold (2 s), then the cut.
TEST(Director_HoldsOnADeath)
{
	ShotDirector shots;
	auto cuts = run(shots, 14, [](float t) {
		Sample one = fighting(1);
		one.sinceKill = 0;
		if (t >= 8) {
			one.alive = false;
		}
		return std::vector<Sample> {one, living(2)};
	});
	CHECK(cuts.size() == 2);
	CHECK(cuts.size() == 2 && cuts[0].subject == 1);
	CHECK(cuts.size() == 2 && cuts[1].subject == 2 && cuts[1].reason == "death" && cuts[1].time == 10.0f);
	// While it holds, the director says so and keeps the subject.
	ShotDirector held;
	run(held, 9, [](float t) {
		Sample one = fighting(1);
		one.alive = t < 8;
		return std::vector<Sample> {one, fighting(2)};
	});
	CHECK(held.subject() == 1 && held.holdingDeath());
}

// Hysteresis: a candidate a little better than the subject doesn't take over; a clearly better one does.
TEST(Director_HysteresisKeepsTheSubjectAgainstACloseCandidate)
{
	// Both fight; bot 1 also captures (the subject from the start). From 2 s bot 2
	// assaults a Titan: 0.5 more than bot 1, within the margin (2, or a quarter
	// of bot 1's interest), so the shot runs to its 20 s end.
	ShotDirector shots;
	std::vector<Cut> cuts = run(shots, 18, [](float t) {
		Sample one = fighting(1);
		one.capturing = true;
		Sample two = fighting(2);
		if (t >= 2) {
			two.titanAssault = true; // +2.5 over one's +2: within the margin
		}
		return std::vector<Sample> {one, two};
	});
	CHECK(cuts.size() == 1 && shots.subject() == 1);
	ShotDirector clear;
	cuts = run(clear, 18, [](float t) {
		Sample one = fighting(1);
		one.capturing = true;
		Sample two = fighting(2);
		if (t >= 2) {
			two.titanAssault = two.titanCore = true;
			two.sinceKill = 0;
		}
		return std::vector<Sample> {one, two};
	});
	CHECK(cuts.size() == 2 && cuts[1].subject == 2 && cuts[1].reason == "better" && cuts[1].time == 6.0f);
}

// Variety: between equal candidates, one not watched lately wins.
TEST(Director_PrefersAFreshSubjectWhenScoresAreClose)
{
	ShotDirector shots;
	std::vector<Cut> cuts = run(shots, 70, [](float) {
		return std::vector<Sample> {fighting(1), fighting(2), fighting(3), fighting(4)};
	});
	// Every 20 s (max shot) the next, never one of the last few back while another is as good.
	CHECK(cuts.size() == 4);
	std::vector<int> order;
	for (const Cut& c : cuts) {
		order.push_back(c.subject);
	}
	CHECK((order == std::vector<int> {1, 2, 3, 4}));
	// A clearly better recent subject still wins over a fresh one.
	Sample star = fighting(1);
	star.sinceKill = 0;
	star.sinceDamage = 0;
	ShotDirector again;
	again.update(0, {star, living(2)});
	again.forceCut();
	std::optional<Cut> forced = again.update(0.5f, {star, living(2)});
	CHECK(forced && forced->subject == 2 && forced->reason == "forced"); // forced: away from the subject
	forced = again.update(7, {star, living(2)});
	CHECK(forced && forced->subject == 1 && forced->reason == "better");
}

// The modes are varied: mostly chase, first person for some infantry shots, a few wide ones; never wide twice running.
TEST(Director_VariesTheCameraModes)
{
	ShotDirector shots;
	int chase = 0, first = 0, wide = 0, vehicleFirst = 0;
	CameraMode last = CameraMode::Chase;
	bool wideTwice = false;
	for (int i = 0; i < 400; ++i) {
		Sample s = fighting(i % 7);
		if (i % 3 == 0) {
			s.vehicle = 99;
		}
		shots.forceCut();
		auto cut = shots.update(static_cast<float>(i), {s, fighting(100 + i % 5)});
		if (!cut) {
			continue;
		}
		const bool vehicle = cut->subject == s.player && s.vehicle;
		chase += cut->mode == CameraMode::Chase;
		first += cut->mode == CameraMode::FirstPerson;
		wide += cut->mode == CameraMode::Wide;
		vehicleFirst += vehicle && cut->mode == CameraMode::FirstPerson;
		wideTwice = wideTwice || (cut->mode == CameraMode::Wide && last == CameraMode::Wide);
		last = cut->mode;
	}
	std::printf("  modes over 400 cuts: chase %d, first person %d, wide %d\n", chase, first, wide);
	CHECK(chase > first && first > wide && wide >= 20 && wide <= 80);
	CHECK(vehicleFirst == 0 && !wideTwice);
}

// The camera smooths a bot's aim jitter (no snapping) but keeps up with a real turn.
TEST(Director_CameraSmoothsAimJitterWithoutLagging)
{
	using game::director::DirectorCamera;
	using game::director::Pose;
	const game::camera::Ground flat {[](float, float) { return 0.0f; }, {}};
	DirectorCamera camera;
	camera.cut();
	Pose pose;
	pose.eye = {0, 1.47f, 0};
	// +-4 degrees of jitter each tick around 0 (8 degrees a tick): settled, the view moves under a degree a tick.
	float worst = 0, previous = 0;
	for (int tick = 0; tick < 90; ++tick) {
		pose.yaw = tick % 2 ? 4.0f : -4.0f;
		camera.update(CameraMode::FirstPerson, pose, 1 / 30.0f, flat);
		if (tick > 10) {
			worst = std::max(worst, std::fabs(camera.yaw() - previous));
		}
		previous = camera.yaw();
	}
	CHECK(worst < 1.0f);
	// A 90 degree turn: most of the way in a third of a second, all of it in a second.
	pose.yaw = 90;
	for (int tick = 0; tick < 10; ++tick) {
		camera.update(CameraMode::FirstPerson, pose, 1 / 30.0f, flat);
	}
	const float third = camera.yaw();
	for (int tick = 0; tick < 20; ++tick) {
		camera.update(CameraMode::FirstPerson, pose, 1 / 30.0f, flat);
	}
	std::printf("  jitter: %.2f degrees a tick at worst; a 90 degree turn %.0f after 1/3 s, %.1f after 1 s\n", worst, third,
		camera.yaw());
	CHECK(third > 75 && std::fabs(camera.yaw() - 90) < 0.5f);
	// The eye at the aim: the view looks along the yaw.
	const auto& look = camera.last();
	CHECK(std::fabs(look.eye.x) < 1e-3f && look.at.x - look.eye.x > 0.99f);
	// A cut takes the new aim at once (no smoothing across it).
	pose.yaw = -120;
	camera.cut();
	camera.update(CameraMode::FirstPerson, pose, 1 / 30.0f, flat);
	CHECK(std::fabs(camera.yaw() + 120) < 1e-3f);
	// The chase sits behind the soldier (yaw 0 faces +z: behind is -z), a few metres off, above the ground.
	camera.cut();
	pose.yaw = 0;
	const game::camera::Look chase = camera.update(CameraMode::Chase, pose, 1 / 30.0f, flat);
	CHECK(chase.eye.z < -2 && chase.eye.z > -4 && chase.eye.y > pose.eye.y);
	// The wide shot orbits the action from afar, slowly.
	pose.focus = {10, 0, 10};
	camera.cut();
	const game::camera::Look wide = camera.update(CameraMode::Wide, pose, 1 / 30.0f, flat);
	const float dx = wide.eye.x - 10, dz = wide.eye.z - 10;
	CHECK(std::sqrt(dx * dx + dz * dz) > 20 && wide.eye.y > 8);
	const float angle = std::atan2(dx, dz);
	game::camera::Look later = wide;
	for (int tick = 0; tick < 30; ++tick) {
		later = camera.update(CameraMode::Wide, pose, 1 / 30.0f, flat);
	}
	const float turned = std::remainder(std::atan2(later.eye.x - 10, later.eye.z - 10) - angle, 6.2831853f) * 57.29578f;
	CHECK(turned > 3 && turned < 15);
}
