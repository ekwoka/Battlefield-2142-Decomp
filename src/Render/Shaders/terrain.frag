#version 450
// Terrain: colormap lit by the patch lightmap, with detail textures.
//
// Lighting as TerrainShader_Hi.fx: light = 2 * g * sun + saturate(2 * b * GI) * 0.5
// and color = colormap * detail * light * 2, where RendDX9 uploads
// sun = sunColor / 4 and GI = GIColor / 2 (0x100b0570, 0x100b0670).
//
// Detail: each patch has up to two detail maps whose RGB channels weight
// detail layers 0-2 and 3-5; each layer tiles its own texture across the
// patch. Detail textures centre on 0.5, so 2 * detail leaves the colormap
// unchanged on average; it fades out with distance. RendDX9 draws each
// layer's triangles with the layer's pass (0x1014e570): side-projected
// layers (FullDetailMounten) add x and z planes from their side texture,
// envmap layers (FullDetailWithEnvMap) lerp towards the current cube; here
// one pass blends all six by their weights (detailBlend).
//
// Low detail (TerrainShader_Hi.fx, Hi_PS_FullDetail with HIGHTERRAIN): the
// level's lowDetailTexture tiled across the terrain by the far tilings
// (terraindata.raw's farTopTiling on the y plane, farSideTiling and
// farYOffset on the x and z planes) modulates both: with the patch's
// low-detail component map c (absent for the primary patches, so filled
// with red: c = (1, 0, 0)),
//   low = lerp(0.5, Y.b, c.r * (0.5 + 0.5 * f)) * 4 * lerp(0.5, mountain, c.b)
//   detail = lerp(2 * detail * low, low, f)
// where f is the geomorph's interpolation towards the next LOD (LOD 0
// cells; the farther cells are the low-detail pass, f = 1) and the
// mountain term blends the three planes' x/y/z channels by the normal less
// vBlendMod (0.2, 0.5, 0.2: the effect's default, RendDX9 never sets it).
// The planes' coordinates: a patch across (TEXSCALE), -0.00615148 a metre
// up. The low-detail pass (Shared_PS_LowDetail) also tints the ground under
// the sea: lerp(color, TerrainWaterColor, saturate((WaterHeight - y) / 3)),
// here by f too so it fades in with the LOD.
// The dynamic objects' shadows darken the sun term (shadow.glsl). RendDX9
// draws the patches twice for it, per patch kind (+0xf8, 0..2): those within
// the shadow distance into a dynamic light map (TerrainMakeDynamicLightMap,
// 0x1014f1d0), then again with it (TerrainDynamicShadow, 0x1014f400, from the
// terrain pass 0x100b4d80); here the map is sampled in the one pass.
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in float inDepth;
layout(location = 3) in vec3 inWorldPosition;
layout(location = 4) in float inFade;

layout(set = 2, binding = 0) uniform sampler2D colorMap;
layout(set = 2, binding = 1) uniform sampler2D lightMap;
layout(set = 2, binding = 2) uniform sampler2D detailWeights0;
layout(set = 2, binding = 3) uniform sampler2D detailWeights1;
layout(set = 2, binding = 4) uniform sampler2D layer0;
layout(set = 2, binding = 5) uniform sampler2D layer1;
layout(set = 2, binding = 6) uniform sampler2D layer2;
layout(set = 2, binding = 7) uniform sampler2D layer3;
layout(set = 2, binding = 8) uniform sampler2D layer4;
layout(set = 2, binding = 9) uniform sampler2D layer5;
layout(set = 2, binding = 10) uniform sampler2D lowDetailTexture;
#define SHADOW_UNIFORM_BINDING 2
#define SHADOW_MAP_BINDING 11
#include "shadow.glsl"
layout(set = 2, binding = 13) uniform samplerCube envCube;  // the current cube (EnvMaps.h)
layout(set = 2, binding = 14) uniform sampler2D sideLayerA; // "<texture>_side" of a side-projected layer
layout(set = 2, binding = 15) uniform sampler2D sideLayerB;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection;
	vec4 sunColor;     // terrain sun color
	vec4 skyColor;     // terrain GI color
	vec4 fogColor;     // rgb, w: the near term's floor (FogColor.w)
	vec4 fogRange;     // RaCommon.fx FogRange (fog.glsl)
	vec4 material;     // y: 1 with a lightmap
	vec4 uvOffset;
	vec4 tint;
	vec4 lightCount;   // x: point lights in use
	vec4 lightPosition[8]; // xyz, w: the range where it is gone
	vec4 lightColor[8];    // rgb, w: the range it is full up to
	vec4 lightSpot[8];     // a spot light's axis, w: its ConeAngle (radians;
						   // below -1: a point light)
	vec4 lightExtra[8];    // y: 1 when it lights lightmapped surfaces too
};
#include "fog.glsl"

// A light's cone (1 for a point light) and whether it lights this surface
// (affectLightmappedObjects 0 leaves lightmapped ones to their lightmaps).
// The cone is SkinnedMesh.fx's: saturate(cos - (1 - c)) / c with c the
// light's ConeAngle (coneAngle2 in radians, BF2142.exe 0x0064aad0).
float lightCone(int i, vec3 fromLight)
{
	if (material.y > 0.5 && lightExtra[i].y < 0.5) {
		return 0.0;
	}
	if (lightSpot[i].w < -1.0) {
		return 1.0;
	}
	float c = max(lightSpot[i].w, 1e-3);
	return clamp(dot(fromLight, lightSpot[i].xyz) - (1.0 - c), 0.0, 1.0) / c;
}

layout(set = 3, binding = 1) uniform Detail {
	vec4 tiling0;      // layers 0-3: repeats across a patch
	vec4 tiling1;      // layers 4-5
	vec4 detailFade;   // z: 1 if the patch has detail
	vec4 farTiling;    // side x, side y, top, y offset (repeats across a patch)
	vec4 farScale;     // x: 1 / a patch's metres, y: 1 with a low-detail texture, w: a metre up
	vec4 water;        // TerrainWaterColor, WaterHeight
	vec4 layerInfo[6]; // NEARTEXTILING's side tiling (xy) and y offset (z); w: the pass (TerrainLayerKind)
	vec4 eye;          // the camera (vCamerapos)
};

layout(location = 0) out vec4 outColor;

// The x and z planes' blend by the normal (Hi_VS_FullDetailMounten:
// saturate(|n| - vBlendMod) over its sum, vBlendMod the effect's default
// (0.2, 0.5, 0.2)), shared with the low-detail texture's planes.
vec3 planeBlend()
{
	vec3 blend = clamp(abs(normalize(inNormal)) - vec3(0.2, 0.5, 0.2), 0.0, 1.0);
	return blend / max(dot(blend, vec3(1.0)), 1e-4);
}

// A layer's colour (rgb) and alpha. Its pass (RendDX9 0x1014e570):
// FullDetail and FullDetailWithEnvMap sample the texture on the y plane at
// the patch coordinate times the tiling; FullDetailMounten adds the x and
// z planes (TerrainShader_Hi.fx tex.xy / tex.zy: the patch coordinate
// across, -0.00615148 a metre up) from the side texture, at that times
// the side tiling plus the y offset on the vertical, blended by the normal.
vec4 layerSample(sampler2D layer, float tiling, int n, vec3 blend)
{
	vec4 y = texture(layer, inTexCoord * tiling);
	int kind = int(layerInfo[n].w + 0.5);
	if (kind < 1 || kind > 3) {
		return y;
	}
	vec4 info = layerInfo[n];
	float up = -inWorldPosition.y * farScale.w * info.y + info.z;
	vec2 xPlane = vec2(inTexCoord.y * info.x, up);
	vec2 zPlane = vec2(inTexCoord.x * info.x, up);
	vec4 x, z;
	if (kind == 2) {
		x = texture(sideLayerA, xPlane);
		z = texture(sideLayerA, zPlane);
	} else if (kind == 3) {
		x = texture(sideLayerB, xPlane);
		z = texture(sideLayerB, zPlane);
	} else {
		x = texture(layer, xPlane);
		z = texture(layer, zPlane);
	}
	return x * blend.x + y * blend.y + z * blend.z;
}

// The detail layers blended by their weights: per layer the full-detail
// pass's detailout = lerp(2 x detail x low, low, f), and for an envmap
// layer (FullDetailWithEnvMap) the share k = detail.a x (1 - f) of the
// lit colour that goes to the cube: lerp(lit, cube, k) x 2. Returns
// sum(w (1 - k) detailout) / sum(w) in rgb and sum(w k) / sum(w) in a,
// so the lit colour is lit x rgb + 2 x cube x a.
vec4 detailBlend(float low, float fade)
{
	vec3 w0 = texture(detailWeights0, inTexCoord).rgb;
	vec3 w1 = texture(detailWeights1, inTexCoord).rgb;
	// Layer n's weight is the map's (b, g, r)[n % 3] (RendDX9 0x10178cd0:
	// vComponentsel (n%3 == 2, n%3 == 1, n%3 == 0) against the component
	// map, TerrainShader_Hi.fx's chartcontrib).
	float weights[6] = float[6](w0.b, w0.g, w0.r, w1.b, w1.g, w1.r);
	float total = dot(w0, vec3(1.0)) + dot(w1, vec3(1.0));
	if (total <= 0.01) {
		return vec4(vec3(low), 0.0);
	}
	vec3 blend = planeBlend();
	vec4 samples[6];
	samples[0] = layerSample(layer0, tiling0.x, 0, blend);
	samples[1] = layerSample(layer1, tiling0.y, 1, blend);
	samples[2] = layerSample(layer2, tiling0.z, 2, blend);
	samples[3] = layerSample(layer3, tiling0.w, 3, blend);
	samples[4] = layerSample(layer4, tiling1.x, 4, blend);
	samples[5] = layerSample(layer5, tiling1.y, 5, blend);
	vec3 sum = vec3(0.0);
	float env = 0.0;
	for (int n = 0; n < 6; ++n) {
		vec3 detailOut = mix(2.0 * samples[n].rgb * low, vec3(low), fade);
		float k = int(layerInfo[n].w + 0.5) == 4 ? samples[n].a * (1.0 - fade) : 0.0;
		sum += weights[n] * (1.0 - k) * detailOut;
		env += weights[n] * k;
	}
	return vec4(sum, env) / total;
}

// The low-detail modulation for a component map value (r: flat, b: mountain).
float lowDetail(vec3 component, float fade)
{
	if (farScale.y <= 0.0) {
		return 1.0;
	}
	vec3 tex = vec3(inWorldPosition.z * farScale.x, -inWorldPosition.y * farScale.w, inWorldPosition.x * farScale.x);
	vec4 yPlane = texture(lowDetailTexture, tex.zx * farTiling.z);
	vec4 xPlane = texture(lowDetailTexture, tex.xy * farTiling.xy + vec2(0.0, farTiling.w));
	vec4 zPlane = texture(lowDetailTexture, tex.zy * farTiling.xy + vec2(0.0, farTiling.w));
	vec3 blend = clamp(abs(normalize(inNormal)) - vec3(0.2, 0.5, 0.2), 0.0, 1.0);
	blend /= max(dot(blend, vec3(1.0)), 1e-4);
	float mountain = xPlane.g * blend.x + yPlane.r * blend.y + zPlane.g * blend.z;
	return mix(0.5, yPlane.b, component.r * (0.5 + 0.5 * fade)) * 4.0 * mix(0.5, mountain, component.b);
}

void main()
{
	vec3 albedo = texture(colorMap, inTexCoord).rgb;
	float fade = clamp(inFade, 0.0, 1.0);
	float low = lowDetail(vec3(1.0, 0.0, 0.0), fade);
	float envShare = 0.0;
	if (detailFade.z > 0.0) {
		vec4 detail = detailBlend(low, fade);
		albedo *= detail.rgb;
		envShare = detail.a;
	} else {
		albedo *= low;
	}
	vec3 color;
	if (material.y > 0.0) {
		vec4 lightmap = texture(lightMap, inTexCoord);
		vec3 sun = clamp(sunColor.rgb * 0.25, 0.0, 1.0);
		vec3 gi = clamp(skyColor.rgb * 0.5, 0.0, 1.0);
		vec3 light = 2.0 * lightmap.g * sun * shadowFactor(inWorldPosition, false) +
			clamp(2.0 * lightmap.b * gi, 0.0, 1.0) * 0.5;
		color = albedo * light * 2.0;
	} else {
		float sun = max(dot(normalize(inNormal), sunDirection.xyz), 0.0);
		color = albedo * (skyColor.rgb * 0.5 + sunColor.rgb * sun * shadowFactor(inWorldPosition, false));
	}
	if (envShare > 0.0) {
		// FullDetailWithEnvMap: the cube at the view reflected off the flat
		// ground (reflect(wPos - vCamerapos, (0, 1, 0)), Hi_VS_FullDetailWithEnvMap).
		vec3 view = inWorldPosition - eye.xyz;
		color += 2.0 * texture(envCube, vec3(view.x, -view.y, view.z)).rgb * envShare;
	}
	if (water.w > -1e8) {
		color = mix(color, water.rgb, clamp((water.w - inWorldPosition.y) / 3.0, 0.0, 1.0) * fade);
	}
	if (detailFade.x > 0.5) { // the LOD debug tint (BF2142_TERRAIN_LODS)
		color *= detailFade.x < 1.5 ? vec3(1.0, 0.6, 0.6) : detailFade.x < 2.5 ? vec3(0.6, 1.0, 0.6) : vec3(0.6, 0.6, 1.0);
	}
	float fog = fogAmount(inDepth);
	outColor = vec4(mix(color, fogColor.rgb, fog), 1.0);
	// The dynamic Point lights, each an additive pass over the lit terrain
	// in the original (0x1014e080, Hi_Terrain's PerPixelPointlight):
	// calcPVPointTerrain x 0.5 = colour x N.L x saturate(1 - d^2 / R^2) x
	// 0.5, not times the terrain's colour (lightPosition.w = 1 / R^2).
	vec3 normal = normalize(inNormal);
	int count = int(lightCount.x + 0.5);
	for (int i = 0; i < count && i < 8; ++i) {
		vec3 toLight = lightPosition[i].xyz - inWorldPosition;
		float attenuation = clamp(1.0 - dot(toLight, toLight) * lightPosition[i].w, 0.0, 1.0);
		float nl = max(dot(normalize(toLight), normal), 0.0);
		outColor.rgb += lightColor[i].rgb * (nl * attenuation * 0.5);
	}
	if (shadowExtra.w > 0.5) { // BF2142_SHADOW_DEBUG: the dynamic shadow factor, the region tinted
		vec4 p = shadowMatrix * vec4(inWorldPosition, 1.0);
		bool inside = all(greaterThan(p.xy, vec2(0.0))) && all(lessThan(p.xy, vec2(1.0)));
		outColor = vec4(vec3(shadowFactor(inWorldPosition, false)) * (inside ? vec3(1.0, 0.6, 0.6) : vec3(1.0)), 1.0);
	}
}
