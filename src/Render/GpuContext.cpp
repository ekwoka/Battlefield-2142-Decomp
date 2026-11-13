#include "GpuContext.h"
#include "HudBlend.h"

#include "EmbeddedShaders.h"

#include <algorithm>
#include <cstring>

namespace dice::hfe::render {

namespace {

SDL_GPUShader* loadShader(SDL_GPUDevice* device, SDL_GPUShaderStage stage, const unsigned char* spirv,
	size_t spirvSize, const unsigned char* msl, size_t mslSize, uint32_t samplers, uint32_t uniformBuffers)
{
	SDL_GPUShaderCreateInfo info {};
	info.stage = stage;
	info.num_samplers = samplers;
	info.num_uniform_buffers = uniformBuffers;
	const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
	if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
		info.format = SDL_GPU_SHADERFORMAT_SPIRV;
		info.code = spirv;
		info.code_size = spirvSize;
		info.entrypoint = "main";
	} else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
		info.format = SDL_GPU_SHADERFORMAT_MSL;
		info.code = msl;
		info.code_size = mslSize;
		info.entrypoint = "main0"; // spirv-cross renames main
	} else {
		return nullptr;
	}
	return SDL_CreateGPUShader(device, &info);
}

SDL_GPUTextureFormat gpuFormat(texture::TextureFormat format)
{
	switch (format) {
	case texture::TextureFormat::BC1:
		return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
	case texture::TextureFormat::BC2:
		return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
	case texture::TextureFormat::BC3:
		return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
	case texture::TextureFormat::RGBA8:
		return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	}
	return SDL_GPU_TEXTUREFORMAT_INVALID;
}

SDL_GPUSampleCount sampleCountOf(uint32_t samples)
{
	return samples >= 8 ? SDL_GPU_SAMPLECOUNT_8
		: samples >= 4  ? SDL_GPU_SAMPLECOUNT_4
		: samples >= 2  ? SDL_GPU_SAMPLECOUNT_2
						: SDL_GPU_SAMPLECOUNT_1;
}

} // namespace

std::unique_ptr<GpuContext> GpuContext::create(SDL_Window* window, std::string& error)
{
	std::unique_ptr<GpuContext> context(new GpuContext);
	context->m_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
	if (!context->m_device) {
		error = std::string("SDL_CreateGPUDevice: ") + SDL_GetError();
		return nullptr;
	}
	if (window) {
		if (!SDL_ClaimWindowForGPUDevice(context->m_device, window)) {
			error = std::string("SDL_ClaimWindowForGPUDevice: ") + SDL_GetError();
			return nullptr;
		}
		context->m_window = window;
		context->m_colorFormat = SDL_GetGPUSwapchainTextureFormat(context->m_device, window);
	}
	if (!context->createPipelines(error, 1, context->m_singleSampled)) {
		return nullptr;
	}

	SDL_GPUSamplerCreateInfo sampler {};
	sampler.min_filter = SDL_GPU_FILTER_LINEAR;
	sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
	sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
	sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	sampler.enable_anisotropy = true;
	sampler.max_anisotropy = 8;
	sampler.max_lod = 1000;
	context->m_repeatSampler = SDL_CreateGPUSampler(context->m_device, &sampler);
	sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
		SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	context->m_clampSampler = SDL_CreateGPUSampler(context->m_device, &sampler);
	sampler.enable_anisotropy = false;
	sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	sampler.max_lod = 0;
	sampler.enable_compare = true;
	sampler.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
	context->m_shadowSampler = SDL_CreateGPUSampler(context->m_device, &sampler);
	sampler.enable_compare = false;
	sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
	context->m_pointSampler = SDL_CreateGPUSampler(context->m_device, &sampler);
	// SwiffMenu.fx TexMapSamplerClamp/Wrap and Font.fx RegularScaled: linear, MipFilter None, no anisotropy.
	sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
	context->m_menuClampSampler = SDL_CreateGPUSampler(context->m_device, &sampler);
	sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	context->m_menuRepeatSampler = SDL_CreateGPUSampler(context->m_device, &sampler);
	context->m_emptyShadowMap = context->createShadowMap(1);
	if (context->m_emptyShadowMap) {
		// Cleared to the far plane: everything is lit by it.
		SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(context->m_device);
		SDL_GPUDepthStencilTargetInfo depth {};
		depth.texture = context->m_emptyShadowMap;
		depth.clear_depth = 1.0f;
		depth.load_op = SDL_GPU_LOADOP_CLEAR;
		depth.store_op = SDL_GPU_STOREOP_STORE;
		depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, nullptr, 0, &depth));
		SDL_SubmitGPUCommandBuffer(commands);
	}

	const uint32_t white = 0xffffffff;
	context->m_white = context->createRgbaTexture(1, 1, &white);
	const uint32_t black = 0xff000000;
	context->m_black = context->createRgbaTexture(1, 1, &black);
	const uint32_t flat = 0xffff8080; // (0.5, 0.5, 1): a tangent-space normal straight out
	context->m_flatNormal = context->createRgbaTexture(1, 1, &flat);
	{
		texture::DdsTexture cube;
		cube.format = texture::TextureFormat::RGBA8;
		cube.width = cube.height = 1;
		cube.cube = true;
		cube.faces.assign(6, {{1, 1, {0xff, 0xff, 0xff, 0xff}}});
		context->m_whiteCube = context->createTexture(cube, true);
		texture::DdsTexture volume; // two slices: a 3D texture
		volume.format = texture::TextureFormat::RGBA8;
		volume.width = volume.height = 1;
		volume.depth = 2;
		volume.faces.assign(1, {{1, 1, std::vector<uint8_t>(8, 0xff), 2}});
		context->m_whiteVolume = context->createTexture(volume);
	}
	context->flushUploads();
	return context;
}

GpuContext::~GpuContext()
{
	if (!m_device) {
		return;
	}
	flushUploads();
	SDL_WaitForGPUIdle(m_device);
	SDL_ReleaseGPUTexture(m_device, m_white);
	SDL_ReleaseGPUTexture(m_device, m_black);
	SDL_ReleaseGPUTexture(m_device, m_flatNormal);
	SDL_ReleaseGPUTexture(m_device, m_whiteCube);
	SDL_ReleaseGPUTexture(m_device, m_whiteVolume);
	SDL_ReleaseGPUSampler(m_device, m_repeatSampler);
	SDL_ReleaseGPUSampler(m_device, m_clampSampler);
	SDL_ReleaseGPUSampler(m_device, m_shadowSampler);
	SDL_ReleaseGPUSampler(m_device, m_pointSampler);
	SDL_ReleaseGPUSampler(m_device, m_menuClampSampler);
	SDL_ReleaseGPUSampler(m_device, m_menuRepeatSampler);
	for (SDL_GPUGraphicsPipeline* pipeline : {m_enhanced.mesh, m_enhanced.meshCulled, m_enhanced.fadedMesh,
			 m_enhanced.additiveMesh, m_enhanced.terrain, m_enhanced.undergrowth, m_enhanced.road, m_enhanced.roadMesh,
			 m_enhanced.depth, m_enhanced.terrainDepth, m_enhanced.terrainCaster, m_enhanced.gbuffer, m_enhanced.motion,
			 m_enhanced.motionOverlay, m_enhanced.gtao, m_enhanced.aoBlur, m_enhanced.taa}) {
		if (pipeline) {
			SDL_ReleaseGPUGraphicsPipeline(m_device, pipeline);
		}
	}
	SDL_ReleaseGPUTexture(m_device, m_emptyShadowMap);
	releaseWorldPipelines(m_singleSampled);
	releaseWorldPipelines(m_multisampled);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_shadowCasterPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_postPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_bloomPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_uiPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_uiSubmitPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_uiTestPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_uiClearPipeline);
	for (const auto& [key, pipeline] : m_uiBlendPipelines) {
		SDL_ReleaseGPUGraphicsPipeline(m_device, pipeline);
	}
	if (m_window) {
		SDL_ReleaseWindowFromGPUDevice(m_device, m_window);
	}
	SDL_DestroyGPUDevice(m_device);
}

bool GpuContext::createPipelines(std::string& error, uint32_t samples, WorldPipelines& world)
{
	// The multisampled set: the world's pipelines only (the shadow maps,
	// the post-production and the HUD stay single-sampled).
	const bool single = samples <= 1;
	m_depthFormat = SDL_GPUTextureSupportsFormat(m_device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
						SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
		? SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT
		: SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
	m_shadowDepthFormat = SDL_GPUTextureSupportsFormat(m_device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
							  SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)
		? SDL_GPU_TEXTUREFORMAT_D32_FLOAT
		: SDL_GPU_TEXTUREFORMAT_D16_UNORM;
	SDL_GPUVertexBufferDescription buffer {};
	buffer.slot = 0;
	buffer.pitch = sizeof(Vertex);
	buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	SDL_GPUVertexAttribute attributes[11] = {
		{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, position)},
		{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, normal)},
		{2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, texCoord)},
		{3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, lightmapCoord)},
		{4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(Vertex, part)},
		{5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(Vertex, part2)},
		{6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(Vertex, weight)},
		{7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, detailCoord)},
		{8, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, dirtCoord)},
		{9, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, crackCoord)},
		{10, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, tangent)},
	};

	SDL_GPUColorTargetDescription color {};
	color.format = m_colorFormat;

	SDL_GPUGraphicsPipelineCreateInfo info {};
	info.vertex_input_state.vertex_buffer_descriptions = &buffer;
	info.vertex_input_state.num_vertex_buffers = 1;
	info.vertex_input_state.vertex_attributes = attributes;
	info.vertex_input_state.num_vertex_attributes = 11;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	// Front faces are clockwise as in Direct3D. Two-sided by default: the
	// culled mesh pipelines below serve the materials that keep CullMode 3
	// (MeshQueues.h's meshCulled).
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
	info.depth_stencil_state.enable_depth_test = true;
	info.depth_stencil_state.enable_depth_write = true;
	info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
	info.target_info.color_target_descriptions = &color;
	info.target_info.num_color_targets = 1;
	info.target_info.depth_stencil_format = m_depthFormat;
	info.target_info.has_depth_stencil_target = true;
	info.multisample_state.sample_count = sampleCountOf(samples);

	const auto build = [&](const unsigned char* vs, size_t vsSize, const unsigned char* vsMsl, size_t vsMslSize,
						   const unsigned char* fs, size_t fsSize, const unsigned char* fsMsl, size_t fsMslSize,
						   uint32_t fragmentSamplers, uint32_t fragmentUniforms,
						   const char* name, uint32_t vertexUniforms = 1, uint32_t vertexSamplers = 0) -> SDL_GPUGraphicsPipeline* {
		SDL_GPUShader* vertex = loadShader(
			m_device, SDL_GPU_SHADERSTAGE_VERTEX, vs, vsSize, vsMsl, vsMslSize, vertexSamplers, vertexUniforms);
		SDL_GPUShader* fragment = loadShader(m_device, SDL_GPU_SHADERSTAGE_FRAGMENT, fs, fsSize, fsMsl, fsMslSize,
			fragmentSamplers, fragmentUniforms);
		SDL_GPUGraphicsPipeline* pipeline = nullptr;
		if (vertex && fragment) {
			info.vertex_shader = vertex;
			info.fragment_shader = fragment;
			pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
		}
		if (!pipeline) {
			error = std::string(name) + " pipeline: " + SDL_GetError();
		}
		SDL_ReleaseGPUShader(m_device, vertex);
		SDL_ReleaseGPUShader(m_device, fragment);
		return pipeline;
	};
	world.mesh = build(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_mesh_frag_spv, k_mesh_frag_spv_size, k_mesh_frag_msl, k_mesh_frag_msl_size, kMeshSamplers, 2, "mesh");
	// The same with back faces culled (CullMode CCW, Direct3D's front faces
	// clockwise): every material but the blended bundled and skinned ones
	// and the trees' leaves (meshCulled).
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
	world.culledMesh = build(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_mesh_frag_spv, k_mesh_frag_spv_size, k_mesh_frag_msl, k_mesh_frag_msl_size, kMeshSamplers, 2, "culled mesh");
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	if (!single) {
		// The overgrowth's leaves with alpha to coverage (RaShaderLeaf's
		// Alpha2Mask, 0x1002cef0): two-sided, opaque.
		info.multisample_state.enable_alpha_to_coverage = true;
		world.coverageMesh = build(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
			k_mesh_frag_spv, k_mesh_frag_spv_size, k_mesh_frag_msl, k_mesh_frag_msl_size, kMeshSamplers, 2, "coverage mesh");
		info.multisample_state.enable_alpha_to_coverage = false;
	}
	world.terrain = build(k_terrain_vert_spv, k_terrain_vert_spv_size, k_terrain_vert_msl,
		k_terrain_vert_msl_size, k_terrain_frag_spv, k_terrain_frag_spv_size, k_terrain_frag_msl,
		k_terrain_frag_msl_size, 16, 3, "terrain", 2);
	world.surrounding = build(k_terrain_vert_spv, k_terrain_vert_spv_size, k_terrain_vert_msl,
		k_terrain_vert_msl_size, k_surrounding_frag_spv, k_surrounding_frag_spv_size, k_surrounding_frag_msl,
		k_surrounding_frag_msl_size, 3, 2, "surrounding terrain", 2);
	// The undergrowth: Undergrowth.fx's every technique sets CullMode CW, so
	// Direct3D's front faces (clockwise on screen) are the ones culled. Its
	// meshes (Undergrowth.dat, made by the editor's .obj import 0x1010cb70,
	// which negates z) are wound the other way round from the static meshes:
	// their faces' normals by winding point inwards (79 of the 94 distinct
	// shipped meshes enclose a negative volume, every rock among them), and the
	// generator (0x1010ba40) turns them by a rotation and copies the indices
	// as they are. So the near side of a rock or a grass tuft is drawn and
	// its far side culled. The passes also set ZFunc Less (the other
	// pipelines keep less-or-equal), so of two tufts' cards at one depth the
	// first drawn stays.
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_FRONT;
	info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
	world.undergrowth = build(k_undergrowth_vert_spv, k_undergrowth_vert_spv_size, k_undergrowth_vert_msl,
		k_undergrowth_vert_msl_size, k_undergrowth_frag_spv, k_undergrowth_frag_spv_size, k_undergrowth_frag_msl,
		k_undergrowth_frag_msl_size, 5, 2, "undergrowth", 1, 2);
	if (!single) {
		info.multisample_state.enable_alpha_to_coverage = true;
		world.coverageUndergrowth = build(k_undergrowth_vert_spv, k_undergrowth_vert_spv_size, k_undergrowth_vert_msl,
			k_undergrowth_vert_msl_size, k_undergrowth_frag_spv, k_undergrowth_frag_spv_size, k_undergrowth_frag_msl,
			k_undergrowth_frag_msl_size, 5, 2, "coverage undergrowth", 1, 2);
		info.multisample_state.enable_alpha_to_coverage = false;
	}
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
	info.depth_stencil_state.enable_depth_test = false;
	info.depth_stencil_state.enable_depth_write = false;
	world.sky = build(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_sky_frag_spv, k_sky_frag_spv_size, k_sky_frag_msl, k_sky_frag_msl_size, 3, 1, "sky");
	info.depth_stencil_state.enable_depth_test = true;
	color.blend_state.enable_blend = true;
	color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
	color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
	color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
	info.depth_stencil_state.enable_depth_write = false;
	world.fadedMesh = build(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_mesh_frag_spv, k_mesh_frag_spv_size, k_mesh_frag_msl, k_mesh_frag_msl_size, kMeshSamplers, 2, "faded mesh");
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
	world.culledFadedMesh = build(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_mesh_frag_spv, k_mesh_frag_spv_size, k_mesh_frag_msl, k_mesh_frag_msl_size, kMeshSamplers, 2,
		"culled faded mesh");
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	world.particle = build(k_particle_vert_spv, k_particle_vert_spv_size, k_particle_vert_msl,
		k_particle_vert_msl_size, k_particle_frag_spv, k_particle_frag_spv_size, k_particle_frag_msl,
		k_particle_frag_msl_size, 1, 1, "particle");
	world.shimmer = build(k_particle_vert_spv, k_particle_vert_spv_size, k_particle_vert_msl,
		k_particle_vert_msl_size, k_shimmer_frag_spv, k_shimmer_frag_spv_size, k_shimmer_frag_msl,
		k_shimmer_frag_msl_size, 2, 1, "heat shimmer");
	color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
	world.additiveMesh = build(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_mesh_frag_spv, k_mesh_frag_spv_size, k_mesh_frag_msl, k_mesh_frag_msl_size, kMeshSamplers, 2, "additive mesh");
	world.additiveParticle = build(k_particle_vert_spv, k_particle_vert_spv_size, k_particle_vert_msl,
		k_particle_vert_msl_size, k_particle_frag_spv, k_particle_frag_spv_size, k_particle_frag_msl,
		k_particle_frag_msl_size, 1, 1, "additive particle");
	color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	{
		// Cloaked meshes into their mask: depth-tested, not written, unblended.
		color.blend_state.enable_blend = false;
		world.camo = build(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
			k_camo_frag_spv, k_camo_frag_spv_size, k_camo_frag_msl, k_camo_frag_msl_size, 0, 0, "camouflage");
		// The full-screen passes: no vertex buffers, no depth test.
		const SDL_GPUVertexInputState meshInput = info.vertex_input_state;
		info.vertex_input_state = {};
		info.depth_stencil_state.enable_depth_test = false;
		if (single) {
			m_postPipeline = build(k_fullscreen_vert_spv, k_fullscreen_vert_spv_size, k_fullscreen_vert_msl,
				k_fullscreen_vert_msl_size, k_post_frag_spv, k_post_frag_spv_size, k_post_frag_msl, k_post_frag_msl_size, 11, 1,
				"post-production", 0);
		}
		info.target_info.has_depth_stencil_target = false;
		if (single) {
			m_bloomPipeline = build(k_fullscreen_vert_spv, k_fullscreen_vert_spv_size, k_fullscreen_vert_msl,
				k_fullscreen_vert_msl_size, k_bloom_frag_spv, k_bloom_frag_spv_size, k_bloom_frag_msl, k_bloom_frag_msl_size, 1, 1,
				"bloom", 0);
		}
		// The shadow casters: depth only into the shadow maps' format.
		info.vertex_input_state = meshInput;
		info.target_info.num_color_targets = 0;
		info.target_info.has_depth_stencil_target = true;
		info.target_info.depth_stencil_format = m_shadowDepthFormat;
		info.depth_stencil_state.enable_depth_test = true;
		info.depth_stencil_state.enable_depth_write = true;
		info.rasterizer_state.enable_depth_bias = true;
		info.rasterizer_state.depth_bias_constant_factor = 1.0f;
		info.rasterizer_state.depth_bias_slope_factor = 2.0f;
		if (single) {
			m_shadowCasterPipeline = build(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
				k_shadowcaster_frag_spv, k_shadowcaster_frag_spv_size, k_shadowcaster_frag_msl, k_shadowcaster_frag_msl_size, 1,
				1, "shadow caster");
		}
		info.rasterizer_state.enable_depth_bias = false;
		info.rasterizer_state.depth_bias_constant_factor = 0;
		info.rasterizer_state.depth_bias_slope_factor = 0;
		info.target_info.num_color_targets = 1;
		info.target_info.depth_stencil_format = m_depthFormat;
		info.depth_stencil_state.enable_depth_write = false;
		color.blend_state.enable_blend = true;
	}
	// The HUD: blended over everything, no depth.
	info.depth_stencil_state.enable_depth_test = false;
	if (single) {
		m_uiPipeline = build(k_ui_vert_spv, k_ui_vert_spv_size, k_ui_vert_msl, k_ui_vert_msl_size, k_ui_frag_spv,
			k_ui_frag_spv_size, k_ui_frag_msl, k_ui_frag_msl_size, 1, 0, "ui");
	}
	{
		const auto stencil = [&](SDL_GPUCompareOp compare, SDL_GPUStencilOp pass, SDL_GPUStencilOp fail, bool colour,
								 const char* name) {
			SDL_GPUStencilOpState ops {};
			ops.compare_op = compare;
			ops.pass_op = pass;
			ops.fail_op = fail;
			ops.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
			info.depth_stencil_state.enable_stencil_test = true;
			info.depth_stencil_state.front_stencil_state = ops;
			info.depth_stencil_state.back_stencil_state = ops;
			info.depth_stencil_state.compare_mask = 0xff;
			info.depth_stencil_state.write_mask = 0xff;
			color.blend_state.enable_color_write_mask = !colour;
			color.blend_state.color_write_mask = 0;
			SDL_GPUGraphicsPipeline* pipeline = build(k_ui_vert_spv, k_ui_vert_spv_size, k_ui_vert_msl, k_ui_vert_msl_size,
				k_ui_frag_spv, k_ui_frag_spv_size, k_ui_frag_msl, k_ui_frag_msl_size, 1, 0, name);
			info.depth_stencil_state.enable_stencil_test = false;
			color.blend_state.enable_color_write_mask = false;
			return pipeline;
		};
		if (single) {
			m_uiSubmitPipeline = stencil(SDL_GPU_COMPAREOP_GREATER, SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP,
				SDL_GPU_STENCILOP_REPLACE, false, "ui mask");
			m_uiTestPipeline = stencil(SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_KEEP, true, "ui masked");
			m_uiClearPipeline = stencil(SDL_GPU_COMPAREOP_ALWAYS, SDL_GPU_STENCILOP_REPLACE, SDL_GPU_STENCILOP_REPLACE, false,
				"ui mask clear");
		}
	}
	info.depth_stencil_state.enable_depth_test = true;
	world.water = build(k_water_vert_spv, k_water_vert_spv_size, k_water_vert_msl, k_water_vert_msl_size,
		k_water_frag_spv, k_water_frag_spv_size, k_water_frag_msl, k_water_frag_msl_size, 4, 2, "water");
	// Roads: blended like water, pulled towards the camera over the terrain.
	info.rasterizer_state.enable_depth_bias = true;
	info.rasterizer_state.depth_bias_constant_factor = -1.0f;
	info.rasterizer_state.depth_bias_slope_factor = -1.0f;
	world.road = build(k_road_vert_spv, k_road_vert_spv_size, k_road_vert_msl, k_road_vert_msl_size,
		k_road_frag_spv, k_road_frag_spv_size, k_road_frag_msl, k_road_frag_msl_size, 5, 3, "road");
	// The road static meshes (RaShaderRoad*.fx): the same, back faces culled
	// (CullMode CCW with Direct3D's clockwise front faces).
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
	world.roadMesh = build(k_roadmesh_vert_spv, k_roadmesh_vert_spv_size, k_roadmesh_vert_msl,
		k_roadmesh_vert_msl_size, k_road_frag_spv, k_road_frag_spv_size, k_road_frag_msl, k_road_frag_msl_size, 5, 3,
		"road mesh");
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	world.decal = build(k_decal_vert_spv, k_decal_vert_spv_size, k_decal_vert_msl, k_decal_vert_msl_size,
		k_decal_frag_spv, k_decal_frag_spv_size, k_decal_frag_msl, k_decal_frag_msl_size, 2, 1, "decal");
	const bool worldMade = world.mesh && world.culledMesh && world.culledFadedMesh && world.surrounding && world.fadedMesh && world.additiveMesh && world.terrain && world.sky && world.water && world.road && world.roadMesh && world.decal &&
		world.undergrowth && world.particle && world.shimmer && world.additiveParticle && world.camo &&
		(single || (world.coverageMesh && world.coverageUndergrowth));
	const bool made = worldMade && (!single || (m_uiPipeline && m_uiSubmitPipeline && m_uiTestPipeline && m_uiClearPipeline &&
																		  m_shadowCasterPipeline && m_postPipeline && m_bloomPipeline));
	if (!made || !single) {
		return made;
	}
	// The enhanced mode's (not retail; Enhanced/LevelRendererEnhanced.cpp):
	// each left null when it can't be made, which turns its pass off, never
	// the renderer. Every state is set afresh here.
	std::string enhancedError;
	const auto enhanced = [&](auto&&... args) {
		SDL_GPUGraphicsPipeline* pipeline = build(args...);
		if (!pipeline && enhancedError.empty()) {
			enhancedError = error;
		}
		return pipeline;
	};
	const auto state = [&](bool blend, bool additive, bool depthWrite, SDL_GPUCullMode cull, float bias) {
		info.vertex_input_state.vertex_buffer_descriptions = &buffer;
		info.vertex_input_state.num_vertex_buffers = 1;
		info.vertex_input_state.vertex_attributes = attributes;
		info.vertex_input_state.num_vertex_attributes = 11;
		info.rasterizer_state = {};
		info.rasterizer_state.cull_mode = cull;
		info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
		info.rasterizer_state.enable_depth_bias = bias != 0;
		info.rasterizer_state.depth_bias_constant_factor = bias;
		info.rasterizer_state.depth_bias_slope_factor = bias;
		info.depth_stencil_state = {};
		info.depth_stencil_state.enable_depth_test = true;
		info.depth_stencil_state.enable_depth_write = depthWrite;
		info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
		color = {};
		color.format = m_colorFormat;
		color.blend_state.enable_blend = blend;
		color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		color.blend_state.dst_color_blendfactor = additive ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		info.target_info.color_target_descriptions = &color;
		info.target_info.num_color_targets = 1;
		info.target_info.depth_stencil_format = m_depthFormat;
		info.target_info.has_depth_stencil_target = true;
	};
	// The lit variants: as the retail pipelines above, one sampler more (the AO).
	state(false, false, true, SDL_GPU_CULLMODE_NONE, 0);
	m_enhanced.mesh = enhanced(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_enh_mesh_frag_spv, k_enh_mesh_frag_spv_size, k_enh_mesh_frag_msl, k_enh_mesh_frag_msl_size, kMeshSamplers + 1, 2,
		"enhanced mesh");
	m_enhanced.terrain = enhanced(k_terrain_vert_spv, k_terrain_vert_spv_size, k_terrain_vert_msl,
		k_terrain_vert_msl_size, k_enh_terrain_frag_spv, k_enh_terrain_frag_spv_size, k_enh_terrain_frag_msl,
		k_enh_terrain_frag_msl_size, 14, 3, "enhanced terrain", 2);
	m_enhanced.undergrowth = enhanced(k_undergrowth_vert_spv, k_undergrowth_vert_spv_size, k_undergrowth_vert_msl,
		k_undergrowth_vert_msl_size, k_enh_undergrowth_frag_spv, k_enh_undergrowth_frag_spv_size,
		k_enh_undergrowth_frag_msl, k_enh_undergrowth_frag_msl_size, 6, 2, "enhanced undergrowth", 1, 2);
	state(false, false, true, SDL_GPU_CULLMODE_BACK, 0);
	m_enhanced.meshCulled = enhanced(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_enh_mesh_frag_spv, k_enh_mesh_frag_spv_size, k_enh_mesh_frag_msl, k_enh_mesh_frag_msl_size, kMeshSamplers + 1, 2,
		"enhanced mesh (culled)");
	state(true, false, false, SDL_GPU_CULLMODE_NONE, 0);
	m_enhanced.fadedMesh = enhanced(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_enh_mesh_frag_spv, k_enh_mesh_frag_spv_size, k_enh_mesh_frag_msl, k_enh_mesh_frag_msl_size, kMeshSamplers + 1, 2,
		"enhanced faded mesh");
	state(true, true, false, SDL_GPU_CULLMODE_NONE, 0);
	m_enhanced.additiveMesh = enhanced(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_enh_mesh_frag_spv, k_enh_mesh_frag_spv_size, k_enh_mesh_frag_msl, k_enh_mesh_frag_msl_size, kMeshSamplers + 1, 2,
		"enhanced additive mesh");
	state(true, false, false, SDL_GPU_CULLMODE_NONE, -1.0f);
	m_enhanced.road = enhanced(k_road_vert_spv, k_road_vert_spv_size, k_road_vert_msl, k_road_vert_msl_size,
		k_enh_road_frag_spv, k_enh_road_frag_spv_size, k_enh_road_frag_msl, k_enh_road_frag_msl_size, 6, 3, "enhanced road");
	state(true, false, false, SDL_GPU_CULLMODE_BACK, -1.0f);
	m_enhanced.roadMesh = enhanced(k_roadmesh_vert_spv, k_roadmesh_vert_spv_size, k_roadmesh_vert_msl,
		k_roadmesh_vert_msl_size, k_enh_road_frag_spv, k_enh_road_frag_spv_size, k_enh_road_frag_msl,
		k_enh_road_frag_msl_size, 6, 3, "enhanced road mesh");
	// Depth only into the shadow maps' (sampleable) format: the prepass,
	// and the cascades' terrain casters with the casters' slope bias.
	state(false, false, true, SDL_GPU_CULLMODE_NONE, 0);
	info.target_info.num_color_targets = 0;
	info.target_info.depth_stencil_format = m_shadowDepthFormat;
	m_enhanced.depth = enhanced(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_shadowcaster_frag_spv, k_shadowcaster_frag_spv_size, k_shadowcaster_frag_msl, k_shadowcaster_frag_msl_size, 1, 1,
		"enhanced depth");
	m_enhanced.terrainDepth = enhanced(k_terrain_vert_spv, k_terrain_vert_spv_size, k_terrain_vert_msl,
		k_terrain_vert_msl_size, k_depth_frag_spv, k_depth_frag_spv_size, k_depth_frag_msl, k_depth_frag_msl_size, 0, 0,
		"enhanced terrain depth", 2);
	info.rasterizer_state.enable_depth_bias = true;
	info.rasterizer_state.depth_bias_constant_factor = 1.0f;
	info.rasterizer_state.depth_bias_slope_factor = 2.0f;
	m_enhanced.terrainCaster = enhanced(k_terrain_vert_spv, k_terrain_vert_spv_size, k_terrain_vert_msl,
		k_terrain_vert_msl_size, k_depth_frag_spv, k_depth_frag_spv_size, k_depth_frag_msl, k_depth_frag_msl_size, 0, 0,
		"enhanced terrain caster", 2);
	const auto renders = [&](SDL_GPUTextureFormat format) {
		return SDL_GPUTextureSupportsFormat(m_device, format, SDL_GPU_TEXTURETYPE_2D,
			SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
	};
	if (renders(kNormalDepthFormat) && renders(kMotionFormat) && renders(kAOFormat)) {
		// The moving meshes' motion: tested against the prepass, pulled a
		// little towards the camera (another vertex shader than its depth's).
		SDL_GPUColorTargetDescription motionTarget {};
		motionTarget.format = kMotionFormat;
		state(false, false, false, SDL_GPU_CULLMODE_NONE, -1.0f);
		info.target_info.color_target_descriptions = &motionTarget;
		info.target_info.depth_stencil_format = m_shadowDepthFormat;
		m_enhanced.motion = enhanced(k_motion_vert_spv, k_motion_vert_spv_size, k_motion_vert_msl, k_motion_vert_msl_size,
			k_motion_frag_spv, k_motion_frag_spv_size, k_motion_frag_msl, k_motion_frag_msl_size, 1, 1, "enhanced motion", 2);
		state(false, false, false, SDL_GPU_CULLMODE_NONE, 0);
		info.target_info.color_target_descriptions = &motionTarget;
		info.depth_stencil_state.enable_depth_test = false;
		info.target_info.has_depth_stencil_target = false;
		m_enhanced.motionOverlay = enhanced(k_motion_vert_spv, k_motion_vert_spv_size, k_motion_vert_msl,
			k_motion_vert_msl_size, k_motion_frag_spv, k_motion_frag_spv_size, k_motion_frag_msl, k_motion_frag_msl_size, 1,
			1, "enhanced motion overlay", 2);
		// The full-screen passes.
		info.vertex_input_state = {};
		SDL_GPUColorTargetDescription targets[2] {};
		targets[0].format = kNormalDepthFormat;
		targets[1].format = kMotionFormat;
		info.target_info.color_target_descriptions = targets;
		info.target_info.num_color_targets = 2;
		m_enhanced.gbuffer = enhanced(k_fullscreen_vert_spv, k_fullscreen_vert_spv_size, k_fullscreen_vert_msl,
			k_fullscreen_vert_msl_size, k_gbuffer_frag_spv, k_gbuffer_frag_spv_size, k_gbuffer_frag_msl,
			k_gbuffer_frag_msl_size, 1, 1, "enhanced g-buffer", 0);
		targets[0].format = kAOFormat;
		info.target_info.num_color_targets = 1;
		m_enhanced.gtao = enhanced(k_fullscreen_vert_spv, k_fullscreen_vert_spv_size, k_fullscreen_vert_msl,
			k_fullscreen_vert_msl_size, k_gtao_frag_spv, k_gtao_frag_spv_size, k_gtao_frag_msl, k_gtao_frag_msl_size, 1, 1,
			"enhanced gtao", 0);
		m_enhanced.aoBlur = enhanced(k_fullscreen_vert_spv, k_fullscreen_vert_spv_size, k_fullscreen_vert_msl,
			k_fullscreen_vert_msl_size, k_aoblur_frag_spv, k_aoblur_frag_spv_size, k_aoblur_frag_msl, k_aoblur_frag_msl_size,
			2, 1, "enhanced ao blur", 0);
		targets[0].format = m_colorFormat;
		m_enhanced.taa = enhanced(k_fullscreen_vert_spv, k_fullscreen_vert_spv_size, k_fullscreen_vert_msl,
			k_fullscreen_vert_msl_size, k_taa_frag_spv, k_taa_frag_spv_size, k_taa_frag_msl, k_taa_frag_msl_size, 4, 1,
			"enhanced taa", 0);
	}
	if (!enhancedError.empty()) {
		SDL_Log("enhanced mode: %s", enhancedError.c_str());
	}
	error.clear();
	return true;
}

void GpuContext::releaseWorldPipelines(WorldPipelines& world)
{
	for (SDL_GPUGraphicsPipeline* pipeline : {world.mesh, world.culledMesh, world.culledFadedMesh, world.terrain,
			 world.surrounding, world.sky, world.fadedMesh, world.additiveMesh, world.water, world.road, world.roadMesh,
			 world.decal, world.undergrowth, world.particle, world.shimmer, world.additiveParticle, world.camo,
			 world.coverageMesh, world.coverageUndergrowth}) {
		if (pipeline) {
			SDL_ReleaseGPUGraphicsPipeline(m_device, pipeline);
		}
	}
	world = {};
}

std::vector<uint32_t> GpuContext::sampleCounts() const
{
	std::vector<uint32_t> counts;
	for (const uint32_t count : {2u, 4u, 8u}) {
		if (SDL_GPUTextureSupportsSampleCount(m_device, m_colorFormat, sampleCountOf(count)) &&
			SDL_GPUTextureSupportsSampleCount(m_device, m_depthFormat, sampleCountOf(count))) {
			counts.push_back(count);
		}
	}
	return counts;
}

uint32_t GpuContext::setWorldSamples(uint32_t requested)
{
	// The most samples at or below the request both targets' formats take
	// (the original lists the device's multisample types, 0x1000fb60).
	uint32_t samples = 1;
	for (const uint32_t count : sampleCounts()) {
		if (count <= requested) {
			samples = count;
		}
	}
	if (samples > 1 && samples != m_multisampledCount) {
		releaseWorldPipelines(m_multisampled);
		std::string error;
		if (createPipelines(error, samples, m_multisampled)) {
			m_multisampledCount = samples;
		} else {
			SDL_Log("antialiasing (%u samples): %s", samples, error.c_str());
			releaseWorldPipelines(m_multisampled);
			m_multisampledCount = 1;
			samples = 1;
		}
	}
	m_worldSamples = samples;
	return samples;
}

SDL_GPUTexture* GpuContext::createMultisampleTarget(uint32_t width, uint32_t height, bool depth)
{
	SDL_GPUTextureCreateInfo info {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = depth ? m_depthFormat : m_colorFormat;
	info.usage = depth ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	info.width = std::max<uint32_t>(width, 1);
	info.height = std::max<uint32_t>(height, 1);
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = sampleCountOf(m_worldSamples);
	return SDL_CreateGPUTexture(m_device, &info);
}

namespace {

// Translate the HUD's Meme enum before using Direct3D's factor numbers.
bool blendFactor(int d3dBlend, SDL_GPUBlendFactor& factor)
{
	switch (hudBlendToD3D(d3dBlend)) {
	case 1: factor = SDL_GPU_BLENDFACTOR_ZERO; return true;
	case 2: factor = SDL_GPU_BLENDFACTOR_ONE; return true;
	case 3: factor = SDL_GPU_BLENDFACTOR_SRC_COLOR; return true;
	case 4: factor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR; return true;
	case 5: factor = SDL_GPU_BLENDFACTOR_SRC_ALPHA; return true;
	case 6: factor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; return true;
	case 7: factor = SDL_GPU_BLENDFACTOR_DST_ALPHA; return true;
	case 8: factor = SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA; return true;
	case 9: factor = SDL_GPU_BLENDFACTOR_DST_COLOR; return true;
	case 10: factor = SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR; return true;
	case 11: factor = SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE; return true;
	default: return false;
	}
}

} // namespace

SDL_GPUGraphicsPipeline* GpuContext::uiBlendPipeline(int source, int destination)
{
	if (source == 0 && destination == 0) {
		return m_uiPipeline;
	}
	const int key = source * 16 + destination;
	if (const auto it = m_uiBlendPipelines.find(key); it != m_uiBlendPipelines.end()) {
		return it->second;
	}
	SDL_GPUGraphicsPipeline* pipeline = nullptr;
	SDL_GPUBlendFactor src {}, dst {};
	if (blendFactor(source, src) && blendFactor(destination, dst)) {
		// uiPipeline()'s state (the mesh vertex layout, no depth, the
		// HUD pass's depth-stencil format) with the effect's factors.
		SDL_GPUVertexBufferDescription buffer {};
		buffer.slot = 0;
		buffer.pitch = sizeof(Vertex);
		buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
		const SDL_GPUVertexAttribute attributes[11] = {
			{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, position)},
			{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, normal)},
			{2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, texCoord)},
			{3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, lightmapCoord)},
			{4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(Vertex, part)},
			{5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(Vertex, part2)},
			{6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(Vertex, weight)},
			{7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, detailCoord)},
			{8, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, dirtCoord)},
			{9, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, crackCoord)},
			{10, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, tangent)},
		};
		SDL_GPUColorTargetDescription color {};
		color.format = m_colorFormat;
		color.blend_state.enable_blend = true;
		color.blend_state.src_color_blendfactor = src;
		color.blend_state.dst_color_blendfactor = dst;
		color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		color.blend_state.src_alpha_blendfactor = src;
		color.blend_state.dst_alpha_blendfactor = dst;
		color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		SDL_GPUGraphicsPipelineCreateInfo info {};
		info.vertex_input_state.vertex_buffer_descriptions = &buffer;
		info.vertex_input_state.num_vertex_buffers = 1;
		info.vertex_input_state.vertex_attributes = attributes;
		info.vertex_input_state.num_vertex_attributes = 11;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
		info.target_info.color_target_descriptions = &color;
		info.target_info.num_color_targets = 1;
		info.target_info.depth_stencil_format = m_depthFormat;
		info.target_info.has_depth_stencil_target = true;
		SDL_GPUShader* vertex = loadShader(m_device, SDL_GPU_SHADERSTAGE_VERTEX, k_ui_vert_spv, k_ui_vert_spv_size,
			k_ui_vert_msl, k_ui_vert_msl_size, 0, 1);
		SDL_GPUShader* fragment = loadShader(m_device, SDL_GPU_SHADERSTAGE_FRAGMENT, k_ui_frag_spv, k_ui_frag_spv_size,
			k_ui_frag_msl, k_ui_frag_msl_size, 1, 0);
		if (vertex && fragment) {
			info.vertex_shader = vertex;
			info.fragment_shader = fragment;
			pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
		}
		SDL_ReleaseGPUShader(m_device, vertex);
		SDL_ReleaseGPUShader(m_device, fragment);
	}
	m_uiBlendPipelines[key] = pipeline;
	return pipeline;
}

SDL_GPUTexture* GpuContext::createColorTexture(uint32_t width, uint32_t height)
{
	SDL_GPUTextureCreateInfo info {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = m_colorFormat;
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = std::max<uint32_t>(width, 1);
	info.height = std::max<uint32_t>(height, 1);
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	return SDL_CreateGPUTexture(m_device, &info);
}

SDL_GPUTexture* GpuContext::createRenderTexture(uint32_t width, uint32_t height, SDL_GPUTextureFormat format, bool depth)
{
	SDL_GPUTextureCreateInfo info {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = format;
	info.usage = (depth ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) |
		SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = std::max<uint32_t>(width, 1);
	info.height = std::max<uint32_t>(height, 1);
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	return SDL_CreateGPUTexture(m_device, &info);
}

SDL_GPUTexture* GpuContext::createShadowMap(uint32_t size)
{
	SDL_GPUTextureCreateInfo info {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = m_shadowDepthFormat;
	info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = info.height = std::max<uint32_t>(size, 1);
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	return SDL_CreateGPUTexture(m_device, &info);
}

SDL_GPUCopyPass* GpuContext::copyPass()
{
	if (!m_uploadPass) {
		m_uploadCommands = SDL_AcquireGPUCommandBuffer(m_device);
		m_uploadPass = SDL_BeginGPUCopyPass(m_uploadCommands);
	}
	return m_uploadPass;
}

void GpuContext::flushUploads()
{
	if (!m_uploadPass) {
		return;
	}
	SDL_EndGPUCopyPass(m_uploadPass);
	SDL_SubmitGPUCommandBuffer(m_uploadCommands);
	m_uploadPass = nullptr;
	m_uploadCommands = nullptr;
	for (SDL_GPUTransferBuffer* transfer : m_pendingTransfers) {
		SDL_ReleaseGPUTransferBuffer(m_device, transfer); // freed once the GPU is done with it
	}
	m_pendingTransfers.clear();
}

SDL_GPUBuffer* GpuContext::createBuffer(SDL_GPUBufferUsageFlags usage, const void* data, uint32_t size)
{
	SDL_GPUBufferCreateInfo info {};
	info.usage = usage;
	info.size = size;
	SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(m_device, &info);
	SDL_GPUTransferBufferCreateInfo transferInfo {};
	transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transferInfo.size = size;
	SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
	if (!buffer || !transfer) {
		return buffer;
	}
	std::memcpy(SDL_MapGPUTransferBuffer(m_device, transfer, false), data, size);
	SDL_UnmapGPUTransferBuffer(m_device, transfer);
	SDL_GPUTransferBufferLocation source {transfer, 0};
	SDL_GPUBufferRegion destination {buffer, 0, size};
	SDL_UploadToGPUBuffer(copyPass(), &source, &destination, false);
	m_pendingTransfers.push_back(transfer);
	return buffer;
}

void GpuContext::uploadDynamic(SDL_GPUBuffer*& buffer, SDL_GPUBufferUsageFlags usage, const void* data, uint32_t size)
{
	if (size == 0) {
		return;
	}
	auto it = buffer ? m_dynamicBuffers.find(buffer) : m_dynamicBuffers.end();
	if (it == m_dynamicBuffers.end() || it->second.capacity < size) {
		const uint32_t capacity = std::max(size, it == m_dynamicBuffers.end() ? 0u : it->second.capacity + it->second.capacity / 2);
		releaseDynamicBuffer(buffer);
		SDL_GPUBufferCreateInfo info {};
		info.usage = usage;
		info.size = capacity;
		buffer = SDL_CreateGPUBuffer(m_device, &info);
		SDL_GPUTransferBufferCreateInfo transferInfo {};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = capacity;
		SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
		if (!buffer || !transfer) {
			if (transfer) {
				SDL_ReleaseGPUTransferBuffer(m_device, transfer);
			}
			return;
		}
		it = m_dynamicBuffers.emplace(buffer, DynamicBuffer {capacity, transfer}).first;
	}
	void* mapped = SDL_MapGPUTransferBuffer(m_device, it->second.transfer, true);
	if (!mapped) {
		return;
	}
	std::memcpy(mapped, data, size);
	SDL_UnmapGPUTransferBuffer(m_device, it->second.transfer);
	SDL_GPUTransferBufferLocation source {it->second.transfer, 0};
	SDL_GPUBufferRegion destination {buffer, 0, size};
	SDL_UploadToGPUBuffer(copyPass(), &source, &destination, true);
}

void GpuContext::releaseDynamicBuffer(SDL_GPUBuffer*& buffer)
{
	if (!buffer) {
		return;
	}
	if (const auto it = m_dynamicBuffers.find(buffer); it != m_dynamicBuffers.end()) {
		SDL_ReleaseGPUTransferBuffer(m_device, it->second.transfer);
		m_dynamicBuffers.erase(it);
	}
	SDL_ReleaseGPUBuffer(m_device, buffer);
	buffer = nullptr;
}

SDL_GPUTexture* GpuContext::createTexture(const texture::DdsTexture& texture, bool cube)
{
	if (texture.faces.empty() || texture.cube != cube) {
		return nullptr;
	}
	SDL_GPUTextureCreateInfo info {};
	// Cube maps (the water's and the envmaps): six faces in Direct3D's
	// order, which SDL's cube layers share.
	info.type = texture.cube ? SDL_GPU_TEXTURETYPE_CUBE
		: texture.depth > 1  ? SDL_GPU_TEXTURETYPE_3D
							 : SDL_GPU_TEXTURETYPE_2D;
	info.format = gpuFormat(texture.format);
	info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = texture.width;
	info.height = texture.height;
	info.layer_count_or_depth = texture.cube ? 6 : texture.depth;
	info.num_levels = texture.levelCount();
	if (!SDL_GPUTextureSupportsFormat(m_device, info.format, info.type, info.usage)) {
		return nullptr;
	}
	SDL_GPUTexture* gpuTexture = SDL_CreateGPUTexture(m_device, &info);
	if (!gpuTexture) {
		return nullptr;
	}
	size_t total = 0;
	for (const auto& face : texture.faces) {
		for (const auto& level : face) {
			total += level.data.size();
		}
	}
	SDL_GPUTransferBufferCreateInfo transferInfo {};
	transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transferInfo.size = static_cast<uint32_t>(total);
	SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
	auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_device, transfer, false));
	size_t offset = 0;
	for (const auto& face : texture.faces) {
		for (const auto& level : face) {
			std::memcpy(mapped + offset, level.data.data(), level.data.size());
			offset += level.data.size();
		}
	}
	SDL_UnmapGPUTransferBuffer(m_device, transfer);
	offset = 0;
	for (size_t face = 0; face < texture.faces.size(); ++face) {
		for (uint32_t i = 0; i < texture.levelCount() && i < texture.faces[face].size(); ++i) {
			const auto& level = texture.faces[face][i];
			SDL_GPUTextureTransferInfo source {};
			source.transfer_buffer = transfer;
			source.offset = static_cast<uint32_t>(offset);
			SDL_GPUTextureRegion destination {};
			destination.texture = gpuTexture;
			destination.mip_level = i;
			destination.layer = static_cast<Uint32>(face);
			destination.w = level.width;
			destination.h = level.height;
			destination.d = level.depth;
			SDL_UploadToGPUTexture(copyPass(), &source, &destination, false);
			offset += level.data.size();
		}
	}
	m_pendingTransfers.push_back(transfer);
	return gpuTexture;
}

SDL_GPUTexture* GpuContext::createRgbaTexture(uint32_t width, uint32_t height, const void* pixels)
{
	texture::DdsTexture texture;
	texture.format = texture::TextureFormat::RGBA8;
	texture.width = width;
	texture.height = height;
	texture.faces.resize(1);
	texture.faces[0].push_back({width, height, {}});
	const auto* bytes = static_cast<const uint8_t*>(pixels);
	texture.faces[0][0].data.assign(bytes, bytes + static_cast<size_t>(width) * height * 4);
	return createTexture(texture);
}

void GpuContext::ensureDepth(Target& target, uint32_t width, uint32_t height)
{
	if (target.depth && target.width == width && target.height == height) {
		return;
	}
	if (target.depth) {
		SDL_ReleaseGPUTexture(m_device, target.depth);
	}
	SDL_GPUTextureCreateInfo info {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = m_depthFormat;
	info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	info.width = width;
	info.height = height;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	target.depth = SDL_CreateGPUTexture(m_device, &info);
	target.width = width;
	target.height = height;
}

GpuContext::Target GpuContext::createOffscreenTarget(uint32_t width, uint32_t height)
{
	Target target;
	SDL_GPUTextureCreateInfo info {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = m_colorFormat;
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = width;
	info.height = height;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	target.color = SDL_CreateGPUTexture(m_device, &info);
	ensureDepth(target, width, height);
	return target;
}

void GpuContext::releaseTarget(Target& target)
{
	if (target.color) {
		SDL_ReleaseGPUTexture(m_device, target.color);
	}
	if (target.depth) {
		SDL_ReleaseGPUTexture(m_device, target.depth);
	}
	target = {};
}

bool GpuContext::readPixels(const Target& target, std::vector<uint8_t>& rgba)
{
	if (!target.color) {
		return false;
	}
	const uint32_t size = target.width * target.height * 4;
	SDL_GPUTransferBufferCreateInfo transferInfo {};
	transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	transferInfo.size = size;
	SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
	SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(m_device);
	SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(commands);
	SDL_GPUTextureRegion source {};
	source.texture = target.color;
	source.w = target.width;
	source.h = target.height;
	source.d = 1;
	SDL_GPUTextureTransferInfo destination {};
	destination.transfer_buffer = transfer;
	SDL_DownloadFromGPUTexture(pass, &source, &destination);
	SDL_EndGPUCopyPass(pass);
	SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
	SDL_WaitForGPUFences(m_device, true, &fence, 1);
	SDL_ReleaseGPUFence(m_device, fence);
	const auto* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(m_device, transfer, false));
	rgba.assign(pixels, pixels + size);
	SDL_UnmapGPUTransferBuffer(m_device, transfer);
	SDL_ReleaseGPUTransferBuffer(m_device, transfer);
	if (m_colorFormat == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) {
		for (size_t i = 0; i < rgba.size(); i += 4) {
			std::swap(rgba[i], rgba[i + 2]);
		}
	}
	return true;
}

} // namespace dice::hfe::render
