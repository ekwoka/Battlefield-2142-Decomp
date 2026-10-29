// The player's display objects, which are also the objects scripts see
// (gameswf's character, which derives from as_object): movie clips, text
// fields, buttons, and the shapes and morph shapes that only draw.
//
// A clip's display list maps depth to child. Timeline placements use the
// tag's depth (1..16383); script depths d are stored as d + 16384, so
// getDepth() of a timeline instance is negative as in Flash, and the
// timeline only manages instances below 16384.
//
// Member lookup on a display object, in order: its own properties (what
// scripts stored), then (clips) a child of that instance name, then the
// display properties (_x, _alpha, _parent, ...; text fields: text,
// textColor, ...), then the prototype chain (MovieClip.prototype or the
// registered class).
#pragma once

#include "Draw.h"
#include "DisplayList.h"

#include "UI/Avm1/Interpreter.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::ui {

class Player;

namespace player {

constexpr int kDepthOffset = 16384;

enum class Kind : uint8_t { Shape, MorphShape, Text, Button, Clip };

class Clip;

class DisplayObject : public avm1::Object {
public:
	DisplayObject(Player& player, Kind kind, uint16_t character) : player(player), kind(kind), character(character) {}

	Player& player;
	const Kind kind;
	uint16_t character = 0; // 0: made by script, or the root
	int depth = 0;          // display list key (see above)
	Clip* parent = nullptr; // null for the root and once removed
	swf::Matrix matrix;
	swf::ColorTransform colour;
	float ratio = 0;
	std::string name;
	uint16_t clipDepth = 0; // a mask layer over depths up to this (drawn into the stencil, Stencil.h)
	bool visible = true;
	bool placedByTimeline = false;
	bool scriptTransformed = false; // script set _x & co.: timeline moves no longer apply
	bool removed = false;
	bool isMask = false; // the argument of some setMask(): not drawn itself
	// DICE's _isScroller (sprite property 0x19, +0xbf): drawn as the
	// credits roll instead of itself (0x100dfe77, Scroller.h).
	bool isScroller = false;
	uint64_t bornTick = 0;
	std::vector<ClipEvent> events;

	// The last _xscale/_yscale/_rotation set or read, so that values
	// survive the matrix round trip (Flash keeps them too).
	bool decomposed = false;
	double xscale = 100, yscale = 100, rotation = 0;

	// --- Script side
	bool getMember(avm1::Interpreter& vm, const std::string& name, avm1::Value& out) override;
	void setMember(avm1::Interpreter& vm, const std::string& name, const avm1::Value& value) override;
	bool hasOwnMember(const std::string& name) const override;
	const char* typeName() const override { return kind == Kind::Clip ? "movieclip" : "object"; }
	bool defaultValue(avm1::Interpreter& vm, bool preferString, avm1::Value& out) override;

	// GetProperty/SetProperty by ClipProperty index.
	avm1::Value property(int index);
	void setProperty(avm1::Interpreter& vm, int index, const avm1::Value& value);
	// Members beyond the indexed properties (_parent, _root, text...).
	virtual bool specialMember(avm1::Interpreter& vm, const std::string& name, avm1::Value& out);
	virtual bool setSpecialMember(avm1::Interpreter& vm, const std::string& name, const avm1::Value& value);

	// --- Geometry (twips)
	swf::Matrix worldMatrix() const;
	// Bounds in this object's own space; false when empty.
	virtual bool localBounds(swf::Rect& out);
	// Bounds in the space `space` maps to (e.g. the parent's: matrix).
	bool boundsIn(const swf::Matrix& space, swf::Rect& out);
	// Does the stage point (twips) touch this object? shape: exact
	// outlines, else the bounds. Invisible objects only count when
	// `includeHidden`.
	bool hitTest(swf::Point stage, bool shape, bool includeHidden);
	// The same for a point in this object's own space.
	virtual bool hitLocal(swf::Point local, bool shape, bool includeHidden);
	virtual void draw(Drawer& drawer, const swf::Matrix& world, const swf::ColorTransform& colour, std::vector<MenuBatch>& out);
	// Draws this object as a mask (into the stencil): as draw(), though it
	// is a mask layer or a setMask() argument.
	void drawMask(Drawer& drawer, const swf::Matrix& world, std::vector<MenuBatch>& out);

	std::string targetPath() const; // "_level0.a.b"
	std::string slashPath() const;  // "/a/b"
	Clip* root();
	void setScale(double xs, double ys, double rot);
	void decompose();
	bool isAncestorOf(const DisplayObject* other) const;
};

class TextField : public DisplayObject {
public:
	TextField(Player& player, uint16_t character, const swf::EditText& definition);

	swf::EditText field; // the definition with what scripts changed
	std::string value;   // what the field holds (HTML source when html)
	std::string variable;
	std::string lastVariableText; // what the bound variable held at the last sync
	bool input = false;  // type "input"
	bool selectable = true;
	bool background = false;
	swf::Rgba backgroundColour {255, 255, 255, 255};
	swf::Rgba borderColour {0, 0, 0, 255};
	std::string autoSize = "none";
	int maxChars = 0;
	int scroll = 1;
	size_t caret = 0; // in UTF-8 bytes

	std::string plainText() const;
	// Sets the text the way scripts do (and the bound variable).
	void setText(avm1::Interpreter& vm, const std::string& text, bool html);
	void applyAutoSize();
	float textWidth() const;  // pixels, the widest line
	int textHeight() const;   // pixels, as SwiffPlayer.dll reports it (0x100c2a60)
	float firstLineWidth() const; // pixels: Locale.getTextboxWidth (0x1003a9d0)

	bool hasOwnMember(const std::string& name) const override;
	bool specialMember(avm1::Interpreter& vm, const std::string& name, avm1::Value& out) override;
	bool setSpecialMember(avm1::Interpreter& vm, const std::string& name, const avm1::Value& value) override;
	bool localBounds(swf::Rect& out) override;
	bool hitLocal(swf::Point local, bool shape, bool includeHidden) override;
	void draw(Drawer& drawer, const swf::Matrix& world, const swf::ColorTransform& colour, std::vector<MenuBatch>& out) override;
};

class ButtonObject : public DisplayObject {
public:
	ButtonObject(Player& player, uint16_t character, const swf::Button& button);

	const swf::Button& button;
	uint8_t state = swf::kButtonUp;
	// One object per record (record i at parts[i]), made when the button is.
	std::vector<std::shared_ptr<DisplayObject>> parts;

	bool localBounds(swf::Rect& out) override;
	bool hitLocal(swf::Point local, bool shape, bool includeHidden) override;
	void draw(Drawer& drawer, const swf::Matrix& world, const swf::ColorTransform& colour, std::vector<MenuBatch>& out) override;
	void clearReferences() override;
};

class Clip : public DisplayObject {
public:
	Clip(Player& player, uint16_t character, const Timeline* timeline) : DisplayObject(player, Kind::Clip, character), timeline(timeline) {}
	~Clip() override;

	const Timeline* timeline; // never null (empty clips share a one-frame timeline)
	int frame = -1;           // 0-based, -1 before the first
	bool playing = true;
	std::map<int, std::shared_ptr<DisplayObject>> children;

	// loadMovie() of an image: drawn at the origin instead of the timeline.
	std::shared_ptr<const MenuImage> image;
	std::shared_ptr<VideoSource> video; // a movie's pictures become `image`
	std::string url;
	size_t bytes = 0;

	// The drawing API (moveTo, lineTo, beginFill...).
	swf::Shape drawing;
	swf::ShapeMesh drawingMesh;
	bool drawingDirty = false;
	uint32_t drawingFill = 0, drawingLine = 0; // current 1-based styles
	swf::Point pen;
	avm1::ObjectRef mask; // setMask(): the clip shows only where this object draws (Stencil.h)

	int frameCount() const { return static_cast<int>(timeline->frames.size()); }
	DisplayObject* childNamed(const std::string& name) const;
	std::shared_ptr<DisplayObject> childAt(int depth) const;
	int highestDepth() const; // the display list's highest key, or -1

	bool getMember(avm1::Interpreter& vm, const std::string& name, avm1::Value& out) override;
	bool hasOwnMember(const std::string& name) const override;
	bool localBounds(swf::Rect& out) override;
	bool hitLocal(swf::Point local, bool shape, bool includeHidden) override;
	void draw(Drawer& drawer, const swf::Matrix& world, const swf::ColorTransform& colour, std::vector<MenuBatch>& out) override;
	void clearReferences() override;
};

// Union of two rects (an empty first one is replaced).
void unite(swf::Rect& into, bool& any, const swf::Rect& r);
// The axis-aligned box of `r` under `m`.
swf::Rect transformRect(const swf::Matrix& m, const swf::Rect& r);

} // namespace player
} // namespace dice::hfe::ui
