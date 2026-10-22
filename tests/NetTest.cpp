// The server browser: GameSpy's cipher, the master server's host, and a
// list reply built the way the master builds one.
#include "Net/BitStream.h"
#include "Net/Fesl.h"
#include "Net/GameConnection.h"
#include "Net/GameEvents.h"
#include "Net/Md5.h"
#include "Net/GameSpyCrypt.h"
#include "Net/ServerBrowser.h"
#include "TestMain.h"

#include <cstring>

using namespace dice::hfe;

TEST(Net_MasterServerHost)
{
	// OpenSpy answers BF2142's lists at stella.ms5.
	CHECK(net::masterServerHost("stella") == "stella.ms5.openspy.net");
}

TEST(Net_CryptRoundTrip)
{
	const uint8_t key[8] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
	net::GameSpyCrypt out(key, 8), in(key, 8);
	const char* text = "\\hostname\\Server\\mapname\\Minsk";
	bool same = true, changed = false;
	for (size_t i = 0; text[i]; ++i) {
		const uint8_t c = out.encrypt(static_cast<uint8_t>(text[i]));
		changed = changed || c != static_cast<uint8_t>(text[i]);
		same = same && in.decrypt(c) == static_cast<uint8_t>(text[i]);
	}
	CHECK(same);
	CHECK(changed);
}

TEST(Net_ParsesListReply)
{
	const std::array<uint8_t, 8> challenge {'0', '1', '2', '3', '4', '5', '6', '7'};
	// The plain list: our IP, the default port, two fields (a string and a
	// byte), one popular value, two servers, the end.
	std::vector<uint8_t> plain {5, 6, 7, 8, 0x70, 0xec};
	plain.push_back(2);
	for (const char* f : {"\0hostname", "\1numplayers"}) {
		plain.push_back(static_cast<uint8_t>(f[0]));
		plain.insert(plain.end(), f + 1, f + std::strlen(f + 1) + 1);
		plain.push_back(0);
	}
	plain.push_back(1);
	const char* popular = "Popular";
	plain.insert(plain.end(), popular, popular + std::strlen(popular) + 1);
	// Server 1: keys, default port, its name inline.
	plain.insert(plain.end(), {0x40, 10, 0, 0, 1, 0xff});
	const char* name = "Inline";
	plain.insert(plain.end(), name, name + std::strlen(name) + 1);
	plain.push_back(12);
	// Server 2: keys, its own port, the popular name.
	plain.insert(plain.end(), {0x50, 10, 0, 0, 2, 0x71, 0x00, 0x00, 3});
	plain.insert(plain.end(), {0x00, 0xff, 0xff, 0xff, 0xff});

	// The header: 0xec ^ (length - 2), the server challenge's length ^ 0xea.
	const std::vector<uint8_t> serverChallenge {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 11, 12, 13, 14};
	std::vector<uint8_t> reply {static_cast<uint8_t>(0xec ^ 2), 0, static_cast<uint8_t>(serverChallenge.size() ^ 0xea)};
	reply[0] = static_cast<uint8_t>(0xec ^ (3 - 2));
	reply.insert(reply.end(), serverChallenge.begin(), serverChallenge.end());
	std::array<uint8_t, 8> key = challenge;
	const char* secret = net::kSecretKey;
	for (size_t i = 0; i < serverChallenge.size(); ++i) {
		const size_t at = static_cast<size_t>(static_cast<int>(static_cast<signed char>(secret[i % 6])) * static_cast<int>(i)) & 7;
		key[at] = static_cast<uint8_t>(key[at] ^ key[i & 7] ^ serverChallenge[i]);
	}
	net::GameSpyCrypt crypt(key.data(), key.size());
	for (uint8_t b : plain) {
		reply.push_back(crypt.encrypt(b));
	}

	std::string error;
	const auto list = net::parseListReply(reply, challenge, {}, net::kSecretKey, &error);
	CHECK(list.has_value());
	if (!list) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return;
	}
	CHECK(list->publicIp == "5.6.7.8" && list->defaultPort == 28908);
	CHECK(list->servers.size() == 2);
	if (list->servers.size() == 2) {
		CHECK(list->servers[0].ip == "10.0.0.1" && list->servers[0].port == 28908);
		CHECK(list->servers[0].fields.at("hostname") == "Inline" && list->servers[0].fields.at("numplayers") == "12");
		CHECK(list->servers[1].port == 0x7100 && list->servers[1].fields.at("hostname") == "Popular");
	}
	// A cut-off reply is incomplete, not wrong.
	reply.resize(reply.size() - 3);
	CHECK(!net::parseListReply(reply, challenge, {}, net::kSecretKey, &error) && error == "incomplete");
}

TEST(Net_BitStreamIsLsbFirst)
{
	net::BitWriter out;
	out.write(0xf, 4);  // packet type 15
	out.write(0x01, 8); // connection 1
	out.write(0x2a, 6);
	CHECK(out.data().size() == 3);
	CHECK(out.data()[0] == 0x1f && out.data()[1] == 0xa0 && out.data()[2] == 0x02);
	net::BitReader in(out.data().data(), out.data().size());
	CHECK(in.read(4) == 15 && in.read(8) == 1 && in.read(6) == 0x2a);
	uint32_t past = 0;
	CHECK(!in.read(past, 32) && in.bad());
}

TEST(Net_ConnectPacketLayout)
{
	net::ConnectRequest request;
	request.password = "secret";
	const auto packet = net::connectPacket(request);
	// 4 + 8 + 32 + 32 + 1 + 32 + 256 + 256 bits.
	CHECK(packet.size() == (621 + 7) / 8);
	net::BitReader in(packet.data(), packet.size());
	CHECK(in.read(4) == 1 && in.read(8) == 0);
	CHECK(in.read(32) == 0x1002 && in.read(32) == 0x1a007000 && in.read(1) == 0);
	in.read(32);
	CHECK(in.readText(32) == "secret" && in.readText(32) == "mods/bf2142");
}

TEST(Net_ConnectReplies)
{
	net::BitWriter accept;
	accept.write(2, 4);
	accept.write(0, 8);
	accept.write(5, 8);
	accept.write(123456, 32);
	accept.write(1, 1);
	const auto ok = net::readConnectReply(accept.data());
	CHECK(ok.outcome == net::ConnectResult::Outcome::Accepted && ok.connection == 5 && ok.serverClock == 123456);
	net::BitWriter refuse;
	refuse.write(3, 4);
	refuse.write(0, 8);
	refuse.write(0x11, 32);
	refuse.write(1, 1);
	refuse.writeText("Bad password", 32);
	const auto no = net::readConnectReply(refuse.data());
	CHECK(no.outcome == net::ConnectResult::Outcome::Refused && no.reason == 0x11 && no.message == "Bad password");
	CHECK(std::string(net::refusalReason(0x11)) == "wrong password");
}

TEST(Net_NotifySequences)
{
	net::NotifyState state;
	CHECK(state.receive(1, true) && state.lastReceived == 1 && state.receivedMask == 1);
	CHECK(!state.receive(1, true));         // repeated
	CHECK(state.receive(4, true));          // 2 and 3 missed
	CHECK(state.receivedMask == 0x9);       // 1 (three back) and 4
	CHECK(!state.receive(40, true));        // too far ahead
	CHECK(state.receive(5, false) && state.receivedMask == 0x12);
	// A pong carries our header and both clocks.
	const auto pong = net::pongPacket(3, state, 1000, 2000);
	net::ConnectedPacket packet;
	CHECK(net::readConnectedPacket(pong, packet));
	CHECK(static_cast<int>(packet.type) == 8 && packet.acknowledged == 5 && packet.mask == 0x12);
	CHECK(!packet.sequenced && packet.time == 1000 && packet.echo == 2000);
	// A ping (0x006a9bb0) is sequenced once we have sent data, as a pong is.
	CHECK(net::readConnectedPacket(net::pingPacket(3, state, 777), packet));
	CHECK(static_cast<int>(packet.type) == 7 && !packet.sequenced && packet.time == 777);
	state.sendSequence = 9;
	CHECK(net::readConnectedPacket(net::pingPacket(3, state, 778), packet));
	CHECK(packet.sequenced && packet.sequence == 9 && packet.time == 778);
}

TEST(Net_Md5)
{
	CHECK(net::md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
	CHECK(net::md5Hex("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6");
}

TEST(Net_CdKeyResponse)
{
	const std::string r = net::cdKeyResponse("KEY", "abcdefghi", 0x12345678);
	CHECK(r.size() == 72);
	CHECK(r.substr(0, 32) == net::md5Hex("KEY"));
	CHECK(r.substr(32, 8) == "12345678");
	CHECK(r.substr(40) == net::md5Hex("KEY" + std::to_string(0x12345678u % 0xffff) + "abcdefghi"));
}

TEST(Net_EventBlocks)
{
	// A challenge as a live server sent it (the payload after its length).
	const std::vector<uint8_t> challenge {0x06, 0x00, 0x01, 0x3c, 0xbd, 0x38, 0x38, 0xb9, 0x35, 0x39, 0x39, 0x34, 0x00, 0x00};
	const net::EventBlock block = net::readEventBlock(challenge);
	CHECK(!block.inputPresent && block.present && block.sequence == 0 && block.complete);
	CHECK(block.events.size() == 1);
	if (!block.events.empty()) {
		CHECK(block.events[0].type == net::kCdKeyChallenge && block.events[0].challenge == "xzqprkrrh");
	}
	// Our answer: event 2 with the response, the mixed value, the build and the profile.
	const auto payload = net::cdKeyResponsePayload(3, std::string(72, 'a'), 7, 0xffffffff);
	net::BitReader in(payload.data(), payload.size());
	CHECK(in.read(1) == 0 && in.read(1) == 1 && in.read(8) == 1 && in.read(5) == 3 && in.read(1) == 0);
	CHECK(in.read(7) == net::kCdKeyResponse);
	CHECK(in.readText(73) == std::string(72, 'a'));
	CHECK(in.read(32) == 7 && in.read(32) == net::kBuild && in.read(32) == 0xffffffff);
}

TEST(Net_FeslMessages)
{
	// Encoding: big-endian type, id and whole length, lines, a closing NUL.
	const net::FeslMessage hello {net::kFeslSystem, 0xc0000001, {{"TXN", "Hello"}, {"TID", "1"}}};
	const auto bytes = net::encodeFesl(hello);
	CHECK(bytes.size() == 12 + 16 + 1);
	CHECK(std::memcmp(bytes.data(), "fsys\xc0\x00\x00\x01\x00\x00\x00\x1d", 12) == 0);
	CHECK(std::string(bytes.begin() + 12, bytes.end() - 1) == "TXN=Hello\nTID=1\n" && bytes.back() == 0);

	// Decoding a reply as OpenSpy sends it, split across reads, followed by
	// a second message.
	std::vector<uint8_t> stream = net::encodeFesl({net::kFeslAccount, 0x80000002,
		{{"TXN", "GetSubAccounts"}, {"TID", "3"}, {"subAccounts.[]", "2"}, {"subAccounts.0", "\"Sol%20One\""},
			{"subAccounts.1", "\"Two\""}}});
	const auto ping = net::encodeFesl({net::kFeslSystem, 0x80000000, {{"TXN", "Ping"}}});
	stream.insert(stream.end(), ping.begin(), ping.end());
	std::vector<uint8_t> buffer(stream.begin(), stream.begin() + 20);
	CHECK(net::takeFeslMessages(buffer).empty() && buffer.size() == 20);
	buffer.insert(buffer.end(), stream.begin() + 20, stream.end());
	const auto messages = net::takeFeslMessages(buffer);
	CHECK(messages.size() == 2 && buffer.empty());
	if (messages.size() == 2) {
		CHECK(messages[0].type == net::kFeslAccount && messages[0].txn() == "GetSubAccounts" && messages[0].number("TID") == 3);
		const auto soldiers = messages[0].list("subAccounts");
		CHECK(soldiers.size() == 2 && soldiers[0] == "Sol One" && soldiers[1] == "Two");
		CHECK(messages[1].txn() == "Ping" && !messages[1].isError());
	}
	CHECK(net::feslUnquote("\"Oct-21-2026%2012%3a00%3a00%20UTC\"") == "Oct-21-2026 12:00:00 UTC");

	bool bad = false;
	std::vector<uint8_t> junk {'f', 's', 'y', 's', 0, 0, 0, 0, 0, 0, 0, 3};
	CHECK(net::takeFeslMessages(junk, &bad).empty() && bad);
}
