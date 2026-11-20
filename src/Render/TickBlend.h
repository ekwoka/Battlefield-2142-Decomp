// Drawing between the fixed 30 Hz simulation ticks, as the original's frame
// does (0x0040bd90): after the ticks it stores their count (0x0040c300 ->
// 0x0043ec00), then before drawing takes alpha = (now - last tick) /
// (count x 1/30 s), clamped to -0.1 .. 1.1 and 0 before any tick
// (0x0040c534 -> 0x0043f170), and draws each object between its previous
// and its current transform (0x0043ef30, always on: 0x009fcd90 is 1, read
// nowhere else). An object whose height changed by more than 4 m
// (0x00920ca0) is drawn where it is.
#pragma once

#include "RenderMath.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::render {

struct TickBlend {
	bool ticked = true; // the simulation stepped since the last sync
	float alpha = 1;    // 1: draw the current transforms (screenshots, tests)
};

constexpr float kTickBlendJump = 4.0f;

inline float tickBlendAlpha(double now, double lastTick, int ticks, float tickLength)
{
	if (ticks <= 0) {
		return 0.0f;
	}
	return std::clamp(static_cast<float>((now - lastTick) / (ticks * tickLength)), -0.1f, 1.1f);
}

inline bool tickBlendJumped(float fromY, float toY) { return std::fabs(toY - fromY) > kTickBlendJump; }

// Row-vector world adjustment: keep a model's coordinates in the raw view
// unchanged when it is drawn through the smoothed view. Views are rigid.
inline Mat4 tickBlendViewAdjustment(const Mat4& rawView, const Mat4& drawnView)
{
	if (rawView.m == drawnView.m) {
		return {};
	}
	Mat4 inverse;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			inverse.at(i, j) = drawnView.at(j, i);
		}
	}
	for (int i = 0; i < 3; ++i) {
		inverse.at(3, i) = -(drawnView.at(3, 0) * inverse.at(0, i)
			+ drawnView.at(3, 1) * inverse.at(1, i) + drawnView.at(3, 2) * inverse.at(2, i));
	}
	return rawView * inverse;
}

// A row-vector transform between two, as 0x0043ef30: the translation
// linearly (0x0043dd80), the rotation as quaternions slerped by alpha
// (0x006ac740 / 0x0043de60 to quaternions, 0x006ac980 the slerp, the short
// way; 0x006ac870 back). The axis lengths, which retail's matrices don't
// carry, are lerped so a scaled transform keeps its size.
inline Mat4 blendTransform(const Mat4& from, const Mat4& to, float alpha)
{
	if (alpha == 1.0f) {
		return to;
	}
	struct Quat { float w, x, y, z; };
	float length[2][3];
	const auto quat = [&length](const Mat4& m, int which, Quat& q) {
		float r[3][3];
		for (int i = 0; i < 3; ++i) {
			const float l = std::sqrt(m.at(i, 0) * m.at(i, 0) + m.at(i, 1) * m.at(i, 1) + m.at(i, 2) * m.at(i, 2));
			if (l <= 0) {
				return false;
			}
			length[which][i] = l;
			for (int j = 0; j < 3; ++j) {
				r[i][j] = m.at(i, j) / l;
			}
		}
		const float trace = r[0][0] + r[1][1] + r[2][2];
		if (trace > 0) {
			const float s = 2 * std::sqrt(trace + 1);
			q = {s / 4, (r[1][2] - r[2][1]) / s, (r[2][0] - r[0][2]) / s, (r[0][1] - r[1][0]) / s};
		} else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
			const float s = 2 * std::sqrt(1 + r[0][0] - r[1][1] - r[2][2]);
			q = {(r[1][2] - r[2][1]) / s, s / 4, (r[1][0] + r[0][1]) / s, (r[2][0] + r[0][2]) / s};
		} else if (r[1][1] > r[2][2]) {
			const float s = 2 * std::sqrt(1 + r[1][1] - r[0][0] - r[2][2]);
			q = {(r[2][0] - r[0][2]) / s, (r[1][0] + r[0][1]) / s, s / 4, (r[2][1] + r[1][2]) / s};
		} else {
			const float s = 2 * std::sqrt(1 + r[2][2] - r[0][0] - r[1][1]);
			q = {(r[0][1] - r[1][0]) / s, (r[2][0] + r[0][2]) / s, (r[2][1] + r[1][2]) / s, s / 4};
		}
		return true;
	};
	Quat a, b;
	if (!quat(from, 0, a) || !quat(to, 1, b)) {
		return to;
	}
	float cosine = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
	if (cosine < 0) {
		cosine = -cosine;
		b = {-b.w, -b.x, -b.y, -b.z};
	}
	float wa = 1 - alpha, wb = alpha;
	if (1 - cosine > 1e-4f) {
		const float angle = std::acos(std::min(cosine, 1.0f)), sine = std::sin(angle);
		wa = std::sin((1 - alpha) * angle) / sine;
		wb = std::sin(alpha * angle) / sine;
	}
	Quat q {wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z};
	const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
	q = {q.w / n, q.x / n, q.y / n, q.z / n};
	const float r[3][3] = {
		{1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y + q.w * q.z), 2 * (q.x * q.z - q.w * q.y)},
		{2 * (q.x * q.y - q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.w * q.x)},
		{2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)},
	};
	Mat4 out = to;
	for (int i = 0; i < 3; ++i) {
		const float l = length[0][i] + (length[1][i] - length[0][i]) * alpha;
		for (int j = 0; j < 3; ++j) {
			out.at(i, j) = r[i][j] * l;
		}
	}
	for (int j = 0; j < 3; ++j) {
		out.at(3, j) = from.at(3, j) + (to.at(3, j) - from.at(3, j)) * alpha;
	}
	return out;
}

} // namespace dice::hfe::render
