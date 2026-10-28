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
// the map (Bf2MenuMapNode, 0x00795620) as the exe draws them; lists with
// commands take clicks on their rows (the squad and commander menus).
// Transform lists (Bf2TransformListNode, render 0x007eba70) stack the
// nodes added to them at a position variable (the commander's and squad
// leader's right-click menus at OrderMenuPosX/Y); move and blend effects
// slide nodes in and out and set their blend; object markers
// (Bf2ObjectMarkerNode, 0x007e6880) mark the lock-on weapons' targets the
// game hands in (setMarkerSource).
// What is approximate: the corner roots' positions (the engine places them
// by the screen size); see docs/engine.md "HUD nodes" for the rest.
#pragma once

#include "HudFont.h"

#include <array>
#include <cstdint>
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
	// addNodeBlendEffect <source> <destination> (0x007b20c0: a
	// "<node>BlendEffect" render-state node above the node): Direct3D 9
	// D3DBLEND values for the quads below it (the vehicle HUDs' "7 2":
	// D3DBLEND_DESTALPHA, D3DBLEND_ONE). 0 0: the HUD's usual
	// source-alpha blend.
	int blend[2] = {0, 0};
	// setPictureNodeCenterPoint x y: the rotation's pivot (the picture
	// node's render, 0x007c67b0, turns about the stored point instead of
	// the rectangle's centre), in the 800 x 600 screen; unset, the centre.
	bool pivotSet = false;
	float pivot[2] = {0, 0};
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
		// > 0: the column's cells are pictures (a texture path under
		// Menu/HUD/Texture/), this many pixels square (the VOIP list's
		// kit and squad icons, the map vote's buttons).
		float iconSize = 0;
	};
	void setList(int data, std::vector<std::vector<std::string>> rows);
	// Edit boxes by data id (setEditNodeData: 1 chat, 2 the squad name, 3
	// the commander's chat): their text, the box with the keys (-1 none;
	// it draws a caret) and the scripts' maximum length (0 none).
	void setEditText(int data, const std::string& text) { m_editText[data] = text; }
	std::string editText(int data) const
	{
		const auto it = m_editText.find(data);
		return it == m_editText.end() ? std::string() : it->second;
	}
	void setEditFocus(int data) { m_editFocus = data; }
	int editFocus() const { return m_editFocus; }
	void setEditMaxLength(int data, int length) { m_editMaxLength[data] = length; }
	int editMaxLength(int data) const
	{
		const auto it = m_editMaxLength.find(data);
		return it == m_editMaxLength.end() ? 0 : it->second;
	}
	void setListColumns(int data, std::vector<ListColumn> columns);
	// The cells' text colour (the list data's cells carry it, 0x007ddb90;
	// black unless set) and the selected row (-1: none).
	void setListTextColor(int data, float r, float g, float b, float a);
	// Per row (the chat's lines by who said them); rows without one take
	// the list's.
	void setListRowColors(int data, std::vector<std::array<float, 4>> colors) { m_listRowColors[data] = std::move(colors); }
	void setListSelection(int data, int row) { m_listSelection[data] = row; }
	int listSelection(int data) const
	{
		const auto it = m_listSelection.find(data);
		return it == m_listSelection.end() ? -1 : it->second;
	}
	// A row's cell text ("" past the row's cells or the list's rows); rows
	// may carry cells past the columns drawn (the squad menu's id and row
	// type in cells 13 and 14).
	std::string listCell(int data, int row, int column) const;
	// The last row clicked in a list with commands (setListNodeConCmd):
	// its data source, row and the column under the pointer.
	struct ListClick {
		int data = -1;
		int row = -1;
		int column = -1;
	};
	const ListClick& lastListClick() const { return m_listClick; }
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
	// buttons run their console commands (setButtonNodeConCmd; with
	// `alternate`, the right button, setButtonNodeAltConCmd) through the
	// runner, and the mouse-over nodes under it their release commands
	// (setMouseOverNodeConCmd ... 3). True when a button took the click.
	void setPointer(float x, float y) { m_pointer[0] = x; m_pointer[1] = y; }
	float pointerX() const { return m_pointer[0]; }
	float pointerY() const { return m_pointer[1]; }
	bool click(float x, float y, bool alternate = false);

	// The pointer's textures (HudManager.setMouseTexture*, the HUD
	// manager's slots from 0x0075e87f: the plain pointer, the map's, and
	// one per commander tool) and its size (setPointerSize, slot 0x1b4).
	enum class Pointer { Empty, Full, Artillery, Uav, Supply, MoveTitan, OrbitalStrike, Count };
	void setPointerTexture(Pointer kind, const std::string& texture) { m_pointerTextures[static_cast<int>(kind)] = texture; }
	const std::string& pointerTexture(Pointer kind) const { return m_pointerTextures[static_cast<int>(kind)]; }
	void setPointerSize(float w, float h) { m_pointerSize[0] = w; m_pointerSize[1] = h; }
	// The pointer as a quad centred on the pointer (false: no texture or
	// no pointer).
	bool pointerQuad(Pointer kind, HudQuad& quad) const;
	// HudManager's other settings, kept by name (setCommPos, setCommSize,
	// setCommMousePos, setSpottedMenuPos/Size, setSpottedMousePos,
	// setCommMouseSensitivity, setPointerMouseSensitivity,
	// setMaximumNrOfCPs, ...): up to four numbers each.
	void setSetting(const std::string& name, std::array<float, 4> values) { m_settings[name] = values; }
	std::array<float, 4> setting(const std::string& name) const
	{
		const auto it = m_settings.find(name);
		return it == m_settings.end() ? std::array<float, 4> {} : it->second;
	}

	// The minimap's filters (Minimap.setPaintKit / setPaintVehicle,
	// 0x0077e670 / 0x0077e6b0: a bit per kit or vehicle kind toggled;
	// setPaintAllKits / AllVehicles 0x0077fff0 / 0x00780040: all on, or
	// all off when all were on) and its zoom (Minimap.setZoom,
	// 0x007800d0 -> 0x0077e850: the zoom index steps 0, 1, 2 and back to 0).
	bool paintsKit(int kit) const { return kit < 0 || kit >= 8 || (m_paintKits >> kit & 1) != 0; }
	bool paintsVehicle(int kind) const { return kind < 0 || kind >= 32 || (m_paintVehicles >> kind & 1) != 0; }
	void togglePaintKit(int kit);
	void togglePaintVehicle(int kind);
	void paintAllKits();
	void paintAllVehicles();
	int mapZoom() const { return m_mapZoomIndex; }
	// In the commander screen (CommanderShow) the step goes to its own
	// index (0x0077e850 keeps one per mode, +0x744 for the commander's).
	void cycleMapZoom()
	{
		int& index = number("CommanderShow") != 0 ? m_commanderZoomIndex : m_mapZoomIndex;
		index = index + 1 < 3 ? index + 1 : 0;
	}

	// The commander screen's map: the node at setCommanderPos/Size (the
	// 600 x 600 square between the screen's black side borders) and its
	// zoom index, 2 being the overhead view (the asset zoom buttons set
	// it, 0x007d7f90 -> the map's slot 0xd8 (2, 1)).
	void setCommanderMapZoom(int index) { m_commanderZoomIndex = index < 0 ? 0 : index > 2 ? 2 : index; }
	int commanderMapZoom() const { return m_commanderZoomIndex; }
	// The overhead view (the team's OverheadCamera over the commander's
	// point, which the player's view takes, 0x00779680): while it is on
	// the map's texture fades out (MenuMapAlpha, +0x7b8 to 0, eased at
	// +0x7b0) to show the view through the node, and the map is zoomed so
	// the node's height spans the view's ground width (0x0077c4b0: 2 tan
	// (fov / 2) times the camera's height), centred on the camera.
	void setOverhead(bool on, float x, float z, float groundWidth)
	{
		m_overhead = on;
		m_overheadPoint[0] = x;
		m_overheadPoint[1] = z;
		m_overheadWidth = groundWidth;
	}
	bool overhead() const { return m_overhead; }
	float overheadX() const { return m_overheadPoint[0]; }
	float overheadZ() const { return m_overheadPoint[1]; }
	// The node's rectangle (800 x 600 screen) as last drawn with the
	// overhead view on; false when it is off or the map isn't shown.
	bool overheadRect(float rect[4]) const
	{
		if (!m_overhead || !m_mapView.drawn) {
			return false;
		}
		rect[0] = m_mapView.x0;
		rect[1] = m_mapView.y0;
		rect[2] = m_mapView.x1;
		rect[3] = m_mapView.y1;
		return true;
	}

	// Object markers (Bf2ObjectMarkerNode, its update 0x007e6880): a
	// visible marker node asks the game for the targets of its lock-on
	// weapon (setObjectMarkerNodeWeapon: the seat's weapon, +0x58) of its
	// target type (setObjectMarkerNodeLockOnType, +0x54: 2 TTHeat by
	// default, 1 TTLaser, 5 TTBeacon, -1 the type of the weapon's own
	// target), at most setObjectMarkerNodeObjects (+8) of them, cut by
	// the weapon's range with setObjectMarkerNodeRC (+0x61). Each target is
	// a point in the 800 x 600 screen with its state (0 a candidate, 1
	// being locked, 2 locked: the marker textures 0..2) and its distance
	// (the labels' text, 0x007e56e0).
	struct MarkerQuery {
		int lockOnType = 2;
		int weapon = 0;
		int maxObjects = 0;
		bool rangeCheck = false;
		float x0 = 0, y0 = 0, x1 = 800, y1 = 600; // the node's rectangle
	};
	struct MarkerTarget {
		float x = 0, y = 0;
		int state = 0;
		float distance = 0;
	};
	using MarkerSource = std::function<std::vector<MarkerTarget>(const MarkerQuery&)>;
	void setMarkerSource(MarkerSource source) { m_markerSource = std::move(source); }
	void setCommandRunner(std::function<void(const std::string&)> run) { m_run = std::move(run); }
	// Text ids ("HUD_TEXT_...") to the language's text (Localization).
	void setLocalizer(std::function<std::string(const std::string&)> localize) { m_localize = std::move(localize); }
	void setActive(Node* node) { m_active = node; }
	// A node with setNodeShowConCmd commands (checked each frame).
	void noteShowCommands(Node* node);
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
	void drawMarkers(Node& node, float x, float y, const float color[4], std::vector<HudQuad>& out);
	void layoutTransformLists();
	void runCommands(const std::vector<std::pair<int, std::string>>& commands, int event);
	std::string m_pointerTextures[static_cast<int>(Pointer::Count)];
	float m_pointerSize[2] = {32, 32};
	std::map<std::string, std::array<float, 4>> m_settings;
	uint32_t m_paintKits = 0xff, m_paintVehicles = 0xffffffff;
	MarkerSource m_markerSource;
	uint64_t m_frameNumber = 0;
	std::vector<Node*> m_transformLists; // in creation order
	std::vector<Node*> m_showCommandNodes;
	struct MouseOverHit {
		float x0, y0, x1, y1;
		const Node* node;
	};
	std::vector<MouseOverHit> m_mouseOverHits;
	std::map<int, std::vector<std::vector<std::string>>> m_lists;
	std::map<int, std::vector<std::array<float, 4>>> m_listRowColors;
	std::map<int, std::string> m_editText;
	std::map<int, int> m_editMaxLength;
	int m_editFocus = -1;
	std::map<int, std::vector<ListColumn>> m_listColumns;
	std::map<int, std::array<float, 4>> m_listTextColors;
	std::map<int, int> m_listSelection;
	std::function<std::string(const std::string&)> m_localize;
	size_t m_parentStart = 0; // where the drawing node's parent's quads began
	void drawMap(const Node& node, float alpha, float seconds, std::vector<HudQuad>& out);
	std::string m_mapTexture;
	int m_mapZoomIndex = 1;
	int m_commanderZoomIndex = 0;
	bool m_overhead = false;
	float m_overheadPoint[2] = {0, 0};
	float m_overheadWidth = 0;
	float m_mapAlpha = 1; // the map texture's alpha as eased (+0x7b0)
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
	struct ListHit {
		float x0, y0, x1, y1;
		const Node* node;
		int row;
		std::vector<float> columns; // each column's left edge
	};
	std::vector<ListHit> m_listHits;
	ListClick m_listClick;
	float m_listClickTime = -1;
	float m_clock = 0; // seconds of frames drawn
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
