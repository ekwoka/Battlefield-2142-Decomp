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

	// The Radio component's automatic messages (its properties, the Radio
	// template component 0xc4b6, script names radio.*): BF2142.exe's
	// senders of these weren't traced, so the rewrite sends them where the
	// property names say, on TCServerTeamMessage (0xd) from no one (a
	// commander's automatic line, 0x004a5cf0's server send):
	//   team change to a team: friendlyTeamChangeMessage to it,
	//     enemyTeamChangeMessage to the other; to none:
	//     neutralTeamChangeMessage to the old owner;
	//   destroyed: friendlyDestroyedMessage to its team,
	//     enemyDestroyedMessage to the other.
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
		bool destroyed = false;
	};
	std::map<uint32_t, Watched> m_watched; // objects whose template has radio messages
	int m_scanIn = 0;
	struct SiloSounds {
		std::string weapon;                       // the firearm with the countdown
		std::vector<std::pair<int, float>> times; // (index, seconds before loaded)
		float lastLeft = -1;
	};
	std::map<uint32_t, SiloSounds> m_silos;
};

} // namespace dice::hfe
