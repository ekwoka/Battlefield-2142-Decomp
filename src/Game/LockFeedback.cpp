// The lock-on feedback (GameWorld::LockEvent): BF2142.exe's target
// component, its update 0x005e1b30 and its fire handler 0x005e1ee0.
#include "GameWorld.h"
#include "World/Object.h"

namespace dice::hfe::game {

void GameWorld::lockEvent(LockEvent::Kind kind, const Player& player, uint32_t target)
{
	if (m_lockEvents.size() >= 64) {
		m_lockEvents.erase(m_lockEvents.begin());
	}
	m_lockEvents.push_back({kind, player.id, target, player.lock.system});
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
