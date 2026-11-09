// LevelRenderer's enhanced mode (Scene/EnhancedSettings.h, --enhanced): render
// passes the original doesn't have, each behind its enhanced.* switch, none
// run while enhanced.enabled is 0 (the frame is then drawn as before).
//
//   prepass  the world's depth (terrain, opaque meshes) with the frame's
//            projection, then from it the view-space normals, linear depth
//            and the camera's motion (gbuffer.frag); the moving meshes'
//            own motion over that from their last frame's transforms
//            (motion.vert), the first person's as none.
//   ssao     GTAO (gtao.frag, aoblur.frag) from the prepass, which the lit
//            shaders' enhanced variants (Shaders/Enhanced/*.patch) multiply
//            into their ambient term only, in the world's opaque draws.
//   taa      the projections jittered (Halton 2, 3 over 8 frames), the
//            scene resolved against its history (taa.frag) before the
//            bloom, the tinnitus's frames and post.frag read it; the HUD
//            and menus are drawn after, unjittered.
//   csm      four cascades in place of the single shadow region (and of
//            the SimpleShadowmap factor where they reach), the dynamic
//            objects' and the static meshes' plus the terrain's in two
//            atlases, filtered with PCF (enhanced_shadow.glsl).
#include "Render/Enhanced/EnhancedMath.h"
#include "Render/LevelRenderer.h"
#include "Render/MeshQueues.h"
#include "Render/SceneMath.h"
#include "Render/TerrainLod.h"

#include "Scene/EffectSettings.h"
#include "Scene/EnhancedSettings.h"
#include "Scene/LightSettings.h"
#include "Scene/PostProductionSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace dice::hfe::render {

namespace {

struct GBufferUniforms {
	Mat4 inverseViewProjection;
	Mat4 view;
	Mat4 viewProjection;
	Mat4 previousViewProjection;
	float texel[4] {};
};

struct GtaoUniforms {
	float projection[4] {};
	float params[4] {};
	float texel[4] {};
};

struct TaaUniforms {
	float texel[4] {};
	float params[4] {};
};

struct MotionUniforms {
	Mat4 current;  // world x this frame's view-projection, unjittered
	Mat4 previous; // last frame's world x its view-projection
	Mat4 previousParts[kMaxMeshParts];
};

constexpr float kAORadius = 1.5f;    // metres the GTAO searches
constexpr float kFarPlane = 6000.0f; // the world pass's (LevelRenderer::draw)
constexpr float kTaaHistory = 0.9f;  // the history's share of a resolve
constexpr float kCutDistance = 25.0f; // a camera moved farther in a frame starts afresh

void release(SDL_GPUDevice* device, SDL_GPUTexture*& texture)
{
	if (texture) {
		SDL_ReleaseGPUTexture(device, texture);
		texture = nullptr;
	}
}

} // namespace

void LevelRenderer::releaseEnhancedTargets()
{
	SDL_GPUDevice* device = m_gpu.device();
	release(device, m_enh.depth);
	release(device, m_enh.normalDepth);
	release(device, m_enh.motion);
	release(device, m_enh.aoRaw);
	release(device, m_enh.ao);
	release(device, m_enh.history[0]);
	release(device, m_enh.history[1]);
	release(device, m_enh.cascadeDynamic);
	release(device, m_enh.cascadeStatic);
	m_enh.size[0] = m_enh.size[1] = 0;
	m_enh.cascadeSize = 0;
	m_enh.historyValid = false;
	m_enh.staticValid = false;
}

void LevelRenderer::ensureEnhancedTargets(uint32_t width, uint32_t height)
{
	if (m_enh.depth && m_enh.size[0] == width && m_enh.size[1] == height) {
		return;
	}
	SDL_GPUDevice* device = m_gpu.device();
	for (SDL_GPUTexture** texture : {&m_enh.depth, &m_enh.normalDepth, &m_enh.motion, &m_enh.aoRaw, &m_enh.ao,
			 &m_enh.history[0], &m_enh.history[1]}) {
		release(device, *texture);
	}
	m_enh.depth = m_gpu.createRenderTexture(width, height, m_gpu.shadowDepthFormat(), true);
	m_enh.normalDepth = m_gpu.createRenderTexture(width, height, GpuContext::kNormalDepthFormat);
	m_enh.motion = m_gpu.createRenderTexture(width, height, GpuContext::kMotionFormat);
	m_enh.aoRaw = m_gpu.createRenderTexture(width, height, GpuContext::kAOFormat);
	m_enh.ao = m_gpu.createRenderTexture(width, height, GpuContext::kAOFormat);
	m_enh.history[0] = m_gpu.createColorTexture(width, height);
	m_enh.history[1] = m_gpu.createColorTexture(width, height);
	m_enh.size[0] = width;
	m_enh.size[1] = height;
	m_enh.historyValid = false;
}

void LevelRenderer::beginEnhancedFrame(const Camera& camera, uint32_t width, uint32_t height, bool overhead, bool post,
	const Mat4& projection)
{
	const scene::EnhancedSettings* settings = m_sources ? m_sources->enhanced : nullptr;
	const GpuContext::EnhancedPipelines& p = m_gpu.enhancedPipelines();
	const bool on = settings && settings->enabled;
	m_enh.on = on;
	// The prepass reconstructs positions with the perspective's scales: not
	// in the commander's overhead view (its own projection).
	const bool prepassable = on && !overhead && p.depth && p.terrainDepth && p.gbuffer;
	m_enh.ssao = prepassable && settings->ssao && p.gtao && p.aoBlur && m_gpu.enhancedLitAvailable();
	// TAA resolves the scene texture: none without the post-production.
	m_enh.taa = prepassable && settings->taa && post && p.taa && p.motion && p.motionOverlay;
	m_enh.prepass = prepassable && (settings->prepass || m_enh.ssao || m_enh.taa);
	m_enh.csm = on && settings->csm && m_gpu.enhancedLitAvailable() && p.terrainCaster;
	m_gpu.setEnhancedLit(m_enh.ssao || m_enh.csm);
	m_enh.aoBound = false;
	m_enh.jittered = false;
	m_enh.jitter = Mat4 {};
	if (!on) {
		m_enh.previousValid = false;
		m_enh.historyValid = false;
		return;
	}
	if (m_enh.prepass) {
		ensureEnhancedTargets(width, height);
		if (!m_enh.depth || !m_enh.normalDepth || !m_enh.motion || !m_enh.ao || !m_enh.aoRaw || !m_enh.history[0] ||
			!m_enh.history[1]) {
			m_enh.prepass = m_enh.ssao = m_enh.taa = false;
			m_gpu.setEnhancedLit(m_enh.csm);
		}
	}
	// Printed when they change, so a run (a benchmark) shows what it measured.
	const int passes = (m_enh.prepass ? 1 : 0) | (m_enh.taa ? 2 : 0) | (m_enh.ssao ? 4 : 0) | (m_enh.csm ? 8 : 0);
	if (passes != m_enh.reported) {
		m_enh.reported = passes;
		std::printf("enhanced: prepass %d taa %d ssao %d csm %d\n", m_enh.prepass, m_enh.taa, m_enh.ssao, m_enh.csm);
	}
	m_enh.view = camera.view();
	m_enh.projection = projection;
	m_enh.viewProjection = m_enh.view * projection;
	const Vec3f moved = camera.position - m_enh.previousEye;
	if (!m_enh.previousValid || overhead != m_enh.previousOverhead || dot(moved, moved) > kCutDistance * kCutDistance) {
		m_enh.previousViewProjection = m_enh.viewProjection; // a cut: nothing moved, no history
		m_enh.historyValid = false;
	}
	if (!m_enh.taa) {
		m_enh.historyValid = false;
		return;
	}
	m_enh.jitter = enhanced::jitterMatrix(enhanced::jitterOffset(m_enh.frame, width, height));
	m_enh.jittered = true;
}

void LevelRenderer::endEnhancedFrame(const Camera& camera, bool overhead)
{
	if (!m_enh.on) {
		return;
	}
	m_enh.previousViewProjection = m_enh.viewProjection;
	m_enh.previousEye = camera.position;
	m_enh.previousOverhead = overhead;
	m_enh.previousValid = true;
	if (m_enh.taa) {
		// The moving meshes' placement, for next frame's motion.
		for (const DrawItem& item : m_drawList) {
			if (!item.mesh->bundled && !item.mesh->skinned) {
				continue;
			}
			const Instance& instance = *item.instance;
			instance.previousWorld = instance.world;
			instance.previousParts = instance.parts;
			const auto pose = m_poses.find(&instance);
			if (pose != m_poses.end()) {
				instance.previousPose = pose->second;
			} else {
				instance.previousPose.clear();
			}
			instance.previousFrame = m_enh.frame;
		}
	}
	++m_enh.frame;
}

void LevelRenderer::useEnhancedShadows(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, uint32_t slot,
	uint32_t firstSampler, bool hemi, bool undergrowth)
{
	EnhancedShadowUniforms uniforms;
	if (m_enh.csm) {
		uniforms = m_enh.shadows;
	} else {
		uniforms.base = m_shadowUniforms; // the single region (cascadeParams.x 0)
	}
	uniforms.base.extra[2] = undergrowth ? 1.0f : 0.0f;
	const bool ao = m_enh.ssao && m_enh.aoBound && m_enh.ao;
	uniforms.aoParams[0] = 1.0f / static_cast<float>(std::max<uint32_t>(m_enh.size[0], 1));
	uniforms.aoParams[1] = 1.0f / static_cast<float>(std::max<uint32_t>(m_enh.size[1], 1));
	uniforms.aoParams[2] = ao ? 1.0f : 0.0f;
	uniforms.aoParams[3] = 1.0f;
	SDL_PushGPUFragmentUniformData(commands, slot, &uniforms, sizeof(uniforms));
	SDL_GPUTexture* empty = m_gpu.emptyShadowMap();
	SDL_GPUTexture* dynamicMap;
	SDL_GPUTexture* staticMap;
	if (uniforms.cascadeParams[0] > 0.5f) {
		dynamicMap = m_enh.cascadeDynamic;
		staticMap = m_enh.staticValid ? m_enh.cascadeStatic : empty;
	} else {
		dynamicMap = m_shadow.enabled && m_shadowDynamic ? m_shadowDynamic : empty;
		staticMap = m_shadow.enabled && m_shadowStatic && m_shadowStaticValid ? m_shadowStatic : empty;
	}
	SDL_GPUTextureSamplerBinding samplers[3] = {
		{dynamicMap, m_gpu.shadowSampler()},
		{staticMap, m_gpu.shadowSampler()},
		{m_hemiMap ? m_hemiMap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
	};
	SDL_BindGPUFragmentSamplers(pass, firstSampler, samplers, hemi ? 3 : 2);
	// The variants' AO sampler: after mesh.frag's 11, else after the maps.
	SDL_GPUTextureSamplerBinding aoMap {ao ? m_enh.ao : m_gpu.whiteTexture(), m_gpu.clampSampler()};
	SDL_BindGPUFragmentSamplers(pass, hemi ? kMeshSamplers : firstSampler + 2, &aoMap, 1);
}

// ---- the prepass ------------------------------------------------------------------

void LevelRenderer::drawTerrainDepth(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Camera& camera,
	const Mat4& viewProjection, bool coarse)
{
	const scene::RendererSettings defaults;
	const scene::RendererSettings& renderer = m_sources && m_sources->renderer ? *m_sources->renderer : defaults;
	TransformUniforms transforms {viewProjection, Mat4 {}, {1, 1, 0, 0}, {}};
	transforms.partsUsed = 0;
	SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
	const auto morph = [&](int lod, bool on) {
		const TerrainMorph m = terrainMorph(lod, on, m_terrainLodSwitch, renderer, camera.position.x, camera.position.z,
			m_terrainHeightAboveGround);
		TerrainMorphUniforms u {};
		static_assert(sizeof(u) == sizeof(m));
		std::memcpy(&u, &m, sizeof(u));
		SDL_PushGPUVertexUniformData(commands, 1, &u, sizeof(u));
	};
	if (!m_terrainCells.empty()) {
		SDL_GPUBufferBinding indexBinding {m_terrainLodIndices, 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		const int perPatch = m_terrainCellsPerPatch;
		int pushed = -1;
		if (coarse) {
			morph(2, false);
			pushed = 2;
		}
		for (size_t p = 0; p < m_terrain.size(); ++p) {
			const int px = static_cast<int>(p) % m_patchesPerSide, pz = static_cast<int>(p) / m_patchesPerSide;
			bool bound = false;
			for (int j = 0; j < perPatch; ++j) {
				for (int i = 0; i < perPatch; ++i) {
					const size_t c = static_cast<size_t>(pz * perPatch + j) * m_terrainCellsPerSide +
						static_cast<size_t>(px * perPatch + i);
					const TerrainLodCell& cell = m_terrainCells[c];
					if (!coarse && !cell.visible) {
						continue;
					}
					const int lod = coarse ? 2 : cell.lod;
					const auto& ranges = m_terrainNodeRanges[static_cast<size_t>(m_terrainCellNodes[c])];
					TerrainNodeRange range = ranges[static_cast<size_t>(lod * 11 + (coarse ? 0 : cell.mask))];
					if (range.count == 0) {
						range = ranges[static_cast<size_t>(lod * 11)];
					}
					if (range.count == 0) {
						continue;
					}
					if (!bound) {
						SDL_GPUBufferBinding vertexBinding {m_terrain[p].vertices, 0};
						SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
						bound = true;
					}
					if (pushed != lod) {
						morph(lod, true);
						pushed = lod;
					}
					SDL_DrawGPUIndexedPrimitives(pass, range.count, 1, range.start, 0, 0);
				}
			}
		}
	} else if (m_terrainIndices) {
		SDL_GPUBufferBinding indexBinding {m_terrainIndices, 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		morph(0, false);
		const Frustum frustum = Frustum::fromViewProjection(viewProjection);
		for (const TerrainPatch& patch : m_terrain) {
			if (!coarse && !frustum.containsBox(patch.boundsMin, patch.boundsMax)) {
				continue;
			}
			SDL_GPUBufferBinding vertexBinding {patch.vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
			SDL_DrawGPUIndexedPrimitives(pass, m_terrainIndexCount, 1, 0, 0, 0);
		}
	}
}

void LevelRenderer::renderEnhancedPrepass(SDL_GPUCommandBuffer* commands, const Camera& camera,
	const Mat4& viewProjection, float seconds, float aspect)
{
	const GpuContext::EnhancedPipelines& p = m_gpu.enhancedPipelines();
	const scene::RendererSettings switchDefaults;
	const scene::RendererSettings& switches = m_sources && m_sources->renderer ? *m_sources->renderer : switchDefaults;
	const float texel[4] = {1.0f / static_cast<float>(m_enh.size[0]), 1.0f / static_cast<float>(m_enh.size[1]),
		static_cast<float>(m_enh.size[0]), static_cast<float>(m_enh.size[1])};
	const auto fullscreen = [&](SDL_GPUTexture* target, SDL_GPUGraphicsPipeline* pipeline, const void* uniforms,
								uint32_t size, std::initializer_list<SDL_GPUTextureSamplerBinding> samplers) {
		SDL_GPUColorTargetInfo color {};
		color.texture = target;
		color.load_op = SDL_GPU_LOADOP_DONT_CARE;
		color.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
		SDL_BindGPUGraphicsPipeline(pass, pipeline);
		SDL_PushGPUFragmentUniformData(commands, 0, uniforms, size);
		SDL_BindGPUFragmentSamplers(pass, 0, samplers.begin(), static_cast<uint32_t>(samplers.size()));
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	};
	// An instance's transforms as the world pass sends them (parts, the leaves' sway).
	const float windSpeed = m_sources && m_sources->wind ? m_sources->wind->speedAt() : 0.0f;
	const auto placeInstance = [&](const Instance& instance, const GpuMesh& mesh, TransformUniforms& transforms) {
		transforms.worldViewProjection = instance.world * viewProjection;
		transforms.world = instance.world;
		if (instance.parts.empty() && !mesh.skinned) {
			transforms.parts[0] = Mat4 {};
			transforms.partsUsed = 1;
		} else {
			for (int i = 0; i < kMaxMeshParts; ++i) {
				transforms.parts[i] = static_cast<size_t>(i) < instance.parts.size() ? instance.parts[static_cast<size_t>(i)]
																					: Mat4 {};
			}
			transforms.partsUsed = kMaxMeshParts;
		}
		const scene::WindSettings* wind = m_sources ? m_sources->wind : nullptr;
		transforms.sway[0] = 0;
		transforms.sway[1] = mesh.bounds.radius;
		transforms.sway[2] = (wind && !wind->localWinds.empty()
				? wind->speedAt({instance.world.at(3, 0), instance.world.at(3, 1), instance.world.at(3, 2)})
				: windSpeed) + 5;
		transforms.sway[3] = seconds;
		transforms.uvUsed = false;
	};
	// Its opaque materials (the overgrowth's atlas ones left out: their
	// coordinates need the atlas's offset, which the caster shader hasn't).
	const auto drawn = [](const Submesh& submesh, float fade) {
		return meshQueue(submesh.blended, submesh.additive, fade) == MeshQueue::Opaque && submesh.uvAtlas[0] == 1 &&
			submesh.uvAtlas[1] == 1;
	};
	const auto bindMesh = [](SDL_GPURenderPass* pass, const GpuMesh*& bound, const GpuMesh* mesh) {
		if (mesh != bound) {
			bound = mesh;
			SDL_GPUBufferBinding vertexBinding {mesh->vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
			SDL_GPUBufferBinding indexBinding {mesh->indices, 0};
			SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		}
	};
	const auto alphaTest = [&](SDL_GPURenderPass* pass, const Submesh& submesh) {
		const float caster[4] = {submesh.alphaReference, 0, 0, 0};
		SDL_PushGPUFragmentUniformData(commands, 0, caster, sizeof(caster));
		SDL_GPUTextureSamplerBinding sampler {submesh.texture, m_gpu.repeatSampler()};
		SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
	};
	static const std::vector<anim::Matrix> unposed;

	// The depth: the terrain, then the opaque meshes.
	{
		SDL_GPUDepthStencilTargetInfo depth {};
		depth.texture = m_enh.depth;
		depth.clear_depth = 1.0f;
		depth.load_op = SDL_GPU_LOADOP_CLEAR;
		depth.store_op = SDL_GPU_STOREOP_STORE;
		depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth);
		if (switches.drawTerrain) {
			SDL_BindGPUGraphicsPipeline(pass, p.terrainDepth);
			drawTerrainDepth(commands, pass, camera, viewProjection, false);
		}
		SDL_BindGPUGraphicsPipeline(pass, p.depth);
		const GpuMesh* bound = nullptr;
		TransformUniforms transforms {};
		for (const DrawItem& item : m_drawList) {
			const float fade = item.drawnFade();
			if (std::none_of(item.mesh->submeshes.begin(), item.mesh->submeshes.end(),
					[&](const Submesh& s) { return drawn(s, fade); })) {
				continue;
			}
			bindMesh(pass, bound, item.mesh);
			placeInstance(*item.instance, *item.mesh, transforms);
			SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
			const std::vector<anim::Matrix>& pose = item.mesh->skinned ? poseFor(*item.instance, seconds) : unposed;
			bool swaying = false;
			for (const Submesh& submesh : item.mesh->submeshes) {
				if (!drawn(submesh, fade)) {
					continue;
				}
				if (item.mesh->skinned) {
					if (submesh.rig < 0 || pose.empty()) {
						continue;
					}
					skinPalette(*item.mesh, submesh, pose, transforms);
					SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
				}
				const bool sway = submesh.leaf && !item.instance->overgrowth;
				if (sway != swaying) {
					swaying = sway;
					transforms.sway[0] = sway ? 1.0f : 0.0f;
					SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
				}
				alphaTest(pass, submesh);
				SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
			}
		}
		SDL_EndGPURenderPass(pass);
	}

	// The normals, the linear depth and the camera's motion.
	{
		GBufferUniforms g;
		g.inverseViewProjection = enhanced::inverse(viewProjection);
		g.view = m_enh.view;
		g.viewProjection = m_enh.viewProjection;
		g.previousViewProjection = m_enh.previousViewProjection;
		std::copy(texel, texel + 4, g.texel);
		SDL_GPUColorTargetInfo targets[2] {};
		targets[0].texture = m_enh.normalDepth;
		targets[1].texture = m_enh.motion;
		for (SDL_GPUColorTargetInfo& target : targets) {
			target.load_op = SDL_GPU_LOADOP_DONT_CARE;
			target.store_op = SDL_GPU_STOREOP_STORE;
		}
		SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, targets, 2, nullptr);
		SDL_BindGPUGraphicsPipeline(pass, p.gbuffer);
		SDL_PushGPUFragmentUniformData(commands, 0, &g, sizeof(g));
		SDL_GPUTextureSamplerBinding sampler {m_enh.depth, m_gpu.pointSampler()};
		SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	}

	// The moving meshes' motion, then the first person's (none: it rides the view).
	if (m_enh.taa) {
		SDL_GPUColorTargetInfo color {};
		color.texture = m_enh.motion;
		color.load_op = SDL_GPU_LOADOP_LOAD;
		color.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPUDepthStencilTargetInfo depth {};
		depth.texture = m_enh.depth;
		depth.load_op = SDL_GPU_LOADOP_LOAD;
		depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
		depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);
		SDL_BindGPUGraphicsPipeline(pass, p.motion);
		const GpuMesh* bound = nullptr;
		TransformUniforms transforms {};
		MotionUniforms motion;
		TransformUniforms previousPalette {};
		for (const DrawItem& item : m_drawList) {
			const GpuMesh& mesh = *item.mesh;
			const Instance& instance = *item.instance;
			const float fade = item.drawnFade();
			if ((!mesh.bundled && !mesh.skinned) ||
				std::none_of(mesh.submeshes.begin(), mesh.submeshes.end(), [&](const Submesh& s) { return drawn(s, fade); })) {
				continue;
			}
			// Last frame's placement if it was drawn then, else this frame's.
			const bool known = instance.previousFrame + 1 == m_enh.frame;
			bindMesh(pass, bound, &mesh);
			placeInstance(instance, mesh, transforms);
			transforms.partsUsed = kMaxMeshParts;
			SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
			motion.current = instance.world * m_enh.viewProjection;
			motion.previous = (known ? instance.previousWorld : instance.world) * m_enh.previousViewProjection;
			const std::vector<Mat4>& parts = known ? instance.previousParts : instance.parts;
			for (int i = 0; i < kMaxMeshParts; ++i) {
				motion.previousParts[i] = static_cast<size_t>(i) < parts.size() ? parts[static_cast<size_t>(i)] : Mat4 {};
			}
			const std::vector<anim::Matrix>& pose = mesh.skinned ? poseFor(instance, seconds) : unposed;
			const std::vector<anim::Matrix>& previousPose = known && !instance.previousPose.empty() ? instance.previousPose : pose;
			bool pushed = false;
			for (const Submesh& submesh : mesh.submeshes) {
				if (!drawn(submesh, fade)) {
					continue;
				}
				if (mesh.skinned) {
					if (submesh.rig < 0 || pose.empty()) {
						continue;
					}
					skinPalette(mesh, submesh, pose, transforms);
					SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
					skinPalette(mesh, submesh, previousPose, previousPalette);
					std::copy(std::begin(previousPalette.parts), std::end(previousPalette.parts), std::begin(motion.previousParts));
					pushed = false;
				}
				if (!pushed) {
					SDL_PushGPUVertexUniformData(commands, 1, &motion, sizeof(motion));
					pushed = true;
				}
				alphaTest(pass, submesh);
				SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
			}
		}
		SDL_EndGPURenderPass(pass);

		if (m_hasFirstPerson && m_sources) {
			const FirstPersonScene& scene = m_firstPerson;
			const Mat4 projection = camera.view() * Mat4::perspective(scene.fovY, aspect, scene.nearPlane, scene.farPlane);
			color.load_op = SDL_GPU_LOADOP_LOAD;
			pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
			SDL_BindGPUGraphicsPipeline(pass, p.motionOverlay);
			bound = nullptr;
			for (const FirstPersonModel& model : scene.models) {
				const GpuMesh* gpuMesh = mesh(*m_sources, model.geometry, model.geometryIndex, model.lod);
				if (!gpuMesh) {
					continue;
				}
				bindMesh(pass, bound, gpuMesh);
				TransformUniforms t {model.world * projection * m_enh.jitter, model.world, {1, 1, 0, 0}, {}};
				for (size_t i = 0; i < model.parts.size() && i < static_cast<size_t>(kMaxMeshParts); ++i) {
					t.parts[i] = model.parts[i];
				}
				motion.current = motion.previous = model.world * projection; // no motion
				for (const Submesh& submesh : gpuMesh->submeshes) {
					if (!drawn(submesh, 1.0f)) {
						continue;
					}
					if (gpuMesh->skinned) {
						if (submesh.rig < 0 || model.pose.empty()) {
							continue;
						}
						const auto& rig = gpuMesh->rigs[static_cast<size_t>(submesh.rig)];
						for (size_t b = 0; b < rig.size() && b < static_cast<size_t>(kMaxMeshParts); ++b) {
							const int node = rig[b].node;
							const anim::Matrix skin = node >= 0 && static_cast<size_t>(node) < model.pose.size()
								? anim::multiply(rig[b].meshToBone, model.pose[static_cast<size_t>(node)])
								: rig[b].meshToBone;
							std::copy(skin.begin(), skin.end(), t.parts[b].m.begin());
						}
					}
					SDL_PushGPUVertexUniformData(commands, 0, &t, pushUniformSize(t));
					std::copy(std::begin(t.parts), std::end(t.parts), std::begin(motion.previousParts));
					SDL_PushGPUVertexUniformData(commands, 1, &motion, sizeof(motion));
					alphaTest(pass, submesh);
					SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
				}
			}
			SDL_EndGPURenderPass(pass);
		}
	}

	// The ambient occlusion, then its pattern blurred away.
	if (m_enh.ssao) {
		GtaoUniforms g;
		g.projection[0] = 1.0f / m_enh.projection.at(0, 0);
		g.projection[1] = 1.0f / m_enh.projection.at(1, 1);
		g.projection[2] = m_enh.jitter.at(3, 0);
		g.projection[3] = m_enh.jitter.at(3, 1);
		g.params[0] = kAORadius;
		g.params[1] = kFarPlane * 0.98f;
		g.params[2] = std::fmod(static_cast<float>(m_enh.frame) * 0.618034f, 1.0f);
		std::copy(texel, texel + 4, g.texel);
		fullscreen(m_enh.aoRaw, p.gtao, &g, sizeof(g), {{m_enh.normalDepth, m_gpu.pointSampler()}});
		fullscreen(m_enh.ao, p.aoBlur, texel, sizeof(texel),
			{{m_enh.aoRaw, m_gpu.pointSampler()}, {m_enh.normalDepth, m_gpu.pointSampler()}});
	}
}

void LevelRenderer::showEnhancedBuffer(SDL_GPUCommandBuffer* commands)
{
	static const char* view = std::getenv("BF2142_ENHANCED_VIEW");
	if (!view) {
		return;
	}
	const std::string name = view;
	SDL_GPUTexture* buffer = name == "ao" ? m_enh.ao
		: name == "aoraw"                 ? m_enh.aoRaw
		: name == "normals"               ? m_enh.normalDepth
		: name == "motion"                ? m_enh.motion
										  : nullptr;
	if (buffer) {
		BloomUniforms copy;
		copy.params[0] = 4; // bloom.frag's copy
		fullscreenPass(commands, m_sceneColor, buffer, copy);
	}
}

// ---- TAA ----------------------------------------------------------------------------

void LevelRenderer::resolveTaa(SDL_GPUCommandBuffer* commands)
{
	SDL_GPUTexture* resolved = m_enh.history[m_enh.historyNext];
	SDL_GPUTexture* previous = m_enh.history[1 - m_enh.historyNext];
	TaaUniforms u;
	u.texel[0] = 1.0f / static_cast<float>(m_enh.size[0]);
	u.texel[1] = 1.0f / static_cast<float>(m_enh.size[1]);
	u.params[0] = m_enh.historyValid ? 1.0f : 0.0f;
	u.params[1] = kTaaHistory;
	SDL_GPUColorTargetInfo color {};
	color.texture = resolved;
	color.load_op = SDL_GPU_LOADOP_DONT_CARE;
	color.store_op = SDL_GPU_STOREOP_STORE;
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.enhancedPipelines().taa);
	SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof(u));
	SDL_GPUTextureSamplerBinding samplers[4] = {
		{m_sceneColor, m_gpu.clampSampler()},
		{previous, m_gpu.clampSampler()},
		{m_enh.motion, m_gpu.pointSampler()},
		{m_enh.normalDepth, m_gpu.pointSampler()},
	};
	SDL_BindGPUFragmentSamplers(pass, 0, samplers, 4);
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	SDL_EndGPURenderPass(pass);
	m_postSource = resolved;
	m_enh.historyNext = 1 - m_enh.historyNext;
	m_enh.historyValid = true;
}

// ---- the cascaded shadows ---------------------------------------------------------

void LevelRenderer::renderCascades(SDL_GPUCommandBuffer* commands, const Camera& camera,
	const scene::LightSettings& lights, float seconds, float aspect)
{
	const scene::ShadowSettings defaults;
	const scene::ShadowSettings& settings = m_sources && m_sources->shadows ? *m_sources->shadows : defaults;
	EnhancedShadowUniforms& u = m_enh.shadows;
	u = EnhancedShadowUniforms {};
	u.base = m_shadowUniforms; // the hemi map's constants (renderShadows)
	// The same switches as the single region's.
	if (!(settings.dynamicShadowsEnabled && settings.quality > 0 && lights.enableSun && m_gpu.emptyShadowMap())) {
		return;
	}
	const uint32_t size = std::min<uint32_t>(shadowMapTexels(settings.quality, settings.shadowMapSize), 2048);
	if (size != m_enh.cascadeSize) {
		release(m_gpu.device(), m_enh.cascadeDynamic);
		release(m_gpu.device(), m_enh.cascadeStatic);
		m_enh.cascadeDynamic = m_gpu.createShadowMap(size * 2);
		m_enh.cascadeStatic = m_gpu.createShadowMap(size * 2);
		m_enh.cascadeSize = size;
		m_enh.staticValid = false;
	}
	if (!m_enh.cascadeDynamic || !m_enh.cascadeStatic) {
		return;
	}
	// Four cascades to three times the single region's reach (120..400 m),
	// each a sphere about its slice of the view, its centre snapped to a
	// quarter of its radius (the static atlas is redrawn only when one moves).
	const float reach = std::clamp(shadowRegionRadius(settings.quality, settings.shadowMapDepthDistance) * 3.0f, 120.0f, 400.0f);
	const auto far = enhanced::cascadeSplits(camera.nearPlane, reach);
	const float tanY = std::tan(camera.fovY * 0.5f);
	const Vec3f forward = camera.forward();
	const Vec3f towardsLight = normalize(Vec3f {-lights.sunDirection[0], -lights.sunDirection[1], -lights.sunDirection[2]});
	const Vec3f up = std::fabs(towardsLight.y) > 0.99f ? Vec3f {0, 0, 1} : Vec3f {0, 1, 0};
	const float depthRange = kShadowHeadroom * 2;
	bool moved = !m_enh.staticValid || m_enh.staticInstances != m_instances.size();
	for (int i = 0; i < enhanced::kCascades; ++i) {
		const float nearSplit = i == 0 ? camera.nearPlane : far[static_cast<size_t>(i - 1)];
		const enhanced::SliceSphere sphere = enhanced::sliceSphere(nearSplit, far[static_cast<size_t>(i)], tanY * aspect, tanY);
		const float radius = std::ceil(sphere.radius * 1.25f);
		const float step = radius * 0.25f;
		const Vec3f wanted = camera.position + forward * sphere.distance;
		const Vec3f center {std::round(wanted.x / step) * step, std::round(wanted.y / step) * step,
			std::round(wanted.z / step) * step};
		const Mat4 vp = Mat4::lookAt(center + towardsLight * kShadowHeadroom, center, up) * orthographic(radius, depthRange);
		m_enh.cascadeViewProjection[i] = vp;
		m_enh.cascadeRadius[i] = radius;
		m_enh.cascadeCenter[i] = center;
		moved = moved || radius != m_enh.staticRadius[i] || center.x != m_enh.staticCenter[i].x ||
			center.y != m_enh.staticCenter[i].y || center.z != m_enh.staticCenter[i].z;
		Mat4 quadrant;
		quadrant.at(0, 0) = 0.5f;
		quadrant.at(1, 1) = 0.5f;
		quadrant.at(3, 0) = static_cast<float>(i & 1) * 0.5f;
		quadrant.at(3, 1) = static_cast<float>(i >> 1) * 0.5f;
		u.cascadeMatrix[i] = vp * kClipToTexture * quadrant;
		u.cascadeFar[i] = far[static_cast<size_t>(i)];
		// 5 cm and a texel of the light's depth (the slope bias does the rest).
		u.cascadeBias[i] = (0.05f + settings.shadowMapDepthBias + 2 * radius / static_cast<float>(size)) / depthRange;
	}
	const scene::RendererSettings switchDefaults;
	const scene::RendererSettings& switches = m_sources && m_sources->renderer ? *m_sources->renderer : switchDefaults;
	const GpuContext::EnhancedPipelines& p = m_gpu.enhancedPipelines();
	const auto drawAtlas = [&](SDL_GPUTexture* atlas, bool statics) {
		SDL_GPUDepthStencilTargetInfo depth {};
		depth.texture = atlas;
		depth.clear_depth = 1.0f;
		depth.load_op = SDL_GPU_LOADOP_CLEAR;
		depth.store_op = SDL_GPU_STOREOP_STORE;
		depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth);
		size_t casters = 0, draws = 0;
		for (int i = 0; i < enhanced::kCascades; ++i) {
			const SDL_GPUViewport viewport {static_cast<float>((i & 1) * size), static_cast<float>((i >> 1) * size),
				static_cast<float>(size), static_cast<float>(size), 0, 1};
			SDL_SetGPUViewport(pass, &viewport);
			SDL_BindGPUGraphicsPipeline(pass, m_gpu.shadowCasterPipeline());
			drawShadowCasterInstances(commands, pass, m_enh.cascadeViewProjection[i], m_enh.cascadeRadius[i], statics,
				seconds, casters, draws);
			if (statics && switches.drawTerrain) {
				SDL_BindGPUGraphicsPipeline(pass, p.terrainCaster);
				drawTerrainDepth(commands, pass, camera, m_enh.cascadeViewProjection[i], true);
			}
		}
		SDL_EndGPURenderPass(pass);
		if (std::getenv("BF2142_SHADOW_STATS")) {
			std::printf("cascades (%s): %zu casters, %zu draws, ends %.0f/%.0f/%.0f/%.0f m, radii %.0f/%.0f/%.0f/%.0f\n",
				statics ? "static" : "dynamic", casters, draws, far[0], far[1], far[2], far[3], m_enh.cascadeRadius[0],
				m_enh.cascadeRadius[1], m_enh.cascadeRadius[2], m_enh.cascadeRadius[3]);
		}
	};
	if (settings.dynamicStaticMeshShadowsEnabled && settings.shadowMapIncludeStaticMeshes) {
		if (moved) {
			drawAtlas(m_enh.cascadeStatic, true);
			m_enh.staticValid = true;
			m_enh.staticInstances = m_instances.size();
			std::copy(std::begin(m_enh.cascadeRadius), std::end(m_enh.cascadeRadius), std::begin(m_enh.staticRadius));
			std::copy(std::begin(m_enh.cascadeCenter), std::end(m_enh.cascadeCenter), std::begin(m_enh.staticCenter));
		}
	} else {
		m_enh.staticValid = false;
	}
	for (const Instance& instance : m_instances) {
		instance.shadowCaster = false;
	}
	drawAtlas(m_enh.cascadeDynamic, false);
	u.base.params[0] = 0; // the single region's map is off
	u.base.extra[0] = settings.shadowIntensityBias;
	u.base.extra[1] = 0.8f;
	u.cascadeParams[0] = static_cast<float>(enhanced::kCascades);
	u.cascadeParams[1] = 1.0f / static_cast<float>(size * 2);
	u.cascadeParams[2] = m_enh.staticValid ? 1.0f : 0.0f;
	u.viewEye[0] = camera.position.x;
	u.viewEye[1] = camera.position.y;
	u.viewEye[2] = camera.position.z;
	u.viewForward[0] = forward.x;
	u.viewForward[1] = forward.y;
	u.viewForward[2] = forward.z;
}

} // namespace dice::hfe::render
