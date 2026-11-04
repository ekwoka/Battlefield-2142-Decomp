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
//   - the distance counter and range finder (0x007c1470: TargetDistance-
//     String, TargetCoordX/Y/ZString) and the scan (0x007be380:
//     HudScanProgress, HudScanInProgress);
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

#include "Game/GameWorld.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
namespace host {
class HudWorld;
}
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
	// The range finder's ray (hasRangeFinder): what it met and how far from
	// the controlled object; the distance counter's projectile distance
	// (hasDistanceCounter, -1 none); the scan (hasScanProgress, 0..1).
	bool rangeHit = false;
	float targetDistance = 0;
	std::array<float, 3> target {};
	int travelled = -1;
	bool scanning = false;
	float scan = 0;
};

class WeaponHud {
public:
	// Sets the weapon variables (prefix "Primary", or "Secondary" for a
	// seat's second gun, 0x007c2d50); returns the sound to start (the
	// overheat's), or "".
	std::string update(hud::HudManager& hud, const WeaponHudInput& in, bool secondary = false);
	// The weapon's weaponHud.* fields, read once per template.
	struct Fields {
		bool showAmmo = true, showClips = true, hasMag = true, hasFireRate = true, battery = false;
		bool rangeFinder = false, distanceCounter = false, scanProgress = false, ignoreTargetDistance = false;
		bool guiShown = true;
		std::string name, crosshair, barFront, barBack, clipIcon, ammoIcon, overheatSound;
		void read(const world::ObjectTemplate* w);
	};
	const Fields& fields() const { return m_fields; }

private:
	bool m_overheatPlayed = false; // +0xb0
	bool m_read = false;
	const world::ObjectTemplate* m_template = nullptr;
	Fields m_fields;
};

// The crosshair's four parts (0x007be770, each frame from 0x007c1f10's
// weapon HUD): the spread is ceil(deviation x deviationFactor) pixels
// (the weapon HUD's +0x104, weaponHud.deviationFactor, 10 by default),
// pushed out by minVisualDeviation (+0x10c, 0) and held at
// maxVisualDeviation (+0x108, 1000): an offset min + spread over the
// maximum is the maximum. Up = 300 - length - offset, Down = 301 + offset,
// Left = 399 - length - offset, Right = 400 + offset, about the 800 x 600
// centre (the clamped branch takes the node's half size, the same 400 /
// 300). The block fields are the WeaponHud component's (constructor
// 0x006390e0: +0xf8 10, +0xfc 1000, +0x100 0; read through slot 0x50 at
// +0xb0 / +0xb4 / +0xb8).
struct CrosshairParts {
	float up = 0, down = 0, left = 0, right = 0;
	int offset = 0;
};
CrosshairParts crosshairParts(float deviation, int deviationFactor, int minVisual, int maxVisual, float length = 20);
// The same with the weapon template's weaponHud.* (defaults as above).
CrosshairParts crosshairParts(float deviation, const world::ObjectTemplate* weapon, float length = 20);

// The Titans' consoles and shields on the HUD (Friendly/EnemyStrategic-
// Object%iShow, 0..9; the HUD information layer's +0xa89 + (team - 1) x
// 10, copied for the local team and the other by 0x00448850). Every
// entry is 1 from the round's start (0x00448300, 0x00448760); a destroyed
// object (event 0x18, 0x0063c6a0) whose first templated ancestor has
// vehicleHud.sendDestroyedHudEvent (the vehicle block's +0xb3, the
// component's +0xfb, 0 by default) posts event 2/0x32 with itself, and the
// handler (0x0044b680, case 0x32) clears the entry of the root
// PlayerControlObject's team (1 or 2) at the template's vehicleHud.guiIndex
// (0..9; the consoles 1..4, the AS forcefield none so 0, eu_forcefield_small
// 5). Nothing sets an entry back before the next round. The Titan HUD
// (HudElementsTitans.con) shows a team's console row while entry 0 is 0
// (its shield down) and each console lit while its entry is 1.
class StrategicObjects {
public:
	void reset() { m_alive.fill(true); }
	// A part destroyed: `team` the root's (1 or 2), `t` the part's template.
	void destroyed(int team, const world::ObjectTemplate* t);
	bool shown(int team, int index) const
	{
		return team >= 1 && team <= 2 && index >= 0 && index < 10 && m_alive[static_cast<size_t>((team - 1) * 10 + index)];
	}
	// Sets Friendly/EnemyStrategicObject%iShow for the local team.
	void update(hud::HudManager& hud, int localTeam) const;

private:
	std::array<bool, 20> m_alive {true, true, true, true, true, true, true, true, true, true, true, true, true, true,
		true, true, true, true, true, true};
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
	// Opens the list (for `seconds`, the selection's +0x20) with the item in
	// hand selected.
	void open(const std::vector<SelectItem>& items, int selectedIndex, float seconds = 2.0f);
	// A weapon key's pick (0x0079c190): the slot of that item index (+0x18;
	// the first without one) flashes, its ItemSlot%iActive toggled every
	// 0.1 s six times (+0x1c, +0x28; 0x0079c050 opens the list again for
	// its seconds at each toggle), the last flash's slot shown again.
	void flash(int itemIndex);
	int flashing() const { return m_flash; }
	// The mouse wheel's moves (the HUD's input 0x00755d50: c_PINextItem,
	// input 0x30, 0x0079c1e0; c_PIPrevItem, 0x31, 0x0079c230): the next
	// slot if it is usable, else the first if that is; the previous one if
	// usable, else the last if usable, else the first. A move selects by
	// the item index (0x0079c120: the open time starts again) and plays
	// "weaponOver"; false when nothing was selected (an empty list, or
	// 0x0079c1e0 finding neither usable).
	bool next();
	bool previous();
	// The open time (+0x20) the next opening or move keeps it up for.
	void setSeconds(float seconds) { m_seconds = seconds; }
	// The selected slot's item index (0x0079bdb0; -1 for none).
	int selectedItem() const;
	// Each frame: the timer and the variables.
	void update(hud::HudManager& hud, float dt);
	bool shown() const { return m_shown; }
	int selected() const { return m_selected; }
	struct Slot {
		bool active = false;
		int index = -1;
		std::string icon;
		float alpha = 1;
		bool usable = true; // the entry's +8 (0x0079c290: slot 0x170)
	};
	const std::array<Slot, 10>& slots() const { return m_slots; }

private:
	bool m_reverse = false; // +8
	bool m_shown = false;   // +9
	int m_selected = -1;    // +0xc
	void select(int itemIndex); // 0x0079c120
	int m_flash = -1;       // +0x18: the flashing slot
	int m_flashCount = 0;   // +0x1c: toggles left
	float m_seconds = 10;   // +0x20: how long it stays open (the constructor's 10)
	float m_timer = -1;     // +0x24
	float m_flashTimer = -1; // +0x28: to the next toggle
	std::array<Slot, 10> m_slots;
};

// The HUD's input pass over the item list (part of 0x00755d50), run on
// the frame's input before the soldier takes it. With the list open (menu
// state 0xb: the wheel's, a key's or an activation's) a press of fire
// (input 8) is the pick: the selected item's index + 0xd (0x0079bdb0,
// clamped to 0xe..0x16) is pressed as that PIWeaponSelect key (0x00583880)
// and fire is cleared, and fire stays cleared until 0.3 s after (the
// list's +0x38 = the time + its +0x30, the constructor's 0.3). Without
// fire, a wheel step (c_PINextItem / c_PIPrevItem, inputs 0x30 / 0x31)
// where the list may open opens it for 2 s (+0x20 = 2.0, state 0xb) and
// moves the selection (0x0079c1e0 / 0x0079c230, "weaponOver"). The wheel's
// input still reaches the soldier (a zoomed launcher's airburst distance,
// 0x005d5440).
struct ItemListInput {
	// Whether the list may open (0x00755d50's param_2 byte 3): on foot
	// unless the weapon in hand is zoomed (its firearm's slot 300), in a
	// vehicle only with the control object's vehicleHud.useSelectionIcons
	// (block +0xbc); never in a demo. The active weapon's +0x14 bit 0,
	// which also forbids it, isn't traced.
	bool allowed = false;
	std::vector<SelectItem> items; // 0x0079c470's list
	int inHand = -1;               // the item index in hand (selected at opening)
	double fireBlockUntil = -1;    // +0x38
	// `item` is the frame's weapon key (0 none), set by a pick; returns the
	// sound to play ("" none).
	std::string apply(ItemSelection& list, bool& fire, int& item, int itemStep, double now);
};

// The item list in a seat (0x0079c470): the control object's PIFire
// group (its weapon list 0, slot 0x70(0)) by item index, each dimmed when it
// can't be selected (0x0079c290: slot 0x170, isSelectable); a seat without
// one lists the soldier's own (player slot 0xb4's list: the kit's weapons,
// `carried`); with the seat's vehicleHud.useSelectionIcons (block +0xbc) and
// a PIAltFire group (slot 0x70(1)) longer than that, the PIAltFire group
// (the list's +0x10 = 1). `find` gives a weapon's template (its
// weaponHud.selectIcon).
std::vector<SelectItem> seatSelectItems(const game::GameWorld::SeatLayout& seat, const world::ObjectTemplate* seatTemplate,
	const std::function<const world::ObjectTemplate*(const std::string&)>& find,
	const std::vector<std::pair<int, std::string>>& carried);

// A seat dot's state (0x007a28e0).
int seatDotState(bool occupied, bool local, int squad, int localSquad, bool sameTeam);

struct SeatDot {
	int state = 0;
	float x = 0, y = 0; // vehicleHud.vehicleIconPos of the seat's object
	// Its occupant (the passengers' names): the name, and 0 none, 1 an
	// enemy, 2 a teammate, 3 the local player's squad.
	std::string occupant;
	int relation = 0;
};

// The passengers' names (0x007a34f0, OccupyingPlayerName%iShow/String and
// OccupyingPlayer%iColor*): per occupied seat "%d. %s" with the seat's
// number (counted over every seat from 1; a player whose slot 0x6c says so
// is "R2D2", never in the rewrite), in the colour table 0x00b3c1e0's
// (0.4, 0.6, 1) for the team, (1, 0.4, 0.4) for the enemy, or the squad's
// (0.4, 1, 0) (DAT_00b3c380); packed into the ten lines from the last
// filled one down (the first name in line count - 1, the last in line 0).
struct PassengerLine {
	std::string text; // "" hidden
	std::array<float, 3> colour {1, 1, 1};
};
std::array<PassengerLine, 10> passengerLines(const std::vector<SeatDot>& seats);

// A HUD blink (BF2142.exe's blinker, constructor 0x00778040, vtable
// 0x009b264c, its tick 0x00778080 with the frame's time): it watches a
// value and toggles a show flag while the value is below `threshold`. The
// vehicle and player HUD's four (made with its variables by 0x0079e5a0, at
// 0x0079fa9f..0x0079fb89): PlayerHealthBlink (+0x2ae) on PlayerHealth
// (+0x1cc), VehicleArmorBlink (+0x2b9) on VehicleArmor (+0x1d8),
// PlayerStaminaBlink (+0x2aa) on PlayerStamina (+0x1d0) and
// VehicleStaminaBlink (+0x2ac) on VehicleStamina (+0x1d4), each with
// threshold 0.3, floor 0, periods 0.7 / 0.15 and the scaled period.
//   - Below the threshold (and the threshold not 0, the value not below
//     0) the timer runs; once it passes the period ((0.7 - 0.15) / 0.3 x
//     the value + 0.15 scaled, else 0.7) the flag toggles (held off at or
//     below the floor) and the timer starts again: the nearer empty the
//     faster it blinks.
//   - Otherwise a lit flag goes off once the timer passes the period.
class HudBlink {
public:
	HudBlink(float threshold = 0.3f, float floor = 0, float slow = 0.7f, float fast = 0.15f, bool scaled = true)
		: m_threshold(threshold), m_floor(floor), m_slow(slow), m_fast(fast), m_scaled(scaled)
	{
	}
	// One frame (0x00778080): returns the flag.
	bool tick(float value, float seconds);
	bool lit() const { return m_lit; }
	void reset()
	{
		m_lit = false;
		m_timer = 0;
	}

private:
	float m_threshold, m_floor, m_slow, m_fast; // +0xc, +0x10, +0x14, +0x18
	bool m_scaled;                              // +0x1c
	float m_timer = 0;                          // +0x20
	bool m_lit = false;                         // *+8
};

// The other seats' turrets on the vehicle HUD (0x007a2b50 → 0x007a1e00 →
// 0x007a07c0): GunnerAngle (+0x20c) and SecondaryGunnerAngle (+0xb0).
// With the local player in a vehicle, every PlayerControlObject of the
// root's list (its slot 0x64) that isn't a soldier (interface 0xc4d7) or
// his own seat is searched (the seat's subtree, depth first) for the first
// RotationalBundle whose maxSpeed turns on the yaw alone (x not 0, y and z
// 0: 0x006147c0); with the controlled object's
// vehicleHud.showOneSecondaryGunnerAngle (block +0xc1: the APCs' drivers)
// the search stops after the first such seat, found or not. Each bundle
// found (its rotation r, degrees: slot 0x188):
//   - the first bundle seen since entering is kept (+0xa8), a second
//     different one too (+0xac); 0x007a1ee0 clears both as the player's
//     controlled object becomes his soldier again;
//   - without a second one, GunnerAngle = clamp(-r, -180, 180);
//   - with one, the second gives SecondaryGunnerAngle = clamp(-(r +
//     1.5708), ...) and the first GunnerAngle = clamp(r - 1.5708, ...)
//     (the exe adds pi / 2 to degrees before turning them to radians).
// The rewrite's HUD keeps angles in degrees (its picture rotations and
// compasses), so the values are the exe's radians x 180 / pi. The angles
// keep their last values while no bundle is found.
struct OtherSeat {
	bool hasBundle = false; // a yaw-only RotationalBundle under the seat
	int bundle = -1;        // which (the node's +0x20: the seat index here)
	float rotation = 0;     // its yaw rotation, degrees
};

struct VehicleHudInput {
	const world::ObjectTemplate* vehicle = nullptr;
	// The controlled object's template (the seat's PlayerControlObject;
	// the vehicle's for the driver): showOneSecondaryGunnerAngle.
	const world::ObjectTemplate* seat = nullptr;
	std::vector<OtherSeat> others; // the other seats in the root's list order
	float armor = 1;      // its share, 0..1
	float speed = 0;      // m/s
	float climb = 0;      // m/s up
	float altitude = 0;   // over the sea
	std::vector<SeatDot> seats;
	// The torque the HUD eases toward (the vehicle HUD's +0x2f4: nothing in
	// BF2142.exe writes it, so 0) and the control object's sprint (its slots
	// 0x108 stamina, 0x100 sprinting; the rewrite's vehicles have no
	// stamina of their own, so 1 and not sprinting).
	float torque = 0;
	float seconds = 0; // the frame's time (the stamina text's timer)
	float stamina = 1;
	bool sprinting = false;
};

class VehicleHud {
public:
	void update(hud::HudManager& hud, const VehicleHudInput& in);
	void clear(hud::HudManager& hud);

private:
	float m_climbAngle = 0, m_climbPrevious = 0; // +0x204 / +0x208
	float m_torque = 0, m_secondTorque = 1;     // +0x210 / +0x21c (the constructor's 0, 1)
	float m_staminaWas = 1, m_staminaTimer = -1; // +0x1d4, +0x23c
	HudBlink m_staminaBlink, m_armorBlink;       // +0x2ac, +0x2b9
	int m_firstBundle = -1, m_secondBundle = -1; // +0xa8 / +0xac (-1: none)
	float m_gunnerAngle = 0, m_secondaryAngle = 0; // +0x20c / +0xb0, degrees
	void gunnerBundle(const OtherSeat& seat); // 0x007a07c0
	// The template's fields, read when the vehicle changes.
	const world::ObjectTemplate* m_template = nullptr;
	std::string m_name;
	bool m_turretIcon = false;
	float m_climbSpeed = 10;
};

// The map relation of a vehicle (0x00781680).
enum class MapRelation { Neutral = 0x10, Friendly = 0x12, Enemy = 0x13, Own = 0x25 };
MapRelation vehicleRelation(bool hasOccupant, bool localInside, int occupantTeam, int vehicleTeam, int localTeam);
// Whether the map shows the vehicle, its texture and size; false when not.
struct VehicleMarker {
	std::string texture;
	float width = 16, height = 16;
	bool asPlayer = false; // usePlayerIcon: draw the occupant
	// miniMapIconOffset (miniMapIconOffsetInside while inside; block
	// +0x100 / +0x108, 0x0077dd70 into the marker's +0x20): pixels, the icon
	// drawn that far from its point in its own frame and turned about the
	// point (0x0077c610: the rectangle floor(point + offset - size / 2), the
	// turn's pivot the rectangle's centre - offset). The Titans' 0/8 and
	// 0/10.5 (0.94/11.2 the PAC's inside).
	float offsetX = 0, offsetY = 0;
};
bool vehicleMarker(const world::ObjectTemplate& vehicle, MapRelation relation, bool inside, bool spotted, VehicleMarker& out);

// A template's map fields (the VehicleHud component, 0x00637b30's
// defaults), read once per template:
//   - miniMapInfo (0x00636220, lower-cased: "icon" 0, "reload" 1, "all" 2;
//     "Icon" by default): what the map draws (0x00793b60's switch). 0 the
//     icon; 1 the reload icon (slot 0x144, 0x0077dee0: miniMapReloadIcon
//     at miniMapIconSize) with its wipe; 2 (the Titan silos) the
//     background (slot 0x148, 0x0077df60: miniMapBackgroundIcon) and the
//     reload icon, both layers of the icon (mini_reloadComposite, through
//     its texture's slot 0x7c), so the background and the wiped reload
//     ring show.
//   - the wipe (0x0078a990): a fan from the top centre over 2 pi - 2 pi x
//     the seat's gun's progress (slot 0x94), none for a neutral one
//     (relation 0x10); the reload texture's alpha is cleared and the fan
//     written, so the ring shows over the time still to go.
//   - spottedIcon / spottedIconSize (16; 0x0077dd70 with its spotted flag,
//     from 0x0077dfe0 for what spotting reveals): the kits' mini_Soldier.
//   - miniMapIconOffset / miniMapIconOffsetInside (0, 0; VehicleMarker).
//   - zoomOnMinimap (block +0xb0, 0x00793b60 at 0x0079429c): such a
//     vehicle (the Titans) isn't drawn on a map node at zoom index 2
//     (+0x74c) while the local player's overhead view is on (player slot
//     0xdc, +0x8f, set with +0x8e by 0x00593840: the commander's overhead
//     camera), whose picture shows the Titan itself.
// The silo icons' files are numbered frames (mini_titanSilo_01..06,
// mini_reload_01..06); the frame is the silo's place in its control
// points' order here (the frame's pick, through the texture's own frames,
// isn't traced).
struct MapIcons {
	std::string icon, iconInside, reload, background, spotted;
	std::array<float, 2> size {16, 16}, sizeInside {16, 16};
	std::array<float, 2> offset {0, 0}, offsetInside {0, 0};
	float spottedSize = 16;
	bool zoomOnMinimap = false;
	int info = 0;
	int vehicleType = -1;
	bool showOnEnemy = false, always = false, usePlayerIcon = false;
};
// Cached by template (checked against its name and its number of
// settings, as templates can be made again).
const MapIcons& mapIcons(const world::ObjectTemplate& t);
// zoomOnMinimap's test (0x00793b60): hidden on a map at zoom index 2 while
// the local player's overhead view is on.
bool hiddenOnZoomedMap(const MapIcons& icons, int zoomIndex, bool overheadView);

// The controlled object's vehicleHud flags (its template: a seat's
// PlayerControlObject, the vehicle's for the driver, the soldier's on
// foot; 1 by default, 0x00637b30):
//   - standardHelpEnabled (block +0xbb): the help manager's (0x00646cb0)
//     exit-vehicle message 0x48 (case at 0x00648906: in a vehicle that isn't
//     a LadderContainer, 0x9c46) and enter-vehicle message 0x47 (0x006487db:
//     on foot, a vehicle within 3 m, the controlled object not a Ladder,
//     0x9c40) need it; the static guns, the parachute and the bipods set 0.
//   - useVehicleCommRose (block +0xbe): the radio key (0x0075be30) opens
//     the vehicle's radio rose (menu state 6, RadioVehicle) when the
//     controlled object isn't the soldier, that rose is enabled (its slot
//     0x1c, +5: 1 from 0x007b88e0) and the flag is set; else the soldier's
//     (state 5, Radio) if that is enabled. The static AA / AV guns set 1,
//     the parachute, LadderContainer and the bipods 0.
bool standardHelpEnabled(const world::ObjectTemplate* controlled);
bool usesVehicleCommRose(const world::ObjectTemplate* controlled);
// A numbered frame of an icon without an extension (mini_titanSilo +
// 2 -> mini_titanSilo_02.dds); a name with one stays.
std::string iconFrame(const std::string& icon, int frame);
// The reload wipe's sweep (radians): 2 pi x the share of the reload still
// to go; 0 when loaded or for a neutral one.
float reloadSweep(float reloadLeft, float reloadTime, MapRelation relation);

// The squad radar's markers on the minimap (drawn in red, (1, 0.3, 0.3)):
//   - each enemy in `revealed` alive: what spotting shows, its kit's
//     spottedIcon at spottedIconSize (0x0077dfe0 -> 0x0077dd70 with the
//     spotted flag: the kits' mini_Soldier), else the kit's own map icon,
//     else mini_Soldier at 16; one on foot within 60 m across of a decoy (a
//     TargetObject of targetType TTDecoy, the Bp1_Decoy; 0x00508590 through
//     HudManager slot 0x70, 0x0075bd30) is drawn at the decoy instead;
//   - the vehicles the battlefield reports spotted (2/0x2e: the map node's
//     markers, 0x00790d60, 16 x 16) by their miniMapIcon (mini_Soldier
//     without one).
struct RadarMarker {
	std::string texture;
	float x = 0, z = 0, size = 16;
};
std::vector<RadarMarker> radarMarkers(const host::HudWorld& world, game::GameWorld& round, const std::vector<int>& revealed);

} // namespace dice::hfe::menu
