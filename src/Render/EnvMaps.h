// The level's environment maps (GPU-free, for the tests).
//
// RendDX9's EnvMapManager (DAT_101ff9cc, vtable 0x101c6378) reads
// <level>/Envmaps/EnvironmentMapInfo.emi at level load (0x100639b0 ->
// slot 0x34, loadEMIFile 0x10064770): '#' comments, then one line per map,
// "EnvMapN x,y,z,bool", the cube <level>/Envmaps/EnvMapN.dds at that place.
// Every retail level ships one (in its server archive; the cubes in the
// client's). Each frame 0x10064f30 makes the one nearest the camera the
// current cube (+100; with a single map, that one), which every static
// mesh's material takes (0x100bdb00, the drawable's +0x13c) and bundled
// materials with an "envmap" technique (0x100c5030, +0x120).
#pragma once

#include <array>
#include <cctype>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace dice::hfe::render {

struct EnvMapEntry {
	std::string name; // "EnvMap0": the cube is Envmaps/<name>.dds
	std::array<float, 3> position {};
	bool flag = false; // the line's fourth value (the editor's)
};

inline std::vector<EnvMapEntry> parseEnvMapInfo(const std::string& text)
{
	std::vector<EnvMapEntry> entries;
	std::istringstream lines(text);
	std::string line;
	while (std::getline(lines, line)) {
		size_t start = line.find_first_not_of(" \t\r");
		if (start == std::string::npos || line[start] == '#') {
			continue;
		}
		size_t end = start;
		while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) {
			++end;
		}
		EnvMapEntry entry;
		entry.name = line.substr(start, end - start);
		int flag = 0;
		if (std::sscanf(line.c_str() + end, " %f,%f,%f,%d", &entry.position[0], &entry.position[1], &entry.position[2],
				&flag) < 3) {
			continue;
		}
		entry.flag = flag != 0;
		entries.push_back(entry);
	}
	return entries;
}

// 0x10064f30: the entry nearest the camera (-1 with none).
inline int nearestEnvMap(const std::vector<EnvMapEntry>& entries, const std::array<float, 3>& camera)
{
	int best = -1;
	float bestDistance = std::numeric_limits<float>::max();
	for (size_t i = 0; i < entries.size(); ++i) {
		const float dx = camera[0] - entries[i].position[0];
		const float dy = camera[1] - entries[i].position[1];
		const float dz = camera[2] - entries[i].position[2];
		const float d = dx * dx + dy * dy + dz * dz;
		if (d < bestDistance) {
			bestDistance = d;
			best = static_cast<int>(i);
		}
	}
	return best;
}

} // namespace dice::hfe::render
