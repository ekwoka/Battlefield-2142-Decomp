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
	// A text field: glyph triangles in its colour.
	uint16_t fieldId = 0;
	for (const auto& [id, character] : dictionary.characters()) {
		if (const auto* field = std::get_if<swf::EditText>(&character); field && drawer.outlineFont(field->fontId)) {
			fieldId = id;
			break;
		}
	}
	CHECK(fieldId != 0);
	std::vector<ui::MenuBatch> text;
	drawer.editText(fieldId, "Battlefield 2142", {}, {}, text);
	CHECK(text.size() == 1 && text[0].vertices.size() > 30);

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
