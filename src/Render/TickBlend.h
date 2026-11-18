// Drawing between the fixed 30 Hz simulation ticks, as the original's frame
// does (0x0040bd90): after the ticks it stores their count (0x0040c300 ->
// 0x0043ec00), then before drawing takes alpha = (now - last tick) /
// (count x 1/30 s), clamped to -0.1 .. 1.1 and 0 before any tick
// (0x0040c534 -> 0x0043f170), and draws each object between its previous
// and its current transform (0x0043ef30, always on: 0x009fcd90 is 1). An
// object whose height changed by more than 4 m (0x00920ca0) is drawn where it is.
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

// A row-vector transform between two: the translation row linearly; the axis
// rows linearly, then made orthogonal again with their lengths kept, so a turn
// neither shrinks nor shears what is drawn. A turn of more than 60 degrees in
// one tick (a rotor) is not blended.
inline Mat4 blendTransform(const Mat4& from, const Mat4& to, float alpha)
{
	if (alpha == 1.0f) {
		return to;
	}
	const auto row = [](const Mat4& m, int i) { return Vec3f {m.at(i, 0), m.at(i, 1), m.at(i, 2)}; };
	Vec3f axis[3];
	float length[3];
	for (int i = 0; i < 3; ++i) {
		const Vec3f a = row(from, i), b = row(to, i);
		const float la = std::sqrt(dot(a, a)), lb = std::sqrt(dot(b, b));
		if (la <= 0 || lb <= 0 || dot(a, b) < 0.5f * la * lb) {
			return to;
		}
		axis[i] = a + (b - a) * alpha;
		length[i] = la + (lb - la) * alpha;
	}
	const Vec3f x = normalize(axis[0]);
	const Vec3f y = normalize(axis[1] - x * dot(x, axis[1]));
	const Vec3f z = normalize(axis[2] - x * dot(x, axis[2]) - y * dot(y, axis[2]));
	Mat4 r = to;
	const Vec3f rows[3] = {x * length[0], y * length[1], z * length[2]};
	for (int i = 0; i < 3; ++i) {
		r.at(i, 0) = rows[i].x;
		r.at(i, 1) = rows[i].y;
		r.at(i, 2) = rows[i].z;
	}
	for (int j = 0; j < 3; ++j) {
		r.at(3, j) = from.at(3, j) + (to.at(3, j) - from.at(3, j)) * alpha;
	}
	return r;
}

} // namespace dice::hfe::render
