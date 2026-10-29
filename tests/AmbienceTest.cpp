// Level ambience: Trigger and AmbientEffectArea objects (AmbientTriggers.h).
#include "Game/AmbientTriggers.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <cmath>

using namespace dice::hfe;

namespace {

// A trigger (events, chance, average time, minimum time) at the origin of
// radius 10 and its area at (20, 0, 0), radius 5, playing e_bird.
void addTriggerAndArea(world::ObjectTemplateManager& templates, world::ObjectManager& objects, const char* events,
	const char* chance, const char* average, const char* minimum, bool project = false, bool networked = false)
{
	world::ObjectTemplate* trigger = templates.create("Trigger", "trigger_1");
	trigger->settings()["triggerId"] = "1";
	trigger->settings()["radius"] = "10";
	trigger->settings()["events"] = events;
	trigger->settings()["chance"] = chance;
	if (average) {
		trigger->settings()["averageTimeBetweenTests"] = average;
	}
	trigger->settings()["minimumTimeBetweenTriggering"] = minimum;
	world::ObjectTemplate* area = templates.create("AmbientEffectArea", "trigger_1_area");
	area->settings()["triggerId"] = "1";
	area->settings()["radius"] = "5";
	area->settings()["linkedEffectContainer"] = "e_bird";
	area->settings()["projectOnHeightmap"] = project ? "1" : "0"; // on by default (0x005b9340); the levels say 0
	if (networked) {
		area->settings()["numNetworkedEffects"] = "1";
	}
	objects.create(trigger, {0, 0, 0});
	objects.create(area, {20, 3, 0});
}

} // namespace

TEST(Ambience_TestChanceOverTime)
{
	// 1 - (1 - chance / max(average, 1))^t, t clamped to 0..100 (0x005d3600).
	CHECK(std::fabs(game::AmbientTriggers::testChance(1, 10, 1) - 0.1f) < 1e-5f);
	CHECK(std::fabs(game::AmbientTriggers::testChance(1, 10, 2) - 0.19f) < 1e-5f);
	CHECK(std::fabs(game::AmbientTriggers::testChance(0.5f, -1, 1) - 0.5f) < 1e-5f); // the default -1 counts as 1
	CHECK(game::AmbientTriggers::testChance(1, 10, 500) == game::AmbientTriggers::testChance(1, 10, 100));
}

TEST(Ambience_PlayerInTriggerPlaysTheArea)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	addTriggerAndArea(templates, objects, "1", "1", "1", "10", true);
	game::AmbientTriggers ambience;
	ambience.groundHeight = [](float, float) { return 7.0f; };
	ambience.link(objects, templates);
	CHECK(ambience.triggers() == 1 && ambience.areas() == 1);
	// Nobody near: nothing.
	std::vector<game::AmbientTriggers::Activator> away {{0, 1, {50, 0, 0}, 0}};
	for (int i = 0; i < 60; ++i) {
		ambience.update(i / 30.0, static_cast<uint32_t>(i), away);
	}
	CHECK(ambience.takeEffects().empty());
	// A player in it: tested each tick once the minimum time has passed
	// since it last fired; chance 1 over 1 s sets it off within a tick or two.
	std::vector<game::AmbientTriggers::Activator> near {{0, 1, {3, 0, 0}, 0}};
	std::vector<game::AmbientTriggers::Effect> fired;
	for (int i = 60; i < 60 + 30 * 25; ++i) {
		ambience.update(i / 30.0, static_cast<uint32_t>(i), near);
		for (const auto& e : ambience.takeEffects()) {
			fired.push_back(e);
		}
	}
	// 25 s with 10 s between firings: three.
	CHECK(fired.size() == 3);
	for (const auto& e : fired) {
		CHECK(e.bundle == "e_bird");
		// In the area's disc, on the ground (projectOnHeightmap).
		CHECK(std::hypot(e.position.x - 20, e.position.z) <= 5.0f + 1e-4f);
		CHECK(std::fabs(e.position.y - 7) < 1e-4f);
	}
}

TEST(Ambience_MovingPlayersAndExplosions)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	addTriggerAndArea(templates, objects, "12", "1", nullptr, "2"); // 4 moving, 8 explosions
	game::AmbientTriggers ambience;
	ambience.link(objects, templates);
	// Standing still in it: no test fires it.
	std::vector<game::AmbientTriggers::Activator> still {{0, 1, {3, 0, 0}, 0}};
	for (int i = 0; i < 90; ++i) {
		ambience.update(i / 30.0, static_cast<uint32_t>(i), still);
	}
	CHECK(ambience.takeEffects().empty());
	// Running through it does.
	std::vector<game::AmbientTriggers::Activator> running {{0, 1, {3, 0, 0}, 5}};
	ambience.update(3.1, 93, running);
	ambience.update(3.2, 96, running);
	CHECK(ambience.takeEffects().size() == 1);
	// An explosion in it, past the minimum time: chance 1.
	ambience.explosion(3.3, 99, {1, 0, 1});
	CHECK(ambience.takeEffects().empty()); // within 2 s of the last
	ambience.explosion(6.0, 180, {1, 0, 1});
	CHECK(ambience.takeEffects().size() == 1);
	ambience.explosion(9.0, 270, {40, 0, 1}); // outside the sphere
	CHECK(ambience.takeEffects().empty());
}

TEST(Ambience_AreasCapTheirEffects)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	addTriggerAndArea(templates, objects, "8", "1", nullptr, "0");
	game::AmbientTriggers ambience;
	ambience.link(objects, templates);
	size_t alive = 11;
	ambience.liveEffects = [&](uint32_t) { return alive; };
	ambience.explosion(1, 30, {0, 0, 0});
	CHECK(ambience.takeEffects().empty()); // more than ten alive (0x005ba490)
	alive = 10;
	ambience.bundleTimeToLive = [](const std::string&) { return 30.0f; };
	ambience.explosion(2, 60, {0, 0, 0});
	CHECK(ambience.takeEffects().empty()); // a bundle of 30 s isn't played
	ambience.bundleTimeToLive = [](const std::string&) { return 1.0f; };
	ambience.explosion(3, 90, {0, 0, 0});
	CHECK(ambience.takeEffects().size() == 1);
}

// numNetworkedEffects: the server plays the area and says what it played
// (0x00510270: the area's id, the Bundle child, the offset); a joined
// client plays only what the server sends (0x00510230).
TEST(Ambience_NetworkedAreasAreTheServers)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	addTriggerAndArea(templates, objects, "1", "1", "1", "10", false, true);
	const auto children = [](const std::string& bundle) {
		return bundle == "e_bird" ? std::vector<std::string> {"e_bird_a", "e_bird_b"} : std::vector<std::string> {};
	};
	game::AmbientTriggers server, client;
	server.bundleChildren = client.bundleChildren = children;
	server.link(objects, templates);
	client.link(objects, templates);
	client.client = true;
	std::vector<game::AmbientTriggers::Activator> near {{0, 1, {3, 0, 0}, 0}};
	std::vector<game::AmbientTriggers::Effect> sent;
	for (int i = 0; i < 90; ++i) {
		server.update(i / 30.0, static_cast<uint32_t>(i), near);
		client.update(i / 30.0, static_cast<uint32_t>(i), near);
		for (const auto& e : server.takeEffects()) {
			sent.push_back(e);
		}
		CHECK(client.takeEffects().empty());
	}
	CHECK(sent.size() == 1);
	const game::AmbientTriggers::Effect& e = sent.front();
	CHECK(e.networked && e.areaId == 0 && (e.index == 1 || e.index == 2));
	CHECK(e.bundle == (e.index == 1 ? "e_bird_a" : "e_bird_b"));
	CHECK(std::fabs(e.position.x - (20 + e.offset.x)) < 1e-4f && std::fabs(e.position.z - e.offset.z) < 1e-4f);
	// The client plays the server's pick at the server's place.
	CHECK(client.playNetworked(e.areaId, e.index, e.offset));
	const auto played = client.takeEffects();
	CHECK(played.size() == 1 && played[0].bundle == e.bundle);
	CHECK(std::fabs(played[0].position.x - e.position.x) < 1e-4f && std::fabs(played[0].position.z - e.position.z) < 1e-4f);
	CHECK(!client.playNetworked(5, 1, {}) && !client.playNetworked(0, 3, {}));
}
