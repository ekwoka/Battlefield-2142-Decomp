// The Flash menus' .dfm container and the SWF reader: synthetic movies
// round-trip through the XOR, and (with $BF2142_GAME_DIR) the shipped
// mainMenu.dfm decodes to a sane movie.
#include "IO/File/ZipArchive.h"
#include "TestMain.h"
#include "UI/Dfm.h"
#include "UI/Swf.h"

#include <cstdlib>

using namespace dice::hfe;

namespace {

void put16(std::vector<uint8_t>& out, uint16_t v)
{
	out.push_back(static_cast<uint8_t>(v));
	out.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t>& out, uint32_t v)
{
	put16(out, static_cast<uint16_t>(v));
	put16(out, static_cast<uint16_t>(v >> 16));
}

void putTag(std::vector<uint8_t>& out, uint16_t code, const std::vector<uint8_t>& body, bool longForm = false)
{
	if (longForm || body.size() >= 0x3f) {
		put16(out, static_cast<uint16_t>(code << 6 | 0x3f));
		put32(out, static_cast<uint32_t>(body.size()));
	} else {
		put16(out, static_cast<uint16_t>(code << 6 | body.size()));
	}
	out.insert(out.end(), body.begin(), body.end());
}

// An 800x600 stage at 30 fps, two frames: SetBackgroundColor, a DoAction,
// a DefineSprite holding a ShowFrame, ShowFrame twice and End.
std::vector<uint8_t> makeMovie()
{
	std::vector<uint8_t> swf {'F', 'W', 'S', 7, 0, 0, 0, 0};
	// RECT with 15-bit fields: 0, 16000, 0, 12000 twips.
	std::vector<bool> bits;
	const auto putBits = [&bits](uint32_t value, int count) {
		for (int i = count - 1; i >= 0; --i) {
			bits.push_back((value >> i) & 1);
		}
	};
	putBits(15, 5);
	for (uint32_t field : {0u, 16000u, 0u, 12000u}) {
		putBits(field, 15);
	}
	for (size_t i = 0; i < bits.size(); i += 8) { // 65 bits -> 9 bytes
		uint8_t byte = 0;
		for (size_t j = 0; j < 8; ++j) {
			byte = static_cast<uint8_t>(byte << 1 | (i + j < bits.size() && bits[i + j]));
		}
		swf.push_back(byte);
	}
	put16(swf, 30 << 8);
	put16(swf, 2);
	putTag(swf, 9, {0x10, 0x20, 0x30});
	putTag(swf, ui::kSwfDoAction, {0x07, 0x00}, true); // ActionStop, ActionEnd
	std::vector<uint8_t> sprite;
	put16(sprite, 1);
	put16(sprite, 1);
	putTag(sprite, ui::kSwfShowFrame, {});
	putTag(sprite, ui::kSwfEnd, {});
	putTag(swf, ui::kSwfDefineSprite, sprite);
	putTag(swf, ui::kSwfShowFrame, {});
	putTag(swf, ui::kSwfShowFrame, {});
	putTag(swf, ui::kSwfEnd, {});
	const uint32_t length = static_cast<uint32_t>(swf.size());
	for (int i = 0; i < 4; ++i) {
		swf[4 + i] = static_cast<uint8_t>(length >> (8 * i));
	}
	return swf;
}

// The same movie as "CWS", using stored (uncompressed) deflate blocks so
// the test needs no compressor.
std::vector<uint8_t> makeCompressed(const std::vector<uint8_t>& fws)
{
	std::vector<uint8_t> cws(fws.begin(), fws.begin() + 8);
	cws[0] = 'C';
	cws.push_back(0x78);
	cws.push_back(0x01);
	const size_t size = fws.size() - 8;
	cws.push_back(0x01); // final stored block
	put16(cws, static_cast<uint16_t>(size));
	put16(cws, static_cast<uint16_t>(~size));
	cws.insert(cws.end(), fws.begin() + 8, fws.end());
	uint32_t a = 1, b = 0;
	for (size_t i = 8; i < fws.size(); ++i) {
		a = (a + fws[i]) % 65521;
		b = (b + a) % 65521;
	}
	const uint32_t adler = b << 16 | a;
	for (int shift = 24; shift >= 0; shift -= 8) {
		cws.push_back(static_cast<uint8_t>(adler >> shift));
	}
	return cws;
}

} // namespace

TEST(Dfm_KeyMatchesShippedHeader)
{
	// Both shipped menus start "4d fa a3 0a": "FWS" and version 7 under the key.
	std::vector<uint8_t> head {0x4d, 0xfa, 0xa3, 0x0a};
	ui::xorDfm(head.data(), head.size());
	CHECK(head == (std::vector<uint8_t> {'F', 'W', 'S', 7}));
	// The key follows the file position, not the buffer.
	std::vector<uint8_t> tail {0xfa, 0xa3};
	ui::xorDfm(tail.data(), tail.size(), 1);
	CHECK(tail == (std::vector<uint8_t> {'W', 'S'}));
}

TEST(Dfm_RoundTrip)
{
	const std::vector<uint8_t> swf = makeMovie();
	const auto dfm = ui::encodeDfm(swf);
	CHECK(dfm.has_value());
	CHECK(dfm && *dfm != swf);
	const auto back = dfm ? ui::decodeDfm(*dfm) : std::nullopt;
	CHECK(back && *back == swf);

	std::string error;
	CHECK(!ui::decodeDfm(swf, &error));
	CHECK(!error.empty());
	CHECK(!ui::encodeDfm({1, 2, 3}));
}

TEST(Dfm_CompressedMovieInflates)
{
	const std::vector<uint8_t> swf = makeMovie();
	const auto dfm = ui::encodeDfm(makeCompressed(swf));
	std::string error;
	const auto back = dfm ? ui::decodeDfm(*dfm, &error) : std::nullopt;
	CHECK(back && *back == swf);
	CHECK(error.empty());
}

TEST(Swf_ParsesHeaderAndTags)
{
	const std::vector<uint8_t> swf = makeMovie();
	const auto movie = ui::parseSwf(swf);
	CHECK(movie.has_value());
	if (!movie) {
		return;
	}
	CHECK(movie->header.version == 7);
	CHECK(movie->header.fileLength == swf.size());
	CHECK(movie->header.widthTwips() == 16000 && movie->header.heightTwips() == 12000);
	CHECK(movie->header.framesPerSecond() == 30.0);
	CHECK(movie->header.frameCount == 2);
	CHECK(movie->tags.size() == 6);
	if (movie->tags.size() == 6) {
		CHECK(movie->tags[0].code == 9 && movie->tags[0].length == 3);
		CHECK(movie->tags[1].code == ui::kSwfDoAction && movie->tags[1].length == 2);
		CHECK(swf[movie->tags[1].offset] == 0x07);
		CHECK(movie->tags.back().code == ui::kSwfEnd);
		std::vector<ui::SwfTag> inner;
		CHECK(ui::readSpriteTags(swf, movie->tags[2], inner));
		CHECK(inner.size() == 2 && inner[0].code == ui::kSwfShowFrame);
	}
	CHECK(std::string(ui::swfTagName(39)) == "DefineSprite");
	CHECK(std::string(ui::swfTagName(1000)) == "Unknown");

	std::vector<uint8_t> truncated(swf.begin(), swf.end() - 3);
	truncated[4] = static_cast<uint8_t>(truncated.size());
	CHECK(!ui::parseSwf(truncated));
}

TEST(Dfm_ShippedMainMenu)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	const auto zip = io::ZipArchive::open(std::string(gameDir) + "/mods/bf2142/Menu_server.zip");
	CHECK(zip != nullptr);
	if (!zip) {
		return;
	}
	std::vector<uint8_t> dfm;
	CHECK(zip->read("External/FlashMenu/mainMenu.dfm", dfm));
	std::string error;
	const auto swf = ui::decodeDfm(dfm, &error);
	CHECK(swf.has_value());
	if (!swf) {
		std::fprintf(stderr, "mainMenu.dfm: %s\n", error.c_str());
		return;
	}
	const auto movie = ui::parseSwf(*swf, &error);
	CHECK(movie.has_value());
	if (!movie) {
		return;
	}
	CHECK(movie->header.version == 7);
	CHECK(movie->header.fileLength == swf->size());
	CHECK(movie->header.widthTwips() > 0 && movie->header.heightTwips() > 0);
	CHECK(movie->header.frameRate > 0);
	CHECK(movie->tags.size() > 100);
	CHECK(!movie->tags.empty() && movie->tags.back().code == ui::kSwfEnd);
}
