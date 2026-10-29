// The HUD's squad menu (SquadMenu, 0x007a4000-0x007a8800) and the
// commander screen's tabs, on a world of players without a level, and the
// HUD's list clicks they read.
#include "Console/Console.h"
#include "Game/GameWorld.h"
#include "Hud/Hud.h"
#include "Main/HudMenus.h"
#include "Main/HudMenusComm.h"
#include "Main/HudMenusVehicles.h"
#include "Console/ConsoleBinding.h"
#include "IO/File/FileManager.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <cstdlib>
#include <set>
#include <string>
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
