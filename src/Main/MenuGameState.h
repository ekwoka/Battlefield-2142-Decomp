#pragma once

#include "Main/MenuBindings.h"
#include "UI/Player/Player.h"

namespace dice::hfe::menu {

// The retained playlist page only reads isGameRunning when it is created.
// Update its authored controls when the game takes over or is disconnected.
inline void updateMenuGameState(ui::Player& player, MenuBindings& bindings, bool running)
{
	bindings.setGameRunning(running);
	if (!player.interpreter()) return;
	for (const auto& button : {"btnInstantAction", "btnStartPlaylist", "btnDisconnect", "btnDisconnect2"}) {
		const auto control = player.evaluate(std::string("singleplay.mc_singleplayer.") + button);
		if (control.isObject()) {
			const bool disconnect = std::string(button).find("btnDisconnect") == 0;
			player.interpreter()->setMember(control, "_visible", ui::avm1::Value(disconnect == running));
		}
	}
}

} // namespace dice::hfe::menu
