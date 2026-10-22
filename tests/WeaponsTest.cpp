// Firearms and projectiles.
#include "Game/Weapons.h"
#include "TestMain.h"
#include "World/ObjectTemplateManager.h"

#include <cmath>

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
