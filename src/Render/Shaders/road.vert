#version 450
// Compiled roads (RoadCompiled.fx): position, two texture coordinates and
// an alpha that fades the road's ends in. Lifted 1 cm off the terrain as
// the original does.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inUnusedNormal;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;
layout(location = 4) in float inUnusedPart;
layout(location = 5) in float inUnusedPart2;
layout(location = 6) in float inAlpha;
layout(location = 7) in vec2 inUnusedDetailCoord;
layout(location = 8) in vec2 inUnusedDirtCoord;
layout(location = 9) in vec2 inUnusedCrackCoord;

layout(set = 1, binding = 0) uniform Transforms {
	mat4 worldViewProjection;
	mat4 world;
	vec4 lightmapTransform; // world xz -> terrain patch lightmap: scale xy, offset zw
	mat4 parts[64];
};

layout(location = 0) out vec2 outTexCoord0;
layout(location = 1) out vec2 outTexCoord1;
layout(location = 2) out vec2 outLightmapCoord;
layout(location = 3) out float outAlpha;
layout(location = 4) out float outDepth;

void main()
{
	vec4 position = vec4(inPosition + vec3(0.0, 0.01, 0.0), 1.0);
	gl_Position = worldViewProjection * position;
	vec3 worldPosition = (world * position).xyz;
	outLightmapCoord = worldPosition.xz * lightmapTransform.xy + lightmapTransform.zw;
	outTexCoord0 = inTexCoord0;
	outTexCoord1 = inTexCoord1;
	outAlpha = inAlpha;
	outDepth = gl_Position.w;
}
