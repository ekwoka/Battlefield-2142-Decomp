// The virtual file system ("dice.hfe.io.FileManager.Standard").
//
// Game paths are relative to the mod (e.g. "Objects/Vehicles/Land/..."), and
// are case-insensitive with either slash. A path resolves as follows:
//   1. archives mounted at the longest matching mount point, in mount order
//      (the first archive that has the file wins, so a booster pack mounted
//      after the base game only adds files);
//   2. loose files under each search path, in the order they were added
//      (normally just "mods/<mod>/").
// "@ROOT@/..." names the installation directory and "@HOME@/..." the user's
// profile directory; both bypass archives.
//
// Archives can only be mounted while the game initialises; afterwards
// mountArchive fails with "not supported in runtime", as in the original.
#pragma once

#include "ZipArchive.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dice::hfe::io {

class FileManager {
public:
	FileManager();
	~FileManager();

	void setRootPath(std::string path);
	void setHomePath(std::string path);
	const std::string& rootPath() const { return m_root; }
	const std::string& homePath() const { return m_home; }

	// Directories (relative to the root, or absolute) searched for loose files
	// and archives, highest priority first.
	void addSearchPath(const std::string& path);

	void setMountingAllowed(bool allowed) { m_mountingAllowed = allowed; }
	bool mountArchive(const std::string& archive, const std::string& mountPoint);
	size_t mountedArchiveCount() const;

	bool exists(const std::string& path) const;
	bool readFile(const std::string& path, std::vector<uint8_t>& out) const;
	bool readText(const std::string& path, std::string& out) const;

	// Expands @ROOT@/@HOME@ and search paths into a real path for writing.
	std::string expandPath(const std::string& path) const;

	// File names (lower case, without directory) in a virtual directory.
	std::vector<std::string> listFiles(const std::string& directory) const;
	// Paths (lower case, relative to `directory`) of all files below it.
	std::vector<std::string> listFilesRecursive(const std::string& directory) const;

private:
	struct MountPoint {
		std::vector<std::unique_ptr<ZipArchive>> archives; // mount order
	};

	const MountPoint* findMount(const std::string& normalized, std::string& relative) const;
	std::string resolveLooseFile(const std::string& normalized) const;

	std::string m_root;
	std::string m_home;
	std::vector<std::string> m_searchPaths; // absolute, with trailing '/'
	std::map<std::string, MountPoint> m_mounts; // normalised mount point -> archives
	std::set<std::string> m_mountedFiles;
	bool m_mountingAllowed = true;
};

// Finds `relative` under `base` ignoring case, for case-sensitive file
// systems. Returns the real path or an empty string.
std::string findFileIgnoringCase(const std::string& base, const std::string& relative);

} // namespace dice::hfe::io
