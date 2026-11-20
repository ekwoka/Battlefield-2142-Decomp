#version 450

// Seed the static cascade atlas with cached mesh depth before drawing terrain.
layout(set = 2, binding = 0) uniform sampler2D meshDepth;

void main()
{
	gl_FragDepth = texelFetch(meshDepth, ivec2(gl_FragCoord.xy), 0).r;
}
