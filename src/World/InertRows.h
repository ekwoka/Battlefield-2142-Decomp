// Console rows the shipped scripts call that do nothing a player can see
// in this rewrite, bound as documented no-ops, and the one such row whose
// value is kept for the renderer (texturemanager.customTextureSuffix).
#pragma once

#include <string>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::world {

// Registers the inert rows (see InertRows.cpp for why each is inert). They
// are registered as stand-ins (ConsoleMethod::stub), so a real binding
// registered later under the same name takes their place, and with the
// original's argument counts and property kinds (symbols/*.console.csv).
void registerInertConsoleRows(Console& console);

// texturemanager.customTextureSuffix (RendDX9 row 0x10053c80, executor
// 0x10053d80: the texture manager's +0xf8 and its reload flag +0x114).
// Once it is set, the manager's reload pass (0x10018ca0) tries every loaded
// texture again as "<name>_<suffix>.dds" (0x10017f20) and keeps that
// variant where it exists: the snow kits and vehicles of the Northern
// Strike winter maps ("Snow": Objects/Kits_bp1 eu_kits_b_snow.dds,
// as_apc_b_snow.dds...), Wake_Island_2142's billboards ("wake":
// billboard_color01_wake.dds). Empty when the level sets none.
// The renderer's texture lookup is to use it (not yet done there).
const std::string& customTextureSuffix();
void setCustomTextureSuffix(std::string suffix);

} // namespace dice::hfe::world
