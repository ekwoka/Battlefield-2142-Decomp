#include "Pose.h"

#include <cmath>

namespace dice::hfe::anim {

Matrix matrixFromRotationTranslation(const std::array<float, 4>& q, const std::array<float, 3>& t)
{
	// The files' quaternions rotate column vectors; our matrices act on row
	// vectors (v * M), so this is the matrix of the conjugate.
	const float x = -q[0], y = -q[1], z = -q[2], w = q[3];
	return {
		1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
		2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
		2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
		t[0], t[1], t[2], 1,
	};
}

Matrix multiply(const Matrix& a, const Matrix& b)
{
	Matrix r {};
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			float sum = 0;
			for (int k = 0; k < 4; ++k) {
				sum += a[i * 4 + k] * b[k * 4 + j];
			}
			r[i * 4 + j] = sum;
		}
	}
	return r;
}

Quaternion slerp(const Quaternion& a, Quaternion b, float t)
{
	float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	if (dot < 0) {
		dot = -dot;
		for (float& v : b) {
			v = -v;
		}
	}
	std::array<float, 4> r;
	if (dot > 0.9995f) {
		for (int i = 0; i < 4; ++i) {
			r[i] = a[i] + (b[i] - a[i]) * t;
		}
	} else {
		const float angle = std::acos(dot);
		const float s = std::sin(angle);
		const float wa = std::sin((1 - t) * angle) / s;
		const float wb = std::sin(t * angle) / s;
		for (int i = 0; i < 4; ++i) {
			r[i] = a[i] * wa + b[i] * wb;
		}
	}
	const float length = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
	if (length > 0) {
		for (float& v : r) {
			v /= length;
		}
	}
	return r;
}

LocalPose setupPose(const Skeleton& skeleton)
{
	LocalPose pose;
	for (const SkeletonNode& node : skeleton.nodes) {
		pose.rotations.push_back(node.rotation);
		pose.positions.push_back(node.position);
	}
	return pose;
}

bool sampleNode(const Animation& animation, size_t node, float fraction, bool looping, Quaternion& rotation,
	Position& position)
{
	const BoneTrack* track = nullptr;
	for (const BoneTrack& bone : animation.bones) {
		if (bone.node == node) {
			track = &bone;
			break;
		}
	}
	const uint32_t frames = animation.frameCount;
	if (!track || frames == 0 || track->rotations.empty()) {
		return false;
	}
	// Looping animations interpolate over all frames back to the first;
	// others over frames - 1 and hold their ends.
	const uint32_t span = looping ? frames : frames - 1;
	uint32_t f0 = 0, f1 = 0;
	float t = 0;
	if (!looping && fraction >= 1) {
		f0 = span > 0 ? span - 1 : 0;
		f1 = span;
		t = 1;
	} else if (!looping && fraction <= 0) {
		f0 = 0;
		f1 = frames > 1 ? 1 : 0;
	} else {
		float whole = fraction - std::floor(fraction);
		const float at = whole * static_cast<float>(span);
		f0 = static_cast<uint32_t>(at);
		t = at - std::floor(at);
		if (f0 >= frames) {
			f0 = 0;
		}
		f1 = f0 + 1 >= frames ? 0 : f0 + 1;
	}
	const Quaternion& q0 = track->rotations[f0];
	if (q0[0] == 0 && q0[1] == 0 && q0[2] == 0 && q0[3] == 0) {
		return false; // unused bone
	}
	rotation = slerp(q0, track->rotations[f1], t);
	for (size_t i = 0; i < 3; ++i) {
		position[i] = track->positions[f0][i] * (1 - t) + track->positions[f1][i] * t;
	}
	return true;
}

std::vector<Matrix> worldMatrices(const Skeleton& skeleton, const LocalPose& pose)
{
	std::vector<Matrix> world(skeleton.nodes.size());
	for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
		const Matrix local = matrixFromRotationTranslation(pose.rotations[i], pose.positions[i]);
		const int parent = skeleton.nodes[i].parent;
		world[i] = parent >= 0 && static_cast<size_t>(parent) < i ? multiply(local, world[static_cast<size_t>(parent)]) : local;
	}
	return world;
}

std::vector<Matrix> evaluatePose(const Skeleton& skeleton, const Animation* animation, float frame)
{
	LocalPose pose = setupPose(skeleton);
	if (animation && animation->frameCount > 0) {
		const float fraction = frame / static_cast<float>(animation->frameCount);
		for (const BoneTrack& track : animation->bones) {
			if (track.node < skeleton.nodes.size()) {
				sampleNode(*animation, track.node, fraction, true, pose.rotations[track.node], pose.positions[track.node]);
			}
		}
	}
	return worldMatrices(skeleton, pose);
}

} // namespace dice::hfe::anim
