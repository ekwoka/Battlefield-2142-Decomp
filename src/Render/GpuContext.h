// The GPU device and the pipelines the level renderer uses (SDL3 GPU API:
// Metal on macOS, Vulkan or Direct3D 12 elsewhere).
//
// This replaces RendDX9.dll's Direct3D 9 device: the pipelines of the
// level's passes (meshes, terrain, water, roads, decals, undergrowth,
// particles, the UI), the shadow maps' and the post-production's.
#pragma once

#include "RenderMath.h"
#include "Texture/DdsFile.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
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
constexpr int kMaxPointLights = 8;

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
	float parameters[4];     // alpha add, alpha height multiplier, specular power, has an env map
	float animation[4];      // waterScroll xy, WaterCycleTime, 1 with the volume (the 3D variants)
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
	float detailFade[4]; // unused, unused, has detail, unused
	float farTiling[4];  // the low-detail texture: side x, side y, top, y offset
	float farScale[4];   // its coordinates a metre across, has a low-detail texture, unused, a metre up
	float water[4];      // TerrainWaterColor, WaterHeight (below -1e8: no sea)
};

// terrain.vert's geomorph (TerrainShader_Shared.fx geoMorphPosition); the
// vertices carry their three deltas in metres and the LOD step their edge
// drops at.
struct TerrainMorphUniforms {
	float camera[4];   // x, unused, z, the height above the ground (CAMERAPOS.w - 1)
	float limits[4];   // NEARFARMORPHLIMITS (squared), 1 to morph, 1 to fade the detail by it
	float selector[4]; // MORPHDELTASELECTOR over the scale
	float adder[3][4]; // MORPHDELTAADDER by edge step
	float next[4];     // a LOD 0 cell's morph on towards LOD 2 (TerrainLod.h): LOD 1's limits, its weight
};

struct LightingUniforms {
	float sunDirection[4];
	float sunColor[4];
	float skyColor[4];
	float fogColor[4]; // w: fog end distance
	float material[4]; // x: alpha reference, 0 for opaque; y: 1 with a lightmap;
					   // z: mesh layers (1 detail, 2 dirt, 4 crack) plus the
					   // material's kind (kMaterialBlended, kMaterialAdditive);
					   // w: the object's fade, 0 for none (fadedMeshPipeline)
	float uvOffset[4] {}; // xy: added to the diffuse texture coordinates (mesh.frag)
	float tint[4] {1, 1, 1, 1}; // times the texture (mesh.frag: mesh particles' colour)
	// Point lights (mesh.frag, terrain.frag): count in x; each its position
	// and the range it is gone at (w), its colour and the range it is full
	// up to (w).
	float lightCount[4] {};
	float lightPosition[kMaxPointLights][4] {};
	float lightColor[kMaxPointLights][4] {};
	// Spot lights: the cone's axis (xyz, world space) and the cosine it is
	// gone outside (w; below -1 for a point light).
	float lightSpot[kMaxPointLights][4] {};
	// x: the cosine it is full inside; y: 1 when it lights lightmapped
	// surfaces too (affectLightmappedObjects).
	float lightExtra[kMaxPointLights][4] {};
	// mesh.frag's hemi map (RaShaderBM/SM): HemiMapConstantColor (rgb; a
	// Titan interior map's colour under the object, else 1) and the
	// object's HeightOverTerrain (w), per object (0x10041100's block
	// +0x1ec / +0x1e4).
	float hemiObject[4] {1, 1, 1, 0};
};

// The dynamic shadows and the hemi map (shadow.glsl's Shadows block).
struct ShadowUniforms {
	Mat4 shadowMatrix;           // world -> the maps' uv and depth
	float params[4] {};          // x: 1 on, y: a texel in uv, z: depth bias, w: 1 with the static map
	float hemiConstants[4] {};   // xy: corner (x, z), z: size (0 none), w: lerp bias
	float hemiSky[4] {};         // rgb: the sky colour, w: 1 for the sun's occlusion
	float extra[4] {0, 0.8f, 0, 0}; // x: shadowIntensityBias, y: the fade's start, z: 1 for the undergrowth
};

// The sky dome (sky.frag).
struct SkyUniforms {
	float domeCenter[4] {};   // xyz, w: the cloud layers
	float cloudOffsets[4] {}; // TEXOFFSET, TEXOFFSET2
	float cloudFade[4] {};    // start, length, CLOUDLERPFACTORS
	float domeScale[4] {1, 0, 0, 0};
};

// LightingUniforms::material[2] bits beside the layers.
constexpr float kMaterialBlended = 8;   // alpha from the texture (times the fade)
constexpr float kMaterialAdditive = 16; // unlit, added (additiveMeshPipeline)
constexpr float kMaterialHemi = 32;     // a bundled or skinned mesh: the hemi map's ambient

class GpuContext {
public:
	// `window` may be null to render offscreen only.
	static std::unique_ptr<GpuContext> create(SDL_Window* window, std::string& error);
	~GpuContext();

	SDL_GPUDevice* device() const { return m_device; }
	SDL_Window* window() const { return m_window; }
	SDL_GPUGraphicsPipeline* meshPipeline() const { return m_meshPipeline; }
	SDL_GPUGraphicsPipeline* terrainPipeline() const { return m_terrainPipeline; }
	// The surrounding terrain (secondary heightmaps): terrain.vert with
	// surrounding.frag.
	SDL_GPUGraphicsPipeline* surroundingPipeline() const { return m_surroundingPipeline; }
	// Mesh shaders without depth test or write, for the sky dome.
	SDL_GPUGraphicsPipeline* skyPipeline() const { return m_skyPipeline; }
	// Meshes fading out (LightingUniforms material.w their alpha): blended,
	// no depth writes.
	// Also blended materials (RendDX9's transparency queues): SRCALPHA /
	// INVSRCALPHA, no culling (CullMode 1, 0x100c3880).
	SDL_GPUGraphicsPipeline* fadedMeshPipeline() const { return m_fadedMeshPipeline; }
	// Additive materials (hasAdditiveAlpha, BMadditive): SRCALPHA / ONE,
	// depth-tested, not written.
	SDL_GPUGraphicsPipeline* additiveMeshPipeline() const { return m_additiveMeshPipeline; }
	// Alpha-blended, depth-tested but not written.
	SDL_GPUGraphicsPipeline* waterPipeline() const { return m_waterPipeline; }
	// Water's blending plus a depth bias, for road decals.
	SDL_GPUGraphicsPipeline* roadPipeline() const { return m_roadPipeline; }
	// The same state for decals (decal.vert / decal.frag).
	SDL_GPUGraphicsPipeline* decalPipeline() const { return m_decalPipeline; }
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
	// The HUD with the blend factors of a HudBuilder.addNodeBlendEffect
	// (Direct3D 9 D3DBLEND values, 1 ZERO .. 11 SRCALPHASAT, for colour and
	// alpha alike; 0 0 is uiPipeline()'s SRCALPHA / INVSRCALPHA), made the
	// first time a pair is asked for. Null when the pair can't be built.
	SDL_GPUGraphicsPipeline* uiBlendPipeline(int source, int destination);
	SDL_GPUGraphicsPipeline* particlePipeline(bool additive) const
	{
		return additive ? m_additiveParticlePipeline : m_particlePipeline;
	}
	// The shadow maps' casters (mesh.vert, shadowcaster.frag): depth only,
	// into shadowDepthFormat(), biased by the slope, two-sided.
	SDL_GPUGraphicsPipeline* shadowCasterPipeline() const { return m_shadowCasterPipeline; }
	// Cloaked meshes into the camouflage mask (camo.frag): depth-tested
	// against the world's depth, not written.
	SDL_GPUGraphicsPipeline* camoPipeline() const { return m_camoPipeline; }
	// The post-production (fullscreen.vert, post.frag) into a target with
	// the HUD's depth-stencil; the bloom's filters (bloom.frag) into colour
	// only.
	SDL_GPUGraphicsPipeline* postPipeline() const { return m_postPipeline; }
	SDL_GPUGraphicsPipeline* bloomPipeline() const { return m_bloomPipeline; }
	SDL_GPUTextureFormat shadowDepthFormat() const { return m_shadowDepthFormat; }
	// A comparison sampler for the shadow maps (lit where the reference is
	// at most the stored depth), filtered.
	SDL_GPUSampler* shadowSampler() const { return m_shadowSampler; }
	// A 1 x 1 shadow map cleared to the far plane (bound when there are no shadows).
	SDL_GPUTexture* emptyShadowMap() const { return m_emptyShadowMap; }
	// A sampled colour texture of the swapchain's format (post-production targets).
	SDL_GPUTexture* createColorTexture(uint32_t width, uint32_t height);
	// A sampled depth texture of shadowDepthFormat().
	SDL_GPUTexture* createShadowMap(uint32_t size);
	SDL_GPUSampler* repeatSampler() const { return m_repeatSampler; }
	SDL_GPUSampler* clampSampler() const { return m_clampSampler; }
	SDL_GPUTexture* whiteTexture() const { return m_white; }
	SDL_GPUTexture* blackTexture() const { return m_black; }
	SDL_GPUTexture* whiteCubeTexture() const { return m_whiteCube; }
	SDL_GPUTexture* whiteVolumeTexture() const { return m_whiteVolume; }
	SDL_GPUTextureFormat colorFormat() const { return m_colorFormat; }
	// Depth with a stencil (the menus' masks): D32_FLOAT_S8_UINT, else
	// D24_UNORM_S8_UINT (SDL guarantees one of them).
	SDL_GPUTextureFormat depthFormat() const { return m_depthFormat; }

	// Uploads are recorded into one copy pass and submitted by flushUploads().
	SDL_GPUBuffer* createBuffer(SDL_GPUBufferUsageFlags usage, const void* data, uint32_t size);
	// A 2D texture, or with `cube` a cube map (null if the file is the other kind).
	SDL_GPUTexture* createTexture(const texture::DdsTexture& texture, bool cube = false);
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
	SDL_GPUGraphicsPipeline* m_surroundingPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_skyPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_fadedMeshPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_additiveMeshPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_waterPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_roadPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_decalPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_undergrowthPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_particlePipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_additiveParticlePipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_uiPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_uiSubmitPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_uiTestPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_uiClearPipeline = nullptr;
	std::map<int, SDL_GPUGraphicsPipeline*> m_uiBlendPipelines; // by source * 16 + destination
	SDL_GPUGraphicsPipeline* m_shadowCasterPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_camoPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_postPipeline = nullptr;
	SDL_GPUGraphicsPipeline* m_bloomPipeline = nullptr;
	SDL_GPUTextureFormat m_shadowDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	SDL_GPUSampler* m_shadowSampler = nullptr;
	SDL_GPUTexture* m_emptyShadowMap = nullptr;
	SDL_GPUTextureFormat m_depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
	SDL_GPUSampler* m_repeatSampler = nullptr;
	SDL_GPUSampler* m_clampSampler = nullptr;
	SDL_GPUTexture* m_white = nullptr;
	SDL_GPUTexture* m_black = nullptr;
	SDL_GPUTexture* m_whiteCube = nullptr;
	SDL_GPUTexture* m_whiteVolume = nullptr;
	SDL_GPUCommandBuffer* m_uploadCommands = nullptr;
	SDL_GPUCopyPass* m_uploadPass = nullptr;
	std::vector<SDL_GPUTransferBuffer*> m_pendingTransfers;
};

} // namespace dice::hfe::render
