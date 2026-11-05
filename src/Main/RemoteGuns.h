#pragma once

// The seat guns of the players in a joined game's vehicles, run on the
// client from each player's networked fire state.
//
// BF2142.exe's client update (0x00590e10) goes through the players each
// tick and, for a player whose control object is active, turns the fire
// states the player networkable carries (player vtable 0x1d8 / 0x1e0 /
// 0x1e8: +0x19c..+0x19e or a press between two updates, +0x24c; masks 0x40,
// 0x80, 0x20000, which the reader 0x005981a0 sets for the others' players)
// into input for that control object (0x004cc7a0: c_PIFire 8, c_PIAltFire
// 0x1f and the third group's 0x2f, then event 6 to the object). The
// control object is the player's soldier on foot (GameWorld::
// animateRemoteSoldier) or the seat he sits in: the seat's active weapon
// of that group (the seat's ghost, 0x00605650, carries each group's
// active weapon) fires on the client as it does on the server. Its fire
// (FireComp 0x005f31d0) sends event 0 to the weapon's children (the
// third-person list +0x144 for another player's: the muzzle flash and
// smoke bundles, the tanks' GEB dust; ObjectEffects) and its sound
// component plays the Fire sound (DefaultSoundComp 0x005dbdc0).
//
// This runs those guns: the rounds at the template's roundsPerMinute and
// fire mode (single and burst a press, automatic while held), the server's
// rounds in the magazine while it fires and the reload after reloadTime
// when the simulated magazine runs out with ammo.autoReload; each round
// is GameWorld::remoteGunFired, and its projectile the client's own
// (GameWorld::fireRemoteSeatRound: FireComp's 0x005f2bb0 makes a round
// without networkable info on every machine; a networked one is the
// server's ghost). The exe's client finds the muzzle through its copy of
// the vehicle's tree, whose RotationalBundles take their angles from the
// bundles' networkables (0x00602070); the rewrite's client has the same
// angles from the ghosts (net::RemoteWorld::partPose) and turns the seat's
// layout by them. The rounds stray by the client's own deviation: nothing
// networks or seeds it (the client's firearm builds it up as it fires and
// draws its own random numbers), so a round's path is the client's, not
// the server's.

#include "Game/GameWorld.h"
#include "Game/Weapons.h"
#include "World/ObjectTemplateManager.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dice::hfe::client {

class RemoteSeatGuns {
public:
	// A seated player's state this tick.
	struct Seat {
		int player = -1;
		uint32_t vehicle = 0; // the vehicle's object on the client
		int seat = 0;         // its seat (the seat ghost's part)
		// Per weapon group (0 PIFire, 1 PIAltFire, 2 the third): the fire
		// state, the server's rounds (-1 unknown), fire mode (-1 the
		// template's first) and the group's active weapon (its place in
		// the group's list, -1 unknown: the first).
		bool firing[3] {false, false, false};
		int rounds[3] {-1, -1, -1};
		int fireMode[3] {-1, -1, -1};
		int active[3] {-1, -1, -1};
		int presses = -1; // the fire's presses (mask 0x4000000), group 0's
		physics::Vec3 velocity; // the vehicle's (its ghost's), which the rounds keep
	};
	// One tick: fires the seats' guns; forgets the guns of players not listed.
	void step(game::GameWorld& round, const std::vector<Seat>& seats, float seconds);

	// The seat's weapon of a group: the `place`-th of the group's weapons in
	// the seat's list (0x005a98b0's order), the first for -1; null for none.
	static const game::GameWorld::SeatWeapon* groupWeapon(const game::GameWorld::SeatLayout& seat, int group, int place);

private:
	struct Gun {
		std::string weapon;
		game::FireArmSettings arm;
		int rounds = -1;
		int presses = -1;
		float nextRound = 0;
		float reloadLeft = 0;
		int burstLeft = 0;
		bool wasFiring = false;
		int barrel = 0;       // the next barrel (MultiFireComp +0x1a8)
		int tracerCount = 0;  // rounds to the next tracer (the fire comp's +0x78)
		// Its SimpleDeviationComp on the client (0x005dedb0 each tick, the
		// round's 0x005f5540): built up by the client's own firing.
		game::DeviationBuildUp deviation;
		bool hasDeviation = false;
	};
	std::map<std::pair<int, int>, Gun> m_guns; // by (player, group)
};

// The commanders' strikes on a joined client. The strike's projectile is
// the server's ghost, but its effects at the firearm come from the client
// firing the asset's firearm itself: 0x005a0e50 on the server sends every
// client kEvArtillery kind 2 (net::StrikeFireInfo) as it fires, and the
// client's handler (0x0050eeb0) puts the asset's first weapon (interface
// 0xc4c5's slot 0x70(0)) at (x, orbitalBombardmentHeight, z), the height
// alone (the event carries no ground height; the server's is the target's
// plus it), facing down, and fires it (+0x260, event 7). The firearm runs
// its burst (fire.burstSize rounds at fire.roundsPerMinute) and each round
// sends event 0 to its children (FireComp 0x005f31d0): the EMP strike's
// GEB_emp_strike looks 2000 m down from there and starts the ground's
// effect. Its rounds make no projectile here (a networked template:
// 0x005f2bb0 makes none on a client).
class RemoteStrikes {
public:
	struct Round {
		uint32_t asset = 0;   // the asset's object on the client
		std::string fireArm;  // its firearm (template name)
		physics::Vec3 at;     // where the firearm stands
	};
	// The asset's firearm: the first GenericFireArm of its tree (pre-order,
	// the PCO's weapon list order); empty for none.
	static std::string firstFireArm(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& asset);
	// The event, for the asset's object: its burst from now.
	void fired(const world::ObjectTemplateManager& templates, uint32_t asset, const world::ObjectTemplate& assetTemplate,
		float x, float z, float height);
	// One tick: the rounds due.
	std::vector<Round> step(float seconds);

private:
	struct Burst {
		Round round;
		int left = 0;
		float interval = 0.1f;
		float next = 0;
	};
	std::vector<Burst> m_bursts;
};

} // namespace dice::hfe::client
