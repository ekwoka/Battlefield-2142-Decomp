// BF2142.exe's demo files (.bf2142demo; the Demo object 0x005177b0, its
// writer 0x00516950 / 0x00516a00 / 0x00516a70 / 0x005169c0 and reader
// 0x00516ab0 / 0x00516c80, the header 0x005791a0 / 0x005793b0). The whole
// file is one zlib stream (deflateInit_ level 3, 0x006a9a00; inflate
// 0x006a9910). Inside, little-endian:
//
//   u32 version (0x70, 0x005170f0; the reader doesn't check it)
//   u32 time(NULL) when the recording started
//   u32 length + bytes: sv.serverName
//   u32 length + bytes, twice: the game's +0x3c and +4 strings (slots 0xf4,
//       0xf8; taken here as the level and the game mode, which isn't
//       settled)
//
// then records to the end: i32 time, u32 length, the bytes. A time of -1
// plays at once (the join's data, before the connection plays); -2 is a
// marker whose length field is a value (0 after the join's data); the
// last record is (tick, 0). Each payload is the game-level data payload
// the server sent its demo connection 0x81 (0x004b02a0: the input, event
// and ghost blocks; no datagram or notify header), its length the bits
// rounded up to bytes.
#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::net {

struct DemoHeader {
	uint32_t version = 0x70;
	uint32_t started = 0;
	std::string serverName, level, gameMode;
};

struct DemoRecord {
	int32_t time = 0; // the server's tick, -1 at once, -2 a marker
	uint32_t value = 0; // a marker's value
	std::vector<uint8_t> payload;
};

class DemoWriter {
public:
	DemoWriter();
	~DemoWriter();
	// Fails if the file can't be made (0x00432cc0: "Failed to record demo.
	// Maybe the demo ... already exists.").
	bool open(const std::string& path, const DemoHeader& header);
	bool isOpen() const;
	void write(int32_t time, const std::vector<uint8_t>& payload);
	void marker(uint32_t value);
	// The end record (tick, 0) and the stream's end (Z_FINISH).
	void close(int32_t tick);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// The whole file at once.
bool readDemo(const std::string& path, DemoHeader& header, std::vector<DemoRecord>& records);
bool parseDemo(const std::vector<uint8_t>& inflated, DemoHeader& header, std::vector<DemoRecord>& records);

// Playback's clock (0x00516c80): -1 records go one a call at once and
// markers are skipped; the first timed record, or one more than 100 ticks
// ahead, sets the clock to its time (0x006660b0); a record still ahead
// waits.
class DemoPlayer {
public:
	explicit DemoPlayer(std::vector<DemoRecord> records) : m_records(std::move(records)) {}
	// The game's ticks went on (30 a second x the speed; 0 paused).
	void advance(int ticks) { m_tick += ticks; }
	int tick() const { return m_tick; }
	// 1: a payload due now, 0: none yet, -1: the end (the demo is finished).
	int next(std::vector<uint8_t>& payload);
	bool finished() const { return m_at >= m_records.size(); }
	bool started() const { return m_started; } // the clock set by a timed record (the reader's +0xdc)
	void restart() { m_at = 0, m_tick = 0, m_started = false; } // demo.restartDemo
	void finish() { m_at = m_records.size(); } // demo.shutdownDemo: next() answers the end

private:
	std::vector<DemoRecord> m_records;
	size_t m_at = 0;
	int m_tick = 0;
	bool m_started = false;
};

// sv.demoQuality (1..10, the setter 0x0041f80c; 1 by default) as the demo
// connection's rates (0x0057b3f0 for a kind-1 connection, 0x0057b582..
// 0x0057b64c): with f = (q - 1) x 0.11111111, truncated as the original's
// fistp does, 4 - (int)(f x -16) packets a second (4..20), 0x104 -
// (int)(f x -3836) bytes a packet (260..4096) and floor(f x 215) + 40
// ghost updates a block (40..255). 0x004a6160 leaves the rate table out
// for the demo connection.
struct DemoRates {
	int packetsPerSecond = 4;
	int packetBytes = 260;
	int maxUpdates = 40;
};
DemoRates demoRates(int quality);

// enhanced.demoQuality (Scene/EnhancedSettings.h; not retail): the demo
// connection sends every tick (30 a second) with retail's largest packet
// (4096 bytes, 255 updates: the ghost block's 8-bit count), and when one
// packet can't hold what changed, up to `packetsPerSend` payloads that
// tick (records of the same time, which playback, ours and 0x00516c80,
// takes in one go). A 64-player round then plays back at about a client's
// rate instead of starving most ghosts (sv.demoQuality 10 tops at 20 x
// 255 updates a second).
struct EnhancedDemoRates : DemoRates {
	int packetsPerSend = 6;
	EnhancedDemoRates()
	{
		packetsPerSecond = 30;
		packetBytes = 4096;
		maxUpdates = 255;
	}
};

// A demo's ghost traffic (demo.recordDemo's quality): from its records'
// ghost blocks, each ghost's updates by the record's time; the soldiers'
// ghosts are those the HandlePickup events name.
struct DemoStats {
	size_t records = 0, bytes = 0; // the timed records and their payloads' bytes
	int firstTick = 0, lastTick = 0;
	size_t ghosts = 0, updates = 0, soldierGhosts = 0;
	size_t soldierUpdates = 0; // the soldiers' ghosts' updates
	// The gaps (ticks) between a ghost's updates while it lives, over the
	// soldiers' and over every ghost: the median, the 95th percentile and
	// the largest, and the share of soldier gaps over 3 ticks (a tenth of
	// a second).
	double soldierMedian = 0, soldierP95 = 0, soldierMax = 0, soldierOver3 = 0;
	double allMedian = 0, allP95 = 0;
	double seconds() const { return (lastTick - firstTick) / 30.0; }
	// Each soldier's updates a second (30: every tick).
	double soldierUpdateRate() const
	{
		return soldierGhosts && seconds() > 0 ? static_cast<double>(soldierUpdates) / static_cast<double>(soldierGhosts) / seconds() : 0;
	}
};
DemoStats demoStats(const std::vector<DemoRecord>& records);
std::string formatDemoStats(const DemoStats& stats, size_t fileBytes);

// A demo camera file's keyframe (.bf2142cam; the camera object 0x005f93d0:
// written once a tick by 0x005f8d30 from 0x005f9630, read by 0x005f91f0;
// the same zlib stream wrapper as a demo, no header). recordDemo writes
// <name>-def.bf2142cam beside the demo only with a local player (a
// dedicated server makes none). Each keyframe, little-endian:
//   u32 tick, u32 mode (follow when exactly 1 on reading), u8 attached,
//   u8 override (the reader then takes mode 0);
//   mode 0, attached or override: 7 floats, a quaternion x, y, z, w then
//     the position (zeroed on reading when |p|^2 > 1e10);
//   else: u32 the followed object's id, u8, 5 floats (the follow
//     camera's parameters, not settled).
struct CameraKeyframe {
	uint32_t tick = 0;
	uint32_t mode = 0;
	bool attached = false, override = false;
	float rotation[4] = {0, 0, 0, 1};
	float position[3] = {0, 0, 0};
	uint32_t followed = 0;
	uint8_t followFlag = 0;
	float follow[5] = {0, 0, 0, 0, 0};
	bool free() const { return mode == 0 || attached || override; }
};
std::vector<uint8_t> encodeCameraKeyframes(const std::vector<CameraKeyframe>& frames);
bool decodeCameraKeyframes(const std::vector<uint8_t>& inflated, std::vector<CameraKeyframe>& frames);
bool writeCameraFile(const std::string& path, const std::vector<CameraKeyframe>& frames);
bool readCameraFile(const std::string& path, std::vector<CameraKeyframe>& frames);

// A camera file written as it goes (0x005f8d30 writes each keyframe into
// the camera's zlib stream once a tick), flushed now and then so a crash
// leaves what was recorded readable.
class CameraFileWriter {
public:
	CameraFileWriter();
	~CameraFileWriter();
	bool open(const std::string& path);
	bool isOpen() const;
	void write(const CameraKeyframe& frame);
	void flush(); // a sync point: everything so far inflates
	void close();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// A file name's extension (0x0065b5c0): the text after its last '.', none
// when that '.' past the second character follows another '.' (so "a/../x"
// has none, "../x" has "/x"); slashes aren't looked at.
std::string fileExtension(const std::string& name);

// demo.recordDemo's name (0x00432cc0): '\\' as '/', and with no extension
// Demos/<name>.bf2142demo.
std::string demoPath(const std::string& name);

// --demo's file: the name as given when that file exists, else where
// demo.recordDemo puts that name (demoPath: a bare "match1" plays
// Demos/match1.bf2142demo), all relative to the working folder.
std::string demoFileToPlay(const std::string& name);

} // namespace dice::hfe::net
