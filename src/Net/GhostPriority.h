// The server's per-client ghost scheduling (BF2142.exe's GhostManager, one
// per ServerConnection at +0x2c: constructor 0x00582a50, vtable 0x0096531c;
// GhostManager.cpp, 0x00581e80).
//
// Each client keeps a state per ghost it has (0x54 bytes: the ghost's slot,
// 12 dirty masks, +0x40 always, +0x44 priority, +0x48 accumulated, +0x50
// sendable, a status: 1 in scope, 3 out of scope):
//
// - Scoring (0x0057dc10, at most every 0.1 s a client): every state not
//   always falls out of scope with priority 10000 (0x00581d40; always ones
//   255). Then each object tree in range is scored once by its root
//   (ghostScore, 0x0057d300) and each ghost of it gets (score + 0.3 for a
//   projectile) x its networkable's base priority (0x0057d560), back in
//   scope; each living teammate's soldier or vehicle gets at least 0.03.
//   A ghost new to the client starts at 10000 with every mask set
//   (0x00581e80), so it goes out first and whole.
// - Queueing (0x005823c0, each packet): every state out of scope or with
//   something to send adds its priority to its accumulated value and joins
//   a max-heap on it (0x0057ee60); a written one starts again from 0. An
//   in-scope state with nothing to send neither queues nor accumulates.
// - Writing (0x00582cd0): at most 40 entries a packet (gm +0x213c, 0x28);
//   one that doesn't fit is skipped (it keeps its accumulated value) and
//   the next tried, until under 24 bits are left. An out-of-scope state
//   writes kind 3 (2 + 16 bits) and goes once written (0x00581dc0); the
//   client hides that object until a kind-1 update brings it back.
// - A lost packet (0x005830e0) dirties again what it carried that no later
//   packet carried, at priority 10000.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace dice::hfe::net {

struct GhostVec3 {
	float x = 0, y = 0, z = 0;
};

// The client's eye for scoring (0x0057dc10: the player's camera matrix).
struct GhostEye {
	GhostVec3 position;
	GhostVec3 forward;   // unit
	float fov = 1.1f;    // the player's field of view (vt+0xa0), radians
	float radius = 850;  // max(GSViewDistance, 500)
};

// The scope radius (0x004b02a0): GSViewDistance (850 by default,
// 0x004cc8d0; GameLogic slot 46, 0x004cc870, clamps it to 20..3000) but at
// least 500.
float ghostScopeRadius(float viewDistance);

// A tree's score for a client (0x0057d300): 0 at or past the radius (for a
// projectile whose vt+0x108 holds, LargerProjectileRadius, 1000), else
// min(1, |v|^2 x 0.000625) + (dir . forward >= fov x 0.5 x 4/3 ? 1 : 0; the
// original compares that angle with a cosine) + (1 - d / radius); a
// distance under FLT_EPSILON counts as 0.001.
float ghostScore(const GhostEye& eye, const GhostVec3& position, const GhostVec3& velocity, bool largeProjectile = false);

constexpr float kGhostNewPriority = 10000; // 0x00581e80, 0x00581d40
constexpr float kGhostAlwaysPriority = 255; // 0x437f0000
constexpr float kGhostProjectileBonus = 0.3f; // 0x0057d560
constexpr float kGhostTeammatePriority = 0.03f; // 0x3cf5c28f
constexpr size_t kGhostEntriesPerPacket = 40; // gm +0x213c
constexpr size_t kGhostMinimumBitsLeft = 24;  // 0x18
constexpr float kGhostRescoreSeconds = 0.1f;  // 0x00920bd8

class GhostScheduler {
public:
	enum class Status { InScope = 1, OutOfScope = 3 };
	struct State {
		Status status = Status::InScope;
		bool always = false;
		float priority = kGhostNewPriority;
		float accumulated = 0;
		bool dirty = true; // the rewrite's stand-in for the 12 masks
	};
	// 0x00581e80: p < 0 makes an always state (every mask set); keepMax
	// keeps an existing priority under 10000 that is at least p. A ghost
	// new to the client starts at 10000 whatever p is.
	void setPriority(uint16_t ghost, float p, bool keepMax);
	// 0x00581d40: everything not always out of scope at 10000, always ones
	// at 255.
	void beginRescore();
	void markDirty(uint16_t ghost, bool urgent = false); // urgent: a loss, priority 10000
	void forget(uint16_t ghost);                         // the object went (DestroyObject)
	void clear() { m_states.clear(); }
	// 0x005823c0: the ghosts to try this packet, highest accumulated first.
	std::vector<uint16_t> queue();
	// After writing: an in-scope state is clean and starts again; an
	// out-of-scope one is gone (its kind 3 went).
	void written(uint16_t ghost);
	const State* state(uint16_t ghost) const;
	bool has(uint16_t ghost) const { return m_states.count(ghost) != 0; }
	size_t size() const { return m_states.size(); }

private:
	std::map<uint16_t, State> m_states;
};

} // namespace dice::hfe::net
