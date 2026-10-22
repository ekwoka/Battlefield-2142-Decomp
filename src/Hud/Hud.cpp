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
	// Bars: textures 0 (empty) and 1 (full), a value variable 0..1.
	int barType = 3;
	std::string barTextures[2];
	std::string valueVariable;
	int snap = 0;
	bool inverse = false;
	float spanMin = 0, spanMax = 1;
	// Lists: rows from the game's data source (setListNodeData).
	int listData = -1;
	int rowHeight = 19;
	std::string listFonts[2];
	float listBackground[4] = {0, 0, 0, 0};
	int listBorder[4] = {0, 0, 0, 0}; // setListNodeBorder: the first is the header band above the rows
	// Maps: where the mini and the big map go (from the screen's centre).
	float miniPos[2] = {245, -291}, miniSize[2] = {146, 146};
	float maxiPos[2] = {-122, -271}, maxiSize[2] = {512, 512};
	// Compasses: a strip texture scrolled by a value variable (degrees).
	std::string compassTexture;
	float compassTextureSize[2] = {0, 0};
	std::string compassVariable;
	float compassOffset = 0;
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
	if (Node* parent = parentName.empty() ? nullptr : find(parentName)) {
		if (node->parent != parent) {
			parent->children.push_back(node);
			node->parent = parent;
		}
	}
	m_active = node;
	return node;
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
	for (m_pass = 0; m_pass < 2; ++m_pass) {
		m_numbers["affectedbyempshow"] = static_cast<float>(m_pass);
		m_numbers["updateallenabled"] = static_cast<float>(1 - m_pass);
		for (Node* root : m_roots) {
			draw(*root, 0, 0, 1, nullptr, seconds, quads);
		}
	}
	m_numbers["affectedbyempshow"] = 0;
	return quads;
}

void HudManager::draw(Node& node, float originX, float originY, float alpha, const float* tint, float seconds,
	std::vector<HudQuad>& out)
{
	const bool show = visible(node);
	float& shown = node.shown[m_pass];
	if (!node.started[m_pass]) {
		node.started[m_pass] = true;
		shown = show ? 1.0f : 0.0f;
	} else if (node.alphaShowEffect && (node.inTime > 0 || node.outTime > 0)) {
		const float time = show ? node.inTime : node.outTime;
		const float step = time > 0 ? seconds / time : 1.0f;
		shown = std::clamp(shown + (show ? step : -step), 0.0f, 1.0f);
	} else {
		shown = show ? 1.0f : 0.0f;
	}
	if (shown <= 0) {
		return;
	}
	alpha *= shown;
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
	// (Until the game sets one the node keeps its own.)
	for (int axis = 0; axis < 2; ++axis) {
		if (!node.posVariables[axis].empty()) {
			const auto it = m_numbers.find(lower(node.posVariables[axis]));
			if (it != m_numbers.end()) {
				(axis == 0 ? x : y) = (axis == 0 ? originX : originY) + it->second;
			}
		}
	}
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
	switch (node.type) {
	case kPicture: {
		std::string texture = node.textureVariable.empty() ? node.texture : string(node.textureVariable);
		const float rotation = node.rotation + (node.rotateVariable.empty() ? 0 : number(node.rotateVariable));
		quad(texture, x, y, x + w, y + h, 0, 0, 1, 1, rotation);
		break;
	}
	case kButton: {
		const bool over = m_pointer[0] >= x && m_pointer[0] <= x + w && m_pointer[1] >= y && m_pointer[1] <= y + h;
		const std::string& normal = node.buttonTextures[1].empty() ? node.buttonTextures[0] : node.buttonTextures[1];
		quad(over && !node.buttonTextures[2].empty() ? node.buttonTextures[2] : normal, x, y, x + w, y + h, 0, 0, 1, 1, 0);
		if (!node.buttonCommands.empty() && color[3] > 0) {
			m_buttonHits.push_back({x, y, x + w, y + h, &node});
		}
		break;
	}
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
		if (value > 0) {
			switch (node.barType) {
			case 0: // filling upwards
				quad(node.barTextures[1], x, y + h * (1 - value), x + w, y + h, 0, 1 - value, 1, 1, 0);
				break;
			case 1: // downwards
				quad(node.barTextures[1], x, y, x + w, y + h * value, 0, 0, 1, value, 0);
				break;
			case 2: // from the right
				quad(node.barTextures[1], x + w * (1 - value), y, x + w, y + h, 1 - value, 0, 1, 1, 0);
				break;
			default: // from the left
				quad(node.barTextures[1], x, y, x + w * value, y + h, 0, 0, value, 1, 0);
				break;
			}
		}
		break;
	}
	case kText:
		drawText(node, x, y, color, out);
		break;
	case kList:
		drawList(node, x, y, color, out);
		break;
	case kMap:
		drawMap(node, alpha, out);
		break;
	case kCompass: {
		// Provisional (RendDX9.dll draws it): the strip covers 360 degrees,
		// centred on the value, offset by setCompassNodeOffset pixels; a
		// window as wide as the node shows, in two pieces where it wraps.
		const float stripWidth = node.compassTextureSize[0] > 0 ? node.compassTextureSize[0] : w;
		const float span = std::min(w / stripWidth, 1.0f);
		float u0 = number(node.compassVariable) / 360.0f + node.compassOffset / stripWidth - span * 0.5f;
		u0 -= std::floor(u0);
		const float u1 = u0 + span;
		if (u1 <= 1) {
			quad(node.compassTexture, x, y, x + w, y + h, u0, 0, u1, 1, 0);
		} else {
			const float split = x + w * (1 - u0) / span;
			quad(node.compassTexture, x, y, split, y + h, u0, 0, 1, 1, 0);
			quad(node.compassTexture, split, y, x + w, y + h, 0, 0, u1 - 1, 1, 0);
		}
		break;
	}
	default:
		break;
	}
	// Splits pass their origin on; positioned nodes give their own.
	const bool positioned = node.type == kTransform || node.type == kTransformList || node.parent == nullptr;
	const size_t parentStart = m_parentStart;
	m_parentStart = out.size();
	for (Node* child : node.children) {
		draw(*child, positioned ? x : originX, positioned ? y : originY, alpha, tint, seconds, out);
	}
	m_parentStart = parentStart;
}

void HudManager::drawText(const Node& node, float x, float y, const float color[4], std::vector<HudQuad>& out)
{
	std::string text = node.textVariable.empty() ? node.text : string(node.textVariable);
	if (m_localize && !text.empty()) {
		text = m_localize(text);
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

void HudManager::setList(int data, std::vector<std::vector<std::string>> rows) { m_lists[data] = std::move(rows); }

void HudManager::setListColumns(int data, std::vector<ListColumn> columns) { m_listColumns[data] = std::move(columns); }

// A list: its background below the header band (the border's first value;
// the scripts put column headers there), then a row per entry of its data
// source, each cell at its column (the first cell in font 0, the others in
// font 1). Provisional: the list's drawing is RendDX9.dll's, not decompiled.
void HudManager::drawList(const Node& node, float x, float y, const float color[4], std::vector<HudQuad>& out)
{
	const float w = static_cast<float>(node.w);
	const float header = static_cast<float>(std::clamp(node.listBorder[0], 0, node.h));
	const float h = static_cast<float>(node.h) - header;
	if (node.listBackground[3] > 0) {
		const float fill[4] = {node.listBackground[0], node.listBackground[1], node.listBackground[2],
			node.listBackground[3] * color[3]};
		// The header band goes beneath the nodes drawn before the list under
		// the same parent (its column headers).
		out.insert(out.begin() + static_cast<std::ptrdiff_t>(std::min(m_parentStart, out.size())),
			{"Menu/Console/white.dds", x, y, x + w, y + header, 0, 0, 1, 1, {fill[0], fill[1], fill[2], fill[3]}, 0});
		out.push_back({"Menu/Console/white.dds", x, y + header, x + w, y + header + h, 0, 0, 1, 1,
			{fill[0], fill[1], fill[2], fill[3]}, 0});
	}
	y += header;
	const auto rows = m_lists.find(node.listData);
	const auto columns = m_listColumns.find(node.listData);
	if (rows == m_lists.end() || columns == m_listColumns.end()) {
		return;
	}
	const float text[4] = {0, 0, 0, color[3]};
	float rowY = y + 3;
	for (const std::vector<std::string>& row : rows->second) {
		if (rowY + static_cast<float>(node.rowHeight) > y + h) {
			break;
		}
		for (size_t i = 0; i < row.size() && i < columns->second.size(); ++i) {
			const ListColumn& column = columns->second[i];
			const std::string& fontPath = node.listFonts[i == 0 ? 0 : 1].empty() ? node.listFonts[0] : node.listFonts[i == 0 ? 0 : 1];
			drawString(fontPath, row[i], x + column.x, rowY, column.width, column.alignment, text, out);
		}
		rowY += static_cast<float>(node.rowHeight);
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
// release (event 3) commands. True when a button took it.
bool HudManager::click(float x, float y)
{
	for (auto it = m_buttonHits.rbegin(); it != m_buttonHits.rend(); ++it) {
		if (x < it->x0 || x > it->x1 || y < it->y0 || y > it->y1) {
			continue;
		}
		if (m_run) {
			for (int event : {0, 3}) {
				for (const auto& [when, command] : it->node->buttonCommands) {
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
	const float u = m.u0 + (x - m.x0) / (m.x1 - m.x0) * m.span;
	const float v = m.v0 + (y - m.y0) / (m.y1 - m.y0) * m.span;
	worldX = m_mapOrigin[0] + u * m_mapSize[0];
	worldZ = m_mapOrigin[1] + (1 - v) * m_mapSize[1];
	return true;
}

// The map (provisional: RendDX9.dll draws it): the texture, then the
// markers inside it. The minimap shows a third of the level round the
// focus, kept inside the map.
void HudManager::drawMap(const Node& node, float alpha, std::vector<HudQuad>& out)
{
	m_mapHits.clear();
	m_mapView.drawn = false;
	if (m_mapTexture.empty()) {
		return;
	}
	const bool full = number("MapFullSize") != 0;
	const float* pos = full ? node.maxiPos : node.miniPos;
	const float* size = full ? node.maxiSize : node.miniSize;
	const float x0 = 400 + pos[0], y0 = 300 + pos[1], x1 = x0 + size[0], y1 = y0 + size[1];
	// The texture window: u east, v south.
	float span = full ? 1.0f : 1.0f / 3.0f;
	float u0 = 0, v0 = 0;
	if (!full) {
		const float cu = (m_mapFocus[0] - m_mapOrigin[0]) / m_mapSize[0];
		const float cv = 1 - (m_mapFocus[1] - m_mapOrigin[1]) / m_mapSize[1];
		u0 = std::clamp(cu - span / 2, 0.0f, 1 - span);
		v0 = std::clamp(cv - span / 2, 0.0f, 1 - span);
	}
	out.push_back({m_mapTexture, x0, y0, x1, y1, u0, v0, u0 + span, v0 + span, {1, 1, 1, alpha}, 0});
	m_mapView = {true, x0, y0, x1, y1, u0, v0, span};
	for (const MapMarker& marker : m_mapMarkers) {
		const float u = (marker.x - m_mapOrigin[0]) / m_mapSize[0];
		const float v = 1 - (marker.z - m_mapOrigin[1]) / m_mapSize[1];
		const float sx = x0 + (u - u0) / span * (x1 - x0), sy = y0 + (v - v0) / span * (y1 - y0);
		if (sx < x0 || sx > x1 || sy < y0 || sy > y1) {
			continue;
		}
		const float half = marker.size / 2;
		out.push_back({kTextureRoot + slashes(marker.texture), sx - half, sy - half, sx + half, sy + half, 0, 0, 1, 1,
			{marker.color[0], marker.color[1], marker.color[2], marker.color[3] * alpha}, marker.yaw});
		if (marker.id >= 0) {
			m_mapHits.push_back({sx - half, sy - half, sx + half, sy + half, marker.id});
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
	create("createMouseOverNode", kOther);
	create("createAnimationNode", kOther);
	create("createHoverNode", kOther);
	create("createObjectSelectionNode", kOther);
	create("createTileNode", kOther);
	create("createEditNode", kOther);
	bindMethod(console, "HudBuilder.createBarNode", access,
		[m](std::string parent, std::string name, int type, int x, int y, int w, int h) {
			Node* node = m->create(kBar, parent, name, x, y, w, h);
			node->barType = type;
			return true;
		});
	bindMethod(console, "HudBuilder.createTransformListNode", access,
		[m](std::string parent, std::string name, int, int x, int y, int w, int h) {
			return m->create(kTransformList, parent, name, x, y, w, h) != nullptr;
		});
	bindMethod(console, "HudBuilder.createOccupiedNode", access,
		[m](std::string parent, std::string name, int, int x, int y, int w, int h) {
			return m->create(kOther, parent, name, x, y, w, h) != nullptr;
		});
	bindMethod(console, "HudBuilder.createCompassNode", access,
		[m](std::string parent, std::string name, int, int x, int y, int w, int h, bool, bool) {
			return m->create(kCompass, parent, name, x, y, w, h) != nullptr;
		});
	bindMethod(console, "HudBuilder.createListNode", access,
		[m](std::string parent, std::string name, int x, int y, int w, int h, int rowHeight, bool) {
			Node* n = m->create(kList, parent, name, x, y, w, h);
			if (n) {
				n->rowHeight = std::max(rowHeight, 1);
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
	bindMethod(console, "HudBuilder.setCompassNodeTexture", access, [active](int, std::string texture) {
		if (Node* n = active()) {
			n->compassTexture = texture;
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
	bindMethod(console, "HudBuilder.setCompassNodeOffset", access, [active](float offset) {
		if (Node* n = active()) {
			n->compassOffset = offset;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setListNodeBorder", access, [active](int a, int b, int c, int d) {
		if (Node* n = active()) {
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
	bindMethod(console, "HudBuilder.setListNodeFont", access, [active](std::string font, int index) {
		if (Node* n = active(); n && index >= 0 && index < 2) {
			n->listFonts[index] = font;
		}
		return true;
	});
	bindMethod(console, "HudBuilder.setListNodeBackgroundColor", access, [active](float r, float g, float b, float a) {
		if (Node* n = active()) {
			n->listBackground[0] = r;
			n->listBackground[1] = g;
			n->listBackground[2] = b;
			n->listBackground[3] = a;
		}
		return true;
	});
	// setActiveObject <parent> <node>: later calls change that node.
	bindMethod(console, "HudBuilder.setActiveObject", access, [m](std::string, std::string name) {
		m->setActive(m->find(name));
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
}

} // namespace dice::hfe::hud
