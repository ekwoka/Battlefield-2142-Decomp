#version 450
// Road static meshes (RaShaderRoad.fx, RaShaderRoadDetail.fx,
// RaShaderRoadDetailNoBlend.fx): a static mesh under objects/roads/ drawn as
// a road (RendDX9 0x100bdb00's keys 0xf1 / 0xf2 / 0xf4000000). The mesh's
// own vertices: the diffuse coordinates and the detail layer's (texture
// coordinate set 1); lifted 1 cm in the world as RoadCompiled.fx lifts the
// compiled roads. road.frag lights and fades it.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inUnusedNormal;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inUnusedLightmapCoord;
layout(location = 4) in float inUnusedPart;
layout(location = 5) in float inUnusedPart2;
layout(location = 6) in float inUnusedWeight;
layout(location = 7) in vec2 inDetailCoord;
layout(location = 8) in vec2 inUnusedDirtCoord;
layout(location = 9) in vec2 inUnusedCrackCoord;

layout(set = 1, binding = 0) uniform Transforms {
	mat4 worldViewProjection;
	mat4 world;
	vec4 lightmapTransform; // world xz -> terrain patch lightmap: scale xy, offset zw
	vec4 unusedSway;
	vec4 unusedNormalSpace;
	vec4 unusedTrunkSunDirection;
	mat4 viewProjection; // TransformUniforms::parts[0]
};

layout(location = 0) out vec2 outTexCoord0;
layout(location = 1) out vec2 outTexCoord1;
layout(location = 2) out vec2 outLightmapCoord;
layout(location = 3) out float outAlpha;
layout(location = 4) out float outDepth;
layout(location = 5) out vec3 outWorldPosition;

void main()
{
	vec4 worldPosition = world * vec4(inPosition, 1.0);
	worldPosition.y += 0.01; // wPos.y += .01
	gl_Position = viewProjection * worldPosition;
	outLightmapCoord = worldPosition.xz * lightmapTransform.xy + lightmapTransform.zw;
	outTexCoord0 = inTexCoord0;
	outTexCoord1 = inDetailCoord;
	outAlpha = 1.0;
	outDepth = gl_Position.w;
	outWorldPosition = worldPosition.xyz;
}
