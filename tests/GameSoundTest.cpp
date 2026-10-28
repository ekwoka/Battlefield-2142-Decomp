// The game's side of sound without an audio device: the sound manager's
// muting fades (0x007f9f50), the "Sound" console's game sounds and ambient
// triggers, the weapons' sounds (DefaultSoundComp), the objects' loops,
// the voice-message table and its radio / local / help playing
// (0x004d8b90, 0x004d7950, 0x004d8110, 0x0042e0c0), and what GameAudio
// hands the sound layer from a round. With $BF2142_GAME_DIR, also the
// shipped Sound/VoiceMessages.con.
#include "Console/Console.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Main/GameAudio.h"
#include "Physics/CollisionTemplateManager.h"
#include "Sound/Game/GameSounds.h"
#include "Sound/Game/SoundConsole.h"
#include "Sound/Game/SoundManager.h"
#include "Sound/Game/SoundObjectTemplate.h"
#include "Sound/Game/VoiceMessages.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

// A Sound template with a file (the trigger log needs one to play).
void addSoundTemplate(world::ObjectTemplateManager& templates, const std::string& name, const std::string& file = "x.wav")
{
	auto* sound = dynamic_cast<sound::SoundObjectTemplate*>(templates.create(sound::SoundObjectTemplate::kClassId, name));
	CHECK(sound != nullptr);
	if (sound) {
		sound->setSoundFilenames(file);
	}
}

struct Sounds {
	world::ObjectTemplateManager templates;
	sound::SoundManager manager {nullptr, templates};
	sound::GameSounds game {manager};
	std::vector<sound::GameSounds::Triggered> triggered;
	Sounds()
	{
		manager.registerTemplateType();
		game.onTrigger = [this](const sound::GameSounds::Triggered& t) { triggered.push_back(t); };
	}
	bool played(const std::string& name) const
	{
		for (const auto& t : triggered) {
			if (t.name == name) {
				return true;
			}
		}
		return false;
	}
};

} // namespace

// 0x007f9af0 / 0x007f9f50: firing (reason 4) brings the world's effects
// (group 0) to 0.4 in steps of 0.1667 and back up in steps of 0.333; a
// voice-over (reason 3, asked for each update) brings groups 0 and 4 to
// 0.125 in steps of 1/15; first-person weapons (group 4) don't duck under
// their own fire.
TEST(SoundManager_MutingFades)
{
	world::ObjectTemplateManager templates;
	sound::SoundManager manager(nullptr, templates);
	manager.setMuted(4, true);
	manager.update(1);
	CHECK(near(manager.duckedVolume(0), 1 - 0.1667f) && manager.duckedVolume(4) == 1);
	for (int i = 0; i < 10; ++i) {
		manager.update(1);
	}
	CHECK(near(manager.duckedVolume(0), 0.4f) && manager.duckedVolume(4) == 1 && manager.duckedVolume(1) == 1);
	manager.setMuted(4, false);
	manager.update(1);
	CHECK(near(manager.duckedVolume(0), 0.733f));
	manager.update(1);
	CHECK(near(manager.duckedVolume(0), 1));
	for (int i = 0; i < 20; ++i) {
		manager.setMuted(3, true);
		manager.update(1);
	}
	CHECK(near(manager.duckedVolume(0), 0.125f) && near(manager.duckedVolume(4), 0.125f) && manager.duckedVolume(2) == 1);
	manager.update(1); // not asked again: back up
	CHECK(manager.duckedVolume(0) > 0.125f && !manager.muted(3));
}

// Sound/GameSounds.con: "Sound.addSound <name>" makes S_Game_<name> the
// active template (0x007fae20), so its ObjectTemplate.* lines set it up;
// Sound.addTrigger takes a level's ambient sound (AmbientObjects.con).
TEST(SoundConsole_AddSoundMakesTheTemplate)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::registerWorldConsoleMethods(console, templates, objects);
	sound::SoundManager manager(nullptr, templates);
	manager.registerTemplateType();
	sound::registerSoundConsoleMethods(console, manager);
	std::string out;
	CHECK(console.execute("Sound.addSound order_smoke", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(console.execute("ObjectTemplate.soundFilename \"sound/x.wav\"", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(console.execute("ObjectTemplate.minDistance 4", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	const auto* sound = dynamic_cast<const sound::SoundObjectTemplate*>(templates.find("S_Game_order_smoke"));
	CHECK(sound && sound->minDistance() == 4 && manager.hasSound("S_Game_order_smoke"));
	CHECK(console.execute("Sound.addTrigger S_Game_order_smoke", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	// CountdownSoundComp's countdown times are kept on the weapon.
	templates.registerType(9, "GenericFireArm");
	templates.create("GenericFireArm", "silo_gun");
	CHECK(console.execute("ObjectTemplate.sound.addReloadTimeSound 2 12", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(templates.find("silo_gun")->settings().at("sound.reloadTimeSound.2") == "12");
}

// DefaultSoundComp (0x005dbdc0, 0x005dbfe0, 0x005dc1d0): the shooter's own
// view plays the 1P sounds (Fire1P_Outdoor in the open), anyone else's the
// 3P ones where it happens; muteOnFire ducks until the firing stops; a
// deploy isn't repeated within 9 ticks; the generated sounds without
// files stay silent.
TEST(GameSounds_WeaponSounds)
{
	Sounds s;
	for (const char* event : {"Fire1P", "Fire1P_Outdoor", "Fire3P", "Reload1P", "Reload3P", "Deploy3P"}) {
		addSoundTemplate(s.templates, std::string("S_rifle_") + event);
	}
	using E = sound::GameSounds::WeaponEvent;
	s.game.weapon(E::Fire, "rifle", true, {1, 2, 3});
	CHECK(s.triggered.size() == 1 && s.triggered[0].name == "S_rifle_Fire1P_Outdoor" && s.triggered[0].firstPerson);
	CHECK(s.manager.muted(4));
	s.game.weapon(E::Fire, "rifle", false, {5, 0, 5});
	CHECK(s.triggered.size() == 2 && s.triggered[1].name == "S_rifle_Fire3P" && !s.triggered[1].firstPerson
		&& s.triggered[1].position.x == 5);
	for (int i = 0; i < 3; ++i) {
		s.game.endTick();
	}
	CHECK(!s.manager.muted(4));
	s.game.weapon(E::Fire, "rifle", true, {}, false); // sound.muteOnFire 0
	CHECK(!s.manager.muted(4));
	s.game.weapon(E::Reload, "rifle", false, {});
	CHECK(s.triggered.back().name == "S_rifle_Reload3P");
	s.game.weapon(E::Deploy, "rifle", false, {});
	s.game.endTick();
	s.game.weapon(E::Deploy, "rifle", false, {});
	CHECK(s.triggered.back().name == "S_rifle_Deploy3P" && s.triggered.size() == 5);
	// Deploy1P has no file: nothing.
	const size_t before = s.triggered.size();
	for (int i = 0; i < 12; ++i) {
		s.game.endTick();
	}
	s.game.weapon(E::Deploy, "rifle", true, {});
	CHECK(s.triggered.size() == before);
	// With a roof over the listener the plain Fire1P.
	s.manager.setRayTest([](const sound::Vec3& from, const sound::Vec3& to, float& distance) {
		distance = std::fabs(to.y - from.y) * 0.2f;
		return true;
	});
	s.game.weapon(E::Fire, "rifle", true, {});
	CHECK(s.triggered.back().name == "S_rifle_Fire1P");
}

// 0x007fa100: two rays up and one down, 15 m; a 6 m room gives
// 0.5 x 6 / (0.98 x 6 + 0.3).
TEST(SoundManager_IndoorLevel)
{
	world::ObjectTemplateManager templates;
	sound::SoundManager manager(nullptr, templates);
	CHECK(manager.indoorLevel({0, 0, 0}) == 0 && manager.outdoors({0, 0, 0}));
	manager.setRayTest([](const sound::Vec3& from, const sound::Vec3& to, float& distance) {
		distance = to.y > from.y ? 4.0f : 2.0f; // ceiling 4 m up, floor 2 m down
		return true;
	});
	CHECK(near(manager.indoorLevel({0, 0, 0}), 0.5f * 6 / (0.98f * 6 + 0.02f * 15)));
	manager.setRayTest([](const sound::Vec3& from, const sound::Vec3& to, float& distance) {
		distance = 1;
		return to.y < from.y; // no ceiling
	});
	CHECK(manager.outdoors({0, 0, 0}));
}

// Loops are kept while their owner refreshes them and dropped the tick it
// doesn't.
TEST(GameSounds_LoopsFollowTheirOwners)
{
	Sounds s;
	addSoundTemplate(s.templates, "S_tank_Engine_Idle");
	s.game.loop(7, "S_tank_Engine_Idle", {1, 0, 0}, {}, 0.5f);
	s.game.loop(7, "S_nothing", {1, 0, 0}, {}, 0.5f);
	CHECK(s.game.loops() == 1 && s.triggered.size() == 1);
	s.game.endTick();
	s.game.loop(7, "S_tank_Engine_Idle", {2, 0, 0}, {}, 0.6f);
	CHECK(s.game.loops() == 1 && s.triggered.size() == 1); // started once
	s.game.endTick();
	s.game.endTick();
	CHECK(s.game.loops() == 0);
}

// The message table (0x004d8af0, 0x004d8b90, 0x004d8de0, 0x004d8e40): each
// message's samples per voice together, the team prefixes from their
// languages, a voice's samples out of order rejected.
TEST(VoiceMessages_Table)
{
	world::ObjectTemplateManager templates;
	sound::SoundManager manager(nullptr, templates);
	sound::VoiceMessages voices(manager);
	Console console;
	sound::registerVoiceMessageConsoleMethods(console, voices);
	voices.reset("Russian", "English");
	CHECK(voices.teamPrefix(1) == "sound/vo/Russian/" && voices.teamPrefix(2) == "sound/vo/English/");
	std::string out;
	for (const char* line : {"gamelogic.messages.setVolumes 0.72 0.85",
			 "gamelogic.messages.addRadioFilter \"commander\" \"Sound/VO/Activate_radio.wav\"",
			 "gamelogic.messages.addMessage \"roger_that\"",
			 "gamelogic.messages.addRadioVoice \"grunt\" \"grunt/filter/a.ogg\" A \"grunt/No_filter/a.ogg\" 1",
			 "gamelogic.messages.addRadioVoice \"grunt\" \"grunt/filter/b.ogg\" B \"grunt/No_filter/b.ogg\" 1",
			 "gamelogic.messages.addRadioVoice \"commander\" \"commander/filter/c.ogg\" C",
			 "gamelogic.messages.addEmoteTrigger \"affirmative\"",
			 "gamelogic.messages.addMessage \"AUTO_RULES_welostacp\"",
			 "gamelogic.messages.addRadioVoice \"commander\" \"commander/filter/d.ogg\" \"\"",
			 "gamelogic.messages.addHelpMessage \"HUD_HELP_x\" \"Help_VO/x.ogg\""}) {
		CHECK(console.execute(line, out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	}
	CHECK(voices.messageCount() == 2 && voices.message("roger_that") == 0 && voices.voice("commander") == 1);
	CHECK(near(voices.voiceOverVolume(), 0.72f) && near(voices.filterVolume(), 0.85f));
	CHECK(voices.samples(0, voices.voice("grunt")) == std::make_pair(0, 2));
	CHECK(voices.samples(0, voices.voice("commander")) == std::make_pair(2, 1));
	CHECK(voices.samples(1, voices.voice("grunt")).second == 0 && voices.pickSample(1, voices.voice("grunt")) == -1);
	const int pick = voices.pickSample(0, voices.voice("grunt"));
	CHECK(pick == 0 || pick == 1);
	CHECK(voices.radioFile(1, 1) == "sound/vo/Russian/grunt/filter/b.ogg"
		&& voices.localFile(2, 0) == "sound/vo/English/grunt/No_filter/a.ogg" && voices.localFile(2, 2).empty());
	CHECK(voices.text(2) == "C" && voices.emote(0) == "affirmative" && voices.emote(1).empty());
	// The grunt's samples of message 0 were left behind: no more of them.
	CHECK(voices.addRadioVoice("grunt", "grunt/filter/z.ogg", "Z") == 4); // message 1's own grunt run
	CHECK(voices.addRadioVoice("commander", "late.ogg", "L") == -1);      // not after the grunt's
	// A player's language other than English: the team speaking it, else
	// the English one, takes it.
	voices.reset("Russian", "English", "German");
	CHECK(voices.teamPrefix(1) == "sound/vo/Russian/" && voices.teamPrefix(2) == "sound/vo/German/");
}

// 0x0042e0c0: a teammate on foot within 15 m speaks it in person; farther
// it comes over the radio of his team's voices; an enemy's only in person;
// a message from no one is the commander's over the commander filter;
// 0x004d8280: the help voice.
TEST(VoiceMessages_HeardAsTheClientPlaysThem)
{
	world::ObjectTemplateManager templates;
	sound::SoundManager manager(nullptr, templates);
	sound::VoiceMessages voices(manager);
	voices.reset("Russian", "English");
	voices.addRadioFilter("commander", "Sound/VO/Activate_radio.wav");
	voices.addMessage("roger_that");
	voices.addRadioVoice("grunt", "grunt/filter/a.ogg", "A", "grunt/No_filter/a.ogg", 0);
	voices.addRadioVoice("squadleader", "squadleader/filter/s.ogg", "S", "squadleader/No_filter/s.ogg", 1);
	voices.addMessage("AUTO_RULES_welostacp");
	voices.addRadioVoice("commander", "commander/filter/w.ogg", "W");
	voices.addHelpMessage("HUD_HELP_x", "Help_VO/x.ogg");
	voices.build();
	std::vector<std::pair<std::string, bool>> played;
	voices.onPlay = [&](const std::string& file, bool local) { played.push_back({file, local}); };

	sound::VoiceMessages::Heard in;
	in.message = "roger_that";
	in.hasSender = true;
	in.sender = {5, 2, {10, 0, 0}};
	in.listenerTeam = 2;
	in.listener = {0, 0, 0};
	auto result = voices.heard(in);
	CHECK(result.played == sound::VoiceMessages::Played::Local && result.text == "A");
	CHECK(played.back() == std::make_pair(std::string("sound/vo/English/grunt/No_filter/a.ogg"), true));
	in.sender.position = {40, 0, 0};
	result = voices.heard(in);
	CHECK(result.played == sound::VoiceMessages::Played::Radio && played.back().first == "sound/vo/English/grunt/filter/a.ogg");
	// An enemy grunt (flags 0: same team only) far or near isn't heard.
	in.sender.team = 1;
	CHECK(voices.heard(in).played == sound::VoiceMessages::Played::None);
	in.sender.position = {5, 0, 0};
	CHECK(voices.heard(in).played == sound::VoiceMessages::Played::None);
	// An enemy squad leader (flags 1: both teams) near is.
	in.squadLeader = true;
	result = voices.heard(in);
	CHECK(result.played == sound::VoiceMessages::Played::Local
		&& played.back().first == "sound/vo/Russian/squadleader/No_filter/s.ogg");
	// From no one: the commander's voice of the listener's team.
	sound::VoiceMessages::Heard automatic;
	automatic.message = "AUTO_RULES_welostacp";
	automatic.listenerTeam = 1;
	result = voices.heard(automatic);
	CHECK(result.played == sound::VoiceMessages::Played::Radio && played.back().first == "sound/vo/Russian/commander/filter/w.ogg");
	CHECK(voices.playHelp("HUD_HELP_x") == 0 && played.back().first == "sound/vo/English/Help_VO/x.ogg");
	CHECK(voices.playHelp("nope") == -1);
}

namespace {

// A round on flat ground: the local player (EU) with a rifle whose sounds
// have files, 20 m from a PAC bot.
struct Range {
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	io::FileManager files;
	physics::CollisionTemplateManager collisionTemplates;
	std::unique_ptr<game::GameWorld> world;
	sound::SoundManager manager {nullptr, templates};
	sound::GameSounds game {manager};
	sound::VoiceMessages voices {manager};
	std::vector<sound::GameSounds::Triggered> triggered;

	Range()
	{
		manager.registerTemplateType();
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
		templates.registerType(4, "GenericFireArm");
		templates.registerType(5, "GenericProjectile");
		templates.registerType(6, "SimpleObject");
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			templates.create("Soldier", name)->settings()["armor.maxHitPoints"] = "100";
		}
		for (const char* name : {"eu_ar_rifle", "as_ar_rifle"}) {
			world::ObjectTemplate* rifle = templates.create("GenericFireArm", name);
			rifle->settings()["projectileTemplate"] = "bullet";
			rifle->settings()["velocity"] = "300";
			rifle->settings()["fire.roundsPerMinute"] = "600";
			rifle->settings()["ammo.magSize"] = "3";
			rifle->settings()["ammo.reloadTime"] = "1";
		}
		templates.create("GenericProjectile", "bullet")->settings()["damage"] = "1";
		for (const char* event : {"Fire1P", "Fire3P", "Reload1P", "Reload3P", "Deploy1P", "Deploy3P"}) {
			addSoundTemplate(templates, std::string("S_eu_ar_rifle_") + event);
			addSoundTemplate(templates, std::string("S_as_ar_rifle_") + event);
		}
		for (int team = 1; team <= 2; ++team) {
			world::ObjectTemplate* point = templates.create("ControlPoint", team == 1 ? "cp_pac" : "cp_eu");
			point->settings()["controlPointId"] = std::to_string(team);
			point->settings()["team"] = std::to_string(team);
			point->settings()["unableToChangeTeam"] = "1";
			objects.create(point, {team == 1 ? 100.0f : -100.0f, 0, 0});
		}
		world::ObjectTemplate* spawn = templates.create("SpawnPoint", "sp");
		objects.create(spawn, {0, 0, 20}, {90, 0, 0})->setControlPointId(1);
		objects.create(spawn, {0, 0, 0}, {0, 0, 0})->setControlPointId(2);
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->enablePhysics(files, collisionTemplates,
			{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return 0.0f;
			 }});
		world->startRound();
		game.onTrigger = [this](const sound::GameSounds::Triggered& t) { triggered.push_back(t); };
	}
	size_t count(const std::string& name) const
	{
		size_t n = 0;
		for (const auto& t : triggered) {
			n += t.name == name;
		}
		return n;
	}
};

} // namespace

// GameWorld's weapon sound events through GameAudio: the local player's
// shots in first person, a bot's in third, the reload after the magazine.
TEST(GameAudio_WeaponsOfARound)
{
	Range range;
	GameAudio audio(*range.world, range.objects, range.templates, range.manager, range.game, nullptr);
	const int bot = range.world->addPlayer("Bot", 1, true);
	CHECK(range.world->spawnSoldier(bot));
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::SoldierInput fire;
	fire.fire = true;
	range.world->setPlayerInput(fire);
	GameAudio::Camera camera;
	camera.firstPerson = true;
	for (int tick = 0; tick < 20; ++tick) {
		range.world->tick(1 / 30.0f);
		audio.tick(camera);
	}
	CHECK(range.count("S_eu_ar_rifle_Fire1P") == 3 && range.count("S_eu_ar_rifle_Fire3P") == 0);
	CHECK(range.count("S_eu_ar_rifle_Reload1P") == 1 && audio.weaponSounds >= 4);
	// From the chase camera the same rifle is a 3P sound.
	camera.firstPerson = false;
	for (int tick = 0; tick < 40; ++tick) {
		range.world->tick(1 / 30.0f);
		audio.tick(camera);
	}
	CHECK(range.count("S_eu_ar_rifle_Fire3P") >= 1);
}

// The local player's radio through GameAudio: a teammate's message within
// 15 m in person, an object's automatic team-change message from no one
// over the commander's radio (TCServerTeamMessage).
TEST(GameAudio_RadioVoiceOverAndAutomaticMessages)
{
	Range range;
	range.voices.reset("Russian", "English");
	range.voices.addRadioFilter("commander", "Sound/VO/Activate_radio.wav");
	range.voices.addMessage("roger_that");
	range.voices.addRadioVoice("grunt", "grunt/filter/a.ogg", "A", "grunt/No_filter/a.ogg", 1);
	range.voices.addMessage("AUTO_TITAN_C_friendlycapturedsilo");
	range.voices.addRadioVoice("commander", "commander/filter/cap.ogg", "CAP");
	range.voices.addMessage("AUTO_TITAN_C_enemycapturedsilo");
	range.voices.addRadioVoice("commander", "commander/filter/lost.ogg", "LOST");
	range.voices.build();
	std::vector<std::string> played;
	range.voices.onPlay = [&](const std::string& file, bool) { played.push_back(file); };
	// A silo with radio messages, PAC's.
	world::ObjectTemplate* silo = range.templates.create("SimpleObject", "silo");
	silo->settings()["radio.friendlyTeamChangeMessage"] = "AUTO_TITAN_C_friendlycapturedsilo";
	silo->settings()["radio.enemyTeamChangeMessage"] = "AUTO_TITAN_C_enemycapturedsilo";
	world::Object* object = range.objects.create(silo, {50, 0, 0});
	object->setTeam(1);
	GameAudio audio(*range.world, range.objects, range.templates, range.manager, range.game, &range.voices);
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int mate = range.world->addPlayer("Mate", 2, true);
	CHECK(range.world->spawnSoldier(mate));
	range.world->tick(1 / 30.0f);
	range.world->radioMessage(mate, game::GameWorld::targetChannel("TCTeam"), "roger_that");
	audio.tick({});
	CHECK(audio.radioHeard == 1 && audio.voiceOvers == 1 && !played.empty()
		&& played.back() == "sound/vo/English/grunt/No_filter/a.ogg");
	// EU takes the silo.
	object->setTeam(2);
	const auto sent = audio.autoRadio();
	CHECK(sent.size() == 2 && sent[0] == std::make_pair(2, std::string("AUTO_TITAN_C_friendlycapturedsilo"))
		&& sent[1] == std::make_pair(1, std::string("AUTO_TITAN_C_enemycapturedsilo")));
	audio.tick({});
	CHECK(audio.autoRadio().empty());
	CHECK(played.back() == "sound/vo/English/commander/filter/cap.ogg");
}

// The shipped table: Sound/VoiceMessages.con and the files it names.
TEST(VoiceMessages_ShippedTable)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	files.setMountingAllowed(true);
	CHECK(files.mountArchive("Sound_server.zip", "Sound"));
	CHECK(files.mountArchive("Sound_bp1_server.zip", "Sound"));
	CHECK(files.mountArchive("Sound_client.zip", "Sound"));
	files.setMountingAllowed(false);
	Console console {&files};
	world::ObjectTemplateManager templates;
	sound::SoundManager manager(nullptr, templates);
	sound::VoiceMessages voices(manager);
	sound::registerVoiceMessageConsoleMethods(console, voices);
	voices.reset("Russian", "English");
	std::string out;
	CHECK(console.run("Sound/VoiceMessages.con", {}, out));
	CHECK(voices.messageCount() > 250 && voices.messageCount() <= 300);
	CHECK(voices.voice("grunt") >= 0 && voices.voice("squadleader") >= 0 && voices.voice("commander") >= 0);
	CHECK(voices.filter("commander") >= 0 && near(voices.voiceOverVolume(), 0.72f));
	const int spotted = voices.message("tank_spotted");
	const auto grunt = voices.samples(spotted, voices.voice("grunt"));
	CHECK(spotted >= 0 && grunt.second == 2);
	// The files are there, in either team's language.
	CHECK(files.exists(voices.radioFile(1, grunt.first)) && files.exists(voices.radioFile(2, grunt.first)));
	CHECK(files.exists(voices.localFile(2, grunt.first)));
	const auto automatic = voices.samples(voices.message("AUTO_RULES_welostacp"), voices.voice("commander"));
	CHECK(automatic.second == 2 && files.exists(voices.radioFile(2, automatic.first)));
}
