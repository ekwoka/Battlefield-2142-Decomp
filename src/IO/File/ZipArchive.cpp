#include "ZipArchive.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace dice::hfe::io {

namespace {

constexpr uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr uint32_t kCentralDirectoryEntry = 0x02014b50;
constexpr uint32_t kLocalFileHeader = 0x04034b50;

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

class File {
public:
	explicit File(const std::string& path) : m_file(std::fopen(path.c_str(), "rb")) {}
	~File()
	{
		if (m_file) {
			std::fclose(m_file);
		}
	}
	explicit operator bool() const { return m_file != nullptr; }
	bool readAt(long offset, void* buffer, size_t bytes) const
	{
		return std::fseek(m_file, offset, SEEK_SET) == 0 && std::fread(buffer, 1, bytes, m_file) == bytes;
	}
	long size() const
	{
		std::fseek(m_file, 0, SEEK_END);
		return std::ftell(m_file);
	}

private:
	FILE* m_file;
};

std::string directoryOf(const std::string& name)
{
	const size_t slash = name.rfind('/');
	return slash == std::string::npos ? std::string() : name.substr(0, slash);
}

} // namespace

std::string normalizePath(std::string path)
{
	std::string out;
	out.reserve(path.size());
	for (char c : path) {
		c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (c == '/' && !out.empty() && out.back() == '/') {
			continue;
		}
		out.push_back(c);
	}
	return out;
}

std::unique_ptr<ZipArchive> ZipArchive::open(const std::string& path)
{
	File file(path);
	if (!file) {
		return nullptr;
	}
	// The end-of-central-directory record sits in the last 64 KiB + 22 bytes.
	const long size = file.size();
	const long tail = std::min<long>(size, 0xffff + 22);
	std::vector<uint8_t> buffer(static_cast<size_t>(tail));
	if (!file.readAt(size - tail, buffer.data(), buffer.size())) {
		return nullptr;
	}
	long eocd = -1;
	for (long i = tail - 22; i >= 0; --i) {
		if (u32(&buffer[static_cast<size_t>(i)]) == kEndOfCentralDirectory) {
			eocd = i;
			break;
		}
	}
	if (eocd < 0) {
		return nullptr;
	}
	const uint8_t* end = &buffer[static_cast<size_t>(eocd)];
	const uint16_t entryCount = u16(end + 10);
	const uint32_t directorySize = u32(end + 12);
	const uint32_t directoryOffset = u32(end + 16);

	std::vector<uint8_t> directory(directorySize);
	if (!file.readAt(static_cast<long>(directoryOffset), directory.data(), directory.size())) {
		return nullptr;
	}

	auto archive = std::unique_ptr<ZipArchive>(new ZipArchive);
	archive->m_path = path;
	size_t pos = 0;
	for (uint16_t i = 0; i < entryCount && pos + 46 <= directory.size(); ++i) {
		const uint8_t* h = &directory[pos];
		if (u32(h) != kCentralDirectoryEntry) {
			break;
		}
		const uint16_t nameLength = u16(h + 28);
		const uint16_t extraLength = u16(h + 30);
		const uint16_t commentLength = u16(h + 32);
		Entry entry;
		entry.method = u16(h + 10);
		entry.compressedSize = u32(h + 20);
		entry.uncompressedSize = u32(h + 24);
		entry.localHeaderOffset = u32(h + 42);
		entry.name.assign(reinterpret_cast<const char*>(h + 46), nameLength);
		pos += 46 + nameLength + extraLength + commentLength;
		if (!entry.name.empty() && entry.name.back() != '/' && entry.name.back() != '\\') {
			std::string key = normalizePath(entry.name);
			archive->m_entries.emplace(std::move(key), std::move(entry));
		}
	}
	return archive;
}

bool ZipArchive::contains(const std::string& name) const
{
	return m_entries.count(normalizePath(name)) != 0;
}

uint32_t ZipArchive::size(const std::string& name) const
{
	const auto it = m_entries.find(normalizePath(name));
	return it == m_entries.end() ? 0 : it->second.uncompressedSize;
}

bool ZipArchive::read(const std::string& name, std::vector<uint8_t>& out) const
{
	const auto it = m_entries.find(normalizePath(name));
	if (it == m_entries.end()) {
		return false;
	}
	const Entry& entry = it->second;
	File file(m_path);
	uint8_t header[30];
	if (!file || !file.readAt(static_cast<long>(entry.localHeaderOffset), header, sizeof(header)) ||
		u32(header) != kLocalFileHeader) {
		return false;
	}
	const long dataOffset = static_cast<long>(entry.localHeaderOffset) + 30 + u16(header + 26) + u16(header + 28);
	std::vector<uint8_t> compressed(entry.compressedSize);
	if (!file.readAt(dataOffset, compressed.data(), compressed.size())) {
		return false;
	}
	if (entry.method == 0) {
		out = std::move(compressed);
		return true;
	}
	if (entry.method != 8) {
		return false;
	}
	out.resize(entry.uncompressedSize);
	z_stream stream {};
	if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
		return false;
	}
	stream.next_in = compressed.data();
	stream.avail_in = static_cast<uInt>(compressed.size());
	stream.next_out = out.data();
	stream.avail_out = static_cast<uInt>(out.size());
	const int result = inflate(&stream, Z_FINISH);
	inflateEnd(&stream);
	return result == Z_STREAM_END && stream.total_out == entry.uncompressedSize;
}

std::vector<std::string> ZipArchive::list(const std::string& directory) const
{
	const std::string dir = normalizePath(directory);
	std::vector<std::string> names;
	for (const auto& [key, entry] : m_entries) {
		if (directoryOf(key) == dir) {
			names.push_back(key);
		}
	}
	std::sort(names.begin(), names.end());
	return names;
}

std::vector<std::string> ZipArchive::listRecursive(const std::string& directory) const
{
	std::string prefix = normalizePath(directory);
	if (!prefix.empty() && prefix.back() != '/') {
		prefix.push_back('/');
	}
	std::vector<std::string> names;
	for (const auto& [key, entry] : m_entries) {
		if (key.compare(0, prefix.size(), prefix) == 0) {
			names.push_back(key);
		}
	}
	std::sort(names.begin(), names.end());
	return names;
}

} // namespace dice::hfe::io
