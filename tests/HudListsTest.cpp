// The HUD's chat and kill lists, say boxes, map vote and VOIP list
// (Main/HudMenusChat.h), the kit buttons and customise screen
// (Main/HudMenusKits.h), and the award definitions (Stats.h) with the
// menu's award progress list.
#include "TestMain.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Hud/Hud.h"
#include "IO/File/FileManager.h"
#include "Main/HudMenusChat.h"
#include "Main/HudMenusKits.h"
#include "Render/TextureSuffix.h"
#include "Main/MenuBindings.h"
#include "Stats/Stats.h"
#include "UI/Player/Natives.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/TemplateFiles.h"
#include "World/WorldConsole.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

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
	CHECK(hud.number("ChatListShow") == 1 && hud.listCell(13, 0, 0) == "You were killed by Bob"); // the personal list
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

// A question's lines (category 8, HUD slot 0x174) go together when it is
// answered (slot 0x180 with 8); other personal lines stay.
TEST(HudLists_QuestionLinesCleared)
{
	hud::HudManager hud = emptyHud();
	menu::HudChat chat(hud);
	chat.addPersonal("timed out");
	chat.addQuestion("You have been accepted as Commander");
	chat.addQuestion("Press 'PageUp' to Accept");
	chat.update(0.1f);
	CHECK(hud.listCell(13, 2, 0) == "Press 'PageUp' to Accept");
	chat.clearQuestions();
	chat.addPersonal("declined");
	chat.update(0.1f);
	CHECK(hud.listCell(13, 0, 0) == "timed out" && hud.listCell(13, 1, 0) == "declined" && hud.listCell(13, 2, 0).empty());
}

// The local player's kill of an enemy (0x0075a07a): a personal line.
TEST(HudLists_KillScoreLine)
{
	hud::HudManager hud = emptyHud();
	menu::HudChat chat(hud);
	chat.setLocalizer([](const std::string& key) {
		return key == "HUD_CHAT_YOUGOTAKILLSCORE" ? std::string("You killed #PLAYERNAME#") : key;
	});
	chat.addKillScore("Exodus AlexNorton");
	chat.update(0.1f);
	CHECK(hud.listCell(13, 0, 0) == "You killed Exodus AlexNorton");
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
	make("GenericFireArm", "rifle", {{"itemIndex", "3"}, {"weaponHud.weaponIcon", "rifle_icon.tga"}, {"weaponHud.unlockIcon", "rifle_unlock.tga"}});
	make("GenericFireArm", "carbine", {{"itemIndex", "3"}, {"weaponHud.weaponIcon", "carbine_icon.tga"}, {"weaponHud.unlockIcon", "carbine.tga"}});
	// A sidearm unlock has a weaponIcon too, but itemIndex 2: its branch's row.
	make("GenericFireArm", "smg", {{"itemIndex", "2"}, {"weaponHud.weaponIcon", "smg_icon.tga"}, {"weaponHud.unlockIcon", "smg.tga"}});
	make("GenericFireArm", "shotgun", {{"weaponHud.unlockIcon", "shotgun.tga"}, {"weaponHud.descrString", "DESC_SHOTGUN"},
		{"weaponHud.altWeaponIcon", "shotgun_mini.tga"}, {"weaponHud.renderedPicture", "shotgun_big.tga"}});
	make("GenericFireArm", "defib", {{"weaponHud.unlockIcon", "defib.tga"}});
	make("ItemContainer", "Light1", {{"unlockLevel", "1"}}, {"shotgun"});
	make("ItemContainer", "Medic0", {{"unlockLevel", "0"}, {"unlockBranch", "UB_Second"}}, {"defib"});
	make("ItemContainer", "Leader0", {{"unlockLevel", "0"}, {"unlockBranch", "UB_SquadLeader"}}, {"smg"});
	make("ItemContainer", "Light3", {{"unlockLevel", "3"}}, {"carbine"});
	make("Kit", "Assault", {{"vehicleHud.hudName", "\"KIT_ASSAULT\""}, {"vehicleHud.vehicleIcon", "kit.tga"}},
		{"rifle", "Light1", "Medic0", "Leader0", "Light3"});
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
	// 0x00798dd0: itemIndex 3 is the main weapon area (the carbine after the
	// rifle), the SMG stays in the squad leader row.
	CHECK(kits.item(0, 1).present && hud.string("Kit0Item1Icon") == "carbine.tga" && !kits.item(0, 2).present);
	CHECK(kits.item(4, 0).present && hud.string("Kit4Item0Icon") == "smg.tga");
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
	CHECK(hud.string("CustomSoldierPicture") == "Ingame/Customization/soldiers/Soldier_001.dds");
	CHECK(hud.number("Kit0Branch0UnlockLevel") == 0.25f && hud.number("Kit0Branch0MaxUnlockLevel") == 0.25f);
	kits.skipUnchanged = true;
	kits.setUnlocked({}, true);
	kits.update(1);
	kits.chooseKitCustom(0, 1);
	kits.update(1);
	CHECK(hud.string("KitWeaponIcon0Path") == "carbine_icon.tga");
	kits.commit();
	kits.update(1);
	CHECK(hud.string("KitWeaponIcon0Path") == "carbine_icon.tga");
	kits.chooseKitCustom(0, 0);
	kits.update(1);
	CHECK(hud.string("KitWeaponIcon0Path") == "rifle_icon.tga");
	// Shipped top tab order: open customization, then hide field unlocks.
	// The per-kit button hides field unlocks first, then opens customization.
	Console kitConsole;
	kits.bind(kitConsole);
	std::string commandOutput;
	for (int cycle = 0; cycle < 2; ++cycle) {
		CHECK(kitConsole.execute("kitInterface.closeCustomInterface 1", commandOutput) == ConsoleResult::Ok);
		CHECK(!kits.customOpen());
		CHECK(kitConsole.execute("kitInterface.chooseKitSelection -1 0", commandOutput) == ConsoleResult::Ok);
		CHECK(kitConsole.execute("unlockInterface.showUnlockInterface 0", commandOutput) == ConsoleResult::Ok);
		kits.update(1);
		CHECK(kits.customOpen() && hud.number("CustomInfoShow") == 1);
	}
	CHECK(kitConsole.execute("kitInterface.closeCustomInterface 1", commandOutput) == ConsoleResult::Ok);
	CHECK(kitConsole.execute("unlockInterface.showUnlockInterface 0", commandOutput) == ConsoleResult::Ok);
	kits.update(1);
	CHECK(!kits.customOpen() && hud.number("CustomInfoShow") == 0);
	CHECK(kitConsole.execute("unlockInterface.showUnlockInterface 0", commandOutput) == ConsoleResult::Ok);
	CHECK(kitConsole.execute("kitInterface.chooseKitSelection -1 1", commandOutput) == ConsoleResult::Ok);
	kits.update(1);
	CHECK(kits.customOpen() && hud.number("CustomInfoShow") == 1);

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

// The customise grid with the shipped kits, as `+modPath <mod>` loads them
// (needs $BF2142_GAME_DIR): every kit of both teams gets its weapons with icons.
static void checkShippedKitGrid(const char* gameDir, const char* modPath)
{
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath(modPath);
	if (std::string(modPath) != "mods/bf2142") {
		files.addSearchPath("mods/bf2142");
	}
	files.setMountingAllowed(true);
	Console console(&files);
	bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
		[&files](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
	std::string out;
	CHECK(console.run("ServerArchives.con", {}, out));
	CHECK(console.run("ClientArchives.con", {}, out));
	files.setMountingAllowed(false);
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::registerWorldConsoleMethods(console, templates, objects);
	templates.setCurrentFileProvider([&console] { return console.currentFile(); });
	std::vector<std::string> untyped;
	std::ifstream table(std::string(BF2142_SOURCE_DIR) + "/symbols/BF2142.exe.console.csv");
	std::string line;
	std::getline(table, line);
	while (std::getline(table, line)) {
		std::vector<std::string> cols;
		std::stringstream row(line);
		for (std::string col; std::getline(row, col, ',');) {
			cols.push_back(col);
		}
		if (cols.size() >= 6 && cols[0] == "ObjectTemplate" && !console.hasMethod("ObjectTemplate." + cols[1])
			&& (cols[2] == "property" || (cols[2] == "method" && cols[4] == "1" && cols[5] == "1"))) {
			untyped.push_back(cols[1]);
		}
	}
	world::registerUntypedTemplateProperties(console, templates, untyped);
	templates.setLoading(true);
	world::loadObjectTemplates(console, files, "Objects/", false);
	templates.setLoading(false);

	game::GameWorld round(templates, objects);
	game::GameLogic logic;
	for (const char* kit : {"Recon", "Assault", "Anti-Vehicle", "Support"}) {
		logic.team(1).kits.push_back({std::string("EU_") + kit, "eu_light_soldier", "eu_heavy_soldier"});
		logic.team(2).kits.push_back({std::string("Pac_") + kit, "pac_light_soldier", "pac_heavy_soldier"});
	}
	hud::HudManager hud([&files](const std::string& path, std::string& text) {
		std::vector<uint8_t> data;
		if (!files.readFile(path, data)) {
			return false;
		}
		text.assign(data.begin(), data.end());
		return true;
	});
	hud::registerHudConsoleMethods(console, hud);
	console.setScriptOutput(false, false);
	CHECK(console.run("Menu/HUD/HudSetup/HudSetupMain.con", {}, out));
	menu::KitMenu kits(round, logic, templates, hud);
	kits.setUnlocked({}, true);
	for (int team = 1; team <= 2; ++team) {
		for (int kit = 0; kit < 4; ++kit) {
			kits.setKit(kit);
			kits.update(team);
			int shown = 0, icons = 0, missing = 0;
			for (int row = 0; row < menu::KitMenu::kRows; ++row) {
				for (int i = 0; i < menu::KitMenu::kItems; ++i) {
					const std::string p = "Kit" + std::to_string(row) + "Item" + std::to_string(i);
					shown += hud.number(p + "Show") == 1;
					icons += hud.string(p + "Icon").find("empty.tga") == std::string::npos;
					std::string path = "Menu/HUD/Texture/" + hud.string(p + "Icon");
					std::replace(path.begin(), path.end(), '\\', '/');
					if (hud.number(p + "Show") == 1 && !files.exists(path)) {
						std::printf("  missing %s = %s\n", p.c_str(), hud.string(p + "Icon").c_str());
						++missing;
					}
				}
			}
			// The open customise screen on the spawn menu draws the weapon icons.
			hud.set("ShowIngameHud", 1);
			hud.set("ShowSoldierHud", 1);
			hud.set("MenuBackgroundAlpha", 1);
			hud.set("SpawnShow", 1);
			hud.set("KitsShow", 1);
			hud.set("MembersShow", 0);
			kits.chooseKitSelection(-1, 0);
			kits.update(team);
			std::vector<hud::HudQuad> quads;
			for (int f = 0; f < 10; ++f) {
				quads = hud.frame(0.2f);
			}
			std::set<std::string> drawnTextures;
			for (const hud::HudQuad& q : quads) {
				drawnTextures.insert(q.texture);
			}
			int drawn = 0;
			for (int row = 0; row < menu::KitMenu::kRows; ++row) {
				for (int i = 0; i < menu::KitMenu::kItems; ++i) {
					std::string icon = hud.string("Kit" + std::to_string(row) + "Item" + std::to_string(i) + "Icon");
					std::replace(icon.begin(), icon.end(), '\\', '/');
					const bool shown = drawnTextures.count("Menu/HUD/Texture/" + icon) > 0;
					drawn += shown;
				}
			}
			CHECK(drawn >= 8);
			// The Assault's main weapon area: its rifle and the rifles it unlocks
			// (the HUD lays out 3 slots, Remaster's kits 5).
			int primaries = 0;
			for (int i = 0; i < menu::KitMenu::kItems; ++i) {
				primaries += kits.item(0, i).present;
			}
			if (kit == 1) {
				CHECK(primaries == (std::string(modPath) == "mods/bf2142" ? 3 : 5));
			}
			// Selecting each shipped primary updates the deployment card, including
			// Remaster's Engineer replacements, and survives leaving the kit.
			for (int index = 1; index < primaries; ++index) {
				const auto* container = templates.find(kits.item(0, index).container);
				CHECK(container && !container->children().empty());
				if (!container || container->children().empty()) { continue; }
				const auto* weapon = templates.find(container->children().front().name);
				CHECK(weapon);
				if (!weapon) { continue; }
				const auto icon = weapon->settings().find("weaponHud.weaponIcon");
				CHECK(icon != weapon->settings().end());
				if (icon == weapon->settings().end()) { continue; }
				kits.chooseKitCustom(0, index);
				kits.update(team);
				CHECK(hud.string("KitWeaponIcon" + std::to_string(kit) + "Path") == icon->second);
				kits.setKit((kit + 1) % 4);
				kits.update(team);
				CHECK(hud.string("KitWeaponIcon" + std::to_string(kit) + "Path") == icon->second);
				kits.setKit(kit);
				kits.update(team);
				kits.chooseKitCustom(0, 0);
				kits.update(team);
			}
			// Both armours use the active mod's matching DDS canvases, rather
			// than mixing a cropped vanilla light TGA with a heavy DDS.
			for (bool heavy : {false, true}) {
				kits.setHeavyArmor(heavy);
				kits.update(team);
				const std::string portrait = "Menu/HUD/Texture/" + hud.string("CustomSoldierPicture");
				CHECK(portrait.substr(portrait.size() - 4) == ".dds");
				std::vector<uint8_t> portraitData;
				CHECK(files.readFile(portrait, portraitData));
				CHECK(portraitData.size() >= 20);
				if (portraitData.size() >= 20) {
					CHECK(std::string(portraitData.begin(), portraitData.begin() + 4) == "DDS ");
					const auto dimension = [&](size_t offset) {
						return unsigned(portraitData[offset]) | (unsigned(portraitData[offset + 1]) << 8)
							| (unsigned(portraitData[offset + 2]) << 16) | (unsigned(portraitData[offset + 3]) << 24);
					};
					const unsigned scale = std::string(modPath) == "mods/bf2142" ? 1 : 2;
					CHECK(dimension(12) == 512 * scale && dimension(16) == 128 * scale);
				}
				const auto names = render::texturePathCandidates(portrait);
				CHECK(std::any_of(names.begin(), names.end(), [&files](const std::string& n) { return files.exists(n); }));
			}
			kits.setHeavyArmor(false);
			CHECK(shown >= 8 && icons == shown && missing == 0);
		}
	}
}

TEST(HudLists_KitGridShippedKits)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	checkShippedKitGrid(gameDir, "mods/bf2142");
	checkShippedKitGrid(gameDir, "mods/Project_Remaster_v17_Final");
}

TEST(HudLists_KitMenuSkipsUnchangedInEnhancedMode)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.create("GenericFireArm", "shotgun")->settings()["weaponHud.unlockIcon"] = "shotgun.tga";
	world::ObjectTemplate* light = templates.create("ItemContainer", "Light1");
	light->settings()["unlockLevel"] = "1";
	light->children().push_back({"shotgun", {}, {}});
	templates.create("Kit", "Assault")->children().push_back({"Light1", {}, {}});
	game::GameWorld round(templates, objects);
	game::GameLogic logic;
	logic.team(1).kits.push_back({"Assault", "soldier", "heavy_soldier"});
	hud::HudManager hud = emptyHud();
	menu::KitMenu kits(round, logic, templates, hud);
	kits.skipUnchanged = true;
	kits.setUnlocked({"Light1"}, false);
	kits.update(1);
	CHECK(hud.number("Kit0Show") == 1 && hud.number("Kit0SprintAbility") == 0.75f);
	// Nothing changed: the writes are skipped (a marker survives).
	hud.set("Kit0Show", 7);
	kits.update(1);
	CHECK(hud.number("Kit0Show") == 7);
	// Each input it shows refreshes it: the armour, a choice, the unlocks, the team's kits.
	kits.setHeavyArmor(true);
	kits.update(1);
	CHECK(hud.number("Kit0Show") == 1 && hud.number("Kit0SprintAbility") == 0.25f);
	hud.set("Kit0Show", 7);
	kits.chooseKitCustom(1, 1);
	kits.update(1);
	CHECK(hud.number("Kit0Show") == 1 && hud.number("Kit1Item1Active") == 1);
	hud.set("Kit0Show", 7);
	kits.setUnlocked({}, false);
	kits.update(1);
	CHECK(hud.number("Kit0Show") == 1 && hud.number("Kit1Item1Alpha") == 0.3f);
	hud.set("Kit0Show", 7);
	logic.team(1).kits.push_back({"Support", "soldier", "heavy_soldier"});
	kits.update(1);
	CHECK(hud.number("Kit0Show") == 1);
	// Retail (off): every frame rewrites.
	kits.skipUnchanged = false;
	hud.set("Kit0Show", 7);
	kits.update(1);
	CHECK(hud.number("Kit0Show") == 1);
}
