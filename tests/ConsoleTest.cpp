// Console script language: keywords, scopes, run/include, method calls.
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "IO/File/FileManager.h"
#include "TestMain.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using dice::hfe::Console;
using dice::hfe::ConsoleMethod;
using dice::hfe::ConsoleMethodKind;
using dice::hfe::ConsoleResult;

namespace {

// A directory of script files that is removed afterwards.
class ScriptDir {
public:
	ScriptDir() : m_path(fs::temp_directory_path() / ("bf2142_console_" + std::to_string(std::rand())))
	{
		fs::create_directories(m_path);
		m_files.setRootPath(m_path.string());
		m_files.addSearchPath(m_path.string());
	}
	~ScriptDir() { fs::remove_all(m_path); }

	void write(const std::string& name, const std::string& text)
	{
		const fs::path file = m_path / name;
		fs::create_directories(file.parent_path());
		std::ofstream(file, std::ios::binary) << text;
	}
	dice::hfe::io::FileManager& files() { return m_files; }

private:
	fs::path m_path;
	dice::hfe::io::FileManager m_files;
};

// Records calls to "test.log" and keeps a read/write property "test.value".
struct Recorder {
	std::vector<std::string> calls;
	std::string value = "7";

	void install(Console& console)
	{
		ConsoleMethod log;
		log.name = "test.log";
		log.maxArgs = 4;
		log.invoke = [this](const std::vector<std::string>& args) {
			std::string line;
			for (const std::string& arg : args) {
				line += (line.empty() ? "" : " ") + arg;
			}
			calls.push_back(line);
			return std::string();
		};
		console.registerMethod(log);

		ConsoleMethod property;
		property.name = "test.value";
		property.kind = ConsoleMethodKind::ReadWriteProperty;
		property.maxArgs = 1;
		property.invoke = [this](const std::vector<std::string>& args) {
			if (!args.empty()) {
				value = args[0];
			}
			return value;
		};
		console.registerMethod(property);
	}
};

} // namespace

TEST(Console_ConditionOperators)
{
	bool result = false;
	std::string error;
	CHECK(dice::hfe::evaluateConsoleCondition("1", "", "", result, error) && result);
	CHECK(dice::hfe::evaluateConsoleCondition("abc", "", "", result, error) && !result);
	CHECK(dice::hfe::evaluateConsoleCondition("not", "0", "", result, error) && result);
	CHECK(dice::hfe::evaluateConsoleCondition("a", "==", "a", result, error) && result);
	CHECK(dice::hfe::evaluateConsoleCondition("1.0", "==", "1", result, error) && !result); // text comparison
	CHECK(dice::hfe::evaluateConsoleCondition("1.5", "greaterThan", "1", result, error) && result);
	CHECK(dice::hfe::evaluateConsoleCondition("2", "<=", "2", result, error) && result);
	CHECK(dice::hfe::evaluateConsoleCondition("1", "and", "0", result, error) && !result);
	CHECK(dice::hfe::evaluateConsoleCondition("0", "||", "1", result, error) && result);
	CHECK(dice::hfe::evaluateConsoleCondition("", "==", "", result, error) && result);
	CHECK(!dice::hfe::evaluateConsoleCondition("", "<", "1", result, error));
	CHECK(!dice::hfe::evaluateConsoleCondition("1", "~", "1", result, error));
	CHECK(error == "Error: Unknown operand in expression \"~\"!");
}

TEST(Console_LinesAndMethods)
{
	Console console;
	Recorder recorder;
	recorder.install(console);
	std::string out;

	CHECK(console.execute("Test.Log a \"b c\" d", out) == ConsoleResult::Ok);
	CHECK(recorder.calls.size() == 1 && recorder.calls[0] == "a b c d");
	CHECK(console.execute("test.value", out) == ConsoleResult::Ok && out == "7");
	CHECK(console.execute("test.value 9", out) == ConsoleResult::Ok && recorder.value == "9");
	CHECK(console.execute("test.getValue", out) == ConsoleResult::Ok && out == "9");
	CHECK(console.execute("test.setValue 3", out) == ConsoleResult::Ok && recorder.value == "3");
	CHECK(console.execute("test.value 1 2", out) == ConsoleResult::Error && out == "Properties only takes one argument!");
	CHECK(console.execute("test.log 1 2 3 4 5", out) == ConsoleResult::Error);
	CHECK(out == "Too many arguments, the max no of arguments is 4!");
	CHECK(console.execute("test.nothing", out) == ConsoleResult::Error && out == "Unknown object or method!");
	CHECK(console.execute("nodot", out) == ConsoleResult::Error && out == "Wrong syntax!");
	CHECK(console.execute("test.log \"open", out) == ConsoleResult::Error && out == "no closing '\"'!");
	CHECK(console.execute("rem test.log x", out) == ConsoleResult::Skipped);
	CHECK(console.execute("echo a b", out) == ConsoleResult::Echo && out == "ab");
	CHECK(console.execute("if 1", out) == ConsoleResult::Error); // not at the top level

	// Variables at the console, "->" capture.
	CHECK(console.execute("var v_x = 5", out) == ConsoleResult::Skipped);
	CHECK(console.execute("var v_x", out) == ConsoleResult::Error && out == "Variable redefinition \"v_x\"!");
	CHECK(console.execute("v_x", out) == ConsoleResult::Ok && out == "5");
	CHECK(console.execute("test.value -> v_x", out) == ConsoleResult::Ok);
	CHECK(console.execute("V_X", out) == ConsoleResult::Ok && out == "3");
	CHECK(console.execute("test.log v_missing", out) == ConsoleResult::Error && out == "Unknown variable v_missing!");
	CHECK(console.execute("const c_k = v_x", out) == ConsoleResult::Skipped);
	CHECK(console.execute("c_k", out) == ConsoleResult::Ok && out == "3");
}

TEST(Console_ScriptControlFlow)
{
	ScriptDir dir;
	Console console(&dir.files());
	Recorder recorder;
	recorder.install(console);

	dir.write("Main.con",
		"rem control flow\r\n"
		"var v_n = 2\n"
		"if v_n == 2\n"
		"  test.log two\n"
		"  var v_inner = x\n"
		"elseIf v_n == 3\n"
		"  test.log three\n"
		"else\n"
		"  test.log other\n"
		"endIf\n"
		"if v_n lessThan 1\n"
		"  test.log never\n"
		"  if 1\n"
		"    test.log never2\n"
		"  endIf\n"
		"elseIf v_n greaterThan 1\n"
		"  test.log greater\n"
		"endIf\n"
		"test.log v_inner\n" // scoped to the if: fails
		"beginRem\n"
		"test.log commented\n"
		"endRem\n"
		"beginNoExecution\n"
		"test.log notExecuted\n"
		"endNoExecution\n"
		"var v_r\n"
		"run sub/Child.con v_n hello -> v_r\n"
		"test.log result v_r\n"
		"include Shared\n"
		"include missing.tweak\n"
		"test.log after\n"
		"if 1\n"
		"  return\n"
		"endIf\n"
		"test.log afterReturn\n");
	dir.write("sub/Child.con",
		"test.log child v_noArg v_arg1 v_arg2\n"
		"include Grand\n" // relative to sub/
		"v_result = done\n");
	dir.write("sub/Grand.inc", "test.log grand v_arg2\n");
	dir.write("Shared.inc", "test.log shared v_arg1\n");

	std::string out;
	CHECK(console.run("Main", {"top"}, out));
	const std::vector<std::string> expected = {
		"two", "greater", "child 2 2 hello", "grand hello", "result done", "shared top", "after",
	};
	CHECK(recorder.calls == expected);

	CHECK(!console.run("NoSuchFile", {}, out));
	CHECK(out == "Could not open file: NoSuchFile.con!");
	// The run's scopes are gone again.
	std::string value;
	CHECK(!console.getVariable("v_n", value));
}

TEST(Console_RunsGameArchiveScript)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	dice::hfe::io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	Console console(&files);

	ConsoleMethod mount;
	mount.name = "fileManager.mountArchive";
	mount.minArgs = 2;
	mount.maxArgs = 3;
	mount.invoke = [&files](const std::vector<std::string>& args) {
		files.mountArchive(args[0], args[1]);
		return std::string();
	};
	console.registerMethod(mount);

	std::string out;
	CHECK(console.run("ClientArchives.con", {}, out));
	CHECK(files.mountedArchiveCount() == 27);
}

TEST(Console_TypedBindings)
{
	using namespace dice::hfe;
	Console console;
	float volume = 0.5f;
	ConsoleVec3 position;
	std::string lastName;
	int lastCount = 0;
	bindProperty<float>(console, "audio.volume", ConsoleAccess::User, [&] { return volume; },
		[&](const float& v) { volume = v; });
	bindProperty<ConsoleVec3>(console, "object.position", ConsoleAccess::User, [&] { return position; },
		[&](const ConsoleVec3& v) { position = v; });
	bindProperty<bool>(console, "game.ready", ConsoleAccess::User, [] { return true; }, nullptr);
	bindMethod(console, "object.create", ConsoleAccess::User, [&](std::string name, int count) {
		lastName = name;
		lastCount = count;
		return count * 2;
	}, 1);

	std::string out;
	CHECK(console.execute("audio.volume", out) == ConsoleResult::Ok && out == "0.5");
	CHECK(console.execute("audio.volume 0.25", out) == ConsoleResult::Ok && volume == 0.25f);
	CHECK(console.execute("object.position 1/2.5/-3", out) == ConsoleResult::Ok);
	CHECK(position.x == 1 && position.y == 2.5f && position.z == -3);
	CHECK(console.execute("object.getPosition", out) == ConsoleResult::Ok && out == "1/2.5/-3");
	CHECK(console.execute("game.ready", out) == ConsoleResult::Ok && out == "1");
	CHECK(console.execute("game.ready 0", out) == ConsoleResult::Error && out == "Property is only readable!");
	CHECK(console.execute("object.create \"Kit A\" 3", out) == ConsoleResult::Ok && out == "6");
	CHECK(lastName == "Kit A" && lastCount == 3);
	CHECK(console.execute("object.create Tank", out) == ConsoleResult::Ok && out == "0");
	CHECK(console.execute("object.create", out) == ConsoleResult::Error);
}

TEST(Console_SameNameMethodsResolveToActiveOne)
{
	using namespace dice::hfe;
	Console console;
	std::string activeClass = "Kit";
	for (const std::string& owner : {"Kit", "Vehicle"}) {
		ConsoleMethod method;
		method.name = "ObjectTemplate.geometry";
		method.maxArgs = 1;
		method.isActive = [&activeClass, owner] { return activeClass == owner; };
		method.invoke = [owner](const std::vector<std::string>&) { return owner; };
		console.registerMethod(method);
	}
	std::string out;
	CHECK(console.execute("ObjectTemplate.geometry", out) == ConsoleResult::Ok && out == "Kit");
	activeClass = "Vehicle";
	CHECK(console.execute("ObjectTemplate.geometry", out) == ConsoleResult::Ok && out == "Vehicle");
	activeClass = "None";
	CHECK(console.execute("ObjectTemplate.geometry", out) == ConsoleResult::Error && out == "Unknown object or method!");
}

// A stand-in (the API table's stubs) gives way to the real method
// registered later under the same name.
TEST(Console_RealMethodReplacesStub)
{
	Console console;
	int stubCalls = 0, realCalls = 0;
	dice::hfe::ConsoleMethod stub;
	stub.name = "Commander.commanderApply";
	stub.access = dice::hfe::ConsoleAccess::Anyone;
	stub.stub = true;
	stub.invoke = [&stubCalls](const std::vector<std::string>&) {
		++stubCalls;
		return std::string();
	};
	console.registerMethod(stub);
	dice::hfe::ConsoleMethod real = stub;
	real.stub = false;
	real.invoke = [&realCalls](const std::vector<std::string>&) {
		++realCalls;
		return std::string();
	};
	console.registerMethod(real);
	std::string out;
	console.execute("Commander.commanderApply", out, dice::hfe::ConsoleAccess::Anyone);
	CHECK(realCalls == 1 && stubCalls == 0);
	// A stub registered after a real method does not replace it either.
	console.registerMethod(stub);
	console.execute("Commander.commanderApply", out, dice::hfe::ConsoleAccess::Anyone);
	CHECK(realCalls == 2 && stubCalls == 0);
}
