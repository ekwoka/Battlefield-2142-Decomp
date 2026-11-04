#include "HudCombatArea.h"

#include "Game/CombatAreas.h"
#include "Hud/Hud.h"
#include "IO/File/FileManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dice::hfe::menu {

namespace {

// 0x004d9d50 (the player's side; its controlled object having seats is
// the caller's: a soldier or a vehicle seat always has).
bool applies(const game::CombatArea& area, int team, int category)
{
	return (team == 0 || area.team == 0 || area.team == team) && area.binds(category);
}

// The area's outline (its points, or its box).
std::vector<std::pair<float, float>> outline(const game::CombatArea& area)
{
	if (area.polygon && area.points.size() >= 3) {
		return area.points;
	}
	return {{area.minX, area.minZ}, {area.maxX, area.minZ}, {area.maxX, area.maxZ}, {area.minX, area.maxZ}};
}

// Fills `value` (ORed) into the texels whose centres lie inside the
// outline (even-odd, a scanline a row).
void rasterise(const std::vector<std::pair<float, float>>& points, uint8_t value, float originX, float originZ, float width,
	float depth, int size, std::vector<uint8_t>& out)
{
	std::vector<float> crossings;
	for (int row = 0; row < size; ++row) {
		// Row 0 is the north edge (the map's v 0: max z).
		const float z = originZ + (1.0f - (row + 0.5f) / size) * depth;
		crossings.clear();
		for (size_t i = 0; i < points.size(); ++i) {
			const auto& [x0, z0] = points[i];
			const auto& [x1, z1] = points[(i + 1) % points.size()];
			if ((z0 <= z) == (z1 <= z)) {
				continue;
			}
			crossings.push_back(x0 + (z - z0) / (z1 - z0) * (x1 - x0));
		}
		std::sort(crossings.begin(), crossings.end());
		for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
			// Texel columns whose centres lie between the two crossings.
			const int first = std::max(0, static_cast<int>(std::ceil((crossings[i] - originX) / width * size - 0.5f)));
			const int last = std::min(size - 1, static_cast<int>(std::floor((crossings[i + 1] - originX) / width * size - 0.5f)));
			for (int column = first; column <= last; ++column) {
				out[static_cast<size_t>(row) * size + column] |= value;
			}
		}
	}
}

} // namespace

uint32_t combatAreaMask(const game::CombatAreas& areas, int team, int category)
{
	uint32_t mask = 0;
	for (size_t i = 0; i < areas.areas.size() && i < 32; ++i) {
		if (applies(areas.areas[i], team, category)) {
			mask |= 1u << i;
		}
	}
	return mask;
}

bool combatAreaBounds(const game::CombatAreas& areas, int team, int category, float& minX, float& minZ, float& maxX,
	float& maxZ)
{
	bool any = false;
	const auto add = [&](const game::CombatArea& area) {
		for (const auto& [x, z] : outline(area)) {
			minX = any ? std::min(minX, x) : x;
			maxX = any ? std::max(maxX, x) : x;
			minZ = any ? std::min(minZ, z) : z;
			maxZ = any ? std::max(maxZ, z) : z;
			any = true;
		}
	};
	// 0x0078b490(1): the applying areas that don't bind category 0; none,
	// every area that does.
	for (const game::CombatArea& area : areas.areas) {
		if (!area.binds(0) && applies(area, team, category)) {
			add(area);
		}
	}
	if (!any) {
		for (const game::CombatArea& area : areas.areas) {
			if (area.binds(0)) {
				add(area);
			}
		}
	}
	return any;
}

std::vector<uint8_t> combatAreaCoverage(const game::CombatAreas& areas, int team, int category, float originX,
	float originZ, float width, float depth, int size)
{
	std::vector<uint8_t> out(static_cast<size_t>(size) * size, 0);
	if (size <= 0 || width <= 0 || depth <= 0) {
		return out;
	}
	for (const game::CombatArea& area : areas.areas) {
		if (!area.binds(0) && applies(area, team, category)) {
			rasterise(outline(area), 0x80, originX, originZ, width, depth, size, out);
		}
	}
	for (const game::CombatArea& area : areas.areas) {
		if (area.binds(0)) {
			rasterise(outline(area), 0xff, originX, originZ, width, depth, size, out);
		}
	}
	return out;
}

std::vector<uint8_t> combatAreaTexture(const std::vector<uint8_t>& dds, const std::vector<uint8_t>& coverage, int size)
{
	// An uncompressed 32-bit DDS: "DDS ", the 124-byte header (height +12,
	// width +16, the pixel format's flags +80 / bits +88), texels from 128.
	const auto u32 = [&dds](size_t at) {
		uint32_t v = 0;
		std::memcpy(&v, dds.data() + at, 4);
		return v;
	};
	if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0 || u32(88) != 32 || size <= 0 ||
		coverage.size() < static_cast<size_t>(size) * size) {
		return {};
	}
	const uint32_t height = u32(12), width = u32(16);
	if (width == 0 || height == 0 || dds.size() < 128 + static_cast<size_t>(width) * height * 4) {
		return {};
	}
	std::vector<uint8_t> out = dds;
	for (uint32_t y = 0; y < height; ++y) {
		const size_t row = static_cast<size_t>(y) * size / height;
		for (uint32_t x = 0; x < width; ++x) {
			const size_t column = static_cast<size_t>(x) * size / width;
			uint8_t& alpha = out[128 + (static_cast<size_t>(y) * width + x) * 4 + 3];
			// 0x10187e70's 0x700: (255 - the areas') x the texture's own.
			alpha = static_cast<uint8_t>((255 - coverage[row * size + column]) * alpha / 256);
		}
	}
	// Only the top level: the mip levels (none in the shipped file) are dropped.
	if (u32(28) > 1) {
		out.resize(128 + static_cast<size_t>(width) * height * 4);
		const uint32_t one = 1;
		std::memcpy(out.data() + 28, &one, 4);
	}
	return out;
}

void CombatAreaLayer::setLevel(const std::string& level, float originX, float originZ, float width, float depth)
{
	m_level = level;
	m_origin[0] = originX;
	m_origin[1] = originZ;
	m_size[0] = width;
	m_size[1] = depth;
	m_built = false;
	m_mask = 0;
	m_team = m_category = -1;
}

void CombatAreaLayer::update(io::FileManager& files, const game::CombatAreas& areas, int team, int category,
	hud::HudManager& hud)
{
	if (!areas.use || areas.areas.empty() || m_level.empty()) {
		if (m_built) {
			hud.setMapLayer({});
			hud.clearMapArea();
			m_built = false;
		}
		return;
	}
	const uint32_t mask = combatAreaMask(areas, team, category);
	// DAT_00b3c4a0: rebuilt only when the applying areas change (from 0, so
	// a player no area applies to has no layer); the land areas don't
	// depend on the player, the count catches areas added later.
	if (mask == m_mask && areas.areas.size() == m_areaCount && (m_built || mask == 0)) {
		return;
	}
	m_mask = mask;
	m_areaCount = areas.areas.size();
	float minX = 0, minZ = 0, maxX = 0, maxZ = 0;
	if (combatAreaBounds(areas, team, category, minX, minZ, maxX, maxZ)) {
		hud.setMapArea(minX, minZ, maxX, maxZ);
	} else {
		hud.clearMapArea();
	}
	static const char* kShipped = "Menu/HUD/Texture/Ingame/Minimap/map_CombatArea32.dds";
	if (!m_shippedRead) {
		m_shippedRead = true;
		files.readFile(kShipped, m_shipped);
	}
	constexpr int kSize = 512; // the exe's texture's
	const std::vector<uint8_t> coverage =
		combatAreaCoverage(areas, team, category, m_origin[0], m_origin[1], m_size[0], m_size[1], kSize);
	std::vector<uint8_t> texture = combatAreaTexture(m_shipped, coverage, kSize);
	if (texture.empty()) {
		hud.setMapLayer({});
		m_built = true;
		return;
	}
	// A path per level and mask: the renderer keeps its textures by path.
	char name[96];
	std::snprintf(name, sizeof(name), "Menu/HUD/Texture/Ingame/Minimap/map_CombatArea32_%08x_%zu.dds", mask, areas.areas.size());
	std::string path = name;
	path.insert(path.size() - 4, "_" + m_level);
	files.addMemoryFile(path, std::move(texture));
	m_texture = path;
	hud.setMapLayer(path);
	m_built = true;
}

} // namespace dice::hfe::menu
