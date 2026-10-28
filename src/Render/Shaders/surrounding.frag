#version 450
// The surrounding terrain: the secondary heightmaps around the level, as
// TerrainShader_Shared.fx's Shared_SurroundingTerrain (Shared_PS_STNormal)
// draws them. Their colormaps (<colormap base>_s<i>.dds) hold the light
// already, so there is no lightmap; the low-detail component map
// (<low-detail base>_s<i>.dds) weights the low-detail texture's flat (r ->
// its b channel) and mountain (b -> the planes' r/g channels) parts:
//   low = lerp(0.5, Y.b, c.r) * lerp(0.5, mountain, c.b)
//   color = low * colormap * 4
layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in float inDepth;
layout(location = 3) in vec3 inWorldPosition;

layout(set = 2, binding = 0) uniform sampler2D colorMap;
layout(set = 2, binding = 1) uniform sampler2D lowComponentMap;
layout(set = 2, binding = 2) uniform sampler2D lowDetailTexture;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection;
	vec4 sunColor;
	vec4 skyColor;
	vec4 fogColor; // w: fog end
	vec4 material;
};

layout(set = 3, binding = 1) uniform Detail {
	vec4 tiling0;
	vec4 tiling1;
	vec4 detailFade;
	vec4 farTiling; // STFARTEXTILING: side x, side y, top, y offset
	vec4 farScale;  // x: STTEXSCALE.x (a metre across), y: 1 with a low-detail texture, w: a metre up
	vec4 water;
};

layout(location = 0) out vec4 outColor;

void main()
{
	vec3 colormap = texture(colorMap, inTexCoord).rgb;
	float low = 0.25;
	if (farScale.y > 0.0) {
		vec4 component = texture(lowComponentMap, inTexCoord);
		vec3 tex = vec3(inWorldPosition.z * farScale.x, -inWorldPosition.y * farScale.w, inWorldPosition.x * farScale.x);
		vec4 yPlane = texture(lowDetailTexture, tex.zx * farTiling.z);
		vec4 xPlane = texture(lowDetailTexture, tex.xy * farTiling.xy + vec2(0.0, farTiling.w));
		vec4 zPlane = texture(lowDetailTexture, tex.zy * farTiling.xy + vec2(0.0, farTiling.w));
		vec3 blend = clamp(abs(normalize(inNormal)) - vec3(0.2, 0.5, 0.2), 0.0, 1.0); // vBlendMod
		blend /= max(dot(blend, vec3(1.0)), 1e-4);
		float mountain = xPlane.g * blend.x + yPlane.r * blend.y + zPlane.g * blend.z;
		low = mix(0.5, yPlane.b, component.r) * mix(0.5, mountain, component.b);
	}
	vec3 color = low * colormap * 4.0;
	float fog = clamp(inDepth / fogColor.w, 0.0, 1.0);
	outColor = vec4(mix(color, fogColor.rgb, fog * fog), 1.0);
}
