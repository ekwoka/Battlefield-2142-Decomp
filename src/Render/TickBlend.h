// Drawing between the fixed 30 Hz simulation ticks, as the original's frame
// does (0x0040bd90): after the ticks it stores their count (0x0040c300 ->
// 0x0043ec00), then before drawing takes alpha = (now - last tick) /
// (count x 1/30 s), clamped to -0.1 .. 1.1 and 0 before any tick
// (0x0040c534 -> 0x0043f170), and draws each object between its previous
// and its current transform (0x0043ef30, always on: 0x009fcd90 is 1, read
// nowhere else). An object whose height changed by more than 4 m
// (0x00920ca0) is drawn where it is.
#pragma once

#include "Physics/RigidBody.h"
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

// The inverse of a row-vector affine transform (axes may be scaled).
inline Mat4 affineInverse(const Mat4& m)
{
	const float a = m.at(0, 0), b = m.at(0, 1), c = m.at(0, 2), d = m.at(1, 0), e = m.at(1, 1), f = m.at(1, 2),
				g = m.at(2, 0), h = m.at(2, 1), k = m.at(2, 2);
	const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
	if (std::fabs(det) < 1e-12f) {
		return {};
	}
	const float s = 1 / det;
	Mat4 r;
	r.at(0, 0) = (e * k - f * h) * s;
	r.at(0, 1) = (c * h - b * k) * s;
	r.at(0, 2) = (b * f - c * e) * s;
	r.at(1, 0) = (f * g - d * k) * s;
	r.at(1, 1) = (a * k - c * g) * s;
	r.at(1, 2) = (c * d - a * f) * s;
	r.at(2, 0) = (d * h - e * g) * s;
	r.at(2, 1) = (b * g - a * h) * s;
	r.at(2, 2) = (a * e - b * d) * s;
	for (int j = 0; j < 3; ++j) {
		r.at(3, j) = -(m.at(3, 0) * r.at(0, j) + m.at(3, 1) * r.at(1, j) + m.at(3, 2) * r.at(2, j));
	}
	return r;
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
	const auto rotation = [](const Mat4& m) {
		physics::Mat3 r;
		for (int i = 0; i < 3; ++i) {
			r.rows[static_cast<size_t>(i)] = {m.at(i, 0), m.at(i, 1), m.at(i, 2)};
		}
		return r;
	};
	const physics::Mat3 r = physics::slerpRotation(rotation(from), rotation(to), alpha);
	Mat4 out = to;
	for (int i = 0; i < 3; ++i) {
		const physics::Vec3& row = r.rows[static_cast<size_t>(i)];
		out.at(i, 0) = row.x;
		out.at(i, 1) = row.y;
		out.at(i, 2) = row.z;
	}
	for (int j = 0; j < 3; ++j) {
		out.at(3, j) = from.at(3, j) + (to.at(3, j) - from.at(3, j)) * alpha;
	}
	return out;
}

} // namespace dice::hfe::render
