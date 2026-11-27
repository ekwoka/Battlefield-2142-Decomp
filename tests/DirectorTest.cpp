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

// --- --director-mode kills (Policy::Kills)

namespace {

game::director::Settings killsSettings()
{
	game::director::Settings settings;
	settings.policy = game::director::Policy::Kills;
	return settings;
}

Sample killedBy(Sample s, int killer)
{
	s.alive = false;
	s.killer = killer;
	return s;
}

// The cuts that changed the subject (not the "angle" ones).
std::vector<Cut> subjectCuts(const std::vector<Cut>& cuts)
{
	std::vector<Cut> out;
	for (const Cut& c : cuts) {
		if (c.reason != "angle") {
			out.push_back(c);
		}
	}
	return out;
}

} // namespace

// The subject's death: after the hold, its killer. The killer is followed
// past maxShot while it fights, against a far better candidate and through
// its own kills and kills elsewhere; after followIdle without action it is
// left, and the next bot to kill is followed.
TEST(Director_KillsFollowsTheKillerPastTheMaximum)
{
	ShotDirector shots(killsSettings());
	// Teams: odd players 2, even 1. Bot 1 fights; bot 2 kills it at 3 s and
	// fights on to 40 s, then idles; bot 3 is a star from 10 s; bot 2 kills bot 4
	// at 15 s, bot 5 kills bot 6 at 20 s (elsewhere); bot 7 kills bot 8 at 80 s.
	const std::vector<Cut> all = run(shots, 90, [](float t) {
		Sample one = t < 3 ? fighting(1) : killedBy(fighting(1), 2);
		Sample two = t < 40 ? fighting(2) : idling(2, t - 40);
		Sample three = t < 10 ? living(3) : fighting(3);
		if (t >= 10) {
			three.sinceKill = three.sinceDamage = 0.5f;
			three.titanAssault = three.titanCore = true;
		}
		Sample four = t < 15 ? living(4) : killedBy(living(4), 2);
		Sample five = living(5), six = t < 20 ? living(6) : killedBy(living(6), 5);
		Sample seven = living(7), eight = t < 80 ? living(8) : killedBy(living(8), 7);
		return std::vector<Sample> {one, two, three, four, five, six, seven, eight};
	});
	const std::vector<Cut> cuts = subjectCuts(all);
	for (const Cut& c : all) {
		std::printf("  %5.1f s: %d -> %d %s, %s\n", c.time, c.previous, c.subject, game::director::modeName(c.mode), c.reason.c_str());
	}
	CHECK(cuts.size() == 4);
	CHECK(cuts.size() == 4 && cuts[0].subject == 1 && cuts[0].reason == "start");
	// The death hold (2 s), then the killer.
	CHECK(cuts.size() == 4 && cuts[1].subject == 2 && cuts[1].reason == "killer" && cuts[1].time == 5.0f);
	// Kept to 39.5 s (its last fight) plus followIdle: no "better" for bot 3, no "max" at 25 s.
	CHECK(cuts.size() == 4 && cuts[2].reason == "idle-long" && cuts[2].previous == 2 && cuts[2].time == 64.5f);
	// Waiting again, then the next kill anywhere.
	CHECK(cuts.size() == 4 && cuts[3].subject == 7 && cuts[3].reason == "first-kill" && cuts[3].time == 80.0f);
	CHECK(shots.subject() == 7 && shots.following());
	// The long follow changed camera now and then (angleEvery, 12 s), the subject kept.
	int angles = 0;
	for (const Cut& c : all) {
		if (c.reason == "angle") {
			++angles;
			CHECK(c.subject == 2 && c.previous == 2 && c.time > 5 && c.time < 64.5f);
		}
	}
	CHECK(angles >= 4 && angles <= 5);
	// The summary: two follows (the killer's, the first kill's) and the shots' lengths.
	const game::director::Summary sum = game::director::summarize(all, 90);
	CHECK(sum.shots == 4 && sum.angles == angles && sum.follows == 2 && sum.chained == 0 && sum.longestChain == 1);
	CHECK(std::fabs(sum.longestFollow - 59.5f) < 1e-3f && std::fabs(sum.meanFollow - (59.5f + 10) / 2) < 1e-3f);
	CHECK(std::fabs(sum.meanShot - 90.0f / 4) < 1e-3f && std::fabs(sum.followShare - 69.5f / 90) < 1e-3f);
}

// From the start the interest rules choose the waiting shots (cut at maxShot,
// not held); the first kill anywhere takes the camera, and of two in one
// decision the killer with the higher interest (then the lower id) wins.
TEST(Director_KillsWaitsThenTakesTheFirstKiller)
{
	const auto match = [](int fastKiller) {
		return [fastKiller](float t) {
			Sample three = living(3), five = living(5);
			if (fastKiller == 5) {
				five.enemyInRange = true;
			}
			Sample four = t < 25 ? living(4) : killedBy(living(4), 3);
			Sample six = t < 25 ? living(6) : killedBy(living(6), 5);
			return std::vector<Sample> {fighting(1), fighting(2), three, four, five, six};
		};
	};
	ShotDirector shots(killsSettings());
	std::vector<Cut> cuts = run(shots, 30, match(0));
	CHECK(cuts.size() == 3);
	CHECK(cuts.size() == 3 && cuts[0].reason == "start" && cuts[1].reason == "max" && cuts[1].time == 20.0f);
	CHECK(cuts.size() == 3 && cuts[2].reason == "first-kill" && cuts[2].time == 25.0f && cuts[2].subject == 3);
	CHECK(!cuts.empty() && cuts.back().mode != CameraMode::Wide && shots.following());
	ShotDirector other(killsSettings());
	cuts = run(other, 30, match(5));
	CHECK(cuts.size() == 3 && cuts[2].subject == 5 && cuts[2].reason == "first-kill");
	// The same again: the same pick (deterministic).
	ShotDirector again(killsSettings());
	const std::vector<Cut> repeat = run(again, 30, match(5));
	CHECK(repeat.size() == cuts.size() && repeat.back().subject == cuts.back().subject && repeat.back().mode == cuts.back().mode);
	// A kill by the waiting shot's own bot: it is followed, without a new shot.
	ShotDirector own(killsSettings());
	cuts = run(own, 10, [](float t) {
		Sample two = t < 4 ? living(2) : killedBy(living(2), 1);
		return std::vector<Sample> {fighting(1), two, living(3)};
	});
	CHECK(cuts.size() == 2 && cuts[1].subject == 1 && cuts[1].previous == 1 && cuts[1].reason == "first-kill");
	CHECK(cuts.size() == 2 && cuts[1].mode == cuts[0].mode && own.following() && own.shotStart() == 0.0f);
}

// A suicide, a team kill or an environment death of the subject: the next
// shot by interest ("death"), waiting; a team kill elsewhere is no first
// kill; a followed killer leaving the match: "left".
TEST(Director_KillsFallsBackOnASuicideOrTeamKill)
{
	for (const int killer : {1, 3, -1}) { // bot 1 itself, bot 3 (its team), nobody
		ShotDirector shots(killsSettings());
		std::vector<Cut> cuts = run(shots, 10, [killer](float t) {
			Sample one = fighting(1);
			one.sinceKill = 0;
			Sample four = t < 2 ? living(4) : killedBy(living(4), 6); // 6 and 4: team 1
			return std::vector<Sample> {t < 4 ? one : killedBy(one, killer), living(2), living(3), four, living(6)};
		});
		CHECK(cuts.size() == 2);
		CHECK(cuts.size() == 2 && cuts[0].subject == 1 && cuts[1].reason == "death" && cuts[1].time == 6.0f);
		CHECK(!shots.following());
	}
	// Following bot 2 (bot 1's killer), which then leaves.
	ShotDirector shots(killsSettings());
	std::vector<Cut> cuts = run(shots, 10, [](float t) {
		std::vector<Sample> samples {t < 1 ? fighting(1) : killedBy(fighting(1), 2), living(3)};
		if (t < 7) {
			samples.push_back(fighting(2));
		}
		return samples;
	});
	cuts = subjectCuts(cuts);
	CHECK(cuts.size() == 3 && cuts[1].reason == "killer" && cuts[1].subject == 2);
	CHECK(cuts.size() == 3 && cuts[2].reason == "left" && cuts[2].time == 7.0f && cuts[2].subject == 3 && !shots.following());
}

// A followed killer in a vehicle: the chase, every other angleEvery a wide
// orbit, never first person. On foot the camera alternates chase and first
// person every angleEvery; the default policy never makes an "angle" cut.
TEST(Director_KillsChasesAVehicleAndVariesTheAngle)
{
	ShotDirector shots(killsSettings());
	std::vector<Cut> cuts = run(shots, 120, [](float t) {
		Sample two = fighting(2);
		if (t >= 60) {
			two.vehicle = 42;
		}
		return std::vector<Sample> {t < 1 ? fighting(1) : killedBy(fighting(1), 2), two};
	});
	int onFoot = 0, inVehicle = 0;
	CameraMode last = CameraMode::Chase;
	for (const Cut& c : cuts) {
		if (c.reason == "angle") {
			(c.time < 60 ? onFoot : inVehicle) += 1;
			CHECK(c.time < 60 || c.mode != CameraMode::FirstPerson);
			CHECK(c.time < 60 || c.mode == CameraMode::Chase || last == CameraMode::Chase); // the orbit, then back
		}
		last = c.mode;
	}
	CHECK(subjectCuts(cuts).size() == 2 && shots.subject() == 2 && shots.mode() != CameraMode::FirstPerson);
	CHECK(onFoot >= 3 && onFoot <= 4 && inVehicle >= 4 && inVehicle <= 6);
	ShotDirector plain;
	cuts = run(plain, 120, [](float t) {
		return std::vector<Sample> {t < 1 ? fighting(1) : killedBy(fighting(1), 2), fighting(2), fighting(3)};
	});
	for (const Cut& c : cuts) {
		CHECK(c.reason != "angle" && c.reason != "killer" && c.reason != "first-kill");
	}
}

// --director-first-person with --director-mode kills: a followed killer on
// foot stays in his own eye (no chase or orbit angle changes); in a vehicle
// the chase and orbit still alternate.
TEST(Director_KillsFirstPersonOnlyKeepsTheEye)
{
	game::director::Settings settings = killsSettings();
	settings.firstPersonShare = 1;
	settings.wideShare = 0;
	ShotDirector shots(settings);
	std::vector<Cut> cuts = run(shots, 120, [](float t) {
		Sample two = fighting(2);
		if (t >= 60) {
			two.vehicle = 42;
		}
		return std::vector<Sample> {t < 1 ? fighting(1) : killedBy(fighting(1), 2), two};
	});
	int inVehicle = 0;
	for (const Cut& c : cuts) {
		if (c.subject == 2 && c.time < 60) {
			CHECK(c.mode == CameraMode::FirstPerson);
		}
		if (c.reason == "angle" && c.time >= 60) {
			++inVehicle;
			CHECK(c.mode != CameraMode::FirstPerson);
		}
	}
	CHECK(inVehicle >= 1);
}

// --director-mode kills-rotate: a followed bot on a gun of its own Titan,
// killing and never hit, is left after titanGunStreak ("streak") and isn't
// taken back for its next kill while benched; plain kills mode stays on it.
TEST(Director_KillsRotateLeavesAnUnharmedTitanGun)
{
	const auto match = [](float t) {
		Sample two = fighting(2);
		two.vehicle = 42;
		two.ownTitan = true;
		two.sinceHit = game::director::kNever;
		Sample four = t < 30 ? living(4) : killedBy(living(4), 2);
		return std::vector<Sample> {t < 1 ? fighting(1) : killedBy(fighting(1), 2), two, fighting(3), four};
	};
	game::director::Settings settings = killsSettings();
	settings.policy = game::director::Policy::KillsRotate;
	ShotDirector shots(settings);
	const std::vector<Cut> cuts = subjectCuts(run(shots, 80, match));
	float followed = -1, left = -1;
	for (const Cut& c : cuts) {
		if (c.subject == 2 && followed < 0) {
			followed = c.time;
		}
		if (c.reason == "streak") {
			CHECK(c.previous == 2 && c.subject != 2);
			left = c.time;
		}
		if (left >= 0 && c.time > left) {
			CHECK(c.subject != 2); // benched through its kill at 30 s
		}
	}
	CHECK(followed >= 0 && followed <= 3);
	CHECK(left >= followed + settings.titanGunStreak && left <= followed + settings.titanGunStreak + 1);
	ShotDirector plain(killsSettings());
	for (const Cut& c : run(plain, 80, match)) {
		CHECK(c.reason != "streak");
	}
	CHECK(plain.subject() == 2 && plain.following());
}

// kills-rotate: a vehicle hit now and then, or a bot on foot, keeps its
// follow (the streak is time in a vehicle without taking damage); an
// unharmed tank is left after Settings::streak.
TEST(Director_KillsRotateKeepsAVehicleUnderFire)
{
	game::director::Settings settings = killsSettings();
	settings.policy = game::director::Policy::KillsRotate;
	const auto runWith = [&](float hitEvery, bool inVehicle) {
		ShotDirector shots(settings);
		return subjectCuts(run(shots, 150, [=](float t) {
			Sample two = fighting(2);
			two.vehicle = inVehicle ? 42 : 0;
			two.sinceHit = hitEvery > 0 ? std::fmod(t, hitEvery) : game::director::kNever;
			return std::vector<Sample> {t < 1 ? fighting(1) : killedBy(fighting(1), 2), two, fighting(3)};
		}));
	};
	const auto streaks = [](const std::vector<Cut>& cuts) {
		return std::count_if(cuts.begin(), cuts.end(), [](const Cut& c) { return c.reason == "streak"; });
	};
	CHECK(streaks(runWith(30, true)) == 0);
	CHECK(streaks(runWith(0, false)) == 0);
	const std::vector<Cut> tank = runWith(0, true);
	CHECK(streaks(tank) >= 1);
	for (const Cut& c : tank) {
		if (c.reason == "streak") {
			CHECK(c.time >= settings.streak && c.time <= settings.streak + 4);
			break;
		}
	}
}

TEST(Director_PolicyNames)
{
	game::director::Policy policy = game::director::Policy::Interest;
	CHECK(game::director::parsePolicy("kills-rotate", policy) && policy == game::director::Policy::KillsRotate);
	CHECK(std::string(game::director::policyName(policy)) == "kills-rotate");
	CHECK(!game::director::parsePolicy("rotate", policy) && policy == game::director::Policy::KillsRotate);
}

// Two gunners of one transport are one subject: the interest never cuts
// between them as their fire comes and goes, a crewmate's kill doesn't take
// the kills follow from one to the other, and a streak benches the crew.
TEST(Director_TreatsAVehicleCrewAsOneSubject)
{
	const auto crew = [](float t) {
		// Players 1 and 3 (one team) gun vehicle 9 in turn, 5 s each; player 5 idles about.
		const bool first = std::fmod(t, 10.0f) < 5;
		Sample one = first ? fighting(1) : living(1), three = first ? living(3) : fighting(3);
		one.vehicle = three.vehicle = 9;
		one.speed = three.speed = 8;
		one.sinceHit = three.sinceHit = game::director::kNever;
		return std::vector<Sample> {one, three, idling(5, t)};
	};
	const auto noCrewCut = [](const std::vector<Cut>& cuts) {
		for (const Cut& c : cuts) {
			const bool between = (c.previous == 1 && c.subject == 3) || (c.previous == 3 && c.subject == 1);
			CHECK(!between);
		}
	};
	ShotDirector interest;
	noCrewCut(run(interest, 60, crew));
	// Kills: the crew kills player 2 at 2 s and player 4 at 30 s (by the other gunner).
	const auto killing = [&](float t) {
		std::vector<Sample> all = crew(t);
		all.push_back(t < 2 ? living(2) : killedBy(living(2), 1));
		all.push_back(t < 30 ? living(4) : killedBy(living(4), 3));
		return all;
	};
	ShotDirector kills(killsSettings());
	const std::vector<Cut> followed = subjectCuts(run(kills, 60, killing));
	noCrewCut(followed);
	CHECK(kills.following() && (kills.subject() == 1 || kills.subject() == 3));
	// kills-rotate: after the streak the crew is benched; its second kill (30 s) brings neither gunner back.
	game::director::Settings settings = killsSettings();
	settings.policy = game::director::Policy::KillsRotate;
	settings.streak = 20;
	ShotDirector rotate(settings);
	const std::vector<Cut> rotated = subjectCuts(run(rotate, 60, killing));
	noCrewCut(rotated);
	float left = -1;
	for (const Cut& c : rotated) {
		if (c.reason == "streak") {
			left = c.time;
			CHECK(c.subject != 1 && c.subject != 3);
		} else if (left >= 0) {
			CHECK(c.subject != 1 && c.subject != 3);
		}
	}
	CHECK(left > 20 && left < 24);
}
