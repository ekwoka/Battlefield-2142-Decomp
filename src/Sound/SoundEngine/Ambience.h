// Level ambience (originally part of BfSoundEngine.cpp).
//
// Levels place ambient emitters: spheres (min distance as radius, optional
// vertical extent) each with a looping 2D sound. Every frame the engine picks
// the innermost sphere containing the listener - or, inside a titan, the
// titan interior ambience - and crossfades to its sound. While indoors the ambience is muffled through the
// sound's occlusion hook.
#pragma once

#include "ISoundEngine.h"
#include "SoundEnvelope.h"

#include <memory>
#include <vector>

namespace dice::hfe::sound {

// Silent stand-in meaning "no ambience" (the original's null sound object).
ISound& nullSound();

class AmbientCrossfader final : public ISoundOcclusion {
public:
	AmbientCrossfader();
	~AmbientCrossfader();

	// Crossfades to `sound` over fadeSeconds; queued if a fade is running.
	void setAmbient(ISound* sound, float fadeSeconds);
	void stop();
	void update(int ticks);

	ISound* current() const { return m_current; }

	// The indoor muffle applied to ambient sounds.
	float occlusion(ISound*, const Vec3*) override { return m_indoorCurrent; }

private:
	void startCrossfade(ISound* sound, float fadeSeconds);
	void releaseSound(ISound*& sound);
	void restoreEnvelope(ISound* sound);
	bool usesFadeEnvelope(ISound* sound);

	ISound* m_current = nullptr;
	ISound* m_next = nullptr;
	ISound* m_queued = nullptr;
	int m_fadeStart = 0;
	int m_fadeEnd = -1;
	float m_queuedFadeSeconds = 0.0f;
	float m_fadeProgress = 0.0f;     // envelope input of the fading sounds
	float m_indoorTarget = 1.0f;
	float m_indoorCurrent = 1.0f;
	float m_indoorSpeed = 0.0f;
	std::unique_ptr<SoundEnvelope> m_fadeIn;
	std::unique_ptr<SoundEnvelope> m_fadeOut;
};

class AmbientEmitters {
public:
	~AmbientEmitters() { clear(); }

	void add(ISoundTemplate* emitter);
	void remove(ISoundTemplate* emitter);
	void clear();
	int count() const { return static_cast<int>(m_emitters.size()); }

	// A sound that overrides the emitter selection (e.g. inside a titan).
	void setGlobal(ISound* sound);
	ISound* global() const { return m_global; }

	// Larger spheres first, so scanning backwards finds the innermost.
	void sortByRadius();

	// Picks the innermost emitter around the listener (or the titan interior
	// ambience) and hands it to the crossfader.
	void update(const Vec3* listenerPosition, bool insideTitan);

private:
	std::vector<ISound*> m_emitters;
	ISound* m_global = nullptr;
	bool m_globalChanged = false;
};

} // namespace dice::hfe::sound
