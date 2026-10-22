// Flash masks as the game drew them: with the stencil buffer, through
// gameswf's render handler (SwiffPlayer.dll calls begin_submit_mask,
// end_submit_mask and disable_mask at 0x100c6ce0, 0x100c6d00 and
// 0x100c6d20; RendDX9.dll implements them at 0x10118920, 0x10118a10 and
// 0x10118aa0):
//
//   begin_submit_mask  at level 0 clear the stencil to 0; level += 1;
//                      colour writes off; stencil test GREATER than the
//                      reference (the level): pass INCR, fail REPLACE
//   end_submit_mask    colour writes on; draw only where the stencil
//                      EQUALs the level; keep it
//   disable_mask       stencil test off; level -= 1
//
// What a mask draws (its fills and strokes, any alpha) marks the stencil;
// what follows is cut to the pixels whose value equals the level. Here the
// stencil is kept as disjoint convex polygons on the stage, each with its
// value (the rest is 0), and the triangles drawn while testing are cut to
// the polygons of the level's value, so the result is the stencil's
// without a depth buffer. Drawing goes into one batch list; the state
// applies to the batches added since the last change (flush()).
#pragma once

#include "UI/MenuDraw.h"

#include <cstddef>
#include <vector>

namespace dice::hfe::ui {

class Stencil {
public:
	struct Point {
		float x, y;
	};
	struct Piece {
		std::vector<Point> polygon; // convex, stage pixels
		int value = 0;
	};

	// Frame start: no mask, the stencil cleared.
	void reset();
	void beginSubmit(std::vector<MenuBatch>& out);
	void endSubmit(std::vector<MenuBatch>& out);
	void disable(std::vector<MenuBatch>& out);
	// Applies the current state to the batches added since the last change.
	void flush(std::vector<MenuBatch>& out);

	int level() const { return m_level; }
	bool submitting() const { return m_mode == Mode::Submit; }
	const std::vector<Piece>& pieces() const { return m_pieces; }

private:
	enum class Mode { Off, Submit, Test };
	void submitTriangle(const Point t[3]);

	Mode m_mode = Mode::Off;
	int m_level = 0;
	size_t m_done = 0; // batches before this have had their state applied
	std::vector<Piece> m_pieces;
};

} // namespace dice::hfe::ui
