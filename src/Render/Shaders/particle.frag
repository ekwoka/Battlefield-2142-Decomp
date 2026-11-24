#version 450
#extension GL_GOOGLE_include_directive : require
// Particles: the texture times the particle's colour. material.w is 1 (+2 for additive groups) where
// retail enables fixed-function fog (trails, non-screen-aligned quads): the
// colour toward FogColor, alpha kept. Particles.fx's sprites never enable it,
// so far smoke stays grey and dense.
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

layout(location = 0) out vec4 outColor;

void main()
{
	// psParticleMedium/High, psParticleAdditiveHigh: lerp(frame, next frame, blend).
	vec4 texel = mix(texture(particleTexture, inTexCoord), texture(particleTexture, inNextFrame.xy), inNextFrame.z);
	float fog = mod(material.w, 2.0) > 0.5 ? fogAmount(inDepth) : 0.0;
	outColor = vec4(mix(texel.rgb * inColor.rgb, fogColor.rgb, fog), texel.a * inColor.a);
	// Particles.fx psParticleAdditive*: the colour is masked by alpha in the shader and
	// clamped as the pixel shader's output, before the one/one add (material.w >= 2).
	if (material.w > 1.5) {
		outColor.rgb = clamp(outColor.rgb * outColor.a, 0.0, 1.0);
	}
}
