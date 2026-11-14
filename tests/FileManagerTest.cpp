// Mounts the real game archives the way ClientArchives.con does and reads
// files through the virtual file system. Needs $BF2142_GAME_DIR (skipped otherwise).
#include "IO/File/FileManager.h"
#include "TestMain.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace io = dice::hfe::io;

namespace {

const char* gameDir() { return std::getenv("BF2142_GAME_DIR"); }

// Runs the fileManager.mountArchive lines of an archives .con file.
int mountFromCon(io::FileManager& files, const std::string& conText)
{
	int mounted = 0;
	std::istringstream lines(conText);
	std::string line;
	while (std::getline(lines, line)) {
		std::istringstream words(line);
		std::string command, archive, mountPoint;
		words >> command >> archive >> mountPoint;
		if (command == "fileManager.mountArchive" && files.mountArchive(archive, mountPoint)) {
			++mounted;
		}
	}
	return mounted;
}

} // namespace

TEST(FileManager_ModMountsInstallRelativeBaseArchive)
{
	if (!gameDir()) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir());
	files.addSearchPath("mods/a_separate_mod");
	CHECK(files.mountArchive("mods/bf2142/Sound_client.zip", "Sound"));
	CHECK(files.exists("Sound/Cockpits/Missile_Warning.wav"));
}

TEST(FileManager_MountsGameArchives)
{
	if (!gameDir()) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir());
	files.addSearchPath("mods/bf2142");

	std::string clientArchives;
	CHECK(files.readText("ClientArchives.con", clientArchives));
	const int mounted = mountFromCon(files, clientArchives);
	CHECK(mounted == 27);
	CHECK(files.mountedArchiveCount() == 27);

	// Mounting again is a no-op; mounting after initialisation is refused.
	CHECK(!files.mountArchive("Sound_client.zip", "Sound"));
	files.setMountingAllowed(false);
	CHECK(!files.mountArchive("Sound_server.zip", "Sound"));

	// Archive content, case-insensitive with either slash.
	std::vector<uint8_t> data;
	CHECK(files.readFile("Sound/Cockpits/Missile_Warning.wav", data));
	CHECK(data.size() > 44 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F');
	CHECK(files.exists("sound\\COCKPITS\\missile_warning.wav"));
	CHECK(!files.exists("Sound/Cockpits/does_not_exist.wav"));

	// Shaders ship as HLSL text in Shaders_client.zip.
	std::string shader;
	CHECK(files.readText("Shaders/BundledMesh.fx", shader));
	CHECK(shader.find("technique") != std::string::npos);

	// Loose files in the mod directory resolve through the search path.
	CHECK(files.exists("Init.con"));
	CHECK(!files.listFiles("Shaders").empty());
}

// The audits' three path cases: exists erases "bf2/content/" wherever it is
// (0x00698e40); expandPath leaves a path already under the first search
// path there (0x00697d70); readFile falls back to loose files (0x00698b60's
// archive-only mode is never set).
TEST(FileManager_PathEdgeCases)
{
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / ("bf2142_fm_" + std::to_string(std::rand()));
	fs::create_directories(root / "mods/bf2142/Settings");
	std::ofstream(root / "mods/bf2142/Settings/Test.con") << "x";
	io::FileManager files;
	files.setRootPath(root.string());
	files.addSearchPath("mods/bf2142");

	CHECK(files.exists("Settings/Test.con"));
	CHECK(files.exists("bf2/content/Settings/Test.con"));
	CHECK(files.exists("Settings/bf2/content/Test.con"));
	CHECK(!files.exists("Settings/bf2/content/Other.con"));

	const std::string base = files.rootPath();
	CHECK(files.expandPath("Settings/Test.con") == base + "mods/bf2142/Settings/Test.con");
	CHECK(files.expandPath("mods/bf2142/Settings/Test.con") == base + "mods/bf2142/Settings/Test.con");
	CHECK(files.expandPath("Mods\\BF2142/Settings/Test.con") == base + "Mods\\BF2142/Settings/Test.con");
	CHECK(files.expandPath("@ROOT@/x.con") == base + "x.con");

	std::vector<uint8_t> data;
	CHECK(files.readFile("Settings/Test.con", data) && data.size() == 1);
	std::error_code ec;
	fs::remove_all(root, ec);
}
