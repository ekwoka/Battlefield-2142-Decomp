// The HUD's squad menu (SquadMenu, 0x007a4000-0x007a8800) and the
// commander screen's tabs, on a world of players without a level, and the
// HUD's list clicks they read.
#include "Console/Console.h"
#include "Game/GameWorld.h"
#include "Hud/Hud.h"
#include "Main/DemoCamera.h"
#include "Main/HudMenus.h"
#include "Main/HudMenusChat.h"
#include "Main/HudMenusComm.h"
#include "Main/HudMenusVehicles.h"
#include "Main/HudWorld.h"
#include "Console/ConsoleBinding.h"
#include "IO/File/FileManager.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace dice::hfe;

namespace {

struct Squads {
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	game::GameWorld world {templates, objects};
	hud::HudManager hud {nullptr};
	menu::SquadMenu squads {world, hud};
	std::vector<std::string> messages;

	Squads()
	{
		squads.onMessage = [this](const std::string& id) { messages.push_back(id); };
	}

	// The squad list's row for a squad or a member (cells 13 and 14).
	int row(int id, int kind) const
	{
		for (int r = 0; r < 64; ++r) {
			if (hud.listCell(5, r, 13) == std::to_string(id) && hud.listCell(5, r, 14) == std::to_string(kind)) {
				return r;
			}
		}
		return -1;
	}
	int squadOf(int player) const
	{
		game::GameWorld::PlayerInfo info;
		return world.playerInfo(player, info) ? info.squad : -1;
	}
};

} // namespace

TEST(HudMenus_SquadListJoinLeaveCreate)
{
	Squads s;
	const int me = s.world.addPlayer("Me", 2, false);
	const int a = s.world.addPlayer("A", 2, true);
	const int b = s.world.addPlayer("B", 2, true);
	s.world.addPlayer("Enemy", 1, true);
	s.world.joinSquad(a, 1);
	s.world.joinSquad(b, 1);
	s.squads.update(me);
	// Squad 1 (Alpha) with its two, the leader first; one unassigned.
	CHECK(s.hud.number("NrOfSquads") == 1 && s.hud.number("NrOfUnassigned") == 1);
	const int alpha = s.row(1, 2);
	CHECK(alpha == 0 && s.hud.listCell(5, alpha, 5) == "Alpha" && s.hud.listCell(5, alpha, 6) == "2/6");
	CHECK(s.row(a, 4) == 1 && s.row(b, 4) == 2);
	// JOIN on the squad's row (column 10) joins it; LEAVE leaves.
	CHECK(s.hud.listCell(5, alpha, 10) == "HUD_TEXT_MENU_SPAWN_SQUAD_JOIN");
	s.squads.clickSquadList(me, alpha, 10);
	CHECK(s.squadOf(me) == 1);
	s.squads.update(me);
	CHECK(s.hud.listCell(5, s.row(1, 2), 10) == "HUD_TEXT_MENU_SPAWN_SQUAD_LEAVE" && s.hud.number("IsInSquad") == 1);
	// In a squad, creating one is refused until it's left.
	CHECK(!s.squads.setCreateSelect(me, true) && s.messages.back() == "HUD_HUD_SQUAD_MUST_LEAVE_TO_CREATE");
	s.squads.clickSquadList(me, s.row(1, 2), 10);
	CHECK(s.squadOf(me) == 0);
	// Creating: the first free id (2, Bravo), locked as the box says.
	CHECK(s.squads.setCreateSelect(me, true) && s.squads.createShown());
	s.squads.update(me);
	CHECK(s.hud.string("SquadCreateTagString") == "2" && s.hud.number("SquadCreateShow") == 1);
	s.squads.toggleCreateLock();
	CHECK(s.squads.createSquad(me) && !s.squads.createShown());
	game::GameWorld::PlayerInfo info;
	CHECK(s.world.playerInfo(me, info) && info.squad == 2 && info.squadLeader && s.squads.locked(2, 2));
	s.squads.update(me);
	CHECK(s.hud.listCell(5, s.row(2, 2), 5) == "Bravo" && s.hud.number("IsSquadLeader") == 1);
	// A locked squad can't be joined from the list; its leader unlocks it
	// (column 8).
	s.world.joinSquad(a, 0);
	s.squads.clickSquadList(a, s.row(2, 2), 10);
	CHECK(s.squadOf(a) == 0);
	s.squads.clickSquadList(me, s.row(2, 2), 8);
	CHECK(!s.squads.locked(2, 2));
	s.squads.clickSquadList(a, s.row(2, 2), 10);
	CHECK(s.squadOf(a) == 2);
}

TEST(HudMenus_InviteList)
{
	Squads s;
	const int me = s.world.addPlayer("Me", 2, false);
	const int mate = s.world.addPlayer("Mate", 2, true);
	const int loner = s.world.addPlayer("Loner", 2, true);
	const int other = s.world.addPlayer("Other", 2, true);
	s.world.joinSquad(me, 1);
	s.world.joinSquad(mate, 1);
	s.world.joinSquad(other, 3);
	// Only a leader opens it; the members start selected.
	s.squads.setShowInviteList(mate, true);
	CHECK(!s.squads.inviteListShown());
	s.squads.setShowInviteList(me, true);
	s.squads.update(me);
	CHECK(s.hud.number("InviteListShow") == 1);
	const auto rowOf = [&](int id) {
		for (int r = 0; r < 16; ++r) {
			if (s.hud.listCell(8, r, 2) == std::to_string(id)) {
				return r;
			}
		}
		return -1;
	};
	CHECK(s.hud.listCell(8, rowOf(mate), 0) == "|cMate" && s.hud.listCell(8, rowOf(loner), 0) == "Loner");
	// Select the loner and the one in another squad, unselect the mate.
	s.squads.clickInviteList(me, rowOf(loner));
	s.squads.clickInviteList(me, rowOf(other));
	s.squads.clickInviteList(me, rowOf(mate));
	s.squads.update(me);
	CHECK(s.hud.listCell(8, rowOf(loner), 0) == "|cLoner" && s.hud.listCell(8, rowOf(mate), 0) == "Mate");
	s.squads.applyInviteList(me);
	// The mate is out; the loner (a bot in no squad) takes the invite; the
	// other stays in his squad.
	CHECK(s.squadOf(mate) == 0 && s.squadOf(loner) == 1 && s.squadOf(other) == 3 && !s.squads.inviteListShown());
}

TEST(HudMenus_ListClicks)
{
	Console console;
	hud::HudManager manager(nullptr);
	hud::registerHudConsoleMethods(console, manager);
	std::vector<std::string> ran;
	manager.setCommandRunner([&ran](const std::string& line) { ran.push_back(line); });
	std::string out;
	for (const char* line : {"hudBuilder.createSplitNode Global Root", "hudBuilder.createListNode Root Squads 12 74 246 463 19 1",
			 "hudBuilder.setListNodeData 5", "hudBuilder.setListNodeConCmd 1 SquadMenu.singleClickNew",
			 "hudBuilder.setListNodeConCmd 2 SquadMenu.doubleClickNew"}) {
		console.execute(line, out, ConsoleAccess::Anyone);
	}
	manager.setListColumns(5, {{6, 14, 2}, {-1, 0, 0}, {24, 122, 0}, {194, 46, 2}});
	manager.setList(5, {{"1", "", "Alpha", "JOIN"}, {"", "", "  Bot", ""}});
	manager.frame(0.1f);
	// The second row, under the JOIN column (x 12 + 194 and on).
	CHECK(manager.click(12 + 200, 74 + 19 + 5));
	CHECK(ran.size() == 1 && ran[0] == "SquadMenu.singleClickNew");
	CHECK(manager.lastListClick().data == 5 && manager.lastListClick().row == 1 && manager.lastListClick().column == 3);
	CHECK(manager.listSelection(5) == 1 && manager.listCell(5, 1, 2) == "  Bot");
	// Again at once: a double click.
	manager.click(12 + 30, 74 + 19 + 5);
	CHECK(ran.size() == 3 && ran[2] == "SquadMenu.doubleClickNew" && manager.lastListClick().column == 2);
}

TEST(HudMenus_CommanderSquadList)
{
	Squads s;
	menu::CommanderMenu commander(s.world, s.hud);
	const int me = s.world.addPlayer("Me", 2, false);
	const int a = s.world.addPlayer("A", 2, true);
	const int b = s.world.addPlayer("B", 2, true);
	const int c = s.world.addPlayer("C", 2, true);
	s.world.joinSquad(a, 1);
	s.world.joinSquad(b, 1);
	s.world.joinSquad(c, 4);
	s.world.setCommanderEnabled(true);
	CHECK(s.world.setCommander(2, me));
	commander.update(me);
	// A row per squad with members: Alpha, Delta.
	CHECK(s.hud.listCell(15, 0, 1) == "Alpha" && s.hud.listCell(15, 0, 4) == "2/6" && s.hud.listCell(15, 1, 13) == "4");
	// Column 4: that squad alone selected, its members shown.
	commander.click(me, 1, 4);
	commander.update(me);
	CHECK(commander.selected() == std::set<int> {4} && s.hud.number("ShowSquadMemberList") == 1);
	CHECK(s.hud.string("CurrentSquadName") == "Delta" && s.hud.string("CurrentSquadLeaderName") == "C");
	commander.click(me, 0, 1);
	commander.update(me);
	CHECK(commander.selected() == std::set<int> {1} && s.hud.listCell(15, 0, 1) == "|cAlpha");
	// 0x007ab380's modifiers: Ctrl toggles one among the others, Shift
	// takes the range from the anchor, a plain click on the only one
	// selected clears it.
	bool ctrl = false, shift = false;
	commander.ctrlHeld = [&ctrl] { return ctrl; };
	commander.shiftHeld = [&shift] { return shift; };
	ctrl = true;
	commander.click(me, 1, 1);
	CHECK((commander.selected() == std::set<int> {1, 4}));
	commander.click(me, 1, 1);
	CHECK(commander.selected() == std::set<int> {1});
	ctrl = false;
	shift = true;
	commander.click(me, 1, 1); // no anchor (4 went out): nothing
	CHECK(commander.selected() == std::set<int> {1});
	shift = false;
	commander.click(me, 0, 1); // the only one: cleared
	CHECK(commander.selected().empty());
	commander.click(me, 0, 1); // the anchor 1
	shift = true;
	commander.click(me, 1, 1); // 1 .. 4
	CHECK((commander.selected() == std::set<int> {1, 2, 3, 4}));
	shift = false;
	commander.click(me, 0, 1);
	// The tabs: closing the squad tab hides the members.
	commander.tabs.toggleSquad();
	commander.update(me);
	CHECK(s.hud.number("CommanderSquadTabShowFull") == 0 && s.hud.number("ShowSquadMemberList") == 0
		&& s.hud.number("CommanderMapTabShowFull") == 1);
	// Not the commander: clicks do nothing.
	commander.click(a, 1, 4);
	CHECK(commander.selected() == std::set<int> {1});
}

// The comm roses (0x007b8310, 0x007b7130, 0x007b73d0): the string map's
// texts on the HUD, a level below an entry, the command an entry runs.
TEST(CommRose_LevelsAndCommands)
{
	hud::HudManager hud([](const std::string&, std::string&) { return false; });
	menu::CommRoses roses(hud);
	Console console(nullptr);
	std::vector<std::string> ran;
	roses.bind(console, [&ran](const std::string& line) { ran.push_back(line); });
	std::string out;
	console.execute("RadioInterface.setStringMap Interface_1:1 HUD_COMROSE_GRUNT_spotted \"Radio.sendSpottedMessage 1 VTNoVehicleType\"", out);
	console.execute("RadioInterface.setStringMap Interface_1:8 HUD_COMROSE_GRUNT_rogerthat \"game.radioMessage TCTeamAndLocalEnemies roger_that\"", out);
	console.execute("RadioInterface.setStringMap Interface_1:2 Sub \"\"", out);
	console.execute("RadioInterface.setStringMap Interface_1:2:1 Deeper \"game.radioMessage TCTeam negative\"", out);
	roses.openRadio(false);
	CHECK(roses.shown() == "Radio" && hud.number("RadioInterfaceShow") == 1 && hud.number("RadioVehicleInterfaceShow") == 0);
	CHECK(hud.string("Radio1String") == "HUD_COMROSE_GRUNT_spotted" && hud.string("Radio8String") == "HUD_COMROSE_GRUNT_rogerthat"
		&& hud.string("Radio3String").empty());
	console.execute("RadioInterface.mouseOver 8", out);
	CHECK(hud.number("Radio8MouseOver") == 1 && hud.number("Radio1MouseOver") == 0);
	console.execute("RadioInterface.selectOrder 8", out);
	CHECK(ran.size() == 1 && ran[0] == "game.radioMessage TCTeamAndLocalEnemies roger_that" && roses.shown().empty()
		&& hud.number("RadioInterfaceShow") == 0);
	// A level down, then its entry.
	roses.openRadio(false);
	console.execute("RadioInterface.selectOrder 2", out);
	menu::CommRose* radio = roses.find("Radio");
	CHECK(radio && radio->path() == "1:2" && hud.string("Radio1String") == "Deeper" && hud.string("Radio8String").empty()
		&& roses.shown() == "Radio");
	console.execute("RadioInterface.selectOrder 1", out);
	CHECK(ran.size() == 2 && ran[1] == "game.radioMessage TCTeam negative");
	// The tactical rose by role; the vehicle's radio aboard.
	roses.openTactical(false, true, true);
	CHECK(roses.shown() == "SquadLeader" && hud.number("SquadLeaderInterfaceShow") == 1);
	roses.openRadio(true);
	CHECK(roses.shown() == "RadioVehicle" && hud.number("SquadLeaderInterfaceShow") == 0);
	roses.close();
	CHECK(roses.shown().empty());
}

// The shipped radio rose (Menu/HUD/HudSetup/Communication/Radio.con): 11
// entries, each a text and a command.
TEST(CommRose_ShippedRadio)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	Console console(&files);
	bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
		[&files](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
	std::string out;
	console.run("ServerArchives.con", {}, out);
	hud::HudManager hud([](const std::string&, std::string&) { return false; });
	menu::CommRoses roses(hud);
	std::vector<std::string> ran;
	roses.bind(console, [&ran](const std::string& line) { ran.push_back(line); });
	CHECK(console.run("Menu/HUD/HudSetup/Communication/Radio.con", {}, out));
	CHECK(console.run("Menu/HUD/HudSetup/Communication/SquadLeader.con", {}, out));
	const menu::CommRose* radio = roses.find("Radio");
	CHECK(radio && radio->entries() == 11);
	for (int i = 1; radio && i <= 11; ++i) {
		CHECK(!radio->text(i).empty() && !radio->command(i).empty());
	}
	CHECK(radio && radio->command(7) == "Radio.callForMedic" && radio->command(3) == "game.radioMessage TCLocalGroundVehicle req_pickup");
	roses.openTactical(false, true, true);
	console.execute("SquadLeaderInterface.selectOrder 9", out);
	CHECK(ran.size() == 1 && ran[0] == "SquadLeader.sendRequest 0 0");
}

// The scoreboard's rows (0x007bb540): black when dead, green for the
// squad and the commanding player, blue friends and red enemies; the
// local row's and every odd row's backgrounds.
TEST(HudScoreboard_RowColours)
{
	menu::ScoreboardRow row;
	CHECK(menu::scoreboardStyle(row, 0).text[0] == 0 && menu::scoreboardStyle(row, 0).text[2] == 0); // dead: black
	row.alive = true;
	CHECK(menu::scoreboardStyle(row, 0).text[0] > 0.9f); // an enemy: red
	row.friendlyList = true;
	CHECK(menu::scoreboardStyle(row, 0).text[2] == 1 && menu::scoreboardStyle(row, 0).text[0] == 0); // blue
	row.sameSquad = true;
	CHECK(menu::scoreboardStyle(row, 0).text[1] > 0.4f && menu::scoreboardStyle(row, 0).text[2] < 0.1f); // green
	row.sameSquad = false;
	row.local = row.localCommands = true;
	const auto local = menu::scoreboardStyle(row, 3);
	CHECK(local.text[1] > 0.4f && local.background[3] == 0.35f);
	row.local = false;
	CHECK(menu::scoreboardStyle(row, 1).background[3] == 0.8f);
	CHECK(menu::scoreboardStyle(row, 2).background[3] == 0);
	// The HUD fills the rows' backgrounds between the side borders.
	hud::HudManager hud(nullptr);
	Console console;
	hud::registerHudConsoleMethods(console, hud);
	std::string out;
	console.execute("hudBuilder.createListNode Global Board 0 0 200 100 10 1", out);
	console.execute("hudBuilder.setListNodeData 2", out);
	console.execute("hudBuilder.setListNodeBorder 10 0 3 5", out);
	hud.setListColumns(2, {{0, 50, 0}});
	hud.setList(2, {{"a"}, {"b"}});
	hud.setListRowBackgrounds(2, {{0, 0, 0, 0}, {0.5f, 0.5f, 0.5f, 0.8f}});
	bool found = false;
	for (const auto& q : hud.frame(0)) {
		found = found || (q.color[3] == 0.8f && q.x0 == 3 && q.x1 == 195 && q.y1 - q.y0 == 10);
	}
	CHECK(found);
}

// The weapon HUD (0x007c1f10): the bar, the clips, the eternal and single
// magazines, the colours, the fire rate's icon and the overheat sound once.
TEST(HudWeapon_AmmoClipsAndHeat)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "GenericFireArm");
	world::registerWorldConsoleMethods(console, templates, objects);
	world::registerUntypedTemplateProperties(console, templates,
		{"weaponHud.clipIcon", "weaponHud.ammoIcon", "weaponHud.overheatSound", "weaponHud.hasFireRate", "weaponHud.selectIcon"});
	std::string out;
	for (const char* line : {"ObjectTemplate.create GenericFireArm rifle", "ObjectTemplate.weaponHud.clipIcon Ingame\\clip.tga",
			 "ObjectTemplate.weaponHud.ammoIcon Ingame\\ammo.tga", "ObjectTemplate.weaponHud.overheatSound S_Overheat"}) {
		console.execute(line, out);
	}
	const world::ObjectTemplate* rifle = templates.find("rifle");
	CHECK(rifle);
	hud::HudManager hud(nullptr);
	menu::WeaponHud weaponHud;
	menu::WeaponHudInput in;
	in.weapon = rifle;
	in.rounds = 6;
	in.magSize = 30;
	in.mags = 4;
	in.nrOfMags = 5;
	in.fireRate = 2;
	CHECK(weaponHud.update(hud, in).empty());
	CHECK(hud.number("PrimaryAmmoShow") == 1 && hud.number("PrimaryAmmoBarShow") == 1 && hud.number("PrimaryClipsShow") == 1);
	CHECK(hud.string("PrimaryAmmoString") == "6" && hud.string("TotPrimaryAmmoString") == "/ 96");
	CHECK(hud.string("PrimaryClipString") == "(3)" && hud.number("PrimaryClip3Show") == 1);
	CHECK(hud.string("PrimaryClipPathString") == "Ingame/clip.tga" && hud.number("PrimaryAmmoIconShow") == 0);
	CHECK(hud.number("PrimaryAmmoColorGreen") < 0.2f); // 6 / 30 is under 0.3: the low colour
	CHECK(hud.string("FireRateIconPathString") == "Ingame/Weapons/Icons/Hud/firerate_2.tga" && hud.number("FireRateIconShow") == 1);
	in.rounds = 20;
	weaponHud.update(hud, in);
	CHECK(hud.number("PrimaryAmmoColorGreen") == 1);
	// No magazine: the eternal icon and no text.
	in.magSize = -1;
	weaponHud.update(hud, in);
	CHECK(hud.number("PrimaryEternalShow") == 1 && hud.string("PrimaryAmmoString").empty());
	CHECK(hud.number("PrimaryAmmoIconShow") == 1 && hud.string("PrimaryAmmoIconPathString") == "Ingame/ammo.tga");
	// One round a magazine: single fire.
	in.magSize = 1;
	in.rounds = 1;
	weaponHud.update(hud, in);
	CHECK(hud.number("PrimarySingleFireShow") == 1);
	// The overheat's sound once each time.
	in.overheated = true;
	CHECK(weaponHud.update(hud, in) == "S_Overheat");
	CHECK(weaponHud.update(hud, in).empty());
	in.overheated = false;
	weaponHud.update(hud, in);
	in.overheated = true;
	CHECK(weaponHud.update(hud, in) == "S_Overheat");
}

// The item selection (0x0079c290, 0x0079be20, 0x0079c050): packed from slot
// 0 by item index (reversed with ItemSelection.setReverseItems), the item
// in hand selected, closed when the timer runs out.
TEST(HudItemSelection_SlotsAndTimer)
{
	hud::HudManager hud(nullptr);
	Console console;
	menu::ItemSelection items;
	items.bind(console);
	items.open({{1, "knife.tga"}, {3, "rifle.tga"}, {5, "", false}}, 3);
	items.update(hud, 0.1f);
	CHECK(hud.number("ShowItemSelect") == 1 && hud.number("ItemSelectActive") == 1);
	CHECK(hud.string("Item0Index") == "1" && hud.string("Item1Icon") == "rifle.tga" && hud.number("ItemSlot2Alpha") == 0.4f);
	CHECK(hud.number("ItemSlot3Active") == 0);
	items.update(hud, 2.0f);
	CHECK(hud.number("ShowItemSelect") == 0);
	std::string out;
	console.execute("ItemSelection.setReverseItems 1", out);
	items.open({{1, "knife.tga"}, {3, "rifle.tga"}}, 1);
	items.update(hud, 0);
	CHECK(hud.string("Item0Index") == "3" && hud.number("ItemSelectActive") == 1);
}

// The vehicle HUD's seat dots (0x007a28e0, BfOccupiedVehicleNode
// 0x007e1520), its texts, and the map's vehicle markers (0x00781680,
// 0x00793b60, 0x0077dd70).
TEST(HudVehicle_SeatsAndMarkers)
{
	CHECK(menu::seatDotState(false, false, 0, 0, true) == 4);
	CHECK(menu::seatDotState(true, true, 2, 2, true) == 1);
	CHECK(menu::seatDotState(true, true, 0, 0, true) == 2);
	CHECK(menu::seatDotState(true, false, 2, 2, true) == 7);
	CHECK(menu::seatDotState(true, false, 0, 2, true) == 5);
	CHECK(menu::seatDotState(true, false, 0, 2, false) == 6);
	hud::HudManager hud(nullptr);
	Console console;
	hud::registerHudConsoleMethods(console, hud);
	std::string out;
	console.execute("hudBuilder.createOccupiedNode Global Dot1 0 6 -32 8 8", out);
	console.execute("hudBuilder.setOccupiedNodePosVariable 0 OccupyingPlayerPos1X", out);
	console.execute("hudBuilder.setOccupiedNodePosVariable 1 OccupyingPlayerPos1Y", out);
	console.execute("hudBuilder.setOccupiedNodeData 1", out);
	menu::VehicleHud vehicleHud;
	menu::VehicleHudInput in;
	in.armor = 0.25f;
	in.speed = 12.4f;
	in.altitude = 30.2f;
	in.seats = {{2, 72, 12}};
	vehicleHud.update(hud, in);
	CHECK(hud.string("VehicleArmorString") == "25" && hud.string("SpeedString") == "120" && hud.string("AltitudeString") == "31");
	CHECK(hud.number("VehicleArmorColorGreen") < 0.2f);
	bool dot = false;
	for (const auto& q : hud.frame(0)) {
		dot = dot || (q.texture.find("vehiclePositionPlayerBlue") != std::string::npos && q.x0 == 72 && q.y0 == 12);
	}
	CHECK(dot);
	using R = menu::MapRelation;
	CHECK(menu::vehicleRelation(false, true, 0, 1, 1) == R::Own);
	CHECK(menu::vehicleRelation(true, false, 2, 1, 1) == R::Enemy);
	CHECK(menu::vehicleRelation(false, false, 0, 0, 1) == R::Neutral);
	CHECK(menu::vehicleRelation(false, false, 0, 1, 1) == R::Friendly);
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "PlayerControlObject");
	world::registerWorldConsoleMethods(console, templates, objects);
	world::registerUntypedTemplateProperties(console, templates,
		{"vehicleHud.miniMapIcon", "vehicleHud.miniMapIconSize", "vehicleHud.showOnEnemyMinimap"});
	console.execute("ObjectTemplate.create PlayerControlObject tank", out);
	console.execute("ObjectTemplate.vehicleHud.miniMapIcon Ingame\\mini_tank.tga", out);
	console.execute("ObjectTemplate.vehicleHud.miniMapIconSize 20/35", out);
	const world::ObjectTemplate* tank = templates.find("tank");
	CHECK(tank);
	menu::VehicleMarker marker;
	CHECK(menu::vehicleMarker(*tank, R::Friendly, false, false, marker));
	CHECK(marker.texture == "Ingame/mini_tank.tga" && marker.width == 20 && marker.height == 35);
	CHECK(!menu::vehicleMarker(*tank, R::Enemy, false, false, marker));
	CHECK(menu::vehicleMarker(*tank, R::Enemy, false, true, marker)); // spotted
	console.execute("ObjectTemplate.vehicleHud.showOnEnemyMinimap 1", out);
	CHECK(menu::vehicleMarker(*tank, R::Enemy, false, false, marker));
}

// The passengers' names (0x007a34f0) and the scoreboard's squads
// (0x007baed0 / 0x007ba570).
TEST(HudVehicle_PassengersAndScoreboardSquads)
{
	std::vector<menu::SeatDot> seats(4);
	seats[0].occupant = "Driver";
	seats[0].relation = 3;
	seats[2].occupant = "Gunner";
	seats[2].relation = 2;
	const auto lines = menu::passengerLines(seats);
	CHECK(lines[1].text == "1. Driver" && lines[1].colour[1] == 1);
	CHECK(lines[0].text == "3. Gunner" && std::fabs(lines[0].colour[1] - 0.6f) < 1e-6f);
	CHECK(lines[2].text.empty());
	hud::HudManager hudManager(nullptr);
	menu::VehicleHud vehicleHud;
	menu::VehicleHudInput in;
	in.seats = seats;
	vehicleHud.update(hudManager, in);
	CHECK(hudManager.number("OccupyingPlayerName2Show") == 1 && hudManager.string("OccupyingPlayerName2String") == "1. Driver");
	CHECK(hudManager.number("OccupyingPlayerName3Show") == 0);

	std::vector<menu::ScoreboardMember> members {
		{0, 2, false, 30, 0, 3, 1, 50}, {1, 0, false, 20, 0, 2, 2, 0}, {2, 1, true, 10, 0, 1, 0, 0}, {3, 2, false, 5, 0, 0, 4, 30}};
	const auto rows = menu::scoreboardBySquad(members);
	CHECK(rows.size() == 7);
	CHECK(rows[0].member == -1 && rows[0].squad == -1 && !rows[0].totals && rows[1].member == 2);
	CHECK(rows[2].member == -1 && rows[2].squad == 2 && rows[2].score == 35 && rows[2].deaths == 5 && rows[2].ping == 40);
	CHECK(rows[3].member == 0 && rows[4].member == 3);
	CHECK(rows[5].squad == 0 && rows[5].totals && rows[6].member == 1);
}

// The weapon HUD's range finder (0x007c1470) and the second gun's prefix.
TEST(HudWeapon_RangeFinderAndSecondary)
{
	world::ObjectTemplate rifle(1, "GenericFireArm", "rifle", 1);
	rifle.settings()["weaponHud.hasRangeFinder"] = "1";
	hud::HudManager hudManager(nullptr);
	menu::WeaponHud weaponHud;
	menu::WeaponHudInput in;
	in.weapon = &rifle;
	in.magSize = 30;
	in.rounds = 10;
	in.mags = 2;
	weaponHud.update(hudManager, in);
	CHECK(hudManager.string("TargetDistanceString") == "999" && hudManager.string("TargetCoordXString") == "999");
	in.rangeHit = true;
	in.targetDistance = 123.4f;
	in.target = {10.4f, 20.6f, 30.2f};
	weaponHud.update(hudManager, in);
	CHECK(hudManager.string("TargetDistanceString") == "123");
	CHECK(hudManager.string("TargetCoordXString") == "10" && hudManager.string("TargetCoordYString") == "-30"
		&& hudManager.string("TargetCoordZString") == "21");
	menu::WeaponHud second;
	second.update(hudManager, in, true);
	CHECK(hudManager.string("SecondaryAmmoString") == "10" && hudManager.string("TotSecondaryAmmoString") == "/ 40");
}

// The Titan silos' map icons (miniMapInfo "all", 0x00636220; the layers of
// 0x0077dee0 / 0x0077df60; the reload wipe of 0x0078a990) and the wedge's
// pieces.
TEST(HudVehicle_SiloReloadIcons)
{
	Console console;
	std::string out;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "PlayerControlObject");
	world::registerWorldConsoleMethods(console, templates, objects);
	world::registerUntypedTemplateProperties(console, templates,
		{"vehicleHud.miniMapIcon", "vehicleHud.miniMapIconSize", "vehicleHud.miniMapInfo", "vehicleHud.miniMapReloadIcon",
			"vehicleHud.miniMapBackgroundIcon", "vehicleHud.spottedIcon"});
	console.execute("ObjectTemplate.create PlayerControlObject missile_silo", out);
	console.execute("ObjectTemplate.vehicleHud.miniMapIcon Ingame\\Vehicles\\Icons\\Minimap\\mini_reloadComposite", out);
	console.execute("ObjectTemplate.vehicleHud.miniMapReloadIcon Ingame\\Vehicles\\Icons\\Minimap\\mini_reload", out);
	console.execute("ObjectTemplate.vehicleHud.miniMapBackgroundIcon Ingame\\Vehicles\\Icons\\Minimap\\mini_titanSilo", out);
	console.execute("ObjectTemplate.vehicleHud.miniMapIconSize 32", out);
	console.execute("ObjectTemplate.vehicleHud.miniMapInfo All", out);
	const world::ObjectTemplate* silo = templates.find("missile_silo");
	CHECK(silo);
	const menu::MapIcons& icons = menu::mapIcons(*silo);
	CHECK(icons.info == 2 && icons.size[0] == 32 && icons.size[1] == 32 && icons.spottedSize == 16);
	CHECK(menu::iconFrame(icons.background, 3) == "Ingame/Vehicles/Icons/Minimap/mini_titanSilo_03.dds");
	CHECK(menu::iconFrame("Ingame/x/kit.tga", 3) == "Ingame/x/kit.tga");
	CHECK(&menu::mapIcons(*silo) == &icons); // read once
	using R = menu::MapRelation;
	CHECK(std::fabs(menu::reloadSweep(30, 120, R::Friendly) - 6.2831855f / 4) < 1e-5f);
	CHECK(menu::reloadSweep(0, 120, R::Enemy) == 0 && menu::reloadSweep(60, 120, R::Neutral) == 0);
	// A quarter turn of a 2 x 2 square about its centre: its top right
	// quarter; three quarters: three of the four.
	const std::vector<std::array<float, 4>> square {{0, 0, 0, 0}, {2, 0, 1, 0}, {2, 2, 1, 1}, {0, 2, 0, 1}};
	const auto area = [](const std::vector<std::vector<std::array<float, 4>>>& pieces) {
		float sum = 0;
		for (const auto& p : pieces) {
			for (size_t i = 0; i < p.size(); ++i) {
				const auto& a = p[i];
				const auto& b = p[(i + 1) % p.size()];
				sum += (a[0] * b[1] - b[0] * a[1]) * 0.5f;
			}
		}
		return std::fabs(sum);
	};
	const auto quarter = hud::sweepPieces(square, 1, 1, 6.2831853f / 4);
	CHECK(quarter.size() == 1 && std::fabs(area(quarter) - 1) < 1e-4f);
	for (const auto& corner : quarter[0]) {
		CHECK(corner[0] >= 1 - 1e-4f && corner[1] <= 1 + 1e-4f); // right of centre, above it
	}
	CHECK(std::fabs(area(hud::sweepPieces(square, 1, 1, 6.2831853f * 0.75f)) - 3) < 1e-4f);
	CHECK(std::fabs(area(hud::sweepPieces(square, 1, 1, 6.2831853f / 8)) - 0.5f) < 1e-4f);
	CHECK(hud::sweepPieces(square, 1, 1, 7).size() == 1 && hud::sweepPieces(square, 1, 1, 0).empty());
}

// The votes' HUD lines (the client's vote view 0x005740e0, its tick
// 0x00573ae0, the lines 0x005725e0).
TEST(HudVotes_Lines)
{
	menu::VoteMessages votes;
	std::vector<std::string> lines;
	votes.localize = [](const std::string& id) {
		if (id == "HUD_HUD_KICKVOTE_PLAYER_START_REMOTE") return std::string("#PLAYERNAME1# vs #PLAYERNAME2#");
		if (id == "HUD_HUD_KICKVOTE_PLAYER_STAT") return std::string("#YESVOTES#/#NEEDED# #PLAYERNAME#");
		if (id == "HUD_HUD_KICKVOTE_MAP_START_LOCAL") return std::string("#PLAYERNAME# #MAPNAME# #MINUTES#");
		if (id == "HUD_HUD_KICKVOTE_UNABLE_TO_ISSUE") return std::string("wait #MINUTES#");
		return id;
	};
	votes.playerName = [](int id) { return id == 1 ? std::string("Me") : id == 2 ? std::string("Bob") : std::string("Eve"); };
	votes.mapName = [](int) { return std::string("Sidi_Power_Plant"); };
	votes.onLine = [&](const std::string& s) { lines.push_back(s); };
	votes.localPlayer = 1;
	// Eve starts a kick of Bob: the remote start line, again every 5 s
	// until we vote; the count every 30 s.
	votes.handle({2, 0, 2, 3, 0, 4, 90});
	CHECK(lines.size() == 1 && lines[0] == "Eve vs Bob");
	for (int i = 0; i < 30 * 5 + 1; ++i) {
		votes.tick(1.0f / 30);
	}
	CHECK(lines.size() == 2);
	votes.handle({2, 4, 2, 1, 0, 0, 0}); // we say yes
	CHECK(lines.back() == "HUD_HUD_KICKVOTE_VOTE_YES");
	for (int i = 0; i < 30 * 26; ++i) {
		votes.tick(1.0f / 30);
	}
	CHECK(lines.size() == 4 && lines.back() == "2/4 Bob"); // the starter and us
	votes.handle({2, 3, 2, 3, 0, 0, 0});
	CHECK(lines.back() == "HUD_HUD_KICKVOTE_PLAYER_KICKED" && !votes.running());
	// Our own map vote: the local start line; then 300 s before the next.
	votes.handle({1, 0, 5, 1, 0, 3, 120});
	CHECK(lines.back() == "Me Sidi Power Plant 2");
	votes.handle({1, 1, 5, 1, 0, 0, 0});
	CHECK(lines.back() == "HUD_HUD_KICKVOTE_MAP_STAYS");
	CHECK(!votes.mayStart(1, 2, false) && lines.back() == "wait 5");
	CHECK(!votes.mayStart(2, 1, false) && lines.back() == "HUD_HUD_KICKVOTE_CANNOT_KICK_SELF");
	CHECK(!votes.mayStart(2, 3, true) && lines.back() == "HUD_HUD_KICKVOTE_CANNOT_KICK_HOST");
	// A refusal goes to the starter only.
	const size_t before = lines.size();
	votes.handle({2, 6, 2, 3, 0, 0, 0});
	CHECK(lines.size() == before);
	votes.handle({2, 7, 2, 1, 0, 0, 0});
	CHECK(lines.back() == "HUD_HUD_KICKVOTE_UNABLE_TO_START_VOTE");
}

// The commander screen's squad panel (0x007ab790): a squad's order picture
// in its row and the panel, orderNew while it has a request; the squad info
// bar (0x007a0f80).
TEST(HudMenus_CommanderSquadPanelAndInfo)
{
	Squads s;
	menu::CommanderMenu commander(s.world, s.hud);
	const int me = s.world.addPlayer("Me", 2, false);
	const int a = s.world.addPlayer("A", 2, true);
	s.world.joinSquad(a, 4);
	s.world.setCommanderEnabled(true);
	CHECK(s.world.setCommander(2, me));
	CHECK(s.world.issueOrder(2, 4, game::GameWorld::OrderType::MapMove, 10, 10, me));
	commander.update(me);
	commander.click(me, 0, 4);
	commander.update(me);
	CHECK(s.hud.listCell(15, 0, 2) == "Ingame/Commander/Orders/orderMove.tga");
	CHECK(s.hud.string("SquadOrderPicturePathString") == "Ingame/Commander/Orders/orderMove.tga"
		&& s.hud.number("ShowSquadOrderPicture") == 1);
	CHECK(s.hud.string("SquadLeaderKitPicturePathString") == "Ingame/Commander/kits/icon_kit_dead.tga");
	CHECK(s.world.storeSquadRequest(2, 4, 1, {0, 0, 0}));
	commander.update(me);
	CHECK(s.hud.listCell(15, 0, 2) == "Ingame/Commander/Orders/orderNew.tga");
	// The info bar: the commander's, and a squad member's.
	menu::updateSquadInfo(s.hud, s.world, me, false, {});
	CHECK(s.hud.number("ShowCommanderIcon") == 1 && s.hud.string("SquadInfoText") == "HUD_HUD_SQUADINFO_COMMANDER");
	menu::updateSquadInfo(s.hud, s.world, me, true, {});
	CHECK(s.hud.number("SquadInfoBarShow") == 0);
	menu::updateSquadInfo(s.hud, s.world, a, false, {});
	CHECK(s.hud.number("ShowSquadIcon") == 1 && s.hud.string("SquadInfoIconPath") == "Ingame/SquadInfo/icon_squad4.tga"
		&& s.hud.string("SquadInfoText") == "HUD_HUD_SQUADINFO_SQUADLEADER");
}

// The HUD's questions (0x0079e440 / 0x0079d1b0 / 0x0079e320 / 0x0079e170):
// one per kind, the front the one with the least time left, its lines,
// the answer, the timeout's quiet no and line.
TEST(HudQuestions_AskAnswerAndTimeout)
{
	menu::HudQuestions q;
	std::vector<std::string> lines;
	std::vector<std::tuple<int, int, bool, bool>> answers; // kind, value, yes, quiet
	q.localize = [](const std::string& id) { return id; };
	q.onLine = [&lines](const std::string& line) { lines.push_back(line); };
	q.onAnswer = [&answers](const menu::HudQuestions::Question& asked, bool yes, bool quiet) {
		answers.push_back({asked.kind, asked.value, yes, quiet});
	};
	q.ask(menu::HudQuestions::Invite, 3, "invited to Charlie");
	CHECK(lines.size() == 2 && lines[0] == "invited to Charlie" && lines[1] == "HUD_HUD_SQUAD_RECIEVED_ACCEPT_REJECT");
	q.tick(5);
	q.ask(menu::HudQuestions::CommanderOffer, 1, "commander of the PAC");
	// The invite has 15 s left, the offer 20: the invite stays in front.
	CHECK(q.front() && q.front()->kind == menu::HudQuestions::Invite);
	q.answer(true);
	CHECK(answers.size() == 1 && std::get<0>(answers[0]) == menu::HudQuestions::Invite && std::get<1>(answers[0]) == 3
		&& std::get<2>(answers[0]) && !std::get<3>(answers[0]));
	CHECK(q.front() && q.front()->kind == menu::HudQuestions::CommanderOffer && lines.back() == "HUD_HUD_SQUAD_RECIEVED_ACCEPT_REJECT");
	// Unanswered for its 20 s: a quiet no and the timed-out line.
	q.tick(19.9f);
	CHECK(answers.size() == 1);
	q.tick(0.2f);
	CHECK(answers.size() == 2 && std::get<0>(answers[1]) == menu::HudQuestions::CommanderOffer && !std::get<2>(answers[1])
		&& std::get<3>(answers[1]));
	CHECK(lines.back() == "HUD_TEXT_MENU_COMMANDER_APPLICATION_TIMED_OUT" && !q.front());
	q.answer(true); // nothing asked
	CHECK(answers.size() == 2);
}

// The radio's spam check (player slot 0x340, 0x00591d40; the reset in the
// player's tick 0x0059bd80).
TEST(RadioSpam_CountBlockAndReset)
{
	menu::RadioSpam spam; // 6 messages, 6 s, 30 s blocked
	double now = 100;
	for (int i = 0; i < 6; ++i) {
		spam.tick(false, now);
		CHECK(!spam.check(true, false, now));
		now += 0.5;
	}
	spam.tick(false, now);
	CHECK(spam.check(true, false, now)); // the seventh within the interval: refused
	CHECK(spam.secondsLeft(now) == 31);  // the block's clock starts at the refusal
	// Blocked for radioBlockedDurationTime from then.
	now += 29;
	spam.tick(false, now);
	CHECK(spam.check(true, false, now));
	now += 1.5;
	spam.tick(false, now);
	CHECK(!spam.check(true, false, now));
	// Under the limit the count goes after radioSpamInterval; a commander may
	// say twice as many.
	now += 7;
	spam.tick(true, now);
	CHECK(spam.count == 0);
	for (int i = 0; i < 12; ++i) {
		CHECK(!spam.check(true, true, now));
	}
	CHECK(spam.check(true, true, now));
	// Not counted (a spot by the local player): never refused under the limit.
	menu::RadioSpam spots;
	for (int i = 0; i < 20; ++i) {
		CHECK(!spots.check(false, false, now));
	}
}

// The crosshair's parts (0x007be770): the spread by deviationFactor, pushed
// out by minVisualDeviation and held at maxVisualDeviation.
TEST(HudWeapon_CrosshairVisualDeviation)
{
	auto parts = menu::crosshairParts(1.5f, 10, 0, 1000);
	CHECK(parts.offset == 15 && parts.right == 415 && parts.left == 364 && parts.up == 265 && parts.down == 316);
	parts = menu::crosshairParts(1.5f, 10, 3, 1000);
	CHECK(parts.offset == 18 && parts.right == 418 && parts.down == 319);
	parts = menu::crosshairParts(4.0f, 10, 3, 23);
	CHECK(parts.offset == 23 && parts.right == 423 && parts.left == 356 && parts.up == 257 && parts.down == 324);
	// The template's fields, with the constructor's defaults (0x006390e0).
	world::ObjectTemplate rifle(1, "GenericFireArm", "rifle", 1);
	CHECK(menu::crosshairParts(0.21f, &rifle).offset == 3);
	rifle.settings()["weaponHud.deviationFactor"] = "20";
	rifle.settings()["weaponHud.minVisualDeviation"] = "15";
	rifle.settings()["weaponHud.maxVisualDeviation"] = "75";
	CHECK(menu::crosshairParts(1.0f, &rifle).offset == 35);
	CHECK(menu::crosshairParts(9.0f, &rifle).offset == 75);
}

// The Titans' consoles and shields on the HUD (0x0044b680 case 0x32,
// 0x00448850): all shown from the round's start, cleared at the destroyed
// part's guiIndex when it has sendDestroyedHudEvent.
TEST(HudTitan_StrategicObjects)
{
	world::ObjectTemplate console2(1, "PlayerControlObject", "as_titan_console_01", 1);
	console2.settings()["vehicleHud.sendDestroyedHudEvent"] = "1";
	console2.settings()["vehicleHud.guiIndex"] = "2";
	world::ObjectTemplate shield(2, "PlayerControlObject", "as_titan_forcefield", 1);
	shield.settings()["vehicleHud.sendDestroyedHudEvent"] = "1"; // no guiIndex: 0
	world::ObjectTemplate gun(3, "PlayerControlObject", "as_titan_aa", 1);
	gun.settings()["vehicleHud.guiIndex"] = "3"; // no sendDestroyedHudEvent
	menu::StrategicObjects objects;
	hud::HudManager hudManager(nullptr);
	objects.update(hudManager, 1);
	CHECK(hudManager.number("FriendlyStrategicObject0Show") == 1 && hudManager.number("EnemyStrategicObject2Show") == 1);
	objects.destroyed(2, &console2);
	objects.destroyed(2, &gun);
	objects.destroyed(0, &shield);
	objects.update(hudManager, 1);
	CHECK(hudManager.number("EnemyStrategicObject2Show") == 0 && hudManager.number("EnemyStrategicObject3Show") == 1);
	CHECK(hudManager.number("FriendlyStrategicObject2Show") == 1 && hudManager.number("EnemyStrategicObject0Show") == 1);
	objects.destroyed(2, &shield);
	objects.update(hudManager, 2);
	CHECK(hudManager.number("FriendlyStrategicObject0Show") == 0 && hudManager.number("EnemyStrategicObject0Show") == 1);
	objects.reset();
	CHECK(objects.shown(2, 0) && objects.shown(2, 2));
}

// The demo's playback and camera (DemoCamera.h: 0x004b32d0, 0x004b3b90,
// 0x005f9e30, 0x005fa130, 0x005f8f40, 0x00434610).
TEST(HudMenus_DemoPlaybackAndCamera)
{
	dice::hfe::host::DemoPlayback demo;
	double carry = 0;
	CHECK(demo.advance(30, carry) == 30);
	demo.speed = 50;
	CHECK(demo.advance(3, carry) == 1 && demo.advance(1, carry) == 1); // 1.5, then 0.5 + 0.5
	CHECK(demo.togglePause() && demo.speed == 0 && demo.advance(30, carry) == 0);
	demo.ended = true;
	CHECK(!demo.togglePause() && demo.speed == 0); // refused at the end
	demo.restart();
	CHECK(demo.togglePause() && demo.speed == 100);
	CHECK(std::abs(demo.fovY() - 1.1780972f) < 1e-4f); // 90 degrees x 300 / 400
	dice::hfe::host::DemoCamera& camera = demo.camera;
	CHECK(camera.mode == dice::hfe::host::DemoCamera::Mode::Free);
	const auto eligible = [](int id) { return id == 3 || id == 7; };
	camera.toggle(eligible); // from free: the current (none: 0) on
	CHECK(camera.mode == dice::hfe::host::DemoCamera::Mode::Follow && camera.followed == 3);
	camera.step(1, eligible);
	CHECK(camera.followed == 7);
	camera.step(1, eligible); // wraps through 255
	CHECK(camera.followed == 3);
	camera.step(-1, eligible);
	CHECK(camera.followed == 7);
	camera.toggle(eligible);
	CHECK(camera.mode == dice::hfe::host::DemoCamera::Mode::Free);
	CHECK(!camera.step(1, [](int) { return false; }) && camera.mode == dice::hfe::host::DemoCamera::Mode::Free);
	// Following: 2 m behind along the camera's yaw, looking at the target.
	const float at[3] = {10, 5, 20};
	const auto look = camera.follow(at, 0);
	CHECK(std::abs(look.eye[0] - 10) < 1e-4f && std::abs(look.eye[2] - 18) < 1e-4f && std::abs(look.yaw) < 1e-4f);
}

// The commander rose's and squad leader rose's rows (main.cpp's
// Commander.zoomCommander 0x007d7be0, detailedView 0x007d8db0,
// spottedMouseOver 0x00751f80, CommanderMenu.sendRadioMessage 0x007a9f60,
// SquadLeader.sendRequestForOrder 0x007c7cd0, issueSquadRequest 0x007d6820).
TEST(HudMenus_CommanderRoseRows)
{
	CHECK(menu::commanderZoomStep(0) == 1 && menu::commanderZoomStep(1) == 2 && menu::commanderZoomStep(2) == 0);
	game::GameWorld::PlayerInfo me;
	me.position = {5, 0, 7};
	const std::pair<float, float> view {100, 200};
	CHECK(menu::detailedViewPoint(&view, &me) == view);
	CHECK((menu::detailedViewPoint(nullptr, &me) == std::pair<float, float> {5, 7}));
	Squads s;
	menu::spottedMouseOver(s.hud, 3);
	CHECK(s.hud.number("Spotted3MouseOver") == 1 && s.hud.number("Spotted1MouseOver") == 0 && s.hud.number("Spotted11MouseOver") == 0);
	menu::spottedMouseOver(s.hud, 11);
	CHECK(s.hud.number("Spotted3MouseOver") == 0 && s.hud.number("Spotted11MouseOver") == 1);
	// sendRadioMessage: each selected squad, the spam check on the first.
	menu::CommanderMenu commander(s.world, s.hud);
	const int cmd = s.world.addPlayer("Me", 2, false);
	const int a = s.world.addPlayer("A", 2, true);
	const int b = s.world.addPlayer("B", 2, true);
	s.world.joinSquad(a, 1);
	s.world.joinSquad(b, 4);
	s.world.setCommanderEnabled(true);
	CHECK(s.world.setCommander(2, cmd));
	commander.update(cmd);
	bool ctrl = true;
	commander.ctrlHeld = [&ctrl] { return ctrl; };
	commander.click(cmd, 0, 1);
	commander.click(cmd, 1, 4); // squad 4's members shown
	commander.click(cmd, 0, 1);
	CHECK(commander.tabs.squadMembers);
	std::vector<std::pair<int, bool>> said;
	commander.sendRadioMessage([&said](int squad, bool first) { said.push_back({squad, first}); });
	CHECK((said == std::vector<std::pair<int, bool>> {{1, true}, {4, false}}));
	CHECK(!commander.tabs.squadMembers);
	// sendRequestForOrder: req_new_order to the commander, then kind 1.
	std::vector<std::string> voices;
	int kind = -1;
	menu::sendRequestForOrder([&voices](int channel, const std::string& voice) { voices.push_back(std::to_string(channel) + voice); },
		[&kind](int k) { kind = k; });
	CHECK(voices == std::vector<std::string> {"2req_new_order"} && kind == 1);
	// issueSquadRequest: the armed order at the rose's point, nothing else.
	float placedX = -1, placedZ = -1;
	const auto place = [&](float x, float z) { placedX = x, placedZ = z; };
	const auto point = [](float& x, float& z) { return x = 30, z = 40, true; };
	CHECK(!menu::issueSquadRequest(false, point, place) && placedX < 0);
	CHECK(!menu::issueSquadRequest(true, [](float&, float&) { return false; }, place) && placedX < 0);
	CHECK(menu::issueSquadRequest(true, point, place) && placedX == 30 && placedZ == 40);
}

// The squad panel's voice icons (0x007aaf10) and the tactical rose's
// all-squads row (0x007ac8f0 with +0x1e4).
TEST(HudMenus_CommanderVoiceAndAllSquads)
{
	Squads s;
	menu::CommanderMenu commander(s.world, s.hud);
	const int cmd = s.world.addPlayer("Me", 2, false);
	const int a = s.world.addPlayer("A", 2, true);
	const int b = s.world.addPlayer("B", 2, true);
	const int c = s.world.addPlayer("C", 2, true);
	s.world.joinSquad(a, 1);
	s.world.joinSquad(b, 2);
	s.world.joinSquad(c, 3);
	s.world.setCommanderEnabled(true);
	CHECK(s.world.setCommander(2, cmd));
	std::vector<int> tutorials;
	commander.voice = [&](int player) { return player == a ? 1 : player == b ? 2 : 0; };
	commander.onTutorial = [&tutorials](int index) { tutorials.push_back(index); };
	commander.update(cmd);
	CHECK(s.hud.listCell(15, 0, 3) == "Ingame/Commander/voip/voip_icon_blocked.tga");
	CHECK(s.hud.listCell(15, 1, 3) == "Ingame/Commander/voip/voip_icon_transmitting.tga");
	CHECK(s.hud.listCell(15, 2, 3).empty());
	CHECK(tutorials == std::vector<int> {0x26});
	commander.voice = [&](int player) { return player == c ? 3 : player == b ? 2 : 0; };
	commander.update(cmd);
	CHECK(s.hud.listCell(15, 2, 3) == "Ingame/Commander/voip/voip_icon_distorted.tga");
	CHECK((tutorials == std::vector<int> {0x26, 0x26, 0x27}));
	// The all-squads row: last, selected, not a squad to click.
	commander.localize = [](const std::string& id) { return id == "SELECTED_ALL_SQUADS" ? "All squads" : id; };
	commander.allSquads = true;
	commander.update(cmd);
	CHECK(s.hud.listCell(15, 3, 1) == "|c All squads");
	commander.click(cmd, 3, 1);
	CHECK(commander.selected().empty());
	commander.allSquads = false;
	commander.update(cmd);
	CHECK(s.hud.listCell(15, 3, 1).empty());
}

namespace {

// A world for the HUD with the players given (a joined game's, as
// NametagsTest's).
struct RadarWorld : host::HudWorld {
	std::vector<PlayerInfo> players;
	std::vector<uint32_t> vehicles;
	bool joined() const override { return true; }
	int localPlayer() const override { return 0; }
	bool playerInfo(int player, PlayerInfo& out) const override
	{
		for (const PlayerInfo& p : players) {
			if (p.id == player) {
				out = p;
				return true;
			}
		}
		return false;
	}
	std::vector<PlayerInfo> playerList() const override { return players; }
	Scores scores(int) const override { return {}; }
	float tickets(int) const override { return 0; }
	int commander(int) const override { return -1; }
	std::vector<ControlPointInfo> controlPointList() const override { return {}; }
	std::vector<int> spawnablePoints(int) const override { return {}; }
	int spawnChoice(int) const override { return -1; }
	std::vector<VehicleInfo> vehicleList() const override { return {}; }
	std::vector<int> vehicleOccupants(uint32_t) const override { return {}; }
	float hitPoints(uint32_t) const override { return 100; }
	float objectHealth(uint32_t) const override { return 1; }
	bool squadOrder(int, int, bool, SquadOrder&) const override { return false; }
	std::vector<SquadRequest> squadRequests(int) const override { return {}; }
	const world::Object* soldier() const override { return nullptr; }
	const world::Object* vehicle() const override { return nullptr; }
	float health() const override { return 1; }
	Weapon heldWeapon() const override { return {}; }
	Weapon seatGroupWeapon(int) const override { return {}; }
	std::vector<uint32_t> spottedVehicles() const override { return vehicles; }
};

} // namespace

// The squad radar's markers (menu::radarMarkers): a decoy within 60 m of an
// enemy on foot shows instead of him (0x00508590), one in a vehicle stays
// where he is; the reports' vehicles by their miniMapIcon (0x00790d60).
TEST(HudRadar_DecoyAndVehicleMarkers)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	game::GameWorld round(templates, objects);
	templates.registerType(1, "TargetObject");
	templates.registerType(2, "PlayerControlObject");
	world::ObjectTemplate* decoyTemplate = templates.create("TargetObject", "Bp1_Decoy");
	decoyTemplate->settings()["targetType"] = "TTDecoy";
	world::ObjectTemplate* tank = templates.create("PlayerControlObject", "tank");
	tank->settings()["vehicleHud.miniMapIcon"] = "Ingame/mini_tank.tga";
	objects.create(decoyTemplate, {40, 0, 10});
	const uint32_t car = objects.create(tank, {100, 0, 100})->id();
	RadarWorld world;
	game::GameWorld::PlayerInfo onFoot;
	onFoot.id = 5;
	onFoot.alive = true;
	onFoot.position = {10, 0, 10};
	game::GameWorld::PlayerInfo riding = onFoot;
	riding.id = 6;
	riding.vehicle = car;
	riding.position = {20, 0, 10};
	game::GameWorld::PlayerInfo far = onFoot;
	far.id = 7;
	far.position = {-200, 0, 10};
	world.players = {onFoot, riding, far};
	auto markers = menu::radarMarkers(world, round, {5, 6, 7});
	CHECK(markers.size() == 3);
	if (markers.size() == 3) {
		CHECK(markers[0].x == 40 && markers[0].z == 10); // at the decoy
		CHECK(markers[1].x == 20 && markers[1].z == 10); // in a vehicle: his own place
		CHECK(markers[2].x == -200);                     // too far from it
		CHECK(markers[0].texture == "Ingame/Player/Icons/Minimap/mini_Soldier.dds" && markers[0].size == 16);
	}
	// Without a decoy template's targetType the enemy is where he is.
	decoyTemplate->settings()["targetType"] = "TTHeat";
	markers = menu::radarMarkers(world, round, {5});
	CHECK(markers.size() == 1 && markers[0].x == 10);
	// The reports' vehicle.
	world.vehicles = {car};
	markers = menu::radarMarkers(world, round, {});
	CHECK(markers.size() == 1 && markers[0].texture == "Ingame/mini_tank.tga" && markers[0].x == 100 && markers[0].size == 16);
}

// The vehicle HUD's torque arrows and sprint (0x007a2e60): the torque eased
// a hundredth a frame (its source +0x2f4 has no writer: 0), the stamina
// bar while sprinting or below 1 and its text for 3 s after it fell.
TEST(HudVehicle_TorqueAndStamina)
{
	hud::HudManager hud(nullptr);
	menu::VehicleHud vehicleHud;
	menu::VehicleHudInput in;
	in.seconds = 0.5f;
	vehicleHud.update(hud, in);
	CHECK(hud.number("TorqueAngle") == 0 && hud.string("TorqueString") == "0");
	CHECK(std::fabs(hud.number("SecondTorqueAngle") - 6.2831855f) < 1e-4f);
	CHECK(hud.number("VehicleStaminaShow") == 0 && hud.number("VehicleStamina") == 1);
	in.torque = 1;
	vehicleHud.update(hud, in);
	CHECK(std::fabs(hud.number("Torque") - 0.01f) < 1e-5f && hud.string("TorqueString") == "1");
	in.sprinting = true;
	in.stamina = 0.8f;
	vehicleHud.update(hud, in);
	CHECK(hud.number("VehicleStaminaShow") == 1 && hud.number("VehicleStaminaTextStringShow") == 1);
	in.sprinting = false;
	for (int i = 0; i < 6; ++i) {
		vehicleHud.update(hud, in);
	}
	CHECK(hud.number("VehicleStaminaShow") == 1 && hud.number("VehicleStaminaTextStringShow") == 0);
	in.stamina = 1;
	vehicleHud.update(hud, in);
	CHECK(hud.number("VehicleStaminaShow") == 0);
}

// The item list in a seat (0x0079c470): the PIFire group by item index
// (dimmed when not selectable); with useSelectionIcons a longer PIAltFire
// group instead; without a PIFire group the soldier's own.
TEST(HudItemSelection_SeatLists)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "GenericFireArm");
	templates.registerType(2, "PlayerControlObject");
	world::ObjectTemplate* gun = templates.create("GenericFireArm", "gun");
	gun->settings()["weaponHud.selectIcon"] = "gun.tga";
	world::ObjectTemplate* seatTemplate = templates.create("PlayerControlObject", "seat");
	const auto find = [&templates](const std::string& name) -> const world::ObjectTemplate* { return templates.find(name); };
	game::GameWorld::SeatLayout seat;
	game::GameWorld::SeatWeapon w;
	w.weapon = "gun";
	w.group = 0;
	w.itemIndex = 1;
	seat.weapons.push_back(w);
	w.group = 1;
	w.itemIndex = 2;
	seat.weapons.push_back(w);
	w.itemIndex = 3;
	w.selectable = false;
	seat.weapons.push_back(w);
	auto items = menu::seatSelectItems(seat, seatTemplate, find, {});
	CHECK(items.size() == 1 && items[0].index == 1 && items[0].icon == "gun.tga" && items[0].usable);
	seatTemplate->settings()["vehicleHud.useSelectionIcons"] = "1";
	items = menu::seatSelectItems(seat, seatTemplate, find, {});
	CHECK(items.size() == 2 && items[0].index == 2 && items[1].index == 3 && !items[1].usable);
	seat.weapons.erase(seat.weapons.begin());
	seat.weapons[0].group = seat.weapons[1].group = 2;
	items = menu::seatSelectItems(seat, seatTemplate, find, {{3, "gun"}, {1, "knife"}});
	CHECK(items.size() == 2 && items[0].index == 3 && items[0].icon == "gun.tga" && items[1].index == 1);
}

// The mouse wheel's item list (0x00755d50, inputs 0x30 / 0x31): a step
// opens it for 2 s on the item in hand and moves (0x0079c1e0: the next if
// usable, else the first; 0x0079c230: the previous if usable, else the
// last, else the first), "weaponOver" each move; fire with it open is the
// pick (the selected item's PIWeaponSelect key, fire cleared) and fire
// stays cleared 0.3 s; not where the list may not open (a zoomed weapon).
TEST(HudItemSelection_MouseWheel)
{
	menu::ItemSelection list;
	menu::ItemListInput pass;
	pass.allowed = true;
	pass.items = {{1, "knife", true}, {2, "pistol", true}, {3, "rifle", true}, {4, "grenade", false}, {5, "medkit", true}};
	pass.inHand = 3;
	bool fire = false;
	int item = 0;
	// Opened on the rifle (3); the next, 4, isn't usable: the first.
	CHECK(pass.apply(list, fire, item, 1, 10.0) == "weaponOver");
	CHECK(list.shown() && list.selectedItem() == 1);
	CHECK(list.next() && list.selectedItem() == 2);
	CHECK(list.previous() && list.selectedItem() == 1);
	CHECK(list.previous() && list.selectedItem() == 5); // from the first: the last
	CHECK(list.previous() && list.selectedItem() == 5); // the previous (4) unusable: the last again
	CHECK(list.next() && list.selectedItem() == 1);     // past the end: the first
	CHECK(list.next() && list.selectedItem() == 2);
	// The pick: fire takes the selected item's key and is cleared.
	fire = true;
	item = 0;
	CHECK(pass.apply(list, fire, item, 0, 10.5).empty());
	CHECK(!fire && item == list.selectedItem());
	// Closed again: fire stays cleared until 0.3 s after the pick.
	hud::HudManager hud(nullptr);
	list.update(hud, 5.0f);
	CHECK(!list.shown());
	fire = true;
	item = 0;
	pass.apply(list, fire, item, 0, 10.7);
	CHECK(!fire && item == 0);
	fire = true;
	pass.apply(list, fire, item, 0, 10.9);
	CHECK(fire);
	// Not allowed (zoomed): the wheel leaves the list shut.
	pass.allowed = false;
	fire = false;
	CHECK(pass.apply(list, fire, item, -1, 11.0).empty());
	CHECK(!list.shown());
	// A step down from the item in hand: the previous usable one.
	pass.allowed = true;
	pass.inHand = 3;
	CHECK(pass.apply(list, fire, item, -1, 12.0) == "weaponOver");
	CHECK(list.shown() && list.selectedItem() == 2);
}

// minimumSquadSizeForSelection (0x005c3080, the firearm's slot 0x188): too
// few in the squad refuses the weapon (one fewer for a bot), never without
// a squad; the key's refusal goes to the HUD (message 0x52) once.
TEST(HudItemSelection_SquadSizeRefusal)
{
	Squads s;
	world::ObjectTemplate beacon(1, "GenericFireArm", "beacon", 1);
	beacon.settings()["minimumSquadSizeForSelection"] = "2";
	const int me = s.world.addPlayer("Me", 1, false);
	const int bot = s.world.addPlayer("Bot", 1, true);
	CHECK(!s.world.squadTooSmall(me, beacon)); // no squad
	s.world.joinSquad(me, 1);
	CHECK(s.world.squadTooSmall(me, beacon));
	s.world.joinSquad(bot, 2);
	CHECK(!s.world.squadTooSmall(bot, beacon)); // a bot needs one fewer
	s.world.joinSquad(bot, 1);
	CHECK(!s.world.squadTooSmall(me, beacon));
	CHECK(s.world.takeSquadSizeRefused(me) == 0);
}

// The other seats' turrets (0x007a2b50 → 0x007a07c0): one other seat's
// bundle gives GunnerAngle -r; a second makes the first r - 1.5708 and the
// second SecondaryGunnerAngle -(r + 1.5708); showOneSecondaryGunnerAngle
// stops at the first other seat; leaving forgets the bundles.
TEST(HudVehicle_GunnerAngles)
{
	hud::HudManager hud(nullptr);
	Console console;
	std::string out;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "PlayerControlObject");
	world::registerWorldConsoleMethods(console, templates, objects);
	world::registerUntypedTemplateProperties(console, templates, {"vehicleHud.showOneSecondaryGunnerAngle"});
	console.execute("ObjectTemplate.create PlayerControlObject apc", out);
	console.execute("ObjectTemplate.vehicleHud.showOneSecondaryGunnerAngle 1", out);
	console.execute("ObjectTemplate.create PlayerControlObject tank", out);
	menu::VehicleHud vehicleHud;
	menu::VehicleHudInput in;
	in.seat = templates.find("tank");
	in.others = {{true, 1, 30}};
	vehicleHud.update(hud, in);
	CHECK(hud.number("GunnerAngle") == -30 && hud.number("SecondaryGunnerAngle") == 0);
	in.others = {{true, 1, 30}, {false, 2, 0}, {true, 3, 200}};
	vehicleHud.update(hud, in);
	// The first seat was met before the second this frame: still -r.
	CHECK(hud.number("GunnerAngle") == -30);
	CHECK(hud.number("SecondaryGunnerAngle") == -180); // clamped
	in.others = {{true, 1, 10}, {true, 3, -20}};
	vehicleHud.update(hud, in);
	CHECK(std::fabs(hud.number("GunnerAngle") - (10 - 1.5707964f)) < 1e-4f);
	CHECK(std::fabs(hud.number("SecondaryGunnerAngle") - (20 - 1.5707964f)) < 1e-4f);
	// The APC driver's: only the first other seat, even without a bundle.
	vehicleHud.clear(hud);
	in.seat = templates.find("apc");
	in.others = {{false, 1, 0}, {true, 2, 45}};
	vehicleHud.update(hud, in);
	CHECK(std::fabs(hud.number("GunnerAngle") - (10 - 1.5707964f)) < 1e-4f); // kept
	in.others = {{true, 1, 45}, {true, 2, 90}};
	vehicleHud.update(hud, in);
	CHECK(hud.number("GunnerAngle") == -45);
}

// The blinks (0x00778040 / 0x00778080): below 0.3 the flag toggles once
// the timer passes (0.55 / 0.3) x value + 0.15 s; at 0 it stays off; above
// it a lit flag goes out after the period.
TEST(HudBlink_StaminaAndArmour)
{
	menu::HudBlink blink;
	CHECK(!blink.tick(0.5f, 1.0f));
	// 0.1: a period of 0.3333 s; the timer already ran past it above.
	CHECK(blink.tick(0.1f, 0.0f));
	CHECK(blink.tick(0.1f, 0.2f));
	CHECK(!blink.tick(0.1f, 0.2f));
	CHECK(!blink.tick(0.1f, 0.2f));
	CHECK(blink.tick(0.1f, 0.2f));
	// Empty: held off.
	CHECK(!blink.tick(0.0f, 1.0f));
	// Lit, then back above: off once the period (1.8 s at 0.9) passes.
	CHECK(blink.tick(0.1f, 1.0f));
	CHECK(blink.tick(0.9f, 1.0f));
	CHECK(!blink.tick(0.9f, 1.0f));
	// The vehicle HUD's armour and stamina blinks.
	hud::HudManager hud(nullptr);
	menu::VehicleHud vehicleHud;
	menu::VehicleHudInput in;
	in.armor = 0.1f;
	in.stamina = 0.2f;
	in.seconds = 1.0f;
	vehicleHud.update(hud, in);
	CHECK(hud.number("VehicleArmorBlink") == 1 && hud.number("VehicleStaminaBlink") == 1);
	vehicleHud.clear(hud);
	CHECK(hud.number("VehicleArmorBlink") == 0 && hud.number("VehicleStaminaBlink") == 0);
}

// The weapon HUD's blinks (0x007bee10's eight, ticked by 0x007c40c0 after
// the update): the ammunition below 0.3 of a magazine, the spare
// magazines below 1.5 (0.7 for the Secondary block), the heat above 0.7
// (0x007782f0 / 0x00778330: off again at 1), TVBlink every 0.7 s.
TEST(HudBlink_WeaponHud)
{
	// The heat's twin: above 0.7 it toggles; at 1 it is held off.
	menu::HudBlink heat(0.7f, 1, 0.3f, 0.15f, true, menu::HudBlink::Watch::above);
	CHECK(!heat.tick(0.5f, 1.0f));
	CHECK(heat.tick(0.8f, 0.0f)); // the timer already past (0.15 / 0.7 x 0.8 + 0.15)
	CHECK(!heat.tick(0.8f, 0.4f));
	CHECK(heat.tick(0.8f, 0.4f));
	CHECK(!heat.tick(1.0f, 1.0f));
	// No watched value: every `slow` seconds.
	menu::HudBlink tv(0, 0, 0.7f, 0.7f, false);
	CHECK(!tv.tickFree(0.5f));
	CHECK(tv.tickFree(0.3f));
	CHECK(tv.tickFree(0.5f));
	CHECK(!tv.tickFree(0.3f));

	hud::HudManager hud(nullptr);
	menu::WeaponHud weaponHud;
	menu::WeaponHudInput in;
	in.rounds = 3;
	in.magSize = 30;
	in.mags = 2; // one spare
	in.nrOfMags = 5;
	in.heat = 0.8f;
	in.seconds = 1.0f;
	weaponHud.update(hud, in);
	CHECK(hud.number("PrimaryAmmoBlink") == 1 && hud.number("PrimaryClipBlink") == 1);
	CHECK(hud.number("PrimaryHeatBlink") == 1 && hud.number("TVBlink") == 1);
	// A reload keeps the share the blink watches (+0xb4 isn't written).
	in.reload = 0.5f;
	in.rounds = 30;
	weaponHud.update(hud, in);
	CHECK(hud.number("PrimaryAmmoBlink") == 0);
	weaponHud.update(hud, in);
	CHECK(hud.number("PrimaryAmmoBlink") == 1);
	// No magazine: the blink cleared and the share 1.
	in.reload = 0;
	in.magSize = -1;
	weaponHud.update(hud, in);
	CHECK(hud.number("PrimaryAmmoBlink") == 0 && hud.number("PrimaryClipBlink") == 0);
	// Hidden: the ammunition's and clips' flags off.
	in.magSize = 30;
	in.rounds = 3;
	weaponHud.update(hud, in);
	CHECK(hud.number("PrimaryAmmoBlink") == 1);
	weaponHud.hide(hud);
	CHECK(hud.number("PrimaryAmmoBlink") == 0 && hud.number("PrimaryAmmo") == 0);
	// The Secondary block: one spare magazine is above its 0.7.
	menu::WeaponHud second;
	in.mags = 2;
	second.update(hud, in, true);
	CHECK(hud.number("SecondaryClipBlink") == 0 && hud.number("SecondaryAmmoBlink") == 1);
}

// displaySelectionOnEnter's gate (0x007a3920 on the vehicle HUD's +0xb4,
// written by the seat dots 0x007a28e0): the last occupied seat's
// controlled object a Parachute that isn't a pod holds the list back.
TEST(HudVehicle_SelectionOnEnterGate)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "PlayerControlObject");
	templates.registerType(2, "Parachute");
	world::ObjectTemplate* gun = templates.create("PlayerControlObject", "us_ag");
	gun->settings()["vehicleHud.displaySelectionOnEnter"] = "1";
	world::ObjectTemplate* chute = templates.create("Parachute", "parachute");
	chute->settings()["vehicleHud.displaySelectionOnEnter"] = "1";
	world::ObjectTemplate* pod = templates.create("Parachute", "pod");
	pod->settings()["vehicleHud.displaySelectionOnEnter"] = "1";
	pod->settings()["isPod"] = "1";
	world::ObjectTemplate* tank = templates.create("PlayerControlObject", "tank");
	CHECK(menu::displaySelectionOnEnter(gun, gun));
	CHECK(menu::displaySelectionOnEnter(gun, nullptr));
	CHECK(!menu::displaySelectionOnEnter(chute, chute));
	CHECK(menu::displaySelectionOnEnter(pod, pod));
	CHECK(!menu::displaySelectionOnEnter(tank, tank));
}

// zoomOnMinimap, miniMapIconOffset(Inside), standardHelpEnabled and
// useVehicleCommRose as the shipped templates give them.
TEST(HudVehicle_MapOffsetZoomAndFlags)
{
	Console console;
	std::string out;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "PlayerControlObject");
	world::registerWorldConsoleMethods(console, templates, objects);
	world::registerUntypedTemplateProperties(console, templates,
		{"vehicleHud.miniMapIcon", "vehicleHud.miniMapIconInside", "vehicleHud.miniMapIconOffset",
			"vehicleHud.miniMapIconOffsetInside", "vehicleHud.zoomOnMinimap", "vehicleHud.standardHelpEnabled",
			"vehicleHud.useVehicleCommRose"});
	console.execute("ObjectTemplate.create PlayerControlObject as_titan_wake", out);
	console.execute("ObjectTemplate.vehicleHud.miniMapIcon Ingame\\mini_titan.dds", out);
	console.execute("ObjectTemplate.vehicleHud.miniMapIconInside Ingame\\mini_titanInside.dds", out);
	console.execute("ObjectTemplate.vehicleHud.miniMapIconOffset 0/8", out);
	console.execute("ObjectTemplate.vehicleHud.miniMapIconOffsetInside 0.94/11.2", out);
	console.execute("ObjectTemplate.vehicleHud.zoomOnMinimap 1", out);
	console.execute("ObjectTemplate.create PlayerControlObject static_aa", out);
	console.execute("ObjectTemplate.vehicleHud.standardHelpEnabled 0", out);
	console.execute("ObjectTemplate.vehicleHud.useVehicleCommRose 1", out);
	console.execute("ObjectTemplate.create PlayerControlObject parachute", out);
	console.execute("ObjectTemplate.vehicleHud.useVehicleCommRose 0", out);
	const world::ObjectTemplate* titan = templates.find("as_titan_wake");
	CHECK(titan);
	menu::VehicleMarker marker;
	CHECK(menu::vehicleMarker(*titan, menu::MapRelation::Friendly, false, false, marker));
	CHECK(marker.offsetX == 0 && marker.offsetY == 8);
	CHECK(menu::vehicleMarker(*titan, menu::MapRelation::Own, true, false, marker));
	CHECK(std::fabs(marker.offsetX - 0.94f) < 1e-5f && std::fabs(marker.offsetY - 11.2f) < 1e-5f);
	const menu::MapIcons& icons = menu::mapIcons(*titan);
	CHECK(menu::hiddenOnZoomedMap(icons, 2, true));
	CHECK(!menu::hiddenOnZoomedMap(icons, 1, true) && !menu::hiddenOnZoomedMap(icons, 2, false));
	CHECK(!menu::hiddenOnZoomedMap(menu::mapIcons(*templates.find("static_aa")), 2, true));
	CHECK(!menu::standardHelpEnabled(templates.find("static_aa")));
	CHECK(menu::standardHelpEnabled(titan) && menu::standardHelpEnabled(nullptr));
	CHECK(menu::usesVehicleCommRose(templates.find("static_aa")) && menu::usesVehicleCommRose(titan));
	CHECK(!menu::usesVehicleCommRose(templates.find("parachute")));
}
