#include "Game/TickTimer.h"
#include "Main/MissileViewSmoothing.h"
#include "Render/TickBlend.h"

#include <cmath>
#include <unordered_map>
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
	// A part shown from collapsed (the zero matrix) comes whole, not sliding in.
	Mat4 zero;
	zero.m.fill(0);
	CHECK(blendTransform(zero, yaw(40, 10), 0.5f).m == yaw(40, 10).m);
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

// LevelRenderer::syncObjects' history (TickBlend.h): a new id starts where
// it is, unblended; a known one is drawn between its last two ticks; a
// frame without a tick keeps the previous tick; a gone id is erased.
TEST(TickBlend_ObjectHistoryByIdAsSyncObjects)
{
	using namespace dice::hfe::render;
	std::unordered_map<uint32_t, ObjectBlend> blends;
	const Mat4 a = Mat4::translation({0, 0, 0});
	const Mat4 b = Mat4::translation({2, 1, 0});
	ObjectBlend& first = sampleObjectBlend(blends, 7u, a, TickBlend {true, 0.5f});
	CHECK(first.previous.m == a.m && first.current.m == a.m && first.drawn.m == a.m);
	const ObjectBlend& second = sampleObjectBlend(blends, 7u, b, TickBlend {true, 0.25f});
	CHECK(second.previous.m == a.m && second.current.m == b.m);
	CHECK(std::fabs(second.drawn.at(3, 0) - 0.5f) < 1e-5f && std::fabs(second.drawn.at(3, 1) - 0.25f) < 1e-5f);
	// No tick since: the previous tick stays, the alpha moves on.
	const ObjectBlend& between = sampleObjectBlend(blends, 7u, b, TickBlend {false, 0.75f});
	CHECK(between.previous.m == a.m && std::fabs(between.drawn.at(3, 0) - 1.5f) < 1e-5f);
	// Another id added later starts unblended too, the first keeps its own.
	const Mat4 c = Mat4::translation({5, 0, 5});
	CHECK(sampleObjectBlend(blends, 9u, c, TickBlend {true, 0.5f}).drawn.m == c.m);
	pruneObjectBlends(blends, [](uint32_t id) { return id != 7; });
	CHECK(blends.size() == 1 && blends.count(9) == 1 && blends.count(7) == 0);
	// Back again: a fresh history, not the old one.
	CHECK(sampleObjectBlend(blends, 7u, b, TickBlend {true, 0.5f}).previous.m == b.m);
}

// More than 4 m of height between the ticks (0x0043ef30, *0x00920ca0):
// drawn where it is now; 4 m or less is blended.
TEST(TickBlend_ObjectHeightJumpSnaps)
{
	using namespace dice::hfe::render;
	for (const float rise : {3.9f, 4.0f, 4.1f, -4.1f}) {
		std::unordered_map<uint32_t, ObjectBlend> blends;
		sampleObjectBlend(blends, 1u, Mat4::translation({0, 10, 0}), TickBlend {true, 0.5f});
		const ObjectBlend& o = sampleObjectBlend(blends, 1u, Mat4::translation({0, 10 + rise, 0}), TickBlend {true, 0.5f});
		const float expected = std::fabs(rise) > 4.0f ? 10 + rise : 10 + rise * 0.5f;
		CHECK(std::fabs(o.drawn.at(3, 1) - expected) < 1e-4f);
	}
}

// The soldier's eye (Viewer.cpp through MissileViewSmoothing.h): the carrier
// (the camera less the eye, the carrier's yaw) is smoothed; the eye adds the
// blended height plus a 0.2 step towards the pose's (0x005890a0), turns by
// the carrier's blended yaw less its tick's plus the frame's look, and the
// pitch stays within 85 degrees.
TEST(TickBlend_SoldierEyeComposition)
{
	using dice::hfe::MissileViewSmoothing;
	const MissileViewSmoothing::Pose camera {{1, 11.6f, 3}, 1.0f, 0.2f, 0};
	const MissileViewSmoothing::Pose carrier = dice::hfe::soldierEyeCarrier(camera, 1.6f, 0.5f);
	CHECK(std::fabs(carrier.position[1] - 10) < 1e-5f && carrier.yaw == 0.5f && carrier.eye == 1.6f);
	CHECK(carrier.position[0] == 1 && carrier.position[2] == 3 && carrier.pitch == 0.2f);
	// The carrier turned 0.2 rad over the tick and the eye dropped 1 m
	// (standing to crouched): half way through.
	MissileViewSmoothing smooth;
	smooth.sample(5, true, 0, carrier);
	const MissileViewSmoothing::Pose next = dice::hfe::soldierEyeCarrier({{1, 10.6f, 3}, 1.0f, 0.2f, 0}, 0.6f, 0.7f);
	const MissileViewSmoothing::Pose half = smooth.sample(5, true, 0.5f, next);
	CHECK(std::fabs(half.yaw - 0.6f) < 1e-5f && std::fabs(half.eye - 1.1f) < 1e-5f && std::fabs(half.position[1] - 10) < 1e-5f);
	const dice::hfe::SoldierEyeView eye = dice::hfe::composeSoldierEye(half, 1.0f, 0.2f, 0.7f, 0.6f, 10, 5);
	CHECK(std::fabs(eye.y - (10 + 1.1f + (0.6f - 1.1f) * 0.2f)) < 1e-5f); // 11.0
	CHECK(std::fabs(eye.yaw - (1.0f - 0.1f + 10 * 3.14159265f / 180)) < 1e-5f);
	CHECK(std::fabs(eye.pitch - (0.2f + 5 * 3.14159265f / 180)) < 1e-5f);
	// The pitch is held within 85 degrees either way.
	CHECK(std::fabs(dice::hfe::composeSoldierEye(half, 0, 1.4f, 0.7f, 0.6f, 0, 10).pitch - 85 * 3.14159265f / 180) < 1e-5f);
	CHECK(std::fabs(dice::hfe::composeSoldierEye(half, 0, -1.4f, 0.7f, 0.6f, 0, -10).pitch + 85 * 3.14159265f / 180) < 1e-5f);
}
