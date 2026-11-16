#pragma once

namespace dice::main {
	enum class DeploymentDoneAction {
		None,
		InitialSpawn,
		Respawn,
	};

	// Keep the deployment button's policy independent of the SDL/HUD callback:
	// DONE submits immediately; the world waits for its countdown before
	// creating a soldier (retail 0x0074e765 -> game logic slot 0x5c).
	constexpr DeploymentDoneAction deploymentDoneAction(bool done, bool playerPresent, bool alive, bool initialPending, float /*respawnIn*/)
	{
		if (!done) return DeploymentDoneAction::None;
		if (!playerPresent) return DeploymentDoneAction::None;
		if (initialPending && !alive) return DeploymentDoneAction::InitialSpawn;
		if (!alive) return DeploymentDoneAction::Respawn;
		return DeploymentDoneAction::None;
	}

	// 0x0044d278..0x0044d517: an unconfirmed highlighted group asks for
	// DONE; a confirmed group shows its timer, including an instant retry.
	constexpr const char* deploymentSpawnInfo(bool selected, bool requested, float respawnIn, bool valid = true)
	{
		if (!selected) return "HUD_CENTERINFOBOX_selectspawnpoint";
		if (!valid) return "HUD_CENTERINFOBOX_invalidspawnpoint";
		if (!requested) return "HUD_CENTERINFOBOX_presstospawn";
		return respawnIn > 0 ? "HUD_CENTERINFOBOX_timetospawn" : "HUD_CENTERINFOBOX_instantspawn";
	}

	// Enter toggles the spawn screen (menu state 1, 0x00754d74 -> 0x0074fd90) alive or dead; the respawn
	// timer is the server's (0x004bfe40) and never looks at the screen. Dead, it starts shown.
	constexpr bool deploymentScreenShown(bool alive, bool open, bool hiddenWhileDead)
	{
		return alive ? open : !hiddenWhileDead;
	}

	// The centred killed lines (KilledState, HudElementsKilled.con) of a dead or downed local player: shown
	// while the spawn screen is not (the screen has its own timer bar, SpawnInfoString); man down only shows
	// it once opened. A player who never died (the initial deployment) has no timer.
	constexpr bool killedInfoShown(bool manDown, float respawnIn, bool open, bool hiddenWhileDead)
	{
		return (manDown || respawnIn > 0.0f) && !(manDown ? open : deploymentScreenShown(false, open, hiddenWhileDead));
	}
}
