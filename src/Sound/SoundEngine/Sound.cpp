#include "Sound.h"

#include "BfSoundEngine.h"
#include "OpenALSample.h"
#include "SoundServices.h"
#include "SoundTemplate.h"
#include "SoundTweaks.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dice::hfe::sound {

namespace {

// Stand-in when the game sets no transform: no matrix, no velocity, so
// positions fall back to the template's.
class NullTransform final : public ISoundTransform {
public:
	const Matrix4* worldTransform() override { return nullptr; }
	const Vec3* velocity() override { return nullptr; }
};
NullTransform g_nullTransform;
int g_soundsAlive = 0;

const Matrix4 kIdentity;
const Vec3 kZero;

bool nearlyZero(const Vec3& v)
{
	constexpr float eps = 1.1920929e-07f;
	return std::fabs(v.x) < eps && std::fabs(v.y) < eps && std::fabs(v.z) < eps;
}

} // namespace

// The last transform and occlusion of a sound whose owner released it while
// samples still play, so they finish where they started.
class Sound::DetachedState final : public ISoundTransform, public ISoundOcclusion {
public:
	const Matrix4* worldTransform() override { return &matrix; }
	const Vec3* velocity() override { return &m_velocity; }
	float occlusion(ISound*, const Vec3*) override { return occlusionValue; }

	Matrix4 matrix;
	Vec3 m_velocity;
	float occlusionValue = 0.0f;
};

// A fade of the template volume, interpolated on a loudness curve.
struct Sound::Fade {
	float from;
	float to;
	float current;
	float remaining; // ticks
	int startTime;
	float slope;     // per tick

	// Returns false once the fade has finished.
	bool update(int now)
	{
		// The original stored the start time as a float and subtracted its
		// bit pattern from an integer tick count, so fades never progressed.
		// This uses the intended elapsed time instead.
		const float elapsed = static_cast<float>(now - startTime);
		startTime = now;
		if (elapsed > 0.0f) {
			current += elapsed * slope;
			remaining -= elapsed;
			if (remaining <= 0.0f) {
				current = to;
				slope = 0.0f;
				return false;
			}
		}
		return true;
	}

	int remainingTicks() const { return static_cast<int>(std::floor(remaining + 0.5f)); }

	// Maps the linear fade position onto a logarithmic loudness curve.
	float loudness() const
	{
		if (!(current > 0.0f && current < 1.0f && from >= 0.0f && from <= 1.0f && to >= 0.0f && to <= 1.0f &&
				from != to)) {
			return current;
		}
		const float lo = std::min(from, to);
		const float hi = std::max(from, to);
		const float a = (lo * 10.0f - hi) / 9.0f;
		return std::log10((current - a) / (lo - a)) * (hi - lo) + lo;
	}
};

Sound::Sound(SoundTemplate* soundTemplate) : m_template(soundTemplate), m_transform(&g_nullTransform)
{
	m_template->addRef();
	++g_soundsAlive;
}

Sound::~Sound()
{
	m_template->release();
	--g_soundsAlive;
}

void* Sound::destroy(unsigned flags)
{
	if (flags & 1) {
		delete this;
		return nullptr;
	}
	this->~Sound();
	return this;
}

int Sound::release()
{
	if (--m_refCount >= 1) {
		return m_refCount;
	}
	if (m_activeSamples < 1) {
		destroy(1);
	}
	else if (!detach() && soundEngine().isInitialized()) {
		// Can't play out on its own (e.g. a loop): stop now; the last
		// finishing sample deletes the sound.
		soundEngine().stopSound(this, true);
	}
	return 0;
}

bool Sound::detach()
{
	if (m_loopCount == 0) {
		return false;
	}
	// Envelope inputs point into the owner, which is going away.
	if (volumeEnvelope() != m_template->volumeEnvelopeObject() ||
		pitchEnvelope() != m_template->pitchEnvelopeObject()) {
		return false;
	}
	auto state = std::make_unique<DetachedState>();
	state->matrix = worldMatrix();
	state->m_velocity = velocity();
	if (m_occlusion) {
		const Vec3 position = state->matrix.translation();
		state->occlusionValue = m_occlusion->occlusion(this, &position);
		m_occlusion = state.get();
	}
	m_transform = state.get();
	m_detached = std::move(state);
	return true;
}

void Sound::setTransform(ISoundTransform* transform)
{
	m_transform = transform ? transform : &g_nullTransform;
}

void Sound::setVolumeEnvelope(ISoundEnvelope* envelope, const float* input)
{
	m_volumeEnvelope = envelope;
	m_volumeInput = input;
}

ISoundEnvelope* Sound::volumeEnvelope()
{
	return m_volumeEnvelope ? m_volumeEnvelope : m_template->volumeEnvelopeObject();
}

void Sound::setPitchEnvelope(ISoundEnvelope* envelope, const float* input)
{
	m_pitchEnvelope = envelope;
	m_pitchInput = input;
}

ISoundEnvelope* Sound::pitchEnvelope()
{
	return m_pitchEnvelope ? m_pitchEnvelope : m_template->pitchEnvelopeObject();
}

ISoundTemplate* Sound::soundTemplate() { return m_template; }

int Sound::aliveCount() { return g_soundsAlive; }

const float* Sound::sampleTiming(int index)
{
	SoundFile* file = m_template->soundFile(index);
	return file ? file->timing() : nullptr;
}

const Matrix4& Sound::worldMatrix() const
{
	const Matrix4* matrix = m_transform ? m_transform->worldTransform() : nullptr;
	return matrix ? *matrix : kIdentity;
}

Vec3 Sound::velocity() const
{
	const Vec3* v = m_transform ? m_transform->velocity() : nullptr;
	return v ? *v : kZero;
}

Vec3 Sound::worldPosition(const Vec3* offset) const
{
	if (!m_transform) {
		return *m_template->position();
	}
	Vec3 local = *m_template->position();
	if (offset) {
		local = local + *offset;
	}
	const Matrix4& matrix = worldMatrix();
	return nearlyZero(local) ? matrix.translation() : matrix.transformPoint(local);
}

float Sound::computeVolume(float envelopeInput, unsigned seed)
{
	float volume = m_template->volume();
	if (ISoundEnvelope* envelope = volumeEnvelope()) {
		if (m_volumeInput) {
			envelopeInput = *m_volumeInput;
		}
		volume *= envelope->evaluate(envelopeInput, seed + static_cast<unsigned>(std::floor(envelopeInput)));
	}
	return soundEngine().volume(m_template->volumeGroup()) * volume;
}

float Sound::computePitch(float envelopeInput, unsigned seed)
{
	float pitch = m_template->pitch();
	if (ISoundEnvelope* envelope = pitchEnvelope()) {
		if (m_pitchInput) {
			envelopeInput = *m_pitchInput;
		}
		pitch *= envelope->evaluate(envelopeInput, seed + static_cast<unsigned>(std::floor(envelopeInput)));
	}
	return std::clamp(pitch, 0.3f, 2.0f);
}

int Sound::chooseSampleIndex()
{
	const int count = m_template->usableSampleCount();
	if (count < 2) {
		return 0;
	}
	int index;
	switch (m_template->randomMode()) {
	case 1: // random, never the same twice in a row
		do {
			index = std::rand() % count;
		} while (index == m_lastSampleIndex);
		break;
	case 2: // random, alternating between even and odd variations
		// Signed remainder, so the initial index -1 matches no parity.
		index = m_lastSampleIndex;
		while (index == m_lastSampleIndex || index % 2 == m_lastSampleIndex % 2) {
			index = std::rand() % count;
		}
		break;
	default: // sequential
		index = m_lastSampleIndex + 1 < count ? m_lastSampleIndex + 1 : 0;
		break;
	}
	m_lastSampleIndex = index;
	return index;
}

bool Sound::scheduleSample(int index, const Vec3* position, int loopCount)
{
	if (!soundEngine().isInitialized() || !m_template->soundFile(index)) {
		return false;
	}
	m_loopCount = loopCount;
	soundEngine().scheduleSample(*this, index, position);
	return true;
}

void Sound::playLooped(int loopCount)
{
	const int index = chooseSampleIndex();
	scheduleSample(index, nullptr, loopCount);
}

void Sound::play()
{
	const int loopCount = m_template->loopCount();
	const int index = chooseSampleIndex();
	scheduleSample(index, nullptr, loopCount);
}

void Sound::playAtLooped(const Vec3* position, int loopCount)
{
	const int index = chooseSampleIndex();
	scheduleSample(index, position, loopCount);
}

void Sound::playAt(const Vec3* position)
{
	const int loopCount = m_template->loopCount();
	const int index = chooseSampleIndex();
	scheduleSample(index, position, loopCount);
}

bool Sound::playSampleLooped(int index, int loopCount) { return scheduleSample(index, nullptr, loopCount); }
bool Sound::playSample(int index) { return scheduleSample(index, nullptr, m_template->loopCount()); }
bool Sound::playSampleAtLooped(const Vec3* position, int index, int loopCount)
{
	return scheduleSample(index, position, loopCount);
}
bool Sound::playSampleAt(const Vec3* position, int index)
{
	return scheduleSample(index, position, m_template->loopCount());
}

void Sound::stopImmediately()
{
	if (soundEngine().isInitialized()) {
		soundEngine().stopSound(this, true);
	}
}

void Sound::fadeOut(int ticks)
{
	const char* name = m_template->soundFilename();
	if (ticks < 1) {
		if (m_fading) {
			m_fading = false;
			if (m_fade) {
				m_template->setVolume(m_fade->from);
				m_fade.reset();
			}
			SND_INFO("[KRE] Fading interrupted: %s", name);
		}
		if (soundEngine().isInitialized()) {
			soundEngine().stopSound(this, false);
		}
		return;
	}

	const float duration = static_cast<float>(ticks);
	if (m_fading) {
		SND_INFO("[KRE] Fading interrupted with new fade: %s", name);
		// An ongoing fade-out that ends sooner wins.
		if (m_fade && m_fade->to == 0.0f && static_cast<float>(m_fade->remainingTicks()) < duration) {
			return;
		}
	}
	m_fading = true;
	const float volume = m_template->volume();
	m_fade.reset(new Fade {volume, 0.0f, volume, duration, soundEngine().now(), -volume / duration});
	SND_INFO("[KRE] Start fading: %s", name);
}

bool Sound::updateFade(int now)
{
	if (!m_fade) {
		m_fading = false;
		return false;
	}
	if (m_fade->update(now)) {
		m_template->setVolume(m_fade->loudness());
		SND_DEBUG("[KRE] Fading Volume: %f", m_fade->loudness());
		return true;
	}
	// The fade changed the shared template volume; restore it.
	if (m_fade->to <= 0.0f) {
		m_template->setVolume(m_fade->from);
	}
	m_fading = false;
	m_fade.reset();
	return false;
}

int Sound::chooseVoiceSlot(const Vec3* offset)
{
	unsigned mode = m_playFlags & VoiceModeMask;
	if (!offset) {
		mode = 0;
	}
	if (mode == 0) {
		return m_nextVoice;
	}
	if (mode != 1) {
		return -1;
	}

	BfSoundEngine& engine = soundEngine();
	const SoundTweaks& tweaks = soundTweaks();
	const Vec3 position = worldPosition(offset);
	const Vec3 relative = position - *engine.listenerPosition();
	const Vec3& forward = *engine.listenerForward();
	const Vec3& up = *engine.listenerUp();

	const float kFrontBack = tweaks.get(Tweak::PrioKFrontBack);
	const float kRightLeft = tweaks.get(Tweak::PrioKRightLeft);
	const unsigned front = dot(relative, forward) < 0.0f ? 0u : 2u;
	const unsigned right = dot(relative, cross(forward, up)) > 0.0f ? 1u : 0u;
	const float distance = relative.length();

	int best = m_nextVoice;
	if (m_voices[m_nextVoice]) {
		const float kAge = tweaks.get(Tweak::PrioKAge);
		const float ageOnlyDistance = tweaks.get(Tweak::PrioAgeOnlyDistance);
		const float kDistance = tweaks.get(Tweak::PrioKDistance);
		// Beyond the age-only distance, nearer sounds score higher. (The
		// original compares the new sound's distance but scores each voice by
		// its own; reproduced as-is.)
		const auto distanceFactor = [&](float d) {
			return distance > ageOnlyDistance ? 1.0f / ((ageOnlyDistance - d) + 1.0f) : 1.0f;
		};
		const float newScore = distanceFactor(distance) * kDistance + (front ? kFrontBack : -kFrontBack) + kAge;

		// The voice about to be reused is the older one.
		float score[2];
		score[m_nextVoice] = -kAge;
		score[(m_nextVoice + 1) & 1] = 0.0f;
		for (int v = 0; v < 2; ++v) {
			if (right == (m_voiceFlags[v] & 1)) {
				score[v] -= kRightLeft;
			}
			score[v] += (m_voiceFlags[v] & 2) ? kFrontBack : -kFrontBack;
			score[v] += distanceFactor(m_voiceDistance[v]) * kDistance;
		}

		best = -1;
		float bestScore = 1000.0f;
		if (score[0] <= newScore && score[0] < bestScore) {
			best = 0;
			bestScore = score[0];
		}
		if (score[1] <= newScore && score[1] < bestScore) {
			best = 1;
		}
		if (best < 0) {
			return -1;
		}
	}
	m_voiceDistance[best] = distance;
	m_voiceFlags[best] = right | front;
	return best;
}

bool Sound::claimVoice(Sample* sample)
{
	BfSoundEngine& engine = soundEngine();
	if (!(m_activeSamples > 0 || m_detached)) {
		return false;
	}
	if (!(m_playFlags & AllowRetriggerSameFrame) && m_lastTriggerFrame == engine.frame()) {
		return false;
	}
	const int slot = chooseVoiceSlot(&sample->positionOffset());
	if (slot < 0) {
		return false;
	}
	m_nextVoice = slot;
	if (m_voices[slot]) {
		engine.stopSample(m_voices[slot], true);
	}
	m_voices[m_nextVoice] = sample;
	m_nextVoice = (m_nextVoice + 1) & 1;
	m_lastTriggerFrame = engine.frame();
	return true;
}

void Sound::sampleFinished(Sample* sample)
{
	for (int v = 0; v < 2; ++v) {
		if (m_voices[v] == sample) {
			m_voices[v] = nullptr;
			m_nextVoice = v;
			break;
		}
	}
	if (--m_activeSamples == 0 && m_refCount == 0) {
		destroy(1);
	}
}

} // namespace dice::hfe::sound
