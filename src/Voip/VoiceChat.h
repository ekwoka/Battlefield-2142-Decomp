// Voice chat in the game: the joined server's voice settings (its
// settings transfer), the VoipSession / VoipPlayerMute / VoipOnOff events,
// the player's squad and keys, and the options page's settings, put
// together around a VoipClient.
//
// How the original starts its client: the server's settings (data block
// 0) carry voipEnabled, the voice server's address and ports and the
// quality (0x004ab030 keeps them in clientGameLogic, 0x004cbdb0); the
// server then sends the player's VoipSession event with a session id
// (0x00512320 hands it to the local player) and clientGameLogic starts the
// client (0x004cbe40 -> Bf2VoipClient 0x0043c630), the address 1 standing
// for the game server's own.
#pragma once

#include "VoipClient.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dice::hfe::net {
struct GameEvent;
}

namespace dice::hfe::voip {

class VoiceChat {
public:
	explicit VoiceChat(std::unique_ptr<VoiceDevice> device = nullptr);
	~VoiceChat();
	VoiceChat(const VoiceChat&) = delete;
	VoiceChat& operator=(const VoiceChat&) = delete;

	// The microphone and speakers (openVoiceDevice()); opened by the menu's
	// voipInitialize or when the client starts if not given.
	void setDevice(std::unique_ptr<VoiceDevice> device);
	bool hasDevice() const;
	// Opens the default device if there is none yet (false without audio).
	bool openDevice();

	// The client (not locked: for tests and single-threaded use).
	VoipClient& client() { return m_client; }
	const VoipClient& client() const { return m_client; }

	// Updates every 10 ms on a thread of its own (as BF2142.exe runs its
	// voice server, 0x00645480), so voice keeps going through menus and
	// loading screens. Every member locks; update() isn't needed then.
	void runInBackground(bool on);

	// --- The joined server

	// Its voice settings and the game server's host (for the address 1).
	void setServer(const ServerVoipSettings& settings, const std::string& gameServerHost);
	// Reads them from the settings transfer (data block 0); false if it
	// doesn't parse. The same block again does nothing.
	bool setServerSettingsBlock(const std::string& block, const std::string& gameServerHost);
	// VoipSession (0x36), VoipPlayerMute (0x26) and VoipOnOff (0x23); false
	// for other events.
	bool handleEvent(const net::GameEvent& event, int localPlayer);
	// Whether the client runs (settings, a session and voice enabled on both sides).
	bool active() const;
	// Leaving the server.
	void leave();

	// --- Each frame

	// The local player's place: who may talk and to whom.
	void setRole(bool commander, bool squadLeader, int squad, const std::vector<int>& selectedSquads = {});
	// c_GIVoipPushToTalk and c_GIVoipUseLeaderChannel held.
	void setKeys(bool pushToTalk, bool leaderChannel);
	void update(double now);
	// Players who switched their voice chat on or off (VoipOnOff).
	bool playerVoipOn(int player) const;

	// --- The options page (AudioSettings, dice.bf2.Sound)

	void setSettings(const VoiceSettings& settings);
	VoiceSettings settings() const;
	void startTest();
	void stopTest();
	bool testing() const;
	bool sendingVoice() const;
	float sendingVoiceVolume() const;

private:
	void tryStart();
	void applyChannels();
	double clock() const;

	mutable std::recursive_mutex m_mutex;
	std::thread m_thread;
	std::atomic<bool> m_stop {false};
	std::string m_lastBlock;

	std::unique_ptr<VoiceDevice> m_device;
	VoipClient m_client;
	VoiceSettings m_settings;

	bool m_haveServer = false;
	ServerVoipSettings m_server;
	std::string m_gameServerHost;
	uint16_t m_session = 0;

	bool m_commander = false, m_squadLeader = false;
	int m_squad = 0;
	std::vector<int> m_selectedSquads;
	bool m_leaderKey = false;
	std::map<int, bool> m_voipOn;
};

// The default microphone and speakers through SDL3 (8 kHz mono), or
// nullptr when the build has no SDL3 or there is no audio device.
std::unique_ptr<VoiceDevice> openVoiceDevice(std::string* error = nullptr);
// Whether this build can open one.
bool voiceDeviceAvailable();

} // namespace dice::hfe::voip
