// Read-only ZIP archives (the engine's "dice.hfe.io.ArchiveHandler.ZIP").
//
// Game data ships in plain ZIP files (stored or deflated entries). Entry
// names are matched case-insensitively with '/' as separator, like the rest
// of the virtual file system.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dice::hfe::io {

class ZipArchive {
public:
	// Returns null if the file is missing or not a ZIP.
	static std::unique_ptr<ZipArchive> open(const std::string& path);

	bool contains(const std::string& name) const;
	// Reads and inflates an entry. Returns false if missing or corrupt.
	bool read(const std::string& name, std::vector<uint8_t>& out) const;
	uint32_t size(const std::string& name) const;

	// Entry names (normalised) whose directory part equals `directory`.
	std::vector<std::string> list(const std::string& directory) const;
	// Entry names (normalised) below `directory` ("" for all).
	std::vector<std::string> listRecursive(const std::string& directory) const;

	const std::string& path() const { return m_path; }
	size_t entryCount() const { return m_entries.size(); }

private:
	struct Entry {
		uint32_t localHeaderOffset;
		uint32_t compressedSize;
		uint32_t uncompressedSize;
		uint16_t method; // 0 stored, 8 deflate
		std::string name;
	};

	std::string m_path;
	std::unordered_map<std::string, Entry> m_entries; // keyed by normalised name
};

// Lower-case and use '/' separators; collapses duplicate slashes.
std::string normalizePath(std::string path);

} // namespace dice::hfe::io
