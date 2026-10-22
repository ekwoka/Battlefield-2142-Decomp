#version 450
// Sea water: a grid at sea level; texCoord.x carries the water depth.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec2 inUnused;
layout(location = 4) in float inUnusedPart;
layout(location = 5) in float inUnusedPart2;
layout(location = 6) in float inUnusedWeight;
layout(location = 7) in vec2 inUnusedDetailCoord;
layout(location = 8) in vec2 inUnusedDirtCoord;
layout(location = 9) in vec2 inUnusedCrackCoord;

layout(set = 1, binding = 0) uniform Transforms {
	mat4 worldViewProjection;
	mat4 world;
	vec4 lightmapTransform;
	mat4 parts[64];
};

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out float outWaterDepth;
layout(location = 2) out float outDepth;

void main()
{
	gl_Position = worldViewProjection * vec4(inPosition, 1.0);
	outWorldPosition = inPosition;
	outWaterDepth = inTexCoord.x;
	outDepth = gl_Position.w;
}
