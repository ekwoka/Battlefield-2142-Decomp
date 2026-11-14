#version 450
// Temporal anti-aliasing's resolve (enhanced mode, not retail): this
// frame (drawn with a sub-pixel jitter) blended with the history (the
// last resolve, fetched where the pixel was: its motion, taken from the
// nearest of its 3 x 3 neighbours so edges move with their foreground),
// the history first clipped to the neighbourhood's colour range (YCoCg,
// mean +- 1.25 standard deviations within min / max), which rejects what
// was disoccluded or changed (particles, muzzle flashes). The first person
// (flagged in the motion, kept in the resolve's alpha as 0) has no motion,
// and neither takes history from the other (the world from where the weapon
// was last frame): on busy textures the clip lets it through, a ghost.
layout(location = 0) in vec2 inTexCoord;

layout(set = 2, binding = 0) uniform sampler2D sceneMap;
layout(set = 2, binding = 1) uniform sampler2D historyMap;
layout(set = 2, binding = 2) uniform sampler2D motionMap;
layout(set = 2, binding = 3) uniform sampler2D normalDepth;

layout(set = 3, binding = 0) uniform Taa {
	vec4 texel;  // xy: 1 / the size
	vec4 params; // x: 1 with a history, y: the history's weight
};

layout(location = 0) out vec4 outColor;

const float kFirstPerson = 4.0; // motion.frag writes 8: no real motion stays on screen

vec3 toYCoCg(vec3 c)
{
	return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 toRgb(vec3 c)
{
	return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// The history fetched with a Catmull-Rom filter in five bilinear taps
// (Jimenez, "Filmic SMAA", 2016): bilinear alone softens the image a
// little more each frame the camera moves.
vec3 sampleHistory(vec2 uv)
{
	vec2 size = 1.0 / texel.xy;
	vec2 position = uv * size;
	vec2 center = floor(position - 0.5) + 0.5;
	vec2 f = position - center;
	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);
	vec2 w12 = w1 + w2;
	vec2 tc0 = (center - 1.0) * texel.xy;
	vec2 tc3 = (center + 2.0) * texel.xy;
	vec2 tc12 = (center + w2 / w12) * texel.xy;
	vec3 c = textureLod(historyMap, vec2(tc12.x, tc0.y), 0.0).rgb * (w12.x * w0.y) +
		textureLod(historyMap, vec2(tc0.x, tc12.y), 0.0).rgb * (w0.x * w12.y) +
		textureLod(historyMap, vec2(tc12.x, tc12.y), 0.0).rgb * (w12.x * w12.y) +
		textureLod(historyMap, vec2(tc3.x, tc12.y), 0.0).rgb * (w3.x * w12.y) +
		textureLod(historyMap, vec2(tc12.x, tc3.y), 0.0).rgb * (w12.x * w3.y);
	float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
	return max(c / weight, vec3(0.0));
}

void main()
{
	vec3 current = vec3(0.0);
	vec3 sum = vec3(0.0), sum2 = vec3(0.0);
	vec3 low = vec3(1e9), high = vec3(-1e9);
	float nearest = 1e30;
	vec2 nearestUV = inTexCoord;
	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			vec2 uv = inTexCoord + vec2(float(x), float(y)) * texel.xy;
			vec3 c = toYCoCg(textureLod(sceneMap, uv, 0.0).rgb);
			if (x == 0 && y == 0) {
				current = c;
			}
			sum += c;
			sum2 += c * c;
			low = min(low, c);
			high = max(high, c);
			float z = textureLod(normalDepth, uv, 0.0).w;
			if (z < nearest) {
				nearest = z;
				nearestUV = uv;
			}
		}
	}
	// The weapon is not in the depth: a world pixel beside it keeps its own motion.
	const vec2 own = textureLod(motionMap, inTexCoord, 0.0).xy;
	const bool firstPerson = own.x > kFirstPerson;
	vec2 motion = textureLod(motionMap, nearestUV, 0.0).xy;
	if (firstPerson) {
		motion = vec2(0.0);
	} else if (motion.x > kFirstPerson) {
		motion = own;
	}
	vec2 previousUV = inTexCoord - motion;
	const float alpha = firstPerson ? 0.0 : 1.0;
	if (params.x < 0.5 || any(lessThan(previousUV, vec2(0.0))) || any(greaterThan(previousUV, vec2(1.0))) ||
		(textureLod(historyMap, previousUV, 0.0).a < 0.5) != firstPerson) {
		outColor = vec4(toRgb(current), alpha);
		return;
	}
	vec3 mean = sum / 9.0;
	vec3 deviation = sqrt(max(sum2 / 9.0 - mean * mean, vec3(0.0)));
	vec3 history = toYCoCg(sampleHistory(previousUV));
	history = clamp(history, max(low, mean - 1.25 * deviation), min(high, mean + 1.25 * deviation));
	outColor = vec4(toRgb(mix(current, history, params.y)), alpha);
}
