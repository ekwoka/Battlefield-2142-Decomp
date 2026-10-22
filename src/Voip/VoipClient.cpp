#include "VoipClient.h"

#include "VoipServerConnection.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dice::hfe::voip {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kKeepAliveInterval = 3.0;    // GameSpy Voice's 0x5c
constexpr double kTalkingTimeout = 0.5;       // 0x80: 500 ms
constexpr int kHangoverFrames = 20;           // 0x007210f0: 0x14 frames after the last loud one
constexpr double kMaxTalkSeconds = 10.0;      // Bf2VoipClient 0xa8
constexpr double kBlockSeconds = 5.0;         // Bf2VoipClient 0xb8
constexpr size_t kMaxPayload = 0x3fe;         // 0x0043be90's frame buffer
constexpr float kBoostGain = 4.0f;            // provisional (+12 dB)

// Jitter buffer (provisional: GameSpy Voice's source list, 0x00737710,
// is not rewritten; these are ours).
constexpr int kPrebufferFrames = 3;           // 60 ms before a speaker starts
constexpr int kMaxConcealedFrames = 3;        // then the speaker stops until more arrives
constexpr int kMaxAheadFrames = 50;           // a jump of more than 1 s restarts the speaker
constexpr double kTalkerExpiry = 2.0;
constexpr int kMaxFramesPerUpdate = 10;       // playback catching up
constexpr int kMaxQueuedFrames = 15;          // 300 ms in the device: skip a frame

// BF2142's capture filter (0x00712250 set up by 0x0043c420 with gain 2,
// 2500 Hz, 8000 Hz, 1.75, on; run per frame by 0x0043bb60, which clears
// its history first): an RBJ band-pass, the output doubled and clamped.
struct VoiceFilter {
	double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, gain = 2.0;
	VoiceFilter()
	{
		const double w = 2500.0 / 8000.0 * 2.0 * kPi;
		const double bandwidth = 1.75;
		const double alpha = std::sin(w) * std::sinh(std::log(2.0) / 2.0 * bandwidth * w / std::sin(w));
		const double inv = 1.0 / (1.0 + alpha);
		b0 = alpha * inv;
		b1 = 0.0;
		b2 = -alpha * inv;
		a1 = -2.0 * std::cos(w) * inv;
		a2 = (1.0 - alpha) * inv;
	}
	void run(int16_t* samples, int count) const
	{
		double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		for (int i = 0; i < count; ++i) {
			const double x = samples[i];
			const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
			x2 = x1;
			x1 = x;
			y2 = y1;
			y1 = y;
			samples[i] = static_cast<int16_t>(std::clamp(y * gain, -32767.0, 32767.0)); // _ftol truncates
		}
	}
};

const VoiceFilter& voiceFilter()
{
	static const VoiceFilter filter;
	return filter;
}

int16_t clampSample(int value)
{
	return static_cast<int16_t>(std::clamp(value, -32768, 32767));
}

} // namespace

struct VoipClient::Talker {
	SpeexDecoder decoder;
	std::map<int64_t, std::vector<uint8_t>> frames; // by unwrapped sequence
	int64_t next = 0;      // the sequence played next
	bool playing = false;
	int concealed = 0;
	double lastHeard = 0;
};

VoipClient::VoipClient()
{
	setQuality(3);
}

VoipClient::~VoipClient()
{
	stop();
}

void VoipClient::setQuality(int voipQuality)
{
	m_encoder = std::make_unique<SpeexEncoder>(speexQualityFor(voipQuality));
	m_pending.clear();
	m_pendingFrames = 0;
}

bool VoipClient::start(const net::UdpServerSocket::Address& server, uint16_t localPort, int voipQuality, bool loopbackOnly)
{
	stop();
	if (!m_socket.bind(localPort, loopbackOnly)) {
		return false;
	}
	m_server = server;
	setQuality(voipQuality);
	m_running = true;
	m_nextKeepAlive = 0;
	return true;
}

bool VoipClient::start(const std::string& host, uint16_t serverPort, uint16_t localPort, int voipQuality, bool loopbackOnly)
{
	net::UdpServerSocket::Address server;
	return resolveAddress(host, serverPort, server) && start(server, localPort, voipQuality, loopbackOnly);
}

void VoipClient::stop()
{
	m_socket.close();
	m_running = false;
	m_talkers.clear();
	m_lastHeard.clear();
	m_pending.clear();
	m_pendingFrames = 0;
	m_transmitting = false;
}

void VoipClient::setSession(uint16_t session)
{
	m_session = session;
	m_nextKeepAlive = 0; // register at once
}

void VoipClient::setSettings(const VoiceSettings& settings)
{
	m_settings = settings;
	m_settings.playbackVolume = std::clamp(m_settings.playbackVolume, 0.0f, 1.0f);
	m_settings.captureVolume = std::clamp(m_settings.captureVolume, 0.0f, 1.0f);
	m_settings.captureThreshold = std::clamp(m_settings.captureThreshold, 0.0f, 1.0f);
}

void VoipClient::setMuted(int player, bool muted)
{
	if (muted) {
		m_muted.insert(player);
	} else {
		m_muted.erase(player);
	}
}

void VoipClient::setLoopbackTest(bool on)
{
	m_loopback = on;
	m_talkers.erase(kLoopbackTalker);
	m_captured.clear();
	m_pending.clear();
	m_pendingFrames = 0;
}

bool VoipClient::talking(int player) const
{
	const auto it = m_lastHeard.find(player);
	return it != m_lastHeard.end() && m_now - it->second < kTalkingTimeout;
}

std::vector<int> VoipClient::talkers() const
{
	std::vector<int> out;
	for (const auto& [player, at] : m_lastHeard) {
		if (m_now - at < kTalkingTimeout) {
			out.push_back(player);
		}
	}
	return out;
}

void VoipClient::update(double now)
{
	m_now = now;
	m_sentThisUpdate = false;
	if (m_running) {
		receive();
		if (m_session != 0 && now >= m_nextKeepAlive) {
			// GameSpy Voice 0x00711400 / 0x00711760.
			if (m_socket.sendTo(m_server, buildKeepAlivePacket(m_session))) {
				++m_counters.keepAlives;
			}
			m_nextKeepAlive = now + kKeepAliveInterval;
		}
	}
	if (m_device && m_settings.enabled && (m_running || m_loopback)) {
		capture();
	} else if (m_device) {
		// Not capturing: keep the device's buffer from growing.
		int16_t drop[kFrameSamples * 4];
		while (m_device->readCapture(drop, std::size(drop)) == std::size(drop)) {
		}
	}
	m_transmitting = m_sentThisUpdate || (m_lastSent >= 0 && now - m_lastSent < kTalkingTimeout);
	play();
	for (auto it = m_talkers.begin(); it != m_talkers.end();) {
		it = now - it->second->lastHeard > kTalkerExpiry ? m_talkers.erase(it) : std::next(it);
	}
}

void VoipClient::receive()
{
	std::vector<uint8_t> packet;
	net::UdpServerSocket::Address from;
	while (m_socket.receiveFrom(packet, from, 0) > 0) {
		ForwardedVoice voice;
		if (!(from == m_server) || !parseForwardedVoice(packet, voice)) {
			continue;
		}
		++m_counters.packetsReceived;
		if (m_muted.count(voice.speaker)) {
			++m_counters.mutedDropped; // 0x0043c0b0: a muted speaker's voice is dropped
			continue;
		}
		if (!m_receiveEnabled || !m_settings.enabled) {
			continue;
		}
		addVoice(voice.speaker, voice.sequence, voice.frames);
	}
}

void VoipClient::capture()
{
	int16_t buffer[kFrameSamples * 4];
	for (;;) {
		const size_t n = m_device->readCapture(buffer, std::size(buffer));
		m_captured.insert(m_captured.end(), buffer, buffer + n);
		if (n < std::size(buffer)) {
			break;
		}
	}
	size_t at = 0;
	for (; at + kFrameSamples <= m_captured.size(); at += kFrameSamples) {
		processFrame(m_captured.data() + at);
	}
	m_captured.erase(m_captured.begin(), m_captured.begin() + static_cast<std::ptrdiff_t>(at));
	flush();
}

void VoipClient::processFrame(int16_t* frame)
{
	// Boost, then voice activity on what was captured (0x00737a20), then
	// the capture volume and the filter (0x007210f0, 0x0043bb60), then Speex.
	int peak = 0;
	for (int i = 0; i < kFrameSamples; ++i) {
		if (m_settings.boost) {
			frame[i] = clampSample(static_cast<int>(frame[i] * kBoostGain));
		}
		peak = std::max(peak, std::abs(static_cast<int>(frame[i])));
	}
	m_captureLevel = static_cast<float>(peak) / 32768.0f;
	bool send = false;
	if (m_settings.pushToTalk && !m_loopback) {
		send = m_pushToTalkHeld; // the threshold is 0 while held, 1 when not (0x0043bdd0)
	} else {
		const float threshold = m_settings.captureThreshold;
		const bool loud = threshold <= 0.0f || static_cast<double>(peak) > threshold * 32767.0;
		if (loud) {
			m_lastLoud = m_sequence;
			m_heardLoud = true;
		}
		send = loud || (m_heardLoud && static_cast<uint16_t>(m_sequence - m_lastLoud) < kHangoverFrames);
	}
	if (send) {
		if (m_settings.captureVolume < 1.0f) {
			for (int i = 0; i < kFrameSamples; ++i) {
				frame[i] = static_cast<int16_t>(frame[i] * m_settings.captureVolume);
			}
		}
		voiceFilter().run(frame, kFrameSamples);
		if (m_pendingFrames > 0 && static_cast<uint16_t>(m_pendingSequence + m_pendingFrames) != m_sequence) {
			flush(); // only consecutive frames share a packet
		}
		if (m_pendingFrames == 0) {
			m_pendingSequence = m_sequence;
		}
		m_encoder->encode(frame, m_pending);
		++m_pendingFrames;
		if (m_pending.size() + m_encoder->frameBytes() > kMaxPayload) {
			flush();
		}
	}
	++m_sequence;
}

void VoipClient::flush()
{
	if (m_pendingFrames == 0) {
		return;
	}
	const std::vector<uint8_t> frames = std::move(m_pending);
	const uint16_t sequence = m_pendingSequence;
	const size_t count = m_pendingFrames;
	m_pending.clear();
	m_pendingFrames = 0;
	if (m_loopback) {
		// 0x007115e0: straight to playback.
		addVoice(kLoopbackTalker, sequence, frames);
		m_sentThisUpdate = true;
		m_lastSent = m_now;
		return;
	}
	if (!m_running || m_session == 0 || !m_transmitAllowed) {
		return;
	}
	// 0x0043c140: after 10 s of talking, 5 s of silence.
	if (blocked()) {
		return;
	}
	if (m_lastSent < 0 || m_now - m_lastSent > kTalkingTimeout) {
		m_talkStart = m_now;
	}
	if (m_now - m_talkStart > kMaxTalkSeconds) {
		m_blockedUntil = m_now + kBlockSeconds;
		m_talkStart = -1;
		return;
	}
	const std::vector<uint8_t> packet = buildVoicePacket(buildVoicePayload(frames, sequence), m_channels, m_session);
	if (m_socket.sendTo(m_server, packet)) {
		++m_counters.packetsSent;
		m_counters.framesSent += count;
		m_sentThisUpdate = true;
		m_lastSent = m_now;
	}
}

void VoipClient::addVoice(int speaker, uint16_t sequence, const std::vector<uint8_t>& frames)
{
	const size_t frameBytes = m_encoder->frameBytes();
	if (frameBytes == 0 || frames.size() < frameBytes) {
		return;
	}
	m_lastHeard[speaker] = m_now;
	std::unique_ptr<Talker>& slot = m_talkers[speaker];
	const bool fresh = !slot || m_now - slot->lastHeard > kTalkingTimeout;
	if (!slot) {
		slot = std::make_unique<Talker>();
	}
	Talker& talker = *slot;
	// The sequence relative to the one playing next (16 bits wrap).
	int64_t first = talker.next + static_cast<int16_t>(static_cast<uint16_t>(sequence - static_cast<uint16_t>(talker.next)));
	if (fresh || first > talker.next + kMaxAheadFrames) {
		talker.frames.clear();
		talker.next = first;
		talker.playing = false;
		talker.concealed = 0;
	}
	talker.lastHeard = m_now;
	for (size_t at = 0; at + frameBytes <= frames.size(); at += frameBytes, ++first) {
		++m_counters.framesReceived;
		if (first < talker.next) {
			++m_counters.framesLate;
			continue;
		}
		talker.frames[first].assign(frames.begin() + static_cast<std::ptrdiff_t>(at),
			frames.begin() + static_cast<std::ptrdiff_t>(at + frameBytes));
	}
}

void VoipClient::play()
{
	if (m_playStart < 0) {
		m_playStart = m_now;
		m_framesPlayed = 0;
	}
	int64_t due = static_cast<int64_t>((m_now - m_playStart) * kFramesPerSecond) - m_framesPlayed;
	if (due > kMaxFramesPerUpdate) {
		// Far behind (a stall): skip ahead.
		m_framesPlayed += due - kMaxFramesPerUpdate;
		due = kMaxFramesPerUpdate;
	}
	for (; due > 0; --due) {
		++m_framesPlayed;
		if (m_device && m_device->playbackQueued() > static_cast<size_t>(kMaxQueuedFrames * kFrameSamples)) {
			continue; // the device runs slower than our clock
		}
		int16_t frame[kFrameSamples];
		mixFrame(frame);
		if (m_device) {
			m_device->writePlayback(frame, kFrameSamples);
		}
	}
}

void VoipClient::mixFrame(int16_t* out)
{
	int mix[kFrameSamples] = {};
	int16_t decoded[kFrameSamples];
	for (auto& [speaker, pointer] : m_talkers) {
		Talker& talker = *pointer;
		if (!talker.playing) {
			if (talker.frames.size() < static_cast<size_t>(kPrebufferFrames)) {
				continue;
			}
			talker.playing = true;
			talker.next = talker.frames.begin()->first;
		}
		const auto it = talker.frames.find(talker.next);
		if (it != talker.frames.end()) {
			if (talker.decoder.decode(it->second.data(), it->second.size(), decoded)) {
				++m_counters.framesDecoded;
			}
			talker.frames.erase(it);
			talker.concealed = 0;
		} else if (talker.concealed < kMaxConcealedFrames) {
			talker.decoder.conceal(decoded);
			++talker.concealed;
			++m_counters.framesConcealed;
		} else {
			talker.playing = false; // ran dry: wait for more
			continue;
		}
		// Frames that arrived too late for their turn.
		while (!talker.frames.empty() && talker.frames.begin()->first <= talker.next) {
			talker.frames.erase(talker.frames.begin());
		}
		++talker.next;
		for (int i = 0; i < kFrameSamples; ++i) {
			mix[i] += decoded[i];
		}
	}
	const float volume = m_settings.playbackVolume;
	for (int i = 0; i < kFrameSamples; ++i) {
		out[i] = clampSample(static_cast<int>(std::lround(mix[i] * volume)));
	}
}

} // namespace dice::hfe::voip
