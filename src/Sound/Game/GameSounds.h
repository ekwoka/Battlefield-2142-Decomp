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
	// unplaced). False when there is no such template.
	bool play(const std::string& templateName, const Vec3& position);
	// A first-person sound: at the listener, head-relative.
	bool playFirstPerson(const std::string& templateName, int sample = -1);

	// A looping sound kept on for an owner while it is refreshed each tick
	// (an engine's idle and rpm loops, a rocket's S_<projectile>_Looping):
	// placed at `position`, moving at `velocity`, its volume and pitch
	// envelopes read at `input` (the engine's rpm). Loops not refreshed
	// between two endTick calls fade out (stop type) and are released.
	void loop(uint64_t owner, const std::string& templateName, const Vec3& position, const Vec3& velocity,
		float input = 0);
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
	enum class WeaponEvent { Fire, Reload, Deploy, Zoom, SwitchFireRate };
	void weapon(WeaponEvent event, const std::string& weaponTemplate, bool firstPerson, const Vec3& position,
		bool muteOnFire = true);

	void stopAll();

private:
	class Transform;
	struct Loop;

	SoundManager& m_manager;
	bool m_log = false;
	std::map<std::pair<uint64_t, std::string>, std::unique_ptr<Loop>> m_loops;
	int m_tick = 0;
	int m_firingTick = -100; // muteOnFire's last shot
	std::map<std::string, int> m_lastDeploy;
};

} // namespace dice::hfe::sound
