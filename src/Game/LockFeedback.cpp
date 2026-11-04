// The lock-on feedback (GameWorld::LockEvent): BF2142.exe's target
// component, its update 0x005e1b30 and its fire handler 0x005e1ee0.
#include "GameWorld.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

namespace dice::hfe::game {

void GameWorld::lockEvent(LockEvent::Kind kind, const Player& player, uint32_t target)
{
	if (m_lockEvents.size() >= 64) {
		m_lockEvents.erase(m_lockEvents.begin());
	}
	m_lockEvents.push_back({kind, player.id, target, player.lock.system});
}

// The lock tones (weaponHud.firstLockSound / secondLockSound, the
// WeaponHud template's +0x80 / +0x9c, "" by default, constructor
// 0x006390e0): the HUD's lock box (update 0x007e6520, draw 0x007e5a10)
// works out each frame 0 (no target), 1 (tracking) or 2 (locked) for the
// local player's weapon and hands it to the weapon's runtime WeaponHud
// (slot 0x40, 0x00657ce0), which on a change only stops the other and
// starts its own: 1 the first sound, 2 the second, 0 both stopped. The
// sounds (TargetTrack / TargetLock, GameSounds.con: 2D, looping) play by
// name on the sound manager (slots 0x2c / 0x34). A weapon change, the zoom
// or fire-mode toggle (0x005c0da0, 0x005c0f40) and leaving the weapon
// force 0; here a weapon change or the player's death does.
void GameWorld::stepLockTone()
{
	const Player* player = m_player && m_player->alive ? m_player : nullptr;
	const std::string weapon = player ? player->lock.weapon : std::string();
	const auto sound = [this](const std::string& name, const char* key) {
		const world::ObjectTemplate* t = name.empty() ? nullptr : m_templates.find(name);
		const auto it = t ? t->settings().find(key) : decltype(t->settings().end()) {};
		return t && it != t->settings().end() ? it->second : std::string();
	};
	if (weapon != m_lockTone.weapon) {
		if (m_lockTone.state != 0) {
			for (const char* key : {"weaponHud.firstLockSound", "weaponHud.secondLockSound"}) {
				if (const std::string name = sound(m_lockTone.weapon, key); !name.empty()) {
					m_lockTones.push_back({name, false});
				}
			}
		}
		m_lockTone = {weapon, 0};
	}
	const int state = !player || player->lock.target == 0 ? 0 : player->lock.state == 2 ? 2 : 1;
	if (state == m_lockTone.state) {
		return;
	}
	m_lockTone.state = state;
	const std::string first = sound(weapon, "weaponHud.firstLockSound");
	const std::string second = sound(weapon, "weaponHud.secondLockSound");
	const auto stop = [this](const std::string& name) {
		if (!name.empty()) {
			m_lockTones.push_back({name, false});
		}
	};
	const auto play = [this](const std::string& name) {
		if (!name.empty()) {
			m_lockTones.push_back({name, true});
		}
	};
	if (state == 1) {
		stop(second);
		play(first);
	} else if (state == 2) {
		stop(first);
		play(second);
	} else {
		stop(first);
		stop(second);
	}
	if (m_lockTones.size() > 32) {
		m_lockTones.erase(m_lockTones.begin(), m_lockTones.end() - 32);
	}
}

void GameWorld::lockFeedback(Player& player, uint32_t targetBefore, int stateBefore)
{
	Player::Lock& lock = player.lock;
	const bool local = m_player && m_player == &player;
	// The target taken (0x005e1e70: +0x4c the tick, +0x44 cleared).
	if (lock.target != targetBefore && lock.target != 0) {
		lock.chosenAt = m_time;
		lock.announce = false;
	}
	// Its gone or destroyed locked target (0x0058e3e0 false): "target_lost".
	if (targetBefore != 0 && lock.target != targetBefore && stateBefore == 2) {
		const world::Object* was = m_objects.find(targetBefore);
		if (!was || was->destroyed()) {
			lockEvent(LockEvent::Lost, player, targetBefore);
			if (local && lock.system == 2) {
				hearRadio(player.id, 2, "target_lost");
			}
		}
	}
	// Locked (the lock delay +0x90 run out: slot 0xa4 with 1, +0x44 set).
	if (lock.target != 0 && lock.state == 2 && stateBefore != 2) {
		lock.announce = true;
	}
	// Said 35 ticks after the target was taken (+0x4c + 0x23).
	if (lock.announce && lock.target != 0 && m_time - lock.chosenAt > 35.0 / 30.0) {
		lock.announce = false;
		lockEvent(LockEvent::Acquired, player, lock.target);
		if (local && lock.system == 2) {
			hearRadio(player.id, 2, "target_aquired");
		}
	}
	if (lock.target == 0) {
		lock.announce = false;
	}
}

void GameWorld::missileAway(const Player& player)
{
	// 0x005e1ee0: a round leaving with a target; the local player's says
	// "missile_away" on TCPlayer (0xe) to the target's player.
	if (player.lock.target == 0) {
		return;
	}
	lockEvent(LockEvent::MissileAway, player, player.lock.target);
	if (m_player && m_player == &player) {
		const int targetPlayer = playerOf(player.lock.target);
		if (targetPlayer >= 0) {
			radioMessage(player.id, 14, "missile_away", targetPlayer);
		}
	}
}

} // namespace dice::hfe::game
