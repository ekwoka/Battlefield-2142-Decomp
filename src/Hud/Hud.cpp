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
		drawMap(node, alpha, seconds, out);
		break;
	case kCompass:
		drawCompass(node, x, y, color, out);
		break;
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

void HudManager::setList(int data, std::vector<std::vector<std::string>> rows) { m_lists[data] = std::move(rows); }

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
		if (node.listSelectable && static_cast<int>(r) == selected) {
			fill(node.listSelectColor, x + left, rowY, x + w - right, rowY + node.rowHeight);
		}
		for (size_t i = 0; i < rows[r].size() && i < columns.size(); ++i) {
			drawCell(node, rows[r][i], x, rowY, columns[i], columnEnd(i), textColor, color[3], out);
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
