#version 450
// The bloom's filters (FSQuadDrawer.fx), one per pass by params.x:
//  0 FSBMContrastFilter: the source cubed, to luminance, less CONTRAST
//    (bloomFilterContrast), into the small target;
//  1, 2 FSBMLargeBloomFilter across and down: thirteen taps 0.01 and 0.015
//    x BLOOMSIZE x 1..3 of the screen apart, weighted 1, 0.45 ... 0.15
//    times BLOOMWEIGHTSCALE, times 0.9;
//  3 FSBMGaussianBlur5x5CheapFilter: a 5 x 5 Gaussian in 13 taps;
//  4 a copy (the tinnitus's frame history).
layout(location = 0) in vec2 inTexCoord;

layout(set = 2, binding = 0) uniform sampler2D source;

layout(set = 3, binding = 0) uniform Bloom {
	vec4 params; // x: the filter, y: contrast, z: size, w: weight scale
	vec4 texel;  // xy: a source texel
};

layout(location = 0) out vec4 outColor;

const vec3 kLuminance = vec3(0.3086, 0.6094, 0.0820);

vec4 largeBloom(vec2 axis)
{
	const float weights[6] = float[6](0.45, 0.35, 0.30, 0.25, 0.20, 0.15);
	vec4 color = texture(source, inTexCoord);
	for (int i = 0; i < 3; ++i) {
		float s = params.z * float(i + 1);
		color += weights[i * 2] * texture(source, inTexCoord + axis * 0.01 * s);
		color += weights[i * 2 + 1] * texture(source, inTexCoord + axis * 0.015 * s);
		color += weights[5 - i * 2] * texture(source, inTexCoord - axis * 0.01 * (float(3 - i)) * params.z);
		color += weights[4 - i * 2] * texture(source, inTexCoord - axis * 0.015 * (float(3 - i)) * params.z);
	}
	return color * params.w * 0.9;
}

void main()
{
	int mode = int(params.x + 0.5);
	if (mode == 0) {
		vec3 c = clamp(pow(texture(source, inTexCoord).rgb, vec3(3.0)), 0.0, 1.0);
		outColor = vec4(vec3(dot(c, kLuminance) - params.y), 1.0);
	} else if (mode == 1) {
		outColor = largeBloom(vec2(1.0, 0.0));
	} else if (mode == 2) {
		outColor = largeBloom(vec2(0.0, 1.0));
	} else if (mode == 3) {
		vec2 t = texel.xy;
		vec4 sum = texture(source, inTexCoord) * 0.16;
		sum += (texture(source, inTexCoord + vec2(t.x, 0)) + texture(source, inTexCoord - vec2(t.x, 0)) +
				   texture(source, inTexCoord + vec2(0, t.y)) + texture(source, inTexCoord - vec2(0, t.y))) * 0.1;
		sum += (texture(source, inTexCoord + t) + texture(source, inTexCoord - t) +
				   texture(source, inTexCoord + vec2(t.x, -t.y)) + texture(source, inTexCoord + vec2(-t.x, t.y))) * 0.065;
		sum += (texture(source, inTexCoord + vec2(2.0 * t.x, 0)) + texture(source, inTexCoord - vec2(2.0 * t.x, 0)) +
				   texture(source, inTexCoord + vec2(0, 2.0 * t.y)) + texture(source, inTexCoord - vec2(0, 2.0 * t.y))) * 0.045;
		outColor = vec4(sum.rgb, 1.0);
	} else {
		outColor = texture(source, inTexCoord);
	}
}
