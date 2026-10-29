#version 450
// Particles (Particles.fx): camera-facing quads; or, with a negative size,
// triangles placed on the CPU (NonScreenAlignedParticles.fx, Trail.fx). Each vertex carries the
// particle's centre, a corner (-1..1), its size and rotation, and its
// colour and alpha (computed on the CPU from the template's graphs).
layout(location = 0) in vec3 inCenter;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inCorner;
layout(location = 3) in vec2 inUnusedLightmap;
layout(location = 4) in float inSize;
layout(location = 5) in float inRotation;
layout(location = 6) in float inAlpha;
layout(location = 7) in vec2 inPivot; // pivotOffset: the centre moved by half-sizes
layout(location = 8) in vec2 inFrameCorner; // the flipbook frame's u0 v0 (Effects' spriteFrame)
layout(location = 9) in vec2 inFrameSize;   // and its size (0: the whole texture)

layout(set = 1, binding = 0) uniform Particles {
	mat4 viewProjection;
	vec4 cameraRight;
	vec4 cameraUp;
};

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColor;
layout(location = 2) out float outDepth;

void main()
{
	vec3 position;
	if (inSize < 0.0) {
		// Placed on the CPU (non-screen-aligned quads, trails): the
		// coordinates come in the lightmap slot.
		position = inCenter;
		outTexCoord = inUnusedLightmap;
	} else {
		float c = cos(inRotation), s = sin(inRotation);
		vec2 shifted = inCorner + inPivot;
		vec2 corner = vec2(shifted.x * c - shifted.y * s, shifted.x * s + shifted.y * c);
		position = inCenter + (cameraRight.xyz * corner.x + cameraUp.xyz * corner.y) * inSize;
		vec2 frameSize = inFrameSize.x > 0.0 ? inFrameSize : vec2(1.0);
		outTexCoord = inFrameCorner + (inCorner * vec2(0.5, -0.5) + 0.5) * frameSize;
	}
	gl_Position = viewProjection * vec4(position, 1.0);
	outColor = vec4(inColor, inAlpha);
	outDepth = gl_Position.w;
}
