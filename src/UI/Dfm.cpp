#include "Dfm.h"

#include <algorithm>

#include <zlib.h>

namespace dice::hfe::ui {

namespace {

constexpr size_t kSwfHeaderSize = 8; // signature, version, file length

void setError(std::string* error, const char* message)
{
	if (error) {
		*error = message;
	}
}

bool hasSignature(const std::vector<uint8_t>& data, char first)
{
	return data.size() >= kSwfHeaderSize && data[0] == static_cast<uint8_t>(first) && data[1] == 'W' && data[2] == 'S';
}

uint32_t get32le(const uint8_t* p)
{
	return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
		static_cast<uint32_t>(p[3]) << 24;
}

} // namespace

void xorDfm(uint8_t* data, size_t size, uint64_t position)
{
	// 0x10007e80 indexes the key by the stream position before the read
	// (the "& 0x80000003" there is a signed % 4), so chunked reads line up.
	for (size_t i = 0; i < size; ++i) {
		data[i] ^= kDfmKey[(position + i) & 3];
	}
}

std::optional<std::vector<uint8_t>> uncompressSwf(const std::vector<uint8_t>& swf, std::string* error)
{
	if (hasSignature(swf, 'F')) {
		return swf;
	}
	if (!hasSignature(swf, 'C')) {
		setError(error, "not a SWF (no FWS/CWS signature)");
		return std::nullopt;
	}
	// The header's length counts the uncompressed movie, header included;
	// zlib data follows the eight header bytes.
	const uint32_t length = get32le(swf.data() + 4);
	if (length < kSwfHeaderSize) {
		setError(error, "SWF length smaller than its header");
		return std::nullopt;
	}
	std::vector<uint8_t> out(length);
	std::copy(swf.begin(), swf.begin() + kSwfHeaderSize, out.begin());
	out[0] = 'F';

	z_stream z {};
	if (inflateInit(&z) != Z_OK) {
		setError(error, "zlib init failed");
		return std::nullopt;
	}
	z.next_in = const_cast<Bytef*>(swf.data() + kSwfHeaderSize);
	z.avail_in = static_cast<uInt>(swf.size() - kSwfHeaderSize);
	z.next_out = out.data() + kSwfHeaderSize;
	z.avail_out = static_cast<uInt>(length - kSwfHeaderSize);
	const int status = inflate(&z, Z_FINISH);
	const size_t produced = kSwfHeaderSize + z.total_out;
	inflateEnd(&z);
	if (status != Z_STREAM_END || produced != length) {
		setError(error, "CWS body does not inflate to the header's length");
		return std::nullopt;
	}
	return out;
}

std::optional<std::vector<uint8_t>> decodeDfm(const std::vector<uint8_t>& dfm, std::string* error)
{
	std::vector<uint8_t> swf = dfm;
	xorDfm(swf.data(), swf.size());
	if (!hasSignature(swf, 'F') && !hasSignature(swf, 'C')) {
		setError(error, "not a .dfm (decoded data has no FWS/CWS signature)");
		return std::nullopt;
	}
	if (swf[0] == 'C') {
		return uncompressSwf(swf, error);
	}
	if (get32le(swf.data() + 4) != swf.size()) {
		setError(error, "SWF length does not match the file size");
		return std::nullopt;
	}
	return swf;
}

std::optional<std::vector<uint8_t>> encodeDfm(const std::vector<uint8_t>& swf, std::string* error)
{
	if (!hasSignature(swf, 'F') && !hasSignature(swf, 'C')) {
		setError(error, "not a SWF (no FWS/CWS signature)");
		return std::nullopt;
	}
	std::vector<uint8_t> dfm = swf;
	xorDfm(dfm.data(), dfm.size());
	return dfm;
}

} // namespace dice::hfe::ui
