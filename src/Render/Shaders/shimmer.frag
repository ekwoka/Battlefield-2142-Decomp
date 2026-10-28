#version 450
// The heat shimmer (Particles.fx / NonScreenAlignedParticles.fx /
// Trail.fx technique ParticleHeatShimmer, psParticleHeatShimmer): the
// scene behind the particle, its coordinates displaced by
// ShimmerIntensity texels in waves of ShimmerPhases across the screen that
// roll with FracTime (frac(t x ShimmerSpeed), RendDX9 0x10132de0) plus the
// particle's own offset; blended by the texture's alpha times the
// particle's (SrcAlpha / InvSrcAlpha, alpha-tested at 0).
layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inDepth;

layout(set = 2, binding = 0) uniform sampler2D particleTexture;
layout(set = 2, binding = 1) uniform sampler2D sceneTexture;

layout(set = 3, binding = 0) uniform Shimmer {
	vec4 screen; // xy: the texel size, z: FracTime, w: ShimmerIntensity
	vec4 waves;  // x: ShimmerPhases, y: the particle's timing offset
};

layout(location = 0) out vec4 outColor;

void main()
{
	vec2 uv = gl_FragCoord.xy * screen.xy;
	const float pi = 3.14159265;
	float angle = (screen.z + waves.y) * pi * 2.0;
	float toAngle = pi * 2.0 * waves.x;
	vec2 moved = uv + vec2(cos(uv.y * toAngle + angle) * screen.x * screen.w,
		sin(uv.x * toAngle + angle) * screen.y * screen.w);
	vec4 scene = texture(sceneTexture, moved);
	float alpha = texture(particleTexture, inTexCoord).a * inColor.a;
	if (alpha <= 0.0) {
		discard;
	}
	outColor = vec4(scene.rgb, alpha);
}
