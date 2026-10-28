// Firearms and projectiles.
#include "Game/Weapons.h"
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
