// Targa (.tga) images, the format of most HUD and menu textures: true-colour
// (type 2) and run-length encoded true-colour (type 10) at 24 or 32 bits,
// and greyscale (types 3 and 11) at 8 bits. Loaded as one RGBA8 level.
#pragma once

#include "DdsFile.h"

namespace dice::hfe::texture {

std::optional<DdsTexture> parseTga(const std::vector<uint8_t>& data, std::string* error = nullptr);

} // namespace dice::hfe::texture
