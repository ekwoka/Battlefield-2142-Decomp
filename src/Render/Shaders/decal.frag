#version 450
#extension GL_GOOGLE_include_directive : require
// Decals (Decals.fx): the texture faded out between
// decalFadeDistanceAndInterval (100 m and 30 m further) and by the fog.
//  - psDecal (techniques 0 and 1; the shadowed one's shadow is switched off
//    in the shipped file): times the decal's colour, lit by the ambient and
//    the sun (N.L).
//  - psDecalNormalMapped (technique 2, a vehicle's decals with a
//    decalNormalMapName): lit per pixel by the normal map in the square's
//    frame, ambient + sun x N.L + (N.H)^3 x the map's alpha (H halfway
//    between the sun and the eye), without the decal's colour.
layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in float inDepth;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in vec3 inToEye;

layout(set = 2, binding = 0) uniform sampler2D decalTexture;
layout(set = 2, binding = 1) uniform sampler2D decalNormalMap;

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
	vec3 n = normalize(inNormal);
	vec3 lit;
	if (inTangent.w > 0.5) {
		// The square's frame: u along the tangent, its v rising along
		// normal x tangent (the corners' layout); the map's green up the
		// image, as Direct3D's maps are drawn.
		vec3 t = normalize(inTangent.xyz - n * dot(inTangent.xyz, n));
		vec3 b = cross(n, t);
		vec4 bump = texture(decalNormalMap, inTexCoord);
		vec3 m = normalize(bump.rgb * 2.0 - 1.0);
		vec3 pixelNormal = normalize(t * m.x + b * m.y + n * m.z);
		vec3 sun = normalize(sunDirection.xyz);
		vec3 halfway = normalize(sun + normalize(inToEye));
		float diffuse = clamp(dot(pixelNormal, sun), 0.0, 1.0);
		float spec = clamp(dot(pixelNormal, halfway), 0.0, 1.0);
		spec = spec * spec * spec;
		lit = texel.rgb * (skyColor.rgb + diffuse * sunColor.rgb + spec * bump.a);
	} else {
		vec3 lighting = skyColor.rgb + sunColor.rgb * max(dot(n, sunDirection.xyz), 0.0);
		lit = texel.rgb * inColor.rgb * lighting;
	}
	float alpha = texel.a * inColor.a * (1.0 - clamp((inDepth - 100.0) / 30.0, 0.0, 1.0));
	float fog = fogAmount(inDepth);
	outColor = vec4(mix(lit, fogColor.rgb, fog), alpha);
}
