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
	void restart() { m_at = 0, m_tick = 0, m_started = false; } // demo.restartDemo

private:
	std::vector<DemoRecord> m_records;
	size_t m_at = 0;
	int m_tick = 0;
	bool m_started = false;
};

// demo.recordDemo's name (0x00432cc0): '\\' as '/', and with no extension
// Demos/<name>.bf2142demo.
std::string demoPath(const std::string& name);

} // namespace dice::hfe::net
