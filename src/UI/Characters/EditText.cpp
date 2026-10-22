#include "EditText.h"

namespace dice::hfe::ui::swf {

bool parseDefineEditText(const uint8_t* body, size_t size, EditText& out, std::string* error)
{
	BitReader in(body, size);
	out = EditText {};
	out.id = in.u16();
	out.bounds = in.rect();
	const uint8_t a = in.u8();
	const uint8_t b = in.u8();
	out.hasText = a & 0x80;
	out.wordWrap = a & 0x40;
	out.multiline = a & 0x20;
	out.password = a & 0x10;
	out.readOnly = a & 0x08;
	out.hasTextColor = a & 0x04;
	out.hasMaxLength = a & 0x02;
	out.hasFont = a & 0x01;
	// b & 0x80 is HasFontClass (SWF 9).
	out.autoSize = b & 0x40;
	out.hasLayout = b & 0x20;
	out.noSelect = b & 0x10;
	out.border = b & 0x08;
	out.wasStatic = b & 0x04;
	out.html = b & 0x02;
	out.useOutlines = b & 0x01;
	if (out.hasFont) {
		out.fontId = in.u16();
		out.fontHeight = in.u16();
	}
	if (out.hasTextColor) {
		out.textColor = in.rgba();
	}
	if (out.hasMaxLength) {
		out.maxLength = in.u16();
	}
	if (out.hasLayout) {
		out.align = static_cast<EditText::Align>(in.u8() & 3);
		out.leftMargin = in.u16();
		out.rightMargin = in.u16();
		out.indent = in.u16();
		out.leading = in.s16();
	}
	out.variableName = in.string();
	if (out.hasText) {
		out.initialText = in.string();
	}
	if (!in.ok()) {
		if (error) {
			*error = "truncated DefineEditText";
		}
		return false;
	}
	return true;
}

} // namespace dice::hfe::ui::swf
