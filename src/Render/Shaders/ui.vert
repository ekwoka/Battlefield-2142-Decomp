#version 450
// The HUD (quads in its 800 x 600 screen, rotation already applied) and
// the Flash menus (triangles in their stage), stretched to the window. The
// lightmap and part slots carry a colour to add (Flash colour transforms;
// zero for the HUD).
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec2 inAddRG;
layout(location = 4) in float inAddB;
layout(location = 5) in float inAddA;
layout(location = 6) in float inAlpha;
layout(location = 7) in vec2 inUnusedDetailCoord;
layout(location = 8) in vec2 inUnusedDirtCoord;
layout(location = 9) in vec2 inUnusedCrackCoord;

layout(set = 1, binding = 0) uniform Screen {
	vec4 size; // the screen: 800, 600 for the HUD, the stage for menus
};

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec4 outAdd;

void main()
{
	gl_Position = vec4(inPosition.x / size.x * 2.0 - 1.0, 1.0 - inPosition.y / size.y * 2.0, 0.0, 1.0);
	outTexCoord = inTexCoord;
	outColor = vec4(inColor, inAlpha);
	outAdd = vec4(inAddRG, inAddB, inAddA);
}
