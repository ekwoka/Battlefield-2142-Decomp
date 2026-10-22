#include "BfSoundEngine.h"

#include "Music.h"
#include "OpenALProvider.h"
#include "OpenALSample.h"
#include "Reverb.h"
#include "Sound.h"
#include "SoundFile.h"
#include "SoundServices.h"
#include "SoundTemplate.h"
#include "SoundTweaks.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace dice::hfe::sound {

namespace {

const Vec3 kZero;
const Vec3 kDefaultForward {0.0f, 0.0f, 1.0f};
const Vec3 kDefaultUp {0.0f, 1.0f, 0.0f};

// Speed of sound, used to delay distant one-shots.
constexpr float kSecondsPerMetre = 0.002915452f; // 1/343
constexpr int kTicksPerSecond = 30;

// One-shots that can't start within two seconds are dropped.
constexpr int kMaxStartDelayTicks = 60;

constexpr float kCullVolume = 0.003f;
constexpr float kNeverCullDistance = 5.0f;

bool laterStart(const Sample* a, const Sample* b) { return a->scheduledTime > b->scheduledTime; }

// Higher priority first; on ties the newer sample wins.
bool higherPriority(const Sample* a, const Sample* b)
{
	if (a->priority == b->priority) {
		return a->startTime > b->startTime;
	}
	return a->priority > b->priority;
}

// Whether any audio output exists. The original enumerated DirectSound
// devices; OpenAL reports failure itself when opening the device, so this
// only exists to keep the original early-out.
bool soundCardPresent() { return true; }

} // namespace

BfSoundEngine::BfSoundEngine()
	: m_openAL(std::make_unique<OpenALProvider>()), m_listenerPosition(&kZero), m_listenerForward(&kDefaultForward),
	  m_listenerUp(&kDefaultUp), m_listenerVelocity(&kZero)
{
}

BfSoundEngine::~BfSoundEngine()
{
	if (m_tinnitus) {
		m_tinnitus->release();
	}
	shutdown();
}

void* BfSoundEngine::destroy(unsigned flags)
{
	if (flags & 1) {
		delete this;
		return nullptr;
	}
	this->~BfSoundEngine();
	return this;
}

// --- Initialisation ---------------------------------------------------------

int BfSoundEngine::initialize(int qualityLevel, const char* provider, int maxVoices)
{
	if (!soundCardPresent()) {
		SND_WARNING("No sound card detected");
		return 1;
	}
	const std::string name = provider ? provider : "0";
	if (m_initialized && qualityLevel == m_initQuality && name == m_initProvider && maxVoices == m_initMaxVoices) {
		return 0;
	}
	m_initMaxVoices = maxVoices;
	m_initProvider = name;
	m_initQuality = qualityLevel;

	const bool musicWasPlaying = music::stop(0.0f);
	m_initialized = false;
	m_qualityLevel = qualityLevel;
	refreshProviders();
	requeuePlaying();
	unloadSoundFiles(true, false);

	if (const int result = openProvider(name); result != 0) {
		return result;
	}
	setMaxVoices(maxVoices);
	// The original only pushed the gain when it changed, which left a
	// reopened device at full volume; always apply it.
	if (m_provider) {
		m_provider->setMasterGain(m_masterVolume);
	}
	m_initialized = true;
	reloadFiles();
	if (m_reverb) {
		m_reverb->invalidate();
	}
	if (musicWasPlaying) {
		music::start();
	}
	return 0;
}

bool BfSoundEngine::refreshProviders()
{
	m_providerNames = m_openAL->providerNames();
	m_providerNamePointers.clear();
	for (const std::string& provider : m_providerNames) {
		m_providerNamePointers.push_back(provider.c_str());
	}
	if (m_providerNames.empty()) {
		SND_WARNING("No 3D sound providers are available.");
		return false;
	}
	return true;
}

int BfSoundEngine::openProvider(const std::string& name)
{
	// Settings files may store the provider as its index ("0".."9").
	std::string resolved = name;
	if (name.size() == 1 && std::isdigit(static_cast<unsigned char>(name[0]))) {
		const size_t index = static_cast<size_t>(name[0] - '0');
		if (index < m_providerNames.size()) {
			resolved = m_providerNames[index];
		}
	}
	const int result = static_cast<int>(m_openAL->open(resolved));
	if (result == 0) {
		m_provider = m_openAL.get();
	}
	else {
		SND_WARNING("Failed to open 3d provider \"%s\"", resolved.c_str());
		m_provider = nullptr;
	}
	return result;
}

void BfSoundEngine::setMaxVoices(int count)
{
	if (count == 0) {
		count = 16;
	}
	else if (count < 2) {
		count = 2;
	}
	int voices = count + static_cast<int>(std::lround(soundTweaks().get(Tweak::MinFreeSources)));
	if (m_provider) {
		voices = std::min(voices, m_provider->maxVoices() - 1);
	}
	m_maxVoicesInUse = voices;
	setMaxSources(voices);
}

void BfSoundEngine::shutdown()
{
	music::stop(0.0f);
	setListenerPosition(nullptr);
	setListenerOrientation(nullptr, nullptr);
	setListenerVelocity(nullptr);
	requeuePlaying();
	unloadSoundFiles(true, true);
	for (SoundFile* file : m_fileCache) {
		file->release();
	}
	for (Sample* sample : m_starting) {
		delete sample;
	}
	m_starting.clear();
	deleteAll(m_playing);
	deleteAll(m_virtual);
	for (Sample* sample : m_pending) {
		delete sample;
	}
	m_pending.clear();
	m_fading.clear();
	m_fileCache.clear();
	if (m_provider) {
		m_provider->close();
	}
	m_provider = nullptr;
	setFileSystem(nullptr);
	setLogger(nullptr);
	m_initialized = false;
}

void BfSoundEngine::deleteAll(std::list<Sample*>& list)
{
	for (Sample* sample : list) {
		delete sample;
	}
	list.clear();
}

// --- Files --------------------------------------------------------------------

SoundFile* BfSoundEngine::acquireSoundFile(const char* path)
{
	std::string name(path);
	std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
	for (SoundFile* file : m_fileCache) {
		if (file->path() == name) {
			file->addRef();
			return file;
		}
	}
	auto* file = new SoundFile(name);
	file->addRef(); // the cache's reference
	m_fileCache.push_back(file);
	file->addRef();
	return file;
}

void BfSoundEngine::unloadSoundFiles(bool force, bool forceStreaming)
{
	for (SoundFile* file : m_fileCache) {
		file->unload(force, forceStreaming);
	}
}

void BfSoundEngine::reloadFiles()
{
	if (!m_editorMode) {
		std::sort(m_fileCache.begin(), m_fileCache.end(),
			[](const SoundFile* a, const SoundFile* b) { return a->path() < b->path(); });
		for (SoundFile* file : m_fileCache) {
			file->load(m_qualityLevel);
		}
	}
}

void BfSoundEngine::reloadSoundFiles()
{
	unloadSoundFiles(false, false);
	reloadFiles();
	m_emitters.sortByRadius();
}

int BfSoundEngine::countLoadedSoundFiles(int* count)
{
	// Counts files that are wanted but not loaded yet (the loading screen's progress).
	if (count) {
		*count = 0;
		for (SoundFile* file : m_fileCache) {
			if (file && file->enabledAt(m_qualityLevel) && file->inUse() && !file->loaded()) {
				++*count;
			}
		}
	}
	return 0;
}

ISoundTemplate* BfSoundEngine::createTemplate()
{
	auto* templ = new SoundTemplate();
	templ->addRef();
	return templ;
}

// --- Volume, listener, provider settings ---------------------------------------

void BfSoundEngine::setMasterVolume(float volume)
{
	volume = std::clamp(volume, 0.0f, 1.0f);
	if (volume != m_masterVolume) {
		m_masterVolume = volume;
		if (m_provider) {
			m_provider->setMasterGain(volume);
		}
	}
}

void BfSoundEngine::setVolume(int group, float volume)
{
	if (group >= 0 && group < 8) {
		m_volumes[group] = std::clamp(volume, 0.0f, 1.0f);
	}
}

float BfSoundEngine::volume(int group)
{
	return group >= 0 && group < 8 ? m_volumes[group] : 1.0f;
}

void BfSoundEngine::setListenerPosition(const Vec3* position)
{
	m_listenerPosition = position ? position : &kZero;
}

void BfSoundEngine::setListenerOrientation(const Vec3* forward, const Vec3* up)
{
	m_listenerForward = forward ? forward : &kDefaultForward;
	m_listenerUp = up ? up : &kDefaultUp;
}

void BfSoundEngine::setListenerVelocity(const Vec3* velocity)
{
	m_listenerVelocity = velocity ? velocity : &kZero;
}

void BfSoundEngine::setIndoorLevel(float level)
{
	m_indoorLevel = std::clamp(level, 0.0f, 1.0f);
}

void BfSoundEngine::setDopplerFactor(float factor)
{
	if (m_provider) {
		m_provider->setDopplerFactor(factor);
	}
}

float BfSoundEngine::dopplerFactor()
{
	return m_provider ? m_provider->dopplerFactor() : 0.0f;
}

int BfSoundEngine::eaxVersion()
{
	return m_provider ? m_provider->eaxVersion() : 0;
}

int BfSoundEngine::reverbInfo(int* out)
{
	if (out) {
		*out = 0;
	}
	return 0;
}

void BfSoundEngine::setTweak(const char* name, float value) { soundTweaks().set(name, value); }
float BfSoundEngine::tweak(const char* name) { return soundTweaks().get(name); }

void BfSoundEngine::loadReverbPreset(const char* path)
{
	if (!m_reverb) {
		m_reverb = std::make_unique<Reverb>();
	}
	if (!m_reverb->load(path)) {
		m_reverb.reset();
	}
}

void BfSoundEngine::setFileSystem(ISoundFileSystem* fileSystem) { setSoundFileSystem(fileSystem); }
void BfSoundEngine::setLogger(ISoundLogger* logger) { setSoundLogger(logger); }

const char* const* BfSoundEngine::providerNames(int* count)
{
	refreshProviders();
	if (count) {
		*count = static_cast<int>(m_providerNamePointers.size());
	}
	return m_providerNamePointers.empty() ? nullptr : m_providerNamePointers.data();
}

const SoundStatistics* BfSoundEngine::statistics()
{
	const SoundFileStats& files = soundFileStats();
	const SamplePoolStats& pools = samplePoolStats();
	m_statistics.providerDescription = m_provider ? m_provider->description() : "";
	m_statistics.playingSamples = static_cast<int>(m_playing.size());
	m_statistics.virtualSamples = static_cast<int>(m_virtual.size());
	m_statistics.pendingSamples = static_cast<int>(m_pending.size());
	m_statistics.staticBufferBytes = static_cast<int>(files.staticBytes);
	m_statistics.streamingFileBytes = static_cast<int>(files.streamingBytes);
	m_statistics.streamingBufferBytes = pools.streamingBytes;
	m_statistics.staticBuffers = static_cast<int>(files.buffers);
	m_statistics.streamingBuffers = pools.streamingBuffers;
	m_statistics.loadedFiles = static_cast<int>(files.loadedFiles);
	m_statistics.soundsAlive = Sound::aliveCount();
	m_statistics.samplesAlive = pools.samples;
	m_statistics.sources = pools.sources;
	m_statistics.maxVoices = m_maxVoicesInUse;
	return &m_statistics;
}

void BfSoundEngine::clearAmbience()
{
	m_crossfader.stop();
	m_emitters.clear();
}

// --- Music --------------------------------------------------------------------

void BfSoundEngine::setMusic(const char* path)
{
	if (isInitialized()) {
		music::playTrack(path);
	}
}

void BfSoundEngine::startMusic()
{
	if (isInitialized()) {
		music::start();
	}
}

void BfSoundEngine::stopMusic(float fadeSeconds)
{
	if (isInitialized()) {
		music::stop(fadeSeconds);
	}
}

// --- Tinnitus -------------------------------------------------------------------

void BfSoundEngine::setupTinnitus(const char* const* files, int count)
{
	stopTinnitus(true);
	if (m_tinnitus) {
		m_tinnitus->release();
		m_tinnitus = nullptr;
	}
	ISoundTemplate* templ = createTemplate();
	templ->setIs3dSound(false);
	templ->setSoundFilenames(files, count);
	templ->setLoopCount(0);
	templ->setReverbLevel(0.0f);
	templ->setOcclusionFactor(0.0f);
	templ->setStopType(0);
	templ->markSoundFilesUsed();
	m_tinnitus = templ->createSound();
	templ->release();
	if (!m_tinnitus) {
		SND_WARNING("Failed to create tinnitus sound");
	}
}

void BfSoundEngine::startTinnitus(float attack, float sustain, float release, float level, int sampleIndex)
{
	if (!m_tinnitus) {
		return;
	}
	if (m_tinnitus->isPlaying()) {
		stopTinnitus(true);
	}
	attack = std::max(attack, 0.1f);
	const bool holdUntilReleased = sustain < 0.0f;
	sustain = std::max(sustain, 0.1f);
	release = std::max(release, 0.1f);
	level = std::clamp(level, 0.0f, 1.0f);

	// Attack/sustain/release as a volume envelope over the whole duration.
	const float total = attack + sustain + release;
	const float scale = 1.0f / total;
	const float envelope[] = {
		0.0f, 0.0f, 0.0f,
		scale * attack, 1.0f, 0.0f,
		scale * (attack + sustain), 1.0f, 0.0f,
		1.0f, 0.0f, 0.0f,
	};
	m_tinnitus->soundTemplate()->setVolumeEnvelope(0.0f, 1.0f, 0.0f, 1.0f, false, 4, envelope);
	m_tinnitus->setVolumeEnvelope(nullptr, &m_tinnitusEnvelopeInput);

	const int ticks = std::max(static_cast<int>(std::lround(total * kTicksPerSecond)), 20);
	if (m_tinnitus->playSample(sampleIndex)) {
		m_tinnitusStart = m_now;
		m_tinnitusEnd = m_now + ticks;
		m_tinnitusMaxLevel = level;
		m_tinnitusEnvelopeInput = 0.0f;
		m_tinnitusLevel = 0.0f;
		m_tinnitusHoldPoint = holdUntilReleased ? (sustain * 0.5f + attack) * scale : -1.0f;
	}
}

void BfSoundEngine::stopTinnitus(bool immediate)
{
	if (!m_tinnitus || m_tinnitusEnd < 0) {
		return;
	}
	if (!immediate && m_tinnitusHoldPoint >= 0.0f) {
		m_tinnitusHoldPoint = -1.0f; // release the hold and let it fade
		return;
	}
	m_tinnitus->stop();
	m_tinnitusEnd = -1;
	m_tinnitusLevel = 0.0f;
}

void BfSoundEngine::updateTinnitus()
{
	if (m_tinnitusEnd < 0) {
		return;
	}
	const float t = static_cast<float>(m_now - m_tinnitusStart) / static_cast<float>(m_tinnitusEnd - m_tinnitusStart);
	m_tinnitusEnvelopeInput = t;
	if (m_tinnitusHoldPoint >= 0.0f && t > m_tinnitusHoldPoint) {
		m_tinnitusEnvelopeInput = m_tinnitusHoldPoint;
	}
	const float envelope = m_tinnitus->volumeEnvelope()->evaluate(m_tinnitusEnvelopeInput, 0);
	m_tinnitusLevel = envelope * m_tinnitusMaxLevel;
	if (envelope <= FLT_EPSILON && m_tinnitusStart < m_now) {
		stopTinnitus(true);
	}
}

// --- Per-frame update -------------------------------------------------------------

void BfSoundEngine::update(int ticks)
{
	if (ticks < 1 || !isInitialized()) {
		return;
	}
	updateBalance();
	updateTinnitus();
	updateListener();
	m_crossfader.update(1);
	m_emitters.update(m_listenerPosition, m_insideTitan);

	for (Sample* sample : m_starting) {
		queueStartingSample(sample);
	}
	m_starting.clear();

	updateSamples(ticks);
	m_now += ticks;
	updateFades();
	++m_frame;

	if (!m_starting.empty()) {
		SND_WARNING("Scheduled sounds list is not empty when it should be. The following samples were found:");
		for (size_t i = 0; i < m_starting.size(); ++i) {
			const SoundFile* file = m_starting[i] ? m_starting[i]->soundFile() : nullptr;
			SND_WARNING("%2d: %s", static_cast<int>(i), file ? file->path().c_str() : "");
		}
	}
	processStops();
	music::updateVolume();

	if (soundTraceEnabled() && m_frame % 150 == 0) {
		const SoundStatistics& stats = *statistics();
		SND_DEBUG("stats: playing %d virtual %d pending %d sounds %d samples %d sources %d files %d static %d KiB",
			stats.playingSamples, stats.virtualSamples, stats.pendingSamples, stats.soundsAlive, stats.samplesAlive,
			stats.sources, stats.loadedFiles, stats.staticBufferBytes / 1024);
	}
}

void BfSoundEngine::updateBalance()
{
	// Balance between 2D and 3D sounds: 0.5 is even, higher favours 2D.
	const bool eax = eaxVersion() > 0;
	const float balance = soundTweaks().get(eax ? Tweak::Balance2d3dEax : Tweak::Balance2d3dDefault);
	m_gain2d = 2.0f * (1.0f - std::clamp(balance, 0.5f, 1.0f));
	m_gain3d = 2.0f * std::clamp(balance, 0.0f, 0.5f);
	if (eax) {
		m_gain3d *= m_masterVolume;
	}
}

void BfSoundEngine::updateListener()
{
	const Vec3& forward = *m_listenerForward;
	const Vec3& up = *m_listenerUp;
	Matrix4 listener;
	const Vec3 rows[4] = {cross(up, forward), up, forward, *m_listenerPosition};
	for (int r = 0; r < 4; ++r) {
		listener.m[r][0] = rows[r].x;
		listener.m[r][1] = rows[r].y;
		listener.m[r][2] = rows[r].z;
		listener.m[r][3] = r == 3 ? 1.0f : 0.0f;
	}
	m_listenerMatrix = inverseRigid(listener);
	m_listenerVelocityLocal = m_listenerMatrix.transformVector(*m_listenerVelocity);
	if (m_provider) {
		m_provider->setListenerVelocity(m_listenerVelocityLocal);
		if (m_reverb) {
			m_reverb->setRoomGain(m_indoorLevel);
			m_reverb->apply(*m_provider);
		}
	}
}

void BfSoundEngine::toListenerSpace(Sound& sound, Vec3& position, Vec3& velocity) const
{
	if (sound.headRelative()) {
		position = {};
		velocity = {};
		return;
	}
	position = m_listenerMatrix.transformPoint(position);
	velocity = m_listenerMatrix.transformVector(velocity);
}

float BfSoundEngine::airAbsorption(float distance) const
{
	// High frequencies fade with distance; EAX does it in hardware with its own range.
	const SoundTweaks& tweaks = soundTweaks();
	float maximum = 1.0f;
	float near = tweaks.get(Tweak::AirAbsorptionEaxMinDistance);
	float far = tweaks.get(Tweak::AirAbsorptionEaxMaxDistance);
	if (const_cast<BfSoundEngine*>(this)->eaxVersion() < 2) {
		maximum = tweaks.get(Tweak::AirAbsorptionMax);
		near = tweaks.get(Tweak::AirAbsorptionMinDistance);
		far = tweaks.get(Tweak::AirAbsorptionMaxDistance);
	}
	if (distance <= near) {
		return 0.0f;
	}
	if (distance < far) {
		return (distance - near) * maximum / (far - near);
	}
	return maximum;
}

// --- Scheduling ---------------------------------------------------------------------

bool BfSoundEngine::shouldCull(Sound& sound, const Vec3* position)
{
	if (!soundTweaks().enabled(Tweak::EnableCulling)) {
		return false;
	}
	SoundTemplate& templ = sound.templ();
	if (!templ.is3dSound() || templ.loopCount() == 0) {
		return false; // never cull 2D sounds or loops: they may come into range
	}
	const float distance = (*m_listenerPosition - sound.worldPosition(position)).length();
	if (distance < kNeverCullDistance || distance <= templ.minDistance()) {
		return false;
	}
	if (distance > templ.maxDistance()) {
		return true;
	}
	const float attenuated = sound.computeVolume(0.0f, 0) /
		(((distance - templ.minDistance()) * templ.rolloffFactor()) / templ.minDistance() + 1.0f);
	return attenuated < kCullVolume;
}

void BfSoundEngine::scheduleSample(Sound& sound, int sampleIndex, const Vec3* position)
{
	if (!m_provider) {
		return;
	}
	SoundTemplate& templ = sound.templ();
	auto* file = static_cast<SoundFile*>(templ.loadedSoundFile(sampleIndex));
	if (!file || shouldCull(sound, position)) {
		return;
	}
	Sample* sample = m_provider->createSample(templ, sampleIndex, position).release();
	if (!sample) {
		return;
	}
	sample->setSound(&sound);
	sound.sampleStarted();
	sample->sampleIndex = static_cast<int16_t>(sampleIndex);
	if (file->frames() == 0) {
		delete sample;
		return;
	}
	m_starting.push_back(sample);
	sample->voiceAssigned = false;
}

void BfSoundEngine::pushPending(Sample* sample)
{
	m_pending.push_back(sample);
	std::push_heap(m_pending.begin(), m_pending.end(), laterStart);
}

Sample* BfSoundEngine::popPending()
{
	std::pop_heap(m_pending.begin(), m_pending.end(), laterStart);
	Sample* sample = m_pending.back();
	m_pending.pop_back();
	return sample;
}

void BfSoundEngine::queueStartingSample(Sample* sample)
{
	if (!sample) {
		return;
	}
	// Distant one-shots start late: sound travels at 343 m/s.
	int delay = 0;
	if (sample->is3d() && sample->sound()->loopCountForPlay() > 0) {
		const float distance = (sample->worldPosition() - *m_listenerPosition).length();
		const float threshold = soundTweaks().get(Tweak::DistanceDelayThreshold);
		if (distance > threshold) {
			delay = static_cast<int>(std::floor((distance - threshold) * soundTweaks().get(Tweak::DistanceDelayFactor) *
				kSecondsPerMetre * kTicksPerSecond));
		}
	}
	sample->startTime = sample->scheduledTime = m_now + delay;
	const int random = std::rand();
	sample->pitchSeed = static_cast<uint8_t>(random);
	sample->volumeSeed = static_cast<uint8_t>(random >> 8);
	pushPending(sample);
}

void BfSoundEngine::requeuePlaying()
{
	// Everything playing or waiting goes back to pending, e.g. while the
	// provider is reopened.
	for (Sample* sample : m_starting) {
		delete sample;
	}
	m_starting.clear();
	while (!m_playing.empty()) {
		Sample* sample = m_playing.front();
		m_playing.pop_front();
		sample->releaseVoice();
		sample->scheduledTime = m_now;
		pushPending(sample);
	}
	while (!m_virtual.empty()) {
		Sample* sample = m_virtual.front();
		m_virtual.pop_front();
		sample->scheduledTime = m_now + 1;
		pushPending(sample);
	}
}

void BfSoundEngine::startNextPending()
{
	Sample* sample = popPending();
	if (sample->sound()) {
		const bool stillPlaying = sample->durationTicks() > m_now - sample->startTime;
		if (sample->sampleIndex < sample->sound()->templ().usableSampleCount() && stillPlaying) {
			if (sample->voiceAssigned || sample->sound()->claimVoice(sample)) {
				sample->voiceAssigned = true;
				m_virtual.push_front(sample);
				return;
			}
		}
	}
	delete sample;
}

bool BfSoundEngine::startVirtualSample()
{
	Sample* sample = m_virtual.front();
	m_virtual.pop_front();
	const SoundFile* file = sample->soundFile();
	int loopsLeft = sample->sound()->loopCountForPlay();

	if (!sample->started) {
		if (m_now - sample->startTime > kMaxStartDelayTicks && loopsLeft != 0) {
			delete sample; // too late for a one-shot
			return true;
		}
		sample->startTime = m_now;
	}

	// Work out where in the file the sample should be by now.
	const int elapsed = m_now - sample->startTime;
	int duration = sample->durationTicks();
	if (sample->stopRequested() && loopsLeft == 0) {
		// A stopped loop only has its end block left.
		const float pitch = sample->sound()->computePitch(0.0f, 0);
		duration = static_cast<int>(std::lround(static_cast<float>(kTicksPerSecond) /
			static_cast<float>(file->sampleRate()) * pitch * static_cast<float>(file->frames() - file->loopStart())));
	}
	int startFrame;
	if (elapsed < duration) {
		if (sample->stopRequested()) {
			startFrame = file->loopEnd();
		}
		else if (elapsed < 11) {
			startFrame = 0;
		}
		else if (loopsLeft == 0) {
			startFrame = file->loopStart();
		}
		else if (loopsLeft >= 2) {
			startFrame = file->loopStart();
			loopsLeft = 1;
		}
		else {
			startFrame = file->loopEnd();
		}
	}
	else {
		startFrame = static_cast<int>(file->frames());
	}
	if (startFrame == file->loopEnd() && loopsLeft == 1 && file->endBuffer() == 0) {
		startFrame = file->loopStart();
	}

	if (startFrame == static_cast<int>(file->frames())) {
		delete sample;
		return true;
	}
	switch (sample->start(startFrame, loopsLeft)) {
	case Sample::StartResult::Started:
		m_playing.push_back(sample);
		sample->started = true;
		return true;
	case Sample::StartResult::NoVoice:
		m_virtual.push_front(sample);
		return false;
	default:
		delete sample;
		return true;
	}
}

void BfSoundEngine::stealVoice()
{
	Sample* sample = m_playing.back();
	m_playing.pop_back();
	sample->releaseVoice();
	// Retry shortly if more than half a second remains.
	if (m_now - sample->startTime + 15 < sample->durationTicks()) {
		sample->scheduledTime = m_now + 5;
		pushPending(sample);
	}
	else {
		delete sample;
	}
}

void BfSoundEngine::computePriorities(std::list<Sample*>& samples)
{
	for (Sample* sample : samples) {
		float priority = 2.0f; // 2D sounds outrank anything audible in 3D
		if (sample->is3d()) {
			SoundTemplate& templ = sample->sound()->templ();
			const float distance = (*m_listenerPosition - sample->worldPosition()).length();
			const float minDistance = templ.minDistance();
			priority = sample->sound()->computeVolume(0.0f, 0);
			if (minDistance < distance) {
				priority /= ((distance - minDistance) * templ.rolloffFactor()) / minDistance + 1.0f;
				if (templ.maxDistance() <= distance) {
					priority -= 1.0f;
				}
			}
		}
		sample->priority = priority;
	}
}

void BfSoundEngine::updateSamples(int)
{
	for (auto it = m_playing.begin(); it != m_playing.end();) {
		if ((*it)->state() == Sample::State::Finished) {
			delete *it;
			it = m_playing.erase(it);
		}
		else {
			++it;
		}
	}
	while (!m_pending.empty() && m_pending.front()->scheduledTime <= m_now) {
		startNextPending();
	}

	if (!m_virtual.empty()) {
		for (auto it = m_virtual.begin(); it != m_virtual.end();) {
			if ((*it)->durationTicks() <= m_now - (*it)->startTime) {
				delete *it;
				it = m_virtual.erase(it);
			}
			else {
				++it;
			}
		}
		computePriorities(m_virtual);
		m_virtual.sort(higherPriority);
		computePriorities(m_playing);
		m_playing.sort(higherPriority);

		while (!m_virtual.empty() && startVirtualSample()) {
		}
		// Steal voices from quieter playing samples.
		while (!m_virtual.empty() && !m_playing.empty()) {
			const Sample* waiting = m_virtual.front();
			const Sample* weakest = m_playing.back();
			if (waiting->priority == weakest->priority ? waiting->startTime <= weakest->startTime
			                                           : waiting->priority <= weakest->priority) {
				break;
			}
			stealVoice();
			startVirtualSample();
		}
	}

	for (Sample* sample : m_playing) {
		sample->update();
	}
}

// --- Stopping -------------------------------------------------------------------

bool BfSoundEngine::stopInList(std::list<Sample*>& list, Sample* sample, bool immediate)
{
	for (auto it = list.begin(); it != list.end(); ++it) {
		if (*it == sample) {
			if (sample->stop(immediate)) {
				delete sample;
				list.erase(it);
			}
			return true;
		}
	}
	return false;
}

void BfSoundEngine::stopSoundInList(std::list<Sample*>& list, Sound* sound, bool immediate)
{
	for (auto it = list.begin(); it != list.end();) {
		if ((*it)->sound() == sound && (*it)->stop(immediate)) {
			delete *it;
			it = list.erase(it);
		}
		else {
			++it;
		}
	}
}

void BfSoundEngine::stopSample(Sample* sample, bool immediate)
{
	for (Sample*& starting : m_starting) {
		if (starting && starting == sample) {
			if (immediate) {
				delete starting;
				starting = nullptr;
			}
			else {
				m_stopping.push_back(starting);
			}
			return;
		}
	}
	if (stopInList(m_playing, sample, immediate) || stopInList(m_virtual, sample, true)) {
		return;
	}
	// Still pending: orphan it; it is deleted when it comes up.
	if (std::find(m_pending.begin(), m_pending.end(), sample) != m_pending.end()) {
		if (Sound* sound = sample->sound()) {
			sound->sampleFinished(sample);
		}
		sample->setSound(nullptr);
	}
}

void BfSoundEngine::stopSound(Sound* sound, bool immediate)
{
	for (Sample*& starting : m_starting) {
		if (starting && starting->sound() == sound) {
			if (immediate) {
				delete starting;
				starting = nullptr;
			}
			else {
				m_stopping.push_back(starting);
			}
		}
	}
	stopSoundInList(m_playing, sound, immediate);
	stopSoundInList(m_virtual, sound, true);
	for (Sample* sample : m_pending) {
		if (sample->sound() == sound) {
			sound->sampleFinished(sample);
			sample->setSound(nullptr);
		}
	}
}

void BfSoundEngine::processStops()
{
	const std::vector<Sample*> stopping = std::move(m_stopping);
	m_stopping.clear();
	for (Sample* sample : stopping) {
		stopSample(sample, false);
	}
}

void BfSoundEngine::stopAllSounds()
{
	for (Sample* sample : m_starting) {
		delete sample;
	}
	m_starting.clear();
	for (std::list<Sample*>* list : {&m_playing, &m_virtual}) {
		for (auto it = list->begin(); it != list->end();) {
			if ((*it)->stop(true)) {
				delete *it;
				it = list->erase(it);
			}
			else {
				++it;
			}
		}
	}
	processStops();
	for (Sample* sample : m_pending) {
		delete sample;
	}
	m_pending.clear();
}

void BfSoundEngine::updateFades()
{
	for (auto it = m_fading.begin(); it != m_fading.end();) {
		ISound* sound = *it;
		if (!sound || !sound->isFading()) {
			SND_INFO("[KRE] Something is weird: %s", sound ? sound->soundTemplate()->soundFilename() : "");
		}
		else if (sound->updateFade(m_now)) {
			++it;
			continue;
		}
		else {
			SND_INFO("[KRE] Fading done: %s", sound->soundTemplate()->soundFilename());
			stopSound(static_cast<Sound*>(sound), true);
		}
		it = m_fading.erase(it);
	}
}

// --- Singleton / exports --------------------------------------------------------

namespace {
BfSoundEngine* g_engine = nullptr;
} // namespace

BfSoundEngine& soundEngine()
{
	return *g_engine;
}

ISoundEngine* getSoundEngine()
{
	if (!g_engine) {
		g_engine = new BfSoundEngine();
	}
	return g_engine;
}

void shutdownSoundEngine()
{
	if (g_engine) {
		g_engine->destroy(1);
	}
	g_engine = nullptr;
}

} // namespace dice::hfe::sound
