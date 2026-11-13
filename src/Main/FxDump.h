#pragma once
// BF2142_FX_DUMP=1: once a second, one greppable "fx-dump:" line for every live particle system, placed mesh group
// and round mesh within `radius` metres of the camera (a diagnostic for a glow that stays where it should not:
// which emitter it is, where, and what carries it). Off by default; the lines are only built when it is on.
#include "Effects/Effects.h"
#include "Main/RoundVisuals.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace dice::hfe::client {

// Whether a colour reads orange: red clearly over green, green over blue.
inline bool fxWarm(const effects::Vec3& c)
{
	return c.x > 0.6f && c.x > c.y * 1.15f && c.y > c.z * 1.2f;
}

inline std::string fxFormat(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline std::string fxFormat(const char* fmt, ...)
{
	char text[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(text, sizeof text, fmt, args);
	va_end(args);
	return text;
}

// `source` names the manager ("world" or "objects"); `describe` names what an anchor is (an object's template, a
// round's), empty when unknown.
inline void fxDumpEmitters(const effects::EffectManager& manager, const char* source, const effects::Vec3& camera,
	float radius, const std::function<std::string(uint32_t)>& describe, std::vector<std::string>& out)
{
	for (const auto& e : manager.emitterList()) {
		const effects::Vec3 d = e->position() - camera;
		const float distance = std::sqrt(physics::lengthSquared(d));
		if (distance > radius) {
			continue;
		}
		const effects::ParticleTemplate& t = e->particleTemplate();
		const char* kind = t.trail ? "trail" : t.mesh ? "mesh" : t.nonScreenAligned ? "quad" : "sprite";
		out.push_back(fxFormat(
			"fx-dump: %s emitter %s tex=%s at=%.1f/%.1f/%.1f dist=%.1f anchor=%u tag=%u parent=%s age=%.1f particles=%zu "
			"stopped=%d emitTime=%.1f looping=%d color=%.2f/%.2f/%.2f warm=%d",
			source, kind, t.texture.c_str(), e->position().x, e->position().y, e->position().z, distance, e->anchor, e->tag,
			e->anchor && describe ? describe(e->anchor).c_str() : "-", e->age(), e->particles().size(), e->stopped() ? 1 : 0,
			t.emitTime, t.isLooping ? 1 : 0, t.color1.x, t.color1.y, t.color1.z, fxWarm(t.color1) ? 1 : 0));
	}
	for (const auto& [geometry, sprites] : manager.placedMeshes()) {
		for (const effects::MeshSprite& s : sprites) {
			const float distance = std::sqrt(physics::lengthSquared(s.position - camera));
			if (distance > radius) {
				continue;
			}
			out.push_back(fxFormat("fx-dump: %s placed-mesh geometry=%s at=%.1f/%.1f/%.1f dist=%.1f color=%.2f/%.2f/%.2f warm=%d",
				source, geometry.c_str(), s.position.x, s.position.y, s.position.z, distance, s.color[0], s.color[1],
				s.color[2], fxWarm({s.color[0], s.color[1], s.color[2]}) ? 1 : 0));
		}
	}
}

inline void fxDumpRounds(const std::vector<RoundVisuals::Mesh>& meshes, const effects::Vec3& camera, float radius,
	std::vector<std::string>& out)
{
	for (const RoundVisuals::Mesh& m : meshes) {
		const float distance = std::sqrt(physics::lengthSquared(m.position - camera));
		if (distance <= radius && m.objectTemplate) {
			out.push_back(fxFormat("fx-dump: round-mesh template=%s at=%.1f/%.1f/%.1f dist=%.1f", m.objectTemplate->name().c_str(),
				m.position.x, m.position.y, m.position.z, distance));
		}
	}
}

} // namespace dice::hfe::client
