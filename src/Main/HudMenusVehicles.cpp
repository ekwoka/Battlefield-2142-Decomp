#include "HudMenusVehicles.h"

#include "HudAlerts.h"
#include "HudWorld.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Hud/Hud.h"
#include "World/Object.h"
#include "World/ObjectTemplate.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cstdlib>
#include <unordered_map>

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
	showInfo = flag(w, "weaponHud.showInfo", true); // +0x114 (block +0xd0)
	name = hudSetting(w, "weaponHud.hudName");
	crosshair = slashes(hudSetting(w, "weaponHud.crosshairIcon"));
	altCrosshair = slashes(hudSetting(w, "weaponHud.altCrosshairIcon")); // block +0x8c (executor 0x005576cc)
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
	if (!m_blinksMade) {
		// 0x007bee10: the secondary's clip blink starts at 0.7 spare
		// magazines, the primary's at 1.5.
		m_blinksMade = true;
		if (secondary) {
			m_clipBlink = HudBlink {0.7f, 0, 0.3f, 0.15f};
		}
	}
	// The seat's second gun (0x007c2d50, the PIAltFire group's active
	// weapon) sets the Secondary* variables as 0x007c1f10 the Primary* ones.
	const std::string P = secondary ? "Secondary" : "Primary";
	const auto var = [&P](const char* name) { return P + name; };
	if (!f.showInfo) {
		// showInfo 0 (block +0xd0: the horns, Car_horn / Car_horn_Alt): the
		// primary frame runs 0x007c1b90 (the show flags +0x18 / +0x16d off)
		// and stops after the crosshair's parts, before the name, the icons
		// and the ammunition (PrimaryAmmoShow +0x15d stays off); the
		// secondary's (0x007c2d50) clears its show +0x16d and stops.
		hud.set(var("AmmoShow"), 0);
		return {};
	}
	const bool showAmmo = f.showAmmo, showClips = f.showClips, battery = f.battery;
	hud.set(var("AmmoShow"), f.guiShown ? 1 : 0);
	hud.set(var("AmmoBarShow"), showAmmo ? 1 : 0);
	hud.set(var("ClipsShow"), showClips ? 1 : 0);
	if (!f.name.empty()) {
		hud.setString(var("WeaponNameString"), f.name);
	}
	// 0x00657bb0: the alternate one while firing, when it has one.
	const std::string& crosshair = in.firing && !f.altCrosshair.empty() ? f.altCrosshair : f.crosshair;
	if (!secondary && !crosshair.empty()) {
		hud.setString("CrosshairIconPathString", crosshair);
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
		m_ammo = 1;
		hud.set(var("Ammo"), 1);
		m_ammoBlink.off();
		hud.set(var("EternalShow"), 1);
		hud.set(var("SingleFireShow"), 0);
	} else {
		const float share = in.magSize > 0 ? static_cast<float>(in.rounds) / static_cast<float>(in.magSize) : 0;
		m_ammo = share;
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
		m_clips = static_cast<float>(spare);
		hud.set(var("Clips"), m_clips);
		const float clipShare = in.nrOfMags > 0 ? static_cast<float>(spare) / static_cast<float>(in.nrOfMags) : 1.0f;
		setColor(hud, var("ClipColor"), hudLevelColor(clipShare, 0.3f));
	} else {
		setColor(hud, var("ClipColor"), hudLevelColor(-10, 0.3f));
		m_clipBlink.off();
		m_clips = -1;
		hud.set(var("Clips"), -1);
		hud.set(var("Clip1Show"), 0);
		hud.set(var("Clip2Show"), 0);
		hud.set(var("Clip3Show"), 0);
		hud.set(var("ClipStringShow"), 0);
	}
	// The blinks, after the update (0x007c40c0 → 0x00778420).
	hud.set(var("AmmoBlink"), m_ammoBlink.tick(m_ammo, in.seconds) ? 1 : 0);
	hud.set(var("ClipBlink"), m_clipBlink.tick(m_clips, in.seconds) ? 1 : 0);
	hud.set(var("HeatBlink"), m_heatBlink.tick(in.heat, in.seconds) ? 1 : 0);
	if (!secondary) {
		hud.set("TVBlink", m_tvBlink.tickFree(in.seconds) ? 1 : 0);
	}
	return sound;
}

void WeaponHud::hide(hud::HudManager& hud, bool secondary)
{
	// 0x007c40c0's other branch: +0xb4 / +0xc4 = 0 and the ammunition and
	// clip blinks (+0x15e, +0x165, +0x16a, +0x172) off; the heat and TV
	// blinks keep their flags.
	const std::string P = secondary ? "Secondary" : "Primary";
	m_ammo = 0;
	m_ammoBlink.off();
	m_clipBlink.off();
	hud.set(P + "Ammo", 0);
	hud.set(P + "AmmoBlink", 0);
	hud.set(P + "ClipBlink", 0);
}

// --- The crosshair's parts (0x007be770) ---

CrosshairParts crosshairParts(float deviation, int deviationFactor, int minVisual, int maxVisual, float length)
{
	CrosshairParts parts;
	const int spread = static_cast<int>(std::ceil(deviation * static_cast<float>(deviationFactor)));
	// maxVisualDeviation < minVisualDeviation + spread: the maximum.
	parts.offset = maxVisual < minVisual + spread ? maxVisual : minVisual + spread;
	const float o = static_cast<float>(parts.offset);
	parts.up = 300 - length - o;
	parts.down = 301 + o;
	parts.left = 399 - length - o;
	parts.right = 400 + o;
	return parts;
}

CrosshairParts crosshairParts(float deviation, const world::ObjectTemplate* weapon, float length)
{
	return crosshairParts(deviation, static_cast<int>(hudNumber(weapon, "weaponHud.deviationFactor", 10)),
		static_cast<int>(hudNumber(weapon, "weaponHud.minVisualDeviation", 0)),
		static_cast<int>(hudNumber(weapon, "weaponHud.maxVisualDeviation", 1000)), length);
}

// --- The Titans' consoles (0x0044b680 case 0x32, 0x00448850) ---

// --- The ability bar (0x007c3af0) ---

namespace {
constexpr const char* kAbilityAmmoIcon = "Ingame/Player/Bars/abilitybar_smallAmmoIcon.tga";
constexpr const char* kAbilityMedicIcon = "Ingame/Player/Bars/abilitybar_smallMedicIcon.tga";
constexpr const char* kAbilityRepairIcon = "Ingame/Player/Bars/abilitybar_smallRepairIcon.tga";
constexpr int64_t kGiftTicks = 15; // the stamp's tick + 0xf
} // namespace

const AbilityBar::Out& AbilityBar::update(const game::GameWorld::AbilityBarInfo& info, bool inVehicle)
{
	m_out.show = m_out.wrench = m_out.valueShow = false;
	const auto recent = [](int64_t since) { return since >= 0 && since < kGiftTicks; };
	if (inVehicle) {
		if (info.healFlag && recent(info.sinceHeal)) {
			m_out.icon = info.repairFlag ? kAbilityRepairIcon : kAbilityMedicIcon;
			m_out.show = true;
		} else if (info.ammoFlag && recent(info.sinceAmmo)) {
			m_out.icon = kAbilityAmmoIcon;
			m_out.show = true;
		}
		if (m_out.show) {
			m_out.bar = 1;
		}
		m_out.sound = !m_out.show ? -1
			: info.healFlag      ? (info.repairFlag ? 1 : 0)
			: info.ammoFlag      ? 2
								 : -1;
		return m_out;
	}
	if (!info.kitAbility) {
		m_out.sound = -1; // no Ability component (every shipped kit has one)
		return m_out;
	}
	float value = 0;
	for (size_t i = 0; i < info.targets.size(); ++i) {
		float share = info.targets[i];
		if (!info.kitAmmo && share < 0) {
			continue; // no armour
		}
		if (i != 0) {
			share = (share + value) * 0.5f;
		}
		value = share;
		m_out.show = true;
		m_out.wrench = !info.kitAmmo;
	}
	if (info.alive) {
		int kind = -1;
		if (info.kitAmmo) {
			kind = 2;
			if (m_out.show) {
				m_out.icon = kAbilityAmmoIcon;
				m_out.bar = value;
			}
		} else if (info.kitRepairing) {
			kind = 1;
			if (m_out.show) {
				m_out.icon = kAbilityRepairIcon;
				m_out.bar = value;
			}
		} else if (info.kitHealing) {
			kind = 0;
			if (m_out.show) {
				m_out.icon = kAbilityMedicIcon;
				m_out.bar = value;
			}
		} else if (m_out.show) {
			m_out.icon = kAbilityRepairIcon;
			m_out.bar = 1 - value;
		}
		m_out.sound = kind >= 0 && info.active && m_out.show ? kind : -1;
	} else {
		m_out.sound = -1;
	}
	if (info.replenishingType != 3) {
		m_out.valueShow = true;
		m_out.value = info.battery;
	}
	if (!info.alive) {
		m_out.show = false;
	}
	return m_out;
}

const AbilityBar::Out& AbilityBar::hide()
{
	m_out.show = m_out.wrench = false;
	m_out.sound = -1;
	return m_out;
}

void AbilityBar::apply(hud::HudManager& hud) const
{
	hud.set("AbilityBarShow", m_out.show ? 1 : 0);
	hud.set("WrenchRepairingShow", m_out.wrench ? 1 : 0);
	hud.set("AbilityValueShow", m_out.valueShow ? 1 : 0);
	hud.set("AbilityBar", m_out.bar);
	hud.set("AbilityValue", m_out.value);
	hud.setString("AbilityIconPath", m_out.icon);
	if (m_out.valueShow) {
		hud.set("PrimaryAmmo", m_out.value);
	}
}

void StrategicObjects::destroyed(int team, const world::ObjectTemplate* t)
{
	if (!t || (team != 1 && team != 2) || !flag(t, "vehicleHud.sendDestroyedHudEvent", false)) {
		return;
	}
	const int index = static_cast<int>(hudNumber(t, "vehicleHud.guiIndex", 0));
	if (index >= 0 && index < 10) {
		m_alive[static_cast<size_t>((team - 1) * 10 + index)] = false;
	}
}

void StrategicObjects::update(hud::HudManager& hud, int localTeam) const
{
	const int own = localTeam == 1 ? 1 : 2, other = 3 - own;
	for (int i = 0; i < 10; ++i) {
		hud.set("FriendlyStrategicObject" + std::to_string(i) + "Show", shown(own, i) ? 1 : 0);
		hud.set("EnemyStrategicObject" + std::to_string(i) + "Show", shown(other, i) ? 1 : 0);
	}
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
		placed[static_cast<size_t>(at)] = {true, item.index, item.icon, item.usable ? 1.0f : 0.4f, item.usable};
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
	m_seconds = seconds;
	m_timer = seconds;
	m_flash = -1;
	m_flashCount = 0;
	m_flashTimer = -1;
}

void ItemSelection::flash(int itemIndex)
{
	// 0x0079c190: the old flash's slot shown, six toggles 0.1 s apart, the
	// slot of the index (0 without one) and the open time anew.
	if (m_flash >= 0 && static_cast<size_t>(m_flash) < m_slots.size()) {
		m_slots[static_cast<size_t>(m_flash)].active = true;
	}
	m_flashCount = 6;
	m_flashTimer = 0.1f;
	m_flash = 0;
	for (size_t i = 0; i < m_slots.size(); ++i) {
		if (m_slots[i].active && m_slots[i].index == itemIndex) {
			m_flash = static_cast<int>(i);
			break;
		}
	}
	m_timer = m_seconds;
}

void ItemSelection::select(int itemIndex)
{
	// 0x0079c120: the slot of that index (the first without one), the open
	// time anew (+0x24 = +0x20).
	int at = 0;
	for (size_t i = 0; i < m_slots.size() && m_slots[i].active; ++i) {
		if (m_slots[i].index == itemIndex) {
			at = static_cast<int>(i);
			break;
		}
	}
	m_selected = at;
	m_timer = m_seconds;
}

namespace {

int activeSlots(const std::array<ItemSelection::Slot, 10>& slots)
{
	int n = 0;
	while (n < static_cast<int>(slots.size()) && slots[static_cast<size_t>(n)].active) {
		++n;
	}
	return n;
}

} // namespace

bool ItemSelection::next()
{
	const int count = activeSlots(m_slots); // +0x14
	if (count == 0) {
		return false;
	}
	const int at = std::max(0, m_selected);
	if (at + 1 < count && m_slots[static_cast<size_t>(at + 1)].usable) {
		select(m_slots[static_cast<size_t>(at + 1)].index);
	} else if (m_slots[0].usable) {
		select(m_slots[0].index);
	} else {
		return false; // 0x0079c1e0 leaves it as it is (no sound)
	}
	m_shown = true;
	return true;
}

bool ItemSelection::previous()
{
	const int count = activeSlots(m_slots);
	if (count == 0) {
		return false;
	}
	const int at = std::max(0, m_selected);
	if (at > 0 && m_slots[static_cast<size_t>(at - 1)].usable) {
		select(m_slots[static_cast<size_t>(at - 1)].index);
	} else if (m_slots[static_cast<size_t>(count - 1)].usable) {
		select(m_slots[static_cast<size_t>(count - 1)].index);
	} else {
		select(m_slots[0].index);
	}
	m_shown = true;
	return true;
}

int ItemSelection::selectedItem() const
{
	if (m_selected < 0 || m_selected >= activeSlots(m_slots)) {
		return -1;
	}
	return m_slots[static_cast<size_t>(m_selected)].index;
}

std::string ItemListInput::apply(ItemSelection& list, bool& fire, int& item, int itemStep, double now)
{
	if (fire) {
		if (list.shown()) {
			const int selected = list.selectedItem();
			if (selected >= 1) {
				item = std::min(selected, 9); // 0xe..0x16: PIWeaponSelect1..9
			}
			fire = false;
			fireBlockUntil = now + 0.3;
		} else if (now < fireBlockUntil) {
			fire = false;
		}
		return {};
	}
	if (itemStep == 0 || !allowed || items.empty()) {
		return {};
	}
	if (!list.shown()) {
		list.setSeconds(2.0f);
		list.open(items, inHand, 2.0f);
	}
	const bool moved = itemStep > 0 ? list.next() : list.previous();
	return moved ? "weaponOver" : "";
}

void ItemSelection::update(hud::HudManager& hud, float dt)
{
	// 0x0079c050: the timer runs down; at its end the list closes (the
	// flashing slot shown again). While a key's pick flashes, each 0.1 s
	// its slot's Active flips and the open time starts again; after the
	// sixth the flash ends.
	if (!m_shown || m_timer < 0) {
		if (m_flash >= 0 && static_cast<size_t>(m_flash) < m_slots.size()) {
			m_slots[static_cast<size_t>(m_flash)].active = true;
		}
		m_shown = false;
		m_timer = -1;
		m_flash = -1;
		m_flashCount = 0;
		m_flashTimer = -1;
	} else {
		m_timer -= dt;
		if (m_flashTimer >= 0) {
			m_flashTimer -= dt;
		}
		if (m_flash >= 0 && m_flashCount > 0 && m_flashTimer <= 0) {
			Slot& slot = m_slots[static_cast<size_t>(m_flash)];
			slot.active = !slot.active;
			m_timer = m_seconds;
			m_flashTimer = 0.1f;
			if (--m_flashCount < 1) {
				m_flash = -1;
				m_flashTimer = -1;
			}
		}
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

std::vector<SelectItem> seatSelectItems(const game::GameWorld::SeatLayout& seat, const world::ObjectTemplate* seatTemplate,
	const std::function<const world::ObjectTemplate*(const std::string&)>& find,
	const std::vector<std::pair<int, std::string>>& carried)
{
	size_t fire = 0, alt = 0;
	for (const auto& w : seat.weapons) {
		fire += w.group == 0 ? 1 : 0;
		alt += w.group == 1 ? 1 : 0;
	}
	std::vector<SelectItem> items;
	if (fire == 0) {
		for (const auto& [slot, name] : carried) {
			items.push_back({slot, hudSetting(find(name), "weaponHud.selectIcon"), true});
		}
		return items;
	}
	const int group = flag(seatTemplate, "vehicleHud.useSelectionIcons", false) && alt > fire ? 1 : 0;
	for (const auto& w : seat.weapons) {
		if (w.group == group) {
			items.push_back({w.itemIndex, hudSetting(find(w.weapon), "weaponHud.selectIcon"), w.selectable});
		}
	}
	return items;
}

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

float mechFootValue(float length, float time)
{
	if (length < 0.3f) {
		return 0;
	}
	return std::sin(std::min(time / length, 1.0f) * 6.2831855f) + 1;
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
	// 0x007a2e60: the torque, eased a hundredth of the way each frame:
	// Torque (+0x210) toward max(0, torque), TorqueString ceil(x 100) "%d",
	// TorqueAngle (+0x218) 2 pi x Torque; SecondTorque (+0x21c) toward
	// clamp(torque + 1, 0, 1), SecondTorqueAngle (+0x224) 2 pi x that (the
	// gunships' and transports' arrows, the mechs' step control).
	m_torque += (std::max(in.torque, 0.0f) - m_torque) * 0.01f;
	m_secondTorque += (std::clamp(in.torque + 1.0f, 0.0f, 1.0f) - m_secondTorque) * 0.01f;
	hud.set("Torque", m_torque);
	hud.setString("TorqueString", std::to_string(static_cast<int>(std::ceil(m_torque * 100.0f))));
	hud.set("TorqueAngle", m_torque * 6.2831855f);
	hud.set("SecondTorque", m_secondTorque);
	hud.set("SecondTorqueAngle", m_secondTorque * 6.2831855f);
	// The vehicle's sprint (0x007a2e60's end, the controlled object not the
	// soldier): VehicleStamina (+0x1d4) the control object's slot 0x108,
	// VehicleStaminaShow (+0x2ab) while sprinting or below 1,
	// VehicleStaminaTextStringShow (+0x2b4) for 3 s after it last fell
	// (+0x23c, run by 0x007a05c0 with the frame's time).
	if (m_staminaTimer >= 0) {
		m_staminaTimer += in.seconds;
		if (m_staminaTimer >= 3) {
			m_staminaTimer = -1;
		}
	}
	if (in.stamina < m_staminaWas) {
		m_staminaTimer = 0;
	}
	m_staminaWas = in.stamina;
	hud.set("VehicleStamina", in.stamina);
	hud.set("VehicleStaminaShow", in.sprinting || in.stamina < 1 ? 1 : 0);
	hud.set("VehicleStaminaTextStringShow", m_staminaTimer >= 0 ? 1 : 0);
	hud.set("VehicleStaminaBlink", m_staminaBlink.tick(in.stamina, in.seconds) ? 1 : 0);
	// 0x007a18a0: the armour's share, its text (per cent) and colour.
	const float armor = std::clamp(in.armor, 0.0f, 1.0f);
	hud.set("VehicleArmor", armor);
	hud.set("VehicleArmorBlink", m_armorBlink.tick(armor, in.seconds) ? 1 : 0);
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
	// The other seats' turrets (0x007a2b50 → 0x007a1e00 / 0x007a07c0).
	const bool one = in.seat && flag(in.seat, "vehicleHud.showOneSecondaryGunnerAngle", false);
	for (const OtherSeat& other : in.others) {
		if (other.hasBundle) {
			gunnerBundle(other);
		}
		if (one) {
			break;
		}
	}
	// The repair cells (0x007a2e60 -> 0x007a1e50).
	for (size_t n = 0; n < in.repairCells.size() && n < 9; ++n) {
		hud.set("RepairCell" + std::to_string(n + 1) + "Active", in.repairCells[n] ? 1 : 0);
	}
	hud.set("GunnerAngle", m_gunnerAngle);
	hud.set("SecondaryGunnerAngle", m_secondaryAngle);
	// The walkers' step (0x007a0940).
	if (in.walking) {
		m_footValue = mechFootValue(in.walkLength, in.walkTime);
	}
	hud.set("MechFootAnimationValue", m_footValue);
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

void VehicleHud::gunnerBundle(const OtherSeat& seat)
{
	// 0x007a07c0: the first bundle kept (+0xa8), a second different one
	// (+0xac); the angles clamped to [-pi, pi] (0x00402dd0), here degrees.
	if (m_firstBundle >= 0 && m_firstBundle != seat.bundle) {
		m_secondBundle = seat.bundle;
	} else {
		m_firstBundle = seat.bundle;
	}
	if (m_secondBundle < 0) {
		m_gunnerAngle = std::clamp(-seat.rotation, -180.0f, 180.0f);
	} else if (m_firstBundle >= 0 && m_firstBundle != seat.bundle) {
		m_secondaryAngle = std::clamp(-(seat.rotation + 1.5707964f), -180.0f, 180.0f);
	} else {
		m_gunnerAngle = std::clamp(seat.rotation - 1.5707964f, -180.0f, 180.0f);
	}
}

bool HudBlink::tick(float value, float seconds)
{
	// 0x00778080 / 0x00778330 (the watched value present).
	const float period = m_scaled ? (m_slow - m_fast) / m_threshold * value + m_fast : m_slow;
	m_timer += seconds;
	const bool past = m_watch == Watch::below ? value < m_threshold : value > m_threshold;
	if (past && m_threshold != 0 && !(value < 0)) {
		if (m_timer > period) {
			// Held off at or below the floor (below), at or above it (above).
			const bool held = m_watch == Watch::below ? value <= m_floor : m_floor <= value;
			m_lit = held ? false : !m_lit;
			m_timer = 0;
		}
	} else if (m_lit && m_timer > period) {
		m_timer = 0;
		m_lit = false;
	}
	return m_lit;
}

bool displaySelectionOnEnter(const world::ObjectTemplate* controlled, const world::ObjectTemplate* lastOccupied)
{
	// 0x007a28e0: +0xb4 = a non-pod Parachute; 0x007a3920 opens the list
	// only while it is 0.
	const bool held = lastOccupied && lastOccupied->typeName() == "Parachute" && hudNumber(lastOccupied, "isPod", 0) == 0;
	return !held && hudNumber(controlled, "vehicleHud.displaySelectionOnEnter", 0) != 0;
}

bool HudBlink::tickFree(float seconds)
{
	// 0x00778080's branch without a watched value (+4 null): toggle every
	// `slow` (+0x14) seconds.
	m_timer += seconds;
	if (m_slow < m_timer) {
		m_lit = !m_lit;
		m_timer = 0;
	}
	return m_lit;
}

void VehicleHud::clear(hud::HudManager& hud)
{
	// 0x007a1ee0 as the controlled object becomes the soldier again.
	m_firstBundle = m_secondBundle = -1;
	m_staminaBlink.reset();
	m_armorBlink.reset();
	hud.set("VehicleStaminaBlink", 0);
	hud.set("VehicleArmorBlink", 0);
	for (int i = 1; i <= 10; ++i) {
		hud.set("OccupyingPlayerName" + std::to_string(i) + "Show", 0);
	}
	m_climbAngle = m_climbPrevious = 0;
	m_staminaWas = 1;
	m_staminaTimer = -1;
	hud.set("VehicleStaminaShow", 0);
	hud.set("VehicleStaminaTextStringShow", 0);
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

const MapIcons& mapIcons(const world::ObjectTemplate& t)
{
	struct Entry {
		std::string name;
		size_t settings = 0; // a template given more settings is read again
		MapIcons icons;
	};
	static std::unordered_map<const world::ObjectTemplate*, Entry> cache;
	const auto known = cache.find(&t);
	if (known != cache.end() && known->second.settings == t.settings().size() && known->second.name == t.name()) {
		return known->second.icons;
	}
	MapIcons m;
	m.icon = slashes(hudSetting(&t, "vehicleHud.miniMapIcon"));
	m.iconInside = slashes(hudSetting(&t, "vehicleHud.miniMapIconInside"));
	m.reload = slashes(hudSetting(&t, "vehicleHud.miniMapReloadIcon"));
	m.background = slashes(hudSetting(&t, "vehicleHud.miniMapBackgroundIcon"));
	m.spotted = slashes(hudSetting(&t, "vehicleHud.spottedIcon"));
	m.size = pair(&t, "vehicleHud.miniMapIconSize", 16);
	m.sizeInside = pair(&t, "vehicleHud.miniMapIconSizeInside", 16);
	m.spottedSize = hudNumber(&t, "vehicleHud.spottedIconSize", 16);
	m.offset = pair(&t, "vehicleHud.miniMapIconOffset", 0);
	m.offsetInside = pair(&t, "vehicleHud.miniMapIconOffsetInside", 0);
	m.zoomOnMinimap = flag(&t, "vehicleHud.zoomOnMinimap", false);
	std::string info = hudSetting(&t, "vehicleHud.miniMapInfo", "Icon");
	std::transform(info.begin(), info.end(), info.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	m.info = info == "reload" ? 1 : info == "all" ? 2 : 0; // anything else keeps "icon"'s 0
	m.vehicleType = static_cast<int>(hudNumber(&t, "vehicleHud.vehicleType", -1));
	m.showOnEnemy = flag(&t, "vehicleHud.showOnEnemyMinimap", false);
	m.always = flag(&t, "vehicleHud.alwaysShowOnMinimap", false);
	m.usePlayerIcon = flag(&t, "vehicleHud.usePlayerIcon", false);
	Entry& slot = cache[&t];
	slot = {t.name(), t.settings().size(), std::move(m)};
	return slot.icons;
}

bool hiddenOnZoomedMap(const MapIcons& icons, int zoomIndex, bool overheadView)
{
	return icons.zoomOnMinimap && zoomIndex == 2 && overheadView;
}

bool standardHelpEnabled(const world::ObjectTemplate* controlled)
{
	return flag(controlled, "vehicleHud.standardHelpEnabled", true);
}

bool usesVehicleCommRose(const world::ObjectTemplate* controlled)
{
	return flag(controlled, "vehicleHud.useVehicleCommRose", true);
}

const world::ObjectTemplate* childArmorTemplate(const world::ObjectTemplate* controlled,
	const world::ObjectTemplateManager& templates)
{
	if (!controlled || !flag(controlled, "vehicleHud.useChildArmor", false)) {
		return nullptr;
	}
	// An Armor component (createComponent Armor; the settings' armor.* for
	// one that only sets values).
	const auto armoured = [](const world::ObjectTemplate& t) {
		for (const std::string& c : t.components()) {
			std::string lower = c;
			for (char& ch : lower) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			if (lower == "armor") {
				return true;
			}
		}
		return t.settings().count("armor.maxHitPoints") != 0;
	};
	// 0x005c6460: the object itself, then each child's subtree in order.
	const std::function<const world::ObjectTemplate*(const world::ObjectTemplate&, int)> walk =
		[&](const world::ObjectTemplate& t, int depth) -> const world::ObjectTemplate* {
		if (armoured(t)) {
			return &t;
		}
		if (depth > 8) {
			return nullptr;
		}
		for (const world::ChildTemplate& child : t.children()) {
			if (const world::ObjectTemplate* c = templates.find(child.name)) {
				if (const world::ObjectTemplate* found = walk(*c, depth + 1)) {
					return found;
				}
			}
		}
		return nullptr;
	};
	return walk(*controlled, 0);
}

bool seatPostProcessing(const world::ObjectTemplate* controlled, const world::ObjectTemplate* weapon, bool zoomed)
{
	if (controlled && hudNumber(controlled, "vehicleHud.enablePostProcessingOnGuiIndex", 0) != 0) {
		return true;
	}
	if (!weapon) {
		return false;
	}
	const auto& settings = weapon->settings();
	const auto on = settings.find("weaponHud.enablePostProcessingOnGuiIndex");
	return on != settings.end() && std::atoi(on->second.c_str()) == weaponGuiIndex(weapon, zoomed);
}

bool soldierWeaponFlag(bool jumpHold, const world::ObjectTemplate* weapon)
{
	if (!jumpHold) {
		return true;
	}
	if (!weapon) {
		return false; // no weapon in hand (slot 0x78(0) none)
	}
	// The fire component's interfaces 0x94de / 0x94df, the ammo
	// component's 0x94f1.
	const world::CaseInsensitiveLess less;
	for (const std::string& component : weapon->components()) {
		for (const std::string kind : {"ThrownFireComp", "ExplosivesFireComp", "CamouflageAmmoComp"}) {
			if (!less(component, kind) && !less(kind, component)) {
				return true;
			}
		}
	}
	return false;
}

int weaponGuiIndex(const world::ObjectTemplate* weapon, bool zoomed, bool sprintTest)
{
	if (!weapon) {
		return 0;
	}
	const auto& settings = weapon->settings();
	// 0x00657a70's first test: the sprint test and disableOnSprint (slot 0x13c).
	if (sprintTest) {
		const auto off = settings.find("weaponHud.disableOnSprint");
		if (off != settings.end() && std::atoi(off->second.c_str()) != 0) {
			return 0;
		}
	}
	const auto alt = settings.find("weaponHud.altGuiIndex");
	if (zoomed && alt != settings.end() && std::atoi(alt->second.c_str()) != -1) {
		return std::atoi(alt->second.c_str());
	}
	const auto gui = settings.find("weaponHud.guiIndex");
	return gui != settings.end() ? std::atoi(gui->second.c_str()) : 0;
}

std::vector<int> showOnCamModes(const world::ObjectTemplate* t, const char* component)
{
	// WorldConsole keeps the list as "<component>.showOnCamMode.list".
	const std::string text = hudSetting(t, (std::string(component) + ".showOnCamMode.list").c_str(), "1");
	std::vector<int> modes;
	const char* p = text.c_str();
	char* end = nullptr;
	for (long v = std::strtol(p, &end, 10); end != p; v = std::strtol(p, &end, 10)) {
		modes.push_back(static_cast<int>(v));
		p = end;
	}
	return modes;
}

namespace {

bool hasVehicleHud(const world::ObjectTemplate* t)
{
	if (!t) {
		return false;
	}
	for (const std::string& c : t->components()) {
		if (c.size() == 10 && std::equal(c.begin(), c.end(), "VehicleHud",
				[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) {
			return true;
		}
	}
	// A template that only sets vehicleHud.* values (the settings map is
	// ordered case-insensitively, so they sit together).
	const auto it = t->settings().lower_bound("vehicleHud.");
	return it != t->settings().end() && it->first.size() > 11
		&& std::equal(it->first.begin(), it->first.begin() + 11, "vehicleHud.",
			[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

bool lists(const std::vector<int>& modes, int mode)
{
	return std::find(modes.begin(), modes.end(), mode) != modes.end();
}

} // namespace

int vehicleGuiIndex(const VehicleGuiInput& in)
{
	// 0x007a01e0: no VehicleHud, the first weapon's HUD as it is.
	if (!hasVehicleHud(in.controlled)) {
		return weaponGuiIndex(in.weapon, in.zoomed, in.sprintTest);
	}
	// 0x00657660's first test: the sprint test and the vehicle HUD's
	// disableOnSprint (slot 0x78, +0x107).
	if (in.sprintTest && flag(in.controlled, "vehicleHud.disableOnSprint", false)) {
		return 0;
	}
	// The runtime WeaponHud's slot 0x48 (0x00657c50).
	const auto shows = [&in](const world::ObjectTemplate* weapon, int mode) {
		return lists(showOnCamModes(weapon, "weaponHud"), mode) || in.projectileCamera;
	};
	if (!flag(in.controlled, "vehicleHud.useChildHud", false)) {
		if (in.weapon) {
			const int gui = weaponGuiIndex(in.weapon, in.zoomed, in.sprintTest);
			if (gui != -1) {
				if (!shows(in.weapon, in.cameraMode)) {
					const std::vector<int> modes = showOnCamModes(in.weapon, "weaponHud");
					if (modes.size() > 1 || (modes.size() == 1 && !shows(in.weapon, -1))) {
						return 0;
					}
				}
				return gui;
			}
			if (in.second) {
				return shows(in.second, in.cameraMode) ? weaponGuiIndex(in.second, in.zoomed, in.sprintTest) : 0;
			}
		}
	} else if (in.child) {
		return shows(in.child, in.cameraMode) ? weaponGuiIndex(in.child, in.zoomed, in.sprintTest) : 0;
	}
	// The vehicle HUD's own (template slots 0xd4 and 0x30).
	if (!lists(showOnCamModes(in.controlled, "vehicleHud"), in.cameraMode)) {
		return 0;
	}
	return static_cast<int>(hudNumber(in.controlled, "vehicleHud.guiIndex", 0));
}

std::string cameraRotationBundle(const world::ObjectTemplate& t, const world::ObjectTemplateManager& templates)
{
	if (!flag(&t, "vehicleHud.useVehicleCameraRotation", false)) {
		return {};
	}
	// 0x005c63b0: depth first, each child before its siblings; the nearest
	// RotationalBundle above the Camera is what turns it.
	const std::function<bool(const world::ObjectTemplate&, const std::string&, int, std::string&)> walk =
		[&](const world::ObjectTemplate& node, const std::string& bundle, int depth, std::string& out) {
			for (const world::ChildTemplate& child : node.children()) {
				const world::ObjectTemplate* c = templates.find(child.name);
				if (!c) {
					continue;
				}
				if (c->classId() == 0x9472) { // Camera
					out = bundle;
					return true;
				}
				const bool turns = c->classId() == 0x947d; // RotationalBundle
				if (depth < 12 && walk(*c, turns ? c->name() : bundle, depth + 1, out)) {
					return true;
				}
			}
			return false;
		};
	std::string bundle;
	walk(t, std::string(), 0, bundle);
	return bundle;
}

std::string iconFrame(const std::string& icon, int frame)
{
	const size_t slash = icon.find_last_of('/');
	if (icon.find('.', slash == std::string::npos ? 0 : slash) != std::string::npos) {
		return icon;
	}
	char suffix[8];
	std::snprintf(suffix, sizeof(suffix), "_%02d", std::clamp(frame, 1, 99));
	return icon + suffix + ".dds";
}

float reloadSweep(float reloadLeft, float reloadTime, MapRelation relation)
{
	if (relation == MapRelation::Neutral || reloadTime <= 0 || reloadLeft <= 0) {
		return 0;
	}
	return 6.2831855f * std::min(reloadLeft / reloadTime, 1.0f);
}

bool vehicleMarker(const world::ObjectTemplate& vehicle, MapRelation relation, bool inside, bool spotted, VehicleMarker& out)
{
	const MapIcons& m = mapIcons(vehicle);
	const bool useInside = inside && !m.iconInside.empty();
	const std::string& texture = useInside ? m.iconInside : m.icon;
	if (texture.empty()) {
		return false;
	}
	// 0x00793b60: friends', neutral and his own always; the enemy's while
	// spotted or with showOnEnemyMinimap; any with alwaysShowOnMinimap.
	const bool shown = relation != MapRelation::Enemy || spotted || m.showOnEnemy || m.always;
	if (!shown) {
		return false;
	}
	out.texture = texture;
	const auto& size = useInside ? m.sizeInside : m.size;
	out.width = size[0];
	out.height = size[1];
	const auto& offset = useInside ? m.offsetInside : m.offset;
	out.offsetX = offset[0];
	out.offsetY = offset[1];
	out.asPlayer = m.usePlayerIcon && relation != MapRelation::Neutral;
	return true;
}

std::vector<RadarMarker> radarMarkers(const host::HudWorld& world, game::GameWorld& round, const std::vector<int>& revealed)
{
	std::vector<RadarMarker> out;
	std::vector<game::GameWorld::PlayerInfo> enemies;
	bool onFoot = false;
	for (const int id : revealed) {
		game::GameWorld::PlayerInfo enemy;
		if (world.playerInfo(id, enemy) && enemy.alive) {
			onFoot = onFoot || !enemy.vehicle;
			enemies.push_back(enemy);
		}
	}
	// The decoys, looked for only with an enemy on foot to stand in for.
	std::vector<const world::Object*> decoys;
	if (onFoot) {
		for (const auto& object : round.objects().objects()) {
			const world::ObjectTemplate* t = object->objectTemplate();
			if (!t || t->typeName() != "TargetObject") {
				continue;
			}
			const auto type = t->settings().find("targetType");
			if (type != t->settings().end() && targetTypeIndex(type->second) == 6) {
				decoys.push_back(object.get());
			}
		}
	}
	for (game::GameWorld::PlayerInfo& enemy : enemies) {
		if (!enemy.vehicle) {
			for (const world::Object* decoy : decoys) {
				const float dx = decoy->position().x - enemy.position.x, dz = decoy->position().z - enemy.position.z;
				if (dx * dx + dz * dz < 60.0f * 60.0f) {
					enemy.position = {decoy->position().x, decoy->position().y, decoy->position().z};
					break;
				}
			}
		}
		RadarMarker m {"Ingame/Player/Icons/Minimap/mini_Soldier.dds", enemy.position.x, enemy.position.z, 16};
		const world::Object* kit = enemy.kit ? round.objects().find(enemy.kit) : nullptr;
		if (kit && kit->objectTemplate()) {
			const MapIcons& icons = mapIcons(*kit->objectTemplate());
			if (!icons.spotted.empty()) {
				m.texture = icons.spotted;
				m.size = icons.spottedSize;
			} else if (!icons.icon.empty()) {
				m.texture = icons.icon;
				m.size = icons.size[0];
			}
		}
		out.push_back(std::move(m));
	}
	for (const uint32_t id : world.spottedVehicles()) {
		if (const world::Object* vehicle = round.objects().find(id)) {
			RadarMarker m {"Ingame/Player/Icons/Minimap/mini_Soldier.dds", vehicle->position().x, vehicle->position().z, 16};
			if (vehicle->objectTemplate()) {
				const MapIcons& icons = mapIcons(*vehicle->objectTemplate());
				if (!icons.icon.empty()) {
					m.texture = icons.icon;
				}
			}
			out.push_back(std::move(m));
		}
	}
	return out;
}

} // namespace dice::hfe::menu
