#version 450
// The post-production in one pass (PostProduction_nv3x.fx; the passes and
// their order are in PostProduction.h). The original runs each as a pass
// over the backbuffer; the ones that move pixels (EMP's roll, the camera
// effect's fisheye, the TV's distortion) read the earlier ones at the moved
// coordinates here instead.
layout(location = 0) in vec2 inTexCoord;

layout(set = 2, binding = 0) uniform sampler2D scene;
layout(set = 2, binding = 1) uniform sampler2D bloomTexture;
layout(set = 2, binding = 2) uniform sampler2D noiseTexture;    // common/textures/noise
layout(set = 2, binding = 3) uniform sampler2D randomTexture;   // common/textures/random
layout(set = 2, binding = 4) uniform sampler2D frameTexture;    // common/textures/frame
layout(set = 2, binding = 5) uniform sampler2D scanlineTexture; // common/textures/scanline
layout(set = 2, binding = 6) uniform sampler2D camoMask;
layout(set = 2, binding = 7) uniform sampler2D history0;
layout(set = 2, binding = 8) uniform sampler2D history1;
layout(set = 2, binding = 9) uniform sampler2D history2;
layout(set = 2, binding = 10) uniform sampler2D history3;

layout(set = 3, binding = 0) uniform Post {
	vec4 colorRow[3];    // the grade (COLORTRANSFORMMATRIX)
	vec4 contrast;       // CONTRASTPOLYNOM; w: the mode bits
	vec4 bloom;          // x: GLOWSTRENGTH (bloomOverlayAmount), y: 1 to show the bloom alone
	vec4 emp;            // INTERFERENCE, DISTORTIONROLL, DISTORTIONSCALE, DISTORTIONFREQ
	vec4 empColorRow[3]; // the EMP's (or the view's) grade
	vec4 timeValues;     // FRACTIME, FRACTIME256, FRACSINE, the camera effect's scanline time
	vec4 tinnitus;       // xy: SAMPLEOFFSET, z: BACKBUFFERLERPBIAS
	vec4 camo;           // xy: CAMOUFLAGEOFFSET
	vec4 tv;             // INTERFERENCE, DISTORTIONROLL, DISTORTIONFREQ, DISTORTIONSCALE
	vec4 tvTime;         // FRACTIME, FRACTIME256, FRACSINE
};

layout(location = 0) out vec4 outColor;

const vec3 kLuminance = vec3(0.3086, 0.6094, 0.0820);

bool hasMode(int bit)
{
	return (int(contrast.w + 0.5) & bit) != 0;
}

vec3 transform(vec4 rows[3], vec3 c)
{
	return vec3(dot(rows[0].xyz, c), dot(rows[1].xyz, c), dot(rows[2].xyz, c));
}

vec3 polynomial(vec3 c)
{
	return contrast.x * c + contrast.y * c * c + contrast.z;
}

// The frame with the cloaked soldiers' refraction (FSBMExtractCamouflage,
// the 5x5 blur, FSBMCamouflageBlurPassThrough: what is behind them, moved
// by CAMOUFLAGEOFFSET, a little contrast added, blended by the mask).
vec3 sceneAt(vec2 uv)
{
	vec3 color = texture(scene, uv).rgb;
	if (hasMode(64)) {
		vec2 t = 1.5 / vec2(textureSize(camoMask, 0));
		float m = 0.25 * (texture(camoMask, uv + vec2(-t.x, -t.y)).r + texture(camoMask, uv + vec2(t.x, -t.y)).r +
							 texture(camoMask, uv + vec2(-t.x, t.y)).r + texture(camoMask, uv + vec2(t.x, t.y)).r);
		float alpha = clamp((max(0.4, m) - 0.5) * 2.0, 0.0, 1.0);
		if (alpha > 0.0) {
			vec2 at = uv + camo.xy;
			vec3 behind = 0.25 * (texture(scene, at + vec2(-t.x, 0)).rgb + texture(scene, at + vec2(t.x, 0)).rgb +
									 texture(scene, at + vec2(0, -t.y)).rgb + texture(scene, at + vec2(0, t.y)).rgb);
			behind += (vec3(dot(behind, kLuminance)) - 0.5) * 0.1;
			color = mix(color, behind, alpha);
		}
	}
	return color;
}

// Soften (the bloom added) or the tinnitus's frames.
vec3 baseAt(vec2 uv)
{
	vec3 color = sceneAt(uv);
	if (hasMode(32)) {
		vec2 at = uv - tinnitus.xy;
		vec3 accum = texture(history0, at).rgb * 0.5 + texture(history1, at).rgb * 0.25 +
			texture(history2, at).rgb * 0.125 + texture(history3, at).rgb * 0.0675;
		return mix(accum, color, tinnitus.z);
	}
	if (hasMode(1)) {
		vec3 b = texture(bloomTexture, uv).rgb;
		return bloom.y > 0.5 ? b : clamp(color + b * bloom.x, 0.0, 1.0);
	}
	return color;
}

// ColorTransform, or CameraEffect's fisheye and scanline with the same grade.
vec3 gradedAt(vec2 uv)
{
	vec3 color;
	if (hasMode(4)) {
		float lerpFactor = texture(frameTexture, uv).r * 0.5;
		vec2 d = uv - 0.5;
		vec2 fisheye = uv - d * d * d * lerpFactor;
		// sampler2wrap: the scanline point sampled, wrapped.
		ivec2 lines = textureSize(scanlineTexture, 0);
		vec2 at = fract(vec2(uv.x, uv.y - timeValues.w)) * vec2(lines);
		float scanline = texelFetch(scanlineTexture, min(ivec2(at), lines - 1), 0).r;
		// sampler0bilinwrap: the scanline's push past the right edge wraps.
		color = polynomial(transform(colorRow, baseAt(fract(vec2(fisheye.x + scanline * 0.03, fisheye.y)))));
	} else if (hasMode(2)) {
		color = polynomial(transform(colorRow, baseAt(uv)));
	} else {
		color = baseAt(uv);
	}
	if (hasMode(128)) {
		color = polynomial(transform(empColorRow, color));
	}
	return color;
}

void main()
{
	vec2 uv = inTexCoord;
	vec3 color;
	if (hasMode(8)) {
		// psDx9_EMP20: the noise row moves the image across; the static
		// takes from it (and adds a little red); the contrast again.
		float offset = texture(randomTexture, vec2(0.0, fract(uv.y * emp.x))).r - 0.5;
		vec2 img = vec2(fract(uv.x + offset * emp.y), uv.y);
		float noise = texture(randomTexture, fract(uv * emp.z * timeValues.y)).r - 0.5;
		float n = max(0.0, noise * 2.0);
		vec3 accum = transform(empColorRow, gradedAt(img)) - n;
		accum.r += n * 0.3;
		color = polynomial(accum);
	} else {
		color = gradedAt(uv);
	}
	if (hasMode(16)) {
		// ps_TVEffect20 over the clip-space position.
		vec2 pos = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
		float rand = texture(randomTexture, fract(1.5 * pos + tvTime.y)).r - 0.2;
		float noisy = texture(noiseTexture, fract(0.5 * vec2(0.5 * pos.y, 0.1 * tvTime.x))).r - 0.5;
		float dst = fract(pos.y * tv.z + tv.y * tvTime.z);
		dst *= 1.0 - dst;
		dst /= 1.0 + tv.w * abs(pos.y);
		vec2 img = vec2(uv.x + tv.w * noisy * dst, uv.y);
		float image = dot(vec3(0.3, 0.59, 0.11), gradedAt(img));
		color = vec3(tv.x * rand + image * 0.75 + 0.25);
	}
	outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
