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
	mat4 inverseViewProjection;  // the jittered clip space -> world
	mat4 view;                   // world -> view
	mat4 viewProjection;         // world -> clip, this frame's, unjittered
	mat4 previousViewProjection; // last frame's, unjittered
	vec4 texel;                  // xy: 1 / the size
};

layout(location = 0) out vec4 outNormalDepth;
layout(location = 1) out vec2 outMotion;

vec3 worldAt(vec2 uv)
{
	float d = textureLod(depthMap, uv, 0.0).r;
	vec4 p = inverseViewProjection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
	return p.xyz / p.w;
}

void main()
{
	vec3 p = worldAt(inTexCoord);
	vec3 right = worldAt(inTexCoord + vec2(texel.x, 0.0)) - p;
	vec3 left = p - worldAt(inTexCoord - vec2(texel.x, 0.0));
	vec3 down = worldAt(inTexCoord + vec2(0.0, texel.y)) - p;
	vec3 up = p - worldAt(inTexCoord - vec2(0.0, texel.y));
	vec3 dx = dot(right, right) < dot(left, left) ? right : left;
	vec3 dy = dot(down, down) < dot(up, up) ? down : up;
	vec4 viewPosition = view * vec4(p, 1.0);
	vec3 n = mat3(view) * cross(dx, dy);
	n = dot(n, n) > 1e-12 ? normalize(n) : vec3(0.0, 0.0, -1.0);
	if (dot(n, viewPosition.xyz) > 0.0) {
		n = -n; // towards the camera
	}
	outNormalDepth = vec4(n, viewPosition.z);
	vec4 now = viewProjection * vec4(p, 1.0);
	vec4 before = previousViewProjection * vec4(p, 1.0);
	outMotion = (now.xy / now.w - before.xy / before.w) * vec2(0.5, -0.5);
}
