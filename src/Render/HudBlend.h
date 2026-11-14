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

} // namespace dice::hfe::render
