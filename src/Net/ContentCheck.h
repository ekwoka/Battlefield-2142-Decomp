// The pure-server content check (BF2142.exe's "dep" check; the shipped
// generate_md5.py writes the fingerprints it compares).
//
// A server whose mod has std_archive.md5 is pure: the game server's init
// (0x004aff80) loads it (0x004adb60: "<ordinal> <md5>" lines into a map,
// and the md5 of the archive lists, below) and sets the settings' content
// check flag (DAT_00a3fcc0 +0x54, data block 0's contentCheck bit) from
// it. Each level start (0x004aefe0) draws the round's ordinal, rand() % 10
// with srand(ms), into the level block (data block 2's last field, level
// info +0x5c), and loads the level's archive.md5 (0x004adda0: "<level>
// <ordinal> <md5>" lines; +0x1ad says it was there).
//
// A client of a pure server, once it has loaded the level (0x004b5eb0
// case 2), fingerprints its content for that ordinal (0x0068ba40) and
// sends the ContentCheck event (0x2e; 0x005123f0 copies the three
// digests, at +4, +0x14 and +0x24 of the client's fingerprint, read by
// 0x005a9ea0 / 0x0087b560 / 0x0058cc60; written by 0x005124d0, read by
// 0x00512470, 128 bits each):
//   - lists: md5 of ClientArchives.con, ServerArchives.con, Init.con and
//     GameLogicInit.con, whole;
//   - common: the striped fingerprint of every archive outside levels/;
//   - level: the same of the level's archives (client.zip, server.zip).
// The striped fingerprint: archives sorted by their lower-cased path; of
// each, its 16 KB blocks whose index % 10 is the ordinal, at most 500
// blocks ((size >> 14) + 1 of them); a file over 500 blocks contributes
// its first ten blocks whole and then its last 500; then its size in
// decimal ("%d").
//
// The server (game server slot 51, 0x004aec40, from the event's server
// handler 0x005123a0) compares: lists with its own md5 of the four files,
// and, when the level's archive.md5 was there, common and level with the
// ordinal's lines of the two .md5 files. A match marks the connection
// checked (its slot 0x28(1)); a mismatch kicks the client with reason
// 0x1b ("dep: kicking client <id> (<name>) for using modified data").
// A client still unchecked when it answers DatabaseComplete (0x004af710)
// is kicked with 0x1b too.
#pragma once

#include "GameProtocol.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe::net {

using Digest = std::array<uint8_t, 16>;

struct ContentDigests {
	Digest lists {};  // the four .con files
	Digest common {}; // the archives outside levels/
	Digest level {};  // the level's archives
};

// 0x0068b100: 32 lower-case hex digits.
std::string digestHex(const Digest& digest);

// The archives ClientArchives.con and ServerArchives.con mount
// ("fileManager.mountArchive <path> <mount>"), as generate_md5.py lists
// them: relative to the mod ("mods/bf2142/..."), '/'-separated, lower case,
// sorted. `modRoot` is the mod's folder on disk; `modDirectory` its name
// in the game ("mods/bf2142").
std::vector<std::string> commonArchives(const std::string& modRoot, const std::string& modDirectory = "mods/bf2142");
// The level's archives (client.zip and server.zip of `levelDirectory`,
// "levels/Suez_Canal/"), the same way.
std::vector<std::string> levelArchives(const std::string& levelDirectory, const std::string& modDirectory = "mods/bf2142");

// The striped fingerprint of `archives` (paths as above, found under
// `gameRoot` without regard to case) for one ordinal (0-9); a file that
// isn't there adds nothing.
Digest stripedFingerprint(const std::string& gameRoot, const std::vector<std::string>& archives, int ordinal);
// All ten, as generate_md5.py writes them ("<ordinal> <md5>").
std::array<std::string, 10> stripedFingerprints(const std::string& gameRoot, const std::vector<std::string>& archives);
// md5 of ClientArchives.con, ServerArchives.con, Init.con, GameLogicInit.con.
Digest archiveListsDigest(const std::string& modRoot);

// The client's fingerprint for a level and ordinal (0x0068ba40(0)).
// `gameRoot` is the game's folder (holding mods/).
ContentDigests computeContentDigests(const std::string& gameRoot, const std::string& modDirectory,
	const std::string& levelDirectory, int ordinal);

// std_archive.md5 / a level's archive.md5: ordinal -> md5 hex (a level's
// lines start with the level's name).
std::map<int, std::string> parseArchiveMd5(const std::string& text, bool levelFile);

// The server's side (the game server's +0x178 lists digest, +0x194 and
// +0x1a0 maps, +0x1ac / +0x1ad flags).
class ContentChecker {
public:
	// 0x004adb60: std_archive.md5 and the lists' md5. False (not pure)
	// without the file.
	bool loadCommon(const std::string& gameRoot, const std::string& modDirectory = "mods/bf2142");
	// 0x004adda0: the level's archive.md5 (`levelDirectory` "levels/X/").
	bool loadLevel(const std::string& gameRoot, const std::string& levelDirectory,
		const std::string& modDirectory = "mods/bf2142");
	bool pure() const { return m_pure; }
	bool hasLevel() const { return m_hasLevel; }
	// 0x004aec40's comparison for the round's ordinal.
	bool matches(int ordinal, const ContentDigests& digests) const;

private:
	bool m_pure = false, m_hasLevel = false;
	std::string m_lists;
	std::map<int, std::string> m_common, m_level;
};

// The ContentCheck event (0x2e) and back.
GameEvent makeContentCheck(const ContentDigests& digests);
bool parseContentCheck(const GameEvent& event, ContentDigests& out);

// The kick reason for content that doesn't match, or no check at all.
constexpr uint32_t kKickModifiedContent = 0x1b;

} // namespace dice::hfe::net
