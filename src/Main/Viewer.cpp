#include "Viewer.h"

#include "Render/GpuContext.h"
#include "Render/LevelRenderer.h"
#include "Render/PngWriter.h"
#include "Game/TickTimer.h"
#include "Input/ControlMap.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include <sstream>
#include <set>
#include <cstdlib>
#include <cstdio>

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
		return SDL_GetScancodeFromName(name.c_str());
	};
	const auto keyEvent = [&](SDL_Scancode code, bool down) {
		SDL_Event event {};
		event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.windowID = id;
		event.key.scancode = code;
		event.key.key = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
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

	auto target = gpu->createOffscreenTarget(static_cast<uint32_t>(options.width), static_cast<uint32_t>(options.height));
	SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(gpu->device());
	level.syncObjects();
	level.draw(commands, target.color, target, camera, sources.lights);
	SDL_SubmitGPUCommandBuffer(commands);
	std::vector<uint8_t> pixels;
	const bool ok = gpu->readPixels(target, pixels) &&
		render::writePng(options.screenshot, target.width, target.height, pixels);
	gpu->releaseTarget(target);
	std::printf("%s %s\n", ok ? "wrote" : "failed to write", options.screenshot.c_str());
	return ok ? 0 : 1;
}

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
		if (options.onMenuInput) {
			SDL_StartTextInput(window);
		}
		if (options.onMenuInput && !options.menuActive) {
			std::printf("the menu has the mouse and keyboard; close the window to quit\n");
		} else if (playing) {
			SDL_SetWindowRelativeMouseMode(window, true);
			std::printf("WASD run, shift sprints, the mouse looks around, left button fires, Esc %s\n",
				options.onEscape ? "shows the menu" : "quits");
		} else {
			std::printf("WASD/QE move, right mouse button looks around, shift is faster, Esc quits\n");
		}
		float pitch = -0.2f;
		float turn = 0;

		render::GpuContext::Target target;
		Uint64 last = SDL_GetTicksNS();
		const Uint64 start = last;
		TickTimer ticks(10, 30.0f); // as BF2Engine
		ticks.advance(0);
		bool running = true;
		bool pointer = false; // the mouse is a pointer (a menu is up)
		while (running) {
			const bool wanted = options.wantsPointer && options.wantsPointer();
			if (playing && wanted != pointer) {
				pointer = wanted;
				SDL_SetWindowRelativeMouseMode(window, !pointer);
			}
			SDL_Event event;
			int windowWidth = 1, windowHeight = 1;
			SDL_GetWindowSize(window, &windowWidth, &windowHeight);
			const auto menu = [&](ViewerOptions::MenuInput input) {
				options.onMenuInput(input);
			};
			const bool menuUp = options.onMenuInput && (!options.menuActive || options.menuActive());
			if (options.wantsQuit && options.wantsQuit()) {
				running = false;
			}
			shown.runScript();
			while (SDL_PollEvent(&event)) {
				if (event.type == SDL_EVENT_QUIT) {
					running = false;
					shown.closed = true;
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
					menu({ViewerOptions::MenuInput::Kind::Wheel, 0, 0, static_cast<int>(event.wheel.y)});
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
				} else if (event.type == SDL_EVENT_MOUSE_MOTION && playing && !pointer) {
					const float scale = options.mouseScale ? options.mouseScale() : 1.0f;
					const float up = options.invertMouse && options.invertMouse() ? -1.0f : 1.0f;
					turn += event.motion.xrel * 0.15f * scale;
					pitch = std::fmax(-1.2f, std::fmin(1.0f, pitch - event.motion.yrel * 0.003f * scale * up));
				} else if (event.type == SDL_EVENT_MOUSE_MOTION && (event.motion.state & SDL_BUTTON_RMASK)) {
					camera.yaw += event.motion.xrel * 0.004f;
					camera.pitch = std::fmax(-1.5f, std::fmin(1.5f, camera.pitch - event.motion.yrel * 0.004f));
				}
			}
			const Uint64 now = SDL_GetTicksNS();
			// The engine clamps frame time to 1/15 s (and at least 10 us).
			const float dt = std::fmax(1e-5f, std::fmin(static_cast<float>(now - last) / 1e9f, 1.0f / 15.0f));
			last = now;
			const bool* keys = shown.keyboard();
			const auto pressed = [keys](SDL_Scancode code) { return keys[code] ? 1.0f : 0.0f; };
			if (playing && options.onPlayerInput && options.controls) {
				// The control maps: the vehicle's map while driving (land;
				// provisional: aircraft and ships use theirs too), else on foot's.
				const Uint32 buttons = shown.mouseButtons();
				const input::ControlMaps::IsDown down = [keys, buttons, pointer](const std::string& control) {
					if (control.compare(0, 9, "IDButton_") == 0) {
						const int button = std::atoi(control.c_str() + 9);
						return !pointer && (buttons & SDL_BUTTON_MASK(button + 1)) != 0;
					}
					const SDL_Scancode code = scancodeFor(control);
					return code != SDL_SCANCODE_UNKNOWN && keys[code];
				};
				ViewerOptions::PlayerView view;
				const bool driving = options.playerView && options.playerView(view) && view.driving;
				const std::string map = driving ? input::ControlMaps::kLand : input::ControlMaps::kInfantry;
				const input::ControlMaps& c = *options.controls;
				ViewerOptions::PlayerInput input;
				input.throttle = c.value(map, "c_PIThrottle", down);
				input.strafe = c.value(map, "c_PIYaw", down);
				input.sprint = c.pressed(map, "c_PISprint", down);
				input.turnDegrees = turn;
				input.aimPitchDegrees = pitch * 57.29578f;
				input.fire = c.pressed(map, "c_PIFire", down);
				input.altFire = c.pressed(map, "c_PIAltFire", down);
				input.use = c.pressed(map, "c_PIUse", down);
				input.scoreboard = c.pressed(map, "c_GIShowScoreboard", down);
				input.commanderScreen = c.pressed(map, "c_GILeader", down);
				input.voipPushToTalk = c.pressed(map, "c_GIVoipPushToTalk", down);
				input.voipLeaderChannel = c.pressed(map, "c_GIVoipUseLeaderChannel", down);
				for (int item = 0; item < 8; ++item) {
					if (c.pressed(map, "c_PIWeaponSelect" + std::to_string(item + 1), down)) {
						input.item = item + 1;
					}
				}
				for (int seat = 0; seat < 8; ++seat) {
					if (c.pressed(map, "c_PIPositionSelect" + std::to_string(seat + 1), down)) {
						input.seat = seat + 1;
					}
				}
				options.onPlayerInput(input);
				turn = 0;
			} else if (playing && options.onPlayerInput) {
				ViewerOptions::PlayerInput input;
				input.throttle = pressed(SDL_SCANCODE_W) - pressed(SDL_SCANCODE_S);
				input.strafe = pressed(SDL_SCANCODE_D) - pressed(SDL_SCANCODE_A);
				input.sprint = keys[SDL_SCANCODE_LSHIFT];
				input.turnDegrees = turn;
				input.aimPitchDegrees = pitch * 57.29578f;
				input.fire = (shown.mouseButtons() & SDL_BUTTON_LMASK) != 0 && !pointer;
				input.altFire = (shown.mouseButtons() & SDL_BUTTON_RMASK) != 0 && !pointer;
				input.use = keys[SDL_SCANCODE_E];
				input.scoreboard = keys[SDL_SCANCODE_TAB];
				input.commanderScreen = keys[SDL_SCANCODE_CAPSLOCK];
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
				options.onPlayerInput(input);
				turn = 0;
			}
			const int elapsedTicks = ticks.advance(static_cast<double>(now - start) / 1e9);
			if (elapsedTicks > 0 && options.onTicks) {
				options.onTicks(elapsedTicks);
			}
			ViewerOptions::PlayerView view;
			if (playing && options.playerView(view)) {
				// Third person: behind and above the soldier.
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
	SDL_Quit();
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
			SDL_Quit();
		}
		return result;
	}
	if (window) {
		return interactive(sources, options, *window->impl);
	}
	std::unique_ptr<Window> own = Window::open(options.width, options.height);
	return own ? interactive(sources, options, *own->impl) : 1;
}

} // namespace dice::hfe
