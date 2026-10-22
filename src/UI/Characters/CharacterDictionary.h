// A movie's characters by id, decoded from its definition tags, and the
// names ExportAssets gives them (what attachMovie() looks up).
//
// Definitions live at the top level of the tag list; sprites only place
// them. A sprite is kept as its id, frame count and own tag list for the
// display-list layer to play. Bitmaps are decoded on load (DefineBits needs
// the JPEGTables tag that precedes it). Tags that fail to decode are
// skipped and noted in warnings(); the rest of the movie still loads.
#pragma once

#include "UI/Characters/Bitmap.h"
#include "UI/Characters/Button.h"
#include "UI/Characters/EditText.h"
#include "UI/Characters/Font.h"
#include "UI/Characters/Shape.h"
#include "UI/Swf.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace dice::hfe::ui::swf {

struct Sprite {
	uint16_t id = 0;
	uint16_t frameCount = 0;
	SwfTag tag; // the DefineSprite tag itself
	std::vector<SwfTag> tags; // its timeline, End included
};

using Character = std::variant<Shape, MorphShape, Bitmap, Font, EditText, Button, Sprite>;

// Definition tag codes handled here.
enum CharacterTagCode : uint16_t {
	kTagDefineShape = 2,
	kTagDefineBits = 6,
	kTagJpegTables = 8,
	kTagDefineButton2 = 34,
	kTagDefineBitsLossless = 20,
	kTagDefineBitsJpeg2 = 21,
	kTagDefineShape2 = 22,
	kTagDefineShape3 = 32,
	kTagDefineBitsJpeg3 = 35,
	kTagDefineBitsLossless2 = 36,
	kTagDefineEditText = 37,
	kTagDefineSprite = 39,
	kTagDefineMorphShape = 46,
	kTagDefineFont2 = 48,
	kTagExportAssets = 56,
};

class CharacterDictionary {
public:
	// Reads every definition in `movie` (parsed from `swf`). `swf` must
	// outlive any use of button action offsets. Returns false only when
	// nothing could be read.
	bool load(const std::vector<uint8_t>& swf, const SwfMovie& movie, std::string* error = nullptr);

	const Character* find(uint16_t id) const;
	const Shape* shape(uint16_t id) const { return get<Shape>(id); }
	const MorphShape* morphShape(uint16_t id) const { return get<MorphShape>(id); }
	const Bitmap* bitmap(uint16_t id) const { return get<Bitmap>(id); }
	const Font* font(uint16_t id) const { return get<Font>(id); }
	const EditText* editText(uint16_t id) const { return get<EditText>(id); }
	const Button* button(uint16_t id) const { return get<Button>(id); }
	const Sprite* sprite(uint16_t id) const { return get<Sprite>(id); }

	// The id exported as `name` (exact match), or 0.
	uint16_t exportedId(const std::string& name) const;

	const std::map<uint16_t, Character>& characters() const { return m_characters; }
	const std::unordered_map<std::string, uint16_t>& exports() const { return m_exports; }
	const std::vector<std::string>& warnings() const { return m_warnings; }

private:
	template <typename T>
	const T* get(uint16_t id) const
	{
		const Character* c = find(id);
		return c ? std::get_if<T>(c) : nullptr;
	}

	void warn(const SwfTag& tag, const std::string& message);

	std::map<uint16_t, Character> m_characters;
	std::unordered_map<std::string, uint16_t> m_exports;
	std::vector<std::string> m_warnings;
};

} // namespace dice::hfe::ui::swf
