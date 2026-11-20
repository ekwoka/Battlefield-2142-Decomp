#include "Game/TickTimer.h"
#include "Main/MissileViewSmoothing.h"
#include "Render/TickBlend.h"

#include <cmath>
#include <utility>
#include "TestMain.h"

TEST(TickTimer_CountsWholeTicks)
{
	dice::hfe::TickTimer timer(10, 30.0f);
	CHECK(timer.advance(0.01) == 0);
	CHECK(timer.advance(0.034) == 1);    // one tick of 1/30 s
	CHECK(timer.advance(0.05) == 0);     // the remainder carries over
	CHECK(timer.advance(0.101) == 2);    // 0.0333 .. 0.1 (exactly 0.1 is a hair short in float)
	CHECK(timer.lastTickTime() > 0.0999 && timer.lastTickTime() < 0.1001);
}

TEST(TickTimer_ResynchronisesWhenFarBehind)
{
	dice::hfe::TickTimer timer(10, 30.0f);
	CHECK(timer.advance(5.0) == 10); // 150 ticks behind: capped, clock jumps to now
	CHECK(timer.lastTickTime() == 5.0);
	CHECK(timer.advance(5.04) == 1);
}

// The frame's draw blend (0x0043f170): (now - the timer's +0x10) over the
// ticking frame's count x 1/30 s, -0.1 .. 1.1, 0 before any tick.
TEST(TickBlend_AlphaFollowsTheRetailFrame)
{
	using dice::hfe::render::tickBlendAlpha;
	dice::hfe::TickTimer timer(10, 30.0f);
	CHECK(timer.advance(0.034) == 1); // the first: +0x10 is still the constructor's 0
	CHECK(timer.advance(0.07) == 1);
	const double last = timer.previousTickTime();
	CHECK(std::fabs(last - timer.lastTickTime()) < 1e-6); // one tick: the last tick's time
	CHECK(tickBlendAlpha(0.05, last, 0, timer.tickLength()) == 0);
	CHECK(std::fabs(tickBlendAlpha(last + 0.5 / 30, last, 1, timer.tickLength()) - 0.5f) < 1e-4f);
	CHECK(std::fabs(tickBlendAlpha(last + 0.5 / 30, last, 2, timer.tickLength()) - 0.25f) < 1e-4f);
	CHECK(tickBlendAlpha(last + 1, last, 1, timer.tickLength()) == 1.1f);
	CHECK(tickBlendAlpha(last - 1, last, 1, timer.tickLength()) == -0.1f);
}

TEST(TickBlend_TransformSlerpsAsRetail)
{
	using namespace dice::hfe::render;
	const auto yaw = [](float degrees, float x) {
		const float r = degrees * 3.14159265f / 180;
		Mat4 m;
		m.at(0, 0) = std::cos(r); m.at(0, 2) = -std::sin(r);
		m.at(2, 0) = std::sin(r); m.at(2, 2) = std::cos(r);
		m.at(3, 0) = x;
		return m;
	};
	const Mat4 half = blendTransform(yaw(0, 0), yaw(40, 10), 0.5f);
	CHECK(std::fabs(half.at(3, 0) - 5) < 1e-4f);
	CHECK(std::fabs(half.at(0, 0) - std::cos(20 * 3.14159265f / 180)) < 1e-3f); // the axis turned halfway, unit length
	CHECK(std::fabs(half.at(0, 0) * half.at(0, 0) + half.at(0, 2) * half.at(0, 2) - 1) < 1e-4f);
	CHECK(std::fabs(half.at(1, 1) - 1) < 1e-5f);
	// No limit on the turn (0x006ac980 slerps any): 90 degrees halfway is 45.
	const Mat4 quarter = blendTransform(yaw(0, 0), yaw(90, 10), 0.5f);
	const Mat4 expect = yaw(45, 5);
	for (size_t i = 0; i < expect.m.size(); ++i) {
		CHECK(std::fabs(quarter.m[i] - expect.m[i]) < 1e-4f);
	}
	CHECK(blendTransform(yaw(0, 0), yaw(40, 10), 1).m == yaw(40, 10).m);
	// Any orientation comes back whole at either end, the short way round.
	const Mat4 tilted = Mat4::yawPitchRoll(130, -60, 170) * Mat4::translation({1, 2, 3});
	const Mat4 other = Mat4::yawPitchRoll(-150, 20, -30);
	for (const auto& [alpha, want] : {std::pair {0.0f, &tilted}, std::pair {0.9999f, &other}}) {
		const Mat4 got = blendTransform(tilted, other, alpha);
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 3; ++c) {
				CHECK(std::fabs(got.at(r, c) - want->at(r, c)) < 1e-3f);
			}
		}
	}
	CHECK(dice::hfe::render::tickBlendJumped(0, 4.5f) && !dice::hfe::render::tickBlendJumped(0, 3.5f));
}

TEST(TickBlend_CameraSnapsOnAHeightJump)
{
	dice::hfe::MissileViewSmoothing smooth;
	dice::hfe::MissileViewSmoothing::Pose ground {{0, 0, 0}, 0, 0};
	dice::hfe::MissileViewSmoothing::Pose lifted {{1, 6, 0}, 0, 0};
	smooth.sample(7, true, 0, ground);
	auto p = smooth.sample(7, true, 0.5f, lifted, dice::hfe::render::kTickBlendJump);
	CHECK(p.position[1] == 6 && p.position[0] == 1);
}

TEST(TickBlend_CameraRollBlendsTheShortWay)
{
	dice::hfe::MissileViewSmoothing smooth;
	smooth.sample(3, true, 0, {{0, 0, 0}, 0, 0, 3.0f});
	const auto p = smooth.sample(3, true, 0.5f, {{0, 0, 0}, 0, 0, -3.0f}); // across the seam
	CHECK(std::fabs(std::remainder(p.roll - 3.14159265f, 6.28318530718f)) < 1e-3f);
}

TEST(TickBlend_AffineInverseUndoesATransform)
{
	using namespace dice::hfe::render;
	Mat4 m = Mat4::yawPitchRoll(30, -20, 70) * Mat4::translation({4, -2, 9});
	m.at(2, 0) *= 2; m.at(2, 1) *= 2; m.at(2, 2) *= 2; // a stretched axis
	const Mat4 one = m * affineInverse(m);
	for (int r = 0; r < 4; ++r) {
		for (int c = 0; c < 3; ++c) {
			CHECK(std::fabs(one.at(r, c) - (r == c ? 1.0f : 0.0f)) < 1e-4f);
		}
	}
}

// A held part under a node without a part of its own (LevelRenderer's
// blendParts): the node keeps its template place, so blending the part's
// combined transform equals retail's blending the part in the node.
TEST(TickBlend_FixedNodeFoldsIntoTheBlend)
{
	using namespace dice::hfe::render;
	const Mat4 node = Mat4::yawPitchRoll(25, 40, -10) * Mat4::translation({0.3f, -0.1f, 0.5f});
	const Mat4 from = Mat4::yawPitchRoll(-30, 5, 0) * Mat4::translation({0, 0.2f, 0});
	const Mat4 to = Mat4::yawPitchRoll(70, -15, 20) * Mat4::translation({0.1f, 0, 0.4f});
	const Mat4 folded = blendTransform(from * node, to * node, 0.3f);
	const Mat4 nested = blendTransform(from, to, 0.3f) * node;
	for (size_t i = 0; i < folded.m.size(); ++i) {
		CHECK(std::fabs(folded.m[i] - nested.m[i]) < 1e-4f);
	}
}
