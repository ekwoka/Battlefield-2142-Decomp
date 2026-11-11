#include "FileManager.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>

namespace dice::hfe::io {

namespace fs = std::filesystem;

namespace {

bool startsWith(const std::string& s, const std::string& prefix)
{
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string withTrailingSlash(std::string path)
{
	if (!path.empty() && path.back() != '/') {
		path.push_back('/');
	}
	return path;
}

bool equalsIgnoringCase(const std::string& a, const std::string& b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
	});
}

// Old content paths were prefixed with this; the original strips it.
constexpr const char kLegacyContentPrefix[] = "bf2/content/";

} // namespace

std::string findFileIgnoringCase(const std::string& base, const std::string& relative)
{
	fs::path current = base;
	std::error_code ec;
	size_t start = 0;
	while (start <= relative.size()) {
		const size_t slash = relative.find('/', start);
		const std::string part = relative.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
		start = slash == std::string::npos ? relative.size() + 1 : slash + 1;
		if (part.empty()) {
			continue;
		}
		const fs::path exact = current / part;
		if (fs::exists(exact, ec)) {
			current = exact;
			continue;
		}
		bool found = false;
		for (const auto& entry : fs::directory_iterator(current, ec)) {
			if (equalsIgnoringCase(entry.path().filename().string(), part)) {
				current = entry.path();
				found = true;
				break;
			}
		}
		if (!found) {
			return {};
		}
	}
	return fs::is_regular_file(current, ec) ? current.string() : std::string();
}

FileManager::FileManager() = default;
FileManager::~FileManager() = default;

void FileManager::setRootPath(std::string path)
{
	m_root = withTrailingSlash(std::move(path));
}

void FileManager::setHomePath(std::string path)
{
	m_home = withTrailingSlash(std::move(path));
}

void FileManager::addSearchPath(const std::string& path)
{
	const fs::path p(path);
	m_searchPaths.push_back(withTrailingSlash(p.is_absolute() ? path : m_root + path));
}

size_t FileManager::mountedArchiveCount() const
{
	size_t count = 0;
	for (const auto& [point, mount] : m_mounts) {
		count += mount.archives.size();
	}
	return count;
}

// 0x0069a810: refused with "not supported in runtime" once mounting is
// closed (+0x6d); an archive already mounted isn't mounted again; the
// first search path that opens it mounts it, appended to its mount
// point's list.
bool FileManager::mountArchive(const std::string& archive, const std::string& mountPoint)
{
	if (!m_mountingAllowed) {
		std::fprintf(stderr, "FileManager: mountArchive %s: not supported in runtime\n", archive.c_str());
		return false;
	}
	const std::string archiveName = normalizePath(archive);
	for (const std::string& searchPath : m_searchPaths) {
		const std::string real = findFileIgnoringCase(searchPath, archiveName);
		if (real.empty()) {
			continue;
		}
		if (!m_mountedFiles.insert(normalizePath(real)).second) {
			return false; // already mounted
		}
		std::unique_ptr<ZipArchive> zip = ZipArchive::open(real);
		if (!zip) {
			continue;
		}
		std::string point = normalizePath(mountPoint);
		while (!point.empty() && point.back() == '/') {
			point.pop_back();
		}
		m_mounts[point].archives.push_back(std::move(zip));
		return true;
	}
	return false;
}

const FileManager::MountPoint* FileManager::findMount(const std::string& normalized, std::string& relative) const
{
	// The longest mount point that is a directory prefix of the path.
	const MountPoint* best = nullptr;
	size_t bestLength = 0;
	for (const auto& [point, mount] : m_mounts) {
		const bool matches = point.empty() ||
			(startsWith(normalized, point) && normalized.size() > point.size() && normalized[point.size()] == '/');
		if (matches && (best == nullptr || point.size() > bestLength)) {
			best = &mount;
			bestLength = point.size();
			relative = point.empty() ? normalized : normalized.substr(point.size() + 1);
		}
	}
	return best;
}

std::string FileManager::resolveLooseFile(const std::string& normalized) const
{
	if (startsWith(normalized, "@root@/")) {
		return findFileIgnoringCase(m_root, normalized.substr(7));
	}
	if (startsWith(normalized, "@home@/")) {
		return findFileIgnoringCase(m_home, normalized.substr(7));
	}
	if (fs::path(normalized).is_absolute()) {
		std::error_code ec;
		return fs::is_regular_file(normalized, ec) ? normalized : std::string();
	}
	for (const std::string& searchPath : m_searchPaths) {
		std::string real = findFileIgnoringCase(searchPath, normalized);
		if (!real.empty()) {
			return real;
		}
	}
	return {};
}

// 0x00698e40: the first "bf2/content/" in the path erased, wherever it is
// (find on the lower-case path, then erase 12), then the path's mount's
// archives (slot 0x1c), then each search path's loose file.
bool FileManager::exists(const std::string& path) const
{
	std::string normalized = normalizePath(path);
	{
		const std::lock_guard<std::mutex> lock(m_memoryMutex);
		if (m_memoryFiles.count(normalized)) {
			return true;
		}
	}
	if (const size_t legacy = normalized.find(kLegacyContentPrefix); legacy != std::string::npos) {
		normalized.erase(legacy, sizeof(kLegacyContentPrefix) - 1);
	}
	std::string relative;
	if (const MountPoint* mount = findMount(normalized, relative)) {
		for (const auto& archive : mount->archives) {
			if (archive->contains(relative)) {
				return true;
			}
		}
	}
	return !resolveLooseFile(normalized).empty();
}

void FileManager::addMemoryFile(const std::string& path, std::vector<uint8_t> data)
{
	const std::lock_guard<std::mutex> lock(m_memoryMutex);
	m_memoryFiles[normalizePath(path)] = std::move(data);
}

void FileManager::removeMemoryFile(const std::string& path)
{
	const std::lock_guard<std::mutex> lock(m_memoryMutex);
	m_memoryFiles.erase(normalizePath(path));
}

// Opening for reading, 0x00698b60: the path's mount's archives in mount
// order (slot 0x18), then each search path's loose file. Its +0x68 mode 1
// (archives only, except @HOME@, @ROOT@ and .png paths) is never set: the
// constructor 0x0069aee0 clears it, no other code of the exe writes it
// (only the getter, slot 0xa4 = 0x006971b0, reads it), so loose files are
// always looked at, as here.
bool FileManager::readFile(const std::string& path, std::vector<uint8_t>& out) const
{
	const std::string normalized = normalizePath(path);
	{
		const std::lock_guard<std::mutex> lock(m_memoryMutex);
		if (!m_memoryFiles.empty()) {
			if (const auto it = m_memoryFiles.find(normalized); it != m_memoryFiles.end()) {
				out = it->second;
				return true;
			}
		}
	}
	std::string relative;
	if (const MountPoint* mount = findMount(normalized, relative)) {
		for (const auto& archive : mount->archives) {
			if (archive->read(relative, out)) {
				return true;
			}
		}
	}
	const std::string real = resolveLooseFile(normalized);
	if (real.empty()) {
		return false;
	}
	FILE* file = std::fopen(real.c_str(), "rb");
	if (!file) {
		return false;
	}
	std::fseek(file, 0, SEEK_END);
	const long size = std::ftell(file);
	std::fseek(file, 0, SEEK_SET);
	out.resize(size > 0 ? static_cast<size_t>(size) : 0);
	const bool ok = std::fread(out.data(), 1, out.size(), file) == out.size();
	std::fclose(file);
	return ok;
}

bool FileManager::readText(const std::string& path, std::string& out) const
{
	std::vector<uint8_t> data;
	if (!readFile(path, data)) {
		return false;
	}
	out.assign(data.begin(), data.end());
	return true;
}

// 0x00697d70: a path that already starts with the first search path as it
// is (0x0065b9c0, _strnicmp over the search path's length), then an
// absolute path as it is, @ROOT@ / @HOME@ to the root and home
// directories, anything else under the first search path. The original's
// search paths are relative to the installation ("mods/bf2142/"); here they
// are absolute, so a path starting with the first one's part under the root
// is put under the root.
std::string FileManager::expandPath(const std::string& path) const
{
	const std::string normalized = normalizePath(path);
	if (!m_searchPaths.empty()) {
		const std::string& first = m_searchPaths.front();
		const std::string given =
			!m_root.empty() && startsWith(first, m_root) ? first.substr(m_root.size()) : first;
		if (!given.empty() && startsWith(normalized, normalizePath(given))) {
			return fs::path(given).is_absolute() ? path : m_root + path;
		}
	}
	if (startsWith(normalized, "@root@/")) {
		return m_root + path.substr(7);
	}
	if (startsWith(normalized, "@home@/")) {
		return m_home + path.substr(7);
	}
	if (fs::path(path).is_absolute() || m_searchPaths.empty()) {
		return path;
	}
	return m_searchPaths.front() + path;
}

std::vector<std::string> FileManager::listFiles(const std::string& directory) const
{
	std::set<std::string> names;
	std::string normalized = normalizePath(directory);
	while (!normalized.empty() && normalized.back() == '/') {
		normalized.pop_back();
	}
	// Mount lookup works on file paths; append a placeholder file name.
	std::string relative;
	if (const MountPoint* mount = findMount(normalized + "/.", relative)) {
		// `relative` is "." at the mount point itself, else "<dir>/.".
		const std::string dir = relative == "." ? std::string() : relative.substr(0, relative.size() - 2);
		for (const auto& archive : mount->archives) {
			for (const std::string& name : archive->list(dir)) {
				names.insert(name.substr(name.rfind('/') + 1));
			}
		}
	}
	std::error_code ec;
	for (const std::string& searchPath : m_searchPaths) {
		const std::string real = searchPath + normalized;
		for (const auto& entry : fs::directory_iterator(real, ec)) {
			if (entry.is_regular_file(ec)) {
				names.insert(normalizePath(entry.path().filename().string()));
			}
		}
	}
	return {names.begin(), names.end()};
}

std::vector<std::string> FileManager::listFilesRecursive(const std::string& directory) const
{
	std::set<std::string> paths;
	std::string normalized = normalizePath(directory);
	while (!normalized.empty() && normalized.back() == '/') {
		normalized.pop_back();
	}
	// Archives mounted at or below the directory, or above it.
	for (const auto& [point, mount] : m_mounts) {
		std::string inside; // the directory relative to the mount point
		std::string outside; // the mount point relative to the directory
		if (point.empty()) {
			inside = normalized;
		} else if (normalized.empty()) {
			outside = point + "/";
		} else if (startsWith(normalized + "/", point + "/")) {
			inside = normalized.size() > point.size() ? normalized.substr(point.size() + 1) : std::string();
		} else if (startsWith(point, normalized + "/")) {
			outside = point.substr(normalized.size() + 1) + "/";
		} else {
			continue;
		}
		for (const auto& archive : mount.archives) {
			for (const std::string& name : archive->listRecursive(inside)) {
				paths.insert(outside + (inside.empty() ? name : name.substr(inside.size() + 1)));
			}
		}
	}
	std::error_code ec;
	for (const std::string& searchPath : m_searchPaths) {
		const fs::path base = fs::path(searchPath) / normalized;
		for (auto it = fs::recursive_directory_iterator(base, ec); !ec && it != fs::recursive_directory_iterator();
			 it.increment(ec)) {
			if (it->is_regular_file(ec)) {
				paths.insert(normalizePath(fs::relative(it->path(), base, ec).generic_string()));
			}
		}
	}
	return {paths.begin(), paths.end()};
}

} // namespace dice::hfe::io
