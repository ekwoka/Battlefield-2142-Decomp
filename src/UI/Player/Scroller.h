// The credits roll (SwiffHost's scroller): text lines that RendDX9's
// SwiffRender draws in place of the clip whose _isScroller is set
// (gameswf 0x100dfe77 calls 0x100c6ee0, then the host's SwiffRender slot
// 0x110, 0x10117910). credits.con fills it through the console:
//   swiffHost.setScrollFadeSpeed 200      (+0xe08; read by nothing that draws)
//   swiffHost.setScrollSpeed 800          (+0xe0c: twips a second, 500..4000)
//   swiffHost.setScrollerRect 200 195 1000 648   (+0xe1c: left top right bottom)
//   swiffHost.addCredit "CREDITS_DICE_producer" 1   (+0xd9c: text, size)
// and the credits page turns it on with dice.bf2.Logic.enableScroller
// (SwiffPlayer 0x1003be40, SwiffHost 0x0046c2f0: on, from the first line).
//
// Each line is built once (0x101774b0): its text localized (a missing key
// stays as it is, names pass), in StandardTextBold_15 (size 2),
// StandardTextBold_13 (size 1) or DynamicText_11 (else), centred in a box
// W + W * 175 / 1024 wide (W the screen's width, so at x 599.5 of the
// 1024-wide stage). Each frame (0x10117910, 0x10177290): 40 slots, the
// first at y 15360 twips (768 px, the stage's bottom) and each next 400
// twips (20 px) lower, whatever its size, empty lines too; drawn opaque
// white, cut to the rectangle (the D3D scissor: no fade at the edges),
// then moved up by the speed times the frame's time (here as the
// player's time passes, the same each frame). A slot above -400
// twips leaves, and a new one takes the next line 400 twips below the
// last; after the last line the first comes again.
#pragma once

#include "UI/MenuDraw.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dice::hfe::ui {

class Drawer;

class CreditsScroller {
public:
	struct Credit {
		std::string text;
		int size = 0;
	};
	void addCredit(const std::string& text, int size);
	void clearCredits();
	const std::vector<Credit>& credits() const { return m_credits; }
	// 0x0046c2a0: clamped to 500..4000.
	void setSpeed(float twipsPerSecond);
	float speed() const { return m_speed; }
	void setFadeSpeed(int value) { m_fadeSpeed = value; }
	int fadeSpeed() const { return m_fadeSpeed; }
	void setRect(int left, int top, int right, int bottom);
	// 0x0046c2f0: on or off; either way from the first line again.
	void enable(bool on);
	bool enabled() const { return m_enabled; }

	// Time passing (the player's advance): the lines move up.
	void addTime(double seconds);
	// The roll's batches (stage pixels), when on. `localize` gives a key's
	// text (the key itself when missing).
	void draw(Drawer& drawer, const std::function<std::string(const std::string&)>& localize, std::vector<MenuBatch>& out);

	// The slots' lines and heights (twips), for tests.
	struct Slot {
		size_t line = 0;
		float y = 0;
	};
	const std::vector<Slot>& slots() const { return m_slots; }

private:
	size_t nextLine();
	std::vector<Credit> m_credits;
	struct Line {
		std::vector<uint32_t> codes;
		int size = 0;
	};
	std::vector<Line> m_lines; // built at the first draw
	bool m_built = false;
	float m_speed = 500;
	int m_fadeSpeed = 1;
	int m_rect[4] = {0, 0, 1024, 768};
	bool m_enabled = false;
	bool m_reset = false;
	size_t m_index = 0;
	std::vector<Slot> m_slots;
};

} // namespace dice::hfe::ui
