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
layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in float inDepth;

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

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection;
	vec4 sunColor;     // terrain sun color
	vec4 skyColor;     // terrain GI color
	vec4 fogColor;
	vec4 material;     // y: 1 with a lightmap
};

layout(set = 3, binding = 1) uniform Detail {
	vec4 tiling0;      // layers 0-3: repeats across a patch
	vec4 tiling1;      // layers 4-5
	vec4 detailFade;   // x: start, y: end distance; z: 1 if the patch has detail
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

void main()
{
	vec3 albedo = texture(colorMap, inTexCoord).rgb;
	if (detailFade.z > 0.0) {
		float fade = clamp((inDepth - detailFade.x) / (detailFade.y - detailFade.x), 0.0, 1.0);
		albedo *= mix(2.0 * detailColor(), vec3(1.0), fade);
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
	float fog = clamp(inDepth / fogColor.w, 0.0, 1.0);
	outColor = vec4(mix(color, fogColor.rgb, fog * fog), 1.0);
}
