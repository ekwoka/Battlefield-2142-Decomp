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
layout(location = 10) in vec4 inTangent; // xyz, w: the binormal's sign

layout(set = 1, binding = 0) uniform Transforms {
	mat4 worldViewProjection;
	mat4 world;
	vec4 lightmapTransform; // xy scale, zw offset into the lightmap atlas
	// The leaves' sway (RaShaderLeaf.fx, not OVERGROWTH): x on, y ObjRadius,
	// z WindSpeed + WIND_ADD, w GlobalTime.
	vec4 sway;
	// x: the normal map's space (0 none, 1 the tangent's: bundled and static
	// meshes, a skinned mesh's tangent-space map; 2 the object's).
	vec4 normalSpace;
	mat4 parts[64];         // bundled-mesh part transforms (identity for others; a
	                        // static mesh is sent only the first)
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
// A normal map's frame in the world (its columns: what the map's x, y and
// z are): the tangent, binormal and normal (RaShaderBM createWorld2TanMat,
// RaShaderSM getTangentBasis, RaShaderSTM getTanBasisTranspose; the
// binormal cross(tangent, normal) times the sign the vertex carries), or
// the object's axes for an object-space map (RaShaderSM
// _OBJSPACENORMALMAP_), skinned with the vertex.
layout(location = 9) out vec3 outBasisX;
layout(location = 10) out vec3 outBasisY;
layout(location = 11) out vec3 outBasisZ;

void main()
{
	mat4 part = parts[clamp(int(inPart + 0.5), 0, 63)] * inWeight +
		parts[clamp(int(inPart2 + 0.5), 0, 63)] * (1.0 - inWeight);
	vec4 local = part * vec4(inPosition, 1.0);
	if (sway.x > 0.5) {
		// inPos.xyz += sin((GlobalTime / (ObjRadius + y)) * WindSpeed) *
		// (ObjRadius + y)^2 / LEAF_MOVEMENT (1024).
		float r = max(sway.y + local.y, 1e-3);
		local.xyz += vec3(sin((sway.w / r) * sway.z) * r * r / 1024.0);
	}
	gl_Position = worldViewProjection * local;
	outWorldPosition = (world * local).xyz;
	mat3 toWorld = mat3(world) * mat3(part);
	outNormal = toWorld * inNormal;
	if (normalSpace.x > 1.5) {
		outBasisX = toWorld[0];
		outBasisY = toWorld[1];
		outBasisZ = toWorld[2];
	} else if (normalSpace.x > 0.5 && dot(inTangent.xyz, inTangent.xyz) > 1e-8) {
		vec3 binormal = normalize(cross(inTangent.xyz, inNormal)) * inTangent.w;
		outBasisX = toWorld * inTangent.xyz;
		outBasisY = toWorld * binormal;
		outBasisZ = outNormal;
	} else {
		outBasisX = vec3(1.0, 0.0, 0.0);
		outBasisY = vec3(0.0, 0.0, 1.0);
		outBasisZ = outNormal;
	}
	outTexCoord = inTexCoord;
	outDepth = gl_Position.w;
	outLightmapCoord = inLightmapCoord * lightmapTransform.xy + lightmapTransform.zw;
	outDetailCoord = inDetailCoord;
	outDirtCoord = inDirtCoord;
	outCrackCoord = inCrackCoord;
	outObjectY = world[3][1];
}
