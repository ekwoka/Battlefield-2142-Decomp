#version 450
// The HUD and menus: the texture times the colour, plus the colour to add.
layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec4 inAdd;

layout(set = 2, binding = 0) uniform sampler2D hudTexture;

layout(location = 0) out vec4 outColor;

void main()
{
	outColor = clamp(texture(hudTexture, inTexCoord) * inColor + inAdd, 0.0, 1.0);
}
