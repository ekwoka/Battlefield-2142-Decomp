// RaCommon.fx calcFog, included after a Lighting block that declares
// fogColor and fogRange. The renderer fills FogRange as 0x100355c0 does from
// the fog's start s, end e and base (renderer.fogStartEndAndBase, the end
// and the fourth value scaled by the view distance, Fog.h): (1 / (e - s),
// -base / (e - s), -s / (e - s), e / (e - s)); FogColor.w is the fourth
// value. `w` is the clip-space w (the view depth in metres), as the
// shaders' calcFog(Out.Pos.w). Returns the fog's share (1 - calcFog).
float fogAmount(float w)
{
	vec2 fogVals = w * fogRange.xy + fogRange.zw;
	float close = max(fogVals.y, fogColor.w);
	float far = pow(max(fogVals.x, 0.0), 3.0);
	return 1.0 - clamp(close - far, 0.0, 1.0);
}
