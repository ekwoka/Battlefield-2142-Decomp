// The Flash menus' .dfm container and the SWF reader: synthetic movies
// round-trip through the XOR, and (with $BF2142_GAME_DIR) the shipped
// mainMenu.dfm decodes to a sane movie.
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Hud/HudFont.h"
#include "IO/File/FileManager.h"
#include "IO/File/ZipArchive.h"
#include "IO/Localization/Localization.h"
#include "TestMain.h"
#include "UI/Characters/CharacterDictionary.h"
#include "UI/Dfm.h"
#include "UI/Swf.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

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

// The menus' text in other languages. SwiffRender draws every edit text
// with an engine font (ui::Drawer::engineFont: the font's export name and
// round(height x 0.05) px, from Fonts/<language>/ first, 0x10185e90), never
// with the movie's own glyphs (gameswf's 0x100c06a0 layout, which skips a
// missing glyph with "missing glyph for char", is not reached): the
// embedded fonts of mainMenu.dfm hold Western glyph sets (no Cyrillic, no
// CJK). So the languages' menu strings need the engine fonts' glyphs:
// checked here for each font size the menu's edit texts use, but the 24 px
// titles' StandardTextBold_24, which every language ships Latin-only.
TEST(Dfm_MenuEngineFontsCoverLanguages)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	Console console(&files);
	bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
		[&files](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
	std::string out;
	console.run("ClientArchives.con", {}, out);
	console.run("ServerArchives.con", {}, out);
	std::vector<uint8_t> dfm, csv;
	CHECK(files.readFile("menu/External/FlashMenu/mainMenu.dfm", dfm) || files.readFile("External/FlashMenu/mainMenu.dfm", dfm));
	CHECK(files.readFile("Localization/strings.csv", csv));
	const auto swf = ui::decodeDfm(dfm);
	const auto movie = swf ? ui::parseSwf(*swf) : std::nullopt;
	ui::swf::CharacterDictionary dictionary;
	CHECK(movie && dictionary.load(*swf, *movie));
	// The edit texts' engine fonts (Drawer::engineFontName: the export of
	// the font, or of another font of the same name).
	std::map<std::string, std::string> byName;
	std::map<uint16_t, std::string> names;
	for (const auto& [name, id] : dictionary.exports()) {
		if (const ui::swf::Font* font = dictionary.font(id)) {
			names[id] = name;
			byName[font->name] = name;
		}
	}
	std::set<std::pair<std::string, int>> used;
	size_t cyrillic = 0, embedded = 0;
	for (const auto& [id, character] : dictionary.characters()) {
		if (const auto* font = std::get_if<ui::swf::Font>(&character)) {
			if (!names.count(id) && byName.count(font->name)) {
				names[id] = byName[font->name];
			}
			if (!font->glyphs.empty()) {
				++embedded;
				for (const ui::swf::FontGlyph& g : font->glyphs) {
					cyrillic += g.code >= 0x400 && g.code < 0x500 ? 1 : 0;
				}
			}
		}
	}
	for (const auto& [id, character] : dictionary.characters()) {
		if (const auto* field = std::get_if<ui::swf::EditText>(&character); field && names.count(field->fontId)) {
			used.insert({names[field->fontId], static_cast<int>(std::nearbyint(field->fontHeight * 0.05f))});
		}
	}
	std::fprintf(stderr, "  %zu engine fonts in use; %zu embedded fonts, %zu Cyrillic glyphs in them\n", used.size(), embedded, cyrillic);
	CHECK(!used.empty() && embedded > 0 && cyrillic == 0);
	const char* ids[] = {"OPTIONS_1_hud", "OPTIONS_1_killmessages", "HEADER_history", "HEADER_badges", "HEADER_deaths",
		"TEXT_singleplayer_easy", "TEXT_changesneedsrestart", "TEXT_accuracywithweapon"};
	for (const char* language : {"German", "Polish", "Russian", "Chinese_Simplified"}) {
		io::Localization strings;
		CHECK(strings.load(csv, language));
		std::set<uint32_t> codes;
		for (const char* id : ids) {
			const std::string text = strings.text(id);
			for (size_t at = 0; at < text.size();) {
				const uint32_t c = hud::nextCode(text, at);
				if (c > ' ') {
					codes.insert(c);
				}
			}
		}
		size_t fonts = 0, missing = 0;
		for (const auto& [name, pixels] : used) {
			const std::string key = name + "_" + std::to_string(pixels) + ".dif";
			for (const std::string& path : {"Fonts/" + std::string(language) + "/" + key, "Fonts/English/" + key, "Fonts/" + key}) {
				std::vector<uint8_t> data;
				if (!files.readFile(path, data)) {
					continue;
				}
				const auto font = hud::parseHudFont(std::string(data.begin(), data.end()));
				// StandardTextBold_24 (the big titles) is shipped with its 98
				// Latin glyphs in every language's folder, as in the original.
				if (font && name + "_" + std::to_string(pixels) != "StandardTextBold_24") {
					++fonts;
					for (const uint32_t c : codes) {
						missing += font->glyph(c) ? 0 : 1;
					}
				}
				break;
			}
		}
		std::fprintf(stderr, "  %s: %zu characters, %zu fonts, %zu glyphs missing\n", language, codes.size(), fonts, missing);
		CHECK(codes.size() > 10 && fonts > 0 && missing == 0);
	}
}
