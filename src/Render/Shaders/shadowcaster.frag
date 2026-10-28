#version 450
// The shadow maps' casters (mesh.vert with the light's view-projection):
// depth only; alpha-tested materials cut out by their texture when
// shadowManager.allowAlphaShadows lets them cast (SHADOWALPHATHRESHOLD).
layout(location = 1) in vec2 inTexCoord;

layout(set = 2, binding = 0) uniform sampler2D diffuseMap;

layout(set = 3, binding = 0) uniform Caster {
	vec4 caster; // x: the alpha reference (0: none)
};

void main()
{
	if (caster.x > 0.0 && texture(diffuseMap, inTexCoord).a < caster.x) {
		discard;
	}
}
