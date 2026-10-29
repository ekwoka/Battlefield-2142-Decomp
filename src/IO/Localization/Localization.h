// Localized strings (0x00405360): Localization/strings.csv in the
// profile's language (ExcelLexicon, load 0x006957c0, lookup 0x00695c70).
// The game reads no other file (strings_patch.csv holds the patch
// installer's strings, each also in strings.csv with the same text). Each file is UTF-16LE text, one row per line:
// tab-separated, the string id first, then a column per language named in
// the header row (English, Chinese_Simplified, ... Russian). Values may be
// wrapped in double quotes. The game picks the profile's GSLanguage and
// falls back to English.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dice::hfe::io {

class Localization {
public:
	// Adds a file's rows for `language` (UTF-16LE bytes); an id already
	// loaded keeps its text (0x006956f0). Ids ignore case (0x0065b2c0).
	// False when the file has no such column.
	bool load(const std::vector<uint8_t>& data, const std::string& language = "English");
	// The text for an id (UTF-8), or null.
	const std::string* find(const std::string& id) const;
	// The text for an id, or the id itself.
	std::string text(const std::string& id) const;
	size_t size() const { return m_strings.size(); }

private:
	std::unordered_map<std::string, std::string> m_strings;
};

// UTF-16LE (with or without a byte-order mark) to UTF-8.
std::string utf16ToUtf8(const uint8_t* data, size_t size);

} // namespace dice::hfe::io
