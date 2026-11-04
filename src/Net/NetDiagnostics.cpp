#include "NetDiagnostics.h"

#include "BitStream.h"
#include "GameConnection.h"
#include "GameProtocol.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dice::hfe::net {

namespace {

constexpr char kCaptureMagic[12] = {'B', 'F', '2', '1', '4', '2', 'N', 'E', 'T', 'C', 'A', 'P'};
constexpr uint32_t kCaptureVersion = 1;

void put32(std::string& out, uint32_t v)
{
	for (int i = 0; i < 4; ++i) {
		out.push_back(static_cast<char>(v >> (8 * i) & 0xff));
	}
}

std::string hex(const std::vector<uint8_t>& bytes, size_t from = 0)
{
	std::string out;
	char b[4];
	for (size_t i = from; i < bytes.size(); ++i) {
		if (i > from && (i - from) % 32 == 0) {
			out += "\n      ";
		}
		std::snprintf(b, sizeof(b), "%02x", bytes[i]);
		out += b;
		if ((i - from) % 32 != 31) {
			out += ' ';
		}
	}
	return out;
}

std::string printable(const std::string& bytes)
{
	std::string out = "\"";
	for (unsigned char c : bytes) {
		if (c >= 32 && c < 127 && c != '"' && c != '\\') {
			out += static_cast<char>(c);
		} else {
			char b[8];
			std::snprintf(b, sizeof(b), "\\x%02x", c);
			out += b;
		}
	}
	return out + "\"";
}

#if defined(__MINGW32__)
std::string format(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
#else
std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
std::string format(const char* fmt, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	return buffer;
}

std::string eventText(const GameEvent& e)
{
	std::string out = format("%s (0x%02x)", gameEventName(e.type) ? gameEventName(e.type) : "?", e.type);
	if (!e.values.empty()) {
		out += " values";
		for (uint32_t v : e.values) {
			out += format(" %u", v);
		}
	}
	for (const std::string& b : e.bytes) {
		out += " " + printable(b);
	}
	return out;
}

void walkInput(BitReader& in, PayloadReport& r)
{
	InputBlock input;
	const size_t at = in.position();
	if (!readInputBlock(in, input)) {
		r.problems.push_back(format("read past end: the input block (from bit %zu)", at));
		return;
	}
	if (!input.present) {
		r.text += "    input: none\n";
		return;
	}
	r.text += format("    input: %zu moves, value9 %u, first %d\n", input.moves.size(), input.value9,
		input.moves.empty() ? 0 : input.moves.front().index);
	for (const PlayerMove& m : input.moves) {
		r.text += format("      move %d: axes %d %d %d %d %d %d, triggers %08x %03x, axis %d\n", m.index, m.axes[0],
			m.axes[1], m.axes[2], m.axes[3], m.axes[4], m.axes[5], m.triggersLow, m.triggersHigh, m.axisPitchRoll ? 1 : 0);
	}
}

bool walkEvents(BitReader& in, PayloadReport& r)
{
	const size_t at = in.position();
	const uint32_t present = in.read(1);
	if (in.bad()) {
		r.problems.push_back(format("read past end: the event block (from bit %zu)", at));
		return false;
	}
	if (!present) {
		r.text += "    events: none\n";
		return true;
	}
	const uint32_t count = in.read(8);
	const uint32_t sequence = in.read(5);
	const uint32_t resent = in.read(1);
	r.text += format("    events: block %u%s, %u events\n", sequence, resent ? " (resent)" : "", count);
	for (uint32_t i = 0; i < count; ++i) {
		const size_t start = in.position();
		GameEvent event;
		if (!readGameEvent(in, event)) {
			if (in.bad()) {
				r.problems.push_back(format("read past end: event %u of %u (type %u at bit %zu)", i + 1, count, event.type, start));
			} else {
				r.problems.push_back(format("unknown event type: %u (event %u of %u at bit %zu)", event.type, i + 1, count, start));
			}
			return false;
		}
		r.text += format("      @%zu+%zu ", start, in.position() - start) + eventText(event) + "\n";
	}
	return true;
}

void walkGhosts(BitReader& in, PayloadReport& r, unsigned lengthBits)
{
	const size_t at = in.position();
	GhostBlock block;
	const bool ok = readGhostBlock(in, block, lengthBits);
	if (!block.present && ok) {
		r.text += "    ghosts: none\n";
		return;
	}
	r.text += format("    ghosts: tick %u, %zu updates%s\n", block.tick, block.updates.size(),
		block.control ? ", a control-object record" : "");
	if (block.control) {
		const ControlObjectRecord& c = *block.control;
		r.text += format("      @%zu control: length %zu, last move %d, position %.3f %.3f %.3f, ghost %u, second %u, "
						 "vehicle %u, %zu bits of state\n",
			c.bitOffset, c.length, c.lastMove, c.position.x, c.position.y, c.position.z, c.ghost, c.second, c.vehicle,
			c.restBits);
	}
	for (const GhostUpdate& u : block.updates) {
		if (u.kind == 1) {
			r.text += format("      @%zu update ghost %u%s, %zu bits\n", u.bitOffset, u.ghost, u.full ? " (full)" : "",
				u.stateBits);
		} else {
			r.text += format("      @%zu kind %u ghost %u%s\n", u.bitOffset, u.kind, u.ghost,
				u.kind == 3 ? " (removed)" : "");
		}
	}
	if (!ok) {
		if (!block.updates.empty() && block.updates.back().kind == 2) {
			r.problems.push_back(format("ghost kind 2: ghost %u at bit %zu (the exe's error)", block.updates.back().ghost,
				block.updates.back().bitOffset));
		} else {
			r.problems.push_back(format("read past end: the ghost block (from bit %zu)", at));
		}
	}
}

} // namespace

PayloadReport describePayload(const std::vector<uint8_t>& payload, bool fromServer, unsigned ghostLengthBits)
{
	PayloadReport r;
	(void)fromServer;
	BitReader in(payload.data(), payload.size());
	walkInput(in, r);
	if (!r.problems.empty() || !walkEvents(in, r)) {
		return r;
	}
	walkGhosts(in, r, ghostLengthBits);
	r.endBit = in.position();
	if (!in.bad() && r.problems.empty()) {
		const size_t left = payload.size() * 8 - r.endBit;
		if (left >= 8) {
			r.problems.push_back(format("bits left: %zu after the ghost block (ended at bit %zu of %zu)", left, r.endBit,
				payload.size() * 8));
		} else if (left > 0) {
			const uint32_t pad = in.read(static_cast<unsigned>(left));
			if (pad != 0) {
				r.problems.push_back(format("padding not zero: %zu bits 0x%x after bit %zu", left, pad, r.endBit));
			}
		}
	}
	return r;
}

PayloadReport describeDatagram(const std::vector<uint8_t>& d, bool outgoing, unsigned ghostLengthBits)
{
	PayloadReport r;
	BitReader in(d.data(), d.size());
	const uint32_t type = in.read(4);
	const uint32_t connection = in.read(8);
	const char* names[16] = {nullptr, "connect", "accept", "refuse", "acknowledge", "disconnect", nullptr, "ping", "pong",
		nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "data"};
	r.text = format("  %s %s (type %u), connection %u, %zu bytes\n", outgoing ? "out" : "in",
		names[type] ? names[type] : "?", type, connection, d.size());
	r.text += "      " + hex(d) + "\n";
	switch (type) {
	case 1: {
		const uint32_t protocol = in.read(32), build = in.read(32), ac = in.read(1);
		r.text += format("    protocol 0x%x, build 0x%x, anti-cheat %u, key %s, password %s, mod %s\n", protocol, build, ac,
			printable(in.readText(4)).c_str(), printable(in.readText(32)).c_str(), printable(in.readText(32)).c_str());
		break;
	}
	case 2: {
		const uint32_t id = in.read(8), clock = in.read(32), ac = in.read(1);
		r.text += format("    connection %u, server clock %u, anti-cheat %u\n", id, clock, ac);
		break;
	}
	case 3: {
		const uint32_t reason = in.read(32);
		r.text += format("    reason 0x%x (%s)", reason, refusalReason(reason));
		if (in.read(1) == 1) {
			r.text += " " + printable(in.readText(32));
		}
		r.text += "\n";
		break;
	}
	case 4:
		break;
	case 5: {
		const uint32_t reason = in.read(32);
		r.text += format("    reason 0x%x (%s)\n", reason, disconnectReason(reason));
		break;
	}
	case 7:
	case 8:
	case 15: {
		ConnectedPacket packet;
		if (!readConnectedPacket(d, packet)) {
			r.problems.push_back("read past end: the packet header");
			break;
		}
		r.text += format("    sequence %u, acknowledged %u, mask %08x", packet.sequence, packet.acknowledged, packet.mask);
		if (type != 15) {
			r.text += format(", sequenced %d, time %u", packet.sequenced ? 1 : 0, packet.time);
			if (type == 8) {
				r.text += format(", echo %u", packet.echo);
			}
			r.text += "\n";
			break;
		}
		if (packet.payload.size() < 2) {
			r.text += "\n";
			r.problems.push_back("read past end: the data packet's length");
			break;
		}
		const size_t length = static_cast<size_t>(packet.payload[0]) | static_cast<size_t>(packet.payload[1]) << 8;
		r.text += format(", payload %zu bytes\n", length);
		if (length != packet.payload.size() - 2) {
			r.problems.push_back(format("length: the data packet says %zu payload bytes, the datagram holds %zu", length,
				packet.payload.size() - 2));
		}
		std::vector<uint8_t> payload(packet.payload.begin() + 2, packet.payload.end());
		if (payload.size() > length) {
			payload.resize(length);
		}
		PayloadReport inner = describePayload(payload, !outgoing, ghostLengthBits);
		r.text += inner.text;
		r.problems.insert(r.problems.end(), inner.problems.begin(), inner.problems.end());
		r.endBit = inner.endBit;
		break;
	}
	default:
		r.problems.push_back(format("unknown packet type: %u", type));
		break;
	}
	return r;
}

bool writeCapture(const std::string& path, const std::vector<CaptureRecord>& records)
{
	std::string out(kCaptureMagic, sizeof(kCaptureMagic));
	put32(out, kCaptureVersion);
	for (const CaptureRecord& r : records) {
		out.push_back(r.outgoing ? 1 : 0);
		put32(out, r.milliseconds);
		const size_t size = std::min<size_t>(r.datagram.size(), 0xffff);
		out.push_back(static_cast<char>(size & 0xff));
		out.push_back(static_cast<char>(size >> 8));
		out.append(reinterpret_cast<const char*>(r.datagram.data()), size);
	}
	std::ofstream file(path, std::ios::binary);
	file.write(out.data(), static_cast<std::streamsize>(out.size()));
	return static_cast<bool>(file);
}

bool readCapture(const std::string& path, std::vector<CaptureRecord>& records)
{
	std::ifstream file(path, std::ios::binary);
	const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (data.size() < 16 || std::memcmp(data.data(), kCaptureMagic, sizeof(kCaptureMagic)) != 0) {
		return false;
	}
	const auto u8 = [&data](size_t at) { return static_cast<uint32_t>(static_cast<unsigned char>(data[at])); };
	const auto u32 = [&u8](size_t at) { return u8(at) | u8(at + 1) << 8 | u8(at + 2) << 16 | u8(at + 3) << 24; };
	if (u32(12) != kCaptureVersion) {
		return false;
	}
	records.clear();
	for (size_t at = 16; at < data.size();) {
		if (at + 7 > data.size()) {
			return false;
		}
		CaptureRecord r;
		r.outgoing = data[at] != 0;
		r.milliseconds = u32(at + 1);
		const size_t size = u8(at + 5) | u8(at + 6) << 8;
		at += 7;
		if (at + size > data.size()) {
			return false;
		}
		r.datagram.assign(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + size));
		at += size;
		records.push_back(std::move(r));
	}
	return true;
}

NetDiagnostics::~NetDiagnostics()
{
	if (!m_capturePath.empty() && !saveCapture()) {
		std::fprintf(stderr, "net: cannot write the capture %s\n", m_capturePath.c_str());
	}
	if (strict) {
		std::fputs(summary().c_str(), stderr);
	}
	if (m_log) {
		std::fputs(summary().c_str(), m_log);
		std::fclose(m_log);
	}
}

bool NetDiagnostics::openLog(const std::string& path)
{
	if (m_log) {
		std::fclose(m_log);
	}
	m_log = std::fopen(path.c_str(), "w");
	return m_log != nullptr;
}

bool NetDiagnostics::saveCapture() const
{
	return m_capturePath.empty() || writeCapture(m_capturePath, m_records);
}

uint32_t NetDiagnostics::now()
{
	const uint64_t ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
	if (!m_haveStart) {
		m_haveStart = true;
		m_start = ms;
	}
	return static_cast<uint32_t>(ms - m_start);
}

void NetDiagnostics::write(const std::string& text)
{
	if (m_log) {
		std::fputs(text.c_str(), m_log);
	}
}

void NetDiagnostics::datagram(bool outgoing, const std::vector<uint8_t>& d)
{
	const uint32_t ms = now();
	++m_index;
	if (!m_capturePath.empty()) {
		m_records.push_back({outgoing, ms, d});
	}
	if (!m_log && !strict) {
		return;
	}
	const PayloadReport report = describeDatagram(d, outgoing);
	write(format("#%zu %u ms\n", m_index, ms) + report.text);
	for (const std::string& p : report.problems) {
		const size_t colon = p.find(':');
		problem(colon == std::string::npos ? p : p.substr(0, colon), colon == std::string::npos ? "" : p.substr(colon + 2));
	}
	if (m_log) {
		std::fflush(m_log);
	}
}

void NetDiagnostics::problem(const std::string& category, const std::string& text)
{
	++m_problemCount;
	++m_categories[category];
	const std::string line = format("!! #%zu %s: ", m_index, category.c_str()) + text;
	if (m_problems.size() < 1000) {
		m_problems.push_back(line);
	}
	write(line + "\n");
	if (strict) {
		std::fprintf(stderr, "net-strict %s\n", line.c_str());
	}
}

void NetDiagnostics::note(const std::string& text)
{
	write("   " + text + "\n");
}

std::string NetDiagnostics::summary() const
{
	std::string out = format("net diagnostics: %zu datagrams, %zu problems\n", m_index, m_problemCount);
	for (const auto& [category, count] : m_categories) {
		out += format("  %zu %s\n", count, category.c_str());
	}
	return out;
}

} // namespace dice::hfe::net
