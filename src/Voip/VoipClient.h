// The voice client: BF2142.exe's Bf2VoipClient (0x0043ba40-0x0043cd40) on
// GameSpy Voice 2's client (0x00711070-0x00711e90) and its capture and
// playback (0x00720910-0x00721390, 0x00737710).
//
// - A UDP socket bound to sv.voipBFClientPort talks to the voice server.
//   A keep-alive every 3 s registers the client's address there.
// - Capture: 160-sample frames at 8 kHz; the capture volume, BF2142's
//   voice filter (a 2500 Hz band-pass with gain 2, 0x0043bb60), voice
//   activity detection (a frame counts when a sample exceeds the threshold;
//   20 frames of hang-over, 0x007210f0) or push-to-talk (the threshold
//   drops to 0 while the key is held, 0x0043bdd0), Speex, a 16-bit frame
//   sequence. Frames go out with the channel choice and the session id.
// - Playback: a jitter buffer per speaker, Speex decoding, mixing at the
//   playback volume. Muted speakers are dropped on arrival (0x0043c0b0).
// - The mic test (0x007115e0) loops capture through the codec to playback
//   without the network.
//
// Audio comes and goes through a VoiceDevice (SdlVoiceDevice in the game,
// a memory device in the tests). Everything happens in update().
#pragma once

#include "Net/Socket.h"
#include "VoipCodec.h"
#include "VoipProtocol.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dice::hfe::voip {

// 8 kHz mono 16-bit audio in and out.
class VoiceDevice {
public:
	virtual ~VoiceDevice() = default;
	// Captured samples since the last call, at most `max`.
	virtual size_t readCapture(int16_t* out, size_t max) = 0;
	// Plays samples after the ones queued.
	virtual void writePlayback(const int16_t* samples, size_t count) = 0;
	// Samples queued and not played yet.
	virtual size_t playbackQueued() const = 0;
};

// AudioSettings' voip* settings (the options page).
struct VoiceSettings {
	bool enabled = true;           // voipEnabled
	float playbackVolume = 1.0f;   // voipPlaybackVolume, 0-1
	float captureVolume = 1.0f;    // voipCaptureVolume, 0-1
	float captureThreshold = 0.1f; // voipCaptureThreshold, 0-1 of full scale (GameSpy Voice's default 0.1)
	bool boost = false;            // voipBoostEnabled (provisional: +12 dB; the original sets the OS mixer's mic boost)
	bool pushToTalk = true;        // voipUsePushToTalk
};

class VoipClient {
public:
	VoipClient();
	~VoipClient();
	VoipClient(const VoipClient&) = delete;
	VoipClient& operator=(const VoipClient&) = delete;

	// Opens the socket (localPort 0: any) for a voice server. voipQuality
	// is the server's (1-5).
	bool start(const net::UdpServerSocket::Address& server, uint16_t localPort, int voipQuality, bool loopbackOnly = false);
	bool start(const std::string& host, uint16_t serverPort, uint16_t localPort, int voipQuality, bool loopbackOnly = false);
	void stop();
	bool running() const { return m_running; }
	uint16_t localPort() const { return m_socket.port(); }

	// The session id from the game server's VoipSession event.
	void setSession(uint16_t session);
	uint16_t session() const { return m_session; }

	// Audio (not owned; nullptr for none).
	void setDevice(VoiceDevice* device) { m_device = device; }
	void setSettings(const VoiceSettings& settings);
	const VoiceSettings& settings() const { return m_settings; }
	// The codec without a server (the mic test before joining).
	void setQuality(int voipQuality);

	// Who hears us (selectChannels()).
	void setChannels(const ChannelSelection& channels) { m_channels = channels; }
	const ChannelSelection& channels() const { return m_channels; }
	// Whether we may talk: in a squad or commander (0x0043c8d0).
	void setTransmitAllowed(bool allowed) { m_transmitAllowed = allowed; }
	// c_GIVoipPushToTalk held.
	void setPushToTalk(bool held) { m_pushToTalkHeld = held; }
	// Whether received voice is played (0x59).
	void setReceiveEnabled(bool enabled) { m_receiveEnabled = enabled; }

	// Muting a player drops their voice (VoipPlayerMute events).
	void setMuted(int player, bool muted);
	bool muted(int player) const { return m_muted.count(player) != 0; }

	// The mic test: what is captured is played back through the codec.
	void setLoopbackTest(bool on);
	bool loopbackTest() const { return m_loopback; }

	// Receives, captures, sends and plays what is due. `now` in seconds.
	void update(double now);

	// --- State

	// Voice went out in the last update (or would have, in the mic test).
	bool transmitting() const { return m_transmitting; }
	// The last captured frame's peak, 0-1.
	float captureLevel() const { return m_captureLevel; }
	// Talked for 10 s: held back for 5 s (0x0043c140, provisional timing of the release).
	bool blocked() const { return m_blockedUntil > m_now; }
	// Players heard in the last 500 ms (0x80: 500 ms).
	bool talking(int player) const;
	std::vector<int> talkers() const;

	struct Counters {
		size_t packetsSent = 0, framesSent = 0, keepAlives = 0;
		size_t packetsReceived = 0, framesReceived = 0, framesDecoded = 0, framesConcealed = 0, framesLate = 0, mutedDropped = 0;
	};
	const Counters& counters() const { return m_counters; }

	// The talker id the mic test plays as.
	static constexpr int kLoopbackTalker = 0xffff;

private:
	struct Talker;

	void receive();
	void capture();
	void processFrame(int16_t* frame);
	void flush();
	void addVoice(int speaker, uint16_t sequence, const std::vector<uint8_t>& frames);
	void play();
	void mixFrame(int16_t* out);

	net::UdpServerSocket m_socket;
	net::UdpServerSocket::Address m_server;
	bool m_running = false;
	uint16_t m_session = 0;
	double m_now = 0;
	double m_nextKeepAlive = 0;

	VoiceDevice* m_device = nullptr;
	VoiceSettings m_settings;
	std::unique_ptr<SpeexEncoder> m_encoder;
	ChannelSelection m_channels;
	bool m_transmitAllowed = true;
	bool m_pushToTalkHeld = false;
	bool m_receiveEnabled = true;
	bool m_loopback = false;
	std::set<int> m_muted;

	// Capture.
	std::vector<int16_t> m_captured;
	uint16_t m_sequence = 0;
	uint16_t m_lastLoud = 0;
	bool m_heardLoud = false;
	std::vector<uint8_t> m_pending; // encoded frames waiting to go
	uint16_t m_pendingSequence = 0;
	size_t m_pendingFrames = 0;
	bool m_transmitting = false;
	bool m_sentThisUpdate = false;
	float m_captureLevel = 0;
	double m_talkStart = -1, m_lastSent = -1, m_blockedUntil = -1;

	// Playback.
	std::map<int, std::unique_ptr<Talker>> m_talkers;
	std::map<int, double> m_lastHeard;
	double m_playStart = -1;
	int64_t m_framesPlayed = 0;

	Counters m_counters;
};

} // namespace dice::hfe::voip
