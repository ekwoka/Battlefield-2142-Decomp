// DefineButton2 (34): a button's look per state and the ActionScript run on
// state transitions.
//
// Each record places a character at a depth in some of the four states
// (up, over, down, and hit, the invisible shape that is tested against the
// mouse). Condition actions fire on transitions between idle (mouse
// outside), over-up, over-down and out-down (pressed, then dragged out), or
// on a key press. The bytecode stays in the movie: actionOffset/Length
// locate it (ActionEnd included) in the buffer the tag came from.
#pragma once

#include "UI/Characters/BitReader.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dice::hfe::ui::swf {

enum ButtonState : uint8_t {
	kButtonUp = 0x01,
	kButtonOver = 0x02,
	kButtonDown = 0x04,
	kButtonHitTest = 0x08,
};

struct ButtonRecord {
	uint8_t states = 0; // ButtonState bits
	uint16_t characterId = 0;
	uint16_t depth = 0;
	Matrix matrix;
	ColorTransform colorTransform;
};

// Transition bits of ButtonCondAction::conditions.
enum ButtonCondition : uint16_t {
	kCondIdleToOverUp = 0x0001, // roll over
	kCondOverUpToIdle = 0x0002, // roll out
	kCondOverUpToOverDown = 0x0004, // press
	kCondOverDownToOverUp = 0x0008, // release
	kCondOverDownToOutDown = 0x0010, // drag out
	kCondOutDownToOverDown = 0x0020, // drag over
	kCondOutDownToIdle = 0x0040, // release outside
	kCondIdleToOverDown = 0x0080, // drag over (menu buttons)
	kCondOverDownToIdle = 0x0100, // drag out (menu buttons)
};

struct ButtonCondAction {
	uint16_t conditions = 0; // ButtonCondition bits
	uint8_t keyCode = 0; // 0 none; 1-19 special keys, 32-126 ASCII
	size_t actionOffset = 0; // into the movie buffer
	size_t actionLength = 0;
};

struct Button {
	uint16_t id = 0;
	bool trackAsMenu = false;
	std::vector<ButtonRecord> records;
	std::vector<ButtonCondAction> actions;
};

// `bodyOffset` is where `body` sits in the movie buffer, for the action
// offsets.
bool parseDefineButton2(const uint8_t* body, size_t size, size_t bodyOffset, Button& out, std::string* error = nullptr);

} // namespace dice::hfe::ui::swf
