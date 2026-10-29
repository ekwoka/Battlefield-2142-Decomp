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

std::string WeaponHud::update(hud::HudManager& hud, const WeaponHudInput& in)
{
	const world::ObjectTemplate* w = in.weapon;
	const bool showAmmo = flag(w, "weaponHud.showAmmo", true);   // template +0x11a
	const bool showClips = flag(w, "weaponHud.showClips", true); // +0x11b
	const bool hasMag = flag(w, "weaponHud.hasMag", true);       // +0x117
	const bool hasFireRate = flag(w, "weaponHud.hasFireRate", true); // +0x116
	const bool battery = flag(w, "weaponHud.showBatteryIconInHud", false); // +0x127
	hud.set("PrimaryAmmoShow", hudNumber(w, "weaponHud.guiIndex", 5) != -1 ? 1 : 0);
	hud.set("PrimaryAmmoBarShow", showAmmo ? 1 : 0);
	hud.set("PrimaryClipsShow", showClips ? 1 : 0);
	if (const std::string name = hudSetting(w, "weaponHud.hudName"); !name.empty()) {
		hud.setString("PrimaryWeaponNameString", name);
	}
	if (const std::string icon = hudSetting(w, "weaponHud.crosshairIcon"); !icon.empty()) {
		hud.setString("CrosshairIconPathString", slashes(icon));
	}
	if (const std::string icon = hudSetting(w, "weaponHud.ammoBarFrontIcon"); !icon.empty()) {
		hud.setString("PrimaryAmmoBarFrontPathString", slashes(icon));
	}
	if (const std::string icon = hudSetting(w, "weaponHud.ammoBarBackIcon"); !icon.empty()) {
		hud.setString("PrimaryAmmoBarBackPathString", slashes(icon));
	}
	const bool magazine = hasMag && in.magSize >= 0;
	hud.set("PrimaryAmmoIconShow", magazine ? 0 : 1);
	if (magazine) {
		if (const std::string icon = hudSetting(w, "weaponHud.clipIcon"); !icon.empty()) {
			hud.setString("PrimaryClipPathString", slashes(icon));
		}
	} else if (const std::string icon = hudSetting(w, "weaponHud.ammoIcon"); !icon.empty()) {
		hud.setString("PrimaryAmmoIconPathString", slashes(icon));
	}
	// 0x007be640.
	const int rate = hasFireRate ? in.fireRate : -1;
	hud.set("FireRateIconShow", rate >= 0 ? 1 : 0);
	hud.setString("FireRateIconPathString",
		rate >= 0 ? "Ingame/Weapons/Icons/Hud/firerate_" + std::to_string(rate) + ".tga" : "Ingame/Weapons/Icons/Hud/firerate_0");

	const bool reloading = in.reload > 0 && in.reload < 1;
	const bool eternal = in.magSize < 0;
	hud.set("PrimaryEternalShow", 0);
	if (!showAmmo || reloading) {
		// 0x007bede0: the texts off.
		hud.set("PrimaryAmmoTextStringShow", 0);
		hud.set("PrimaryReloadTextStringShow", 0);
		hud.set("PrimaryHeatTextStringShow", 0);
	} else if (eternal) {
		setColor(hud, "PrimaryAmmoColor", hudLevelColor(1, 0.3f));
		hud.setString("PrimaryAmmoString", "");
		hud.setString("TotPrimaryAmmoString", "");
		hud.set("PrimaryAmmo", 1);
		hud.set("PrimaryAmmoBlink", 0);
		hud.set("PrimaryEternalShow", 1);
		hud.set("PrimarySingleFireShow", 0);
	} else {
		const float share = in.magSize > 0 ? static_cast<float>(in.rounds) / static_cast<float>(in.magSize) : 0;
		hud.set("PrimaryAmmo", share);
		hud.setString("PrimaryAmmoString", std::to_string(in.rounds));
		hud.setString("TotPrimaryAmmoString", "/ " + std::to_string((in.mags - 1) * in.magSize + in.rounds));
		const bool single = in.magSize < 2;
		setColor(hud, "PrimaryAmmoColor", hudLevelColor(share, single ? 0.1f : 0.3f));
		hud.set("PrimarySingleFireShow", single ? 1 : 0);
	}
	hud.set("PrimaryEternalMagsShow", in.nrOfMags < 0 && !eternal ? 1 : 0);
	hud.set("PrimaryBatteryIconShow", battery ? 1 : 0);
	if (battery) {
		hud.set("PrimarySingleFireShow", 0);
	}
	// The reload and the heat.
	hud.set("PrimaryReloadTime", in.reload);
	hud.set("PrimaryReloadTimeShow", reloading ? 1 : 0);
	hud.set("PrimaryHeatValue", in.heat);
	hud.set("PrimaryHeatShow", in.heat > 0 ? 1 : 0);
	hud.set("PrimaryHeatBlink", in.overheated ? 1 : 0);
	std::string sound;
	if (in.overheated && !m_overheatPlayed) {
		sound = hudSetting(w, "weaponHud.overheatSound");
		m_overheatPlayed = true;
	} else if (!in.overheated) {
		m_overheatPlayed = false;
	}
	// The clips (0x007be960).
	if (showClips && magazine) {
		const int spare = in.mags - 1;
		hud.set("PrimaryClip1Show", spare > 0 ? 1 : 0);
		hud.set("PrimaryClip2Show", spare > 1 ? 1 : 0);
		hud.set("PrimaryClip3Show", spare > 2 ? 1 : 0);
		hud.set("PrimaryClipStringShow", spare > -1 ? 1 : 0);
		if (spare > -1) {
			hud.setString("PrimaryClipString", "(" + std::to_string(spare) + ")");
		}
		hud.set("PrimaryClips", static_cast<float>(spare));
		const float clipShare = in.nrOfMags > 0 ? static_cast<float>(spare) / static_cast<float>(in.nrOfMags) : 1.0f;
		setColor(hud, "PrimaryClipColor", hudLevelColor(clipShare, 0.3f));
	} else {
		setColor(hud, "PrimaryClipColor", hudLevelColor(-10, 0.3f));
		hud.set("PrimaryClipBlink", 0);
		hud.set("PrimaryClips", -1);
		hud.set("PrimaryClip1Show", 0);
		hud.set("PrimaryClip2Show", 0);
		hud.set("PrimaryClip3Show", 0);
		hud.set("PrimaryClipStringShow", 0);
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
	// 0x007a2e60: the name and the turret icon.
	if (const std::string name = hudSetting(v, "vehicleHud.hudName"); !name.empty()) {
		hud.setString("VehicleNameString", name);
	}
	hud.set("TurretIconShow", flag(v, "vehicleHud.hasTurretIcon", false) ? 1 : 0);
	// 0x007a18a0: the armour's share, its text (per cent) and colour.
	const float armor = std::clamp(in.armor, 0.0f, 1.0f);
	hud.set("VehicleArmor", armor);
	hud.setString("VehicleArmorString", std::to_string(static_cast<int>(std::ceil(armor * 100.0f))));
	setColor(hud, "VehicleArmorColor", hudLevelColor(armor, 0.3f));
	hud.set("VehicleArmorShow", 1);
	hud.set("VehicleHealthTextStringShow", 1);
	// The speed (round(|v|) x 10) and the climb (0x007a2b50's end).
	hud.setString("SpeedString", std::to_string(static_cast<int>(std::lround(in.speed)) * 10));
	const float climbSpeed = std::max(hudNumber(v, "vehicleHud.maxClimbSpeed", 10), 1.0f);
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
	// The bottom-left vehicle panel (0x007a0a90: the faded alphas are the
	// panel's alpha less 1 - MenuBackgroundAlpha; the panels' swap is the
	// HUD animation "BottomLeft/Alpha", here taken as done).
	const float background = hud.number("MenuBackgroundAlpha");
	hud.set("BottomLeftVehicleAlpha", 1);
	hud.set("BottomLeftHealthAlpha", 0);
	hud.set("BottomLeftVehicleFadedAlpha", std::clamp(1 - (1 - background), 0.0f, 1.0f));
	hud.set("BottomLeftHealthFadedAlpha", 0);
}

void VehicleHud::clear(hud::HudManager& hud)
{
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
