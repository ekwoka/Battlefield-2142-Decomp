#include "HudMenusKits.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Hud/Hud.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cstdlib>

namespace dice::hfe::menu {

namespace {

constexpr const char* kEmpty = "Ingame/GeneralIcons/empty.tga";

// UnlockBranch (0x00508xxx): UB_First 0, UB_Second 1, UB_Player 2, UB_SquadLeader 3.
int branchOf(const std::string& value)
{
	if (value == "UB_Second" || value == "1") {
		return 1;
	}
	if (value == "UB_Player" || value == "2") {
		return 2;
	}
	if (value == "UB_SquadLeader" || value == "3") {
		return 3;
	}
	return 0;
}

std::string orEmpty(const std::string& path) { return path.empty() ? std::string(kEmpty) : path; }

} // namespace

KitMenu::KitMenu(game::GameWorld& round, game::GameLogic& logic, world::ObjectTemplateManager& templates, hud::HudManager& hud)
	: m_round(round), m_logic(logic), m_templates(templates), m_hud(hud)
{
	for (auto& kit : m_chosen) {
		kit.fill(-1);
	}
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
	// UnlockInterface (0x007b8e60-0x007b9a20): without field-unlock points
	// (offline) the screen doesn't open and nothing is spent.
	bindMethod(console, "UnlockInterface.showUnlockInterface", any, [this](int show) {
		if (!show && m_customOpen) {
			closeCustomInterface(); // 0x007b95a0's second case
		}
		m_hud.set("UnlocksShow", 0);
		m_hud.set("UnlockInfoShow", 0);
	});
	bindMethod(console, "UnlockInterface.setInformationString", any, [this](int kit, int branch) {
		// 0x007b9890: the branch's next unlock.
		std::string description, picture;
		const auto& kits = m_logic.team(m_team == 1 ? 1 : 2).kits;
		const world::ObjectTemplate* t = kit >= 0 && static_cast<size_t>(kit) < kits.size() ? m_templates.find(kits[static_cast<size_t>(kit)].name) : nullptr;
		for (size_t c = 0; t && c < t->children().size(); ++c) {
			const std::string& name = t->children()[c].name;
			const world::ObjectTemplate* container = m_templates.find(name);
			if (!container || container->typeName() != "ItemContainer" || container->children().empty()
				|| branchOf(setting(name, "unlockBranch")) != branch
				|| std::find(m_unlocked.begin(), m_unlocked.end(), name) != m_unlocked.end()) {
				continue;
			}
			const std::string item = container->children().front().name;
			description = setting(item, "weaponHud.descrString");
			picture = setting(item, "weaponHud.renderedPicture");
			break;
		}
		m_hud.setString("UnlockInfoString", description.empty() ? std::string() : text(description));
		m_hud.setString("UnlockInfoPic", orEmpty(picture));
	});
	bindMethod(console, "UnlockInterface.resetInformationString", any, [this](int) {
		m_hud.setString("UnlockInfoString", text("HUD_TEXT_MENU_FU_DEFAULT_TEXT"));
		m_hud.setString("UnlockInfoPic", kEmpty);
	});
	bindMethod(console, "UnlockInterface.setFieldUnlock", any, [this](int kit, int branch) {
		for (int k = 0; k < kKits; ++k) {
			for (int b = 0; b < 4; ++b) {
				m_hud.set("Kit" + std::to_string(k) + "Branch" + std::to_string(b) + "Selected", k == kit && b == branch ? 1 : 0);
			}
		}
	});
	bindMethod(console, "UnlockInterface.selectUnlock", any, [](int) {});           // no points to spend
	bindMethod(console, "UnlockInterface.spendFieldUnlock", any, [](int, int) {});  // 0x004c1f30: none offline
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
			// Row 0: the kit's own weapon in the primary slot (the one with a weaponIcon).
			if (!m_grid[0][0].present && !setting(child.name, "weaponHud.weaponIcon").empty()) {
				fill(m_grid[0][0], child.name);
				m_grid[0][0].available = true;
			}
			continue;
		}
		const int level = std::atoi(setting(child.name, "unlockLevel").c_str());
		if (level < 0 || level >= kItems || t->children().empty()) {
			continue; // the level-15 replacements aren't on the grid
		}
		Item& item = m_grid[static_cast<size_t>(branchOf(setting(child.name, "unlockBranch")) + 1)][static_cast<size_t>(level)];
		fill(item, t->children().front().name);
		item.container = child.name;
		item.available = m_allUnlocked || std::find(m_unlocked.begin(), m_unlocked.end(), child.name) != m_unlocked.end();
	}
	m_grid[0][0].active = true; // no other primary chosen
	for (int row = 1; row < kRows; ++row) {
		const int chosen = m_chosen[static_cast<size_t>(m_kit)][static_cast<size_t>(row)];
		if (chosen >= 0 && m_grid[static_cast<size_t>(row)][static_cast<size_t>(chosen)].available) {
			m_grid[static_cast<size_t>(row)][static_cast<size_t>(chosen)].active = true;
		}
	}
}

void KitMenu::update(int team)
{
	if (!m_built || team != m_team) {
		build(team);
	}
	const auto& kits = m_logic.team(team == 1 ? 1 : 2).kits;
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
		for (const world::ChildTemplate& child : kit->children()) {
			const world::ObjectTemplate* t = m_templates.find(child.name);
			if (!t) {
				continue;
			}
			if (t->typeName() == "ItemContainer") {
				const int level = std::atoi(setting(child.name, "unlockLevel").c_str());
				if (branchOf(setting(child.name, "unlockBranch")) == 0 && level < kItems) {
					unlockLevels = std::max(unlockLevels, level);
					// 0x0044a3f0: the first branch's unlock in the primary slot.
					if (altIcon.empty() && !t->children().empty()) {
						altIcon = setting(t->children().front().name, "weaponHud.altWeaponIcon");
					}
				}
			} else if (weaponIcon.empty()) {
				weaponIcon = setting(child.name, "weaponHud.weaponIcon");
			}
		}
		m_hud.setString("KitWeaponIcon" + n + "Path", orEmpty(weaponIcon));
		m_hud.set("KitUnlock" + n + "Show", unlockLevels >= 1 ? 1 : 0);
		m_hud.setString("KitAltWeaponIcon" + n + "Path", orEmpty(altIcon));
		m_hud.set("KitUnlockArrow" + n + "Show", 0); // no field unlock to take
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
			m_hud.set(p + "FieldUnlocked", 0);
			m_hud.set(p + "Show", it.present ? 1 : 0);
			m_hud.setString(p + "Icon", orEmpty(it.icon));
			m_hud.set(p + "Alpha", it.available ? 1.0f : 0.3f);
			if (it.active) {
				chosenIcon = it.icon;
			}
		}
		m_hud.setString("KitCustom" + std::to_string(row) + "Icon", orEmpty(chosenIcon));
	}
	m_hud.set("CustomInfoShow", m_customOpen ? 1 : 0);
	m_hud.set("HeavyArmorActive", m_heavy ? 1 : 0);
	m_hud.set("NotAntiVechicleKitChoosen", 1);
	m_hud.set("UnlockInfoBarShow", 0);
	m_hud.set("FUBlinkAlpha", 0);
	m_hud.set("UnlockBlinkAlpha", 0);
	// 0x007975e0: the soldier picture by team, kit and armour.
	m_hud.setString("CustomSoldierPicture", "Ingame/Customization/soldiers/Soldier_" + std::to_string(std::max(0, team - 1))
			+ std::to_string(m_kit) + (m_heavy ? "1" : "") + ".tga");
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
				if (!t || t->typeName() != "ItemContainer" || branchOf(setting(name, "unlockBranch")) != b || level >= kItems
					|| t->children().empty()) {
					continue;
				}
				++count;
				const bool have = m_allUnlocked || std::find(m_unlocked.begin(), m_unlocked.end(), name) != m_unlocked.end();
				if (have) {
					reached = std::max(reached, level);
				}
			}
			for (size_t c = 0; kit && c < kit->children().size(); ++c) {
				const std::string& name = kit->children()[c].name;
				const world::ObjectTemplate* t = m_templates.find(name);
				if (t && t->typeName() == "ItemContainer" && !t->children().empty() && branchOf(setting(name, "unlockBranch")) == b
					&& std::atoi(setting(name, "unlockLevel").c_str()) == reached + 1) {
					const std::string item = t->children().front().name;
					nextIcon = setting(item, "weaponHud.unlockIcon");
					const std::string hudName = setting(item, "weaponHud.hudName");
					nextName = hudName.empty() ? std::string() : text(hudName);
					largePic = !setting(item, "weaponHud.weaponIcon").empty();
				}
			}
			const std::string p = "Kit" + std::to_string(k) + "Branch" + std::to_string(b);
			m_hud.set(p + "UnlockLevel", static_cast<float>(reached + 1) * 0.125f);
			m_hud.set(p + "MaxUnlockLevel", static_cast<float>(count) * 0.125f);
			m_hud.set(p + "Alpha", 1.0f);
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
	if (!it.present || !it.available || row == 0) {
		return;
	}
	auto& chosen = m_chosen[static_cast<size_t>(m_kit)][static_cast<size_t>(row)];
	chosen = chosen == index ? -1 : index;
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
		for (int row = 1; row < kRows; ++row) {
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
