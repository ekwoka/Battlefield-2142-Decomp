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
// The dynamic shadows darken the sun term (lightmapped surfaces: their
// lightmap's sun visibility); without a lightmap the hemi map gives the
// ambient and the sun's occlusion (shadow.glsl).
// SDL_GPU binds fragment samplers in set 2 and uniform buffers in set 3.
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in float inDepth;
layout(location = 3) in vec2 inLightmapCoord;
layout(location = 4) in vec2 inDetailCoord;
layout(location = 5) in vec2 inDirtCoord;
layout(location = 6) in vec2 inCrackCoord;
layout(location = 7) in vec3 inWorldPosition;
layout(location = 8) in float inObjectY;
layout(location = 9) in vec3 inBasisX; // the normal map's frame in the world (mesh.vert)
layout(location = 10) in vec3 inBasisY;
layout(location = 11) in vec3 inBasisZ;

layout(set = 2, binding = 0) uniform sampler2D diffuseMap;
layout(set = 2, binding = 1) uniform sampler2D lightMap;
layout(set = 2, binding = 2) uniform sampler2D detailMap;
layout(set = 2, binding = 3) uniform sampler2D dirtMap; // bundled and skinned meshes: their normal map
layout(set = 2, binding = 4) uniform sampler2D crackMap;
layout(set = 2, binding = 8) uniform sampler2D detailNormalMap; // static meshes: NDetail (alpha: the parallax height)
layout(set = 2, binding = 9) uniform sampler2D crackNormalMap;  // static meshes: NCrack
layout(set = 2, binding = 10) uniform samplerCube envMap;       // the level's nearest envmap
#define SHADOW_UNIFORM_BINDING 1
#define SHADOW_MAP_BINDING 5
#define HEMI_MAP_BINDING 7
#include "shadow.glsl"

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection; // towards the sun
	vec4 sunColor;
	vec4 skyColor;
	vec4 fogColor;     // rgb, w: the near term's floor (FogColor.w)
	vec4 fogRange;     // RaCommon.fx FogRange (fog.glsl)
	vec4 material;     // x: alpha reference (0 = off), y: 1 with a lightmap,
					   // z: layers (1 detail, 2 dirt, 4 crack) and the kind
					   // (8 blended, 16 additive, 32 hemi-lit: bundled and
					   // skinned meshes, 64 a normal map, 128 no dynamic
					   // shadows), w: the object's fade (0 = none; else the
					   // alpha it is blended with)
	vec4 uvOffset;     // xy: added to the diffuse coordinates (additive scrolling,
					   // an atlas's offset); zw: their scale in an atlas (0: none)
	vec4 tint;         // multiplies the texture (mesh particles' colour)
	vec4 lightCount;   // x: point lights in use
	vec4 lightPosition[8]; // xyz, w: the range where it is gone
	vec4 lightColor[8];    // rgb, w: the range it is full up to
	vec4 lightSpot[8];     // a spot light's axis, w: its ConeAngle (radians;
						   // below -1: a point light)
	vec4 lightExtra[8];    // y: 1 when it lights lightmapped surfaces too
	vec4 hemiObject;       // rgb: HemiMapConstantColor, w: HeightOverTerrain
	vec4 eye;              // xyz: the camera, w: the specular kind (0 none)
	vec4 specular;         // rgb: its colour, w: the static gloss
	vec4 layerMaps;        // xy: ParallaxScaleBias's scale, z: 1 detail normal map,
						   // 2 crack normal map, 4 parallax, 8 the envmap
};
#include "fog.glsl"

// The specular term. Static meshes (RaShaderSTM.fx getSpecularPixelLighting):
// pow(saturate(N.H), 32) x the lightmap's sun (g) x the gloss (StaticGloss,
// or the detail map's alpha) x StaticSpecularColor. Bundled meshes
// (RaShaderBM.fx): SpecLUT64SpecularColor at N.H (texel i = (i/255)^64 x
// sunSpecColor, 0x1014aec0) x the gloss (the colour map's alpha with
// ColormapGloss, else the normal map's alpha with one, else StaticGloss),
// killed in the shadows. Skinned meshes (RaShaderSM.fx): pow(N.H,
// SpecularPower) x the gloss (the normal map's alpha, else StaticGloss) x
// the sun's specular colour, SpecularPower the material block's default 20
// (0x10040070 +0x74); without a normal map lit() gives none facing away.
vec3 specularTerm(vec3 normal, float sunVisible, float gloss)
{
	int kind = int(eye.w + 0.5);
	if (kind == 0) {
		return vec3(0.0);
	}
	vec3 h = normalize(sunDirection.xyz + normalize(eye.xyz - inWorldPosition));
	float nh = clamp(dot(normal, h), 0.0, 1.0);
	float power = kind <= 2 ? 32.0 : kind == 5 ? 20.0 : 64.0;
	if (kind == 5 && dot(normal, sunDirection.xyz) <= 0.0) {
		return vec3(0.0);
	}
	return pow(nh, power) * sunVisible * gloss * specular.rgb;
}

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
	int extra = int(layerMaps.z + 0.5);
	float fade = material.w > 0.0 ? material.w : 1.0;
	float fog = fogAmount(inDepth);
	if ((layers & 16) != 0) {
		outColor = vec4(mix(albedo.rgb, fogColor.rgb, fog), albedo.a * fade);
		return;
	}
	vec3 normal = normalize(inNormal);
	vec3 toEye = normalize(eye.xyz - inWorldPosition);
	// A static mesh's per-pixel path (RaShaderSTM PERPIXEL: NDetail, NCrack,
	// parallax): the eye in the tangent frame (mesh.vert's basis).
	bool perPixel = (extra & 7) != 0;
	vec3 tangentEye = normalize(vec3(dot(toEye, inBasisX), dot(toEye, inBasisY), dot(toEye, inBasisZ)));
	vec2 detailCoord = inDetailCoord;
	if ((extra & 4) != 0) {
		// calculateParallaxCoordinatesFromAlpha: the normal map's alpha
		// times the scale, less half of it, along the eye's (x, -y).
		vec2 height = texture(detailNormalMap, inDetailCoord).aa * layerMaps.xy - 0.5 * layerMaps.xy;
		detailCoord = inDetailCoord + height * tangentEye.xy * vec2(1.0, -1.0);
	}
	vec4 detail = vec4(1.0);
	if ((layers & 1) != 0) {
		detail = texture(detailMap, detailCoord);
		albedo *= detail;
	}
	if ((layers & 2) != 0) {
		albedo.rgb *= texture(dirtMap, inDirtCoord).rgb;
	}
	vec4 crack = vec4(0.0);
	if ((layers & 4) != 0) {
		crack = texture(crackMap, inCrackCoord);
		albedo.rgb = albedo.rgb * (1.0 - crack.a) + crack.rgb * crack.a;
	}
	if (material.x > 0.0 && albedo.a < material.x) {
		discard;
	}
	int kind = int(eye.w + 0.5);
	float gloss = kind == 4 ? albedo.a : specular.w;
	if (kind == 2) {
		gloss = (layers & 1) != 0 ? detail.a : specular.w;
	}
	// A normal map (RaShaderBM/SM ps: tex * 2 - 1, renormalised) turns the
	// per-pixel lighting's normal; its alpha is the gloss unless the colour
	// map's is (ColormapGloss). A static mesh's (getCompositeNormals): the
	// detail normal map, the crack normal map over it by the crack's alpha.
	vec3 lit = normal;
	vec3 tangentNormal = vec3(0.0, 0.0, 1.0);
	if ((layers & 64) != 0) {
		vec4 bump = texture(dirtMap, inTexCoord * atlasScale + uvOffset.xy);
		vec3 n = normalize(bump.xyz * 2.0 - 1.0);
		lit = normalize(inBasisX * n.x + inBasisY * n.y + inBasisZ * n.z);
		if (kind != 4) {
			gloss = bump.a;
		}
	} else if (perPixel) {
		vec3 n = texture(detailNormalMap, detailCoord).rgb;
		if ((extra & 2) != 0) {
			n = mix(n, texture(crackNormalMap, inCrackCoord).rgb, crack.a);
		}
		tangentNormal = normalize(n * 2.0 - 1.0);
		lit = normalize(inBasisX * tangentNormal.x + inBasisY * tangentNormal.y + inBasisZ * tangentNormal.z);
	}
	// RaShaderBM _HASENVMAP_: the colour towards the envmap's reflection by
	// a quarter of the gloss, before the lighting.
	bool envMapped = (extra & 8) != 0;
	if (envMapped && (kind == 3 || kind == 4)) {
		albedo.rgb = mix(albedo.rgb, texture(envMap, reflect(-toEye, normal)).rgb, gloss / 4.0);
	}
	vec3 sun = sunColor.rgb * max(dot(lit, sunDirection.xyz), 0.0);
	vec3 light;
	vec3 shine = vec3(0.0);
	bool shadowed = (layers & 128) == 0;
	if (material.y > 0.0) {
		vec4 lightmap = texture(lightMap, inLightmapCoord);
		float sunVisible = lightmap.g * shadowFactor(inWorldPosition, false);
		// getDiffusePixelLighting: the sky by the bumped normal against
		// skyNormal (0.78, 0.52, 0.65) in the tangent frame.
		float skyShare = perPixel ? dot(tangentNormal, vec3(0.78, 0.52, 0.65)) : 0.52;
		light = 2.0 * (skyShare * lightmap.b * skyColor.rgb + sun * sunVisible);
		shine = specularTerm(lit, sunVisible, gloss);
	} else {
		// Bundled and skinned meshes (32) take the hemi map (RaShaderBM/SM's
		// _USEHEMIMAP_: by the vertex normal, a skinned mesh's per pixel);
		// unlit statics the sky colour.
		vec3 hemiNormal = kind == 5 ? lit : normal;
		vec4 hemi = (layers & 32) != 0 ? hemiLight(inWorldPosition, hemiNormal, inObjectY, skyColor.rgb, hemiObject)
									   : vec4(skyColor.rgb, 1.0);
		float sunVisible = hemi.a * (shadowed ? shadowFactor(inWorldPosition, true) : 1.0);
		light = hemi.rgb + sun * sunVisible;
		shine = specularTerm(lit, sunVisible * (sunColor.a > 0.0 ? 1.0 : 0.0), gloss);
	}
	light += pointLights(lit);
	float alpha = (layers & 8) != 0 ? albedo.a * fade : material.w > 0.0 ? material.w : albedo.a;
	vec3 color = albedo.rgb * light + shine;
	// RaShaderSTM's envmap, after the lighting: per pixel towards the cube
	// at the reflection of the eye off the bumped normal by (1 -
	// saturate(0.95 (E.N)^2)) x the gloss; per vertex by gloss x 2 x (1 -
	// |E.N|) (unclamped, as the shader's lerp).
	if (envMapped && (kind == 1 || kind == 2)) {
		float en = dot(toEye, lit);
		float share = perPixel ? (1.0 - clamp(en * en * 0.95, 0.0, 1.0)) * gloss : gloss * 2.0 * (1.0 - abs(en));
		color = mix(color, texture(envMap, reflect(-toEye, lit)).rgb, share);
	}
	outColor = vec4(mix(color, fogColor.rgb, fog), alpha);
}
