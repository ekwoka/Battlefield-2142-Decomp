#include "Hud.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace dice::hfe::hud {

namespace {

std::string lower(std::string s)
{
	for (char& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

std::string slashes(std::string s)
{
	std::replace(s.begin(), s.end(), '\\', '/');
	return s;
}

const char* kTextureRoot = "Menu/HUD/Texture/";

enum NodeType {
	kSplit,
	kTransform,
	kPicture,
	kText,
	kBar,
	kButton,
	kCompass,
	kTransformList,
	kObjectMarker,
	kList,
	kMap,
	kEdit,
	kMouseOver,
	kOccupied,
	kOther,
};

// The corners the engine creates, in the 800 x 600 screen (approximate).
struct Root {
	const char* name;
	int x, y;
};
constexpr Root kRoots[] = {
	{"Global", 0, 0},
	{"TopLayer", 0, 0},
	{"BottomLeftStatic", 0, 566},
	{"BottomLeftAnimate", 0, 566},
	{"BottomRightStatic", 400, 566},
	{"BottomRightAnimate", 400, 566},
};

} // namespace

struct HudManager::Node {
	std::string name;
	int type = kSplit;
	Node* parent = nullptr;
	std::vector<Node*> children;
	int x = 0, y = 0, w = 0, h = 0;
	float color[4] = {1, 1, 1, 1};
	std::string showVariable;
	struct Logic {
		std::string op; // AND, OR, NOT, EQUAL
		std::string variable;
		float value;
	};
	std::vector<Logic> logic;
	std::string alphaVariable;
	std::string rgbVariables[3];
	std::string posVariables[2];
	float inTime = 0, outTime = 0;
	bool alphaShowEffect = false;
	// Per drawing pass (see frame()):
	float shown[2] = {0, 0};          // 0..1 while fading in or out
	bool started[2] = {false, false}; // the first frame shows or hides at once
	// Pictures and buttons.
	std::string texture;
	std::string textureVariable;
	std::string rotateVariable;
	float rotation = 0;
	std::string buttonTextures[4]; // 1 normal, 2 under the pointer (0 older scripts)
	std::vector<std::pair<int, std::string>> buttonCommands; // (event, console line)
	// Text.
	std::string text;
	std::string textVariable;
	std::string font;
	int alignment = 0; // 0 left, 1 right, 2 centre
	// Edit boxes (createEditNode; the HUD manager's edit nodes by data id,
	// slot 0x210 0x007c6d40): 1 the chat's SayAll, 2 the squad name, 3
	// the commander's chat.
	int editData = -1;
	int editMaxLength = 0;
	// Bars: textures 0 (empty) and 1 (full), a value variable 0..1.
	int barType = 3;
	std::string barTextures[2];
	std::string valueVariable;
	int snap = 0;
	bool inverse = false;
	float spanMin = 0, spanMax = 1;
	// Lists (Bf2NewListBoxNode): rows from the game's data source
	// (setListNodeData). Defaults from createListNode (0x007aeae0).
	int listData = -1;
	float rowHeight = 19;      // 0x18
	float rowSpacing = 0;      // 0x1c (setListNodeRowSpacing)
	bool listSelectable = false; // 0x94 (createListNode's last argument)
	bool listBordered = false; // 0x95 (set by setListNodeBorder)
	std::string listFonts[4];  // setListNodeFont <font> <index>; text picks one with "§n"
	float listBackground[4] = {1, 0, 0, 0};    // 0xa8
	float listBorderColor[4] = {1, 1, 0, 0};   // 0xb8
	float listSelectColor[4] = {1, 1, 1, 0.5f}; // 0xc8
	float listScrollbarColor[4] = {0, 0, 0, 0}; // 0xd8
	float listScrollbarBackground[4] = {0, 0, 0, 0}; // 0xe8
	int listBorder[4] = {0, 0, 0, 0}; // 0x98..: top (the header band), bottom, left, right
	float scrollbarWidth = 0;  // 0x24 (setListNodeScrollbar <width> <margin>)
	float scrollbarMargin = 1; // 0xf8
	bool listOutline = false;  // setListNodeOutline (the data's 0x20)
	// setListNodeConCmd <event> <command>: 1 a click on a row, 2 a double
	// click (the squad menu's SquadMenu.singleClickNew / doubleClickNew).
	std::vector<std::pair<int, std::string>> listCommands;
	// Maps: where the mini and the big map go (from the screen's centre).
	float miniPos[2] = {245, -291}, miniSize[2] = {146, 146};
	float maxiPos[2] = {-122, -271}, maxiSize[2] = {512, 512};
	// Compasses (Bf2CompassNode, a bar node): a strip texture slid by a
	// value variable; createCompassNode <parent> <name> <direction> x y w h
	// <wrap> <snap> (0x007e91c0).
	int compassDirection = 0;  // 0x130: 0 vertical, 3 horizontal
	bool compassWrap = false;  // 0x169
	bool compassSnap = false;  // 0x168
	std::string compassTexture;
	std::string compassSnapTextures[2]; // 0x14c (past the high end), 0x150 (past the low end)
	float compassTextureSize[2] = {0, 0}; // 0x140, 0x144
	int compassBorder[4] = {0, 0, 0, 0};  // 0x74..0x80
	int compassSnapOffset[4] = {0, 0, 0, 0}; // 0x158..0x164
	std::string compassVariable;
	int compassOffset = 0;     // 0x154
	int compassTiltMode = 0;   // 0x170: 2 left, 3 right
	float compassTilt = 0;     // 0x16c
	// setNodeOffset x y (kept apart too: a transform list places the
	// node at its slot plus this).
	int offset[2] = {0, 0};
	// Transform lists (Bf2TransformListNode, 0x007ebcd0; render
	// 0x007eba70): the nodes added (addTransformListNode, slot 0xd4
	// 0x007ebde0) are stacked downwards from the list's position (the
	// position variables, slot 0xd0 0x007eb9d0, else where it was made;
	// plus setTranformListNodeOffset, slot 0xcc 0x007ebd60), each its
	// height plus the list's spacing (createTransformListNode's last
	// argument, +0x24) below the one before.
	std::vector<Node*> listItems;
	int listSpacing = 0;
	int listOffset[2] = {0, 0};
	std::string listPosVariables[2];
	int createdAt[2] = {0, 0};
	// addNodeMoveShowEffect <direction> <distance> (0x007b1f40: a
	// Bf2MoveEffect node, 0x007e4230, above the node): while it fades in
	// or out the node and its children sit `distance` pixels along the
	// direction (radians, 0 to the right, pi/2 down) times what is left of
	// the fade.
	bool moveEffect = false;
	float moveAngle = 0, moveDistance = 0;
	int blend[2] = {0, 0}; // addNodeBlendEffect
	// setNodeTimesBlink <times> <mode> / setNodeIntervalBlink <seconds>
	// (0x007b1c00 / 0x007b1da0, the "<node>BlinkEffect" node's slots 0xdc
	// and 0xd8): once shown the node blinks `times` times, `interval`
	// seconds on and off, then stays.
	int blinkTimes = 0;
	float blinkInterval = 0;
	float shownSince[2] = {-1, -1};
	// setNodeShowConCmd <command> <event> (0x007b0e00: an
	// executeConsoleCommand effect): event 1 runs when the node shows, 0
	// when it hides.
	std::vector<std::pair<int, std::string>> showCommands;
	uint64_t drawnFrame = 0;
	bool wasDrawn = false;
	// Buttons: setButtonNodeMouseArea x y w h (slot 0x104: the clickable
	// rectangle from the node's corner), setButtonNodeAltConCmd (slot
	// 0x100: the right button's commands), setButtonNodeDebug (slot 0x10c).
	bool mouseArea = false;
	int area[4] = {0, 0, 0, 0};
	std::vector<std::pair<int, std::string>> altCommands;
	bool buttonDebug = false;
	// Mouse-over nodes (createMouseOverNode): setMouseOverNodeConCmd
	// <command> <event> (0x007b02a0, slot 0xf8), event 3 on the button's
	// release over the node (the kit screen's drop targets).
	std::vector<std::pair<int, std::string>> mouseOverCommands;
	uint64_t mouseOverFrame = 0;
	// Pictures: setPictureNodeBorder <left> <top> <right> <bottom> and
	// setPictureNodeBorderColor (0x007af640 / 0x007af6d0, slots 0xe4 /
	// 0xe8: lines of those widths in that colour round the picture),
	// setPictureNodeCenterPoint (slot 0xe0: the rotation's pivot from the
	// corner; unset, the centre).
	int pictureBorder[4] = {0, 0, 0, 0};
	float pictureBorderColor[4] = {0, 0, 0, 0};
	bool centerSet = false;
	int center[2] = {0, 0};
	// Text: setTextNodeOutLine <font> (0x007afa40: the outline font, the
	// text node's font object slot 0x5c), setTextNodeOutLineOffset (0x60)
	// and setTruncate (0x68: the text stops at the node's width).
	std::string outlineFont;
	float outlineOffset[2] = {0, 0};
	bool truncate = false;
	// Bars: setBarNodeBorder <a> <b> <c> <d> (0x007b0540, slot 0x104: the
	// fill keeps a and b pixels off its start and end along the bar, c and
	// d across), setBarNodeIncreaseShow (0x110), setBarNodeSnapDir
	// (0x108), setBarNodeRound <mode> <w> <h> (0x120), kept.
	int barBorder[4] = {0, 0, 0, 0};
	bool barIncreaseShow = false, barSnapDir = false;
	int barRound = 0;
	float barRoundSize[2] = {0, 0};
	// Occupied nodes (a vehicle's seat dots): the seat (createOccupiedNode's
	// third argument), setOccupiedNodeData (0x007afce0) and
	// setOccupiedNodePosVariable (0x007afc40, slot 0xcc).
	int occupiedSeat = 0;
	int occupiedData = 0;
	// Object markers (Bf2ObjectMarkerNode 0x007e5f30): textures and sizes
	// per state (slots 0xd8 / 0xdc, 4 each), the targets at most (+8, slot
	// 0xc8), the lock-on type (+0x54, 2), the weapon (+0x58), the range cut
	// (+0x61), the labels: on and their variables' prefix (slot 0xd0,
	// 0x007e4bd0: <prefix><n>String, <prefix><n>Show), their offset from
	// the marker (+0x40, -3 0) and their nodes (slot 0xd4 0x007e5cf0).
	std::string markerTextures[4];
	float markerSizes[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
	int markerObjects = 0;
	int lockOnType = 2;
	int markerWeapon = 0;
	bool markerRange = false;
	bool lockText = false;
	std::string lockTextPrefix;
	float lockTextOffset[2] = {-3, 0};
	std::vector<Node*> lockTextNodes;
};

HudManager::HudManager(Reader reader) : m_reader(std::move(reader))
{
	for (const Root& root : kRoots) {
		Node* node = create(kSplit, "", root.name, root.x, root.y, 800, 600);
		m_roots.push_back(node);
	}
}

HudManager::~HudManager() = default;

void HudManager::set(const std::string& name, float value) { m_numbers[lower(name)] = value; }
void HudManager::setString(const std::string& name, const std::string& value) { m_strings[lower(name)] = value; }

float HudManager::number(const std::string& name) const
{
	const auto it = m_numbers.find(lower(name));
	return it == m_numbers.end() ? 0.0f : it->second;
}

std::string HudManager::string(const std::string& name) const
{
	const auto it = m_strings.find(lower(name));
	if (it != m_strings.end()) {
		return it->second;
	}
	const auto n = m_numbers.find(lower(name));
	if (n != m_numbers.end()) {
		const float v = n->second;
		return v == std::floor(v) ? std::to_string(static_cast<int>(v)) : std::to_string(v);
	}
	return {};
}

HudManager::Node* HudManager::find(const std::string& name) const
{
	const auto it = m_nodes.find(lower(name));
	return it == m_nodes.end() ? nullptr : it->second.get();
}

HudManager::Node* HudManager::create(int type, const std::string& parentName, const std::string& name, int x, int y,
	int w, int h)
{
	auto& slot = m_nodes[lower(name)];
	if (!slot) {
		slot = std::make_unique<Node>();
	}
	Node* node = slot.get();
	node->name = name;
	node->type = type;
	node->x = x;
	node->y = y;
	node->w = w;
	node->h = h;
	node->createdAt[0] = x;
	node->createdAt[1] = y;
	if (type == kTransformList && std::find(m_transformLists.begin(), m_transformLists.end(), node) == m_transformLists.end()) {
		m_transformLists.push_back(node);
	}
	if (Node* parent = parentName.empty() ? nullptr : find(parentName)) {
		if (node->parent != parent) {
			parent->children.push_back(node);
			node->parent = parent;
		}
	}
	m_active = node;
	return node;
}

void HudManager::noteShowCommands(Node* node)
{
	if (std::find(m_showCommandNodes.begin(), m_showCommandNodes.end(), node) == m_showCommandNodes.end()) {
		m_showCommandNodes.push_back(node);
	}
}

const HudFont* HudManager::font(const std::string& path)
{
	const std::string key = lower(slashes(path));
	auto& slot = m_fonts[key];
	if (!slot) {
		// "Fonts/x.dif": the English, 800 x 600 one first.
		const std::string file = key.substr(key.rfind('/') + 1);
		std::string text;
		for (const std::string& candidate : {"Fonts/English/800/" + file, "Fonts/800/" + file, "Fonts/English/" + file, key}) {
			if (m_reader && m_reader(candidate, text)) {
				if (auto parsed = parseHudFont(text)) {
					slot = std::make_unique<HudFont>(std::move(*parsed));
					slot->name = candidate.substr(0, candidate.size() - 4); // the .dds beside it
				}
				break;
			}
		}
		if (!slot) {
			slot = std::make_unique<HudFont>(); // missing: draws nothing
		}
	}
	return slot->glyphs.empty() ? nullptr : slot.get();
}

// The show variable and the logic entries are terms ANDed together; an OR
// entry joins the term before it ("NOT AffectedByEMPShow 1" then "OR
// UpdateAllEnabled 1" is one term: either holds). NOT is "!=", the others
// "==".
bool HudManager::visible(const Node& node) const
{
	bool show = true;
	bool term = node.showVariable.empty() || number(node.showVariable) != 0;
	bool haveTerm = !node.showVariable.empty();
	for (const Node::Logic& l : node.logic) {
		const bool equal = number(l.variable) == l.value;
		if (l.op == "or") {
			term = haveTerm ? term || equal : equal;
		} else {
			show = show && term;
			term = l.op == "not" ? !equal : equal; // and, equal
		}
		haveTerm = true;
	}
	return show && term;
}

// RendDX9.dll draws the HUD twice a frame (0x1002d5d9): first with
// AffectedByEMPShow 0 and UpdateAllEnabled 1 (the communication, game
// info and soldier parts), then with AffectedByEMPShow 1 and
// UpdateAllEnabled 0 (the weapon and vehicle parts, which an EMP
// distorts), resetting AffectedByEMPShow after. Nodes check these in their
// show logic, so each pass draws its own half.
std::vector<HudQuad> HudManager::frame(float seconds)
{
	std::vector<HudQuad> quads;
	m_buttonHits.clear();
	m_listHits.clear();
	m_mouseOverHits.clear();
	m_clock += seconds;
	++m_frameNumber;
	layoutTransformLists();
	for (m_pass = 0; m_pass < 2; ++m_pass) {
		m_numbers["affectedbyempshow"] = static_cast<float>(m_pass);
		m_numbers["updateallenabled"] = static_cast<float>(1 - m_pass);
		for (Node* root : m_roots) {
			draw(*root, 0, 0, 1, nullptr, seconds, quads);
		}
	}
	m_numbers["affectedbyempshow"] = 0;
	// setNodeShowConCmd: the nodes that began or stopped being drawn.
	for (Node* node : m_showCommandNodes) {
		const bool drawn = node->drawnFrame == m_frameNumber;
		if (drawn != node->wasDrawn) {
			node->wasDrawn = drawn;
			runCommands(node->showCommands, drawn ? 1 : 0);
		}
	}
	return quads;
}

void HudManager::runCommands(const std::vector<std::pair<int, std::string>>& commands, int event)
{
	if (!m_run) {
		return;
	}
	for (const auto& [when, command] : commands) {
		if (when == event) {
			m_run(command);
		}
	}
}

// Bf2TransformListNode::render (0x007eba70), for every list each frame
// before drawing: the list's position is its position variables' values
// (when the game has set them) or where it was made, plus its offset; the
// block of its nodes (as tall as their heights and the spacing between
// them, as wide as the widest) is kept on the screen; then each node goes
// to its slot (the node's slot 0xcc, 0x007da5b0 for transform nodes), its
// own setNodeOffset on top. (The exe adds the offset to the position each
// frame; without position variables that would creep, so here the offset
// applies once. Its bounds are the parent's rectangle about the screen's
// centre; with the HUD's roots that is the 800 x 600 screen.)
void HudManager::layoutTransformLists()
{
	for (Node* list : m_transformLists) {
		if (list->listItems.empty()) {
			continue;
		}
		float pos[2] = {static_cast<float>(list->createdAt[0]), static_cast<float>(list->createdAt[1])};
		for (int axis = 0; axis < 2; ++axis) {
			if (!list->listPosVariables[axis].empty()) {
				const auto it = m_numbers.find(lower(list->listPosVariables[axis]));
				if (it != m_numbers.end()) {
					pos[axis] = static_cast<float>(static_cast<int>(it->second)); // the variables are ints
				}
			}
			pos[axis] += static_cast<float>(list->listOffset[axis]);
		}
		float width = 0, height = 0;
		const float spacing = static_cast<float>(list->listSpacing);
		for (const Node* item : list->listItems) {
			width = std::max(width, static_cast<float>(item->w));
			height += static_cast<float>(item->h) + spacing;
		}
		pos[0] = std::max(0.0f, std::min(pos[0], 800 - width));
		pos[1] = std::max(0.0f, std::min(pos[1], 600 - height));
		float y = pos[1];
		for (Node* item : list->listItems) {
			item->x = static_cast<int>(pos[0]) + item->offset[0];
			item->y = static_cast<int>(y) + item->offset[1];
			y += static_cast<float>(item->h) + spacing;
		}
		list->x = static_cast<int>(pos[0]);
		list->y = static_cast<int>(pos[1]);
	}
}

void HudManager::draw(Node& node, float originX, float originY, float alpha, const float* tint, float seconds,
	std::vector<HudQuad>& out)
{
	const bool show = visible(node);
	float& shown = node.shown[m_pass];
	const bool animated = (node.alphaShowEffect || node.moveEffect) && (node.inTime > 0 || node.outTime > 0);
	const bool wasHidden = shown <= 0;
	if (!node.started[m_pass]) {
		node.started[m_pass] = true;
		shown = show ? 1.0f : 0.0f;
	} else if (animated) {
		const float time = show ? node.inTime : node.outTime;
		const float step = time > 0 ? seconds / time : 1.0f;
		shown = std::clamp(shown + (show ? step : -step), 0.0f, 1.0f);
	} else {
		shown = show ? 1.0f : 0.0f;
	}
	if (shown <= 0) {
		node.shownSince[m_pass] = -1;
		return;
	}
	if (wasHidden || node.shownSince[m_pass] < 0) {
		node.shownSince[m_pass] = m_clock;
	}
	// Blinking: off every other interval for its first `times` blinks.
	if (node.blinkTimes > 0 && node.blinkInterval > 0) {
		const float since = m_clock - node.shownSince[m_pass];
		const int phase = static_cast<int>(std::floor(since / node.blinkInterval));
		if (phase < node.blinkTimes * 2 && phase % 2 == 1) {
			return;
		}
	}
	node.drawnFrame = m_frameNumber;
	if (node.alphaShowEffect || !node.moveEffect) {
		alpha *= shown;
	}
	if (!node.alphaVariable.empty()) {
		alpha *= std::clamp(number(node.alphaVariable), 0.0f, 1.0f);
	}
	float color[4] = {node.color[0], node.color[1], node.color[2], node.color[3] * alpha};
	// Colour variables tint the node and the nodes below it (the crosshair
	// parts take their split node's CrossHairColor*).
	if (!node.rgbVariables[0].empty()) {
		for (int i = 0; i < 3; ++i) {
			color[i] = number(node.rgbVariables[i]);
		}
		tint = color;
	} else if (tint) {
		for (int i = 0; i < 3; ++i) {
			color[i] = tint[i];
		}
	}
	float x = originX + static_cast<float>(node.x);
	float y = originY + static_cast<float>(node.y);
	// A position variable sets the coordinate (the crosshair's parts have
	// x 0 and take CrossLeftPos = 399 - length - spread, 0x007be770).
	// (Until the game sets one the node keeps its own.) Its setNodeOffset
	// goes on top (the transform node's +0x2c, 0x007da720: the map menus'
	// markers sit -20 -20 from OrderMenuPosX/Y).
	for (int axis = 0; axis < 2; ++axis) {
		if (!node.posVariables[axis].empty()) {
			const auto it = m_numbers.find(lower(node.posVariables[axis]));
			if (it != m_numbers.end()) {
				(axis == 0 ? x : y) = (axis == 0 ? originX : originY) + it->second + static_cast<float>(node.offset[axis]);
			}
		}
	}
	// The move effect's slide, for the node and everything below it.
	float moveX = 0, moveY = 0;
	if (node.moveEffect && shown < 1) {
		moveX = std::cos(node.moveAngle) * node.moveDistance * (1 - shown);
		moveY = std::sin(node.moveAngle) * node.moveDistance * (1 - shown);
		x += moveX;
		y += moveY;
	}
	const size_t first = out.size();
	const float w = static_cast<float>(node.w), h = static_cast<float>(node.h);
	const auto quad = [&](const std::string& texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
						  float v1, float rotation) {
		if (texture.empty() || color[3] <= 0) {
			return;
		}
		HudQuad q {kTextureRoot + slashes(texture), x0, y0, x1, y1, u0, v0, u1, v1, {color[0], color[1], color[2], color[3]},
			rotation};
		out.push_back(q);
	};
	const auto fill = [&](const float rgba[4], float x0, float y0, float x1, float y1) {
		if (rgba[3] * alpha > 0 && x1 > x0 && y1 > y0) {
			out.push_back({"Menu/Console/white.dds", x0, y0, x1, y1, 0, 0, 1, 1, {rgba[0], rgba[1], rgba[2], rgba[3] * alpha}, 0});
		}
	};
	switch (node.type) {
	case kPicture: {
		std::string texture = node.textureVariable.empty() ? node.texture : string(node.textureVariable);
		const float rotation = node.rotation + (node.rotateVariable.empty() ? 0 : number(node.rotateVariable));
		quad(texture, x, y, x + w, y + h, 0, 0, 1, 1, rotation);
		const int* b = node.pictureBorder;
		fill(node.pictureBorderColor, x, y, x + w, y + static_cast<float>(b[1]));
		fill(node.pictureBorderColor, x, y + h - static_cast<float>(b[3]), x + w, y + h);
		fill(node.pictureBorderColor, x, y + static_cast<float>(b[1]), x + static_cast<float>(b[0]), y + h - static_cast<float>(b[3]));
		fill(node.pictureBorderColor, x + w - static_cast<float>(b[2]), y + static_cast<float>(b[1]), x + w,
			y + h - static_cast<float>(b[3]));
		break;
	}
	case kButton: {
		const float ax = node.mouseArea ? x + static_cast<float>(node.area[0]) : x;
		const float ay = node.mouseArea ? y + static_cast<float>(node.area[1]) : y;
		const float aw = node.mouseArea ? static_cast<float>(node.area[2]) : w;
		const float ah = node.mouseArea ? static_cast<float>(node.area[3]) : h;
		const bool over = m_pointer[0] >= ax && m_pointer[0] <= ax + aw && m_pointer[1] >= ay && m_pointer[1] <= ay + ah;
		const std::string& normal = node.buttonTextures[1].empty() ? node.buttonTextures[0] : node.buttonTextures[1];
		quad(over && !node.buttonTextures[2].empty() ? node.buttonTextures[2] : normal, x, y, x + w, y + h, 0, 0, 1, 1, 0);
		if ((!node.buttonCommands.empty() || !node.altCommands.empty()) && color[3] > 0) {
			m_buttonHits.push_back({ax, ay, ax + aw, ay + ah, &node});
		}
		break;
	}
	case kMouseOver:
		// (Once a frame: a node outside the EMP logic is in both passes.)
		if (!node.mouseOverCommands.empty() && node.drawnFrame != node.mouseOverFrame) {
			node.mouseOverFrame = node.drawnFrame;
			m_mouseOverHits.push_back({x, y, x + w, y + h, &node});
		}
		break;
	case kBar: {
		quad(node.barTextures[0], x, y, x + w, y + h, 0, 0, 1, 1, 0);
		float value = node.valueVariable.empty() ? 0 : number(node.valueVariable);
		if (node.spanMax != node.spanMin) {
			value = (value - node.spanMin) / (node.spanMax - node.spanMin);
		}
		value = std::clamp(node.inverse ? 1 - value : value, 0.0f, 1.0f);
		if (node.snap > 0) {
			value = std::floor(value * static_cast<float>(node.snap) + 0.5f) / static_cast<float>(node.snap);
		}
		// The border (top, bottom, left, right): the fill takes the inner
		// rectangle, filling it along the bar (the conquest screens' point
		// bars split one node, "0 0 17 76" from the right and "0 0 77 15"
		// from the left, 152 wide).
		const float bx0 = x + static_cast<float>(node.barBorder[2]), bx1 = x + w - static_cast<float>(node.barBorder[3]);
		const float by0 = y + static_cast<float>(node.barBorder[0]), by1 = y + h - static_cast<float>(node.barBorder[1]);
		const float u0 = w > 0 ? (bx0 - x) / w : 0, u1 = w > 0 ? (bx1 - x) / w : 1;
		const float v0 = h > 0 ? (by0 - y) / h : 0, v1 = h > 0 ? (by1 - y) / h : 1;
		if (value > 0 && bx1 > bx0 && by1 > by0) {
			const float fx = (bx1 - bx0) * value, fy = (by1 - by0) * value;
			const float fu = (u1 - u0) * value, fv = (v1 - v0) * value;
			switch (node.barType) {
			case 0: // filling upwards
				quad(node.barTextures[1], bx0, by1 - fy, bx1, by1, u0, v1 - fv, u1, v1, 0);
				break;
			case 1: // downwards
				quad(node.barTextures[1], bx0, by0, bx1, by0 + fy, u0, v0, u1, v0 + fv, 0);
				break;
			case 2: // from the right
				quad(node.barTextures[1], bx1 - fx, by0, bx1, by1, u1 - fu, v0, u1, v1, 0);
				break;
			default: // from the left
				quad(node.barTextures[1], bx0, by0, bx0 + fx, by1, u0, v0, u0 + fu, v1, 0);
				break;
			}
		}
		break;
	}
	case kText:
		drawText(node, x, y, color, out);
		break;
	case kEdit: {
		// The text typed so far and the caret while it has the keys.
		const auto it = m_editText.find(node.editData);
		std::string text = it == m_editText.end() ? std::string() : it->second;
		if (m_editFocus == node.editData) {
			text += '_';
		}
		drawString(node.font, text, x, y, static_cast<float>(node.w), 0, color, out);
		break;
	}
	case kList:
		drawList(node, x, y, color, out);
		break;
	case kMap:
		drawMap(node, alpha, seconds, out);
		break;
	case kCompass:
		drawCompass(node, x, y, color, out);
		break;
	case kObjectMarker:
		drawMarkers(node, x, y, color, out);
		break;
	default:
		break;
	}
	// Splits pass their origin on; positioned nodes give their own (every
	// other kind: the commander screen's tab pictures place their buttons,
	// labels and lists, e.g. CommanderToolsTabSelected at 3,519 with its
	// button at 0,0).
	const bool positioned = node.type != kSplit || node.parent == nullptr;
	const size_t parentStart = m_parentStart;
	m_parentStart = out.size();
	for (Node* child : node.children) {
		draw(*child, positioned ? x : originX + moveX, positioned ? y : originY + moveY, alpha, tint, seconds, out);
	}
	m_parentStart = parentStart;
	if (node.blend[0] != 0 || node.blend[1] != 0) {
		for (size_t i = first; i < out.size(); ++i) {
			if (out[i].blend[0] == 0 && out[i].blend[1] == 0) {
				out[i].blend[0] = node.blend[0];
				out[i].blend[1] = node.blend[1];
			}
		}
	}
}

void HudManager::drawText(const Node& node, float x, float y, const float color[4], std::vector<HudQuad>& out)
{
	std::string text = node.textVariable.empty() ? node.text : string(node.textVariable);
	if (m_localize && !text.empty()) {
		text = m_localize(text);
	}
	if (node.truncate && !node.font.empty()) {
		// setTruncate: the text cut where it leaves the node.
		if (const HudFont* f = font(node.font)) {
			while (!text.empty() && f->width(text) > static_cast<float>(node.w)) {
				text.pop_back();
			}
		}
	}
	if (!node.outlineFont.empty()) {
		// setTextNodeOutLine: the outline font's glyphs beneath, moved by
		// the outline offset, in the text's alpha (black).
		const float shade[4] = {0, 0, 0, color[3]};
		drawString(node.outlineFont, text, x + node.outlineOffset[0], y + node.outlineOffset[1], static_cast<float>(node.w),
			node.alignment, shade, out);
	}
	drawString(node.font, text, x, y, static_cast<float>(node.w), node.alignment, color, out);
}

void HudManager::drawString(const std::string& fontPath, const std::string& text, float x, float y, float width,
	int alignment, const float color[4], std::vector<HudQuad>& out)
{
	if (text.empty() || fontPath.empty() || color[3] <= 0) {
		return;
	}
	const HudFont* f = font(fontPath);
	if (!f) {
		return;
	}
	const float textWidth = f->width(text);
	float pen = x;
	if (alignment == 1) {
		pen = x + width - textWidth;
	} else if (alignment == 2) {
		pen = x + (width - textWidth) * 0.5f;
	}
	const float tw = static_cast<float>(std::max(f->textureWidth, 1));
	const float th = static_cast<float>(std::max(f->textureHeight, 1));
	uint32_t previous = 0;
	for (unsigned char c : text) {
		const Glyph* g = f->glyph(c);
		if (!g) {
			continue;
		}
		pen += f->kern(previous, c);
		previous = c;
		const float gx = std::round(pen + g->before);
		const float gy = y + static_cast<float>(g->top);
		if (g->x1 > g->x0 && g->y1 > g->y0) {
			HudQuad q {f->name + ".dds", gx, gy, gx + static_cast<float>(g->x1 - g->x0), gy + static_cast<float>(g->y1 - g->y0),
				static_cast<float>(g->x0) / tw, static_cast<float>(g->y0) / th, static_cast<float>(g->x1) / tw,
				static_cast<float>(g->y1) / th, {color[0], color[1], color[2], color[3]}, 0};
			out.push_back(q);
		}
		pen += g->advance();
	}
}

namespace {

// Clips a quad to a rectangle as the exe's clip rectangle does (0x0077af80
// keeps the clip with the quad): an unrotated quad is cut, its texture
// coordinates with it; a rotated one carries the rectangle for the
// renderer. False when nothing is left.
bool clipTo(HudQuad& q, float x0, float y0, float x1, float y1)
{
	if (q.rotation != 0) {
		q.clipped = true;
		q.clip[0] = x0;
		q.clip[1] = y0;
		q.clip[2] = x1;
		q.clip[3] = y1;
		return true;
	}
	const float w = q.x1 - q.x0, h = q.y1 - q.y0;
	if (w <= 0 || h <= 0) {
		return false;
	}
	const float du = (q.u1 - q.u0) / w, dv = (q.v1 - q.v0) / h;
	if (q.x0 < x0) {
		q.u0 += (x0 - q.x0) * du;
		q.x0 = x0;
	}
	if (q.x1 > x1) {
		q.u1 -= (q.x1 - x1) * du;
		q.x1 = x1;
	}
	if (q.y0 < y0) {
		q.v0 += (y0 - q.y0) * dv;
		q.y0 = y0;
	}
	if (q.y1 > y1) {
		q.v1 -= (q.y1 - y1) * dv;
		q.y1 = y1;
	}
	return q.x1 > q.x0 && q.y1 > q.y0;
}

} // namespace

// Bf2CompassNode::render (0x007e9560). The strip texture (its size set by
// setCompassNodeTextureSize) is placed at the node's corner and slid back
// by c pixels, then cut to the node:
//   c = floor(value / 360 * (strip length - border) + offset)
// along y for direction 0 (the border's second value) and along x for
// direction 3 (its fourth). The exe's value is in radians and scaled by
// 57.29578 (0x00a0d138); our variables are degrees, the same product.
// Snapping nodes (the weapon HUDs' movement arrows) stop the strip where
// it would leave the node less the snap offsets and swap in the snap
// textures; tilted ones (setCompassNodeTiltValues 2|3 n) also slide
// sideways by c / strip height * n. Wrapping nodes (the map's compass)
// draw a second copy a period (strip length - border) to the side when
// the first leaves a gap.
void HudManager::drawCompass(const Node& node, float x, float y, const float color[4], std::vector<HudQuad>& out)
{
	if (color[3] <= 0) {
		return;
	}
	const float w = static_cast<float>(node.w), h = static_cast<float>(node.h);
	const float texW = node.compassTextureSize[0], texH = node.compassTextureSize[1];
	float value = node.compassVariable.empty() ? 0.0f : number(node.compassVariable);
	if (node.compassWrap) {
		// A heading, as the exe keeps them: in [-180, 180) (0x0078fc80
		// wraps the map angle by 2pi * (a / 2pi - floor(a / 2pi + 0.5))).
		value -= 360.0f * std::floor(value / 360.0f + 0.5f);
	}
	const float offset = static_cast<float>(node.compassOffset);
	std::string texture = node.compassTexture;
	float texX = x, texY = y, clipX = x;
	int c = 0;
	if (node.compassDirection == 0) {
		c = static_cast<int>(std::floor(value / 360.0f * (texH - static_cast<float>(node.compassBorder[1])) + offset));
		if (node.compassSnap) {
			const int o = node.compassOffset;
			if (static_cast<float>(c - o) > h - static_cast<float>(node.compassSnapOffset[1])) {
				c = static_cast<int>(std::floor(h - static_cast<float>(node.compassSnapOffset[1]) + offset));
				if (!node.compassSnapTextures[0].empty()) {
					texture = node.compassSnapTextures[0];
				}
			} else if (static_cast<float>(o - c) > h - static_cast<float>(node.compassSnapOffset[0])) {
				c = static_cast<int>(std::floor(offset - (h - static_cast<float>(node.compassSnapOffset[0]))));
				if (!node.compassSnapTextures[1].empty()) {
					texture = node.compassSnapTextures[1];
				}
			}
		}
		texY -= static_cast<float>(c);
	} else if (node.compassDirection == 3) {
		c = static_cast<int>(std::floor(value / 360.0f * (texW - static_cast<float>(node.compassBorder[3])) + offset));
		if (node.compassSnap) {
			const int o = node.compassOffset;
			const float high = w * 0.5f - static_cast<float>(node.compassSnapOffset[2]);
			const float low = w * 0.5f - static_cast<float>(node.compassSnapOffset[3]);
			if (static_cast<float>(c - o) > high) {
				c = static_cast<int>(std::floor(offset + high));
				if (!node.compassSnapTextures[0].empty()) {
					texture = node.compassSnapTextures[0];
				}
			} else if (static_cast<float>(o - c) > low) {
				c = static_cast<int>(std::floor(offset - low));
				if (!node.compassSnapTextures[1].empty()) {
					texture = node.compassSnapTextures[1];
				}
			}
		}
		texX -= static_cast<float>(c);
	}
	if (node.compassTilt > 0 && texH != 0 && (node.compassTiltMode == 2 || node.compassTiltMode == 3)) {
		const float tilt = static_cast<float>(c) / texH * node.compassTilt;
		const float shift = node.compassTiltMode == 2 ? -tilt : tilt;
		texX += shift;
		clipX += shift;
	}
	const auto place = [&](float at) {
		if (texture.empty()) {
			return;
		}
		HudQuad q {kTextureRoot + slashes(texture), at, texY, at + texW, texY + texH, 0, 0, 1, 1,
			{color[0], color[1], color[2], color[3]}, 0};
		if (clipTo(q, clipX, y, clipX + w, y + h)) {
			out.push_back(q);
		}
	};
	place(texX);
	if (node.compassWrap) {
		const float period = texW - static_cast<float>(node.compassBorder[3]);
		const float fc = static_cast<float>(c);
		if (c < 0 && offset - period * 0.5f <= fc) {
			place(texX - period);
		} else if (period - w < fc && fc < period - (period * 0.5f - offset)) {
			place(texX + period);
		}
	}
}

// Bf2ObjectMarkerNode (update 0x007e6880): the targets (0x007e6520, here
// from the game through setMarkerSource), each drawn with the texture and
// size of its state centred on its point (0x007e5a10 -> 0x007e5060), at
// most setObjectMarkerNodeObjects of them; with lock text on, the n-th
// target's label node goes to the point plus half the marker's size plus
// the label offset, rounded up, and the HUD variables <prefix><n>String
// (its distance, whole metres rounded up) and <prefix><n>Show (1) are set
// for it and <prefix><n>Show cleared for the rest of the 20 (0x007e56e0).
void HudManager::drawMarkers(Node& node, float x, float y, const float color[4], std::vector<HudQuad>& out)
{
	std::vector<MarkerTarget> targets;
	if (m_markerSource && color[3] > 0) {
		MarkerQuery query;
		query.lockOnType = node.lockOnType;
		query.weapon = node.markerWeapon;
		query.maxObjects = node.markerObjects;
		query.rangeCheck = node.markerRange;
		query.x0 = x;
		query.y0 = y;
		query.x1 = x + static_cast<float>(node.w);
		query.y1 = y + static_cast<float>(node.h);
		targets = m_markerSource(query);
	}
	if (node.markerObjects > 0 && targets.size() > static_cast<size_t>(node.markerObjects)) {
		targets.resize(static_cast<size_t>(node.markerObjects));
	}
	for (size_t i = 0; i < targets.size(); ++i) {
		const MarkerTarget& t = targets[i];
		const int state = std::clamp(t.state, 0, 3);
		const std::string& texture = node.markerTextures[state].empty() ? node.markerTextures[0] : node.markerTextures[state];
		const float* size = node.markerSizes[state][0] > 0 ? node.markerSizes[state] : node.markerSizes[0];
		if (!texture.empty() && size[0] > 0) {
			out.push_back({kTextureRoot + slashes(texture), t.x - size[0] * 0.5f, t.y - size[1] * 0.5f, t.x + size[0] * 0.5f,
				t.y + size[1] * 0.5f, 0, 0, 1, 1, {color[0], color[1], color[2], color[3]}, 0});
		}
		if (node.lockText && i < node.lockTextNodes.size()) {
			Node* label = node.lockTextNodes[i];
			label->x = static_cast<int>(std::ceil(t.x + size[0] * 0.5f + node.lockTextOffset[0]));
			label->y = static_cast<int>(std::ceil(t.y + size[1] * 0.5f + node.lockTextOffset[1]));
		}
		if (node.lockText) {
			const std::string n = node.lockTextPrefix + std::to_string(i);
			setString(n + "String", std::to_string(static_cast<int>(std::ceil(t.distance))));
			set(n + "Show", 1);
		}
	}
	if (node.lockText) {
		for (size_t i = targets.size(); i < 20; ++i) {
			set(node.lockTextPrefix + std::to_string(i) + "Show", 0);
		}
	}
}

bool HudManager::pointerQuad(Pointer kind, HudQuad& quad) const
{
	const std::string& texture = m_pointerTextures[static_cast<int>(kind)];
	if (texture.empty() || m_pointer[0] < 0 || m_pointer[1] < 0) {
		return false;
	}
	const float w = m_pointerSize[0], h = m_pointerSize[1];
	quad = {kTextureRoot + slashes(texture), m_pointer[0] - w * 0.5f, m_pointer[1] - h * 0.5f, m_pointer[0] + w * 0.5f,
		m_pointer[1] + h * 0.5f, 0, 0, 1, 1, {1, 1, 1, 1}, 0};
	return true;
}

void HudManager::togglePaintKit(int kit)
{
	if (kit >= 0 && kit < 8) {
		m_paintKits ^= 1u << kit;
	}
}

void HudManager::togglePaintVehicle(int kind)
{
	if (kind >= 0 && kind < 32) {
		m_paintVehicles ^= 1u << kind;
	}
}

void HudManager::paintAllKits() { m_paintKits = m_paintKits == 0xff ? 0 : 0xff; }
void HudManager::paintAllVehicles() { m_paintVehicles = m_paintVehicles == 0xffffffff ? 0 : 0xffffffff; }

void HudManager::setList(int data, std::vector<std::vector<std::string>> rows) { m_lists[data] = std::move(rows); }

std::string HudManager::listCell(int data, int row, int column) const
{
	const auto it = m_lists.find(data);
	if (it == m_lists.end() || row < 0 || row >= static_cast<int>(it->second.size()) || column < 0) {
		return {};
	}
	const auto& cells = it->second[static_cast<size_t>(row)];
	return column < static_cast<int>(cells.size()) ? cells[static_cast<size_t>(column)] : std::string();
}

void HudManager::setListColumns(int data, std::vector<ListColumn> columns) { m_listColumns[data] = std::move(columns); }

void HudManager::setListTextColor(int data, float r, float g, float b, float a) { m_listTextColors[data] = {r, g, b, a}; }

// Bf2NewListBoxNode::render (0x007de7d0), in order:
//  - the background (setListNodeBackgroundColor) over the whole node;
//  - with a border (setListNodeBorder top bottom left right), the border
//    colour's four bands: the top band across, the right one below it,
//    the bottom one up to the right band and the left one below the top;
//  - the rows, from top + row spacing down, a row height plus the row
//    spacing apart, while a row fits above the bottom border. The selected
//    row (selectable lists) gets the select colour across the node less
//    the side borders. Each cell's text goes at its column (left; centred
//    or right-aligned in the column, which ends at the next column or the
//    right border, 6 pixels in, 0x007dd800), centred in the row by its
//    glyphs' extent; the text stops at the glyph that leaves the column.
//    (Columns don't stop at the next column: that takes a flag of the
//    list data, 0x22, the game's lists don't set.)
//    "§n" (n 0..3) switches the font, "|c" toggles the highlight colour
//    (0.996, 0.639, 0.169). Outlined lists draw each glyph eight times one
//    pixel around in (0.15, 0.15, 0.15) first, and glyphs advance a pixel
//    more instead of their space before;
//  - with more rows than fit and a scroll bar (setListNodeScrollbar width
//    margin), the bar's track down the right inside the borders and its
//    thumb (the shown share of the track, at least 4 pixels).
void HudManager::drawList(const Node& node, float x, float y, const float color[4], std::vector<HudQuad>& out)
{
	const float w = static_cast<float>(node.w), h = static_cast<float>(node.h);
	const float top = static_cast<float>(node.listBorder[0]), bottom = static_cast<float>(node.listBorder[1]);
	const float left = static_cast<float>(node.listBorder[2]), right = static_cast<float>(node.listBorder[3]);
	const auto fill = [&](const float rgba[4], float x0, float y0, float x1, float y1) {
		if (rgba[3] * color[3] > 0 && x1 > x0 && y1 > y0) {
			out.push_back({"Menu/Console/white.dds", x0, y0, x1, y1, 0, 0, 1, 1,
				{rgba[0], rgba[1], rgba[2], rgba[3] * color[3]}, 0});
		}
	};
	// The header band (the background and the border above the rows) goes
	// beneath the nodes drawn before the list under the same parent: the
	// scoreboard's column headers are in a split node made before it.
	const size_t band = out.size();
	fill(node.listBackground, x, y, x + w, y + top);
	if (node.listBordered && node.listBorderColor[3] > 0) {
		fill(node.listBorderColor, x, y, x + w, y + top);
	}
	std::vector<HudQuad> header(out.begin() + static_cast<std::ptrdiff_t>(band), out.end());
	out.resize(band);
	out.insert(out.begin() + static_cast<std::ptrdiff_t>(std::min(m_parentStart, out.size())), header.begin(), header.end());
	fill(node.listBackground, x, y + top, x + w, y + h);
	if (node.listBordered && node.listBorderColor[3] > 0) {
		fill(node.listBorderColor, x + w - right, y + top, x + w, y + h);
		fill(node.listBorderColor, x, y + h - bottom, x + w - right, y + h);
		fill(node.listBorderColor, x, y + top, x + left, y + h);
	}
	const auto rowsIt = m_lists.find(node.listData);
	const auto columnsIt = m_listColumns.find(node.listData);
	if (rowsIt == m_lists.end() || columnsIt == m_listColumns.end()) {
		return;
	}
	const std::vector<std::vector<std::string>>& rows = rowsIt->second;
	const std::vector<ListColumn>& columns = columnsIt->second;
	float textColor[4] = {0, 0, 0, 1};
	if (const auto c = m_listTextColors.find(node.listData); c != m_listTextColors.end()) {
		std::copy(c->second.begin(), c->second.end(), textColor);
	}
	const auto selection = m_listSelection.find(node.listData);
	const int selected = selection == m_listSelection.end() ? -1 : selection->second;
	const bool scrollbar = node.scrollbarWidth > 0;
	// Where a column's text may go (0x007dd800): its x plus its width when
	// it has one, else the node's width less the right border; kept 6
	// pixels in from that edge (or clear of the scroll bar).
	const auto columnEnd = [&](size_t i) {
		const float inner = w - (node.listBordered ? right : 0);
		float end = inner;
		if (columns[i].width > 0 && columns[i].width < end) {
			end = columns[i].width + columns[i].x;
		}
		const float keep = scrollbar ? std::floor(node.scrollbarMargin + node.scrollbarWidth) : 6.0f;
		if (inner - keep < end) {
			end -= keep;
		}
		return end;
	};
	int shown = 0;
	float rowY = y + top + node.rowSpacing;
	const float lastRow = y + h - bottom - node.rowHeight;
	for (size_t r = 0; r < rows.size() && rowY <= lastRow; ++r, ++shown) {
		if (!node.listCommands.empty() && color[3] > 0) {
			ListHit hit {x + left, rowY, x + w - right, rowY + node.rowHeight, &node, static_cast<int>(r), {}};
			for (const ListColumn& column : columns) {
				hit.columns.push_back(x + column.x);
			}
			m_listHits.push_back(std::move(hit));
		}
		if (node.listSelectable && static_cast<int>(r) == selected) {
			fill(node.listSelectColor, x + left, rowY, x + w - right, rowY + node.rowHeight);
		}
		const auto rowColors = m_listRowColors.find(node.listData);
		const bool ownColor = rowColors != m_listRowColors.end() && r < rowColors->second.size();
		for (size_t i = 0; i < rows[r].size() && i < columns.size(); ++i) {
			if (columns[i].iconSize > 0) {
				if (!rows[r][i].empty()) {
					const float size = columns[i].iconSize;
					const float iy = std::floor(rowY + (node.rowHeight - size) * 0.5f);
					const float ix = x + columns[i].x;
					const float* tint = ownColor ? rowColors->second[r].data() : nullptr;
					out.push_back({kTextureRoot + slashes(rows[r][i]), ix, iy, ix + size, iy + size, 0, 0, 1, 1,
						{tint ? tint[0] : 1.0f, tint ? tint[1] : 1.0f, tint ? tint[2] : 1.0f, color[3]}, 0});
				}
				continue;
			}
			drawCell(node, rows[r][i], x, rowY, columns[i], columnEnd(i), ownColor ? rowColors->second[r].data() : textColor,
				color[3], out);
		}
		rowY += node.rowHeight + node.rowSpacing;
	}
	const int total = static_cast<int>(rows.size());
	if (scrollbar && shown < total && total > 0) {
		const float track = h - (top + bottom);
		float thumb = static_cast<float>(shown) / static_cast<float>(total) * (track - 2 * (node.rowHeight + node.rowSpacing));
		thumb = std::max(thumb, 4.0f);
		const float barX = x + w - (node.scrollbarMargin + node.scrollbarWidth);
		fill(node.listScrollbarBackground, barX, y + top, barX + node.scrollbarWidth, y + top + track);
		const float thumbY = y + top; // the first row shown is the first row: the thumb starts at the top
		fill(node.listScrollbarColor, barX, thumbY, barX + node.scrollbarWidth, thumbY + thumb);
	}
}

// One cell's text (0x007de7d0's glyph loops).
void HudManager::drawCell(const Node& node, const std::string& text, float x, float rowY, const ListColumn& column,
	float columnEnd, const float textColor[4], float alpha, std::vector<HudQuad>& out)
{
	const auto fontAt = [&](int index) -> const HudFont* {
		const std::string& path = node.listFonts[index].empty() ? node.listFonts[0] : node.listFonts[index];
		return path.empty() ? nullptr : font(path);
	};
	// The glyphs as drawn: font switches and colour toggles taken out,
	// measured until one leaves the column (0x007dd9a0: the column's room).
	struct Placed {
		const HudFont* font;
		const Glyph* glyph;
		bool highlight;
	};
	std::vector<Placed> glyphs;
	int fontIndex = 0;
	bool highlight = false;
	float width = 0, minTop = 10, maxBottom = 0;
	const float room = columnEnd - column.x;
	for (size_t i = 0; i < text.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		// "§" (0xa7, or UTF-8's 0xc2 0xa7) and a digit 0..3.
		size_t digit = i + 1;
		if (c == 0xc2 && digit < text.size() && static_cast<unsigned char>(text[digit]) == 0xa7) {
			++digit;
		}
		if ((c == 0xa7 || (c == 0xc2 && digit == i + 2)) && digit < text.size() && text[digit] >= '0' && text[digit] <= '3') {
			fontIndex = text[digit] - '0';
			i = digit;
			continue;
		}
		if (c == '|' && i + 1 < text.size() && (text[i + 1] == 'c' || text[i + 1] == 'C')) {
			highlight = !highlight;
			i += 1;
			continue;
		}
		const HudFont* f = fontAt(fontIndex);
		const Glyph* g = f ? f->glyph(c) : nullptr;
		if (!g) {
			continue;
		}
		glyphs.push_back({f, g, highlight});
		width += node.listOutline ? g->width + g->after + 1 : g->advance();
		minTop = std::min(minTop, static_cast<float>(g->top));
		maxBottom = std::max(maxBottom, static_cast<float>(g->top + (g->y1 - g->y0)));
		if (room < width) {
			break;
		}
	}
	if (glyphs.empty() || alpha <= 0) {
		return;
	}
	float pen = x + column.x;
	if (column.alignment == 2) {
		pen = x + column.x + (columnEnd - column.x) * 0.5f - width * 0.5f;
	} else if (column.alignment == 1) {
		pen = x + columnEnd - width;
	}
	// Measured with the outline's one pixel instead of the space before;
	// drawn with both (as the exe does).
	pen = std::floor(pen);
	// Centred in the row by the glyphs' extent (the cell's vertical
	// alignment 1, the default, 0x007ddd20).
	const float lineY = std::floor(rowY + (node.rowHeight - (maxBottom - minTop)) * 0.5f);
	for (const Placed& p : glyphs) {
		const Glyph& g = *p.glyph;
		const float tw = static_cast<float>(std::max(p.font->textureWidth, 1));
		const float th = static_cast<float>(std::max(p.font->textureHeight, 1));
		pen += g.before;
		const float gx = pen, gy = lineY + (static_cast<float>(g.top) - minTop);
		const float gw = static_cast<float>(g.x1 - g.x0), gh = static_cast<float>(g.y1 - g.y0);
		const auto glyph = [&](float dx, float dy, const float rgba[4]) {
			if (gw > 0 && gh > 0) {
				out.push_back({p.font->name + ".dds", gx + dx, gy + dy, gx + dx + gw, gy + dy + gh,
					static_cast<float>(g.x0) / tw, static_cast<float>(g.y0) / th, static_cast<float>(g.x1) / tw,
					static_cast<float>(g.y1) / th, {rgba[0], rgba[1], rgba[2], rgba[3]}, 0});
			}
		};
		const float shade[4] = {0.15f, 0.15f, 0.15f, textColor[3] * alpha};
		if (node.listOutline) {
			for (const auto& d : {std::pair {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}}) {
				glyph(static_cast<float>(d.first), static_cast<float>(d.second), shade);
			}
		}
		const float tint[4] = {p.highlight ? 0.996f : textColor[0], p.highlight ? 0.639f : textColor[1],
			p.highlight ? 0.169f : textColor[2], textColor[3] * alpha};
		glyph(0, 0, tint);
		pen += g.width + g.after + (node.listOutline ? 1 : 0);
	}
}

void HudManager::setMap(const std::string& texture, float originX, float originZ, float width, float depth)
{
	m_mapTexture = texture;
	m_mapOrigin[0] = originX;
	m_mapOrigin[1] = originZ;
	m_mapSize[0] = std::max(width, 1.0f);
	m_mapSize[1] = std::max(depth, 1.0f);
}

// A click: the topmost button drawn there runs its press (event 0) and
// release (event 3) commands (the alternate button its
// setButtonNodeAltConCmd ones). True when a button took it.
bool HudManager::click(float x, float y, bool alternate)
{
	// The mouse-over nodes under the pointer take the release (event 3).
	if (!alternate) {
		for (const MouseOverHit& hit : m_mouseOverHits) {
			if (x >= hit.x0 && x <= hit.x1 && y >= hit.y0 && y <= hit.y1) {
				runCommands(hit.node->mouseOverCommands, 3);
			}
		}
	}
	// A list's row (Bf2NewListBoxNode's click, 0x007dd3b0 reads its cells):
	// the row is selected, the column under the pointer kept (the list's
	// +0xc, which the squad menu switches on), and the list's commands run:
	// event 1, then 2 when the same row was clicked within half a second.
	for (auto it = m_listHits.rbegin(); !alternate && it != m_listHits.rend(); ++it) {
		if (x < it->x0 || x > it->x1 || y < it->y0 || y > it->y1) {
			continue;
		}
		const int data = it->node->listData;
		// The column whose left edge is the nearest one left of the pointer.
		int column = 0;
		for (size_t i = 0; i < it->columns.size(); ++i) {
			if (x >= it->columns[i] && it->columns[i] > it->columns[static_cast<size_t>(column)]) {
				column = static_cast<int>(i);
			}
		}
		const bool twice = m_listClick.data == data && m_listClick.row == it->row && m_clock - m_listClickTime < 0.5f;
		m_listClick = {data, it->row, column};
		m_listClickTime = m_clock;
		m_listSelection[data] = it->row;
		if (m_run) {
			for (int event : twice ? std::initializer_list<int> {1, 2} : std::initializer_list<int> {1}) {
				for (const auto& [when, command] : it->node->listCommands) {
					if (when == event) {
						m_run(command);
					}
				}
			}
		}
		return true;
	}
	for (auto it = m_buttonHits.rbegin(); it != m_buttonHits.rend(); ++it) {
		if (x < it->x0 || x > it->x1 || y < it->y0 || y > it->y1) {
			continue;
		}
		const auto& commands = alternate ? it->node->altCommands : it->node->buttonCommands;
		if (commands.empty()) {
			continue;
		}
		if (m_run) {
			for (int event : {0, 3}) {
				for (const auto& [when, command] : commands) {
					if (when == event) {
						m_run(command);
					}
				}
			}
		}
		return true;
	}
	return false;
}

int HudManager::mapMarkerAt(float x, float y) const
{
	for (auto it = m_mapHits.rbegin(); it != m_mapHits.rend(); ++it) {
		if (x >= it->x0 && x <= it->x1 && y >= it->y0 && y <= it->y1) {
			return it->id;
		}
	}
	return -1;
}

bool HudManager::mapWorldAt(float x, float y, float& worldX, float& worldZ) const
{
	const MapView& m = m_mapView;
	if (!m.drawn || x < m.x0 || x > m.x1 || y < m.y0 || y > m.y1) {
		return false;
	}
	const float u = (x - m.mapX) / m.mapW;
	const float v = (y - m.mapY) / m.mapH;
	worldX = m_mapOrigin[0] + u * m_mapSize[0];
	worldZ = m_mapOrigin[1] + (1 - v) * m_mapSize[1];
	return true;
}

void HudManager::setMapArea(float minX, float minZ, float maxX, float maxZ)
{
	m_mapAreaSet = true;
	m_mapArea[0] = minX;
	m_mapArea[1] = minZ;
	m_mapArea[2] = maxX;
	m_mapArea[3] = maxZ;
}

// Bf2MenuMapNode::render (0x00795620) with its zoom (0x0077b7b0,
// 0x0077a390; eased at 0x007901f0) and its shown area (0x0078b490).
//
// The node is the minimap's or the big map's rectangle (setMiniPos/Size,
// setMaxiPos/Size, from the screen's centre). The zoom is 2.3 to the power
// of a level, divided by the shown area's share of the map (so that at
// level 0 the shown area fills the node). The level eases to its target
// by 1 - e^(-6 dt) a frame, settling within 0.0001. The targets by the
// zoom index (1 at start, 0x00780xxx's 0x74c):
//   minimap on foot     0.5  1.5  2.5   (0x754)
//   minimap in vehicle  0    1    2     (0x760)
//   big map             0    1    2     (0x76c; the exe fits its third
//                                        to the camera's view, 0x007796c0)
// times 1, 2 and 2.3 (0x778) on the minimap when the camera is inside a
// Titan (RendDX9's ObjectDrawer flag, 0x100b9db0). The big map is drawn at
// index 0 (the exe's per-screen indices, 0x718, give 0 to the spawn and
// commander screens).
//
// The map's texture is the node's size times the zoom, placed with the
// focus at the node's centre, then pushed back so that the shown area
// covers the node; it is cut to the node and drawn white at the map's
// transparency (0x7b0, 1).
//
// Markers (0x0077c610) keep their pixel size at every zoom: a marker's
// rectangle is floor(its point - size / 2) by floor(size) (the exe takes
// a further half pixel off each for Direct3D 9's pixel centres), turned
// by its yaw and cut to the node.
void HudManager::drawMap(const Node& node, float alpha, float seconds, std::vector<HudQuad>& out)
{
	m_mapHits.clear();
	m_mapView.drawn = false;
	if (m_mapTexture.empty()) {
		return;
	}
	const bool full = number("MapFullSize") != 0;
	const float* pos = full ? node.maxiPos : node.miniPos;
	const float* size = full ? node.maxiSize : node.miniSize;
	const float nx = 400 + pos[0], ny = 300 + pos[1], nw = size[0], nh = size[1];
	// The zoom level.
	static const float kFoot[3] = {0.5f, 1.5f, 2.5f}, kVehicle[3] = {0, 1, 2}, kTitan[3] = {1, 2, 2.3f};
	const float target = full ? 0.0f
							  : (m_mapInVehicle ? kVehicle : kFoot)[m_mapZoomIndex] *
				(m_mapInsideTitan ? kTitan[m_mapZoomIndex] : 1.0f);
	if (!m_mapLevelSet) {
		m_mapLevel = target;
		m_mapLevelSet = true;
	} else if (std::abs(target - m_mapLevel) < 0.0001f) {
		m_mapLevel = target;
	} else {
		m_mapLevel += (1 - std::exp(-seconds * 6)) * (target - m_mapLevel);
	}
	// The shown area as parts of the map: the combat area's bounds in the
	// big map's pixels, widened by 20 pixels a side (when that leaves the
	// map, by half the overshoot, and the whole map if that's negative),
	// kept off the negative side, made square about its centre and kept
	// inside the map.
	float area[4] = {0, 0, 1, 1}; // offset x, y; size x, y
	if (m_mapAreaSet) {
		const float refX = node.maxiSize[0], refY = node.maxiSize[1];
		float minX = (m_mapArea[0] - m_mapOrigin[0]) / m_mapSize[0] * refX;
		float minY = (1 - (m_mapArea[3] - m_mapOrigin[1]) / m_mapSize[1]) * refY;
		float sizeX = (m_mapArea[2] - m_mapArea[0]) / m_mapSize[0] * refX;
		float sizeY = (m_mapArea[3] - m_mapArea[1]) / m_mapSize[1] * refY;
		float margin = 20; // 0xc38
		if (refX < minX + sizeX + margin) {
			margin = (minX + sizeX - refX) / 2;
		}
		if (refY < minY + sizeY + margin) {
			margin = (minY + sizeY - refY) / 2;
		}
		if (margin >= 0) {
			sizeX += margin * 2;
			sizeY += margin * 2;
			minX -= margin;
			minY -= margin;
		} else {
			sizeX = refX;
			sizeY = refY;
			minX = minY = 0;
		}
		minX = std::max(minX, 0.0f);
		minY = std::max(minY, 0.0f);
		if (sizeX <= sizeY) {
			float d = sizeY - sizeX;
			sizeX = sizeY;
			if (refX < minX - d / 2 + sizeX) {
				d = (minX + sizeX - refX) * 2;
			}
			minX -= d / 2;
		} else {
			float d = sizeX - sizeY;
			sizeY = sizeX;
			if (refY < minY - d / 2 + sizeY) {
				d = (minY + sizeY - refY) * 2;
			}
			minY -= d / 2;
		}
		area[0] = minX / refX;
		area[1] = minY / refY;
		area[2] = sizeX / refX;
		area[3] = sizeY / refY;
	}
	const float zoom = std::pow(2.3f, m_mapLevel);
	// Where the whole map goes.
	float mapW = nw * zoom / area[2], mapH = nh * zoom / area[3];
	const float fu = (m_mapFocus[0] - m_mapOrigin[0]) / m_mapSize[0];
	const float fv = 1 - (m_mapFocus[1] - m_mapOrigin[1]) / m_mapSize[1];
	float mapX = nx + nw / 2 - fu * mapW;
	float mapY = ny + nh / 2 - fv * mapH;
	const float offX = area[0] * mapW, offY = area[1] * mapH;
	const float shownW = area[2] * mapW, shownH = area[3] * mapH;
	if (nx < mapX + offX) {
		mapX = nx - offX;
	}
	mapW = std::max(mapW, nw);
	if (ny < mapY + offY) {
		mapY = ny - offY;
	}
	mapH = std::max(mapH, nh);
	if (mapX + offX + shownW < nx + nw) {
		mapX = nx + nw - (offX + shownW);
	}
	if (mapY + offY + shownH < ny + nh) {
		mapY = ny + nh - (offY + shownH);
	}
	HudQuad map {m_mapTexture, mapX, mapY, mapX + mapW, mapY + mapH, 0, 0, 1, 1, {1, 1, 1, alpha}, 0};
	if (clipTo(map, nx, ny, nx + nw, ny + nh)) {
		out.push_back(map);
	}
	m_mapView = {true, nx, ny, nx + nw, ny + nh, mapX, mapY, mapW, mapH};
	for (const MapMarker& marker : m_mapMarkers) {
		const float u = (marker.x - m_mapOrigin[0]) / m_mapSize[0];
		const float v = 1 - (marker.z - m_mapOrigin[1]) / m_mapSize[1];
		const float sx = mapX + u * mapW, sy = mapY + v * mapH;
		const float side = std::floor(marker.size);
		const float x0 = std::floor(sx - marker.size * 0.5f), y0 = std::floor(sy - marker.size * 0.5f);
		if (x0 + side <= nx || x0 >= nx + nw || y0 + side <= ny || y0 >= ny + nh) {
			continue;
		}
		HudQuad q {kTextureRoot + slashes(marker.texture), x0, y0, x0 + side, y0 + side, 0, 0, 1, 1,
			{marker.color[0], marker.color[1], marker.color[2], marker.color[3] * alpha}, marker.yaw};
		if (!clipTo(q, nx, ny, nx + nw, ny + nh)) {
			continue;
		}
		out.push_back(q);
		if (marker.id >= 0) {
			m_mapHits.push_back({std::max(x0, nx), std::max(y0, ny), std::min(x0 + side, nx + nw), std::min(y0 + side, ny + nh),
				marker.id});
		}
	}
}

void registerHudConsoleMethods(Console& console, HudManager& hud)
{
	const uint32_t access = ConsoleAccess::Anyone;
	HudManager* m = &hud;
	using Node = HudManager::Node;
	const auto active = [m]() { return m->active(); };
	const auto create = [&console, m](const char* method, int type) {
		bindMethod(console, std::string("HudBuilder.") + method, access,
			[m, type](std::string parent, std::string name, int x, int y, int w, int h) {
				return m->create(type, parent, name, x, y, w, h) != nullptr;
			});
	};
	// The profile's crosshair colour (GeneralOptions.con; 0..255 each, as
	// 0x0049bb20 divides them for the HUD; alpha stays opaque).
	bindMethod(console, "game.setCrossHairColor", access, [m](float r, float g, float b) {
		m->set("CrossHairColorRed", std::clamp(r / 255.0f, 0.0f, 1.0f));
		m->set("CrossHairColorGreen", std::clamp(g / 255.0f, 0.0f, 1.0f));
		m->set("CrossHairColorBlue", std::clamp(b / 255.0f, 0.0f, 1.0f));
		m->set("CrossHairColorAlpha", 1);
	});
	bindMethod(console, "HudBuilder.createSplitNode", access,
		[m](std::string parent, std::string name) { return m->create(kSplit, parent, name, 0, 0, 0, 0) != nullptr; });
	create("createTransformNode", kTransform);
	create("createPictureNode", kPicture);
	create("createTextNode", kText);
	create("createButtonNode", kButton);
	create("createObjectMarkerNode", kObjectMarker);
	create("createMouseOverNode", kMouseOver);
	create("createAnimationNode", kOther);
	create("createHoverNode", kOther);
	create("createObjectSelectionNode", kOther);
	create("createTileNode", kOther);
	create("createEditNode", kEdit);
	bindMethod(console, "HudBuilder.setEditNodeFont", access, [active](std::string font, int) {
		if (Node* n = active()) {
			n->font = font;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setEditNodeData", access, [active](int data) {
		if (Node* n = active()) {
			n->editData = data;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setEditNodeString", access, [](int) { return true; }); // the string slot: the data's
	bindMethod(console, "HudBuilder.setEditNodeMaxLength", access, [m, active](int length) {
		if (Node* n = active()) {
			n->editMaxLength = length;
			m->setEditMaxLength(n->editData, length);
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setEditNodeColor", access, [active](float r, float g, float b, float a) {
		if (Node* n = active()) {
			n->color[0] = r;
			n->color[1] = g;
			n->color[2] = b;
			n->color[3] = a;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.createBarNode", access,
		[m](std::string parent, std::string name, int type, int x, int y, int w, int h) {
			Node* node = m->create(kBar, parent, name, x, y, w, h);
			node->barType = type;
			return true;
		});
	// createTransformListNode <parent> <name> x y w h <spacing> (0x007b2b50).
	bindMethod(console, "HudBuilder.createTransformListNode", access,
		[m](std::string parent, std::string name, int x, int y, int w, int h, int spacing) {
			Node* n = m->create(kTransformList, parent, name, x, y, w, h);
			n->listSpacing = spacing;
			return true;
		});
	bindMethod(console, "HudBuilder.createOccupiedNode", access,
		[m](std::string parent, std::string name, int seat, int x, int y, int w, int h) {
			Node* n = m->create(kOccupied, parent, name, x, y, w, h);
			n->occupiedSeat = seat;
			return true;
		});
	bindMethod(console, "HudBuilder.createCompassNode", access,
		[m](std::string parent, std::string name, int direction, int x, int y, int w, int h, bool wrap, bool snap) {
			Node* n = m->create(kCompass, parent, name, x, y, w, h);
			if (n) {
				n->compassDirection = direction;
				n->compassWrap = wrap;
				n->compassSnap = snap;
			}
			return n != nullptr;
		});
	bindMethod(console, "HudBuilder.createListNode", access,
		[m](std::string parent, std::string name, int x, int y, int w, int h, int rowHeight, bool selectable) {
			Node* n = m->create(kList, parent, name, x, y, w, h);
			if (n) {
				n->rowHeight = static_cast<float>(std::max(rowHeight, 1));
				n->listSelectable = selectable;
			}
			return n != nullptr;
		});
	bindMethod(console, "HudBuilder.createMapNode", access, [m](std::string parent, std::string name) {
		return m->create(kMap, parent, name, 0, 0, 800, 600) != nullptr;
	});
	const auto mapSetting = [&console, active](const char* method, int which) {
		bindMethod(console, std::string("HudBuilder.") + method, access, [active, which](std::string value) {
			Node* n = active();
			float a = 0, b = 0;
			if (!n || n->type != kMap || std::sscanf(value.c_str(), "%f/%f", &a, &b) != 2) {
				return false;
			}
			float* target[] = {n->miniPos, n->miniSize, n->maxiPos, n->maxiSize};
			target[which][0] = a;
			target[which][1] = b;
			return true;
		});
	};
	mapSetting("setMiniPos", 0);
	mapSetting("setMiniSize", 1);
	mapSetting("setMaxiPos", 2);
	mapSetting("setMaxiSize", 3);
	// 0x007ea4a0: texture 1 is the strip (the other index is kept by the
	// bar node beneath and not drawn by the compass).
	bindMethod(console, "HudBuilder.setCompassNodeTexture", access, [active](int index, std::string texture) {
		if (Node* n = active(); n && index == 1) {
			n->compassTexture = texture;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setCompassNodeBorder", access, [active](int a, int b, int c, int d) {
		if (Node* n = active()) {
			n->compassBorder[0] = a;
			n->compassBorder[1] = b;
			n->compassBorder[2] = c;
			n->compassBorder[3] = d;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setCompassNodeSnapOffset", access, [active](int a, int b, int c, int d) {
		if (Node* n = active()) {
			n->compassSnapOffset[0] = a;
			n->compassSnapOffset[1] = b;
			n->compassSnapOffset[2] = c;
			n->compassSnapOffset[3] = d;
		}
		return true;
	});
	// 0x007e94c0: true is the texture past the high end, false past the low.
	bindMethod(console, "HudBuilder.setCompassNodeSnapTexture", access, [active](bool high, std::string texture) {
		if (Node* n = active()) {
			n->compassSnapTextures[high ? 0 : 1] = texture;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setCompassNodeTiltValues", access, [active](int mode, int amount) {
		if (Node* n = active()) {
			n->compassTiltMode = mode;
			n->compassTilt = static_cast<float>(amount);
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setCompassNodeTextureSize", access, [active](float w, float h) {
		if (Node* n = active()) {
			n->compassTextureSize[0] = w;
			n->compassTextureSize[1] = h;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setCompassNodeValueVariable", access, [active](std::string variable) {
		if (Node* n = active()) {
			n->compassVariable = variable;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setCompassNodeOffset", access, [active](int offset) {
		if (Node* n = active()) {
			n->compassOffset = offset;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setListNodeBorder", access, [active](int a, int b, int c, int d) {
		if (Node* n = active()) {
			n->listBordered = true; // 0x007add90
			n->listBorder[0] = a;
			n->listBorder[1] = b;
			n->listBorder[2] = c;
			n->listBorder[3] = d;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setListNodeData", access, [active](int data) {
		if (Node* n = active()) {
			n->listData = data;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setListNodeConCmd", access, [active](int event, std::string command) {
		if (Node* n = active()) {
			n->listCommands.emplace_back(event, command);
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setListNodeFont", access, [active](std::string font, int index) {
		if (Node* n = active(); n && index >= 0 && index < 4) {
			n->listFonts[index] = font;
		}
		return true;
	});
	const auto colorSetter = [&console, active](const char* method, int which) {
		bindMethod(console, std::string("HudBuilder.") + method, access, [active, which](float r, float g, float b, float a) {
			if (Node* n = active()) {
				float* target[] = {n->listBackground, n->listSelectColor, n->listBorderColor, n->listScrollbarColor,
					n->listScrollbarBackground};
				target[which][0] = r;
				target[which][1] = g;
				target[which][2] = b;
				target[which][3] = a;
			}
			return true;
		});
	};
	colorSetter("setListNodeBackgroundColor", 0);
	colorSetter("setListNodeSelectColor", 1);
	colorSetter("setListNodeBorderColor", 2);
	colorSetter("setListNodeScrollbarColor", 3);
	colorSetter("setListNodeScrollbarBackgroundColor", 4);
	bindMethod(console, "HudBuilder.setListNodeScrollbar", access, [active](int width, int margin) {
		if (Node* n = active()) {
			n->scrollbarWidth = static_cast<float>(width);
			n->scrollbarMargin = static_cast<float>(margin);
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setListNodeRowSpacing", access, [active](int spacing) {
		if (Node* n = active()) {
			n->rowSpacing = static_cast<float>(spacing);
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setListNodeOutline", access, [active](bool outline) {
		if (Node* n = active()) {
			n->listOutline = outline;
		}
		return true;
	});
	// setActiveObject <parent> <node>: later calls change that node. The
	// exe's text nodes sit below a transform node of their name, the text
	// itself named "<name>TextNode" (the object markers' labels set their
	// variables on those); here one node is both.
	bindMethod(console, "HudBuilder.setActiveObject", access, [m](std::string, std::string name) {
		Node* n = m->find(name);
		const std::string suffix = "textnode";
		const std::string low = lower(name);
		if (!n && low.size() > suffix.size() && low.compare(low.size() - suffix.size(), suffix.size(), suffix) == 0) {
			n = m->find(name.substr(0, name.size() - suffix.size()));
		}
		m->setActive(n);
		return m->active() != nullptr;
	});

	bindMethod(console, "HudBuilder.setNodeColor", access, [active](float r, float g, float b, float a) {
		if (Node* n = active()) {
			n->color[0] = r;
			n->color[1] = g;
			n->color[2] = b;
			n->color[3] = a;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setNodeShowVariable", access, [active](std::string variable) {
		if (Node* n = active()) {
			n->showVariable = variable;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setNodeLogicShowVariable", access,
		[active](std::string op, std::string variable, float value) {
			if (Node* n = active()) {
				std::string o = op;
				for (char& c : o) {
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				n->logic.push_back({o, variable, value});
			}
			return true;
		});
	bindMethod(console, "HudBuilder.setNodeAlphaVariable", access, [active](std::string variable) {
		if (Node* n = active()) {
			n->alphaVariable = variable;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setNodeRGBVariables", access, [active](std::string r, std::string g, std::string b) {
		if (Node* n = active()) {
			n->rgbVariables[0] = r;
			n->rgbVariables[1] = g;
			n->rgbVariables[2] = b;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setNodePosVariable", access, [active](int axis, std::string variable) {
		if (Node* n = active(); n && (axis == 0 || axis == 1)) {
			n->posVariables[axis] = variable; // 0: x, 1: y
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setNodeOffset", access, [active](int x, int y) {
		if (Node* n = active()) {
			n->x += x;
			n->y += y;
			n->offset[0] += x;
			n->offset[1] += y;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setNodeInTime", access, [active](float t) {
		if (Node* n = active()) {
			n->inTime = t;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setNodeOutTime", access, [active](float t) {
		if (Node* n = active()) {
			n->outTime = t;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.addNodeAlphaShowEffect", access, [active]() {
		if (Node* n = active()) {
			n->alphaShowEffect = true;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setPictureNodeTexture", access, [active](std::string texture) {
		if (Node* n = active()) {
			n->texture = texture;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setPictureNodeVariableTexture", access, [active](std::string variable) {
		if (Node* n = active()) {
			n->textureVariable = variable;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setPictureNodeRotateVariable", access, [active](std::string variable) {
		if (Node* n = active()) {
			n->rotateVariable = variable;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setPictureNodeRotation", access, [active](int degrees) {
		if (Node* n = active()) {
			n->rotation = static_cast<float>(degrees);
		}
		return true;
	});
	m->setCommandRunner([&console](const std::string& line) {
		std::string output;
		console.execute(line, output);
	});
	bindMethod(console, "HudBuilder.setButtonNodeConCmd", access, [active](std::string command, int event) {
		if (Node* n = active()) {
			n->buttonCommands.emplace_back(event, command);
		}
		return true;
	});
	// What buttons run: closing a screen, setting HUD items, the scoreboard's tabs.
	bindMethod(console, "hudManager.setDone", access, [m](int done) { m->set("HudDone", static_cast<float>(done)); });
	bindMethod(console, "hudItems.setBool", access, [m](std::string name, int value) {
		m->set(name, value != 0 ? 1.0f : 0.0f);
	});
	bindMethod(console, "hudItems.setFloat", access, [m](std::string name, float value) { m->set(name, value); });
	bindMethod(console, "hudItems.setString", access, [m](std::string name, std::string value) { m->setString(name, value); });
	bindMethod(console, "scoreboard.setToggleShow", access, [m](int tab) {
		const char* tabs[] = {"ToggleScore", "ToggleSquads", "ToggleManage", "ToggleServer"};
		for (int i = 0; i < 4; ++i) {
			m->set(tabs[i], i == tab ? 1.0f : 0.0f);
		}
	});
	bindMethod(console, "HudBuilder.setButtonNodeTexture", access, [active](int index, std::string texture) {
		if (Node* n = active(); n && index >= 0 && index < 4) {
			n->buttonTextures[index] = texture;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setTextNodeString", access, [active](std::string text) {
		if (Node* n = active()) {
			n->text = text;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setTextNodeStringVariable", access, [active](std::string variable) {
		if (Node* n = active()) {
			n->textVariable = variable;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setTextNodeStyle", access, [active](std::string font, int alignment) {
		if (Node* n = active()) {
			n->font = font;
			n->alignment = alignment;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setBarNodeTexture", access, [active](int index, std::string texture) {
		if (Node* n = active(); n && (index == 0 || index == 1)) {
			n->barTextures[index] = texture;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setBarNodeValueVariable", access, [active](std::string variable) {
		if (Node* n = active()) {
			n->valueVariable = variable;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setBarNodeSnap", access, [active](int snap) {
		if (Node* n = active()) {
			n->snap = snap;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setBarNodeInverse", access, [active](bool inverse) {
		if (Node* n = active()) {
			n->inverse = inverse;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setBarNodeSpanValues", access, [active](float low, float high) {
		if (Node* n = active()) {
			n->spanMin = low;
			n->spanMax = high;
		}
		return true;
	});

	// --- Transform lists (Bf2TransformListNode) ---
	// addTransformListNode <node> (0x007aed80 -> slot 0xd4, 0x007ebde0).
	bindMethod(console, "HudBuilder.addTransformListNode", access, [m, active](std::string name) {
		Node* list = active();
		Node* item = m->find(name);
		if (!list || list->type != kTransformList || !item) {
			return false;
		}
		list->listItems.push_back(item);
		return true;
	});
	// setTranformListNodePosVariable <axis> <variable> (0x007aedf0 -> slot
	// 0xd0, 0x007eb9d0: 0 x, 1 y).
	bindMethod(console, "HudBuilder.setTranformListNodePosVariable", access, [active](int axis, std::string variable) {
		Node* n = active();
		if (!n || n->type != kTransformList || axis < 0 || axis > 1 || variable.empty()) {
			return false;
		}
		n->listPosVariables[axis] = variable;
		return true;
	});
	// setTranformListNodeOffset x y (0x007ae270 -> slot 0xcc, 0x007ebd60).
	bindMethod(console, "HudBuilder.setTranformListNodeOffset", access, [active](int x, int y) {
		Node* n = active();
		if (!n || n->type != kTransformList) {
			return false;
		}
		n->listOffset[0] = x;
		n->listOffset[1] = y;
		return true;
	});

	// --- Show effects ---
	bindMethod(console, "HudBuilder.addNodeMoveShowEffect", access, [active](float angle, int distance) {
		if (Node* n = active()) {
			n->moveEffect = true;
			n->moveAngle = angle;
			n->moveDistance = static_cast<float>(distance);
		}
		return true;
	});
	bindMethod(console, "HudBuilder.addNodeBlendEffect", access, [active](int source, int destination) {
		if (Node* n = active()) {
			n->blend[0] = source;
			n->blend[1] = destination;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setNodeTimesBlink", access, [active](int times, int) {
		if (Node* n = active()) {
			n->blinkTimes = times;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setNodeIntervalBlink", access, [active](float seconds) {
		if (Node* n = active()) {
			n->blinkInterval = seconds;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setNodeShowConCmd", access, [m, active](std::string command, int event) {
		Node* n = active();
		if (!n || event < 0 || event > 1 || command.empty()) {
			return false;
		}
		n->showCommands.emplace_back(event, command);
		m->noteShowCommands(n);
		return true;
	});
	// setNodeSize w h (0x007b0a10: the node made last, slot 0xc8).
	bindMethod(console, "HudBuilder.setNodeSize", access, [active](int w, int h) {
		if (Node* n = active()) {
			n->w = w;
			n->h = h;
		}
		return true;
	});

	// --- Buttons and mouse-over nodes ---
	bindMethod(console, "HudBuilder.setButtonNodeMouseArea", access, [active](int x, int y, int w, int h) {
		Node* n = active();
		if (!n || n->type != kButton) {
			return false;
		}
		n->mouseArea = true;
		n->area[0] = x;
		n->area[1] = y;
		n->area[2] = w;
		n->area[3] = h;
		return true;
	});
	bindMethod(console, "HudBuilder.setButtonNodeAltConCmd", access, [active](std::string command, int event) {
		Node* n = active();
		if (!n || n->type != kButton || event < 0 || event > 3) {
			return false;
		}
		n->altCommands.emplace_back(event, command);
		return true;
	});
	bindMethod(console, "HudBuilder.setButtonNodeDebug", access, [active](bool debug) {
		Node* n = active();
		if (!n || n->type != kButton) {
			return false;
		}
		n->buttonDebug = debug;
		return true;
	});
	bindMethod(console, "HudBuilder.setMouseOverNodeConCmd", access, [active](std::string command, int event) {
		Node* n = active();
		if (!n || n->type != kMouseOver || event < 0 || event > 3) {
			return false;
		}
		n->mouseOverCommands.emplace_back(event, command);
		return true;
	});

	// --- Pictures, text, bars ---
	bindMethod(console, "HudBuilder.setPictureNodeBorder", access, [active](int left, int top, int right, int bottom) {
		if (Node* n = active()) {
			n->pictureBorder[0] = left;
			n->pictureBorder[1] = top;
			n->pictureBorder[2] = right;
			n->pictureBorder[3] = bottom;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setPictureNodeBorderColor", access, [active](float r, float g, float b, float a) {
		if (Node* n = active()) {
			n->pictureBorderColor[0] = r;
			n->pictureBorderColor[1] = g;
			n->pictureBorderColor[2] = b;
			n->pictureBorderColor[3] = a;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setPictureNodeCenterPoint", access, [active](int x, int y) {
		if (Node* n = active()) {
			n->centerSet = true;
			n->center[0] = x;
			n->center[1] = y;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setTextNodeOutLine", access, [active](std::string font) {
		Node* n = active();
		if (!n || n->type != kText) {
			return false;
		}
		n->outlineFont = font;
		return true;
	});
	bindMethod(console, "HudBuilder.setTextNodeOutLineOffset", access, [active](float x, float y) {
		Node* n = active();
		if (!n || n->type != kText) {
			return false;
		}
		n->outlineOffset[0] = x;
		n->outlineOffset[1] = y;
		return true;
	});
	bindMethod(console, "HudBuilder.setTruncate", access, [active](bool truncate) {
		Node* n = active();
		if (!n || n->type != kText) {
			return false;
		}
		n->truncate = truncate;
		return true;
	});
	bindMethod(console, "HudBuilder.setBarNodeBorder", access, [active](int a, int b, int c, int d) {
		Node* n = active();
		if (!n || (n->type != kBar && n->type != kCompass)) {
			return false;
		}
		n->barBorder[0] = a;
		n->barBorder[1] = b;
		n->barBorder[2] = c;
		n->barBorder[3] = d;
		return true;
	});
	bindMethod(console, "HudBuilder.setBarNodeIncreaseShow", access, [active](int on) {
		if (Node* n = active()) {
			n->barIncreaseShow = on != 0;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setBarNodeSnapDir", access, [active](bool on) {
		if (Node* n = active()) {
			n->barSnapDir = on;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setBarNodeRound", access, [active](int mode, float w, float h) {
		if (Node* n = active()) {
			n->barRound = mode;
			n->barRoundSize[0] = w;
			n->barRoundSize[1] = h;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setOccupiedNodeData", access, [active](int data) {
		Node* n = active();
		if (!n || n->type != kOccupied) {
			return false;
		}
		n->occupiedData = data;
		return true;
	});
	bindMethod(console, "HudBuilder.setOccupiedNodePosVariable", access, [active](int axis, std::string variable) {
		Node* n = active();
		if (!n || n->type != kOccupied || axis < 0 || axis > 1 || variable.empty()) {
			return false;
		}
		n->posVariables[axis] = variable;
		return true;
	});

	// --- Object markers (0x007af210..0x007af440 -> Bf2ObjectMarkerNode's slots) ---
	const auto marker = [active]() -> Node* {
		Node* n = active();
		return n && n->type == kObjectMarker ? n : nullptr;
	};
	bindMethod(console, "HudBuilder.setObjectMarkerNodeTexture", access, [marker](int index, std::string texture) {
		Node* n = marker();
		if (!n || index < 0 || index >= 4) {
			return false;
		}
		n->markerTextures[index] = texture;
		return true;
	});
	bindMethod(console, "HudBuilder.setObjectMarkerNodeTextureSize", access, [marker](int index, int w, int h) {
		Node* n = marker();
		if (!n || index < 0 || index >= 4) {
			return false;
		}
		n->markerSizes[index][0] = static_cast<float>(w);
		n->markerSizes[index][1] = static_cast<float>(h);
		return true;
	});
	bindMethod(console, "HudBuilder.setObjectMarkerNodeObjects", access, [marker](int count) {
		Node* n = marker();
		if (!n || count < 0) {
			return false;
		}
		n->markerObjects = count;
		return true;
	});
	bindMethod(console, "HudBuilder.setObjectMarkerNodeLockOnType", access, [marker](int type) {
		Node* n = marker();
		if (!n) {
			return false;
		}
		n->lockOnType = type;
		return true;
	});
	bindMethod(console, "HudBuilder.setObjectMarkerNodeWeapon", access, [marker](int weapon) {
		Node* n = marker();
		if (!n || weapon < 0) {
			return false;
		}
		n->markerWeapon = weapon;
		return true;
	});
	bindMethod(console, "HudBuilder.setObjectMarkerNodeRC", access, [marker](int on) {
		Node* n = marker();
		if (!n) {
			return false;
		}
		n->markerRange = on != 0;
		return true;
	});
	bindMethod(console, "HudBuilder.setObjectMarkerNodeLockText", access, [marker](bool on, std::string prefix) {
		Node* n = marker();
		if (!n) {
			return false;
		}
		n->lockText = on;
		n->lockTextPrefix = prefix;
		return true;
	});
	bindMethod(console, "HudBuilder.setObjectMarkerNodeLockTextOffset", access, [marker](int x, int y) {
		Node* n = marker();
		if (!n) {
			return false;
		}
		n->lockTextOffset[0] = static_cast<float>(x);
		n->lockTextOffset[1] = static_cast<float>(y);
		return true;
	});
	bindMethod(console, "HudBuilder.addObjectMarkerNodeLockTextNode", access, [m, marker](std::string name) {
		Node* n = marker();
		Node* label = m->find(name);
		if (!n || !label) {
			return false;
		}
		n->lockTextNodes.push_back(label);
		m->setActive(n); // the marker stays the node being built
		return true;
	});

	// --- The maps' and selection nodes' settings, kept ---
	bindMethod(console, "HudBuilder.createMiniMapNode", access,
		[m](std::string parent, std::string name, int x, int y, int w, int h) {
			return m->create(kOther, parent, name, x, y, w, h) != nullptr;
		});
	const auto keep = [&console, m](const char* method) {
		bindMethod(console, std::string("HudBuilder.") + method, access, [m, method](std::string value) {
			float a = 0, b = 0;
			std::sscanf(value.c_str(), "%f/%f", &a, &b);
			m->setSetting(std::string("HudBuilder.") + method, {a, b, 0, 0});
			return true;
		});
	};
	keep("setCommanderSize");
	keep("setCommanderPos");
	bindMethod(console, "HudBuilder.setZoomIcons", access, [m](bool on) {
		m->setSetting("HudBuilder.setZoomIcons", {on ? 1.0f : 0.0f, 0, 0, 0});
		return true;
	});
	bindMethod(console, "HudBuilder.setCPFont", access, [m](std::string font) {
		m->setSetting("HudBuilder.setCPFont", {});
		m->setString("HudBuilder.CPFont", font);
		return true;
	});
	bindMethod(console, "HudBuilder.setCPFontColor", access, [m](float r, float g, float b, float a) {
		m->setSetting("HudBuilder.setCPFontColor", {r, g, b, a});
		return true;
	});
	bindMethod(console, "HudBuilder.setAlphaMask", access, [m](std::string texture) {
		m->setString("HudBuilder.AlphaMask", texture);
		return true;
	});
	bindMethod(console, "HudBuilder.setObjectSelectionNodePointerSize", access, [m](int w, int h) {
		m->setSetting("HudBuilder.setObjectSelectionNodePointerSize", {static_cast<float>(w), static_cast<float>(h), 0, 0});
		return true;
	});
	// newLayer (0x007ad480): the builder's offset (+0xc, +0x10) back to 0.
	bindMethod(console, "HudBuilder.newLayer", access, [] { return true; });

	// --- HudManager's settings (the HUD manager's slots from 0x0075e732) ---
	using Pointer = HudManager::Pointer;
	for (const auto& [method, kind] : std::vector<std::pair<std::string, Pointer>> {{"setMouseTextureEmpty", Pointer::Empty},
			 {"setMouseTextureFull", Pointer::Full}, {"setMouseTextureArtillery", Pointer::Artillery},
			 {"setMouseTextureUAV", Pointer::Uav}, {"setMouseTextureSupply", Pointer::Supply},
			 {"setMouseTextureMoveTitan", Pointer::MoveTitan}, {"setMouseTextureOrbitalStrike", Pointer::OrbitalStrike}}) {
		const Pointer which = kind;
		bindMethod(console, "HudManager." + method, access, [m, which](std::string texture) {
			m->setPointerTexture(which, texture);
			return true;
		});
	}
	bindMethod(console, "HudManager.setPointerSize", access, [m](int w, int h) {
		m->setPointerSize(static_cast<float>(w), static_cast<float>(h));
		return true;
	});
	for (const char* method : {"setCommSize", "setCommPos", "setCommMousePos", "setSpottedMenuSize", "setSpottedMenuPos",
			 "setSpottedMousePos"}) {
		const std::string name = method;
		bindMethod(console, "HudManager." + name, access, [m, name](int a, int b) {
			m->setSetting("HudManager." + name, {static_cast<float>(a), static_cast<float>(b), 0, 0});
			return true;
		});
	}
	for (const char* method : {"setCommMouseSensitivity", "setPointerMouseSensitivity"}) {
		const std::string name = method;
		bindMethod(console, "HudManager." + name, access, [m, name](int value) {
			m->setSetting("HudManager." + name, {static_cast<float>(value), 0, 0, 0});
			return true;
		});
	}
	for (const char* method : {"setMaximumNrOfCPs"}) {
		const std::string name = method;
		bindMethod(console, "HudManager." + name, access, [m, name](int value) {
			m->setSetting("HudManager." + name, {static_cast<float>(value), 0, 0, 0});
		});
	}
	for (const char* method : {"setSpottedAngle", "setTargetTop"}) {
		const std::string name = method;
		bindMethod(console, "HudManager." + name, access, [m, name](float value) {
			m->setSetting("HudManager." + name, {value, 0, 0, 0});
		});
	}
	for (const char* method : {"setPaint", "setUpdate", "setDisplayControlpoints", "setDisplayTickets", "setMapStatic"}) {
		const std::string name = method;
		bindMethod(console, "HudManager." + name, access, [m, name](bool value) {
			m->setSetting("HudManager." + name, {value ? 1.0f : 0.0f, 0, 0, 0});
		});
	}
	bindMethod(console, "HudManager.addTextureAtlas", access, [m](std::string atlas) { m->setString("HudManager.TextureAtlas", atlas); });

	// --- Minimap (0x0076f773..0x00770017) ---
	bindMethod(console, "Minimap.setZoom", access, [m] { m->cycleMapZoom(); });
	bindMethod(console, "Minimap.setPaintKit", access, [m](int kit) { m->togglePaintKit(kit); });
	bindMethod(console, "Minimap.setPaintAllKits", access, [m] { m->paintAllKits(); });
	bindMethod(console, "Minimap.setPaintVehicle", access, [m](int kind) { m->togglePaintVehicle(kind); });
	bindMethod(console, "Minimap.setPaintAllVehicles", access, [m] { m->paintAllVehicles(); });
	bindMethod(console, "Minimap.setCurrOrderList", access, [m](int list) {
		m->setSetting("Minimap.setCurrOrderList", {static_cast<float>(list), 0, 0, 0});
	});
	bindMethod(console, "Minimap.setFullScreenZoom", access, [m](int zoom) {
		m->setSetting("Minimap.setFullScreenZoom", {static_cast<float>(zoom), 0, 0, 0});
	});
	bindMethod(console, "Minimap.toggleShowKits", access, [m] {
		m->setSetting("Minimap.showKits", {m->setting("Minimap.showKits")[0] != 0 ? 0.0f : 1.0f, 0, 0, 0});
	});
	bindMethod(console, "Minimap.setCommanderOffset", access, [m](std::string value) {
		float a = 0, b = 0;
		std::sscanf(value.c_str(), "%f/%f", &a, &b);
		m->setSetting("Minimap.setCommanderOffset", {a, b, 0, 0});
	});
	bindMethod(console, "Minimap.setSourceBlend", access, [m](int blend) {
		m->setSetting("Minimap.setSourceBlend", {static_cast<float>(blend), 0, 0, 0});
	});
	bindMethod(console, "Minimap.setDestinationBlend", access, [m](int blend) {
		m->setSetting("Minimap.setDestinationBlend", {static_cast<float>(blend), 0, 0, 0});
	});
	bindMethod(console, "Minimap.setFilterColor", access, [m](float r, float g, float b, float a) {
		m->setSetting("Minimap.setFilterColor", {r, g, b, a});
	});
}

} // namespace dice::hfe::hud
