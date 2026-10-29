#version 450
#extension GL_GOOGLE_include_directive : require
// Particles: the texture times the particle's colour; fog fades them out.
layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inDepth;

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
	vec4 texel = texture(particleTexture, inTexCoord);
	float fog = fogAmount(inDepth);
	outColor = vec4(texel.rgb * inColor.rgb, texel.a * inColor.a * (1.0 - fog));
}
