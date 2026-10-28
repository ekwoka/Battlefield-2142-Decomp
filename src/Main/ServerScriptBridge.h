// The scripts' game on a server: ScriptBridge plus the server settings
// (host.ss_getParam, 0x00568350) and the remote console (host.rcon_invoke
// 0x004427b0, host.rcon_feedback 0x00442940).
#pragma once

#include "ServerAdmin.h"

#include "Game/ScriptBridge.h"

#include <functional>
#include <string>

namespace dice::hfe::host {

class ServerScriptBridge : public game::ScriptBridge {
public:
	ServerScriptBridge(game::GameWorld& world, game::GameLogic& logic, game::ServerSettings settings,
		std::string modDirectory, ServerAdmin* admin, Console* console)
		: game::ScriptBridge(world, logic, std::move(settings), std::move(modDirectory)), m_admin(admin),
		  m_console(console)
	{
	}

	// 0x00568350: gameMode and mapName come from the game logic, the rest
	// from the server settings (teamRatioPct as a float).
	game::ScriptValue serverSetting(const std::string& name) override
	{
		using game::ScriptValue;
		if (!m_admin || name == "gameMode" || name == "mapName") {
			return game::ScriptBridge::serverSetting(name);
		}
		const net::ServerSettings& s = m_admin->settings;
		const auto flag = [](bool on) { return ScriptValue::integer(on ? 1 : 0); };
		if (name == "ticketRatio") {
			return ScriptValue::integer(s.ticketRatio);
		}
		if (name == "maxPlayers") {
			return ScriptValue::integer(s.maxPlayers);
		}
		if (name == "timeLimit") {
			return ScriptValue::integer(s.timeLimit);
		}
		if (name == "scoreLimit") {
			return ScriptValue::integer(s.scoreLimit);
		}
		if (name == "autoBalance") {
			return flag(s.autoBalanceTeam);
		}
		if (name == "teamRatioPct") {
			return ScriptValue::number(s.teamRatioPercent);
		}
		if (name == "tkpEnabled") {
			return flag(s.tkPunishEnabled);
		}
		if (name == "tkpNeeded") {
			return ScriptValue::integer(s.tkNumPunishToKick);
		}
		if (name == "tkpDefault") {
			return flag(s.tkPunishByDefault);
		}
		if (name == "globRank") {
			return flag(s.useGlobalRank);
		}
		if (name == "globUnlocks") {
			return flag(s.useGlobalUnlocks);
		}
		if (name == "ranked") {
			return flag(s.ranked);
		}
		if (name == "maxRank") {
			return ScriptValue::integer(s.maxRank);
		}
		return game::ScriptBridge::serverSetting(name); // "unknown serversetting" in the exe
	}

	std::string rconInvoke(const std::string& command) override
	{
		return m_admin && m_console ? m_admin->rconInvoke(*m_console, command) : std::string();
	}

	void rconFeedback(int player, const std::string& text) override
	{
		if (onFeedback) {
			onFeedback(player, text);
		}
	}

	std::string overlayDirectory() override { return overlay; }

	// Where rcon_feedback goes (a player's console; -1 the server's).
	std::function<void(int player, const std::string& text)> onFeedback;
	// sgl_getOverlayDirectory: where admin/default.cfg is read from.
	std::string overlay;

private:
	ServerAdmin* m_admin;
	Console* m_console;
};

} // namespace dice::hfe::host
