// RaCommon.fx calcFog as the fog's share: GLSL that also compiles as C++
// (Render/ShaderMath.h), so the tests evaluate the shaders' own code. Keep
// it to what both languages read alike: .x/.y/.z/.w, float literals with f,
// and the helpers ShaderMath.h mirrors.
#ifndef SHADER_INLINE
#define SHADER_INLINE
#endif

// `range` is FogRange (Render/Fog.h fogRange): (1 / (e - s), -base / (e - s),
// -s / (e - s), e / (e - s)); `closeFloor` is FogColor.w, the fog's fourth
// value. calcFog(w) = max(w FogRange.y + FogRange.w, FogColor.w)
// - (w FogRange.x + FogRange.z)^3 is how much of the surface shows (D3D9's
// fog factor, saturated); this returns the rest, the fog's share.
SHADER_INLINE float fogShare(float w, vec4 range, float closeFloor)
{
	float close = max(w * range.y + range.w, closeFloor);
	float beyond = max(w * range.x + range.z, 0.0f);
	return 1.0f - clamp(close - beyond * beyond * beyond, 0.0f, 1.0f);
}
