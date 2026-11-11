// The video options' display-mode list ("%ix%i@%iHz"), as RendDX9's
// 0x10010830 makes it from the adapter's modes.
//
// The original's list comes from three filters in turn:
//   - 0x100079b0 enumerates Direct3D's modes (adapter 0, R5G6B5 then
//     X8R8G8B8) and, with the request's +2 flag (set by 0x10010830), keeps
//     only those at least 640 x 480 at 60 Hz or more (0x27f < width,
//     0x1df < height, 0x3b < refresh);
//   - 0x10007b60 (the +1 flag, also set) lists the desktop's modes
//     (EnumDisplaySettingsA) of 16 or 32 bits, with no display flags and
//     the same bounds; without WideScreenEx (the command line's
//     "+widescreen 1", 0x00409950 case 0x13, which also sets GSWideScreen)
//     only 4:3 ones (|w / h - 1.3333334| <= 0.001), and never 848 x 480 or
//     1024 x 480. 0x100084a0 keeps a Direct3D mode only when that list has
//     one of the same size, refresh and depth;
//   - 0x10010830 itself lists, in that order, the modes of 32 bits (the
//     record's +6, ' ') at least 800 wide (799 < width) whose depth formats
//     (0x10007ce0: D32 / D24S8 as the device accepts them) include the one
//     in use. Each mode is one record, so each is listed once.
// The modes come in Direct3D's order, which is ascending: by width, then
// height, then refresh.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

namespace dice::hfe {

struct DisplayMode {
	int width = 0;
	int height = 0;
	int refresh = 0;      // Hz; 0 when the platform doesn't say (kept)
	int bitsPerPixel = 32; // 16 or 32 in the original's lists
};

inline std::vector<std::string> listDisplayModes(std::vector<DisplayMode> modes, bool wideScreen)
{
	// A display with no 4:3 modes (common on a Mac's built-in panel) would
	// leave the original's list empty; then the wider modes are offered as
	// with +widescreen 1 (port addition).
	if (!wideScreen) {
		std::vector<std::string> narrow = listDisplayModes(modes, true);
		narrow.erase(std::remove_if(narrow.begin(), narrow.end(), [](const std::string& text) {
			int w = 0, h = 0;
			return std::sscanf(text.c_str(), "%dx%d", &w, &h) != 2 ||
				std::fabs(static_cast<float>(w) / static_cast<float>(h) - 1.3333334f) > 0.001f;
		}), narrow.end());
		return narrow.empty() ? listDisplayModes(std::move(modes), true) : narrow;
	}
	std::stable_sort(modes.begin(), modes.end(), [](const DisplayMode& a, const DisplayMode& b) {
		return std::tie(a.width, a.height, a.refresh) < std::tie(b.width, b.height, b.refresh);
	});
	std::vector<std::string> list;
	for (const DisplayMode& m : modes) {
		// 0x100079b0 / 0x10007b60: at least 640 x 480 at 60 Hz. A refresh the
		// platform leaves unknown (0) isn't held against the mode.
		if (m.width < 640 || m.height < 480 || (m.refresh != 0 && m.refresh < 60)) {
			continue;
		}
		// 0x10007b60: 4:3 only without WideScreenEx (applied above); never
		// 848 or 1024 x 480.
		if (m.height == 480 && (m.width == 848 || m.width == 1024)) {
			continue;
		}
		// 0x10010830: 32 bits, at least 800 wide.
		if (m.bitsPerPixel != 32 || m.width < 800) {
			continue;
		}
		const std::string text =
			std::to_string(m.width) + "x" + std::to_string(m.height) + "@" + std::to_string(m.refresh) + "Hz";
		if (std::find(list.begin(), list.end(), text) == list.end()) {
			list.push_back(text);
		}
	}
	return list;
}

} // namespace dice::hfe
