#include "AnimationFiles.h"

#include <cstring>

namespace dice::hfe::anim {

namespace {

class Reader {
public:
	explicit Reader(const std::vector<uint8_t>& data) : m_data(data) {}
	bool ok() const { return m_ok; }
	bool atEnd() const { return m_pos == m_data.size(); }
	size_t position() const { return m_pos; }

	template <class T>
	T read()
	{
		T value {};
		if (!m_ok || m_data.size() - m_pos < sizeof(T)) {
			m_ok = false;
			return value;
		}
		std::memcpy(&value, m_data.data() + m_pos, sizeof(T));
		m_pos += sizeof(T);
		return value;
	}

	std::string string(size_t length)
	{
		if (!m_ok || m_data.size() - m_pos < length) {
			m_ok = false;
			return {};
		}
		std::string text(reinterpret_cast<const char*>(m_data.data() + m_pos), length);
		m_pos += length;
		const size_t terminator = text.find('\0');
		if (terminator != std::string::npos) {
			text.resize(terminator);
		}
		return text;
	}

private:
	const std::vector<uint8_t>& m_data;
	size_t m_pos = 0;
	bool m_ok = true;
};

template <class T>
std::optional<T> failure(std::string* error, const char* why)
{
	if (error) {
		*error = why;
	}
	return std::nullopt;
}

// One run-length-encoded channel; false if it doesn't hold `frames` values.
bool readChannel(Reader& r, uint32_t frames, std::vector<int16_t>& values)
{
	const uint16_t words = r.read<uint16_t>();
	const size_t start = r.position();
	values.clear();
	while (r.ok() && values.size() < frames) {
		const uint8_t header = r.read<uint8_t>();
		r.read<uint8_t>(); // offset of the next header
		const uint32_t count = header & 0x7f;
		if (header & 0x80) {
			values.insert(values.end(), count, r.read<int16_t>());
		} else {
			for (uint32_t i = 0; i < count; ++i) {
				values.push_back(r.read<int16_t>());
			}
		}
	}
	return r.ok() && values.size() == frames && r.position() - start == static_cast<size_t>(words) * 2;
}

} // namespace

int Skeleton::find(const std::string& name) const
{
	for (size_t i = 0; i < nodes.size(); ++i) {
		if (nodes[i].name == name) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

std::optional<Skeleton> parseSkeleton(const std::vector<uint8_t>& data, std::string* error)
{
	Reader r(data);
	if (r.read<uint32_t>() != 2) {
		return failure<Skeleton>(error, "unsupported skeleton version");
	}
	const uint32_t count = r.read<uint32_t>();
	if (count > 4096) {
		return failure<Skeleton>(error, "too many nodes");
	}
	Skeleton skeleton;
	skeleton.nodes.resize(count);
	for (SkeletonNode& node : skeleton.nodes) {
		node.name = r.string(r.read<uint16_t>());
		node.parent = r.read<int16_t>();
		for (float& q : node.rotation) {
			q = r.read<float>();
		}
		for (float& p : node.position) {
			p = r.read<float>();
		}
		if (node.parent >= static_cast<int>(count)) {
			return failure<Skeleton>(error, "parent index out of range");
		}
	}
	if (!r.ok() || !r.atEnd()) {
		return failure<Skeleton>(error, "malformed skeleton");
	}
	return skeleton;
}

std::optional<Animation> parseAnimation(const std::vector<uint8_t>& data, std::string* error)
{
	Reader r(data);
	if (r.read<uint32_t>() != 4) {
		return failure<Animation>(error, "unsupported animation version");
	}
	Animation animation;
	animation.bones.resize(r.read<uint16_t>());
	for (BoneTrack& bone : animation.bones) {
		bone.node = r.read<uint16_t>();
	}
	animation.frameCount = r.read<uint32_t>();
	animation.precision = r.read<uint8_t>();
	if (!r.ok() || animation.frameCount > 100000 || animation.precision > 31) {
		return failure<Animation>(error, "malformed header");
	}
	const float rotationScale = 1.0f / 32767.0f;
	const float positionScale = 1.0f / static_cast<float>(1u << animation.precision);
	std::vector<int16_t> channel;
	for (BoneTrack& bone : animation.bones) {
		r.read<uint16_t>(); // total channel length
		bone.rotations.resize(animation.frameCount);
		bone.positions.resize(animation.frameCount);
		for (int c = 0; c < 7; ++c) {
			if (!readChannel(r, animation.frameCount, channel)) {
				return failure<Animation>(error, "malformed channel");
			}
			for (uint32_t f = 0; f < animation.frameCount; ++f) {
				if (c < 4) {
					bone.rotations[f][static_cast<size_t>(c)] = channel[f] * rotationScale;
				} else {
					bone.positions[f][static_cast<size_t>(c - 4)] = channel[f] * positionScale;
				}
			}
		}
	}
	if (!r.atEnd()) {
		return failure<Animation>(error, "unexpected data at the end");
	}
	return animation;
}

} // namespace dice::hfe::anim
