#version 450
// Sea water, after RaShaderWaterBase.fx's Water pixel shader (the PS_20
// variant with USE_FRESNEL and USE_SPECULAR). The normal: in the 3D variants
// (USE_3DTEXTURE) the volume Textures/watervolume.dds at
//   xy = P.xz / (29.13, 31.81) + WaterScroll * cycle
//   z = cycle * 10 + 0.7 x + 1.13 y
// and in the 2D ones (no volume) two waterbump frames at P.xz / (99.13,
// 71.81) blended by the cycle:
//   N = sample * 2 - 1         (world space, green up)
//   R = reflect(P - camera, N)
//   env = the level's water cube map (Water/envmap.dds) at R
//   specular = pow(saturate(dot(towards sun, normalize(R))), SpecularPower) * SpecularColor.a
//   color = specular * SpecularColor * shade + lerp(WaterColor, env, 0.4 + m)
//   m = -(1 - saturate(shade + 0.75))   (SHADOW_FACTOR)
//   alpha = depth alpha * (1.5 - pow(dot(normalize(P - camera), up), 30)) + alpha add
// where the original's lightmap gives the depth alpha (r) and the sun's
// shade (g). The levels ship no water lightmaps, so the shader's
// NO_LIGHTMAP lightmap is PointColor, (1, terrain.waterSunIntensity, 0.8,
// 1) (0x100767e9): the shade is the sun intensity (waterColor.w here); the
// depth alpha grows with the water's depth (renderer.waterAlphaHeightMul).
layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in float inWaterDepth;
layout(location = 2) in float inDepth;

layout(set = 2, binding = 0) uniform sampler2D waterFrame0;
layout(set = 2, binding = 1) uniform sampler2D waterFrame1;
layout(set = 2, binding = 2) uniform samplerCube envMap;
layout(set = 2, binding = 3) uniform sampler3D waterVolume;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection;
	vec4 sunColor;
	vec4 skyColor;
	vec4 fogColor;
	vec4 material;
};

layout(set = 3, binding = 1) uniform Water {
	vec4 cameraPosition; // w: time in seconds
	vec4 waterColor;     // w: the sun's share (terrain.waterSunIntensity)
	vec4 specularColor;  // w: intensity
	vec4 parameters;     // x: alpha add, y: alpha height multiplier, z: specular power, w: 1 with an env map
	vec4 animation;      // xy: WaterScroll, z: WaterCycleTime, w: 1 with the volume
};

layout(location = 0) out vec4 outColor;

void main()
{
	vec3 normal;
	if (animation.w > 0.5) {
		vec3 tex;
		tex.xy = inWorldPosition.xz / vec2(29.13, 31.81) + animation.xy * animation.z;
		tex.z = animation.z * 10.0 + (tex.x * 0.7 + tex.y * 1.13);
		normal = texture(waterVolume, tex).rgb * 2.0 - 1.0;
	} else {
		vec2 tex = inWorldPosition.xz / vec2(99.13, 71.81);
		normal = mix(texture(waterFrame0, tex).rgb, texture(waterFrame1, tex).rgb, animation.z) * 2.0 - 1.0;
	}
	vec3 lookup = inWorldPosition - cameraPosition.xyz;
	vec3 reflected = reflect(lookup, normal);
	vec3 env = parameters.w > 0.0 ? texture(envMap, reflected).rgb : fogColor.rgb;
	float specular = pow(clamp(dot(sunDirection.xyz, normalize(reflected)), 0.0, 1.0), parameters.z) * specularColor.w;
	float shade = waterColor.w;
	float lerpMod = -(1.0 - clamp(shade + 0.75, 0.0, 1.0));
	vec3 color = specular * specularColor.rgb * shade + mix(waterColor.rgb, env, 0.4 + lerpMod);
	float fresnel = 1.5 - pow(abs(dot(normalize(lookup), vec3(0.0, 1.0, 0.0))), 30.0);
	float depthAlpha = clamp(max(inWaterDepth, 0.0) * max(parameters.y, 0.08), 0.0, 1.0);
	float alpha = clamp(depthAlpha * fresnel + parameters.x, 0.0, 1.0);
	float fog = clamp(inDepth / fogColor.w, 0.0, 1.0);
	outColor = vec4(mix(color, fogColor.rgb, fog * fog), alpha);
}
