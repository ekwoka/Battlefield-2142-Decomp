// Voice chat (src/Voip): Speex round trips, the packet formats, the
// channel plans, and whole sessions on the loopback interface: a game
// server's connection authenticating with the voice server, clients
// talking through it with synthetic audio, team/squad/leader routing,
// muting, removal, and refused passwords, versions and second servers.
#include "Net/GameProtocol.h"
#include "Net/Md5.h"
#include "TestMain.h"
#include "UI/Avm1/Interpreter.h"
#include "UI/Player/Natives.h"
#include "Voip/VoiceChat.h"
#include "Voip/VoipClient.h"
#include "Voip/VoipCodec.h"
#include "Voip/VoipNatives.h"
#include "Voip/VoipProtocol.h"
#include "Voip/VoipServer.h"
#include "Voip/VoipServerConnection.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <memory>
#include <thread>

using namespace dice::hfe;
using namespace dice::hfe::voip;

namespace {

constexpr double kTwoPi = 6.283185307179586;

std::vector<int16_t> sine(double frequency, double amplitude, size_t samples, size_t start = 0)
{
	std::vector<int16_t> out(samples);
	for (size_t i = 0; i < samples; ++i) {
		out[i] = static_cast<int16_t>(amplitude * std::sin(kTwoPi * frequency * static_cast<double>(start + i) / kSampleRate));
	}
	return out;
}

// The power at one frequency (Goertzel), per sample.
double tonePower(const int16_t* samples, size_t n, double frequency)
{
	const double k = 2.0 * std::cos(kTwoPi * frequency / kSampleRate);
	double s1 = 0, s2 = 0;
	for (size_t i = 0; i < n; ++i) {
		const double s0 = samples[i] + k * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return (s1 * s1 + s2 * s2 - k * s1 * s2) / static_cast<double>(n * n);
}

double rms(const int16_t* samples, size_t n)
{
	double sum = 0;
	for (size_t i = 0; i < n; ++i) {
		sum += static_cast<double>(samples[i]) * samples[i];
	}
	return n ? std::sqrt(sum / static_cast<double>(n)) : 0;
}

// A microphone fed by the test and speakers that keep what they played.
struct MemoryDevice : VoiceDevice {
	std::deque<int16_t> capture;
	std::vector<int16_t> played;
	double frequency = 1000.0;
	bool talking = false;
	size_t phase = 0;

	// 20 ms of the tone (or silence).
	void tick()
	{
		const std::vector<int16_t> frame = talking ? sine(frequency, 8000, kFrameSamples, phase) : std::vector<int16_t>(kFrameSamples, 0);
		phase += kFrameSamples;
		capture.insert(capture.end(), frame.begin(), frame.end());
	}
	size_t readCapture(int16_t* out, size_t max) override
	{
		const size_t n = std::min(max, capture.size());
		std::copy(capture.begin(), capture.begin() + static_cast<std::ptrdiff_t>(n), out);
		capture.erase(capture.begin(), capture.begin() + static_cast<std::ptrdiff_t>(n));
		return n;
	}
	void writePlayback(const int16_t* samples, size_t count) override { played.insert(played.end(), samples, samples + count); }
	size_t playbackQueued() const override { return 0; }

	// The loudest 20 ms played since `from`.
	double loudestSince(size_t from) const
	{
		double best = 0;
		for (size_t at = from; at + kFrameSamples <= played.size(); at += kFrameSamples) {
			best = std::max(best, rms(played.data() + at, kFrameSamples));
		}
		return best;
	}
};

struct Player {
	PlayerData data;
	MemoryDevice device;
	VoipClient client;
};

// A voice server, a game server's connection and players on loopback.
struct Session {
	VoipServerInstance server;
	VoipServerConnection connection;
	std::vector<std::unique_ptr<Player>> players;
	double now = 100.0;

	bool start(const std::string& serverPassword, const std::string& connectionPassword)
	{
		VoipServerInstance::Options options;
		options.port = 0;
		options.password = serverPassword;
		options.loopbackOnly = true;
		options.seed = 1234;
		if (!server.start(options)) {
			return false;
		}
		VoipServerConnection::Options c;
		c.host = "127.0.0.1";
		c.port = server.port();
		c.password = connectionPassword;
		c.loopbackOnly = true;
		return connection.start(c);
	}

	Player& add(uint16_t id, uint8_t team, uint8_t squad, bool leader, bool commander)
	{
		auto p = std::make_unique<Player>();
		p->data.team = team;
		p->data.squad = squad;
		p->data.squadLeader = leader;
		p->data.commander = commander;
		p->data.playerId = id;
		p->data.session = static_cast<uint16_t>(1000 + id * 7);
		net::UdpServerSocket::Address address;
		resolveAddress("127.0.0.1", server.port(), address);
		p->client.start(address, 0, 3, true);
		p->client.setDevice(&p->device);
		VoiceSettings settings;
		settings.pushToTalk = true;
		p->client.setSettings(settings);
		p->client.setSession(p->data.session);
		p->client.setChannels(selectChannels(commander, leader, false));
		players.push_back(std::move(p));
		return *players.back();
	}

	void tablesFromPlayers()
	{
		std::vector<PlayerData> list;
		for (const auto& p : players) {
			list.push_back(p->data);
		}
		connection.setPlayers(list);
	}

	// 20 ms steps: devices, game server, voice server, clients.
	void step(int count = 1)
	{
		for (int i = 0; i < count; ++i) {
			now += 0.02;
			for (auto& p : players) {
				p->device.tick();
			}
			connection.update(now);
			server.update(now);
			for (auto& p : players) {
				p->client.update(now);
			}
			if (i % 8 == 7) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
	}

	// Each player's played-sample count, to look at what comes next.
	std::vector<size_t> marks() const
	{
		std::vector<size_t> out;
		for (const auto& p : players) {
			out.push_back(p->device.played.size());
		}
		return out;
	}

	// `who` talks for a second; returns what everyone heard (the loudest frame).
	std::vector<double> talk(size_t who, bool leaderKey = false, const std::vector<int>& squads = {})
	{
		Player& p = *players[who];
		p.client.setChannels(selectChannels(p.data.commander, p.data.squadLeader, leaderKey, squads));
		const std::vector<size_t> from = marks();
		p.device.talking = true;
		p.client.setPushToTalk(true);
		step(50);
		p.device.talking = false;
		p.client.setPushToTalk(false);
		step(15);
		std::vector<double> heard;
		for (size_t i = 0; i < players.size(); ++i) {
			heard.push_back(players[i]->device.loudestSince(from[i]));
		}
		return heard;
	}
};

constexpr double kHeard = 500.0; // RMS of a frame of voice; silence decodes to ~0

} // namespace

TEST(Voip_CodecRoundTrip)
{
	// Frame sizes as GameSpy Voice computes them (bitrate / 400).
	const size_t expected[6] = {0, 9, 14, 20, 37, 61};
	for (int quality = 1; quality <= 5; ++quality) {
		SpeexEncoder encoder(speexQualityFor(quality));
		SpeexDecoder decoder;
		CHECK(encoder.frameBytes() == expected[quality]);
		const std::vector<int16_t> input = sine(440.0, 8000, kFrameSamples * 50);
		std::vector<int16_t> output(input.size());
		bool decoded = true;
		for (size_t f = 0; f < 50; ++f) {
			std::vector<uint8_t> bytes;
			encoder.encode(input.data() + f * kFrameSamples, bytes);
			CHECK(bytes.size() == encoder.frameBytes());
			decoded = decoder.decode(bytes.data(), bytes.size(), output.data() + f * kFrameSamples) && decoded;
		}
		CHECK(decoded);
		// After the codec settles: the tone comes back at about its level.
		const size_t skip = kFrameSamples * 10, n = input.size() - skip;
		const double in = rms(input.data() + skip, n), out = rms(output.data() + skip, n);
		CHECK(out > in * 0.5 && out < in * 1.5);
		CHECK(tonePower(output.data() + skip, n, 440.0) > 20.0 * tonePower(output.data() + skip, n, 1500.0));
	}
	// A lost frame is filled in.
	SpeexDecoder decoder;
	int16_t frame[kFrameSamples];
	decoder.conceal(frame);
}

TEST(Voip_Packets)
{
	CHECK(std::string(kVersionString) == "000001000010000112000000");
	CHECK(buildKeepAlivePacket(0x1234) == std::vector<uint8_t>({0x34, 0x12, 6}));

	const std::vector<uint8_t> payload = buildVoicePayload({1, 2, 3, 4}, 0x0102);
	CHECK(payload == std::vector<uint8_t>({1, 2, 3, 4, 2, 1}));
	ChannelSelection alternate;
	alternate.mode = kVoiceAlternateChannelSet;
	alternate.alternateSet = 1;
	const std::vector<uint8_t> packet = buildVoicePacket(payload, alternate, 0xabcd);
	CHECK(packet == std::vector<uint8_t>({1, 2, 3, 4, 2, 1, 1, 0xcd, 0xab, 2}));
	ClientPacket parsed;
	CHECK(parseClientPacket(packet, parsed));
	CHECK(parsed.type == kVoiceAlternateChannelSet && parsed.session == 0xabcd && parsed.channels.alternateSet == 1);
	CHECK(parsed.payload == payload);

	ChannelSelection multi;
	multi.mode = kVoiceMultiChannelSet;
	multi.multiSets = {2, 5};
	CHECK(parseClientPacket(buildVoicePacket(payload, multi, 7), parsed));
	std::vector<uint8_t> sets = parsed.channels.multiSets;
	std::sort(sets.begin(), sets.end());
	CHECK(sets == std::vector<uint8_t>({2, 5}) && parsed.payload == payload);
	CHECK(parseClientPacket(buildVoicePacket(payload, {}, 9), parsed) && parsed.type == kVoiceDefaultChannelSet);

	ForwardedVoice voice;
	CHECK(parseForwardedVoice(buildForwardedVoice(payload, 17), voice));
	CHECK(voice.speaker == 17 && voice.sequence == 0x0102 && voice.frames == std::vector<uint8_t>({1, 2, 3, 4}));

	// Game server packets: payload, version, session string, type.
	const std::string session = sessionStringFor("abcdefghij", "pw");
	CHECK(session.size() == kSessionStringSize && session == net::md5Hex("abcdefghijpw").substr(0, 20));
	PlayerData player;
	player.team = 2;
	player.squadLeader = true;
	player.squad = 4;
	player.session = 0x4321;
	player.playerId = 9;
	ServerPacket server;
	CHECK(parseServerPacket(buildServerPacket(kSetPlayerData, encodePlayerData(player), session), server));
	CHECK(server.type == kSetPlayerData && server.sessionString == session && server.version == kVersionString);
	PlayerData back;
	CHECK(decodePlayerData(server.payload.data(), server.payload.size(), back) && back == player);
	CHECK(encodePlayerData(player) == std::vector<uint8_t>({2, 0, 1, 4, 0x21, 0x43, 9, 0}));

	// Routing records.
	const RoutingRecord record = routingRecordFor(player);
	CHECK(record.flags == (0x80 | 0x20 | 4) && record.session == 0x4321);
	CHECK(playerDataFor(record, 9) == player);
	RoutingTable table {};
	table[9] = record;
	RoutingTable decoded {};
	CHECK(encodeRoutingTable(table).size() == 0x104);
	CHECK(decodeRoutingTable(encodeRoutingTable(table), decoded) && decoded == table);

	std::mt19937 random(7);
	const std::string challenge = makeChallenge(random);
	CHECK(challenge.size() == 10 && std::all_of(challenge.begin(), challenge.end(), [](char c) { return c >= 'a' && c <= 'z'; }));
	CHECK(buildChallengePacket(challenge).back() == 'h');
	CHECK(speexQualityFor(3) == 4 && speexQualityFor(5) == 10 && speexQualityFor(0) == 2);
}

TEST(Voip_ChannelPlans)
{
	ChannelPlan plan;
	PlayerData commander;
	commander.team = 1;
	commander.commander = true;
	CHECK(planChannels(commander, plan));
	CHECK(plan.incoming == 0 && plan.sets.size() == 10);
	CHECK(plan.sets[0] == std::vector<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8, 9}));
	CHECK(plan.sets[3] == std::vector<uint8_t>({3}));

	PlayerData leader;
	leader.team = 2;
	leader.squadLeader = true;
	leader.squad = 3;
	CHECK(planChannels(leader, plan));
	CHECK(plan.incoming == 0x16 && plan.sets.size() == 2);
	CHECK(plan.sets[0] == std::vector<uint8_t>({0x1f}) && plan.sets[1] == std::vector<uint8_t>({0x13}));
	CHECK(std::string(channelName(plan.incoming)) == "CHANNEL_TEAM_2_SQUAD_C_LEADER 3");

	PlayerData member;
	member.team = 1;
	member.squad = 2;
	CHECK(planChannels(member, plan));
	CHECK(plan.incoming == 11 && plan.sets.size() == 1 && plan.sets[0] == std::vector<uint8_t>({11, 2}));

	PlayerData loner;
	loner.team = 1;
	CHECK(!planChannels(loner, plan));
	member.squad = 10;
	CHECK(!planChannels(member, plan));

	CHECK(selectChannels(false, false, true).mode == kVoiceDefaultChannelSet);
	const ChannelSelection toCommander = selectChannels(false, true, true);
	CHECK(toCommander.mode == kVoiceAlternateChannelSet && toCommander.alternateSet == 1);
	CHECK(selectChannels(true, false, true).mode == kVoiceDefaultChannelSet);
	const ChannelSelection squads = selectChannels(true, false, false, {2, 4});
	CHECK(squads.mode == kVoiceMultiChannelSet && squads.multiSets == std::vector<uint8_t>({2, 4}));
}

TEST(Voip_ServerSettingsBlock)
{
	ServerVoipSettings settings;
	settings.enabled = true;
	settings.address = 1;
	settings.serverPort = 55125;
	settings.clientPort = 55123;
	settings.quality = 5;
	ServerVoipSettings back;
	CHECK(parseServerVoipSettings(buildServerVoipSettingsBlock("My server", settings), back));
	CHECK(back.enabled && back.address == 1 && back.serverPort == 55125 && back.clientPort == 55123 && back.quality == 5);
	CHECK(!parseServerVoipSettings(std::string {5, 0, 'a', 'b'}, back));
}

// A game server authenticates, players talk; who hears whom.
TEST(Voip_LoopbackSession)
{
	Session s;
	CHECK(s.start("secret", "secret"));
	Player& commander = s.add(0, 1, 0, false, true);   // 0
	Player& leader1 = s.add(1, 1, 1, true, false);     // 1
	Player& member1 = s.add(2, 1, 1, false, false);    // 2
	Player& member1b = s.add(3, 1, 1, false, false);   // 3
	Player& member2 = s.add(4, 1, 2, false, false);    // 4
	Player& enemy = s.add(5, 2, 1, false, false);      // 5
	Player& loner = s.add(6, 1, 0, false, false);      // 6: no squad
	s.tablesFromPlayers();
	s.step(10);

	// The challenge was answered; the table made the clients.
	CHECK(s.server.gameServerConnected());
	CHECK(s.connection.authenticated());
	CHECK(s.server.counters().invalidAuthentication == 0);
	CHECK(s.server.clientCount() == 6);
	CHECK(s.server.client(loner.data.session) == nullptr);
	CHECK(s.server.client(member1.data.session) && s.server.client(member1.data.session)->incoming == memberChannel(1, 1));
	CHECK(s.server.routing()[2] == routingRecordFor(member1.data));
	// Keep-alives (every 3 s) give the voice server the clients' addresses.
	s.step(160);
	CHECK(s.server.client(enemy.data.session)->address.port != 0);

	// A squad member: the squad and its leader.
	std::vector<double> heard = s.talk(2);
	CHECK(heard[1] > kHeard && heard[3] > kHeard);
	CHECK(heard[0] < kHeard && heard[4] < kHeard && heard[5] < kHeard && heard[2] < kHeard);
	CHECK(leader1.client.counters().framesDecoded > 20);
	CHECK(member1b.client.counters().framesConcealed < 10);
	// The voice is the tone that was said (through the filter and Speex).
	{
		const auto& played = member1b.device.played;
		size_t loudest = 0;
		double best = 0;
		for (size_t at = 0; at + kFrameSamples * 10 <= played.size(); at += kFrameSamples) {
			const double r = rms(played.data() + at, kFrameSamples * 10);
			if (r > best) {
				best = r;
				loudest = at;
			}
		}
		CHECK(tonePower(played.data() + loudest, kFrameSamples * 10, 1000.0)
			> 10.0 * tonePower(played.data() + loudest, kFrameSamples * 10, 2500.0));
	}

	// The squad leader: the squad; the commander with the leader key.
	heard = s.talk(1);
	CHECK(heard[2] > kHeard && heard[3] > kHeard && heard[0] < kHeard);
	heard = s.talk(1, true);
	CHECK(heard[0] > kHeard && heard[2] < kHeard && heard[3] < kHeard);

	// The commander: all squad leaders with the key, else the selected squads.
	heard = s.talk(0, true);
	CHECK(heard[1] > kHeard && heard[2] < kHeard && heard[4] < kHeard);
	heard = s.talk(0, false, {2});
	CHECK(heard[1] < kHeard);
	heard = s.talk(0, false, {1, 2});
	CHECK(heard[1] > kHeard);

	// Without a squad nobody hears the loner (the server doesn't know the session).
	const size_t unknown = s.server.counters().unknownSession;
	heard = s.talk(6);
	CHECK(std::all_of(heard.begin(), heard.end(), [](double h) { return h < kHeard; }));
	CHECK(s.server.counters().unknownSession > unknown);

	// Muting: the member mutes the leader; the other member still hears.
	member1.client.setMuted(1, true);
	heard = s.talk(1);
	CHECK(heard[2] < kHeard && heard[3] > kHeard);
	CHECK(member1.client.counters().mutedDropped > 0);
	member1.client.setMuted(1, false);

	// Removed from the server: no longer heard.
	s.connection.removePlayer(member1b.data.session);
	s.step(2);
	CHECK(s.server.client(member1b.data.session) == nullptr);
	CHECK(s.server.routing()[3] == RoutingRecord {});
	heard = s.talk(3);
	CHECK(heard[1] < kHeard && heard[2] < kHeard);

	// The routing table brings it back (and moves the squad-2 member to squad 1).
	member2.data.squad = 1;
	s.tablesFromPlayers();
	s.step(3);
	CHECK(s.server.client(member1b.data.session) != nullptr);
	CHECK(s.server.client(member2.data.session)->incoming == memberChannel(1, 1));
	s.step(160); // the keep-alives restore the addresses
	heard = s.talk(4);
	CHECK(heard[1] > kHeard && heard[2] > kHeard && heard[3] > kHeard);

	// Talking for over 10 s: held back for 5 s.
	Player& talker = member1;
	talker.device.talking = true;
	talker.client.setPushToTalk(true);
	s.step(560);
	CHECK(talker.client.blocked());
	talker.device.talking = false;
	talker.client.setPushToTalk(false);
	s.step(300);
	CHECK(!talker.client.blocked());
	(void)commander;
}

TEST(Voip_RefusedGameServers)
{
	// A wrong password: challenged, never accepted.
	{
		Session s;
		CHECK(s.start("secret", "guess"));
		s.add(1, 1, 1, false, false);
		s.tablesFromPlayers();
		s.step(5);
		s.now += 6;
		s.step(5);
		CHECK(!s.server.gameServerConnected());
		CHECK(s.server.counters().invalidAuthentication > 0);
		CHECK(s.server.clientCount() == 0);
		CHECK(s.connection.authenticated()); // it answered, wrongly
	}
	// A wrong version; a second game server while the first is there; the
	// first going quiet for 28 s lets the second in.
	{
		Session s;
		CHECK(s.start("secret", "secret"));
		for (int i = 0; i < 100 && !s.server.gameServerConnected(); ++i) {
			s.step();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CHECK(s.server.gameServerConnected());

		net::UdpServerSocket other;
		CHECK(other.bind(0, true));
		net::UdpServerSocket::Address address, otherAddress;
		CHECK(resolveAddress("127.0.0.1", s.server.port(), address));
		CHECK(resolveAddress("127.0.0.1", other.port(), otherAddress));
		std::vector<uint8_t> reply;
		net::UdpServerSocket::Address from;
		// Updates the voice server at `at` until `done` (the packet arrived).
		const auto pump = [&](double at, const std::function<bool()>& done) {
			for (int i = 0; i < 200; ++i) {
				if (done()) {
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				s.server.update(at);
			}
			return false;
		};

		other.sendTo(address, buildServerPacket(kResetPlayerRouting, {}, unauthenticatedSessionString()));
		CHECK(pump(s.now, [&] { return s.server.counters().inUse == 1; }));
		CHECK(other.receiveFrom(reply, from, 50) == 0); // refused without a challenge

		// The first one is quiet: a new challenge, the second gets it.
		const double later = s.now + 30;
		other.sendTo(address, buildServerPacket(kResetPlayerRouting, {}, unauthenticatedSessionString()));
		CHECK(pump(later, [&] { return other.receiveFrom(reply, from, 0) > 0; }));
		CHECK(!s.server.gameServerConnected());
		CHECK(!reply.empty() && reply.back() == kChallenge);
		const std::string session = sessionStringFor(std::string(reply.begin(), reply.end() - 1), "secret");
		other.sendTo(address, buildServerPacket(kResetPlayerRouting, {}, session, "000001000010000111000000"));
		CHECK(pump(later, [&] { return s.server.counters().invalidVersion == 1; }));
		CHECK(!s.server.gameServerConnected());
		other.sendTo(address, buildServerPacket(kResetPlayerRouting, {}, session));
		CHECK(pump(later, [&] { return s.server.gameServerConnected(); }));
		CHECK(s.server.gameServer() == otherAddress);
	}
}

TEST(Voip_VoiceActivityAndMicTest)
{
	// Without push-to-talk a frame goes when it is louder than the threshold,
	// and 20 more after the last loud one.
	MemoryDevice device;
	VoipClient client;
	client.setDevice(&device);
	VoiceSettings settings;
	settings.pushToTalk = false;
	settings.captureThreshold = 0.1f;
	client.setSettings(settings);
	client.setLoopbackTest(true);
	double now = 0;
	device.talking = true;
	for (int i = 0; i < 30; ++i) {
		device.tick();
		client.update(now += 0.02);
	}
	CHECK(client.transmitting());
	CHECK(client.captureLevel() > 0.2f);
	const size_t before = device.played.size();
	CHECK(device.loudestSince(0) > kHeard); // the mic test plays it back
	device.talking = false;
	for (int i = 0; i < 60; ++i) {
		device.tick();
		client.update(now += 0.02);
	}
	CHECK(!client.transmitting());
	CHECK(device.played.size() > before);
	// The last 20 frames played are silence.
	CHECK(rms(device.played.data() + device.played.size() - kFrameSamples * 20, kFrameSamples * 20) < 50);

	// A quiet microphone stays under the threshold.
	MemoryDevice quiet;
	VoipClient quietClient;
	quietClient.setDevice(&quiet);
	quietClient.setSettings(settings);
	quietClient.setLoopbackTest(true);
	now = 0;
	for (int i = 0; i < 30; ++i) {
		const std::vector<int16_t> soft = sine(1000.0, 1000, kFrameSamples, static_cast<size_t>(i) * kFrameSamples);
		quiet.capture.insert(quiet.capture.end(), soft.begin(), soft.end());
		quietClient.update(now += 0.02);
	}
	CHECK(!quietClient.transmitting());
	CHECK(quiet.loudestSince(0) < 50);
}

TEST(Voip_ChatEventsAndNatives)
{
	auto owned = std::make_unique<MemoryDevice>();
	MemoryDevice& device = *owned;
	VoiceChat chat(std::move(owned));
	ui::NativeRegistry natives;
	bindVoipNatives(natives, chat);
	ui::avm1::Interpreter vm;
	const auto call = [&](const char* member, std::vector<ui::avm1::Value> args = {}) {
		return natives.call(vm, "dice.bf2.Sound", member, args);
	};
	CHECK(ui::avm1::toBoolean(call("voipInitialize")));
	call("voipSetRecieveVolume", {ui::avm1::Value(0.25)});
	CHECK(std::fabs(vm.toNumber(call("voipGetRecieveVolume")) - 0.25) < 1e-6);
	call("voipSetUsePushToTalk", {ui::avm1::Value(false)});
	CHECK(!ui::avm1::toBoolean(call("voipGetUsePushToTalk")));
	call("voipSetBoost", {ui::avm1::Value(true)});
	CHECK(ui::avm1::toBoolean(call("voipIsBoostEnabled")));
	call("voipSetBoost", {ui::avm1::Value(false)});
	call("voipSetRecieveVolume", {ui::avm1::Value(1.0)});

	// The options page's mic test.
	call("voipStartTest");
	device.talking = true;
	double now = 0;
	for (int i = 0; i < 25; ++i) {
		device.tick();
		chat.update(now += 0.02);
	}
	CHECK(ui::avm1::toBoolean(call("voipSendingVoice")));
	CHECK(vm.toNumber(call("voipGetSendingVoiceVolume")) > 0.2);
	CHECK(device.loudestSince(0) > kHeard);
	call("voipStopTest");
	CHECK(!chat.testing());

	// Events: the session starts the client once the server's settings are
	// known; mutes for the local player apply.
	VoipServerInstance server;
	VoipServerInstance::Options options;
	options.port = 0;
	options.loopbackOnly = true;
	CHECK(server.start(options));
	ServerVoipSettings settings;
	settings.enabled = true;
	settings.address = 1; // the game server's
	settings.serverPort = server.port();
	settings.clientPort = 0;
	settings.quality = 3;
	CHECK(chat.setServerSettingsBlock(buildServerVoipSettingsBlock("Test", settings), "127.0.0.1"));
	CHECK(!chat.active());
	net::GameEvent session;
	session.type = net::kEvVoipSession;
	session.values = {4242};
	CHECK(chat.handleEvent(session, 3));
	CHECK(chat.active() && chat.client().session() == 4242);
	net::GameEvent mute;
	mute.type = net::kEvVoipPlayerMute;
	mute.values = {1, 7, 3};
	CHECK(chat.handleEvent(mute, 3));
	CHECK(chat.client().muted(7));
	mute.values = {1, 8, 2}; // someone else's mute
	chat.handleEvent(mute, 3);
	CHECK(!chat.client().muted(8));
	net::GameEvent onOff;
	onOff.type = net::kEvVoipOnOff;
	onOff.values = {5, 1};
	CHECK(chat.handleEvent(onOff, 3));
	CHECK(chat.playerVoipOn(5) && !chat.playerVoipOn(6));
	// Without a squad we may not talk; a squad leader with the key talks to the commander.
	chat.setRole(false, true, 2);
	chat.setKeys(false, true);
	CHECK(chat.client().channels().mode == kVoiceAlternateChannelSet);
	chat.update(now += 0.02);
	server.update(now);
	chat.leave();
	CHECK(!chat.active());

	// On its own thread (as the game runs it): starts and stops cleanly.
	VoiceChat background(std::make_unique<MemoryDevice>()); // not the real microphone
	background.runInBackground(true);
	background.startTest();
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CHECK(background.testing());
	background.runInBackground(false);
}
