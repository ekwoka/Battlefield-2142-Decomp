#include "Button.h"

namespace dice::hfe::ui::swf {

namespace {

bool fail(std::string* error, const char* message)
{
	if (error) {
		*error = message;
	}
	return false;
}

} // namespace

bool parseDefineButton2(const uint8_t* body, size_t size, size_t bodyOffset, Button& out, std::string* error)
{
	BitReader in(body, size);
	out = Button {};
	out.id = in.u16();
	out.trackAsMenu = in.u8() & 1;
	// Counted from the field itself; 0 when there are no actions.
	const size_t actionsAt = in.position();
	const uint16_t actionOffset = in.u16();
	while (in.ok()) {
		const uint8_t flags = in.u8();
		if (flags == 0) {
			break; // CharacterEndFlag
		}
		if (flags & 0x10) {
			return fail(error, "button record with filters (SWF 8)");
		}
		ButtonRecord r;
		r.states = flags & 0x0f;
		r.characterId = in.u16();
		r.depth = in.u16();
		r.matrix = in.matrix();
		r.colorTransform = in.cxform(true);
		if (flags & 0x20) {
			in.u8(); // blend mode (SWF 8)
		}
		out.records.push_back(r);
	}
	if (!in.ok()) {
		return fail(error, "truncated button records");
	}
	if (actionOffset == 0) {
		return true;
	}
	size_t at = actionsAt + actionOffset;
	while (at + 4 <= size) {
		const size_t next = static_cast<size_t>(body[at]) | static_cast<size_t>(body[at + 1]) << 8;
		ButtonCondAction a;
		a.conditions = static_cast<uint16_t>(body[at + 2] | (body[at + 3] & 1) << 8);
		a.keyCode = body[at + 3] >> 1;
		a.actionOffset = bodyOffset + at + 4;
		const size_t end = next == 0 ? size : at + next;
		if (next != 0 && (next < 4 || end > size)) {
			return fail(error, "button condition action past the tag");
		}
		a.actionLength = end - (at + 4);
		out.actions.push_back(a);
		if (next == 0) {
			return true;
		}
		at = end;
	}
	return fail(error, "truncated button condition actions");
}

} // namespace dice::hfe::ui::swf
