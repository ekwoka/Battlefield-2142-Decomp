#include "ContentCheck.h"

#include "Md5.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dice::hfe::net {

namespace {

namespace fs = std::filesystem;

constexpr size_t kBlock = 0x4000;    // 16 KB
constexpr size_t kMaxBlocks = 500;   // 0x7d0000 bytes
constexpr int kOrdinals = 10;

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// `relative` ('/'-separated) under `root`, each part matched without
// regard to case (the archives' names in the .con files and on disk
// differ in case).
fs::path findNoCase(const fs::path& root, const std::string& relative)
{
	fs::path at = root;
	std::stringstream parts(relative);
	std::string part;
	while (std::getline(parts, part, '/')) {
		if (part.empty() || part == ".") {
			continue;
		}
		const fs::path exact = at / part;
		std::error_code error;
		if (fs::exists(exact, error)) {
			at = exact;
			continue;
		}
		bool found = false;
		for (const auto& entry : fs::directory_iterator(at, error)) {
			if (lower(entry.path().filename().string()) == lower(part)) {
				at = entry.path();
				found = true;
				break;
			}
		}
		if (!found) {
			return {};
		}
	}
	return at;
}

bool readWhole(const fs::path& path, std::string& out)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return false;
	}
	std::ostringstream text;
	text << in.rdbuf();
	out = text.str();
	return true;
}

std::string slashes(std::string s)
{
	std::replace(s.begin(), s.end(), '\\', '/');
	return s;
}

// One archive into the ordinal's context (0x0068ba40's inner loop).
void fingerprintFile(const fs::path& path, int ordinal, Md5& context)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return;
	}
	std::error_code error;
	const uint64_t size = fs::file_size(path, error);
	if (error) {
		return;
	}
	std::vector<char> block(kBlock);
	const auto readBlock = [&]() -> size_t {
		in.read(block.data(), static_cast<std::streamsize>(kBlock));
		const size_t got = static_cast<size_t>(in.gcount());
		in.clear();
		return got;
	};
	size_t blocks = static_cast<size_t>(size >> 14) + 1;
	if (size > kMaxBlocks * kBlock) {
		// A big archive: its first ten blocks whole, then its last 500.
		for (int i = 0; i < 10; ++i) {
			context.update(block.data(), readBlock());
		}
		in.seekg(static_cast<std::streamoff>(size - kMaxBlocks * kBlock));
	}
	blocks = std::min(blocks, kMaxBlocks);
	for (size_t i = 0; i < blocks; ++i) {
		if (static_cast<int>(i % kOrdinals) == ordinal) {
			context.update(block.data(), readBlock());
		} else {
			in.seekg(static_cast<std::streamoff>(kBlock), std::ios::cur);
		}
	}
	context.update(std::to_string(size)); // "%d" (0x00918be0)
}

} // namespace

std::string digestHex(const Digest& digest)
{
	static const char* hex = "0123456789abcdef";
	std::string out;
	for (uint8_t b : digest) {
		out += hex[b >> 4];
		out += hex[b & 15];
	}
	return out;
}

std::vector<std::string> commonArchives(const std::string& modRoot, const std::string& modDirectory)
{
	std::vector<std::string> archives;
	const std::string mod = lower(slashes(modDirectory));
	for (const char* list : {"ClientArchives.con", "ServerArchives.con"}) {
		std::string text;
		const fs::path path = findNoCase(modRoot, list);
		if (path.empty() || !readWhole(path, text)) {
			continue;
		}
		std::stringstream lines(text);
		std::string line;
		while (std::getline(lines, line)) {
			std::stringstream words(line);
			std::string command, archive;
			if (!(words >> command >> archive)) {
				continue;
			}
			archive = lower(slashes(archive));
			if (archive.rfind("mods", 0) != 0) {
				archive = mod + "/" + archive;
			}
			archives.push_back(archive);
		}
	}
	std::sort(archives.begin(), archives.end());
	return archives;
}

std::vector<std::string> levelArchives(const std::string& levelDirectory, const std::string& modDirectory)
{
	std::string dir = lower(slashes(levelDirectory));
	while (!dir.empty() && dir.back() == '/') {
		dir.pop_back();
	}
	const std::string base = lower(slashes(modDirectory)) + "/" + dir + "/";
	return {base + "client.zip", base + "server.zip"};
}

Digest stripedFingerprint(const std::string& gameRoot, const std::vector<std::string>& archives, int ordinal)
{
	Md5 context;
	for (const std::string& archive : archives) {
		const fs::path path = findNoCase(gameRoot, archive);
		if (!path.empty()) {
			fingerprintFile(path, ordinal, context);
		}
	}
	return context.digest();
}

std::array<std::string, 10> stripedFingerprints(const std::string& gameRoot, const std::vector<std::string>& archives)
{
	std::array<std::string, 10> out;
	for (int o = 0; o < kOrdinals; ++o) {
		out[static_cast<size_t>(o)] = std::to_string(o) + " " + digestHex(stripedFingerprint(gameRoot, archives, o));
	}
	return out;
}

Digest archiveListsDigest(const std::string& modRoot)
{
	// 0x0068ba40's list: ClientArchives.con, ServerArchives.con, Init.con
	// (0x0090ee58), GameLogicInit.con (0x0092a034), read through in 16 KB
	// pieces.
	Md5 context;
	for (const char* file : {"ClientArchives.con", "ServerArchives.con", "Init.con", "GameLogicInit.con"}) {
		std::string text;
		const fs::path path = findNoCase(modRoot, file);
		if (!path.empty() && readWhole(path, text)) {
			context.update(text);
		}
	}
	return context.digest();
}

ContentDigests computeContentDigests(const std::string& gameRoot, const std::string& modDirectory,
	const std::string& levelDirectory, int ordinal)
{
	ContentDigests out;
	const fs::path modRoot = findNoCase(gameRoot, slashes(modDirectory));
	out.lists = archiveListsDigest(modRoot.string());
	// Two passes over the mounted archives (sorted, 0x0068b910): those
	// outside "/levels/" (+0x14), then the level's (+0x24).
	out.common = stripedFingerprint(gameRoot, commonArchives(modRoot.string(), modDirectory), ordinal);
	out.level = stripedFingerprint(gameRoot, levelArchives(levelDirectory, modDirectory), ordinal);
	return out;
}

std::map<int, std::string> parseArchiveMd5(const std::string& text, bool levelFile)
{
	std::map<int, std::string> out;
	std::stringstream lines(text);
	std::string line;
	while (std::getline(lines, line)) {
		std::stringstream words(line);
		std::string level, md5;
		int ordinal = 0;
		if (levelFile && !(words >> level)) {
			continue;
		}
		if (words >> ordinal >> md5) {
			out[ordinal] = md5;
		}
	}
	return out;
}

bool ContentChecker::loadCommon(const std::string& gameRoot, const std::string& modDirectory)
{
	m_pure = false;
	m_common.clear();
	const fs::path modRoot = findNoCase(gameRoot, slashes(modDirectory));
	if (modRoot.empty()) {
		return false;
	}
	m_lists = digestHex(archiveListsDigest(modRoot.string())); // 0x0068ba40(1)
	std::string text;
	const fs::path path = findNoCase(modRoot, "std_archive.md5");
	if (path.empty() || !readWhole(path, text)) {
		return false;
	}
	m_common = parseArchiveMd5(text, false);
	m_pure = true;
	return true;
}

bool ContentChecker::loadLevel(const std::string& gameRoot, const std::string& levelDirectory,
	const std::string& modDirectory)
{
	m_hasLevel = false;
	m_level.clear();
	std::string text;
	const fs::path path = findNoCase(gameRoot, slashes(modDirectory) + "/" + slashes(levelDirectory) + "/archive.md5");
	if (path.empty() || !readWhole(path, text)) {
		return false;
	}
	m_level = parseArchiveMd5(text, true);
	m_hasLevel = true;
	return true;
}

bool ContentChecker::matches(int ordinal, const ContentDigests& digests) const
{
	const auto common = m_common.find(ordinal);
	if (common == m_common.end() || digestHex(digests.lists) != m_lists) {
		return false;
	}
	if (!m_hasLevel) {
		return true; // 0x004aec40 compares the archives only with the level's file
	}
	const auto level = m_level.find(ordinal);
	return level != m_level.end() && common->second == digestHex(digests.common) && level->second == digestHex(digests.level);
}

GameEvent makeContentCheck(const ContentDigests& digests)
{
	GameEvent e;
	e.type = kEvContentCheck;
	for (const Digest* d : {&digests.lists, &digests.common, &digests.level}) {
		e.bytes.emplace_back(reinterpret_cast<const char*>(d->data()), d->size());
	}
	return e;
}

bool parseContentCheck(const GameEvent& event, ContentDigests& out)
{
	if (event.type != kEvContentCheck || event.bytes.size() < 3) {
		return false;
	}
	Digest* digests[3] = {&out.lists, &out.common, &out.level};
	for (size_t i = 0; i < 3; ++i) {
		if (event.bytes[i].size() < 16) {
			return false;
		}
		std::copy_n(reinterpret_cast<const uint8_t*>(event.bytes[i].data()), 16, digests[i]->begin());
	}
	return true;
}

} // namespace dice::hfe::net
