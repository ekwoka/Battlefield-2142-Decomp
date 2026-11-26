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

	// Enter closes state 1 by calling setDone(true), just like the button
	// (dead: 0x00754f29; alive: 0x00754e49..0x00754e65). Opening it
	// calls setDone(false) and, while dead, clears the submitted group.
	// From squad states 18/19 Enter selects state 1 first; it does not close.
	constexpr bool deploymentConfirmPressed(bool doneButton, bool enterPressed, bool screenShown, bool squadTab = false)
	{
		return doneButton || (enterPressed && screenShown && !squadTab);
	}

	// A visible joined marker is an explicit pick. If its group disappeared
	// before submission, keep deployment open instead of sending -1 (the
	// server's random-team-spawn request). No marker remains the legacy default.
	constexpr bool joinedSpawnPickCanSubmit(bool hasExplicitPick, int group)
	{
		return !hasExplicitPick || group >= 0;
	}

	// Enter toggles the spawn screen (menu state 1, 0x00754d74 -> 0x0074fd90) alive or dead; the respawn
	// timer is the server's (0x004bfe40) and never looks at the screen. Dead, it starts shown.
	constexpr bool deploymentScreenShown(bool alive, bool open, bool hiddenWhileDead)
	{
		return alive ? open : !hiddenWhileDead;
	}

	// The centred killed lines (KilledState, HudElementsKilled.con) of a dead or downed local player: shown
	// while the spawn screen is closed (it has its own timer bar, SpawnInfoString).
	// A player who never died (the initial deployment) has no timer.
	constexpr bool killedInfoShown(bool manDown, float respawnIn, bool open, bool hiddenWhileDead)
	{
		return (manDown || respawnIn > 0.0f) && !deploymentScreenShown(false, open, hiddenWhileDead);
	}

	// A press that began on the spawn screen or a menu stays theirs until released: the DONE
	// click (taken on the press) is no fresh c_PIFire for a downed soldier once the screen
	// closes under the held button (0x00585df0's revive call, "man down").
	constexpr bool pressHeldByMenu(bool heldBefore, bool held, bool menuShown)
	{
		return held && (heldBefore || menuShown);
	}
}
