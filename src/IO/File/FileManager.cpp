#include "FileManager.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <system_error>

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

bool FileManager::exists(const std::string& path) const
{
	std::string normalized = normalizePath(path);
	if (startsWith(normalized, kLegacyContentPrefix)) {
		normalized.erase(0, sizeof(kLegacyContentPrefix) - 1);
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

bool FileManager::readFile(const std::string& path, std::vector<uint8_t>& out) const
{
	const std::string normalized = normalizePath(path);
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

std::string FileManager::expandPath(const std::string& path) const
{
	const std::string normalized = normalizePath(path);
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
