// Which static meshes keep their lightmaps (RendDX9's static mesh template).
#pragma once

namespace dice::hfe::render {

// The static mesh template's "use lightmaps" (0x100df010), once its geometry
// is loaded (+0x189): a Titan's exterior or interior part (+0xc8 / +0xc9) or
// a mesh under objects/water/ (+0x2a8) always; any other only from the
// medium lighting quality up (the settings' slot 2, +0x38, at least 2).
inline bool staticKeepsLightmap(bool titanPart, bool waterObject, int lightingQuality)
{
	return titanPart || waterObject || lightingQuality >= 2;
}

} // namespace dice::hfe::render
