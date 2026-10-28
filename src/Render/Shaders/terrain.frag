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
// unchanged on average; it fades out with distance.
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

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection;
	vec4 sunColor;     // terrain sun color
	vec4 skyColor;     // terrain GI color
	vec4 fogColor;
	vec4 material;     // y: 1 with a lightmap
	vec4 uvOffset;
	vec4 tint;
	vec4 lightCount;   // x: point lights in use
	vec4 lightPosition[8]; // xyz, w: the range where it is gone
	vec4 lightColor[8];    // rgb, w: the range it is full up to
	vec4 lightSpot[8];     // a spot light's axis, w: the cosine it is gone
						   // outside (below -1: a point light)
	vec4 lightExtra[8];    // x: the cosine it is full inside, y: 1 when
						   // it lights lightmapped surfaces too
};

// A light's cone (1 for a point light) and whether it lights this surface
// (affectLightmappedObjects 0 leaves lightmapped ones to their lightmaps).
float lightCone(int i, vec3 fromLight)
{
	if (material.y > 0.5 && lightExtra[i].y < 0.5) {
		return 0.0;
	}
	if (lightSpot[i].w < -1.0) {
		return 1.0;
	}
	float c = dot(fromLight, lightSpot[i].xyz);
	return smoothstep(lightSpot[i].w, max(lightExtra[i].x, lightSpot[i].w + 1e-3), c);
}

layout(set = 3, binding = 1) uniform Detail {
	vec4 tiling0;      // layers 0-3: repeats across a patch
	vec4 tiling1;      // layers 4-5
	vec4 detailFade;   // z: 1 if the patch has detail
	vec4 farTiling;    // side x, side y, top, y offset (repeats across a patch)
	vec4 farScale;     // x: 1 / a patch's metres, y: 1 with a low-detail texture, w: a metre up
	vec4 water;        // TerrainWaterColor, WaterHeight
};

layout(location = 0) out vec4 outColor;

vec3 detailColor()
{
	vec3 w0 = texture(detailWeights0, inTexCoord).rgb;
	vec3 w1 = texture(detailWeights1, inTexCoord).rgb;
	vec3 sum = w0.r * texture(layer0, inTexCoord * tiling0.x).rgb + w0.g * texture(layer1, inTexCoord * tiling0.y).rgb +
		w0.b * texture(layer2, inTexCoord * tiling0.z).rgb + w1.r * texture(layer3, inTexCoord * tiling0.w).rgb +
		w1.g * texture(layer4, inTexCoord * tiling1.x).rgb + w1.b * texture(layer5, inTexCoord * tiling1.y).rgb;
	float total = dot(w0, vec3(1.0)) + dot(w1, vec3(1.0));
	return total > 0.01 ? sum / total : vec3(0.5);
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
	if (detailFade.z > 0.0) {
		albedo *= mix(2.0 * detailColor() * low, vec3(low), fade);
	} else {
		albedo *= low;
	}
	vec3 color;
	if (material.y > 0.0) {
		vec4 lightmap = texture(lightMap, inTexCoord);
		vec3 sun = clamp(sunColor.rgb * 0.25, 0.0, 1.0);
		vec3 gi = clamp(skyColor.rgb * 0.5, 0.0, 1.0);
		vec3 light = 2.0 * lightmap.g * sun + clamp(2.0 * lightmap.b * gi, 0.0, 1.0) * 0.5;
		color = albedo * light * 2.0;
	} else {
		float sun = max(dot(normalize(inNormal), sunDirection.xyz), 0.0);
		color = albedo * (skyColor.rgb * 0.5 + sunColor.rgb * sun);
	}
	// Point lights, as mesh.frag's.
	vec3 normal = normalize(inNormal);
	int count = int(lightCount.x + 0.5);
	for (int i = 0; i < count && i < 8; ++i) {
		vec3 toLight = lightPosition[i].xyz - inWorldPosition;
		float distance = length(toLight);
		float range1 = lightColor[i].w, range2 = lightPosition[i].w;
		float attenuation = 1.0 - clamp((distance - range1) / max(range2 - range1, 0.01), 0.0, 1.0);
		vec3 direction = toLight / max(distance, 1e-3);
		color += albedo * lightColor[i].rgb * attenuation * lightCone(i, -direction) * max(dot(normal, direction), 0.0);
	}
	if (water.w > -1e8) {
		color = mix(color, water.rgb, clamp((water.w - inWorldPosition.y) / 3.0, 0.0, 1.0) * fade);
	}
	if (detailFade.x > 0.5) { // the LOD debug tint (BF2142_TERRAIN_LODS)
		color *= detailFade.x < 1.5 ? vec3(1.0, 0.6, 0.6) : detailFade.x < 2.5 ? vec3(0.6, 1.0, 0.6) : vec3(0.6, 0.6, 1.0);
	}
	float fog = clamp(inDepth / fogColor.w, 0.0, 1.0);
	outColor = vec4(mix(color, fogColor.rgb, fog * fog), 1.0);
}
