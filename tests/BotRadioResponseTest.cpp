#include "Game/Bots.h"
#include "TitanLevel.h"
#include "World/TemplateFiles.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dice::hfe;

namespace {
struct RadioRound {
	titantest::TitanLevel level;
	game::BotSettings settings;
	std::unique_ptr<game::BotManager> bots;
	int human = -1, helper = -1;

	RadioRound(const char* gameDir, const char* mod, const char* kit)
		: level(gameDir, "Cerbere_Landing", 16, true, mod, false, nullptr, "gpm_sp")
	{
		CHECK(level.loaded);
		game::registerBotConsoleMethods(level.console, settings);
		std::string out;
		CHECK(level.console.run("AI/AIBehaviours.ai", {}, out));
		CHECK(level.console.run("AI/AIDefault.ai", {}, out));
		for (const auto& script : world::templateScripts(level.files, "Objects/", true)) {
			if (script.size() > 3 && script.compare(script.size() - 3, 3, ".ai") == 0) level.console.run(script, {}, out);
		}
		level.world->autoRespawn = false;
		// The human keeps the primary weapon drawn; the bot must draw its own hub.
		human = add("Caller", false, "EU_Assault");
		helper = add("Helper", true, kit);
		const auto at = info(human).position;
		const float x = at.x + 15;
		CHECK(level.world->setSoldierPosition(info(helper).soldier,
			{x, level.heightmaps.primary()->heightAt(x, at.z), at.z}));
	}
	int add(const char* name, bool ai, const char* kit)
	{
		const int id = level.world->addPlayer(name, 2, ai);
		CHECK(level.templates.find(kit));
		level.world->setKit(id, kit, "us_light_soldier");
		CHECK(level.world->joinSquad(id, 1));
		CHECK(level.world->spawnSoldier(id));
		return id;
	}
	game::GameWorld::PlayerInfo info(int id)
	{
		game::GameWorld::PlayerInfo p;
		CHECK(level.world->playerInfo(id, p));
		return p;
	}
	void run(float seconds)
	{
		for (int tick = 0; tick < static_cast<int>(seconds * 30); ++tick) {
			if (bots) bots->update(1 / 30.0f);
			level.world->tick(1 / 30.0f);
		}
	}
	void startBots() { bots = std::make_unique<game::BotManager>(*level.world, settings); }
	int rounds()
	{
		for (const auto& w : level.world->weaponAmmo(human)) {
			if (w.inHand) return w.rounds;
		}
		return -1;
	}
};
}

TEST(BotRadio_RealMedicCallRestoresHealth)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		RadioRound f(gameDir, mod, "EU_Assault");
		f.startBots();
		f.run(0.7f);
		CHECK(f.bots->lastUrgency(f.helper, "MedicAssist") == 0); // healthy caller
		f.level.world->setHitPoints(f.info(f.human).soldier, 90);
		f.run(0.7f);
		const float before = f.bots->lastUrgency(f.helper, "MedicAssist");
		f.level.world->radioMessage(f.human, game::GameWorld::targetChannel("TCLocalTeamMedic"), "medic");
		f.run(0.7f);
		CHECK(f.bots->keepsRadio(f.helper, 3, f.human));
		const float called = f.bots->lastUrgency(f.helper, "MedicAssist");
		bool assisted = false, hubDrawn = false;
		for (int tick = 0; tick < 30 * 25; ++tick) {
			f.run(1 / 30.0f);
			assisted = assisted || f.bots->behaviour(f.helper) == "MedicAssist";
			hubDrawn = hubDrawn || f.info(f.helper).inHandSlot == 4;
		}
		const float health = f.level.world->hitPoints(f.info(f.human).soldier);
		std::printf("  %s medic: urgency %.3f -> %.3f, health 90 -> %.1f, assist=%d hub=%d\n", mod, before, called, health, assisted, hubDrawn);
		CHECK(called > before);
		CHECK(assisted && hubDrawn && health > 90);
	}
}

TEST(BotRadio_RealAmmoCallRestoresAmmunition)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		RadioRound f(gameDir, mod, "EU_Support");
		game::SoldierInput input;
		input.selectItem = 3;
		f.level.world->setInput(f.human, input);
		f.run(1);
		const int full = f.rounds();
		for (int magazine = 0; magazine < 3; ++magazine) {
			input = {}; input.fire = true; input.aimPitch = 75;
			f.level.world->setInput(f.human, input);
			f.run(6);
			input = {}; input.reload = true;
			f.level.world->setInput(f.human, input);
			f.run(0.1f);
			f.level.world->setInput(f.human, {});
			f.run(4);
		}
		const int spent = f.rounds();
		CHECK(spent < full);
		f.startBots();
		f.run(0.7f);
		const float before = f.bots->lastUrgency(f.helper, "AmmoAssist");
		f.level.world->radioMessage(f.human, game::GameWorld::targetChannel("TCTeam"), "need_ammo");
		f.run(0.7f);
		CHECK(f.bots->keepsRadio(f.helper, 10, f.human));
		const float called = f.bots->lastUrgency(f.helper, "AmmoAssist");
		bool assisted = false, hubDrawn = false;
		for (int tick = 0; tick < 30 * 25; ++tick) {
			f.run(1 / 30.0f);
			assisted = assisted || f.bots->behaviour(f.helper) == "AmmoAssist";
			hubDrawn = hubDrawn || f.info(f.helper).inHandSlot == 4;
		}
		const int after = f.rounds();
		std::printf("  %s ammo: full=%d spent=%d after=%d urgency %.3f -> %.3f assist=%d hub=%d\n", mod, full, spent, after, before, called, assisted, hubDrawn);
		CHECK(called > before);
		CHECK(assisted && hubDrawn && after > spent);
	}
}

TEST(BotRadio_RealRepairCallRestoresVehicle)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		RadioRound f(gameDir, mod, "EU_Anti-Vehicle");
		const auto at = f.info(f.human).position;
		const float x = at.x + 5;
		auto* vehicleTemplate = f.level.templates.find("eu_fav");
		CHECK(vehicleTemplate);
		const uint32_t car = f.level.objects.create(vehicleTemplate,
			{x, f.level.heightmaps.primary()->heightAt(x, at.z) + 1.5f, at.z})->id();
		f.run(2);
		physics::Vec3 entry;
		CHECK(f.level.world->entryPoint(car, entry));
		CHECK(f.level.world->setSoldierPosition(f.info(f.human).soldier, entry));
		game::SoldierInput input; input.use = true;
		f.level.world->setInput(f.human, input); f.run(1 / 30.0f);
		f.level.world->setInput(f.human, {}); f.run(1 / 30.0f);
		CHECK(f.info(f.human).vehicle == car);
		// Fill the other seats: boarding is otherwise a competing real AI action.
		for (int seat = 0; seat < 2; ++seat) {
			const int rider = f.add("Passenger", false, "EU_Assault");
			CHECK(f.level.world->entryPoint(car, entry));
			CHECK(f.level.world->setSoldierPosition(f.info(rider).soldier, entry));
			f.level.world->setInput(rider, input); f.run(1 / 30.0f);
			f.level.world->setInput(rider, {}); f.run(1 / 30.0f);
			CHECK(f.info(rider).vehicle == car);
		}
		const float full = f.level.world->hitPoints(car);
		f.level.world->setHitPoints(car, full * 0.5f);
		const float damaged = f.level.world->hitPoints(car);
		f.startBots(); f.run(0.7f);
		f.level.world->radioMessage(f.human, game::GameWorld::targetChannel("TCTeam"), "need_repair");
		f.run(0.7f);
		CHECK(f.bots->keepsRadio(f.helper, 4, f.human));
		bool assisted = false, toolDrawn = false;
		for (int tick = 0; tick < 30 * 30; ++tick) {
			f.run(1 / 30.0f);
			assisted = assisted || f.bots->behaviour(f.helper) == "RepairVehicle";
			toolDrawn = toolDrawn || f.info(f.helper).inHandSlot == 4;
		}
		const float after = f.level.world->hitPoints(car);
		std::printf("  %s repair: HP %.1f -> %.1f (full %.1f) assist=%d tool=%d\n", mod, damaged, after, full, assisted, toolDrawn);
		CHECK(assisted && toolDrawn && after > damaged);
	}
}

TEST(BotRadio_RealPickupCallMakesDriverWaitAndAnswer)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		RadioRound f(gameDir, mod, "EU_Anti-Vehicle");
		const auto at = f.info(f.human).position;
		const auto look = f.level.world->viewForward(f.human);
		const float x = at.x + look.x * 20, z = at.z + look.z * 20;
		const uint32_t car = f.level.objects.create(f.level.templates.find("eu_fav"),
			{x, f.level.heightmaps.primary()->heightAt(x, z) + 1.5f, z})->id();
		f.run(2);
		physics::Vec3 entry;
		CHECK(f.level.world->entryPoint(car, entry));
		CHECK(f.level.world->setSoldierPosition(f.info(f.helper).soldier, entry));
		game::SoldierInput input;
		input.use = true;
		f.level.world->setInput(f.helper, input); f.run(1 / 30.0f);
		f.level.world->setInput(f.helper, {}); f.run(1 / 30.0f);
		CHECK(f.info(f.helper).vehicle == car);
		f.startBots(); f.run(0.1f);
		f.level.world->radioMessage(f.human, game::GameWorld::targetChannel("TCLocalGroundVehicle"), "req_pickup");
		f.run(0.5f);
		const bool kept = f.bots->keepsRadio(f.helper, 2, f.human);
		const float idle = f.bots->lastUrgency(f.helper, "Idle");
		f.run(3);
		bool answered = false;
		for (const auto& r : f.bots->radioSent(f.helper)) answered = answered || r.type == 6;
		const float speed = std::sqrt(physics::lengthSquared(f.info(f.helper).velocity));
		std::printf("  %s pickup: received=%d idle-priority=%.1f acknowledged=%d behavior=%s speed=%.3f m/s\n",
			mod, kept, idle, answered, f.bots->behaviour(f.helper).c_str(), speed);
		CHECK(kept && answered && idle >= 30);
		CHECK(f.info(f.helper).vehicle == car && f.bots->behaviour(f.helper) == "Idle" && speed < 1);
	}
}

// AIDLL 0x1006a137..0x1006a3a1: a bot driver hearing "go go go" from a
// passenger aboard its own vehicle answers roger_that (6) on channel 7.
TEST(BotRadio_GoGoGoFromPassengerGetsRoger)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		RadioRound f(gameDir, mod, "EU_Anti-Vehicle");
		const auto at = f.info(f.human).position;
		const auto look = f.level.world->viewForward(f.human);
		const float x = at.x + look.x * 20, z = at.z + look.z * 20;
		const uint32_t car = f.level.objects.create(f.level.templates.find("eu_fav"),
			{x, f.level.heightmaps.primary()->heightAt(x, z) + 1.5f, z})->id();
		f.run(2);
		physics::Vec3 entry;
		CHECK(f.level.world->entryPoint(car, entry));
		game::SoldierInput use;
		use.use = true;
		for (const int id : {f.helper, f.human}) {
			CHECK(f.level.world->setSoldierPosition(f.info(id).soldier, entry));
			f.level.world->setInput(id, use); f.run(1 / 30.0f);
			f.level.world->setInput(id, {}); f.run(1 / 30.0f);
			CHECK(f.info(id).vehicle == car);
		}
		CHECK(f.info(f.helper).driving && !f.info(f.human).driving);
		f.startBots(); f.run(0.1f);
		f.level.world->radioMessage(f.human, game::GameWorld::targetChannel("TCLocalTeam"), "PLAYER_TACTICS_gogogo");
		f.run(2);
		bool roger = false;
		for (const auto& r : f.bots->radioSent(f.helper)) roger = roger || (r.type == 6 && r.channel == 7);
		std::printf("  %s gogogo: roger on channel 7=%d\n", mod, roger);
		CHECK(roger);
	}
}

// AIDLL 0x1006a29f: while the driver waits for a human (player slot 0x84),
// a passenger's "go go go" is marked answered and dropped: no roger, and
// the wait goes on.
TEST(BotRadio_GoGoGoIgnoredWhileWaitingForHuman)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		RadioRound f(gameDir, mod, "EU_Anti-Vehicle");
		const int rider = f.add("Rider", false, "EU_Assault");
		const auto at = f.info(f.human).position;
		const auto look = f.level.world->viewForward(f.human);
		const float x = at.x + look.x * 20, z = at.z + look.z * 20;
		const uint32_t car = f.level.objects.create(f.level.templates.find("eu_fav"),
			{x, f.level.heightmaps.primary()->heightAt(x, z) + 1.5f, z})->id();
		f.run(2);
		physics::Vec3 entry;
		CHECK(f.level.world->entryPoint(car, entry));
		game::SoldierInput use;
		use.use = true;
		for (const int id : {f.helper, rider}) {
			CHECK(f.level.world->setSoldierPosition(f.info(id).soldier, entry));
			f.level.world->setInput(id, use); f.run(1 / 30.0f);
			f.level.world->setInput(id, {}); f.run(1 / 30.0f);
			CHECK(f.info(id).vehicle == car);
		}
		f.startBots(); f.run(0.1f);
		f.level.world->radioMessage(f.human, game::GameWorld::targetChannel("TCLocalGroundVehicle"), "req_pickup");
		f.run(3); // past roger_that's 2 s send period
		f.level.world->radioMessage(rider, game::GameWorld::targetChannel("TCLocalTeam"), "PLAYER_TACTICS_gogogo");
		f.run(0.5f);
		bool roger = false;
		for (const auto& r : f.bots->radioSent(f.helper)) roger = roger || (r.type == 6 && r.channel == 7);
		const float idle = f.bots->lastUrgency(f.helper, "Idle");
		std::printf("  %s gogogo while waiting for a human: roger on channel 7=%d idle=%.1f\n", mod, roger, idle);
		CHECK(!roger && idle >= 30);
	}
}

// AIDLL 0x10069eee: a human asking for a pickup beyond the 50 m the driver
// waits for (0x10069dd9) gets "negative" (7) on channel 6, and no wait.
TEST(BotRadio_FarPickupCallGetsNegative)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		RadioRound f(gameDir, mod, "EU_Anti-Vehicle");
		const auto at = f.info(f.human).position;
		const auto look = f.level.world->viewForward(f.human);
		const float x = at.x + look.x * 70, z = at.z + look.z * 70;
		const uint32_t car = f.level.objects.create(f.level.templates.find("eu_fav"),
			{x, f.level.heightmaps.primary()->heightAt(x, z) + 1.5f, z})->id();
		f.run(2);
		physics::Vec3 entry;
		CHECK(f.level.world->entryPoint(car, entry));
		CHECK(f.level.world->setSoldierPosition(f.info(f.helper).soldier, entry));
		game::SoldierInput use;
		use.use = true;
		f.level.world->setInput(f.helper, use); f.run(1 / 30.0f);
		f.level.world->setInput(f.helper, {}); f.run(1 / 30.0f);
		CHECK(f.info(f.helper).vehicle == car);
		f.startBots(); f.run(0.1f);
		f.level.world->radioMessage(f.human, game::GameWorld::targetChannel("TCLocalGroundVehicle"), "req_pickup");
		f.run(2);
		const bool kept = f.bots->keepsRadio(f.helper, 2, f.human);
		bool negative = false, roger = false;
		for (const auto& r : f.bots->radioSent(f.helper)) {
			negative = negative || (r.type == 7 && r.channel == 6);
			roger = roger || r.type == 6;
		}
		const float idle = f.bots->lastUrgency(f.helper, "Idle");
		std::printf("  %s far pickup: received=%d negative=%d roger=%d idle=%.1f\n", mod, kept, negative, roger, idle);
		CHECK(kept && negative && !roger && idle < 30);
	}
}
