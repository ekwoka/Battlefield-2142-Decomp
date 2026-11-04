// Firearms and projectiles.
#include "Game/Weapons.h"
#include "Physics/MaterialManager.h"
#include "TestMain.h"
#include "World/ObjectTemplateManager.h"
#include "World/Object.h"
#include "World/WorldConsole.h"
#include "Console/Console.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace dice::hfe;

TEST(Weapons_DamageFallsOffWithDistance)
{
	game::ProjectileSettings rifle; // eu_ar_rifleProjectile
	rifle.damage = 29;
	rifle.minDamage = 20;
	rifle.distToStartLoseDamage = 35;
	rifle.distToMinDamage = 65;
	CHECK(rifle.damageAt(10) == 29);
	CHECK(std::fabs(rifle.damageAt(50) - 24.5f) < 1e-4f);
	CHECK(rifle.damageAt(200) == 20);
}

TEST(Weapons_FireRateAndReload)
{
	game::FireArmSettings settings; // 600 rounds per minute, 30 rounds, 5 mags, 2 s reload
	settings.magSize = 4;
	game::FireArm arm(settings);
	int shots = 0;
	for (int tick = 0; tick < 30; ++tick) {
		shots += arm.tick(1 / 30.0f, true) ? 1 : 0;
		if (tick == 11) {
			CHECK(shots == 4);         // a round every 3 ticks, then empty
			CHECK(arm.reloading());
		}
	}
	CHECK(shots == 4); // still reloading after one second
	for (int tick = 0; tick < 60; ++tick) {
		shots += arm.tick(1 / 30.0f, true) ? 1 : 0;
	}
	CHECK(shots > 4 && arm.magsLeft() == 4);
}

TEST(Weapons_ProjectilesFall)
{
	game::ProjectileSettings settings;
	game::Projectile p;
	p.settings = &settings;
	p.velocity = {0, 0, 900};
	p.timeLeft = 1;
	for (int i = 0; i < 30; ++i) {
		game::moveProjectile(p, 1 / 30.0f);
	}
	// A second at 900 m/s, about half of -14.73 * 1^2 lower.
	CHECK(std::fabs(p.position.z - 900) < 1e-2f);
	CHECK(p.position.y < -6 && p.position.y > -7.5f);
	CHECK(!p.alive);
}

TEST(Weapons_RandomDistributions)
{
	const auto ttl = game::RandomDistribution::parse("CRD_NONE/1/0/0");
	game::Random random;
	CHECK(ttl.sample(random) == 1);
	const auto up = game::RandomDistribution::parse("CRD_UNIFORM/0.52/1.05/0");
	for (int i = 0; i < 100; ++i) {
		const float v = up.sample(random);
		CHECK(v >= 0.52f && v <= 1.05f);
	}
}

TEST(Weapons_GunsHeatUp)
{
	// eu_fav_MachineGun_Firearm: no magazine, 0.02 heat a round, cooling
	// 0.4 a second, 2 s stopped when overheated.
	game::FireArmSettings settings;
	settings.magSize = -1;
	settings.roundsPerMinute = 1800; // a round a tick
	settings.heatAddWhenFire = 0.02f;
	settings.coolDownPerSec = 0.4f;
	settings.overheatPenalty = 2;
	game::FireArm gun(settings);
	int rounds = 0;
	while (!gun.overheated() && rounds < 1000) {
		rounds += gun.tick(1 / 30.0f, true) ? 1 : 0;
	}
	// Each tick cools 0.0133 while a round adds 0.02: about 150 rounds.
	CHECK(rounds > 140 && rounds < 160);
	CHECK(!gun.tick(1 / 30.0f, true));
	for (int i = 0; i < 61; ++i) {
		gun.tick(1 / 30.0f, false);
	}
	CHECK(!gun.overheated() && gun.heat() < 0.25f && gun.tick(1 / 30.0f, true));
}

// ammo.reloadWithoutPlayer (0x005f4bb0 / 0x005f3a40): a gun left with a
// reload under way finishes it (runReload); one without the flag forgets
// it (dropReload) and keeps its empty magazine.
TEST(Weapons_ReloadWithoutPlayer)
{
	game::FireArmSettings settings;
	settings.magSize = 2;
	settings.nrOfMags = 2;
	settings.reloadTime = 1;
	settings.roundsPerMinute = 1800;
	for (const bool runs : {true, false}) {
		game::FireArm gun(settings);
		int rounds = 0;
		for (int i = 0; i < 4; ++i) {
			rounds += gun.tick(1 / 30.0f, true) ? 1 : 0;
		}
		CHECK(rounds == 2 && gun.roundsInMag() == 0 && gun.reloading());
		if (runs) {
			for (int i = 0; i < 40; ++i) {
				gun.runReload(1 / 30.0f);
			}
			CHECK(!gun.reloading() && gun.roundsInMag() == 2);
		} else {
			gun.dropReload();
			CHECK(!gun.reloading() && gun.roundsInMag() == 0);
		}
	}
}

// fire.busyUntilButtonRelease (0x005d4800 / 0x005d3c20): a launcher held
// fires one round and neither fires again nor reloads until let go.
TEST(Weapons_BusyUntilButtonRelease)
{
	game::FireArmSettings settings;
	settings.magSize = 1;
	settings.nrOfMags = 3;
	settings.reloadTime = 0.5f;
	settings.roundsPerMinute = 600;
	settings.busyUntilButtonRelease = true;
	game::FireArm gun(settings);
	int rounds = 0;
	for (int i = 0; i < 60; ++i) { // two seconds held
		rounds += gun.tick(1 / 30.0f, true) ? 1 : 0;
	}
	CHECK(rounds == 1 && gun.reloading() && std::fabs(gun.reloadLeft() - 0.5f) < 1e-4f);
	for (int i = 0; i < 20; ++i) { // let go: the reload runs
		gun.tick(1 / 30.0f, false);
	}
	CHECK(!gun.reloading() && gun.roundsInMag() == 1);
	CHECK(gun.tick(1 / 30.0f, true));
}

TEST(Weapons_ProjectileKinds)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericProjectile");
	// Unl_Grenade_EMP_Projectile: an EMP blast of 9 m with the default
	// strength (explosionDamage 500, 0x005e9df0), a fuse.
	world::ObjectTemplate* emp = templates.create("GenericProjectile", "emp");
	emp->settings()["detonation.isEMPWeapon"] = "1";
	emp->settings()["detonation.explosionRadius"] = "9";
	emp->settings()["detonation.explosionMaterial"] = "0";
	emp->settings()["physicsType"] = "RotationalPoint";
	emp->settings()["timeToLive"] = "CRD_NONE/8/0/0";
	const auto e = game::ProjectileSettings::fromTemplate(*emp);
	CHECK(e.emp && e.bounces && e.explosionRadius == 9 && e.explosionDamage == 500 && !e.remote);
	// Unl_c4_Projectile: no trigger, no fuse to speak of, no radius of its
	// own (the default 10).
	world::ObjectTemplate* c4 = templates.create("GenericProjectile", "c4");
	c4->settings()["detonation.explosionDamage"] = "1000";
	c4->settings()["detonation.explosionMaterial"] = "80";
	c4->settings()["timeToLive"] = "CRD_NONE/9999/0/0";
	const auto c = game::ProjectileSettings::fromTemplate(*c4);
	CHECK(c.remote && c.explosionRadius == 10 && !c.emp);
	// A bare DefaultDetonationComp (the car horn's): the constructor's
	// defaults, material -1, so no blast; no component: no detonation.
	world::ObjectTemplate* horn = templates.create("GenericProjectile", "horn");
	horn->components().push_back("DefaultDetonationComp");
	horn->settings()["timeToLive"] = "CRD_NONE/9999/0/0";
	const auto h = game::ProjectileSettings::fromTemplate(*horn);
	CHECK(h.detonates && h.explosionDamage == 500 && h.explosionRadius == 10 && h.explosionForce == 150
		&& h.explosionYMod == 1 && h.explosionMaterial == -1 && !h.remote);
	const auto bullet = game::ProjectileSettings::fromTemplate(*templates.create("GenericProjectile", "bullet"));
	CHECK(!bullet.detonates && bullet.explosionRadius == 0);
	// A mine: a trigger.
	world::ObjectTemplate* mine = templates.create("GenericProjectile", "mine");
	mine->settings()["detonation.explosionDamage"] = "200";
	mine->settings()["detonation.explosionRadius"] = "8";
	mine->settings()["detonation.triggerRadius"] = "3";
	mine->settings()["timeToLive"] = "CRD_NONE/9999/0/0";
	const auto m = game::ProjectileSettings::fromTemplate(*mine);
	CHECK(!m.remote && m.triggerRadius == 3 && m.explosionRadius == 8);
}

TEST(Weapons_ExplosionFalloffAndEmpSteps)
{
	// 0x005a61a0: full within a fifth of the radius, then linear.
	CHECK(game::explosionFalloff(10, 1) == 1 && game::explosionFalloff(10, 2) == 1);
	CHECK(std::fabs(game::explosionFalloff(10, 6) - 0.5f) < 1e-5f && game::explosionFalloff(10, 10) == 0);
	CHECK(game::explosionFalloff(10, 15) == 0);
	// 0x00652980: whole steps of the step size (120 by default), at most
	// the number of steps; a level already falling counts in.
	CHECK(game::addEmpDamage(0, 500, 120, 3) == 3);  // the EMP grenade at its centre
	CHECK(game::addEmpDamage(0, 100, 120, 3) == 1);  // at 80 % of its radius
	CHECK(game::addEmpDamage(0.4f, 100, 120, 3) == 2);
	CHECK(game::addEmpDamage(0, 0.2f, 120, 2) == 1);
}

// A frag grenade (Grenade_hand: friction 0.25, elasticity 0.5) landing on
// grass (friction 0.8, elasticity 0): elasticity (0.5 + 0) / 2 = 0.25 of
// its speed into the ground comes back, and 1 - (0.25 + 0.8) / 2 = 0.475
// of its slide is kept (0x006c0d00); friction over 1 stops the slide.
TEST(Weapons_GrenadeBounce)
{
	const physics::Vec3 v = game::bounceVelocity({4, -8, 0}, {0, 1, 0}, 0.25f, 0.525f);
	CHECK(std::fabs(v.x - 4 * 0.475f) < 1e-5f && std::fabs(v.y - 2) < 1e-5f && v.z == 0);
	const physics::Vec3 stairs = game::bounceVelocity({4, -8, 0}, {0, 1, 0}, 0.25f, 2.6f);
	CHECK(stairs.x == 0 && std::fabs(stairs.y - 2) < 1e-5f);
}

// The motion mine's follow (Unl_Mine_Motion: acceleration 60, deAcceleration
// 50, armingSpeed 7.2, speedModifier 2, useTargetSpeed): a parked vehicle
// gives no speed to chase at; one at 5 m/s is chased up to 5 m/s; once it
// has gone over 7.2 m/s, at twice its top speed.
TEST(Weapons_MotionMineFollow)
{
	game::ProjectileSettings s;
	s.seek = game::ProjectileSettings::Seek::Magnet;
	s.acceleration = 60;
	s.deAcceleration = 50;
	s.maxSpeed = 300;
	s.armingSpeed = 7.2f;
	s.speedModifier = 2;
	s.predictionFactor = 0.1f;
	s.useTargetSpeed = true;
	game::Projectile mine;
	mine.settings = &s;
	mine.resting = true;
	game::followMagnet(mine, {10, 0, 0}, {0, 0, 0}, 1 / 30.0f);
	CHECK(mine.resting && physics::lengthSquared(mine.velocity) == 0); // parked: never chased
	game::followMagnet(mine, {10, 0, 0}, {0, 0, 5}, 1 / 30.0f);
	CHECK(!mine.resting && std::fabs(mine.velocity.x - 2) < 1e-4f && !mine.armed);
	for (int i = 0; i < 30; ++i) {
		game::followMagnet(mine, {10, 0, 0}, {0, 0, 5}, 1 / 30.0f);
	}
	const float capped = std::sqrt(physics::lengthSquared(mine.velocity));
	CHECK(capped <= 5 + 2 && capped >= 5 - 2);
	game::followMagnet(mine, {10, 0, 0}, {0, 0, 9}, 1 / 30.0f);
	CHECK(mine.armed && std::fabs(mine.targetTopSpeed - 9) < 1e-4f);
	for (int i = 0; i < 60; ++i) {
		game::followMagnet(mine, {10, 0, 0}, {0, 0, 4}, 1 / 30.0f); // slower now: still chased at 18
	}
	const float armed = std::sqrt(physics::lengthSquared(mine.velocity));
	CHECK(armed > 16 && armed < 20);
}

// The deviation components' spread (0x005b28d0, 0x005f5540): normal(0, 1)
// numbers; a round deviating by `dev` moves dev x 0.01 x the firearm's
// velocity per unit along its up and right axes; 0.01 or less is none.
TEST(Weapons_DeviationIsNormal)
{
	game::Random random;
	game::Gaussian gaussian;
	double sum = 0, squares = 0;
	constexpr int kCount = 20000;
	for (int i = 0; i < kCount; ++i) {
		const double g = gaussian.next(random);
		sum += g;
		squares += g * g;
	}
	const double mean = sum / kCount, deviation = std::sqrt(squares / kCount - mean * mean);
	CHECK(std::fabs(mean) < 0.03 && std::fabs(deviation - 1) < 0.03);
	const physics::Vec3 v = game::deviateVelocity({0, -10, 0}, {1, 0, 0}, {0, 0, 1}, 10, 45, 1, -2);
	CHECK(std::fabs(v.z - 4.5f) < 1e-4f && std::fabs(v.x + 9) < 1e-4f && v.y == -10);
	const physics::Vec3 none = game::deviateVelocity({0, -10, 0}, {1, 0, 0}, {0, 0, 1}, 10, 0.01f, 1, 1);
	CHECK(none.x == 0 && none.z == 0);
}

// deviation.setFireDev max perShot decay (a method of three floats,
// 0x004e7c73 -> 0x005f5360) keeps its numbers on the template; the build-up (0x005f5540 a
// round, 0x005dedb0 an update) grows by perShot to max, decays unless the
// trigger is held, and never stays below 0 once it decays; minDev adds.
TEST(Weapons_DeviationBuildUp)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	Console console;
	world::registerWorldConsoleMethods(console, templates, objects);
	world::registerUntypedTemplateProperties(console, templates, {"deviation.minDev"});
	templates.registerType(1, "GenericFireArm");
	std::string out;
	for (const char* line : {"ObjectTemplate.create GenericFireArm rifle", "ObjectTemplate.deviation.setFireDev 2.0 0.24 0.09",
			 "ObjectTemplate.deviation.minDev 0.5", "ObjectTemplate.create GenericFireArm mg",
			 "ObjectTemplate.deviation.setFireDev -1 -0.09 -0.009"}) {
		const ConsoleResult r = console.execute(line, out, ConsoleAccess::Script);
		CHECK(r != ConsoleResult::Error);
		if (r == ConsoleResult::Error) {
			std::fprintf(stderr, "  %s: %s\n", line, out.c_str());
		}
	}
	const world::ObjectTemplate* rifle = templates.find("rifle");
	CHECK(rifle && rifle->settings().count("deviation.setFireDev"));
	if (!rifle || !rifle->settings().count("deviation.setFireDev")) {
		return;
	}
	CHECK(rifle->settings().at("deviation.setFireDev") == "2.0 0.24 0.09");
	game::DeviationBuildUp dev = game::DeviationBuildUp::fromTemplate(*rifle);
	CHECK(dev.max == 2.0f && std::fabs(dev.perShot - 0.24f) < 1e-6f && std::fabs(dev.decay - 0.09f) < 1e-6f && dev.minDev == 0.5f);
	for (int i = 0; i < 12; ++i) {
		dev.fired();
		dev.update(true); // held: no decay
	}
	CHECK(dev.value == 2.0f && dev.deviation() == 2.5f);
	dev.update(false);
	CHECK(std::fabs(dev.value - 1.91f) < 1e-5f);
	for (int i = 0; i < 30; ++i) {
		dev.update(false);
	}
	CHECK(dev.value == 0);
	// A machine gun's grows steadier, held at -1; let go, it is 0 at once.
	game::DeviationBuildUp mg = game::DeviationBuildUp::fromTemplate(*templates.find("mg"));
	for (int i = 0; i < 20; ++i) {
		mg.fired();
	}
	CHECK(mg.value == -1.0f);
	mg.update(false);
	CHECK(mg.value == 0);
}

// The push of a blast (0x005a66e0): along the object's axis nearest the line
// from the blast plus that line made a unit, as strong as falloff x
// forceMod x explosionForce x share, at most forceMax less the object's
// speed; a soldier's line gets 5 more upwards; in water a tenth.
TEST(Weapons_BlastPush)
{
	game::BlastPush blast;
	blast.line = {3, 0.5f, 0};
	blast.falloff = 0.5f;
	blast.forceMod = 16;
	blast.forceMax = 5000;
	blast.explosionForce = 10;
	physics::Vec3 push = game::blastPush(blast);
	// The x axis (nearest) plus the unit line, normalised, times 80.
	const float l = std::sqrt(9 + 0.25f);
	physics::Vec3 way {1 + 3 / l, 0.5f / l, 0};
	const float wl = std::sqrt(physics::lengthSquared(way));
	CHECK(std::fabs(push.x - way.x / wl * 80) < 1e-3f && std::fabs(push.y - way.y / wl * 80) < 1e-3f);
	blast.speed = 4990;
	CHECK(std::fabs(std::sqrt(physics::lengthSquared(game::blastPush(blast))) - 10) < 1e-3f);
	blast.speed = 0;
	blast.forceMax = 50;
	CHECK(std::fabs(std::sqrt(physics::lengthSquared(game::blastPush(blast))) - 50) < 1e-3f);
	// A soldier: up by 5, the random factor, and a tenth in water.
	blast.soldier = true;
	blast.forceMax = 1000;
	blast.soldierRandom = 1;
	push = game::blastPush(blast);
	CHECK(push.y > push.x);
	blast.inWater = true;
	CHECK(std::fabs(std::sqrt(physics::lengthSquared(game::blastPush(blast))) - 8) < 1e-3f);
}

// A detonation with a negative explosionDamage (its component 0xc54d's slot
// 0x4c below 0: 0x005a66e0's bVar5) pushes 20 x explosionForce x share,
// along the nearest axis alone, and as a torque pair: (0, f, 0) at the x
// axis turned to the blast, (0, -f/2, 0) opposite and the axis x -f/2 at
// the centre, f = force / 2.9 x max(width, 2).
TEST(Weapons_BlastTwist)
{
	game::BlastPush blast;
	blast.line = {3, 0.5f, 0};
	blast.falloff = 0.25f; // not used
	blast.forceMod = 16;   // not used
	blast.forceMax = 1000;
	blast.explosionForce = 10;
	blast.twist = true;
	CHECK(std::fabs(game::blastPushForce(blast) - 200) < 1e-3f);
	const physics::Vec3 push = game::blastPush(blast);
	CHECK(std::fabs(push.x - 200) < 1e-3f && push.y == 0 && push.z == 0);
	blast.forceMax = 150;
	CHECK(std::fabs(game::blastPushForce(blast) - 150) < 1e-3f);
	// The blast on the object's -x side: a is -x.
	const auto pair = game::blastTwist(blast.axes, blast.line, 200, 5);
	const float f = 200 / 2.9f * 5;
	CHECK(pair[0].at.x == -1 && std::fabs(pair[0].force.y - f) < 1e-2f);
	CHECK(pair[1].at.x == 1 && std::fabs(pair[1].force.y + f / 2) < 1e-2f);
	CHECK(physics::lengthSquared(pair[2].at) == 0 && std::fabs(pair[2].force.x - f / 2) < 1e-2f);
	// The other side, and a narrow object counts 2 m across.
	const auto other = game::blastTwist(blast.axes, {-3, 0, 0}, 200, 1);
	CHECK(other[0].at.x == 1 && std::fabs(other[0].force.y - 200 / 2.9f * 2) < 1e-2f);
}

// The soldier deviation (0x005de980) with eu_ar_rifle's numbers: standing
// still it is minDev x devModStand; running adds the speed's part up to its
// maximum; crouched and zoomed it narrows; in the air the misc part jumps
// to its maximum; firing builds up and wears off off the trigger. The
// 3- and 4-number setters are kept by the console (WorldConsole).
TEST(Weapons_SoldierDeviation)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "GenericFireArm");
	world::registerWorldConsoleMethods(console, templates, objects);
	std::string out;
	for (const char* line :
		{"ObjectTemplate.create GenericFireArm rifle", "ObjectTemplate.createComponent SoldierDeviationComp",
			"ObjectTemplate.deviation.setFireDev 2.0 0.24 0.09", "ObjectTemplate.deviation.setTurnDev 1 0.1 0.2 0.05",
			"ObjectTemplate.deviation.setSpeedDev 1 0.2 0.04 0.12", "ObjectTemplate.deviation.setMiscDev 7 7 0.14"}) {
		CHECK(console.execute(line, out) == ConsoleResult::Ok);
	}
	world::ObjectTemplate* rifle = templates.find("rifle");
	CHECK(rifle->settings()["deviation.setSpeedDev"] == "1 0.2 0.04 0.12");
	rifle->settings()["deviation.minDev"] = "0.43";
	rifle->settings()["deviation.devModStand"] = "1.3";
	rifle->settings()["deviation.devModCrouch"] = "1.1";
	rifle->settings()["deviation.devModZoom"] = "0.6";
	rifle->settings()["deviation.minDevModZoom"] = "0.2";
	game::SoldierDeviation d = game::SoldierDeviation::fromTemplate(*rifle);
	CHECK(d.present);
	d.update(0, 0, 0, 0, false, false, 0, false);
	CHECK(std::fabs(d.deviation() - 0.43f * 1.3f) < 1e-5f);
	for (int i = 0; i < 10; ++i) {
		d.update(1, 0, 0, 0, false, false, 0, false);
	}
	CHECK(std::fabs(d.speed - 1) < 1e-5f); // 0.2 a tick, held at 1
	CHECK(std::fabs(d.deviation() - (0.43f + 1) * 1.3f) < 1e-5f);
	d.update(0, 0, 0, 0, false, false, 1, true); // still, crouched, zoomed: speed decays 0.12
	CHECK(std::fabs(d.deviation() - (0.43f * 0.2f + 0.88f) * 0.6f * 1.1f) < 1e-4f);
	d.update(0, 0, 2, 0, true, false, 0, false); // a jump and a turn
	CHECK(std::fabs(d.misc - 6.86f) < 1e-4f && std::fabs(d.turn - (0.4f - 0.05f)) < 1e-5f);
	d.fire.fired();
	d.update(0, 0, 0, 0, false, true, 0, false);
	CHECK(std::fabs(d.fire.value - 0.24f) < 1e-5f); // held on the trigger
	d.update(0, 0, 0, 0, false, false, 0, false);
	CHECK(std::fabs(d.fire.value - 0.15f) < 1e-5f);
}

// The gun sway (0x005e0da0): zoomed (gunSwayUseZoom), every update a
// step of Max / Steps is put on the look, swinging back at Max; not
// zoomed, nothing. Holding the sprint key with the scope stabilizer
// steadies it (the stability falls to 0 over stabilityTime).
TEST(Weapons_GunSway)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericFireArm");
	world::ObjectTemplate* sniper = templates.create("GenericFireArm", "sniper");
	sniper->components().push_back("DefaultGunSwayComp");
	auto& s = sniper->settings();
	s["sway.gunSwayUseZoom"] = "1";
	s["sway.gunSwayPitchSteps"] = "10";
	s["sway.gunSwayPitchFactor"] = "30";
	s["sway.gunSwayPitchMax"] = "1";
	s["sway.stabilityTime"] = "1";
	s["sway.stabilityRecoverTime"] = "2";
	s["sway.stabilityDuration"] = "5";
	game::GunSway sway = game::GunSway::fromTemplate(*sniper);
	CHECK(sway.present && sway.useZoom);
	const auto random = [] { return 0.5f; };
	float x = 0, y = 0;
	sway.update(1 / 30.0f, false, false, false, x, y, random);
	CHECK(x == 0 && y == 0);
	float swung = 0, most = 0, least = 0;
	for (int i = 0; i < 60; ++i) {
		x = y = 0;
		sway.update(1 / 30.0f, true, false, false, x, y, random);
		swung += y;
		most = std::max(most, swung);
		least = std::min(least, swung);
		CHECK(std::fabs(std::fabs(y) - 30 * 0.1f / 30) < 1e-5f); // factor x step x dt
	}
	std::fprintf(stderr, "  sway between %.3f and %.3f\n", least, most);
	CHECK(most > 0.05f && least < most);
	// Steadied: after a second of the sprint key the stability is 0.
	for (int i = 0; i < 32; ++i) {
		x = y = 0;
		sway.update(1 / 30.0f, true, true, true, x, y, random);
	}
	CHECK(sway.steadying && sway.stability < 1e-4f);
	x = y = 0;
	sway.update(1 / 30.0f, true, true, true, x, y, random);
	CHECK(y == 0);
}

// c_PIReload (0x005c50e0's action 10, 0x005c0f40): a magazine not full is
// replaced after reloadTime, using one of the spares; a full one, one
// reloading already, or none to spare starts nothing.
TEST(Weapons_ManualReload)
{
	game::FireArmSettings settings;
	settings.magSize = 5;
	settings.nrOfMags = 2;
	settings.reloadTime = 1;
	settings.roundsPerMinute = 1800;
	game::FireArm arm(settings);
	CHECK(!arm.startReload()); // full
	CHECK(arm.tick(1 / 30.0f, true) && arm.roundsInMag() == 4);
	CHECK(arm.startReload() && arm.reloading() && !arm.startReload());
	for (int i = 0; i < 28; ++i) {
		CHECK(!arm.tick(1 / 30.0f, true)); // no rounds while it reloads
	}
	for (int i = 0; i < 4; ++i) {
		arm.tick(1 / 30.0f, false);
	}
	CHECK(!arm.reloading() && arm.roundsInMag() == 5 && arm.magsLeft() == 1);
	arm.tick(1 / 30.0f, true);
	CHECK(arm.startReload());
	for (int i = 0; i < 32; ++i) {
		arm.tick(1 / 30.0f, false);
	}
	CHECK(arm.magsLeft() == 0 && arm.roundsInMag() == 5);
	arm.tick(1 / 30.0f, true);
	CHECK(!arm.startReload()); // none to spare
}

// The fire modes (0x005f28a0, 0x005f1370, 0x005f1820, 0x005f1da0): single
// fire takes one round a press, burst burstSize a press (running on
// released), automatic while held; the toggle steps through the rates.
TEST(Weapons_FireModes)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericFireArm");
	world::ObjectTemplate* rifle = templates.create("GenericFireArm", "rifle");
	rifle->settings()["fire.addFireRate.list"] = "0 2";
	rifle->settings()["fire.roundsPerMinute"] = "600";
	const game::FireArmSettings settings = game::FireArmSettings::fromTemplate(*rifle);
	CHECK(settings.fireRates == (std::vector<int> {0, 2}) && settings.burstSize == 3);
	game::FireArm arm(settings);
	CHECK(arm.fireMode() == game::kFireSingle); // the first rate
	const auto hold = [&](int ticks, bool trigger) {
		int shots = 0;
		for (int i = 0; i < ticks; ++i) {
			shots += arm.tick(1 / 30.0f, trigger) ? 1 : 0;
		}
		return shots;
	};
	CHECK(hold(30, true) == 1); // held a second: one round
	CHECK(hold(1, false) == 0);
	// A second press a tick after a round waits for the gun (3 ticks a round).
	CHECK(hold(1, true) == 1 && hold(1, false) == 0 && hold(1, true) == 0 && hold(3, false) == 1);
	CHECK(arm.toggleFireMode() && arm.fireMode() == game::kFireAutomatic);
	CHECK(hold(30, true) == 10); // 600 a minute
	CHECK(arm.toggleFireMode() && arm.fireMode() == game::kFireSingle); // back to the first
	hold(5, false);

	game::FireArmSettings burstSettings = settings;
	burstSettings.fireRates = {1};
	burstSettings.burstSize = 4;
	game::FireArm burst(burstSettings);
	CHECK(burst.fireMode() == game::kFireBurst && !burst.toggleFireMode()); // one rate: no toggle
	int shots = burst.tick(1 / 30.0f, true) ? 1 : 0; // a press, then let go
	for (int i = 0; i < 30; ++i) {
		shots += burst.tick(1 / 30.0f, false) ? 1 : 0;
	}
	CHECK(shots == 4);

	// Without fire.addFireRate: automatic; with the modes off, as held.
	game::FireArm plain(game::FireArmSettings {});
	CHECK(plain.fireMode() == game::kFireAutomatic);
	game::FireArm held(settings);
	held.setUseFireModes(false);
	int heldShots = 0;
	for (int i = 0; i < 30; ++i) {
		heldShots += held.tick(1 / 30.0f, true) ? 1 : 0;
	}
	CHECK(heldShots == 10);
}

// Tracers (0x005f2bb0, 0x005b3e60, 0x005b3cb0): every tracerInterval-th
// round from the first; the tracer trails the round along its flight,
// tracerScaler x tracerSizeModifier long at most and never past its start,
// and closes on it from the muzzle over tracerConvergeDistance.
TEST(Weapons_Tracers)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericProjectile");
	world::ObjectTemplate* round = templates.create("GenericProjectile", "round");
	round->settings()["tracerTemplate"] = "p_tracer2_eu";
	round->settings()["tracerInterval"] = "2";
	round->settings()["tracerScaler"] = "40";
	round->settings()["tracerSizeModifier"] = "0.25";
	round->settings()["tracerConvergeDistance"] = "75";
	const game::ProjectileSettings s = game::ProjectileSettings::fromTemplate(*round);
	CHECK(s.tracerTemplate == "p_tracer2_eu" && s.tracerInterval == 2 && s.tracerScaler == 40);
	CHECK(s.tracerSizeModifier == 0.25f && s.tracerConvergeDistance == 75 && s.maxTracerScaler == 0.1f);
	game::FireArm gun(game::FireArmSettings {});
	std::vector<bool> tracers;
	for (int i = 0; i < 5; ++i) {
		tracers.push_back(gun.takeTracer(s.tracerInterval));
	}
	CHECK(tracers == (std::vector<bool> {true, false, true, false, true}));

	const physics::Vec3 origin {0, 1, 0}, offset {0.3f, -0.2f, 0};
	const physics::Vec3 velocity {0, 0, 300};
	// 5 m out: the length is the 5 m flown; 15 % of the way to converging.
	game::TracerLook look = game::tracerLook(s, {0, 1, 5}, velocity, origin, origin, offset, false);
	CHECK(look.shown);
	CHECK(std::fabs(look.axes.rows[2].z - 5) < 1e-4f && std::fabs(look.axes.rows[0].x - 0.25f) < 1e-5f);
	CHECK(std::fabs(look.axes.rows[1].y - 0.25f) < 1e-5f);
	CHECK(std::fabs(look.position.x - 0.3f * (1 - 5 / 75.0f)) < 1e-5f);
	// 50 m: the full 10 m; past 75 m on the round.
	look = game::tracerLook(s, {0, 1, 50}, velocity, origin, origin, offset, false);
	CHECK(std::fabs(look.axes.rows[2].z - 10) < 1e-4f);
	look = game::tracerLook(s, {0, 1, 80}, velocity, origin, origin, offset, false);
	CHECK(look.position.x == 0 && look.position.z == 80);
	// After a hit it sits on the round; a converge distance of -1 always does.
	look = game::tracerLook(s, {0, 1, 5}, velocity, origin, origin, offset, true);
	CHECK(look.position.x == 0);
	game::ProjectileSettings at = s;
	at.tracerConvergeDistance = -1;
	CHECK(game::tracerLook(at, {0, 1, 5}, velocity, origin, origin, offset, false).position.x == 0);
	// Not yet out of the barrel, or not moving: nothing to draw.
	CHECK(!game::tracerLook(s, {0, 1, 0}, velocity, origin, origin, offset, false).shown);
	CHECK(!game::tracerLook(s, {0, 1, 5}, {0, 0, 0.1f}, origin, origin, offset, false).shown);
}

// The camera's recoil (0x00610180, 0x006101c0): one sine over 1 / speed
// seconds, size x 0.6 degrees at its peak.
TEST(Weapons_CameraRecoil)
{
	game::CameraRecoil recoil;
	CHECK(recoil.update(1 / 30.0f) == 0);
	recoil.start(7.5f, 2); // eu_tank's main gun: cameraRecoilSpeed 12, size 2 (here 7.5: a quarter at 1/30 s)
	const float first = recoil.update(1 / 30.0f); // phase 0.25
	CHECK(std::fabs(first - 1.2f) < 1e-4f);
	const float second = recoil.update(1 / 30.0f); // 0.5
	CHECK(std::fabs(second) < 1e-4f);
	CHECK(std::fabs(recoil.update(1 / 30.0f) + 1.2f) < 1e-4f); // 0.75
	recoil.update(1 / 30.0f);                                    // 1: still on
	CHECK(recoil.on);
	recoil.update(1 / 30.0f); // past 1: done
	CHECK(!recoil.on && recoil.update(1 / 30.0f) == 0);
}

// WeaponBasedRecoilComp's own slide (0x005ddfc0 / 0x005ddd80): eu_tank's
// main gun (recoilSpeed 12, recoilSpeedGB 0.45, recoilSize 2): the kick
// lasts round(pi / 12 x 15) = 4 ticks out to 2 x 0.025 = 5 cm back, then
// the return over round(1.57 / 0.45 x 30) = 105 ticks, linear.
TEST(Weapons_GunRecoil)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericFireArm");
	world::ObjectTemplate* gun = templates.create("GenericFireArm", "tank_gun");
	gun->settings()["recoil.recoilSpeed"] = "12";
	gun->settings()["recoil.recoilSpeedGB"] = "0.45";
	gun->settings()["recoil.recoilSize"] = "2";
	gun->settings()["recoil.recoilForce"] = "125";
	CHECK(!game::GunRecoil::fromTemplate(*gun).any()); // without the component nothing
	gun->components().push_back("WeaponBasedRecoilComp");
	game::GunRecoil recoil = game::GunRecoil::fromTemplate(*gun);
	CHECK(recoil.any() && recoil.force == 125);
	CHECK(recoil.slide(100) == 0);
	recoil.fired(100);
	CHECK(recoil.kickEnd == 104 && recoil.returnEnd == 209);
	CHECK(std::fabs(recoil.slide(100) - 0.05f * std::sin((3.14f / 12 * 0.5f + 4 / 30.0f) * 12)) < 1e-5f); // ~0
	CHECK(recoil.slide(100) < 0.005f);
	CHECK(std::fabs(recoil.slide(104) - 0.05f) < 1e-4f); // all the way back
	const float half = recoil.slide(104 + 52);           // half the return gone
	CHECK(std::fabs(half - 0.05f * (53 / 30.0f) / (1.57f / 0.45f)) < 1e-4f);
	CHECK(recoil.slide(209) == 0 && recoil.slide(400) == 0);
}

// The handheld pulse meter's scan (PulseMeterAmmoComp: 0x005d9cf0 starts
// it with a round, 0x005d99e0 runs it at pulse-meter-scan-step a second
// and clears it at 1), which the weapon HUD's HudScanProgress shows.
TEST(Weapons_PulseMeterScan)
{
	game::FireArmSettings s;
	s.magSize = -1;
	s.nrOfMags = -1;
	s.roundsPerMinute = 60;
	s.pulseScan = true;
	game::FireArm meter(s);
	CHECK(!meter.scanning() && meter.scan() == 0);
	CHECK(meter.tick(1 / 30.0f, true));
	CHECK(meter.scanning() && meter.scan() == 0);
	for (int i = 0; i < 15; ++i) {
		meter.tick(1 / 30.0f, false);
	}
	CHECK(meter.scanning() && std::fabs(meter.scan() - 0.5f) < 1e-3f);
	for (int i = 0; i < 15; ++i) {
		meter.tick(1 / 30.0f, false);
	}
	CHECK(!meter.scanning() && meter.scan() == 0);
	game::FireArmSettings plain = s;
	plain.pulseScan = false;
	game::FireArm gun(plain);
	CHECK(gun.tick(1 / 30.0f, true) && !gun.scanning());
}

// A ThrownFireComp's round (0x005e5a10 -> 0x005e4c80): decided on the
// press, it leaves fire.pullbackTime + fire.fireLaunchDelay later (the frag
// grenade's 1 + 0.39 s: 30 + 12 ticks), the magazine spent at once; other
// fire components fire at once.
TEST(Weapons_ThrownRoundsWaitForThePullBack)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericFireArm");
	world::ObjectTemplate* frag = templates.create("GenericFireArm", "Unl_Grenade_Frag");
	frag->components().push_back("ThrownFireComp");
	frag->settings()["fire.pullBackTime"] = "1";
	frag->settings()["fire.fireLaunchDelay"] = "0.39";
	frag->settings()["ammo.magSize"] = "1";
	frag->settings()["ammo.nrOfMags"] = "2";
	const game::FireArmSettings settings = game::FireArmSettings::fromTemplate(*frag);
	CHECK(settings.thrown && settings.pullbackTicks == 30 && settings.launchDelayTicks == 12);
	game::FireArm grenade(settings);
	int tick = 0, left = -1;
	CHECK(!grenade.tick(1 / 30.0f, true));
	CHECK(grenade.throwing() && grenade.roundsInMag() == 0);
	for (tick = 1; tick < 100; ++tick) {
		if (grenade.tick(1 / 30.0f, false)) {
			left = tick;
			break;
		}
	}
	CHECK(left == 42 && !grenade.throwing());
	// The default pull-back is 0.2 s (the setter 0x005e4c40's template default).
	world::ObjectTemplate* hub = templates.create("GenericFireArm", "Unl_Hub_Ammo");
	hub->components().push_back("ThrownFireComp");
	CHECK(game::FireArmSettings::fromTemplate(*hub).pullbackTicks == 6);
	world::ObjectTemplate* rifle = templates.create("GenericFireArm", "eu_ar_rifle");
	rifle->settings()["fire.fireLaunchDelay"] = "0.39";
	game::FireArm gun(game::FireArmSettings::fromTemplate(*rifle));
	CHECK(gun.tick(1 / 30.0f, true));
}

// The detonation component's defaults (0x005e9df0): maxDepthForExplosion
// 2, useCollisionNormal on.
TEST(Weapons_DetonationDepthAndNormal)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericProjectile");
	world::ObjectTemplate* shell = templates.create("GenericProjectile", "shell");
	game::ProjectileSettings s = game::ProjectileSettings::fromTemplate(*shell);
	CHECK(s.maxDepthForExplosion == 2 && s.useCollisionNormal);
	shell->settings()["detonation.maxDepthForExplosion"] = "0.5";
	shell->settings()["detonation.useCollisionNormal"] = "0";
	s = game::ProjectileSettings::fromTemplate(*shell);
	CHECK(s.maxDepthForExplosion == 0.5f && !s.useCollisionNormal);
}

// A GenericFireArm without ObjectTemplate.velocity fires at 600 m/s (its
// template's constructor, 0x005c2800: +0x2f0 = 600): the shipped SMGs and
// handguns set none.
TEST(Weapons_FireArmVelocityDefault)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericFireArm");
	world::ObjectTemplate* smg = templates.create("GenericFireArm", "smg");
	smg->settings()["projectileTemplate"] = "smg_round";
	CHECK(game::FireArmSettings::fromTemplate(*smg).velocity == 600);
	smg->settings()["velocity"] = "0"; // ParachuteLauncher's
	CHECK(game::FireArmSettings::fromTemplate(*smg).velocity == 0);
	smg->settings()["velocity"] = "900";
	CHECK(game::FireArmSettings::fromTemplate(*smg).velocity == 900);
}

// The soldier deviation's firing build-up off the trigger (0x005de980):
// it decays by `decay` and stops at 0 on the side it builds from. A machine
// gun's (setFireDev -1 -0.09 -0.009) steadies with firing, then comes back
// up to 0 and no further, however long it rests (it used to grow without
// bound: 0.27 a second, tens of degrees after a minute).
TEST(Weapons_SoldierDeviationSteadiesBackToZero)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericFireArm");
	world::ObjectTemplate* mg = templates.create("GenericFireArm", "mg");
	mg->components().push_back("SoldierDeviationComp");
	mg->settings()["deviation.setFireDev"] = "-1 -0.09 -0.009";
	mg->settings()["deviation.minDev"] = "1.15";
	mg->settings()["deviation.devModStand"] = "2";
	game::SoldierDeviation d = game::SoldierDeviation::fromTemplate(*mg);
	for (int i = 0; i < 20; ++i) {
		d.fire.fired();
		d.update(0, 0, 0, 0, false, true, 0, false);
	}
	CHECK(std::fabs(d.fire.value + 1) < 1e-5f); // held at max -1
	CHECK(std::fabs(d.deviation() - 0.15f * 2) < 1e-4f);
	d.update(0, 0, 0, 0, false, false, 0, false);
	CHECK(std::fabs(d.fire.value + 0.991f) < 1e-5f);
	for (int i = 0; i < 30 * 120; ++i) {
		d.update(0, 0, 0, 0, false, false, 0, false);
	}
	CHECK(d.fire.value == 0);
	CHECK(std::fabs(d.deviation() - 1.15f * 2) < 1e-4f);
	// A rifle's (positive perShot) stops at 0 from above, as before.
	mg->settings()["deviation.setFireDev"] = "2.0 0.24 0.09";
	d = game::SoldierDeviation::fromTemplate(*mg);
	d.fire.fired();
	for (int i = 0; i < 10; ++i) {
		d.update(0, 0, 0, 0, false, false, 0, false);
	}
	CHECK(d.fire.value == 0);
}

// The underarm (soft) throw (ThrownFireComp with fire.altFireInput,
// 0x005e4d80 / 0x005e4eb0 / 0x005e5720): the alternate press starts the
// throw, the timer waits while it is held, the power is min(1, ticks held
// / 30) and the round leaves pull-back + launch delay after the release at
// the speed x the power; the primary press throws at full power.
TEST(Weapons_SoftThrowScalesThePower)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericFireArm");
	world::ObjectTemplate* frag = templates.create("GenericFireArm", "Unl_Grenade_Frag");
	frag->components().push_back("ThrownFireComp");
	frag->settings()["fire.pullBackTime"] = "0.2";
	frag->settings()["fire.altFireInput"] = "PIAltFire";
	frag->settings()["ammo.magSize"] = "1";
	frag->settings()["ammo.nrOfMags"] = "5";
	frag->settings()["ammo.reloadTime"] = "0";
	const game::FireArmSettings settings = game::FireArmSettings::fromTemplate(*frag);
	CHECK(settings.softThrow && settings.pullbackTicks == 6);
	const auto throwFor = [&](game::FireArm& g, int held) {
		CHECK(!g.tick(1 / 30.0f, false, true)); // the press
		for (int i = 1; i < held; ++i) {
			CHECK(!g.tick(1 / 30.0f, false, true));
		}
		int ticks = 0;
		while (!g.tick(1 / 30.0f, false, false) && ticks < 100) {
			++ticks;
		}
		return ticks;
	};
	game::FireArm a(settings);
	const int after = throwFor(a, 16);
	CHECK(after == 6);                         // the pull-back after the release
	CHECK(std::fabs(a.throwPower() - 0.5f) < 1e-4f); // 15 ticks held after the press
	game::FireArm b(settings);
	throwFor(b, 60);
	CHECK(b.throwPower() == 1);
	game::FireArm c(settings);
	CHECK(!c.tick(1 / 30.0f, true));
	int ticks = 0;
	while (!c.tick(1 / 30.0f, false) && ticks < 100) {
		++ticks;
	}
	CHECK(c.throwPower() == 1 && ticks == 5);
	// No altFireInput: the alternate does nothing.
	frag->settings().erase("fire.altFireInput");
	game::FireArm d(game::FireArmSettings::fromTemplate(*frag));
	CHECK(!d.tick(1 / 30.0f, false, true) && !d.throwing());
}

// DefaultPenetrateComp (0x005eaca0, called from the round's 0x005b54e0):
// the material default damageLoss -1 stops everything; a solid needs
// allowSolidPenetration unless the material has overrideNeverPenetrate
// (the Titans' shields, 4112 / 4118); one-sided materials cost
// minDamageLoss, others thickness x damageLoss within min and max, or max
// without a way out; the round goes on with at least a tenth of its
// template damage.
TEST(Weapons_PenetrationRules)
{
	game::ProjectileSettings bullet;
	bullet.penetrator = true;
	bullet.damage = 40;
	game::ProjectileSettings rocket = bullet;
	rocket.allowSolidPenetration = false;
	rocket.allowLiquidPenetration = false;
	rocket.damage = 110000;
	game::ProjectileSettings none = bullet;
	none.penetrator = false;

	physics::Material concrete; // the defaults (0x0060a9b0)
	CHECK(concrete.damageLoss == -1 && concrete.maxDamageLoss == 100000);
	CHECK(game::penetration(bullet, concrete, true) == game::Penetration::No);

	physics::Material glass; // Penetrable_Glass_common
	glass.damageLoss = 0;
	glass.minDamageLoss = 10;
	glass.isOneSided = true;
	CHECK(game::penetration(bullet, glass, true) == game::Penetration::Loss);
	CHECK(game::penetration(rocket, glass, true) == game::Penetration::No);
	CHECK(game::penetration(none, glass, true) == game::Penetration::No);
	CHECK(game::penetration(bullet, glass, false) == game::Penetration::No); // the terrain
	CHECK(game::penetrationLoss(glass, false, 0) == 10);
	CHECK(game::penetrationLeft(bullet, 40 - 10) && !game::penetrationLeft(bullet, 3.9f));

	physics::Material shield; // veh_titan_penetrate_shield_red
	shield.damageLoss = 1000;
	shield.minDamageLoss = 1000;
	shield.penetrationDeviation = 0;
	shield.overrideNeverPenetrate = true;
	CHECK(game::penetration(rocket, shield, true) == game::Penetration::Loss);
	const float loss = game::penetrationLoss(shield, true, 1.64f);
	CHECK(std::fabs(loss - 1640) < 0.5f);
	CHECK(game::penetrationLeft(rocket, 110000 - loss));
	CHECK(!game::penetrationLeft(bullet, 40 - loss));
	CHECK(game::penetrationLoss(shield, false, 0) == 100000);

	physics::Material water; // Water_deep
	water.damageLoss = 25;
	water.hasWaterPhysics = true;
	CHECK(game::penetration(bullet, water, false) == game::Penetration::Free);
	CHECK(game::penetration(rocket, water, false) == game::Penetration::No);
}

// delayToUse (template +0x2b8, round(x 30) ticks at +0x2bc): after the
// draw (0x005c47d0) the busy test (0x005c14f0) holds that many ticks; the
// magazine swaps at changeMagAt (0.7) of the reload (0x005f4830), the
// weapon busy to its end; autoReload + instantReloadOnEnable fill an
// empty magazine at the draw (0x005f4bb0); toggleWhenNoAmmo makes an
// empty weapon without magazines unselectable (0x005f39c0).
TEST(Weapons_DrawDelayMagazineSwapAndToggle)
{
	game::FireArmSettings smg;
	smg.roundsPerMinute = 1800;
	smg.magSize = 2;
	smg.nrOfMags = 3;
	smg.reloadTime = 2;
	smg.delayToUseTicks = 35; // eu_smg's 1.166 s
	game::FireArm a(smg);
	a.enable(true);
	CHECK(a.readying());
	int ticks = 0;
	while (!a.tick(1 / 30.0f, true) && ticks < 100) {
		++ticks;
	}
	CHECK(ticks == 35);
	// Empty it: the reload starts by itself, the swap at 1.4 s, the end at 2 s.
	while (a.roundsInMag() > 0) {
		a.tick(1 / 30.0f, true);
	}
	CHECK(a.reloading());
	int swapAt = -1, readyAt = -1;
	for (int t = 1; t <= 90 && readyAt < 0; ++t) {
		a.tick(1 / 30.0f, false);
		if (swapAt < 0 && a.roundsInMag() == 2) {
			swapAt = t;
		}
		if (!a.reloading()) {
			readyAt = t;
		}
	}
	CHECK(swapAt >= 41 && swapAt <= 43);
	CHECK(readyAt >= 59 && readyAt <= 61);
	CHECK(a.magsLeft() == 2);

	game::FireArmSettings c4;
	c4.magSize = 1;
	c4.nrOfMags = 1;
	c4.reloadTime = 1;
	c4.autoReload = true;
	c4.instantReloadOnEnable = true;
	c4.toggleWhenNoAmmo = true;
	game::FireArm b(c4);
	b.enable(true);
	CHECK(b.tick(1 / 30.0f, true) && b.roundsInMag() == 0);
	b.dropReload();       // put away before its reload
	b.enable(true);       // drawn again: filled at once
	CHECK(b.roundsInMag() == 1 && b.magsLeft() == 0 && b.selectable());
	CHECK(b.tick(1 / 30.0f, false) == false);
	for (int t = 0; t < 5 && !b.tick(1 / 30.0f, t % 2 == 0); ++t) {
	}
	CHECK(b.roundsInMag() == 0 && !b.selectable()); // the last one gone
}
