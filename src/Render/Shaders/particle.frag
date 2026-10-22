#version 450
// Particles: the texture times the particle's colour; fog fades them out.
layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inDepth;

layout(set = 2, binding = 0) uniform sampler2D particleTexture;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 sunDirection;
	vec4 sunColor;
	vec4 skyColor;
	vec4 fogColor; // w: fog end
	vec4 material;
};

layout(location = 0) out vec4 outColor;

void main()
{
	vec4 texel = texture(particleTexture, inTexCoord);
	float fog = clamp(inDepth / fogColor.w, 0.0, 1.0);
	outColor = vec4(texel.rgb * inColor.rgb, texel.a * inColor.a * (1.0 - fog * fog));
}
