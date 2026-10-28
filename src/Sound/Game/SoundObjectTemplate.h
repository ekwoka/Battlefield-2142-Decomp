// The "Sound" object template (class ID 0x985fd1, constructor 0x007ff2a0).
//
// Scripts define sounds as object templates:
//     ObjectTemplate.create Sound S_flag_flutter
//     ObjectTemplate.soundFilename "sound/effects/flag.wav"
//     ObjectTemplate.volume 0.8
// Each one owns a sound template in the audio engine and forwards its
// properties to it. Volume, pitch, distances, reverb and radius are scaled
// by per-template multipliers ("Sound.tweakTemplate"); envelopes and file
// lists are passed as text and parsed here (vtable 0x009bfb18).
#pragma once

#include "Sound/SoundEngine/ISoundEngine.h"
#include "World/ObjectTemplate.h"

#include <array>
#include <string>

namespace dice::hfe::sound {

class SoundObjectTemplate : public world::ObjectTemplate {
public:
	static constexpr uint32_t kClassId = 0x985fd1;

	// `engine` may be null (no sound): properties are then only stored.
	SoundObjectTemplate(ISoundEngine* engine, uint32_t classId, std::string typeName, std::string name, uint32_t id);
	~SoundObjectTemplate() override;

	ISoundTemplate* engineTemplate() const { return m_sound; }

	// Comma-separated file names.
	void setSoundFilenames(const std::string& list);
	std::string soundFilenames() const;
	// Whether the script gave it any file (the generated S_<owner>_<event>
	// sounds mostly have none).
	bool hasFiles() const { return m_filenameText.find_first_not_of(" ,\"") != std::string::npos; }

	// "minX/maxX/minY/maxY/randomized/count/" then "x/yLow/yHigh/" per point.
	void setVolumeEnvelope(const std::string& text);
	std::string volumeEnvelope() const;
	void setPitchEnvelope(const std::string& text);
	std::string pitchEnvelope() const;

	void setVolume(float volume);
	float volume() const { return m_volume; }
	void setPitch(float pitch);
	float pitch() const { return m_pitch; }
	void setMinDistance(float distance);
	float minDistance() const { return m_minDistance; }
	// 0 means twice the minimum distance.
	void setHalfVolumeDistance(float distance);
	float halfVolumeDistance() const { return m_halfVolumeDistance; }
	void setReverbLevel(float level);
	float reverbLevel() const { return m_reverbLevel; }
	void setSoundRadius(float radius);
	float soundRadius() const { return m_soundRadius; }
	void setTitanAmbient(bool interior);
	bool isTitanAmbient() const { return m_titanAmbient; }

	// "Sound.tweakTemplate": multipliers for volume, pitch, min distance,
	// half-volume distance, reverb level and radius; reapplies them.
	void setTweaks(const std::array<float, 6>& multipliers);

	float globalEffectLevel = 1; // stored only; its use isn't known yet

private:
	enum Tweak { VolumeTweak, PitchTweak, MinDistanceTweak, HalfVolumeTweak, ReverbTweak, RadiusTweak };

	ISoundTemplate* m_sound = nullptr;
	float m_volume = 1;
	float m_pitch = 1;
	float m_minDistance = 1;
	float m_halfVolumeDistance = 0;
	float m_reverbLevel = 0;
	float m_soundRadius = 0;
	bool m_titanAmbient = false;
	std::array<float, 6> m_tweaks {1, 1, 1, 1, 1, 1};
	std::string m_filenameText;
	std::string m_volumeEnvelopeText = "0/1/0/1/0/0/";
	std::string m_pitchEnvelopeText = "0/1/0.1/3.0/0/0/";
};

} // namespace dice::hfe::sound
