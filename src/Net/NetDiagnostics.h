// Network diagnostics for joining servers we didn't write: a log of every
// datagram in and out (hex and decoded fields), a strict check that flags
// whatever doesn't add up, and a capture of the datagrams that can be played
// back into the client offline.
//
// The log (`--net-log <file>`): one entry per datagram with its time, the
// direction, the packet header (type, connection, the notify sequence,
// acknowledgement and mask), the bytes in hex, and for data packets the
// three blocks decoded: the input block's moves, each event (its bit
// offset, name and fields) and the ghost block (the control-object record
// and each update's kind, ghost, length and where it starts).
//
// The problems (`--net-strict` also prints each as it happens and a summary
// at the end), each prefixed "!!" in the log:
//   - an unknown packet type, a data packet whose 16-bit length isn't what
//     the datagram holds;
//   - an event type no event is registered for, an event or block read past
//     the end of the payload;
//   - a ghost update of kind 2 (the exe's error), bits left after the ghost
//     block beyond the byte's padding, or padding that isn't zero;
//   - from RemoteWorld: an object of a template we don't have, an update of
//     a ghost never created, a ghost state (or the control-object record,
//     or the player's own state in it) whose decoded length doesn't end where
//     its length field says, or that can't be decoded.
// Every one of these means our reading of a record disagrees with the
// server's writing, so one live session lists exactly what to fix.
//
// The capture (`--net-capture <file>`, played with `--net-replay <file>`):
//   "BF2142NETCAP" (12 bytes), u32 version (1), then records of
//   u8 direction (0 received, 1 sent), u32 milliseconds since the first
//   record, u16 size and the datagram, all little-endian. A replay hands the
//   received datagrams to the client as if they came from the socket (from
//   the connect reply on) and sends nothing.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe::net {

struct CaptureRecord {
	bool outgoing = false;
	uint32_t milliseconds = 0;
	std::vector<uint8_t> datagram;
};

bool writeCapture(const std::string& path, const std::vector<CaptureRecord>& records);
bool readCapture(const std::string& path, std::vector<CaptureRecord>& records);

// What the payload walk found (also used by the tests).
struct PayloadReport {
	std::string text;                  // the decoded fields, one line each
	std::vector<std::string> problems; // see above
	size_t endBit = 0;                 // where the ghost block ended
};
// Decodes a data payload (the bytes after the 16-bit length) block by
// block, as 0x0057a170 reads it; `fromServer` only labels the text.
PayloadReport describePayload(const std::vector<uint8_t>& payload, bool fromServer, unsigned ghostLengthBits = 11);
// The same for a whole datagram: the header and, for data, the payload.
PayloadReport describeDatagram(const std::vector<uint8_t>& datagram, bool outgoing, unsigned ghostLengthBits = 11);

class NetDiagnostics {
public:
	NetDiagnostics() = default;
	~NetDiagnostics();
	NetDiagnostics(const NetDiagnostics&) = delete;
	NetDiagnostics& operator=(const NetDiagnostics&) = delete;

	// The text log (empty: none). False when it can't be written.
	bool openLog(const std::string& path);
	// Keeps every datagram for writeCapture (saveCapture writes them; the
	// destructor does too, and with `strict` prints the summary).
	void captureTo(const std::string& path) { m_capturePath = path; }
	bool saveCapture() const;
	// Each problem also goes to stderr as it is found.
	bool strict = false;

	// A datagram in or out (GameConnection::onDatagram).
	void datagram(bool outgoing, const std::vector<uint8_t>& datagram);
	// A problem found outside the datagram's own walk (RemoteWorld's).
	void problem(const std::string& category, const std::string& text);
	// A plain line for the log (no problem).
	void note(const std::string& text);

	size_t problems() const { return m_problemCount; }
	const std::map<std::string, size_t>& problemsByCategory() const { return m_categories; }
	const std::vector<std::string>& problemList() const { return m_problems; }
	size_t datagrams() const { return m_index; }
	// The counts per category, for the end of a session.
	std::string summary() const;

private:
	uint32_t now();
	void write(const std::string& text);

	std::FILE* m_log = nullptr;
	std::string m_capturePath;
	std::vector<CaptureRecord> m_records;
	size_t m_index = 0;
	bool m_haveStart = false;
	uint64_t m_start = 0;
	size_t m_problemCount = 0;
	std::map<std::string, size_t> m_categories;
	std::vector<std::string> m_problems; // the first 1000
};

} // namespace dice::hfe::net
