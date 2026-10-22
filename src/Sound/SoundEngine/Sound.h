// Playable sounds (originally Sound.cpp).
//
// A Sound belongs to a game object and plays its template's samples. Besides
// starting samples it implements:
//  - two voice slots for rapid-fire sounds (play flag mode 1): a new shot
//    replaces whichever of the two playing voices matters least, scored by
//    age, distance and front/back and left/right position;
//  - random/alternating/sequential choice between the template's variations;
//  - "detaching" when the owner releases the sound while samples still play,
//    freezing its last transform so the tail plays out in place;
//  - fades driven by the engine.
#pragma once

#include "ISoundEngine.h"

#include <memory>

namespace dice::hfe::sound {

class Sample;
class SoundTemplate;

class Sound final : public ISound {
public:
	enum PlayFlags : unsigned {
		VoiceModeMask = 0xff,            // 0 = one voice, 1 = two alternating voices
		AllowRetriggerSameFrame = 0x100,
		SkipStartBlock = 0x200,          // jump straight into the loop
	};

	explicit Sound(SoundTemplate* soundTemplate);

	int addRef() override { return ++m_refCount; }
	int release() override;
	void setPlayFlags(unsigned flags) override { m_playFlags = flags; }
	unsigned playFlags() override { return m_playFlags; }
	void playLooped(int loopCount) override;
	void play() override;
	void playAtLooped(const Vec3* position, int loopCount) override;
	void playAt(const Vec3* position) override;
	bool playSampleLooped(int index, int loopCount) override;
	bool playSample(int index) override;
	bool playSampleAtLooped(const Vec3* position, int index, int loopCount) override;
	bool playSampleAt(const Vec3* position, int index) override;
	void fadeOut(int ticks) override;
	void stop() override { fadeOut(0); }
	void stopImmediately() override;
	bool isPlaying() override { return m_activeSamples > 0; }
	void setTransform(ISoundTransform* transform) override;
	ISoundTransform* transform() override { return m_transform; }
	void setVolumeEnvelope(ISoundEnvelope* envelope, const float* input) override;
	ISoundEnvelope* volumeEnvelope() override;
	void setPitchEnvelope(ISoundEnvelope* envelope, const float* input) override;
	ISoundEnvelope* pitchEnvelope() override;
	void setOcclusion(ISoundOcclusion* occlusion) override { m_occlusion = occlusion; }
	ISoundOcclusion* occlusion() override { return m_occlusion; }
	void setHeadRelative(bool relative) override { m_headRelative = relative; }
	bool headRelative() override { return m_headRelative; }
	void setParameter(int index, unsigned value) override { m_parameters[index] = value; }
	unsigned parameter(int index) override { return m_parameters[index]; }
	const float* sampleTiming(int index) override;
	ISoundTemplate* soundTemplate() override;
	void setFading(bool fading) override { m_fading = fading; }
	bool isFading() override { return m_fading; }
	bool updateFade(int now) override;
	void* destroy(unsigned flags) override;

	// Engine/sample side.
	SoundTemplate& templ() const { return *m_template; }
	int loopCountForPlay() const { return m_loopCount; }
	Vec3 worldPosition(const Vec3* offset) const;
	const Matrix4& worldMatrix() const;
	Vec3 velocity() const;
	float computeVolume(float envelopeInput, unsigned seed);
	float computePitch(float envelopeInput, unsigned seed);
	void sampleStarted() { ++m_activeSamples; }
	void sampleFinished(Sample* sample);
	// Claims a voice slot for a starting sample (two-voice mode); false if the
	// sample isn't allowed to play this frame.
	bool claimVoice(Sample* sample);
	static int aliveCount();

private:
	class DetachedState;
	struct Fade;

	~Sound();
	int chooseSampleIndex();
	int chooseVoiceSlot(const Vec3* offset);
	bool scheduleSample(int index, const Vec3* position, int loopCount);
	bool detach();

	int m_refCount = 0;
	int m_activeSamples = 0;
	SoundTemplate* m_template;
	ISoundTransform* m_transform;
	ISoundEnvelope* m_volumeEnvelope = nullptr;
	const float* m_volumeInput = nullptr;
	ISoundEnvelope* m_pitchEnvelope = nullptr;
	const float* m_pitchInput = nullptr;
	ISoundOcclusion* m_occlusion = nullptr;
	int m_loopCount = 0;
	Sample* m_voices[2] = {};
	float m_voiceDistance[2] = {};
	unsigned m_voiceFlags[2] = {};
	int m_lastTriggerFrame = -1;
	int m_nextVoice = 0;
	unsigned m_parameters[3] = {};
	std::unique_ptr<DetachedState> m_detached;
	unsigned m_playFlags = 0;
	int m_lastSampleIndex = -1;
	bool m_headRelative = false;
	bool m_fading = false;
	std::unique_ptr<Fade> m_fade;
};

} // namespace dice::hfe::sound
