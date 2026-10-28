#version 450
// Static, bundled and skinned meshes: position, normal, texture and
// lightmap coordinates. Bundled meshes (vehicles) consist of parts that move
// separately, skinned meshes (soldiers) of bones; each vertex is first
// transformed by its part or bone matrices: w * M[a] + (1 - w) * M[b].
// SDL_GPU binds vertex-stage uniform buffers in descriptor set 1.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec2 inLightmapCoord;
layout(location = 4) in float inPart;
layout(location = 5) in float inPart2;
layout(location = 6) in float inWeight;
layout(location = 7) in vec2 inDetailCoord;
layout(location = 8) in vec2 inDirtCoord;
layout(location = 9) in vec2 inCrackCoord;

layout(set = 1, binding = 0) uniform Transforms {
	mat4 worldViewProjection;
	mat4 world;
	vec4 lightmapTransform; // xy scale, zw offset into the lightmap atlas
	mat4 parts[64];         // bundled-mesh part transforms (identity for others)
};

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) out float outDepth;
layout(location = 3) out vec2 outLightmapCoord;
layout(location = 4) out vec2 outDetailCoord;
layout(location = 5) out vec2 outDirtCoord;
layout(location = 6) out vec2 outCrackCoord;
layout(location = 7) out vec3 outWorldPosition;
layout(location = 8) out float outObjectY; // the object's origin height (the hemi lerp's World[3][1])

void main()
{
	mat4 part = parts[clamp(int(inPart + 0.5), 0, 63)] * inWeight +
		parts[clamp(int(inPart2 + 0.5), 0, 63)] * (1.0 - inWeight);
	vec4 local = part * vec4(inPosition, 1.0);
	gl_Position = worldViewProjection * local;
	outWorldPosition = (world * local).xyz;
	outNormal = mat3(world) * (mat3(part) * inNormal);
	outTexCoord = inTexCoord;
	outDepth = gl_Position.w;
	outLightmapCoord = inLightmapCoord * lightmapTransform.xy + lightmapTransform.zw;
	outDetailCoord = inDetailCoord;
	outDirtCoord = inDirtCoord;
	outCrackCoord = inCrackCoord;
	outObjectY = world[3][1];
}
