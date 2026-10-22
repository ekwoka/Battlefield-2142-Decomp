// A movie's timelines and display lists (gameswf's sprite_instance and
// display_list). The root movie and every DefineSprite has a timeline: its
// tags split into frames at ShowFrame. A movie clip plays one: entering a
// frame runs its control tags (PlaceObject2, RemoveObject2, FrameLabel)
// against the clip's display list, a map from depth to instance (a
// character placed with a matrix, colour transform, morph ratio, name and
// mask depth), and queues the frame's actions for the interpreter.
//
// Going back replays the timeline from the first frame, as Flash does;
// instances placed by script (or whose character stayed) keep their
// state. Masks (clip depths) are not applied here: mask layers are not
// drawn and what they mask shows whole.
//
// This MovieClip is the script-less player the drawing tests use; the
// full player (Player.h) shares Timeline and parsePlaceObject with it and
// keeps its own display objects (Stage.h).
#pragma once

#include "UI/Characters/CharacterDictionary.h"
#include "UI/MenuDraw.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::ui {

class Drawer;

// A run of action bytes in the movie buffer (DoAction, a clip event, a
// button condition).
struct ActionBlock {
	size_t offset = 0;
	size_t length = 0;
};

// PlaceObject2 clip actions: the event bits (SWF 6+: 32 bits) and code.
struct ClipEvent {
	uint32_t events = 0; // kClipEvent* bits
	uint8_t keyCode = 0;
	ActionBlock actions;
};
enum ClipEventFlag : uint32_t {
	kClipEventKeyUp = 1u << 7,
	kClipEventKeyDown = 1u << 6,
	kClipEventMouseUp = 1u << 5,
	kClipEventMouseDown = 1u << 4,
	kClipEventMouseMove = 1u << 3,
	kClipEventUnload = 1u << 2,
	kClipEventEnterFrame = 1u << 1,
	kClipEventLoad = 1u << 0,
	kClipEventDragOver = 1u << 15,
	kClipEventRollOut = 1u << 14,
	kClipEventRollOver = 1u << 13,
	kClipEventReleaseOutside = 1u << 12,
	kClipEventRelease = 1u << 11,
	kClipEventPress = 1u << 10,
	kClipEventInitialize = 1u << 9,
	kClipEventData = 1u << 8,
	kClipEventConstruct = 1u << 18,
	kClipEventKeyPress = 1u << 17,
	kClipEventDragOut = 1u << 16,
};

// A PlaceObject/PlaceObject2 tag: what it sets (the has* flags) and the
// values.
struct PlaceRecord {
	uint16_t depth = 0;
	bool move = false;         // PlaceObject2's "move" flag: modify the existing instance
	bool hasCharacter = false;
	uint16_t character = 0;
	bool hasMatrix = false;
	swf::Matrix matrix;
	bool hasColour = false;
	swf::ColorTransform colour;
	bool hasRatio = false;
	float ratio = 0;           // 0..1
	bool hasName = false;
	std::string name;
	bool hasClipDepth = false;
	uint16_t clipDepth = 0;
	bool hasEvents = false;
	std::vector<ClipEvent> events;
};

// Reads a PlaceObject (4) or PlaceObject2 (26) tag of `swf`.
PlaceRecord parsePlaceObject(const SwfTag& tag, const std::vector<uint8_t>& swf);
// The depth a RemoveObject (5) or RemoveObject2 (28) tag removes.
uint16_t removedDepth(const SwfTag& tag, const std::vector<uint8_t>& swf);

struct Frame {
	std::vector<SwfTag> controls; // PlaceObject2, RemoveObject2, FrameLabel... in order
	std::vector<ActionBlock> actions;  // DoAction
	std::string label;
};

struct Timeline {
	std::vector<Frame> frames;
	std::map<std::string, int> labels; // name -> 0-based frame
	static Timeline fromTags(const std::vector<SwfTag>& tags, const std::vector<uint8_t>& swf);
};

class MovieClip;

struct Instance {
	uint16_t character = 0;
	uint16_t depth = 0;
	swf::Matrix matrix;
	swf::ColorTransform colour;
	float ratio = 0;       // morph shapes 0..1
	std::string name;
	uint16_t clipDepth = 0; // a mask over depths up to this
	std::vector<ClipEvent> events;
	bool visible = true;
	std::string text;      // text fields: what they show
	uint8_t buttonState = swf::kButtonUp;
	std::unique_ptr<MovieClip> clip; // sprites: their own timeline
};

class MovieClip {
public:
	MovieClip(const swf::CharacterDictionary& dictionary, const std::vector<uint8_t>& swf, const Timeline& timeline,
		uint16_t characterId, MovieClip* parent);

	uint16_t characterId() const { return m_characterId; } // 0 for the root
	MovieClip* parent() const { return m_parent; }
	int currentFrame() const { return m_frame; } // 0-based; -1 before the first
	int frameCount() const { return static_cast<int>(m_timeline.frames.size()); }
	bool playing() const { return m_playing; }
	void play() { m_playing = true; }
	void stop() { m_playing = false; }

	// The next frame (looping at the end) if playing; then the same for
	// child clips. Actions of entered frames queue up in `actions`
	// (outermost first).
	void advance(std::vector<std::pair<MovieClip*, ActionBlock>>& actions);
	// Jumps (0-based); a frame label resolves through the timeline.
	void gotoFrame(int frame, std::vector<std::pair<MovieClip*, ActionBlock>>& actions);
	int frameOfLabel(const std::string& label) const;

	std::map<uint16_t, Instance>& displayList() { return m_displayList; }
	const std::map<uint16_t, Instance>& displayList() const { return m_displayList; }
	Instance* instanceNamed(const std::string& name);

	// Appends this clip's drawing (children by depth) under `world` and
	// `colour`.
	void draw(Drawer& drawer, const swf::Matrix& world, const swf::ColorTransform& colour, std::vector<MenuBatch>& out) const;

private:
	void enterFrame(int frame, std::vector<std::pair<MovieClip*, ActionBlock>>& actions, bool runActions);
	void place(const SwfTag& tag);
	void remove(uint16_t depth);

	const swf::CharacterDictionary& m_dictionary;
	const std::vector<uint8_t>& m_swf;
	const Timeline& m_timeline;
	uint16_t m_characterId;
	MovieClip* m_parent;
	int m_frame = -1;
	bool m_playing = true;
	std::map<uint16_t, Instance> m_displayList;
	// Sprite timelines, made once per sprite and shared by its clips.
	std::map<uint16_t, std::unique_ptr<Timeline>>* m_timelines;
	std::unique_ptr<std::map<uint16_t, std::unique_ptr<Timeline>>> m_ownTimelines; // the root owns them
};

} // namespace dice::hfe::ui
