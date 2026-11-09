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
}
