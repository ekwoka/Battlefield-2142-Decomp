// Mounts the real game archives the way ClientArchives.con does and reads
// files through the virtual file system. Needs $BF2142_GAME_DIR (skipped otherwise).
#include "IO/File/FileManager.h"
#include "TestMain.h"

#include <cstdlib>
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
