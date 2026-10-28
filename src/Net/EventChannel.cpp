#include "EventChannel.h"

#include "BitStream.h"

#include <algorithm>

namespace dice::hfe::net {

std::optional<ReliableEventBlock> EventChannel::next()
{
	while (!m_resend.empty()) {
		const uint32_t sequence = m_resend.front();
		m_resend.pop_front();
		const auto it = m_unacked.find(sequence);
		if (it != m_unacked.end()) {
			ReliableEventBlock block = it->second;
			block.resent = true;
			++m_blocksResent;
			return block;
		}
	}
	if (m_outgoing.empty() || m_unacked.size() >= 15) {
		return std::nullopt;
	}
	ReliableEventBlock block;
	block.sequence = m_nextOutgoing;
	size_t bits = 0;
	while (!m_outgoing.empty() && block.events.size() < 255) {
		BitWriter one;
		writeGameEvent(one, m_outgoing.front());
		if (!block.events.empty() && bits + one.bits() > 8 * 1000) {
			break;
		}
		bits += one.bits();
		block.events.push_back(m_outgoing.front());
		m_outgoing.pop_front();
	}
	m_unacked[block.sequence] = block;
	m_nextOutgoing = (m_nextOutgoing + 1) & 0x1f;
	return block;
}

void EventChannel::acknowledge(uint32_t acknowledged, uint32_t mask)
{
	for (auto it = m_packetBlock.begin(); it != m_packetBlock.end();) {
		const uint32_t behind = (acknowledged - it->first) & 0x3f;
		if (behind >= 32) {
			++it; // not acknowledged either way yet
			continue;
		}
		const uint32_t blockSequence = it->second;
		if ((mask >> behind & 1u) != 0) {
			m_unacked.erase(blockSequence);
		} else if (m_unacked.count(blockSequence)
			&& std::find(m_resend.begin(), m_resend.end(), blockSequence) == m_resend.end()) {
			m_resend.push_back(blockSequence);
		}
		it = m_packetBlock.erase(it);
	}
}

void EventChannel::receive(const ReliableEventBlock& block, const std::function<void(const GameEvent&)>& deliver)
{
	const uint32_t ahead = (block.sequence - m_nextIncoming) & 0x1f;
	if (ahead >= 16 || m_pendingIncoming.count(block.sequence)) {
		return; // a repeat of one taken already, or too far ahead
	}
	m_pendingIncoming[block.sequence] = block.events;
	for (auto it = m_pendingIncoming.find(m_nextIncoming); it != m_pendingIncoming.end();
		 it = m_pendingIncoming.find(m_nextIncoming)) {
		const std::vector<GameEvent> events = std::move(it->second);
		m_pendingIncoming.erase(it);
		m_nextIncoming = (m_nextIncoming + 1) & 0x1f;
		for (const GameEvent& event : events) {
			deliver(event);
		}
	}
}

} // namespace dice::hfe::net
