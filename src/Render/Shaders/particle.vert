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
// The tangent slot: rgb the unlit tint (Enhanced), w the next flipbook frame
// (column + 256 x row); its blend sits in inUnusedLightmap.y.
layout(location = 10) in vec4 inParticleTint;

layout(set = 1, binding = 0) uniform Particles {
	mat4 viewProjection;
	vec4 cameraRight;
	vec4 cameraUp;
};

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColor;
layout(location = 2) out float outDepth;
layout(location = 5) out vec3 outNextFrame; // the next frame's coordinates, and its blend

void main()
{
	vec3 position;
	if (inSize < 0.0) {
		// Placed on the CPU (non-screen-aligned quads, trails): the
		// coordinates come in the lightmap slot.
		position = inCenter;
		outTexCoord = inUnusedLightmap;
		outNextFrame = vec3(outTexCoord, 0.0);
	} else {
		// vsParticle: the quad stays screen aligned, its corners (+-1 less the
		// pivot) x size (RendDX9 0x101301dc: displaceCoords); the rotation
		// turns the texture coordinates (+-1, 0x1013021e) inside it, scaled by
		// uvScale 1/sqrt(2), so the quad shows the frame's middle and the
		// picture is sqrt(2) the quad's size.
		vec2 shifted = inCorner + inPivot;
		position = inCenter + (cameraRight.xyz * shifted.x + cameraUp.xyz * shifted.y) * inSize;
		float c = cos(inRotation), s = sin(inRotation);
		vec2 turned = vec2(inCorner.x * c + inCorner.y * s, -inCorner.x * s + inCorner.y * c) * 0.70710678;
		vec2 frameSize = inFrameSize.x > 0.0 ? inFrameSize : vec2(1.0);
		vec2 inFrame = (turned * vec2(0.5, -0.5) + 0.5) * frameSize;
		outTexCoord = inFrameCorner + inFrame;
		// Particles.fx texCoords1: the same place in the next frame (uvOffsets.zw).
		float row = floor(inParticleTint.w / 256.0);
		outNextFrame = vec3(vec2(inParticleTint.w - row * 256.0, row) * frameSize + inFrame, inUnusedLightmap.y);
	}
	gl_Position = viewProjection * vec4(position, 1.0);
	outColor = vec4(inColor, inAlpha);
	outDepth = gl_Position.w;
}
