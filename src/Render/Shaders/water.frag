#version 450
// Sea water, a first approximation of RaShaderWater: the level's water
// color, a sky reflection weighted by a Fresnel term, sun specular, and
// opacity growing with depth from renderer.waterAlphaAdd.
layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in float inWaterDepth;
layout(location = 2) in float inDepth;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection;
	vec4 sunColor;
	vec4 skyColor;
	vec4 fogColor;
	vec4 material;
};

layout(set = 3, binding = 1) uniform Water {
	vec4 cameraPosition; // w: time in seconds
	vec4 waterColor;
	vec4 specularColor;  // w: intensity
	vec4 parameters;     // x: alpha add, y: alpha height multiplier, z: specular power
};

layout(location = 0) out vec4 outColor;

void main()
{
	float time = cameraPosition.w;
	vec2 p = inWorldPosition.xz;
	// Small procedural ripples until the water normal maps are used.
	vec3 normal = normalize(vec3(0.03 * sin(p.x * 0.35 + time * 1.3) + 0.02 * sin(p.y * 0.6 - time * 0.9), 1.0,
		0.03 * cos(p.y * 0.4 + time * 1.1) + 0.02 * cos(p.x * 0.55 - time * 1.7)));
	vec3 view = normalize(inWorldPosition - cameraPosition.xyz);
	float fresnel = pow(1.0 - max(dot(-view, normal), 0.0), 4.0);
	vec3 reflected = reflect(view, normal);
	float specular = pow(max(dot(reflected, sunDirection.xyz), 0.0), parameters.z);
	vec3 color = mix(waterColor.rgb, fogColor.rgb, 0.25 + 0.65 * fresnel) +
		sunColor.rgb * specularColor.rgb * specularColor.w * specular;
	float alpha = clamp(parameters.x + max(inWaterDepth, 0.0) * max(parameters.y, 0.08) + fresnel * 0.5, 0.0, 0.95);
	float fog = clamp(inDepth / fogColor.w, 0.0, 1.0);
	outColor = vec4(mix(color, fogColor.rgb, fog * fog), alpha);
}
