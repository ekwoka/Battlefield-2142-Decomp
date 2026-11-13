// The game's side of sound once a tick: what the round did, handed to the
// sound layer (src/Sound/Game) the way BF2142.exe's objects drive their
// own sound components.
//   - the listener at the camera (the sound manager's update, 0x007f9d50);
//   - the weapons' fire, reload and deploy sounds (GameWorld's weapon
//     sound events; DefaultSoundComp, 0x005dbdc0 / 0x005dbfe0 / 0x005dc1d0);
//   - the engines' idle and rpm loops of the vehicles that have someone
//     aboard (the engine audio, 0x007fe050: S_<engine>_Idle, _Rpm1, _Rpm2
//     at |rpm| x 0.8333, 0x007fe740), while the engine runs;
//   - the vehicles' rolling-surface loops (groundContactVolume:
//     GameWorld::rollingSounds);
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

#include "Game/GameWorld.h"
#include "Sound/SoundEngine/SoundMath.h"

#include <cstdint>
#include <map>
#include <set>
#include <functional>
#include <string>
#include <vector>

namespace dice::hfe {
namespace game {
class GameWorld;
}
namespace host {
class HudWorld;
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
class ISound;
}

class GameAudio {
public:
	GameAudio(game::GameWorld& round, world::ObjectManager& objects, world::ObjectTemplateManager& templates,
		sound::SoundManager& manager, sound::GameSounds& sounds, sound::VoiceMessages* voices);
	~GameAudio();
	GameAudio(const GameAudio&) = delete;
	GameAudio& operator=(const GameAudio&) = delete;

	struct Camera {
		sound::Vec3 position, forward {0, 0, 1}, up {0, 1, 0}, velocity;
		bool firstPerson = false; // the local player's own eyes (its weapon's 1P sounds)
	};
	// A sound some effect started this tick (S_<bundle>_Start).
	void effectSound(const std::string& name, const sound::Vec3& position);
	// An effect bundle's looping start sound, kept on while it is asked for
	// each tick, its envelopes read at the bundle's sound intensity
	// (EffectManager::bundleLoops).
	void effectLoop(uint64_t key, const std::string& name, const sound::Vec3& position, float intensity);
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
	// enemyTeamChangeMessage if not, a missile silo's with its "_N"
	// (siloSuffix). The weapons' fail and use messages are
	// GameWorld's (the players say them).
	// Returns the messages sent this tick (team, message), for tests.
	std::vector<std::pair<int, std::string>> autoRadio();

	// Every radio message the local player hears, as it is heard (the
	// name tags' calls).
	std::function<void(int sender, const std::string& voice)> onHeard;
	// The emote of each radio message the local player hears from a sender
	// on foot (0x0042e0c0: the message's addEmoteTrigger, 0x004d7fe0, to
	// the sender's soldier's slot 0x88, 0x005edbf0; GameWorld::playEmote).
	std::function<void(int sender, const std::string& emote)> onEmote;
	// The text of each radio message the local player hears (the sample's
	// text id, 0x0042e0c0 -> 0x0042d7f0 -> 0x0042bef0: the HUD's chat
	// list with the sender, slot 0x158 -> ChatList 0x007b4cd0, or without
	// one, slot 0x15c): the lock-on lines ("target_aquired", "target_lost",
	// "missile_away", 0x005e1b30 / 0x005e1ee0) among them.
	std::function<void(int sender, int channel, const std::string& textId)> onRadioText;

	// The SiloSyncEvent's suffix (0x00514030: "_%d", DAT_00928708, of the
	// silo's index, its ancestor's slot 0x38, into the Radio's +0x18) that
	// the team-change messages (0x00656e80) end with: "_<index>" for a
	// missile silo, else "".
	// The ticket state's announcement (0x00449070, from the state change at
	// 0x0044b7e8 with state % 20): "" for none.
	static std::string ticketStateMessage(int team, int state, int localTeam, const std::string& gameMode);
	static std::string siloSuffix(int index) { return index > 0 ? "_" + std::to_string(index) : std::string(); }

	// Who the local player is and what the players are, for the radio
	// (heard and the local plays): a joined game's (HudWorld.h's
	// RemoteHudWorld: the server's players), else the round's.
	void setHudWorld(const host::HudWorld* world) { m_hud = world; }

	// Counters, for --verbose and tests.
	int weaponSounds = 0, radioHeard = 0, voiceOvers = 0, materialRadios = 0;

	// What the listener was given this tick (tests, --verbose): the local
	// player's seat PlayerControlObject (empty on foot or without one),
	// its interior loop ("" none), the listener-3d-obstruction tweak, the
	// wind wanted and the inside-Titan flag.
	struct ListenerState {
		std::string seat;
		std::string ambient;
		float listenerObstruction = 0;
		bool windWanted = false;
		bool insideTitan = false;
		int insideTitanTeam = 0; // the team of the Titan the listener is in
		bool panting = false;
		int injuries = 0;
	};
	const ListenerState& listenerState() const { return m_listenerState; }
	// The new-style engine audio's (audio.newStyleAudio, 0x007fe740) state
	// of one engine and its step, for tests: `on` the throttle held,
	// `clutch` 0..1, `rpm` the engine's, `gear`, `speed` the root's,
	// `clock` seconds; gives the envelopes' input and the two rpm loops'
	// volume scales (Rpm1, Rpm2).
	struct NewStyleEngine {
		float throttleBlend = 0;      // +0x78
		float jitter = 0;             // +0x80
		int jitterCountdown = 0;      // +0x7c
		float smoothed = 0, smoothedSpeed = 0; // +0x84, +0x94
		int gear = -1, lastGear = 1;  // +0x90
		float swayLeft = -1;          // +0x8c
		uint32_t random = 0x1234567u;
		bool shifted = false;         // the Load sound's cue this step
		float input = 0, rpm1Scale = 1, rpm2Scale = 0;
	};
	static void stepNewStyle(NewStyleEngine& engine, bool on, float clutch, float rpm, int gear, float speed,
		double clock);

private:
	void listener(Camera& camera);
	void interior(const game::GameWorld::PlayerInfo& me, bool firstPerson);
	void soldierHud(const game::GameWorld::PlayerInfo& me, bool alive);
	void materialRadio();
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
	const host::HudWorld* m_hud = nullptr;
	int localPlayer() const;
	bool playerInfo(int player, game::GameWorld::PlayerInfo& out) const;
	std::map<std::string, std::string> m_looping; // projectile template -> its loop ("" none)
	struct Watched {
		int team = 0;
		float hitPoints = -1;
		bool damageArmed = true, destroyedArmed = true, repairedArmed = false;
		bool destroyed = false;
		bool titanPart = false; // a Titan's console or core: no world object of its own
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
	ListenerState m_listenerState;
	// The interior loops of the local player's seat (S_<pco>_Ambient /
	// _AmbientDamaged, PlayerControlObject +0x2fc / +0x300; 2D loops made
	// by 0x005c6dc0), their envelopes read at +0x304 (the root's speed).
	struct Interior {
		uint32_t vehicle = 0;
		int seat = -2;
		std::string pco, ambientName, damagedName;
		sound::ISound* ambient = nullptr;
		sound::ISound* damaged = nullptr;
		float obstruction = 0, damagedLimit = 0.5f, maxHitPoints = 0;
		float speedInput = 0; // +0x304
	} m_interior;
	void releaseInterior();
	// The soldier HUD's sounds (the vehicleHud object part 0x009890c0):
	// panting's latch (+0x10) and the injury's last fraction (+0x1cc) and
	// timer (+0xb8).
	bool m_panting = false;
	std::string m_pantingSound;
	float m_lastHealth = -1, m_injuryTimer = -1;
	// Who is firing (the automatic loops' release, 0x005db6c0) and when
	// their last round left (ticks).
	struct Trigger {
		bool firing = false;
		int lastRound = -1000;
		int interval = 2;
	};
	std::map<uint64_t, Trigger> m_triggers;
	int m_tick = 0;
	std::map<uint64_t, NewStyleEngine> m_newStyle;
	std::map<std::string, bool> m_newStyleEngines; // engine template -> audio.newStyleAudio
	std::map<std::pair<const void*, int>, double> m_materialRadioNext; // the cells' next times (0x0060c5e0)
	int m_materialScanIn = 0;
};

} // namespace dice::hfe
