// A Flash player for the menus (SwiffPlayer.dll, DICE's gameswf): plays a
// SWF 7 movie's timelines, runs its ActionScript on the AVM1 interpreter,
// feeds it mouse and keyboard input, binds it to the game through the
// natives (Natives.h) and draws frames as MenuFrame batches.
//
// Each tick (one frame at the movie's rate, 30 fps for the menus):
//   1. timelines advance, root first, then children by depth: the next
//      frame's PlaceObject/RemoveObject run and its DoActions queue up;
//   2. new instances are initialised in creation order (clip event
//      Construct, the Object.registerClass constructor, Initialize);
//   3. the queued frame actions run, then onLoad/onUnload events; actions
//      queued meanwhile (gotoAndPlay to a frame with scripts) run after;
//   4. onEnterFrame and onClipEvent(enterFrame) run for every clip;
//   5. setInterval timers that came due run (Interpreter::advance);
//   6. text fields bound to variables pick up their values.
// DoInitAction blocks all run once at load, in tag order, before the
// first frame (the movie's classes are defined there).
//
// Input goes to the topmost interactive object under the mouse: a button
// character, a clip with a mouse handler (onPress, onRelease,
// onReleaseOutside, onRollOver, onRollOut, onDragOver, onDragOut or the
// same clip events) or an input text field. Shapes are hit by their
// tessellated outlines. Key presses go to Key listeners, key clip events,
// buttons' key conditions and the focused text field.
//
// loadMovie()/MovieClipLoader load images (PNG or JPEG) synchronously
// through the file reader, as gameswf did: the clip shows the image at its
// origin from the call on, so scripts can size it right away.
//
// Masks (setMask and timeline clip depths) work as the game's stencil
// masks (Stencil.h). Text fields draw with the engine's bitmap fonts, as
// SwiffPlayer.dll had RendDX9.dll draw them (Drawer::layoutText). Not
// done: sounds, HTML formatting beyond line breaks, selection highlighting
// and text scrolling by mouse.
#pragma once

#include "Natives.h"

#include "UI/Avm1/Host.h"
#include "UI/Avm1/Interpreter.h"
#include "UI/Characters/CharacterDictionary.h"
#include "UI/MenuDraw.h"
#include "UI/Swf.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dice::hfe::ui {

class Drawer;
struct Timeline;
struct PlaceRecord;
struct ActionBlock;

namespace player {
class DisplayObject;
class Clip;
class TextField;
class ButtonObject;
} // namespace player

class Player : private avm1::Host {
public:
	// Reads a file the movie asks for; `path` is relative to
	// "Menu/External/FlashMenu/" (e.g. "images/kits/assault_1.png").
	using FileReader = std::function<bool(const std::string& path, std::vector<uint8_t>& out)>;

	Player();
	~Player() override;
	Player(const Player&) = delete;
	Player& operator=(const Player&) = delete;

	// --- Setup (before load, though natives and the reader may change any time)

	NativeRegistry& natives() { return m_natives; }
	void setFileReader(FileReader reader) { m_reader = std::move(reader); }
	// Opens a movie for a clip: loadMovie of a URL starting with '?' (the
	// menu's "?titan_tutorial.bik", Multiplay.getMapMovie's
	// "?/Levels/<level>/Info/..._menuMovie.bik" and "?/noise.bik",
	// SwiffPlayer 0x100449b0). Null when it cannot be played.
	using VideoOpener = std::function<std::shared_ptr<VideoSource>(const std::string& url)>;
	void setVideoOpener(VideoOpener opener) { m_videoOpener = std::move(opener); }
	void setTraceHandler(std::function<void(const std::string&)> handler) { m_traceHandler = std::move(handler); }
	// getURL()/fscommand requests that are not movie loads.
	void setUrlHandler(std::function<void(const std::string& url, const std::string& target)> handler)
	{
		m_urlHandler = std::move(handler);
	}

	// Loads an uncompressed or compressed SWF and shows its first frame
	// (DoInitActions and frame 1's actions have run).
	bool load(std::vector<uint8_t> swf, std::string* error = nullptr);
	// The same for a .dfm (XOR-encoded SWF, Dfm.h).
	bool loadDfm(const std::vector<uint8_t>& dfm, std::string* error = nullptr);
	bool loaded() const { return m_root != nullptr; }

	// --- Running

	// Moves time on: one tick per 1/frameRate seconds (at most a second's
	// worth per call).
	void advance(double seconds);
	// One tick now.
	void tick();
	MenuFrame frame();
	// Masks cut on the CPU (exact, slow with many masks) instead of tagged
	// for the renderer's stencil: for tools that draw frames without a GPU.
	void setCpuMasks(bool cpu);

	double frameRate() const { return m_frameRate; }
	float stageWidth() const { return m_stageWidth; }
	float stageHeight() const { return m_stageHeight; }
	uint64_t ticks() const { return m_tick; }

	// --- Input (stage pixels; key codes are Flash's, as Key.getCode)

	void mouseMove(float x, float y);
	void mouseDown();
	void mouseUp();
	void mouseWheel(int delta);
	// `charCode` is the character typed (0 for none): it goes to the
	// focused text field and Key.getAscii.
	void keyDown(int code, uint32_t charCode = 0);
	void keyUp(int code);

	float mouseX() const { return m_mouseX; }
	float mouseY() const { return m_mouseY; }
	bool cursorVisible() const { return m_cursorVisible; }
	// The mouse is over something clickable that wants the hand cursor.
	bool handCursor();

	// --- Inspection (tests, tools, the game)

	const std::vector<std::string>& scriptErrors() const { return m_errors; }
	const std::vector<std::string>& traces() const { return m_traces; }
	avm1::Interpreter* interpreter() { return m_vm.get(); }
	avm1::ObjectRef root() const;
	// A value by path from _root ("login.mcName.text", "_root.x").
	avm1::Value evaluate(const std::string& path);
	// The display tree, one object per line (for debugging).
	std::string dumpDisplayList(int maxDepth = 64) const;
	// Visible interactive objects with their stage boxes (pixels), topmost first.
	struct Clickable {
		std::string target;
		float x = 0, y = 0, width = 0, height = 0;
	};
	std::vector<Clickable> clickables();
	// Script errors from the last load or tick are counted here too.
	size_t errorCount() const { return m_errors.size(); }

	// --- Internal: used by the display objects and script bindings

	const swf::CharacterDictionary& dictionary() const { return m_dictionary; }
	const std::vector<uint8_t>& swf() const { return m_swf; }
	Drawer& drawer() { return *m_drawer; }
	avm1::Interpreter& vm() { return *m_vm; }
	uint64_t currentTick() const { return m_tick; }
	const Timeline& timelineOf(uint16_t sprite);
	const Timeline& emptyTimeline();

	// Timeline control of a clip.
	void gotoFrame(player::Clip& clip, int frame, bool play);
	void enterFrame(player::Clip& clip, int frame, bool runActions);
	void advanceClip(player::Clip& clip);

	// Creates a character instance at `depth` of `parent` (replacing what
	// is there) and starts it (first frame, queued init).
	std::shared_ptr<player::DisplayObject> instantiate(player::Clip& parent, uint16_t character, int depth, const std::string& name);
	std::shared_ptr<player::Clip> createEmptyClip(player::Clip& parent, const std::string& name, int depth);
	std::shared_ptr<player::TextField> createTextField(player::Clip& parent, const std::string& name, int depth, const swf::Rect& bounds);
	std::shared_ptr<player::DisplayObject> attachMovie(player::Clip& parent, const std::string& linkage, const std::string& name,
		int depth, const avm1::ObjectRef& initObject);
	std::shared_ptr<player::DisplayObject> duplicate(player::DisplayObject& source, const std::string& name, int depth,
		const avm1::ObjectRef& initObject);
	void remove(player::DisplayObject& object);
	void swapDepths(player::DisplayObject& object, int depth);
	// Runs init (construct events, class constructor, initialize events)
	// of everything created so far.
	void runPendingInits();
	// Loads an image into `clip` (loadMovie); false if it could not be read.
	bool loadImage(player::Clip& clip, const std::string& url);
	void startDrag(player::DisplayObject& object, bool lockCenter, const double* bounds);
	void stopDrag() override;
	// Writes a bound text field's text to its variable.
	void writeVariable(avm1::Interpreter& vm, player::TextField& field);
	// Text focus (Selection).
	player::TextField* focus() const;
	void setFocus(const std::shared_ptr<player::TextField>& field);
	bool keyIsDown(int code) const { return m_keysDown.count(code) > 0; }
	int lastKeyCode() const { return m_lastKey; }
	uint32_t lastKeyChar() const { return m_lastChar; }
	void setCursorVisible(bool visible) { m_cursorVisible = visible; }
	// The constructor Object.registerClass gave the character, or null.
	avm1::ObjectRef registeredClass(uint16_t character) const;
	// Runs clip event actions matching `bits` and then method `method` (if
	// any) of `object` now.
	void dispatch(player::DisplayObject& object, uint32_t bits, const std::string& method, const std::vector<avm1::Value>& args = {});
	// Queues a block of frame actions.
	void queueActions(const std::shared_ptr<player::DisplayObject>& target, const ActionBlock& block);
	void runActions();

private:
	// avm1::Host
	void trace(const std::string& message) override;
	void scriptError(const std::string& message) override;
	avm1::ObjectRef root(const avm1::ObjectRef& from) override;
	avm1::ObjectRef level(int number) override;
	std::string targetPath(const avm1::ObjectRef& clip) override;
	avm1::Value getProperty(const avm1::ObjectRef& clip, int index) override;
	void setProperty(const avm1::ObjectRef& clip, int index, const avm1::Value& value) override;
	void gotoFrame(const avm1::ObjectRef& clip, int frame, bool play) override;
	void gotoLabel(const avm1::ObjectRef& clip, const std::string& label, bool play) override;
	void play(const avm1::ObjectRef& clip) override;
	void stop(const avm1::ObjectRef& clip) override;
	void nextFrame(const avm1::ObjectRef& clip) override;
	void prevFrame(const avm1::ObjectRef& clip) override;
	void callFrame(const avm1::ObjectRef& clip, const std::string& frame) override;
	void getUrl(const avm1::ObjectRef& clip, const std::string& url, const std::string& target, int method, bool loadTarget,
		bool loadVariables) override;
	void startDrag(const avm1::ObjectRef& clip, bool lockCenter, const double* bounds) override;
	void cloneSprite(const avm1::ObjectRef& source, const std::string& name, int depth) override;
	void removeSprite(const avm1::ObjectRef& clip) override;
	bool registerClass(const std::string& linkage, const avm1::ObjectRef& constructor) override;

	struct Action {
		enum class Type { Block, Event } type = Type::Block;
		std::shared_ptr<player::DisplayObject> target;
		size_t offset = 0, length = 0; // Block
		uint32_t bits = 0;             // Event: clip event bits
		std::string method;            // Event: method to call
	};

	void reset();
	void installBuiltins(); // Builtins.cpp
	void installDefaults();
	void place(player::Clip& parent, const PlaceRecord& record);
	void applyPlace(player::DisplayObject& object, const PlaceRecord& record, bool fresh);
	std::shared_ptr<player::DisplayObject> create(uint16_t character);
	void adopt(player::Clip& parent, const std::shared_ptr<player::DisplayObject>& object, int depth);
	void start(const std::shared_ptr<player::DisplayObject>& object);
	void queueUnload(player::DisplayObject& object);
	void runInit(const std::shared_ptr<player::DisplayObject>& object, const avm1::ObjectRef& initObject);
	void runBlock(const std::shared_ptr<player::DisplayObject>& target, size_t offset, size_t length);
	void enterFrameEvents();
	void syncTextFields();
	void syncTextField(player::TextField& field);
	void allObjects(player::DisplayObject& from, std::vector<std::shared_ptr<player::DisplayObject>>& out);

	// Mouse
	bool isInteractive(player::DisplayObject& object);
	using Chain = std::vector<std::shared_ptr<player::DisplayObject>>;
	bool pick(player::Clip& clip, swf::Point stage, Chain& chain);
	Chain pickAt(float x, float y);
	void updateHover(const Chain& now);
	void mouseEvent(player::DisplayObject& object, uint32_t clipBits, const char* method, uint16_t buttonConditions,
		uint8_t buttonState);
	void broadcast(const std::string& object, const std::string& message, const std::vector<avm1::Value>& args = {});
	void clipEventsToAll(uint32_t bits, const char* method, int keyCode = -1);
	void buttonKeys(player::DisplayObject& from, int buttonKey);
	void typeIntoFocus(int code, uint32_t charCode);

	NativeRegistry m_natives;
	FileReader m_reader;
	VideoOpener m_videoOpener;
	std::vector<std::weak_ptr<player::DisplayObject>> m_videoClips; // clips showing a movie
	bool loadVideo(player::Clip& clip, const std::string& url);
	void advanceVideos(double seconds);
	std::function<void(const std::string&)> m_traceHandler;
	std::function<void(const std::string&, const std::string&)> m_urlHandler;

	std::vector<uint8_t> m_swf;
	SwfMovie m_movie;
	swf::CharacterDictionary m_dictionary;
	std::unique_ptr<Drawer> m_drawer;
	bool m_cpuMasks = false;
	std::unique_ptr<avm1::Interpreter> m_vm;
	std::shared_ptr<player::Clip> m_root;
	std::map<uint16_t, std::unique_ptr<Timeline>> m_timelines;
	std::unique_ptr<Timeline> m_rootTimeline;
	std::unique_ptr<Timeline> m_emptyTimeline;
	std::map<std::string, avm1::ObjectRef> m_classes; // linkage -> constructor
	std::multimap<uint16_t, std::string> m_exportNames;

	std::deque<Action> m_actions;
	struct PendingInit {
		std::shared_ptr<player::DisplayObject> object;
		avm1::ObjectRef initObject;
	};
	std::vector<PendingInit> m_inits;
	int m_runningActions = 0;

	double m_frameRate = 30;
	float m_stageWidth = 1024, m_stageHeight = 768;
	double m_time = 0; // seconds not yet ticked
	uint64_t m_tick = 0;
	int m_instanceCount = 0;

	// Input
	float m_mouseX = 0, m_mouseY = 0;
	bool m_mouseIsDown = false;
	std::vector<std::shared_ptr<player::DisplayObject>> m_hover;   // under the mouse, deepest first
	std::vector<std::shared_ptr<player::DisplayObject>> m_pressed; // pressed, until released
	std::weak_ptr<player::TextField> m_focus;
	std::set<int> m_keysDown;
	int m_lastKey = 0;
	uint32_t m_lastChar = 0;
	bool m_cursorVisible = true;
	struct Drag {
		std::weak_ptr<player::DisplayObject> object;
		bool lockCenter = false;
		float offsetX = 0, offsetY = 0;
		bool constrained = false;
		double bounds[4] = {0, 0, 0, 0};
	} m_drag;

	std::vector<std::string> m_errors;
	std::vector<std::string> m_traces;
	std::map<std::string, std::string> m_storage; // Logic.get/setStorageString
};

} // namespace dice::hfe::ui
