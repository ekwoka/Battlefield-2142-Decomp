// DefineEditText (37): text fields, static or editable, plain or HTML.
//
// The text is drawn with font `fontId` (a DefineFont2, embedded only when
// useOutlines is set; otherwise the host's device font of that name) at
// `fontHeight` twips, inside `bounds` less the margins. `variableName` binds
// the field to an ActionScript variable ("_root.foo" or "foo" in the
// enclosing timeline). With `html` the initial text is Flash's HTML subset.
#pragma once

#include "UI/Characters/BitReader.h"

#include <cstdint>
#include <string>

namespace dice::hfe::ui::swf {

struct EditText {
	enum class Align : uint8_t { Left = 0, Right = 1, Center = 2, Justify = 3 };

	uint16_t id = 0;
	Rect bounds;

	bool hasText = false;
	bool wordWrap = false;
	bool multiline = false;
	bool password = false;
	bool readOnly = false;
	bool hasTextColor = false;
	bool hasMaxLength = false;
	bool hasFont = false;
	bool autoSize = false;
	bool hasLayout = false;
	bool noSelect = false;
	bool border = false;
	bool wasStatic = false;
	bool html = false;
	bool useOutlines = false;

	uint16_t fontId = 0;
	uint16_t fontHeight = 0; // twips
	Rgba textColor {0, 0, 0, 255};
	uint16_t maxLength = 0;
	Align align = Align::Left;
	uint16_t leftMargin = 0, rightMargin = 0, indent = 0; // twips
	int16_t leading = 0; // twips
	std::string variableName;
	std::string initialText; // UTF-8
};

bool parseDefineEditText(const uint8_t* body, size_t size, EditText& out, std::string* error = nullptr);

} // namespace dice::hfe::ui::swf
