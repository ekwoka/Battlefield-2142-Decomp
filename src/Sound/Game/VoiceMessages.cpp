#include "VoiceMessages.h"

#include "SoundManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dice::hfe::sound {

// The volume envelope the radio sounds are given (vtable 0x0092aad0): the
// sound's volume times the input, the voice-over or the filter volume.
class VoiceMessages::VolumeEnvelope final : public ISoundEnvelope {
public:
	float evaluate(float x, unsigned) override { return x; }
};

namespace {

std::string withSlash(std::string path)
{
	if (!path.empty() && path.back() != '/') {
		path += '/';
	}
	return path;
}

float distance(const Vec3& a, const Vec3& b) { return (a - b).length(); }

} // namespace

VoiceMessages::VoiceMessages(SoundManager& manager)
	: m_manager(manager), m_envelope(std::make_unique<VolumeEnvelope>())
{
	reset("English", "English");
}

VoiceMessages::~VoiceMessages() { releaseSounds(); }

void VoiceMessages::releaseSounds()
{
	for (ISound*& sound : m_local) {
		if (sound) {
			sound->stopImmediately();
			sound->release();
			sound = nullptr;
		}
	}
	for (ISound** sound : {&m_filterSound, &m_radio[0], &m_radio[1], &m_help}) {
		if (*sound) {
			(*sound)->stopImmediately();
			(*sound)->release();
			*sound = nullptr;
		}
	}
	m_speaker = {-1, -1};
}

// 0x004cdab0 then 0x004d99c0.
void VoiceMessages::reset(const std::string& team1Language, const std::string& team2Language,
	const std::string& userLanguage)
{
	releaseSounds();
	std::array<std::string, 2> languages {team1Language.empty() ? "English" : team1Language,
		team2Language.empty() ? "English" : team2Language};
	const std::string user = userLanguage.empty() ? "English" : userLanguage;
	if (user != "English") {
		// The team speaking the player's language keeps it; else the first
		// English-speaking team takes it (else team 2's slot, which the
		// original indexes past its two teams: here team 2).
		int keep = -1;
		for (int i = 0; i < 2; ++i) {
			if (languages[static_cast<size_t>(i)] == user) {
				keep = i;
			}
		}
		if (keep < 0) {
			for (int i = 0; i < 2; ++i) {
				if (languages[static_cast<size_t>(i)] == "English") {
					keep = i;
				}
			}
			languages[static_cast<size_t>(keep < 0 ? 1 : keep)] = user;
		}
	}
	m_teamPrefix = {withSlash("sound/vo/" + languages[0]), withSlash("sound/vo/" + languages[1])};
	m_helpPrefix = withSlash("sound/vo/" + user);
	m_messages.clear();
	m_voices.clear();
	m_filters.clear();
	m_helps.clear();
	m_emotes.clear();
	m_table.fill({});
	m_texts.clear();
	for (auto& files : m_radioFiles) {
		files.clear();
	}
	for (auto& files : m_localFiles) {
		files.clear();
	}
	m_filterFiles.clear();
	m_helpFiles.clear();
	m_voiceOverVolume = 1;
	m_filterVolume = 1;
}

int VoiceMessages::addMessage(const std::string& name)
{
	const int index = static_cast<int>(m_messages.size());
	if (index >= kMaxMessages) {
		return -1;
	}
	m_messages[name] = index;
	return index;
}

// To the last message: the voice's samples must come together (a voice's
// entry already used by an earlier run of lines is an error, -1).
int VoiceMessages::addRadioVoice(const std::string& voiceName, const std::string& radioSample,
	const std::string& text, const std::string& localSample, int flags)
{
	if (m_messages.empty()) {
		return -1;
	}
	auto found = m_voices.find(voiceName);
	if (found == m_voices.end()) {
		if (static_cast<int>(m_voices.size()) >= kMaxVoices) {
			return -1;
		}
		found = m_voices.emplace(voiceName, static_cast<int>(m_voices.size())).first;
	}
	const int sample = static_cast<int>(m_texts.size());
	Entry& entry = m_table[static_cast<size_t>((static_cast<int>(m_messages.size()) - 1) * kMaxVoices + found->second)];
	if (entry.first < 0) {
		entry.first = static_cast<short>(sample);
		entry.count = 0;
	} else if (entry.first + entry.count != sample) {
		return -1;
	}
	for (size_t team = 0; team < 2; ++team) {
		m_radioFiles[team].push_back(radioSample.empty() ? std::string() : m_teamPrefix[team] + radioSample);
		m_localFiles[team].push_back({localSample.empty() ? std::string() : m_teamPrefix[team] + localSample, flags});
	}
	m_texts.push_back(text);
	++entry.count;
	return sample;
}

int VoiceMessages::addRadioFilter(const std::string& name, const std::string& file)
{
	const int index = static_cast<int>(m_filterFiles.size());
	m_filters[name] = index;
	m_filterFiles.push_back(file);
	return index;
}

int VoiceMessages::addHelpMessage(const std::string& name, const std::string& file)
{
	const int index = static_cast<int>(m_helpFiles.size());
	m_helps[name] = index;
	m_helpFiles.push_back(file.empty() ? std::string() : m_helpPrefix + file);
	return index;
}

void VoiceMessages::addEmoteTrigger(const std::string& emote)
{
	if (!m_messages.empty()) {
		m_emotes[static_cast<int>(m_messages.size()) - 1] = emote;
	}
}

void VoiceMessages::setVolumes(float voiceOver, float radioFilter)
{
	m_voiceOverVolume = std::clamp(voiceOver, 0.0f, 1.0f);
	m_filterVolume = std::clamp(radioFilter, 0.0f, 1.0f);
}

ISound* VoiceMessages::makeSound(const std::vector<std::string>& files, bool is3d, float reverb, float occlusion,
	int group, const float* volumeInput, int copies, std::vector<ISound*>& out)
{
	ISoundEngine* engine = m_manager.engine();
	if (!engine || files.empty()) {
		return nullptr;
	}
	ISoundTemplate* sound = engine->createTemplate();
	if (!sound) {
		return nullptr;
	}
	std::vector<const char*> paths;
	for (const std::string& file : files) {
		paths.push_back(file.c_str());
	}
	sound->setLoopCount(1);
	sound->setIs3dSound(is3d);
	sound->setSoundFilenames(paths.data(), static_cast<int>(paths.size()));
	sound->setReverbLevel(reverb);
	sound->setOcclusionFactor(occlusion);
	if (is3d) {
		sound->setMinDistance(3);
		sound->setStopType(0);
	}
	if (group >= 0) {
		sound->setVolumeGroup(group);
	}
	sound->markSoundFilesUsed();
	for (int i = 0; i < copies; ++i) {
		ISound* instance = sound->createSound();
		if (instance && volumeInput) {
			instance->setVolumeEnvelope(m_envelope.get(), volumeInput);
		}
		out.push_back(instance);
	}
	sound->release();
	return out.empty() ? nullptr : out.front();
}

void VoiceMessages::build(bool dedicated)
{
	releaseSounds();
	std::vector<ISound*> made;
	m_filterSound = makeSound(m_filterFiles, false, 0, 1, -1, &m_filterVolume, 1, made);
	for (size_t team = 0; team < 2; ++team) {
		made.clear();
		m_radio[team] = makeSound(m_radioFiles[team], false, 0, 1, 2, &m_voiceOverVolume, 1, made);
	}
	if (!dedicated) {
		for (size_t team = 0; team < 2; ++team) {
			std::vector<std::string> files;
			for (const Local& local : m_localFiles[team]) {
				files.push_back(local.file);
			}
			made.clear();
			makeSound(files, true, 1, 1, -1, &m_voiceOverVolume, 2, made);
			for (size_t slot = 0; slot < made.size() && slot < 2; ++slot) {
				m_local[team * 2 + slot] = made[slot];
			}
		}
	}
	made.clear();
	m_help = makeSound(m_helpFiles, false, 0, 0, 2, nullptr, 1, made);
}

int VoiceMessages::message(const std::string& name) const
{
	const auto it = m_messages.find(name);
	return it == m_messages.end() ? -1 : it->second;
}

int VoiceMessages::voice(const std::string& name) const
{
	const auto it = m_voices.find(name);
	return it == m_voices.end() ? -1 : it->second;
}

int VoiceMessages::filter(const std::string& name) const
{
	const auto it = m_filters.find(name);
	return it == m_filters.end() ? -1 : it->second;
}

std::string VoiceMessages::emote(int message) const
{
	const auto it = m_emotes.find(message);
	return it == m_emotes.end() ? std::string() : it->second;
}

std::string VoiceMessages::text(int sample) const
{
	return sample >= 0 && sample < static_cast<int>(m_texts.size()) ? m_texts[static_cast<size_t>(sample)] : std::string();
}

std::pair<int, int> VoiceMessages::samples(int message, int voice) const
{
	if (message < 0 || message >= kMaxMessages || voice < 0 || voice >= kMaxVoices) {
		return {-1, 0};
	}
	const Entry& entry = m_table[static_cast<size_t>(message * kMaxVoices + voice)];
	return {entry.first, entry.first < 0 ? 0 : entry.count};
}

int VoiceMessages::pickSample(int message, int voice) const
{
	const auto [first, count] = samples(message, voice);
	if (first < 0 || count < 1) {
		return -1;
	}
	return count > 1 ? first + std::rand() % count : first;
}

std::string VoiceMessages::radioFile(int team, int index) const
{
	const auto& files = m_radioFiles[team == 2 ? 1 : 0];
	return index >= 0 && index < static_cast<int>(files.size()) ? files[static_cast<size_t>(index)] : std::string();
}

std::string VoiceMessages::localFile(int team, int index) const
{
	const auto& files = m_localFiles[team == 2 ? 1 : 0];
	return index >= 0 && index < static_cast<int>(files.size()) ? files[static_cast<size_t>(index)].file : std::string();
}

int VoiceMessages::playRadio(int team, int message, int voice, int filter)
{
	if (team < 1 || team > 2) {
		return -1;
	}
	const int sample = pickSample(message, voice);
	if (sample < 0 || radioFile(team, sample).empty()) {
		return -1;
	}
	if (onPlay) {
		onPlay(radioFile(team, sample), false);
	}
	ISound* radio = m_radio[static_cast<size_t>(team - 1)];
	if (!radio || !radio->playSampleLooped(sample, 1)) {
		return m_manager.engine() ? -1 : sample;
	}
	if (filter >= 0 && m_filterSound) {
		// The static loops over the message: the sample's length (its
		// timing's second value, the loop) less the filter's start, in
		// filter loops, plus one.
		const float* timing = radio->sampleTiming(sample);
		const float* filterTiming = m_filterSound->sampleTiming(filter);
		if (!timing || !filterTiming) {
			return -1;
		}
		int loops = 1;
		if (filterTiming[1] > 0) {
			loops = std::max(1, static_cast<int>(std::lround((timing[1] - filterTiming[0]) / filterTiming[1])) + 1);
		}
		m_filterSound->playSampleLooped(filter, loops);
	}
	return sample;
}

int VoiceMessages::playLocal(int listenerTeam, int message, int voice, const Vec3& listener, const Speaker& speaker)
{
	const int sample = pickSample(message, voice);
	if (sample < 0) {
		return -1;
	}
	const int team = speaker.team == 2 ? 2 : 1;
	const Local& local = m_localFiles[static_cast<size_t>(team - 1)][static_cast<size_t>(sample)];
	if (local.file.empty()) {
		return -1;
	}
	if (!(local.flags & 1) && speaker.team != listenerTeam) {
		return -1;
	}
	if (!(local.flags & 2) && distance(speaker.position, listener) > kLocalDistance) {
		return -1;
	}
	// 0x004d7a30: a slot whose voices are silent, else the farther speaker's
	// if this one is nearer.
	int slot = -1;
	for (int i = 0; i < 2; ++i) {
		bool playing = false;
		for (int t = 0; t < 2; ++t) {
			if (ISound* sound = m_local[static_cast<size_t>(t * 2 + i)]; sound && sound->isPlaying()) {
				playing = true;
			}
		}
		if (!playing) {
			m_speaker[static_cast<size_t>(i)] = -1;
		}
	}
	for (int i = 0; i < 2 && slot < 0; ++i) {
		if (m_speaker[static_cast<size_t>(i)] < 0) {
			slot = i;
		}
	}
	if (slot < 0) {
		float farthest = -1;
		for (int i = 0; i < 2; ++i) {
			const float d = (m_speakerAt[static_cast<size_t>(i)] - listener).lengthSquared();
			if (d > farthest) {
				farthest = d;
				slot = i;
			}
		}
		if ((speaker.position - listener).lengthSquared() >= farthest) {
			return -1;
		}
	}
	for (int t = 0; t < 2; ++t) {
		if (ISound* sound = m_local[static_cast<size_t>(t * 2 + slot)]) {
			sound->stop();
		}
	}
	m_speaker[static_cast<size_t>(slot)] = speaker.id;
	m_speakerAt[static_cast<size_t>(slot)] = speaker.position;
	if (onPlay) {
		onPlay(local.file, true);
	}
	if (ISound* sound = m_local[static_cast<size_t>((team - 1) * 2 + slot)]) {
		sound->playSampleAtLooped(&speaker.position, sample, 1);
	}
	return sample;
}

int VoiceMessages::playHelp(const std::string& name)
{
	const auto it = m_helps.find(name);
	if (it == m_helps.end() || m_helpFiles[static_cast<size_t>(it->second)].empty()) {
		return -1;
	}
	// The voice-over group's volume (manager slot 0xb8, group 2).
	if (m_manager.volumeForGroup(2) <= 0.05f) {
		return -1;
	}
	if (onPlay) {
		onPlay(m_helpFiles[static_cast<size_t>(it->second)], false);
	}
	if (m_help) {
		m_help->stop();
		m_help->playSampleLooped(it->second, 1);
	}
	return it->second;
}

bool VoiceMessages::helpPlaying() const { return m_help && m_help->isPlaying(); }

VoiceMessages::Result VoiceMessages::heard(const Heard& in)
{
	Result result;
	const int id = message(in.message);
	if (id < 0) {
		return result;
	}
	std::string voiceName = "commander", filterName = "commander";
	if (in.hasSender) {
		filterName.clear();
		if (!in.commander) {
			voiceName = in.squadLeader ? "squadleader" : "grunt";
		}
		else {
			filterName = "commander";
		}
		if (in.senderIsListener) {
			filterName.clear();
		}
	}
	const int voiceId = voice(voiceName);
	if (voiceId < 0) {
		return result;
	}
	const int filterId = filterName.empty() ? -1 : filter(filterName);
	if (in.hasSender && !in.senderIsListener && in.onFoot) {
		const int sample = playLocal(in.listenerTeam, id, voiceId, in.listener, in.sender);
		if (sample >= 0) {
			result = {Played::Local, sample, text(sample)};
			return result;
		}
	}
	if (in.hasSender && in.sender.team != in.listenerTeam) {
		return result; // the enemy's radio isn't heard
	}
	const int team = in.hasSender ? in.sender.team : in.listenerTeam;
	const int sample = playRadio(team, id, voiceId, filterId);
	if (sample >= 0) {
		result = {Played::Radio, sample, text(sample)};
	}
	return result;
}

} // namespace dice::hfe::sound
