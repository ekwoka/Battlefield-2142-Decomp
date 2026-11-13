#include "GameSounds.h"

#include "SoundManager.h"
#include "SoundObjectTemplate.h"
#include "Sound/SoundEngine/EnhancedAudio.h"
#include "Sound/SoundEngine/SoundEnvelope.h"

#include <algorithm>
#include <cctype>
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

// The template's volume envelope at x times a scale the owner keeps
// (0x007fe010).
class ScaledEnvelope final : public ISoundEnvelope {
public:
	float evaluate(float x, unsigned seed) override
	{
		const float y = base ? base->evaluate(x, seed) : 1.0f;
		return scale ? y * *scale : y;
	}
	ISoundEnvelope* base = nullptr;
	const float* scale = nullptr;
};

struct GameSounds::Loop {
	ISound* sound = nullptr;
	ScaledEnvelope scaled;
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
	for (auto& [user, state] : m_weapons) {
		release(state);
	}
	m_weapons.clear();
	for (auto& [key, loop] : m_loops) {
		if (loop->sound) {
			loop->sound->stopImmediately();
			loop->sound->release();
		}
	}
	m_loops.clear();
}

ISound* GameSounds::create(const std::string& templateName, uint32_t owner)
{
	ISoundTemplate* t = m_manager.soundTemplate(templateName);
	ISound* sound = t ? t->createSound() : nullptr;
	if (sound) {
		// 0x007f9340: the owner in parameter 0, 1 and 2 cleared, and a 3D
		// sound asks the manager how obstructed it is.
		sound->setParameter(0, owner);
		sound->setParameter(1, 0);
		sound->setParameter(2, 0);
		if (t->is3dSound()) {
			sound->setOcclusion(m_manager.occlusionHook());
		}
	}
	return sound;
}

const std::string& GameSounds::sameWords(const std::string& templateName) const
{
	const auto known = m_sameWords.find(templateName);
	if (known != m_sameWords.end()) {
		return known->second;
	}
	const auto lowercase = [](std::string text) {
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	};
	const auto words = [&lowercase](const std::string& name) {
		std::vector<std::string> list;
		size_t start = 0;
		for (size_t end; (end = name.find('_', start)) != std::string::npos; start = end + 1) {
			list.push_back(lowercase(name.substr(start, end - start)));
		}
		list.push_back(lowercase(name.substr(start)));
		std::sort(list.begin(), list.end());
		return list;
	};
	const std::vector<std::string> wanted = words(templateName);
	std::string found = templateName;
	for (const auto& t : m_manager.templates().templates()) {
		if (t && t->name().size() == templateName.size() && words(t->name()) == wanted && sampleCount(t->name()) > 0) {
			found = t->name();
			break;
		}
	}
	return m_sameWords[templateName] = found;
}

int GameSounds::sampleCount(const std::string& templateName) const
{
	const auto* t = dynamic_cast<const SoundObjectTemplate*>(m_manager.templates().find(templateName));
	if (!t || !t->hasFiles()) {
		return 0;
	}
	int count = 0;
	bool inName = false;
	for (const char c : t->soundFilenames()) {
		const bool separator = c == ',' || c == ' ' || c == '"';
		if (!separator && !inName) {
			++count;
		}
		inName = !separator;
	}
	return count;
}

bool GameSounds::play(const std::string& templateName, const Vec3& position, uint32_t owner)
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
		if (ISound* instance = create(templateName, owner)) {
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
	float input, uint32_t object, const float* volumeScale)
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
			loop->sound = create(templateName, object);
			if (loop->sound) {
				loop->sound->setTransform(&loop->transform);
				// The template's envelopes, read at the owner's value (the
				// engine audio's rpm, 0x007fe050).
				if (volumeScale) {
					loop->scaled.base = sound->volumeEnvelopeObject();
					loop->scaled.scale = volumeScale;
					loop->sound->setVolumeEnvelope(&loop->scaled, &loop->input);
				} else {
					loop->sound->setVolumeEnvelope(nullptr, &loop->input);
				}
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
	// Weapon components not heard from in a minute (players gone) go,
	// unless an automatic loop still waits for its trigger.
	if (m_tick % 64 == 0) {
		for (auto it = m_weapons.begin(); it != m_weapons.end();) {
			if (!it->second.looping && m_tick - it->second.lastTick > 1800) {
				release(it->second);
				it = m_weapons.erase(it);
			} else {
				++it;
			}
		}
	}
	// muteOnFire lasts while the weapon fires (0x005db6c0 clears it when
	// the trigger is let go): two ticks without a round.
	if (m_manager.muted(4) && m_tick - m_firingTick > 2) {
		m_manager.setMuted(4, false);
	}
}

void GameSounds::release(WeaponState& state)
{
	for (ISound*& sound : state.fire) {
		if (sound) {
			sound->stop();
			sound->release();
			sound = nullptr;
		}
	}
	for (ISound*& sound : state.reload) {
		if (sound) {
			sound->release();
			sound = nullptr;
		}
	}
	state.looping = false;
	state.loopSlot = -1;
}

void GameSounds::releaseTrigger(uint64_t user)
{
	// 0x005db6c0: a looping fire sound stops (its stop type's end), and
	// reason 4 is lifted.
	const auto it = m_weapons.find(user);
	if (it != m_weapons.end() && it->second.looping) {
		for (ISound*& sound : it->second.fire) {
			if (sound) {
				sound->stop();
				sound->release();
				sound = nullptr;
			}
		}
		it->second.looping = false;
		it->second.loopSlot = -1;
	}
	if (m_manager.muted(4)) {
		m_manager.setMuted(4, false);
	}
}

void GameSounds::fire(WeaponState& state, const std::string& prefix, bool firstPerson, const Vec3& position,
	bool muteOnFire, uint32_t object, const FireInfo& info)
{
	// Pick the perspective before checking an automatic loop. The local
	// player can change camera while holding fire; its old loop must stop.
	int slot = 2;
	// DefaultSoundComp creates all event templates even when a mod leaves
	// one unconfigured. In particular the vanilla heavy walker's main cannon
	// has an empty Fire1P_Outdoor template but a configured Fire1P sample.
	// Template existence alone must not select that silent variant.
	const bool outdoorSound = sampleCount(prefix + "Fire1P_Outdoor") > 0;
	// Enhanced audio: a 1P sound filed under the same words (a port addition).
	const std::string fire1P = enhancedAudioEnabled() && firstPerson && !outdoorSound && sampleCount(prefix + "Fire1P") == 0
		? sameWords(prefix + "Fire1P")
		: prefix + "Fire1P";
	const bool firstPersonSound = sampleCount(fire1P) > 0;
	if (firstPerson && (outdoorSound || firstPersonSound)) {
		slot = outdoorSound && m_manager.outdoors(m_manager.listenerPosition()) ? 1 : 0;
	}
	if (state.looping && state.loopSlot != slot) {
		const int oldSlot = state.loopSlot;
		for (ISound*& sound : state.fire) {
			if (sound) {
				sound->stopImmediately();
				sound->release();
				sound = nullptr;
			}
		}
		state.looping = false;
		state.loopSlot = -1;
		if (oldSlot != 2 && m_manager.muted(4)) {
			m_manager.setMuted(4, false);
		}
	}
	// 0x005dbdc0. Sounds a burst already played are skipped (+0x3c); an
	// automatic loop under way (+0x43) plays on.
	if (state.skip > 0) {
		--state.skip;
		return;
	}
	if (state.looping) {
		return;
	}
	// The slot: first person selects Fire1P / Fire1P_Outdoor; otherwise
	// Fire3P. First-person fire ducks the world's effects (reason 4).
	// 0x005dbec3..0x005dbee7: a local player's weapon sets reason 4 (muteOnFire) before the
	// sample is looked at, so a gun with no 1P sample still ducks the world's effects.
	// No 1P sample (the right Shepherd gunner's Fire1P is misnamed in both mods'
	// eu_at.tweak): retail 0x005dbdc0 picks slot 0/1 on first person alone and plays
	// nothing. Enhanced audio (a port addition) lets the 3P sample stand in, kept
	// head-relative so occlusion and HRTF don't treat the player's own gun as distant.
	const bool head = firstPerson;
	if (head && muteOnFire) {
		m_manager.setMuted(4, true);
	}
	if (firstPerson && slot == 2 && !enhancedAudioEnabled()) {
		return;
	}
	static const char* const kSlots[3] = {"Fire1P", "Fire1P_Outdoor", "Fire3P"};
	const std::string name = slot == 0 ? fire1P : prefix + kSlots[slot];
	const int samples = sampleCount(name);
	int sample = -1, loops = 1;
	bool keep = false;
	if (samples == 1) {
		if (info.mode == 2) {
			// Automatic: the sample loops until the trigger is let go.
			state.looping = true;
			state.loopSlot = slot;
			keep = true;
			loops = 0;
		} else if (info.burstRounds > 1) {
			// A burst: a slow gun (+0x42, under 301 rounds a minute) a
			// sound a round; a fast one plays the sample once for each
			// round now and skips the burst's other rounds (+0x3c).
			if (info.roundsPerMinute >= 301) {
				loops = std::min(info.burstRounds, 0xffff);
				state.skip = info.burstRounds - 1;
			}
		}
	} else if (samples == 4) {
		// Four samples (the grenades' throws): 1 while the fire state is 2
		// (held), and at the end 0 (a single), or after a hold 2, or 3 when
		// the fire component's slot 0xe0 value is at least 0.5 (it is
		// always 1: after a hold, 3).
		if (info.held) {
			state.burst = true;
			sample = 1;
		} else {
			sample = state.burst ? (info.heldValue >= 0.5f ? 3 : 2) : 0;
			state.burst = false;
		}
	} else {
		return; // two, three or no samples: nothing (0x005dbdc0 plays only 1 or 4)
	}
	if (!m_manager.hasSound(name)) {
		return;
	}
	if (m_log) {
		std::printf("sound: %s%s%s\n", name.c_str(), head ? " (first person)" : "", loops == 0 ? " loops" : "");
	}
	if (onTrigger) {
		onTrigger({name, head ? m_manager.listenerPosition() : position, head, sample});
	}
	ISound* sound = create(name, head ? 0 : object);
	if (!sound) {
		return;
	}
	if (keep && state.fire[static_cast<size_t>(slot)]) {
		state.fire[static_cast<size_t>(slot)]->stopImmediately();
		state.fire[static_cast<size_t>(slot)]->release();
		state.fire[static_cast<size_t>(slot)] = nullptr;
	}
	if (head) {
		sound->setHeadRelative(true);
		if (sample >= 0) {
			sound->playSampleLooped(sample, 1);
		} else {
			sound->playLooped(loops);
		}
	} else if (sample >= 0) {
		sound->playSampleAtLooped(&position, sample, 1);
	} else {
		sound->playAtLooped(&position, loops);
	}
	if (keep) {
		if (state.fire[static_cast<size_t>(slot)]) {
			state.fire[static_cast<size_t>(slot)]->stop();
			state.fire[static_cast<size_t>(slot)]->release();
		}
		state.fire[static_cast<size_t>(slot)] = sound;
	} else {
		sound->release();
	}
}

void GameSounds::reload(WeaponState& state, const std::string& prefix, bool firstPerson, const Vec3& position,
	uint32_t object, int reloadState)
{
	// 0x005dbfe0: state 2 nothing; Reload1P in first person, else
	// Reload3P. With one or two samples sample 0 unless that sound still
	// plays; event 7 sample 1 of two; three samples: a shell reload's
	// first (4) sample 0, its middle (5) sample 1, its last (6) sample 1
	// after a first, else 0. A magazine (3) with three samples: nothing.
	// The burst's skips end (+0x3c = 0).
	if (reloadState == 2) {
		return;
	}
	state.reloaded = true;
	const int slot = firstPerson ? 0 : 1;
	const std::string name = prefix + (firstPerson ? "Reload1P" : "Reload3P");
	state.skip = 0;
	if (!m_manager.hasSound(name)) {
		return;
	}
	const int samples = sampleCount(name);
	int sample = -1;
	ISound*& kept = state.reload[static_cast<size_t>(slot)];
	if (reloadState == 7) {
		if (samples != 2) {
			return;
		}
		sample = 1;
	} else if (samples == 1 || samples == 2) {
		if (kept && kept->isPlaying()) {
			return;
		}
		sample = 0;
	} else if (samples == 3) {
		switch (reloadState) {
		case 4:
			state.reloadPart = 1;
			sample = 0;
			break;
		case 5:
			sample = 1;
			break;
		case 6:
			sample = state.reloadPart != 0 ? 1 : 0;
			state.reloadPart = 2;
			break;
		default:
			return;
		}
	} else {
		return;
	}
	if (m_log) {
		std::printf("sound: %s sample %d%s\n", name.c_str(), sample, firstPerson ? " (first person)" : "");
	}
	if (onTrigger) {
		onTrigger({name, firstPerson ? m_manager.listenerPosition() : position, firstPerson, sample});
	}
	ISound* sound = create(name, firstPerson ? 0 : object);
	if (!sound) {
		return;
	}
	if (firstPerson) {
		sound->setHeadRelative(true);
		sound->playSampleLooped(sample, 1);
	} else {
		sound->playSampleAtLooped(&position, sample, 1);
	}
	if (kept) {
		kept->release();
	}
	kept = sound;
}

void GameSounds::weapon(WeaponEvent event, const std::string& weaponTemplate, bool firstPerson, const Vec3& position,
	bool muteOnFire, uint64_t user, uint32_t object, const FireInfo* fireInfo, int reloadState)
{
	const std::string prefix = "S_" + weaponTemplate + "_";
	WeaponState& state = m_weapons[user];
	if (state.weapon != weaponTemplate) {
		// Another weapon in hand: the last one's component is done with.
		release(state);
		state = WeaponState {};
		state.weapon = weaponTemplate;
	}
	state.lastTick = m_tick;
	const auto person = [&](const char* first, const char* third) {
		if (firstPerson) {
			playFirstPerson(prefix + first);
		} else {
			play(prefix + third, position, object);
		}
	};
	switch (event) {
	case WeaponEvent::Fire:
		if (firstPerson && muteOnFire) {
			m_firingTick = m_tick;
		}
		fire(state, prefix, firstPerson, position, muteOnFire, object, fireInfo ? *fireInfo : FireInfo {});
		break;
	case WeaponEvent::Reload:
		reload(state, prefix, firstPerson, position, object, reloadState);
		break;
	case WeaponEvent::Deploy: {
		state.reloaded = false; // 0x005dc1d0 clears +0x41 with each draw
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
	case WeaponEvent::TriggerClick:
		// 0x005dc160: the first click after a reload event since the draw
		// (+0x41) plays BoltClick (its +0x1c, in either view), else
		// TriggerClick (+0x20) in first person only; +0x41 is cleared.
		if (std::exchange(state.reloaded, false)) {
			person("BoltClick", "BoltClick");
		} else if (firstPerson) {
			playFirstPerson(prefix + "TriggerClick");
		}
		break;
	}
}

} // namespace dice::hfe::sound
