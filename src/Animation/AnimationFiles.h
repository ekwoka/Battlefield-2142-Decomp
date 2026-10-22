// Skeletons (.ske) and bone animations (.baf) of the BF2 engine
// (BF2/Animation/BoneAnimation in DICE's tree).
//
// .ske (version 2): u32 version, u32 node count, then per node a u16 name
// length and name, s16 parent index (-1 for the root), a rotation quaternion
// (x, y, z, w floats) and a position (3 floats), both relative to the parent.
//
// .baf (version 4): u32 version, u16 bone count, u16 skeleton node index per
// bone, u32 frame count, u8 position precision. Then per bone a u16 total
// length (in 16-bit words, excluding the channel lengths) and 7 channels:
// rotation x, y, z, w and position x, y, z. A channel is a u16 length in
// words followed by runs: a header byte (bit 7: repeat, bits 0-6: frames),
// a byte giving the offset of the next header, and either one s16 value
// repeated or one s16 per frame. Rotations are value / 32767, positions
// value / 2^precision metres.
//
// The layouts follow the BF2 modding community's documentation and were
// checked against all 96 skeletons and 5,585 animations of BF2142.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::anim {

struct SkeletonNode {
	std::string name;
	int parent; // -1 for the root
	std::array<float, 4> rotation; // quaternion x, y, z, w
	std::array<float, 3> position;
};

struct Skeleton {
	std::vector<SkeletonNode> nodes;
	int find(const std::string& name) const; // -1 if missing
};

struct BoneTrack {
	uint16_t node; // index into the skeleton
	std::vector<std::array<float, 4>> rotations; // one per frame
	std::vector<std::array<float, 3>> positions;
};

struct Animation {
	uint32_t frameCount = 0;
	uint8_t precision = 0;
	std::vector<BoneTrack> bones;
};

std::optional<Skeleton> parseSkeleton(const std::vector<uint8_t>& data, std::string* error = nullptr);
std::optional<Animation> parseAnimation(const std::vector<uint8_t>& data, std::string* error = nullptr);

} // namespace dice::hfe::anim
