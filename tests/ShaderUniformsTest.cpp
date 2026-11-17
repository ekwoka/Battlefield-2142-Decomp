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
