#include "GameEvents.h"

#include "GameConnection.h"
#include "Md5.h"

#include <cstdio>

namespace dice::hfe::net {

std::string cdKeyResponse(const std::string& cdKey, const std::string& challenge, uint32_t random)
{
	char randomText[16];
	std::snprintf(randomText, sizeof(randomText), "%.8x", random);
	return md5Hex(cdKey) + randomText + md5Hex(cdKey + std::to_string(random % 0xffff) + challenge);
}

EventBlock readEventBlock(const std::vector<uint8_t>& payload)
{
	EventBlock block;
	BitReader in(payload.data(), payload.size());
	// The input block (0x00590990) comes first; a server writes a lone 0 bit.
	if (!readInputBlock(in, block.input)) {
		block.complete = false;
		return block;
	}
	block.inputPresent = block.input.present;
	block.present = in.read(1) == 1;
	if (!block.present || in.bad()) {
		block.complete = !in.bad();
		return block;
	}
	const uint32_t count = in.read(8);
	block.sequence = in.read(5);
	in.read(1); // resent (0x00509a90)
	for (uint32_t i = 0; i < count; ++i) {
		ReceivedEvent event;
		if (!readGameEvent(in, event.event)) {
			block.complete = false;
			break;
		}
		event.type = event.event.type;
		if (event.type == kCdKeyChallenge) { // 0x00509cc0: 10 bytes, then a counted string
			const std::string& text = event.event.text(0);
			event.challenge = text.substr(0, std::min(text.find('\0'), text.size()));
		} else if (event.type == kDataTransfer) { // 0x0050a1a0
			DataBlockChunk chunk;
			parseDataBlock(event.event, chunk);
			event.transferStart = chunk.start;
			event.transferType = chunk.type;
			event.transferSize = chunk.size;
			event.chunk = chunk.bytes;
		}
		block.events.push_back(std::move(event));
	}
	return block;
}

std::vector<uint8_t> connectionTypePayload(uint32_t blockSequence, uint32_t type)
{
	BitWriter out;
	out.write(0, 1);
	out.write(1, 1);
	out.write(1, 8);
	out.write(blockSequence & 0x1f, 5);
	out.write(0, 1);
	out.write(3, kEventTypeBits);
	out.write(type, 3); // 0x00508310(7): bits for 7 values
	return out.data();
}

std::vector<uint8_t> cdKeyResponsePayload(uint32_t blockSequence, const std::string& response, uint32_t mixed,
	uint32_t profileId)
{
	BitWriter out;
	out.write(0, 1); // no input block
	out.write(1, 1); // an event block
	out.write(1, 8);
	out.write(blockSequence & 0x1f, 5);
	out.write(0, 1);
	out.write(kCdKeyResponse, kEventTypeBits);
	out.writeText(response, 73); // 0x248 bits (0x00509eb0)
	out.write(mixed, 32);
	out.write(kBuild, 32);
	out.write(profileId, 32);
	return out.data();
}

} // namespace dice::hfe::net
