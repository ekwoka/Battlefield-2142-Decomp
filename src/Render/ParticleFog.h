// Which particle draws retail fogs (GPU-free, for the tests).
//
// RendDX9 draws the normal-blend sprites through 0x1012fdf0 (Particles.fx
// ParticleLow/Medium/High, by particleSystemShaderQuality 1..3). It first
// calls 0x1012f750 with CL = (the particle's +0x90 != 0): with CL clear and
// the quality not 0 that sets SRCBLEND SRCALPHA, DESTBLEND INVSRCALPHA and
// D3DRS_FOGENABLE 1; otherwise ONE/ONE and fog off (the quality-0 overdraw
// view, ParticleShowFill). The passes don't touch FogEnable, so the sprites
// fog with the vertex shader's oFog (vsParticle: calcFog(HPos.w)); RendDX9
// sets no table fog mode. Templates with blendMode Additive go through
// 0x10130bd0 instead (0x10138920) and draw with 0x10130540: ONE/ONE, fog
// off, AdditiveLow/High (which also set FogEnable FALSE).
#pragma once

namespace dice::hfe::render {

inline bool spriteGroupFogged(bool additive, int shaderQuality)
{
	return !additive && shaderQuality != 0;
}

// particle.frag's material.w: 1 where the group fogs, + 2 where it's
// additive (the colour masked by alpha).
inline float particleMaterialFlags(bool fogged, bool additive)
{
	return (fogged ? 1.0f : 0.0f) + (additive ? 2.0f : 0.0f);
}

// Trails (TrailSystem) and non-screen-aligned quads: RendDX9's trail
// renderer (0x10133c00: technique 0x10133d90, batches 0x10133000) and NSA
// renderer (0x1012e920: technique 0x1012ebf0, batches 0x1012d720) pick a
// technique by particleSystemShaderQuality alone (Trail / NSAParticle
// ShowFill, Low, Medium, High; particleShaderType 1 the heat shimmer) and set
// no render state between BeginPass and EndPass, only textures and
// constants. Trail.fx and NonScreenAlignedParticles.fx have no additive
// technique: each draws SRCALPHA / INVSRCALPHA (ShowFill ONE / ONE) with
// FogEnable TRUE, and their pixel shaders don't mask by alpha. A template's
// particleType Additive (Codename_Excalibur's em_excalibur_trail_TrailGlow,
// the only shipped one) never reaches them, so such a group draws
// normal-blend, lit and fogged like the rest.
struct PlacedParticleDraw {
	bool additive = false;
	bool fogged = true;
};

inline PlacedParticleDraw placedGroupDraw(bool /*templateAdditive*/)
{
	return {};
}

} // namespace dice::hfe::render
