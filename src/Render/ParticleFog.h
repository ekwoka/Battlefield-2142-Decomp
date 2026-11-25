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

} // namespace dice::hfe::render
