#version 450
// The enhanced prepass's moving meshes (not retail): bundled and skinned
// meshes placed by this frame's and last frame's transforms (parts or
// bones), drawn over the camera's motion (gbuffer.frag) where they are.
// As mesh.vert for the position.
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inTexCoord;
layout(location = 4) in float inPart;
layout(location = 5) in float inPart2;
layout(location = 6) in float inWeight;

layout(set = 1, binding = 0) uniform Transforms {
	mat4 worldViewProjection; // jittered: drawn where the world pass draws it
	mat4 world;
	vec4 lightmapTransform;
	vec4 sway;
	vec4 normalSpace;
	vec4 trunkSunDirection;
	mat4 parts[64];
};

layout(set = 1, binding = 1) uniform Motion {
	mat4 currentWorldViewProjection;  // unjittered
	mat4 previousWorldViewProjection; // last frame's world and view-projection
	mat4 previousParts[64];
};

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outCurrent;
layout(location = 2) out vec4 outPrevious;

void main()
{
	int a = clamp(int(inPart + 0.5), 0, 63);
	int b = clamp(int(inPart2 + 0.5), 0, 63);
	vec4 local = (parts[a] * inWeight + parts[b] * (1.0 - inWeight)) * vec4(inPosition, 1.0);
	vec4 before = (previousParts[a] * inWeight + previousParts[b] * (1.0 - inWeight)) * vec4(inPosition, 1.0);
	gl_Position = worldViewProjection * local;
	outCurrent = currentWorldViewProjection * local;
	outPrevious = previousWorldViewProjection * before;
	outTexCoord = inTexCoord;
}
