#version 450
// Undergrowth (Undergrowth.fx): generated in world space. Each vertex
// carries the ground height under its instance in position.y and its own
// height above it separately, so plants grow out of the ground as they come
// into view; they sway by one of 16 offsets.
layout(location = 0) in vec3 inPosition;      // y: ground height
layout(location = 1) in vec3 inUnusedNormal;
layout(location = 2) in vec2 inTexCoord;      // atlas
layout(location = 3) in vec2 inHeight;        // x: height above the ground
layout(location = 4) in float inSwayIndex;
layout(location = 5) in float inSway;         // 0..1
layout(location = 6) in float inTerrainColorMix;
layout(location = 7) in vec2 inUnusedDetailCoord;
layout(location = 8) in vec2 inUnusedDirtCoord;
layout(location = 9) in vec2 inUnusedCrackCoord;

// The simple shader's terrain maps, looked up per vertex (SDL_GPU binds
// vertex-stage samplers in set 0).
layout(set = 0, binding = 0) uniform sampler2D vertexColorMap;
layout(set = 0, binding = 1) uniform sampler2D vertexLightMap;

layout(set = 1, binding = 0) uniform Undergrowth {
	mat4 viewProjection;
	vec4 cameraPosition;
	vec4 fade;            // x: view distance, y: fade factor, z: 1 for the simple shader
	vec4 terrainTexCoord; // world xz -> terrain patch colormap/lightmap: scale xy, offset zw
	vec4 swayOffsets[16];
};

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec2 outTerrainCoord;
layout(location = 2) out float outTerrainColorMix;
layout(location = 3) out float outDepth;
layout(location = 4) out vec3 outWorldPosition;
// undergrowth.simpleShaderEnable (Undergrowth.fx VShader_Simple /
// PShader_Simple, the t0_l*_simple techniques): the terrain's colour and
// light come with the vertex (APP2VS_Simple TerrainColormap / Lightmap)
// instead of being read per pixel; w 1 when they do.
layout(location = 5) out vec4 outTerrainColor;
layout(location = 6) out vec4 outTerrainLight;

void main()
{
	vec3 position = inPosition;
	position.xz += swayOffsets[int(inSwayIndex + 0.5) & 15].xy * inSway * 3.0;
	float distance = length(vec3(position.x, inPosition.y + inHeight.x, position.z) - cameraPosition.xyz);
	float heightScale = clamp((fade.x - distance) * fade.y, 0.0, 1.0);
	position.y = inHeight.x * heightScale + inPosition.y;
	gl_Position = viewProjection * vec4(position, 1.0);
	outTexCoord = inTexCoord;
	outTerrainCoord = position.xz * terrainTexCoord.xy + terrainTexCoord.zw;
	outTerrainColorMix = inTerrainColorMix;
	outDepth = gl_Position.w;
	outWorldPosition = position;
	outTerrainColor = vec4(0.0);
	outTerrainLight = vec4(0.0);
	if (fade.z > 0.5) {
		outTerrainColor = vec4(textureLod(vertexColorMap, outTerrainCoord, 0.0).rgb, 1.0);
		outTerrainLight = vec4(textureLod(vertexLightMap, outTerrainCoord, 0.0).rgb, 1.0);
	}
}
