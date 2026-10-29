#include "Demo.h"

#include <algorithm>
#include <cmath>

#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace dice::hfe::net {

namespace {

void putU32(std::vector<uint8_t>& out, uint32_t v)
{
	for (int i = 0; i < 4; ++i) {
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

void putString(std::vector<uint8_t>& out, const std::string& s)
{
	putU32(out, static_cast<uint32_t>(s.size()));
	out.insert(out.end(), s.begin(), s.end());
}

bool getU32(const std::vector<uint8_t>& in, size_t& at, uint32_t& v)
{
	if (at + 4 > in.size()) {
		return false;
	}
	v = static_cast<uint32_t>(in[at]) | static_cast<uint32_t>(in[at + 1]) << 8 | static_cast<uint32_t>(in[at + 2]) << 16
		| static_cast<uint32_t>(in[at + 3]) << 24;
	at += 4;
	return true;
}

bool getString(const std::vector<uint8_t>& in, size_t& at, std::string& s)
{
	uint32_t length = 0;
	if (!getU32(in, at, length)) {
		return false;
	}
	// 0x005792f0: a length under 1 is an empty string.
	if (static_cast<int32_t>(length) < 1) {
		s.clear();
		return true;
	}
	if (at + length > in.size()) {
		return false;
	}
	s.assign(reinterpret_cast<const char*>(in.data() + at), length);
	at += length;
	return true;
}

} // namespace

struct DemoWriter::Impl {
	std::FILE* file = nullptr;
	z_stream z {};
	void put(const std::vector<uint8_t>& bytes, int flush)
	{
		z.next_in = const_cast<Bytef*>(bytes.data());
		z.avail_in = static_cast<uInt>(bytes.size());
		uint8_t out[4096]; // the wrapper's 4 KB buffers
		int result = Z_OK;
		do {
			z.next_out = out;
			z.avail_out = sizeof out;
			result = deflate(&z, flush);
			std::fwrite(out, 1, sizeof out - z.avail_out, file);
		} while (z.avail_out == 0 || (flush == Z_FINISH && result == Z_OK));
	}
};

DemoWriter::DemoWriter() : m_impl(std::make_unique<Impl>()) {}

DemoWriter::~DemoWriter()
{
	if (isOpen()) {
		close(0);
	}
}

bool DemoWriter::isOpen() const { return m_impl->file != nullptr; }

bool DemoWriter::open(const std::string& path, const DemoHeader& header)
{
	if (isOpen()) {
		return false;
	}
	if (std::FILE* exists = std::fopen(path.c_str(), "rb")) {
		std::fclose(exists);
		return false; // not over another demo
	}
	m_impl->file = std::fopen(path.c_str(), "wb");
	if (!m_impl->file) {
		return false;
	}
	m_impl->z = {};
	deflateInit(&m_impl->z, 3);
	std::vector<uint8_t> bytes;
	putU32(bytes, header.version);
	putU32(bytes, header.started ? header.started : static_cast<uint32_t>(std::time(nullptr)));
	putString(bytes, header.serverName);
	putString(bytes, header.level);
	putString(bytes, header.gameMode);
	m_impl->put(bytes, Z_NO_FLUSH);
	return true;
}

void DemoWriter::write(int32_t time, const std::vector<uint8_t>& payload)
{
	if (!isOpen()) {
		return;
	}
	std::vector<uint8_t> bytes;
	putU32(bytes, static_cast<uint32_t>(time));
	putU32(bytes, static_cast<uint32_t>(payload.size()));
	bytes.insert(bytes.end(), payload.begin(), payload.end());
	m_impl->put(bytes, Z_NO_FLUSH);
}

void DemoWriter::marker(uint32_t value)
{
	if (!isOpen()) {
		return;
	}
	std::vector<uint8_t> bytes;
	putU32(bytes, static_cast<uint32_t>(-2));
	putU32(bytes, value);
	m_impl->put(bytes, Z_NO_FLUSH);
}

void DemoWriter::close(int32_t tick)
{
	if (!isOpen()) {
		return;
	}
	std::vector<uint8_t> bytes;
	putU32(bytes, static_cast<uint32_t>(tick));
	putU32(bytes, 0);
	m_impl->put(bytes, Z_FINISH);
	deflateEnd(&m_impl->z);
	std::fclose(m_impl->file);
	m_impl->file = nullptr;
}

bool parseDemo(const std::vector<uint8_t>& in, DemoHeader& header, std::vector<DemoRecord>& records)
{
	size_t at = 0;
	if (!getU32(in, at, header.version) || !getU32(in, at, header.started) || !getString(in, at, header.serverName)
		|| !getString(in, at, header.level) || !getString(in, at, header.gameMode)) {
		return false;
	}
	records.clear();
	while (at + 8 <= in.size()) {
		DemoRecord r;
		uint32_t time = 0, length = 0;
		getU32(in, at, time);
		getU32(in, at, length);
		r.time = static_cast<int32_t>(time);
		if (r.time == -2) {
			r.value = length;
			records.push_back(std::move(r));
			continue;
		}
		if (length > 0x40000 || at + length > in.size()) {
			break; // larger than the reader's buffer: the end (0x00516c80)
		}
		r.payload.assign(in.begin() + static_cast<long>(at), in.begin() + static_cast<long>(at + length));
		at += length;
		records.push_back(std::move(r));
	}
	return true;
}

namespace {

bool inflateFile(const std::string& path, std::vector<uint8_t>& inflated)
{
	std::FILE* file = std::fopen(path.c_str(), "rb");
	if (!file) {
		return false;
	}
	std::vector<uint8_t> packed;
	uint8_t buffer[4096];
	for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, file)) > 0;) {
		packed.insert(packed.end(), buffer, buffer + n);
	}
	std::fclose(file);
	z_stream z {};
	if (inflateInit(&z) != Z_OK) {
		return false;
	}
	z.next_in = packed.data();
	z.avail_in = static_cast<uInt>(packed.size());
	int result = Z_OK;
	while (result == Z_OK) {
		z.next_out = buffer;
		z.avail_out = sizeof buffer;
		result = inflate(&z, Z_NO_FLUSH);
		inflated.insert(inflated.end(), buffer, buffer + (sizeof buffer - z.avail_out));
		if (result == Z_BUF_ERROR && z.avail_in == 0) {
			break; // cut short: what there is
		}
	}
	inflateEnd(&z);
	return true;
}

void putFloat(std::vector<uint8_t>& out, float v)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &v, 4);
	putU32(out, bits);
}

bool getFloat(const std::vector<uint8_t>& in, size_t& at, float& v)
{
	uint32_t bits = 0;
	if (!getU32(in, at, bits)) {
		return false;
	}
	std::memcpy(&v, &bits, 4);
	return true;
}

} // namespace

bool readDemo(const std::string& path, DemoHeader& header, std::vector<DemoRecord>& records)
{
	std::vector<uint8_t> inflated;
	return inflateFile(path, inflated) && parseDemo(inflated, header, records);
}

std::vector<uint8_t> encodeCameraKeyframes(const std::vector<CameraKeyframe>& frames)
{
	std::vector<uint8_t> out;
	for (const CameraKeyframe& f : frames) {
		putU32(out, f.tick);
		putU32(out, f.mode);
		out.push_back(f.attached ? 1 : 0);
		out.push_back(f.override ? 1 : 0);
		if (f.free()) {
			for (float v : f.rotation) {
				putFloat(out, v);
			}
			for (float v : f.position) {
				putFloat(out, v);
			}
		} else {
			putU32(out, f.followed);
			out.push_back(f.followFlag);
			for (float v : f.follow) {
				putFloat(out, v);
			}
		}
	}
	return out;
}

bool decodeCameraKeyframes(const std::vector<uint8_t>& in, std::vector<CameraKeyframe>& frames)
{
	frames.clear();
	size_t at = 0;
	while (at < in.size()) {
		CameraKeyframe f;
		if (!getU32(in, at, f.tick) || !getU32(in, at, f.mode) || at + 2 > in.size()) {
			return false;
		}
		f.attached = in[at++] != 0;
		f.override = in[at++] != 0;
		if (f.free()) {
			for (float& v : f.rotation) {
				if (!getFloat(in, at, v)) {
					return false;
				}
			}
			for (float& v : f.position) {
				if (!getFloat(in, at, v)) {
					return false;
				}
			}
			const float d2 = f.position[0] * f.position[0] + f.position[1] * f.position[1] + f.position[2] * f.position[2];
			if (d2 > 1e10f) {
				f.position[0] = f.position[1] = f.position[2] = 0;
			}
			if (f.override) {
				f.mode = 0;
			}
		} else {
			if (!getU32(in, at, f.followed) || at + 1 > in.size()) {
				return false;
			}
			f.followFlag = in[at++];
			for (float& v : f.follow) {
				if (!getFloat(in, at, v)) {
					return false;
				}
			}
		}
		if (f.mode != 0 && f.mode != 1) {
			f.mode = 0; // only exactly 1 follows
		}
		frames.push_back(f);
	}
	return true;
}

bool writeCameraFile(const std::string& path, const std::vector<CameraKeyframe>& frames)
{
	const std::vector<uint8_t> raw = encodeCameraKeyframes(frames);
	uLongf size = compressBound(static_cast<uLong>(raw.size()));
	std::vector<uint8_t> packed(size);
	if (compress2(packed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 3) != Z_OK) {
		return false;
	}
	std::FILE* file = std::fopen(path.c_str(), "wb");
	if (!file) {
		return false;
	}
	std::fwrite(packed.data(), 1, size, file);
	std::fclose(file);
	return true;
}

bool readCameraFile(const std::string& path, std::vector<CameraKeyframe>& frames)
{
	std::vector<uint8_t> inflated;
	return inflateFile(path, inflated) && decodeCameraKeyframes(inflated, frames);
}

int DemoPlayer::next(std::vector<uint8_t>& payload)
{
	while (m_at < m_records.size()) {
		const DemoRecord& r = m_records[m_at];
		if (r.time == -2) {
			++m_at; // the marker (render mode's start)
			continue;
		}
		if (r.time != -1) {
			if (!m_started || r.time > m_tick + 100) {
				m_started = true;
				m_tick = r.time;
			}
			if (m_tick < r.time) {
				return 0;
			}
		}
		++m_at;
		if (r.payload.empty()) {
			continue; // the end record
		}
		payload = r.payload;
		return 1;
	}
	return -1;
}

std::string demoPath(const std::string& name)
{
	std::string path = name;
	for (char& c : path) {
		if (c == '\\') {
			c = '/';
		}
	}
	const size_t slash = path.rfind('/');
	if (path.find('.', slash == std::string::npos ? 0 : slash) == std::string::npos) {
		path = "Demos/" + path + ".bf2142demo";
	}
	return path;
}

DemoRates demoRates(int quality)
{
	quality = quality < 2 ? 1 : std::min(quality, 10);
	const float f = static_cast<float>(quality - 1) * 0.11111111f;
	DemoRates r;
	r.packetsPerSecond = 4 - static_cast<int>(f * -16.0f);
	r.packetBytes = 0x104 - static_cast<int>(f * -3836.0f);
	r.maxUpdates = static_cast<int>(std::floor(static_cast<double>(f * 215.0f))) + 40;
	return r;
}

} // namespace dice::hfe::net
