#version 450
// The GTAO's 4 x 4 pattern averaged away (enhanced mode, not retail): the
// 16 texels of the pattern about the pixel, each weighted down by how far
// its depth lies from the pixel's, so edges stay sharp.
layout(location = 0) in vec2 inTexCoord;

layout(set = 2, binding = 0) uniform sampler2D aoMap;
layout(set = 2, binding = 1) uniform sampler2D normalDepth;

layout(set = 3, binding = 0) uniform Blur {
	vec4 texel; // xy: 1 / the size
};

layout(location = 0) out vec4 outAO;

void main()
{
	float z = textureLod(normalDepth, inTexCoord, 0.0).w;
	float sum = 0.0, weights = 0.0;
	for (int y = -2; y < 2; ++y) {
		for (int x = -2; x < 2; ++x) {
			vec2 uv = inTexCoord + vec2(float(x), float(y)) * texel.xy;
			float d = textureLod(normalDepth, uv, 0.0).w;
			float w = 1.0 / (1e-3 + abs(d - z) / max(z * 0.02, 1e-3));
			w = min(w, 1.0);
			sum += textureLod(aoMap, uv, 0.0).r * w;
			weights += w;
		}
	}
	outAO = vec4(sum / max(weights, 1e-4));
}
