#pragma once

namespace dice::hfe::render {

// Meme's blend-function enum is not D3DBLEND. In particular its default
// Bf2BlendFuncEffect is 7/8 (BF2142.exe 0x007e4360): source alpha and its
// inverse. Vehicle HUDs use 7/2 to add only the texture's visible strokes.
constexpr int hudBlendToD3D(int factor)
{
	switch (factor) {
	case 5: return 9;  // destination colour
	case 6: return 10; // inverse destination colour
	case 7: return 5;  // source alpha
	case 8: return 6;  // inverse source alpha
	case 9: return 7;  // destination alpha
	case 10: return 8; // inverse destination alpha
	default: return factor;
	}
}

// Remaster's widescreen HUD fix (Shaders_client_hudfix.zip's PortedMenuShader.fx
// vsFFP) leaves a HUD vertex of alpha 0.01 unscaled and draws it opaque: the
// mod's marker for its own full-width pieces. The shipped PortedMenuShader.fx
// (Shaders_client.zip, vsFFP) passes the vertex colour through untouched, so
// without the hudfix a 0.01 alpha is an ordinary, nearly clear vertex (a fade
// passing through). ui.vert tests the same: the marker only with the hudfix's
// shader (Screen.size.w, LevelSources::hudfixMarkers), and within a tolerance
// of 0.01 rather than float equality (0.001 keeps 2/255 and 3/255 out).
constexpr float kHudfixMarkerAlpha = 0.01f;
constexpr float kHudfixMarkerTolerance = 0.001f;

constexpr bool hudfixMarker(float alpha, bool hudfixShader)
{
	const float off = alpha - kHudfixMarkerAlpha;
	return hudfixShader && off < kHudfixMarkerTolerance && -off < kHudfixMarkerTolerance;
}

} // namespace dice::hfe::render
