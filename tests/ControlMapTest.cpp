// The control maps (Input/ControlMap.h): both script formats, the common
// maps behind a player map, rebinding, and (with $BF2142_GAME_DIR) the
// shipped Settings/Controls.con.
#include "Console/Console.h"
#include "IO/File/FileManager.h"
#include "Input/ControlMap.h"
#include "TestMain.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

using namespace dice::hfe;

namespace {

input::ControlMaps::IsDown pressedKeys(std::set<std::string> keys)
{
	return [keys](const std::string& control) { return keys.count(control) != 0; };
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

} // namespace

TEST(ControlMap_Formats)
{
	Console console;
	input::ControlMaps maps;
	input::registerControlMapConsoleMethods(console, maps);
	std::string out;
	for (const char* line : {
			 // BF2's profile format
			 "ControlMap.create defaultPlayerInputControlMap",
			 "ControlMap.addKeyToTriggerMapping c_PIUse IDFKeyboard IDKey_E c_CMPushAndHold",
			 "ControlMap.addAxisToTriggerMapping c_PINextItem c_PIPrevItem IDFMouse IDAxis_2",
			 // 2142's: repeat delay and slot
			 "ControlMap.create InfantryPlayerInputControlMap",
			 "ControlMap.addKeysToAxisMapping c_PIThrottle IDFKeyboard IDKey_W IDKey_S 0",
			 "ControlMap.addButtonToTriggerMapping c_PIFire IDFMouse IDButton_0 0 0",
			 "ControlMap.addKeyToTriggerMapping c_PIReload IDFKeyboard IDKey_R 10000 0",
			 "ControlMap.addKeyToTriggerMapping c_PILie IDFKeyboard IDKey_Z 0 0",
			 "ControlMap.addButtonToTriggerMapping c_PILie IDFMouse IDButton_3 0 1",
			 "ControlMap.create defaultGameControlMap",
			 "ControlMap.addKeyToTriggerMapping c_GILeader IDFKeyboard IDKey_Capital 10000 0",
		 }) {
		const bool ok = console.execute(line, out, ConsoleAccess::Anyone) != ConsoleResult::Error;
		if (!ok) {
			std::fprintf(stderr, "  %s: %s\n", line, out.c_str());
		}
		CHECK(ok);
	}
	const std::string infantry = input::ControlMaps::kInfantry;
	CHECK(maps.value(infantry, "c_PIThrottle", pressedKeys({"IDKey_W"})) == 1);
	CHECK(maps.value(infantry, "c_PIThrottle", pressedKeys({"IDKey_S"})) == -1);
	// Both keys: the first one wins (0x00680990 tests it first).
	CHECK(maps.value(infantry, "c_PIThrottle", pressedKeys({"IDKey_W", "IDKey_S"})) == 1);
	CHECK(maps.pressed(infantry, "c_PIFire", pressedKeys({"IDButton_0"})));
	// From the common maps.
	CHECK(maps.pressed(infantry, "c_PIUse", pressedKeys({"IDKey_E"})));
	CHECK(maps.pressed(infantry, "c_GILeader", pressedKeys({"IDKey_Capital"})));
	CHECK(!maps.pressed(infantry, "c_PIUse", pressedKeys({"IDKey_Q"})));
	// Hold and once-per-press, slots.
	const auto& lie = maps.find(infantry)->at("c_PILie");
	CHECK(lie.size() == 2 && lie[0].hold && lie[1].slot == 1);
	CHECK(!maps.find(infantry)->at("c_PIReload")[0].hold);
	CHECK(maps.firstControl(infantry, "c_PILie") == "IDKey_Z");
	// Rebinding the primary keeps the secondary.
	maps.rebind(infantry, "c_PILie", "IDKey_X");
	CHECK(maps.firstControl(infantry, "c_PILie") == "IDKey_X");
	CHECK(maps.pressed(infantry, "c_PILie", pressedKeys({"IDButton_3"})));
	CHECK(!maps.pressed(infantry, "c_PILie", pressedKeys({"IDKey_Z"})));
}

// The keys' ramp on an axis (0x00680990), the primary-or-secondary rule
// (0x00680dc0) and a POV hat on an axis pair (0x006807f0).
TEST(ControlMap_RampAndPov)
{
	Console console;
	input::ControlMaps maps;
	input::registerControlMapConsoleMethods(console, maps);
	std::string out;
	for (const char* line : {
			 "ControlMap.create AirPlayerInputControlMap",
			 "ControlMap.addKeysToAxisMapping c_PIRoll IDFKeyboard IDKey_D IDKey_A 0",
			 "ControlMap.addAxisToAxisMapping c_PIRoll IDFGameController_0 IDAxis_0 0 1",
		 }) {
		CHECK(console.execute(line, out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	}
	const std::string air = input::ControlMaps::kAir;
	std::set<std::string> keys;
	float stick = 0;
	int hat = -1;
	input::ControlMaps::Source source;
	source.down = [&keys](const input::Binding& b) { return keys.count(b.control) != 0; };
	source.axis = [&stick](const input::Binding&) { return stick; };
	source.pov = [&hat](const input::Binding&) { return hat; };
	// With the default 0.001 s a frame's time takes a key to its end.
	source.seconds = 1.0f / 60.0f;
	keys = {"IDKey_D"};
	CHECK(maps.value(air, "c_PIRoll", source) == 1);
	keys.clear();
	CHECK(maps.value(air, "c_PIRoll", source) == 0);
	// A slower ramp: 0.5 s up, 0.25 s back.
	maps.settings(air).rampUp = 0.5f;
	maps.settings(air).rampDown = 0.25f;
	source.seconds = 0.1f;
	keys = {"IDKey_A"};
	CHECK(near(maps.value(air, "c_PIRoll", source), -0.2f));
	CHECK(near(maps.value(air, "c_PIRoll", source), -0.4f));
	keys.clear();
	CHECK(near(maps.value(air, "c_PIRoll", source), 0.0f));
	// The primary at rest: the secondary (the stick) counts; the keys win
	// once they move the value.
	maps.settings(air).rampUp = maps.settings(air).rampDown = 0.001f;
	stick = 0.7f;
	CHECK(near(maps.value(air, "c_PIRoll", source), 0.7f));
	keys = {"IDKey_A"};
	CHECK(maps.value(air, "c_PIRoll", source) == -1);
	keys.clear();
	stick = 0;
	// The hat on pitch and roll: right is roll +1, up is pitch -1, the
	// diagonal both.
	maps.addPovToAxisMapping(air, "c_PIRoll", "c_PIPitch", "IDFGameController_0", 0, false, 1);
	hat = 9000;
	CHECK(maps.value(air, "c_PIRoll", source) == 1);
	CHECK(maps.value(air, "c_PIPitch", source) == 0);
	hat = 0;
	CHECK(maps.value(air, "c_PIPitch", source) == -1);
	hat = 22500; // down-left
	CHECK(maps.value(air, "c_PIRoll", source) == -1);
	CHECK(maps.value(air, "c_PIPitch", source) == 1);
	hat = -1;
	CHECK(maps.value(air, "c_PIRoll", source) == 0);
	// Not written back (the profile writer has no POV mapping).
	CHECK(maps.script().find("IDPov_") == std::string::npos);
}

TEST(ControlMap_Shipped)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	Console console(&files);
	input::ControlMaps maps;
	input::registerControlMapConsoleMethods(console, maps);
	std::string out;
	CHECK(console.run("Settings/Controls.con", {}, out));
	for (const char* map : {input::ControlMaps::kInfantry, input::ControlMaps::kLand, input::ControlMaps::kAir,
			 input::ControlMaps::kHelicopter, input::ControlMaps::kSea, input::ControlMaps::kGame,
			 input::ControlMaps::kPlayer}) {
		CHECK(maps.find(map) != nullptr);
	}
	const std::string infantry = input::ControlMaps::kInfantry;
	CHECK(maps.firstControl(infantry, "c_PIFire") == "IDButton_0");
	CHECK(maps.firstControl(infantry, "c_PISprint") == "IDKey_LeftShift");
	CHECK(maps.firstControl(infantry, "c_PIUse") == "IDKey_E");
	CHECK(maps.firstControl(infantry, "c_GILeader") == "IDKey_Capital");
	CHECK(maps.value(infantry, "c_PIYaw", pressedKeys({"IDKey_D"})) == 1);
}

// The maps written out (ControlMaps::script) build the same maps again.
TEST(ControlMap_ScriptRoundTrip)
{
	Console console;
	input::ControlMaps maps;
	input::registerControlMapConsoleMethods(console, maps);
	std::string out;
	for (const char* line : {"ControlMap.create InfantryPlayerInputControlMap",
			 "ControlMap.addKeysToAxisMapping c_PIThrottle IDFKeyboard IDKey_W IDKey_S 0",
			 "ControlMap.addButtonToTriggerMapping c_PIFire IDFMouse IDButton_0 0 0",
			 "ControlMap.addKeyToTriggerMapping c_PIReload IDFKeyboard IDKey_R 10000 0",
			 "ControlMap.addAxisToTriggerMapping c_PINextItem -1 IDFMouse IDAxis_2 0",
			 "ControlMap.addAxisToAxisMapping c_PIMouseLookX IDFMouse IDAxis_0 0 0"}) {
		console.execute(line, out, ConsoleAccess::Anyone);
	}
	maps.bind(input::ControlMaps::kInfantry, "c_PIFire", "IDKey_Q", 1, 1);
	const std::string script = maps.script();
	Console again;
	input::ControlMaps copy;
	input::registerControlMapConsoleMethods(again, copy);
	size_t start = 0;
	while (start < script.size()) {
		const size_t end = script.find('\n', start);
		again.execute(script.substr(start, end - start), out, ConsoleAccess::Anyone);
		start = end + 1;
	}
	CHECK(copy.script() == script);
	const std::string infantry = input::ControlMaps::kInfantry;
	CHECK(copy.value(infantry, "c_PIThrottle", [](const std::string& c) { return c == "IDKey_S"; }) == -1);
	CHECK(copy.pressed(infantry, "c_PIFire", [](const std::string& c) { return c == "IDKey_Q"; }));
	CHECK(!copy.find(infantry)->at("c_PIReload")[0].hold);
}
