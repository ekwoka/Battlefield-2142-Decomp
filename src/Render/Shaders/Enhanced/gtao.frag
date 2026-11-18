#version 450
// Ground-truth ambient occlusion (enhanced mode, not retail; Jimenez et
// al. 2016, "Practical Real-Time Strategies for Accurate Indirect
// Occlusion"): per pixel, two slices through the view vector, each
// searched four steps either way for its horizons within `radius` metres
// (fading to none at the radius), and the visible arc integrated
// cosine-weighted against the normal projected into the slice. The slices
// turn by a 4 x 4 pattern (aoblur.frag averages it away) that also moves
// each frame.
layout(location = 0) in vec2 inTexCoord;

layout(set = 2, binding = 0) uniform sampler2D normalDepth;

layout(set = 3, binding = 0) uniform Gtao {
	vec4 projection; // xy: 1 / the projection's x and y scales, zw: the jitter (clip space)
	vec4 params;     // x: the radius (metres), y: the depth beyond which nothing is occluded, z: the frame's turn
	vec4 texel;      // xy: 1 / the size, zw: the size
};

layout(location = 0) out vec4 outAO;

const float PI = 3.14159265;
const int SLICES = 2;
const int STEPS = 4;

vec3 viewPosition(vec2 uv, float z)
{
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - projection.zw;
	return vec3(ndc * projection.xy * z, z);
}

void main()
{
	vec4 center = textureLod(normalDepth, inTexCoord, 0.0);
	if (center.w >= params.y) {
		outAO = vec4(1.0);
		return;
	}
	vec3 p = viewPosition(inTexCoord, center.w);
	vec3 n = normalize(center.xyz);
	vec3 v = normalize(-p);
	float radius = params.x;
	// The radius in pixels at this depth.
	float pixels = min(radius / (p.z * projection.y) * 0.5 * texel.w, 96.0);
	if (pixels < 1.0) {
		outAO = vec4(1.0);
		return;
	}
	const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
	ivec2 cell = ivec2(gl_FragCoord.xy) & 3;
	float noise = fract((bayer[cell.y * 4 + cell.x] + 0.5) / 16.0 + params.z);
	float stepNoise = fract(noise * 3.7 + 0.25);
	float visibility = 0.0;
	for (int s = 0; s < SLICES; ++s) {
		float phi = (float(s) + noise) / float(SLICES) * PI;
		vec2 direction = vec2(cos(phi), sin(phi)); // on the screen, y down
		vec3 sliceDirection = vec3(direction.x, -direction.y, 0.0);
		vec3 axis = cross(sliceDirection, v);
		float axisLength = length(axis);
		if (axisLength < 1e-4) {
			visibility += 1.0;
			continue;
		}
		axis /= axisLength;
		vec3 ortho = sliceDirection - dot(sliceDirection, v) * v;
		vec3 projected = n - axis * dot(n, axis);
		float projectedLength = length(projected);
		float cosN = clamp(dot(projected, v) / max(projectedLength, 1e-4), 0.0, 1.0);
		float angleN = sign(dot(ortho, projected)) * acos(cosN);
		float horizon0 = -1.0, horizon1 = -1.0;
		for (int k = 0; k < STEPS; ++k) {
			float t = (float(k) + stepNoise) / float(STEPS);
			vec2 offset = direction * max(t * pixels, 1.0) * texel.xy;
			for (int side = 0; side < 2; ++side) {
				vec2 uv = side == 0 ? inTexCoord - offset : inTexCoord + offset;
				vec3 tap = viewPosition(uv, textureLod(normalDepth, uv, 0.0).w);
				vec3 d = tap - p;
				float length2 = dot(d, d);
				float cosH = dot(d, v) * inversesqrt(max(length2, 1e-6));
				cosH = mix(-1.0, cosH, clamp(1.0 - length2 / (radius * radius), 0.0, 1.0));
				if (side == 0) {
					horizon0 = max(horizon0, cosH);
				} else {
					horizon1 = max(horizon1, cosH);
				}
			}
		}
		float h0 = angleN + max(-acos(horizon0) - angleN, -PI * 0.5);
		float h1 = angleN + min(acos(horizon1) - angleN, PI * 0.5);
		float sinN = sin(angleN);
		visibility += projectedLength * 0.25 *
			((-cos(2.0 * h0 - angleN) + cosN + 2.0 * h0 * sinN) + (-cos(2.0 * h1 - angleN) + cosN + 2.0 * h1 * sinN));
	}
	outAO = vec4(clamp(visibility / float(SLICES), 0.0, 1.0));
}
