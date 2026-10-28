#version 450
// Compiled roads (RoadCompiled.fx): the two road textures blended by the
// template's blend factor, alpha from the first texture, the vertex alpha
// and a distance fade.
//
// The original lights roads from a screen-space buffer of the terrain's
// lighting; this samples the terrain lightmap of one patch instead (the road
// is drawn once per patch it crosses, and fragments outside the patch are
// discarded) and lights it like the terrain, the dynamic objects' shadows
// darkening its sun term (shadow.glsl).
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec2 inTexCoord0;
layout(location = 1) in vec2 inTexCoord1;
layout(location = 2) in vec2 inLightmapCoord;
layout(location = 3) in float inAlpha;
layout(location = 4) in float inDepth;
layout(location = 5) in vec3 inWorldPosition;

layout(set = 2, binding = 0) uniform sampler2D texture0;
layout(set = 2, binding = 1) uniform sampler2D texture1;
layout(set = 2, binding = 2) uniform sampler2D lightMap;
#define SHADOW_UNIFORM_BINDING 2
#define SHADOW_MAP_BINDING 3
#include "shadow.glsl"

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection;
	vec4 sunColor;     // terrain sun color
	vec4 skyColor;     // terrain GI color
	vec4 fogColor;
	vec4 material;     // y: 1 with a lightmap
};

layout(set = 3, binding = 1) uniform Road {
	vec4 road; // x: blend factor, y: fade start, z: fade end distance
};

layout(location = 0) out vec4 outColor;

void main()
{
	if (any(lessThan(inLightmapCoord, vec2(0.0))) || any(greaterThanEqual(inLightmapCoord, vec2(1.0)))) {
		discard;
	}
	vec4 t0 = texture(texture0, inTexCoord0);
	vec4 t1 = texture(texture1, inTexCoord1);
	vec3 albedo = mix(t1.rgb, t0.rgb, road.x);
	float fade = 1.0 - clamp((inDepth - road.y) / max(road.z - road.y, 1.0), 0.0, 1.0);
	vec3 light = vec3(1.0);
	if (material.y > 0.0) {
		vec4 lightmap = texture(lightMap, inLightmapCoord);
		vec3 sun = clamp(sunColor.rgb * 0.25, 0.0, 1.0);
		vec3 gi = clamp(skyColor.rgb * 0.5, 0.0, 1.0);
		light = (2.0 * lightmap.g * sun * shadowFactor(inWorldPosition, false) +
			clamp(2.0 * lightmap.b * gi, 0.0, 1.0) * 0.5) * 2.0;
	}
	float fog = clamp(inDepth / fogColor.w, 0.0, 1.0);
	outColor = vec4(mix(albedo * light, fogColor.rgb, fog * fog), t0.a * inAlpha * fade);
}
