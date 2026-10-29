#version 450
// Terrain patches (and the surrounding terrain): position, normal,
// colormap/lightmap coordinates; the world position for the low-detail
// texture's planar projections.
//
// The primary terrain's compiled vertices are geomorphed as
// TerrainShader_Shared.fx's geoMorphPosition does: with d^2 the squared
// distance from the camera (its height above the ground standing in for
// the vertical, CAMERAPOS.w),
//   interp = saturate(d^2 * limits.x - limits.y)
//   y -= dot(selector, delta) * interp + dot(adder[edge step], delta)
// so a vertex slides to where the next LOD puts it before that LOD takes
// over, and a stitched edge's extra vertices sit on the coarser edge.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec2 inMorphDelta01; // the deltas to LOD 1 and LOD 2, metres
layout(location = 4) in float inMorphDelta2;
layout(location = 5) in float inEdgeStep;    // the LOD its cell edge drops it at (0: none; -1: not compiled)
layout(location = 6) in float inUnusedWeight;
layout(location = 7) in vec2 inUnusedDetailCoord;
layout(location = 8) in vec2 inUnusedDirtCoord;
layout(location = 9) in vec2 inUnusedCrackCoord;

layout(set = 1, binding = 0) uniform Transforms {
	mat4 worldViewProjection;
	mat4 world;
	vec4 lightmapTransform;
};

layout(set = 1, binding = 1) uniform Morph {
	vec4 camera;   // x, unused, z, height above the ground
	vec4 limits;   // NEARFARMORPHLIMITS, 1 to morph, 1 to fade the detail by the morph
	vec4 selector; // MORPHDELTASELECTOR
	vec4 adder[3]; // MORPHDELTAADDER
	vec4 next;     // LOD 0 cells: LOD 1's limits and the LOD 2 delta's weight (the rewrite's, TerrainLod.h)
};

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) out float outDepth;
layout(location = 3) out vec3 outWorldPosition;
layout(location = 4) out float outFade; // towards the next LOD (Hi_VS_FullDetail's BlendValueAndFade.w)

void main()
{
	vec3 position = inPosition;
	vec3 toCamera = vec3(camera.x - position.x, camera.w, camera.z - position.z);
	float d2 = dot(toCamera, toCamera);
	float interp = clamp(d2 * limits.x - limits.y, 0.0, 1.0);
	if (limits.z > 0.5 && inEdgeStep >= 0.0) {
		vec3 delta = vec3(inMorphDelta01, inMorphDelta2);
		int edge = clamp(int(inEdgeStep + 0.5), 0, 2);
		position.y -= dot(selector.xyz, delta) * interp + dot(adder[edge].xyz, delta) +
			delta.y * clamp(d2 * next.x - next.y, 0.0, 1.0) * next.z;
	}
	gl_Position = worldViewProjection * vec4(position, 1.0);
	outNormal = inNormal;
	outTexCoord = inTexCoord;
	outDepth = gl_Position.w;
	outWorldPosition = position;
	outFade = limits.w > 0.5 ? interp : 1.0;
}
