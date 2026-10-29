#include "EffectSettings.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::scene {

namespace {

constexpr uint32_t kAccess = ConsoleAccess::Anyone;

void bindFloat(Console& console, const char* name, float& value)
{
	bindProperty<float>(console, name, kAccess, [&value] { return value; }, [&value](const float& v) { value = v; });
}

void bindBool(Console& console, const char* name, bool& value)
{
	bindProperty<bool>(console, name, kAccess, [&value] { return value; }, [&value](const bool& v) { value = v; });
}

void bindInt(Console& console, const char* name, int& value)
{
	bindProperty<int>(console, name, kAccess, [&value] { return value; }, [&value](const int& v) { value = v; });
}

// The effects rows (RendDX9 DAT_101f45a8, 0x3c bytes each, by
// VideoSettings' effects level): the view-distance scale, the sprite,
// mesh and other pools, the priority scales, the LOD fractions, the fade
// and LOD starts, the trail pool, the shader quality, the quality level.
struct EffectsRow {
	float viewScale;
	int pools[3];
	float priority[3];
	float lod[3];
	float fadeStart, lodStart;
	int trailPool;
	int shaderQuality;
	int qualityLevel;
};
constexpr EffectsRow kEffectsRows[5] = {
	{0.0f, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, 0, 0, 0, 1, 0},
	{0.5f, {300, 300, 300}, {0.4f, 0.55f, 0.7f}, {0.3f, 0.6f, 1.0f}, 0.2f, 0.7f, 2, 1, 0},
	{0.75f, {500, 500, 500}, {0.4f, 0.55f, 0.7f}, {0.3f, 0.6f, 1.0f}, 0.3f, 0.85f, 1, 3, 1},
	{1.0f, {1000, 700, 600}, {0.4f, 0.55f, 0.7f}, {0.3f, 0.6f, 1.0f}, 0.4f, 0.85f, 0, 3, 2},
	{1.0f, {1500, 1500, 1000}, {0.4f, 0.55f, 0.7f}, {0.3f, 0.6f, 1.0f}, 0.85f, 0.85f, 0, 3, 2},
};

int level3(int value)
{
	return std::clamp(value, 1, 3);
}

} // namespace

float WindSettings::speedAt() const
{
	const auto w = windAt();
	return std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
}

namespace {

using Vec = std::array<float, 3>;

float dot3(const Vec& a, const Vec& b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec normalized3(const Vec& v)
{
	const float l = std::sqrt(dot3(v, v));
	return l > 1e-6f ? Vec {v[0] / l, v[1] / l, v[2] / l} : Vec {0, 0, 0};
}

} // namespace

std::array<float, 3> WindSettings::LocalWind::windAt(const std::array<float, 3>& point) const
{
	const Vec d {point[0] - position[0], point[1] - position[1], point[2] - position[2]};
	if (falloff <= 0 || dot3(d, d) >= falloff * falloff) {
		return {0, 0, 0}; // out of reach (0x007040d0)
	}
	Vec towards;
	float strength = 0;
	switch (type) {
	case Type::Directional:
	case Type::Point: {
		// (1 - min(d^2 / falloff^2, 1)) x speed, along the turned direction
		// or away from the affector.
		strength = (1 - std::min(dot3(d, d) / (falloff * falloff), 1.0f)) * speed;
		towards = type == Type::Point ? normalized3(d) : direction;
		break;
	}
	case Type::RadialDir: {
		// Along its axis: (1 - min(a / falloff, 1)) x speed, a the distance
		// along it; nothing beyond radialFalloff from the axis, and the
		// direction bends out from the axis by min(r^2 / rf^2 + 0.2, 1)
		// (0x00a051a0).
		const float along = dot3(d, direction);
		strength = (1 - std::min(along / falloff, 1.0f)) * speed;
		const Vec radial {d[0] - direction[0] * along, d[1] - direction[1] * along, d[2] - direction[2] * along};
		const float r2 = dot3(radial, radial);
		if (radialFalloff <= 0 || r2 >= radialFalloff * radialFalloff) {
			return {0, 0, 0};
		}
		const float bend = std::min(r2 / (radialFalloff * radialFalloff) + 0.2f, 1.0f);
		const Vec out = normalized3(radial);
		towards = {out[0] * bend + direction[0] * (1 - bend), out[1] * bend + direction[1] * (1 - bend),
			out[2] * bend + direction[2] * (1 - bend)};
		break;
	}
	case Type::Turbulence:
		return {0, 0, 0}; // no case of its own in 0x0064b5f0
	}
	return {towards[0] * strength, towards[1] * strength, towards[2] * strength};
}

std::array<float, 3> WindSettings::windAt(const std::array<float, 3>& point) const
{
	std::array<float, 3> w = windAt();
	for (const LocalWind& local : localWinds) {
		const auto l = local.windAt(point);
		w = {w[0] + l[0], w[1] + l[1], w[2] + l[2]};
	}
	return w;
}

float WindSettings::speedAt(const std::array<float, 3>& point) const
{
	const auto w = windAt(point);
	return std::sqrt(dot3(w, w));
}

int QualitySettings::terrainLodSwitchDistance() const
{
	static constexpr int kDistance[3] = {64, 80, 100}; // 0x10010040: 0x40, 0x50, 100
	return kDistance[level3(terrain) - 1];
}

float QualitySettings::undergrowthViewDistanceScale() const
{
	static constexpr float kScale[3] = {0.5f, 0.75f, 1.0f};
	return kScale[level3(terrain) - 1];
}

bool QualitySettings::undergrowthSimpleShader() const
{
	if (undergrowthSimpleShaderSet >= 0) {
		return undergrowthSimpleShaderSet != 0;
	}
	return level3(terrain) < 3;
}

float QualitySettings::geometryLodFactor() const
{
	static constexpr float kFactor[3] = {0.5f, 0.8f, 1.0f};
	return kFactor[level3(geometry) - 1];
}

float QualitySettings::overgrowthViewDistanceScale() const
{
	static constexpr float kScale[3] = {0.4f, 0.7f, 1.0f};
	return kScale[level3(geometry) - 1];
}

int QualitySettings::maxSkip3pLods() const
{
	return level3(geometry) == 1 ? 1 : 0;
}

int QualitySettings::skipMipLevels() const
{
	// GSSkipMipLevels: 1 -> 2, 2 -> 1, 3 -> 0 (0x10010040: +0x114 below 0
	// takes the texture level).
	return 3 - level3(texture);
}

int QualitySettings::skipMipLevels(TextureClass textureClass) const
{
	const int skip = skipMipLevels();
	switch (textureClass) {
	case TextureClass::Interface:
		return 0; // the class's "skips" byte is off (0x10017ca0: +0x64)
	case TextureClass::Undergrowth:
	case TextureClass::Overgrowth:
		// +0x70, +0x90, +0xb0, +0xb8: classes 2, 6, 10 and 11.
		return std::clamp(skip - 1, 0, 1);
	default:
		return skip;
	}
}

int QualitySettings::lightingLevel() const
{
	static constexpr int kLevel[3] = {3, 1, 0}; // 0x10010040: renderer +0x6b4
	return kLevel[level3(lighting) - 1];
}

bool QualitySettings::dynamicLightsEnabled() const
{
	return dynamicLighting > 0;
}

bool QualitySettings::decalsEnabled() const
{
	return effects > 1;
}

void QualitySettings::applyEffects(ParticleSystemSettings& s) const
{
	const EffectsRow& row = kEffectsRows[std::clamp(effects, 0, 4)];
	// 0x100991b0: GSViewDistance (500) x the view distance scale x the
	// row's, at least 200.
	s.viewDistance = std::max(500.0f * viewDistanceScale * row.viewScale, 200.0f);
	s.maxParticles = row.pools[0];
	s.priorityScales = {row.priority[0], row.priority[1], row.priority[2]};
	s.lodFractions = {row.lod[0], row.lod[1], row.lod[2]};
	s.startFadePercentageOfViewDistance = row.fadeStart;
	s.startLodPercentageOfLodDistance = row.lodStart;
	s.particleSystemShaderQuality = row.shaderQuality;
	s.qualityLevel = row.qualityLevel;
}

void registerWindConsoleMethods(Console& console, WindSettings& s)
{
	// windManager.globalWindSpeed (0x006ebf7f: slots 0x1c / 0x20, +0x2c),
	// windManager.globalWindDirection (slots 0x24 / 0x28, +0x30).
	bindFloat(console, "windManager.globalWindSpeed", s.globalWindSpeed);
	bindProperty<ConsoleVec3>(console, "windManager.globalWindDirection", kAccess,
		[&s] { return ConsoleVec3 {s.globalWindDirection[0], s.globalWindDirection[1], s.globalWindDirection[2]}; },
		[&s](const ConsoleVec3& v) { s.globalWindDirection = {v.x, v.y, v.z}; });
}

void registerColdBreathConsoleMethods(Console& console, ColdBreathSettings& s)
{
	bindBool(console, "GameLogic.useColdBreath", s.useColdBreath);
}

void registerParticleSystemConsoleMethods(Console& console, ParticleSystemSettings& s)
{
	bindBool(console, "ParticleSystemManager.drawStats", s.drawStats);
	bindMethod(console, "ParticleSystemManager.resetStats", kAccess, [] {});
	bindBool(console, "ParticleSystemManager.drawBoundingBoxEnable", s.drawBoundingBoxEnable);
	bindFloat(console, "ParticleSystemManager.startFadePercentageOfViewDistance", s.startFadePercentageOfViewDistance);
	bindFloat(console, "ParticleSystemManager.viewDistance", s.viewDistance);
	bindFloat(console, "ParticleSystemManager.startLodPercentageOfLodDistance", s.startLodPercentageOfLodDistance);
	bindBool(console, "ParticleSystemManager.affectedByWind", s.affectedByWind);
	bindInt(console, "ParticleSystemManager.particleSystemShaderQuality", s.particleSystemShaderQuality);
	bindFloat(console, "ParticleSystemManager.overdrawScale", s.overdrawScale);
	bindInt(console, "ParticleSystemManager.qualityLevel", s.qualityLevel);
	bindFloat(console, "ParticleSystemManager.shimmerIntensity", s.shimmerIntensity);
	bindFloat(console, "ParticleSystemManager.shimmerPhases", s.shimmerPhases);
	bindFloat(console, "ParticleSystemManager.shimmerSpeed", s.shimmerSpeed);
	bindFloat(console, "ParticleSystemManager.nsaShimmerIntensity", s.nsaShimmerIntensity);
	bindFloat(console, "ParticleSystemManager.nsaShimmerPhases", s.nsaShimmerPhases);
	bindFloat(console, "ParticleSystemManager.nsaShimmerSpeed", s.nsaShimmerSpeed);
	bindFloat(console, "ParticleSystemManager.trailShimmerIntensity", s.trailShimmerIntensity);
	bindFloat(console, "ParticleSystemManager.trailShimmerPhases", s.trailShimmerPhases);
	bindFloat(console, "ParticleSystemManager.trailShimmerSpeed", s.trailShimmerSpeed);
}

void registerQualityConsoleMethods(Console& console, QualitySettings& s)
{
	const auto level = [&console, &s](const char* name, int QualitySettings::*field, int lo, int hi) {
		bindMethod(console, name, kAccess, [&s, field, lo, hi](int value) {
			s.*field = std::clamp(value, lo, hi);
			s.changed = true;
			s.undergrowthSimpleShaderSet = -1; // the switch writes it again
		});
	};
	level("VideoSettings.setTerrainQuality", &QualitySettings::terrain, 1, 3);
	level("VideoSettings.setGeometryQuality", &QualitySettings::geometry, 1, 3);
	level("VideoSettings.setLightingQuality", &QualitySettings::lighting, 1, 3);
	level("VideoSettings.setDynamicLightingQuality", &QualitySettings::dynamicLighting, 0, 3);
	level("VideoSettings.setEffectsQuality", &QualitySettings::effects, 0, 4);
	level("VideoSettings.setTextureQuality", &QualitySettings::texture, 1, 3);
	level("VideoSettings.setTextureFilteringQuality", &QualitySettings::textureFiltering, 0, 3);
	bindMethod(console, "VideoSettings.setViewDistanceScale", kAccess, [&s](float scale) {
		s.viewDistanceScale = std::clamp(scale, 0.0f, 1.0f);
		s.changed = true;
	});
	// undergrowth.simpleShaderEnable (0x100862e7: the undergrowth's slot
	// 0x40, the one the quality switch 0x10010040 writes): holds until the
	// video settings are applied again.
	bindProperty<bool>(console, "undergrowth.simpleShaderEnable", kAccess, [&s] { return s.undergrowthSimpleShader(); },
		[&s](const bool& on) { s.undergrowthSimpleShaderSet = on ? 1 : 0; });
}

} // namespace dice::hfe::scene
