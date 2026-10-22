// Writes RGBA8 pixels as a PNG file (for screenshots and tests).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dice::hfe::render {

bool writePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);

} // namespace dice::hfe::render
