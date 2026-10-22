// Game events: the messages client and server exchange in data packets
// (BF2142.exe's event manager, vtable 0x00985b80; about 70 event types,
// each with a reader, writer and handler, 0x00508000-0x00515000).
//
// A data packet's payload is read by the connection's subsystems in order
// (0x0057a170): first one led by a presence bit (0x00590990, the player's
// input), then the event block (0x006447b0):
//   1 bit present; 8 bits count; 5 bits block sequence (blocks are handled
//   in order, modulo 32); 1 bit; then `count` events, each a 7-bit type
//   (enough for the event types registered) followed by its fields.
//
// Connecting players are challenged for their CD key: the server sends
// event 1 (0x00509cc0: the challenge, 10 bytes, and an 8-bit length with
// that many bytes), the client answers with event 2 (0x004b2790 builds it,
// 0x00509eb0 writes it): GameSpy's CD-key response (73 bytes, 0x00719260),
// a number mixed with the challenge, the build (0x1a007000) and the
// player's profile id (-1 without a login).
#pragma once

#include "BitStream.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::net {

constexpr unsigned kEventTypeBits = 7;

enum EventType : uint32_t {
	kCdKeyChallenge = 1,
	kCdKeyResponse = 2,
	kDataTransfer = 4,
};

// GameSpy's CD-key response (gcd_compute_response, 0x00719260):
// md5(key) + %.8x of `random` + md5(key + (random % 0xffff) + challenge).
std::string cdKeyResponse(const std::string& cdKey, const std::string& challenge, uint32_t random);

struct ReceivedEvent {
	uint32_t type;
	std::string challenge; // kCdKeyChallenge
	// kDataTransfer (0x0050a1a0): a start (two 32-bit values: what is sent
	// - 0 the server's settings, 5 the map list - and its size) or a chunk of
	// up to 255 bytes (servers send 63), for the connection's transfer
	// (0x0050a3a0; the sender's queue is 0x00643720).
	bool transferStart = false;
	uint32_t transferType = 0, transferSize = 0;
	std::string chunk;
};

struct EventBlock {
	bool inputPresent = false; // the first subsystem's block (not decoded yet)
	bool present = false;
	uint32_t sequence = 0;
	std::vector<ReceivedEvent> events;
	bool complete = true; // false when an event type is not decoded yet
};

// Decodes the event block of a data payload (the bytes after the 16-bit
// length).
EventBlock readEventBlock(const std::vector<uint8_t>& payload);

// A payload carrying one event block with event 3, the connection type
// (0x004b48a0 / 0x0050a070: an index 0-6 into the rate settings).
std::vector<uint8_t> connectionTypePayload(uint32_t blockSequence, uint32_t type);

// A payload carrying one event block with the CD-key response.
std::vector<uint8_t> cdKeyResponsePayload(uint32_t blockSequence, const std::string& response, uint32_t mixed,
	uint32_t profileId);

} // namespace dice::hfe::net
