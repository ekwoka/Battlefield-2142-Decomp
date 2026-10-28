// How the server splits a replicated object into ghosts and the state of a
// ghost into groups (BF2142.exe 0x0067c690, cached per template), so a
// client can decode every group of an update, not only the root's.
//
// - The object tree is walked in pre-order (0x0067a270): every object that
//   is a PlayerControlObject (interface 0xc4c5: PlayerControlObject and
//   Parachute templates) starts a ghost of its own, numbered from the
//   CreateObject event's ghost id (its count says how many). A tree without
//   one is a single ghost.
// - A ghost's objects are its root's subtree without the subtrees of other
//   PlayerControlObjects (0x0067a5e0), again in pre-order, and only those
//   with a networkable (the template's networkableInfo, and a class that
//   makes one: object vtable slot 94).
// - They are grouped in order: an object is left out when it is a
//   projectile (interface 0x94c1) with a parent, or when its
//   NetworkableInfo forces a networkable id (setForceNetworkableId: hand
//   fire arms) and it is neither the root nor parentless. A non-unique
//   one (NetworkableInfo.setIsUnique) joins the first group whose first
//   member is the same template, or a RotationalBundle (or Camera) with the
//   same rotation settings (0x00678070); else it starts a group.
// - An update holds each group's state in turn (at most 12; 0x0057f420),
//   and every member of a group takes the same bits (0x0057f670 rewinds the
//   stream for each one).
#pragma once

#include "Networkables.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::world {
class ObjectTemplate;
class ObjectTemplateManager;
} // namespace dice::hfe::world

namespace dice::hfe::net {

// A template and its children as the ghost layout needs them.
struct GhostTemplateNode {
	uint32_t templateId = 0;
	std::string name;
	std::string type;            // the template type ("PlayerControlObject"...)
	NetworkableHints hints;      // hints.kind Unknown: no networkable
	bool controlObject = false;  // a PlayerControlObject (starts a ghost)
	bool projectile = false;     // a GenericProjectile (interface 0x94c1)
	bool unique = false;         // NetworkableInfo.setIsUnique
	bool forceNetworkableId = false; // NetworkableInfo.setForceNetworkableId
	std::string groupKey;        // equal keys share a group (non-unique only)
	bool fullChildSync = false;  // armor.fullChildSync: hit points in eighths in its root's state
	std::vector<GhostTemplateNode> children;
};

// One object of a ghost, in pre-order.
struct GhostNodeLayout {
	uint32_t templateId = 0;
	std::string name;
	int parent = -1; // index in the ghost's nodes, -1 for the ghost's root
	int group = -1;  // the group whose state it takes, -1 none
	NetworkableKind kind = NetworkableKind::Unknown;
};

struct GhostGroupLayout {
	NetworkableHints hints;   // the first member's
	std::vector<int> members; // node indices
};

struct GhostPartLayout {
	uint32_t templateId = 0; // the ghost root's template
	std::string name;
	std::vector<GhostNodeLayout> nodes;
	std::vector<GhostGroupLayout> groups;
};

// The ghosts an object of this template tree makes, in ghost id order.
std::vector<GhostPartLayout> buildGhostLayout(const GhostTemplateNode& root);

// The groups' states of one update. Returns false when a group couldn't be
// decoded to its end (the groups after it are left out of `states`).
// `mode`: 2 in ghost updates (0x0057f670), 3 in the control-object record
// (0x0057f720 / 0x0057f9f0).
bool decodeGhostState(BitReader& in, const GhostPartLayout& layout, const Vec3f& reference,
	std::vector<NetworkableState>& states, unsigned mode = 2);
// Writes the groups' states (one per group, in order), for tests and the
// loopback server (0x0057f420 / 0x0057f7b0).
void encodeGhostState(BitWriter& out, const GhostPartLayout& layout, const Vec3f* reference,
	const std::vector<NetworkableState>& states, unsigned mode = 2);

// The control-object record's state after its ghost fields (written by
// 0x00580830, read by 0x00580cd0 when the client has the record's ghost):
//   - 1 bit: 0 a fresh state, which the client stores into history slot
//     `slot` (0x0057f750 via 0x00580800: 8 slots of 0x402 bytes at the
//     ghost manager's +0x20f4); 1 a state written against the copy in a
//     slot the client has (the server's +0x20f0, sent while +0x20fc
//     counts down: 0x0057f9b0 with the slot; the networkables that keep
//     history take it, the Soldier's reader ignores it);
//   - 3 bits: the slot;
//   - every group of the record's ghost in mode 3, then those of the
//     second ghost when the record names one;
//   - a 10-bit length and the player's own state (PlayerOwnState; its
//     reader's bits are checked against the length: fewer are skipped,
//     more fail the block).
// The rewrite's server always sends fresh states (the writer's path while
// +0x20f0 is -1), cycling the slot as 0x00580830 does ((+0x20f8 + 1) & 7).
struct ControlState {
	bool recorded = false;
	int slot = 0;
	std::vector<NetworkableState> groups;
	std::vector<NetworkableState> second;
	bool hasOwn = false;
	PlayerOwnState own;
	size_t ownBits = 0;
	bool complete = false;
};
bool decodeControlState(BitReader& in, const GhostPartLayout& ghost, const GhostPartLayout* second,
	const Vec3f& reference, const PlayerOwnStateHints& own, ControlState& out);
void encodeControlState(BitWriter& out, const GhostPartLayout& ghost, const GhostPartLayout* second,
	const Vec3f* reference, const PlayerOwnStateHints& own, const ControlState& state);

// NetworkableInfo settings (Objects/Common/Networkables.con): which infos
// are unique and which force a networkable id. The defaults are 1.51's.
struct NetworkableInfoTable {
	std::vector<std::string> unique {"SpringInfo"};
	std::vector<std::string> forceNetworkableId {"HandFireArmsInfo"};
};

// Builds the node for a template of a client's template manager (the same
// templates the server loaded): its type, its networkableInfo, the hints
// from its settings (hasMobilePhysics, armor.*, maxSpeed, engineType...),
// and its children (ObjectTemplate.addTemplate), recursively.
GhostTemplateNode ghostTemplateFrom(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t,
	const NetworkableInfoTable& infos = {});
// The hints for one template (no children); `hasParent` picks the
// networkable of a PlayerControlObject (SimpleObject as a root,
// PlayerControlObject as a seat).
NetworkableHints networkableHintsFrom(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t,
	bool hasParent);

} // namespace dice::hfe::net
