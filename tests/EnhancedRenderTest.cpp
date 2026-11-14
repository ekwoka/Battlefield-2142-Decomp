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

// gbuffer.frag's normals from the depth buffer, in float as the GPU works:
// a Titan deck 12 m ahead at a grazing angle, seen from the Suez Titan
// (some 400 m from the origin) at 3456 x 2160. Unprojected through the
// view-projection's inverse the depth's rounding near 1 came out as
// millimetres, more than a pixel's spacing, and the normals as noise (the
// GTAO's streaks on the decks, worse the finer the screen); unprojected
// about the camera (the projection's inverse) it stays well under a degree.
TEST(EnhancedRender_ViewSpaceUnprojectionKeepsGrazingNormals)
{
	const Vec3f eye {-380, 171, -11};
	const Mat4 view = Mat4::lookAt(eye, eye + Vec3f {1, -0.8f, 0.1f}, {0, 1, 0});
	const Mat4 projection = Mat4::perspective(1.22f, 1.6f, 0.1f, 6000.0f);
	const Mat4 viewProjection = view * projection;
	const auto rowTimes = [](const float v[4], const Mat4& m, float out[4]) {
		for (int c = 0; c < 4; ++c) {
			out[c] = v[0] * m.at(0, c) + v[1] * m.at(1, c) + v[2] * m.at(2, c) + v[3] * m.at(3, c);
		}
	};
	// The deck (y = 169.5) where the ray through a pixel meets it: its depth
	// as the depth buffer holds it (float), and its ndc.
	const float pixel[2] = {2.0f / 3456, 2.0f / 2160};
	const Mat4 inverseViewProjection = enhanced::inverse(viewProjection);
	const auto depthAt = [&](float x, float y, float ndcOut[3]) {
		const double start[4] = {x, y, 0, 1}, end[4] = {x, y, 1, 1};
		double a[4] = {}, b[4] = {};
		for (int c = 0; c < 4; ++c) {
			for (int r = 0; r < 4; ++r) {
				a[c] += start[r] * inverseViewProjection.at(r, c);
				b[c] += end[r] * inverseViewProjection.at(r, c);
			}
		}
		double p0[3], p1[3];
		for (int k = 0; k < 3; ++k) {
			p0[k] = a[k] / a[3];
			p1[k] = b[k] / b[3];
		}
		const double t = (169.5 - p0[1]) / (p1[1] - p0[1]);
		const double world[4] = {p0[0] + (p1[0] - p0[0]) * t, 169.5, p0[2] + (p1[2] - p0[2]) * t, 1};
		double clip[4] = {};
		for (int c = 0; c < 4; ++c) {
			for (int r = 0; r < 4; ++r) {
				clip[c] += world[r] * viewProjection.at(r, c);
			}
		}
		ndcOut[0] = x;
		ndcOut[1] = y;
		ndcOut[2] = static_cast<float>(clip[2] / clip[3]); // the depth buffer's rounding only
	};
	const Mat4 inverseProjection = enhanced::inverse(projection);
	const Mat4 inverseView = enhanced::inverse(view);
	const auto unproject = [&](const Mat4& m, const float ndc[3], Vec3f& out) {
		const float v[4] = {ndc[0], ndc[1], ndc[2], 1};
		float p[4];
		rowTimes(v, m, p);
		out = {p[0] / p[3], p[1] / p[3], p[2] / p[3]};
	};
	const auto worstAngle = [&](bool viaView) {
		float worst = 0;
		for (int i = 0; i < 64; ++i) {
			const float x = -0.3f + 0.0093f * static_cast<float>(i), y = -0.35f + 0.0041f * static_cast<float>(i);
			float c[3], r[3], d[3];
			depthAt(x, y, c);
			depthAt(x + pixel[0], y, r);
			depthAt(x, y - pixel[1], d);
			Vec3f pc, pr, pd;
			unproject(viaView ? inverseProjection : inverseViewProjection, c, pc);
			unproject(viaView ? inverseProjection : inverseViewProjection, r, pr);
			unproject(viaView ? inverseProjection : inverseViewProjection, d, pd);
			Vec3f n = normalize(cross(pr - pc, pd - pc));
			if (viaView) { // back to the world for the comparison
				const float v[4] = {n.x, n.y, n.z, 0};
				float w[4];
				rowTimes(v, inverseView, w);
				n = normalize(Vec3f {w[0], w[1], w[2]});
			}
			worst = std::max(worst, std::acos(std::min(std::fabs(n.y), 1.0f)) * 57.29578f);
		}
		return worst;
	};
	const float throughWorld = worstAngle(false), aboutCamera = worstAngle(true);
	std::fprintf(stderr, "  deck normal error: %.2f deg through the world, %.3f deg about the camera\n", throughWorld,
		aboutCamera);
	CHECK(throughWorld > 10.0f);
	CHECK(aboutCamera < 1.0f);
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
