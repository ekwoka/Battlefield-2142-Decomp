// The level viewer: a window with a free camera, or one rendered frame
// saved as a PNG (for checking the renderer without a display).
#pragma once

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
		float aimPitchDegrees = 0;
		bool sprint = false;
		bool fire = false;
		bool altFire = false; // the right button
		bool use = false; // enter or leave a vehicle
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
	};
	std::function<void(const PlayerInput&)> onPlayerInput;
	std::function<bool(PlayerView&)> playerView; // false without a player
	// Menus (the spawn screen): while it says so the mouse is a pointer,
	// and left clicks go to onClick in the HUD's 800 x 600 screen.
	std::function<bool()> wantsPointer;
	std::function<void(float x, float y)> onClick;

	// A Flash menu: all input goes to it (positions as fractions of the
	// window), Esc included; it asks to quit through wantsQuit.
	struct MenuInput {
		enum class Kind { Move, Down, Up, Wheel, Key, KeyUp, Text } kind;
		float x = 0, y = 0;   // 0..1 across the window
		int key = 0;          // Flash key code (Key, KeyUp; 0 for others); wheel steps (Wheel)
		std::string text;     // typed text, UTF-8 (Text)
	};
	std::function<void(const MenuInput&)> onMenuInput;
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
	// The player's keys (Settings/Controls.con); without them W/S/A/D,
	// shift, the mouse buttons, E, Tab, Caps Lock, 1-9 and F1-F8.
	const input::ControlMaps* controls = nullptr;
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

	struct Impl;
	std::unique_ptr<Impl> impl;

private:
	Window();
};

// Returns the process exit code. With a window it draws there (and leaves
// it open); without one it opens its own for this run.
int runViewer(const render::LevelSources& sources, const ViewerOptions& options, Window* window = nullptr);

} // namespace dice::hfe
