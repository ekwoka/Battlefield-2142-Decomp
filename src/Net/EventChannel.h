// Reliable, ordered event blocks over the notify layer, the same on both
// sides of a connection (BF2142.exe's event manager per connection: the
// writer 0x00644bf0, the reader 0x006447b0).
//
// Sending: queued events go out in a block (at most 255 events, about a
// kilobyte) with a 5-bit sequence; a block whose packet the peer reports
// lost (its notify acknowledgement, GameConnection.h) goes out again with
// the resent bit before anything new; at most 15 blocks are in flight.
// Receiving: blocks are handed on in sequence order (modulo 32), repeats
// and blocks more than 15 ahead dropped.
#pragma once

#include "GameProtocol.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>

namespace dice::hfe::net {

class EventChannel {
public:
	void queue(const GameEvent& event) { m_outgoing.push_back(event); }
	bool idle() const { return m_outgoing.empty() && m_unacked.empty(); }
	// The block for the next packet: a lost one first, else new events
	// (none when there is nothing to send or 15 are in flight).
	std::optional<ReliableEventBlock> next();
	// The packet sequence the block from next() went out with.
	void sent(uint32_t packetSequence, uint32_t blockSequence) { m_packetBlock[packetSequence & 0x3f] = blockSequence; }
	// The peer's acknowledgement of our packets: the last it got and the
	// mask of the 32 before (bit 0 that last one).
	void acknowledge(uint32_t acknowledged, uint32_t mask);
	// A received block: its events are delivered once every block before it was.
	void receive(const ReliableEventBlock& block, const std::function<void(const GameEvent&)>& deliver);
	size_t blocksResent() const { return m_blocksResent; }
	void clear() { *this = EventChannel(); }

private:
	std::deque<GameEvent> m_outgoing;
	uint32_t m_nextOutgoing = 0;
	std::map<uint32_t, ReliableEventBlock> m_unacked; // by block sequence
	std::map<uint32_t, uint32_t> m_packetBlock;       // packet sequence -> block sequence
	std::deque<uint32_t> m_resend;                    // block sequences lost
	uint32_t m_nextIncoming = 0;
	std::map<uint32_t, std::vector<GameEvent>> m_pendingIncoming;
	size_t m_blocksResent = 0;
};

} // namespace dice::hfe::net
