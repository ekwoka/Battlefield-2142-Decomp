// The game protocol after the handshake (GameProtocol, RemoteWorld,
// GameClient), tested offline: layouts round-trip, and a loopback server
// built from the same decoded formats plays a short session with the
// client over UDP on 127.0.0.1.
#include "TestMain.h"

#include "Console/Console.h"
#include "IO/File/FileManager.h"
#include "Net/BitStream.h"
#include "Net/GameClient.h"
#include "Net/GameConnection.h"
#include "Net/GameProtocol.h"
#include "Net/GhostLayout.h"
#include "Net/Md5.h"
#include "Net/RemoteInput.h"
#include "Net/RemoteWorld.h"
#include "Net/Socket.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <thread>

namespace net = dice::hfe::net;

namespace {

bool sameBits(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t bits)
{
	for (size_t i = 0; i < bits; ++i) {
		const bool x = i / 8 < a.size() && (a[i / 8] >> (i % 8) & 1);
		const bool y = i / 8 < b.size() && (b[i / 8] >> (i % 8) & 1);
		if (x != y) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST(NetGame_InputBlockLayout)
{
	net::PlayerInput input;
	input.set(net::kPIThrottle, 1.0f);
	input.set(net::kPIYaw, -0.5f);
	input.set(net::kPIFire, 1.0f);
	input.set(net::kPICrouch, 1.0f);
	input.set(net::kPINextItem, 1.0f); // id 48: the high trigger word
	const net::PlayerMove move = input.toMove(41);
	CHECK(move.axes[net::kPIThrottle] == 100 && move.axes[net::kPIYaw] == -50);
	CHECK(move.trigger(net::kPIFire) && move.trigger(net::kPICrouch) && move.trigger(net::kPINextItem));
	CHECK(!move.trigger(net::kPIUse));
	CHECK(move.triggersLow == (1u << 0 | 1u << 30) && move.triggersHigh == 1u << 8);

	net::InputBlock block;
	block.present = true;
	block.value9 = 300;
	block.moves = {move, input.toMove(42)};
	net::BitWriter out;
	net::writeInputBlock(out, block);
	// 1 + 4 + 9 + 32 + 2 x (6 x 16 + 32 + 11 + 1) bits.
	CHECK(out.bits() == 1 + 4 + 9 + 32 + 2 * 140);
	net::BitReader in(out.data().data(), out.data().size());
	CHECK(in.read(1) == 1 && in.read(4) == 2 && in.read(9) == 300);
	CHECK(in.readSigned(32) == 41);
	CHECK(in.read(1) == 1 && in.read(15) == 50); // yaw -0.5
	net::BitReader again(out.data().data(), out.data().size());
	net::InputBlock read;
	CHECK(net::readInputBlock(again, read) && read.present && read.moves.size() == 2);
	if (read.moves.size() == 2) {
		CHECK(read.moves[1].index == 42 && read.moves[1].axes == move.axes);
		CHECK(read.moves[0].triggersLow == move.triggersLow && read.moves[0].triggersHigh == move.triggersHigh);
	}
}

TEST(NetGame_EveryEventLayoutRoundTrips)
{
	// Each registered type read from random bits writes back the same bits.
	std::mt19937 random(1234);
	int decoded = 0;
	for (uint32_t type = 0; type < net::kEventTypeCount + 2; ++type) {
		net::BitWriter source;
		source.write(type, 7);
		for (int i = 0; i < 256; ++i) {
			source.write(random(), 32);
		}
		net::BitReader in(source.data().data(), source.data().size());
		net::GameEvent event;
		const bool ok = net::readGameEvent(in, event);
		CHECK(ok == (net::gameEventName(type) != nullptr));
		if (!ok) {
			continue;
		}
		++decoded;
		net::BitWriter back;
		CHECK(net::writeGameEvent(back, event));
		CHECK(back.bits() == in.position());
		CHECK(sameBits(back.data(), source.data(), in.position()));
	}
	CHECK(decoded == 69);
	CHECK(std::string(net::gameEventName(net::kEvCreateObject)) == "CreateObject");
	CHECK(net::gameEventName(0x12) == nullptr && net::gameEventName(0x35) == nullptr);
}

TEST(NetGame_LiveChallengePayload)
{
	// The challenge a live server sent (the payload after its length): no
	// input, one event block, no ghosts.
	const std::vector<uint8_t> challenge {0x06, 0x00, 0x01, 0x3c, 0xbd, 0x38, 0x38, 0xb9, 0x35, 0x39, 0x39, 0x34, 0x00, 0x00};
	net::GamePayload payload;
	CHECK(net::readGamePayload(challenge, payload) && payload.complete);
	CHECK(!payload.input.present && payload.hasEvents && payload.events.sequence == 0 && !payload.ghosts.present);
	CHECK(payload.events.events.size() == 1);
	if (!payload.events.events.empty()) {
		const net::GameEvent& e = payload.events.events[0];
		CHECK(e.type == net::kEvChallenge && e.text(0).substr(0, 9) == "xzqprkrrh");
	}
}

TEST(NetGame_TypedEvents)
{
	net::CreateObjectInfo object;
	object.templateId = 1234;
	object.ghost = 77;
	object.team = 2;
	object.hasPosition = true;
	object.position = {1.5f, -2.0f, 300.25f};
	net::BitWriter out;
	CHECK(net::writeGameEvent(out, net::makeCreateObject(object)));
	// 7 + 32 + 16 + 2 + 1 + 1 + 96 + 1 bits.
	CHECK(out.bits() == 7 + 32 + 16 + 2 + 1 + 1 + 96 + 1);
	net::BitReader in(out.data().data(), out.data().size());
	net::GameEvent e;
	net::CreateObjectInfo back;
	CHECK(net::readGameEvent(in, e) && net::parseCreateObject(e, back));
	CHECK(back.templateId == 1234 && back.ghost == 77 && back.team == 2 && back.hasPosition && !back.hasRotation);
	CHECK(back.position.z == 300.25f);

	net::CreatePlayerInfo player;
	player.id = 9;
	player.team = 1;
	player.name = "Sol";
	player.profileId = -1;
	net::CreatePlayerInfo p;
	CHECK(net::parseCreatePlayer(net::makeCreatePlayer(player), p) && p.id == 9 && p.name == "Sol" && !p.remote && !p.ai);
	// 0x0050a7c0: team 3, the spawn group 4, remote 1, id 8, object 16,
	// 16, then the bot bit (player vtable 0x68).
	player.remote = true;
	player.ai = true;
	player.spawnGroup = 5;
	{
		net::BitWriter w;
		CHECK(net::writeGameEvent(w, net::makeCreatePlayer(player)));
		net::BitReader r(w.data().data(), w.data().size());
		CHECK(r.read(7) == net::kEvCreatePlayer && r.read(3) == 1 && r.read(4) == 5 && r.read(1) == 1 && r.read(8) == 9);
		r.read(32);
		CHECK(r.read(1) == 1);
		net::BitReader again(w.data().data(), w.data().size());
		net::GameEvent ev;
		CHECK(net::readGameEvent(again, ev) && net::parseCreatePlayer(ev, p) && p.ai && p.remote && p.spawnGroup == 5);
	}

	// KilledBy as 0x0050d750 writes it: the attacker, the victim, has
	// attacker, an id, two template values, the armor byte, the position
	// (mode 0: three floats), the damage in 10 bits.
	{
		net::BitWriter w;
		w.write(net::kEvKilledBy, 7);
		w.write(5, 8);
		w.write(2, 8);
		w.write(1, 1);
		w.write(300, 16);
		w.write(0x1111, 32);
		w.write(0x2222, 32);
		w.write(7, 8);
		w.write(0, 2);
		for (float f : {10.0f, 20.0f, -30.0f}) {
			uint32_t bits;
			std::memcpy(&bits, &f, 4);
			w.write(bits, 32);
		}
		w.write(1023, 10);
		net::BitReader r(w.data().data(), w.data().size());
		net::GameEvent ev;
		net::KilledByInfo k;
		CHECK(net::readGameEvent(r, ev) && net::parseKilledBy(ev, k));
		CHECK(k.attacker == 5 && k.victim == 2 && k.hasAttacker && k.object == 300 && k.value1 == 0x1111 && k.value2 == 0x2222);
		CHECK(k.armorValue == 7 && k.position.x == 10.0f && k.position.z == -30.0f && std::fabs(k.damage - 100.0f) < 0.01f);
	}

	// The move's last bit (0x00590650): pitch and roll from a device axis.
	{
		net::PlayerInput input;
		CHECK(input.toMove(1).axisPitchRoll);
		input.axisPitchRoll = false;
		CHECK(!input.toMove(1).axisPitchRoll);
	}
	// Connection types (0x00928408, 0x004b3df0): 10 packets a second for
	// type 0, a tick's 30 for the others.
	CHECK(net::clientSendRate(0) == 10 && net::clientSendRate(1) == 30 && net::clientSendRate(4) == 30);
	CHECK(net::kConnectionRates[5].serverMaxBytes == 640 && net::kConnectionRates[2].serverPacketsPerSecond == 10);

	net::PostRemoteInfo post;
	CHECK(net::parsePostRemote(net::makePostRemote(6, net::kNeSelectSpawnGroup, net::int32Payload(4)), post));
	CHECK(post.category == 6 && post.id == 6 && post.payloadInt() == 4);

	net::ChatMessage chat;
	chat.channel = 1;
	chat.player = 4;
	chat.text = "hello there";
	const std::string block = net::encodeChat(chat);
	CHECK(block.size() == 11 + 12);
	net::ChatMessage decoded;
	CHECK(net::decodeChat(block, decoded) && decoded.text == "hello there" && decoded.channel == 1
		&& decoded.source == 2 && decoded.player == 4);
	const auto events = net::stringBlockEvents(std::string(100, 'x'));
	CHECK(events.size() == 3 && events[0].u(0) == 1 && events[0].u(1) == 100 && events[2].text(0).size() == 37);

	const net::GameEvent kit = net::makeSelectKit(3, {5, -1, -1, 2});
	CHECK(kit.u(0) == 3 && kit.u(1) == 6 && kit.u(2) == 0 && kit.u(4) == 3);
}

TEST(NetGame_GhostBlockRoundTrip)
{
	net::GhostBlock ghosts;
	ghosts.present = true;
	ghosts.tick = 9000;
	net::ControlObjectRecord control;
	control.lastMove = 55;
	control.position = {100, 50, -25};
	control.ghost = 12;
	control.vehicle = 40;
	control.rest = {0xab, 0x01};
	control.restBits = 9;
	ghosts.control = control;
	net::GhostUpdate update;
	update.kind = 1;
	update.ghost = 300;
	update.full = true;
	update.state = {0xff, 0x0f, 0x03};
	update.stateBits = 18;
	ghosts.updates.push_back(update);
	net::GhostUpdate removal;
	removal.kind = 3;
	removal.ghost = 301;
	ghosts.updates.push_back(removal);

	net::BitWriter out;
	net::writeGhostBlock(out, ghosts);
	net::BitReader in(out.data().data(), out.data().size());
	net::GhostBlock back;
	CHECK(net::readGhostBlock(in, back) && back.present && back.tick == 9000 && back.control);
	if (back.control) {
		CHECK(back.control->lastMove == 55 && back.control->ghost == 12 && back.control->vehicle == 40);
		CHECK(back.control->position.x == 100 && back.control->restBits == 9 && back.control->rest[0] == 0xab);
	}
	CHECK(back.updates.size() == 2);
	if (back.updates.size() == 2) {
		CHECK(back.updates[0].kind == 1 && back.updates[0].ghost == 300 && back.updates[0].stateBits == 18);
		CHECK(back.updates[0].state[2] == 0x03 && back.updates[1].kind == 3 && back.updates[1].ghost == 301);
	}
	CHECK(in.position() == out.bits());

	// The state length: 11 bits on a live connection (0x00582a50 with kind
	// 0), 13 in demos.
	net::GhostBlock big;
	big.present = true;
	net::GhostUpdate full;
	full.kind = 1;
	full.ghost = 5;
	full.state.assign(256, 0x5a);
	full.stateBits = 2047;
	big.updates.push_back(full);
	for (unsigned bits : {net::kGhostStateLengthBits, net::kDemoGhostStateLengthBits}) {
		net::BitWriter w;
		net::writeGhostBlock(w, big, bits);
		CHECK(w.bits() == 1 + 32 + 8 + 1 + 2 + 16 + 1 + bits + 2047);
		net::BitReader r(w.data().data(), w.data().size());
		net::GhostBlock b;
		CHECK(net::readGhostBlock(r, b, bits) && b.updates.size() == 1 && b.updates[0].stateBits == 2047);
	}
	CHECK(net::kGhostStateLengthBits == 11);
}

TEST(NetGame_RemoteWorldFromEvents)
{
	net::RemoteWorld world;
	world.setTemplateResolver([](uint32_t id) { return id == 7 ? std::string("us_light_tank") : std::string(); });
	net::CreatePlayerInfo me;
	me.id = 2;
	me.team = 1;
	me.name = "Me";
	world.apply(net::makeCreatePlayer(me));
	net::CreatePlayerInfo other;
	other.id = 5;
	other.team = 2;
	other.remote = true;
	other.name = "Other";
	world.apply(net::makeCreatePlayer(other));
	CHECK(world.localPlayer() == 2 && world.playerList().size() == 2);
	// HandlePickup (0x0050bfd0): player, the kit, the soldier (0x004bb350
	// asks the third for the kit holder interface 0xc49d).
	{
		net::GameEvent pickup;
		pickup.type = net::kEvHandlePickup;
		pickup.values = {2, 50, 60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
		world.apply(pickup);
		CHECK(world.player(2)->soldierGhost == 60 && world.player(2)->alive);
	}

	net::CreateObjectInfo tank;
	tank.templateId = 7;
	tank.ghost = 40;
	tank.hasPosition = true;
	tank.position = {1, 2, 3};
	world.apply(net::makeCreateObject(tank));
	net::CreateObjectInfo crowd;
	crowd.templateId = 9;
	crowd.ghost = 100;
	crowd.count = 3;
	world.apply(net::makeCreateObject(crowd));
	CHECK(world.objectList().size() == 4);
	CHECK(world.object(40) && world.object(40)->templateName == "us_light_tank" && world.object(102));

	net::GameEvent killed;
	killed.type = net::kEvPythonCommand;
	killed.values = {100, 0, 3, 2, 2, 5}; // type 100, (kind 2, victim 2, attacker 5)
	world.apply(killed);
	CHECK(world.player(2)->deaths == 1 && world.player(5)->kills == 1 && world.takeKills().size() == 1);

	net::GhostBlock ghosts;
	ghosts.present = true;
	ghosts.tick = 30;
	net::ControlObjectRecord control;
	control.ghost = 40;
	control.position = {9, 8, 7};
	ghosts.control = control;
	net::GhostUpdate removal;
	removal.kind = 3;
	removal.ghost = 101;
	ghosts.updates.push_back(removal);
	world.apply(ghosts);
	CHECK(world.object(40)->position.x == 9 && !world.object(101) && world.serverTick() == 30);

	world.apply(net::makePostRemote(0, 1, net::int32Payload(1)));
	CHECK(world.gameState() == 1);
	net::GameEvent destroy;
	destroy.type = net::kEvDestroyObject;
	destroy.values = {40, 0};
	world.apply(destroy);
	CHECK(!world.object(40));
}

TEST(NetGame_SoldierInputMapping)
{
	dice::hfe::game::SoldierInput soldier;
	soldier.throttle = 1;
	soldier.fire = true;
	soldier.pose = dice::hfe::game::SoldierPose::Prone;
	soldier.selectItem = 2;
	const net::PlayerMove move = net::playerInputFrom(soldier).toMove(0);
	CHECK(move.axes[net::kPIThrottle] == 100 && move.trigger(net::kPIFire) && move.trigger(net::kPILie));
	CHECK(move.trigger(net::kPIWeaponSelect1 + 1) && !move.trigger(net::kPICrouch));
}

namespace {

// A game server speaking the decoded protocol to one client on 127.0.0.1.
class LoopbackServer {
public:
	bool start() { return m_socket.bind(0); }
	uint16_t port() const { return m_socket.port(); }

	// Handles what has arrived.
	void poll()
	{
		std::vector<uint8_t> datagram;
		net::UdpServerSocket::Address from;
		while (m_socket.receiveFrom(datagram, from, 0) > 0) {
			handle(datagram, from);
		}
	}

	// Sends a data packet with these events (one reliable block) and ghosts.
	void send(const std::vector<net::GameEvent>& events, const net::GhostBlock& ghosts = {})
	{
		net::GamePayload payload;
		if (!events.empty()) {
			payload.hasEvents = true;
			payload.events.sequence = m_blockSequence++ & 0x1f;
			payload.events.events = events;
		}
		payload.ghosts = ghosts;
		const std::vector<uint8_t> bytes = net::writeGamePayload(payload);
		m_sendSequence = (m_sendSequence + 1) & 0x3f;
		m_socket.sendTo(m_client, net::dataPacket(kConnection, m_sendSequence, m_notify, bytes));
	}

	bool accepted = false;
	int dropData = 0; // client data packets to lose
	std::vector<net::GameEvent> received;
	std::vector<net::InputBlock> inputs;
	int resentBlocks = 0;

private:
	static constexpr uint8_t kConnection = 7;

	void handle(const std::vector<uint8_t>& datagram, const net::UdpServerSocket::Address& from)
	{
		net::BitReader in(datagram.data(), datagram.size());
		const uint32_t type = in.read(4);
		in.read(8);
		if (type == 1) { // connect (checked as 0x00689d30 does: protocol and build)
			if (in.read(32) != net::kProtocol || in.read(32) != net::kBuild) {
				return;
			}
			m_client = from;
			net::BitWriter accept;
			accept.write(2, 4);
			accept.write(0, 8);
			accept.write(kConnection, 8);
			accept.write(123, 32);
			accept.write(0, 1);
			m_socket.sendTo(from, accept.data());
			return;
		}
		if (type == 4) {
			accepted = true;
			// The CD-key challenge (0x00509cc0).
			net::GameEvent challenge;
			challenge.type = net::kEvChallenge;
			challenge.bytes = {std::string("abcdefghi") + '\0', ""};
			send({challenge});
			return;
		}
		if (type == 7) {
			net::ConnectedPacket ping;
			net::readConnectedPacket(datagram, ping);
			m_socket.sendTo(from, net::pongPacket(kConnection, m_notify, ping.time, 999));
			return;
		}
		if (type != 15) {
			return;
		}
		net::ConnectedPacket packet;
		if (!net::readConnectedPacket(datagram, packet)) {
			return;
		}
		if (dropData > 0) {
			--dropData;
			return; // lost on the way: no notify, no events
		}
		if (!m_notify.receive(packet.sequence, true)) {
			return;
		}
		const size_t length = packet.payload[0] | packet.payload[1] << 8;
		const std::vector<uint8_t> payload(packet.payload.begin() + 2, packet.payload.begin() + 2 + length);
		net::GamePayload decoded;
		if (!net::readGamePayload(payload, decoded) || !decoded.complete) {
			return;
		}
		inputs.push_back(decoded.input);
		if (decoded.hasEvents) {
			resentBlocks += decoded.events.resent ? 1 : 0;
			if (decoded.events.sequence == m_nextBlock) {
				m_nextBlock = (m_nextBlock + 1) & 0x1f;
				received.insert(received.end(), decoded.events.events.begin(), decoded.events.events.end());
			}
		}
	}

	net::UdpServerSocket m_socket;
	net::UdpServerSocket::Address m_client;
	net::NotifyState m_notify;
	uint32_t m_sendSequence = 0;
	uint32_t m_blockSequence = 0;
	uint32_t m_nextBlock = 0;
};

bool runUntil(net::GameClient& client, LoopbackServer& server, const std::function<bool()>& done, int milliseconds = 3000)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	while (std::chrono::steady_clock::now() < deadline) {
		client.update();
		server.poll();
		if (done()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return false;
}

size_t countType(const std::vector<net::GameEvent>& events, uint32_t type)
{
	size_t n = 0;
	for (const net::GameEvent& e : events) {
		n += e.type == type ? 1 : 0;
	}
	return n;
}

} // namespace

TEST(NetGame_LoopbackSession)
{
	LoopbackServer server;
	CHECK(server.start());
	net::GameClient client;
	net::GameClientOptions options;
	options.cdKey = "TEST-KEY";
	CHECK(client.connect("127.0.0.1", server.port(), options));

	// Connect, get the challenge, answer it with the response and the connection type.
	CHECK(runUntil(client, server, [&] { return countType(server.received, net::kEvConnectionType) == 1; }));
	CHECK(client.state() == net::GameClient::State::Connected && server.accepted);
	CHECK(countType(server.received, net::kEvChallengeResponse) == 1);
	if (!server.received.empty() && server.received[0].type == net::kEvChallengeResponse) {
		const net::GameEvent& r = server.received[0];
		CHECK(r.text(0).size() == 73 && r.u(1) == net::kBuild && r.u(2) == 0xffffffff);
		const std::string response = r.text(0).substr(0, 72);
		CHECK(response.substr(0, 32) == net::md5Hex("TEST-KEY"));
	}

	// The level's state: settings, players, objects, a kill, a chat line, ghosts.
	std::vector<net::GameEvent> events;
	// Laid out as a live server sent them: the settings start with the
	// server's name; the map list is a count and (name, game mode, size).
	const char rawSettings[] = "\x0b\0Test Server\x01\0\0\0";
	const std::string settings(rawSettings, sizeof(rawSettings) - 1);
	events.push_back(net::makeDataBlockStart(0, static_cast<uint32_t>(settings.size())));
	events.push_back(net::makeDataBlockChunk(settings));
	const char rawMaps[] = "\x02\0\0\0\x0a\0suez_canal\x06\0gpm_cq\x40\0\x05\0minsk\x06\0gpm_cq\x20\0";
	const std::string maps(rawMaps, sizeof(rawMaps) - 1);
	events.push_back(net::makeDataBlockStart(5, static_cast<uint32_t>(maps.size())));
	events.push_back(net::makeDataBlockChunk(maps.substr(0, 20)));
	events.push_back(net::makeDataBlockChunk(maps.substr(20)));
	net::CreatePlayerInfo me;
	me.id = 3;
	me.team = 1;
	me.name = "Tester";
	events.push_back(net::makeCreatePlayer(me));
	net::CreatePlayerInfo enemy;
	enemy.id = 5;
	enemy.team = 2;
	enemy.remote = true;
	enemy.name = "Enemy";
	events.push_back(net::makeCreatePlayer(enemy));
	net::CreateObjectInfo soldier;
	soldier.templateId = 11;
	soldier.ghost = 100;
	soldier.team = 1;
	soldier.hasPosition = true;
	soldier.position = {10, 20, 30};
	events.push_back(net::makeCreateObject(soldier));
	events.push_back(net::makePostRemote(0, 1, net::int32Payload(1)));
	net::GameEvent kill;
	kill.type = net::kEvPythonCommand;
	kill.values = {100, 0, 3, 0, 3, 5};
	events.push_back(kill);
	net::ChatMessage line;
	line.player = 5;
	line.text = "gg";
	for (const net::GameEvent& e : net::stringBlockEvents(net::encodeChat(line))) {
		events.push_back(e);
	}
	net::GhostBlock ghosts;
	ghosts.present = true;
	ghosts.tick = 600;
	net::ControlObjectRecord control;
	control.ghost = 100;
	control.position = {11, 21, 31};
	ghosts.control = control;
	net::GhostUpdate update;
	update.kind = 1;
	update.ghost = 100;
	update.state = {0x55, 0x01};
	update.stateBits = 12;
	ghosts.updates.push_back(update);
	server.send(events, ghosts);

	const net::RemoteWorld& world = client.world();
	CHECK(runUntil(client, server, [&] { return world.serverTick() == 600; }));
	CHECK(world.localPlayer() == 3 && world.playerList().size() == 2);
	CHECK(world.player(5) && world.player(5)->name == "Enemy" && world.player(5)->kills == 1);
	CHECK(world.player(3) && world.player(3)->deaths == 1);
	CHECK(world.object(100) && world.object(100)->position.x == 11 && world.object(100)->updates == 1);
	CHECK(world.gameState() == 1 && world.serverName() == "Test Server");
	const auto mapList = world.mapList();
	CHECK(mapList.size() == 2);
	if (mapList.size() == 2) {
		CHECK(mapList[0].name == "suez_canal" && mapList[0].gameMode == "gpm_cq" && mapList[0].size == 64);
		CHECK(mapList[1].name == "minsk" && mapList[1].size == 32);
	}
	const auto strings = world.dataBlockStrings(5);
	CHECK(std::find(strings.begin(), strings.end(), "suez_canal") != strings.end());
	auto chat = client.world().takeChat();
	CHECK(chat.size() == 1 && chat[0].text == "gg" && chat[0].player == 5);

	// Requests, with the first packet carrying them lost: the block goes again.
	net::PlayerInput input;
	input.set(net::kPIThrottle, 1.0f);
	input.set(net::kPIFire, 1.0f);
	client.setInput(input);
	server.dropData = 1;
	client.selectTeam(2);
	client.selectKit(1);
	client.selectSpawnGroup(4);
	client.say("hello");
	CHECK(runUntil(client, server, [&] {
		server.send({}); // the server's packets carry its acknowledgements
		return countType(server.received, net::kEvStringBlock) >= 2;
	}));
	CHECK(client.blocksResent() >= 1 && server.resentBlocks >= 1);
	std::vector<net::PostRemoteInfo> posts;
	std::string chatBlock;
	for (const net::GameEvent& e : server.received) {
		net::PostRemoteInfo info;
		if (net::parsePostRemote(e, info) && (info.id == net::kNeSelectTeam || info.id == net::kNeSelectSpawnGroup)) {
			posts.push_back(info);
		}
		net::DataBlockChunk chunk;
		if (net::parseStringBlock(e, chunk) && !chunk.start) {
			chatBlock += chunk.bytes;
		}
	}
	CHECK(posts.size() == 2);
	if (posts.size() == 2) {
		CHECK(posts[0].category == 6 && posts[0].id == net::kNeSelectTeam && posts[0].payloadInt() == 2);
		CHECK(posts[1].id == net::kNeSelectSpawnGroup && posts[1].payloadInt() == 4);
	}
	CHECK(countType(server.received, net::kEvSelectKit) == 1);
	net::ChatMessage said;
	CHECK(net::decodeChat(chatBlock, said) && said.text == "hello" && said.source == 2 && said.player == 3);

	// The moves carry the input, the last three in each packet; none were
	// sent before the server made our player.
	CHECK(!server.inputs.empty() && !server.inputs.front().present);
	bool sawInput = false;
	for (const net::InputBlock& block : server.inputs) {
		CHECK(!block.present || (!block.moves.empty() && block.moves.size() <= 3));
		for (const net::PlayerMove& move : block.moves) {
			sawInput = sawInput || (move.axes[net::kPIThrottle] == 100 && move.trigger(net::kPIFire));
		}
	}
	CHECK(sawInput);

	// The level to load (data block 2), the objects sent (6/4), the
	// checksum (6/5) and the start (6/3): the client answers each as the
	// original does (0x004b5eb0, 0x004b6630).
	net::RemoteWorld::LevelInfo level;
	level.gameMode = "gpm_cq";
	level.levelDirectory = "Levels/";
	level.mapName = "suez_canal";
	level.maxPlayers = 64;
	const std::string levelBlock = net::RemoteWorld::encodeLevel(level);
	server.send({net::makeDataBlockStart(2, static_cast<uint32_t>(levelBlock.size())), net::makeDataBlockChunk(levelBlock),
		net::makePostRemote(6, net::kNeDatabaseComplete), net::makePostRemote(6, net::kNeReset, net::int32Payload(0x1234)),
		net::makePostRemote(6, net::kNeStartSimulation)});
	auto postCount = [&](uint32_t id) {
		size_t n = 0;
		for (const net::GameEvent& e : server.received) {
			net::PostRemoteInfo info;
			n += net::parsePostRemote(e, info) && info.category == 6 && info.id == id && info.payload.empty() ? 1 : 0;
		}
		return n;
	};
	CHECK(runUntil(client, server, [&] {
		server.send({});
		return countType(server.received, net::kEvConnectionType) == 2;
	}));
	CHECK(world.hasLevel() && world.level().mapName == "suez_canal" && world.level().maxPlayers == 64);
	CHECK(postCount(net::kNeLoadComplete) == 1 && postCount(net::kNeDatabaseComplete) == 1 && postCount(net::kNeReset) == 1);

	// Squads, the commander, votes and radio go as their own events.
	client.joinSquad(3);
	client.setPrivateSquad(3, true);
	client.applyForCommander();
	client.startVote(2, 5);
	client.radioMessage(12, 1);
	client.createSquad(4, "an overly long squad name of over 31 bytes");
	CHECK(runUntil(client, server, [&] {
		server.send({});
		return countType(server.received, net::kEvRadioMessage) == 1;
	}));
	for (const net::GameEvent& e : server.received) {
		if (e.type == net::kEvJoinSquad) {
			CHECK(e.u(0) == 1 && e.u(1) == 3 && e.u(2) == 3); // team, player, squad
		}
		if (e.type == net::kEvSetPrivateSquad) {
			CHECK(e.u(0) == 1 && e.u(1) == 3 && e.u(2) == 1);
		}
		if (e.type == net::kEvCommander) {
			CHECK(e.u(0) == net::GameClient::kCommanderApply && e.u(1) == 1 && e.i(2) == 3);
		}
		if (e.type == net::kEvVote) {
			CHECK(e.u(0) == 5 && e.u(6) == 3 && e.u(7) == 0 && e.u(8) == 2);
		}
		if (e.type == net::kEvRadioMessage) {
			// 0x004b3020: the channel again in the byte after it, the last field 0.
			CHECK(e.u(0) == 12 && e.u(2) == 1 && e.u(3) == 1 && e.u(5) == 0);
		}
		if (e.type == net::kEvChangeSquadName) {
			// 0x0050d960: squad, team, our player id (the server joins it), at most 31 bytes.
			CHECK(e.u(0) == 4 && e.u(1) == 1 && e.u(2) == 3 && e.text(0) == "an overly long squad name of ov");
		}
	}
	CHECK(countType(server.received, net::kEvChangeSquadName) == 1);
	CHECK(countType(server.received, net::kEvJoinSquad) == 1 && countType(server.received, net::kEvCommander) == 1);
	CHECK(countType(server.received, net::kEvVote) == 1 && countType(server.received, net::kEvSetPrivateSquad) == 1);

	// Kicked: a disconnect packet ends the session.
	client.disconnect();
	CHECK(client.state() == net::GameClient::State::Disconnected && !client.update());
}

TEST(NetGame_NoServerAnswer)
{
	// A bound port that never answers: three tries, then NoAnswer.
	net::UdpServerSocket silent;
	CHECK(silent.bind(0));
	net::GameClient client;
	CHECK(client.connect("127.0.0.1", silent.port()));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (client.update() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(client.state() == net::GameClient::State::Failed);
	CHECK(client.connectResult().outcome == net::ConnectResult::Outcome::NoAnswer);
}

// --- Ghost networkable state (Networkables.h)

namespace {

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

net::GhostUpdate stateUpdate(uint16_t ghost, const net::NetworkableHints& hints, const net::Vec3f* reference,
	const net::NetworkableState& state)
{
	net::BitWriter bits;
	net::encodeNetworkable(bits, hints, reference, state);
	net::GhostUpdate update;
	update.kind = 1;
	update.ghost = ghost;
	update.state = bits.data();
	update.stateBits = bits.bits();
	return update;
}

} // namespace

TEST(NetGame_DeltaQuantization)
{
	// 0x006876c0 / 0x00687510: near the reference small fields, far floats.
	const net::Vec3f ref {100, 20, -50};
	for (const net::Vec3f v : {net::Vec3f {100.4f, 20.1f, -50.2f}, net::Vec3f {150, 22, -10}, net::Vec3f {900, 300, 700}}) {
		net::BitWriter out;
		net::writeDelta(out, net::DeltaTable::Wide, 0.0005f, v, &ref);
		net::BitReader in(out.data().data(), out.data().size());
		const net::Vec3f back = net::readDelta(in, net::DeltaTable::Wide, 0.0005f, ref);
		CHECK(near(back.x, v.x, 0.001f) && near(back.y, v.y, 0.001f) && near(back.z, v.z, 0.001f));
	}
	{
		// 0.4 m away at 0.5 mm is 800 steps: mode 3, 12 bits each.
		net::BitWriter out;
		net::writeDelta(out, net::DeltaTable::Wide, 0.0005f, {100.4f, 20, -50}, &ref);
		CHECK(out.bits() == 2 + 3 * 12);
		net::BitReader in(out.data().data(), out.data().size());
		CHECK(in.read(2) == 3);
	}
	{
		// Without a reference (0x0050d5b0): mode 0 and three floats.
		net::BitWriter out;
		net::writeDelta(out, net::DeltaTable::Wide, 0.0005f, {1, 2, 3}, nullptr);
		CHECK(out.bits() == 2 + 96);
	}
	{
		// 0x00687c20: mode 7 is the reference itself.
		net::BitWriter out;
		net::writeDelta3(out, 0.01f, ref, ref);
		CHECK(out.bits() == 3);
		net::BitReader in(out.data().data(), out.data().size());
		const net::Vec3f back = net::readDelta3(in, 0.01f, ref);
		CHECK(back.x == ref.x && back.z == ref.z);
		net::BitWriter small;
		net::writeDelta3(small, 0.01f, {0.5f, -0.25f, 0}, {});
		net::BitReader in2(small.data().data(), small.data().size());
		CHECK(in2.read(3) == 6); // 50 steps: 8 bits
	}
	{
		const net::Quat q {0.1f, 0.7f, -0.2f, 0.678f};
		net::BitWriter out;
		net::writeQuaternion(out, q, 14);
		CHECK(out.bits() == 56);
		net::BitReader in(out.data().data(), out.data().size());
		const net::Quat back = net::readQuaternion(in, 14);
		const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
		CHECK(near(back.x, q.x / n, 0.002f) && near(back.y, q.y / n, 0.002f) && near(back.w, q.w / n, 0.002f));
	}
}

TEST(NetGame_NetworkableRoundTrips)
{
	const net::Vec3f ref {10, 0, 10};
	{
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::SimpleObject;
		h.mobile = true;
		h.hasArmor = true;
		h.maxHitPoints = 700;
		h.wreckHitPoints = 100;
		net::NetworkableState s;
		s.mask = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x1000;
		s.position = {12.5f, 3, 9};
		s.velocity = {4, 0, -1.5f};
		s.angularVelocity = {0, 0.5f, 0};
		s.orientation = {0, 0.3826834f, 0, 0.9238795f};
		s.health = 512;
		net::BitWriter out;
		net::encodeNetworkable(out, h, &ref, s);
		net::BitReader in(out.data().data(), out.data().size());
		net::NetworkableState back;
		CHECK(net::decodeNetworkable(in, h, ref, back) && back.complete && in.position() == out.bits());
		CHECK(back.hasPosition && near(back.position.x, 12.5f, 0.001f) && near(back.position.z, 9, 0.001f));
		CHECK(back.hasVelocity && near(back.velocity.x, 4, 0.002f) && near(back.velocity.z, -1.5f, 0.002f));
		CHECK(back.hasAngularVelocity && near(back.angularVelocity.y, 0.5f, 0.01f));
		CHECK(back.hasOrientation && near(back.orientation.y, 0.3826834f, 0.001f));
		CHECK(back.hasHealth && back.health == 512);
		// Hit points: 512 + 100 in 0..800, 10 bits.
		net::BitWriter bits;
		h.mobile = false;
		s.mask = 0x10;
		net::encodeNetworkable(bits, h, &ref, s);
		CHECK(bits.bits() == 25 + 1 + 10);
		// Without armor the writer sends no hit points (0x005ff5d0): the
		// state ends after the flag.
		net::NetworkableHints noArmor = h;
		noArmor.hasArmor = false;
		net::BitWriter none;
		net::encodeNetworkable(none, noArmor, &ref, s);
		CHECK(none.bits() == 26);
		net::BitReader in2(bits.data().data(), bits.data().size());
		CHECK(net::decodeNetworkable(in2, noArmor, ref, back) && back.complete && !back.hasHealth && in2.position() == 26);
	}
	{
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::Soldier;
		h.soldierItemSlots = 6;
		net::NetworkableState s;
		s.mask = 0x1 | 0x2 | 0x4 | 0x10 | 0x40 | 0x100 | 0x2000;
		s.position = {11, 1.5f, 9.5f};
		s.velocity = {2, 0, 3};
		s.yaw = 90;
		s.pitch = -10;
		s.aimPitch = 30;
		s.health = 0.75f;
		s.weaponZoom = 1;
		net::BitWriter out;
		net::encodeNetworkable(out, h, &ref, s);
		net::BitReader in(out.data().data(), out.data().size());
		net::NetworkableState back;
		CHECK(net::decodeNetworkable(in, h, ref, back) && back.complete && in.position() == out.bits());
		CHECK(near(back.position.y, 1.5f, 0.002f) && near(back.velocity.z, 3, 0.01f));
		CHECK(back.hasAngles && near(back.yaw, 90, 0.2f) && near(back.pitch, -10, 0.05f) && near(back.aimPitch, 30, 0.05f));
		CHECK(near(back.health, 0.75f, 0.005f) && back.weaponZoom == 1);
		net::NetworkableHints unknownSlots = h;
		unknownSlots.soldierItemSlots = -1;
		net::BitReader in2(out.data().data(), out.data().size());
		CHECK(net::decodeNetworkable(in2, unknownSlots, ref, back) && !back.complete && near(back.yaw, 90, 0.2f));
	}
	{
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::ControlPoint;
		net::NetworkableState s;
		s.mask = 0x3f;
		s.team = 2;
		s.cpNeutralStart = true;
		s.cpValues[0] = 0.5f;
		s.cpValues[1] = -1.0f;
		s.cpAllowTeam1 = true;
		s.cpValue = -1;
		net::BitWriter out;
		net::encodeNetworkable(out, h, nullptr, s);
		CHECK(out.bits() == 6 + 4 + 1 + 32 + 4 + 4 + 4);
		net::BitReader in(out.data().data(), out.data().size());
		net::NetworkableState back;
		CHECK(net::decodeNetworkable(in, h, ref, back) && back.complete);
		CHECK(back.team == 2 && back.cpNeutralStart && near(back.cpValues[0], 0.5f, 0.001f) && near(back.cpValues[1], -1, 0.001f));
		CHECK(back.cpAllowTeam1 && !back.cpAllowTeam2 && back.cpValue == -1);
	}
	{
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::GenericProjectile;
		net::NetworkableState s;
		s.mask = 0x7;
		s.position = {30, 5, 30};
		s.orientation = {0.5f, 0.5f, 0.5f, 0.5f};
		s.hasVelocity = true;
		s.velocity = {1, 0, 0};
		net::BitWriter out;
		net::encodeNetworkable(out, h, &ref, s);
		net::BitReader in(out.data().data(), out.data().size());
		net::NetworkableState back;
		CHECK(net::decodeNetworkable(in, h, ref, back) && back.complete && in.position() == out.bits());
		CHECK(near(back.position.x, 30, 0.01f) && near(back.orientation.x, 0.5f, 0.002f) && near(back.velocity.x, 1, 0.01f));
	}
	{
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::Player;
		net::NetworkableState s;
		s.mask = 0x1 | 0x2 | 0x8 | 0x100 | 0x200 | 0x40 | 0x10000 | 0x8000000;
		s.player.alive = true;
		s.player.spawnGroup = 3;
		s.player.score = -12;
		s.player.kills = 25;
		s.player.deaths = 7;
		s.player.teamwork = 14;
		s.player.teamKills = 1;
		s.player.ping = 87;
		s.player.team = 2;
		net::BitWriter out;
		net::encodeNetworkable(out, h, nullptr, s);
		net::BitReader in(out.data().data(), out.data().size());
		net::NetworkableState back;
		CHECK(net::decodeNetworkable(in, h, ref, back) && back.complete && in.position() == out.bits());
		const net::PlayerNetState& p = back.player;
		CHECK(p.alive && p.spawnGroup == 3 && p.score == -12 && p.kills == 25 && p.deaths == 7);
		CHECK(p.teamwork == 14 && p.teamKills == 1 && p.ping == 87 && p.team == 2);
	}
	{ // Player, 0x00596100's other fields: the kit (0x1000), the squad
	  // (0x2000, 5 bits), rank + 1 (0x4000, 6 bits), the spawn tick and
	  // man-down (0x20), the flag holder (0x400000), the squad leader (always)
	  // and the commander (0x8000000; then 6 bits and 4 bytes).
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::Player;
		net::NetworkableState s;
		s.mask = 0x20 | 0x800 | 0x1000 | 0x2000 | 0x4000 | 0x400000 | 0x8000000;
		s.player.kit = 4;
		s.player.squad = 9;
		s.player.rank = 0; // sent as 1
		s.player.value294 = 200;
		s.player.spawnTick = 123456;
		s.player.manDown = true;
		s.player.flagHolder = true;
		s.player.squadLeader = true;
		s.player.commander = false;
		net::BitWriter out;
		net::encodeNetworkable(out, h, nullptr, s);
		// 30 mask, flag holder 1, spawn tick 32 + 1, 0x800 9, kit 5, squad 5,
		// rank 6, squad leader 1, commander 1 + 6 + 32 (a leader), the last bit.
		CHECK(out.bits() == 30 + 1 + 33 + 9 + 5 + 5 + 6 + 1 + 1 + 6 + 32 + 1);
		net::BitReader in(out.data().data(), out.data().size());
		net::NetworkableState back;
		CHECK(net::decodeNetworkable(in, h, ref, back) && back.complete && in.position() == out.bits());
		const net::PlayerNetState& p = back.player;
		CHECK(p.kit == 4 && p.squad == 9 && p.rank == 0 && p.value294 == 200);
		CHECK(p.hasSpawnTick && p.spawnTick == 123456 && p.manDown && p.hasFlagHolder && p.flagHolder);
		CHECK(p.squadLeader && p.hasCommander && !p.commander);
		// The rank goes out plus one (the reader takes one off, 0x005981a0).
		net::BitReader raw(out.data().data(), out.data().size());
		raw.read(31);
		raw.read(32);
		raw.read(20);
		CHECK(raw.read(6) == 1);
	}
	CHECK(net::networkableKindForTemplateType("PlayerControlObject") == net::NetworkableKind::SimpleObject);
	CHECK(net::networkableKindForTemplateType("PlayerControlObject", true) == net::NetworkableKind::PlayerControlObject);
	CHECK(net::networkableKindForTemplateType("Soldier") == net::NetworkableKind::Soldier);
	// 0x005bf200: hasMobilePhysics turns a DestroyableObject into a SimpleObject.
	CHECK(net::networkableKindForTemplateType("DestroyableObject") == net::NetworkableKind::DestroyableObject);
	CHECK(net::networkableKindForTemplateType("DestroyableObject", false, true) == net::NetworkableKind::SimpleObject);
}

TEST(NetGame_RemoteWorldMovesGhosts)
{
	net::RemoteWorld world;
	net::NetworkableHints tankHints;
	tankHints.kind = net::NetworkableKind::SimpleObject;
	tankHints.mobile = true;
	world.setNetworkableHints([&](uint32_t id) {
		net::NetworkableHints h;
		if (id == 7) {
			h = tankHints;
		} else if (id == 8) {
			h.kind = net::NetworkableKind::Soldier;
		}
		return h;
	});
	net::CreateObjectInfo tank;
	tank.templateId = 7;
	tank.ghost = 40;
	tank.hasPosition = true;
	world.apply(net::makeCreateObject(tank));
	net::CreateObjectInfo soldier;
	soldier.templateId = 8;
	soldier.ghost = 41;
	world.apply(net::makeCreateObject(soldier));
	net::CreatePlayerInfo me;
	me.id = 1;
	me.name = "Me";
	me.object = 90; // the player's networkable ghost
	world.apply(net::makeCreatePlayer(me));
	CHECK(world.object(40)->kind == net::NetworkableKind::SimpleObject && world.object(41)->kind == net::NetworkableKind::Soldier);

	net::GhostBlock ghosts;
	ghosts.present = true;
	ghosts.tick = 10;
	net::ControlObjectRecord control;
	control.ghost = 41;
	control.position = {100, 5, 100};
	ghosts.control = control;
	const net::Vec3f reference = control.position;
	net::NetworkableState tankState;
	tankState.mask = 0x7;
	tankState.position = {120, 6, 95};
	tankState.velocity = {5, 0, 0};
	tankState.orientation = {0, 0.7071068f, 0, 0.7071068f};
	ghosts.updates.push_back(stateUpdate(40, tankHints, &reference, tankState));
	net::NetworkableHints soldierHints;
	soldierHints.kind = net::NetworkableKind::Soldier;
	net::NetworkableState soldierState;
	soldierState.mask = 0x3 | 0x40;
	soldierState.position = {100.5f, 5, 101};
	soldierState.yaw = -45;
	soldierState.health = 1;
	ghosts.updates.push_back(stateUpdate(41, soldierHints, &reference, soldierState));
	net::NetworkableHints playerHints;
	playerHints.kind = net::NetworkableKind::Player;
	net::NetworkableState playerState;
	playerState.mask = 0x8 | 0x100;
	playerState.player.score = 40;
	playerState.player.kills = 4;
	playerState.player.deaths = 2;
	playerState.player.ping = 60;
	ghosts.updates.push_back(stateUpdate(90, playerHints, nullptr, playerState));

	// Writing and reading the block back keeps everything.
	net::BitWriter block;
	net::writeGhostBlock(block, ghosts);
	net::BitReader in(block.data().data(), block.data().size());
	net::GhostBlock back;
	CHECK(net::readGhostBlock(in, back));
	world.apply(back);
	const net::RemoteObject* t = world.object(40);
	CHECK(t && t->decoded && near(t->position.x, 120, 0.001f) && near(t->velocity.x, 5, 0.002f));
	CHECK(t && t->hasOrientation && near(t->orientation.y, 0.7071068f, 0.001f));
	const net::RemoteObject* s = world.object(41);
	CHECK(s && near(s->position.z, 101, 0.002f) && s->hasAngles && near(s->yaw, -45, 0.2f) && near(s->health, 1, 0.01f));
	const net::RemotePlayer* p = world.player(1);
	CHECK(p && p->score == 40 && p->kills == 4 && p->deaths == 2 && p->ping == 60);
	CHECK(!world.object(90)); // a player's ghost is no object
}

namespace {

// Writes a state as the original writer does and reads it back; the reader
// must end where the writer did.
net::NetworkableState roundTrip(const net::NetworkableHints& h, const net::NetworkableState& s, const net::Vec3f& ref,
	size_t* bits = nullptr)
{
	net::BitWriter out;
	net::encodeNetworkable(out, h, &ref, s);
	if (bits) {
		*bits = out.bits();
	}
	net::BitReader in(out.data().data(), out.data().size());
	net::NetworkableState back;
	const bool ok = net::decodeNetworkable(in, h, ref, back);
	CHECK(ok && back.complete && in.position() == out.bits());
	return back;
}

} // namespace

// The networkables of a ghost's later groups (vehicle parts) and the
// SimpleObject / Soldier fields after the first ones, as the writers lay
// them out (0x006015a0, 0x00605170, 0x00608df0, 0x0060a000, 0x00609c60,
// 0x00603fe0, 0x00602b70, 0x006037a0, 0x00604d70, 0x00605650, 0x006044e0,
// 0x005ff5d0, 0x005fb8c0).
TEST(NetGame_PartNetworkablesRoundTrip)
{
	const net::Vec3f ref {100, 20, -50};
	size_t bits = 0;
	{ // RotationalBundle: a turret's yaw and pitch, its speeds, the ignore-parent rotation.
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::RotationalBundle;
		h.maxSpeed = {2, 1.5f, 0};
		net::NetworkableState s;
		s.mask = 0x1 | 0x2 | 0x8 | 0x40 | 0x100 | 0x200;
		s.rotation[0] = 90;
		s.rotation[1] = -12.5f;
		s.rotationSpeed[0] = -20;
		s.excessInput = 10;
		s.parentOrientation = {0, 0.7071068f, 0, 0.7071068f};
		s.parentRotation[0] = 45;
		const net::NetworkableState b = roundTrip(h, s, ref, &bits);
		CHECK(bits == 12 + 12 + 12 + 12 + 16 + 12 + 4 * 14);
		CHECK(near(b.rotation[0], 90, 0.2f) && near(b.rotation[1], -12.5f, 0.2f) && b.rotation[2] == 0);
		CHECK(near(b.rotationSpeed[0], -20, 0.02f) && near(b.excessInput, 10, 0.002f));
		CHECK(near(b.parentRotation[0], 45, 0.2f) && near(b.parentOrientation.y, 0.7071068f, 0.002f));
		// The first step either side of 0 reads as 0 (0x00593c70 snaps |f| <= 2/max).
		net::BitWriter one;
		one.write(1, 12);
		one.write(2048, 12);
		net::BitReader in(one.data().data(), one.data().size());
		net::NetworkableState z;
		CHECK(net::decodeNetworkable(in, h, ref, z) && z.rotation[0] == 0);
	}
	{ // Spring: offset and velocity; newCar2's extra value.
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::Spring;
		net::NetworkableState s;
		s.springPresent = true;
		s.springOffset = {0, -0.12f, 0};
		s.springVelocity = {0, 0.5f, 0};
		net::NetworkableState b = roundTrip(h, s, ref);
		CHECK(b.springPresent && near(b.springOffset.y, -0.12f, 0.002f) && near(b.springVelocity.y, 0.5f, 0.002f));
		h.springExtra = true;
		s.springExtra = -100;
		b = roundTrip(h, s, ref);
		CHECK(near(b.springExtra, -100, 0.01f));
		net::NetworkableState absent;
		roundTrip(h, absent, ref, &bits);
		CHECK(bits == 1);
	}
	{ // Engine: gear (numberOfGears sizes it), rotation, inputs; newCar2 rpm; helicopter angle of attack.
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::Engine;
		h.engineGears = 5;
		h.engineType = 194; // c_ETNewCar2
		h.minRotation = {0, 0, -100};
		h.maxRotation = {0, 0, 300};
		h.minRpm = 800;
		h.maxRpm = 6000;
		net::NetworkableState s;
		s.mask = 0x2 | 0x4 | 0x20 | 0x200 | 0x400;
		s.gear = 3;
		s.engineRotation[1] = 150;
		s.engineInputs[0] = 1;
		s.engineDirection = -1;
		s.rpm = -3000;
		s.clutch = 0.5f;
		s.carGear = -1;
		const net::NetworkableState b = roundTrip(h, s, ref, &bits);
		CHECK(bits == 11 + 3 + 16 + 12 + 2 + 1 + 16 + 8 + 3);
		CHECK(b.gear == 3 && near(b.engineRotation[1], 150, 0.02f) && near(b.engineInputs[0], 1, 0.001f));
		CHECK(b.engineDirection == -1 && near(b.rpm, -3000, 0.1f) && near(b.clutch, 0.5f, 0.005f) && b.carGear == -1);
		h.engineType = 33; // c_ETHelicopter
		h.maxAngleOfAttack = 20;
		s.mask = 0x100;
		s.angleOfAttack = -8;
		CHECK(near(roundTrip(h, s, ref).angleOfAttack, -8, 0.01f));
	}
	{ // MechLegs, TargetObject, Item.
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::MechLegs;
		net::NetworkableState s;
		s.mask = 7;
		s.legState = 9;
		s.legFlag2 = true;
		net::NetworkableState b = roundTrip(h, s, ref, &bits);
		CHECK(bits == 3 + 4 + 1 + 1 && b.legState == 9 && !b.legFlag1 && b.legFlag2);
		h.kind = net::NetworkableKind::TargetObject;
		s = {};
		s.mask = 7;
		s.position = {101, 21, -49};
		s.targetActive = true;
		s.targetValue = 200;
		b = roundTrip(h, s, ref);
		CHECK(near(b.position.x, 101, 0.001f) && b.targetActive && b.targetValue == 200);
		h.kind = net::NetworkableKind::Item;
		s = {};
		s.mask = 0x1a;
		s.itemValue = -77;
		s.itemActive = true;
		s.itemUnlock = 3;
		b = roundTrip(h, s, ref, &bits);
		CHECK(bits == 5 + 9 + 1 + 5 && b.itemValue == -77 && b.itemActive && b.itemUnlock == 3);
	}
	{ // Cameras.
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::FreeCamera;
		net::NetworkableState s;
		s.mask = 7;
		s.position = {100.5f, 25, -50};
		s.orientation = {0, 0, 0, 1};
		s.cameraValue = -3;
		net::NetworkableState b = roundTrip(h, s, ref);
		CHECK(near(b.position.y, 25, 0.001f) && near(b.orientation.w, 1, 0.001f) && b.cameraValue == -3);
		h.kind = net::NetworkableKind::OverheadCamera;
		h.overheadRange = 50;
		s.mask = 0xf;
		s.cameraPan[0] = 25;
		s.cameraPan[1] = -50;
		s.cameraFlag = true;
		b = roundTrip(h, s, ref);
		CHECK(near(b.cameraPan[0], 25, 0.2f) && near(b.cameraPan[1], -50, 0.2f) && b.cameraFlag && b.cameraValue == -3);
	}
	{ // DestroyableWindowsBundle: numWindows bits, no mask.
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::DestroyableWindowsBundle;
		h.windows = 6;
		net::NetworkableState s;
		s.windowBits = 0x2d;
		CHECK(roundTrip(h, s, ref, &bits).windowBits == 0x2d && bits == 6);
		h.windows = -1;
		net::BitReader in(nullptr, 0);
		net::NetworkableState b;
		CHECK(!net::decodeNetworkable(in, h, ref, b));
	}
	{ // A seat with weapons: the active weapons; without: the occupant's soldier state.
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::PlayerControlObject;
		h.seatCameras = 2;
		net::NetworkableState s;
		s.mask = 0x1 | 0x2 | 0x8;
		s.seatWeapon[0] = 1;
		s.seatWeapon[1] = -1;
		s.seatDucking = true;
		net::NetworkableState b = roundTrip(h, s, ref, &bits);
		CHECK(bits == 4 + 4 + 4 + 1 && b.seatWeapon[0] == 1 && b.seatWeapon[1] == -1 && b.seatDucking);
		h.seatCameras = 0;
		auto soldier = std::make_shared<net::NetworkableState>();
		soldier->mask = 0x2 | 0x10 | 0x40;
		soldier->yaw = -45;
		soldier->aimPitch = 10;
		soldier->health = 0.5f;
		s = {};
		s.occupant = soldier;
		b = roundTrip(h, s, ref);
		CHECK(b.occupant && near(b.occupant->yaw, -45, 0.2f) && near(b.occupant->aimPitch, 10, 0.05f));
		CHECK(b.occupantBits > 0 && b.occupant && near(b.occupant->health, 0.5f, 0.005f));
		roundTrip(h, net::NetworkableState {}, ref, &bits);
		CHECK(bits == 11);
	}
	{ // DestroyableObject: hit points, a position and value, a bit per armored part.
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::DestroyableObject;
		h.hasArmor = true;
		h.maxHitPoints = 300;
		h.wreckHitPoints = 50;
		h.armoredChildren = {false, false, true};
		h.armoredChildrenKnown = true;
		net::NetworkableState s;
		s.mask = 7;
		s.health = 120;
		s.position = {90, 20, -40};
		s.destroyValue = 40;
		s.childDamage = {8, 0, 8};
		const net::NetworkableState b = roundTrip(h, s, ref, &bits);
		CHECK(bits == 3 + 9 + (2 + 3 * 12) + 10 + 3);
		CHECK(b.health == 120 && near(b.position.x, 90, 0.01f) && near(b.destroyValue, 40, 0.1f));
		CHECK(b.childDamage == std::vector<int>({8, 0, 8}));
	}
	{ // SimpleObject: EMP, TeamChange, a vehicle's weapons and flags, its armored parts.
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::SimpleObject;
		h.hasArmor = true;
		h.maxHitPoints = 1000;
		h.wreckHitPoints = 100;
		h.hasTeamChange = true;
		h.inVehicle = true;
		h.armoredChildren = {true, false};
		h.armoredChildrenKnown = true;
		net::NetworkableState s;
		s.mask = 0x2 | 0x10 | 0x20 | 0x40 | 0x1000 | 0x2000 | 0x80000 | 0x40000 | 0x100000 | 0x800000 | 0x1000000;
		s.position = {120, 22, -60};
		s.health = -40;
		s.empLevel = 7;
		s.changePoints = 200;
		s.spawnTeam = 2;
		s.seatWeapon[0] = 0;
		s.seatWeapon[2] = 4;
		s.vehicleValue = 9;
		s.ducking = true;
		s.childDamage = {5, 0};
		s.responseFlag = true;
		const net::NetworkableState b = roundTrip(h, s, ref, &bits);
		CHECK(bits == 26 + (2 + 3 * 20) + 11 + 4 + 8 + 2 + 4 + 4 + 5 + 1 + 4 + 1 + 1);
		CHECK(b.health == -40 && b.empLevel == 7 && b.changePoints == 200 && b.spawnTeam == 2);
		CHECK(b.seatWeapon[0] == 0 && b.seatWeapon[1] == -1 && b.seatWeapon[2] == 4 && b.vehicleValue == 9);
		CHECK(b.ducking && !b.vehicleFlag && b.childDamage == std::vector<int>({5, 0}) && b.responseFlag);
		// A parachute: its state instead of the weapons.
		h.parachute = true;
		s.mask = 0x8000;
		s.parachuteState = 5;
		CHECK(roundTrip(h, s, ref, &bits).parachuteState == 5 && bits == 26 + 4);
	}
	{ // Soldier: weapon zoom and fire state, lean, stamina, the weapon, a ladder; dead: the ragdoll.
		net::NetworkableHints h;
		h.kind = net::NetworkableKind::Soldier;
		h.soldierItemSlots = 8;
		net::NetworkableState s;
		s.mask = 0x80 | 0x20 | 0x800 | 0x8000 | 0x2000 | 0x4000 | 0x20000 | 0x100000;
		s.empLevel = 3;
		s.soldierValues[0] = 4;
		s.soldierValues[2] = 2;
		s.weaponZoom = 2;
		s.weaponFireState = 1;
		s.soldierBits = 0x4;
		s.soldierLean[0] = -25;
		s.stamina = 1;
		s.weapon = 6;
		s.ladder = 17;
		s.ladderPosition = 0.5f;
		s.vehicleValue = 3;
		net::NetworkableState b = roundTrip(h, s, ref, &bits);
		CHECK(bits == 21 + 4 + 6 + 3 + 2 + 3 + 10 + 1 + 5 + 4 + 2 + 9 + 12 + 5);
		CHECK(b.empLevel == 3 && b.soldierValues[0] == 4 && b.soldierValues[2] == 2 && b.weaponZoom == 2);
		CHECK(b.weaponFireState == 1 && b.soldierBits == 0x4 && near(b.soldierLean[0], -25, 0.1f) && b.stamina == 1);
		CHECK(b.weapon == 6 && b.ladder == 17 && near(b.ladderPosition, 0.5f, 0.001f) && b.vehicleValue == 3);
		s = {};
		s.mask = 0x10000 | 0x40;
		s.health = 0;
		s.position = {1, 2, 3};
		b = roundTrip(h, s, ref, &bits);
		CHECK(bits == 21 + 9 + 9 + 96 && b.ragdollBits == 96 && b.position.z == 3 && b.health == 0);
	}
}

namespace {

net::GhostTemplateNode node(uint32_t id, const char* name, net::NetworkableKind kind, std::vector<net::GhostTemplateNode> children = {})
{
	net::GhostTemplateNode n;
	n.templateId = id;
	n.name = name;
	n.hints.kind = kind;
	n.children = std::move(children);
	return n;
}

// A buggy-like vehicle: the driver's PlayerControlObject (a SimpleObject
// networkable as the root), its engine, four wheels (unique springs) on two
// steering navs of the same template, a gunner's seat (a ghost of its own)
// with a turret of two RotationalBundles and a gun (no networkable), a
// camera of the same rotation settings as the turret base, and a projectile.
net::GhostTemplateNode buggy()
{
	using K = net::NetworkableKind;
	net::GhostTemplateNode root = node(1, "buggy", K::SimpleObject);
	root.controlObject = true;
	root.hints.mobile = true;
	root.hints.hasArmor = true;
	root.hints.maxHitPoints = 500;
	root.hints.wreckHitPoints = 50;
	root.hints.inVehicle = true;
	root.hints.armoredChildrenKnown = true;
	net::GhostTemplateNode engine = node(2, "buggy_engine", K::Engine);
	engine.hints.engineGears = 3;
	auto wheel = [&](uint32_t id, const char* name) {
		net::GhostTemplateNode w = node(id, name, K::Spring);
		w.unique = true;
		return w;
	};
	net::GhostTemplateNode navL = node(3, "buggy_nav", K::RotationalBundle, {wheel(4, "wheel_lf")});
	net::GhostTemplateNode navR = node(3, "buggy_nav", K::RotationalBundle, {wheel(5, "wheel_rf")});
	engine.children = {navL, navR, wheel(6, "wheel_lb"), wheel(7, "wheel_rb")};
	net::GhostTemplateNode base = node(10, "turret_base", K::RotationalBundle);
	base.groupKey = "rotationalbundle|turret";
	net::GhostTemplateNode gun = node(11, "turret_gun", K::Unknown); // GenericFireArm: none
	net::GhostTemplateNode shell = node(12, "turret_shell", K::GenericProjectile);
	shell.projectile = true;
	net::GhostTemplateNode barrel = node(13, "turret_barrel", K::RotationalBundle, {gun, shell});
	base.children = {barrel};
	net::GhostTemplateNode seat = node(20, "buggy_gunner", K::PlayerControlObject, {base});
	seat.controlObject = true;
	seat.hints.seatCameras = 1;
	net::GhostTemplateNode camera = node(30, "buggy_camera", K::RotationalBundle);
	camera.groupKey = "rotationalbundle|turret"; // a Camera with the turret base's settings
	net::GhostTemplateNode sound = node(40, "buggy_sound", K::Unknown);
	root.children = {engine, seat, camera, sound};
	return root;
}

} // namespace

// 0x0067c690: ghosts per PlayerControlObject, groups in pre-order with
// identical non-unique objects sharing one, and every group read in turn.
// The template settings the reader's hints come from: the physics node
// that moves (0x005f6ba0), the ResponsePhysics grip deciding a newCar2
// spring's extra field (0x00604f10), a seat's cameras (0x0058bdb0) and a
// mobile DestroyableObject's SimpleObject networkable (0x005bf200).
TEST(NetGame_PhysicsHints)
{
	dice::hfe::world::ObjectTemplateManager templates;
	auto make = [&](const char* type, const char* name, std::vector<std::pair<const char*, const char*>> settings,
					std::vector<const char*> children = {}) {
		dice::hfe::world::ObjectTemplate* t = templates.create(type, name);
		t->settings()["networkableInfo"] = "Info";
		for (const auto& [key, value] : settings) {
			t->settings()[key] = value;
		}
		for (const char* child : children) {
			t->children().push_back({child, {}, {}});
		}
		return t;
	};
	make("Spring", "w8", {{"hasCollisionPhysics", "1"}, {"hasMobilePhysics", "1"}, {"physicsType", "Mesh"}, {"grip", "8"}});
	make("Spring", "w128", {{"hasCollisionPhysics", "1"}, {"hasMobilePhysics", "1"}, {"physicsType", "3"}, {"grip", "128"}});
	make("Spring", "wdefault", {{"hasCollisionPhysics", "1"}, {"hasMobilePhysics", "1"}, {"physicsType", "3"}});
	make("Spring", "wloose", {{"hasMobilePhysics", "1"}, {"physicsType", "3"}, {"grip", "8"}});
	make("Engine", "eng", {{"engineType", "c_ETNewCar2"}}, {"w8", "w128", "wdefault", "wloose"});
	make("Camera", "cam", {});
	make("PlayerControlObject", "inner", {});
	make("PlayerControlObject", "seat", {}, {"inner", "cam"});
	make("PlayerControlObject", "bare", {});
	make("RotationalBundle", "nav", {{"hasMobilePhysics", "1"}});
	make("DestroyableObject", "crate", {{"hasMobilePhysics", "1"}});
	make("DestroyableObject", "wall", {{"hasMobilePhysics", "0"}});
	dice::hfe::world::ObjectTemplate* car = make("PlayerControlObject", "car",
		{{"hasCollisionPhysics", "1"}, {"hasMobilePhysics", "1"}, {"physicsType", "3"}}, {"eng", "seat", "bare", "nav"});

	const net::GhostTemplateNode tree = net::ghostTemplateFrom(templates, *car);
	CHECK(tree.hints.mobile && tree.hints.kind == net::NetworkableKind::SimpleObject);
	std::map<std::string, const net::GhostTemplateNode*> nodes;
	std::function<void(const net::GhostTemplateNode&)> walk = [&](const net::GhostTemplateNode& n) {
		nodes[n.name] = &n;
		for (const net::GhostTemplateNode& c : n.children) {
			walk(c);
		}
	};
	walk(tree);
	CHECK(nodes.size() == 11);
	CHECK(nodes["w8"]->hints.springExtra && nodes["w8"]->hints.responseGrip == 8);
	CHECK(!nodes["w128"]->hints.springExtra && nodes["w128"]->hints.responseGrip == 128);
	CHECK(!nodes["wdefault"]->hints.springExtra && nodes["wdefault"]->hints.responseGrip == 2); // ContactGrip
	CHECK(!nodes["wloose"]->hints.springExtra && nodes["wloose"]->hints.responseGrip == 0);   // no collision: no ResponsePhysics
	CHECK(!nodes["nav"]->hints.mobile); // mobile physics without collision makes no physics node
	CHECK(nodes["seat"]->hints.seatCameras == 1 && nodes["inner"]->hints.seatCameras == 0 && nodes["bare"]->hints.seatCameras == 0);
	CHECK(net::networkableHintsFrom(templates, *templates.find("crate"), false).kind == net::NetworkableKind::SimpleObject);
	CHECK(net::networkableHintsFrom(templates, *templates.find("wall"), false).kind == net::NetworkableKind::DestroyableObject);
}

TEST(NetGame_GhostLayoutGroups)
{
	const std::vector<net::GhostPartLayout> layout = net::buildGhostLayout(buggy());
	CHECK(layout.size() == 2);
	if (layout.size() != 2) {
		return;
	}
	const net::GhostPartLayout& car = layout[0];
	const net::GhostPartLayout& gunner = layout[1];
	CHECK(car.templateId == 1 && gunner.templateId == 20 && gunner.name == "buggy_gunner");
	// The car: root, engine, nav, wheel_lf, nav, wheel_rf, wheel_lb, wheel_rb, camera, sound.
	CHECK(car.nodes.size() == 10 && car.nodes[3].name == "wheel_lf" && car.nodes[3].parent == 2);
	// Groups: root, engine, the navs (shared), four wheels (unique), the camera.
	CHECK(car.groups.size() == 8);
	CHECK(car.nodes[2].group == 2 && car.nodes[4].group == 2 && car.groups[2].members.size() == 2);
	CHECK(car.nodes[3].group == 3 && car.nodes[5].group == 4 && car.nodes[7].group == 6);
	CHECK(car.nodes[8].group == 7 && car.nodes[9].group == -1);
	// The gunner: seat, turret base, barrel, gun (none), shell (a projectile with a parent: left out).
	CHECK(gunner.nodes.size() == 5 && gunner.groups.size() == 3 && gunner.nodes[3].group == -1 && gunner.nodes[4].group == -1);
	CHECK(gunner.groups[0].hints.kind == net::NetworkableKind::PlayerControlObject);

	// An update of every group, read back in order.
	std::vector<net::NetworkableState> states(car.groups.size());
	states[0].mask = 0x2 | 0x10;
	states[0].position = {5, 6, 7};
	states[0].health = 321;
	states[1].mask = 0x2;
	states[1].gear = 2;
	states[2].mask = 0x1;
	states[2].rotation[0] = -30;
	for (int w = 3; w < 7; ++w) {
		states[w].springPresent = true;
		states[w].springOffset = {0, -0.01f * static_cast<float>(w), 0};
	}
	states[7].mask = 0x2;
	states[7].rotation[1] = 15;
	net::BitWriter out;
	net::encodeGhostState(out, car, nullptr, states);
	net::BitReader in(out.data().data(), out.data().size());
	std::vector<net::NetworkableState> back;
	CHECK(net::decodeGhostState(in, car, {}, back) && back.size() == 8 && in.position() == out.bits());
	if (back.size() == 8) {
		CHECK(back[0].health == 321 && near(back[0].position.y, 6, 0.001f) && back[1].gear == 2);
		CHECK(near(back[2].rotation[0], -30, 0.2f) && near(back[6].springOffset.y, -0.06f, 0.002f));
		CHECK(near(back[7].rotation[1], 15, 0.2f));
	}
	// A group it can't read (no hints for the windows) stops the rest.
	net::GhostPartLayout broken = car;
	broken.groups[1].hints.kind = net::NetworkableKind::DestroyableWindowsBundle;
	net::BitReader in2(out.data().data(), out.data().size());
	CHECK(!net::decodeGhostState(in2, broken, {}, back) && back.size() == 1);
}

// The ghosts of a vehicle in the RemoteWorld: CreateObject's count, the
// seat's ghost, the parts' states kept between updates, the occupant.
TEST(NetGame_RemoteWorldVehicleGhosts)
{
	net::RemoteWorld world;
	world.setGhostTemplates([](uint32_t id) { return id == 1 ? buggy() : net::GhostTemplateNode {}; });
	net::CreatePlayerInfo me;
	me.id = 2;
	me.name = "Driver";
	world.apply(net::makeCreatePlayer(me));
	net::CreateObjectInfo car;
	car.templateId = 1;
	car.ghost = 300;
	car.count = 2;
	world.apply(net::makeCreateObject(car));
	const net::RemoteObject* root = world.object(300);
	const net::RemoteObject* seat = world.object(301);
	CHECK(root && seat && root->seats == std::vector<uint16_t>({301}) && seat->rootGhost == 300 && seat->part == 1);
	CHECK(seat && seat->templateId == 20 && seat->kind == net::NetworkableKind::PlayerControlObject);
	CHECK(root && root->parts.size() == 10 && root->findPart("WHEEL_RB") && !root->findPart("nope"));

	const std::vector<net::GhostPartLayout>* layout = world.ghostLayout(1);
	CHECK(layout && layout->size() == 2);
	if (!layout || layout->size() != 2) {
		return;
	}
	auto update = [&](uint16_t ghost, const net::GhostPartLayout& part, const std::vector<net::NetworkableState>& states) {
		net::BitWriter out;
		net::encodeGhostState(out, part, nullptr, states);
		net::GhostBlock block;
		block.present = true;
		block.tick = 90;
		net::GhostUpdate u;
		u.kind = 1;
		u.ghost = ghost;
		u.state = out.data();
		u.stateBits = out.bits();
		block.updates.push_back(u);
		world.apply(block);
	};
	std::vector<net::NetworkableState> states((*layout)[0].groups.size());
	states[0].mask = 0x2 | 0x2000;
	states[0].position = {50, 10, 60};
	states[0].seatWeapon[0] = 1;
	states[2].mask = 0x1;
	states[2].rotation[0] = 25; // the navs (steering)
	states[3].springPresent = true;
	states[3].springOffset = {0, -0.1f, 0};
	update(300, (*layout)[0], states);
	root = world.object(300);
	CHECK(root && root->decoded && root->decodedAll && near(root->position.z, 60, 0.001f) && root->seatWeapon[0] == 1);
	const net::RemotePart* nav = root ? &root->parts[4] : nullptr;
	CHECK(nav && nav->name == "buggy_nav" && nav->hasRotation && near(nav->rotation[0], 25, 0.2f));
	const net::RemotePart* lf = root ? root->findPart("wheel_lf") : nullptr;
	CHECK(lf && lf->hasSpring && near(lf->springOffset.y, -0.1f, 0.002f));
	// The next update only moves the navs' pitch: the yaw stays.
	states = std::vector<net::NetworkableState>((*layout)[0].groups.size());
	states[2].mask = 0x2;
	states[2].rotation[1] = 5;
	update(300, (*layout)[0], states);
	root = world.object(300);
	CHECK(root && near(root->parts[2].rotation[0], 25, 0.2f) && near(root->parts[2].rotation[1], 5, 0.2f));

	// The gunner's seat: its weapon and the turret's yaw.
	std::vector<net::NetworkableState> seatStates((*layout)[1].groups.size());
	seatStates[0].mask = 0x1;
	seatStates[0].seatWeapon[0] = 0;
	seatStates[1].mask = 0x1;
	seatStates[1].rotation[0] = 170;
	update(301, (*layout)[1], seatStates);
	seat = world.object(301);
	CHECK(seat && seat->seatWeapon[0] == 0 && seat->findPart("turret_base") && near(seat->findPart("turret_base")->rotation[0], 170, 0.2f));

	// Entering the seat; destroying the car takes the seat's ghost too.
	net::GameEvent enter;
	enter.type = net::kEvEnterVehicle;
	enter.values = {2, 301, 0};
	world.apply(enter);
	CHECK(world.object(301) && world.object(301)->occupant == 2 && world.player(2)->vehicleGhost == 301);
	net::GameEvent exit;
	exit.type = net::kEvExitVehicle;
	exit.values = {2, 0};
	world.apply(exit);
	CHECK(world.object(301)->occupant == -1 && world.player(2)->vehicleGhost == 0);
	net::GameEvent destroy;
	destroy.type = net::kEvDestroyObject;
	destroy.values = {300, 0};
	world.apply(destroy);
	CHECK(!world.object(300) && !world.object(301));
}

// The level, map list, settings and round flow (data blocks 2, 5, 0;
// BeginRound, EndOfRound, the game status) and a map change.
TEST(NetGame_RoundFlowAndLevel)
{
	net::RemoteWorld world;
	auto transfer = [&](uint32_t type, const std::string& data) {
		world.apply(net::makeDataBlockStart(type, static_cast<uint32_t>(data.size())));
		for (size_t at = 0; at < data.size(); at += 63) {
			world.apply(net::makeDataBlockChunk(data.substr(at, 63)));
		}
	};
	net::RemoteWorld::ServerSettings settings;
	settings.name = "Titan Server";
	settings.welcome = {"Welcome", "to the server"};
	settings.allowFreeCam = true;
	settings.gameSpyPort = 29900;
	settings.gamePort = 17567;
	settings.radioSpamInterval = 6;
	settings.votingEnabled = true;
	settings.playersNeededToStart = 4;
	settings.allowTitanMovement = true;
	transfer(0, net::RemoteWorld::encodeSettings(settings));
	const net::RemoteWorld::ServerSettings got = world.settings();
	CHECK(got.complete && got.name == "Titan Server" && got.welcome.size() == 2 && got.welcome[1] == "to the server");
	CHECK(got.allowFreeCam && !got.allowNoseCam && got.gamePort == 17567 && got.radioSpamInterval == 6);
	CHECK(got.votingEnabled && got.playersNeededToStart == 4 && got.allowTitanMovement && world.serverName() == "Titan Server");

	transfer(5, net::RemoteWorld::encodeMapList({{"suez_canal", "gpm_cq", 64}, {"minsk", "gpm_ti", 32}}, 1));
	CHECK(world.mapList().size() == 2 && world.currentMapIndex() == 1 && world.mapList()[1].gameMode == "gpm_ti");

	net::RemoteWorld::LevelInfo level;
	level.gameMode = "gpm_ti";
	level.levelDirectory = "Levels/";
	level.mapName = "minsk";
	level.maxPlayers = 32;
	level.commander = true;
	level.seed = 7;
	transfer(2, net::RemoteWorld::encodeLevel(level));
	CHECK(world.hasLevel() && world.level().mapName == "minsk" && world.level().maxPlayers == 32 && world.level().commander);
	CHECK(world.takeLevelChanges().size() == 1 && world.levelsReceived() == 1);

	net::GameEvent begin;
	begin.type = net::kEvBeginRound;
	begin.values = {2, 4500, static_cast<uint32_t>(-1)};
	world.apply(begin);
	CHECK(world.roundStarted() && world.round().state == 2 && world.round().endTick == 4500 && world.round().playersNeeded == -1);
	world.apply(net::makePostRemote(0, 1, net::int32Payload(1)));
	CHECK(world.gameState() == 1);
	net::GameEvent end;
	end.type = net::kEvEndOfRound;
	end.values = {1, 2, 3};
	world.apply(end);
	world.apply(net::makePostRemote(0, 1, net::int32Payload(2)));
	CHECK(world.roundEnded() && !world.roundStarted() && world.endOfRound().roundsLeft == 1 && world.endOfRound().winner == 2);
	CHECK(world.endOfRound().victoryType == 3 && world.gameState() == 2);

	// The next map: objects go, the players stay.
	net::CreatePlayerInfo p;
	p.id = 1;
	p.name = "A";
	world.apply(net::makeCreatePlayer(p));
	net::CreateObjectInfo o;
	o.templateId = 5;
	o.ghost = 40;
	world.apply(net::makeCreateObject(o));
	transfer(5, net::RemoteWorld::encodeMapList({{"suez_canal", "gpm_cq", 64}, {"minsk", "gpm_ti", 32}}, 0));
	level.mapName = "suez_canal";
	level.gameMode = "gpm_cq";
	transfer(2, net::RemoteWorld::encodeLevel(level));
	CHECK(world.currentMapIndex() == 0 && world.level().mapName == "suez_canal" && world.levelsReceived() == 2);
	CHECK(world.objectList().empty() && world.player(1) && world.takeLevelChanges().size() == 1);
}

// Ghost layouts from the game's own templates: as_fav (a buggy with a
// gunner's seat) from Objects/Vehicles_server.zip.
TEST(NetGame_GhostLayoutFromGameTemplates)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		std::printf("  (BF2142_GAME_DIR not set: skipped)\n");
		return;
	}
	dice::hfe::io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	files.setMountingAllowed(true);
	CHECK(files.mountArchive("Objects/Vehicles_server.zip", "Objects\\Vehicles"));
	files.setMountingAllowed(false);
	dice::hfe::Console console {&files};
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::world::registerWorldConsoleMethods(console, templates, objects);
	dice::hfe::world::registerUntypedTemplateProperties(console, templates,
		{"networkableInfo", "hasMobilePhysics", "armor.maxHitPoints", "armor.wreckHitPoints", "maxSpeed", "minRotation",
			"maxRotation", "engineType", "numberOfGears", "maxAngleOfAttack", "hasCollisionPhysics", "physicsType", "grip"});
	console.registerConstant("c_ETNewCar2", "194"); // symbols/BF2142.exe.constants.csv
	std::string out;
	console.run("Objects/Vehicles/as_fav/as_fav.con", {}, out);
	const dice::hfe::world::ObjectTemplate* fav = templates.find("as_fav");
	CHECK(fav != nullptr);
	if (!fav) {
		return;
	}
	const net::GhostTemplateNode tree = net::ghostTemplateFrom(templates, *fav);
	CHECK(tree.controlObject && tree.hints.kind == net::NetworkableKind::SimpleObject && tree.hints.mobile);
	CHECK(tree.hints.hasArmor && tree.hints.maxHitPoints > 0);
	const std::vector<net::GhostPartLayout> layout = net::buildGhostLayout(tree);
	// The driver's ghost, the gunner's (AS_FAV_SecondPosition, a seat with
	// weapons) and the passenger's (as_fav_openPosition: hand weapons, so
	// its state is the occupant soldier's).
	CHECK(layout.size() == 3);
	if (layout.size() != 3) {
		return;
	}
	CHECK(layout[1].name == "AS_FAV_SecondPosition" && layout[1].groups.size() == 3);
	CHECK(layout[1].groups[0].hints.kind == net::NetworkableKind::PlayerControlObject && layout[1].groups[0].hints.seatCameras > 0);
	CHECK(layout[2].name == "as_fav_openPosition" && layout[2].groups.size() == 1 && layout[2].groups[0].hints.seatCameras == 0);
	size_t springs = 0, engines = 0;
	for (const net::GhostGroupLayout& g : layout[0].groups) {
		springs += g.hints.kind == net::NetworkableKind::Spring ? 1 : 0;
		engines += g.hints.kind == net::NetworkableKind::Engine ? 1 : 0;
	}
	// Root, steering wheel, engine, the navs (one group: same template), four wheels.
	CHECK(springs == 4 && engines == 1 && layout[0].groups.size() == 8);
	for (const net::GhostGroupLayout& g : layout[0].groups) {
		if (g.hints.kind == net::NetworkableKind::Engine) {
			CHECK(g.hints.engineType == 194 && g.hints.engineGears > 0);
		}
		if (g.hints.kind == net::NetworkableKind::Spring) {
			CHECK(g.hints.springExtra);
		}
	}
	std::printf("  as_fav: %zu + %zu + %zu groups\n", layout[0].groups.size(), layout[1].groups.size(), layout[2].groups.size());

	// eu_apc: the wheels with a networkable have grip 8 (EngineGrip), so
	// their ResponsePhysics has flag 8 and 0x00604f10 reads the extra
	// field; the middle ones have grip 128 (EngineDummyGrip) and no
	// networkableInfo, so no networkable at all.
	console.run("Objects/Vehicles/eu_apc/eu_apc.con", {}, out);
	const dice::hfe::world::ObjectTemplate* apc = templates.find("eu_apc");
	CHECK(apc != nullptr);
	if (!apc) {
		return;
	}
	int extra = 0;
	std::function<void(const net::GhostTemplateNode&)> walk = [&](const net::GhostTemplateNode& n) {
		if (n.hints.kind == net::NetworkableKind::Spring) {
			CHECK(n.hints.springExtra && n.hints.responseGrip == 8);
			++extra;
		}
		for (const net::GhostTemplateNode& c : n.children) {
			walk(c);
		}
	};
	walk(net::ghostTemplateFrom(templates, *apc));
	CHECK(extra == 4);
	if (const dice::hfe::world::ObjectTemplate* middle = templates.find("EU_APC_Wheel_LM")) {
		const net::NetworkableHints h = net::networkableHintsFrom(templates, *middle, true);
		CHECK(h.kind == net::NetworkableKind::Unknown);
	}
}
