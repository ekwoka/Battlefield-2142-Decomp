#version 450
// Diffuse texture lit by the sun and sky, with alpha test and distance fog.
// Static meshes multiply in detail and dirt layers and blend cracks over
// them, as StaticMesh_r3x0.fx's ZAndDiffuse passes:
//   color = base * detail * dirt * (1 - crack.a) + crack.rgb * crack.a.
// With a lightmap (StaticMesh_dx9.dfx): g is sun visibility, b sky
// visibility; the sky term also lights sunlit surfaces, and the result is
// doubled: color = albedo * 2 * (0.52 * b * sky + g * sun * N.L).
// SDL_GPU binds fragment samplers in set 2 and uniform buffers in set 3.
layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in float inDepth;
layout(location = 3) in vec2 inLightmapCoord;
layout(location = 4) in vec2 inDetailCoord;
layout(location = 5) in vec2 inDirtCoord;
layout(location = 6) in vec2 inCrackCoord;

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
					   // z: layers (1 detail, 2 dirt, 4 crack), w: the object's
					   // fade (0 = none; else the alpha it is blended with)
};

layout(location = 0) out vec4 outColor;

void main()
{
	vec4 albedo = texture(diffuseMap, inTexCoord);
	int layers = int(material.z + 0.5);
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
	float fog = clamp(inDepth / fogColor.w, 0.0, 1.0);
	outColor = vec4(mix(albedo.rgb * light, fogColor.rgb, fog * fog), material.w > 0.0 ? material.w : albedo.a);
}
