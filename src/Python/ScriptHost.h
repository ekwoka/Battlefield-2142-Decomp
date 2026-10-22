// The game's Python scripting (BF2142.exe embeds the Python 2.3 of
// dice_py.dll and registers the "host" module the scripts in python/bf2 and
// mods/<mod>/python call).
//
// This embeds Python 3 instead: the game's Python 2 scripts are translated
// as they are imported (py2compat.py), and "host" is written here against
// game::ScriptGame. Starting a round imports bf2 (python/bf2/__init__.py),
// runs bf2.init_module() (scoring, stats), then the game mode
// (game.gamemodes.gpm_cq) and its init(); game status changes and events
// (PlayerSpawn, ControlPointChangedOwner, TicketLimitReached...) call the
// handlers the scripts registered. Each update runs due timers (bf2.Timer)
// and the radius triggers scripts created (host.trig_create): objects
// entering or leaving a sphere (or hemisphere) around an object.
#pragma once

#include "Game/ScriptGame.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::script {

class ScriptHost {
public:
	using Log = std::function<void(const std::string& line)>;

	// `paths`: directories scripts import from (the game's python/, the
	// mod's python/, Admin/).
	ScriptHost(game::ScriptGame& game, std::vector<std::string> paths, Log log);
	~ScriptHost();
	ScriptHost(const ScriptHost&) = delete;
	ScriptHost& operator=(const ScriptHost&) = delete;

	bool ok() const;
	// Imports bf2 and the game mode and runs their init.
	bool startGameMode(const std::string& mode);
	void setGameStatus(game::GameStatus status);
	// An engine event for the handlers registered for it.
	void raise(const std::string& event, const game::ScriptArgs& args);
	// Timers and triggers.
	void update();
	// Runs Python source (for tests and the console).
	bool run(const std::string& source);

	struct Impl;

private:
	std::unique_ptr<Impl> m_impl;
};

} // namespace dice::hfe::script
