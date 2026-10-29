// The level viewer: a window with a free camera, or one rendered frame
// saved as a PNG (for checking the renderer without a display).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::render {
struct LevelSources;
}
namespace dice::hfe::input {
class ControlMaps;
class GameControllers;
}

namespace dice::hfe {

struct ViewerOptions {
	std::string screenshot; // render one frame to this PNG instead of opening a window
	int width = 1280;
	int height = 720;
	// "x/y/z/yaw/pitch" (degrees); default: looking at the placed objects.
	std::string camera;
	// Called each frame with the number of simulation ticks (30 per second)
	// that passed, as the engine's frame function advances the world.
	std::function<void(int ticks)> onTicks;

	// Playing: the keyboard and mouse drive the player's soldier instead of
	// the free camera, which follows it.
	struct PlayerInput {
		float throttle = 0, strafe = 0, turnDegrees = 0;
		// In a vehicle: the map's c_PIPitch and c_PIRoll from keys, joystick
		// and gamepad axes and POV hats (-1..1; the mouse's own share comes as
		// turnDegrees and aimPitchDegrees). c_PIYaw is strafe, c_PIThrottle
		// throttle, from the same devices.
		float pitch = 0, roll = 0;
		float aimPitchDegrees = 0;
		bool sprint = false;
		bool fire = false;
		bool altFire = false; // the right button
		bool use = false; // enter or leave a vehicle
		bool toggleFireRate = false; // held: c_PIToggleFireRate (the weapon's next fire mode)
		bool scoreboard = false; // held: the scoreboard (Tab)
		bool commanderScreen = false; // pressed: the commander screen (Caps Lock)
		bool crouch = false;          // held: crouch (c_PICrouch, left Ctrl)
		bool lie = false;             // held: lie down (c_PILie, Z)
		bool jump = false;            // held: jump (c_PIAction, Space)
		bool voipPushToTalk = false;    // held: c_GIVoipPushToTalk (V)
		bool voipLeaderChannel = false; // held: c_GIVoipUseLeaderChannel (B)
		bool toggleCamera = false;      // held: c_PIToggleCameraMode (C)
		int seat = 0;           // F1..F8 held: that vehicle seat (1-based)
		int item = 0;            // 1..9 held: the weapon in that kit slot
		bool sayAll = false, sayTeam = false, saySquad = false; // c_GISayAll (K), c_GISayTeam (L), c_GISaySquad
		bool radioComm = false, tacticalComm = false; // held: c_GIRadioComm (Q), c_GITacticalComm (T)
	};
	struct PlayerView {
		float x = 0, y = 0, z = 0; // the soldier's feet
		float yawDegrees = 0;
		bool driving = false; // the position and yaw are the vehicle's
		// The vehicle's ObjectTemplate.vehicleCategory: 0 VCLand, 1 VCSea,
		// 2 VCAir, 3 VCHelicopter (its control map, 0x004b141a).
		int vehicleCategory = 0;
		// Given: the window's width over its height.
		float aspect = 16.0f / 9.0f;
		// First person: the camera itself (radians; yaw 0 along +z, pitch up
		// positive) and its vertical field of view.
		bool firstPerson = false;
		float eye[3] = {0, 0, 0};
		float eyeYaw = 0, eyePitch = 0;
		float fovY = 1.0f;
		float eyeRoll = 0; // radians (a seat camera's shakes)
		// Third person: the seat camera's worldFOV (0: the viewer's own).
		float chaseFovY = 0;
		// Third person placed by the game (a seat camera's external modes,
		// the death and spectator cameras): eye, eyeYaw, eyePitch, fovY and
		// eyeRoll hold it, drawn with the usual near plane.
		bool external = false;
	};
	std::function<void(const PlayerInput&)> onPlayerInput;
	std::function<bool(PlayerView&)> playerView; // false without a player
	// The recoil's kick to the aim since the last call (degrees, up
	// positive): the viewer adds it to its pitch, as the original adds it to
	// the soldier's own angles (GameWorld::takeAimKick).
	std::function<float()> takeAimKick;
	// Menus (the spawn screen): while it says so the mouse is a pointer,
	// and left clicks go to onClick in the HUD's 800 x 600 screen.
	std::function<bool()> wantsPointer;
	std::function<void(float x, float y)> onClick;
	// The right button's clicks and the pointer's moves (the HUD's
	// right-click map menus, button highlights), same screen.
	std::function<void(float x, float y)> onRightClick;
	std::function<void(float x, float y)> onPointerMove;

	// A Flash menu: all input goes to it (positions as fractions of the
	// window), Esc included; it asks to quit through wantsQuit.
	struct MenuInput {
		enum class Kind { Move, Down, Up, Wheel, Key, KeyUp, Text } kind;
		float x = 0, y = 0;   // 0..1 across the window
		int key = 0;          // Flash key code (Key, KeyUp; 0 for others); wheel steps (Wheel)
		std::string text;     // typed text, UTF-8 (Text)
	};
	std::function<void(const MenuInput&)> onMenuInput;
	// The HUD's text boxes (chat, the squad name): while wantsText says so,
	// typed text (Text) and pressed keys (Key: Flash codes, Enter 13,
	// Backspace 8, Esc 27) go to onHudText instead of the game.
	std::function<bool()> wantsText;
	std::function<void(const MenuInput&)> onHudText;
	std::function<bool()> wantsQuit;
	// A menu over a game: up while this says so (without it, always), and
	// Esc toggles it through onEscape instead of quitting.
	std::function<bool()> menuActive;
	std::function<void()> onEscape;
	// The mouse while playing (ControlSettings): how fast it turns (1 as
	// the game's default) and whether up looks down. Asked as it moves.
	std::function<float()> mouseScale;
	std::function<bool()> invertMouse;
	// Told where the camera is and looks each frame (position, forward),
	// before drawing: the game's level-of-detail choices go by it.
	std::function<void(const float position[3], const float forward[3])> onCamera;
	// The same camera in full (radians; yaw 0 along +z, pitch up positive),
	// with the frame's aspect: the 3D name tags project by it.
	std::function<void(const float position[3], float yaw, float pitch, float fovY, float aspect)> onView;
	// The player's keys (Settings/Controls.con); without them W/S/A/D,
	// shift, the mouse buttons, E, Tab, Caps Lock, 1-9 and F1-F8.
	const input::ControlMaps* controls = nullptr;
	// Joysticks and gamepads (IDFGameController_0..7): the window opens
	// them through SDL3 as they are attached and feeds their state here;
	// the control maps' bindings to them drive the player as the keys do.
	// An axis bound to c_PIMouseLookX/Y turns at up to 120 degrees a second
	// (the rewrite's rate; mouseScale applies).
	input::GameControllers* gameControllers = nullptr;
	// With a menu up: a controller's button pressed, or an axis pushed to
	// its end (the controls page's "press the input", 0x006aadf0).
	std::function<void(const std::string& device, const std::string& control, float direction)> onControllerInput;
};

// The game's window: one SDL window and GPU device from the menu through
// every map, so that changing levels does not close it.
class Window {
public:
	// Null (and a message) when there is no display or GPU.
	static std::unique_ptr<Window> open(int width, int height);
	~Window();
	Window(const Window&) = delete;
	Window& operator=(const Window&) = delete;

	// Whether it was closed (the run is over).
	bool closed() const;
	// Scripted input for unattended runs: "<seconds>:<command>" items
	// separated by spaces, timed from the window's opening:
	//   key:<name> (press and release; SDL key names, '_' for spaces),
	//   down:<name> / up:<name> (hold), click:<x>,<y> / move:<x>,<y>
	//   (fractions of the window), button:<n>,<0|1> (hold a mouse button),
	//   look:<dx>,<dy> (relative mouse), text:<text>, shot:<file.png>, quit.
	void setScript(const std::string& script);

	// The display's modes ("1280x720@60Hz", the largest first), and a new
	// size for the window.
	std::vector<std::string> displayModes() const;
	void setSize(int width, int height);

	// A menu drawn alone between other work (the loading screen while a
	// level loads): each frame() handles the window's events and draws
	// `sources.menu` over black. False once the window was closed.
	class MenuScreen {
	public:
		MenuScreen(Window& window, const render::LevelSources& sources);
		~MenuScreen();
		bool frame();

	private:
		struct State;
		std::unique_ptr<State> m_state;
	};

	// A movie drawn alone (the start-up movies, Movie/MovieQueue.h): each
	// frame() shows the picture scaled to fit over black and handles the
	// window's events. Its sound goes to the default playback device.
	class MovieScreen {
	public:
		explicit MovieScreen(Window& window);
		~MovieScreen();
		// False once the window was closed.
		bool frame(const uint8_t* rgba, int width, int height);
		// Pressed since the last frame(): Esc, and any key or button.
		bool escape() const;
		bool anyKey() const;
		// Interleaved stereo floats at `rate` Hz; and how many seconds of
		// them are still to play.
		void queueAudio(const std::vector<float>& samples, int rate);
		double audioQueued() const;

	private:
		struct State;
		std::unique_ptr<State> m_state;
	};

	struct Impl;
	std::unique_ptr<Impl> impl;

private:
	Window();
};

// Sound straight to the default playback device, outside the game's sound
// groups: a menu clip's movie (the Titan tutorial) plays its own sound as
// Bink does in RendDX9 (0x1011df20 picks BinkOpenDirectSound, and
// 0x1011e0d0 opens the file with BinkOpen(path, 0), no BINKNOSOUND, and
// sets no volume), stopped with the clip.
class AudioOut {
public:
	AudioOut() = default;
	~AudioOut();
	AudioOut(const AudioOut&) = delete;
	AudioOut& operator=(const AudioOut&) = delete;
	// Interleaved stereo floats at `rate` Hz, after what is queued.
	void queue(const std::vector<float>& samples, int rate);
	// Drops what is still to play.
	void clear();
	double queued() const; // seconds

private:
	void* m_stream = nullptr; // SDL_AudioStream
	int m_rate = 0;
};

// One movie frame as the movie screen draws it, rendered off screen at
// width x height and saved as a PNG (for checking without a display).
bool saveMovieFrame(const uint8_t* rgba, int frameWidth, int frameHeight, int width, int height, const std::string& png);

// Returns the process exit code. With a window it draws there (and leaves
// it open); without one it opens its own for this run.
int runViewer(const render::LevelSources& sources, const ViewerOptions& options, Window* window = nullptr);

} // namespace dice::hfe
