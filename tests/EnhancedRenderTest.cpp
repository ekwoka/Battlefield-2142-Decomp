// The enhanced mode's render maths (Render/Enhanced/EnhancedMath.h): TAA's
// jitter, the inverse the prepass reconstructs positions with, and the
// cascades' splits and spheres.
#include "Render/Enhanced/EnhancedMath.h"
#include "TestMain.h"

#include <cmath>

using namespace dice::hfe::render;

TEST(EnhancedRender_JitterStaysWithinHalfAPixelAndCycles)
{
	CHECK(std::fabs(enhanced::halton(1, 2) - 0.5f) < 1e-6f);
	CHECK(std::fabs(enhanced::halton(2, 3) - 2.0f / 3.0f) < 1e-6f);
	float sumX = 0, sumY = 0;
	for (unsigned frame = 0; frame < 8; ++frame) {
		const auto o = enhanced::jitterOffset(frame, 1280, 720);
		// Clip space is 2 / size a pixel: within half a pixel is within 1 / size.
		CHECK(std::fabs(o[0]) <= 1.0f / 1280 + 1e-7f);
		CHECK(std::fabs(o[1]) <= 1.0f / 720 + 1e-7f);
		sumX += o[0] * 1280;
		sumY += o[1] * 720;
		const auto again = enhanced::jitterOffset(frame + 8, 1280, 720);
		CHECK(again[0] == o[0] && again[1] == o[1]);
	}
	// Spread about the pixel's centre.
	CHECK(std::fabs(sumX / 8) < 0.25f);
	CHECK(std::fabs(sumY / 8) < 0.25f);
}

TEST(EnhancedRender_JitterMatrixShiftsClipSpaceByW)
{
	const Mat4 j = enhanced::jitterMatrix({0.01f, -0.02f});
	const Mat4 p = Mat4::perspective(1.0f, 16.0f / 9.0f, 0.5f, 6000.0f);
	const Mat4 pj = p * j;
	// A view-space point (row vector) through both: x and y move by offset x w.
	const float v[4] = {3, -2, 40, 1};
	float a[4] = {}, b[4] = {};
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			a[c] += v[r] * p.at(r, c);
			b[c] += v[r] * pj.at(r, c);
		}
	}
	CHECK(std::fabs(b[0] - (a[0] + 0.01f * a[3])) < 1e-4f);
	CHECK(std::fabs(b[1] - (a[1] - 0.02f * a[3])) < 1e-4f);
	CHECK(b[2] == a[2] && b[3] == a[3]);
}

TEST(EnhancedRender_InverseUndoesAViewProjection)
{
	const Mat4 vp = Mat4::lookAt({10, 20, -30}, {0, 5, 40}, {0, 1, 0}) * Mat4::perspective(0.9f, 1.5f, 0.5f, 6000.0f);
	const Mat4 i = vp * enhanced::inverse(vp);
	for (int r = 0; r < 4; ++r) {
		for (int c = 0; c < 4; ++c) {
			CHECK(std::fabs(i.at(r, c) - (r == c ? 1.0f : 0.0f)) < 1e-3f);
		}
	}
	Mat4 singular;
	singular.m.fill(0);
	CHECK(enhanced::inverse(singular).at(0, 0) == 1); // identity back
}

TEST(EnhancedRender_CascadesSplitTheViewAndHoldTheirSlices)
{
	const auto far = enhanced::cascadeSplits(0.5f, 180.0f);
	CHECK(far[3] == 180.0f);
	for (int i = 1; i < enhanced::kCascades; ++i) {
		CHECK(far[static_cast<size_t>(i)] > far[static_cast<size_t>(i - 1)]);
	}
	CHECK(far[0] > 0.5f && far[0] < 30.0f); // the first cascade is short: the sharpest shadows near
	const float tanY = std::tan(0.5f), tanX = tanY * 16.0f / 9.0f;
	float n = 0.5f;
	for (int i = 0; i < enhanced::kCascades; ++i) {
		const float f = far[static_cast<size_t>(i)];
		const enhanced::SliceSphere s = enhanced::sliceSphere(n, f, tanX, tanY);
		CHECK(s.distance >= n && s.distance <= f);
		// Every corner of the slice lies inside.
		for (const float z : {n, f}) {
			const float dx = z * tanX, dy = z * tanY, dz = z - s.distance;
			CHECK(dx * dx + dy * dy + dz * dz <= s.radius * s.radius * 1.0001f);
		}
		n = f;
	}
}
