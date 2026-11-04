// The sounds the game's objects make, played through the sound manager's
// engine: one-shots where something happens (a shot, a footstep, an
// effect bundle's S_<bundle>_Start) and loops that follow an object
// (engines, projectiles in flight). The listener is the manager's
// (SoundManager::setListener).
//
// In BF2142.exe each object's sound components own ISound instances of
// their templates (the S_<owner>_<event> children SoundManager creates,
// 0x0060e910), play them from their event handlers and give them the
// object as their ISoundTransform. The rewrite's world doesn't carry
// those components, so this keeps the instances by owner instead and is
// fed the events (GameWorld::takeWeaponSounds, the effect bundles'
// started sounds, the vehicles' engines) once a tick.
//
// Every sound asked for is also passed to `onTrigger` (and printed when
// BF2142_SOUND_LOG is set), with or without an engine, so the triggers can
// be tested and traced headless.
#pragma once

#include "Sound/SoundEngine/ISoundEngine.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dice::hfe::sound {

class SoundManager;

class GameSounds {
public:
	explicit GameSounds(SoundManager& manager);
	~GameSounds();
	GameSounds(const GameSounds&) = delete;
	GameSounds& operator=(const GameSounds&) = delete;

	struct Triggered {
		std::string name;  // the Sound template
		Vec3 position;
		bool firstPerson = false; // played at the listener (head-relative)
		int sample = -1;          // a chosen sample, -1 the template's choice
	};
	std::function<void(const Triggered&)> onTrigger;

	// A one-shot of a Sound template at a point (a 2D template plays
	// unplaced). False when there is no such template. A 3D sound gets the
	// manager's obstruction hook with `owner` (its object's root, 0 none)
	// in parameter 0, as the manager's slot 7 (0x007f9340) sets them up.
	bool play(const std::string& templateName, const Vec3& position, uint32_t owner = 0);
	// A first-person sound: at the listener, head-relative.
	bool playFirstPerson(const std::string& templateName, int sample = -1);

	// A looping sound kept on for an owner while it is refreshed each tick
	// (an engine's idle and rpm loops, a rocket's S_<projectile>_Looping):
	// placed at `position`, moving at `velocity`, its volume and pitch
	// envelopes read at `input` (the engine's rpm). Loops not refreshed
	// between two endTick calls fade out (stop type) and are released.
	// `volumeScale` (kept by the caller) multiplies the template's volume
	// envelope (the new-style engine audio's crossfade, vtable 0x009bfa84
	// 0x007fe010: the envelope at x times a scale).
	void loop(uint64_t owner, const std::string& templateName, const Vec3& position, const Vec3& velocity,
		float input = 0, uint32_t object = 0, const float* volumeScale = nullptr);
	void endTick();
	size_t loops() const { return m_loops.size(); }
	bool looping(uint64_t owner, const std::string& templateName) const
	{
		return m_loops.count({owner, templateName}) != 0;
	}

	// The DefaultSoundComp's handlers (vtable 0x00971b48) for a weapon
	// template: its slots +0x10.. are S_<weapon>_Fire1P, Fire1P_Outdoor,
	// Fire3P, BoltClick, TriggerClick, SwitchFireRate, Reload1P, Reload3P,
	// Deploy1P, Deploy3P, Zoom (0x005db760's order). `firstPerson` is the
	// owner seen from its own eyes (0x005c2d80 slot 0x1bc).
	//   fire 0x005dbdc0: Fire1P (Fire1P_Outdoor where the engine says the
	//     listener is outdoors) in first person, else Fire3P, one loop;
	//     with sound.muteOnFire (default on, 0x005dbc00 writes only the 0)
	//     the world's effects duck while it fires (reason 4).
	//   reload 0x005dbfe0: Reload1P or Reload3P.
	//   deploy 0x005dc1d0: Deploy1P or Deploy3P, at most once in 10 ticks.
	//   click 0x005dc160 (the fire attempt 0x005c2ee0 on an empty magazine):
	//     TriggerClick, first person only.
	enum class WeaponEvent { Fire, Reload, Deploy, Zoom, SwitchFireRate, TriggerClick };
	// The fire event's fields the sound reads (0x005c3cb0's record): the
	// fire component's mode (slot 0xdc: 0 single, 1 burst, 2 automatic,
	// +0x18), the rounds the burst will fire (0x005c3b50, +0x14), whether
	// the fire state is 2, still held (+0x1d), and its slot 0xe0 value
	// (+0x38: 0x005b6b10 in every fire component, 1). roundsPerMinute is
	// the fire template's (+0x3c, slot 0x11c 0x005f16d0), which the
	// component's set-up (0x005dc450, from 0x005c2bb0) turns into its
	// +0x42 flag: under 301, one sound a round of a burst.
	struct FireInfo {
		int mode = 0;
		int burstRounds = 1;
		bool held = false;
		float heldValue = 1;
		int roundsPerMinute = 600;
	};
	// `user` keys the weapon's sound component (who holds it), `object`
	// the root its 3D sounds belong to. `reloadState` is the ammo
	// component's (slot 0xd8: 3 a magazine, 4-6 a shell reload's first,
	// middle and last, 7 a reload's second half).
	void weapon(WeaponEvent event, const std::string& weaponTemplate, bool firstPerson, const Vec3& position,
		bool muteOnFire = true, uint64_t user = 0, uint32_t object = 0, const FireInfo* fire = nullptr,
		int reloadState = 3);
	// The trigger let go (0x005db6c0): an automatic weapon's looping fire
	// sounds stop and the first-person muting (reason 4) ends.
	void releaseTrigger(uint64_t user);
	// Samples of a Sound template (its soundFilename list), 0 for none.
	int sampleCount(const std::string& templateName) const;
	// Whether a user's automatic fire loop plays (for tests).
	bool fireLooping(uint64_t user) const
	{
		const auto it = m_weapons.find(user);
		return it != m_weapons.end() && it->second.looping;
	}

	void stopAll();

private:
	class Transform;
	struct Loop;

	// A weapon sound component's state (the object part of vtable
	// 0x00971aa8): its instances by slot (+0x10.., the 1P / 1P_Outdoor /
	// 3P fire and the 1P / 3P reload kept), sounds to skip (+0x3c), the
	// reload's part flag (+0x40), an automatic loop playing (+0x43) and a
	// four-sample burst under way (+0x44).
	struct WeaponState {
		std::string weapon;
		std::array<ISound*, 3> fire {};
		std::array<ISound*, 2> reload {};
		int skip = 0;
		int reloadPart = 0;
		bool looping = false;
		bool burst = false;
		int lastTick = 0;
	};
	ISound* create(const std::string& templateName, uint32_t owner);
	void release(WeaponState& state);
	void fire(WeaponState& state, const std::string& prefix, bool firstPerson, const Vec3& position, bool muteOnFire,
		uint32_t object, const FireInfo& info);
	void reload(WeaponState& state, const std::string& prefix, bool firstPerson, const Vec3& position, uint32_t object,
		int reloadState);

	SoundManager& m_manager;
	std::map<uint64_t, WeaponState> m_weapons;
	bool m_log = false;
	std::map<std::pair<uint64_t, std::string>, std::unique_ptr<Loop>> m_loops;
	int m_tick = 0;
	int m_firingTick = -100; // muteOnFire's last shot
	std::map<std::string, int> m_lastDeploy;
};

} // namespace dice::hfe::sound
