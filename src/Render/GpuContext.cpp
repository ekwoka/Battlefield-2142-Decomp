#include "GpuContext.h"

#include "EmbeddedShaders.h"

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
	if (!context->createPipelines(error)) {
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

	const uint32_t white = 0xffffffff;
	context->m_white = context->createRgbaTexture(1, 1, &white);
	const uint32_t black = 0xff000000;
	context->m_black = context->createRgbaTexture(1, 1, &black);
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
	SDL_ReleaseGPUSampler(m_device, m_repeatSampler);
	SDL_ReleaseGPUSampler(m_device, m_clampSampler);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_meshPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_terrainPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_skyPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_waterPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_roadPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_undergrowthPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_particlePipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_additiveParticlePipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_uiPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_uiSubmitPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_uiTestPipeline);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_uiClearPipeline);
	if (m_window) {
		SDL_ReleaseWindowFromGPUDevice(m_device, m_window);
	}
	SDL_DestroyGPUDevice(m_device);
}

bool GpuContext::createPipelines(std::string& error)
{
	m_depthFormat = SDL_GPUTextureSupportsFormat(m_device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
						SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
		? SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT
		: SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
	SDL_GPUVertexBufferDescription buffer {};
	buffer.slot = 0;
	buffer.pitch = sizeof(Vertex);
	buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	SDL_GPUVertexAttribute attributes[10] = {
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
	};

	SDL_GPUColorTargetDescription color {};
	color.format = m_colorFormat;

	SDL_GPUGraphicsPipelineCreateInfo info {};
	info.vertex_input_state.vertex_buffer_descriptions = &buffer;
	info.vertex_input_state.num_vertex_buffers = 1;
	info.vertex_input_state.vertex_attributes = attributes;
	info.vertex_input_state.num_vertex_attributes = 10;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	// Front faces are clockwise as in Direct3D; vegetation is two-sided, so
	// nothing is culled yet (the original takes this from the material).
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
	info.depth_stencil_state.enable_depth_test = true;
	info.depth_stencil_state.enable_depth_write = true;
	info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
	info.target_info.color_target_descriptions = &color;
	info.target_info.num_color_targets = 1;
	info.target_info.depth_stencil_format = m_depthFormat;
	info.target_info.has_depth_stencil_target = true;

	const auto build = [&](const unsigned char* vs, size_t vsSize, const unsigned char* vsMsl, size_t vsMslSize,
						   const unsigned char* fs, size_t fsSize, const unsigned char* fsMsl, size_t fsMslSize,
						   uint32_t fragmentSamplers, uint32_t fragmentUniforms,
						   const char* name) -> SDL_GPUGraphicsPipeline* {
		SDL_GPUShader* vertex = loadShader(m_device, SDL_GPU_SHADERSTAGE_VERTEX, vs, vsSize, vsMsl, vsMslSize, 0, 1);
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
	m_meshPipeline = build(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_mesh_frag_spv, k_mesh_frag_spv_size, k_mesh_frag_msl, k_mesh_frag_msl_size, 5, 1, "mesh");
	m_terrainPipeline = build(k_terrain_vert_spv, k_terrain_vert_spv_size, k_terrain_vert_msl,
		k_terrain_vert_msl_size, k_terrain_frag_spv, k_terrain_frag_spv_size, k_terrain_frag_msl,
		k_terrain_frag_msl_size, 10, 2, "terrain");
	m_undergrowthPipeline = build(k_undergrowth_vert_spv, k_undergrowth_vert_spv_size, k_undergrowth_vert_msl,
		k_undergrowth_vert_msl_size, k_undergrowth_frag_spv, k_undergrowth_frag_spv_size, k_undergrowth_frag_msl,
		k_undergrowth_frag_msl_size, 3, 1, "undergrowth");
	info.depth_stencil_state.enable_depth_test = false;
	info.depth_stencil_state.enable_depth_write = false;
	m_skyPipeline = build(k_mesh_vert_spv, k_mesh_vert_spv_size, k_mesh_vert_msl, k_mesh_vert_msl_size,
		k_mesh_frag_spv, k_mesh_frag_spv_size, k_mesh_frag_msl, k_mesh_frag_msl_size, 5, 1, "sky");
	info.depth_stencil_state.enable_depth_test = true;
	color.blend_state.enable_blend = true;
	color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
	color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
	color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
	info.depth_stencil_state.enable_depth_write = false;
	m_particlePipeline = build(k_particle_vert_spv, k_particle_vert_spv_size, k_particle_vert_msl,
		k_particle_vert_msl_size, k_particle_frag_spv, k_particle_frag_spv_size, k_particle_frag_msl,
		k_particle_frag_msl_size, 1, 1, "particle");
	color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
	m_additiveParticlePipeline = build(k_particle_vert_spv, k_particle_vert_spv_size, k_particle_vert_msl,
		k_particle_vert_msl_size, k_particle_frag_spv, k_particle_frag_spv_size, k_particle_frag_msl,
		k_particle_frag_msl_size, 1, 1, "additive particle");
	color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	// The HUD: blended over everything, no depth.
	info.depth_stencil_state.enable_depth_test = false;
	m_uiPipeline = build(k_ui_vert_spv, k_ui_vert_spv_size, k_ui_vert_msl, k_ui_vert_msl_size, k_ui_frag_spv,
		k_ui_frag_spv_size, k_ui_frag_msl, k_ui_frag_msl_size, 1, 0, "ui");
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
		m_uiSubmitPipeline = stencil(SDL_GPU_COMPAREOP_GREATER, SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP,
			SDL_GPU_STENCILOP_REPLACE, false, "ui mask");
		m_uiTestPipeline = stencil(SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_KEEP, true, "ui masked");
		m_uiClearPipeline = stencil(SDL_GPU_COMPAREOP_ALWAYS, SDL_GPU_STENCILOP_REPLACE, SDL_GPU_STENCILOP_REPLACE, false,
			"ui mask clear");
	}
	info.depth_stencil_state.enable_depth_test = true;
	m_waterPipeline = build(k_water_vert_spv, k_water_vert_spv_size, k_water_vert_msl, k_water_vert_msl_size,
		k_water_frag_spv, k_water_frag_spv_size, k_water_frag_msl, k_water_frag_msl_size, 0, 2, "water");
	// Roads: blended like water, pulled towards the camera over the terrain.
	info.rasterizer_state.enable_depth_bias = true;
	info.rasterizer_state.depth_bias_constant_factor = -1.0f;
	info.rasterizer_state.depth_bias_slope_factor = -1.0f;
	m_roadPipeline = build(k_road_vert_spv, k_road_vert_spv_size, k_road_vert_msl, k_road_vert_msl_size,
		k_road_frag_spv, k_road_frag_spv_size, k_road_frag_msl, k_road_frag_msl_size, 3, 2, "road");
	return m_meshPipeline && m_terrainPipeline && m_skyPipeline && m_waterPipeline && m_roadPipeline &&
		m_undergrowthPipeline && m_particlePipeline && m_additiveParticlePipeline && m_uiPipeline && m_uiSubmitPipeline &&
		m_uiTestPipeline && m_uiClearPipeline;
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

SDL_GPUTexture* GpuContext::createTexture(const texture::DdsTexture& texture)
{
	if (texture.cube || texture.faces.empty()) {
		return nullptr;
	}
	SDL_GPUTextureCreateInfo info {};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = gpuFormat(texture.format);
	info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = texture.width;
	info.height = texture.height;
	info.layer_count_or_depth = 1;
	info.num_levels = texture.levelCount();
	if (!SDL_GPUTextureSupportsFormat(m_device, info.format, info.type, info.usage)) {
		return nullptr;
	}
	SDL_GPUTexture* gpuTexture = SDL_CreateGPUTexture(m_device, &info);
	if (!gpuTexture) {
		return nullptr;
	}
	size_t total = 0;
	for (const auto& level : texture.faces[0]) {
		total += level.data.size();
	}
	SDL_GPUTransferBufferCreateInfo transferInfo {};
	transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transferInfo.size = static_cast<uint32_t>(total);
	SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
	auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_device, transfer, false));
	size_t offset = 0;
	for (const auto& level : texture.faces[0]) {
		std::memcpy(mapped + offset, level.data.data(), level.data.size());
		offset += level.data.size();
	}
	SDL_UnmapGPUTransferBuffer(m_device, transfer);
	offset = 0;
	for (uint32_t i = 0; i < texture.levelCount(); ++i) {
		const auto& level = texture.faces[0][i];
		SDL_GPUTextureTransferInfo source {};
		source.transfer_buffer = transfer;
		source.offset = static_cast<uint32_t>(offset);
		SDL_GPUTextureRegion destination {};
		destination.texture = gpuTexture;
		destination.mip_level = i;
		destination.w = level.width;
		destination.h = level.height;
		destination.d = 1;
		SDL_UploadToGPUTexture(copyPass(), &source, &destination, false);
		offset += level.data.size();
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
