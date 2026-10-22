#include "Ambience.h"

#include "BfSoundEngine.h"
#include "SoundTweaks.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::sound {

namespace {

class NullSound final : public ISound {
public:
	int addRef() override { return 1; }
	int release() override { return 1; }
	void setPlayFlags(unsigned) override {}
	unsigned playFlags() override { return 0; }
	void playLooped(int) override {}
	void play() override {}
	void playAtLooped(const Vec3*, int) override {}
	void playAt(const Vec3*) override {}
	bool playSampleLooped(int, int) override { return false; }
	bool playSample(int) override { return false; }
	bool playSampleAtLooped(const Vec3*, int, int) override { return false; }
	bool playSampleAt(const Vec3*, int) override { return false; }
	void fadeOut(int) override {}
	void stop() override {}
	void stopImmediately() override {}
	bool isPlaying() override { return false; }
	void setTransform(ISoundTransform*) override {}
	ISoundTransform* transform() override { return nullptr; }
	void setVolumeEnvelope(ISoundEnvelope*, const float*) override {}
	ISoundEnvelope* volumeEnvelope() override { return nullptr; }
	void setPitchEnvelope(ISoundEnvelope*, const float*) override {}
	ISoundEnvelope* pitchEnvelope() override { return nullptr; }
	void setOcclusion(ISoundOcclusion*) override {}
	ISoundOcclusion* occlusion() override { return nullptr; }
	void setHeadRelative(bool) override {}
	bool headRelative() override { return false; }
	void setParameter(int, unsigned) override {}
	unsigned parameter(int) override { return 0; }
	const float* sampleTiming(int) override { return nullptr; }
	ISoundTemplate* soundTemplate() override { return nullptr; }
	void setFading(bool) override {}
	bool isFading() override { return false; }
	bool updateFade(int) override { return false; }
	void* destroy(unsigned) override { return this; }
};

NullSound g_nullSound;


} // namespace

ISound& nullSound() { return g_nullSound; }

// --- AmbientCrossfader ------------------------------------------------------

AmbientCrossfader::AmbientCrossfader()
{
	m_indoorSpeed = soundTweaks().get(Tweak::AmbientIndoorObstructionSpeed);
	const EnvelopePoint fadeIn[] = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
	const EnvelopePoint fadeOut[] = {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
	m_fadeIn = std::make_unique<SoundEnvelope>(0.0f, 1.0f, 0.0f, 1.0f, false, fadeIn, 2);
	m_fadeOut = std::make_unique<SoundEnvelope>(0.0f, 1.0f, 0.0f, 1.0f, false, fadeOut, 2);
}

AmbientCrossfader::~AmbientCrossfader()
{
	stop();
}

bool AmbientCrossfader::usesFadeEnvelope(ISound* sound)
{
	ISoundEnvelope* envelope = sound->volumeEnvelope();
	return !envelope || envelope == m_fadeIn.get() || envelope == m_fadeOut.get();
}

void AmbientCrossfader::restoreEnvelope(ISound* sound)
{
	ISoundEnvelope* envelope = sound->volumeEnvelope();
	if (envelope == m_fadeIn.get() || envelope == m_fadeOut.get()) {
		sound->setVolumeEnvelope(nullptr, nullptr);
	}
}

void AmbientCrossfader::releaseSound(ISound*& sound)
{
	if (!sound) {
		return;
	}
	sound->stop();
	restoreEnvelope(sound);
	sound->setOcclusion(nullptr);
	sound->release();
	sound = nullptr;
}

void AmbientCrossfader::stop()
{
	releaseSound(m_current);
	releaseSound(m_next);
	releaseSound(m_queued);
}

void AmbientCrossfader::setAmbient(ISound* sound, float fadeSeconds)
{
	if (!sound) {
		sound = &nullSound();
	}
	if (!m_next) {
		startCrossfade(sound, fadeSeconds);
	}
	else if (sound != m_next) {
		sound->addRef();
		if (m_queued) {
			m_queued->release();
		}
		m_queued = sound;
		m_queuedFadeSeconds = fadeSeconds;
	}
}

void AmbientCrossfader::startCrossfade(ISound* sound, float fadeSeconds)
{
	if (!sound) {
		releaseSound(m_current);
		releaseSound(m_next);
	}
	else {
		if (sound == m_current) {
			return;
		}
		// Different emitters sharing the same file keep playing seamlessly.
		if (m_current) {
			ISoundTemplate* incoming = sound->soundTemplate();
			ISoundTemplate* outgoing = m_current->soundTemplate();
			if (incoming && outgoing && incoming->loadedSoundFile(0) == outgoing->loadedSoundFile(0)) {
				return;
			}
		}
		m_fadeProgress = 0.0f;
		m_indoorSpeed = soundTweaks().get(Tweak::AmbientIndoorObstructionSpeed);
		if (m_current && fadeSeconds > 0.0f && usesFadeEnvelope(m_current)) {
			m_current->setVolumeEnvelope(m_fadeOut.get(), &m_fadeProgress);
			m_current->setOcclusion(this);
		}
		m_next = sound;
		sound->addRef();
		if (fadeSeconds > 0.0f && usesFadeEnvelope(m_next)) {
			m_next->setVolumeEnvelope(m_fadeIn.get(), &m_fadeProgress);
			m_next->setOcclusion(this);
		}
		if (ISoundTemplate* templ = m_next->soundTemplate()) {
			templ->loadedSoundFile(0);
		}
		m_next->play();
	}
	if (m_current || m_next) {
		m_fadeStart = soundEngine().now();
		m_fadeProgress = 0.0f;
		m_fadeEnd = m_fadeStart + static_cast<int>(std::lround(fadeSeconds * 30.0f));
	}
}

void AmbientCrossfader::update(int ticks)
{
	const int now = soundEngine().now();
	if (m_fadeEnd >= 0) {
		if (now < m_fadeEnd) {
			if (m_fadeStart <= now) {
				m_fadeProgress = static_cast<float>(now - m_fadeStart) / static_cast<float>(m_fadeEnd - m_fadeStart);
			}
		}
		else {
			releaseSound(m_current);
			if (m_next) {
				m_current = m_next;
				m_next = nullptr;
				restoreEnvelope(m_current);
			}
			m_fadeEnd = -1;
			m_fadeProgress = 0.0f;
			if (m_queued) {
				if (m_queued != m_current) {
					startCrossfade(m_queued, m_queuedFadeSeconds);
				}
				m_queued->release();
				m_queued = nullptr;
			}
		}
	}

	// Muffle the area ambience while indoors (a global ambient is unaffected).
	const bool indoors =
		soundEngine().indoorLevel() > 0.0f || soundTweaks().get(Tweak::Listener3dObstruction) > 0.0f;
	m_indoorTarget = (soundEngine().ambientEmitters().global() != m_current && indoors)
		? soundTweaks().get(Tweak::AmbientIndoorObstruction)
		: 0.0f;

	// Ease towards the target without overshooting.
	const float delta = m_indoorTarget - m_indoorCurrent;
	if (std::fabs(delta) >= 1e-5f) {
		float step = static_cast<float>(ticks) * m_indoorSpeed * delta;
		if ((delta > 0.0f && step > delta) || (delta < 0.0f && step < delta)) {
			step = delta;
		}
		m_indoorCurrent += step;
		if (std::fabs(m_indoorCurrent) < 0.001f) {
			m_indoorCurrent = 0.0f;
		}
	}
}

// --- AmbientEmitters --------------------------------------------------------

void AmbientEmitters::add(ISoundTemplate* emitter)
{
	if (!emitter) {
		return;
	}
	emitter->setIs3dSound(false);
	emitter->setReverbLevel(0.0f);
	m_emitters.push_back(emitter->createSound());
}

void AmbientEmitters::remove(ISoundTemplate* emitter)
{
	for (auto it = m_emitters.begin(); it != m_emitters.end(); ++it) {
		if ((*it)->soundTemplate() == emitter) {
			(*it)->release();
			m_emitters.erase(it);
			return;
		}
	}
}

void AmbientEmitters::clear()
{
	for (ISound* sound : m_emitters) {
		sound->release();
	}
	m_emitters.clear();
	if (m_global) {
		m_global->release();
		m_global = nullptr;
	}
}

void AmbientEmitters::setGlobal(ISound* sound)
{
	if (sound == m_global) {
		return;
	}
	if (sound) {
		sound->addRef();
	}
	if (m_global) {
		m_global->release();
		m_global = nullptr;
		m_globalChanged = true;
	}
	m_global = sound;
}

void AmbientEmitters::sortByRadius()
{
	std::sort(m_emitters.begin(), m_emitters.end(), [](ISound* a, ISound* b) {
		return a->soundTemplate()->minDistance() > b->soundTemplate()->minDistance();
	});
}

void AmbientEmitters::update(const Vec3* listenerPosition, bool insideTitan)
{
	ISound* chosen = m_global;
	if (listenerPosition && !chosen) {
		for (int i = static_cast<int>(m_emitters.size()) - 1; i >= 0; --i) {
			ISoundTemplate* templ = m_emitters[i]->soundTemplate();
			// The titan interior ambience only applies inside a titan.
			if (templ->isTitanInteriorAmbient()) {
				if (insideTitan) {
					chosen = m_emitters[i];
					break;
				}
				continue;
			}
			const Vec3 delta = *listenerPosition - *templ->position();
			const float radius = templ->minDistance();
			if (delta.lengthSquared() < radius * radius &&
				(templ->soundRadius() == 0.0f || std::fabs(delta.y) <= templ->soundRadius())) {
				chosen = m_emitters[i];
				break;
			}
			// The editor keeps emitters roughly sorted as they are edited.
			if (soundEngine().editorMode() && i > 0 &&
				m_emitters[i - 1]->soundTemplate()->minDistance() < templ->minDistance()) {
				std::swap(m_emitters[i - 1], m_emitters[i]);
			}
		}
	}

	// Switching to/from the global ambience is quick; walking between areas fades slowly.
	const bool fast = chosen == m_global || m_globalChanged;
	m_globalChanged = false;
	const float fade = soundTweaks().get(fast ? Tweak::AmbientForcedFadeTime : Tweak::AmbientFadeTime);
	soundEngine().setAmbientSound(chosen, fade);
}

} // namespace dice::hfe::sound
