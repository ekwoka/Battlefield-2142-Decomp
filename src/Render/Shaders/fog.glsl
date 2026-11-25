// RaCommon.fx calcFog, included after a Lighting block that declares
// fogColor and fogRange. The renderer fills FogRange as 0x100355c0 does from
// the fog's start s, end e and base (renderer.fogStartEndAndBase, the end
// and the fourth value scaled by the view distance, Fog.h): (1 / (e - s),
// -base / (e - s), -s / (e - s), e / (e - s)); FogColor.w is the fourth
// value. `w` is the clip-space w (the view depth in metres), as the
// shaders' calcFog(Out.Pos.w). Returns the fog's share (1 - calcFog;
// fog_share.glsl, which the tests evaluate).
#include "fog_share.glsl"

float fogAmount(float w)
{
	return fogShare(w, fogRange, fogColor.w);
}
