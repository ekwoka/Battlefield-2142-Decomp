#version 450
// Undergrowth (Undergrowth.fx PShader): the atlas texture tinted by the
// terrain colormap under it (less so by TerrainColorScale) and lit by the
// terrain lightmap:
//   light = lightmap.g * sun * 2 + lightmap.b * GI
//   color = base * mix(terrain, 1, mix) * light * 2
// Alpha-tested against the cfg's AlphaRef, using the atlas's "_alphatest"
// variant (full-range alpha). With undergrowth.dynamicShadowsEnable the
// dynamic objects' shadows darken its sun term (shadow.glsl).
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec2 inTerrainCoord;
layout(location = 2) in float inTerrainColorMix;
layout(location = 3) in float inDepth;
layout(location = 4) in vec3 inWorldPosition;
layout(location = 5) in vec4 inTerrainColor; // the simple shader's (w 1): per vertex
layout(location = 6) in vec4 inTerrainLight;

layout(set = 2, binding = 0) uniform sampler2D atlas;
layout(set = 2, binding = 1) uniform sampler2D colorMap;
layout(set = 2, binding = 2) uniform sampler2D lightMap;
#define SHADOW_UNIFORM_BINDING 1
#define SHADOW_MAP_BINDING 3
#include "shadow.glsl"

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection;
	vec4 sunColor;     // terrain sun color
	vec4 skyColor;     // terrain GI color
	vec4 fogColor;
	vec4 material;     // x: alpha reference, y: 1 with a lightmap
};

layout(location = 0) out vec4 outColor;

void main()
{
	vec4 base = texture(atlas, inTexCoord);
	if (base.a < material.x) {
		discard;
	}
	bool simple = inTerrainColor.w > 0.5;
	vec3 terrain = mix(simple ? inTerrainColor.rgb : texture(colorMap, inTerrainCoord).rgb, vec3(1.0), inTerrainColorMix);
	vec3 light = vec3(1.0);
	if (material.y > 0.0) {
		vec4 lightmap = simple ? inTerrainLight : texture(lightMap, inTerrainCoord);
		vec3 sun = clamp(sunColor.rgb * 0.25, 0.0, 1.0);
		vec3 gi = clamp(skyColor.rgb * 0.5, 0.0, 1.0);
		float shadow = shadowExtra.z > 0.5 ? shadowFactor(inWorldPosition, false) : 1.0;
		light = lightmap.g * sun * 2.0 * shadow + lightmap.b * gi;
	}
	vec3 color = base.rgb * terrain * light * 2.0;
	float fog = clamp(inDepth / fogColor.w, 0.0, 1.0);
	outColor = vec4(mix(color, fogColor.rgb, fog * fog), 1.0);
}
