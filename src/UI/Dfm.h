// The Flash menus' ".dfm" files (Menu_server.zip: External/FlashMenu/
// mainMenu.dfm and endOfRound.dfm, which BF2142.exe asks SwiffPlayer.dll
// for at 0x00480000's calls).
//
// A .dfm is an ordinary SWF movie XORed with a four-byte key. SwiffPlayer
// (DICE's build of gameswf) opens every movie at 0x100080a0: when the name
// ends in ".dfm" it wraps the file in a tu_file (0x100f1020) whose read
// callback is 0x10007e80 instead of the plain 0x10007e60. That callback asks
// the file for its position, reads, and XORs each byte with
// key[position % 4], the key being the bytes 0b ad f0 0d at 0x101019f4
// (0x0df0ad0b little-endian). Nothing else changes: the result goes to
// gameswf's usual "FWS"/"CWS" parser. The shipped menus decode to
// uncompressed version-7 "FWS" movies.
//
// The XOR is its own inverse, so encodeDfm and decodeDfm differ only in
// which side they check for a SWF signature.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::ui {

// 0x101019f4: applied by file position, byte i with kDfmKey[i % 4].
constexpr std::array<uint8_t, 4> kDfmKey {0x0b, 0xad, 0xf0, 0x0d};

// XORs `data` in place as if it started at file offset `position`.
void xorDfm(uint8_t* data, size_t size, uint64_t position = 0);

// Decodes a .dfm into an uncompressed ("FWS") SWF. A "CWS" movie inside is
// inflated. Returns nothing (and says why in `error`) when the result is
// not a SWF.
std::optional<std::vector<uint8_t>> decodeDfm(const std::vector<uint8_t>& dfm, std::string* error = nullptr);

// The reverse, for tools and tests: XORs a SWF (FWS or CWS) into a .dfm.
std::optional<std::vector<uint8_t>> encodeDfm(const std::vector<uint8_t>& swf, std::string* error = nullptr);

// Turns a "CWS" movie into the same "FWS" movie (a copy for "FWS").
std::optional<std::vector<uint8_t>> uncompressSwf(const std::vector<uint8_t>& swf, std::string* error = nullptr);

} // namespace dice::hfe::ui
