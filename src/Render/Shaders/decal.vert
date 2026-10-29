#version 450
// Decals (Decals.fx vsDecal): squares laid on surfaces, built on the CPU in
// world space. texCoord: the atlas frame's coordinates; weight: alpha (the
// decal's age);
// lightmapCoord.xy and part: the decal's colour.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec2 inColorRG;
layout(location = 4) in float inColorB;
layout(location = 5) in float inUnusedPart2;
layout(location = 6) in float inAlpha;
layout(location = 7) in vec2 inUnusedDetailCoord;
layout(location = 8) in vec2 inUnusedDirtCoord;
layout(location = 9) in vec2 inUnusedCrackCoord;

layout(set = 1, binding = 0) uniform Transforms {
	mat4 worldViewProjection;
	mat4 world;
	vec4 lightmapTransform; // xy: DecalFadeDistanceAndInterval
};

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) out float outDepth;
layout(location = 3) out vec4 outColor;

void main()
{
	gl_Position = worldViewProjection * vec4(inPosition, 1.0);
	outNormal = inNormal;
	outTexCoord = inTexCoord;
	outDepth = gl_Position.w;
	// vsDecal: 1 - saturate((HPos.z - distance) / interval), times the
	// decal's age fade; the depth taken as the view's.
	float fade = 1.0 - clamp((gl_Position.w - lightmapTransform.x) / max(lightmapTransform.y, 1e-3), 0.0, 1.0);
	outColor = vec4(inColorRG, inColorB, inAlpha * fade);
}
