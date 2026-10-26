// A Titan-mode level for the tests (needs $BF2142_GAME_DIR): the shipped
// level's gpm_ti gameplay objects with every object template, the terrain,
// the collision meshes, the materials and the combat areas, as
// `bf2142 +level <name> +gameMode gpm_ti` loads them.
#pragma once

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/CombatAreas.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
#include "Terrain/Heightmap.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/TemplateFiles.h"
#include "World/WorldConsole.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

namespace titantest {

using namespace dice::hfe;

struct TitanLevel {
	io::FileManager files;
	Console console {&files};
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	terrain::HeightmapCluster heightmaps;
	physics::CollisionTemplateManager collisionTemplates;
	physics::MaterialManager materials;
	game::CombatAreas areas;
	std::unique_ptr<game::GameWorld> world;
	std::vector<std::pair<std::string, game::ScriptArgs>> events;
	std::function<void(const std::string&, const game::ScriptArgs&)> sink; // also hears the events (the scripts)
	std::function<void()> afterTick;
	bool loaded = false;

	TitanLevel(const char* gameDir, const std::string& name, int size, bool start = true)
	{
		files.setRootPath(gameDir);
		files.addSearchPath("mods/bf2142");
		files.setMountingAllowed(true);
		const std::string level = "Levels/" + name + "/";
		CHECK(files.mountArchive(level + "server.zip", level));
		std::string out;
		bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
			[this](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
		CHECK(console.run("ServerArchives.con", {}, out));
		if (files.exists(level + "ServerArchives.con")) {
			console.run(level + "ServerArchives.con", {}, out);
		}
		files.setMountingAllowed(false);
		world::registerWorldConsoleMethods(console, templates, objects);
		terrain::registerTerrainConsoleMethods(console, heightmaps, files);
		physics::registerCollisionConsoleMethods(console, collisionTemplates);
		physics::registerMaterialConsoleMethods(console, materials);
		game::registerCombatAreaConsoleMethods(console, areas);
		// The template properties not typed yet keep their values on the
		// template, as main's stubs do: every ObjectTemplate property and
		// one-argument setter of the console API table.
		std::vector<std::string> untyped;
		std::ifstream table(std::string(BF2142_SOURCE_DIR) + "/symbols/BF2142.exe.console.csv");
		std::string line;
		std::getline(table, line);
		while (std::getline(table, line)) {
			std::vector<std::string> cols;
			std::stringstream row(line);
			for (std::string col; std::getline(row, col, ',');) {
				cols.push_back(col);
			}
			if (cols.size() >= 6 && cols[0] == "ObjectTemplate" && !console.hasMethod("ObjectTemplate." + cols[1])
				&& (cols[2] == "property" || (cols[2] == "method" && cols[4] == "1" && cols[5] == "1"))) {
				untyped.push_back(cols[1]);
			}
		}
		CHECK(untyped.size() > 100);
		world::registerUntypedTemplateProperties(console, templates, untyped);
		templates.setLoading(true);
		world::loadObjectTemplates(console, files, "Objects/", false);
		templates.setLoading(false);
		console.run("Common/Material/MaterialManagerSettings.con", {}, out);
		console.run(level + "Heightdata.con", {}, out);
		console.run(level + "GameModes/gpm_ti/" + std::to_string(size) + "/GamePlayObjects.con", {"host"}, out);
		const terrain::Heightmap* ground = heightmaps.primary();
		CHECK(ground && ground->loaded());
		if (!ground || !ground->loaded()) {
			return;
		}
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->setGameMode("gpm_ti");
		world->setCombatAreas(&areas);
		world->setEventSink([this](const std::string& event, const game::ScriptArgs& args) {
			events.push_back({event, args});
			if (sink) {
				sink(event, args);
			}
		});
		physics::Ground surface;
		surface.height = [ground](float x, float z) { return ground->heightAt(x, z); };
		surface.heightAndNormal = [ground](float x, float z, physics::Vec3& normal) {
			normal = {0, 1, 0};
			return ground->heightAt(x, z);
		};
		world->enablePhysics(files, collisionTemplates, std::move(surface), &materials);
		loaded = true;
		if (start) {
			world->startRound();
			run(0.1f);
		}
	}

	void run(float seconds)
	{
		for (int tick = 0; tick < static_cast<int>(seconds * 30 + 0.5f); ++tick) {
			world->tick(1 / 30.0f);
			if (afterTick) {
				afterTick();
			}
		}
	}
	const game::GameWorld::TitanInfo* titan(int team)
	{
		static game::GameWorld::TitanInfo info;
		for (const auto& t : world->titans()) {
			if (t.team == team) {
				info = t;
				return &info;
			}
		}
		return nullptr;
	}
	// VehicleDestroyed events of an object.
	size_t destroyed(uint32_t object) const
	{
		return static_cast<size_t>(std::count_if(events.begin(), events.end(), [&](const auto& e) {
			return e.first == "VehicleDestroyed" && !e.second.empty() && static_cast<uint32_t>(e.second[0].i) == object;
		}));
	}
	size_t count(const std::string& event) const
	{
		return static_cast<size_t>(std::count_if(events.begin(), events.end(), [&](const auto& e) { return e.first == event; }));
	}
};


} // namespace titantest
