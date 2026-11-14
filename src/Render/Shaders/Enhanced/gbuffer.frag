#version 450
// The enhanced prepass's G-buffer (not retail), from its depth: the
// view-space normal (xyz, from the neighbouring depths: the nearer of
// each pair, so edges keep the surface's) and the linear view depth (w);
// and the camera's motion of what holds still: this frame's uv less last
// frame's, both without the jitter (moving meshes overwrite theirs,
// motion.frag).
layout(location = 0) in vec2 inTexCoord;

layout(set = 2, binding = 0) uniform sampler2D depthMap;

layout(set = 3, binding = 0) uniform GBuffer {
	mat4 inverseProjection;      // the jittered clip space -> view
	mat4 inverseView;            // view -> world
	mat4 viewProjection;         // world -> clip, this frame's, unjittered
	mat4 previousViewProjection; // last frame's, unjittered
	vec4 texel;                  // xy: 1 / the size
};

layout(location = 0) out vec4 outNormalDepth;
layout(location = 1) out vec2 outMotion;

// In view space: unprojected about the camera, the depth's rounding stays a
// fraction of a millimetre at the Titans' range (through the world's
// inverse it grew into noise in the normals, the GTAO and the motion).
vec3 viewAt(vec2 uv)
{
	float d = textureLod(depthMap, uv, 0.0).r;
	vec4 p = inverseProjection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
	return p.xyz / p.w;
}

void main()
{
	vec3 v = viewAt(inTexCoord);
	vec3 right = viewAt(inTexCoord + vec2(texel.x, 0.0)) - v;
	vec3 left = v - viewAt(inTexCoord - vec2(texel.x, 0.0));
	vec3 down = viewAt(inTexCoord + vec2(0.0, texel.y)) - v;
	vec3 up = v - viewAt(inTexCoord - vec2(0.0, texel.y));
	vec3 dx = dot(right, right) < dot(left, left) ? right : left;
	vec3 dy = dot(down, down) < dot(up, up) ? down : up;
	vec4 viewPosition = vec4(v, 1.0);
	vec3 p = (inverseView * viewPosition).xyz;
	// Pixel-to-pixel distances shrink with resolution and proximity. Testing
	// the unscaled cross product treats valid nearby surfaces as degenerate
	// at high resolutions, producing a false AO band. Test its angle instead.
	float dx2 = dot(dx, dx), dy2 = dot(dy, dy);
	vec3 n = dx2 > 1e-18 && dy2 > 1e-18
		? cross(dx * inversesqrt(dx2), dy * inversesqrt(dy2)) : vec3(0.0);
	n = dot(n, n) > 1e-12 ? normalize(n) : vec3(0.0, 0.0, -1.0);
	if (dot(n, viewPosition.xyz) > 0.0) {
		n = -n; // towards the camera
	}
	outNormalDepth = vec4(n, viewPosition.z);
	vec4 now = viewProjection * vec4(p, 1.0);
	vec4 before = previousViewProjection * vec4(p, 1.0);
	outMotion = (now.xy / now.w - before.xy / before.w) * vec2(0.5, -0.5);
}
