// The in-game HUD (BF2142.exe's HudManager and HudBuilder): a tree of nodes
// built by Menu/HUD/HudSetup/HudSetupMain.con and the scripts it runs,
//   hudBuilder.createPictureNode IngameHud ReferenceCross 0 0 800 600
//   hudBuilder.setPictureNodeTexture Ingame/Crosshair/ReferenceCross.tga
//   hudBuilder.setNodeShowVariable ReferenceCrossShow
//   hudBuilder.setNodeLogicShowVariable NOT IsSpectator 1
// in an 800 x 600 screen, below six roots the engine creates (Global,
// TopLayer and the bottom-left and bottom-right static and animated
// corners). The game drives it through named variables: numbers that show
// nodes (non-zero shows; then each logic entry ANDs "variable == value",
// ORs it, or ANDs "variable != value" for NOT), fill bars (0..1), set
// alpha and colour, and strings for text and texture paths. Each frame
// draws twice, as RendDX9.dll does: AffectedByEMPShow 0, then 1 (the
// weapon and vehicle parts).
//
// Each frame the visible nodes become quads (pictures, bar fills, button
// faces, text glyphs from the .dif fonts) for the renderer. Textures are
// under Menu/HUD/Texture/. Nodes fade in and out over their in/out times
// when they have an alpha show effect.
//
// The node types are BF2142.exe's dice::meme nodes (the exe lays them out;
// RendDX9.dll only draws the quads they make): compasses
// (Bf2CompassNode, 0x007e9560), lists (Bf2NewListBoxNode, 0x007de7d0) and
// the map (Bf2MenuMapNode, 0x00795620) as the exe draws them. What is
// approximate: the corner roots' positions (the engine places them by the
// screen size), and object markers and the spawn menu, kept but not drawn.
#pragma once

#include "HudFont.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::hud {

struct HudQuad {
	std::string texture; // a file path ("Menu/HUD/Texture/..." or a font's .dds)
	float x0, y0, x1, y1; // 800 x 600 screen
	float u0, v0, u1, v1;
	float color[4];
	float rotation = 0;  // degrees, about the quad's centre
	// Drawn only inside this rectangle (the exe's clip rectangle,
	// 0x0077af80) when clipped is set.
	bool clipped = false;
	float clip[4] = {0, 0, 0, 0}; // x0, y0, x1, y1
};

class HudManager {
public:
	using Reader = std::function<bool(const std::string& path, std::string& text)>;
	explicit HudManager(Reader reader);
	~HudManager();

	// Variables the game sets.
	void set(const std::string& name, float value);
	void setString(const std::string& name, const std::string& value);
	float number(const std::string& name) const;
	std::string string(const std::string& name) const;

	// One frame: advances fades and lists what to draw, back to front.
	std::vector<HudQuad> frame(float seconds);
	size_t nodeCount() const { return m_nodes.size(); }

	struct Node;
	Node* find(const std::string& name) const;
	Node* create(int type, const std::string& parent, const std::string& name, int x, int y, int w, int h);
	Node* active() const { return m_active; }

	// Rows of a list data source (setListNodeData: 1 enemy and 2 friendly
	// scores, 9 and 10 their totals, 3 chat...), as cell texts, and where
	// its cells go (x within the list, width, alignment 0 left 1 right 2
	// centre).
	struct ListColumn {
		float x;
		float width;   // > 0: the column ends there (else at the next column)
		int alignment; // 0 left, 1 right, 2 centre
	};
	void setList(int data, std::vector<std::vector<std::string>> rows);
	void setListColumns(int data, std::vector<ListColumn> columns);
	// The cells' text colour (the list data's cells carry it, 0x007ddb90;
	// black unless set) and the selected row (-1: none).
	void setListTextColor(int data, float r, float g, float b, float a);
	void setListSelection(int data, int row) { m_listSelection[data] = row; }
	// The map node (createMapNode, Bf2MenuMapNode): the level's map (a
	// texture covering the terrain from originX/originZ, width by depth
	// metres, north up) and what to mark on it, in world coordinates.
	// MapFullSize picks the big map (the spawn screen) over the minimap.
	// The map is zoomed by 2.3 to the power of a zoom level and centred on
	// `focus`, kept covering the node; see drawMap.
	struct MapMarker {
		std::string texture; // under Menu/HUD/Texture/
		float x, z;          // world position
		float size;          // pixels (fixed: icons don't scale with the zoom)
		float yaw = 0;       // degrees, clockwise (0 points north)
		float color[4] = {1, 1, 1, 1};
		int id = -1;         // >= 0: clickable (mapMarkerAt returns it)
	};
	void setMap(const std::string& texture, float originX, float originZ, float width, float depth);
	void setMapFocus(float x, float z) { m_mapFocus[0] = x; m_mapFocus[1] = z; }
	// The zoom (0x00779600: an index into the zoom levels, 1 at start), the
	// player's seat (a vehicle's minimap levels are lower) and whether the
	// camera is inside a Titan (its levels are multiplied).
	void setMapZoom(int index) { m_mapZoomIndex = index < 0 ? 0 : index > 2 ? 2 : index; }
	void setMapInVehicle(bool inVehicle) { m_mapInVehicle = inVehicle; }
	void setMapInsideTitan(bool inside) { m_mapInsideTitan = inside; }
	// The combat area's bounds (world): the map is kept showing only this
	// (made square, with a margin; 0x0078b490). Unset: the whole level.
	void setMapArea(float minX, float minZ, float maxX, float maxZ);
	void clearMapArea() { m_mapAreaSet = false; }
	void setMapMarkers(std::vector<MapMarker> markers) { m_mapMarkers = std::move(markers); }
	// The clickable marker at a point of the 800 x 600 screen, as last
	// drawn; -1 for none.
	int mapMarkerAt(float x, float y) const;
	// The world point (x, z) under a point of the map as last drawn; false
	// off the map.
	bool mapWorldAt(float x, float y, float& worldX, float& worldZ) const;
	// The pointer (menus) in the 800 x 600 screen, and a click there:
	// buttons run their console commands (setButtonNodeConCmd) through the
	// runner. True when a button took the click.
	void setPointer(float x, float y) { m_pointer[0] = x; m_pointer[1] = y; }
	bool click(float x, float y);
	void setCommandRunner(std::function<void(const std::string&)> run) { m_run = std::move(run); }
	// Text ids ("HUD_TEXT_...") to the language's text (Localization).
	void setLocalizer(std::function<std::string(const std::string&)> localize) { m_localize = std::move(localize); }
	void setActive(Node* node) { m_active = node; }
	const HudFont* font(const std::string& path);

private:
	bool visible(const Node& node) const;
	void draw(Node& node, float originX, float originY, float alpha, const float* tint, float seconds,
		std::vector<HudQuad>& out);
	void drawText(const Node& node, float x, float y, const float color[4], std::vector<HudQuad>& out);
	void drawString(const std::string& font, const std::string& text, float x, float y, float width, int alignment,
		const float color[4], std::vector<HudQuad>& out);
	void drawList(const Node& node, float x, float y, const float color[4], std::vector<HudQuad>& out);
	void drawCell(const Node& node, const std::string& text, float x, float rowY, const ListColumn& column, float columnEnd,
		const float textColor[4], float alpha, std::vector<HudQuad>& out);
	void drawCompass(const Node& node, float x, float y, const float color[4], std::vector<HudQuad>& out);
	std::map<int, std::vector<std::vector<std::string>>> m_lists;
	std::map<int, std::vector<ListColumn>> m_listColumns;
	std::map<int, std::array<float, 4>> m_listTextColors;
	std::map<int, int> m_listSelection;
	std::function<std::string(const std::string&)> m_localize;
	size_t m_parentStart = 0; // where the drawing node's parent's quads began
	void drawMap(const Node& node, float alpha, float seconds, std::vector<HudQuad>& out);
	std::string m_mapTexture;
	int m_mapZoomIndex = 1;
	bool m_mapInVehicle = false, m_mapInsideTitan = false;
	float m_mapLevel = 0;       // the zoom level as eased (0x714)
	bool m_mapLevelSet = false;
	bool m_mapAreaSet = false;
	float m_mapArea[4] = {0, 0, 0, 0}; // world minX, minZ, maxX, maxZ
	float m_mapOrigin[2] = {0, 0}, m_mapSize[2] = {1, 1};
	float m_mapFocus[2] = {0, 0};
	std::vector<MapMarker> m_mapMarkers;
	struct Hit {
		float x0, y0, x1, y1;
		int id;
	};
	std::vector<Hit> m_mapHits;
	// The map as last drawn: the node's rectangle and where the whole map
	// texture went.
	struct MapView {
		bool drawn = false;
		float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		float mapX = 0, mapY = 0, mapW = 1, mapH = 1;
	} m_mapView;
	struct ButtonHit {
		float x0, y0, x1, y1;
		const Node* node;
	};
	std::vector<ButtonHit> m_buttonHits;
	float m_pointer[2] = {-1, -1};
	std::function<void(const std::string&)> m_run;

	Reader m_reader;
	std::map<std::string, std::unique_ptr<Node>> m_nodes; // by lower-case name
	std::vector<Node*> m_roots;
	Node* m_active = nullptr;
	int m_pass = 0; // the drawing pass (frame())
	std::map<std::string, float> m_numbers;
	std::map<std::string, std::string> m_strings;
	std::map<std::string, std::unique_ptr<HudFont>> m_fonts;
};

// hudBuilder.* and hudManager.* console methods.
void registerHudConsoleMethods(Console& console, HudManager& hud);

} // namespace dice::hfe::hud
