// The HUD's weapon and vehicle parts that the templates' weaponHud.* and
// vehicleHud.* components drive (BF2142.exe's HUD information layer).
//
// The components. WeaponHud (class id 0xc4ee, properties from 0x00556f94,
// written back by 0x006381d0) and VehicleHud (0xc4ea, from 0x005532c2,
// written back by 0x00636440) keep their fields in the template; the HUD
// reads them through the component's slot 0x50 (weapon) / 0x78 (vehicle),
// a block 0x44 bytes below the template's fields (the weapon's guiIndex at
// +0xa8 is the template's +0xec, the vehicle's maxClimbSpeed at +0xf0 its
// +0x134), and their textures through slot 0x4c. Here they are read from
// the template's settings by name; the defaults are the constructors'
// (0x00637b30 for the vehicle's; the weapon's as written back).
//
// The weapon HUD (0x007c1f10, each frame for the gun in hand: the
// soldier's, or the seat's), setting the variables of the HUD information
// layer registered by 0x007bf6f0:
//   - PrimaryAmmoShow while guiIndex is not -1; PrimaryAmmoBarShow
//     weaponHud.showAmmo (default 1), PrimaryClipsShow weaponHud.showClips
//     (1);
//   - PrimaryWeaponNameString the hudName, CrosshairIconPathString the
//     crosshairIcon, PrimaryAmmoBarFront/BackPathString the ammo bar's
//     textures;
//   - with hasMag (1) and a magazine (magSize >= 0) the clip icon
//     (PrimaryClipPathString, PrimaryAmmoIconShow 0), else the ammo icon
//     (PrimaryAmmoIconPathString, PrimaryAmmoIconShow 1);
//   - FireRateIconShow / FireRateIconPathString (0x007be640): with
//     hasFireRate (1) and a fire rate n >= 0 (the firearm's slot 0x14),
//     "Ingame/Weapons/Icons/Hud/firerate_<n>.tga", else firerate_0 hidden;
//   - the ammunition, unless the bar is off or a reload runs (0 < progress
//     < 1, which hides the text: 0x007bede0): without a magazine
//     (magSize < 0) PrimaryEternalShow, the bar full and no text; else
//     PrimaryAmmo rounds / magSize, PrimaryAmmoString the rounds,
//     TotPrimaryAmmoString "/ <rounds + (mags - 1) x magSize>", and a
//     magazine of one round (magSize < 2) shows PrimarySingleFireShow;
//     showBatteryIconInHud shows PrimaryBatteryIconShow instead;
//     PrimaryEternalMagsShow for endless magazines (nrOfMags < 0);
//   - the colours (the HUD manager's slot 0xa8, 0x0074eb60: below a share,
//     (0.949, 0.184, 0.184) 0x00a0bbd0, else white): the ammunition's
//     below 0.3 of a magazine (0.1 for single rounds), the clips' below
//     0.3 of the magazines (no clips: the red);
//   - the clips (0x007be960) with showClips and a magazine:
//     PrimaryClip1..3Show for 1..3 spare magazines, PrimaryClipStringShow
//     and PrimaryClipString "(<spare>)";
//   - the reload (PrimaryReloadTime the progress, PrimaryReloadTimeShow
//     while between 0 and 1) and the heat (PrimaryHeatValue,
//     PrimaryHeatShow above 0);
//   - the overheat (the firearm's slot 0x2c): once each time it starts,
//     the weapon's HUD component plays its sound (slot 0x44, latched at
//     +0xb0): weaponHud.overheatSound.
//
// The item selection (0x0079bc70.., the HUD manager's +0xf4, menu state
// 0xb): up to ten items, the kit's weapons by their item index
// (0x0079c290: a weapon's 0xc49c slot 0x30 or 0x94c0 slot 0x19c; reversed
// as 10 - index with ItemSelection.setReverseItems), packed from slot 0 at
// the bottom (0x0079be20). Each slot: Item%iIcon the weapon's
// weaponHud.selectIcon, Item%iIndex its index, ItemSlot%iActive, and
// ItemSlot%iAlpha 1 (0.4 for one that can't be used). The mouse wheel
// opens it for 2 s (0x00756601: +0x20 = 2.0; the constructor's 10, 0x0079bfb0)
// and moves the selection (ItemSelectActive); the timer runs down in
// 0x0079c050, which closes it. Here a change of the weapon in hand opens it.
//
// The vehicle HUD (0x007a3180 → 0x007a2e60, 0x007a18a0, 0x007a2b50; the
// variables of 0x0079e5a0):
//   - VehicleNameString the vehicleHud.hudName; TurretIconShow
//     hasTurretIcon;
//   - VehicleArmor the armour's share (with useChildArmor the child's,
//     0x005c6460), VehicleArmorString ceil(share x 100) and its colour
//     below 0.3;
//   - SpeedString round(|velocity|) x 10, AltitudeString ceil(height over
//     the sea) (DAT_00a83e58 slot 0x60), VehicleElevationSpeedAngle eased
//     by 4 pi / maxClimbSpeed (default 10) x the climb x 0.1;
//   - the seat dots (0x007a28e0 → the "OccupiedData" list of 0x007e1620,
//     drawn by BfOccupiedVehicleNode 0x007e1520): per seat its state and
//     OccupyingPlayerPos<seat+1>X/Y, the seat's vehicleHud.vehicleIconPos
//     (state 0 at 0, 0): 4 an empty seat, 1 the local player in a squad,
//     2 the local player, 7 his squad, 5 his team, 6 the enemy.
//
// The map's vehicles (Bf2MenuMapNode 0x00793b60, its textures 0x0077dd70):
// the relation 0x00781680 (the local player's 0x25, a teammate's or his
// team's empty one 0x12, the enemy's 0x13, a neutral empty one 0x10);
// shown when its own, a friend's or neutral, or alwaysShowOnMinimap, or the
// enemy's with showOnEnemyMinimap; Minimap.setPaintVehicle filters by
// vehicleType (0x0077e520). The texture is miniMapIcon (miniMapIconInside
// while the camera is inside it), its size miniMapIconSize (16 x 16; the
// inside one), turned by the vehicle; with usePlayerIcon a crewed vehicle
// is drawn as its occupant instead (slot 0x138).
#pragma once

#include <array>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
namespace hud {
class HudManager;
}
namespace world {
class ObjectTemplate;
}
} // namespace dice::hfe

namespace dice::hfe::menu {

// A template's setting, or `otherwise` when it has none.
std::string hudSetting(const world::ObjectTemplate* t, const char* key, const std::string& otherwise = {});
float hudNumber(const world::ObjectTemplate* t, const char* key, float otherwise);

// The HUD manager's colour pick (slot 0xa8, 0x0074eb60): below
// `threshold` the low colour (0.949, 0.184, 0.184, 1), else white.
std::array<float, 4> hudLevelColor(float value, float threshold);

struct WeaponHudInput {
	const world::ObjectTemplate* weapon = nullptr; // its weaponHud.*
	int rounds = 0;      // in the magazine (the firearm's slot 0xbc)
	int magSize = 0;     // slot 0xb8; < 0: no magazine
	int mags = 0;        // magazines with the one in the gun (slot 0xb0)
	int nrOfMags = 0;    // slot 0xa8; < 0: endless
	float reload = 0;    // the reload's progress, 0..1 (1 - slot 0x9c)
	float heat = 0;      // slot 0x24
	bool overheated = false; // slot 0x2c
	int fireRate = -1;   // slot 0x14 (-1: none)
};

class WeaponHud {
public:
	// Sets the weapon variables (prefix "Primary"); returns the sound to
	// start (the overheat's), or "".
	std::string update(hud::HudManager& hud, const WeaponHudInput& in);

private:
	bool m_overheatPlayed = false; // +0xb0
};

struct SelectItem {
	int index = 0;       // the item index (1 the knife, 2 the sidearm, 3 the primary...)
	std::string icon;    // weaponHud.selectIcon
	bool usable = true;
};

class ItemSelection {
public:
	void bind(Console& console); // ItemSelection.setReverseItems
	void setReverse(bool reverse) { m_reverse = reverse; }
	bool reverse() const { return m_reverse; }
	// Opens the list (for `seconds`) with the item in hand selected.
	void open(const std::vector<SelectItem>& items, int selectedIndex, float seconds = 2.0f);
	// Each frame: the timer and the variables.
	void update(hud::HudManager& hud, float dt);
	bool shown() const { return m_shown; }
	int selected() const { return m_selected; }
	struct Slot {
		bool active = false;
		int index = -1;
		std::string icon;
		float alpha = 1;
	};
	const std::array<Slot, 10>& slots() const { return m_slots; }

private:
	bool m_reverse = false; // +8
	bool m_shown = false;   // +9
	int m_selected = -1;    // +0xc
	float m_timer = -1;     // +0x24
	std::array<Slot, 10> m_slots;
};

// A seat dot's state (0x007a28e0).
int seatDotState(bool occupied, bool local, int squad, int localSquad, bool sameTeam);

struct SeatDot {
	int state = 0;
	float x = 0, y = 0; // vehicleHud.vehicleIconPos of the seat's object
};

struct VehicleHudInput {
	const world::ObjectTemplate* vehicle = nullptr;
	float armor = 1;      // its share, 0..1
	float speed = 0;      // m/s
	float climb = 0;      // m/s up
	float altitude = 0;   // over the sea
	std::vector<SeatDot> seats;
};

class VehicleHud {
public:
	void update(hud::HudManager& hud, const VehicleHudInput& in);
	void clear(hud::HudManager& hud);

private:
	float m_climbAngle = 0, m_climbPrevious = 0; // +0x204 / +0x208
};

// The map relation of a vehicle (0x00781680).
enum class MapRelation { Neutral = 0x10, Friendly = 0x12, Enemy = 0x13, Own = 0x25 };
MapRelation vehicleRelation(bool hasOccupant, bool localInside, int occupantTeam, int vehicleTeam, int localTeam);
// Whether the map shows the vehicle, its texture and size; false when not.
struct VehicleMarker {
	std::string texture;
	float width = 16, height = 16;
	bool asPlayer = false; // usePlayerIcon: draw the occupant
};
bool vehicleMarker(const world::ObjectTemplate& vehicle, MapRelation relation, bool inside, bool spotted, VehicleMarker& out);

} // namespace dice::hfe::menu
