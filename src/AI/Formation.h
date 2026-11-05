// A squad's formation (AIDLL.dll: the formation object, 0x88 bytes, made
// by the squad's constructor 0x100c4080 at squad +0x7c with 0x100f67d0,
// freed by 0x100f5f80). It has one slot per possible member (IAIGame
// slot 0x54, BF2142.exe 0x008c0360: 6), slot 0 being the leader's; the
// others stand at offsets from their parent slot, turned to the way the
// leader walks, and fall back on a trail of points the leader left behind
// when their own spot is off the mesh.
//
// The pure parts live here; the squad's update that drives them (0x100c4240
// -> 0x100f6dc0, 0x100f69a0) is BotManager's (src/Game/BotsSquad.cpp).
#pragma once

#include "AI/NavMesh.h"

#include <functional>
#include <vector>

namespace dice::hfe::ai {

// A slot (0x24 bytes, made by 0x100f67d0's loop).
struct FormationSlot {
	bool used = false;     // +0x00
	int member = -1;       // +0x04 the member's id (the player)
	Vec3 offset;           // +0x08 side, +0x0c up, +0x10 ahead: from the leader's spot
	int crumb = -1;        // +0x14 the trail point taken (-1 none)
	bool lagging = false;  // +0x18 a human member fell behind (0x100f4f30)
	float lagUntil = 0;    // +0x1c
	int parent = 0;        // +0x20 the slot it is placed from
};

// A trail point (0x10 bytes at +0x6c).
struct FormationCrumb {
	bool valid = false;
	Vec3 at;
};

struct Formation {
	static constexpr int kSlots = 6;
	std::vector<FormationSlot> slots = std::vector<FormationSlot>(kSlots);
	// +0x14: the shape (0 a line abreast; 1 a wedge, 2 and 3 echelons:
	// 0x100f58c0). Only the constructor and the update's third tick
	// (0x100f6dc0) write it, both 0: the shipped squads stand in a line.
	int shape = 0;
	float side = 1, up = 0, ahead = 0; // +0x18, +0x1c, +0x20: the shape's unit step
	bool together = false;             // +0x28: no human member lags (0x100f4f30)
	float speed = 5;                   // +0x2c: the slowest member's top speed x 0.9 (0x100f5120)
	float dirX = 0, dirZ = 1;          // +0x34, +0x38: the way the leader walks (0x100f51f0)
	bool moved = false;                // +0x3c (0x100f4c70)
	Vec3 movedFrom;                    // +0x40
	Vec3 centre;                       // +0x4c: the leader's spot
	bool humanLeads = false;           // +0x58 (0x100f52d0: a human's order)
	bool released = false;             // +0x59 (0x100f5320)
	int ticks = 0;                     // +0x5c
	double movedUntil = 0;             // +0x60
	std::vector<FormationCrumb> crumbs = std::vector<FormationCrumb>(kSlots); // +0x6c, newest first
	Vec3 lastCrumb;                    // +0x78
};

// The shape's step and the slots' offsets (0x100f58c0: shape 0 {1, 0, 0}
// with 0x100f53a0; 1 {0.8, 0, -0.6} with 0x100f54c0; 2 {0.6, 0, -0.8}
// and 3 {0.6, 0, 0.8} with 0x100f55f0). Slots 1 and 2 hang off slot 0,
// then each next pair off the pair before (3 off 1, 4 off 2, 5 off 3),
// alternately right and left; a used slot's offset is its parent's plus
// the step times 3 (its radius + its parent member's radius). The
// echelons put the left one's ahead component the other way. `radius`
// gives a member's AI radius (0x100bf8e0: its AI info's slot 0x28).
void layOutFormation(Formation& formation, const std::function<float(int member)>& radius);

// A slot's offset turned to the formation's way (0x100f5c70): the angle
// acos(dirZ), negative for dirX > 0, x' = cos a * side - sin a * ahead,
// z' = sin a * side + cos a * ahead.
Vec3 turnOffset(const Formation& formation, const Vec3& offset);

// The trail (0x100f4d30): once the leader's spot is 6 m from the last
// point dropped (36 m^2, the first spot taken as is), the points shift
// back and it goes in front.
void dropCrumb(Formation& formation);

// 0x100f4c70: with the leader's slot used, `force`, or the spot 2 m from
// where it was marked and the 0.2 s hold over, marks it moved (+0x3c) and
// keeps the spot; the flag is cleared otherwise.
void markMoved(Formation& formation, bool force, double now);

// Slot bookkeeping: whether `member` has a slot (0x100f5720), taking the
// first free one (0x100f5760), giving one up (0x100f64a0: the last used
// slot moves into the hole; a leader's hole takes the member nearest the
// order spot first, see BotsSquad.cpp), freeing every slot and handing
// them out in member order (0x100f57f0).
bool hasSlot(const Formation& formation, int member);
int slotOf(const Formation& formation, int member);
void takeSlot(Formation& formation, int member);
void clearSlot(FormationSlot& slot);

} // namespace dice::hfe::ai
