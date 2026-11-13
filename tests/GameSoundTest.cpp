// The game's side of sound without an audio device: the sound manager's
// muting fades (0x007f9f50), the "Sound" console's game sounds and ambient
// triggers, the weapons' sounds (DefaultSoundComp), the objects' loops,
// the voice-message table and its radio / local / help playing
// (0x004d8b90, 0x004d7950, 0x004d8110, 0x0042e0c0), and what GameAudio
// hands the sound layer from a round. With $BF2142_GAME_DIR, also the
// shipped Sound/VoiceMessages.con.
#include "Console/Console.h"
#include "Console/Vars.h"
#include "Physics/MaterialManager.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Main/GameAudio.h"
#include "Physics/CollisionTemplateManager.h"
#include "Sound/Game/GameSounds.h"
#include "Sound/SoundEngine/EnhancedAudio.h"
#include "Sound/Game/SoundConsole.h"
#include "Sound/Game/SoundManager.h"
#include "Sound/Game/SoundObjectTemplate.h"
#include "Sound/Game/VoiceMessages.h"
#include "Sound/SoundEngine/ISoundEngine.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <algorithm>
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

// A released GameSounds positional one-shot snapshots its obstruction before
// playback. The rays must end at its actual position, even far from origin.
TEST(GameSounds_DetachedOneShotObstructionUsesWorldPosition)
{
	const char* dir = std::getenv("BF2142_SOUND_TEST_DIR");
	if (!dir) return;
	#ifdef _WIN32
	_putenv_s("ALSOFT_DRIVERS", "null");
	#else
	setenv("ALSOFT_DRIVERS", "null", 1);
	#endif
	auto* engine = sound::getSoundEngine();
	CHECK(engine->initialize(0, "Software Audio", 16) == 0);
	{
		world::ObjectTemplateManager templates;
		sound::SoundManager manager(engine, templates);
		manager.registerTemplateType();
		addSoundTemplate(templates, "StepAtPosition", std::string(dir) + "/Cockpits/Missile_Warning.wav");
		auto* soundTemplate = manager.soundTemplate("StepAtPosition");
		CHECK(soundTemplate != nullptr);
		if (soundTemplate) {
			soundTemplate->setIs3dSound(true);
			soundTemplate->setLoopCount(1);
			const sound::Vec3 offset {2, 0, 0};
			soundTemplate->setPosition(&offset);
		}
		engine->reloadSoundFiles();
		manager.setListener({100, 10, 100}, {0, 0, 1}, {0, 1, 0}, {}, false);
		std::vector<sound::Vec3> ends;
		manager.setObstructionTest([&ends](const sound::Vec3&, const sound::Vec3& to, uint32_t,
			sound::SoundManager::RayHit&) { ends.push_back(to); return false; });
		sound::GameSounds sounds(manager);
		CHECK(sounds.play("StepAtPosition", {110, 10, 100}));
		CHECK(!ends.empty());
		if (!ends.empty()) CHECK(ends[0].x > 111 && ends[0].y > 9 && ends[0].z > 99);
		for (int i = 0; i < 60; ++i) manager.update(1);
	}
	engine->shutdown();
	sound::shutdownSoundEngine();
}

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

// ObjectTemplate.globalEffectLevel (executor 0x007f54a0) goes to the
// engine template's slot 0xb8 (read back by 0xbc): how much occlusion and
// the tinnitus muffle the sound.
TEST(SoundConsole_GlobalEffectLevelIsTheEnginesOcclusionFactor)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::registerWorldConsoleMethods(console, templates, objects);
	sound::SoundManager manager(sound::getSoundEngine(), templates);
	manager.registerTemplateType();
	sound::registerSoundConsoleMethods(console, manager);
	std::string out;
	CHECK(console.execute("ObjectTemplate.create Sound S_hud_beep", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	const auto* sound = dynamic_cast<const sound::SoundObjectTemplate*>(templates.find("S_hud_beep"));
	CHECK(sound && sound->engineTemplate() && sound->engineTemplate()->occlusionFactor() == 1.0f);
	CHECK(console.execute("ObjectTemplate.globalEffectLevel 0.25", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(sound && sound->engineTemplate() && sound->engineTemplate()->occlusionFactor() == 0.25f);
	CHECK(console.execute("ObjectTemplate.globalEffectLevel", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(out.find("0.25") != std::string::npos);
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

// A generated event template can exist without a sample. The vanilla
// heavy walker's main cannon leaves Fire1P_Outdoor empty, so outdoors a
// first-person shot must fall back to its configured Fire1P sample.
TEST(GameSounds_EmptyOutdoorFireVariantFallsBackToFirstPerson)
{
	Sounds s;
	addSoundTemplate(s.templates, "S_walker_cannon_Fire1P");
	auto* outdoor = dynamic_cast<sound::SoundObjectTemplate*>(
		s.templates.create(sound::SoundObjectTemplate::kClassId, "S_walker_cannon_Fire1P_Outdoor"));
	CHECK(outdoor != nullptr && !outdoor->hasFiles());
	sound::GameSounds::FireInfo fire;
	fire.mode = 0;
	s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "walker_cannon", true, {}, true, 1, 0, &fire);
	CHECK(s.triggered.size() == 1 && s.triggered[0].name == "S_walker_cannon_Fire1P");
}

// The right Shepherd gunner's Fire1P is misnamed in the shipped data, so
// only its Fire3P has a sample. Ordinary: silent, as retail. Enhanced: the
// local player's shot plays it head-relative (like the left gunner's 2D
// Fire1P), not 3D at the muzzle where occlusion and HRTF would muffle it.
TEST(GameSounds_FirstPersonShotWithOnlyThirdPersonSampleStaysHeadRelative)
{
	Sounds s;
	addSoundTemplate(s.templates, "S_gunner_Fire3P");
	s.templates.create(sound::SoundObjectTemplate::kClassId, "S_gunner_Fire1P"); // no file
	sound::GameSounds::FireInfo fire;
	fire.mode = 0;
	// Ordinary (retail 0x005dbdc0): the empty first-person slot plays nothing,
	// but muteOnFire still ducks the world (reason 4 is set before the
	// sample is looked at, 0x005dbec3..0x005dbee7).
	sound::setEnhancedAudio(false);
	s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "gunner", true, {4, 0, 0}, true, 1, 0, &fire);
	CHECK(s.triggered.empty());
	CHECK(s.manager.muted(4));
	s.manager.setMuted(4, false);
	// Enhanced: the 3P sample stands in, head-relative.
	sound::setEnhancedAudio(true);
	s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "gunner", true, {4, 0, 0}, true, 1, 0, &fire);
	sound::setEnhancedAudio(false);
	CHECK(s.triggered.size() == 1 && s.triggered[0].name == "S_gunner_Fire3P" && s.triggered[0].firstPerson);
	CHECK(s.manager.muted(4));
	s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "gunner", false, {4, 0, 0}, true, 2, 0, &fire);
	CHECK(s.triggered.size() == 2 && !s.triggered[1].firstPerson && s.triggered[1].position.x == 4);
}

// Enhanced audio: a gun whose Fire1P is filed under the same words in
// another order (the right Shepherd gun: S_eu_at_minigun_R_firearm_Fire1P
// for S_eu_at_R_minigun_firearm_Fire1P) plays that 1P sound, as its twin
// does. Ordinary audio stays retail: nothing (0x005dbdc0), the duck alone.
TEST(GameSounds_EnhancedFindsAFirstPersonSoundUnderReorderedWords)
{
	Sounds s;
	addSoundTemplate(s.templates, "S_at_gun_R_firearm_Fire1P");
	addSoundTemplate(s.templates, "S_at_R_gun_firearm_Fire3P");
	sound::GameSounds::FireInfo fire;
	fire.mode = 0;
	sound::setEnhancedAudio(false);
	s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "at_R_gun_firearm", true, {4, 0, 0}, true, 1, 0, &fire);
	CHECK(s.triggered.empty() && s.manager.muted(4));
	s.manager.setMuted(4, false);
	sound::setEnhancedAudio(true);
	s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "at_R_gun_firearm", true, {4, 0, 0}, true, 1, 0, &fire);
	sound::setEnhancedAudio(false);
	CHECK(s.triggered.size() == 1 && s.triggered[0].name == "S_at_gun_R_firearm_Fire1P" && s.triggered[0].firstPerson);
	CHECK(s.manager.muted(4));
}

// The shipped data's shape: the right Shepherd gun's eu_at.tweak adds
// S_eu_at_R_minigun_firearm_Fire1P (no file) but configures the sound under
// another word order with `activeSafe Sound`, which no one created. Retail
// applies that block to nothing; enhanced audio creates the sound so the lookup
// above finds it (the right gun then plays the left one's 2D sample).
TEST(GameSounds_EnhancedCreatesTheSoundAnActiveSafeBlockConfigures)
{
	for (const bool enhanced : {false, true}) {
		Sounds s;
		world::ObjectManager objects;
		Console console;
		world::registerWorldConsoleMethods(console, s.templates, objects);
		sound::registerSoundConsoleMethods(console, s.manager);
		if (enhanced) {
			s.templates.onMissingSound = [&s](world::ObjectTemplate& created) { s.manager.applyEventDefaults(created); };
		}
		s.templates.create("Sound", "S_at_R_gun_firearm_Fire1P"); // addTemplate: no file
		std::string out;
		CHECK(console.execute("ObjectTemplate.activeSafe Sound S_at_gun_R_firearm_Fire1P", out) == ConsoleResult::Ok);
		console.execute("ObjectTemplate.soundFilename \"sound/weapons/miniguns/drone_gun_mono.wav\"", out);
		CHECK((s.templates.find("S_at_gun_R_firearm_Fire1P") != nullptr) == enhanced);
		CHECK(s.game.sampleCount("S_at_gun_R_firearm_Fire1P") == (enhanced ? 1 : 0));
		sound::setEnhancedAudio(enhanced);
		sound::GameSounds::FireInfo fire;
		fire.mode = 0;
		s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "at_R_gun_firearm", true, {4, 0, 0}, true, 1, 0, &fire);
		sound::setEnhancedAudio(false);
		CHECK(s.triggered.size() == (enhanced ? 1u : 0u));
		CHECK(!enhanced || (s.triggered[0].name == "S_at_gun_R_firearm_Fire1P" && s.triggered[0].firstPerson));
	}
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
		for (const char* event : {"Fire1P", "Fire3P", "Reload1P", "Reload3P", "Deploy1P", "Deploy3P", "TriggerClick"}) {
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
	// The profile's auto reload off (GeneralSettings.setAutoReload 0): the
	// player presses c_PIReload himself.
	range.world->autoReload = false;
	const int bot = range.world->addPlayer("Bot", 1, true);
	CHECK(range.world->spawnSoldier(bot));
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::SoldierInput fire;
	fire.fire = true;
	range.world->setPlayerInput(fire);
	GameAudio::Camera camera;
	camera.firstPerson = true;
	int acceptedShots = 0;
	for (int tick = 0; tick < 12; ++tick) {
		range.world->tick(1 / 30.0f);
		acceptedShots += range.world->playerWeapon()->firedThisTick();
		audio.tick(camera);
	}
	// The rifle has no ammo.autoReload: run dry it stays empty, and a new
	// press clicks (0x005c2ee0 -> 0x005dc160, first person).
	CHECK(range.count("S_eu_ar_rifle_Reload1P") == 0);
	game::SoldierInput idle;
	range.world->setPlayerInput(idle);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput(fire);
	range.world->tick(1 / 30.0f);
	acceptedShots += range.world->playerWeapon()->firedThisTick();
	audio.tick(camera);
	CHECK(range.count("S_eu_ar_rifle_TriggerClick") == 1);
	game::SoldierInput reload;
	reload.reload = true;
	range.world->setPlayerInput(reload);
	range.world->tick(1 / 30.0f);
	audio.tick(camera);
	range.world->setPlayerInput(fire);
	for (int tick = 0; tick < 30; ++tick) {
		range.world->tick(1 / 30.0f);
		acceptedShots += range.world->playerWeapon()->firedThisTick();
		audio.tick(camera);
	}
	// Held automatic fire starts a loop, and the reload starts a fresh loop.
	CHECK(acceptedShots >= 4 && range.count("S_eu_ar_rifle_Fire1P") == 2 && range.count("S_eu_ar_rifle_Fire3P") == 0);
	CHECK(range.count("S_eu_ar_rifle_Reload1P") == 1 && audio.weaponSounds >= 4);
	range.world->setPlayerInput(reload);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput(fire);
	// From the chase camera the same rifle is a 3P sound.
	camera.firstPerson = false;
	int thirdPersonAcceptedShots = 0;
	for (int tick = 0; tick < 40; ++tick) {
		range.world->tick(1 / 30.0f);
		const int fired = range.world->playerWeapon()->firedThisTick();
		acceptedShots += fired;
		thirdPersonAcceptedShots += fired;
		audio.tick(camera);
	}
	CHECK(thirdPersonAcceptedShots >= 2); // enough rounds to distinguish one free-running loop
	CHECK(range.count("S_eu_ar_rifle_Fire3P") == 1);
}

// A round's own Sound child (created and played with it) loops while it
// flies, whatever its name (static_ap_railgun_projectile's is
// S_static_ap_railgun_projectile_projectile_Looping).
TEST(GameAudio_RoundLoopsItsSoundChild)
{
	Range range;
	addSoundTemplate(range.templates, "S_bullet_whiz");
	range.templates.find("bullet")->children().push_back({"S_bullet_whiz", {}, {}});
	GameAudio audio(*range.world, range.objects, range.templates, range.manager, range.game, nullptr);
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::SoldierInput fire;
	fire.fire = true;
	range.world->setPlayerInput(fire);
	range.world->tick(1 / 30.0f);
	CHECK(!range.world->projectiles().empty());
	audio.tick({});
	CHECK(range.count("S_bullet_whiz") == 1);
}

// The local player's radio through GameAudio: a teammate's message within
// 15 m in person, an object's automatic team-change message played for the
// local player by its team.
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
	// The local radio menu echoes its selected message to the sender. It
	// must play the radio sample without a speaker filter.
	const int local = range.world->localPlayer();
	range.world->hearRadio(local, game::GameWorld::targetChannel("TCTeam"), "roger_that", local);
	audio.tick({});
	CHECK(played.back() == "sound/vo/English/grunt/filter/a.ogg");
	// EU takes the silo: played here only, the local player's (EU's) message
	// (0x00656e80 through game slot 0x60).
	object->setTeam(2);
	const auto sent = audio.autoRadio();
	CHECK(sent.size() == 1 && sent[0] == std::make_pair(2, std::string("AUTO_TITAN_C_friendlycapturedsilo")));
	audio.tick({});
	CHECK(audio.autoRadio().empty());
	CHECK(played.back() == "sound/vo/English/commander/filter/cap.ogg");
	// PAC takes it back: the enemy's message, still to the local player.
	object->setTeam(1);
	const auto lost = audio.autoRadio();
	CHECK(lost.size() == 1 && lost[0].second == "AUTO_TITAN_C_enemycapturedsilo");
}

// An object's damage, destroyed and repaired messages (the Radio object
// component's one-shots, 0x00656c90 / 0x00656d10 / 0x006571d0): from no one
// to its team (the other team the enemy's), damage once until repaired.
TEST(GameAudio_DamageDestroyedRepairedMessages)
{
	Range range;
	world::ObjectTemplate* uav = range.templates.create("SimpleObject", "uav_trailer");
	uav->settings()["armor.maxHitPoints"] = "100";
	uav->settings()["radio.damageMessage"] = "COMMSL_UAV_trailerdamaged";
	uav->settings()["radio.friendlyDestroyedMessage"] = "uav_lost";
	uav->settings()["radio.enemyDestroyedMessage"] = "enemy_uav_down";
	uav->settings()["radio.repairedMessage"] = "uav_repaired";
	world::Object* object = range.objects.create(uav, {50, 0, 0});
	object->setTeam(2);
	GameAudio audio(*range.world, range.objects, range.templates, range.manager, range.game, &range.voices);
	CHECK(range.world->spawnPlayer(2) != nullptr);
	range.world->setHitPoints(object->id(), 100);
	CHECK(audio.autoRadio().empty());
	range.world->setHitPoints(object->id(), 80);
	auto sent = audio.autoRadio();
	CHECK(sent.size() == 1 && sent[0] == std::make_pair(2, std::string("COMMSL_UAV_trailerdamaged")));
	range.world->setHitPoints(object->id(), 50);
	CHECK(audio.autoRadio().empty()); // once
	range.world->setHitPoints(object->id(), 0);
	sent = audio.autoRadio();
	CHECK(sent.size() == 2 && sent[0] == std::make_pair(2, std::string("uav_lost"))
		&& sent[1] == std::make_pair(1, std::string("enemy_uav_down")));
	range.world->setHitPoints(object->id(), 100);
	sent = audio.autoRadio();
	CHECK(sent.size() == 1 && sent[0].second == "uav_repaired");
	range.world->setHitPoints(object->id(), 90);
	sent = audio.autoRadio();
	CHECK(sent.size() == 1 && sent[0].second == "COMMSL_UAV_trailerdamaged"); // re-armed
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

// 0x005dbdc0 by the template's samples: one sample loops for an automatic
// weapon until the trigger is let go (0x005db6c0); a fast burst plays the
// sample once per round and skips the burst's other events, a slow one
// (+0x42, under 301 rounds a minute) plays it each round; four samples
// are a grenade's throw (1 while held, 3 at the release after a hold, 0
// for a single); two or three samples play nothing.
TEST(GameSounds_FireSampleRules)
{
	Sounds s;
	addSoundTemplate(s.templates, "S_mg_Fire3P");
	addSoundTemplate(s.templates, "S_nade_Fire3P", "a.wav,b.wav,c.wav,d.wav");
	addSoundTemplate(s.templates, "S_odd_Fire3P", "a.wav,b.wav");
	using E = sound::GameSounds::WeaponEvent;
	sound::GameSounds::FireInfo automatic;
	automatic.mode = 2;
	automatic.burstRounds = 30;
	s.game.weapon(E::Fire, "mg", false, {}, true, 1, 0, &automatic);
	s.game.weapon(E::Fire, "mg", false, {}, true, 1, 0, &automatic);
	CHECK(s.triggered.size() == 1 && s.game.fireLooping(1));
	s.game.releaseTrigger(1);
	CHECK(!s.game.fireLooping(1));
	s.game.weapon(E::Fire, "mg", false, {}, true, 1, 0, &automatic);
	CHECK(s.triggered.size() == 2);
	s.game.releaseTrigger(1);
	// A burst of three at 600 rounds a minute: one sound, two skipped.
	sound::GameSounds::FireInfo burst;
	burst.mode = 1;
	burst.burstRounds = 3;
	for (int i = 0; i < 4; ++i) {
		s.game.weapon(E::Fire, "mg", false, {}, true, 2, 0, &burst);
	}
	CHECK(s.triggered.size() == 4); // rounds 1 and 4
	burst.roundsPerMinute = 300;
	const size_t before = s.triggered.size();
	for (int i = 0; i < 3; ++i) {
		s.game.weapon(E::Fire, "mg", false, {}, true, 3, 0, &burst);
	}
	CHECK(s.triggered.size() == before + 3);
	// The grenade: a single throw sample 0; held then thrown 1 then 3.
	sound::GameSounds::FireInfo throwInfo;
	s.game.weapon(E::Fire, "nade", false, {}, true, 4, 0, &throwInfo);
	CHECK(s.triggered.back().sample == 0);
	throwInfo.held = true;
	s.game.weapon(E::Fire, "nade", false, {}, true, 4, 0, &throwInfo);
	CHECK(s.triggered.back().sample == 1);
	throwInfo.held = false;
	s.game.weapon(E::Fire, "nade", false, {}, true, 4, 0, &throwInfo);
	CHECK(s.triggered.back().sample == 3);
	const size_t odd = s.triggered.size();
	s.game.weapon(E::Fire, "odd", false, {}, true, 5, 0, &throwInfo);
	CHECK(s.triggered.size() == odd);
	CHECK(s.game.sampleCount("S_nade_Fire3P") == 4 && s.game.sampleCount("S_mg_Fire3P") == 1);
}

// An automatic local weapon changes its loop when the listener leaves the
// first-person camera while fire remains held.
TEST(GameSounds_AutomaticFireTracksCameraPerspective)
{
	Sounds s;
	addSoundTemplate(s.templates, "S_mg_Fire1P");
	addSoundTemplate(s.templates, "S_mg_Fire3P");
	sound::GameSounds::FireInfo automatic;
	automatic.mode = 2;
	s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "mg", true, {4, 5, 6}, true, 17, 9, &automatic);
	CHECK(s.triggered.size() == 1 && s.triggered.back().name == "S_mg_Fire1P");
	CHECK(s.game.fireLooping(17) && s.manager.muted(4));
	s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "mg", false, {4, 5, 6}, true, 17, 9, &automatic);
	CHECK(s.triggered.size() == 2 && s.triggered.back().name == "S_mg_Fire3P");
	CHECK(s.game.fireLooping(17) && !s.manager.muted(4));
	s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "mg", true, {4, 5, 6}, true, 17, 9, &automatic);
	CHECK(s.triggered.size() == 3 && s.triggered.back().name == "S_mg_Fire1P");
	CHECK(s.game.fireLooping(17) && s.manager.muted(4));
	s.game.releaseTrigger(17);
	CHECK(!s.game.fireLooping(17) && !s.manager.muted(4));
}

// 0x005dbfe0 by the reload state and samples: one sample plays at a
// magazine reload; three samples play nothing for a magazine and 0, 1, 1
// for a shell reload's first, middle and last; two samples' second at
// event 7.
TEST(GameSounds_ReloadSampleRules)
{
	Sounds s;
	addSoundTemplate(s.templates, "S_rifle_Reload3P");
	addSoundTemplate(s.templates, "S_shotgun_Reload3P", "a.wav,b.wav,c.wav");
	addSoundTemplate(s.templates, "S_rocket_Reload3P", "a.wav,b.wav");
	using E = sound::GameSounds::WeaponEvent;
	s.game.weapon(E::Reload, "rifle", false, {}, true, 1, 0, nullptr, 3);
	CHECK(s.triggered.size() == 1 && s.triggered.back().sample == 0);
	s.game.weapon(E::Reload, "rifle", false, {}, true, 1, 0, nullptr, 2);
	CHECK(s.triggered.size() == 1);
	s.game.weapon(E::Reload, "shotgun", false, {}, true, 2, 0, nullptr, 3);
	CHECK(s.triggered.size() == 1);
	for (const int state : {4, 5, 6}) {
		s.game.weapon(E::Reload, "shotgun", false, {}, true, 2, 0, nullptr, state);
	}
	CHECK(s.triggered.size() == 4 && s.triggered[1].sample == 0 && s.triggered[2].sample == 1
		&& s.triggered[3].sample == 1);
	s.game.weapon(E::Reload, "rocket", false, {}, true, 3, 0, nullptr, 7);
	CHECK(s.triggered.size() == 5 && s.triggered.back().sample == 1);
}

// 0x007fa350: nothing in the way, nothing; every ray blocked gives
// clamp((1 - (atan((dist - h) / 2) + atan(h / 2)) / pi) x 0.8); the
// sound's own (small) root, nothing; beyond 40 m a single ray; inside a
// Titan beyond 55 m, 1; Settings/Sound.con's vars change it.
TEST(SoundManager_Obstruction)
{
	Vars::global().clear();
	world::ObjectTemplateManager templates;
	sound::SoundManager manager(nullptr, templates);
	manager.setListener({0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {});
	CHECK(manager.obstruction({0, 0, 20}, 1, 0) == 0); // no test: nothing
	int rays = 0;
	manager.setObstructionTest([&rays](const sound::Vec3&, const sound::Vec3&, uint32_t, sound::SoundManager::RayHit& hit) {
		++rays;
		hit.fraction = 0.5f;
		hit.root = 7;
		return true;
	});
	const float dist = 20 - 1 - 0.2f;
	const float h = (20 - 1 - 0.2f) * 0.5f;
	const float expected = (1 - (std::atan((dist - h) / 2) + std::atan(h / 2)) * 0.31830987f) * 0.8f;
	CHECK(near(manager.obstruction({0, 0, 20}, 1, 0), std::clamp(expected, 0.0f, 1.0f)));
	CHECK(rays == 3);
	CHECK(manager.obstruction({0, 0, 20}, 1, 7) == 0); // its own root
	rays = 0;
	manager.obstruction({0, 0, 100}, 1, 0);
	CHECK(rays == 1);
	manager.setListener({0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {}, true);
	CHECK(manager.obstruction({0, 0, 100}, 1, 0) == 1);
	// A ray clear: nothing; the one found clear is remembered.
	manager.setObstructionTest([](const sound::Vec3& from, const sound::Vec3&, uint32_t, sound::SoundManager::RayHit&) {
		return from.x == 0 && from.y == 0; // only the middle ray blocked
	});
	int clear = -1;
	CHECK(manager.obstruction({0, 0, 20}, 1, 0, &clear) == 0 && clear == 1);
	Vars::global().set("sound-obstruction-disable", "1");
	manager.setObstructionTest([](const sound::Vec3&, const sound::Vec3&, uint32_t, sound::SoundManager::RayHit&) { return true; });
	CHECK(manager.obstruction({0, 0, 20}, 1, 0) == 0);
	Vars::global().clear();
}

// Vars.set (0x004244c1) feeds the sound tunables: Settings/Sound.con's
// sound-maxreverb 0 leaves the listener never indoors; its
// sound-mute-level-1p 0.3 the firing's duck.
TEST(SoundConsole_VarsSetTheSoundTunables)
{
	Vars::global().clear();
	world::ObjectTemplateManager templates;
	sound::SoundManager manager(nullptr, templates);
	Console console;
	sound::registerSoundConsoleMethods(console, manager);
	std::string out;
	manager.setRayTest([](const sound::Vec3& from, const sound::Vec3& to, float& distance) {
		distance = to.y > from.y ? 4.0f : 2.0f;
		return true;
	});
	CHECK(manager.indoorLevel({0, 0, 0}) > 0);
	CHECK(console.execute("vars.set sound-maxreverb 0", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(manager.indoorLevel({0, 0, 0}) == 0 && manager.outdoors({0, 0, 0}));
	CHECK(console.execute("vars.set sound-mute-level-1p 0.3", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(console.execute("vars.set sound-mute-fadeout-1p 1", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	manager.setMuted(4, true);
	manager.update(1);
	CHECK(near(manager.duckedVolume(0), 0.3f));
	Vars::global().clear();
}

// MaterialManager.setRadioMessage <index> <message> <delay> on the active
// cell (0x0060ce20), looked up by slot 0x78 (0x0060a730: the cell, else
// the surface against material 0); Material.triggerRadio.
TEST(MaterialManager_RadioMessages)
{
	physics::MaterialManager materials;
	Console console;
	physics::registerMaterialConsoleMethods(console, materials);
	std::string out;
	CHECK(console.execute("Material.active 4101", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(console.execute("Material.triggerRadio 1", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(console.execute("MaterialManager.createCell 4101 24", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(console.execute("MaterialManager.setRadioMessage 2 AUTO_TITAN_BB_enemydetectedcargo_inside 100", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(materials.find(4101) && materials.find(4101)->triggerRadio);
	const auto* radio = materials.radioMessage(4101, 24, 2);
	CHECK(radio && radio->message == "AUTO_TITAN_BB_enemydetectedcargo_inside" && radio->delay == 100);
	CHECK(materials.radioMessage(4101, 24, 1) == nullptr && materials.radioMessage(4101, 25, 2) == nullptr);
	CHECK(console.execute("MaterialManager.createCell 4101 0", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(console.execute("MaterialManager.setRadioMessage 2 fallback 30", out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	CHECK(materials.radioMessage(4101, 25, 2) && materials.radioMessage(4101, 25, 2)->message == "fallback");
}

// The new-style engine audio (0x007fe740): the throttle blend moves by at
// most 0.2 an update and crossfades Rpm1 to Rpm2; the input follows |rpm|
// x 0.909 once settled; a gear change cues Load.
TEST(GameAudio_NewStyleEngineAudio)
{
	Vars::global().clear();
	GameAudio::NewStyleEngine e;
	GameAudio::stepNewStyle(e, true, 1, 0.5f, 1, 0, 0);
	CHECK(near(e.throttleBlend, 0.2f) && near(e.rpm1Scale, 0.8f) && near(e.rpm2Scale, 0.2f) && e.shifted);
	for (int i = 0; i < 120; ++i) {
		GameAudio::stepNewStyle(e, false, 1, 0.5f, 1, 0, i / 30.0);
	}
	CHECK(e.throttleBlend == 0 && e.rpm1Scale == 1 && !e.shifted);
	CHECK(near(e.input, 0.5f * 0.9090909f, 0.01f));
	GameAudio::stepNewStyle(e, false, 1, 0.5f, 2, 0, 5);
	CHECK(e.shifted);
}

// The soldier HUD's injury sound (0x006575f0 from 0x007a0f80): once when
// the armour's fraction falls, then not again for 2 s; the local player
// on foot.
TEST(GameAudio_InjurySound)
{
	Range range;
	for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
		range.templates.find(name)->settings()["vehicleHud.injurySound"] = "S_Injury";
	}
	GameAudio audio(*range.world, range.objects, range.templates, range.manager, range.game, nullptr);
	CHECK(range.world->spawnPlayer(2) != nullptr);
	GameAudio::Camera camera;
	camera.firstPerson = true;
	const auto step = [&] {
		range.world->tick(1 / 30.0f);
		audio.tick(camera);
	};
	step();
	game::GameWorld::PlayerInfo me;
	CHECK(range.world->playerInfo(range.world->localPlayer(), me) && me.soldier != 0);
	range.world->setHitPoints(me.soldier, 80);
	step();
	CHECK(audio.listenerState().injuries == 1);
	range.world->setHitPoints(me.soldier, 60);
	step();
	CHECK(audio.listenerState().injuries == 1);
	for (int i = 0; i < 61; ++i) {
		step();
	}
	range.world->setHitPoints(me.soldier, 40);
	step();
	CHECK(audio.listenerState().injuries == 2);
	// On foot in the open, first person: the wind is wanted.
	CHECK(audio.listenerState().windWanted && audio.listenerState().seat.empty());
}

// The shipped S_Injury lives on the worn Kit's vehicleHud, not the soldier template.
TEST(GameAudio_InjurySoundFromKit)
{
	Range range;
	range.templates.registerType(7, "Kit");
	range.templates.create("Kit", "EU_Assault")->settings()["vehicleHud.injurySound"] = "S_Injury";
	GameAudio audio(*range.world, range.objects, range.templates, range.manager, range.game, nullptr);
	CHECK(range.world->spawnPlayer(2) != nullptr);
	GameAudio::Camera camera;
	camera.firstPerson = true;
	range.world->tick(1 / 30.0f);
	audio.tick(camera);
	game::GameWorld::PlayerInfo me;
	CHECK(range.world->playerInfo(range.world->localPlayer(), me) && me.soldier != 0 && me.kit != 0);
	range.world->setHitPoints(me.soldier, 80);
	range.world->tick(1 / 30.0f);
	audio.tick(camera);
	CHECK(audio.listenerState().injuries == 1);
}

TEST(GameSounds_LocalAutomaticFireUsesRetailLoop)
{
	Sounds s;
	addSoundTemplate(s.templates, "S_rifle_Fire1P");
	addSoundTemplate(s.templates, "S_rifle_Fire3P");
	sound::GameSounds::FireInfo fire;
	fire.mode = 2;
	for (int i = 0; i < 5; ++i) {
		s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "rifle", true, {}, true, 1, 0, &fire);
		CHECK(s.game.fireLooping(1));
	}
	CHECK(s.triggered.size() == 1);
	s.game.weapon(sound::GameSounds::WeaponEvent::Fire, "rifle", false, {}, true, 1, 0, &fire);
	CHECK(s.game.fireLooping(1));
	CHECK(s.triggered.size() == 2);
	s.game.releaseTrigger(1);
	CHECK(!s.game.fireLooping(1));
}

// A seat's launcher (fire.addFireRate 0) plays its one-shot sample for each
// round; a seat's minigun (no fire rates: automatic) loops it until released;
// the cannon and rockets of one seat are separate fire components, so one's
// rounds don't cut the other's loop.
TEST(GameSounds_SeatCannonAndRocketsDoNotCutEachOther)
{
	Sounds s;
	addSoundTemplate(s.templates, "S_cannon_Fire3P");
	addSoundTemplate(s.templates, "S_rocket_Fire3P");
	using E = sound::GameSounds::WeaponEvent;
	sound::GameSounds::FireInfo single;
	single.mode = 0;
	single.burstRounds = 1;
	single.roundsPerMinute = 120;
	for (int i = 0; i < 3; ++i) {
		s.game.weapon(E::Fire, "rocket", false, {}, true, 10, 0, &single);
	}
	CHECK(s.triggered.size() == 3 && !s.game.fireLooping(10));
	sound::GameSounds::FireInfo automatic;
	automatic.mode = 2;
	automatic.burstRounds = 1;
	s.game.weapon(E::Fire, "cannon", false, {}, true, 11, 0, &automatic);
	s.game.weapon(E::Fire, "cannon", false, {}, true, 11, 0, &automatic);
	CHECK(s.triggered.size() == 4 && s.game.fireLooping(11));
	// Rockets fired while the cannon holds its loop (another state key).
	s.game.weapon(E::Fire, "rocket", false, {}, true, 12, 0, &single);
	CHECK(s.triggered.size() == 5 && s.game.fireLooping(11) && !s.game.fireLooping(12));
	s.game.releaseTrigger(11);
	CHECK(!s.game.fireLooping(11));
}

// A round's detonation starts its detonation.endEffectTemplate in the
// round's own effects; that bundle's start sound (S_<bundle>_Start) is
// heard like any other effect's (explosions, the commander's strikes).
TEST(GameAudio_DetonationEndEffectStartSound)
{
	Range range;
	range.templates.registerType(7, "EffectBundle");
	addSoundTemplate(range.templates, "S_e_boom_Start");
	range.templates.create("EffectBundle", "e_boom")->children().push_back({"S_e_boom_Start", {}, {}});
	world::ObjectTemplate* bullet = range.templates.find("bullet");
	bullet->settings()["detonation.endEffectTemplate"] = "e_boom";
	bullet->settings()["detonation.explosionRadius"] = "2";
	bullet->settings()["detonation.explosionDamage"] = "0";
	GameAudio audio(*range.world, range.objects, range.templates, range.manager, range.game, nullptr);
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::SoldierInput fire;
	fire.fire = true;
	fire.aimPitch = -60; // into the ground in front
	range.world->setPlayerInput(fire);
	GameAudio::Camera camera;
	for (int tick = 0; tick < 15; ++tick) {
		range.world->tick(1 / 30.0f);
		audio.tick(camera);
	}
	CHECK(!range.world->impacts().empty());
	CHECK(range.count("S_e_boom_Start") >= 1);
}

// A round with a detonation component but no blast (the silo missile's
// explosionRadius 0) still goes off on impact: 0x005e9300 has no radius
// gate, so its end effect (e_EMP_wave) and that bundle's sound play.
TEST(GameAudio_DetonationWithoutBlastRadiusStillStartsEndEffect)
{
	Range range;
	range.templates.registerType(7, "EffectBundle");
	addSoundTemplate(range.templates, "S_e_boom_Start");
	range.templates.create("EffectBundle", "e_boom")->children().push_back({"S_e_boom_Start", {}, {}});
	world::ObjectTemplate* bullet = range.templates.find("bullet");
	bullet->settings()["detonation.endEffectTemplate"] = "e_boom";
	bullet->settings()["detonation.explosionRadius"] = "0";
	bullet->settings()["detonation.explosionDamage"] = "0";
	GameAudio audio(*range.world, range.objects, range.templates, range.manager, range.game, nullptr);
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::SoldierInput fire;
	fire.fire = true;
	fire.aimPitch = -60; // into the ground in front
	range.world->setPlayerInput(fire);
	GameAudio::Camera camera;
	for (int tick = 0; tick < 15; ++tick) {
		range.world->tick(1 / 30.0f);
		audio.tick(camera);
	}
	CHECK(!range.world->impacts().empty());
	CHECK(range.count("S_e_boom_Start") >= 1);
}

// Pod-engine style: an attached loop without a sound-intensity mapping.
TEST(GameAudio_ConstantEffectLoopFollowsPod)
{
	Sounds sounds;
	Console console;
	sound::registerSoundConsoleMethods(console, sounds.manager);
	addSoundTemplate(sounds.templates, "S_pod_motor_Start");
	std::string out;
	CHECK(console.execute("ObjectTemplate.loopCount 0", out) == ConsoleResult::Ok);
	CHECK(sounds.templates.find("S_pod_motor_Start")->settings()["loopCount"] == "0");
	auto* bundle = sounds.templates.create("EffectBundle", "pod_motor");
	bundle->children().clear();
	bundle->children().push_back({"S_pod_motor_Start", {}, {}});
	bundle->settings()["SoundIntensityUsed"] = "0";
	effects::EffectManager effects(sounds.templates);
	effects.setAnchor(7, {0, 100, 0}, {});
	CHECK(effects.spawnAttached("pod_motor", 7, {}));
	CHECK(effects.takeSounds().empty());
	CHECK(effects.bundleLoops().size() == 1);
	effects.setAnchor(7, {100, 50, 0}, {});
	const auto loops = effects.bundleLoops();
	if (loops.size() == 1) CHECK(loops[0].position.x == 100 && loops[0].position.y == 50);
	effects.releaseAnchor(7);
	CHECK(effects.bundleLoops().empty());
}

// A magazine-sized burst (the Titan ground cannon: burst 10, magazine 10)
// starts its reload with the last round. The reload sound (0x005dbfe0) ends
// the burst's skips, so it must follow that round's fire sound: one play per
// burst, none left over for the next burst.
TEST(GameSounds_ReloadAfterLastBurstRoundKeepsOnePlayPerBurst)
{
	Sounds s;
	addSoundTemplate(s.templates, "S_cannon_Fire3P");
	using E = sound::GameSounds::WeaponEvent;
	sound::GameSounds::FireInfo burst;
	burst.mode = 1;
	burst.burstRounds = 10;
	for (int round = 0; round < 3; ++round) {
		for (int i = 0; i < 10; ++i) {
			s.game.weapon(E::Fire, "cannon", false, {}, true, 1, 0, &burst);
		}
		s.game.weapon(E::Reload, "cannon", false, {}, true, 1, 0, nullptr, 0);
	}
	CHECK(s.triggered.size() == 3);
}

// The ticket state's announcement (0x00449070): state % 20; Titan mode
// names both teams' lines, the other modes only the local team's level.
TEST(GameAudio_TicketStateMessages)
{
	CHECK(GameAudio::ticketStateMessage(1, 10, 1, "gpm_ti") == "AUTO_RULES_ticketlevel_titans_10_friendly");
	CHECK(GameAudio::ticketStateMessage(2, 7, 1, "gpm_ti") == "AUTO_RULES_ticketlevel_titans_7_enemy");
	CHECK(GameAudio::ticketStateMessage(1, 22, 1, "gpm_ti") == "AUTO_RULES_ticketlevel_titans_2_friendly");
	CHECK(GameAudio::ticketStateMessage(1, 20, 1, "gpm_ti").empty());
	CHECK(GameAudio::ticketStateMessage(1, 0, 1, "gpm_ti").empty());
	CHECK(GameAudio::ticketStateMessage(2, 15, 1, "gpm_ti") == "AUTO_RULES_TITANS_less_points_enemy");
	CHECK(GameAudio::ticketStateMessage(1, 3, 1, "gpm_cq") == "AUTO_RULES_ticketlevel_3");
	CHECK(GameAudio::ticketStateMessage(2, 3, 1, "gpm_cq").empty());
	CHECK(GameAudio::ticketStateMessage(1, 16, 1, "gpm_ti").empty());
}

// The weapon sound component's +0x41 (0x005dbfe0 sets it with each reload
// event but state 2; 0x005dc1d0, the draw, and 0x005dc160, the click,
// clear it): the first dry click after a reload is the BoltClick, in
// either view; later ones the TriggerClick, in first person only.
TEST(GameSounds_BoltClickAfterAReload)
{
	Range range;
	addSoundTemplate(range.templates, "S_eu_ar_rifle_BoltClick");
	using Event = sound::GameSounds::WeaponEvent;
	const sound::Vec3 at {0, 0, 0};
	const auto event = [&](Event e, bool firstPerson, int reloadState = 0) {
		range.game.weapon(e, "eu_ar_rifle", firstPerson, at, false, 1, 0, nullptr, reloadState);
	};
	event(Event::Deploy, true);
	event(Event::TriggerClick, true);
	CHECK(range.count("S_eu_ar_rifle_TriggerClick") == 1 && range.count("S_eu_ar_rifle_BoltClick") == 0);
	event(Event::Reload, true, 3);
	event(Event::TriggerClick, true);
	CHECK(range.count("S_eu_ar_rifle_BoltClick") == 1 && range.count("S_eu_ar_rifle_TriggerClick") == 1);
	event(Event::TriggerClick, true);
	CHECK(range.count("S_eu_ar_rifle_BoltClick") == 1 && range.count("S_eu_ar_rifle_TriggerClick") == 2);
	// State 2 sets nothing; a draw clears it.
	event(Event::Reload, true, 2);
	event(Event::TriggerClick, true);
	CHECK(range.count("S_eu_ar_rifle_BoltClick") == 1);
	event(Event::Reload, true, 3);
	event(Event::Deploy, true);
	event(Event::TriggerClick, true);
	CHECK(range.count("S_eu_ar_rifle_BoltClick") == 1 && range.count("S_eu_ar_rifle_TriggerClick") == 4);
	// In third person only the BoltClick sounds.
	event(Event::Reload, false, 3);
	event(Event::TriggerClick, false);
	event(Event::TriggerClick, false);
	CHECK(range.count("S_eu_ar_rifle_BoltClick") == 2 && range.count("S_eu_ar_rifle_TriggerClick") == 4);
}
