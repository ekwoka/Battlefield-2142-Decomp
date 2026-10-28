// The spawn screen's kits: the kit buttons, the kit customise screen
// (KitInterface.*) and the field-unlock screen (UnlockInterface.*), as the
// HUD manager and the menu interface of BF2142.exe drive them.
//
// The kit buttons (HudElementsSpawn.con, kit i = 0..6) are recomputed by
// the HUD manager's 0x0044e650(team) on its events (entering the spawn
// screen, a kit change, the armour, an equip): Kit%iShow (KitsShow when
// the team has the kit), KitName%iString (the kit's hudName, localized),
// KitIcon%iPath, KitWeaponIcon%iPath (the chosen primary's icon, else the
// kit weapon's weaponHud.weaponIcon), Kit%iAbilityIcon0..6 (the kit's own
// specialAbilityIcons, then those of the chosen unlocks), KitUnlock%iShow
// (the kit's first branch has unlocks), KitAltWeaponIcon%iPath,
// Kit%iSprintAbility (0x00449600: 0.75 light, 0.25 heavy armour),
// Kit%iHeavyArmorShow / HeavyArmor%iActive, PlayerKitIcon%iSelectShow
// (setPlayerKit, 0x0079aa50), KitUnlockArrow%iShow (a field unlock to
// take: none offline).
//
// The customise screen (KitInterface, constructor 0x00798610; rebuild
// 0x00798dd0) is a grid of five rows of up to six items per kit: row 0
// the primary weapon (the kit's default in its primary slot), rows 1-4
// the unlock branches UB_First, UB_Second, UB_Player, UB_SquadLeader, an
// item at its container's unlockLevel. Each item is available when the
// soldier has unlocked it (alpha 1, else 0.3), shows its
// weaponHud.unlockIcon, and carries its specialAbilityIcon,
// renderedPicture and descrString (else hudName). The display copy of the
// open kit (0x007977a0) sets Kit<row>Item<item>Active / FieldUnlocked /
// Show / Icon / Alpha; KitCustom<row>Icon the chosen item of each row;
// CustomInfoShow, CustomSoldierPicture
// (Ingame/Customization/soldiers/Soldier_<team-1><kit>[1].tga, the 1 for
// heavy armour, 0x007975e0), KitItemInfoString with LargeInfoPic /
// SmallInfoPic (0x007981e0), OpenCloseKitCustomString, HeavyArmorActive,
// NotAntiVechicleKitChoosen.
//
// Picking: the original drags an item (chooseKitCustom 0x00798090 takes
// it on the pointer) onto a slot (selectDroppedItem 0x00798cf0 → equip
// 0x007986f0: rows 0, 3 and 4 choose one item, rows 1 and 2 share two
// drop slots). The rewrite's HUD has no dragging: chooseKitCustom equips
// the item in its own row's slot at once. The choice is committed when
// the soldier spawns or the screen closes (0x00799aa0 sends SelectKit with
// the chosen unlocks): `onEquip` gets the chosen ItemContainers.
//
// The unlock screen (UnlockInterface, 0x007b8e60-0x007b9a20) spends field
// unlocks (in-round unlocks a server grants, the player's +0x2d0 points):
// with none it doesn't open (0x007b95a0), so offline it shows only its
// per-branch variables (0x0044a130): Kit%iBranch%iUnlockLevel and
// MaxUnlockLevel (eighths), Alpha, NextUnlockIconPathString,
// NextUnlockNameString, NextUnlockShow, LargePicShow.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
namespace game {
class GameWorld;
class GameLogic;
} // namespace game
namespace hud {
class HudManager;
}
namespace world {
class ObjectTemplateManager;
}
} // namespace dice::hfe

namespace dice::hfe::menu {

class KitMenu {
public:
	static constexpr int kRows = 5, kItems = 6, kKits = 7;
	KitMenu(game::GameWorld& round, game::GameLogic& logic, world::ObjectTemplateManager& templates, hud::HudManager& hud);

	// KitInterface.* and UnlockInterface.* console methods.
	void bind(Console& console);
	void setLocalizer(std::function<std::string(const std::string&)> localize) { m_localize = std::move(localize); }
	// The containers a kit's unlocked items are in ("all": every one).
	void setUnlocked(std::vector<std::string> containers, bool all)
	{
		if (containers == m_unlocked && all == m_allUnlocked) {
			return;
		}
		m_unlocked = std::move(containers);
		m_allUnlocked = all;
		m_built = false;
	}
	// The kit picked on the spawn screen (setPlayerKit).
	void setKit(int kit);
	int kit() const { return m_kit; }
	// Each frame: the kit buttons, the open kit's grid and the unlock
	// screen's variables for the local player's team.
	void update(int team);
	// The chosen unlocks' ItemContainers (for GameWorld::setUnlocks), told
	// when the choice is committed (closing the screen; spawning).
	std::function<void(const std::vector<std::string>&)> onEquip;
	void commit();

	// The console methods' work.
	void chooseKitSelection(int a, int b);
	void closeCustomInterface();
	void chooseKitCustom(int row, int item);
	void setInformationString(int row, int item);
	void resetInformationString();
	void setHeavyArmor(bool heavy);
	bool heavyArmor() const { return m_heavy; }
	bool customOpen() const { return m_customOpen; }

	struct Item {
		bool present = false;
		bool available = false;
		bool active = false;
		std::string container; // the ItemContainer (empty for row 0)
		std::string icon, abilityIcon, picture, description;
	};
	const Item& item(int row, int index) const { return m_grid[static_cast<size_t>(row)][static_cast<size_t>(index)]; }

private:
	void build(int team);
	std::string text(const std::string& key) const { return m_localize ? m_localize(key) : key; }
	std::string setting(const std::string& templateName, const std::string& key) const;

	game::GameWorld& m_round;
	game::GameLogic& m_logic;
	world::ObjectTemplateManager& m_templates;
	hud::HudManager& m_hud;
	std::function<std::string(const std::string&)> m_localize;
	std::vector<std::string> m_unlocked;
	bool m_allUnlocked = false;
	bool m_built = false;
	int m_team = 0;
	int m_kit = 0;
	bool m_customOpen = false;
	bool m_heavy = false;
	bool m_changed = false; // a choice since the last commit
	std::array<std::array<Item, kItems>, kRows> m_grid;
	std::array<std::array<int, kRows>, kKits> m_chosen {}; // the active item per row, per kit (-1 none)
	bool m_chosenSet = false;
};

} // namespace dice::hfe::menu
