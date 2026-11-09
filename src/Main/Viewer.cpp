#include "Viewer.h"
#include "MouseCapture.h"
#include "MissileViewSmoothing.h"

#include "Render/GpuContext.h"
#include "Render/LevelRenderer.h"
#include "Render/PngWriter.h"
#include "Game/TickTimer.h"
#include "Input/ControlMap.h"
#include "Input/GameControllers.h"
#include "Input/SdlGameControllers.h"

#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <CoreGraphics/CGRemoteOperation.h> // CGAssociateMouseAndMouseCursorPosition
#endif

#include <algorithm>
#include <cmath>
#include <vector>
#include <sstream>
#include <set>
#include <cstdlib>
#include <cstring>
#include <cstdio>


// macOS: SDL can report relative mode while the system cursor still moves
// (the log's "cursor escaped" lines). Detaching the cursor from the mouse is
// what relative mode means, so the viewer also does it itself.
static void lockSystemCursor(bool lock)
{
#ifdef __APPLE__
	CGAssociateMouseAndMouseCursorPosition(lock ? 0 : 1);
#else
	(void)lock;
#endif
}

namespace dice::hfe {

struct Window::Impl {
	SDL_Window* window = nullptr;
	std::unique_ptr<render::GpuContext> gpu;
	bool closed = false;

	// Scripted input (setScript).
	struct Command {
		double at = 0;
		std::string op, arg;
	};
	std::vector<Command> script;
	size_t next = 0;
	Uint64 opened = 0;
	std::set<SDL_Scancode> held;
	std::set<SDL_Scancode> releaseNext; // key: pressed for one frame, released on the next
	std::set<SDL_Scancode> fullscreenKeys; // consumed until key-up so Enter cannot reach gameplay or Flash
	Uint32 heldButtons = 0;
	std::string text;       // the last scripted text (its event points at it)
	std::string shot;       // a screenshot the next frame takes
	float mouseX = 0, mouseY = 0;

	// Pushes the events of the commands now due.
	void runScript();
	// The keyboard as read, with the scripted keys held.
	const bool* keyboard();
	Uint32 mouseButtons() const { return SDL_GetMouseState(nullptr, nullptr) | heldButtons; }
	bool merged[SDL_SCANCODE_COUNT] = {};
};

void Window::Impl::runScript()
{
	const double now = static_cast<double>(SDL_GetTicksNS() - opened) / 1e9;
	int width = 1, height = 1;
	SDL_GetWindowSize(window, &width, &height);
	const SDL_WindowID id = SDL_GetWindowID(window);
	const auto scancode = [](std::string name) {
		std::replace(name.begin(), name.end(), '_', ' ');
		// SDL can rename modifier keys to platform-specific names.
		if (name == "Left Alt" || name == "Left Option") return SDL_SCANCODE_LALT;
		if (name == "Right Alt" || name == "Right Option") return SDL_SCANCODE_RALT;
		return SDL_GetScancodeFromName(name.c_str());
	};
	const auto keyEvent = [&](SDL_Scancode code, bool down) {
		SDL_Event event {};
		event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.windowID = id;
		event.key.scancode = code;
		SDL_Keymod modifiers = SDL_KMOD_NONE;
		if (held.count(SDL_SCANCODE_LALT)) modifiers |= SDL_KMOD_LALT;
		if (held.count(SDL_SCANCODE_RALT)) modifiers |= SDL_KMOD_RALT;
		if (held.count(SDL_SCANCODE_LSHIFT)) modifiers |= SDL_KMOD_LSHIFT;
		if (held.count(SDL_SCANCODE_RSHIFT)) modifiers |= SDL_KMOD_RSHIFT;
		if (held.count(SDL_SCANCODE_LCTRL)) modifiers |= SDL_KMOD_LCTRL;
		if (held.count(SDL_SCANCODE_RCTRL)) modifiers |= SDL_KMOD_RCTRL;
		event.key.key = SDL_GetKeyFromScancode(code, modifiers, false);
		event.key.mod = modifiers;
		event.key.down = down;
		SDL_PushEvent(&event);
	};
	const auto point = [&](const std::string& arg, float& x, float& y) {
		x = static_cast<float>(std::atof(arg.c_str())) * static_cast<float>(width);
		y = static_cast<float>(std::atof(arg.substr(arg.find(',') + 1).c_str())) * static_cast<float>(height);
	};
	for (const SDL_Scancode code : releaseNext) {
		keyEvent(code, false);
		held.erase(code);
	}
	releaseNext.clear();
	while (next < script.size() && script[next].at <= now) {
		const Command& c = script[next++];
		if (c.op == "key" || c.op == "down" || c.op == "up") {
			const SDL_Scancode code = scancode(c.arg);
			if (code == SDL_SCANCODE_UNKNOWN) {
				std::fprintf(stderr, "input script: no key %s\n", c.arg.c_str());
				continue;
			}
			if (c.op != "up") {
				keyEvent(code, true);
				held.insert(code);
			}
			if (c.op == "up") {
				keyEvent(code, false);
				held.erase(code);
			} else if (c.op == "key") {
				releaseNext.insert(code); // held for this frame, as a real press is
			}
		} else if (c.op == "move" || c.op == "click") {
			SDL_Event motion {};
			motion.type = SDL_EVENT_MOUSE_MOTION;
			motion.motion.windowID = id;
			point(c.arg, motion.motion.x, motion.motion.y);
			motion.motion.xrel = motion.motion.x - mouseX;
			motion.motion.yrel = motion.motion.y - mouseY;
			mouseX = motion.motion.x;
			mouseY = motion.motion.y;
			SDL_PushEvent(&motion);
			if (c.op == "click") {
				for (const bool down : {true, false}) {
					SDL_Event button {};
					button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
					button.button.windowID = id;
					button.button.button = SDL_BUTTON_LEFT;
					button.button.down = down;
					button.button.x = mouseX;
					button.button.y = mouseY;
					SDL_PushEvent(&button);
				}
			}
		} else if (c.op == "button") {
			const int button = std::atoi(c.arg.c_str());
			const bool down = c.arg.find(",1") != std::string::npos;
			heldButtons = down ? heldButtons | SDL_BUTTON_MASK(button) : heldButtons & ~SDL_BUTTON_MASK(button);
			SDL_Event event {};
			event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			event.button.windowID = id;
			event.button.button = static_cast<Uint8>(button);
			event.button.down = down;
			event.button.x = mouseX;
			event.button.y = mouseY;
			SDL_PushEvent(&event);
		} else if (c.op == "look") {
			SDL_Event motion {};
			motion.type = SDL_EVENT_MOUSE_MOTION;
			motion.motion.windowID = id;
			motion.motion.x = mouseX;
			motion.motion.y = mouseY;
			motion.motion.xrel = static_cast<float>(std::atof(c.arg.c_str()));
			motion.motion.yrel = static_cast<float>(std::atof(c.arg.substr(c.arg.find(',') + 1).c_str()));
			SDL_PushEvent(&motion);
		} else if (c.op == "text") {
			text = c.arg;
			std::replace(text.begin(), text.end(), '_', ' ');
			SDL_Event event {};
			event.type = SDL_EVENT_TEXT_INPUT;
			event.text.windowID = id;
			event.text.text = text.c_str();
			SDL_PushEvent(&event);
		} else if (c.op == "shot") {
			shot = c.arg;
		} else if (c.op == "quit") {
			SDL_Event event {};
			event.type = SDL_EVENT_QUIT;
			SDL_PushEvent(&event);
		} else {
			std::fprintf(stderr, "input script: unknown command %s\n", c.op.c_str());
		}
	}
}

const bool* Window::Impl::keyboard()
{
	int count = 0;
	const bool* state = SDL_GetKeyboardState(&count);
	for (int i = 0; i < count && i < SDL_SCANCODE_COUNT; ++i) {
		merged[i] = state[i];
	}
	for (const SDL_Scancode code : held) {
		merged[code] = true;
	}
	for (const SDL_Scancode code : fullscreenKeys) {
		merged[code] = false;
	}
	return merged;
}

void Window::setScript(const std::string& script)
{
	std::istringstream items(script);
	for (std::string item; items >> item;) {
		const size_t colon = item.find(':');
		if (colon == std::string::npos) {
			continue;
		}
		Impl::Command c;
		c.at = std::atof(item.substr(0, colon).c_str());
		const std::string rest = item.substr(colon + 1);
		const size_t colon2 = rest.find(':');
		c.op = rest.substr(0, colon2);
		c.arg = colon2 == std::string::npos ? std::string() : rest.substr(colon2 + 1);
		impl->script.push_back(c);
	}
	std::stable_sort(impl->script.begin(), impl->script.end(),
		[](const Impl::Command& a, const Impl::Command& b) { return a.at < b.at; });
}

namespace {

// SDL keycodes to Flash's (Key.getCode): letters as upper-case ASCII,
// digits and space as themselves, and the named keys.
int flashKeyCode(int sdl)
{
	if (sdl >= 'a' && sdl <= 'z') {
		return sdl - 'a' + 'A';
	}
	if ((sdl >= '0' && sdl <= '9') || sdl == ' ') {
		return sdl;
	}
	switch (sdl) {
	case SDLK_BACKSPACE: return 8;
	case SDLK_TAB: return 9;
	case SDLK_RETURN: case SDLK_KP_ENTER: return 13;
	case SDLK_LSHIFT: case SDLK_RSHIFT: return 16;
	case SDLK_LCTRL: case SDLK_RCTRL: return 17;
	case SDLK_ESCAPE: return 27;
	case SDLK_PAGEUP: return 33;
	case SDLK_PAGEDOWN: return 34;
	case SDLK_END: return 35;
	case SDLK_HOME: return 36;
	case SDLK_LEFT: return 37;
	case SDLK_UP: return 38;
	case SDLK_RIGHT: return 39;
	case SDLK_DOWN: return 40;
	case SDLK_DELETE: return 46;
	default: return 0;
	}
}


constexpr float kDegrees = 3.14159265f / 180.0f;
// The world's near plane in first person, nearer than the chase view's so
// that walls by the eye aren't cut (the original's renderer.tweakNearPlane
// is 0.041 with a 400 m view; the rewrite draws to 6 km).
constexpr float kFirstPersonNear = 0.1f;

void applyCameraOption(render::Camera& camera, const std::string& text)
{
	float v[5] = {};
	if (text.empty() || std::sscanf(text.c_str(), "%f/%f/%f/%f/%f", &v[0], &v[1], &v[2], &v[3], &v[4]) < 3) {
		return;
	}
	camera.position = {v[0], v[1], v[2]};
	camera.yaw = v[3] * kDegrees;
	camera.pitch = v[4] * kDegrees;
}

// The control maps' key names (IDKey_...) to SDL's.
SDL_Scancode scancodeFor(const std::string& name)
{
	if (name.compare(0, 6, "IDKey_") != 0) {
		return SDL_SCANCODE_UNKNOWN;
	}
	const std::string key = name.substr(6);
	if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') {
		return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (key[0] - 'A'));
	}
	if (key.size() == 1 && key[0] >= '1' && key[0] <= '9') {
		return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (key[0] - '1'));
	}
	if (key.size() >= 2 && key[0] == 'F' && key.find_first_not_of("0123456789", 1) == std::string::npos) {
		const int f = std::atoi(key.c_str() + 1);
		if (f >= 1 && f <= 12) {
			return static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (f - 1));
		}
	}
	if (key.compare(0, 6, "Numpad") == 0 && key.size() == 7) {
		return key[6] == '0' ? SDL_SCANCODE_KP_0 : static_cast<SDL_Scancode>(SDL_SCANCODE_KP_1 + (key[6] - '1'));
	}
	static const std::pair<const char*, SDL_Scancode> named[] = {{"0", SDL_SCANCODE_0}, {"Space", SDL_SCANCODE_SPACE},
		{"LeftShift", SDL_SCANCODE_LSHIFT}, {"RightShift", SDL_SCANCODE_RSHIFT}, {"LeftCtrl", SDL_SCANCODE_LCTRL},
		{"RightCtrl", SDL_SCANCODE_RCTRL}, {"LeftAlt", SDL_SCANCODE_LALT}, {"RightAlt", SDL_SCANCODE_RALT},
		{"Tab", SDL_SCANCODE_TAB}, {"Enter", SDL_SCANCODE_RETURN}, {"Escape", SDL_SCANCODE_ESCAPE},
		{"Backspace", SDL_SCANCODE_BACKSPACE}, {"Delete", SDL_SCANCODE_DELETE}, {"Insert", SDL_SCANCODE_INSERT},
		{"Home", SDL_SCANCODE_HOME}, {"End", SDL_SCANCODE_END}, {"PageUp", SDL_SCANCODE_PAGEUP},
		{"PageDown", SDL_SCANCODE_PAGEDOWN}, {"ArrowUp", SDL_SCANCODE_UP}, {"ArrowDown", SDL_SCANCODE_DOWN},
		{"ArrowLeft", SDL_SCANCODE_LEFT}, {"ArrowRight", SDL_SCANCODE_RIGHT}, {"Capital", SDL_SCANCODE_CAPSLOCK},
		{"Grave", SDL_SCANCODE_GRAVE}, {"Add", SDL_SCANCODE_KP_PLUS}, {"Subtract", SDL_SCANCODE_KP_MINUS},
		{"PrintScreen", SDL_SCANCODE_PRINTSCREEN}, {"Minus", SDL_SCANCODE_MINUS}, {"Equals", SDL_SCANCODE_EQUALS}};
	for (const auto& [text, code] : named) {
		if (key == text) {
			return code;
		}
	}
	return SDL_SCANCODE_UNKNOWN;
}

void printStats(const render::LevelStats& stats)
{
	std::printf("renderer: %zu meshes (%zu missing), %zu textures (%zu missing), %zu instances (%zu lightmapped), "
				"%zu terrain patches, %zu roads (%zu missing)\n",
		stats.meshes, stats.missingMeshes, stats.textures, stats.missingTextures, stats.instances,
		stats.lightmappedInstances, stats.terrainPatches, stats.roads, stats.missingRoads);
	std::printf("renderer: hemi map %s, %zu cloud layers, sun flare %s, %zu normal-mapped materials\n",
		stats.hemiMap ? "yes" : "no", stats.cloudLayers, stats.sunFlare ? "yes" : "no", stats.normalMapped);
	for (const std::string& missing : stats.missingMeshNames) {
		std::printf("  missing mesh %s\n", missing.c_str());
	}
}

int screenshot(const render::LevelSources& sources, const ViewerOptions& options)
{
	std::string error;
	auto gpu = render::GpuContext::create(nullptr, error);
	if (!gpu) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	render::LevelRenderer level(*gpu);
	level.build(sources);
	printStats(level.stats());
	render::Camera camera = level.suggestedCamera();
	applyCameraOption(camera, options.camera);
	ViewerOptions::PlayerView view;
	view.aspect = static_cast<float>(options.width) / static_cast<float>(options.height);
	if (options.camera.empty() && options.playerView && options.playerView(view) && (view.firstPerson || view.external)) {
		camera.position = {view.eye[0], view.eye[1], view.eye[2]};
		camera.yaw = view.eyeYaw;
		camera.pitch = view.eyePitch;
		camera.fovY = view.fovY;
		camera.roll = view.eyeRoll;
		camera.nearPlane = view.firstPerson ? kFirstPersonNear : render::Camera {}.nearPlane;
	} else if (options.camera.empty() && options.playerView && options.playerView(view)) {
		// The chase view of a soldier known only now (a joined game's: its
		// ghost): framed from behind as main.cpp frames a local one (5 m
		// back, 2.5 m up, 12 degrees down).
		const float yaw = view.yawDegrees * 3.14159265f / 180.0f;
		camera.position = {view.x - std::sin(yaw) * 5, view.y + 2.5f, view.z - std::cos(yaw) * 5};
		camera.yaw = yaw;
		camera.pitch = -12 * 3.14159265f / 180.0f;
	}

	auto target = gpu->createOffscreenTarget(static_cast<uint32_t>(options.width), static_cast<uint32_t>(options.height));
	SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
	if (options.onCamera) {
		const render::Vec3f forward = camera.forward();
		const float position[3] = {camera.position.x, camera.position.y, camera.position.z};
		const float ahead[3] = {forward.x, forward.y, forward.z};
		options.onCamera(position, ahead);
	}
	if (options.onView) {
		const float position[3] = {camera.position.x, camera.position.y, camera.position.z};
		options.onView(position, camera.yaw, camera.pitch, camera.fovY,
			static_cast<float>(options.width) / static_cast<float>(options.height));
	}
	level.syncObjects();
	level.draw(commands, target.color, target, camera, sources.lights);
	SDL_SubmitGPUCommandBuffer(commands);
	// BF2142_FRAME_BENCH=N: draw the same frame N more times, each waited
	// for, and print the mean frame cost (CPU preparation and the GPU's).
	if (const char* bench = std::getenv("BF2142_FRAME_BENCH")) {
		const int frames = std::max(std::atoi(bench), 1);
		const Uint64 start = SDL_GetTicksNS();
		for (int i = 0; i < frames; ++i) {
			SDL_GPUCommandBuffer* again = SDL_AcquireGPUCommandBuffer(gpu->device());
			level.draw(again, target.color, target, camera, sources.lights);
			SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(again);
			if (fence) {
				SDL_WaitForGPUFences(gpu->device(), true, &fence, 1);
				SDL_ReleaseGPUFence(gpu->device(), fence);
			}
		}
		std::printf("frame cost %.3f ms (mean of %d)\n",
			static_cast<double>(SDL_GetTicksNS() - start) / 1e6 / frames, frames);
	}
	std::vector<uint8_t> pixels;
	const bool ok = gpu->readPixels(target, pixels) &&
		render::writePng(options.screenshot, target.width, target.height, pixels);
	gpu->releaseTarget(target);
	std::printf("%s %s\n", ok ? "wrote" : "failed to write", options.screenshot.c_str());
	return ok ? 0 : 1;
}

// The menu's cursors (SwiffHost slot 0x84, 0x0046bcf0: the arrow, busy and
// key binding .ani files of menu/external/flashmenu/cursor/, which the game
// doesn't ship): the system's arrow, its wait cursor and its crosshair,
// shown as the window's WM_SETCURSOR answer (0x00486440) picks them by the
// wait state (slot 0x2f0).
class MenuCursor {
public:
	~MenuCursor()
	{
		for (SDL_Cursor* cursor : m_cursors) {
			if (cursor) {
				SDL_DestroyCursor(cursor);
			}
		}
	}
	void show(int index)
	{
		index = index >= 0 && index < 3 ? index : 0;
		if (index == m_shown) {
			return;
		}
		static const SDL_SystemCursor kSystem[3] = {SDL_SYSTEM_CURSOR_DEFAULT, SDL_SYSTEM_CURSOR_WAIT, SDL_SYSTEM_CURSOR_CROSSHAIR};
		if (!m_cursors[index]) {
			m_cursors[index] = SDL_CreateSystemCursor(kSystem[index]);
		}
		if (m_cursors[index]) {
			SDL_SetCursor(m_cursors[index]);
		}
		m_shown = index;
	}

private:
	SDL_Cursor* m_cursors[3] = {nullptr, nullptr, nullptr};
	int m_shown = 0;
};

} // namespace

bool capsLockOn() { return (SDL_GetModState() & SDL_KMOD_CAPS) != 0; }

namespace {

int interactive(const render::LevelSources& sources, const ViewerOptions& options, Window::Impl& shown)
{
	SDL_Window* window = shown.window;
	render::GpuContext* gpu = shown.gpu.get();
	{
		render::LevelRenderer level(*gpu);
		level.build(sources);
		printStats(level.stats());
		render::Camera camera = level.suggestedCamera();
		applyCameraOption(camera, options.camera);
		const bool playing = static_cast<bool>(options.playerView);
		if (options.onMenuInput || options.onHudText) {
			SDL_StartTextInput(window);
		}
		if (options.onMenuInput && !options.menuActive) {
			std::printf("the menu has the mouse and keyboard; close the window to quit\n");
		} else if (playing) {
			std::printf("WASD run, shift sprints, the mouse looks around, left button fires, Esc %s\n",
				options.onEscape ? "shows the menu" : "quits");
		} else {
			std::printf("WASD/QE move, right mouse button looks around, shift is faster, Esc quits\n");
		}
		float pitch = -0.2f;
		float turn = 0;
		bool drivingNow = false; // last frame's seat: a driver's mouse pitch is not clamped like a soldier's
		float wheelSteps = 0; // the mouse wheel in play (IDAxis_2), since the last input
		// The game controllers, opened as SDL reports them.
		std::unique_ptr<input::SdlGameControllers> sdlControllers;
		if (options.gameControllers) {
			sdlControllers = std::make_unique<input::SdlGameControllers>(*options.gameControllers);
			if (!sdlControllers->start()) {
				sdlControllers.reset();
			}
		}
		bool firstPerson = false; // the last frame's view

		render::GpuContext::Target target;
		Uint64 last = SDL_GetTicksNS();
		const Uint64 start = last;
		shown.opened = last; // scripted input starts after the level is ready
		MissileViewSmoothing missileSmoothing;
		TickTimer ticks(10, 30.0f); // as BF2Engine
		ticks.advance(0);
		bool running = true;
		bool pointer = false; // the mouse is a pointer (a menu is up)
		bool warnedRelativeMode = false;
		bool warnedMouseGrab = false;
		int tracedCaptureState = -1;
		Uint64 escapeCheckNs = 0; // the last time a captured cursor was found outside the window
		bool systemCursorLocked = false;
		bool roseCursorHidden = false; // the comm rose is picked by pointing, without a visible cursor
		// A detached process from the launcher applet may start behind it, and then
		// never receives focus (so never captures) until the user clicks.
		SDL_RaiseWindow(window);
		input::MenuWheelSteps menuWheel;
		MenuCursor menuCursor;
		while (running) {
			const bool wanted = options.wantsPointer && options.wantsPointer();
			if (playing && wanted != pointer) {
				pointer = wanted;
			}
			SDL_Event event;
			int windowWidth = 1, windowHeight = 1;
			SDL_GetWindowSize(window, &windowWidth, &windowHeight);
			const auto menu = [&](ViewerOptions::MenuInput input) {
				options.onMenuInput(input);
			};
			const bool menuUp = options.onMenuInput && (!options.menuActive || options.menuActive());
			if (menuUp && options.menuCursor) {
				menuCursor.show(options.menuCursor());
			}
			if (options.wantsQuit && options.wantsQuit()) {
				running = false;
			}
			shown.runScript();
			if (options.gameControllers) {
				options.gameControllers->beginFrame();
			}
			Uint32 pressedButtons = 0;
			bool captureRefresh = false; // an event after which the OS may have dropped capture
			while (SDL_PollEvent(&event)) {
				switch (event.type) {
				case SDL_EVENT_WINDOW_FOCUS_GAINED:
				// Not MOUSE_ENTER/EXPOSED: releasing relative mode can raise them, re-triggering every frame.
				case SDL_EVENT_WINDOW_SHOWN:
				case SDL_EVENT_WINDOW_RESTORED:
				case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
				case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
				case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
					captureRefresh = true;
					break;
				default:
					break;
				}
				if (sdlControllers && sdlControllers->handle(event)) {
					continue;
				}
				if (event.type == SDL_EVENT_KEY_DOWN && shown.fullscreenKeys.count(event.key.scancode)) {
					continue; // swallow repeats too; the shortcut is one press until key-up
				}
				if (event.type == SDL_EVENT_KEY_UP && shown.fullscreenKeys.erase(event.key.scancode)) {
					continue; // do not let Flash observe the key-up from a consumed shortcut
				}
				if (event.type == SDL_EVENT_KEY_DOWN
					&& (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER
						|| event.key.scancode == SDL_SCANCODE_RETURN || event.key.scancode == SDL_SCANCODE_KP_ENTER)
					&& (event.key.mod & SDL_KMOD_ALT) != 0) {
					shown.fullscreenKeys.insert(event.key.scancode);
					if (!event.key.repeat) {
						const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
						if (!SDL_SetWindowFullscreen(window, !fullscreen)) {
							std::fprintf(stderr, "SDL_SetWindowFullscreen: %s\n", SDL_GetError());
						} else {
							if (!SDL_SyncWindow(window)) std::fprintf(stderr, "SDL_SyncWindow: %s\n", SDL_GetError());
							if (std::getenv("BF2142_INPUT_TRACE")) std::printf("fullscreen-toggle requested=%d actual=%d\n",
								!fullscreen, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0);
							// The normal frame path also re-reads size and swapchain pixels;
							// refresh menu coordinates for any remaining events this frame.
							SDL_GetWindowSize(window, &windowWidth, &windowHeight);
						}
					}
					continue; // never pass Option-Enter to HUD, deployment, or Flash
				}
				if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
					shown.fullscreenKeys.clear();
				}
				if (pointer && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_MOTION)) {
					// Pointer screens: what SDL delivers (every button, every 20th motion), to find lost clicks.
					static int motions = 0;
					if (event.type != SDL_EVENT_MOUSE_MOTION || ++motions % 20 == 1) {
						std::printf("pointer-event: %s button=%d at %.0f,%.0f playing=%d menuUp=%d\n",
							event.type == SDL_EVENT_MOUSE_MOTION ? "motion" : event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "down" : "up",
							event.type == SDL_EVENT_MOUSE_MOTION ? 0 : event.button.button,
							event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.x : event.button.x,
							event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.y : event.button.y, playing, menuUp);
						std::fflush(stdout);
					}
				}
				if (playing && !pointer && !menuUp && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
					pressedButtons |= SDL_BUTTON_MASK(event.button.button);
				}
				if (event.type == SDL_EVENT_QUIT) {
					running = false;
					shown.closed = true;
				} else if (!menuUp && options.onHudText && options.wantsText && options.wantsText()
					&& (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT)) {
					if (event.type == SDL_EVENT_TEXT_INPUT) {
						options.onHudText({ViewerOptions::MenuInput::Kind::Text, 0, 0, 0, event.text.text});
					} else {
						options.onHudText({ViewerOptions::MenuInput::Kind::Key, 0, 0, flashKeyCode(static_cast<int>(event.key.key))});
					}
				} else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && options.onEscape) {
					options.onEscape();
				} else if (menuUp && (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
													  || event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
					using Kind = ViewerOptions::MenuInput::Kind;
					const float x = (event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.x : event.button.x) / static_cast<float>(windowWidth);
					const float y = (event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.y : event.button.y) / static_cast<float>(windowHeight);
					if (event.type == SDL_EVENT_MOUSE_MOTION) {
						menu({Kind::Move, x, y});
					} else if (event.button.button == SDL_BUTTON_LEFT) {
						menu({event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? Kind::Down : Kind::Up, x, y});
					}
				} else if (menuUp && event.type == SDL_EVENT_MOUSE_WHEEL) {
					float x = 0, y = 0;
					SDL_GetMouseState(&x, &y);
					menu({ViewerOptions::MenuInput::Kind::Move, x / windowWidth, y / windowHeight});
					const float delta = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
					if (const int steps = menuWheel.add(delta, static_cast<double>(SDL_GetTicksNS()) / 1e9); steps != 0) {
						menu({ViewerOptions::MenuInput::Kind::Wheel, 0, 0, steps});
					}
				} else if (playing && !pointer && event.type == SDL_EVENT_MOUSE_WHEEL) {
					wheelSteps += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
				} else if (menuUp && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)) {
					menu({event.type == SDL_EVENT_KEY_DOWN ? ViewerOptions::MenuInput::Kind::Key : ViewerOptions::MenuInput::Kind::KeyUp, 0, 0,
						flashKeyCode(static_cast<int>(event.key.key))});
				} else if (menuUp && event.type == SDL_EVENT_TEXT_INPUT) {
					menu({ViewerOptions::MenuInput::Kind::Text, 0, 0, 0, event.text.text});
				} else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
					running = false;
				} else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && pointer
					&& options.onClick) {
					int w = 1, h = 1;
					SDL_GetWindowSize(window, &w, &h);
					options.onClick(event.button.x * 800.0f / static_cast<float>(w), event.button.y * 600.0f / static_cast<float>(h));
				} else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT && pointer
					&& options.onRightClick) {
					int w = 1, h = 1;
					SDL_GetWindowSize(window, &w, &h);
					options.onRightClick(event.button.x * 800.0f / static_cast<float>(w), event.button.y * 600.0f / static_cast<float>(h));
				} else if (event.type == SDL_EVENT_MOUSE_MOTION && playing && pointer && options.onPointerMove) {
					int w = 1, h = 1;
					SDL_GetWindowSize(window, &w, &h);
					options.onPointerMove(event.motion.x * 800.0f / static_cast<float>(w), event.motion.y * 600.0f / static_cast<float>(h));
				} else if (event.type == SDL_EVENT_MOUSE_MOTION && playing && !pointer) {
					const float scale = options.mouseScale ? options.mouseScale() : 1.0f;
					const float up = options.invertMouse && options.invertMouse() ? -1.0f : 1.0f;
					turn += event.motion.xrel * 0.15f * scale;
					// First person looks up and down to 85 degrees (0x005890a0).
					pitch = clampLookPitch(pitch - event.motion.yrel * 0.003f * scale * up, firstPerson, drivingNow);
				} else if (event.type == SDL_EVENT_MOUSE_MOTION && (event.motion.state & SDL_BUTTON_RMASK)) {
					camera.yaw += event.motion.xrel * 0.004f;
					camera.pitch = std::fmax(-1.5f, std::fmin(1.5f, camera.pitch - event.motion.yrel * 0.004f));
				}
			}
			// Relative mode is what keeps gameplay look alive at the window
			// edge, but SDL drops the native grab when focus is lost. Reconcile
			// both SDL states every frame so focus return and menu transitions
			// reacquire/release capture even if no pointer-state callback changed.
			// A Flash menu can be active independently of the HUD pointer flag.
			const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
			const bool capture = gameplayMouseCaptureWanted(playing, pointer, menuUp, focused);
			bool relativeNow = SDL_GetWindowRelativeMouseMode(window);
			bool grabbedNow = SDL_GetWindowMouseGrab(window);
			if (capture && relativeNow && grabbedNow && SDL_GetTicksNS() - escapeCheckNs > 500000000ull) {
				// SDL's flags can stay set after macOS let the cursor go: a cursor
				// seen outside the window means capture was lost, so re-take it.
				float gx = 0, gy = 0;
				int wx = 0, wy = 0, ww = 0, wh = 0;
				SDL_GetGlobalMouseState(&gx, &gy);
				SDL_GetWindowPosition(window, &wx, &wy);
				SDL_GetWindowSize(window, &ww, &wh);
				if (gx < wx || gy < wy || gx >= wx + ww || gy >= wy + wh) {
					std::printf("mouse-capture: cursor escaped at %.0f,%.0f (window %d,%d %dx%d), re-taking\n", gx, gy, wx, wy, ww, wh);
					std::fflush(stdout);
					SDL_WarpMouseInWindow(window, ww * 0.5f, wh * 0.5f); // back inside, so the check settles
					captureRefresh = true;
					escapeCheckNs = SDL_GetTicksNS();
					systemCursorLocked = false; // re-applied below
				}
			}
			const char* pointerWhy = options.pointerReason ? options.pointerReason() : nullptr;
			const bool confine = gameplayPointerConfined(playing, pointer, menuUp, focused, pointerWhy);
			if (mouseCaptureNeedsRelease(capture, relativeNow, grabbedNow, captureRefresh, confine)) {
				if (std::getenv("BF2142_INPUT_TRACE")) std::printf("mouse-capture release refresh=%d wanted=%d\n", captureRefresh, capture);
				SDL_SetWindowMouseGrab(window, false);
				SDL_SetWindowRelativeMouseMode(window, false);
				relativeNow = grabbedNow = false;
			}
			if (mouseCaptureNeedsAcquire(capture, relativeNow, grabbedNow)) {
				if (!relativeNow && !SDL_SetWindowRelativeMouseMode(window, true)) {
					if (!warnedRelativeMode) {
						std::fprintf(stderr, "SDL_SetWindowRelativeMouseMode(true): %s\n", SDL_GetError());
						warnedRelativeMode = true;
					}
				} else {
					warnedRelativeMode = false;
				}
				if (!grabbedNow && !SDL_SetWindowMouseGrab(window, true)) {
					if (!warnedMouseGrab) {
						std::fprintf(stderr, "SDL_SetWindowMouseGrab(true): %s\n", SDL_GetError());
						warnedMouseGrab = true;
					}
				} else {
					warnedMouseGrab = false;
				}
			} else if (confine && !grabbedNow) {
				SDL_SetWindowMouseGrab(window, true); // the cursor shows but stays in the window
			}
			{
				const bool hideCursor = confine && pointerWhy && sameText(pointerWhy, "comm-rose");
				if (hideCursor != roseCursorHidden) {
					if (hideCursor) {
						SDL_HideCursor();
					} else {
						SDL_ShowCursor();
					}
					roseCursorHidden = hideCursor;
				}
			}
			// After relative mode let go, so the release doesn't put the cursor back where it was.
			if (pointer && options.takePointerWarp && !SDL_GetWindowRelativeMouseMode(window)) {
				float px = 0, py = 0;
				if (options.takePointerWarp(px, py)) {
					int ww = 1, wh = 1;
					SDL_GetWindowSize(window, &ww, &wh);
					SDL_WarpMouseInWindow(window, px * ww / 800.0f, py * wh / 600.0f);
				}
			}
			{
				const bool lockWanted = capture && SDL_GetWindowRelativeMouseMode(window);
				if (lockWanted != systemCursorLocked) {
					if (lockWanted) {
						int ww = 0, wh = 0;
						SDL_GetWindowSize(window, &ww, &wh);
						SDL_WarpMouseInWindow(window, ww * 0.5f, wh * 0.5f);
					}
					lockSystemCursor(lockWanted);
					systemCursorLocked = lockWanted;
					std::printf("mouse-capture: system cursor %s\n", lockWanted ? "locked" : "unlocked");
					std::fflush(stdout);
				}
			}
			{
				// Always on, one line per change (not per frame): says which flag blocks capture.
				const bool relative = SDL_GetWindowRelativeMouseMode(window);
				const bool grabbed = SDL_GetWindowMouseGrab(window);
				const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
				const char* why = options.pointerReason ? options.pointerReason() : nullptr;
				const int state = focused | (pointer << 1) | (menuUp << 2) | (relative << 3) | (grabbed << 4) | (fullscreen << 5)
					| (playing << 6) | (capture << 7) | (why ? static_cast<int>(why[0]) << 8 : 0);
				if (state != tracedCaptureState) {
					std::printf("mouse-capture: t=%.1f wanted=%d blocker=%s focused=%d pointer=%d(%s) menu=%d playing=%d actual relative=%d grabbed=%d fullscreen=%d\n",
						SDL_GetTicksNS() / 1e9, capture, mouseCaptureBlocker(playing, pointer, menuUp, focused), focused, pointer, why ? why : "-", menuUp,
						playing, relative, grabbed, fullscreen);
					std::fflush(stdout);
					tracedCaptureState = state;
				}
			}
			const Uint64 now = SDL_GetTicksNS();
			// The engine clamps frame time to 1/15 s (and at least 10 us).
			const float dt = std::fmax(1e-5f, std::fmin(static_cast<float>(now - last) / 1e9f, 1.0f / 15.0f));
			last = now;
			const bool* keys = shown.keyboard();
			const auto pressed = [keys](SDL_Scancode code) { return keys[code] ? 1.0f : 0.0f; };
			if (menuUp && options.gameControllers && options.onControllerInput) {
				if (const auto last = options.gameControllers->lastInput()) {
					options.onControllerInput(last->device, last->control, last->direction);
				}
			}
			if (playing && options.onPlayerInput && options.controls) {
				// The control maps (0x004b141a): on foot the infantry map, in a
				// vehicle the map of its category.
				const Uint32 buttons = shown.mouseButtons() | pressedButtons;
				input::GameControllers* pads = options.gameControllers;
				input::ControlMaps::Source down;
				down.down = [keys, buttons, pointer, pads](const input::Binding& b) {
					const std::string& control = b.control;
					if (const int slot = input::GameControllers::slotNamed(b.device); slot >= 0) {
						return pads && control.compare(0, 9, "IDButton_") == 0 && pads->down(slot, std::atoi(control.c_str() + 9));
					}
					if (control.compare(0, 9, "IDButton_") == 0) {
						const int button = std::atoi(control.c_str() + 9);
						return !pointer && (buttons & SDL_BUTTON_MASK(input::sdlMouseButton(button))) != 0;
					}
					const SDL_Scancode code = scancodeFor(control);
					return code != SDL_SCANCODE_UNKNOWN && keys[code];
				};
				// The controllers' axes (the mouse's come as motion events).
				down.axis = [pads](const input::Binding& b) {
					const int slot = input::GameControllers::slotNamed(b.device);
					if (!pads || slot < 0 || b.control.compare(0, 7, "IDAxis_") != 0) {
						return 0.0f;
					}
					return pads->axis(slot, std::atoi(b.control.c_str() + 7));
				};
				down.pov = [pads](const input::Binding& b) {
					const int slot = input::GameControllers::slotNamed(b.device);
					return pads && slot >= 0 && b.control.compare(0, 6, "IDPov_") == 0
						? pads->pov(slot, std::atoi(b.control.c_str() + 6))
						: -1;
				};
				// The keys' ramp on the axes (0x00680990) runs on the frame's time.
				down.seconds = dt;
				ViewerOptions::PlayerView view;
				const bool driving = options.playerView && options.playerView(view) && view.driving;
				if (drivingNow && !driving) pitch = clampLookPitch(pitch, firstPerson, false);
				drivingNow = driving;
				if (options.takeAimKick) {
					// Not the soldier's limit while driving: it pinned the pilot's pitch input (the gunship's nose-up).
					pitch = clampLookPitch(pitch + options.takeAimKick() / 57.29578f, firstPerson, driving);
				}
				static const char* kVehicleMaps[] = {input::ControlMaps::kLand, input::ControlMaps::kSea,
					input::ControlMaps::kAir, input::ControlMaps::kHelicopter};
				const std::string map = !driving ? input::ControlMaps::kInfantry
					: view.vehicleCategory >= 0 && view.vehicleCategory < 4 ? kVehicleMaps[view.vehicleCategory]
																			 : input::ControlMaps::kLand;
				const input::ControlMaps& c = *options.controls;
				ViewerOptions::PlayerInput input;
				input.throttle = c.value(map, "c_PIThrottle", down);
				input.strafe = c.value(map, "c_PIYaw", down);
				input.sprint = c.pressed(map, "c_PISprint", down);
				if (driving) {
					input.pitch = c.value(map, "c_PIPitch", down);
					input.roll = c.value(map, "c_PIRoll", down);
				}
				if (pads) {
					// A stick on the look inputs: a rate, not a distance.
					const float scale = options.mouseScale ? options.mouseScale() : 1.0f;
					const float up = options.invertMouse && options.invertMouse() ? -1.0f : 1.0f;
					turn += c.value(map, "c_PIMouseLookX", down) * 120.0f * dt * scale;
					pitch = clampLookPitch(pitch - c.value(map, "c_PIMouseLookY", down) * 2.0944f * dt * scale * up, firstPerson, driving);
				}
				input.turnDegrees = turn;
				input.aimPitchDegrees = pitch * 57.29578f;
				input.fire = c.pressed(map, "c_PIFire", down);
				input.altFire = c.pressed(map, "c_PIAltFire", down);
				input.flareFire = c.pressed(map, "c_PIFlareFire", down);
				input.toggleWeapon = c.pressed(map, "c_PIToggleWeapon", down);
				input.use = c.pressed(map, "c_PIUse", down);
				input.toggleFireRate = c.pressed(map, "c_PIToggleFireRate", down);
				input.reload = c.pressed(map, "c_PIReload", down);
				input.mouseLook = c.pressed(map, "c_PIMouseLook", down);
				input.scoreboard = c.pressed(map, "c_GIShowScoreboard", down);
				input.commanderScreen = c.pressed(map, "c_GILeader", down);
				input.ctrl = c.pressed(map, "c_GILeftCtrl", down) || c.pressed(map, "c_GIRightCtrl", down);
				input.shift = c.pressed(map, "c_GILeftShift", down) || c.pressed(map, "c_GIRightShift", down);
				input.crouch = c.pressed(map, "c_PICrouch", down);
				input.lie = c.pressed(map, "c_PILie", down);
				input.jump = c.pressed(map, "c_PIAction", down);
				input.toggleCamera = c.pressed(map, "c_PIToggleCamera", down);
				// Profiles saved by older native builds lost this unrecognised
				// constant. Keep C usable until the player rebinds the camera.
				if (!c.hasBinding(map, "c_PIToggleCamera")) {
					input.toggleCamera = keys[SDL_SCANCODE_C];
				}
				for (int mode = 1; mode <= 4; ++mode) {
					if (c.pressed(map, "c_PICameraMode" + std::to_string(mode), down)) {
						input.camera = mode;
					}
				}
				input.voipPushToTalk = c.pressed(map, "c_GIVoipPushToTalk", down);
				input.voipLeaderChannel = c.pressed(map, "c_GIVoipUseLeaderChannel", down);
				input.sayAll = c.pressed(map, "c_GISayAll", down);
				input.sayTeam = c.pressed(map, "c_GISayTeam", down);
				input.saySquad = c.pressed(map, "c_GISaySquad", down);
				input.radioComm = c.pressed(map, "c_GIRadioComm", down);
				input.tacticalComm = c.pressed(map, "c_GITacticalComm", down);
				input.mapSize = c.pressed(map, "c_GIMapSize", down);
				input.mapZoom = c.pressed(map, "c_GIMapZoom", down);
				input.map3d = c.pressed(map, "c_GI3dMap", down);
				input.deployment = c.pressed(map, "c_GIEnter", down);
				input.yes = c.pressed(map, "c_GIYes", down);
				input.no = c.pressed(map, "c_GINo", down);
				for (int item = 0; item < 9; ++item) {
					if (c.pressed(map, "c_PIWeaponSelect" + std::to_string(item + 1), down)) {
						input.item = item + 1;
					}
				}
				for (int seat = 0; seat < 8; ++seat) {
					if (c.pressed(map, "c_PIPositionSelect" + std::to_string(seat + 1), down)) {
						input.seat = seat + 1;
					}
				}
				// The wheel's axis-to-trigger bindings (Controls.con's
				// "addAxisToTriggerMapping c_PINextItem c_PIPrevItem IDFMouse
				// IDAxis_2": the axis's positive way the first, its negative
				// the second): a frame it turned presses one.
				const auto wheelTo = [&](const char* name) {
					const input::ControlMaps::Map* m = c.find(map);
					if (!m || !m->count(name)) {
						return false;
					}
					for (const input::Binding& b : m->at(name)) {
						if (b.kind == input::Binding::Kind::Axis && !b.hold && b.device == "IDFMouse" && b.control == "IDAxis_2"
							&& wheelSteps * b.sign > 0) {
							return true;
						}
					}
					return false;
				};
				input.itemStep = wheelTo("c_PINextItem") ? 1 : wheelTo("c_PIPrevItem") ? -1 : 0;
				wheelSteps = 0;
				options.onPlayerInput(input);
				turn = 0;
			} else if (playing && options.onPlayerInput) {
				ViewerOptions::PlayerInput input;
				input.throttle = pressed(SDL_SCANCODE_W) - pressed(SDL_SCANCODE_S);
				input.strafe = pressed(SDL_SCANCODE_D) - pressed(SDL_SCANCODE_A);
				input.sprint = keys[SDL_SCANCODE_LSHIFT];
				input.turnDegrees = turn;
				input.aimPitchDegrees = pitch * 57.29578f;
				input.fire = ((shown.mouseButtons() | pressedButtons) & SDL_BUTTON_LMASK) != 0 && !pointer;
				input.altFire = ((shown.mouseButtons() | pressedButtons) & SDL_BUTTON_RMASK) != 0 && !pointer;
				input.flareFire = keys[SDL_SCANCODE_X];
				input.toggleWeapon = keys[SDL_SCANCODE_F];
				input.use = keys[SDL_SCANCODE_E];
				input.reload = keys[SDL_SCANCODE_R];
				input.mouseLook = keys[SDL_SCANCODE_LCTRL];
				input.scoreboard = keys[SDL_SCANCODE_TAB];
				input.crouch = keys[SDL_SCANCODE_LCTRL];
				input.lie = keys[SDL_SCANCODE_Z];
				input.jump = keys[SDL_SCANCODE_SPACE];
				input.toggleCamera = keys[SDL_SCANCODE_C];
				input.deployment = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER];
				for (int mode = 0; mode < 4; ++mode) {
					if (keys[SDL_SCANCODE_F9 + mode]) input.camera = mode + 1;
				}
				input.commanderScreen = keys[SDL_SCANCODE_CAPSLOCK];
				input.ctrl = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
				input.shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
				input.sayAll = keys[SDL_SCANCODE_K];
				input.sayTeam = keys[SDL_SCANCODE_L];
				input.mapSize = keys[SDL_SCANCODE_M];
				input.mapZoom = keys[SDL_SCANCODE_N];
				input.map3d = keys[SDL_SCANCODE_LALT];
				input.yes = keys[SDL_SCANCODE_DELETE];
				input.no = keys[SDL_SCANCODE_END];
				for (int item = 0; item < 9; ++item) {
					if (keys[SDL_SCANCODE_1 + item]) {
						input.item = item + 1;
					}
				}
				for (int seat = 0; seat < 8; ++seat) {
					if (keys[SDL_SCANCODE_F1 + seat]) {
						input.seat = seat + 1;
					}
				}
				input.itemStep = wheelSteps > 0 ? 1 : wheelSteps < 0 ? -1 : 0; // up the next item, down the previous
				wheelSteps = 0;
				options.onPlayerInput(input);
				turn = 0;
			}
			const int elapsedTicks = ticks.advance(static_cast<double>(now - start) / 1e9);
			if (elapsedTicks > 0 && options.onTicks) {
				options.onTicks(elapsedTicks);
			}
			ViewerOptions::PlayerView view;
			{
				int w = 1, h = 1;
				SDL_GetWindowSizeInPixels(window, &w, &h);
				view.aspect = static_cast<float>(w) / static_cast<float>(std::max(h, 1));
			}
			firstPerson = false;
			if (playing && options.playerView(view) && view.firstPerson) {
				// First person: the game's camera.
				firstPerson = true;
				camera.position = {view.eye[0], view.eye[1], view.eye[2]};
				camera.yaw = view.eyeYaw;
				camera.pitch = view.eyePitch;
				camera.fovY = view.fovY;
				camera.roll = view.eyeRoll;
				camera.nearPlane = kFirstPersonNear;
			} else if (playing && options.playerView(view) && view.external) {
				// Third person placed by the game.
				camera.position = {view.eye[0], view.eye[1], view.eye[2]};
				camera.yaw = view.eyeYaw;
				camera.pitch = view.eyePitch;
				camera.fovY = view.fovY;
				camera.roll = view.eyeRoll;
				camera.nearPlane = render::Camera {}.nearPlane;
			} else if (playing && options.playerView(view)) {
				// Third person: behind and above the soldier.
				camera.fovY = view.chaseFovY > 0 ? view.chaseFovY : 1.0f;
				camera.roll = 0;
				camera.nearPlane = render::Camera {}.nearPlane;
				camera.yaw = view.yawDegrees * 3.14159265f / 180.0f;
				camera.pitch = pitch;
				if (view.driving) {
					// Chase camera behind and above the vehicle.
					const render::Vec3f roof {view.x, view.y + 2.5f, view.z};
					camera.position = roof - camera.forward() * 9.0f;
				} else {
					const render::Vec3f head {view.x, view.y + 1.7f, view.z};
					camera.position = head - camera.forward() * 4.0f + camera.right() * 0.6f;
				}
			}
			// The projectile follows fixed 30 Hz simulation samples. Interpolate
			// only the ridden missile camera at the actual render phase.
			MissileViewSmoothing::Pose raw {{camera.position.x, camera.position.y, camera.position.z}, camera.yaw, camera.pitch};
			const auto smooth = missileSmoothing.sample(view.firstPerson ? view.projectileSerial : 0, elapsedTicks > 0,
				static_cast<float>(ticks.interpolation(static_cast<double>(now - start) / 1e9)), raw);
			if (view.firstPerson && view.projectileSerial) {
				camera.position = {smooth.position[0], smooth.position[1], smooth.position[2]};
				camera.yaw = smooth.yaw;
				camera.pitch = smooth.pitch;
			}
			const float speed = (keys[SDL_SCANCODE_LSHIFT] ? 200.0f : 40.0f) * dt;
			const auto key = [keys](SDL_Scancode code) { return keys[code] ? 1.0f : 0.0f; };
			if (!playing) camera.position = camera.position + camera.forward() * ((key(SDL_SCANCODE_W) - key(SDL_SCANCODE_S)) * speed) +
				camera.right() * ((key(SDL_SCANCODE_D) - key(SDL_SCANCODE_A)) * speed) +
				render::Vec3f {0, (key(SDL_SCANCODE_E) - key(SDL_SCANCODE_Q)) * speed, 0};

			SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
			SDL_GPUTexture* swapchain = nullptr;
			Uint32 width = 0;
			Uint32 height = 0;
			if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchain, &width, &height) || !swapchain) {
				SDL_SubmitGPUCommandBuffer(commands);
				continue;
			}
			gpu->ensureDepth(target, width, height);
			if (options.onCamera) {
				const render::Vec3f forward = camera.forward();
				const float position[3] = {camera.position.x, camera.position.y, camera.position.z};
				const float ahead[3] = {forward.x, forward.y, forward.z};
				options.onCamera(position, ahead);
			}
			if (options.onView) {
				const float position[3] = {camera.position.x, camera.position.y, camera.position.z};
				options.onView(position, camera.yaw, camera.pitch, camera.fovY,
					height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f);
			}
			level.syncObjects();
			level.draw(commands, swapchain, target, camera, sources.lights);
			SDL_SubmitGPUCommandBuffer(commands);
			if (!shown.shot.empty()) {
				// The same frame again, off screen, saved.
				auto offscreen = gpu->createOffscreenTarget(width, height);
				SDL_GPUCommandBuffer* again = SDL_AcquireGPUCommandBuffer(gpu->device());
				level.draw(again, offscreen.color, offscreen, camera, sources.lights);
				SDL_SubmitGPUCommandBuffer(again);
				std::vector<uint8_t> pixels;
				const bool ok = gpu->readPixels(offscreen, pixels) &&
					render::writePng(shown.shot, offscreen.width, offscreen.height, pixels);
				gpu->releaseTarget(offscreen);
				std::printf("%s %s\n", ok ? "wrote" : "failed to write", shown.shot.c_str());
				shown.shot.clear();
			}
		}
		gpu->releaseTarget(target);
		SDL_SetWindowRelativeMouseMode(window, false);
		SDL_SetWindowMouseGrab(window, false);
		lockSystemCursor(false);
		SDL_StopTextInput(window);
	}
	return 0;
}

} // namespace

Window::Window() : impl(std::make_unique<Impl>()) {}

Window::~Window()
{
	impl->gpu.reset();
	if (impl->window) {
		SDL_DestroyWindow(impl->window);
	}
	// Only the video SDL_Init started: SDL_Quit would also close the
	// audio streams the voice chat's thread and the menu's movies still
	// use (SDL_INIT_AUDIO is theirs, counted per subsystem).
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

std::unique_ptr<Window> Window::open(int width, int height)
{
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return nullptr;
	}
	std::unique_ptr<Window> window(new Window());
	window->impl->opened = SDL_GetTicksNS();
	window->impl->window =
		SDL_CreateWindow("Battlefield 2142", width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!window->impl->window) {
		std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		return nullptr;
	}
	// Optional 128x128 RGBA icon (local launcher supplies the retail one); on macOS this sets the Dock icon.
	if (const char* iconPath = std::getenv("BF2142_ICON")) {
		if (std::FILE* f = std::fopen(iconPath, "rb")) {
			static unsigned char pixels[128 * 128 * 4];
			if (std::fread(pixels, 1, sizeof pixels, f) == sizeof pixels)
				if (SDL_Surface* icon = SDL_CreateSurfaceFrom(128, 128, SDL_PIXELFORMAT_RGBA32, pixels, 128 * 4)) {
					SDL_SetWindowIcon(window->impl->window, icon);
					SDL_DestroySurface(icon);
				}
			std::fclose(f);
		}
	}
	std::string error;
	window->impl->gpu = render::GpuContext::create(window->impl->window, error);
	if (!window->impl->gpu) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return nullptr;
	}
	return window;
}

bool Window::closed() const { return impl->closed; }

std::vector<std::string> Window::displayModes() const
{
	std::vector<std::string> modes;
	int count = 0;
	SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(impl->window), &count);
	for (int i = 0; list && i < count; ++i) {
		const std::string mode = std::to_string(list[i]->w) + "x" + std::to_string(list[i]->h) + "@" +
			std::to_string(static_cast<int>(std::lround(list[i]->refresh_rate))) + "Hz";
		if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
			modes.push_back(mode);
		}
	}
	SDL_free(list);
	return modes;
}

void Window::setSize(int width, int height) { SDL_SetWindowSize(impl->window, width, height); }

struct Window::MenuScreen::State {
	Window& window;
	render::LevelRenderer level;
	const render::LevelSources& sources;
	render::GpuContext::Target target;
};

Window::MenuScreen::MenuScreen(Window& window, const render::LevelSources& sources)
	: m_state(new State {window, render::LevelRenderer(*window.impl->gpu), sources, {}})
{
	m_state->level.build(sources);
}

Window::MenuScreen::~MenuScreen() { m_state->window.impl->gpu->releaseTarget(m_state->target); }

bool Window::MenuScreen::frame()
{
	Window::Impl& shown = *m_state->window.impl;
	shown.runScript();
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_EVENT_QUIT) {
			shown.closed = true;
		}
	}
	if (shown.closed) {
		return false;
	}
	SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(shown.gpu->device());
	SDL_GPUTexture* swapchain = nullptr;
	Uint32 width = 0, height = 0;
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, shown.window, &swapchain, &width, &height) || !swapchain) {
		SDL_SubmitGPUCommandBuffer(commands);
		return true;
	}
	shown.gpu->ensureDepth(m_state->target, width, height);
	m_state->level.draw(commands, swapchain, m_state->target, m_state->level.suggestedCamera(), m_state->sources.lights);
	SDL_SubmitGPUCommandBuffer(commands);
	return true;
}

int runViewer(const render::LevelSources& sources, const ViewerOptions& options, Window* window)
{
	if (!options.screenshot.empty()) {
		if (!window && !SDL_Init(SDL_INIT_VIDEO)) {
			std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
			return 1;
		}
		const int result = screenshot(sources, options);
		if (!window) {
			SDL_QuitSubSystem(SDL_INIT_VIDEO); // not the voice chat's audio (see ~Window)
		}
		return result;
	}
	if (window) {
		return interactive(sources, options, *window->impl);
	}
	std::unique_ptr<Window> own = Window::open(options.width, options.height);
	return own ? interactive(sources, options, *own->impl) : 1;
}

// --- movies

namespace {

// Uploads `rgba` into `texture` (made or remade at its size), clears
// `target` to black and blits the picture onto it, scaled to fit.
void drawMovieFrame(SDL_GPUDevice* device, SDL_GPUCommandBuffer* commands, SDL_GPUTexture*& texture,
	SDL_GPUTransferBuffer*& transfer, uint32_t& textureWidth, uint32_t& textureHeight, const uint8_t* rgba,
	uint32_t width, uint32_t height, SDL_GPUTexture* target, uint32_t targetWidth, uint32_t targetHeight)
{
	if (!texture || textureWidth != width || textureHeight != height) {
		if (texture) {
			SDL_ReleaseGPUTexture(device, texture);
			SDL_ReleaseGPUTransferBuffer(device, transfer);
		}
		SDL_GPUTextureCreateInfo info {};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = width;
		info.height = height;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		texture = SDL_CreateGPUTexture(device, &info);
		SDL_GPUTransferBufferCreateInfo transferInfo {};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = width * height * 4;
		transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
		textureWidth = width;
		textureHeight = height;
	}
	if (rgba) {
		void* mapped = SDL_MapGPUTransferBuffer(device, transfer, true);
		std::memcpy(mapped, rgba, static_cast<size_t>(width) * height * 4);
		SDL_UnmapGPUTransferBuffer(device, transfer);
		SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
		SDL_GPUTextureTransferInfo from {};
		from.transfer_buffer = transfer;
		SDL_GPUTextureRegion to {};
		to.texture = texture;
		to.w = width;
		to.h = height;
		to.d = 1;
		SDL_UploadToGPUTexture(copy, &from, &to, true);
		SDL_EndGPUCopyPass(copy);
	}
	SDL_GPUColorTargetInfo clear {};
	clear.texture = target;
	clear.load_op = SDL_GPU_LOADOP_CLEAR;
	clear.store_op = SDL_GPU_STOREOP_STORE;
	clear.clear_color = {0, 0, 0, 1};
	SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, &clear, 1, nullptr));
	const float scale = std::min(static_cast<float>(targetWidth) / static_cast<float>(width),
		static_cast<float>(targetHeight) / static_cast<float>(height));
	const auto w = static_cast<uint32_t>(static_cast<float>(width) * scale);
	const auto h = static_cast<uint32_t>(static_cast<float>(height) * scale);
	SDL_GPUBlitInfo blit {};
	blit.source.texture = texture;
	blit.source.w = width;
	blit.source.h = height;
	blit.destination.texture = target;
	blit.destination.x = (targetWidth - w) / 2;
	blit.destination.y = (targetHeight - h) / 2;
	blit.destination.w = w;
	blit.destination.h = h;
	blit.load_op = SDL_GPU_LOADOP_LOAD;
	blit.filter = SDL_GPU_FILTER_LINEAR;
	SDL_BlitGPUTexture(commands, &blit);
}

} // namespace

struct Window::MovieScreen::State {
	Window& window;
	SDL_GPUTexture* texture = nullptr;
	SDL_GPUTransferBuffer* transfer = nullptr;
	uint32_t width = 0, height = 0;
	SDL_AudioStream* audio = nullptr;
	int audioRate = 0;
	bool escape = false, anyKey = false;
};

Window::MovieScreen::MovieScreen(Window& window) : m_state(new State {window}) {}

Window::MovieScreen::~MovieScreen()
{
	SDL_GPUDevice* device = m_state->window.impl->gpu->device();
	if (m_state->texture) {
		SDL_ReleaseGPUTexture(device, m_state->texture);
		SDL_ReleaseGPUTransferBuffer(device, m_state->transfer);
	}
	if (m_state->audio) {
		SDL_DestroyAudioStream(m_state->audio);
	}
}

bool Window::MovieScreen::frame(const uint8_t* rgba, int width, int height)
{
	Window::Impl& shown = *m_state->window.impl;
	shown.runScript();
	m_state->escape = m_state->anyKey = false;
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_EVENT_QUIT) {
			shown.closed = true;
		} else if (event.type == SDL_EVENT_KEY_DOWN) {
			m_state->anyKey = true;
			m_state->escape = m_state->escape || event.key.scancode == SDL_SCANCODE_ESCAPE;
		} else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
			m_state->anyKey = true;
		}
	}
	if (shown.closed) {
		return false;
	}
	SDL_HideCursor(); // as 0x0040bd90 does while a movie plays
	SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(shown.gpu->device());
	SDL_GPUTexture* swapchain = nullptr;
	Uint32 targetWidth = 0, targetHeight = 0;
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, shown.window, &swapchain, &targetWidth, &targetHeight)
		|| !swapchain || width <= 0 || height <= 0) {
		SDL_SubmitGPUCommandBuffer(commands);
		return true;
	}
	drawMovieFrame(shown.gpu->device(), commands, m_state->texture, m_state->transfer, m_state->width, m_state->height, rgba,
		static_cast<uint32_t>(width), static_cast<uint32_t>(height), swapchain, targetWidth, targetHeight);
	SDL_SubmitGPUCommandBuffer(commands);
	if (!shown.shot.empty()) {
		shown.shot.clear(); // a scripted shot of a movie: not kept (saveMovieFrame does those)
	}
	return true;
}

bool Window::MovieScreen::escape() const { return m_state->escape; }
bool Window::MovieScreen::anyKey() const { return m_state->anyKey; }

void Window::MovieScreen::queueAudio(const std::vector<float>& samples, int rate)
{
	if (samples.empty() || rate <= 0) {
		return;
	}
	if (!m_state->audio || m_state->audioRate != rate) {
		if (m_state->audio) {
			SDL_DestroyAudioStream(m_state->audio);
		}
		SDL_InitSubSystem(SDL_INIT_AUDIO);
		SDL_AudioSpec spec {SDL_AUDIO_F32, 2, rate};
		m_state->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		m_state->audioRate = rate;
		if (m_state->audio) {
			SDL_ResumeAudioStreamDevice(m_state->audio);
		}
	}
	if (m_state->audio) {
		SDL_PutAudioStreamData(m_state->audio, samples.data(), static_cast<int>(samples.size() * sizeof(float)));
	}
}

double Window::MovieScreen::audioQueued() const
{
	if (!m_state->audio || m_state->audioRate <= 0) {
		return 0;
	}
	return static_cast<double>(SDL_GetAudioStreamQueued(m_state->audio)) / (sizeof(float) * 2.0 * m_state->audioRate);
}

AudioOut::~AudioOut()
{
	if (m_stream) {
		SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(m_stream));
	}
}

void AudioOut::queue(const std::vector<float>& samples, int rate)
{
	if (samples.empty() || rate <= 0) {
		return;
	}
	if (!m_stream || m_rate != rate) {
		if (m_stream) {
			SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(m_stream));
		}
		SDL_InitSubSystem(SDL_INIT_AUDIO);
		SDL_AudioSpec spec {SDL_AUDIO_F32, 2, rate};
		m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		m_rate = rate;
		if (m_stream) {
			SDL_ResumeAudioStreamDevice(static_cast<SDL_AudioStream*>(m_stream));
		}
	}
	if (m_stream) {
		SDL_PutAudioStreamData(static_cast<SDL_AudioStream*>(m_stream), samples.data(), static_cast<int>(samples.size() * sizeof(float)));
	}
}

void AudioOut::clear()
{
	if (m_stream) {
		SDL_ClearAudioStream(static_cast<SDL_AudioStream*>(m_stream));
	}
}

double AudioOut::queued() const
{
	if (!m_stream || m_rate <= 0) {
		return 0;
	}
	return static_cast<double>(SDL_GetAudioStreamQueued(static_cast<SDL_AudioStream*>(m_stream))) / (sizeof(float) * 2.0 * m_rate);
}

bool saveMovieFrame(const uint8_t* rgba, int frameWidth, int frameHeight, int width, int height, const std::string& png)
{
	const bool started = SDL_WasInit(SDL_INIT_VIDEO) != 0 || SDL_Init(SDL_INIT_VIDEO);
	if (!started) {
		std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return false;
	}
	std::string error;
	auto gpu = render::GpuContext::create(nullptr, error);
	if (!gpu) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return false;
	}
	auto target = gpu->createOffscreenTarget(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
	SDL_GPUTexture* texture = nullptr;
	SDL_GPUTransferBuffer* transfer = nullptr;
	uint32_t textureWidth = 0, textureHeight = 0;
	SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
	drawMovieFrame(gpu->device(), commands, texture, transfer, textureWidth, textureHeight, rgba,
		static_cast<uint32_t>(frameWidth), static_cast<uint32_t>(frameHeight), target.color, target.width, target.height);
	SDL_SubmitGPUCommandBuffer(commands);
	std::vector<uint8_t> pixels;
	const bool ok = gpu->readPixels(target, pixels) && render::writePng(png, target.width, target.height, pixels);
	SDL_ReleaseGPUTexture(gpu->device(), texture);
	SDL_ReleaseGPUTransferBuffer(gpu->device(), transfer);
	gpu->releaseTarget(target);
	std::printf("%s %s\n", ok ? "wrote" : "failed to write", png.c_str());
	return ok;
}

} // namespace dice::hfe
