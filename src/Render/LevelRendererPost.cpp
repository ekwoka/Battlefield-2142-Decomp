// LevelRenderer's dynamic shadows, hemi map, sky layers and
// post-production (see LevelRenderer.h and PostProduction.h).
#include "LevelRenderer.h"
#include "MeshQueues.h"

#include "IO/File/FileManager.h"
#include "Scene/LightSettings.h"
#include "Scene/PostProductionSettings.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace dice::hfe::render {

namespace {

// An orthographic projection over [-r, r] across and [0, depth] along the
// view (Direct3D's depth 0..1).
Mat4 orthographic(float radius, float depth)
{
	Mat4 r;
	r.m = {1 / radius, 0, 0, 0, 0, 1 / radius, 0, 0, 0, 0, 1 / depth, 0, 0, 0, 0, 1};
	return r;
}

// Clip space to the maps' texture coordinates (v down) and depth.
const Mat4 kClipToTexture {{0.5f, 0, 0, 0, 0, -0.5f, 0, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0, 1}};

// The light's depth range: far enough above the region for the tallest
// casters (Titans hover some 100 m up) and below it for the ground.
constexpr float kShadowHeadroom = 400;

// The region's half side by quality (VideoSettings 1 low, 2 medium, 3
// high) over shadowMapDepthDistance (40 m), and the map's size.
float shadowRadius(const scene::ShadowSettings& s)
{
	const float scale = s.quality >= 3 ? 2.0f : s.quality == 2 ? 1.5f : 1.0f;
	return std::max(s.shadowMapDepthDistance, 10.0f) * scale;
}

uint32_t shadowMapSize(const scene::ShadowSettings& s)
{
	const int size = s.quality >= 3 ? s.shadowMapSize * 2 : s.shadowMapSize;
	return static_cast<uint32_t>(std::clamp(size, 256, 4096));
}

} // namespace

// ---- poses -----------------------------------------------------------------

const std::vector<anim::Matrix>& LevelRenderer::poseFor(const Instance& instance, float seconds)
{
	const auto found = m_poses.find(&instance);
	if (found != m_poses.end()) {
		return found->second;
	}
	std::vector<anim::Matrix>& pose = m_poses[&instance];
	if (instance.skeleton
		&& !(m_sources && m_sources->skeletonPose && instance.object != 0
			&& m_sources->skeletonPose(instance.object, *instance.skeleton, pose))) {
		pose = anim::evaluatePose(*instance.skeleton, instance.animation.get(), seconds * anim::kFramesPerSecond);
	}
	return pose;
}

void LevelRenderer::skinPalette(const GpuMesh& mesh, const Submesh& submesh, const std::vector<anim::Matrix>& pose,
	TransformUniforms& transforms)
{
	// Bone palette of this material's rig: mesh -> bone -> world.
	const auto& rig = mesh.rigs[static_cast<size_t>(submesh.rig)];
	for (size_t b = 0; b < rig.size() && b < static_cast<size_t>(kMaxMeshParts); ++b) {
		const int node = rig[b].node;
		const anim::Matrix skin = node >= 0 && static_cast<size_t>(node) < pose.size()
			? anim::multiply(rig[b].meshToBone, pose[static_cast<size_t>(node)])
			: rig[b].meshToBone;
		std::copy(skin.begin(), skin.end(), transforms.parts[b].m.begin());
	}
}

// ---- the hemi map ------------------------------------------------------------

void LevelRenderer::loadHemiMap(const LevelSources& sources)
{
	m_hemiMap = nullptr;
	if (!sources.hemi || sources.hemi->baseHemiMap.empty() || sources.hemi->size <= 0) {
		return;
	}
	m_hemiMap = texture(sources.files, sources.hemi->baseHemiMap + ".dds");
	m_stats.hemiMap = m_hemiMap != nullptr;
}

// ---- the dynamic shadows -------------------------------------------------------

void LevelRenderer::useShadows(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, uint32_t slot,
	uint32_t firstSampler, bool hemi, bool undergrowth)
{
	ShadowUniforms uniforms = m_shadowUniforms;
	uniforms.extra[2] = undergrowth ? 1.0f : 0.0f;
	SDL_PushGPUFragmentUniformData(commands, slot, &uniforms, sizeof(uniforms));
	SDL_GPUTexture* empty = m_gpu.emptyShadowMap();
	SDL_GPUTextureSamplerBinding samplers[3] = {
		{m_shadow.enabled && m_shadowDynamic ? m_shadowDynamic : empty, m_gpu.shadowSampler()},
		{m_shadow.enabled && m_shadowStatic && m_shadowStaticValid ? m_shadowStatic : empty, m_gpu.shadowSampler()},
		{m_hemiMap ? m_hemiMap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
	};
	SDL_BindGPUFragmentSamplers(pass, firstSampler, samplers, hemi ? 3 : 2);
}

// RendDX9 fits a perspective (LSPSM/TSM) shadow map to the view each frame
// (the light manager's shadow frustums, 0x1012b190..); this keeps an
// orthographic one over a square region along the sun, centred ahead of the
// camera and snapped to a quarter of its side so the static map is redrawn
// only when the camera has moved that far.
void LevelRenderer::renderShadows(SDL_GPUCommandBuffer* commands, const Camera& camera,
	const scene::LightSettings& lights, float seconds)
{
	const scene::ShadowSettings defaults;
	const scene::ShadowSettings& settings = m_sources && m_sources->shadows ? *m_sources->shadows : defaults;
	const ShadowUniforms previous = m_shadowUniforms;
	m_shadowUniforms = ShadowUniforms {};
	// The hemi map's constants go with the shadows' (one block).
	if (m_hemiMap && m_sources && m_sources->hemi) {
		const scene::HemiMapSettings& hemi = *m_sources->hemi;
		m_shadowUniforms.hemiConstants[0] = hemi.offset[0];
		m_shadowUniforms.hemiConstants[1] = hemi.offset[2];
		m_shadowUniforms.hemiConstants[2] = hemi.size;
		m_shadowUniforms.hemiConstants[3] = lights.hemiLerpBias;
		m_shadowUniforms.hemiSky[0] = lights.skyColor[0];
		m_shadowUniforms.hemiSky[1] = lights.skyColor[1];
		m_shadowUniforms.hemiSky[2] = lights.skyColor[2];
		m_shadowUniforms.hemiSky[3] = 1;
	}
	m_shadow.enabled = settings.dynamicShadowsEnabled && settings.quality > 0 && lights.enableSun &&
		m_gpu.emptyShadowMap() != nullptr;
	if (!m_shadow.enabled) {
		return;
	}
	const uint32_t size = shadowMapSize(settings);
	if (size != m_shadowMapSize) {
		SDL_GPUDevice* device = m_gpu.device();
		if (m_shadowDynamic) {
			SDL_ReleaseGPUTexture(device, m_shadowDynamic);
		}
		if (m_shadowStatic) {
			SDL_ReleaseGPUTexture(device, m_shadowStatic);
		}
		m_shadowDynamic = m_gpu.createShadowMap(size);
		m_shadowStatic = m_gpu.createShadowMap(size);
		m_shadowMapSize = size;
		m_shadowStaticValid = false;
		m_shadowDrawn = false;
	}
	if (!m_shadowDynamic || !m_shadowStatic) {
		m_shadow.enabled = false;
		return;
	}
	// shadowManager.shadowUpdatesEnabled 0 (LM +0x755, read by the shadow
	// pass 0x10127420): the maps aren't redrawn and keep what they hold,
	// with their projection. (shadowUpdateFactor, +0x750, has no reader.)
	if (!settings.shadowUpdatesEnabled && m_shadowDrawn) {
		m_shadowUniforms.shadowMatrix = previous.shadowMatrix;
		std::copy(std::begin(previous.params), std::end(previous.params), m_shadowUniforms.params);
		std::copy(std::begin(previous.extra), std::end(previous.extra), m_shadowUniforms.extra);
		return;
	}
	const float radius = shadowRadius(settings);
	// Ahead of the camera by half the region, snapped to a quarter of it.
	const Vec3f forward = camera.forward();
	const Vec3f ahead = normalize(Vec3f {forward.x, 0, forward.z});
	const Vec3f wanted = camera.position + ahead * (radius * 0.5f);
	const float step = radius * 0.5f;
	const Vec3f center {std::round(wanted.x / step) * step, std::round(camera.position.y / step) * step,
		std::round(wanted.z / step) * step};
	const Vec3f towardsLight = normalize(Vec3f {-lights.sunDirection[0], -lights.sunDirection[1], -lights.sunDirection[2]});
	const Vec3f up = std::fabs(towardsLight.y) > 0.99f ? Vec3f {0, 0, 1} : Vec3f {0, 1, 0};
	const Vec3f eye = center + towardsLight * kShadowHeadroom;
	m_shadow.center = center;
	m_shadow.radius = radius;
	m_shadow.depth = kShadowHeadroom * 2;
	m_shadow.size = size;
	m_shadow.viewProjection = Mat4::lookAt(eye, center, up) * orthographic(radius, m_shadow.depth);
	static const bool redraw = std::getenv("BF2142_SHADOW_REDRAW") != nullptr; // the static map every frame (its cost)
	const bool moved = redraw || !m_shadowStaticValid || std::fabs(center.x - m_shadowStaticCenter.x) > 0.01f ||
		std::fabs(center.y - m_shadowStaticCenter.y) > 0.01f || std::fabs(center.z - m_shadowStaticCenter.z) > 0.01f ||
		m_shadowStaticInstances != m_instances.size();
	if (settings.dynamicStaticMeshShadowsEnabled && settings.shadowMapIncludeStaticMeshes && moved) {
		drawShadowCasters(commands, m_shadowStatic, true, seconds);
		m_shadowStaticValid = true;
		m_shadowStaticCenter = center;
		m_shadowStaticInstances = m_instances.size();
	} else if (!(settings.dynamicStaticMeshShadowsEnabled && settings.shadowMapIncludeStaticMeshes)) {
		m_shadowStaticValid = false;
	}
	drawShadowCasters(commands, m_shadowDynamic, false, seconds);
	m_shadowDrawn = true;
	m_shadowUniforms.shadowMatrix = m_shadow.viewProjection * kClipToTexture;
	m_shadowUniforms.params[0] = 1;
	m_shadowUniforms.params[1] = 1.0f / static_cast<float>(size);
	// 5 cm of the light's depth (the casters' slope bias does the rest).
	m_shadowUniforms.params[2] = (0.05f + settings.shadowMapDepthBias) / m_shadow.depth;
	m_shadowUniforms.params[3] = m_shadowStaticValid ? 1.0f : 0.0f;
	m_shadowUniforms.extra[0] = settings.shadowIntensityBias;
	m_shadowUniforms.extra[1] = 0.8f;
	static const bool debug = std::getenv("BF2142_SHADOW_DEBUG") != nullptr;
	m_shadowUniforms.extra[3] = debug ? 1.0f : 0.0f;
}

void LevelRenderer::drawShadowCasters(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* map, bool statics, float seconds)
{
	const scene::ShadowSettings defaults;
	const scene::ShadowSettings& settings = m_sources && m_sources->shadows ? *m_sources->shadows : defaults;
	SDL_GPUDepthStencilTargetInfo depth {};
	depth.texture = map;
	depth.clear_depth = 1.0f;
	depth.load_op = SDL_GPU_LOADOP_CLEAR;
	depth.store_op = SDL_GPU_STOREOP_STORE;
	depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth);
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.shadowCasterPipeline());
	// The light's axes, for the region test.
	const Mat4& vp = m_shadow.viewProjection;
	const GpuMesh* bound = nullptr;
	TransformUniforms transforms {};
	size_t casters = 0, draws = 0;
	for (const Instance& instance : m_instances) {
		const GpuMesh* gpuMesh = instance.mesh;
		if (!gpuMesh || instance.fade < 1.0f) {
			continue;
		}
		const bool dynamic = gpuMesh->skinned || gpuMesh->bundled;
		if (dynamic == statics) {
			continue;
		}
		// Hidden, except the first person's own soldier, which still casts
		// (shadowManager.dynamic1pShadowsEnabled).
		if (instance.hidden) {
			const world::Object* object =
				!statics && settings.dynamic1pShadowsEnabled && m_sources && m_sources->hiddenInView && instance.object != 0
				? m_sources->objects.find(instance.object)
				: nullptr;
			if (!object || object->hidden() || !m_sources->hiddenInView(instance.object)) {
				continue;
			}
		}
		BoundingSphere sphere = instanceBounds(instance, *gpuMesh);
		if (sphere.radius < 0) {
			sphere.center = {instance.world.at(3, 0), instance.world.at(3, 1) + 1.0f, instance.world.at(3, 2)};
			sphere.radius = 2.5f;
		}
		if (gpuMesh->radiusShadowBounds) {
			// useRadiusForShadowDepth / Spatial: the caster's box is
			// pos +- radius * sqrt 2 (0x1012b190, 0x10123e70).
			sphere.radius *= 1.4142135f;
		}
		// The sphere's centre in the light's clip space (x, y in -1..1).
		const float cx = sphere.center.x * vp.at(0, 0) + sphere.center.y * vp.at(1, 0) + sphere.center.z * vp.at(2, 0) + vp.at(3, 0);
		const float cy = sphere.center.x * vp.at(0, 1) + sphere.center.y * vp.at(1, 1) + sphere.center.z * vp.at(2, 1) + vp.at(3, 1);
		const float r = sphere.radius / m_shadow.radius;
		if (std::fabs(cx) > 1 + r || std::fabs(cy) > 1 + r) {
			continue;
		}
		// The LOD the world pass picked for it if any, else its own.
		if (instance.lods.size() > 1) {
			for (size_t lod = instance.lods.size(); lod-- > 1;) {
				if (instance.lods[lod].resolved && instance.lods[lod].mesh) {
					gpuMesh = instance.lods[lod].mesh;
					break;
				}
			}
		}
		++casters;
		if (gpuMesh != bound) {
			bound = gpuMesh;
			SDL_GPUBufferBinding vertexBinding {bound->vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
			SDL_GPUBufferBinding indexBinding {bound->indices, 0};
			SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		}
		transforms.worldViewProjection = instance.world * vp;
		transforms.world = instance.world;
		for (int i = 0; i < kMaxMeshParts; ++i) {
			transforms.parts[i] = instance.parts.empty() || static_cast<size_t>(i) >= instance.parts.size()
				? Mat4 {}
				: instance.parts[static_cast<size_t>(i)];
		}
		SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
		const std::vector<anim::Matrix>* pose = bound->skinned ? &poseFor(instance, seconds) : nullptr;
		for (const Submesh& submesh : bound->submeshes) {
			if (submesh.blended || submesh.additive) {
				continue; // glass, decals and glows cast nothing
			}
			if (submesh.alphaReference > 0 && !settings.allowAlphaShadows) {
				continue; // leaves and fences: shadowManager.allowAlphaShadows
			}
			if (bound->skinned) {
				if (submesh.rig < 0 || !pose || pose->empty()) {
					continue;
				}
				skinPalette(*bound, submesh, *pose, transforms);
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
			}
			const float caster[4] = {submesh.alphaReference > 0 ? std::max(submesh.alphaReference, settings.alphaShadowThreshold) : 0.0f,
				0, 0, 0};
			SDL_PushGPUFragmentUniformData(commands, 0, caster, sizeof(caster));
			SDL_GPUTextureSamplerBinding sampler {submesh.texture, m_gpu.repeatSampler()};
			SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
			SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
			++draws;
		}
	}
	SDL_EndGPURenderPass(pass);
	if (std::getenv("BF2142_SHADOW_STATS")) {
		std::printf("shadow map (%s): %zu casters, %zu draws, region %.0f/%.0f/%.0f r %.0f\n", statics ? "static" : "dynamic",
			casters, draws, m_shadow.center.x, m_shadow.center.y, m_shadow.center.z, m_shadow.radius);
	}
}

// ---- the sky's clouds and sun flare ----------------------------------------------

void LevelRenderer::buildSkyLayers(const LevelSources& sources)
{
	const scene::SkySettings& sky = sources.sky;
	m_cloudLayers = 0;
	if (sky.hasCloudLayer && !sky.cloudTexture.empty()) {
		m_cloudTextures[0] = texture(sources.files, sky.cloudTexture + ".dds");
		m_cloudLayers = m_cloudTextures[0] ? 1 : 0;
		if (m_cloudLayers && sky.hasCloudLayer2 && !sky.cloudTexture2.empty()) {
			m_cloudTextures[1] = texture(sources.files, sky.cloudTexture2 + ".dds");
			m_cloudLayers = m_cloudTextures[1] ? 2 : 1;
		}
	}
	m_stats.cloudLayers = static_cast<size_t>(m_cloudLayers);
	m_flareTexture = sky.flareTexture.empty() ? nullptr : texture(sources.files, sky.flareTexture + ".dds");
	if (m_flareTexture && !m_flareVertices) {
		// A quad facing -z, -1..1 (the flare sprite's; turned to the camera).
		Vertex quad[6] {};
		const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
		for (int i = 0; i < 6; ++i) {
			quad[i].position[0] = corners[i][0];
			quad[i].position[1] = corners[i][1];
			quad[i].normal[2] = -1;
			quad[i].texCoord[0] = corners[i][0] * 0.5f + 0.5f;
			quad[i].texCoord[1] = 0.5f - corners[i][1] * 0.5f;
			quad[i].weight = 1;
		}
		m_flareVertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, quad, sizeof(quad));
	}
	m_stats.sunFlare = m_flareTexture != nullptr;
}

void LevelRenderer::drawSky(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection,
	const Camera& camera)
{
	if (!m_skyMesh) {
		return;
	}
	// The clouds scroll by scrollDirection a second (TEXOFFSET, 0x100fd2f0's
	// +0x130 / +0x138).
	const uint64_t now = SDL_GetTicks();
	const float dt = m_lastSkyTicks ? static_cast<float>(now - m_lastSkyTicks) / 1000.0f : 0.0f;
	m_lastSkyTicks = now;
	SkyUniforms sky {};
	if (m_sources) {
		const scene::SkySettings& s = m_sources->sky;
		m_cloudOffsets[0] = std::fmod(m_cloudOffsets[0] + s.scrollDirection[0] * dt, 1.0f);
		m_cloudOffsets[1] = std::fmod(m_cloudOffsets[1] + s.scrollDirection[1] * dt, 1.0f);
		m_cloudOffsets[2] = std::fmod(m_cloudOffsets[2] + s.scrollDirection2[0] * dt, 1.0f);
		m_cloudOffsets[3] = std::fmod(m_cloudOffsets[3] + s.scrollDirection2[1] * dt, 1.0f);
		std::copy(std::begin(m_cloudOffsets), std::end(m_cloudOffsets), sky.cloudOffsets);
		// CLOUDSFADEOUTDIST: the start and the length (y - x).
		sky.cloudFade[0] = s.fadeCloudsDistances[0];
		sky.cloudFade[1] = s.fadeCloudsDistances[1] - s.fadeCloudsDistances[0];
		sky.cloudFade[2] = s.cloudLerpFactors[0];
		sky.cloudFade[3] = s.cloudLerpFactors[1];
	}
	sky.domeCenter[0] = camera.position.x;
	sky.domeCenter[1] = camera.position.y;
	sky.domeCenter[2] = camera.position.z;
	sky.domeCenter[3] = static_cast<float>(m_cloudLayers);
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.skyPipeline());
	SDL_PushGPUFragmentUniformData(commands, 0, &sky, sizeof(sky));
	const Mat4 world = Mat4::rotationY(m_skyRotation * 3.14159265f / 180.0f) * Mat4::translation(camera.position);
	TransformUniforms skyTransforms {world * viewProjection, world, {1, 1, 0, 0}, {}};
	SDL_PushGPUVertexUniformData(commands, 0, &skyTransforms, sizeof(skyTransforms));
	SDL_GPUBufferBinding vertexBinding {m_skyMesh->vertices, 0};
	SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
	SDL_GPUBufferBinding indexBinding {m_skyMesh->indices, 0};
	SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
	for (const Submesh& submesh : m_skyMesh->submeshes) {
		SDL_GPUTextureSamplerBinding samplers[3] = {
			{m_skyTexture ? m_skyTexture : submesh.texture, m_gpu.clampSampler()},
			{m_cloudTextures[0] ? m_cloudTextures[0] : m_gpu.whiteTexture(), m_gpu.repeatSampler()},
			{m_cloudTextures[1] ? m_cloudTextures[1] : m_gpu.whiteTexture(), m_gpu.repeatSampler()},
		};
		SDL_BindGPUFragmentSamplers(pass, 0, samplers, 3);
		SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
	}
}

// The flare (0x100fd900): a sprite towards the sun far off, added; the
// original fades it by an occlusion query's visible share, here the depth
// test hides what is in front of it.
void LevelRenderer::drawSunFlare(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection,
	const Camera& camera)
{
	if (!m_flareTexture || !m_flareVertices || !m_sources) {
		return;
	}
	const scene::SkySettings& sky = m_sources->sky;
	const Vec3f towardsSun = normalize(Vec3f {-sky.flareDirection[0], -sky.flareDirection[1], -sky.flareDirection[2]});
	const Vec3f forward = camera.forward();
	const float facing = dot(towardsSun, forward);
	if (facing <= 0 || towardsSun.y < -0.05f) {
		return;
	}
	constexpr float kDistance = 4000; // inside the 6 km far plane
	constexpr float kHalfSize = 0.12f; // of its distance (the sprite's angular size)
	const Vec3f center = camera.position + towardsSun * kDistance;
	// Turned to the camera: x right, y up, z towards it.
	const Vec3f zAxis = towardsSun * -1.0f;
	const Vec3f helper = std::fabs(zAxis.y) > 0.99f ? Vec3f {1, 0, 0} : Vec3f {0, 1, 0};
	const Vec3f xAxis = normalize(cross(helper, zAxis));
	const Vec3f yAxis = cross(zAxis, xAxis);
	const float s = kDistance * kHalfSize;
	Mat4 world;
	world.m = {xAxis.x * s, xAxis.y * s, xAxis.z * s, 0, yAxis.x * s, yAxis.y * s, yAxis.z * s, 0, zAxis.x, zAxis.y,
		zAxis.z, 0, center.x, center.y, center.z, 1};
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.additiveMeshPipeline());
	useShadows(commands, pass, 1, 5, true);
	TransformUniforms transforms {world * viewProjection, world, {1, 1, 0, 0}, {}};
	SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
	LightingUniforms uniforms {};
	uniforms.fogColor[3] = 1e9f; // unfogged
	uniforms.material[2] = kMaterialAdditive;
	// flareParams[0]: the visible share (1 where the depth test lets it
	// through) plus flareFadeAdd, times flareFadeMul, at most 1; brighter
	// looking into it.
	const float strength = std::clamp((1 + sky.flareFadeAdd) * sky.flareFadeMul, 0.0f, 1.0f) *
		std::clamp(facing * 1.5f - 0.2f, 0.0f, 1.0f);
	uniforms.tint[0] = uniforms.tint[1] = uniforms.tint[2] = strength;
	SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof(uniforms));
	SDL_GPUTextureSamplerBinding samplers[5] = {
		{m_flareTexture, m_gpu.clampSampler()},
		{m_gpu.whiteTexture(), m_gpu.clampSampler()},
		{m_gpu.whiteTexture(), m_gpu.repeatSampler()},
		{m_gpu.whiteTexture(), m_gpu.repeatSampler()},
		{m_gpu.whiteTexture(), m_gpu.repeatSampler()},
	};
	SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
	SDL_GPUBufferBinding vertexBinding {m_flareVertices, 0};
	SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
	SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

// ---- the post-production ----------------------------------------------------------

bool LevelRenderer::postEnabled() const
{
	return !(m_sources && m_sources->post && !m_sources->post->drawPostProduction);
}

void LevelRenderer::releasePostTargets()
{
	SDL_GPUDevice* device = m_gpu.device();
	const auto release = [device](SDL_GPUTexture*& texture) {
		if (texture) {
			SDL_ReleaseGPUTexture(device, texture);
			texture = nullptr;
		}
	};
	release(m_sceneColor);
	release(m_bloomTargets[0]);
	release(m_bloomTargets[1]);
	for (SDL_GPUTexture*& history : m_history) {
		release(history);
	}
	release(m_camoMask);
	m_sceneSize[0] = m_sceneSize[1] = 0;
}

void LevelRenderer::ensurePostTargets(uint32_t width, uint32_t height)
{
	if (m_sceneColor && m_sceneSize[0] == width && m_sceneSize[1] == height) {
		return;
	}
	releasePostTargets();
	m_sceneColor = m_gpu.createColorTexture(width, height);
	m_sceneSize[0] = width;
	m_sceneSize[1] = height;
	// The bloom's small targets (DAT_101fa570 +0x8d0 / +0x8d4): a quarter.
	m_bloomSize[0] = std::max<uint32_t>(width / 4, 1);
	m_bloomSize[1] = std::max<uint32_t>(height / 4, 1);
	m_bloomTargets[0] = m_gpu.createColorTexture(m_bloomSize[0], m_bloomSize[1]);
	m_bloomTargets[1] = m_gpu.createColorTexture(m_bloomSize[0], m_bloomSize[1]);
}

void LevelRenderer::loadPostTextures(const LevelSources& sources)
{
	const char* names[4] = {"common/textures/noise.dds", "common/textures/random.dds", "common/textures/frame.dds",
		"common/textures/scanline.dds"};
	for (int i = 0; i < 4; ++i) {
		m_postTextures[i] = sources.files.exists(names[i]) ? texture(sources.files, names[i]) : nullptr;
	}
}

void LevelRenderer::fullscreenPass(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, SDL_GPUTexture* source,
	const BloomUniforms& uniforms)
{
	SDL_GPUColorTargetInfo colorTarget {};
	colorTarget.texture = target;
	colorTarget.load_op = SDL_GPU_LOADOP_DONT_CARE;
	colorTarget.store_op = SDL_GPU_STOREOP_STORE;
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colorTarget, 1, nullptr);
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.bloomPipeline());
	SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof(uniforms));
	SDL_GPUTextureSamplerBinding sampler {source, m_gpu.clampSampler()};
	SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	SDL_EndGPURenderPass(pass);
}

void LevelRenderer::drawCamouflage(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* depthTexture,
	const Mat4& viewProjection, float seconds)
{
	if (!m_camoMask) {
		m_camoMask = m_gpu.createColorTexture(m_sceneSize[0], m_sceneSize[1]);
	}
	SDL_GPUColorTargetInfo colorTarget {};
	colorTarget.texture = m_camoMask;
	colorTarget.clear_color = {0, 0, 0, 0};
	colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
	colorTarget.store_op = SDL_GPU_STOREOP_STORE;
	SDL_GPUDepthStencilTargetInfo depth {};
	depth.texture = depthTexture;
	depth.load_op = SDL_GPU_LOADOP_LOAD;
	depth.store_op = SDL_GPU_STOREOP_STORE;
	depth.stencil_load_op = SDL_GPU_LOADOP_LOAD;
	depth.stencil_store_op = SDL_GPU_STOREOP_STORE;
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colorTarget, 1, &depth);
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.camoPipeline());
	for (const DrawItem& item : m_cloakedList) {
		const Instance& instance = *item.instance;
		const GpuMesh& gpuMesh = *item.mesh;
		SDL_GPUBufferBinding vertexBinding {gpuMesh.vertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		SDL_GPUBufferBinding indexBinding {gpuMesh.indices, 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		TransformUniforms transforms {instance.world * viewProjection, instance.world, {1, 1, 0, 0}, {}};
		for (size_t i = 0; i < instance.parts.size() && i < static_cast<size_t>(kMaxMeshParts); ++i) {
			transforms.parts[i] = instance.parts[i];
		}
		SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
		const std::vector<anim::Matrix>* pose = gpuMesh.skinned ? &poseFor(instance, seconds) : nullptr;
		for (const Submesh& submesh : gpuMesh.submeshes) {
			if (gpuMesh.skinned) {
				if (submesh.rig < 0 || pose->empty()) {
					continue;
				}
				skinPalette(gpuMesh, submesh, *pose, transforms);
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
			}
			SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
		}
	}
	SDL_EndGPURenderPass(pass);
}

PostUniforms LevelRenderer::preparePost(SDL_GPUCommandBuffer* commands, bool cloaked)
{
	const scene::PostProductionSettings defaults;
	const scene::PostProductionSettings& settings = m_sources && m_sources->post ? *m_sources->post : defaults;
	PostEffects effects;
	if (m_sources && m_sources->postEffects) {
		m_sources->postEffects(effects);
	}
	// BF2142_POST_EFFECT=emp:N | soldieremp:N | tinnitus | tv | camera | flash:S
	// forces one (for screenshots; the flash seconds in).
	if (const char* forced = std::getenv("BF2142_POST_EFFECT")) {
		const std::string what = forced;
		const int value = what.find(':') != std::string::npos ? std::atoi(what.c_str() + what.find(':') + 1) : 1;
		if (what.rfind("emp", 0) == 0) {
			effects.vehicleEmpLevel = value;
		} else if (what.rfind("soldieremp", 0) == 0) {
			effects.soldierEmpLevel = value;
		} else if (what == "tinnitus" && !m_postState.tinnitusActive()) {
			effects.tinnitusSeconds = 7;
		} else if (what == "tv") {
			effects.tv = true;
		} else if (what == "camera") {
			effects.cameraEffect = true;
		} else if (what.rfind("flash", 0) == 0 && !m_postState.flashActive()) {
			effects.titanExplosion = true;
			effects.frameSeconds = static_cast<float>(value);
		}
	}
	const uint64_t now = SDL_GetTicks();
	if (effects.frameSeconds <= 0) {
		effects.frameSeconds = m_lastPostTicks ? static_cast<float>(now - m_lastPostTicks) / 1000.0f : 1.0f / 30.0f;
	}
	m_lastPostTicks = now;
	PostUniforms uniforms = m_postState.update(effects, settings, cloaked);
	const int mode = static_cast<int>(uniforms.contrast[3]);
	if (mode & kPostBloom) {
		// 0x10049140: contrast into A, across into B, the 5x5 blur into A,
		// down into B (B is the bloom).
		BloomUniforms bloom;
		bloom.params[1] = settings.bloomFilterContrast;
		bloom.params[2] = settings.bloomSize;
		bloom.params[3] = settings.bloomWeightScale;
		bloom.texel[0] = 1.0f / static_cast<float>(m_bloomSize[0]);
		bloom.texel[1] = 1.0f / static_cast<float>(m_bloomSize[1]);
		bloom.params[0] = 0;
		fullscreenPass(commands, m_bloomTargets[0], m_sceneColor, bloom);
		bloom.params[0] = 1;
		fullscreenPass(commands, m_bloomTargets[1], m_bloomTargets[0], bloom);
		bloom.params[0] = 3;
		fullscreenPass(commands, m_bloomTargets[0], m_bloomTargets[1], bloom);
		bloom.params[0] = 2;
		fullscreenPass(commands, m_bloomTargets[1], m_bloomTargets[0], bloom);
	}
	if (mode & kPostTinnitus) {
		// The last four frames at a quarter size, newest first.
		for (SDL_GPUTexture*& history : m_history) {
			if (!history) {
				history = m_gpu.createColorTexture(m_bloomSize[0], m_bloomSize[1]);
			}
		}
		BloomUniforms copy;
		copy.params[0] = 4;
		m_historyNext = (m_historyNext + 3) % 4;
		// A new tinnitus starts from four copies of this frame.
		for (int i = 0; i < (m_historyValid ? 1 : 4); ++i) {
			fullscreenPass(commands, m_history[(m_historyNext + i) % 4], m_sceneColor, copy);
		}
		m_historyValid = true;
	} else {
		m_historyValid = false;
	}
	return uniforms;
}

void LevelRenderer::drawPost(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const PostUniforms& uniforms)
{
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.postPipeline());
	SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof(uniforms));
	const auto orWhite = [this](SDL_GPUTexture* t) { return t ? t : m_gpu.whiteTexture(); };
	const auto orBlack = [this](SDL_GPUTexture* t) { return t ? t : m_gpu.blackTexture(); };
	SDL_GPUTexture* scene = m_sceneColor;
	const auto history = [&](int age) { return orBlack(m_history[(m_historyNext + age) % 4] ? m_history[(m_historyNext + age) % 4] : nullptr); };
	SDL_GPUTextureSamplerBinding samplers[11] = {
		{scene, m_gpu.clampSampler()},
		{orBlack(m_bloomTargets[1]), m_gpu.clampSampler()},
		{orWhite(m_postTextures[0]), m_gpu.repeatSampler()},
		{orWhite(m_postTextures[1]), m_gpu.repeatSampler()},
		{orBlack(m_postTextures[2]), m_gpu.clampSampler()},
		{orBlack(m_postTextures[3]), m_gpu.repeatSampler()},
		{orBlack(m_camoMask), m_gpu.clampSampler()},
		{history(0), m_gpu.clampSampler()},
		{history(1), m_gpu.clampSampler()},
		{history(2), m_gpu.clampSampler()},
		{history(3), m_gpu.clampSampler()},
	};
	SDL_BindGPUFragmentSamplers(pass, 0, samplers, 11);
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

} // namespace dice::hfe::render
