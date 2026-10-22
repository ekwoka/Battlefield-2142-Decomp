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
	block.inputPresent = in.read(1) == 1;
	if (block.inputPresent) {
		block.complete = false;
		return block;
	}
	block.present = in.read(1) == 1;
	if (!block.present) {
		return block;
	}
	const uint32_t count = in.read(8);
	block.sequence = in.read(5);
	in.read(1);
	for (uint32_t i = 0; i < count && !in.bad(); ++i) {
		ReceivedEvent event;
		event.type = in.read(kEventTypeBits);
		if (event.type == kCdKeyChallenge) { // 0x00509cc0
			event.challenge = in.readText(10);
			const uint32_t length = in.read(8);
			in.readText(length);
		} else if (event.type == kDataTransfer) {
			event.transferStart = in.read(1) == 1;
			if (event.transferStart) {
				event.transferType = in.read(32);
				event.transferSize = in.read(32);
			} else {
				const uint32_t length = in.read(8);
				for (uint32_t b = 0; b < length; ++b) {
					event.chunk.push_back(static_cast<char>(in.read(8)));
				}
			}
		} else {
			block.events.push_back(event);
			block.complete = false;
			break;
		}
		block.events.push_back(event);
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
