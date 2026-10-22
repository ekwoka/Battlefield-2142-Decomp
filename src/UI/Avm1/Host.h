// What the interpreter needs from the movie player: movie clips and their
// timelines, URL requests and trace output. The display-list layer
// implements this; every method has a harmless default so tests and tools
// can run scripts without a stage.
//
// Movie clips are Objects the host creates (usually a subclass overriding
// getMember/setMember for _x, child clips and so on, with typeName()
// "movieclip"). "clip" parameters are such objects, or null when a target
// could not be resolved.
#pragma once

#include "Object.h"

#include <string>

namespace dice::hfe::ui::avm1 {

// GetProperty/SetProperty indices (SWF 4 "property" numbers).
enum ClipProperty : int {
	kPropX = 0,
	kPropY,
	kPropXScale,
	kPropYScale,
	kPropCurrentFrame,
	kPropTotalFrames,
	kPropAlpha,
	kPropVisible,
	kPropWidth,
	kPropHeight,
	kPropRotation,
	kPropTarget,
	kPropFramesLoaded,
	kPropName,
	kPropDropTarget,
	kPropUrl,
	kPropHighQuality,
	kPropFocusRect,
	kPropSoundBufTime,
	kPropQuality,
	kPropXMouse,
	kPropYMouse,
	kClipPropertyCount
};

// "_x", "_y", ... for an index, or nullptr.
const char* clipPropertyName(int index);

class Host {
public:
	virtual ~Host() = default;

	// trace(): Trace (0x26).
	virtual void trace(const std::string& message) { (void)message; }
	// An uncaught throw or an aborted block (bad bytecode, runaway loop).
	virtual void scriptError(const std::string& message) { (void)message; }

	// --- Clips by name

	// Resolves a target path ("/a/b", "_root.a", "../b", "_level0") from
	// the timeline `from`. Returning null lets the interpreter walk the path
	// itself with getMember (handling "/", "..", "_root" and "_levelN" via
	// root() and level()).
	virtual ObjectRef resolveTarget(const ObjectRef& from, const std::string& path)
	{
		(void)from, (void)path;
		return nullptr;
	}
	// _root as seen from `from`.
	virtual ObjectRef root(const ObjectRef& from)
	{
		(void)from;
		return nullptr;
	}
	// _levelN.
	virtual ObjectRef level(int number)
	{
		(void)number;
		return nullptr;
	}
	// TargetPath (0x45) and String(clip): "_level0.a.b". Empty when `clip`
	// is not a movie clip.
	virtual std::string targetPath(const ObjectRef& clip)
	{
		(void)clip;
		return {};
	}

	// --- GetProperty/SetProperty (index is a ClipProperty)

	virtual Value getProperty(const ObjectRef& clip, int index)
	{
		(void)clip, (void)index;
		return {};
	}
	virtual void setProperty(const ObjectRef& clip, int index, const Value& value) { (void)clip, (void)index, (void)value; }

	// --- Timeline control. Frames are 0-based here.

	virtual void gotoFrame(const ObjectRef& clip, int frame, bool play) { (void)clip, (void)frame, (void)play; }
	virtual void gotoLabel(const ObjectRef& clip, const std::string& label, bool play) { (void)clip, (void)label, (void)play; }
	virtual void play(const ObjectRef& clip) { (void)clip; }
	virtual void stop(const ObjectRef& clip) { (void)clip; }
	virtual void nextFrame(const ObjectRef& clip) { (void)clip; }
	virtual void prevFrame(const ObjectRef& clip) { (void)clip; }
	// WaitForFrame: has `frame` (0-based) loaded?
	virtual bool frameLoaded(const ObjectRef& clip, int frame)
	{
		(void)clip, (void)frame;
		return true;
	}
	// Call (0x9e): runs the actions of a frame ("label" or a 1-based number)
	// of `clip`.
	virtual void callFrame(const ObjectRef& clip, const std::string& frame) { (void)clip, (void)frame; }

	// --- Everything else movie clips do

	// GetURL/GetURL2. method: 0 none, 1 GET, 2 POST. loadTarget means the
	// target names a clip (loadMovie), loadVariables means loadVariables.
	virtual void getUrl(const ObjectRef& clip, const std::string& url, const std::string& target, int method, bool loadTarget, bool loadVariables)
	{
		(void)clip, (void)url, (void)target, (void)method, (void)loadTarget, (void)loadVariables;
	}
	// bounds: x1, y1, x2, y2 in pixels, or null for no constraint.
	virtual void startDrag(const ObjectRef& clip, bool lockCenter, const double* bounds) { (void)clip, (void)lockCenter, (void)bounds; }
	virtual void stopDrag() {}
	virtual void cloneSprite(const ObjectRef& source, const std::string& name, int depth) { (void)source, (void)name, (void)depth; }
	virtual void removeSprite(const ObjectRef& clip) { (void)clip; }
	virtual void toggleQuality() {}
	virtual void stopSounds() {}

	// GetTime (getTimer()) in milliseconds. Negative means "use the
	// interpreter's clock" (the sum of advance() calls).
	virtual double timer() { return -1; }

	// Object.registerClass(linkageName, constructor).
	virtual bool registerClass(const std::string& linkage, const ObjectRef& constructor)
	{
		(void)linkage, (void)constructor;
		return false;
	}
};

} // namespace dice::hfe::ui::avm1
