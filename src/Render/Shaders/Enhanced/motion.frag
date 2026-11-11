#version 450
// The moving meshes' motion (motion.vert): this frame's uv less last
// frame's, alpha-tested as shadowcaster.frag. The first person writes a
// flag instead (taa.frag): it rides the view, and the world behind it must
// not take its history from where it was.
layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inCurrent;
layout(location = 2) in vec4 inPrevious;

layout(set = 2, binding = 0) uniform sampler2D diffuseMap;

layout(set = 3, binding = 0) uniform Caster {
	vec4 caster; // x: the alpha reference (0: none), y: 1 for the first person
};

layout(location = 0) out vec2 outMotion;

void main()
{
	if (caster.x > 0.0 && texture(diffuseMap, inTexCoord).a < caster.x) {
		discard;
	}
	if (caster.y > 0.5) {
		outMotion = vec2(8.0); // taa.frag's kFirstPerson
		return;
	}
	outMotion = (inCurrent.xy / inCurrent.w - inPrevious.xy / inPrevious.w) * vec2(0.5, -0.5);
}
