#version 450
// A triangle over the whole target for the post-production passes
// (PostProduction_nv3x.fx / FSQuadDrawer.fx draw a quad): uv 0..1 from the
// top left.
layout(location = 0) out vec2 outTexCoord;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
	outTexCoord = vec2(p.x, 1.0 - p.y);
}
