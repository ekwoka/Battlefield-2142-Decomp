// The game's side of sound once a tick: what the round did, handed to the
// sound layer (src/Sound/Game) the way BF2142.exe's objects drive their
// own sound components.
//   - the listener at the camera (the sound manager's update, 0x007f9d50);
//   - the weapons' fire, reload and deploy sounds (GameWorld's weapon
//     sound events; DefaultSoundComp, 0x005dbdc0 / 0x005dbfe0 / 0x005dc1d0);
//   - the engines' idle and rpm loops of the vehicles that have someone
//     aboard (the engine audio, 0x007fe050: S_<engine>_Idle, _Rpm1, _Rpm2
//     at |rpm| x 0.8333, 0x007fe740), while the engine runs;
//   - the projectiles' flight loops (DefaultProjSoundComp's
//     S_<projectile>_Looping, 0x005ea930);
//   - the round's sounds (ragdoll impacts, footsteps) and the effect
//     bundles' S_<bundle>_Start sounds (explosions, impacts, muzzles);
//   - the radio messages the local player hears, as voice-over
//     (0x0042e0c0 -> VoiceMessages::heard), and the help voice's muting;
//   - the missile silos' launch countdown (CountdownSoundComp's
//     ReloadTimeTeam<t>_<n> at sound.addReloadTimeSound's seconds);
//   - the objects' automatic radio messages (the Radio template
//     component, 0xc4b6): see AutoRadio below.
#pragma once

#include "Sound/SoundEngine/SoundMath.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dice::hfe {
namespace game {
class GameWorld;
}
namespace world {
class ObjectManager;
class ObjectTemplateManager;
class ObjectTemplate;
}
namespace sound {
class SoundManager;
class GameSounds;
class VoiceMessages;
}

class GameAudio {
public:
	GameAudio(game::GameWorld& round, world::ObjectManager& objects, world::ObjectTemplateManager& templates,
		sound::SoundManager& manager, sound::GameSounds& sounds, sound::VoiceMessages* voices);

	struct Camera {
		sound::Vec3 position, forward {0, 0, 1}, up {0, 1, 0}, velocity;
		bool firstPerson = false; // the local player's own eyes (its weapon's 1P sounds)
	};
	// A sound some effect started this tick (S_<bundle>_Start).
	void effectSound(const std::string& name, const sound::Vec3& position);
	// One tick: everything above, then the manager's update.
	void tick(const Camera& camera);

	// The objects' automatic radio (the Radio template component 0xc4b6;
	// its object part, vtable 0x00988f30, at object +0x7c, with one-shot
	// flags: +0x10 fail, +0x11 use, +0x12 repaired, +0x13 destroyed, +0x14
	// damage). From no one on TCServerTeamMessage (0xd, game slot 0x5c,
	// 0x004a5cf0) to the object's team (0x00656910: the object's or the
	// local player's), the other team's message to the other:
	//   damage (0x00656c90, the armour's hit points going down 0x00655460):
	//     damageMessage, once until repaired;
	//   destroyed (0x00656d10, event 0x18): friendlyDestroyedMessage (the
	//     same field as destroyedMessage) / enemyDestroyedMessage, once;
	//     arms repaired;
	//   repaired (0x006571d0, event 0x19): repairedMessage when armed;
	//     re-arms damage and destroyed;
	//   spawn (0x00657120 from the server's createObject 0x004ab8a0):
	//     friendlySpawnMessage / enemySpawnMessage for objects made after
	//     the round's start.
	// The team changes (0x00656e80 from a control point's setTeam 0x005cc320)
	// play only here, for the local player (game slot 0x60): to no team,
	// neutralTeamChangeMessage if it was the local team's; else
	// friendlyTeamChangeMessage if the local player is on the new team,
	// enemyTeamChangeMessage if not. (The silo's "_N" suffix from
	// SiloSyncEvent isn't added.) The weapons' fail and use messages are
	// GameWorld's (the players say them).
	// Returns the messages sent this tick (team, message), for tests.
	std::vector<std::pair<int, std::string>> autoRadio();

	// Counters, for --verbose and tests.
	int weaponSounds = 0, radioHeard = 0, voiceOvers = 0;

private:
	void weapons(const Camera& camera);
	void engines();
	void projectiles();
	void radio();
	void silos();
	const std::string& loopingSound(const void* projectile, const std::string& name);

	game::GameWorld& m_round;
	world::ObjectManager& m_objects;
	world::ObjectTemplateManager& m_templates;
	sound::SoundManager& m_manager;
	sound::GameSounds& m_sounds;
	sound::VoiceMessages* m_voices;
	std::map<std::string, std::string> m_looping; // projectile template -> its loop ("" none)
	struct Watched {
		int team = 0;
		float hitPoints = -1;
		bool damageArmed = true, destroyedArmed = true, repairedArmed = false;
		bool destroyed = false;
		const world::ObjectTemplate* objectTemplate = nullptr;
	};
	bool m_firstScan = true;
	std::map<const world::ObjectTemplate*, bool> m_radioTemplates; // whether a template has any of them
	std::map<uint32_t, Watched> m_watched; // objects whose template has radio messages
	int m_scanIn = 0;
	// The takeable radio waiting out radio.delay (timed event 2/0x52).
	std::vector<std::pair<float, std::string>> m_delayedRadio;
	struct SiloSounds {
		std::string weapon;                       // the firearm with the countdown
		std::vector<std::pair<int, float>> times; // (index, seconds before loaded)
		float lastLeft = -1;
	};
	std::map<uint32_t, SiloSounds> m_silos;
};

} // namespace dice::hfe
