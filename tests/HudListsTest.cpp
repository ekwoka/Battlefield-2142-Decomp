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
#include "Main/HudMenus.h"
#include "Main/MenuBindings.h"
#include "Physics/CollisionTemplateManager.h"
#include "Render/TextureSuffix.h"
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
	// Each category keeps its lines from the latest: chat 15 s, kills 20 s.
	chat.update(14.95f);
	CHECK(chat.chatRows().size() == 1 && chat.chatRows()[0] == "\xa7" "1Bob [Rifle] Alice\xa7" "0");
	chat.update(5.0f);
	CHECK(chat.chatRows().size() == 0 && hud.number("ChatListShow") == 0);
	// A steady stream of kills keeps three rows, the newest last, and they all
	// go 20 s after the last one.
	for (int i = 0; i < 40; ++i) {
		chat.addKill("Bob", "V" + std::to_string(i), "KILLMESSAGE_WEAPON_X", false, menu::HudChat::Speaker::Enemy);
		chat.update(1.0f);
	}
	CHECK(chat.chatRows().size() == 3 && chat.chatRows()[2] == "\xa7" "1Bob [Rifle] V39\xa7" "0");
	chat.update(19.0f);
	chat.update(0.1f);
	chat.update(0.1f);
	chat.update(0.1f);
	CHECK(chat.chatRows().empty());
	// A death of one's own: HUD_HUD_KILLEDBY_MYSELF's #PLAYERNAME# (0x007518d0).
	chat.setLocalizer([](const std::string& key) {
		return key == "HUD_HUD_KILLEDBY_MYSELF" ? std::string("#PLAYERNAME# is no more.") : key;
	});
	chat.addKill("", "Carol", "", false, menu::HudChat::Speaker::Team);
	chat.update(0.1f);
	CHECK(chat.chatRows().size() == 1 && chat.chatRows()[0] == "\xa7" "1Carol is no more.\xa7" "0");
	chat.update(20.0f);
	chat.update(0.1f);

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
	kits.selectDroppedItem(2);
	kits.chooseKitCustom(1, 1);
	kits.selectDroppedItem(1);
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
	kits.selectDroppedItem(0);
	kits.update(1);
	CHECK(hud.string("KitWeaponIcon0Path") == "carbine_icon.tga");
	kits.commit();
	kits.update(1);
	CHECK(hud.string("KitWeaponIcon0Path") == "carbine_icon.tga");
	kits.chooseKitCustom(0, 0);
	kits.selectDroppedItem(0);
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

TEST(HudLists_FieldUpgradeSelectionAndExpiry)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "Kit");
	templates.registerType(2, "GenericFireArm");
	templates.registerType(3, "ItemContainer");
	templates.registerType(4, "ControlPoint");
	templates.registerType(5, "SpawnPoint");
	templates.registerType(6, "Soldier");
	auto* point = templates.create("ControlPoint", "Home");
	point->settings()["controlPointId"] = "1";
	point->settings()["team"] = "2";
	objects.create(point, {0, 0, 0});
	auto* spawn = templates.create("SpawnPoint", "Spawn");
	objects.create(spawn, {0, 1, 0})->setControlPointId(1);
	templates.create("Soldier", "soldier")->settings()["armor.maxHitPoints"] = "100";
	const auto makeKit = [&](const std::string& kitName, const std::string& name) {
		auto* kit = templates.create("Kit", kitName);
		auto* primary = templates.create("GenericFireArm", kitName + "Rifle");
		primary->settings()["itemIndex"] = "3";
		kit->children().push_back({primary->name(), {}, {}});
		auto* item = templates.create("GenericFireArm", name + "Item");
		item->settings()["itemIndex"] = "5";
		item->settings()["weaponHud.hudName"] = "New gadget";
		item->settings()["weaponHud.unlockIcon"] = "upgrade.tga";
		auto* container = templates.create("ItemContainer", name);
		container->children().push_back({item->name(), {}, {}});
		kit->children().push_back({name, {}, {}});
	};
	makeKit("EU", "EuUnlock");
	makeKit("PAC", "PacUnlock");
	game::GameWorld round(templates, objects);
	game::GameLogic logic;
	logic.team(2).kits.push_back({"EU", "soldier", ""});
	logic.team(1).kits.push_back({"PAC", "soldier", ""});
	round.startRound();
	// A human owns every unlock, so has no field upgrade to take: the
	// credit isn't given and the HUD shows none.
	const int human = round.joinPlayer(2);
	round.giveFieldUnlock(human);
	CHECK(round.fieldUnlocks(human) == 0 && round.ownsKitUnlock(human, "EuUnlock") && round.ownsKitUnlock(human, "PacUnlock"));
	{
		hud::HudManager humanHud = emptyHud();
		CHECK(round.joinSquad(human, 2));
		menu::updateSquadInfo(humanHud, round, human, false, {});
		CHECK(humanHud.number("UnlockInfoBarShow") == 0);
	}
	// The field upgrades' selection, on a bot (owning only what it is given)
	// as the menu's player.
	const int me = round.addPlayer("Bot", 2, true);
	CHECK(round.joinSquad(me, 1));
	round.setKit(me, "EU", "soldier");
	hud::HudManager hud = emptyHud();
	menu::KitMenu kits(round, logic, templates, hud);
	kits.skipUnchanged = true;
	kits.onSpendFieldUpgrade = [&](int kit, int branch) { return round.spendFieldUnlock(me, logic, kit, branch); };
	kits.onEquip = [&](const std::vector<std::string>& items) { round.setUnlocks(me, items); };
	Console console;
	kits.bind(console);
	std::string output;
	const auto execute = [&](const char* command) { CHECK(console.execute(command, output) == ConsoleResult::Ok); };
	const auto update = [&] {
		kits.setFieldUpgrades(round.fieldUnlocks(me), round.fieldUnlockItems(me));
		kits.update(2);
		menu::updateSquadInfo(hud, round, me, false, {});
	};
	round.setSquadOrderGate(true); // a ranked server's squads (0x00430a00)
	round.addSquadOrderPoints(me, 4.5);
	update();
	// 0x007a2100: no bar before a set-max message.
	CHECK(hud.number("SquadOrderInfoBarShow") == 0);
	round.setSquadOrderMax(me, 10);
	update();
	CHECK(hud.number("SquadOrderInfoBarShow") == 1 && hud.number("SquadInfoValue") == 0.45f);
	execute("UnlockInterface.showUnlockInterface 1");
	CHECK(hud.number("UnlocksShow") == 0);
	round.giveFieldUnlock(me);
	round.giveFieldUnlock(me);
	update();
	CHECK(hud.number("UnlockInfoBarShow") == 1 && hud.number("KitUnlockArrow0Show") == 1);
	execute("UnlockInterface.showUnlockInterface 1");
	CHECK(hud.number("UnlocksShow") == 1 && hud.number("UnlockInfoShow") == 1);
	execute("UnlockInterface.setFieldUnlock 0 UB_First");
	CHECK(hud.number("Kit0Branch0Selected") == 1);
	execute("UnlockInterface.selectUnlock 1");
	CHECK(hud.number("UnlockInfoShow") == 0 && hud.string("PickedUnlockNameString") == "New gadget");
	update();
	CHECK(round.fieldUnlocks(me) == 1 && round.ownsKitUnlock(me, "PacUnlock"));
	CHECK(kits.item(1, 0).available && hud.number("Kit1Item0FieldUnlocked") == 1);
	execute("UnlockInterface.spendFieldUnlock 0 UB_First");
	CHECK(round.fieldUnlocks(me) == 1);
	kits.chooseKitCustom(1, 0);
	kits.selectDroppedItem(1);
	kits.commit();
	CHECK(round.unlocksOf(me, false) == std::vector<std::string> {"EuUnlock"});
	CHECK(round.spawnSoldier(me));
	game::GameWorld::PlayerInfo spawned;
	CHECK(round.playerInfo(me, spawned));
	CHECK(std::find(spawned.carried.begin(), spawned.carried.end(), std::make_pair(5, std::string("EuUnlockItem")))
		!= spawned.carried.end());
	round.restartPlayers(1);
	round.resetRound();
	update();
	CHECK(round.fieldUnlocks(me) == 0 && round.unlocksOf(me, false).empty() && !kits.item(1, 0).available);
	CHECK(hud.number("SquadOrderInfoBarShow") == 0 && hud.number("UnlockInfoBarShow") == 0);
}

// 0x00798090 / 0x00798cf0 / 0x007986f0: an item is taken on the press and
// equipped only when released over a drop slot; the two branch rows share
// slots 1 and 2, so two items of one branch (the recon's IT33 and RDX) go together.
TEST(HudLists_KitMenuDragAndDrop)
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
	make("GenericFireArm", "sniper", {{"itemIndex", "3"}, {"weaponHud.unlockIcon", "sniper.tga"}});
	make("GenericFireArm", "mine", {{"itemIndex", "5"}, {"weaponHud.unlockIcon", "mine.tga"}});
	make("GenericFireArm", "c4", {{"itemIndex", "5"}, {"weaponHud.unlockIcon", "c4.tga"}});
	make("GenericFireArm", "camo", {{"itemIndex", "5"}, {"weaponHud.unlockIcon", "camo.tga"}});
	make("GenericFireArm", "beacon", {{"itemIndex", "6"}, {"weaponHud.unlockIcon", "beacon.tga"}});
	make("ItemContainer", "Sni0", {{"unlockLevel", "0"}}, {"mine"});
	make("ItemContainer", "SpecOp0", {{"unlockLevel", "0"}, {"unlockBranch", "UB_Second"}}, {"c4"});
	make("ItemContainer", "SpecOp2", {{"unlockLevel", "2"}, {"unlockBranch", "UB_Second"}}, {"camo"});
	make("ItemContainer", "Leader0", {{"unlockLevel", "0"}, {"unlockBranch", "UB_SquadLeader"}}, {"beacon"});
	make("Kit", "Recon", {}, {"sniper", "Sni0", "SpecOp0", "SpecOp2", "Leader0"});
	game::GameWorld round(templates, objects);
	game::GameLogic logic;
	logic.team(1).kits.push_back({"Recon", "soldier", "heavy_soldier"});
	hud::HudManager hud = emptyHud();
	menu::KitMenu kits(round, logic, templates, hud);
	kits.setUnlocked({}, true);
	kits.update(1);
	std::vector<std::string> equipped;
	kits.onEquip = [&](const std::vector<std::string>& c) { equipped = c; };
	Console console;
	kits.bind(console);
	std::string out;
	const auto run = [&](const std::string& line) { CHECK(console.execute(line, out) == ConsoleResult::Ok); };
	// A press alone equips nothing; let go elsewhere, it is dropped.
	run("kitInterface.chooseKitCustom 2 0");
	kits.endDrag();
	run("kitInterface.selectDroppedItem 1"); // a later release over a slot: nothing taken
	kits.update(1);
	CHECK(!kits.item(2, 0).active && hud.string("KitCustom1Icon") == "Ingame/GeneralIcons/empty.tga");
	// RDX into slot 1, IT33 into slot 2: both carried.
	run("kitInterface.chooseKitCustom 2 0");
	run("kitInterface.selectDroppedItem 1");
	run("kitInterface.chooseKitCustom 2 2");
	run("kitInterface.selectDroppedItem 2");
	kits.update(1);
	CHECK(kits.item(2, 0).active && kits.item(2, 2).active);
	CHECK(hud.string("KitCustom1Icon") == "c4.tga" && hud.string("KitCustom2Icon") == "camo.tga");
	// A squad leader item drops only on its own slot.
	run("kitInterface.chooseKitCustom 4 0");
	run("kitInterface.selectDroppedItem 1");
	kits.update(1);
	CHECK(!kits.item(4, 0).active && hud.string("KitCustom1Icon") == "c4.tga");
	run("kitInterface.chooseKitCustom 4 0");
	run("kitInterface.selectDroppedItem 4");
	kits.commit();
	CHECK((equipped == std::vector<std::string> {"SpecOp0", "SpecOp2", "Leader0"}));
	// The first branch's mine takes slot 1's place; the C4 is out.
	run("kitInterface.chooseKitCustom 1 0");
	run("kitInterface.selectDroppedItem 1");
	kits.update(1);
	CHECK(kits.item(1, 0).active && !kits.item(2, 0).active && kits.item(2, 2).active);
	// IT33 moved to slot 1 leaves slot 2 empty; dropped on its own slot again, it comes out.
	run("kitInterface.chooseKitCustom 2 2");
	run("kitInterface.selectDroppedItem 1");
	kits.update(1);
	CHECK(hud.string("KitCustom1Icon") == "camo.tga" && hud.string("KitCustom2Icon") == "Ingame/GeneralIcons/empty.tga");
	CHECK(!kits.item(1, 0).active);
	run("kitInterface.chooseKitCustom 2 2");
	run("kitInterface.selectDroppedItem 1");
	kits.update(1);
	CHECK(!kits.item(2, 2).active && hud.string("KitCustom1Icon") == "Ingame/GeneralIcons/empty.tga");
	// The taken item rides the pointer until the release.
	hud.setPointer(400, 300);
	run("kitInterface.chooseKitCustom 2 0");
	hud::HudQuad dragged;
	CHECK(hud.dragQuad(dragged) && dragged.texture == "Menu/HUD/Texture/c4.tga");
	kits.endDrag();
	CHECK(!hud.dragQuad(dragged));
}

// Hidden minimap markers must not linger in HudManager::mapMarkerAt() when
// CustomInfo hides the map; overlapping drop MouseOverNodes (Remaster's
// UnlockOneMouseOver / UnlockTwoMouseOver) must let the topmost slot win; and
// setChosenUnlocks() must equip Active Camouflage without wiping allUnlocked
// abilities/replacements.
TEST(HudLists_CustomizeDragClearsHiddenMapAndPreservesAllUnlocked)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "ControlPoint");
	templates.registerType(2, "SpawnPoint");
	templates.registerType(3, "Soldier");
	templates.registerType(4, "Kit");
	templates.registerType(5, "GenericFireArm");
	templates.registerType(6, "ItemContainer");
	auto* cp = templates.create("ControlPoint", "Home");
	cp->settings()["controlPointId"] = "1";
	cp->settings()["team"] = "2";
	objects.create(cp, {0, 0, 0});
	auto* sp = templates.create("SpawnPoint", "Spawn");
	objects.create(sp, {0, 1, 0})->setControlPointId(1);
	templates.create("Soldier", "soldier")->settings()["armor.maxHitPoints"] = "100";

	const auto make = [&](const char* type, const char* name, std::vector<std::pair<const char*, const char*>> settings,
						  std::vector<const char*> children = {}) {
		world::ObjectTemplate* t = templates.create(type, name);
		for (const auto& [key, value] : settings) {
			t->settings()[key] = value;
		}
		for (const char* child : children) {
			t->children().push_back({child, {}, {}});
		}
		return t;
	};
	make("GenericFireArm", "sniper", {{"itemIndex", "3"}, {"weaponHud.unlockIcon", "sniper.tga"}});
	make("GenericFireArm", "item_dummy", {{"itemIndex", "0"}});
	make("GenericFireArm", "frag", {{"itemIndex", "4"}});
	make("GenericFireArm", "mine", {{"itemIndex", "5"}, {"altItemIndex", "6"}, {"weaponHud.unlockIcon", "mine.tga"}});
	make("GenericFireArm", "c4", {{"itemIndex", "5"}, {"altItemIndex", "6"}, {"weaponHud.unlockIcon", "c4.tga"}});
	auto* camo = make("GenericFireArm", "Unl_Active_Camo",
		{{"itemIndex", "5"}, {"altItemIndex", "6"}, {"weaponHud.unlockIcon", "camo.tga"}, {"ammo.magSize", "-1"},
			{"ammo.nrOfMags", "-1"}, {"fire.roundsPerMinute", "1800"}, {"heatAddWhenFire", "0.005"}, {"coolDownPerSec", "0.08"},
			{"overheatPenalty", "1.5"}});
	camo->components().push_back("CamouflageAmmoComp");
	make("ItemContainer", "Sni0", {{"unlockLevel", "0"}}, {"mine"});
	make("ItemContainer", "SpecOp0", {{"unlockLevel", "0"}, {"unlockBranch", "UB_Second"}}, {"c4"});
	make("ItemContainer", "SpecOp2", {{"unlockLevel", "2"}, {"unlockBranch", "UB_Second"}}, {"Unl_Active_Camo"});
	make("ItemContainer", "Player0",
		{{"unlockLevel", "0"}, {"unlockBranch", "UB_Player"}, {"replaceItem.list", "item_dummy;"}, {"replaceWithoutEquip", "1"}},
		{"frag"});
	make("Kit", "EU_Recon", {}, {"sniper", "item_dummy", "Sni0", "SpecOp0", "SpecOp2", "Player0"});

	io::FileManager files;
	physics::CollisionTemplateManager collisionTemplates;
	game::GameWorld round(templates, objects);
	round.unlockAllByDefault = true;
	physics::Ground ground;
	ground.height = [](float, float) { return 0.0f; };
	ground.heightAndNormal = [](float, float, physics::Vec3& n) {
		n = {0, 1, 0};
		return 0.0f;
	};
	ground.material = [](float, float) { return 0u; };
	round.enablePhysics(files, collisionTemplates, ground);
	game::GameLogic logic;
	logic.team(2).kits.push_back({"EU_Recon", "soldier", ""});
	round.startRound();
	const int me = round.joinPlayer(2);
	round.setKit(me, "EU_Recon", "soldier");

	hud::HudManager hud = emptyHud();
	menu::KitMenu kits(round, logic, templates, hud);
	kits.onEquip = [&](const std::vector<std::string>& items) { round.setChosenUnlocks(me, items); };
	Console console;
	hud::registerHudConsoleMethods(console, hud);
	kits.bind(console);
	std::string out;
	hud.setCommandRunner([&](const std::string& command) { console.execute(command, out); });
	for (const char* line : {
			 "hudBuilder.createSplitNode Global MapSplit",
			 "hudBuilder.setNodeLogicShowVariable AND CustomInfoShow 0",
			 "hudBuilder.createMapNode MapSplit Minimap",
			 "hudBuilder.setMaxiPos -122/-271",
			 "hudBuilder.setMaxiSize 512/512",
			 "hudBuilder.setMiniPos 245/-291",
			 "hudBuilder.setMiniSize 146/146",
			 "hudBuilder.createTransformNode Global CustomInfo 276 26 535 535",
			 "hudBuilder.setNodeShowVariable CustomInfoShow",
			 "hudBuilder.createMouseOverNode CustomInfo UnlockOneMouseOver 288 393 60 40",
			 "hudBuilder.setMouseOverNodeConCmd \"kitInterface.selectDroppedItem 1\" 3",
			 "hudBuilder.createMouseOverNode CustomInfo UnlockTwoMouseOver 339 393 60 40",
			 "hudBuilder.setMouseOverNodeConCmd \"kitInterface.selectDroppedItem 2\" 3",
			 "hudBuilder.createButtonNode CustomInfo KitEquipFive 420 275 43 43",
			 "hudBuilder.setButtonNodeConCmd \"kitInterface.chooseKitCustom 2 2\" 0",
			 "hudBuilder.setButtonNodeConCmd \"kitInterface.resetMousePic 1\" 3",
			 "hudBuilder.createMouseOverNode Global ResetPicArea 0 0 800 600",
			 "hudBuilder.setMouseOverNodeConCmd \"kitInterface.resetMousePic 1\" 3",
		 }) {
		CHECK(console.execute(line, out) == ConsoleResult::Ok);
	}
	hud.setMap("Levels/x/Hud/Minimap/ingameMap.dds", -512, -512, 1024, 1024);
	// Place a spawn marker directly underneath KitEquipFive (717, 322).
	hud::HudManager::MapMarker spawn;
	spawn.texture = "spawn.dds";
	spawn.x = 366;
	spawn.z = -74;
	spawn.size = hud::HudManager::kSpawnMarkerSize;
	spawn.id = 4;
	hud.setMapMarkers({spawn});
	hud.set("MapFullSize", 1);
	kits.setUnlocked({}, true);
	kits.update(2);
	hud.frame(0);
	CHECK(hud.mapMarkerAt(717, 322) == 4);

	// Open Customize: map hides and stale map markers must disappear.
	kits.chooseKitSelection(-1, 0);
	kits.update(2);
	hud.frame(0);
	CHECK(hud.mapMarkerAt(717, 322) == -1);

	// Click Active Camouflage and release on the overlap edge of UnlockTwoMouseOver (x=620, y=430).
	CHECK(hud.click(717, 322));
	hud.release(620, 430);
	kits.endDrag();
	kits.update(2);
	kits.commit();
	CHECK(kits.item(2, 2).active);
	CHECK(hud.string("KitCustom1Icon") == "Ingame/GeneralIcons/empty.tga");
	CHECK(hud.string("KitCustom2Icon") == "camo.tga");

	// Spawn Recon: Active Camouflage is selectable and cloaks, and owned
	// replaceWithoutEquip unlocks (frag grenade) remain intact.
	CHECK(round.spawnSoldier(me));
	game::GameWorld::PlayerInfo spawned;
	CHECK(round.playerInfo(me, spawned));
	CHECK(std::find(spawned.carried.begin(), spawned.carried.end(), std::make_pair(5, std::string("Unl_Active_Camo")))
		!= spawned.carried.end());
	CHECK(std::find(spawned.carried.begin(), spawned.carried.end(), std::make_pair(4, std::string("frag")))
		!= spawned.carried.end());
	game::SoldierInput in;
	in.selectItem = 5;
	round.setInput(me, in);
	round.tick(1 / 30.0f);
	in = {};
	in.fire = true;
	round.setInput(me, in);
	round.tick(1 / 30.0f);
	CHECK(round.playerInfo(me, spawned) && spawned.cloaked);
}

// Passive booster-pack Ability upgrades (itemIndex 0 on an Ability container,
// such as Recon's Ability_Active_Camo at Kit2Item3) must set Kit<r>Item<i>Show
// to 0 so HudElementsKitsMenu.con routes them to PlayerInfo rather than the
// draggable ITEMS grid, and must not be draggable into equipment slots.
// Equippable gadgets (itemIndex > 0, even with an Ability component like
// Remaster's Beacon) remain shown and draggable.
TEST(HudLists_KitMenuPassiveAbilityNotInEquipGrid)
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
		return t;
	};
	make("GenericFireArm", "sniper", {{"itemIndex", "3"}, {"weaponHud.unlockIcon", "sniper.tga"}});
	make("GenericFireArm", "carbine", {{"itemIndex", "3"}, {"weaponHud.unlockIcon", "carbine.tga"}});
	make("GenericFireArm", "decoy", {{"itemIndex", "5"}, {"weaponHud.unlockIcon", "decoy.tga"}});
	make("GenericFireArm", "camo_gadget", {{"itemIndex", "5"}, {"weaponHud.unlockIcon", "camo.tga"}});
	make("GenericFireArm", "Ability_Active_Camo", {{"itemIndex", "0"}, {"weaponHud.unlockIcon", "ability_camo.tga"}});
	make("GenericFireArm", "beacon", {{"itemIndex", "8"}, {"weaponHud.unlockIcon", "beacon.tga"}});
	make("ItemContainer", "Sni3", {{"unlockLevel", "3"}, {"unlockBranch", "UB_First"}}, {"carbine"});
	make("ItemContainer", "Sni4", {{"unlockLevel", "4"}, {"unlockBranch", "UB_First"}}, {"decoy"});
	make("ItemContainer", "SpecOp2", {{"unlockLevel", "2"}, {"unlockBranch", "UB_Second"}}, {"camo_gadget"});
	make("ItemContainer", "SpecOp3", {{"unlockLevel", "3"}, {"unlockBranch", "UB_Second"}}, {"carbine"});
	auto* passiveCamo = make("ItemContainer", "SpecOp4",
		{{"unlockLevel", "4"}, {"unlockBranch", "UB_Second"}, {"ability.setOverheatMod.list", "camo_gadget 0.72;"}},
		{"Ability_Active_Camo"});
	passiveCamo->components().push_back("Ability");
	auto* slBeacon = make("ItemContainer", "Leader0", {{"unlockLevel", "0"}, {"unlockBranch", "UB_SquadLeader"}}, {"beacon"});
	slBeacon->components().push_back("Ability");
	make("Kit", "EU_Recon", {}, {"sniper", "Sni3", "Sni4", "SpecOp2", "SpecOp3", "SpecOp4", "Leader0"});

	game::GameWorld round(templates, objects);
	game::GameLogic logic;
	logic.team(2).kits.push_back({"EU_Recon", "soldier", ""});
	hud::HudManager hud = emptyHud();
	menu::KitMenu kits(round, logic, templates, hud);
	kits.setUnlocked({}, true);
	kits.update(2);

	// Kit1Item3 is decoy (equippable): Show=1. Kit2Item3 is Ability_Active_Camo (passive): Show=0.
	CHECK(kits.item(1, 3).present && !kits.item(1, 3).passive && hud.number("Kit1Item3Show") == 1);
	CHECK(kits.item(2, 3).present && kits.item(2, 3).passive && hud.number("Kit2Item3Show") == 0);
	CHECK(kits.item(4, 0).present && !kits.item(4, 0).passive && hud.number("Kit4Item0Show") == 1);

	// Attempting to drag the passive upgrade into slot 1 must be ignored.
	kits.chooseKitCustom(2, 3);
	kits.selectDroppedItem(1);
	kits.update(2);
	CHECK(!kits.item(2, 3).active && hud.string("KitCustom1Icon") == "Ingame/GeneralIcons/empty.tga");

	// Active Camo gadget (2, 2), Decoy (1, 3), and Squad Leader Beacon (4, 0) remain draggable.
	kits.chooseKitCustom(2, 2);
	kits.selectDroppedItem(1);
	kits.chooseKitCustom(1, 3);
	kits.selectDroppedItem(2);
	kits.chooseKitCustom(4, 0);
	kits.selectDroppedItem(4);
	kits.update(2);
	CHECK(kits.item(2, 2).active && hud.string("KitCustom1Icon") == "camo.tga");
	CHECK(kits.item(1, 3).active && hud.string("KitCustom2Icon") == "decoy.tga");
	CHECK(kits.item(4, 0).active && hud.string("KitCustom4Icon") == "beacon.tga");
}

// The spawn screen's ability icons (0x00798b20, from the rebuild while
// the soldier is dead): the player's ability block after the kit build
// (0x00570ab0: each owned container's Ability), addExtraAmmoItem's flag,
// +8 and +4 lighting Ability0..2Active (only ever set; the full reset
// clears them), the sprint use and recharge modifiers, the overheat pairs
// and the beacon modifier Ability3..6Active.
TEST(HudLists_KitMenuAbilityIcons)
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
	make("GenericFireArm", "rifle", {{"weaponHud.weaponIcon", "rifle_icon.tga"}});
	make("GenericFireArm", "ability_item", {});
	make("ItemContainer", "Player0", {{"unlockLevel", "0"}, {"unlockBranch", "UB_Player"}, {"replaceWithoutEquip", "1"},
		{"ability.addExtraAmmoItem.list", "item_dummy 0 0 1;"}}, {"ability_item"});
	make("ItemContainer", "Player1", {{"unlockLevel", "1"}, {"unlockBranch", "UB_Player"}, {"ability.SprintUseModifier", "0.66"}},
		{"ability_item"});
	make("ItemContainer", "Player2", {{"unlockLevel", "2"}, {"unlockBranch", "UB_Player"},
		{"ability.SprintRechargeModifier", "1.5"}}, {"ability_item"});
	make("ItemContainer", "Player3", {{"unlockLevel", "3"}, {"unlockBranch", "UB_Player"},
		{"ability.addExtraAmmoItem.list", "as_handgun 3 0 0;"}}, {"ability_item"});
	make("ItemContainer", "Player4", {{"unlockLevel", "4"}, {"unlockBranch", "UB_Player"},
		{"ability.addExtraAmmoItem.list", "unl_grenade_emp 0 1 1;unl_grenade_frag 0 1 1;"}}, {"ability_item"});
	make("ItemContainer", "Camo", {{"unlockLevel", "2"}, {"ability.setOverheatMod.list", "Unl_Active_Camo 0.72;"}},
		{"ability_item"});
	make("ItemContainer", "Leader", {{"unlockLevel", "4"}, {"unlockBranch", "UB_SquadLeader"},
		{"ability.beaconSpawnTimeMod", "0.8"}}, {"ability_item"});
	make("Kit", "Recon", {}, {"rifle", "Player0", "Player1", "Player2", "Player3", "Player4", "Camo", "Leader"});
	game::GameWorld round(templates, objects);
	game::GameLogic logic;
	logic.team(1).kits.push_back({"Recon", "soldier", "heavy_soldier"});
	hud::HudManager hud = emptyHud();
	menu::KitMenu kits(round, logic, templates, hud);
	const auto lit = [&] {
		std::string on;
		for (int i = 0; i < 7; ++i) {
			on += hud.number("Ability" + std::to_string(i) + "Active") != 0 ? '1' : '0';
		}
		return on;
	};
	// Owned, chosen or not: the grenades' flag and the sprint use.
	kits.setUnlocked({"Player0", "Player1"}, false);
	kits.update(1);
	CHECK(lit() == "1001000");
	// The extra sidearm clip (+8, its first number) lights Ability1, the
	// extra grenade (+4) Ability2, as the script's nodes have them.
	kits.setUnlocked({"Player0", "Player1", "Player3"}, false);
	kits.update(1);
	CHECK(lit() == "1101000");
	kits.setUnlocked({"Player4", "Player2", "Camo", "Leader"}, false);
	kits.update(1);
	CHECK(lit() == "1110111"); // 0..2 stay set
	// Alive: not worked out again.
	kits.setAlive(true);
	kits.setUnlocked({}, false);
	kits.update(1);
	CHECK(lit() == "1110111");
	kits.setAlive(false);
	kits.resetAbilityIcons();
	kits.setUnlocked({"Player2"}, false);
	kits.update(1);
	CHECK(lit() == "0000100");
	// Every one with "all unlocked".
	kits.setUnlocked({}, true);
	kits.update(1);
	CHECK(lit() == "1111111");
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
				kits.selectDroppedItem(0);
				kits.update(team);
				CHECK(hud.string("KitWeaponIcon" + std::to_string(kit) + "Path") == icon->second);
				kits.setKit((kit + 1) % 4);
				kits.update(team);
				CHECK(hud.string("KitWeaponIcon" + std::to_string(kit) + "Path") == icon->second);
				kits.setKit(kit);
				kits.update(team);
				kits.chooseKitCustom(0, 0);
				kits.selectDroppedItem(0);
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
	if (test::remaster()) {
		checkShippedKitGrid(gameDir, "mods/Project_Remaster_v17_Final");
	}
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
	kits.selectDroppedItem(1);
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
