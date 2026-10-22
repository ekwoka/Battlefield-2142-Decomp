// Evaluating a skeleton's pose: node world matrices from the setup pose,
// optionally overridden by an animation frame.
//
// Matrices are Direct3D-style (row vectors, v' = v * M), stored row-major,
// matching the renderer. Quaternions are (x, y, z, w) and convert as
// D3DXMatrixRotationQuaternion does. A node's world matrix is its local
// matrix times its parent's world matrix.
//
// Skinned meshes store per bone the matrix from mesh space to bone space
// (an inverse bind matrix), so a vertex skinned to bone b ends up at
// v * rig[b] * world[node(b)].
#pragma once

#include "AnimationFiles.h"

#include <array>
#include <vector>

namespace dice::hfe::anim {

using Matrix = std::array<float, 16>;

Matrix matrixFromRotationTranslation(const std::array<float, 4>& quaternion, const std::array<float, 3>& translation);
Matrix multiply(const Matrix& a, const Matrix& b);

using Quaternion = std::array<float, 4>;
using Position = std::array<float, 3>;

// Bone animations play at 24 frames per second (0x008c1c..: length =
// frames * 0.041666668).
constexpr float kFramesPerSecond = 24.0f;

Quaternion slerp(const Quaternion& a, Quaternion b, float t);

// Each node's rotation and position relative to its parent.
struct LocalPose {
	std::vector<Quaternion> rotations;
	std::vector<Position> positions;
};
LocalPose setupPose(const Skeleton& skeleton);

// A bone of an animation at a fraction of its length (0x008c1830): looping
// animations wrap from the last frame to the first, others stop at their
// ends. False if the animation does not move that skeleton node.
bool sampleNode(const Animation& animation, size_t node, float fraction, bool looping, Quaternion& rotation,
	Position& position);

// World matrices from local transforms; parents precede children.
std::vector<Matrix> worldMatrices(const Skeleton& skeleton, const LocalPose& pose);

// World matrices of all skeleton nodes. With an animation, `frame` (may be
// fractional; wraps) selects the pose of the animated nodes, interpolated
// between frames; other nodes keep their setup pose.
std::vector<Matrix> evaluatePose(const Skeleton& skeleton, const Animation* animation, float frame);

} // namespace dice::hfe::anim
