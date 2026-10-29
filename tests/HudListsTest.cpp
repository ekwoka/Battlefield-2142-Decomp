// The HUD's chat and kill lists, say boxes, map vote and VOIP list
// (Main/HudMenusChat.h), the kit buttons and customise screen
// (Main/HudMenusKits.h), and the award definitions (Stats.h) with the
// menu's award progress list.
#include "TestMain.h"

#include "Console/Console.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Hud/Hud.h"
#include "Main/HudMenusChat.h"
#include "Main/HudMenusKits.h"
#include "Main/MenuBindings.h"
#include "Stats/Stats.h"
#include "UI/Player/Natives.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

using namespace dice::hfe;

namespace {

hud::HudManager emptyHud()
{
	return hud::HudManager([](const std::string&, std::string&) { return false; });
}

} // namespace

TEST(HudLists_ChatLinesAndSayBoxes)
{
	hud::HudManager hud = emptyHud();
	menu::HudChat chat(hud);
	chat.setLocalizer([](const std::string& key) {
		if (key == "HUD_TEXT_CHAT_TEAM") {
			return std::string("<TEAM> ");
		}
		if (key == "HUD_HUD_KILLEDBY_KILLS") {
			return std::string("#PLAYERNAME1# [#WEAPON#] #PLAYERNAME2#");
		}
		if (key == "HUD_CHAT_YOUWEREKILLEDBY") {
			return std::string("You were killed by #PLAYERNAME#");
		}
		return key == "KILLMESSAGE_WEAPON_X" ? std::string("Rifle") : key;
	});
	chat.addChat("Alice", "hello", 1, menu::HudChat::Speaker::Team);
	chat.addKill("Bob", "Alice", "KILLMESSAGE_WEAPON_X", false, menu::HudChat::Speaker::Enemy);
	chat.setKilledBy("Bob");
	chat.update(0.1f);
	CHECK(chat.chatRows().size() == 2);
	CHECK(chat.chatRows()[0] == "\xa7" "1<TEAM> Alice: hello\xa7" "0");
	CHECK(chat.chatRows()[1] == "\xa7" "1Bob [Rifle] Alice\xa7" "0");
	CHECK(hud.number("ChatListShow") == 1 && hud.string("DeathMessage") == "You were killed by Bob");
	// Each category keeps its lines 5 s from the latest.
	chat.update(4.95f);
	CHECK(chat.chatRows().size() == 0 && hud.number("ChatListShow") == 0);
	// A death of one's own: HUD_HUD_KILLEDBY_MYSELF's #PLAYERNAME# (0x007518d0).
	chat.setLocalizer([](const std::string& key) {
		return key == "HUD_HUD_KILLEDBY_MYSELF" ? std::string("#PLAYERNAME# is no more.") : key;
	});
	chat.addKill("", "Carol", "", false, menu::HudChat::Speaker::Team);
	chat.update(0.1f);
	CHECK(chat.chatRows().size() == 1 && chat.chatRows()[0] == "\xa7" "1Carol is no more.\xa7" "0");
	chat.update(5.0f);

	// Typing: the box opens, printable characters go in, Enter sends.
	std::string said;
	menu::HudChat::Box box = menu::HudChat::kNone;
	chat.onSay = [&](menu::HudChat::Box b, const std::string& text) {
		box = b;
		said = text;
	};
	chat.open(menu::HudChat::kTeam);
	chat.update(0);
	CHECK(hud.number("SayTeamShow") == 1 && hud.editFocus() == 1);
	chat.type("gg\x01!");
	chat.key(8);
	chat.type("?");
	chat.update(0);
	CHECK(hud.editText(1) == "gg?");
	chat.key(13);
	CHECK(said == "gg?" && box == menu::HudChat::kTeam && chat.typing() == menu::HudChat::kNone);
	// The same box's key again closes it.
	chat.open(menu::HudChat::kAll);
	chat.open(menu::HudChat::kAll);
	CHECK(chat.typing() == menu::HudChat::kNone);

	// The squad name box: eight characters at most.
	menu::SquadNameBox name(hud);
	name.select();
	name.type("Raptors1234");
	CHECK(hud.editText(2) == "Raptors1" && name.take() == "Raptors1" && !name.typing());

	// The map vote: a click on the button column votes.
	menu::MapVoteList vote(hud);
	vote.setMaps({"camp_gibraltar", "minsk"});
	int voted = -1;
	vote.onVote = [&](int map) { voted = map; };
	vote.click(1, 2);
	CHECK(voted == -1);
	vote.click(1, 0);
	vote.update();
	CHECK(voted == 1 && hud.listCell(12, 1, 2) == "minsk" && hud.listCell(12, 1, 1).find("vote_square") != std::string::npos);

	// The VOIP list: the talking players, the commander's icon.
	menu::updateVoipList(hud, {{"Cmdr", true, 0}, {"Mate", false, 3}}, true, false);
	CHECK(hud.number("VoipListShow") == 1 && hud.listCell(11, 0, 1) == "Cmdr");
	CHECK(hud.listCell(11, 1, 0).find("mini_Squad_3_small") != std::string::npos && hud.number("VoiceOverTalkIconShow") == 1);
}

TEST(HudLists_KitMenu)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	const auto make = [&](const char* type, const char* name, std::vector<std::pair<const char*, const char*>> settings,
						  std::vector<const char*> children = {}) {
		world::ObjectTemplate* t = templates.create(type, name);
		for (const auto& [key, value] : settings) {
			t->settings()[key] = value;
		}
		for (const char* child : children) {
			t->children().push_back({child, {}, {}});
		}
	};
	make("GenericFireArm", "rifle", {{"weaponHud.weaponIcon", "rifle_icon.tga"}, {"weaponHud.unlockIcon", "rifle_unlock.tga"}});
	make("GenericFireArm", "shotgun", {{"weaponHud.unlockIcon", "shotgun.tga"}, {"weaponHud.descrString", "DESC_SHOTGUN"},
		{"weaponHud.altWeaponIcon", "shotgun_mini.tga"}, {"weaponHud.renderedPicture", "shotgun_big.tga"}});
	make("GenericFireArm", "defib", {{"weaponHud.unlockIcon", "defib.tga"}});
	make("ItemContainer", "Light1", {{"unlockLevel", "1"}}, {"shotgun"});
	make("ItemContainer", "Medic0", {{"unlockLevel", "0"}, {"unlockBranch", "UB_Second"}}, {"defib"});
	make("Kit", "Assault", {{"vehicleHud.hudName", "\"KIT_ASSAULT\""}, {"vehicleHud.vehicleIcon", "kit.tga"}},
		{"rifle", "Light1", "Medic0"});
	game::GameWorld round(templates, objects);
	game::GameLogic logic;
	logic.team(1).kits.push_back({"Assault", "soldier", "heavy_soldier"});
	hud::HudManager hud = emptyHud();
	menu::KitMenu kits(round, logic, templates, hud);
	kits.setUnlocked({"Light1"}, false);
	kits.update(1);
	// The kit buttons.
	CHECK(hud.number("Kit0Show") == 1 && hud.number("Kit1Show") == 0 && hud.string("KitName0String") == "KIT_ASSAULT");
	CHECK(hud.string("KitWeaponIcon0Path") == "rifle_icon.tga" && hud.number("KitUnlock0Show") == 1);
	CHECK(hud.string("KitAltWeaponIcon0Path") == "shotgun_mini.tga" && hud.number("Kit0SprintAbility") == 0.75f);
	// The grid: row 0 the rifle, the shotgun at UB_First level 1, the
	// defibrillator (locked) at UB_Second level 0.
	CHECK(kits.item(0, 0).present && kits.item(0, 0).active);
	CHECK(kits.item(1, 1).present && kits.item(1, 1).available && !kits.item(1, 1).active);
	CHECK(kits.item(2, 0).present && !kits.item(2, 0).available);
	CHECK(hud.string("Kit1Item1Icon") == "shotgun.tga" && hud.number("Kit2Item0Alpha") == 0.3f);
	// Opening, choosing (a locked one can't be), the information, closing commits.
	kits.chooseKitSelection(-1, 0);
	kits.update(1);
	CHECK(hud.number("CustomInfoShow") == 1);
	std::vector<std::string> equipped;
	kits.onEquip = [&](const std::vector<std::string>& c) { equipped = c; };
	kits.chooseKitCustom(2, 0);
	kits.chooseKitCustom(1, 1);
	kits.setInformationString(1, 1);
	kits.update(1);
	CHECK(hud.string("KitCustom1Icon") == "shotgun.tga" && hud.string("KitItemInfoString") == "DESC_SHOTGUN");
	CHECK(hud.string("SmallInfoPic") == "shotgun_big.tga");
	kits.setHeavyArmor(true);
	kits.closeCustomInterface();
	kits.update(1);
	CHECK(equipped == std::vector<std::string> {"Light1"} && hud.number("CustomInfoShow") == 0);
	CHECK(hud.string("CustomSoldierPicture") == "Ingame/Customization/soldiers/Soldier_001.tga");
	CHECK(hud.number("Kit0Branch0UnlockLevel") == 0.25f && hud.number("Kit0Branch0MaxUnlockLevel") == 0.125f);
}

TEST(HudLists_Awards)
{
	const std::string script = "awards.setData 100_2 \"6,1, ,20\" \"9,23,ktt-3,54000\"\n"
							   "awards.setData 215 \"1,40,wins;lose,10\" \"5,41,100_1,1\"\n"
							   "rem nothing\n";
	const auto awards = stats::parseAwards(script);
	CHECK(awards.size() == 2);
	if (awards.size() == 2) {
		CHECK(awards[0].number == 100 && awards[0].level == 2 && awards[0].kind() == 1 && awards[0].criteria.size() == 2);
		CHECK(!awards[0].criteria[0].global && awards[0].criteria[0].check && awards[0].criteria[0].keys.empty());
		CHECK(awards[0].criteria[1].global && awards[0].criteria[1].whole && awards[0].criteria[1].target == 54000);
		CHECK(awards[1].kind() == 0 && awards[1].criteria[0].keys == std::vector<std::string>({"wins", "lose"}));
	}
	stats::SoldierStats soldier;
	soldier.wins = 3;
	soldier.losses = 2;
	CHECK(stats::awardStatValue(soldier, "wlr") == 1.5 && stats::awardStatValue(soldier, "ktt-3") == 0);

	// The progress list: in-round rows under their header, career ones from row 4.
	ui::NativeRegistry natives;
	menu::MenuServices none;
	menu::MenuBindings bindings(natives, none);
	bindings.setAwards(awards);
	bindings.fillAwardProgress("100_2");
	const auto& rows = natives.listRows("awardProgress");
	CHECK(rows.size() == 5);
	if (rows.size() == 5) {
		CHECK(rows[0][0] == "HEADER_inaround" && rows[1][1] == "AWARD_CRITERIA_1" && rows[1][2] == "20");
		CHECK(rows[3][0] == "HEADER_global" && rows[4][1] == "AWARD_CRITERIA_23" && rows[4][2] == "0/54000");
	}
}
