// RendDX9's post-production (PostProduction_nv3x.fx and FSQuadDrawer.fx,
// driven by the object 0x1004a590 builds and the renderer's frame
// 0x1002df12..0x1002e6d0): what it does to the frame after the 3D passes,
// as values for the post shader. No GPU code here (shared with the tests);
// LevelRenderer runs the passes.
//
// In the original's order, each pass reading what the last one left:
//  1. The bloom (0x10049140): FSBMContrastFilter (pass 28: the backbuffer
//     cubed, to luminance, less bloomFilterContrast) into a small target,
//     the large-bloom filter across (29), a 5x5 blur (10), the large bloom
//     down (30), then Soften: backbuffer + bloom * bloomOverlayAmount.
//     With showBloomOverlayTexture the bloom alone. While a tinnitus runs
//     it takes the bloom's place (0x1004a2f0): the last four frames
//     weighted 0.5, 0.25, 0.125, 0.0675 at a jittered offset, lerped to the
//     frame by its progress.
//  2. The colour grade (0x1004a120 then ColorTransform, 0x10049a40): the
//     matrix of colorGrade() and the contrast polynomial a x + b x^2 + c;
//     CameraEffect (0x10049b90) instead while the view's camera asks for it
//     (the zoomed weapon views: weaponHud.enablePostProcessingOnGuiIndex),
//     the same grade on a fisheye ("frame") with a rolling scanline.
//  3. EMP (0x10049d70): a vehicle's EMP level loads the static from the
//     level table (0x10048a30) and greys the view (the vehicle's view
//     colour, saturation 0, or neutral); a soldier's level + 3 (5 for 3)
//     with the saturation swinging between -3 and 3. The noise row shifts
//     the image, the static darkens it, the grade's contrast applies again.
//  4. The Titan core explosion (event 0x14, 0x10049360 / 0x1004a120): the
//     grade brightened 1 + 20 t over two seconds, then white to 12 s.
#pragma once

#include "Scene/PostProductionSettings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace dice::hfe::render {

// The game state the post-production reacts to, asked each frame.
struct PostEffects {
	// The player's vehicle's EMP level (its armour's, rounded up; 0 none)
	// and the soldier's own (0 none).
	int vehicleEmpLevel = 0;
	int soldierEmpLevel = 0;
	// The vehicle's view colour (the view's slot 0x84 on: saturation 0 and
	// this tint; off: neutral).
	bool viewTinted = false;
	std::array<float, 3> viewTint {1, 1, 1};
	// CameraEffect instead of the plain grade.
	bool cameraEffect = false;
	// TVEffect (PostProduction's "TVEffect", the PostProcess component's
	// tvInterference / tvDistortionRoll / tvDistortionFreq / tvDistortionScale;
	// RendDX9's defaults +0xd8..+0xe4).
	bool tv = false;
	float tvInterference = 0.05f, tvDistortionRoll = 0.1f, tvDistortionFreq = 0.5f, tvDistortionScale = 0.5f;
	// A tinnitus starting now: its fade times summed (the event 0xd's three,
	// Sound.tinnitusSetup's 1 2 4), 0 for none.
	float tinnitusSeconds = 0;
	// A Titan's core exploded now (event 0x14).
	bool titanExplosion = false;
	// Seconds since the last frame (the clocks; 1/30 if 0).
	float frameSeconds = 0;
};

// What the post shader does this frame (post.frag's uniform block).
struct PostUniforms {
	float colorRow[3][4] {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}; // the grade's matrix rows
	float contrast[4] {1, 0, 0, 0};    // xyz the polynomial, w: the mode bits
	float bloom[4] {};                 // x: overlay amount, y: 1 to show the bloom alone
	float emp[4] {};                   // interference, roll, scale, frequency
	float empColorRow[3][4] {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
	float time[4] {};                  // FRACTIME, FRACTIME256, FRACSINE, the camera effect's scanline time
	float tinnitus[4] {};              // xy: SAMPLEOFFSET, z: BACKBUFFERLERPBIAS
	float camo[4] {};                  // xy: camouflageOffset, z: 1 with cloaked meshes drawn
	float tv[4] {};                    // interference, roll, frequency, scale
	float tvTime[4] {};                // FRACTIME, FRACTIME256, FRACSINE
};

// The mode bits (PostUniforms::contrast[3]).
constexpr int kPostBloom = 1;
constexpr int kPostGrade = 2;
constexpr int kPostCameraEffect = 4;
constexpr int kPostEmp = 8;
constexpr int kPostTv = 16;
constexpr int kPostTinnitus = 32;
constexpr int kPostCamo = 64;
constexpr int kPostViewGrade = 128;

// The bloom filters' settings (FSQuadDrawer's CONTRAST, BLOOMSIZE,
// BLOOMWEIGHTSCALE: the renderer copies +0x104..+0x10c, 0x10049140).
struct BloomUniforms {
	float params[4] {}; // x: the mode (0 contrast, 1 across, 2 down, 3 blur 5x5, 4 copy), y: contrast, z: size, w: weight scale
	float texel[4] {};  // xy: the source's texel size
};

// FSQuadDrawer's luminance (lumVec, and the post object's +0x7c..+0x84).
constexpr float kLuminance[3] = {0.3086f, 0.6094f, 0.0820f};

// 0x10049900: the saturation matrix (lerp from luminance to the colour by
// `saturation`) times the tint per channel. Row r column c reads channel c
// into channel r: out_r = sum_c ((1 - s) lum_c + s [r == c]) tint_c in_c.
inline void colorGrade(const std::array<float, 3>& tint, float saturation, float rows[3][4])
{
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 3; ++c) {
			rows[r][c] = ((1 - saturation) * kLuminance[c] + (r == c ? saturation : 0.0f)) * tint[static_cast<size_t>(c)];
		}
		rows[r][3] = 0;
	}
}

// The EMP static by effect level (0x10048a30 from the table 0x10048950
// builds: a scale of 1; the noise 0, then 8 for levels 1-5; the roll 0,
// then 0.02; no warp): interference, roll, scale, frequency.
inline std::array<float, 4> empParameters(int level, const scene::PostProductionSettings& settings)
{
	level = std::clamp(level, 0, 5);
	std::array<float, 4> p {level > 0 ? 8.0f : 0.0f, level > 0 ? 0.02f : 0.0f, 0, 0};
	// renderer.emp* override the table's (0x10050250..0x10050730).
	if (settings.empStaticNoiseAmount != 0) {
		p[0] = settings.empStaticNoiseAmount;
	}
	if (settings.empVerticalRoll != 0) {
		p[1] = settings.empVerticalRoll;
	}
	if (settings.empWarpAmount != 0) {
		p[2] = settings.empWarpAmount;
	}
	if (settings.empWarpFrequency != 0) {
		p[3] = settings.empWarpFrequency;
	}
	return p;
}

// The post-production's clocks and timed effects across frames.
class PostProductionState {
public:
	// Steps the clocks and works out this frame's passes; `cloaked` when
	// cloaked meshes were drawn into the camouflage mask.
	PostUniforms update(const PostEffects& effects, const scene::PostProductionSettings& settings, bool cloaked)
	{
		const float dt = effects.frameSeconds > 0 ? effects.frameSeconds : 1.0f / 30.0f;
		const float frames = dt * 30.0f; // the original steps its clocks per frame (at 30 a second)
		PostUniforms u;
		int mode = 0;
		// Tinnitus (0x10049440 event 0xd: (a + b + c) x 30 frames, at least 20).
		if (effects.tinnitusSeconds > 0) {
			m_tinnitusTotal = std::max(effects.tinnitusSeconds, 20.0f / 30.0f);
			m_tinnitusLeft = m_tinnitusTotal;
		}
		if (m_tinnitusLeft > 0) {
			float progress = (m_tinnitusTotal - m_tinnitusLeft) / m_tinnitusTotal;
			if (progress < 0.5f) {
				progress *= 0.5f;
			}
			m_tinnitusLeft -= dt;
			mode |= kPostTinnitus;
			u.tinnitus[0] = jitter() * 0.004f;
			u.tinnitus[1] = jitter() * 0.004f;
			u.tinnitus[2] = progress;
		} else if (settings.useBloom) {
			mode |= kPostBloom;
			u.bloom[0] = settings.bloomOverlayAmount;
			u.bloom[1] = settings.showBloomOverlayTexture ? 1.0f : 0.0f;
		}
		// The Titan explosion's flash (0x10049360 starts it, 0x1004a120 runs it).
		if (effects.titanExplosion) {
			m_flash = 0;
		}
		std::array<float, 3> tint = settings.defaultColorTint;
		float saturation = settings.defaultSaturation;
		std::array<float, 3> contrast = settings.defaultContrast;
		if (m_flash >= 0) {
			if (m_flash < 2) {
				const float t = 1 + m_flash * 20;
				tint = {t, t, t};
				contrast = {1, 0, m_flash * 0.5f};
			} else if (m_flash < 12) {
				saturation = 0;
				tint = {40, 40, 40};
				contrast = {1, 0, 1};
			}
			m_flash += dt;
			if (m_flash >= 12) {
				m_flash = -1;
			}
		}
		colorGrade(tint, saturation, u.colorRow);
		std::copy(contrast.begin(), contrast.end(), u.contrast);
		mode |= effects.cameraEffect ? kPostCameraEffect : kPostGrade;
		// CameraEffect's scanline (DAT_101fbb08: + 0.15 a frame's seconds,
		// used below 1, held at 0 to 1.5).
		m_scanline += dt * 0.15f;
		if (m_scanline >= 1.5f) {
			m_scanline = 0;
		}
		u.time[3] = m_scanline < 1.0f ? m_scanline : 0.0f;
		// EMP (the frame's 0x1002e3xx): the vehicle's, or the soldier's.
		const int vehicleLevel = std::max(effects.vehicleEmpLevel, 0);
		const int soldierLevel = std::max(effects.soldierEmpLevel, 0);
		if (vehicleLevel > 0 || soldierLevel > 0) {
			int level = vehicleLevel;
			std::array<float, 3> empTint {1, 1, 1};
			float empSaturation = 1;
			if (vehicleLevel > 0) {
				if (effects.viewTinted) {
					empTint = effects.viewTint;
					empSaturation = 0;
				}
			} else {
				level = soldierLevel == 3 ? 5 : soldierLevel + 3;
				// DAT_101f506c: 0.01 a frame, bouncing at +-3.
				m_empSwing += m_empDirection * 0.01f * frames;
				if (std::fabs(m_empSwing) > 3) {
					m_empDirection = -m_empDirection;
				}
				empSaturation = m_empSwing;
			}
			const std::array<float, 4> p = empParameters(level, settings);
			std::copy(p.begin(), p.end(), u.emp);
			colorGrade(empTint, empSaturation, u.empColorRow);
			mode |= kPostEmp;
			// FRACTIME: 0.01 a frame; FRACTIME256 takes it when a random in
			// [-frequency, 1) falls below 0 (DAT_101fbb0c): never without one.
			m_empTime += 0.01f * frames;
			m_empTime -= std::floor(m_empTime);
			if (jitter() * 0.5f + 0.5f < p[3] / (1 + p[3])) {
				m_empHeld = m_empTime;
			}
			u.time[0] = m_empTime;
			u.time[1] = m_empHeld;
			u.time[2] = std::sin(m_empTime * 6.2831855f);
		} else {
			m_empSwing = 0;
			if (vehicleLevel == 0 && effects.viewTinted && effects.vehicleEmpLevel >= 0) {
				// No EMP: the view's colour as a second grade (ColorTransform
				// again with the vehicle's tint, saturation 0).
				colorGrade(effects.viewTint, 0, u.empColorRow);
				mode |= kPostViewGrade;
			}
		}
		if (effects.tv) {
			// 0x10048f80: FRACTIME 0.01 a frame; FRACTIME256 its fraction x 256.
			m_tvTime += 0.01f * frames;
			if (m_tvTime >= 1) {
				m_tvTime = 0;
			}
			u.tv[0] = effects.tvInterference;
			u.tv[1] = effects.tvDistortionRoll;
			u.tv[2] = effects.tvDistortionFreq;
			u.tv[3] = effects.tvDistortionScale;
			u.tvTime[0] = m_tvTime;
			u.tvTime[1] = m_tvTime * 256 - std::floor(m_tvTime * 256);
			u.tvTime[2] = std::sin(m_tvTime * 6.2831855f);
			mode |= kPostTv;
		}
		if (cloaked) {
			mode |= kPostCamo;
			u.camo[0] = settings.camouflageOffset[0];
			u.camo[1] = settings.camouflageOffset[1];
			u.camo[2] = 1;
		}
		u.contrast[3] = static_cast<float>(mode);
		return u;
	}

	bool tinnitusActive() const { return m_tinnitusLeft > 0; }
	bool flashActive() const { return m_flash >= 0; }

private:
	// -1..1 (the original's rand()).
	float jitter()
	{
		m_random = m_random * 1664525u + 1013904223u;
		return static_cast<float>(m_random >> 8) / static_cast<float>(1u << 23) - 1.0f;
	}
	float m_tinnitusTotal = 0, m_tinnitusLeft = 0;
	float m_flash = -1;
	float m_scanline = 0;
	float m_empSwing = 0, m_empDirection = 1;
	float m_empTime = 0, m_empHeld = 0;
	float m_tvTime = 0;
	uint32_t m_random = 0x2142u;
};

} // namespace dice::hfe::render
