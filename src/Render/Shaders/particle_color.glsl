// A particle's pixel as retail's Particles.fx pixel shaders and D3D9's
// fixed-function fog make it: GLSL that also compiles as C++
// (Render/ShaderMath.h), so the tests evaluate particle.frag's own code.
// Keep it to what both languages read alike (fog_share.glsl).
#ifndef SHADER_INLINE
#define SHADER_INLINE
#endif

// `texel` is the texture (the frames already lerped), `color` the vertex
// colour (2 x tint x light) and alpha. psParticleLow/Medium/High: colour
// texel x colour, alpha texel x alpha. psParticleAdditiveLow/High mask
// the colour with that alpha ("mask with alpha since were doing an add").
// ps_1_4 saturates its output, so the clamp comes first; then, where
// FogEnable is on (RendDX9 0x1012f750 for the normal-blend sprites;
// Trail.fx and NonScreenAlignedParticles.fx), the fog lerps the colour
// toward FogColor by `fog` (1 - the vertex shader's oFog, calcFog), alpha
// kept. The additive techniques never fog.
SHADER_INLINE vec4 particleColor(vec4 texel, vec4 color, bool additive, float fog, vec3 fogColor)
{
	float alpha = texel.w * color.w;
	vec3 rgb = vec3(texel.x * color.x, texel.y * color.y, texel.z * color.z);
	if (additive) {
		rgb = rgb * alpha;
	}
	rgb = clamp(rgb, 0.0f, 1.0f);
	return vec4(mix(rgb, fogColor, fog), clamp(alpha, 0.0f, 1.0f));
}
