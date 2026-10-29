// Animated texture coordinates on vehicle parts: the tracks' scrolling and
// the road wheels' turning texture (GPU-free, for the tests).
//
// BF2142.exe: a SimpleObject template (and those built on it: the Spring
// wheels) with animatedUVTranslation (+0x12c) or animatedUVRotation
// (+0x14d) gets a 0x20-byte state at construction (0x0063c000: u, v,
// angle, the last position, the rotateAsAnimatedUV object, and a "first
// update" flag set to 1). Each object update (0x0063b9b0, from the
// bundle's 0x0059fac0) takes the distance the part moved along its own z
// axis, negated (-dot(position - last, z)); 0 on the first update. Then:
//  - translation: u += distance / size.x (minus with Reverse) + speed.x,
//    when size.x != 0, wrapped into [0, max.x] (max.x > 0.001) or
//    [max.x, 0] (max.x < 0.001); v likewise from size.y, speed.y and
//    max.y (+0x130..+0x14c). The root object's mesh instance (its +0x44)
//    is told (slot 0x90, RendDX9 0x100c2eb0) to put (u, v) into row 2 of
//    UV matrix `animatedUVTranslationIndex` (+0x148).
//  - rotation: angle += distance / animatedUVRotationRadius (minus with
//    Reverse), wrapped into [0, 2 pi]; the matrix rows (cos sx, -sin sy)
//    and (sin sx, cos sy) over identity (scale +0x150 / +0x154) replace
//    UV matrix animatedUVRotationIndex (+0x158) whole (slot 0x98, RendDX9
//    0x100c3050).
// RendDX9's bundled mesh instance keeps eight identity matrices at +0x28c
// (made on the first set) and copies nrOfAnimatedUVMatrix + 1 of them
// into the draw (0x100c3880 / 0x100c5030); RaShaderBM's
// _HASUVANIMATION_ techniques ("AnimatedUV...") take
// uv = (TexUVRotCenter, 1) x uvMatrix[blend index 3] + TexDiffuse.
// A Spring with rotateUV (+0x2b8) doesn't turn its mesh with the wheel
// (0x006cace0 / 0x006b7261 skip the set rotation 0x00614b50).
#pragma once

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

namespace dice::hfe::render {

constexpr int kAnimatedUVMatrices = 8; // RendDX9's bundled instance +0x28c


// The template's values, with its constructor's defaults (0x005f7d90).
struct AnimatedUVSettings {
	bool translation = false; // +0x12c
	float speed[2] {};        // +0x130 (added each update)
	float max[2] {0.2f, 0};   // +0x138
	float size[2] {8, 0};     // +0x140
	int translationIndex = 0; // +0x148
	bool translationReverse = false; // +0x14c
	bool rotation = false;    // +0x14d
	float scale[2] {1, 1};    // +0x150
	int rotationIndex = 0;    // +0x158
	float radius = 0.3f;      // +0x15c
	bool rotationReverse = false; // +0x160
	bool any() const { return translation || rotation; }
};

struct AnimatedUVState {
	float uv[2] {};
	float angle = 0;
	float last[3] {};
	bool first = true; // the state's +0x1c
};

// A UV matrix as the shader uses it: rows 0 and 1 (xy) and row 2 (xy).
struct UVMatrix {
	float r0[2] {1, 0};
	float r1[2] {0, 1};
	float r2[2] {0, 0};
};

namespace animated_uv_detail {
// 0x0063b9b0's wrap of a translation coordinate.
inline float wrap(float value, float max)
{
	if (max > 0.001f) {
		while (max < value) {
			value -= max;
		}
		while (value < 0.0f) {
			value += max;
		}
	} else if (max < 0.001f && max != 0.0f) {
		// The exe loops forever on a max of 0 with a negative value;
		// shipped templates pair a 0 max with a 0 size (no update).
		const float step = std::fabs(max);
		while (value < max) {
			value += step;
		}
		while (0.0f < value) {
			value -= step;
		}
	}
	return value;
}
} // namespace animated_uv_detail

// One update of a part at `position` with its z axis `zAxis` (world
// space); writes the matrices it sets.
inline void updateAnimatedUV(const AnimatedUVSettings& s, AnimatedUVState& state, const float position[3],
	const float zAxis[3], std::array<UVMatrix, kAnimatedUVMatrices>& matrices)
{
	const float d[3] = {position[0] - state.last[0], position[1] - state.last[1], position[2] - state.last[2]};
	float distance = -(d[0] * zAxis[0] + d[1] * zAxis[1] + d[2] * zAxis[2]);
	if (state.first) {
		distance = 0;
		state.first = false;
	}
	if (s.translation) {
		for (int axis = 1; axis >= 0; --axis) { // the exe does v first
			if (s.size[axis] == 0.0f) {
				continue;
			}
			const float step = distance / s.size[axis];
			state.uv[axis] = (s.translationReverse ? state.uv[axis] - step : state.uv[axis] + step) + s.speed[axis];
			state.uv[axis] = animated_uv_detail::wrap(state.uv[axis], s.max[axis]);
		}
		if (s.translationIndex >= 0 && s.translationIndex < kAnimatedUVMatrices) {
			UVMatrix& m = matrices[static_cast<size_t>(s.translationIndex)];
			m.r2[0] = state.uv[0];
			m.r2[1] = state.uv[1];
		}
	}
	if (s.rotation) {
		const float twoPi = 6.2831855f;
		const float step = s.radius != 0.0f ? (distance / (s.radius * twoPi)) * twoPi : 0.0f;
		state.angle = s.rotationReverse ? state.angle - step : state.angle + step;
		while (twoPi < state.angle) {
			state.angle -= twoPi;
		}
		while (state.angle < 0.0f) {
			state.angle += twoPi;
		}
		if (s.rotationIndex >= 0 && s.rotationIndex < kAnimatedUVMatrices) {
			const float c = std::cos(state.angle);
			const float sn = std::sin(state.angle);
			UVMatrix& m = matrices[static_cast<size_t>(s.rotationIndex)];
			m.r0[0] = c * s.scale[0];
			m.r0[1] = -(sn * s.scale[1]);
			m.r1[0] = sn * s.scale[0];
			m.r1[1] = c * s.scale[1];
			m.r2[0] = m.r2[1] = 0; // slot 0x98 copies the whole matrix
		}
	}
	state.last[0] = position[0];
	state.last[1] = position[1];
	state.last[2] = position[2];
}

// Reads "a/b" (a template's Vec2 setting).
inline void parseAnimatedUVVec2(const std::string& text, float out[2])
{
	const char* p = text.c_str();
	char* end = nullptr;
	out[0] = std::strtof(p, &end);
	if (end && *end == '/') {
		out[1] = std::strtof(end + 1, nullptr);
	}
}

} // namespace dice::hfe::render
