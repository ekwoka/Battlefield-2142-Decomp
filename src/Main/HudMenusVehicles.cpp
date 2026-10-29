#include "HudMenusVehicles.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Hud/Hud.h"
#include "World/ObjectTemplate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dice::hfe::menu {

namespace {

std::string slashes(std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	return path;
}

bool flag(const world::ObjectTemplate* t, const char* key, bool otherwise)
{
	return hudNumber(t, key, otherwise ? 1.0f : 0.0f) != 0;
}

// "x/y" (a Vec2 setting) or one number for both.
std::array<float, 2> pair(const world::ObjectTemplate* t, const char* key, float otherwise)
{
	const std::string text = hudSetting(t, key);
	if (text.empty()) {
		return {otherwise, otherwise};
	}
	const float x = static_cast<float>(std::atof(text.c_str()));
	const size_t slash = text.find('/');
	const float y = slash == std::string::npos ? x : static_cast<float>(std::atof(text.c_str() + slash + 1));
	return {x, y};
}

void setColor(hud::HudManager& hud, const std::string& prefix, const std::array<float, 4>& c)
{
	hud.set(prefix + "Red", c[0]);
	hud.set(prefix + "Green", c[1]);
	hud.set(prefix + "Blue", c[2]);
	hud.set(prefix + "Alpha", c[3]);
}

} // namespace

std::string hudSetting(const world::ObjectTemplate* t, const char* key, const std::string& otherwise)
{
	if (!t) {
		return otherwise;
	}
	const auto it = t->settings().find(key);
	return it == t->settings().end() ? otherwise : it->second;
}

float hudNumber(const world::ObjectTemplate* t, const char* key, float otherwise)
{
	const std::string text = hudSetting(t, key);
	return text.empty() ? otherwise : static_cast<float>(std::atof(text.c_str()));
}

std::array<float, 4> hudLevelColor(float value, float threshold)
{
	// 0x0074eb60: below the threshold the second colour (0x00a0bbd0), below
	// 1 the third and else the first (both white, 0x00b3bc90 / 0x00b3ba80).
	if (value < threshold) {
		return {0.94902f, 0.184314f, 0.184314f, 1};
	}
	return {1, 1, 1, 1};
}

// --- The weapon HUD (0x007c1f10) ---

void WeaponHud::Fields::read(const world::ObjectTemplate* w)
{
	showAmmo = flag(w, "weaponHud.showAmmo", true);   // template +0x11a
	showClips = flag(w, "weaponHud.showClips", true); // +0x11b
	hasMag = flag(w, "weaponHud.hasMag", true);       // +0x117
	hasFireRate = flag(w, "weaponHud.hasFireRate", true); // +0x116
	battery = flag(w, "weaponHud.showBatteryIconInHud", false); // +0x127
	rangeFinder = flag(w, "weaponHud.hasRangeFinder", false);   // +0x118 (block +0xd4)
	distanceCounter = flag(w, "weaponHud.hasDistanceCounter", false); // +0x124 (+0xe0)
	scanProgress = flag(w, "weaponHud.hasScanProgress", false); // +0x125 (+0xe1)
	ignoreTargetDistance = flag(w, "weaponHud.ignoreTargetDistance", false); // +0x126 (+0xe2)
	guiShown = hudNumber(w, "weaponHud.guiIndex", 5) != -1;
	name = hudSetting(w, "weaponHud.hudName");
	crosshair = slashes(hudSetting(w, "weaponHud.crosshairIcon"));
	barFront = slashes(hudSetting(w, "weaponHud.ammoBarFrontIcon"));
	barBack = slashes(hudSetting(w, "weaponHud.ammoBarBackIcon"));
	clipIcon = slashes(hudSetting(w, "weaponHud.clipIcon"));
	ammoIcon = slashes(hudSetting(w, "weaponHud.ammoIcon"));
	overheatSound = hudSetting(w, "weaponHud.overheatSound");
}

std::string WeaponHud::update(hud::HudManager& hud, const WeaponHudInput& in, bool secondary)
{
	// The fields, read once per weapon (no template lookups per frame).
	if (!m_read || in.weapon != m_template) {
		m_template = in.weapon;
		m_fields.read(in.weapon);
		m_read = true;
	}
	const Fields& f = m_fields;
	// The seat's second gun (0x007c2d50, the seat's weapon 1) sets the
	// Secondary* variables as 0x007c1f10 the Primary* ones.
	const std::string P = secondary ? "Secondary" : "Primary";
	const auto var = [&P](const char* name) { return P + name; };
	const bool showAmmo = f.showAmmo, showClips = f.showClips, battery = f.battery;
	hud.set(var("AmmoShow"), f.guiShown ? 1 : 0);
	hud.set(var("AmmoBarShow"), showAmmo ? 1 : 0);
	hud.set(var("ClipsShow"), showClips ? 1 : 0);
	if (!f.name.empty()) {
		hud.setString(var("WeaponNameString"), f.name);
	}
	if (!secondary && !f.crosshair.empty()) {
		hud.setString("CrosshairIconPathString", f.crosshair);
	}
	if (!f.barFront.empty()) {
		hud.setString(var("AmmoBarFrontPathString"), f.barFront);
	}
	if (!f.barBack.empty()) {
		hud.setString(var("AmmoBarBackPathString"), f.barBack);
	}
	const bool magazine = f.hasMag && in.magSize >= 0;
	hud.set(var("AmmoIconShow"), magazine ? 0 : 1);
	if (magazine) {
		if (!f.clipIcon.empty()) {
			hud.setString(var("ClipPathString"), f.clipIcon);
		}
	} else if (!f.ammoIcon.empty()) {
		hud.setString(var("AmmoIconPathString"), f.ammoIcon);
	}
	if (!secondary) {
		// 0x007be640.
		const int rate = f.hasFireRate ? in.fireRate : -1;
		hud.set("FireRateIconShow", rate >= 0 ? 1 : 0);
		hud.setString("FireRateIconPathString",
			rate >= 0 ? "Ingame/Weapons/Icons/Hud/firerate_" + std::to_string(rate) + ".tga" : "Ingame/Weapons/Icons/Hud/firerate_0");
		// 0x007c1470: the distance counter (hasDistanceCounter: the
		// projectile's distance, "999" (DAT_009b7910) without one) and the
		// range finder (hasRangeFinder: the view ray's hit, 999 m out,
		// DAT_00b2d148 slot 0x18c, its distance from the controlled object
		// "%i" and the point "%i" x, -z, y; a miss "999"; not with a comm
		// rose or map menu up, menu states 3-6, 8, 9).
		if (f.distanceCounter) {
			hud.setString("TargetDistanceString", in.travelled >= 0 ? std::to_string(in.travelled) : "999");
		}
		if (f.rangeFinder) {
			if (in.rangeHit) {
				hud.setString("TargetDistanceString", std::to_string(static_cast<int>(std::lround(in.targetDistance))));
				hud.setString("TargetCoordXString", std::to_string(static_cast<int>(std::lround(in.target[0]))));
				hud.setString("TargetCoordYString", std::to_string(-static_cast<int>(std::lround(in.target[2]))));
				hud.setString("TargetCoordZString", std::to_string(static_cast<int>(std::lround(in.target[1]))));
			} else {
				for (const char* name : {"TargetDistanceString", "TargetCoordXString", "TargetCoordYString", "TargetCoordZString"}) {
					hud.setString(name, "999");
				}
			}
		}
		// 0x007be380: the scan (hasScanProgress, the weapon's 0xc4f2 slots
		// 0xf8 / 0xfc).
		hud.set("HudScanInProgress", f.scanProgress && in.scanning ? 1 : 0);
		hud.set("HudScanProgress", f.scanProgress ? in.scan : 0);
	}

	const bool reloading = in.reload > 0 && in.reload < 1;
	const bool eternal = in.magSize < 0;
	hud.set(var("EternalShow"), 0);
	if (!showAmmo || reloading) {
		// 0x007bede0: the texts off.
		hud.set(var("AmmoTextStringShow"), 0);
		hud.set(var("ReloadTextStringShow"), 0);
		hud.set(var("HeatTextStringShow"), 0);
	} else if (eternal) {
		setColor(hud, var("AmmoColor"), hudLevelColor(1, 0.3f));
		hud.setString(var("AmmoString"), "");
		hud.setString("Tot" + P + "AmmoString", "");
		hud.set(var("Ammo"), 1);
		hud.set(var("AmmoBlink"), 0);
		hud.set(var("EternalShow"), 1);
		hud.set(var("SingleFireShow"), 0);
	} else {
		const float share = in.magSize > 0 ? static_cast<float>(in.rounds) / static_cast<float>(in.magSize) : 0;
		hud.set(var("Ammo"), share);
		hud.setString(var("AmmoString"), std::to_string(in.rounds));
		hud.setString("Tot" + P + "AmmoString", "/ " + std::to_string((in.mags - 1) * in.magSize + in.rounds));
		const bool single = in.magSize < 2;
		setColor(hud, var("AmmoColor"), hudLevelColor(share, single ? 0.1f : 0.3f));
		hud.set(var("SingleFireShow"), single ? 1 : 0);
	}
	hud.set(var("EternalMagsShow"), in.nrOfMags < 0 && !eternal ? 1 : 0);
	hud.set(var("BatteryIconShow"), battery ? 1 : 0);
	if (battery) {
		hud.set(var("SingleFireShow"), 0);
	}
	// The reload and the heat.
	hud.set(var("ReloadTime"), in.reload);
	hud.set(var("ReloadTimeShow"), reloading ? 1 : 0);
	hud.set(var("HeatValue"), in.heat);
	hud.set(var("HeatShow"), in.heat > 0 ? 1 : 0);
	hud.set(var("HeatBlink"), in.overheated ? 1 : 0);
	std::string sound;
	if (in.overheated && !m_overheatPlayed) {
		sound = f.overheatSound;
		m_overheatPlayed = true;
	} else if (!in.overheated) {
		m_overheatPlayed = false;
	}
	// The clips (0x007be960).
	if (showClips && magazine) {
		const int spare = in.mags - 1;
		hud.set(var("Clip1Show"), spare > 0 ? 1 : 0);
		hud.set(var("Clip2Show"), spare > 1 ? 1 : 0);
		hud.set(var("Clip3Show"), spare > 2 ? 1 : 0);
		hud.set(var("ClipStringShow"), spare > -1 ? 1 : 0);
		if (spare > -1) {
			hud.setString(var("ClipString"), "(" + std::to_string(spare) + ")");
		}
		hud.set(var("Clips"), static_cast<float>(spare));
		const float clipShare = in.nrOfMags > 0 ? static_cast<float>(spare) / static_cast<float>(in.nrOfMags) : 1.0f;
		setColor(hud, var("ClipColor"), hudLevelColor(clipShare, 0.3f));
	} else {
		setColor(hud, var("ClipColor"), hudLevelColor(-10, 0.3f));
		hud.set(var("ClipBlink"), 0);
		hud.set(var("Clips"), -1);
		hud.set(var("Clip1Show"), 0);
		hud.set(var("Clip2Show"), 0);
		hud.set(var("Clip3Show"), 0);
		hud.set(var("ClipStringShow"), 0);
	}
	return sound;
}

// --- The item selection (0x0079bc70..0x0079c720) ---

void ItemSelection::bind(Console& console)
{
	// 0x007609b5 → the selection's +8.
	bindMethod(console, "ItemSelection.setReverseItems", ConsoleAccess::Anyone, [this](bool reverse) { setReverse(reverse); });
}

void ItemSelection::open(const std::vector<SelectItem>& items, int selectedIndex, float seconds)
{
	// 0x0079c290: each item at its index (10 - index reversed), below 10;
	// 0x0079be20 packs them down from slot 0.
	std::array<Slot, 10> placed {};
	for (const SelectItem& item : items) {
		const int at = m_reverse ? 10 - item.index : item.index;
		if (item.index <= 0 || at < 0 || at >= 10) {
			continue;
		}
		placed[static_cast<size_t>(at)] = {true, item.index, item.icon, item.usable ? 1.0f : 0.4f};
	}
	m_slots = {};
	size_t next = 0;
	for (const Slot& slot : placed) {
		if (slot.active) {
			m_slots[next++] = slot;
		}
	}
	m_selected = 0;
	for (size_t i = 0; i < next; ++i) {
		if (m_slots[i].index == selectedIndex) {
			m_selected = static_cast<int>(i); // 0x0079bdc0
		}
	}
	m_shown = next > 0; // 0x0079c630
	m_timer = seconds;
}

void ItemSelection::update(hud::HudManager& hud, float dt)
{
	// 0x0079c050: the timer runs down; at its end the list closes.
	if (m_shown) {
		m_timer -= dt;
		if (m_timer < 0) {
			m_shown = false;
			m_timer = -1;
		}
	}
	hud.set("ShowItemSelect", m_shown ? 1 : 0);
	hud.set("ItemSelectActive", static_cast<float>(m_selected));
	for (size_t i = 0; i < m_slots.size(); ++i) {
		const std::string n = std::to_string(i);
		const Slot& slot = m_slots[i];
		hud.set("ItemSlot" + n + "Active", slot.active ? 1 : 0);
		hud.set("ItemSlot" + n + "Alpha", slot.alpha);
		hud.setString("Item" + n + "Icon", slashes(slot.icon));
		hud.setString("Item" + n + "Index", slot.active ? std::to_string(slot.index) : std::string());
	}
}

// --- The vehicle HUD ---

int seatDotState(bool occupied, bool local, int squad, int localSquad, bool sameTeam)
{
	if (!occupied) {
		return 4;
	}
	if (local) {
		return localSquad != 0 ? 1 : 2;
	}
	if (squad != 0 && squad == localSquad) {
		return 7;
	}
	return sameTeam ? 5 : 6;
}

void VehicleHud::update(hud::HudManager& hud, const VehicleHudInput& in)
{
	const world::ObjectTemplate* v = in.vehicle;
	if (v != m_template || !v) {
		m_template = v;
		m_name = hudSetting(v, "vehicleHud.hudName");
		m_turretIcon = flag(v, "vehicleHud.hasTurretIcon", false);
		m_climbSpeed = std::max(hudNumber(v, "vehicleHud.maxClimbSpeed", 10), 1.0f);
	}
	// 0x007a2e60: the name and the turret icon.
	if (!m_name.empty()) {
		hud.setString("VehicleNameString", m_name);
	}
	hud.set("TurretIconShow", m_turretIcon ? 1 : 0);
	// 0x007a18a0: the armour's share, its text (per cent) and colour.
	const float armor = std::clamp(in.armor, 0.0f, 1.0f);
	hud.set("VehicleArmor", armor);
	hud.setString("VehicleArmorString", std::to_string(static_cast<int>(std::ceil(armor * 100.0f))));
	setColor(hud, "VehicleArmorColor", hudLevelColor(armor, 0.3f));
	hud.set("VehicleArmorShow", 1);
	hud.set("VehicleHealthTextStringShow", 1);
	// The speed (round(|v|) x 10) and the climb (0x007a2b50's end).
	hud.setString("SpeedString", std::to_string(static_cast<int>(std::lround(in.speed)) * 10));
	const float climbSpeed = m_climbSpeed;
	const float angle = (12.566371f / climbSpeed) * in.climb * 0.1f - m_climbPrevious + m_climbAngle;
	m_climbAngle = m_climbPrevious = angle;
	hud.set("VehicleElevationSpeedAngle", angle);
	hud.setString("AltitudeString", std::to_string(static_cast<int>(std::ceil(in.altitude))));
	// The seat dots (0x007a28e0): states 0 (unset) .. 7 per seat, at the
	// seat's icon position.
	std::vector<int> states(10, 0);
	for (size_t i = 0; i < in.seats.size() && i < states.size(); ++i) {
		const SeatDot& seat = in.seats[i];
		states[i] = seat.x == 0 && seat.y == 0 ? 0 : seat.state;
		const std::string n = std::to_string(i + 1);
		hud.set("OccupyingPlayerPos" + n + "X", seat.x);
		hud.set("OccupyingPlayerPos" + n + "Y", seat.y);
	}
	hud.setOccupiedStates(1, std::move(states));
	// The passengers' names (0x007a34f0).
	const std::array<PassengerLine, 10> lines = passengerLines(in.seats);
	for (size_t i = 0; i < lines.size(); ++i) {
		const std::string n = std::to_string(i + 1);
		hud.set("OccupyingPlayerName" + n + "Show", lines[i].text.empty() ? 0 : 1);
		hud.setString("OccupyingPlayerName" + n + "String", lines[i].text);
		hud.set("OccupyingPlayer" + n + "ColorRed", lines[i].colour[0]);
		hud.set("OccupyingPlayer" + n + "ColorGreen", lines[i].colour[1]);
		hud.set("OccupyingPlayer" + n + "ColorBlue", lines[i].colour[2]);
	}
	// The bottom-left vehicle panel (0x007a0a90: the faded alphas are the
	// panel's alpha less 1 - MenuBackgroundAlpha; the panels' swap is the
	// HUD animation "BottomLeft/Alpha", here taken as done).
	const float background = hud.number("MenuBackgroundAlpha");
	hud.set("BottomLeftVehicleAlpha", 1);
	hud.set("BottomLeftHealthAlpha", 0);
	hud.set("BottomLeftVehicleFadedAlpha", std::clamp(1 - (1 - background), 0.0f, 1.0f));
	hud.set("BottomLeftHealthFadedAlpha", 0);
}

std::array<PassengerLine, 10> passengerLines(const std::vector<SeatDot>& seats)
{
	std::vector<PassengerLine> names;
	for (size_t i = 0; i < seats.size() && i < 10; ++i) {
		const SeatDot& seat = seats[i];
		if (seat.relation == 0) {
			continue;
		}
		PassengerLine line;
		line.text = std::to_string(i + 1) + ". " + seat.occupant;
		line.colour = seat.relation == 3 ? std::array<float, 3> {0.4f, 1, 0}
			: seat.relation == 2          ? std::array<float, 3> {0.4f, 0.6f, 1}
										  : std::array<float, 3> {1, 0.4f, 0.4f};
		names.push_back(line);
	}
	std::array<PassengerLine, 10> lines {};
	for (size_t k = 0; k < names.size(); ++k) {
		lines[names.size() - 1 - k] = names[k];
	}
	return lines;
}

void VehicleHud::clear(hud::HudManager& hud)
{
	for (int i = 1; i <= 10; ++i) {
		hud.set("OccupyingPlayerName" + std::to_string(i) + "Show", 0);
	}
	m_climbAngle = m_climbPrevious = 0;
	hud.setOccupiedStates(1, {});
	hud.set("BottomLeftVehicleAlpha", 0);
	hud.set("BottomLeftHealthAlpha", 1);
	hud.set("BottomLeftVehicleFadedAlpha", 0);
	hud.set("BottomLeftHealthFadedAlpha", std::clamp(hud.number("MenuBackgroundAlpha"), 0.0f, 1.0f));
}

// --- The map's vehicles ---

MapRelation vehicleRelation(bool hasOccupant, bool localInside, int occupantTeam, int vehicleTeam, int localTeam)
{
	if (localInside) {
		return MapRelation::Own;
	}
	if (hasOccupant) {
		return occupantTeam == localTeam ? MapRelation::Friendly : MapRelation::Enemy;
	}
	if (vehicleTeam == 0) {
		return MapRelation::Neutral;
	}
	return vehicleTeam == localTeam ? MapRelation::Friendly : MapRelation::Enemy;
}

bool vehicleMarker(const world::ObjectTemplate& vehicle, MapRelation relation, bool inside, bool spotted, VehicleMarker& out)
{
	const std::string icon = hudSetting(&vehicle, inside ? "vehicleHud.miniMapIconInside" : "vehicleHud.miniMapIcon");
	const std::string texture = icon.empty() ? hudSetting(&vehicle, "vehicleHud.miniMapIcon") : icon;
	if (texture.empty()) {
		return false;
	}
	// 0x00793b60: friends', neutral and his own always; the enemy's while
	// spotted or with showOnEnemyMinimap; any with alwaysShowOnMinimap.
	bool shown = relation != MapRelation::Enemy || spotted;
	if (relation == MapRelation::Enemy && flag(&vehicle, "vehicleHud.showOnEnemyMinimap", false)) {
		shown = true;
	}
	if (flag(&vehicle, "vehicleHud.alwaysShowOnMinimap", false)) {
		shown = true;
	}
	if (!shown) {
		return false;
	}
	out.texture = slashes(texture);
	const auto size = pair(&vehicle, inside && !icon.empty() ? "vehicleHud.miniMapIconSizeInside" : "vehicleHud.miniMapIconSize", 16);
	out.width = size[0];
	out.height = size[1];
	out.asPlayer = flag(&vehicle, "vehicleHud.usePlayerIcon", false) && relation != MapRelation::Neutral;
	return true;
}

} // namespace dice::hfe::menu
