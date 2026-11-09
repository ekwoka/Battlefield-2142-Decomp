// The enhanced mode's render maths (LevelRendererEnhanced.cpp), header-only
// for the tests: TAA's sub-pixel jitter, a 4x4 inverse, and the cascaded
// shadow maps' splits and bounding spheres. Not retail: RendDX9 has none.
#pragma once

#include "Render/RenderMath.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace dice::hfe::render::enhanced {

// The radical inverse of `index` in `base` (the Halton sequence), 0..1.
inline float halton(int index, int base)
{
	float result = 0, fraction = 1;
	for (int i = index; i > 0; i /= base) {
		fraction /= static_cast<float>(base);
		result += fraction * static_cast<float>(i % base);
	}
	return result;
}

// Frame `frame`'s sub-pixel offset in clip space for a width x height
// target: Halton (2, 3) over 8 frames, within half a pixel.
inline std::array<float, 2> jitterOffset(unsigned frame, uint32_t width, uint32_t height)
{
	const int i = static_cast<int>(frame % 8) + 1;
	return {(halton(i, 2) - 0.5f) * 2.0f / static_cast<float>(std::max<uint32_t>(width, 1)),
		(halton(i, 3) - 0.5f) * 2.0f / static_cast<float>(std::max<uint32_t>(height, 1))};
}

// Post-multiplied with a view-projection (row vectors), it moves clip x and
// y by the offset times w: the whole image shifts by it.
inline Mat4 jitterMatrix(const std::array<float, 2>& offset)
{
	Mat4 j;
	j.at(3, 0) = offset[0];
	j.at(3, 1) = offset[1];
	return j;
}

// The inverse of a 4x4 matrix (cofactors); identity if it is singular.
inline Mat4 inverse(const Mat4& a)
{
	const auto& m = a.m;
	std::array<float, 16> inv {};
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	Mat4 r;
	if (std::fabs(det) < 1e-30f) {
		return r;
	}
	for (int i = 0; i < 16; ++i) {
		r.m[static_cast<size_t>(i)] = inv[static_cast<size_t>(i)] / det;
	}
	return r;
}

constexpr int kCascades = 4;

// Where each cascade ends along the view (metres): the practical split
// (log and linear blended by `lambda`) from `nearPlane` to `farPlane`.
inline std::array<float, kCascades> cascadeSplits(float nearPlane, float farPlane, float lambda = 0.75f)
{
	std::array<float, kCascades> far {};
	for (int i = 1; i <= kCascades; ++i) {
		const float t = static_cast<float>(i) / kCascades;
		const float logSplit = nearPlane * std::pow(farPlane / nearPlane, t);
		const float linSplit = nearPlane + (farPlane - nearPlane) * t;
		far[static_cast<size_t>(i - 1)] = lambda * logSplit + (1 - lambda) * linSplit;
	}
	far[kCascades - 1] = farPlane;
	return far;
}

// The smallest sphere about the view axis that holds the frustum's slice
// from `n` to `f` metres along it: its distance along the axis and radius.
// `tanX` / `tanY` are the tangents of the half fields of view.
struct SliceSphere {
	float distance;
	float radius;
};
inline SliceSphere sliceSphere(float n, float f, float tanX, float tanY)
{
	const float k2 = tanX * tanX + tanY * tanY;
	const float z = std::clamp((f + n) * (1 + k2) * 0.5f, n, f);
	const float farCorner = (f - z) * (f - z) + f * f * k2;
	const float nearCorner = (z - n) * (z - n) + n * n * k2;
	return {z, std::sqrt(std::max(farCorner, nearCorner))};
}

} // namespace dice::hfe::render::enhanced
