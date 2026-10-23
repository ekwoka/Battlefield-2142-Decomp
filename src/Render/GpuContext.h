// The GPU device and the pipelines the level renderer uses (SDL3 GPU API:
// Metal on macOS, Vulkan or Direct3D 12 elsewhere).
//
// This replaces RendDX9.dll's Direct3D 9 device. Only what drawing a level
// needs is here so far: textured meshes with alpha test, and terrain.
#pragma once

#include "RenderMath.h"
#include "Texture/DdsFile.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::render {

// The vertex format of both pipelines.
struct Vertex {
	float position[3];
	float normal[3];
	float texCoord[2];
	float lightmapCoord[2];
	float part;   // bundled-mesh part or skinned-mesh bone index, 0 otherwise
	float part2;  // second bone of skinned meshes (= part otherwise)
	float weight; // weight of `part`; 1 - weight goes to `part2`
	// Static meshes' detail, dirt and crack layer coordinates (texture
	// coordinate sets 1-3).
	float detailCoord[2];
	float dirtCoord[2];
	float crackCoord[2];
};

// Uniform blocks (std140-compatible: only vec4 and mat4 members).
constexpr int kMaxMeshParts = 64;

struct TransformUniforms {
	Mat4 worldViewProjection;
	Mat4 world;
	float lightmapTransform[4] {1, 1, 0, 0}; // scale xy, offset zw into the atlas
	Mat4 parts[kMaxMeshParts];              // bundled-mesh part transforms
};

// Water fragment shader, second uniform block.
struct WaterUniforms {
	float cameraPosition[4]; // w: time in seconds
	float color[4];
	float specularColor[4];  // w: intensity
	float parameters[4];     // alpha add, alpha height multiplier, specular power
};

// Particle vertex shader uniforms.
struct ParticleUniforms {
	Mat4 viewProjection;
	float cameraRight[4];
	float cameraUp[4];
};

// Undergrowth vertex shader uniforms.
struct UndergrowthUniforms {
	Mat4 viewProjection;
	float cameraPosition[4];
	float fade[4];            // view distance, fade factor
	float terrainTexCoord[4]; // world xz -> terrain patch texture: scale xy, offset zw
	float swayOffsets[16][4];
};

// Road fragment shader, second uniform block.
struct RoadUniforms {
	float road[4]; // blend factor, fade start, fade end distance, unused
};

// Terrain fragment shader, second uniform block.
struct TerrainDetailUniforms {
	float tiling[8];     // repeats across a patch, per detail layer
	float detailFade[4]; // start, end distance, has detail, unused
};

struct LightingUniforms {
	float sunDirection[4];
	float sunColor[4];
	float skyColor[4];
	float fogColor[4]; // w: fog end distance
	float material[4]; // x: alpha reference, 0 for opaque; y: 1 with a lightmap;
					   // z: mesh layers (1 detail, 2 dirt, 4 crack);
					   // w: the object's fade, 0 for none (fadedMeshPipeline)
};

class GpuContext {
public:
	// `window` may be null to render offscreen only.
	static std::unique_ptr<GpuContext> create(SDL_Window* window, std::string& error);
	~GpuContext();

	SDL_GPUDevice* device() const { return m_device; }
	SDL_Window* window() const { return m_window; }
	SDL_GPUGraphicsPipeline* meshPipeline() const { return m_meshPipeline; }
	SDL_GPUGraphicsPipeline* terrainPipeline() const { return m_terrainPipeline; }
	// Mesh shaders without depth test or write, for the sky dome.
	SDL_GPUGraphicsPipeline* skyPipeline() const { return m_skyPipeline; }
	// Meshes fading out (LightingUniforms material.w their alpha): blended,
	// no depth writes.
	SDL_GPUGraphicsPipeline* fadedMeshPipeline() const { return m_fadedMeshPipeline; }
	// Alpha-blended, depth-tested but not written.
	SDL_GPUGraphicsPipeline* waterPipeline() const { return m_waterPipeline; }
	// Water's blending plus a depth bias, for road decals.
	SDL_GPUGraphicsPipeline* roadPipeline() const { return m_roadPipeline; }
	// Opaque, alpha-tested; undergrowth uniforms in the vertex stage.
	SDL_GPUGraphicsPipeline* undergrowthPipeline() const { return m_undergrowthPipeline; }
	// Particles: depth-tested, not written; blended or added.
	SDL_GPUGraphicsPipeline* uiPipeline() const { return m_uiPipeline; }
	// The UI with gameswf's mask states (RendDX9.dll 0x10118920 /
	// 0x10118a10 / 0x10118aa0), the reference being the mask level:
	// submitting a mask (no colour; where the reference is GREATER than
	// the stencil it INCRements, elsewhere REPLACEs it), drawing inside one
	// (only where the stencil EQUALs the reference), and clearing the
	// stencil to the reference (no colour).
	enum class UiStencil { Off, Submit, Test, Clear };
	SDL_GPUGraphicsPipeline* uiPipeline(UiStencil mode) const
	{
		return mode == UiStencil::Submit ? m_uiSubmitPipeline
			: mode == UiStencil::Test    ? m_uiTestPipeline
			: mode == UiStencil::Clear   ? m_uiClearPipeline
										 : m_uiPipeline;
	}
	SDL_GPUGraphicsPipeline* particlePipeline(bool additive) const
	{
		return additive ? m_additiveParticlePipeline : m_particlePipeline;
	}
	SDL_GPUSampler* repeatSampler() const { return m_repeatSampler; }
	SDL_GPUSampler* clampSampler() const { return m_clampSampler; }
	SDL_GPUTexture* whiteTexture() const { return m_white; }
	SDL_GPUTexture* blackTexture() const { return m_black; }
	SDL_GPUTextureFormat colorFormat() const { return m_colorFormat; }
	// Depth with a stencil (the menus' masks): D32_FLOAT_S8_UINT, else
	// D24_UNORM_S8_UINT (SDL guarantees one of them).
	SDL_GPUTextureFormat depthFormat() const { return m_depthFormat; }

	// Uploads are recorded into one copy pass and submitted by flushUploads().
	SDL_GPUBuffer* createBuffer(SDL_GPUBufferUsageFlags usage, const void* data, uint32_t size);
	SDL_GPUTexture* createTexture(const texture::DdsTexture& texture);
	SDL_GPUTexture* createRgbaTexture(uint32_t width, uint32_t height, const void* pixels);
	void flushUploads();

	// A render target (color + depth) of the given size, recreated on resize.
	struct Target {
		SDL_GPUTexture* color = nullptr; // null when drawing to the swapchain
		SDL_GPUTexture* depth = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
	};
	void ensureDepth(Target& target, uint32_t width, uint32_t height);
	Target createOffscreenTarget(uint32_t width, uint32_t height);
	void releaseTarget(Target& target);

	// Copies an offscreen target's color to RGBA8 pixels (waits for the GPU).
	bool readPixels(const Target& target, std::vector<uint8_t>& rgba);

private:
	GpuContext() = default;
	bool createPipelines(std::string& error);
	SDL_GPUCopyPass* copyPass();

	SDL_GPUDevice* m_device = nullptr;
	SDL_Window* m_window = nullptr;
	SDL_GPUTextureFormat m_colorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	SDL_GPUGraphicsPipeline* m_meshPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_terrainPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_skyPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_fadedMeshPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_waterPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_roadPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_undergrowthPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_particlePipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_additiveParticlePipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_uiPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_uiSubmitPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_uiTestPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_uiClearPipeline = nullptr;
	SDL_GPUTextureFormat m_depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
	SDL_GPUSampler* m_repeatSampler = nullptr;
	SDL_GPUSampler* m_clampSampler = nullptr;
	SDL_GPUTexture* m_white = nullptr;
	SDL_GPUTexture* m_black = nullptr;
	SDL_GPUCommandBuffer* m_uploadCommands = nullptr;
	SDL_GPUCopyPass* m_uploadPass = nullptr;
	std::vector<SDL_GPUTransferBuffer*> m_pendingTransfers;
};

} // namespace dice::hfe::render
