// The wire audit's vectors (docs/engine.md, "The wire audit"): each message
// built bit by bit from the field list derived from BF2142.exe's own reader
// and writer (not from our codec), then read by ours to exactly its length
// and written back bit for bit.
#include "Net/BitStream.h"
#include "Net/EventChannel.h"
#include "Net/GhostLayout.h"
#include "Net/Networkables.h"
#include "World/ObjectTemplateManager.h"
#include "Net/GameConnection.h"
#include "Net/GameProtocol.h"
#include "Net/NetDiagnostics.h"
#include "TestMain.h"

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

// A field list: uN (N bits of a value), sN (sign + N-1 bits), bN (N bytes),
// cN (an 8-bit length N and N bytes), V:N (the literal V in N bits, for
// counts and the bits that gate a branch).
struct Built {
	net::BitWriter bits;
	bool ok = true;
};

Built build(uint32_t type, const std::string& spec, uint32_t seed)
{
	Built b;
	b.bits.write(type, 7);
	std::istringstream in(spec);
	std::string field;
	uint32_t x = seed * 2654435761u + 0x9e3779b9u;
	const auto next = [&x] {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		return x;
	};
	while (in >> field) {
		const size_t colon = field.find(':');
		if (colon != std::string::npos) {
			b.bits.write(static_cast<uint32_t>(std::stoul(field.substr(0, colon))), static_cast<unsigned>(std::stoul(field.substr(colon + 1))));
			continue;
		}
		const unsigned n = static_cast<unsigned>(std::stoul(field.substr(1)));
		switch (field[0]) {
		case 'u':
			b.bits.write(n == 32 ? next() : next() & ((1u << n) - 1), n);
			break;
		case 's': {
			const int32_t magnitude = static_cast<int32_t>(next() & ((n == 32 ? 0x7fffffffu : (1u << (n - 1)) - 1)));
			b.bits.writeSigned((next() & 1) ? -magnitude : magnitude, n);
			break;
		}
		case 'b':
			for (unsigned i = 0; i < n; ++i) {
				b.bits.write(next() & 0xff, 8);
			}
			break;
		case 'c':
			b.bits.write(n, 8);
			for (unsigned i = 0; i < n; ++i) {
				b.bits.write(next() & 0xff, 8);
			}
			break;
		default:
			b.ok = false;
		}
	}
	return b;
}

struct Vector {
	uint32_t type;
	const char* spec;
};

// From the exe's readers (slot +0x28) and writers (slot +0x2c); one line a
// branch for the events with conditions.
const Vector kEvents[] = {
	{0x01, "b10 c9"},                                     // 0x00509cc0 / 0x00509d40
	{0x02, "b73 u32 u32 u32"},                            // 0x00509e20 / 0x00509eb0
	{0x03, "5:3"},                                        // 0x0050a030: 0..6 in 3 bits
	{0x04, "1:1 u32 u32"},                                // 0x0050a1a0 / 0x0050a250: the start
	{0x04, "0:1 c63"},                                    //   a chunk
	{0x05, "u3 u4 u1 u8 u16 u16 u1 b32 s32 s4 u1"},       // 0x0050a4c0 / 0x0050a5e0
	{0x06, "u32 u16 u2 1:1 u8"},                          // 0x0050b150 / 0x0050b270: many ghosts
	{0x06, "u32 u16 u2 0:1 1:1 u32 u32 u32 1:1 u32 u32 u32"},
	{0x06, "u32 u16 u2 0:1 0:1 0:1"},
	{0x07, "u16 u1"},                                     // 0x0050b6a0 / 0x0050b710
	{0x08, "u8"},                                         // 0x0050b550 / 0x0050b5a0
	{0x09, "u8 u16 u1"},                                  // 0x0050b850 / 0x0050b8d0
	{0x0a, "u8 u1"},                                      // 0x00510c20 / 0x00510c80
	{0x0b, "u4 u32 u32 c52"},                             // 0x005096a0 / 0x00509740
	{0x0c, "u8 b32"},                                     // 0x0050ba40 / 0x0050baa0
	{0x0d, "u8 u16 u16 u32 u32 u32"},                     // 0x0050c280 / 0x0050c0f0
	{0x0e, "u8 u16 u16 u8 u8 u8 u8 u8 u8 u8 u8 u1 u32"},  // 0x0050bdb0 / 0x0050beb0
	{0x0f, "1:1 u8"},                                     // 0x0050c360 / 0x0050c3f0
	{0x0f, "0:1 c40"},
	{0x10, "u8 u8 u8"},                                   // 0x0050c590 / 0x0050c610
	{0x11, "u8 u8 u8 u1"},                                // 0x0050c790 / 0x0050c820
	{0x13, "8:4 u8 s16"},                                 // 0x0050c940 / 0x0050c9c0: bitsFor(9)
	{0x14, "u16 u8 u2 u8 19:5 4:3"},                      // 0x0050d100 / 0x0050d1f0
	{0x15, "u8 u8 u1 u16 u32 u32 u8 0:2 u32 u32 u32 u10"}, // 0x0050d620 / 0x0050d750, raw position
	{0x15, "u8 u8 u1 u16 u32 u32 u8 1:2 s20 s20 s20 u10"}, //   delta position, mode 1
	{0x15, "u8 u8 u1 u16 u32 u32 u8 3:2 s12 s12 s12 u10"}, //   mode 3
	{0x16, "u8 u8 u8 c31"},                               // 0x0050d8b0 / 0x0050d960
	{0x17, "u8 u8 u1"},                                   // 0x0050db90 / 0x0050dc10
	{0x18, "u8 u8 u8 1:1 u16 u8 u1"},                     // 0x0050e220 / 0x0050dfe0: an object
	{0x18, "u8 u8 u8 0:1 u32 u32 u32 u32 u32 u32 u32 u32 u32 u8 u1"},
	{0x19, "u8 u8 u8 u1"},                                // 0x0050e5a0 / 0x0050e630
	{0x1a, "u2 100:7 u32 u8"},                            // 0x0050cc80 / 0x0050cd20
	{0x1b, "u8 u8 u1 u8 u8"},                             // 0x0050dd90 / 0x0050de40
	{0x1c, "u8 u8 u8"},                                   // JoinSquad's reader and writer
	{0x1d, "u16 u16 u32 u32 u8 u32 u32"},                 // 0x0050e770 / 0x0050e830
	{0x1e, "1:2 u32 u32 u32 u32"},                        // 0x0050ec60 / 0x0050ed20
	{0x1e, "2:2 u32 u32 u32 u16 u32"},
	{0x1e, "0:2 u32 u32 u32"},
	{0x1f, "u32 u16 u32 u32 u32 u4 u8 u8 u8 u8"},         // 0x0050ac80 / 0x0050aa50
	{0x20, "u16 7:16 u8 u8 u8 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32"}, // 0x0050f720 / 0x0050f460
	{0x20, "u16 0:16 u8 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32 u32"},
	{0x21, "1:2 u32 u32 u32"},                            // 0x0050efd0 / 0x0050f060
	{0x21, "2:2 u32 u16"},
	{0x22, "u16 u8 u16 u32 u32 u32"},                     // 0x005100a0 / 0x00510160
	{0x23, "u8 u1"},                                      // 0x00510c20 / 0x00510c80
	{0x24, "u32 u32 u32"},                                // 0x00510ae0 / 0x00510a50
	{0x25, "u32 u32 u32"},                                // 0x00511110 / 0x00511180
	{0x26, "u1 u8 u8"},                                   // 0x00510e10 / 0x00510e90
	{0x27, "u8 u8 u8 u8 u8 u8 u8 u8 u8 u32"},             // 0x005112b0 / 0x005113c0
	{0x28, ""},                                           // 0x005102e0 / 0x00510300
	{0x29, "u32 u8 u4 u12"},                              // 0x005103c0 / 0x00510450
	{0x2a, "3:2 u8 b56"},                                 // 0x00508b90 / 0x00508c60
	{0x2b, "u16 u8 u4 u1"},                               // 0x0050f9e0 / 0x0050fa70
	{0x2c, "1:2 u8 u8 u8 u8"},                            // 0x00510660 / 0x00510970
	{0x2c, "2:2 u8 u8"},
	{0x2d, "u32 u32 u8 u1"},                              // 0x0050ea60 / 0x0050ead0
	{0x2e, "b16 b16 b16"},                                // 0x00512470 / 0x005124d0
	{0x2f, "u16 u8 u32 u32 u32 u32 u32 u32"},             // 0x0050fd90 / 0x0050fe40
	{0x30, "u32 u32 u32"},                                // 0x00511750 / 0x005117d0
	{0x31, "u32 u8 3:4 u32 u32 u32"},                     // 0x00511a50 / 0x00511970
	{0x32, "u8 u8 4:3"},                                  // 0x00511fb0 / 0x00512040
	{0x33, "u16 u2"},                                     // 0x00512a20 / 0x00512a80
	{0x34, "u16 u8"},                                     // 0x00513f40 / 0x00513fa0
	{0x36, "u16"},                                        // 0x00512250 / 0x005122a0
	{0x37, "u8 u1 u8"},                                   // 0x0050cec0 / 0x0050cf30
	{0x38, "u32 u32 s8"},                                 // 0x00511540 / 0x005115b0
	{0x39, "u8 u4 u1 u1 u1 u8 u8 u16"},                   // 0x005125c0 / 0x005126b0
	{0x3a, "u8 u16"},                                     // 0x00512880 / 0x005128e0
	{0x3b, "u16 0:1 u1 u1 u1 u1"},                        // 0x00512c00 / 0x00512cd0
	{0x3b, "u16 1:1"},
	{0x3c, "1:1 u8 u8 u8 u32 u32 u32"},                   // 0x00513000 / 0x005130c0
	{0x3c, "0:1 u8 u8 u8"},
	{0x3d, "u1"},                                         // 0x00509b20 / 0x00509b70
	{0x3e, "u4"},                                         // 0x0050ad50 / 0x0050ada0
	{0x3f, "u4 u8 u8 u8 u8 u1"},                          // 0x0050ae90 / 0x0050af30
	{0x40, "u16 u32"},                                    // 0x00513410 / 0x00513470
	{0x41, "1:2 200:9 3:7 30:6 256:9 7:9 0:9"},           // 0x00508800 / 0x00508410
	{0x42, "u32"},                                        // 0x00513560 / 0x005135b0
	{0x43, "u32 u32"},                                    // 0x005136f0 / 0x00513750
	{0x44, "u16 u16"},                                    // 0x00513910 / 0x00513970
	{0x45, "u32 u32 u32 u32 u32 u32"},                    // 0x00513b10 / 0x00513ba0
	{0x46, "u8 u4"},                                      // 0x00513d80 / 0x00513de0
	{0x47, "u8 u1 u8 u8"},                                // 0x00514100 / 0x00514190
};

bool sameBits(const net::BitWriter& a, const net::BitWriter& b)
{
	return a.bits() == b.bits() && a.data() == b.data();
}

} // namespace

// Every registered event, every branch: our reader takes exactly the
// exe's bits and our writer gives them back.
TEST(NetWire_EveryEventToItsLength)
{
	uint32_t seed = 1;
	for (const Vector& v : kEvents) {
		const Built b = build(v.type, v.spec, seed++);
		CHECK(b.ok);
		net::BitReader in(b.bits.data().data(), b.bits.data().size());
		in.limit(b.bits.bits());
		net::GameEvent event;
		const bool read = net::readGameEvent(in, event);
		if (!read || in.position() != b.bits.bits()) {
			std::printf("  event 0x%02x \"%s\": read %d, %zu of %zu bits\n", v.type, v.spec, read ? 1 : 0, in.position(),
				b.bits.bits());
		}
		CHECK(read && in.position() == b.bits.bits());
		net::BitWriter out;
		CHECK(net::writeGameEvent(out, event));
		CHECK(sameBits(out, b.bits));
	}
	// The unregistered ids (no registration at 0, 0x12, 0x35 or 0x48 on).
	for (uint32_t type : {0u, 0x12u, 0x35u, 0x48u, 0x7fu}) {
		net::BitWriter w;
		w.write(type, 7);
		w.write(0, 32);
		net::BitReader in(w.data().data(), w.data().size());
		net::GameEvent event;
		CHECK(!net::readGameEvent(in, event));
	}
}

// The event block (0x006447b0 / 0x00644bf0): 1, count (8), sequence (5),
// resent (1), the events back to back, no lengths or alignment.
TEST(NetWire_EventBlockFraming)
{
	net::BitWriter w;
	w.write(1, 1);
	w.write(2, 8);
	w.write(29, 5);
	w.write(1, 1);
	w.write(0x08, 7); // DestroyPlayer
	w.write(0x2a, 8);
	w.write(0x3d, 7); // UavWarning
	w.write(1, 1);
	net::BitReader in(w.data().data(), w.data().size());
	in.limit(w.bits());
	bool present = false;
	net::ReliableEventBlock block;
	CHECK(net::readEventBlock(in, present, block) && present && in.position() == w.bits());
	CHECK(block.sequence == 29 && block.resent && block.events.size() == 2 && block.events[0].u(0) == 0x2a);
	net::BitWriter out;
	net::writeEventBlock(out, &block);
	CHECK(sameBits(out, w));
	net::BitWriter none;
	net::writeEventBlock(none, nullptr);
	CHECK(none.bits() == 1 && none.data()[0] == 0);
}

// The input block (0x00590990 / 0x00590740, moves 0x00590570 /
// 0x00590650): 1, count (4), 9 bits, the first index (sign + 31), per move
// six sign + 15 axes, 32 + 11 trigger bits and the device-axis bit.
TEST(NetWire_InputBlock)
{
	net::BitWriter w;
	w.write(1, 1);
	w.write(2, 4);
	w.write(200, 9);
	w.writeSigned(41, 32);
	for (int m = 0; m < 2; ++m) {
		for (int a = 0; a < 6; ++a) {
			w.write(a & 1, 1);
			w.write(static_cast<uint32_t>(100 * a + m), 15);
		}
		w.write(0x80000001u, 32);
		w.write(0x401, 11);
		w.write(1, 1);
	}
	net::BitReader in(w.data().data(), w.data().size());
	in.limit(w.bits());
	net::InputBlock block;
	CHECK(net::readInputBlock(in, block) && in.position() == w.bits());
	CHECK(block.present && block.value9 == 200 && block.moves.size() == 2 && block.moves[1].index == 42);
	CHECK(block.moves[0].axes[1] == -100 && block.moves[1].axes[4] == 401 && block.moves[0].triggersHigh == 0x401);
	net::BitWriter out;
	net::writeInputBlock(out, block);
	CHECK(sameBits(out, w));
}

// The packets of the connection (0x0068dd00 connect, 0x00689400 accept,
// 0x006a9bb0 ping, 0x006a9c30 pong, 0x0068ef80 data with the notify header
// 0x006aa980).
TEST(NetWire_ConnectionPackets)
{
	net::ConnectRequest request;
	request.key = "abcd";
	request.password = "pw";
	const std::vector<uint8_t> connect = net::connectPacket(request);
	net::BitWriter w;
	w.write(1, 4);
	w.write(1, 8); // 0x0068ddd1: mov dl, 1
	w.write(0x1002, 32);
	w.write(0x1a007000, 32);
	w.write(0, 1);
	w.writeText("abcd", 4);
	w.writeText("pw", 32);
	w.writeText("mods/bf2142", 32);
	CHECK(connect == w.data());

	net::NotifyState state;
	state.sendSequence = 5;
	state.lastReceived = 9;
	state.receivedMask = 0x13;
	net::BitWriter data;
	data.write(15, 4);
	data.write(3, 8);
	data.write(6, 6);
	data.write(9, 6);
	data.write(0x13, 32);
	data.write(2, 16);
	data.write(0xab, 8);
	data.write(0xcd, 8);
	CHECK(net::dataPacket(3, 6, state, {0xab, 0xcd}) == data.data());

	net::BitWriter ping;
	ping.write(7, 4);
	ping.write(3, 8);
	ping.write(5, 6);
	ping.write(9, 6);
	ping.write(0x13, 32);
	ping.write(1, 1);
	ping.write(1234, 32);
	CHECK(net::pingPacket(3, state, 1234) == ping.data());
	net::ConnectedPacket packet;
	CHECK(net::readConnectedPacket(ping.data(), packet) && packet.sequenced && packet.time == 1234);
}

// The diagnostics' payload walk flags what doesn't add up: an unknown
// event type, bits left after the ghost block, a read past the end.
TEST(NetWire_DiagnosticsFlagProblems)
{
	net::BitWriter good;
	good.write(0, 1); // no input
	good.write(0, 1); // no events
	good.write(0, 1); // no ghosts
	CHECK(net::describePayload(good.data(), true).problems.empty());

	net::BitWriter unknown;
	unknown.write(0, 1);
	unknown.write(1, 1);
	unknown.write(1, 8);
	unknown.write(0, 5);
	unknown.write(0, 1);
	unknown.write(0x35, 7);
	const net::PayloadReport r1 = net::describePayload(unknown.data(), true);
	CHECK(r1.problems.size() == 1 && r1.problems[0].rfind("unknown event type", 0) == 0);

	std::vector<uint8_t> extra = good.data();
	extra.push_back(0);
	const net::PayloadReport r2 = net::describePayload(extra, true);
	CHECK(r2.problems.size() == 1 && r2.problems[0].rfind("bits left", 0) == 0);

	net::BitWriter cut;
	cut.write(0, 1);
	cut.write(0, 1);
	cut.write(1, 1); // a ghost block whose header doesn't fit
	const net::PayloadReport r3 = net::describePayload(cut.data(), true);
	CHECK(r3.problems.size() == 1 && r3.problems[0].rfind("read past end", 0) == 0);

	// A whole data datagram whose 16-bit length disagrees with its size.
	net::NotifyState state;
	std::vector<uint8_t> datagram = net::dataPacket(1, 1, state, good.data());
	datagram.push_back(0);
	const net::PayloadReport r4 = net::describeDatagram(datagram, false);
	CHECK(!r4.problems.empty() && r4.problems[0].rfind("length", 0) == 0);
}

// The capture format round trips.
TEST(NetWire_CaptureFile)
{
	std::vector<net::CaptureRecord> records {{false, 0, {1, 2, 3}}, {true, 17, {}}, {false, 4000, std::vector<uint8_t>(1472, 9)}};
	const std::string path = (std::filesystem::temp_directory_path() / "netwire-capture.bf2142cap").string();
	CHECK(net::writeCapture(path, records));
	std::vector<net::CaptureRecord> back;
	CHECK(net::readCapture(path, back) && back.size() == 3);
	CHECK(back[1].outgoing && back[1].milliseconds == 17 && back[2].datagram.size() == 1472 && back[0].datagram[2] == 3);
	std::remove(path.c_str());
}

// The teams' networkable's last bit (0x00560ff0 / 0x0055fe30): sign + 2,
// then two sign + 17s; the state ends exactly there.
TEST(NetWire_TeamStateLastBit)
{
	net::BitWriter w;
	w.write(0x80000400u, 32);
	w.writeSigned(250, 18); // 0x400: the tickets, a bit, a value
	w.write(1, 1);
	w.writeSigned(-3, 18);
	w.writeSigned(2, 3); // 0x80000000
	w.writeSigned(-70000, 18);
	w.writeSigned(5, 18);
	net::BitReader in(w.data().data(), w.data().size());
	in.limit(w.bits());
	net::TeamNetState state;
	CHECK(net::decodeTeamState(in, state) && in.position() == w.bits());
	CHECK(state.tickets(1) == 250 && state.values[31][0] == 2 && state.values[31][1] == -70000 && state.values[31][2] == 5);
	net::BitWriter out;
	net::encodeTeamState(out, state);
	CHECK(out.bits() == w.bits() && out.data() == w.data());
}

// GenericProjectile (0x00606f20): mask 0x10 without armour reads no hit
// points, and the 0x20 bit still follows.
TEST(NetWire_ProjectileWithoutArmour)
{
	net::BitWriter w;
	w.write(0x30, 6);
	w.write(1, 1); // 0x20
	net::NetworkableHints h;
	h.kind = net::NetworkableKind::GenericProjectile;
	net::BitReader in(w.data().data(), w.data().size());
	in.limit(w.bits());
	net::NetworkableState s;
	CHECK(net::decodeNetworkable(in, h, {}, s) && s.complete && in.position() == w.bits());
}

// The input block (0x00590740): no moves is a lone 0; at most the last
// three; the axes truncated and clamped to +-32767 (0x005837d0).
TEST(NetWire_InputBlockLimits)
{
	net::InputBlock empty;
	empty.present = true;
	net::BitWriter w;
	net::writeInputBlock(w, empty);
	CHECK(w.bits() == 1 && w.data()[0] == 0);
	net::InputBlock five;
	five.present = true;
	for (int i = 0; i < 5; ++i) {
		net::PlayerMove m;
		m.index = 10 + i;
		five.moves.push_back(m);
	}
	net::BitWriter out;
	net::writeInputBlock(out, five);
	net::BitReader in(out.data().data(), out.data().size());
	net::InputBlock back;
	CHECK(net::readInputBlock(in, back) && back.moves.size() == 3 && back.moves.front().index == 12);
	net::PlayerMove m;
	m.setAxis(0, 0.299f);
	m.setAxis(1, -1000.0f);
	m.setAxis(2, -0.016f);
	CHECK(m.axes[0] == 29 && m.axes[1] == -32767 && m.axes[2] == -1);
}

// The event channel (0x00644bf0 / 0x006450d0): at most 14 blocks in
// flight; a packet without a block clears its sequence's entry.
TEST(NetWire_EventChannelWindow)
{
	net::EventChannel channel;
	net::GameEvent e;
	e.type = 0x3d;
	e.values = {1};
	int blocks = 0;
	for (uint32_t packet = 1; packet < 40; ++packet) {
		channel.queue(e);
		if (const auto block = channel.next()) {
			channel.sent(packet, block->sequence);
			++blocks;
		}
	}
	CHECK(blocks == 14);
	// Acknowledge packets 1..14 (mask bits 0..13): room again.
	channel.acknowledge(14, 0x3fff);
	CHECK(channel.next().has_value());
	// A stale entry under sequence 50 is cleared by a block-less packet.
	net::EventChannel other;
	other.queue(e);
	const auto block = other.next();
	CHECK(block.has_value());
	other.sent(50, block->sequence);
	other.sentWithout(50);
	other.acknowledge(50, 1); // would have settled the block
	other.acknowledge(51, 0); // and this would have lost it
	CHECK(!other.idle());     // the block is still waiting: neither came from its packet
}

// The connection's send window (0x006aa140): no data packet more than 30
// ahead of the last the peer acknowledged.
TEST(NetWire_SendWindow)
{
	net::GameConnection connection;
	connection.beginReplay();
	// Accepted, then 30 data packets unacknowledged.
	net::BitWriter accept;
	accept.write(2, 4);
	accept.write(0, 8);
	accept.write(7, 8);
	accept.write(0, 32);
	accept.write(0, 1);
	CHECK(connection.inject(accept.data(), {}) && connection.connected());
	int sent = 0;
	for (int i = 0; i < 40; ++i) {
		sent += connection.sendData({0}) ? 1 : 0;
	}
	CHECK(sent == 30 && !connection.canSendData());
	// The server's data packet acknowledging our 30th.
	net::NotifyState theirs;
	theirs.lastReceived = 30;
	theirs.receivedMask = 1;
	CHECK(connection.inject(net::dataPacket(7, 1, theirs, {0}), {}));
	CHECK(connection.canSendData() && connection.notify().lastAcknowledged == 30);
	// The same packet again is stale: dropped, and its acknowledgement too.
	theirs.lastReceived = 2;
	connection.inject(net::dataPacket(7, 1, theirs, {0}), {});
	CHECK(connection.notify().lastAcknowledged == 30);
}

namespace {

dice::hfe::world::ObjectTemplate* make(dice::hfe::world::ObjectTemplateManager& templates, const char* type, const char* name,
	const char* info = "SimpleInfo")
{
	dice::hfe::world::ObjectTemplate* t = templates.create(type, name);
	if (info) {
		t->settings()["networkableInfo"] = info;
	}
	return t;
}

void child(dice::hfe::world::ObjectTemplate* parent, const char* name)
{
	dice::hfe::world::ChildTemplate c;
	c.name = name;
	parent->children().push_back(c);
}

} // namespace

// The layouts as 0x0067c690 builds them: Wings of different templates with
// the same rotation settings share a group (0x00678070); an effect bundle
// (never made on a dedicated server) is left out; a tree whose root has no
// networkable has no ghosts; an Engine without numberOfGears has one gear
// (0x006291d0) and a seat's occupant takes the level's soldier inventory.
TEST(NetWire_TemplateLayouts)
{
	dice::hfe::world::ObjectTemplateManager templates;
	int type = 1;
	for (const char* t : {"PlayerControlObject", "Wing", "EffectBundle", "Engine", "Soldier", "Camera"}) {
		templates.registerType(type++, t);
	}
	dice::hfe::world::ObjectTemplate* soldier = make(templates, "Soldier", "trooper", nullptr);
	soldier->settings()["inventorySize"] = "10";
	dice::hfe::world::ObjectTemplate* plane = make(templates, "PlayerControlObject", "plane");
	for (const char* name : {"wing_l", "wing_r"}) {
		dice::hfe::world::ObjectTemplate* w = make(templates, "Wing", name);
		w->settings()["inputToRoll"] = "c_PIRoll";
		child(plane, name);
	}
	make(templates, "EffectBundle", "smoke");
	child(plane, "smoke");
	make(templates, "Engine", "jet");
	child(plane, "jet");
	make(templates, "PlayerControlObject", "gunner");
	child(plane, "gunner");
	const std::vector<net::GhostPartLayout> layout = net::buildGhostLayout(net::ghostTemplateFrom(templates, *plane));
	CHECK(layout.size() == 2);
	CHECK(layout[0].groups.size() == 3); // the root, both wings, the engine; no smoke
	CHECK(layout[0].groups[1].members.size() == 2);
	CHECK(layout[0].groups[2].hints.kind == net::NetworkableKind::Engine && layout[0].groups[2].hints.engineGears == 1);
	CHECK(layout[1].groups.size() == 1 && layout[1].groups[0].hints.soldierItemSlots == 10);
	dice::hfe::world::ObjectTemplate* bare = make(templates, "PlayerControlObject", "as_drone", nullptr);
	child(bare, "gunner");
	CHECK(net::buildGhostLayout(net::ghostTemplateFrom(templates, *bare)).empty());
	// Without inventorySize a Soldier has 6 (0x005c0a50).
	dice::hfe::world::ObjectTemplate* recruit = make(templates, "Soldier", "recruit", nullptr);
	CHECK(net::networkableHintsFrom(templates, *recruit, false).soldierItemSlots == 6);
}

// The ticket state rides with the tickets (team +0x8c, the value after the
// tickets' bit), which a joined client announces as it changes.
TEST(NetWire_TeamStateCarriesTheTicketState)
{
	net::TeamNetState state;
	state.setTickets(1, 87, false, 10);
	state.setTickets(2, 40, false, 5);
	net::BitWriter out;
	net::encodeTeamState(out, state);
	net::BitReader in(out.data().data(), out.data().size());
	in.limit(out.bits());
	net::TeamNetState back;
	CHECK(net::decodeTeamState(in, back));
	CHECK(back.tickets(1) == 87 && back.ticketState(1) == 10);
	CHECK(back.tickets(2) == 40 && back.ticketState(2) == 5);
}
