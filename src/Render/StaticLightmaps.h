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

// Whether a static mesh looks for its lightmap in the level's object atlas
// (0x10153200, "Lightmaps/Objects/<mesh>=<lod>=x=y=z"). A Titan's hull loads
// only its Titan maps (0x100bde63 / 0x100bf872: drawable +0x362, template
// slot 0xcc, picks 0x10152870 over the atlas) and its rooms are drawn only by
// the interior path, which also loads only those (0x100bd270). Highway Tampa's
// atlas has stale cells at the PAC Titan's start for its front hull and rooms.
// (Not ported: with showAllTitanParts a room drawn from outside reaches the atlas.)
inline bool staticUsesLevelLightmap(bool titanPart)
{
	return !titanPart;
}

} // namespace dice::hfe::render
