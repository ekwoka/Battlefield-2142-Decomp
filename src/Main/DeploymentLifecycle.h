#pragma once

namespace dice::main {
	enum class DeploymentDoneAction {
		None,
		InitialSpawn,
		Respawn,
	};

	// Keep the deployment button's policy independent of the SDL/HUD callback:
	// initial deployment has no death timer, while a dead player must wait for
	// the world-owned countdown before a button press can request a spawn.
	constexpr DeploymentDoneAction deploymentDoneAction(bool done, bool playerPresent, bool alive, bool initialPending, float respawnIn)
	{
		if (!done) return DeploymentDoneAction::None;
		if (!playerPresent) return DeploymentDoneAction::None;
		if (initialPending && !alive) return DeploymentDoneAction::InitialSpawn;
		if (!alive && respawnIn <= 0.0f) return DeploymentDoneAction::Respawn;
		return DeploymentDoneAction::None;
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
