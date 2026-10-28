// The dynamic shadows and the hemispheric ambient, shared by the lit
// fragment shaders. The includer defines the bindings first:
//   SHADOW_UNIFORM_BINDING  the Shadows block (set 3)
//   SHADOW_MAP_BINDING      the dynamic objects' shadow map (set 2), the
//                           static occluders' at the next binding
//   HEMI_MAP_BINDING        (optional) the hemi map (set 2)
//
// The shadows (RaCommon.fx calcShadowProjection / getShadowFactor): the
// position in the light's space, compared with the maps' depths at four
// taps half a texel apart (getShadowFactorExactOther), each a filtered
// comparison here. Statics are lit by their lightmaps, so terrain and
// lightmapped surfaces take only the dynamic objects' map; dynamic objects
// also the static occluders' (RaShaderBM's ShadowOccluderMap). Beyond the
// mapped region it fades to unshadowed.
//
// The hemi map (RaShaderBM.fx / RaShaderSM.fx calcGroundUVAndLerp): the
// map's texel under the point, a metre along its normal, gives the
// ground's colour (rgb) and the sun's occlusion there (a: roofs, bridges);
// the ambient lerps from that ground colour to HemiMapSkyColor by how much
// the normal faces up and how high on its object the point is.

layout(set = 3, binding = SHADOW_UNIFORM_BINDING) uniform Shadows {
	mat4 shadowMatrix;  // world -> the maps' uv (xy) and depth (z)
	vec4 shadowParams;  // x: 1 with shadows, y: a texel in uv, z: depth bias, w: 1 with the static map
	vec4 hemiConstants; // xy: the map's corner (world x, z), z: its size (0: no hemi map), w: lerp bias
	vec4 hemiSky;       // rgb: HemiMapSkyColor, w: 1 to take the sun's occlusion from it
	vec4 shadowExtra;   // x: shadowIntensityBias, y: the fade's start (share of the region), z: 1 for undergrowth
};

layout(set = 2, binding = SHADOW_MAP_BINDING) uniform sampler2DShadow dynamicShadowMap;
layout(set = 2, binding = SHADOW_MAP_BINDING + 1) uniform sampler2DShadow staticShadowMap;

float sampleShadow4(sampler2DShadow map, vec3 p, float texel)
{
	float h = texel * 0.5;
	return 0.25 * (texture(map, vec3(p.xy + vec2(-h, -h), p.z)) + texture(map, vec3(p.xy + vec2(h, -h), p.z)) +
		texture(map, vec3(p.xy + vec2(-h, h), p.z)) + texture(map, vec3(p.xy + vec2(h, h), p.z)));
}

// 1 lit, 0 in shadow.
float shadowFactor(vec3 worldPosition, bool withStatic)
{
	if (shadowParams.x < 0.5) {
		return 1.0;
	}
	vec4 p = shadowMatrix * vec4(worldPosition, 1.0);
	vec2 edge = abs(p.xy * 2.0 - 1.0);
	float reach = max(edge.x, edge.y);
	if (reach >= 1.0 || p.z >= 1.0 || p.z <= 0.0) {
		return 1.0;
	}
	vec3 q = vec3(p.xy, p.z - shadowParams.z);
	float lit = sampleShadow4(dynamicShadowMap, q, shadowParams.y);
	if (withStatic && shadowParams.w > 0.5) {
		lit = min(lit, sampleShadow4(staticShadowMap, q, shadowParams.y));
	}
	lit = clamp(lit + shadowExtra.x, 0.0, 1.0);
	float fade = clamp((reach - shadowExtra.y) / max(1.0 - shadowExtra.y, 1e-3), 0.0, 1.0);
	return mix(lit, 1.0, fade);
}

#ifdef HEMI_MAP_BINDING
layout(set = 2, binding = HEMI_MAP_BINDING) uniform sampler2D hemiMap;

// rgb: the ambient, a: the sun's visibility. `objectY` is the object's
// origin height (World[3][1]).
vec4 hemiLight(vec3 worldPosition, vec3 normal, float objectY, vec3 sky)
{
	if (hemiConstants.z <= 0.0) {
		return vec4(sky, 1.0);
	}
	vec2 uv = ((worldPosition + vec3(hemiConstants.z * 0.5) + normal).xz - hemiConstants.xy) / hemiConstants.z;
	uv.y = 1.0 - uv.y;
	vec4 ground = texture(hemiMap, uv);
	// localHeight at half a metre's scale (InvHemiHeightScale), on the ground
	// (HeightOverTerrain 0).
	float localHeight = (worldPosition.y - objectY) * 0.5;
	float offset = clamp(localHeight * 2.0 - 1.0, -2.0, 0.8);
	float lerpFactor = clamp((normal.y + offset) * 0.5 + 0.5, 0.0, 0.9);
	return vec4(mix(ground.rgb, hemiSky.rgb, lerpFactor), hemiSky.w > 0.5 ? ground.a : 1.0);
}
#endif
