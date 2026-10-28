// Radio voice-over: the game logic's message table (BF2142.exe's
// voice-message manager, 0x26b8 bytes, built by 0x004d99c0 and held at
// game logic +0x170) as Sound/VoiceMessages.con fills it through
// "GameLogic.messages.*", and its playing.
//
// The manager is made when the game logic sets up (0x004cdab0): the team
// prefixes are "sound/vo/<team language>/" (GameLogic.setTeamLanguage;
// with a GSLanguage other than English the team speaking it, or else
// the first English one, takes the player's language) and the help prefix
// "sound/vo/<GSLanguage>/" (English when its help file is missing). It then
// runs Sound/VoiceMessages.con and builds its sounds (0x004d8f00):
//   - per team a 2D radio sound of all the radio samples (volume group 2,
//     reverb 0, occlusion factor 1), its volume envelope read at the VO
//     volume (setVolumes' first, +0x54);
//   - per team, except on a dedicated server, two 3D sounds of the local
//     samples (min distance 3, reverb 1, stop type 0), the speakers' own
//     voices near the listener;
//   - a 2D sound of the radio filters (the static under a radio message),
//     at the filter volume (+0x58);
//   - a 2D sound of the help messages (group 2, reverb and occlusion 0).
// Every message's samples for a voice sit together in those lists: entry
// (message x 8 | voice) of the table at +0xec is its first sample and
// count, and a message plays one of them at random (0x004d78d0).
#pragma once

#include "Sound/SoundEngine/ISoundEngine.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::sound {

class SoundManager;

class VoiceMessages {
public:
	static constexpr int kMaxMessages = 300; // 0x004d8af0
	static constexpr int kMaxVoices = 8;     // 0x004d8b90
	static constexpr float kLocalDistance = 15; // +0x26b4: how near a speaker is heard in person

	explicit VoiceMessages(SoundManager& manager);
	~VoiceMessages();
	VoiceMessages(const VoiceMessages&) = delete;
	VoiceMessages& operator=(const VoiceMessages&) = delete;

	// 0x004cdab0's prefixes (clears the tables): the teams' languages and
	// the player's (GSLanguage).
	void reset(const std::string& team1Language, const std::string& team2Language,
		const std::string& userLanguage = "English");
	const std::string& teamPrefix(int team) const { return m_teamPrefix[team == 2 ? 1 : 0]; }

	// The script's methods.
	int addMessage(const std::string& name);                              // 0x004d8af0
	int addRadioVoice(const std::string& voice, const std::string& radioSample, const std::string& text,
		const std::string& localSample = "", int flags = 0);              // 0x004d8b90
	int addRadioFilter(const std::string& name, const std::string& file); // 0x004d8de0
	int addHelpMessage(const std::string& name, const std::string& file); // 0x004d8e40
	void addEmoteTrigger(const std::string& emote);                       // 0x004d83e0
	void setVolumes(float voiceOver, float radioFilter);                  // 0x004d7860
	// The sounds (0x004d8f00); `dedicated` leaves out the local voices.
	void build(bool dedicated = false);

	int message(const std::string& name) const; // -1 for none
	int voice(const std::string& name) const;
	int filter(const std::string& name) const;
	size_t messageCount() const { return m_messages.size(); }
	// The emote a message's speaker plays ("" for none, 0x004d7fe0).
	std::string emote(int message) const;
	// A sample's on-screen text id (0x004d8320), "" for none.
	std::string text(int sample) const;
	// The sample a message plays for a voice: its first one plus rand() %
	// its count (0x004d78d0), -1 when it has none.
	int pickSample(int message, int voice) const;
	// The samples of a message and voice: first and count (count 0: none).
	std::pair<int, int> samples(int message, int voice) const;
	// Team (1, 2) radio sample `index`'s file and a local one's.
	std::string radioFile(int team, int index) const;
	std::string localFile(int team, int index) const;
	float voiceOverVolume() const { return m_voiceOverVolume; }
	float filterVolume() const { return m_filterVolume; }

	// 0x004d7950: a message over the radio of a team's voices (team 1 or
	// 2), with the filter's static (-1 none) looped to cover the sample.
	// The sample played, -1 for none.
	int playRadio(int team, int message, int voice, int filter = -1);
	// 0x004d8110: a speaker's own voice near the listener (3D, at the
	// speaker): only with a local sample, of the listener's team unless
	// flag 1 (both teams), within 15 m unless flag 2. It takes one of the
	// team's two voices: a silent one, else the farther speaker's if this
	// one is nearer. The sample played, -1 for none.
	struct Speaker {
		int id = -1;
		int team = 0;
		Vec3 position;
	};
	int playLocal(int listenerTeam, int message, int voice, const Vec3& listener, const Speaker& speaker);
	// 0x004d8280: a help message's voice, unless the voice-over volume is
	// off (0.05 or less); the sample, -1 for none.
	int playHelp(const std::string& name);
	// 0x004d78a0: the help voice is playing (the manager mutes the world's
	// effects for it, reason 3, 0x004cdfb0).
	bool helpPlaying() const;

	// What the client does with a radio message it hears (0x0042e0c0):
	// the voice is the sender's rank, "commander" for the commander (and
	// his filter "commander"), "squadleader" for a squad leader, "grunt"
	// otherwise; a message with no sender is the commander's, over the
	// commander filter. A sender on foot near enough (and not the listener)
	// speaks it in person (playLocal); otherwise, from the listener's own
	// team only, it comes over the radio of the sender's team's voices.
	struct Heard {
		std::string message;
		bool hasSender = false;
		bool senderIsListener = false;
		bool commander = false, squadLeader = false;
		bool onFoot = true;
		Speaker sender;
		int listenerTeam = 0;
		Vec3 listener;
	};
	enum class Played { None, Local, Radio };
	struct Result {
		Played played = Played::None;
		int sample = -1;
		std::string text; // the sample's text id
	};
	Result heard(const Heard& heard);

	// Each played sample (with or without an engine): its file.
	std::function<void(const std::string& file, bool local)> onPlay;

private:
	struct Entry {
		short first = -1, count = 0;
	};
	struct Local {
		std::string file;
		int flags = 0;
	};
	class VolumeEnvelope;
	ISound* makeSound(const std::vector<std::string>& files, bool is3d, float reverb, float occlusion, int group,
		const float* volumeInput, int copies, std::vector<ISound*>& out);
	void releaseSounds();

	SoundManager& m_manager;
	std::array<std::string, 2> m_teamPrefix;
	std::string m_helpPrefix;
	std::map<std::string, int> m_messages;
	std::map<std::string, int> m_voices;
	std::map<std::string, int> m_filters;
	std::map<std::string, int> m_helps;
	std::map<int, std::string> m_emotes;
	std::array<Entry, kMaxMessages * kMaxVoices> m_table {};
	std::vector<std::string> m_texts;
	std::array<std::vector<std::string>, 2> m_radioFiles;
	std::array<std::vector<Local>, 2> m_localFiles;
	std::vector<std::string> m_filterFiles;
	std::vector<std::string> m_helpFiles;
	float m_voiceOverVolume = 1; // +0x54
	float m_filterVolume = 1;    // +0x58
	std::unique_ptr<VolumeEnvelope> m_envelope; // +0x5c
	ISound* m_filterSound = nullptr;            // +0x266c
	std::array<ISound*, 2> m_radio {};          // +0x2670
	ISound* m_help = nullptr;                   // +0x2678
	std::array<ISound*, 4> m_local {};          // +0x269c: team x 2 + slot
	std::array<int, 2> m_speaker {-1, -1};      // +0x26ac: who speaks in each slot (per team)
	std::array<Vec3, 2> m_speakerAt {};
	std::array<int, 2> m_speakerTeam {};
};

} // namespace dice::hfe::sound
