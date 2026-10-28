#version 450
// Terrain patches (and the surrounding terrain): position, normal,
// colormap/lightmap coordinates; the world position for the low-detail
// texture's planar projections.
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

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) out float outDepth;
layout(location = 3) out vec3 outWorldPosition;

void main()
{
	gl_Position = worldViewProjection * vec4(inPosition, 1.0);
	outNormal = inNormal;
	outTexCoord = inTexCoord;
	outDepth = gl_Position.w;
	outWorldPosition = inPosition;
}
