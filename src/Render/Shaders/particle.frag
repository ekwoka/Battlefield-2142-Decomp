#version 450
#extension GL_GOOGLE_include_directive : require
// Particles: the texture times the particle's colour (particle_color.glsl).
// material.w is 1 (+2 for additive groups) where retail enables
// fixed-function fog: the normal-blend sprites (RendDX9 0x1012f750 sets
// FogEnable for them), trails and non-screen-aligned quads; the colour
// toward FogColor, alpha kept. The additive techniques (and Rain.fx) don't.
layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inDepth;
layout(location = 5) in vec3 inNextFrame;

layout(set = 2, binding = 0) uniform sampler2D particleTexture;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection;
	vec4 sunColor;
	vec4 skyColor;
	vec4 fogColor;     // rgb, w: the near term's floor (FogColor.w)
	vec4 fogRange;     // RaCommon.fx FogRange (fog.glsl)
	vec4 material;
};
#include "fog.glsl"
#include "particle_color.glsl"

layout(location = 0) out vec4 outColor;

void main()
{
	// psParticleMedium/High, psParticleAdditiveHigh: lerp(frame, next frame, blend).
	vec4 texel = mix(texture(particleTexture, inTexCoord), texture(particleTexture, inNextFrame.xy), inNextFrame.z);
	// The vertex shader's oFog (RendDX9 sets no table fog mode, so D3D9 uses
	// oFog): a sprite's corners share their depth, so per pixel is per vertex.
	float fog = mod(material.w, 2.0) > 0.5 ? fogAmount(inDepth) : 0.0;
	outColor = particleColor(texel, inColor, material.w > 1.5, fog, fogColor.rgb);
}
