#include "HudMenusKits.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Game/KitUnlocks.h"
#include "Hud/Hud.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dice::hfe::menu {

namespace {

constexpr const char* kEmpty = "Ingame/GeneralIcons/empty.tga";

int fieldBranch(const std::string& value)
{
	if (value == "UB_First" || value == "0") return 0;
	const int branch = game::unlockBranch(value);
	return branch == 0 ? -1 : branch;
}

std::string orEmpty(const std::string& path) { return path.empty() ? std::string(kEmpty) : path; }

// The kit template's main-weapon itemIndex (+0x2c8, 3 from its constructor
// 0x005700e9): 0x00798dd0 puts the items with it in the main weapon area.
// shortcut: its +0x2cc (4) case, gated by 0x0056f410, isn't ported; no shipped unlock has itemIndex 4.
constexpr int kMainWeaponIndex = 3;

} // namespace

KitMenu::KitMenu(game::GameWorld& round, game::GameLogic& logic, world::ObjectTemplateManager& templates, hud::HudManager& hud)
	: m_round(round), m_logic(logic), m_templates(templates), m_hud(hud)
{
	for (auto& kit : m_chosen) {
		kit.fill(-1);
	}
}

bool KitMenu::mainWeapon(const std::string& itemTemplate) const
{
	return std::atoi(setting(itemTemplate, "itemIndex").c_str()) == kMainWeaponIndex;
}

std::string KitMenu::setting(const std::string& templateName, const std::string& key) const
{
	const world::ObjectTemplate* t = m_templates.find(templateName);
	if (!t) {
		return {};
	}
	const auto it = t->settings().find(key);
	if (it == t->settings().end()) {
		return {};
	}
	std::string value = it->second;
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
		value = value.substr(1, value.size() - 2);
	}
	return value;
}

void KitMenu::bind(Console& console)
{
	const auto any = ConsoleAccess::Anyone;
	// KitInterface (0x00776db4-0x00777539 → 0x00798070, 0x00799c70, ...).
	bindMethod(console, "KitInterface.closeCustomInterface", any, [this](int) { closeCustomInterface(); });
	bindMethod(console, "KitInterface.chooseKitSelection", any, [this](int a, int b) { chooseKitSelection(a, b); });
	bindMethod(console, "KitInterface.chooseKitCustom", any, [this](int row, int item) { chooseKitCustom(row, item); });
	bindMethod(console, "KitInterface.selectDroppedItem", any, [](int) {}); // no dragging: chooseKitCustom equips
	bindMethod(console, "KitInterface.resetMousePic", any, [](int) {});
	bindMethod(console, "KitInterface.setInformationString", any, [this](int row, int item) { setInformationString(row, item); });
	bindMethod(console, "KitInterface.resetInformationString", any, [this](int) { resetInformationString(); });
	bindMethod(console, "KitInterface.setHeavyArmor", any, [this](int heavy) { setHeavyArmor(heavy != 0); });
	// 0x007b95a0 opens the field-upgrade tab only with a credit.
	bindMethod(console, "UnlockInterface.showUnlockInterface", any, [this](int show) {
		if (show != 1) {
			closeFieldUpgrades();
			return;
		}
		if (m_fieldCredits <= 0) return;
		closeCustomInterface();
		m_hud.set("UnlocksShow", 1);
		m_hud.set("UnlockInfoShow", 1);
		resetFieldInformation();
		selectFieldUpgrade(-1, -1);
	});
	bindMethod(console, "UnlockInterface.setInformationString", any, [this](int kit, const std::string& value) {
		const auto* item = nextFieldItem(kit, fieldBranch(value));
		m_hud.setString("UnlockInfoString", item ? text(setting(item->name(), "weaponHud.descrString")) : std::string());
		m_hud.setString("UnlockInfoPic", item ? orEmpty(setting(item->name(), "weaponHud.renderedPicture")) : kEmpty);
	});
	bindMethod(console, "UnlockInterface.resetInformationString", any, [this](int) { resetFieldInformation(); });
	bindMethod(console, "UnlockInterface.setFieldUnlock", any, [this](int kit, const std::string& branch) {
		selectFieldUpgrade(kit, fieldBranch(branch));
	});
	bindMethod(console, "UnlockInterface.selectUnlock", any, [this](int select) {
		if (select == 1) spendFieldUpgrade(m_fieldKit, m_fieldBranch);
	});
	bindMethod(console, "UnlockInterface.spendFieldUnlock", any, [this](int kit, const std::string& branch) {
		spendFieldUpgrade(kit, fieldBranch(branch));
	});
	bindMethod(console, "UnlockInterface.changeToKit", any, [this](int kit) { setKit(kit); });
	const auto selectUnlock = [this](int kit, int branch, int level, int on) {
		// 0x007b8f30: an unlock the soldier has becomes the branch's choice.
		if (kit < 0 || kit >= kKits || branch < 0 || branch > 3 || level < 0 || level >= kItems) {
			return false;
		}
		setKit(kit);
		build(m_team);
		const Item& it = m_grid[static_cast<size_t>(branch + 1)][static_cast<size_t>(level)];
		if (!it.present || !it.available) {
			return false;
		}
		m_chosen[static_cast<size_t>(kit)][static_cast<size_t>(branch + 1)] = on ? level : -1;
		m_changed = true;
		m_built = false;
		return true;
	};
	bindMethod(console, "UnlockInterface.selectNextUnlock", any,
		[selectUnlock](int kit, int branch, int level, int on) { return selectUnlock(kit, branch, level, on); });
	bindMethod(console, "UnlockInterface.selectUnlockItem", any,
		[selectUnlock](int kit, int branch, int level, int on) { selectUnlock(kit, branch, level, on); });
}

bool KitMenu::owns(const std::string& container) const
{
	return m_allUnlocked || std::find(m_unlocked.begin(), m_unlocked.end(), container) != m_unlocked.end()
		|| std::find(m_fieldItems.begin(), m_fieldItems.end(), container) != m_fieldItems.end();
}

const world::ObjectTemplate* KitMenu::nextFieldItem(int kit, int branch) const
{
	if (kit < 0 || kit >= kKits || branch < 0 || branch >= 4) return nullptr;
	const world::ObjectTemplate* item = nullptr;
	int next = kItems;
	game::visitKitUnlocks(m_templates, m_logic.team(m_team == 1 ? 1 : 2), kit, [&](const std::string& name, int b, int level) {
		if (b != branch || level >= next || owns(name)) return;
		const auto* container = m_templates.find(name);
		if (const auto* child = m_templates.find(container->children().front().name)) {
			next = level;
			item = child;
		}
	});
	return item;
}

void KitMenu::selectFieldUpgrade(int kit, int branch)
{
	const bool selectable = m_fieldCredits > 0 && nextFieldItem(kit, branch);
	m_fieldKit = selectable ? kit : -1;
	m_fieldBranch = selectable ? branch : -1;
	for (int k = 0; k < kKits; ++k) {
		for (int b = 0; b < 4; ++b) {
			m_hud.set("Kit" + std::to_string(k) + "Branch" + std::to_string(b) + "Selected",
				k == m_fieldKit && b == m_fieldBranch ? 1 : 0);
		}
	}
}

bool KitMenu::spendFieldUpgrade(int kit, int branch)
{
	const auto* next = nextFieldItem(kit, branch);
	if (m_fieldCredits <= 0 || !next || !onSpendFieldUpgrade || !onSpendFieldUpgrade(kit, branch)) return false;
	m_hud.setString("PickedUnlockIconPathString", orEmpty(setting(next->name(), "weaponHud.unlockIcon")));
	m_hud.setString("PickedUnlockNameString", text(setting(next->name(), "weaponHud.hudName")));
	m_hud.set("UnlockInfoShow", 0);
	selectFieldUpgrade(-1, -1);
	return true;
}

void KitMenu::resetFieldInformation()
{
	m_hud.setString("UnlockInfoString", text("HUD_TEXT_MENU_FU_DEFAULT_TEXT"));
	m_hud.setString("UnlockInfoPic", kEmpty);
}

void KitMenu::closeFieldUpgrades()
{
	m_hud.set("UnlocksShow", 0);
	m_hud.set("UnlockInfoShow", 0);
	selectFieldUpgrade(-1, -1);
}

void KitMenu::setFieldUpgrades(int credits, const std::vector<std::string>& containers)
{
	if (credits == m_fieldCredits && containers == m_fieldItems) return;
	const bool ownershipChanged = containers != m_fieldItems;
	m_fieldCredits = credits;
	m_fieldItems = containers;
	m_built = false;
	if (credits == 0 && containers.empty()) closeFieldUpgrades();
	if (ownershipChanged) {
		// Rebuild ability icons and forget equipment choices that expired.
		resetAbilityIcons();
		const int saved = m_kit;
		for (int kit = 0; kit < kKits; ++kit) {
			m_kit = kit;
			build(m_team);
			for (int row = 0; row < kRows; ++row) {
				int& chosen = m_chosen[kit][row];
				if (chosen >= 0 && !m_grid[row][chosen].available) chosen = -1;
			}
		}
		m_kit = saved;
		m_built = false;
	}
}

void KitMenu::setKit(int kit)
{
	if (kit != m_kit) {
		m_kit = std::clamp(kit, 0, kKits - 1);
		m_built = false;
	}
}

void KitMenu::build(int team)
{
	// 0x00798dd0: the open kit's grid.
	m_team = team;
	m_built = true;
	++m_builds;
	for (auto& row : m_grid) {
		row.fill({});
	}
	const auto& kits = m_logic.team(team == 1 ? 1 : 2).kits;
	if (m_kit < 0 || static_cast<size_t>(m_kit) >= kits.size()) {
		return;
	}
	const world::ObjectTemplate* kit = m_templates.find(kits[static_cast<size_t>(m_kit)].name);
	if (!kit) {
		return;
	}
	const auto fill = [this](Item& item, const std::string& itemTemplate) {
		item.present = true;
		item.icon = setting(itemTemplate, "weaponHud.unlockIcon");
		item.abilityIcon = setting(itemTemplate, "weaponHud.specialAbilityIcon");
		item.picture = setting(itemTemplate, "weaponHud.renderedPicture");
		item.description = setting(itemTemplate, "weaponHud.descrString");
		if (item.description.empty()) {
			item.description = setting(itemTemplate, "weaponHud.hudName");
		}
	};
	for (const world::ChildTemplate& child : kit->children()) {
		const world::ObjectTemplate* t = m_templates.find(child.name);
		if (!t) {
			continue;
		}
		if (t->typeName() != "ItemContainer") {
			// Row 0: the kit's own main weapon, by its itemIndex.
			const auto free = std::find_if(m_grid[0].begin(), m_grid[0].end(), [](const Item& i) { return !i.present; });
			if (free != m_grid[0].end() && mainWeapon(child.name)) {
				fill(*free, child.name);
				free->available = true;
			}
			continue;
		}
		const int level = std::atoi(setting(child.name, "unlockLevel").c_str());
		if (level < 0 || level >= kItems || t->children().empty()) {
			continue; // the level-15 replacements aren't on the grid
		}
		const std::string& unlockItem = t->children().front().name;
		const int branch = game::unlockBranch(setting(child.name, "unlockBranch"));
		Item* slot = nullptr;
		if (mainWeapon(unlockItem)) {
			// A primary weapon unlock (the HAR/LAR rifle...): the main weapon
			// area's next slot, which the HUD lays out after the kit's own
			// weapon (Kit0Item1...), not a place in its branch.
			const auto free = std::find_if(m_grid[0].begin() + 1, m_grid[0].end(), [](const Item& i) { return !i.present; });
			if (free == m_grid[0].end()) {
				continue;
			}
			slot = &*free;
		} else {
			// The branch's other unlocks close up over those weapons.
			int index = level;
			for (const world::ChildTemplate& other : kit->children()) {
				const world::ObjectTemplate* o = m_templates.find(other.name);
				const int otherLevel = std::atoi(setting(other.name, "unlockLevel").c_str());
				if (o && o->typeName() == "ItemContainer" && !o->children().empty() && otherLevel < level
					&& game::unlockBranch(setting(other.name, "unlockBranch")) == branch
					&& mainWeapon(o->children().front().name)) {
					--index;
				}
			}
			if (index < 0 || index >= kItems) continue;
			slot = &m_grid[static_cast<size_t>(branch + 1)][static_cast<size_t>(index)];
		}
		Item& item = *slot;
		fill(item, unlockItem);
		item.container = child.name;
		item.available = owns(child.name);
		item.fieldUnlocked = std::find(m_fieldItems.begin(), m_fieldItems.end(), child.name) != m_fieldItems.end();
	}
	for (int row = 0; row < kRows; ++row) {
		const int chosen = m_chosen[static_cast<size_t>(m_kit)][static_cast<size_t>(row)];
		if (chosen >= 0 && m_grid[static_cast<size_t>(row)][static_cast<size_t>(chosen)].available) {
			m_grid[static_cast<size_t>(row)][static_cast<size_t>(chosen)].active = true;
		}
	}
	// The kit's own weapon stays the primary unless another was chosen.
	const auto& primary = m_grid[0];
	m_grid[0][0].active = std::none_of(primary.begin() + 1, primary.end(), [](const Item& i) { return i.active; });
}

// 0x00798b20, from each rebuild of the open kit (0x00798dd0) while the
// local player's soldier is dead: the kit is built for the player
// (0x00570ab0: its own items' and the applying unlocks' Ability
// components, 0x005706b0, into the player's ability block, slot 0x2a0),
// then the block lights the icons (HudElementsKitsMenu.con's PlayerEquip
// row):
//   - per addExtraAmmoItem entry (the list +0x18, 0x28 bytes each), only
//     ever set: the flag (+1) Ability0Active (grenades.tga), rounds a
//     magazine (+8) Ability1Active (extra_grenade.tga), magazines (+4)
//     Ability2Active (larger_pistol_clip.tga), as the script pairs them;
//   - sprintUseModifier (+0x50) not 1 Ability3Active (sprint_amount),
//     sprintRechargeModifier (+0x4c) not 1 Ability4Active
//     (sprint_recharge), setOverheatMod's pairs (+0x38) Ability5Active
//     (gun_upgrade), beaconSpawnTimeMod (+0x54) not 1 Ability6Active
//     (beacon_spawnTime).
// An unlock's Ability applies when the player owns the container (the
// build's last pass: player slot 0x2a8, 0x00597760), chosen on the grid
// or not (GameWorld::fillKit); every one with "all unlocked".
void KitMenu::updateAbilityIcons()
{
	const auto& kits = m_logic.team(m_team == 1 ? 1 : 2).kits;
	if (m_alive || m_kit < 0 || static_cast<size_t>(m_kit) >= kits.size()) {
		return;
	}
	const world::ObjectTemplate* kit = m_templates.find(kits[static_cast<size_t>(m_kit)].name);
	if (!kit) {
		return;
	}
	float sprintUse = 1, sprintRecharge = 1, beacon = 1;
	bool overheat = false;
	const auto apply = [&](const world::ObjectTemplate& t) {
		const auto& settings = t.settings();
		if (const auto extra = settings.find("ability.addExtraAmmoItem.list"); extra != settings.end()) {
			std::string list = extra->second;
			for (size_t at = 0; at < list.size();) {
				size_t end = list.find(';', at);
				end = end == std::string::npos ? list.size() : end;
				char item[128] = {};
				int rounds = 0, mags = 0, flag = 0;
				if (std::sscanf(list.substr(at, end - at).c_str(), "%127s %d %d %d", item, &rounds, &mags, &flag) >= 3) {
					m_abilityIcons[0] = m_abilityIcons[0] || flag != 0;
					m_abilityIcons[1] = m_abilityIcons[1] || rounds > 0;
					m_abilityIcons[2] = m_abilityIcons[2] || mags > 0;
				}
				at = end + 1;
			}
		}
		// 0x005706b0 copies each modifier that isn't 1 and appends the pairs.
		const auto modifier = [&settings](const char* key, float& kept) {
			const auto it = settings.find(key);
			if (it != settings.end() && std::atof(it->second.c_str()) != 1.0) {
				kept = static_cast<float>(std::atof(it->second.c_str()));
			}
		};
		modifier("ability.sprintUseModifier", sprintUse);
		modifier("ability.sprintRechargeModifier", sprintRecharge);
		modifier("ability.beaconSpawnTimeMod", beacon);
		if (const auto mods = settings.find("ability.setOverheatMod.list"); mods != settings.end() && !mods->second.empty()) {
			overheat = true;
		}
	};
	for (const world::ChildTemplate& child : kit->children()) {
		const world::ObjectTemplate* t = m_templates.find(child.name);
		if (!t) {
			continue;
		}
		if (t->typeName() != "ItemContainer") {
			apply(*t); // the kit's own items (their Ability, +0x74)
			continue;
		}
		if (owns(child.name)) {
			apply(*t);
		}
	}
	m_abilityIcons[3] = sprintUse != 1;
	m_abilityIcons[4] = sprintRecharge != 1;
	m_abilityIcons[5] = overheat;
	m_abilityIcons[6] = beacon != 1;
}

void KitMenu::update(int team)
{
	if (!m_built || team != m_team) {
		build(team);
		updateAbilityIcons();
	}
	const auto& kits = m_logic.team(team == 1 ? 1 : 2).kits;
	if (skipUnchanged) {
		Shown now {m_builds, team, m_kit, m_heavy, m_customOpen, {}};
		for (const auto& kit : kits) {
			now.kits.push_back(kit.name);
		}
		if (m_shownValid && now == m_shown) {
			return;
		}
		m_shown = std::move(now);
		m_shownValid = true;
	} else {
		m_shownValid = false;
	}
	// The kit buttons (0x0044e650).
	for (int i = 0; i < kKits; ++i) {
		const std::string n = std::to_string(i);
		const world::ObjectTemplate* kit = static_cast<size_t>(i) < kits.size() ? m_templates.find(kits[static_cast<size_t>(i)].name) : nullptr;
		m_hud.set("Kit" + n + "Show", kit ? 1 : 0);
		m_hud.set("PlayerKitIcon" + n + "SelectShow", i == m_kit ? 1 : 0);
		if (!kit) {
			continue;
		}
		const std::string& kitName = kit->name();
		const std::string name = setting(kitName, "vehicleHud.hudName");
		m_hud.setString("KitName" + n + "String", name.empty() ? std::string() : text(name));
		m_hud.setString("KitIcon" + n + "Path", orEmpty(setting(kitName, "vehicleHud.vehicleIcon")));
		std::string weaponIcon, altIcon;
		int unlockLevels = 0;
		int primaryIndex = 1;
		std::string chosenWeaponIcon;
		for (const world::ChildTemplate& child : kit->children()) {
			const world::ObjectTemplate* t = m_templates.find(child.name);
			if (!t) {
				continue;
			}
			if (t->typeName() == "ItemContainer") {
				const int level = std::atoi(setting(child.name, "unlockLevel").c_str());
				if (level >= 0 && level < kItems && !t->children().empty() && mainWeapon(t->children().front().name)) {
					if (m_chosen[static_cast<size_t>(i)][0] == primaryIndex
						&& owns(child.name)) {
						chosenWeaponIcon = setting(t->children().front().name, "weaponHud.weaponIcon");
					}
					++primaryIndex;
				}
				if (game::unlockBranch(setting(child.name, "unlockBranch")) == 0 && level < kItems) {
					unlockLevels = std::max(unlockLevels, level);
					// 0x0044a3f0: the first branch's unlock in the primary slot.
					if (altIcon.empty() && !t->children().empty()) {
						altIcon = setting(t->children().front().name, "weaponHud.altWeaponIcon");
					}
				}
			} else if (weaponIcon.empty() && mainWeapon(child.name)) {
				weaponIcon = setting(child.name, "weaponHud.weaponIcon");
			}
		}
		m_hud.setString("KitWeaponIcon" + n + "Path", orEmpty(chosenWeaponIcon.empty() ? weaponIcon : chosenWeaponIcon));
		m_hud.set("KitUnlock" + n + "Show", unlockLevels >= 1 ? 1 : 0);
		m_hud.setString("KitAltWeaponIcon" + n + "Path", orEmpty(altIcon));
		bool canUpgrade = false;
		for (int branch = 0; m_fieldCredits > 0 && branch < 4 && !canUpgrade; ++branch) {
			canUpgrade = nextFieldItem(i, branch) != nullptr;
		}
		m_hud.set("KitUnlockArrow" + n + "Show", canUpgrade ? 1 : 0);
		m_hud.set("Kit" + n + "UnlockBlinkAlpha", canUpgrade ? 1 : 0);
		m_hud.set("Kit" + n + "HeavyArmorShow", m_heavy ? 1 : 0);
		m_hud.set("HeavyArmor" + n + "Active", m_heavy ? 1 : 0);
		// 0x00449600: light armour sprints 0.75, heavy 0.25 (without the sprint unlock).
		m_hud.set("Kit" + n + "SprintAbility", m_heavy ? 0.25f : 0.75f);
	}
	// The open kit's grid (the display copy, 0x007977a0).
	for (int row = 0; row < kRows; ++row) {
		std::string chosenIcon;
		for (int index = 0; index < kItems; ++index) {
			const Item& it = m_grid[static_cast<size_t>(row)][static_cast<size_t>(index)];
			const std::string p = "Kit" + std::to_string(row) + "Item" + std::to_string(index);
			m_hud.set(p + "Active", it.active ? 1 : 0);
			m_hud.set(p + "FieldUnlocked", it.fieldUnlocked ? 1 : 0);
			m_hud.set(p + "Show", it.present ? 1 : 0);
			m_hud.setString(p + "Icon", orEmpty(it.icon));
			m_hud.set(p + "Alpha", it.available ? 1.0f : 0.3f);
			if (it.active) {
				chosenIcon = it.icon;
			}
		}
		m_hud.setString("KitCustom" + std::to_string(row) + "Icon", orEmpty(chosenIcon));
	}
	// The kit screen's variables (registered by 0x00796d80: Kit%iItem%iActive
	// / Alpha / FieldUnlocked, Ability%iActive, HeavyArmor%iActive,
	// HeavyArmorActive, CustomInfoShow, CustomSoldierPicture).
	m_hud.set("CustomInfoShow", m_customOpen ? 1 : 0);
	for (size_t i = 0; i < m_abilityIcons.size(); ++i) {
		m_hud.set("Ability" + std::to_string(i) + "Active", m_abilityIcons[i] ? 1 : 0);
	}
	m_hud.set("HeavyArmorActive", m_heavy ? 1 : 0);
	m_hud.set("NotAntiVechicleKitChoosen", 1);
	m_hud.set("FUBlinkAlpha", m_fieldCredits > 0 ? 1 : 0);
	m_hud.set("UnlockBlinkAlpha", m_fieldItems.empty() ? 0 : 1);
	// 0x007975e0: the soldier picture by team, kit and armour.
	// Use the matching DDS canvases for both armours. Requesting .tga
	// finds vanilla's cropped light portrait before trying the mod's DDS;
	// heavy portraits only have DDS, so toggling armour changed framing.
	m_hud.setString("CustomSoldierPicture", "Ingame/Customization/soldiers/Soldier_" + std::to_string(std::max(0, team - 1))
			+ std::to_string(m_kit) + (m_heavy ? "1" : "") + ".dds");
	m_hud.setString("OpenCloseKitCustomString",
		text(m_customOpen ? "HUD_TEXT_MENU_UNLOCK_CLOSE" : "HUD_TEXT_MENU_UNLOCK_OPEN"));
	// The unlock screen's branches (0x0044a130): the level reached, the
	// branch's length, both in eighths, and its next unlock.
	for (int k = 0; k < kKits; ++k) {
		const world::ObjectTemplate* kit = static_cast<size_t>(k) < kits.size() ? m_templates.find(kits[static_cast<size_t>(k)].name) : nullptr;
		for (int b = 0; b < 4; ++b) {
			int count = 0, reached = -1;
			std::string nextIcon, nextName;
			bool largePic = false;
			for (size_t c = 0; kit && c < kit->children().size(); ++c) {
				const std::string& name = kit->children()[c].name;
				const world::ObjectTemplate* t = m_templates.find(name);
				const int level = std::atoi(setting(name, "unlockLevel").c_str());
				if (!t || t->typeName() != "ItemContainer" || game::unlockBranch(setting(name, "unlockBranch")) != b || level >= kItems
					|| t->children().empty()) {
					continue;
				}
				++count;
				const bool have = owns(name);
				if (have) {
					reached = std::max(reached, level);
				}
			}
			if (const auto* next = nextFieldItem(k, b)) {
				nextIcon = setting(next->name(), "weaponHud.unlockIcon");
				const std::string hudName = setting(next->name(), "weaponHud.hudName");
				nextName = hudName.empty() ? std::string() : text(hudName);
				largePic = mainWeapon(next->name());
			}
			const std::string p = "Kit" + std::to_string(k) + "Branch" + std::to_string(b);
			m_hud.set(p + "UnlockLevel", static_cast<float>(reached + 1) * 0.125f);
			m_hud.set(p + "MaxUnlockLevel", static_cast<float>(count) * 0.125f);
			m_hud.set(p + "Alpha", nextIcon.empty() ? 0.2f : 1.0f);
			m_hud.setString(p + "NextUnlockIconPathString", nextIcon);
			m_hud.setString(p + "NextUnlockNameString", nextName);
			m_hud.set(p + "NextUnlockShow", nextIcon.empty() ? 0 : 1);
			m_hud.set(p + "LargePicShow", largePic ? 1 : 0);
		}
	}
}

void KitMenu::chooseKitSelection(int a, int b)
{
	// 0x00799c70: -1 opens the customise screen (b 0: rebuilt for the kit).
	if (a >= 0) {
		return;
	}
	m_customOpen = true;
	if (b == 0) {
		m_built = false;
	}
	resetInformationString();
}

void KitMenu::closeCustomInterface()
{
	// 0x00798070 → 0x00797d30.
	m_customOpen = false;
	resetInformationString();
	for (int i = 0; i < kRows; ++i) {
		m_hud.set("KitCustom" + std::to_string(i) + "SelectShow", 0);
		m_hud.set("KitCustom" + std::to_string(i) + "MouseOver", 0);
	}
	commit();
}

void KitMenu::chooseKitCustom(int row, int index)
{
	// 0x00798090 (take) + 0x007986f0 (equip in the row's own slot).
	if (row < 0 || row >= kRows || index < 0 || index >= kItems) {
		return;
	}
	const Item& it = m_grid[static_cast<size_t>(row)][static_cast<size_t>(index)];
	if (!it.present || !it.available) {
		return;
	}
	auto& chosen = m_chosen[static_cast<size_t>(m_kit)][static_cast<size_t>(row)];
	chosen = chosen == index || (row == 0 && index == 0) ? -1 : index; // the kit's own weapon is no unlock
	m_changed = true;
	m_built = false;
}

void KitMenu::setInformationString(int row, int index)
{
	// 0x007981e0: row 5 is the armour; row 0 shows the large picture.
	if (row == 5) {
		m_hud.setString("KitItemInfoString", index == 0 ? text("DESCRIPTION_ARMOR_MOUSEOVER") : std::string());
		m_hud.setString("SmallInfoPic", index == 0 ? "Ingame/Customization/Rendered/Weapons/Armour.tga" : kEmpty);
		m_hud.setString("LargeInfoPic", kEmpty);
		return;
	}
	if (row < 0 || row >= kRows || index < 0 || index >= kItems) {
		return;
	}
	const Item& it = m_grid[static_cast<size_t>(row)][static_cast<size_t>(index)];
	m_hud.setString("KitItemInfoString", it.description.empty() ? std::string() : text(it.description));
	m_hud.setString(row == 0 ? "LargeInfoPic" : "SmallInfoPic", orEmpty(it.picture));
	m_hud.setString(row == 0 ? "SmallInfoPic" : "LargeInfoPic", kEmpty);
	m_hud.set("KitCustom" + std::to_string(row) + "MouseOver", 1);
}

void KitMenu::resetInformationString()
{
	// 0x00797530.
	m_hud.setString("KitItemInfoString", text("HUD_TEXT_MENU_CUSTOMIZE_DEFAULT_TEXT"));
	m_hud.setString("LargeInfoPic", kEmpty);
	m_hud.setString("SmallInfoPic", kEmpty);
	for (int i = 0; i < kRows; ++i) {
		m_hud.set("KitCustom" + std::to_string(i) + "MouseOver", 0);
	}
}

void KitMenu::setHeavyArmor(bool heavy)
{
	// 0x007976c0: the player's armour choice (player +0x388).
	if (heavy != m_heavy) {
		m_heavy = heavy;
		m_changed = true;
	}
}

void KitMenu::commit()
{
	// 0x00799aa0: the chosen unlocks of every kit go with SelectKit.
	if (!m_changed) {
		return;
	}
	m_changed = false;
	std::vector<std::string> containers;
	const int saved = m_kit;
	for (int k = 0; k < kKits; ++k) {
		m_kit = k;
		build(m_team);
		for (int row = 0; row < kRows; ++row) {
			for (const Item& it : m_grid[static_cast<size_t>(row)]) {
				if (it.active && !it.container.empty()) {
					containers.push_back(it.container);
				}
			}
		}
	}
	m_kit = saved;
	build(m_team);
	if (onEquip) {
		onEquip(containers);
	}
}

} // namespace dice::hfe::menu
