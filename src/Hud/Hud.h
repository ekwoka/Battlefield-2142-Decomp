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
// What is approximate: the corner roots' positions (the engine places
// them by the screen size), and node types drawn as their plain parts only
// (compass, object markers, lists, maps and the spawn menu are kept but not
// drawn yet).
#pragma once

#include "HudFont.h"

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
		float width;
		int alignment;
	};
	void setList(int data, std::vector<std::vector<std::string>> rows);
	void setListColumns(int data, std::vector<ListColumn> columns);
	// The map node (createMapNode): the level's map (a texture covering the
	// terrain from originX/originZ, width by depth metres, north up) and
	// what to mark on it, in world coordinates. MapFullSize picks the big
	// map (the spawn screen) over the minimap, which shows a window of a
	// third of the level round `focus`.
	struct MapMarker {
		std::string texture; // under Menu/HUD/Texture/
		float x, z;          // world position
		float size;          // pixels
		float yaw = 0;       // degrees, clockwise (0 points north)
		float color[4] = {1, 1, 1, 1};
		int id = -1;         // >= 0: clickable (mapMarkerAt returns it)
	};
	void setMap(const std::string& texture, float originX, float originZ, float width, float depth);
	void setMapFocus(float x, float z) { m_mapFocus[0] = x; m_mapFocus[1] = z; }
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
	std::map<int, std::vector<std::vector<std::string>>> m_lists;
	std::map<int, std::vector<ListColumn>> m_listColumns;
	std::function<std::string(const std::string&)> m_localize;
	size_t m_parentStart = 0; // where the drawing node's parent's quads began
	void drawMap(const Node& node, float alpha, std::vector<HudQuad>& out);
	std::string m_mapTexture;
	float m_mapOrigin[2] = {0, 0}, m_mapSize[2] = {1, 1};
	float m_mapFocus[2] = {0, 0};
	std::vector<MapMarker> m_mapMarkers;
	struct Hit {
		float x0, y0, x1, y1;
		int id;
	};
	std::vector<Hit> m_mapHits;
	// The map as last drawn: its screen rectangle and texture window.
	struct MapView {
		bool drawn = false;
		float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		float u0 = 0, v0 = 0, span = 1;
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
