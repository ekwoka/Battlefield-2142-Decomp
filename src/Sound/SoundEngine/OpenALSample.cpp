#include "OpenALSample.h"

#include "BfSoundEngine.h"
#include "OggStream.h"
#include "OpenALProvider.h"
#include "Reverb.h"
#include "Sound.h"
#include "SoundFile.h"
#include "SoundServices.h"
#include "SoundTemplate.h"
#include "SoundTweaks.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <list>

namespace dice::hfe::sound {

namespace {

SamplePoolStats g_stats;
int g_maxSources = 16;
std::list<ALuint> g_sourcePool;
std::list<ALuint> g_streamingBufferPool;

// Decoding scratch shared by all streaming samples (they decode one at a time).
uint8_t g_streamScratch[kStreamChunkBytes];

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// Gain to EAX millibels for per-source properties.
int32_t gainToMB(float gain)
{
	if (gain <= 0.0001f) {
		return -10000;
	}
	if (gain >= 0.9999f) {
		return 0;
	}
	return static_cast<int32_t>(std::lround(std::log(gain) * 868.5889638f));
}

void returnSourceToPool(ALuint source)
{
	if (!source) {
		return;
	}
	al().alSourceRewind(source);
	al().alSourcei(source, AL_BUFFER, 0);
	g_sourcePool.push_back(source);
	if (!soundTweaks().enabled(Tweak::UseSourcePool)) {
		purgeSourcePool();
	}
}

// Takes a source from the pool, keeping min-free-sources spare, or creates
// one while under the voice limit.
ALuint acquireSource()
{
	if (soundTweaks().enabled(Tweak::UseSourcePool)) {
		const size_t minFree = static_cast<size_t>(std::lround(soundTweaks().get(Tweak::MinFreeSources)));
		if (g_stats.sources == 0) {
			const size_t prefill = std::min(minFree, static_cast<size_t>(g_maxSources));
			for (size_t i = 0; i < prefill; ++i) {
				ALuint source = 0;
				al().alGenSources(1, &source);
				if (source) {
					++g_stats.sources;
					returnSourceToPool(source);
				}
			}
		}
		if (minFree < g_sourcePool.size()) {
			const ALuint source = g_sourcePool.front();
			g_sourcePool.pop_front();
			return source;
		}
	}
	ALuint source = 0;
	if (g_stats.sources < g_maxSources) {
		al().alGenSources(1, &source);
		if (source) {
			++g_stats.sources;
		}
	}
	return source;
}

ALuint acquireStreamingBuffer()
{
	if (!g_streamingBufferPool.empty()) {
		const ALuint buffer = g_streamingBufferPool.front();
		g_streamingBufferPool.pop_front();
		return buffer;
	}
	ALuint buffer = 0;
	genStreamingBuffers(1, &buffer);
	CHECK_AL_ERROR();
	++g_stats.streamingBuffers;
	g_stats.streamingBytes += kStreamChunkBytes;
	return buffer;
}

void returnStreamingBuffer(ALuint buffer)
{
	g_streamingBufferPool.push_back(buffer);
}

// Decodes the next chunk of `file` into `buffer`. Looping streams rewind at
// the end until loopsRemaining runs out.
bool fillStreamBuffer(OggDecoder& decoder, const SoundFile& file, uint8_t* scratch, int& loopsRemaining,
	ALuint buffer, bool logErrors)
{
	int chunk = file.channels() == 2 ? 0x2000 : 0x1000;
	if (file.sampleRate() < 12000) {
		chunk /= 4;
	}
	else if (file.sampleRate() < 25000) {
		chunk /= 2;
	}

	int filled = 0;
	while (filled < chunk) {
		const long read = decoder.read(scratch + filled, chunk - filled);
		if (read > 0) {
			filled += static_cast<int>(read);
			continue;
		}
		if (read != 0) {
			if (logErrors) {
				SND_WARNING("Failed to read from ogg file %s. Error: %s", file.path().c_str(),
					oggErrorName(static_cast<int>(read)));
			}
			return false;
		}
		if (--loopsRemaining < 1) {
			break;
		}
		decoder.seekFrame(0);
	}
	if (filled == 0) {
		return false;
	}
	al().alBufferData(buffer, file.channels() == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, scratch, filled,
		static_cast<ALsizei>(file.sampleRate()));
	if (logErrors) {
		CHECK_AL_ERROR();
	}
	return true;
}

} // namespace

SamplePoolStats& samplePoolStats() { return g_stats; }
void setMaxSources(int count) { g_maxSources = count; }

void purgeSourcePool()
{
	for (ALuint source : g_sourcePool) {
		al().alDeleteSources(1, &source);
		CHECK_AL_ERROR();
		--g_stats.sources;
	}
	g_sourcePool.clear();
}

void purgeStreamingBufferPool()
{
	for (ALuint buffer : g_streamingBufferPool) {
		al().alDeleteBuffers(1, &buffer);
		CHECK_AL_ERROR();
		--g_stats.streamingBuffers;
		g_stats.streamingBytes -= kStreamChunkBytes;
	}
	g_streamingBufferPool.clear();
}

bool startOggStream(std::unique_ptr<OggDecoder>& decoder, const SoundFile& file, uint8_t* scratch,
	int& loopsRemaining, int startFrame, ALuint source, ALuint buffer, bool logErrors)
{
	bool fresh = false;
	if (!decoder) {
		fresh = true;
		decoder = std::make_unique<OggDecoder>();
		const int result = decoder->open(file.encodedData(), logErrors);
		if (result != 0) {
			if (logErrors) {
				SND_ERROR("ov_open_callbacks failed (%d)", result);
			}
			return false;
		}
	}
	al().alSourcei(source, AL_LOOPING, 0);
	// Only a reused decoder honours the start frame; a new one starts at 0.
	if (!fresh && startFrame != -1) {
		decoder->seekFrame(startFrame);
	}
	if (!fillStreamBuffer(*decoder, file, scratch, loopsRemaining, buffer, logErrors)) {
		return false;
	}
	al().alSourceQueueBuffers(source, 1, &buffer);
	if (logErrors) {
		CHECK_AL_ERROR();
	}
	return true;
}

bool updateOggStream(OggDecoder* decoder, const SoundFile& file, uint8_t* scratch, int& loopsRemaining,
	ALuint source, ALuint primeBuffer, bool logErrors)
{
	if (loopsRemaining < 1 || !decoder) {
		return false;
	}
	bool usedPrimeBuffer = false;
	if (primeBuffer == 0) {
		ALint processed = 0;
		al().alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
		processed = std::min(processed, 2); // refill at most two chunks a frame
		while (processed-- > 0) {
			ALuint buffer = 0;
			al().alSourceUnqueueBuffers(source, 1, &buffer);
			if (logErrors) {
				CHECK_AL_ERROR();
			}
			if (!fillStreamBuffer(*decoder, file, scratch, loopsRemaining, buffer, logErrors)) {
				break;
			}
			al().alSourceQueueBuffers(source, 1, &buffer);
			if (logErrors) {
				CHECK_AL_ERROR();
			}
		}
	}
	else {
		usedPrimeBuffer = true;
		if (fillStreamBuffer(*decoder, file, scratch, loopsRemaining, primeBuffer, logErrors)) {
			al().alSourceQueueBuffers(source, 1, &primeBuffer);
			if (logErrors) {
				CHECK_AL_ERROR();
			}
		}
	}

	// Restart after an underrun.
	ALint queued = 0;
	ALint state = 0;
	al().alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
	al().alGetSourcei(source, AL_SOURCE_STATE, &state);
	if (queued > 0 && state == AL_STOPPED) {
		al().alSourcePlay(source);
		if (logErrors) {
			CHECK_AL_ERROR();
		}
	}
	return usedPrimeBuffer;
}

// --- Sample -----------------------------------------------------------------

Sample::Sample()
{
	++g_stats.samples;
}

Sample::~Sample()
{
	if (m_sound) {
		m_sound->sampleFinished(this);
	}
	m_sound = nullptr;
	--g_stats.samples;
}

SoundFile* Sample::soundFile() const
{
	return m_sound ? m_sound->templ().soundFile(sampleIndex) : nullptr;
}

bool Sample::is3d() const
{
	return m_sound->templ().is3dSound();
}

int Sample::durationTicks() const
{
	return m_sound->templ().sampleDurationTicks(sampleIndex);
}

float Sample::updateLoopProgress(int playOffset, int elapsedTicks)
{
	const SoundFile* file = soundFile();
	float progress;
	if (playOffset >= file->loopEnd()) {
		progress = 1.0f;
	}
	else if (playOffset < file->loopStart()) {
		progress = 0.0f;
	}
	else {
		// Estimated from elapsed time at the engine's output rate.
		const int loopLength = file->loopEnd() - file->loopStart();
		const int framesPerTick = static_cast<int>(soundEngine().outputRate() / 30);
		progress = static_cast<float>((framesPerTick * elapsedTicks - file->loopStart()) % loopLength) /
			static_cast<float>(loopLength);
	}
	if (progress < loopProgress) {
		++pitchSeed;
		++volumeSeed;
	}
	loopProgress = progress;
	return progress;
}

// --- OpenALSample -----------------------------------------------------------

OpenALSample::OpenALSample(const Vec3* positionOffset)
{
	if (positionOffset) {
		m_offset = *positionOffset;
	}
}

OpenALSample::~OpenALSample() = default;

void OpenALSample::releaseVoice()
{
	if (m_source) {
		al().alSourceStop(m_source);
		unqueueAll();
		returnSourceToPool(m_source);
		m_source = 0;
	}
}

int OpenALSample::unqueueAll()
{
	ALint processed = 0;
	al().alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processed);
	for (ALint i = 0; i < processed; ++i) {
		ALuint buffer;
		al().alSourceUnqueueBuffers(m_source, 1, &buffer);
	}
	return processed;
}

Sample::State OpenALSample::state()
{
	ALint state = 0;
	al().alGetSourcei(m_source, AL_SOURCE_STATE, &state);
	switch (state) {
	case AL_INITIAL:
		return State::Finished;
	case AL_PLAYING:
	case AL_PAUSED:
		return State::Playing;
	case AL_STOPPED:
		// A stopped streaming source may just have underrun.
		return finishedLooping() ? State::Finished : State::Playing;
	default:
		return State::None;
	}
}

Vec3 OpenALSample::worldPosition()
{
	return sound()->worldPosition(&m_offset);
}

Sample::StartResult OpenALSample::start(int startFrame, int loopCount)
{
	clearALError();
	m_source = acquireSource();
	if (!m_source) {
		return StartResult::NoVoice;
	}
	al().alSourcef(m_source, AL_PITCH, 1.0f);
	al().alSourcef(m_source, AL_GAIN, 0.0f);
	al().alSourcei(m_source, AL_LOOPING, 0);
	if (!queueBuffers(startFrame, loopCount)) {
		returnSourceToPool(m_source);
		m_source = 0;
		return StartResult::Failed;
	}
	const ALfloat zero[] = {0.0f, 0.0f, 0.0f};
	const ALfloat back[] = {0.0f, 0.0f, -1.0f};
	al().alSourcefv(m_source, AL_POSITION, zero);
	al().alSourcefv(m_source, AL_VELOCITY, zero);
	al().alSourcefv(m_source, AL_DIRECTION, back);
	applyDistanceModel();
	update();
	al().alSourcePlay(m_source);
	CHECK_AL_ERROR();
	return StartResult::Started;
}

void OpenALSample::applyDistanceModel()
{
	SoundTemplate& templ = sound()->templ();
	al().alSourcef(m_source, AL_REFERENCE_DISTANCE, templ.minDistance());
	al().alSourcef(m_source, AL_ROLLOFF_FACTOR, templ.rolloffFactor());
	if (eaxAvailable()) {
		m_eax.room = gainToMB(templ.reverbLevel());
	}
	if (templ.hasCone()) {
		al().alSourcef(m_source, AL_CONE_INNER_ANGLE, templ.coneInnerAngle());
		al().alSourcef(m_source, AL_CONE_OUTER_ANGLE, templ.coneOuterAngle());
		al().alSourcef(m_source, AL_CONE_OUTER_GAIN, templ.coneOuterVolume());
	}
	else {
		al().alSourcef(m_source, AL_CONE_INNER_ANGLE, 360.0f);
		al().alSourcef(m_source, AL_CONE_OUTER_ANGLE, 360.0f);
		al().alSourcef(m_source, AL_CONE_OUTER_GAIN, 0.0f);
	}
}

void OpenALSample::setOcclusion(float occlusion)
{
	m_occlusion = occlusion;
	if (eaxAvailable() && is3d()) {
		const float listener = clamp01(soundTweaks().get(Tweak::Listener3dObstruction));
		m_eax.obstruction = gainToMB((1.0f - clamp01(occlusion)) * (1.0f - listener));
	}
}

void OpenALSample::setTinnitusMuffle(float amount)
{
	if (eaxAvailable()) {
		m_eax.occlusion = gainToMB(1.0f - clamp01(amount));
	}
}

float OpenALSample::computeOcclusionGain()
{
	Sound& snd = *sound();
	float occlusion = 0.0f;
	if (ISoundOcclusion* query = snd.occlusion()) {
		const Vec3 position = worldPosition();
		const float target = query->occlusion(&snd, &position);
		if (state() == State::Finished) {
			occlusion = target; // not audible yet: snap
		}
		else {
			occlusion = m_occlusion + std::clamp(target - m_occlusion, -0.1f, 0.1f);
		}
	}
	occlusion *= snd.templ().occlusionFactor();
	setOcclusion(occlusion);

	if (!eaxAvailable() && is3d()) {
		const float listener = soundTweaks().get(Tweak::Listener3dObstruction);
		return clamp01(1.0f - (listener + occlusion) * soundTweaks().get(Tweak::Listener3dObstructionFactor));
	}
	return clamp01(1.0f - occlusion);
}

void OpenALSample::update()
{
	clearALError();
	SoundFile* file = soundFile();
	if (!file || !file->loaded()) {
		return;
	}
	updateQueue();

	Sound& snd = *sound();
	SoundTemplate& templ = snd.templ();
	BfSoundEngine& engine = soundEngine();
	float directGain = 1.0f;

	if (is3d()) {
		Vec3 position = worldPosition();
		Vec3 velocity = snd.velocity();
		// Guard OpenAL against garbage from the game.
		if (std::fabs(position.x) >= 20000.0f || std::fabs(position.y) >= 20000.0f ||
			std::fabs(position.z) >= 20000.0f || !isFinite(position)) {
			position = {20000.0f, 20000.0f, 20000.0f};
		}
		if (std::fabs(velocity.x) >= 300.0f || std::fabs(velocity.y) >= 300.0f || std::fabs(velocity.z) >= 300.0f ||
			!isFinite(velocity)) {
			velocity = {};
		}
		engine.toListenerSpace(snd, position, velocity);
		position.z = -position.z;
		velocity.z = -velocity.z;
		al().alSourcefv(m_source, AL_POSITION, &position.x);
		al().alSourcefv(m_source, AL_VELOCITY, &velocity.x);

		if (templ.hasCone()) {
			Vec3 direction = snd.worldMatrix().transformVector(*templ.direction());
			if (std::fabs(direction.x) >= 1.1f || std::fabs(direction.y) >= 1.1f || std::fabs(direction.z) >= 1.1f ||
				!isFinite(direction)) {
				direction = {1.0f, 0.0f, 0.0f};
			}
			direction = engine.listenerMatrix().transformVector(direction);
			direction.z = -direction.z;
			al().alSourcefv(m_source, AL_DIRECTION, &direction.x);
		}
		if (file->frames() < 1) {
			return;
		}
		const float airAbsorption = engine.airAbsorption(position.length());
		if (eaxAvailable()) {
			m_eax.airAbsorptionFactor = airAbsorption * 5.0f;
		}
	}

	// The tinnitus effect muffles sounds through EAX, or by lowering their gain.
	const float tinnitus = templ.occlusionFactor() * engine.tinnitusLevel();
	if (!eaxAvailable() || (!is3d() && !soundTweaks().enabled(Tweak::Use2dOcclusion))) {
		directGain = 1.0f - tinnitus * 0.9f;
	}
	else {
		setTinnitusMuffle(tinnitus);
	}

	const float occlusionGain = computeOcclusionGain();
	const float loopProgressValue = updateLoopProgress(playOffset(), engine.now() - startTime);
	const float balance = is3d() ? engine.gain3d() : engine.gain2d();
	const float volume = clamp01(snd.computeVolume(loopProgressValue, volumeSeed) * balance);
	al().alSourcef(m_source, AL_GAIN, volume * occlusionGain * directGain);
	al().alSourcef(m_source, AL_PITCH, snd.computePitch(loopProgressValue, pitchSeed));

	if (engine.editorMode()) {
		applyDistanceModel(); // templates may be edited live
	}
	if (eaxAvailable()) {
		eax().set(&kEax30BufferProperties, EaxBufferAllParameters, m_source, &m_eax, sizeof(m_eax));
	}
	CHECK_AL_ERROR();
}

bool OpenALSample::stop(bool immediate)
{
	if (m_stopRequested && !immediate) {
		return false;
	}
	SoundFile* file = soundFile();
	if (!file || !file->loaded() || !m_source) {
		return true;
	}
	clearALError();
	const int stopType = immediate ? 0 : sound()->templ().stopType();
	bool finished = false;
	switch (stopType) {
	case 1: // let the current loop run out into the end block
		queueEndBlock();
		break;
	case 2: // cut to the end block right away
		if (file->loopEnd() != static_cast<int32_t>(file->frames())) {
			al().alSourceStop(m_source);
			unqueueAll();
			if (queueEndBlock()) {
				al().alSourcePlay(m_source);
				break;
			}
			finished = true;
			break;
		}
		[[fallthrough]];
	default: // stop dead
		al().alSourceStop(m_source);
		unqueueAll();
		finished = true;
		break;
	}
	m_stopRequested = true;
	CHECK_AL_ERROR();
	return finished;
}

// --- StaticSample -----------------------------------------------------------

StaticSample::~StaticSample()
{
	releaseVoice();
}

int StaticSample::playOffset()
{
	if (m_loopAfterStart) {
		return 0;
	}
	const SoundFile* file = soundFile();
	return m_stopRequested ? file->loopEnd() : file->loopStart();
}

bool StaticSample::queueBuffers(int startFrame, int loopCount)
{
	m_loopAfterStart = false;
	const SoundFile* file = soundFile();
	int queued = 0;
	const auto queue = [&](ALuint buffer) {
		al().alSourceQueueBuffers(m_source, 1, &buffer);
		++queued;
	};

	if (startFrame == 0 && file->startBuffer() && !(sound()->playFlags() & Sound::SkipStartBlock)) {
		queue(file->startBuffer());
	}
	if (startFrame <= file->loopStart() && file->loopBuffer()) {
		if (loopCount == 0) {
			// Loop forever: switch the source to looping once the start block is done.
			if (queued == 0) {
				al().alSourcei(m_source, AL_LOOPING, 1);
			}
			else {
				m_loopAfterStart = true;
			}
			queue(file->loopBuffer());
		}
		else {
			for (int i = 0; i < loopCount; ++i) {
				queue(file->loopBuffer());
			}
		}
	}
	if (file->endBuffer() && startFrame <= file->loopEnd() && (loopCount != 0 || startFrame == file->loopEnd())) {
		queue(file->endBuffer());
	}
	return queued > 0;
}

void StaticSample::updateQueue()
{
	if (m_stopRequested || !m_loopAfterStart) {
		return;
	}
	ALint processed = 0;
	al().alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processed);
	if (processed > 0) {
		ALuint startBlock;
		al().alSourceUnqueueBuffers(m_source, 1, &startBlock);
		al().alSourcei(m_source, AL_LOOPING, 1);
		al().alSourcePlay(m_source);
		m_loopAfterStart = false;
	}
}

bool StaticSample::queueEndBlock()
{
	const SoundFile* file = soundFile();
	const bool hasEnd = file->endBuffer() != 0;
	if (hasEnd) {
		const ALuint end = file->endBuffer();
		al().alSourceQueueBuffers(m_source, 1, &end);
	}
	al().alSourcei(m_source, AL_LOOPING, 0);
	return hasEnd;
}

// --- StreamingSample --------------------------------------------------------

StreamingSample::StreamingSample(const Vec3* positionOffset) : OpenALSample(positionOffset) {}

StreamingSample::~StreamingSample()
{
	releaseVoice();
	for (ALuint& buffer : m_buffers) {
		if (buffer) {
			returnStreamingBuffer(buffer);
			buffer = 0;
		}
	}
}

bool StreamingSample::queueBuffers(int startFrame, int loopCount)
{
	m_loopsRemaining = loopCount == 0 ? INT_MAX : loopCount;
	for (ALuint& buffer : m_buffers) {
		buffer = acquireStreamingBuffer();
	}
	m_primed = 0;
	if (startOggStream(m_decoder, *soundFile(), g_streamScratch, m_loopsRemaining, startFrame, m_source,
			m_buffers[m_primed], true)) {
		++m_primed;
	}
	return true;
}

void StreamingSample::updateQueue()
{
	const ALuint prime = m_primed < kBufferCount ? m_buffers[m_primed] : 0;
	if (updateOggStream(m_decoder.get(), *soundFile(), g_streamScratch, m_loopsRemaining, m_source, prime, true)) {
		++m_primed;
	}
}

} // namespace dice::hfe::sound
