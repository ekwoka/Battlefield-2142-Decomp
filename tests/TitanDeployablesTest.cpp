// The kit items set down on a Titan's top deck (Suez_Canal's EU Titan, its
// spawn spot at 8.8 m over the Titan's origin): each stays where it was
// put while the Titan stands, rides it when it moves (it never turns: the
// items keep their place in its frame) and still works there.
#include "Animation/AnimationConsole.h"
#include "Animation/AnimationSystem.h"
#include "Game/Weapons.h"
#include "TitanLevel.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace titantest;

namespace {

constexpr float kDeck = 8.81f; // the EU Titan's top deck over its origin

bool sameName(const std::string& a, const std::string& b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
	});
}

// A gpm_ti Suez_Canal round with the animation systems (the throws wait
// for theirs), an EU soldier on his Titan's deck with a rifle and `item`.
struct DeckRound {
	TitanLevel level;
	std::unique_ptr<anim::AnimationSystems> systems;
	bool holders = false;
	bool enhanced = true, floors = false, deckMines = false;
	int team = 2;
	game::GameWorld::TitanInfo titan {};
	int id = -1;

	DeckRound(const char* dir, const char* item, bool kitFloors, bool spareMines)
		: level(dir, "Suez_Canal", 48, false), floors(kitFloors), deckMines(spareMines)
	{
		if (!level.loaded || !level.templates.find(item)) {
			return;
		}
		systems = std::make_unique<anim::AnimationSystems>(
			[this](const std::string& path, std::vector<uint8_t>& data) { return level.files.readFile(path, data); });
		anim::registerAnimationConsoleMethods(level.console, *systems);
		level.world->setAnimationSystems(systems.get(), [this](const std::string& path) {
			std::string out;
			if (!holders) {
				holders = true;
				level.console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, out);
			}
			return systems->get(path, [&] { level.console.run(path, {}, out); });
		});
		level.world->setTitanKitFloors(&enhanced, &floors);
		level.world->setTitanDeckMines(&enhanced, &deckMines);
		level.world->startRound();
		level.run(0.1f);
		titan = *level.titan(team);
		level.templates.find("EU_Assault")->children() = {{"eu_ar_rifle", {}, {}}, {item, {}, {}}};
		id = level.world->joinPlayer(team);
		level.world->setKit(id, "EU_Assault", "us_light_soldier");
		level.world->setSpawnTitan(id, titan.object);
		CHECK(level.world->spawnSoldier(id));
		level.run(1);
	}
	bool ready() const { return id >= 0; }

	physics::Vec3 inTitan(const physics::Vec3& at)
	{
		const world::Object* hull = level.objects.find(titan.object);
		const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({hull->rotation().x, hull->rotation().y, hull->rotation().z});
		return turn.untransform(at - physics::Vec3 {hull->position().x, hull->position().y, hull->position().z});
	}
	physics::Vec3 toWorld(const physics::Vec3& local) { return level.world->titanToWorld(titan.object, local); }
	game::GameWorld::PlayerInfo me()
	{
		game::GameWorld::PlayerInfo info;
		level.world->playerInfo(id, info);
		return info;
	}
	// Takes the item of `slot` (its delayToUse waited out) and fires it once.
	void place(int slot)
	{
		level.world->setBattery(id, 1);
		game::SoldierInput select;
		select.selectItem = slot;
		level.world->setInput(id, select);
		level.run(0.1f);
		level.world->setInput(id, {});
		level.run(5);
		game::SoldierInput fire;
		fire.fire = true;
		level.world->setInput(id, fire);
		level.run(0.1f);
		level.world->setInput(id, {});
		level.run(3);
	}
	// The placed item: its round, else its object.
	bool find(const char* name, physics::Vec3& at, uint32_t* object = nullptr)
	{
		for (const auto& p : level.world->projectiles()) {
			if (p.alive && p.settings && sameName(p.settings->name, name)) {
				at = p.position;
				return true;
			}
		}
		for (const auto& o : level.objects.objects()) {
			if (o->objectTemplate() && sameName(o->objectTemplate()->name(), name)) {
				at = {o->position().x, o->position().y, o->position().z};
				if (object) {
					*object = o->id();
				}
				return true;
			}
		}
		return false;
	}
	float moved() { return std::sqrt(physics::lengthSquared(level.titan(team)->position - titan.position)); }
};

struct Placed {
	bool found = false, kept = false;
	physics::Vec3 still, after; // in the Titan's frame
};

// Places `item` on the deck, waits 3 s, then has the Titan move for 20 s.
Placed placeAndMove(DeckRound& round, const char* placed, int slot)
{
	Placed result;
	round.place(slot);
	physics::Vec3 at;
	result.found = round.find(placed, at);
	result.still = round.inTitan(at);
	round.level.run(3);
	physics::Vec3 rest;
	if (round.find(placed, rest)) {
		const physics::Vec3 r = round.inTitan(rest);
		CHECK(physics::lengthSquared(r - result.still) < 0.05f); // still on the standing Titan
	}
	CHECK(round.level.world->moveTitan(round.team, 300, -200) == game::GameWorld::TitanMove::Accepted);
	round.level.run(20);
	result.kept = round.find(placed, at);
	result.after = round.inTitan(at);
	std::fprintf(stderr, "  %s: placed %d at %.2f %.2f %.2f; the Titan %.1f m on: kept %d at %.2f %.2f %.2f\n", placed, result.found,
		result.still.x, result.still.y, result.still.z, round.moved(), result.kept, result.after.x, result.after.y, result.after.z);
	return result;
}

bool ridesOnTheDeck(const Placed& p)
{
	const float slid = std::hypot(p.after.x - p.still.x, p.after.z - p.still.z);
	return p.found && p.kept && std::fabs(p.still.y - kDeck) < 0.4f && std::fabs(p.after.y - kDeck) < 0.4f && slid < 1.5f;
}

} // namespace

// The items whose ride is retail's (enhanced flags off): the hubs (mesh
// bodies the moving Titan keeps awake, 0x005aa0e0 -> 0x006bfad0, so the
// deck's contacts carry them; asleep the deck slid out from under them),
// the anti-personnel mine and the sonars and decoy (retail's mesh rigid
// bodies too: a mine lying on a Titan goes with it; it had been let go as
// the deck moved, so it fell through or off).
TEST(Titan_DeckDeployablesRideTheMovingTitan)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	struct Item {
		const char* item;
		const char* placed;
		int slot;
	};
	for (const Item& g : {Item {"Unl_Hub_Medic", "unl_hub_medic_stationary", 4}, Item {"Unl_Hub_Ammo", "unl_hub_ammo_stationary_model", 4},
			 Item {"Unl_Mine_AntiPersonnel", "Unl_Mine_AntiPersonnel_Projectile", 5}, Item {"unl_sonar", "unl_sonar_Projectile", 5},
			 Item {"Bp1_sonar", "Bp1_sonar_Projectile", 5}, Item {"Bp1_Decoy", "Bp1_Decoy_Projectile", 5}}) {
		DeckRound round(dir, g.item, false, false);
		if (!round.ready()) {
			continue;
		}
		const Placed p = placeAndMove(round, g.placed, g.slot);
		CHECK(round.moved() > 10);
		CHECK(ridesOnTheDeck(p));
	}
}

// The portable shield and the sentry gun, set down by their origins on the
// deck, start with their lowest vertices under it (0.28 and 0.21 m) and
// fall through the hull to the floor below (retail's would too).
// enhanced.titanKitFloors starts them standing on the deck; then they ride
// the moving Titan as the hubs do.
TEST(Titan_DeckShieldAndSentryStandOnIt_TitanKitFloors)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	for (const bool on : {false, true}) {
		for (const auto& [item, placed] : {std::pair {"unl_port_shield", "portable_shield"}, std::pair {"Unl_Deployer_Sentrygun", "eu_sentrygun"}}) {
			DeckRound round(dir, item, on, false);
			if (!round.ready()) {
				continue;
			}
			round.place(5);
			physics::Vec3 at;
			CHECK(round.find(placed, at));
			const float y = round.inTitan(at).y;
			std::fprintf(stderr, "  titanKitFloors %d: %s at %.2f over the Titan's origin\n", on, placed, y);
			if (!on) {
				CHECK(y < kDeck - 5); // through the deck
				continue;
			}
			CHECK(std::fabs(y - kDeck) < 0.5f);
			CHECK(round.level.world->moveTitan(round.team, 300, -200) == game::GameWorld::TitanMove::Accepted);
			const physics::Vec3 before = round.inTitan(at);
			round.level.run(20);
			CHECK(round.find(placed, at));
			const physics::Vec3 after = round.inTitan(at);
			std::fprintf(stderr, "    the Titan %.1f m on: at %.2f %.2f %.2f (from %.2f %.2f %.2f)\n", round.moved(), after.x, after.y, after.z,
				before.x, before.y, before.z);
			CHECK(round.moved() > 10);
			CHECK(std::fabs(after.y - kDeck) < 0.5f && std::hypot(after.x - before.x, after.z - before.z) < 1.5f);
		}
	}
}

// The motion and EMP mines lie on the deck while the Titan stands; as it
// moves, the radius trigger (0x005e8aa0, no Titan case) takes it for a
// moving vehicle without a driver to spare and they go off.
// enhanced.titanDeckMines spares them, and they ride along.
TEST(Titan_DeckMinesAndTheMovingTitan_TitanDeckMines)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	for (const bool on : {false, true}) {
		for (const auto& [item, placed] : {std::pair {"Unl_Mine_Motion", "Unl_Mine_Motion_Projectile"}, std::pair {"Unl_Mine_EMP", "Unl_Mine_EMP_Projectile"}}) {
			DeckRound round(dir, item, true, on);
			if (!round.ready()) {
				continue;
			}
			std::fprintf(stderr, "  titanDeckMines %d:\n", on);
			const Placed p = placeAndMove(round, placed, 5);
			CHECK(p.found && std::fabs(p.still.y - kDeck) < 0.4f);
			if (on) {
				CHECK(ridesOnTheDeck(p));
			} else {
				CHECK(!p.kept);
			}
		}
	}
}

namespace {

// An enemy soldier (a PAC bot) put at a place in the Titan's frame.
int enemyAt(DeckRound& round, const physics::Vec3& local)
{
	const int enemy = round.level.world->addPlayer("Pac", 1, true);
	CHECK(round.level.world->spawnSoldier(enemy));
	game::GameWorld::PlayerInfo info;
	round.level.world->playerInfo(enemy, info);
	round.level.world->setSoldierPosition(info.soldier, round.toWorld(local));
	return enemy;
}

} // namespace

// A sentry gun on the moving Titan's deck tracks and shoots an enemy who
// comes aboard (enhanced.titanKitFloors keeps it on the deck).
TEST(Titan_DeckSentryFiresWhileMoving)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	DeckRound round(dir, "Unl_Deployer_Sentrygun", true, true);
	if (!round.ready()) {
		return;
	}
	round.place(5);
	physics::Vec3 at;
	uint32_t sentry = 0;
	CHECK(round.find("eu_sentrygun", at, &sentry) && sentry != 0);
	CHECK(round.level.world->moveTitan(round.team, 300, -200) == game::GameWorld::TitanMove::Accepted);
	round.level.run(5);
	CHECK(round.find("eu_sentrygun", at));
	const physics::Vec3 gun = round.inTitan(at);
	const int enemy = enemyAt(round, {gun.x + 4, kDeck + 0.1f, gun.z + 4});
	bool killed = false;
	for (int i = 0; i < 15 * 30 && !killed; ++i) {
		round.level.run(1 / 30.0f);
		killed = round.me().kills == 1;
	}
	game::GameWorld::PlayerInfo info;
	round.level.world->playerInfo(enemy, info);
	std::fprintf(stderr, "  sentry at %.2f %.2f %.2f, the Titan %.1f m on: enemy killed %d\n", gun.x, gun.y, gun.z, round.moved(), killed);
	CHECK(round.moved() > 1);
	CHECK(killed && !info.alive);
}

// The portable shield on the moving Titan's deck stops rounds: its owner
// steps back six metres behind it and shoots at it; it loses hit points.
TEST(Titan_DeckShieldStopsRoundsWhileMoving)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	DeckRound round(dir, "unl_port_shield", true, true);
	if (!round.ready()) {
		return;
	}
	round.place(5);
	physics::Vec3 at;
	uint32_t shield = 0;
	CHECK(round.find("portable_shield", at, &shield) && shield != 0);
	CHECK(round.level.world->moveTitan(round.team, 300, -200) == game::GameWorld::TitanMove::Accepted);
	round.level.run(5);
	CHECK(round.find("portable_shield", at));
	const game::GameWorld::PlayerInfo me = round.me();
	const float yaw = me.yaw * 3.14159265f / 180.0f;
	const physics::Vec3 back = at - physics::Vec3 {std::sin(yaw), 0, std::cos(yaw)} * 6.0f;
	round.level.world->setSoldierPosition(me.soldier, {back.x, me.position.y, back.z});
	game::SoldierInput take;
	take.selectItem = 3;
	round.level.world->setInput(round.id, take);
	round.level.run(0.1f);
	round.level.world->setInput(round.id, {});
	round.level.run(1.5f);
	const float before = round.level.world->hitPoints(shield);
	game::SoldierInput shoot;
	shoot.aimPitch = -12;
	float after = before;
	for (int burst = 0; burst < 40 && after >= before; ++burst) {
		shoot.fire = true;
		round.level.world->setInput(round.id, shoot);
		round.level.run(1 / 30.0f);
		shoot.fire = false;
		round.level.world->setInput(round.id, shoot);
		round.level.run(0.3f);
		after = round.level.objects.find(shield) ? round.level.world->hitPoints(shield) : 0;
	}
	std::fprintf(stderr, "  shield at %.2f over the deck, the Titan %.1f m on: %.0f -> %.0f hit points\n", round.inTitan(at).y - kDeck,
		round.moved(), before, after);
	CHECK(round.moved() > 1);
	CHECK(after < before);
}

// An anti-personnel mine on the moving Titan's deck goes off as an enemy
// runs at it from ahead.
TEST(Titan_DeckMineTriggersWhileMoving)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	DeckRound round(dir, "Unl_Mine_AntiPersonnel", true, true);
	if (!round.ready()) {
		return;
	}
	round.place(5);
	physics::Vec3 at;
	CHECK(round.find("Unl_Mine_AntiPersonnel_Projectile", at));
	const float yaw = round.me().yaw;
	// Out of the mine's way (its cone ahead, along the thrower's look).
	const game::GameWorld::PlayerInfo me = round.me();
	const physics::Vec3 aside = round.inTitan(me.position) + physics::Vec3 {0, 0.1f, 0};
	round.level.world->setSoldierPosition(me.soldier, round.toWorld({aside.x - 8, aside.y, aside.z - 8}));
	CHECK(round.level.world->moveTitan(round.team, 300, -200) == game::GameWorld::TitanMove::Accepted);
	round.level.run(5); // armed (4 s), the Titan under way
	CHECK(round.find("Unl_Mine_AntiPersonnel_Projectile", at));
	const physics::Vec3 mine = round.inTitan(at);
	const float rad = yaw * 3.14159265f / 180.0f;
	const physics::Vec3 ahead = round.inTitan(at + physics::Vec3 {std::sin(rad), 0, std::cos(rad)}) - mine; // the yaw is the world's
	const int enemy = enemyAt(round, {mine.x + ahead.x * 12, kDeck + 0.1f, mine.z + ahead.z * 12});
	game::GameWorld::PlayerInfo info;
	round.level.world->playerInfo(enemy, info);
	game::SoldierInput run;
	run.throttle = 1;
	run.sprint = true;
	run.turn = std::remainder(yaw + 180 - info.yaw, 360.0f); // face the mine
	const size_t rounds = round.level.world->projectiles().size();
	bool off = false;
	for (int i = 0; i < 150 && !off; ++i) {
		round.level.world->setInput(enemy, run);
		run.turn = 0;
		round.level.run(1 / 30.0f);
		off = !round.find("Unl_Mine_AntiPersonnel_Projectile", at);
	}
	round.level.world->playerInfo(enemy, info);
	std::fprintf(stderr, "  mine at %.2f %.2f %.2f (%zu rounds), the Titan %.1f m on: off %d, the enemy alive %d\n", mine.x, mine.y, mine.z,
		rounds, round.moved(), off, info.alive);
	CHECK(round.moved() > 1);
	CHECK(off && !info.alive);
}
