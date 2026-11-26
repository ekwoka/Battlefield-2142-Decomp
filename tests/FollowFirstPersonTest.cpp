// enhanced.followFirstPerson (not retail): a followed player's own first
// person (PlayerCamera::setSubject) and HUD (HudWorld::viewedPlayer), as
// --director's first-person shots of a bot and a demo's followed player
// (--demo-first-person) show them: his first-person arms and weapon
// (geometry 0 of the soldier's and the weapon's meshes) on his first-person
// skeleton, his zoom's field of view, his seat's camera, and the HUD's
// soldier, vehicle, weapon, rounds and health his. Retail's spectator
// (0x00614d00) and demo (0x005f8f40) follow cameras are chases behind him.
#include "Animation/AnimationConsole.h"
#include "Animation/AnimationSystem.h"
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Main/HostedGame.h"
#include "Main/HudWorld.h"
#include "Main/PlayerCamera.h"
#include "Net/Demo.h"
#include "Net/GameClient.h"
#include "Net/GhostLayout.h"
#include "Net/RemoteWorld.h"
#include "Net/ServerSettings.h"
#include "Physics/CollisionTemplateManager.h"
#include "Render/RenderMath.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

// A collision mesh of one part, a box `half` each way, for the three LOD types.
physics::CollisionMeshFile boxMesh(float h)
{
	const std::vector<std::array<float, 3>> vertices {
		{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h}, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}};
	const std::vector<physics::CollisionFace> faces {{{0, 2, 1}, 0}, {{0, 3, 2}, 0}, {{4, 5, 6}, 0}, {{4, 6, 7}, 0},
		{{0, 1, 5}, 0}, {{0, 5, 4}, 0}, {{3, 6, 2}, 0}, {{3, 7, 6}, 0}, {{0, 4, 7}, 0}, {{0, 7, 3}, 0}, {{1, 2, 6}, 0},
		{{1, 6, 5}, 0}};
	physics::CollisionSubGeometry part;
	for (uint32_t type = 0; type < 3; ++type) {
		physics::CollisionLod lod;
		lod.collisionType = type;
		lod.boundsMin = {-h, -h, -h};
		lod.boundsMax = {h, h, h};
		lod.vertices = vertices;
		lod.faces = faces;
		part.lods.push_back(lod);
	}
	physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	physics::CollisionMeshFile mesh;
	mesh.version = 10;
	mesh.geometries = {geometry};
	return mesh;
}

// Flat ground, a control point and a spawn point a team, the soldiers'
// default templates (us_light_soldier with EU_Assault for team 2).
struct Field {
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	io::FileManager files;
	physics::CollisionTemplateManager collisionTemplates;
	std::unique_ptr<game::GameWorld> world;
	game::GameLogic logic;

	Field()
	{
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
		templates.registerType(4, "GenericFireArm");
		templates.registerType(5, "GenericProjectile");
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			templates.create("Soldier", name)->settings()["armor.maxHitPoints"] = "100";
		}
		for (int team = 1; team <= 2; ++team) {
			world::ObjectTemplate* point = templates.create("ControlPoint", team == 1 ? "cp_pac" : "cp_eu");
			point->settings()["controlPointId"] = std::to_string(team);
			point->settings()["team"] = std::to_string(team);
			point->settings()["unableToChangeTeam"] = "1";
			objects.create(point, {team == 1 ? 100.0f : -100.0f, 0, 0});
		}
		world::ObjectTemplate* spawn = templates.create("SpawnPoint", "sp");
		objects.create(spawn, {0, 0, 20}, {90, 0, 0})->setControlPointId(1);
		objects.create(spawn, {0, 0, 0}, {0, 0, 0})->setControlPointId(2);
	}
	void start()
	{
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->enablePhysics(files, collisionTemplates,
			{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return 0.0f;
			 }});
		world->startRound();
		logic.startRound();
	}
};

// The shipped data a soldier's first person needs (skeleton1P,
// animationSystem1P) and a weapon of it with a zoom: the recon's sniper
// rifle. False without $BF2142_GAME_DIR.
struct FirstPersonData {
	Console console;
	std::unique_ptr<anim::AnimationSystems> systems;
	std::string out;
	world::ObjectTemplate* rifle = nullptr;
	world::ObjectTemplate* soldier = nullptr;
	std::vector<float> factors; // the rifle's zoom.addZoomFactor.list

	explicit FirstPersonData(Field& field) : console(&field.files) {}
	bool load(Field& field)
	{
		const char* gameDir = std::getenv("BF2142_GAME_DIR");
		if (!gameDir || !test::runs("bf2142")) {
			return false;
		}
		field.files.setRootPath(gameDir);
		field.files.addSearchPath("mods/bf2142");
		field.files.setMountingAllowed(true);
		bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
			[&field](std::string archive, std::string mount, bool) { field.files.mountArchive(archive, mount); }, 2);
		world::registerWorldConsoleMethods(console, field.templates, field.objects);
		std::vector<std::string> untyped;
		std::ifstream table(std::string(BF2142_SOURCE_DIR) + "/symbols/BF2142.exe.console.csv");
		std::string line;
		std::getline(table, line);
		while (std::getline(table, line)) {
			std::vector<std::string> columns;
			std::stringstream row(line);
			for (std::string column; std::getline(row, column, ',');) columns.push_back(column);
			if (columns.size() >= 6 && columns[0] == "ObjectTemplate" && !console.hasMethod("ObjectTemplate." + columns[1])
				&& (columns[2] == "property" || (columns[2] == "method" && columns[4] == "1" && columns[5] == "1"))) {
				untyped.push_back(columns[1]);
			}
		}
		world::registerUntypedTemplateProperties(console, field.templates, untyped);
		CHECK(console.run("ServerArchives.con", {}, out));
		CHECK(console.run("ClientArchives.con", {}, out));
		field.files.setMountingAllowed(false);
		CHECK(console.run("Objects/Weapons/Handheld/unl_adv_sni/unl_adv_sni.con", {}, out));
		rifle = field.templates.find("unl_adv_sni");
		if (!rifle || rifle->geometry().empty()) {
			CHECK(false);
			return false;
		}
		std::istringstream list(rifle->settings()["zoom.addZoomFactor.list"]);
		for (float f; list >> f;) factors.push_back(f);
		CHECK(factors.size() >= 2 && factors[1] > 0);
		soldier = field.templates.find("us_light_soldier");
		soldier->setGeometry("us_light_soldier_1p"); // the first-person mesh (geometry 0)
		soldier->settings()["skeleton1P"] = "Objects/Soldiers/Common/Animations/1p_setup.ske";
		soldier->settings()["animationSystem1P"] = "Objects/Soldiers/Common/Animations/AnimationSystem1p.inc";
		world::ObjectTemplate* kit = field.templates.create("Kit", "EU_Assault");
		kit->children().push_back({"unl_adv_sni", {}, {}});
		kit->settings()["networkableInfo"] = "KitInfo"; // a ghost of its own (HandlePickup names it)
		systems = std::make_unique<anim::AnimationSystems>([&field](const std::string& path, std::vector<uint8_t>& data) {
			return field.files.readFile(path, data);
		});
		anim::registerAnimationConsoleMethods(console, *systems);
		CHECK(console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, out));
		return true;
	}
	PlayerCamera::Loader loader()
	{
		return [this](const std::string& path) { return systems->get(path, [&] { console.run(path, {}, out); }); };
	}
	int rifleSlot() const { return static_cast<int>(std::atof(rifle->settings().at("itemIndex").c_str())); }
};

bool hasModel(const std::vector<PlayerCamera::Model>& models, const std::string& geometry, bool skinned)
{
	for (const PlayerCamera::Model& m : models) {
		if (m.geometry == geometry && m.geometryIndex == 0 && (skinned ? !m.pose.empty() : !m.parts.empty())) {
			return true;
		}
	}
	return false;
}

} // namespace

// A bot on foot (the director's first-person shot): his arms and weapon,
// his eye, his zoom's field of view, his soldier left out of the world;
// the HUD's soldier, rifle, rounds and health his. The local (spectating)
// player's camera shows nothing, and the round plays the same.
TEST(FollowFirstPerson_BotOnFootWeaponZoomAndHud)
{
	Field field;
	FirstPersonData data(field);
	if (!data.load(field)) {
		return;
	}
	field.start();
	const PlayerCamera::Loader load = data.loader();
	field.world->setAnimationSystems(data.systems.get(), load);
	const int bot = field.world->addPlayer("Bot", 2, true);
	CHECK(field.world->spawnSoldier(bot));
	game::SoldierInput select;
	select.selectItem = data.rifleSlot();
	field.world->setInput(bot, select);
	int zoom = 0;
	PlayerCamera local(*field.world, *data.systems, load); // the spectator's own: nothing to show
	PlayerCamera follow(*field.world, *data.systems, load);
	const auto step = [&](int ticks) {
		for (int i = 0; i < ticks; ++i) {
			field.world->tick(1 / 30.0f);
			local.tick(1 / 30.0f);
			follow.setSubject(bot, [&zoom](int) { return zoom; });
			follow.tick(1 / 30.0f);
		}
	};
	step(60);
	game::GameWorld::PlayerInfo info;
	CHECK(field.world->playerInfo(bot, info) && info.alive && info.weapon != 0);
	const world::Object* held = field.objects.find(info.weapon);
	CHECK(held && held->objectTemplate() == data.rifle);
	PlayerCamera::View eye;
	CHECK(!local.view(eye, 4.0f / 3.0f));
	CHECK(follow.subject() == bot && follow.insideView() && follow.view(eye, 4.0f / 3.0f));
	CHECK(std::fabs(eye.position[0] - info.position.x) < 0.3f && std::fabs(eye.position[2] - info.position.z) < 0.3f);
	CHECK(eye.position[1] > info.position.y + 1.2f && eye.position[1] < info.position.y + 2.0f); // his standing eye
	const float wide = eye.fovY;
	const auto models = follow.models();
	CHECK(hasModel(models, data.soldier->geometry(), true)); // his arms
	CHECK(hasModel(models, data.rifle->geometry(), false));  // his rifle's first-person geometry
	CHECK(follow.hides(info.soldier) && follow.hides(info.weapon) && !local.hides(info.soldier));
	// He zooms (BotManager::zoomLevel): the rifle's first factor, eased in over zoomDelay.
	zoom = 1;
	step(30);
	CHECK(follow.zoomLevel() == 1 && follow.zoomed());
	CHECK(follow.view(eye, 4.0f / 3.0f) && std::fabs(eye.fovY - data.factors[1]) < 1e-3f && eye.fovY < wide);
	zoom = 0;
	step(30);
	CHECK(follow.zoomLevel() == 0 && follow.view(eye, 4.0f / 3.0f) && std::fabs(eye.fovY - wide) < 1e-3f);
	// The HUD as his.
	host::LocalHudWorld hud(*field.world, field.logic);
	CHECK(hud.soldier() == nullptr && hud.localPlayer() == field.world->localPlayer());
	hud.viewedPlayer = bot;
	CHECK(hud.localPlayer() == bot && hud.soldier() && hud.soldier()->id() == info.soldier && hud.vehicle() == nullptr);
	const host::HudWorld::Weapon w = hud.heldWeapon();
	CHECK(w.weapon == data.rifle && w.gun && w.gun == field.world->playerWeapon(bot));
	CHECK(w.gun && w.rounds == w.gun->roundsInMag() && w.rounds > 0 && w.mags > 0);
	field.world->setHitPoints(info.soldier, 40);
	CHECK(std::fabs(hud.health() - 0.4f) < 1e-3f);
	hud.viewedPlayer = -1;
	CHECK(hud.soldier() == nullptr);
}

// Following a bot reads the round only: the same round with and without a
// follow camera on him ends in the same state.
TEST(FollowFirstPerson_LeavesTheRoundAlone)
{
	uint64_t prints[2] = {};
	for (int followed = 0; followed < 2; ++followed) {
		Field field;
		field.start();
		const int bot = field.world->addPlayer("Bot", 2, true);
		CHECK(field.world->spawnSoldier(bot));
		anim::AnimationSystems systems(nullptr);
		PlayerCamera follow(*field.world, systems, nullptr);
		game::SoldierInput walk;
		walk.throttle = 1;
		walk.fire = true;
		for (int i = 0; i < 90; ++i) {
			walk.turn = i % 30 == 0 ? 20.0f : 0.0f;
			field.world->setInput(bot, walk);
			field.world->tick(1 / 30.0f);
			if (followed) {
				follow.setSubject(bot);
				follow.tick(1 / 30.0f);
			}
		}
		PlayerCamera::View eye;
		CHECK(follow.view(eye, 4.0f / 3.0f) == (followed != 0));
		prints[followed] = field.world->stateFingerprint();
	}
	CHECK(prints[0] == prints[1]);
}

// A bot in a vehicle seat: the seat's own camera (its inside view at the
// camera's insideFOV) and the vehicle's cockpit, as the local player's in
// that seat; the HUD's vehicle his.
TEST(FollowFirstPerson_BotInASeatGetsTheSeatCamera)
{
	Field field;
	field.collisionTemplates.add("jeep_mesh", boxMesh(1));
	field.templates.registerType(20, "PlayerControlObject");
	field.templates.registerType(23, "Engine");
	field.templates.registerType(24, "Camera");
	field.templates.registerType(99, "EntryPoint");
	field.templates.create("Engine", "jeep_engine")->settings()["engineType"] = "c_ETCar";
	field.templates.create("EntryPoint", "jeep_entry")->settings()["entryRadius"] = "4";
	field.templates.create("Camera", "jeep_cam")->settings()["insideFOV"] = "1.2";
	world::ObjectTemplate* jeepTemplate = field.templates.create("PlayerControlObject", "jeep");
	jeepTemplate->settings()["collisionMesh"] = "jeep_mesh";
	jeepTemplate->settings()["hasMobilePhysics"] = "1";
	jeepTemplate->settings()["physicsType"] = "Mesh";
	jeepTemplate->children().push_back({"jeep_entry", {}, {}});
	jeepTemplate->children().push_back({"jeep_engine", {}, {}});
	jeepTemplate->children().push_back({"jeep_cam", {0, 1.5f, 0.5f}, {}});
	const uint32_t jeep = field.objects.create(jeepTemplate, {0, 1, 3})->id();
	field.start();
	const int bot = field.world->addPlayer("Bot", 2, true);
	CHECK(field.world->spawnSoldier(bot));
	field.world->tick(1 / 30.0f); // the jeep's body
	game::SoldierInput use;
	use.use = true;
	field.world->setInput(bot, use);
	field.world->tick(1 / 30.0f);
	field.world->setInput(bot, {});
	field.world->tick(1 / 30.0f);
	game::GameWorld::PlayerInfo info;
	CHECK(field.world->playerInfo(bot, info) && info.vehicle == jeep && info.seat == 0);
	anim::AnimationSystems systems(nullptr);
	PlayerCamera follow(*field.world, systems, nullptr);
	follow.setSubject(bot);
	follow.tick(1 / 30.0f);
	PlayerCamera::View eye;
	CHECK(follow.insideView() && follow.mode() == game::camera::Mode::Inside && follow.view(eye, 4.0f / 3.0f));
	const world::Object* car = field.objects.find(jeep);
	const render::Mat4 place = render::Mat4::yawPitchRoll(car->rotation().x, car->rotation().y, car->rotation().z)
		* render::Mat4::translation({car->position().x, car->position().y, car->position().z});
	const float local[3] = {0, 1.5f, 0.5f};
	float at[3];
	for (int c = 0; c < 3; ++c) {
		at[c] = local[0] * place.at(0, c) + local[1] * place.at(1, c) + local[2] * place.at(2, c) + place.at(3, c);
	}
	CHECK(std::fabs(eye.position[0] - at[0]) < 1e-3f && std::fabs(eye.position[1] - at[1]) < 1e-3f
		&& std::fabs(eye.position[2] - at[2]) < 1e-3f);
	CHECK(std::fabs(eye.fovY - 1.2f) < 1e-4f && !eye.soldierEye);
	PlayerCamera::Cockpit cockpit;
	CHECK(follow.cockpit(cockpit) && cockpit.vehicle == jeep);
	CHECK(follow.hides(info.soldier));
	CHECK(follow.models().empty()); // his seat has a camera of its own: no arms
	host::LocalHudWorld hud(*field.world, field.logic);
	hud.viewedPlayer = bot;
	CHECK(hud.vehicle() && hud.vehicle()->id() == jeep && hud.soldier() && hud.soldier()->id() == info.soldier);
	// Back to the local player (who has no soldier): nothing.
	follow.setSubject(-1);
	follow.tick(1 / 30.0f);
	CHECK(!follow.view(eye, 4.0f / 3.0f) && follow.subject() == -1);
}

namespace {

host::HostSetup demoSetup(const net::ServerSettings* settings)
{
	host::HostSetup setup;
	setup.serverName = "Follow Demo";
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_sp";
	setup.mapSize = 16;
	setup.settings = settings;
	setup.modDirectory = "mods/bf2142";
	return setup;
}

} // namespace

// A demo of a local round played back, following a bot in his first person
// (--demo-follow, --demo-first-person): the ghosts give his soldier, his
// look, his weapon in hand (the kit's by the soldier's selected slot), his
// zoom level (the soldier networkable's, which the recording's server now
// sends: 0x005fc1a0 writes the firearm's zoom, slot 300) and the rounds in
// his magazine (the player networkable's fire state, while he fires); the
// follow camera shows his arms and rifle and his zoom's field of view, and
// the HUD his soldier, rifle and rounds, but not his magazines (not sent).
TEST(FollowFirstPerson_DemoPlaybackFollowsABot)
{
	Field field;
	FirstPersonData data(field);
	if (!data.load(field)) {
		return;
	}
	field.start();
	const PlayerCamera::Loader load = data.loader();
	field.world->setAnimationSystems(data.systems.get(), load);
	const int me = field.world->joinPlayer(1);
	field.world->setSpectator(me, true);
	const int bot = field.world->addPlayer("Sniper", 2, true);
	field.world->setPlayerName(bot, "Sniper");
	const std::string path = "bf2142_test_follow_demo.bf2142demo";
	std::remove(path.c_str());
	net::ServerSettings sv;
	host::HostedGame recorder(*field.world, field.logic, field.templates, demoSetup(&sv));
	recorder.prime();
	CHECK(recorder.recordDemo(path));
	for (int i = 0; i < 60; ++i) { // the recording under way, then he spawns (his HandlePickup)
		field.world->tick(1 / 30.0f);
		recorder.tick();
	}
	CHECK(field.world->spawnSoldier(bot));
	game::SoldierInput input;
	input.selectItem = data.rifleSlot();
	for (int i = 0; i < 150; ++i) {
		input.fire = i >= 60 && i < 64; // a few shots: the rounds go with the fire state
		field.world->setInput(bot, input);
		input.selectItem = 0;
		// He zooms from 90 on, as the bots' zoom tells the round (setZoomed).
		field.world->setZoomed(bot, i >= 90, 1, i >= 90 ? 1 : 0);
		field.world->tick(1 / 30.0f);
		recorder.tick();
	}
	game::GameWorld::PlayerInfo info;
	CHECK(field.world->playerInfo(bot, info) && info.alive && info.zoomLevel == 1);
	const int roundsLeft = info.roundsInMag;
	recorder.stopDemo();

	net::DemoHeader header;
	std::vector<net::DemoRecord> records;
	CHECK(net::readDemo(path, header, records));
	std::remove(path.c_str());
	net::GameClient viewer;
	viewer.world().setGhostTemplates([&field](uint32_t id) {
		const world::ObjectTemplate* t = field.templates.findById(id);
		return t ? net::ghostTemplateFrom(field.templates, *t) : net::GhostTemplateNode {};
	});
	net::DemoPlayer player(records);
	viewer.playDemo([&player](std::vector<uint8_t>& payload) { return player.next(payload); });
	for (int played = 0; viewer.update() && played < 3000; ++played) {
		player.advance(1);
	}
	CHECK(player.finished());
	const net::RemoteWorld& world = viewer.world();
	int sniperId = -1;
	for (const net::RemotePlayer& p : world.playerList()) {
		sniperId = p.name == "Sniper" ? p.id : sniperId;
	}
	const net::RemotePlayer* sniper = world.player(sniperId);
	CHECK(sniper && sniper->alive && sniper->soldierGhost && sniper->kitGhost);
	if (!sniper || !sniper->soldierGhost || !sniper->kitGhost) {
		return;
	}
	const net::RemoteObject* body = world.object(sniper->soldierGhost);
	CHECK(body && body->hasPosition && body->weaponZoom == 1 && body->weapon == data.rifleSlot());
	CHECK(sniper->knownRounds[0] >= 0 && sniper->knownRounds[0] == roundsLeft);
	// The playback's level: the ghosts as objects of a client round (main's
	// remote step makes them by their templates).
	world::ObjectManager clientObjects;
	game::GameWorld clientRound(field.templates, clientObjects);
	std::map<uint16_t, uint32_t> ghostObjects;
	for (const uint16_t ghost : {sniper->soldierGhost, sniper->kitGhost}) {
		const net::RemoteObject* o = world.object(ghost);
		world::ObjectTemplate* t = o ? field.templates.findById(o->templateId) : nullptr;
		CHECK(t != nullptr);
		if (t) {
			ghostObjects[ghost] = clientObjects.create(t, {o->position.x, o->position.y, o->position.z})->id();
		}
	}
	host::RemoteHudWorld hud(world, clientRound, ghostObjects);
	PlayerCamera::Remote remote;
	CHECK(hud.followed(sniper->id, remote));
	CHECK(remote.alive && remote.soldier == ghostObjects[sniper->soldierGhost] && remote.kit == ghostObjects[sniper->kitGhost]);
	CHECK(remote.weaponTemplate == data.rifle && remote.zoomLevel == 1 && remote.vehicle == 0);
	PlayerCamera follow(clientRound, *data.systems, load);
	follow.setRemote([&hud, &sniper](PlayerCamera::Remote& me) { return hud.followed(sniper->id, me); });
	follow.setSubject(sniper->id);
	for (int i = 0; i < 30; ++i) {
		follow.tick(1 / 30.0f);
	}
	PlayerCamera::View eye;
	CHECK(follow.view(eye, 4.0f / 3.0f));
	CHECK(std::fabs(eye.position[0] - body->position.x) < 0.3f && std::fabs(eye.position[2] - body->position.z) < 0.3f);
	CHECK(follow.zoomLevel() == 1 && follow.zoomed() && std::fabs(eye.fovY - data.factors[1]) < 1e-3f);
	const auto models = follow.models();
	CHECK(hasModel(models, data.soldier->geometry(), true));
	CHECK(hasModel(models, data.rifle->geometry(), false));
	CHECK(follow.hides(remote.soldier));
	// His HUD: his soldier, rifle and its rounds; his magazines unknown.
	CHECK(hud.soldier() == nullptr); // (the demo's own player has none)
	hud.viewedPlayer = sniper->id;
	CHECK(hud.localPlayer() == sniper->id && hud.soldier() && hud.soldier()->id() == remote.soldier);
	const host::HudWorld::Weapon w = hud.heldWeapon();
	CHECK(w.weapon == data.rifle && w.rounds == roundsLeft && w.mags == -1 && w.gun == nullptr);
	game::GameWorld::AbilityBarInfo bar;
	CHECK(!hud.abilityBar(bar)); // his battery and gifts aren't sent
}
