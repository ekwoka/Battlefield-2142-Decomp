// Sound templates (part of the original Sound.cpp): the shared settings of a
// sound as set by "ObjectTemplate.create Sound" in the game's .con files.
#pragma once

#include "ISoundEngine.h"
#include "SoundEnvelope.h"
#include "SoundFile.h"

#include <climits>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::sound {

class SoundTemplate final : public ISoundTemplate {
public:
	enum Flags : unsigned {
		LoadOnDemand = 1, // load files when first played instead of up front
	};

	SoundTemplate();

	int addRef() override { return ++m_refCount; }
	int release() override;

	void setSoundFilename(const char* path) override;
	const char* soundFilename() override;
	void setSoundFilenames(const char* const* paths, int count) override;
	const char* const* soundFilenames(int* count) override;

	void setLoopCount(int count) override { m_loopCount = count; }
	int loopCount() override { return m_loopCount; }
	void setVolume(float volume) override;
	float volume() override { return m_volume; }
	void setVolumeEnvelope(float minX, float maxX, float minY, float maxY, bool randomized, int pointCount,
		const float* points) override;
	const float* volumeEnvelope(float* minX, float* maxX, float* minY, float* maxY, bool* randomized,
		int* pointCount) override;
	ISoundEnvelope* volumeEnvelopeObject() override { return m_volumeEnvelope.get(); }
	void setPitch(float pitch) override { m_pitch = pitch < 0.01f ? 0.01f : pitch; }
	float pitch() override { return m_pitch; }
	void setPitchEnvelope(float minX, float maxX, float minY, float maxY, bool randomized, int pointCount,
		const float* points) override;
	const float* pitchEnvelope(float* minX, float* maxX, float* minY, float* maxY, bool* randomized,
		int* pointCount) override;
	ISoundEnvelope* pitchEnvelopeObject() override { return m_pitchEnvelope.get(); }
	void setVolumeGroup(int group) override;
	int volumeGroup() override { return m_volumeGroup; }
	void setPan(float pan) override;
	float pan() override { return m_pan; }
	void setMinDistance(float distance) override;
	float minDistance() override { return m_minDistance; }
	void setHalfVolumeDistance(float distance) override;
	float halfVolumeDistance() override { return m_halfVolumeDistance; }
	void setIs3dSound(bool is3d) override { m_is3d = is3d; }
	bool is3dSound() override { return m_is3d; }
	void setStopType(int type) override;
	int stopType() override { return m_stopType; }
	void setReverbLevel(float level) override;
	float reverbLevel() override { return m_reverbLevel; }
	void setPosition(const Vec3* position) override { m_position = *position; }
	const Vec3* position() override { return &m_position; }
	void setDirection(const Vec3* direction) override;
	const Vec3* direction() override { return &m_direction; }
	void setSoundRadius(float radius) override { m_soundRadius = radius < 0.0f ? 0.0f : radius; }
	float soundRadius() override { return m_soundRadius; }
	void setTitanInteriorAmbient(bool interior) override { m_titanInterior = interior ? 1.0f : 0.0f; }
	bool isTitanInteriorAmbient() override { return m_titanInterior != 0.0f; }
	void setConeInnerAngle(float degrees) override;
	float coneInnerAngle() override { return m_coneInnerAngle; }
	void setConeOuterAngle(float degrees) override;
	float coneOuterAngle() override { return m_coneOuterAngle; }
	void setConeOuterVolume(float volume) override;
	float coneOuterVolume() override { return m_coneOuterVolume; }
	void setOcclusionFactor(float factor) override;
	float occlusionFactor() override { return m_occlusionFactor; }
	void markSoundFilesUsed() override;
	void setSampleCount(int qualityLevel, int count) override { m_sampleCounts[qualityLevel] = count < 0 ? INT_MAX : count; }
	int sampleCount(int qualityLevel) override { return m_sampleCounts[qualityLevel]; }
	void setRandomMode(unsigned char mode) override { m_randomMode = mode; }
	unsigned char randomMode() override { return m_randomMode; }
	void setFlags(unsigned flags) override { m_flags = flags; }
	unsigned flags() override { return m_flags; }
	void* loadedSoundFile(int index) override;
	ISound* createSound() override;
	void* destroy(unsigned flags) override;

	// Engine-side helpers.
	SoundFile* soundFile(int index);
	int usableSampleCount();
	int sampleDurationTicks(int index);
	float rolloffFactor() const { return m_rolloffFactor; }
	float maxDistance() const { return m_maxDistance; }
	bool hasCone() const { return m_coneInnerAngle < 360.0f && m_direction.lengthSquared() > 0.01f; }
	void loadSoundFiles(int qualityLevel);
	void unloadSoundFiles(bool force, bool forceStreaming);

private:
	~SoundTemplate();
	void clearSoundFiles();
	void updateRolloff();

	int m_refCount = 0;
	int m_loopCount = 1;
	float m_minDistance = 1.0f;
	float m_halfVolumeDistance = 0.0f;
	float m_maxDistance = 0.0f;   // derived
	float m_rolloffFactor = 0.0f; // derived
	bool m_is3d = true;
	int m_stopType = 1;
	float m_reverbLevel = 1.0f;
	float m_volume = 1.0f;
	float m_pitch = 1.0f;
	float m_pan = 0.5f;
	std::unique_ptr<SoundEnvelope> m_volumeEnvelope;
	std::unique_ptr<SoundEnvelope> m_pitchEnvelope;
	Vec3 m_position;
	float m_soundRadius = 0.0f;
	float m_titanInterior = 0.0f;
	Vec3 m_direction;
	float m_coneInnerAngle = 360.0f;
	float m_coneOuterAngle = 360.0f;
	float m_coneOuterVolume = 1.0f;
	float m_occlusionFactor = 1.0f;
	std::vector<SoundFile*> m_files;
	std::vector<std::string> m_filenames;
	std::vector<const char*> m_filenamePointers;
	unsigned m_flags = 0;
	int m_volumeGroup = 0;
	unsigned char m_randomMode = 1;
	int m_sampleCounts[3] = {INT_MAX, INT_MAX, INT_MAX};
};

} // namespace dice::hfe::sound
