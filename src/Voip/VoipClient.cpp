#include "VoipClient.h"

#include "VoipServerConnection.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <list>

namespace dice::hfe::voip {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kKeepAliveInterval = 3.0;    // GameSpy Voice's 0x5c
constexpr double kTalkingTimeout = 0.5;       // 0x80: 500 ms
constexpr int kHangoverFrames = 20;           // 0x007210f0: 0x14 frames after the last loud one
constexpr double kMaxTalkSeconds = 10.0;      // Bf2VoipClient +0xa8 (set by 0x0043c420)
constexpr double kTalkEndSeconds = 0.5;       // player +0x398: the talk ends after this long without voice
constexpr size_t kMaxPayload = 0x3fe;         // 0x0043be90's frame buffer
// voipBoostEnabled: the original never scales samples. 0x0070ff50 finds the
// microphone line of the OS mixer's two destinations (component type
// 0x1003 or a name containing "Mic", 0x0070fa90) and switches its LOUDNESS
// control (0x20010004), else its ONOFF one (0x20010001), with
// mixerSetControlDetails. How much that adds is the sound card driver's
// business (AC'97's "Mic +20 dB", register 0Eh bit 6, being the usual
// one); the rewrite adds those 20 dB itself.
constexpr float kBoostGain = 10.0f;

// GameSpy Voice 2's source list (0x00737470 sets it up, 0x00737710 adds a
// packet, 0x00737630 its frames, 0x00737590 a source, 0x007378c0 mixes).
constexpr uint16_t kSyncDelayFrames = 10;     // DAT_00b2d084: 200 ms (3200 bytes in whole 320-byte frames)
constexpr int kMaxClockAhead = 10;            // the offset more than 10 above the average: one frame dropped
constexpr uint16_t kClockBehind = 0xfffb;     // more than 5 below: one frame later
// The rewrite's device pacing, standing in for DirectSound's write cursor
// (0x00720000.c:586-605), which drives the original's mixing.
constexpr int kMaxFramesPerUpdate = 10;       // playback catching up
constexpr int kMaxQueuedFrames = 15;          // 300 ms in the device: skip a frame

// 16-bit clock comparisons (0x007218c0, 0x007218d0): a after b, a at or after b.
bool after(uint16_t a, uint16_t b)
{
	return static_cast<uint16_t>(b - a) > 0x7fff;
}
bool atOrAfter(uint16_t a, uint16_t b)
{
	return static_cast<uint16_t>(b - a - 1) > 0x7fff;
}

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

// A source (0x24 bytes): +4 the speaker, +0xc the clock the last frame
// ends at, +0xe the offset from the speaker's sequence to the play clock,
// +0x10 its running average, +0x14 the decoder, +0x1c the frames sorted by
// play clock.
struct VoipClient::Talker {
	int speaker = 0;
	uint16_t finished = 0;
	uint16_t clockOffset = 0;
	float average = 0;
	SpeexDecoder decoder;
	struct Frame {
		uint16_t stamp;
		std::vector<uint8_t> bytes;
	};
	std::list<Frame> frames;
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
	for (auto& talker : m_talkers) {
		talker.reset();
	}
	m_lastHeard.clear();
	m_pending.clear();
	m_pendingFrames = 0;
	m_transmitting = false;
}

void VoipClient::setSession(uint16_t session)
{
	m_session = session;
	m_haveSession = true;
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
	for (auto& talker : m_talkers) {
		if (talker && talker->speaker == kLoopbackTalker) {
			talker.reset();
		}
	}
	m_captured.clear();
	m_pending.clear();
	m_pendingFrames = 0;
}

bool VoipClient::talking(int player) const
{
	const auto it = m_lastHeard.find(player);
	return it != m_lastHeard.end() && m_now - it->second < kTalkingTimeout;
}

size_t VoipClient::sources() const
{
	return static_cast<size_t>(std::count_if(m_talkers.begin(), m_talkers.end(), [](const auto& t) { return t != nullptr; }));
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
		if (m_haveSession && now >= m_nextKeepAlive) {
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
	// 0x0043c8d0: a block is lifted once the talk has ended, then the talk
	// ends after kTalkEndSeconds without voice (0x0043c230 -> 0x007113c0).
	if (m_blocked && !m_talking) {
		m_blocked = false;
	}
	if (m_talking && now - m_lastVoice > kTalkEndSeconds) {
		m_talking = false;
		m_talkStart = -1;
	}
	m_transmitting = m_sentThisUpdate || (m_lastSent >= 0 && now - m_lastSent < kTalkingTimeout);
	play();
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
		deliver(voice);
	}
}

void VoipClient::deliver(const ForwardedVoice& voice)
{
	++m_counters.packetsReceived;
	if (m_muted.count(voice.speaker)) {
		++m_counters.mutedDropped; // 0x0043c0b0: a muted speaker's voice is dropped
		return;
	}
	if (!m_receiveEnabled || !m_settings.enabled) {
		return;
	}
	addVoice(voice.speaker, voice.sequence, voice.frames);
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
	if (send && !m_loopback && m_running && m_haveSession) {
		// 0x0043c140, for every captured packet (sent or not): talking for
		// more than 10 s blocks (0x0043c2f0 -> 0x0043c290(1): +0xa3, player
		// +0x388). Out of a game GameSpy Voice's 0x00711360 only tracks it.
		m_lastVoice = m_now;
		if (!m_talking) {
			m_talking = true;
			m_talkStart = m_now;
		}
		if (m_now - m_talkStart > kMaxTalkSeconds) {
			m_blocked = true;
		}
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
	// 0x0043be90 sends only while neither blocked (+0xa3) nor held back (+0xc0).
	if (!m_running || !m_haveSession || !m_transmitAllowed || m_blocked) {
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

VoipClient::Talker* VoipClient::newSource(int speaker)
{
	// 0x00737590: the first free source, else the first whose frames have
	// all played; none: the packet is dropped.
	std::unique_ptr<Talker>* slot = nullptr;
	for (auto& talker : m_talkers) {
		if (!talker) {
			slot = &talker;
			break;
		}
		if (!slot && talker->frames.empty()) {
			slot = &talker;
		}
	}
	if (!slot) {
		return nullptr;
	}
	*slot = std::make_unique<Talker>();
	(*slot)->speaker = speaker;
	return slot->get();
}

void VoipClient::storeFrames(Talker& talker, uint16_t stamp, const uint8_t* data, size_t count, size_t frameBytes)
{
	// 0x00737630: frames go in at the play clock stamp + offset, in order;
	// a frame already there drops the packet.
	auto at = static_cast<uint16_t>(stamp + talker.clockOffset);
	const auto end = static_cast<uint16_t>(at + count);
	if (after(end, talker.finished)) {
		talker.finished = end;
	}
	auto it = talker.frames.begin();
	for (; it != talker.frames.end(); ++it) {
		if (it->stamp == at) {
			++m_counters.framesLate;
			return;
		}
		if (after(it->stamp, at)) {
			break;
		}
	}
	for (size_t i = 0; i < count; ++i, ++at, data += frameBytes) {
		talker.frames.insert(it, Talker::Frame {at, std::vector<uint8_t>(data, data + frameBytes)});
	}
}

void VoipClient::addVoice(int speaker, uint16_t sequence, const std::vector<uint8_t>& frames)
{
	const size_t frameBytes = m_encoder->frameBytes();
	if (frameBytes == 0 || frames.size() < frameBytes) {
		return;
	}
	size_t count = frames.size() / frameBytes;
	m_lastHeard[speaker] = m_now;
	m_counters.framesReceived += count;
	const uint8_t* data = frames.data();
	// 0x00737710: where the packet's first frame would play were it to wait
	// the sync delay from now.
	const auto offset = static_cast<uint16_t>(kSyncDelayFrames - sequence + m_playClock);
	Talker* talker = nullptr;
	for (auto& t : m_talkers) {
		if (t && t->speaker == speaker) {
			talker = t.get();
			break;
		}
	}
	if (!talker) {
		talker = newSource(speaker);
		if (!talker) {
			m_counters.framesLate += count;
			return;
		}
		talker->clockOffset = offset;
		talker->finished = static_cast<uint16_t>(offset + sequence);
		talker->average = static_cast<float>(offset);
		storeFrames(*talker, sequence, data, count, frameBytes);
		return;
	}
	// The average follows the offset (1/32 per packet), unwrapped against it
	// (by 0xffff, as the original does).
	int value = offset;
	if (talker->average > static_cast<float>(value)) {
		if (after(offset, static_cast<uint16_t>(static_cast<int>(talker->average)))) {
			value += 0xffff;
		}
	} else if (talker->average < static_cast<float>(value)) {
		if (after(static_cast<uint16_t>(static_cast<int>(talker->average)), offset)) {
			value -= 0xffff;
		}
	}
	float average = static_cast<float>(value) * 0.03125f + talker->average * 0.96875f;
	if (average < 0.0f) {
		average += 65535.0f;
	} else if (average >= 65535.0f) {
		average -= 65535.0f;
	}
	talker->average = average;
	const auto drift = static_cast<uint16_t>(talker->clockOffset - static_cast<int>(average));
	if (after(drift, kMaxClockAhead)) {
		// Ahead of the average: one frame less (the first; the rest keep
		// the packet's sequence, so they move a frame earlier).
		--talker->clockOffset;
		if (count == 1) {
			++m_counters.framesLate;
			return;
		}
		data += frameBytes;
		--count;
		++m_counters.framesLate;
	} else if (after(kClockBehind, drift)) {
		++talker->clockOffset; // behind it: a frame later
	}
	if (after(m_playClock, static_cast<uint16_t>(talker->clockOffset + sequence))) {
		m_counters.framesLate += count; // too late
		return;
	}
	storeFrames(*talker, sequence, data, count, frameBytes);
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
	// 0x007378c0 for one frame: each source plays its frames stamped with
	// this clock, drops the older ones, and is freed once drained and past
	// its last frame. Nothing is concealed: a missing frame is silence.
	int mix[kFrameSamples] = {};
	int16_t decoded[kFrameSamples];
	const uint16_t start = m_playClock;
	const auto end = static_cast<uint16_t>(start + 1);
	for (auto& slot : m_talkers) {
		if (!slot) {
			continue;
		}
		Talker& talker = *slot;
		while (!talker.frames.empty()) {
			Talker::Frame& frame = talker.frames.front();
			if (atOrAfter(frame.stamp, end)) {
				break;
			}
			if (atOrAfter(frame.stamp, start)) {
				if (talker.decoder.decode(frame.bytes.data(), frame.bytes.size(), decoded)) {
					++m_counters.framesDecoded;
					for (int i = 0; i < kFrameSamples; ++i) {
						mix[i] += decoded[i];
					}
				}
			} else {
				++m_counters.framesLate;
			}
			talker.frames.pop_front();
		}
		if (talker.frames.empty() && atOrAfter(end, talker.finished)) {
			slot.reset();
		}
	}
	++m_playClock;
	const float volume = m_settings.playbackVolume;
	for (int i = 0; i < kFrameSamples; ++i) {
		out[i] = clampSample(static_cast<int>(std::lround(mix[i] * volume)));
	}
}

} // namespace dice::hfe::voip
