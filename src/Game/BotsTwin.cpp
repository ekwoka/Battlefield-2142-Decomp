// enhanced.botTwinNav: the AI navigation data of a level's coop twin (see
// Bots.h aiTwinLevel). Not retail: AIDLL's QuadTreeManager (0x100ae4e0)
// reads <the level's path>/AIPathFinding/<map>.qtr and .clb only.
#include "Game/Bots.h"

#include "IO/File/ZipArchive.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <memory>
#include <set>
#include <system_error>
#include <vector>

namespace dice::hfe::game {

namespace {

std::string lower(std::string s)
{
	for (char& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

bool hasNavigation(const io::ZipArchive& zip)
{
	return zip.contains("AIPathFinding/Infantry.qtr") && zip.contains("AIPathFinding/Infantry.clb");
}

} // namespace

std::string aiTwinLevel(const std::string& levelsDir, const std::string& level)
{
	namespace fs = std::filesystem;
	std::error_code error;
	const std::string wanted = lower(level) + "_coop";
	std::string twin;
	std::string own;
	for (const auto& entry : fs::directory_iterator(fs::path(levelsDir), error)) {
		const std::string name = entry.path().filename().string();
		if (lower(name) == wanted) {
			twin = name;
		} else if (lower(name) == lower(level)) {
			own = name;
		}
	}
	if (twin.empty() || own.empty()) {
		return {};
	}
	const auto mine = io::ZipArchive::open((fs::path(levelsDir) / own / "server.zip").string());
	const auto theirs = io::ZipArchive::open((fs::path(levelsDir) / twin / "server.zip").string());
	if (!mine || !theirs || hasNavigation(*mine) || !hasNavigation(*theirs)) {
		return {};
	}
	// The same map: its terrain byte for byte, and its static objects.
	std::vector<uint8_t> a, b;
	if (!mine->read("HeightmapPrimary.raw", a) || !theirs->read("HeightmapPrimary.raw", b) || a != b) {
		return {};
	}
	if (!mine->read("StaticObjects.con", a) || !theirs->read("StaticObjects.con", b)) {
		return {};
	}
	return sameStatics(a, b) ? twin : std::string();
}

std::multiset<std::string> staticPlacements(const std::vector<uint8_t>& script)
{
	std::multiset<std::string> placed;
	std::string name, line;
	const auto flush = [&](const std::string& position) {
		if (!name.empty()) {
			float x = 0, y = 0, z = 0;
			if (std::sscanf(position.c_str(), "%f/%f/%f", &x, &y, &z) == 3) {
				char key[64];
				std::snprintf(key, sizeof(key), " %.0f/%.0f/%.0f", x * 10, y * 10, z * 10);
				placed.insert(name + key);
			}
		}
		name.clear();
	};
	for (size_t i = 0; i <= script.size(); ++i) {
		const char c = i < script.size() ? static_cast<char>(script[i]) : '\n';
		if (c != '\n' && c != '\r') {
			line += c;
			continue;
		}
		const size_t start = line.find_first_not_of(" \t");
		const std::string trimmed = start == std::string::npos ? std::string() : lower(line.substr(start));
		line.clear();
		if (trimmed.rfind("object.create ", 0) == 0) {
			name = trimmed.substr(14);
			name.erase(name.find_last_not_of(" \t") + 1);
		} else if (trimmed.rfind("object.absoluteposition ", 0) == 0) {
			flush(trimmed.substr(24));
		}
	}
	return placed;
}

bool sameStatics(const std::vector<uint8_t>& level, const std::vector<uint8_t>& twin)
{
	const std::multiset<std::string> a = staticPlacements(level), b = staticPlacements(twin);
	if (a.empty() || b.empty()) {
		return false;
	}
	std::vector<std::string> common;
	std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(common));
	return common.size() * 100 >= std::max(a.size(), b.size()) * 98;
}

} // namespace dice::hfe::game
