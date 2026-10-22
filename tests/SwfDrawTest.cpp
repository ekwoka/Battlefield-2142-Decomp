// Drawing SWF characters as menu batches.
#include "IO/File/ZipArchive.h"
#include "TestMain.h"
#include "UI/Dfm.h"
#include "UI/Player/DisplayList.h"
#include "UI/Player/Draw.h"
#include "UI/Swf.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace dice::hfe;
namespace swf = dice::hfe::ui::swf;

TEST(SwfDraw_Matrices)
{
	const swf::Matrix scale {2, 0, 0, 2, 0, 0};
	const swf::Matrix move {1, 0, 0, 1, 100, 50};
	const swf::Matrix both = ui::multiply(move, scale); // scale, then move
	const swf::Point p = both.apply({10, 10});
	CHECK(p.x == 120 && p.y == 70);
	const swf::Point back = ui::inverse(both).apply(p);
	CHECK(std::abs(back.x - 10) < 1e-4f && std::abs(back.y - 10) < 1e-4f);
	swf::ColorTransform half;
	half.mult[3] = 0.5f;
	swf::ColorTransform red;
	red.add[0] = 100;
	const swf::ColorTransform c = ui::combine(half, red);
	CHECK(c.mult[3] == 0.5f && c.add[0] == 100);
}

TEST(SwfDraw_MenuCharacters)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	auto zip = io::ZipArchive::open(std::string(gameDir) + "/mods/bf2142/Menu_server.zip");
	CHECK(zip != nullptr);
	if (!zip) {
		return;
	}
	std::vector<uint8_t> dfm;
	CHECK(zip->read("External/FlashMenu/mainMenu.dfm", dfm));
	const auto swfBytes = ui::decodeDfm(dfm);
	CHECK(swfBytes.has_value());
	const auto movie = ui::parseSwf(*swfBytes);
	CHECK(movie.has_value());
	swf::CharacterDictionary dictionary;
	CHECK(dictionary.load(*swfBytes, *movie));
	ui::Drawer drawer(dictionary);
	// Shape 32: a green box with a black outline, at 10 px from the corner.
	std::vector<ui::MenuBatch> batches;
	drawer.shape(32, {1, 0, 0, 1, 200, 200}, {}, batches);
	CHECK(!batches.empty());
	size_t vertices = 0;
	for (const auto& batch : batches) {
		vertices += batch.vertices.size();
		CHECK(batch.image && batch.vertices.size() % 3 == 0);
	}
	CHECK(vertices >= 6);
	// Every shape and bitmap fill draws without trouble.
	size_t drawn = 0;
	for (const auto& [id, character] : dictionary.characters()) {
		if (std::holds_alternative<swf::Shape>(character)) {
			std::vector<ui::MenuBatch> out;
			drawer.shape(id, {}, {}, out);
			drawn += out.empty() ? 0 : 1;
		}
	}
	CHECK(drawn > 200);
	// Text fields draw with the engine's bitmap fonts (Fonts_client.zip),
	// chosen by the export name of the font with the field's font's name.
	CHECK(drawer.engineFontName(1) == "StandardTextBold");
	CHECK(drawer.engineFontName(13) == "DynamicText");       // device "Helvetica" -> font 3's export
	CHECK(drawer.engineFontName(71) == "StandardText");      // "Helvetica Condensed" -> font 2's
	CHECK(drawer.engineFontName(149).empty());               // "TradeGothic LT BdCondTwenty": none, not drawn
	auto fonts = io::ZipArchive::open(std::string(gameDir) + "/mods/bf2142/Fonts_client.zip");
	CHECK(fonts != nullptr);
	if (fonts) {
		drawer.setFontReader([&fonts](const std::string& path, std::string& text) {
			std::vector<uint8_t> bytes;
			std::string inZip = path.substr(path.find('/') + 1); // the zip is mounted at Fonts/
			if (!fonts->read(inZip, bytes)) {
				return false;
			}
			text.assign(bytes.begin(), bytes.end());
			return true;
		});
		uint16_t fieldId = 0;
		for (const auto& [id, character] : dictionary.characters()) {
			const auto* field = std::get_if<swf::EditText>(&character);
			if (field && field->fontId == 13 && field->fontHeight == 220) {
				fieldId = id;
				break;
			}
		}
		CHECK(fieldId != 0);
		swf::EditText field = *dictionary.editText(fieldId);
		std::vector<ui::MenuBatch> text;
		drawer.editText(field, "Battlefield 2142", {}, {}, text);
		CHECK(text.size() == 1 && text[0].texture == "Fonts/DynamicText_11.dds" && text[0].vertices.size() >= 6 * 13);
		// Word wrap: lines break at the last space before the width (less
		// the right margin and 2 pixels), dropping it; "|L" and '\r' break too.
		field.wordWrap = true;
		field.autoSize = true;
		field.bounds = {0, 60 * 20, 0, 20 * 20};
		field.rightMargin = 0;
		field.leading = 40;
		const auto layout = drawer.layoutText(field, "one two three|Lfour\rfive");
		CHECK(layout.font != nullptr && layout.lineStep == 260);
		std::vector<std::string> lines;
		for (const auto& line : layout.lines) {
			lines.emplace_back(line.codes.begin(), line.codes.end());
			CHECK(line.end < 58 || line.codes.size() == 1);
		}
		CHECK(lines.size() >= 4 && lines[0] == "one two" && lines[lines.size() - 2] == "four" && lines.back() == "five");
		// The field's textHeight with autoSize counts every line.
		CHECK(layout.textHeight == static_cast<int>(std::nearbyint((220 + 260 * (lines.size() - 1)) * 0.05f)));
		// Without autoSize, lines starting below the field are dropped.
		field.autoSize = false;
		CHECK(drawer.layoutText(field, "one two three|Lfour\rfive").lines.size() == 2);
		// Right alignment: the line ends at the field's right edge.
		field.wordWrap = false;
		field.align = swf::EditText::Align::Right;
		text.clear();
		drawer.editText(field, "four", {1, 0, 0, 1, 2000, 1000}, {}, text);
		float right = 0;
		for (const auto& v : text[0].vertices) {
			right = std::max(right, v.x);
		}
		const float end = drawer.layoutText(field, "four").lines[0].end;
		const float x = std::nearbyint((2000 + 40) * 0.05f + 0.5f) + 60 - end;
		CHECK(right <= x + end + 2 && right > x + end - 4); // a glyph may reach a pixel past its advance
		// A field whose font has no engine font draws nothing.
		field.fontId = 149;
		text.clear();
		drawer.editText(field, "four", {}, {}, text);
		CHECK(text.empty());
	}

	// The root timeline (no scripts yet): its frames play and draw.
	const ui::Timeline root = ui::Timeline::fromTags(movie->tags, *swfBytes);
	CHECK(root.frames.size() == movie->header.frameCount);
	ui::MovieClip clip(dictionary, *swfBytes, root, 0, nullptr);
	std::vector<std::pair<ui::MovieClip*, ui::ActionBlock>> actions;
	size_t mostBatches = 0;
	for (int frame = 0; frame < 40; ++frame) {
		clip.advance(actions);
		std::vector<ui::MenuBatch> drawn;
		clip.draw(drawer, {}, {}, drawn);
		mostBatches = std::max(mostBatches, drawn.size());
	}
	CHECK(clip.currentFrame() == 39 && !actions.empty() && mostBatches > 10);
	// Back to a label replays from the start.
	if (!root.labels.empty()) {
		const auto& [label, frame] = *root.labels.begin();
		clip.gotoFrame(clip.frameOfLabel(label), actions);
		CHECK(clip.currentFrame() == frame);
	}
}
