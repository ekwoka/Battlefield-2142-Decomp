#pragma once

// What a round in flight or lying shows: its own geometry and the effect
// bundles of its template tree.
//
// In BF2142.exe a round is an object like any other (a GenericProjectile,
// its template's ObjectTemplate.geometry its mesh): the render object its
// geometry gives it is drawn by RendDX9 as any bundled mesh, with the
// round's matrix, and its child templates are created with it (the
// trail's EffectBundle, its looping Sound). The bundles start as their
// events say (0x0061a4e0): those with startAtCreation with the round;
// event 0 at the engine's start (the launch 0x005b3990 calls 0x005b2c10 at
// once, or after startDelay through the event queue, 0x005b62a0; with
// follow.targetActivation instead when the seek takes a target), which
// starts the bundles whose startOnEffects is 0; event 1 when it stops
// (0x005b2ca0: the target lost, or the round's end 0x005b4cb0), which
// stops those whose stopOnEffects is 0. The round's matrix is its launch
// frame, turned only by the follow (a Point round's physics leaves it:
// game::noseOf); a stuck round's up is the face's normal (0x005e83d0,
// game::stuckFrame) and a lying one keeps its facing.
//
// The rewrite's rounds are GameWorld's projectiles, without objects:
// RoundVisuals gives each living one an anchor in the object effects
// (ObjectEffects, with the events above) and a placed mesh for the
// renderer. Bullets have no geometry (only tracers, GameWorld::
// placeTracers); the rockets, missiles, grenades, mines, C4 and the like
// have one. A joined client's rounds that follow a server's projectile
// ghost are left out: the ghost's own object is drawn and carries the
// bundles.

#include "Effects/Effects.h"
#include "Game/Weapons.h"
#include "World/ObjectTemplateManager.h"

#include <cstdint>
#include <map>
#include <vector>

namespace dice::hfe::client {

// The anchors the rounds take in the object effects (above any object id).
constexpr uint32_t kRoundAnchor = 0xa0000000u;

// A round's frame (rows x, y, z): stuck, up along the face's normal with
// its x axis kept from its facing (0x005e83d0); lying, its facing; in
// flight, along its nose (game::noseOf), unrolled.
physics::Mat3 roundFrame(const game::Projectile& round);

// Whether the round's engine runs (event 0 sent, event 1 not since): from
// the launch, after startDelay; with follow.targetActivation while it has
// a target (0x005b3990, 0x005b2c10, 0x005b2ca0).
bool roundEngineOn(const game::Projectile& round);

class RoundVisuals {
public:
	explicit RoundVisuals(const world::ObjectTemplateManager& templates) : m_templates(templates) {}

	// Once a tick, after the round's: the rounds' states for the object
	// effects, added to `states` (the engine's events where it started or
	// stopped since the last call).
	void appendEffectStates(const std::vector<game::Projectile>& rounds,
		std::vector<effects::ObjectEffects::ObjectState>& states);

	// A round's mesh this frame: its template (whose geometry is drawn) and place.
	struct Mesh {
		const world::ObjectTemplate* objectTemplate = nullptr;
		physics::Vec3 position;
		physics::Mat3 rotation;
	};
	// `viewedFrom`: the round whose camera is the view (Projectile::serial,
	// 0 none), left out. The original draws it, but its opaque techniques
	// (BundledMesh.fx Full / Full20 / t1) leave Direct3D's CullMode CCW on,
	// and its camera sits inside its nose (AAGuided_missile_Camera at z
	// 0.33, remote_missile's mesh to 0.38): every face it could see turns
	// its back and none is drawn. The rewrite draws meshes without culling.
	void meshes(const std::vector<game::Projectile>& rounds, std::vector<Mesh>& out, uint32_t viewedFrom = 0);

	// What a round's template shows: its geometry and whether its tree
	// holds effect bundles (looked up once per template).
	struct Look {
		const world::ObjectTemplate* objectTemplate = nullptr;
		bool geometry = false;
		bool bundles = false;
	};
	const Look& lookOf(const game::Projectile& round);

private:
	const world::ObjectTemplateManager& m_templates;
	std::map<const game::ProjectileSettings*, Look> m_looks;
	effects::ObjectEffects::BundleCache m_bundleCache;
	std::map<uint32_t, bool> m_engines; // by serial: the engine's state last tick
};

} // namespace dice::hfe::client
