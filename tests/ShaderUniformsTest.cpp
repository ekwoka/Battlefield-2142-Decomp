// The shaders' uniform blocks against what the renderer pushes
// (Render/GpuContext.h): each block at most 4 KiB, the range SDL's Vulkan
// backend binds a slot with (MAX_UBO_SECTION_SIZE), and no block longer
// than its push: SDL's Metal backend binds a push where it lands in its
// 32 KiB buffer, so a longer block runs past the buffer's end (the Metal
// validation layer's "has space for N bytes, but argument has a
// length(M)"). The sizes are the SPIR-V's reflection (spirv-cross
// --reflect, cmake/Shaders.cmake).
#include "TestMain.h"

#ifdef BF2142_RENDER_TESTS
#include "Render/GpuContext.h"
#include "Render/PostProduction.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>

using namespace dice::hfe::render;

namespace {

// A shader's uniform blocks by binding: (name, size).
using Blocks = std::map<uint32_t, std::pair<std::string, uint32_t>>;

// The "ubos" of a spirv-cross reflection: flat objects of scalars.
Blocks uniformBlocks(const std::string& json)
{
	Blocks blocks;
	const size_t ubos = json.find("\"ubos\"");
	if (ubos == std::string::npos) {
		return blocks;
	}
	const size_t end = json.find(']', ubos);
	const auto field = [](const std::string& object, const char* key) {
		const size_t at = object.find(std::string("\"") + key + "\"");
		if (at == std::string::npos) {
			return std::string();
		}
		size_t value = object.find(':', at) + 1;
		while (value < object.size() && (object[value] == ' ' || object[value] == '"')) {
			++value;
		}
		size_t stop = value;
		while (stop < object.size() && object[stop] != '"' && object[stop] != ',' && object[stop] != '\n') {
			++stop;
		}
		return object.substr(value, stop - value);
	};
	for (size_t open = json.find('{', ubos); open != std::string::npos && open < end; open = json.find('{', open + 1)) {
		const std::string object = json.substr(open, json.find('}', open) - open);
		blocks[static_cast<uint32_t>(std::stoul(field(object, "binding")))] = {
			field(object, "name"), static_cast<uint32_t>(std::stoul(field(object, "block_size")))};
	}
	return blocks;
}

std::map<std::string, Blocks> shaderBlocks()
{
	std::map<std::string, Blocks> shaders;
	for (const auto& entry : std::filesystem::directory_iterator(BF2142_SHADER_DIR)) {
		if (entry.path().extension() == ".json") {
			std::ifstream file(entry.path());
			std::stringstream text;
			text << file.rdbuf();
			shaders[entry.path().stem().string()] = uniformBlocks(text.str());
		}
	}
	return shaders;
}

} // namespace

TEST(ShaderUniforms_EveryBlockFitsSdlsVulkanRange)
{
	const auto shaders = shaderBlocks();
	CHECK(shaders.size() >= 38);
	for (const auto& [shader, blocks] : shaders) {
		for (const auto& [binding, block] : blocks) {
			if (block.second > kMaxUniformBlockSize) {
				std::fprintf(stderr, "  %s %s (binding %u): %u bytes\n", shader.c_str(), block.first.c_str(), binding,
					block.second);
			}
			CHECK(block.second <= kMaxUniformBlockSize);
		}
	}
}

TEST(ShaderUniforms_MeshBlocksAreWhatPushTransformsSends)
{
	auto shaders = shaderBlocks();
	// mesh.vert: the Transforms block whole, the parts their own block.
	Blocks& mesh = shaders["mesh.vert"];
	CHECK(mesh.size() == 2);
	CHECK(mesh[0].first == "Transforms" && mesh[0].second == kTransformsBlockSize);
	CHECK(mesh[kPartsSlot].first == "Parts" && mesh[kPartsSlot].second == sizeof(TransformUniforms::parts));
	// motion.vert: the same, the Motion block and last frame's parts (pushMotion).
	Blocks& motion = shaders["motion.vert"];
	CHECK(motion.size() == 4);
	CHECK(motion[0].first == "Transforms" && motion[0].second == kTransformsBlockSize);
	CHECK(motion[1].first == "Motion" && motion[1].second == kMotionBlockSize);
	CHECK(motion[kPartsSlot].first == "Parts" && motion[kPartsSlot].second == sizeof(TransformUniforms::parts));
	CHECK(motion[kPreviousPartsSlot].first == "PreviousParts" &&
		motion[kPreviousPartsSlot].second == sizeof(MotionUniforms::previousParts));
	// roadmesh.vert reads the view-projection from the parts block.
	Blocks& roadMesh = shaders["roadmesh.vert"];
	CHECK(roadMesh[kPartsSlot].first == "Parts" && roadMesh[kPartsSlot].second == sizeof(TransformUniforms::parts));
	// The other shaders pushed a TransformUniforms read a prefix of the block.
	for (const char* shader : {"roadmesh.vert", "terrain.vert", "road.vert", "water.vert", "decal.vert"}) {
		Blocks& blocks = shaders[shader];
		CHECK(blocks[0].first == "Transforms" && blocks[0].second <= kTransformsBlockSize);
	}
}

TEST(ShaderUniforms_NoBlockOutrunsItsPush)
{
	auto shaders = shaderBlocks();
	// Each (shader, binding) and the C++ block pushed there whole.
	const struct {
		const char* shader;
		uint32_t binding;
		const char* name;
		size_t pushed;
	} pushes[] = {
		{"terrain.vert", 1, "Morph", sizeof(TerrainMorphUniforms)},
		{"undergrowth.vert", 0, "Undergrowth", sizeof(UndergrowthUniforms)},
		{"particle.vert", 0, "Particles", sizeof(ParticleUniforms)},
		{"enh_particle.vert", 0, "Particles", sizeof(ParticleUniforms)},
		{"mesh.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"enh_mesh.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"terrain.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"enh_terrain.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"surrounding.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"undergrowth.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"enh_undergrowth.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"road.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"enh_road.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"decal.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"water.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"particle.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"enh_particle.frag", 0, "Lighting", sizeof(LightingUniforms)},
		{"mesh.frag", 1, "Shadows", sizeof(ShadowUniforms)},
		{"terrain.frag", 2, "Shadows", sizeof(ShadowUniforms)},
		{"road.frag", 2, "Shadows", sizeof(ShadowUniforms)},
		{"undergrowth.frag", 1, "Shadows", sizeof(ShadowUniforms)},
		{"terrain.frag", 1, "Detail", sizeof(TerrainDetailUniforms)},
		{"enh_terrain.frag", 1, "Detail", sizeof(TerrainDetailUniforms)},
		{"surrounding.frag", 1, "Detail", sizeof(TerrainDetailUniforms)},
		{"road.frag", 1, "Road", sizeof(RoadUniforms)},
		{"enh_road.frag", 1, "Road", sizeof(RoadUniforms)},
		{"water.frag", 1, "Water", sizeof(WaterUniforms)},
		{"sky.frag", 0, "Sky", sizeof(SkyUniforms)},
		{"post.frag", 0, "Post", sizeof(PostUniforms)},
		{"bloom.frag", 0, "Bloom", sizeof(BloomUniforms)},
	};
	for (const auto& push : pushes) {
		const auto& block = shaders[push.shader][push.binding];
		if (block.first != push.name || block.second > push.pushed) {
			std::fprintf(stderr, "  %s binding %u: %s %u bytes, pushed %zu\n", push.shader, push.binding,
				block.first.c_str(), block.second, push.pushed);
		}
		CHECK(block.first == push.name && block.second <= push.pushed);
	}
}

#endif

// The shaders' own fog and particle math, compiled as C++ (Render/ShaderMath.h:
// fog_share.glsl, particle_color.glsl), against numbers worked out from
// retail's RaCommon.fx, Particles.fx and D3D9's fixed-function fog.
#include "Render/Fog.h"
#include "Render/ParticleFog.h"
#include "Render/ShaderMath.h"

#include <cmath>

namespace {

bool approx(float a, float b)
{
	return std::fabs(a - b) < 1e-5f;
}

bool approx(const dice::hfe::render::glsl::vec4& v, float x, float y, float z, float w)
{
	return approx(v.x, x) && approx(v.y, y) && approx(v.z, z) && approx(v.w, w);
}

} // namespace

// RaCommon.fx calcFog(w) = max(w FogRange.y + FogRange.w, FogColor.w)
// - (w FogRange.x + FogRange.z)^3, FogRange from 0x100355c0. Fog from 100 to
// 500 m, base 1: at 300 m 0.5 - 0.5^3 = 0.375 shows (a 0.625 share of fog);
// a near floor (FogColor.w) of 0.6 lifts that to 0.475; before the start
// none, past the end all.
TEST(ShaderMath_FogShareIsRetailsCalcFog)
{
	using namespace dice::hfe::render;
	const std::array<float, 4> fog {100, 500, 1, 0};
	const auto r = fogRange(fog);
	const glsl::vec4 range(r[0], r[1], r[2], r[3]);
	CHECK(approx(glsl::fogShare(300, range, 0), 0.625f));
	CHECK(approx(glsl::fogShare(300, range, 0.6f), 0.525f));
	CHECK(approx(glsl::fogShare(50, range, 0), 0));
	CHECK(approx(glsl::fogShare(600, range, 0), 1));
	// The CPU's copy (Fog.h) agrees.
	for (float w : {0.0f, 50.0f, 150.0f, 300.0f, 450.0f, 600.0f}) {
		CHECK(approx(glsl::fogShare(w, range, 0), 1 - fogVisibility(fog, w)));
	}
}

// RendDX9 0x1012f750 (called by the sprite draw 0x1012fdf0 at 0x1012fe38):
// the normal-blend sprites draw SRCALPHA / INVSRCALPHA with D3DRS_FOGENABLE
// 1 at every shader quality but the overdraw view (0); the additive ones
// (0x10130540, AdditiveLow/High) one/one with fog off.
TEST(ShaderMath_NormalBlendSpritesFogAdditiveOnesDont)
{
	using namespace dice::hfe::render;
	for (int quality = 1; quality <= 3; ++quality) {
		CHECK(spriteGroupFogged(false, quality));
		CHECK(!spriteGroupFogged(true, quality));
	}
	CHECK(!spriteGroupFogged(false, 0));
	// particle.frag's material.w: 1 fog, + 2 the additive mask.
	CHECK(particleMaterialFlags(true, false) == 1.0f);
	CHECK(particleMaterialFlags(false, true) == 2.0f);
	CHECK(particleMaterialFlags(false, false) == 0.0f);
}

// psParticleLow/Medium/High then the fog: texel x colour, saturated as
// ps_1_4's output is (red 0.8 x 1.5 = 1.2 -> 1), then lerped toward FogColor
// by the fog's share: 1 x 0.375 + 0.2 x 0.625 = 0.5 (not 0.575, as the
// unsaturated colour would give). Alpha 0.5 x 0.8, untouched by the fog.
TEST(ShaderMath_NormalParticlesSaturateThenFog)
{
	using namespace dice::hfe::render;
	const glsl::vec4 texel(0.8f, 0.6f, 0.4f, 0.5f), color(1.5f, 1.0f, 0.5f, 0.8f);
	const glsl::vec3 fogColor(0.2f, 0.4f, 0.6f);
	CHECK(approx(glsl::particleColor(texel, color, false, 0, fogColor), 1, 0.6f, 0.2f, 0.4f));
	const auto r = fogRange({100, 500, 1, 0});
	const float fog = glsl::fogShare(300, glsl::vec4(r[0], r[1], r[2], r[3]), 0);
	CHECK(approx(glsl::particleColor(texel, color, false, fog, fogColor), 0.5f, 0.475f, 0.45f, 0.4f));
}

// Particles.fx psParticleAdditive*: alpha masks the colour inside the shader and
// the pixel output clamps before the one/one add, so a colour over 1 keeps its
// blue (0.5/1.2/2 x 0.5 is 0.25/0.6/1, not the cyan 0.25/0.5/0.5 of clamp-then-mask).
TEST(ShaderMath_AdditiveParticlesMaskBeforeClamp)
{
	using namespace dice::hfe::render;
	const glsl::vec4 texel(1, 1, 1, 0.5f), color(0.5f, 1.2f, 2, 1);
	CHECK(approx(glsl::particleColor(texel, color, true, 0, glsl::vec3(0.2f, 0.4f, 0.6f)), 0.25f, 0.6f, 1, 0.5f));
}
