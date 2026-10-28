#version 450
// Cloaked soldiers (RaShaderSMActiveCamo.fx / RaShaderBMActiveCammo.fx
// technique Camouflage): they draw nothing but a mask (the original writes
// alpha only), which the post pass turns into the refraction of what is
// behind them.
layout(location = 0) out vec4 outColor;

void main()
{
	outColor = vec4(1.0);
}
