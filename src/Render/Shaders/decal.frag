#version 450
#extension GL_GOOGLE_include_directive : require
// Decals (Decals.fx psDecal): the texture times the decal's colour, lit by
// the ambient and the sun (N.L), faded out between decalFadeDistanceAndInterval
// (100 m and 30 m further) and by the fog.
layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in float inDepth;
layout(location = 3) in vec4 inColor;

layout(set = 2, binding = 0) uniform sampler2D decalTexture;

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
	vec4 texel = texture(decalTexture, inTexCoord);
	vec3 lighting = skyColor.rgb + sunColor.rgb * max(dot(normalize(inNormal), sunDirection.xyz), 0.0);
	float alpha = texel.a * inColor.a * (1.0 - clamp((inDepth - 100.0) / 30.0, 0.0, 1.0));
	float fog = fogAmount(inDepth);
	outColor = vec4(mix(texel.rgb * inColor.rgb * lighting, fogColor.rgb, fog), alpha);
}
