#include "GameSounds.h"

#include "SoundManager.h"

#include <cstdio>
#include <cstdlib>

namespace dice::hfe::sound {

// A loop's place: the engine reads the matrix and velocity each update.
class GameSounds::Transform final : public ISoundTransform {
public:
	const Matrix4* worldTransform() override { return &matrix; }
	const Vec3* velocity() override { return &m_velocity; }

	Matrix4 matrix;
	Vec3 m_velocity;
};

struct GameSounds::Loop {
	ISound* sound = nullptr;
	Transform transform;
	float input = 0; // the envelopes' (read through the pointer)
	bool refreshed = true;
};

GameSounds::GameSounds(SoundManager& manager) : m_manager(manager)
{
	const char* log = std::getenv("BF2142_SOUND_LOG");
	m_log = log && *log && *log != '0';
}

GameSounds::~GameSounds() { stopAll(); }

void GameSounds::stopAll()
{
	for (auto& [key, loop] : m_loops) {
		if (loop->sound) {
			loop->sound->stopImmediately();
			loop->sound->release();
		}
	}
	m_loops.clear();
}

bool GameSounds::play(const std::string& templateName, const Vec3& position)
{
	if (!m_manager.hasSound(templateName)) {
		return false;
	}
	ISoundTemplate* sound = m_manager.soundTemplate(templateName);
	if (m_log) {
		std::printf("sound: %s at %.1f %.1f %.1f\n", templateName.c_str(), position.x, position.y, position.z);
	}
	if (onTrigger) {
		onTrigger({templateName, position, false, -1});
	}
	if (sound) {
		// A sound of its own, released at once: it plays out where it
		// started (Sound::detach) as an object's component sound would.
		if (ISound* instance = sound->createSound()) {
			instance->playAt(&position);
			instance->release();
		}
	}
	return true;
}

bool GameSounds::playFirstPerson(const std::string& templateName, int sample)
{
	if (!m_manager.hasSound(templateName)) {
		return false;
	}
	ISoundTemplate* sound = m_manager.soundTemplate(templateName);
	if (m_log) {
		std::printf("sound: %s (first person)\n", templateName.c_str());
	}
	if (onTrigger) {
		onTrigger({templateName, m_manager.listenerPosition(), true, sample});
	}
	if (sound) {
		if (ISound* instance = sound->createSound()) {
			instance->setHeadRelative(true);
			if (sample >= 0) {
				instance->playSampleLooped(sample, 1);
			} else {
				instance->playLooped(1);
			}
			instance->release();
		}
	}
	return true;
}

void GameSounds::loop(uint64_t owner, const std::string& templateName, const Vec3& position, const Vec3& velocity,
	float input)
{
	const auto key = std::make_pair(owner, templateName);
	auto it = m_loops.find(key);
	if (it == m_loops.end()) {
		if (!m_manager.hasSound(templateName)) {
			return;
		}
		ISoundTemplate* sound = m_manager.soundTemplate(templateName);
		if (m_log) {
			std::printf("sound: %s loops at %.1f %.1f %.1f\n", templateName.c_str(), position.x, position.y,
				position.z);
		}
		if (onTrigger) {
			onTrigger({templateName, position, false, -1});
		}
		auto loop = std::make_unique<Loop>();
		loop->input = input;
		loop->transform.matrix.m[3][0] = position.x;
		loop->transform.matrix.m[3][1] = position.y;
		loop->transform.matrix.m[3][2] = position.z;
		loop->transform.m_velocity = velocity;
		if (sound) {
			loop->sound = sound->createSound();
			if (loop->sound) {
				loop->sound->setTransform(&loop->transform);
				// The template's envelopes, read at the owner's value (the
				// engine audio's rpm, 0x007fe050).
				loop->sound->setVolumeEnvelope(nullptr, &loop->input);
				loop->sound->setPitchEnvelope(nullptr, &loop->input);
				loop->sound->play();
			}
		}
		it = m_loops.emplace(key, std::move(loop)).first;
	}
	Loop& loop = *it->second;
	loop.refreshed = true;
	loop.input = input;
	loop.transform.matrix.m[3][0] = position.x;
	loop.transform.matrix.m[3][1] = position.y;
	loop.transform.matrix.m[3][2] = position.z;
	loop.transform.m_velocity = velocity;
}

void GameSounds::endTick()
{
	++m_tick;
	for (auto it = m_loops.begin(); it != m_loops.end();) {
		Loop& loop = *it->second;
		if (loop.refreshed) {
			loop.refreshed = false;
			++it;
			continue;
		}
		if (loop.sound) {
			// The stop type's ending (an engine's start-stop sample plays
			// its tail), then the instance goes when that has played.
			loop.sound->stop();
			loop.sound->setVolumeEnvelope(nullptr, nullptr);
			loop.sound->setPitchEnvelope(nullptr, nullptr);
			loop.sound->release();
		}
		it = m_loops.erase(it);
	}
	// muteOnFire lasts while the weapon fires (0x005db6c0 clears it when
	// the trigger is let go): two ticks without a round.
	if (m_manager.muted(4) && m_tick - m_firingTick > 2) {
		m_manager.setMuted(4, false);
	}
}

void GameSounds::weapon(WeaponEvent event, const std::string& weaponTemplate, bool firstPerson, const Vec3& position,
	bool muteOnFire)
{
	const std::string prefix = "S_" + weaponTemplate + "_";
	const auto person = [&](const char* first, const char* third) {
		if (firstPerson) {
			playFirstPerson(prefix + first);
		} else {
			play(prefix + third, position);
		}
	};
	switch (event) {
	case WeaponEvent::Fire:
		if (firstPerson) {
			// Slot 1 (Fire1P_Outdoor) where the engine finds no roof over the
			// listener (manager slot 0x98, 0x007fa320), else slot 0.
			const bool outdoor = m_manager.hasSound(prefix + "Fire1P_Outdoor")
				&& m_manager.outdoors(m_manager.listenerPosition());
			playFirstPerson(prefix + (outdoor ? "Fire1P_Outdoor" : "Fire1P"));
			if (muteOnFire) {
				m_manager.setMuted(4, true);
				m_firingTick = m_tick;
			}
		} else {
			play(prefix + "Fire3P", position);
		}
		break;
	case WeaponEvent::Reload:
		person("Reload1P", "Reload3P");
		break;
	case WeaponEvent::Deploy: {
		// Not again within 9 ticks of the last (+0x48).
		const auto last = m_lastDeploy.find(weaponTemplate);
		if (last != m_lastDeploy.end() && m_tick - last->second <= 9) {
			break;
		}
		m_lastDeploy[weaponTemplate] = m_tick;
		person("Deploy1P", "Deploy3P");
		break;
	}
	case WeaponEvent::Zoom:
		// 0x005dc250: first person only.
		if (firstPerson) {
			playFirstPerson(prefix + "Zoom");
		}
		break;
	case WeaponEvent::SwitchFireRate:
		// 0x005dc290: first person only.
		if (firstPerson) {
			playFirstPerson(prefix + "SwitchFireRate");
		}
		break;
	}
}

} // namespace dice::hfe::sound
