#version 450
// Diffuse texture lit by the sun and sky, with alpha test and distance fog.
// Static meshes multiply in detail and dirt layers and blend cracks over
// them, as StaticMesh_r3x0.fx's ZAndDiffuse passes:
//   color = base * detail * dirt * (1 - crack.a) + crack.rgb * crack.a.
// With a lightmap (StaticMesh_dx9.dfx): g is sun visibility, b sky
// visibility; the sky term also lights sunlit surfaces, and the result is
// doubled: color = albedo * 2 * (0.52 * b * sky + g * sun * N.L).
// Blended materials (alpha mode 1, BundledMesh's "Alpha" techniques) are
// lit the same and take their alpha from the texture (RaShaderBM.fx:
// outColor.a = diffuse.a * Transparency.a); additive ones (the geometry's
// hasAdditiveAlpha) are the texture unlit, its alpha times the
// transparency, fogged (RaShaderBMadditive.fx, technique depthAndFog), and
// their coordinates scroll (frac(GlobalTime * simpleUVTranslation)).
// Point lights (effect bundles' LightSources) add colour * N.L, full up to
// attenuationRange1 and fading to nothing at attenuationRange2 (the
// original reads a radial attenuation table over distance / range,
// LightManager_r3x0.fx; a linear ramp here).
// SDL_GPU binds fragment samplers in set 2 and uniform buffers in set 3.
layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in float inDepth;
layout(location = 3) in vec2 inLightmapCoord;
layout(location = 4) in vec2 inDetailCoord;
layout(location = 5) in vec2 inDirtCoord;
layout(location = 6) in vec2 inCrackCoord;
layout(location = 7) in vec3 inWorldPosition;

layout(set = 2, binding = 0) uniform sampler2D diffuseMap;
layout(set = 2, binding = 1) uniform sampler2D lightMap;
layout(set = 2, binding = 2) uniform sampler2D detailMap;
layout(set = 2, binding = 3) uniform sampler2D dirtMap;
layout(set = 2, binding = 4) uniform sampler2D crackMap;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection; // towards the sun
	vec4 sunColor;
	vec4 skyColor;
	vec4 fogColor;     // w: fog end distance
	vec4 material;     // x: alpha reference (0 = off), y: 1 with a lightmap,
					   // z: layers (1 detail, 2 dirt, 4 crack) and the kind
					   // (8 blended, 16 additive), w: the object's fade (0 =
					   // none; else the alpha it is blended with)
	vec4 uvOffset;     // xy: added to the diffuse coordinates (additive scrolling,
					   // an atlas's offset); zw: their scale in an atlas (0: none)
	vec4 tint;         // multiplies the texture (mesh particles' colour)
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

layout(location = 0) out vec4 outColor;

vec3 pointLights(vec3 normal)
{
	vec3 sum = vec3(0.0);
	int count = int(lightCount.x + 0.5);
	for (int i = 0; i < count && i < 8; ++i) {
		vec3 toLight = lightPosition[i].xyz - inWorldPosition;
		float distance = length(toLight);
		float range1 = lightColor[i].w, range2 = lightPosition[i].w;
		float attenuation = 1.0 - clamp((distance - range1) / max(range2 - range1, 0.01), 0.0, 1.0);
		vec3 direction = toLight / max(distance, 1e-3);
		sum += lightColor[i].rgb * attenuation * lightCone(i, -direction) * max(dot(normal, direction), 0.0);
	}
	return sum;
}

void main()
{
	vec2 atlasScale = uvOffset.z > 0.0 ? uvOffset.zw : vec2(1.0);
	vec4 albedo = texture(diffuseMap, inTexCoord * atlasScale + uvOffset.xy) * tint;
	int layers = int(material.z + 0.5);
	float fade = material.w > 0.0 ? material.w : 1.0;
	float fog = clamp(inDepth / fogColor.w, 0.0, 1.0);
	if ((layers & 16) != 0) {
		outColor = vec4(mix(albedo.rgb, fogColor.rgb, fog * fog), albedo.a * fade);
		return;
	}
	if ((layers & 1) != 0) {
		albedo *= texture(detailMap, inDetailCoord);
	}
	if ((layers & 2) != 0) {
		albedo.rgb *= texture(dirtMap, inDirtCoord).rgb;
	}
	if ((layers & 4) != 0) {
		vec4 crack = texture(crackMap, inCrackCoord);
		albedo.rgb = albedo.rgb * (1.0 - crack.a) + crack.rgb * crack.a;
	}
	if (material.x > 0.0 && albedo.a < material.x) {
		discard;
	}
	vec3 normal = normalize(inNormal);
	vec3 sun = sunColor.rgb * max(dot(normal, sunDirection.xyz), 0.0);
	vec3 light;
	if (material.y > 0.0) {
		vec4 lightmap = texture(lightMap, inLightmapCoord);
		light = 2.0 * (0.52 * lightmap.b * skyColor.rgb + lightmap.g * sun);
	} else {
		light = skyColor.rgb + sun;
	}
	light += pointLights(normal);
	float alpha = (layers & 8) != 0 ? albedo.a * fade : material.w > 0.0 ? material.w : albedo.a;
	outColor = vec4(mix(albedo.rgb * light, fogColor.rgb, fog * fog), alpha);
}
