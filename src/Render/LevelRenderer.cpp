#include "LevelRenderer.h"
#include "DistanceFade.h"
#include "OvergrowthLod.h"
#include "MeshLights.h"
#include "Scene/OvergrowthSettings.h"
#include "MeshQueues.h"
#include "SceneMath.h"
#include "RoadStatics.h"
#include "StaticLightmaps.h"
#include "TextureSuffix.h"
#include "TreeMaterials.h"

#include <array>
#include <limits>

#include "Geometry/GeometryTemplateManager.h"
#include "Geometry/MeshFile.h"
#include "Geometry/RoadFile.h"
#include "Hud/HudFont.h"
#include "IO/File/FileManager.h"
#include "Scene/EffectSettings.h"
#include "Scene/LightSettings.h"
#include "Scene/PostProductionSettings.h"
#include "Terrain/Heightmap.h"
#include "Terrain/TerrainData.h"
#include "Terrain/Undergrowth.h"
#include "Texture/TgaFile.h"
#include "Effects/Effects.h"
#include "World/InertRows.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>

namespace dice::hfe::render {

namespace {

constexpr int kDefaultTerrainPatches = 8; // colormaps per side without terrain data
// The low-detail texture's x and z planes: its v a metre up (TEXSCALE.y,
// 0x100af7e0; STTEXSCALE.y is the secondary y scale x 400 x 1.53787e-05 of
// the samples, the same a metre).
constexpr float kLowDetailVerticalScale = 0.00615148f;

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

Mat4 objectTransform(const world::Vec3& position, const world::Vec3& rotation)
{
	return Mat4::yawPitchRoll(rotation.x, rotation.y, rotation.z) *
		Mat4::translation({position.x, position.y, position.z});
}

// The lighting block's sun, sky and fog (fog.glsl: FogColor with the fog's
// fourth value and FogRange from the view's start, end and base, Fog.h).
LightingUniforms lighting(const ViewFog& view, const scene::LightSettings& lights, const std::array<float, 3>& sunColor,
	const std::array<float, 3>& skyColor, float alphaReference, bool lightmapped)
{
	const Vec3f sun = normalize(Vec3f {-lights.sunDirection[0], -lights.sunDirection[1], -lights.sunDirection[2]});
	const auto& fog = view.color;
	const float on = lights.enableSun ? 1.0f : 0.0f; // lightManager.enableSun
	const auto range = fogRange(view.startEndAndBase);
	return LightingUniforms {
		{sun.x, sun.y, sun.z, 0},
		{sunColor[0] * on, sunColor[1] * on, sunColor[2] * on, 1},
		{skyColor[0], skyColor[1], skyColor[2], 1},
		{fog[0], fog[1], fog[2], view.startEndAndBase[3]},
		{range[0], range[1], range[2], range[3]},
		{alphaReference, lightmapped ? 1.0f : 0.0f, 0, 0},
	};
}

// A material's queue (MeshQueues.h).
template <class S>
MeshQueue queueOf(const S& submesh, float fade)
{
	return meshQueue(submesh.blended, submesh.additive, fade);
}

// A material's kind and its object's fade in the mesh shader's uniforms.
template <class S>
void materialUniforms(LightingUniforms& uniforms, const S& submesh, float fade, float seconds)
{
	uniforms.material[2] = (submesh.detail ? 1.0f : 0.0f) + (submesh.dirt ? 2.0f : 0.0f) + (submesh.crack ? 4.0f : 0.0f)
		+ (submesh.additive ? kMaterialAdditive : submesh.blended ? kMaterialBlended : 0.0f);
	uniforms.material[3] = fade < 1.0f ? std::max(fade, 1e-3f) : 0.0f;
	uniforms.uvOffset[0] = scrollOffset(submesh.uvScroll[0], seconds) + submesh.uvAtlas[2];
	uniforms.uvOffset[1] = scrollOffset(submesh.uvScroll[1], seconds) + submesh.uvAtlas[3];
	uniforms.uvOffset[2] = submesh.uvAtlas[0];
	uniforms.uvOffset[3] = submesh.uvAtlas[1];
}

// The specular term's uniforms (mesh.frag specularTerm): the eye and the
// kind, the colour (staticSpecularColor for static meshes, sunSpecColor's
// SpecLUT64 for bundled ones; none without the sun) and StaticGloss
// (GeometryTemplate.setSpecularStaticGloss, 0.15 by default).
template <class S>
void specularUniforms(LightingUniforms& uniforms, const S& submesh, const Camera& camera, const scene::LightSettings& lights)
{
	uniforms.eye[0] = camera.position.x;
	uniforms.eye[1] = camera.position.y;
	uniforms.eye[2] = camera.position.z;
	uniforms.eye[3] = lights.enableSun ? static_cast<float>(submesh.specularKind) : 0.0f;
	const auto& color = submesh.specularKind <= 2 ? lights.staticSpecularColor : lights.sunSpecColor;
	uniforms.specular[0] = color[0];
	uniforms.specular[1] = color[1];
	uniforms.specular[2] = color[2];
	uniforms.specular[3] = submesh.staticGloss;
}

// A bundled or skinned mesh's kind bits (mesh.frag): the hemi map (not at
// the low lighting level, which also drops their dynamic shadows: RaShaderBM
// and RaShaderSM's DeprecationList rows 1 and 2) and its normal map.
template <class S>
void meshKindUniforms(LightingUniforms& uniforms, const S& submesh, bool hemiAndShadows)
{
	uniforms.material[2] += !hemiAndShadows ? kMaterialNoShadows : submesh.noHemi ? 0.0f : kMaterialHemi;
	if (submesh.normalMap) {
		uniforms.material[2] += kMaterialNormalMap;
	}
}

// The static per-pixel maps and the envmap in the mesh shader's uniforms
// (LightingUniforms::layers).
template <class S>
void layerUniforms(LightingUniforms& uniforms, const S& submesh, bool envMap)
{
	uniforms.layers[0] = submesh.parallax[0];
	uniforms.layers[1] = submesh.parallax[1];
	uniforms.layers[2] = (submesh.detailNormal ? kLayerDetailNormal : 0.0f) +
		(submesh.crackNormal ? kLayerCrackNormal : 0.0f) + (submesh.parallax[0] > 0 ? kLayerParallax : 0.0f) +
		(envMap && submesh.envMap ? kLayerEnvMap : 0.0f);
}

float transparencyKey(const Mat4& world, const Vec3f& camera)
{
	const float origin[3] = {world.at(3, 0), world.at(3, 1), world.at(3, 2)};
	const float eye[3] = {camera.x, camera.y, camera.z};
	return render::transparencyKey(origin, eye);
}

} // namespace

Vec3f Camera::forward() const
{
	return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}

Vec3f Camera::right() const
{
	return {std::cos(yaw), 0, -std::sin(yaw)};
}

Mat4 Camera::view() const
{
	if (roll == 0) {
		return Mat4::lookAt(position, position + forward(), {0, 1, 0});
	}
	// The world's up turned about the view axis (Rodrigues).
	const Vec3f k = forward();
	const float c = std::cos(roll), s = std::sin(roll);
	const Vec3f kxUp {-k.z, 0, k.x}; // k x (0, 1, 0)
	const Vec3f up {kxUp.x * s + k.x * k.y * (1 - c), c + k.y * k.y * (1 - c), kxUp.z * s + k.z * k.y * (1 - c)};
	return Mat4::lookAt(position, position + k, up);
}

struct LevelRenderer::FpsText {
	std::optional<hud::HudFont> font;
	std::string texture; // the .dds of the .dif's name
	bool loaded = false;
	uint64_t lastNs = 0;
	double milliseconds = 0;
};

LevelRenderer::LevelRenderer(GpuContext& gpu) : m_gpu(gpu) {}

LevelRenderer::~LevelRenderer()
{
	SDL_GPUDevice* device = m_gpu.device();
	SDL_WaitForGPUIdle(device);
	for (auto& [name, mesh] : m_meshes) {
		if (mesh) {
			SDL_ReleaseGPUBuffer(device, mesh->vertices);
			SDL_ReleaseGPUBuffer(device, mesh->indices);
		}
	}
	for (auto& [path, texture] : m_textures) {
		if (texture) {
			SDL_ReleaseGPUTexture(device, texture);
		}
	}
	for (TerrainPatch& patch : m_terrain) {
		SDL_ReleaseGPUBuffer(device, patch.vertices);
	}
	for (WaterBlock& block : m_waterBlocks) {
		SDL_ReleaseGPUBuffer(device, block.vertices);
		SDL_ReleaseGPUBuffer(device, block.indices);
	}
	for (SurroundingPatch& patch : m_surrounding) {
		SDL_ReleaseGPUBuffer(device, patch.vertices);
	}
	if (m_surroundingIndices) {
		SDL_ReleaseGPUBuffer(device, m_surroundingIndices);
	}
	for (auto& [path, texture] : m_cubeTextures) {
		if (texture) {
			SDL_ReleaseGPUTexture(device, texture);
		}
	}
	if (m_terrainIndices) {
		SDL_ReleaseGPUBuffer(device, m_terrainIndices);
	}
	if (m_terrainLodIndices) {
		SDL_ReleaseGPUBuffer(device, m_terrainLodIndices);
	}
	m_gpu.releaseDynamicBuffer(m_overlayVertices);
	m_gpu.releaseDynamicBuffer(m_menuVertices);
	for (const auto& [key, entry] : m_menuTextures) {
		if (entry.texture) {
			SDL_ReleaseGPUTexture(device, entry.texture);
		}
	}
	m_gpu.releaseDynamicBuffer(m_particleVertices);
	m_gpu.releaseDynamicBuffer(m_shimmerVertices);
	m_gpu.releaseDynamicBuffer(m_firstPersonParticleVertices);
	m_gpu.releaseDynamicBuffer(m_decalVertices);
	for (Road& road : m_roads) {
		SDL_ReleaseGPUBuffer(device, road.vertices);
		SDL_ReleaseGPUBuffer(device, road.indices);
	}
	for (auto& [index, patch] : m_undergrowthPatches) {
		if (patch.vertices) {
			SDL_ReleaseGPUBuffer(device, patch.vertices);
			SDL_ReleaseGPUBuffer(device, patch.indices);
		}
	}
	if (m_shadowDynamic) {
		SDL_ReleaseGPUTexture(device, m_shadowDynamic);
	}
	if (m_shadowStatic) {
		SDL_ReleaseGPUTexture(device, m_shadowStatic);
	}
	if (m_flareVertices) {
		SDL_ReleaseGPUBuffer(device, m_flareVertices);
	}
	releasePostTargets();
	releaseEnhancedTargets();
	releaseMultisampleTargets();
}

SDL_GPUTexture* LevelRenderer::texture(const io::FileManager& files, const std::string& path,
	scene::TextureClass textureClass)
{
	// VideoSettings' texture quality: the class's mip skips (0x10017710).
	const int skip = m_sources && m_sources->quality ? m_sources->quality->skipMipLevels(textureClass) : 0;
	// The per-frame callers (the HUD's quads, the particles' groups) ask
	// for the same paths each frame: found by the path as given first, no
	// lower-casing or key building.
	auto& byPath = m_texturesByPath[static_cast<size_t>(textureClass) % m_texturesByPath.size()];
	if (const auto hit = byPath.find(path); hit != byPath.end() && hit->second.first == skip) {
		return hit->second.second;
	}
	const std::string key = skip > 0 ? lower(path) + "#skip" + std::to_string(skip) : lower(path);
	const auto it = m_textures.find(key);
	if (it != m_textures.end()) {
		byPath[path] = {skip, it->second};
		return it->second;
	}
	SDL_GPUTexture* gpuTexture = nullptr;
	std::vector<uint8_t> data;
	// The level's customTextureSuffix variant where one exists (0x10018ca0).
	const std::string variant = customSuffixedTexturePath(path, m_textureSuffix);
	const bool swapped = !variant.empty() && files.readFile(variant, data);
	std::string found = swapped ? variant : files.readFile(path, data) ? lower(path) : std::string();
	if (found.empty()) {
		// Not there by its own name: with ".dds", then bare (0x10017340).
		for (const std::string& name : textureFallbackPaths(path)) {
			if (files.readFile(name, data)) {
				found = lower(name);
				break;
			}
		}
	}
	if (!found.empty()) {
		// By the name read (the key may carry "#skip").
		const std::string& read = found;
		const bool targa = read.size() > 4 && read.compare(read.size() - 4, 4, ".tga") == 0;
		if (auto image = targa ? texture::parseTga(data) : texture::parseDds(data)) {
			texture::skipTopLevels(*image, skip);
			gpuTexture = m_gpu.createTexture(*image);
			if (gpuTexture) {
				m_textureSizes[gpuTexture] = {image->width, image->height};
			}
		}
	}
	++(gpuTexture ? m_stats.textures : m_stats.missingTextures);
	m_textures[key] = gpuTexture;
	byPath[path] = {skip, gpuTexture};
	return gpuTexture;
}

SDL_GPUTexture* LevelRenderer::cubeTexture(const io::FileManager& files, const std::string& path)
{
	const std::string key = lower(path);
	const auto it = m_cubeTextures.find(key);
	if (it != m_cubeTextures.end()) {
		return it->second;
	}
	SDL_GPUTexture* gpuTexture = nullptr;
	std::vector<uint8_t> data;
	const std::string variant = customSuffixedTexturePath(path, m_textureSuffix); // 0x10018ca0
	if ((!variant.empty() && files.readFile(variant, data)) || files.readFile(path, data)) {
		if (const auto image = texture::parseDds(data)) {
			gpuTexture = m_gpu.createTexture(*image, true);
		}
	}
	m_cubeTextures[key] = gpuTexture;
	return gpuTexture;
}

const LevelRenderer::GpuMesh* LevelRenderer::mesh(const LevelSources& sources, const std::string& geometryName,
	int wantedGeometry, int wantedLod, bool overgrowth)
{
	const std::string key = (wantedGeometry < 0 && wantedLod == 0
		? lower(geometryName)
		: lower(geometryName) + "#" + std::to_string(wantedGeometry) + "/" + std::to_string(wantedLod))
		+ (overgrowth ? "#overgrowth" : "");
	const auto it = m_meshes.find(key);
	if (it != m_meshes.end()) {
		return it->second.get();
	}
	std::unique_ptr<GpuMesh>& slot = m_meshes[key];
	const geom::GeometryTemplate* geometry = sources.geometries.find(geometryName);
	std::vector<uint8_t> data;
	const auto missing = [&](const std::string& why) -> const GpuMesh* {
		++m_stats.missingMeshes;
		m_stats.missingMeshNames.push_back(geometryName + ": " + why);
		return nullptr;
	};
	if (!geometry) {
		return missing("no geometry template");
	}
	if (geometry->kind == geom::MeshKind::Road) {
		return nullptr; // drawn by buildRoads
	}
	if (!sources.files.readFile(geometry->path, data)) {
		return missing("can't read " + geometry->path);
	}
	const auto file = geom::parseMesh(data, geometry->kind);
	if (!file || file->geometries.empty()) {
		return missing("can't parse " + geometry->path);
	}
	// Bundled and skinned meshes hold the first-person, exterior (and
	// wreck) geometries; the world draws the exterior.
	const bool bundled = geometry->kind == geom::MeshKind::Bundled;
	const bool skinned = geometry->kind == geom::MeshKind::Skinned;
	const size_t geometryIndex = wantedGeometry >= 0 ? static_cast<size_t>(wantedGeometry)
		: (bundled || skinned) && file->geometries.size() > 1 ? 1 : 0;
	if (geometryIndex >= file->geometries.size()) {
		return missing("no geometry " + std::to_string(geometryIndex) + " in " + geometry->path);
	}
	if (file->geometries[geometryIndex].lods.empty()) {
		return missing("no LODs in " + geometry->path);
	}
	const size_t lodIndex = std::min(static_cast<size_t>(std::max(wantedLod, 0)), file->geometries[geometryIndex].lods.size() - 1);
	const geom::MeshLod& lod = file->geometries[geometryIndex].lods[lodIndex];
	const geom::VertexElement* position = file->element(geom::VertexUsage::Position);
	const geom::VertexElement* normal = file->element(geom::VertexUsage::Normal);
	const geom::VertexElement* texCoord = file->element(geom::VertexUsage::TexCoord);
	// The lightmap coordinates are the last texture coordinate set.
	const geom::VertexElement* lightmapCoord = nullptr;
	for (const geom::VertexElement& e : file->vertexElements) {
		if (e.usage == geom::VertexUsage::TexCoord && e.usageIndex > 0 &&
			(!lightmapCoord || e.usageIndex > lightmapCoord->usageIndex)) {
			lightmapCoord = &e;
		}
	}
	if (!position) {
		return missing("no vertex positions");
	}
	// Static meshes' layer coordinates sit in fixed sets: 1 detail, 2 dirt,
	// 3 crack (4 is the lightmap's).
	const geom::VertexElement* layerCoords[3] = {
		file->element(geom::VertexUsage::TexCoord, 1),
		file->element(geom::VertexUsage::TexCoord, 2),
		file->element(geom::VertexUsage::TexCoord, 3),
	};
	// A bundled mesh's part index is the first byte of its blend indices; a
	// skinned mesh's two bones are the first two bytes, weighted by the
	// blend weight (for the first).
	const geom::VertexElement* partIndex =
		bundled || skinned ? file->element(geom::VertexUsage::BlendIndices) : nullptr;
	const geom::VertexElement* blendWeight = skinned ? file->element(geom::VertexUsage::BlendWeight) : nullptr;
	// The tangents (Vertex::tangent): the binormal is cross(tangent,
	// normal) times 1 - 2 x the third blend index byte for bundled and
	// skinned meshes (RaShaderBM/SM createWorld2TanMat / getTangentBasis),
	// -1 for static ones (RaShaderSTM getBinormalFlipping with a FLOAT3
	// position's w of 1).
	const geom::VertexElement* tangent = file->element(geom::VertexUsage::Tangent);
	std::vector<Vertex> vertices(file->vertexCount);
	for (uint32_t i = 0; i < file->vertexCount; ++i) {
		Vertex& v = vertices[i];
		file->readFloats(i, *position, v.position, 3);
		if (!normal || !file->readFloats(i, *normal, v.normal, 3)) {
			v.normal[0] = 0;
			v.normal[1] = 1;
			v.normal[2] = 0;
		}
		if (!texCoord || !file->readFloats(i, *texCoord, v.texCoord, 2)) {
			v.texCoord[0] = v.texCoord[1] = 0;
		}
		if (!lightmapCoord || !file->readFloats(i, *lightmapCoord, v.lightmapCoord, 2)) {
			v.lightmapCoord[0] = v.texCoord[0];
			v.lightmapCoord[1] = v.texCoord[1];
		}
		float* layerTargets[3] = {v.detailCoord, v.dirtCoord, v.crackCoord};
		for (int layer = 0; layer < 3; ++layer) {
			if (!layerCoords[layer] || !file->readFloats(i, *layerCoords[layer], layerTargets[layer], 2)) {
				layerTargets[layer][0] = v.texCoord[0];
				layerTargets[layer][1] = v.texCoord[1];
			}
		}
		const size_t vertexOffset = static_cast<size_t>(i) * file->vertexStride;
		v.tangent[0] = 1;
		v.tangent[1] = v.tangent[2] = 0;
		if (tangent) {
			file->readFloats(i, *tangent, v.tangent, 3);
		}
		v.tangent[3] = partIndex ? 1.0f - 2.0f * file->vertices[vertexOffset + partIndex->offset + 2]
			: bundled || skinned ? 1.0f : -1.0f;
		v.part = partIndex ? file->vertices[vertexOffset + partIndex->offset] : 0.0f;
		v.part2 = skinned && partIndex ? file->vertices[vertexOffset + partIndex->offset + 1] : v.part;
		// A bundled vertex's UV matrix (RaShaderBM getSkinnedUVMatrix: the
		// fourth blend index byte) rides in the dirt coordinates, which
		// only static meshes use; its TexUVRotCenter is texture set 1, the
		// detail coordinates.
		if (bundled && partIndex) {
			v.dirtCoord[0] = file->vertices[vertexOffset + partIndex->offset + 3];
		}
		v.weight = 1;
		if (blendWeight) {
			file->readFloats(i, *blendWeight, &v.weight, 1);
		}
	}
	auto gpuMesh = std::make_unique<GpuMesh>();
	gpuMesh->bundled = bundled;
	gpuMesh->skinned = skinned;
	if (skinned) gpuMesh->skinnedRadius = boxHalfDiagonal(lod.bounds.min, lod.bounds.max);
	gpuMesh->radiusShadowBounds = geometry->useRadiusForShadowDepth || geometry->useRadiusForShadowSpatial;
	gpuMesh->titanInterior = geometry->titanInterior;
	gpuMesh->titanExterior = geometry->titanExterior;
	gpuMesh->titanPart = geometry->titanExteriorPart;
	gpuMesh->boxMin = {lod.bounds.min[0], lod.bounds.min[1], lod.bounds.min[2]};
	gpuMesh->boxMax = {lod.bounds.max[0], lod.bounds.max[1], lod.bounds.max[2]};
	gpuMesh->geometries = file->geometries.size();
	gpuMesh->lodCount = file->geometries[geometryIndex].lods.size();
	if (geometryIndex < geometry->lodDistances.size()) {
		gpuMesh->lodDistances = geometry->lodDistances[geometryIndex];
	}
	// The third-person and wreck geometries' skipped LODs (bundled
	// 0x100a31d0: of three geometries 1 takes maxSkip3pLods and 2
	// maxSkipWreckLods, of two 1 the former; skinned 0x100d4b..: the third
	// person's, geometry 0 of one and 1 of two), at most renderer +0x910 (the
	// geometry quality's) and one less than the LODs. Static meshes
	// (0x100dd480): geometry 0 maxSkip3pLods (+0xc0), 1 maxSkipWreckLods
	// (+0xc4), none for an isTitanExterior template (its slot 0xcc, +0xc8:
	// the hull's parts pair their LODs, TitanParts.h).
	if (wantedLod == 0) {
		const size_t count = file->geometries.size();
		unsigned wanted = 0;
		if (bundled && count == 3) {
			wanted = geometryIndex == 1 ? geometry->maxSkip3pLods : geometryIndex == 2 ? geometry->maxSkipWreckLods : 0;
		} else if (!bundled && !skinned) {
			wanted = geometry->titanExterior ? 0u
				: geometryIndex == 0         ? geometry->maxSkip3pLods
				: geometryIndex == 1         ? geometry->maxSkipWreckLods
											 : 0u;
		} else if (count == 2 || (skinned && count == 1)) {
			wanted = geometryIndex == count - 1 ? geometry->maxSkip3pLods : 0;
		}
		const unsigned cap = sources.quality ? static_cast<unsigned>(sources.quality->maxSkip3pLods()) : 0u;
		gpuMesh->skippedLods = static_cast<int>(std::min({wanted, cap, static_cast<unsigned>(gpuMesh->lodCount - 1)}));
	}
	// The bounds of this LOD's vertices, by part (culling).
	if (!skinned) {
		std::vector<std::vector<Vec3f>> partPoints(bundled ? kMaxMeshParts : 1);
		std::vector<Vec3f> all;
		for (const geom::MeshMaterial& material : lod.materials) {
			for (uint32_t i = 0; i < material.indexCount; ++i) {
				const size_t index = static_cast<size_t>(material.indexStart) + i;
				if (index >= file->indices.size()) {
					break;
				}
				const size_t vertex = static_cast<size_t>(material.vertexStart) + file->indices[index];
				if (vertex >= vertices.size()) {
					continue;
				}
				const Vertex& v = vertices[vertex];
				const Vec3f p {v.position[0], v.position[1], v.position[2]};
				const size_t part = bundled ? std::min(static_cast<size_t>(v.part), partPoints.size() - 1) : 0;
				partPoints[part].push_back(p);
				all.push_back(p);
			}
		}
		// RendDX9's bundled-mesh load (0x100a0dc0, with GSCreateSubPartBoundingBoxes)
		// grows a box per part from its vertices' part byte (+0x18); here a
		// sphere per part.
		for (const auto& points : partPoints) {
			gpuMesh->partBounds.push_back(sphereAround(points));
		}
		gpuMesh->bounds = sphereAround(all);
	}
	if (skinned) {
		for (const auto& rig : lod.rigs) {
			std::vector<RigBone> bones;
			for (const geom::Bone& bone : rig) {
				anim::Matrix m;
				std::copy(bone.matrix.begin(), bone.matrix.end(), m.begin());
				bones.push_back({bone.id, m});
			}
			gpuMesh->rigs.push_back(std::move(bones));
		}
	}
	gpuMesh->vertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
		static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
	gpuMesh->indices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_INDEX, file->indices.data(),
		static_cast<uint32_t>(file->indices.size() * sizeof(uint16_t)));
	const auto& materials = lod.materials;
	// The texture class of the mesh's materials (scene::TextureClass): a
	// tree's are the overgrowth's (0x10151830 with the template's +0x2a2).
	const bool tree = !bundled && !skinned && lower(geometry->path).find("vegitation") != std::string::npos;
	gpuMesh->tree = tree;
	gpuMesh->waterObject = !bundled && !skinned && lower(geometry->path).find("objects/water/") != std::string::npos;
	gpuMesh->road = !bundled && !skinned && roadStaticPath(geometry->path);
	gpuMesh->roadNoBlend = gpuMesh->road && roadStaticNoBlend(geometry->path);
	const scene::TextureClass textureClass = skinned ? scene::TextureClass::SkinnedMesh
		: bundled                                    ? scene::TextureClass::BundledMesh
		: tree                                       ? scene::TextureClass::Overgrowth
													 : scene::TextureClass::StaticMesh;
	// The lighting quality's level (renderer +0x6b4): normal maps only at 0,
	// static meshes' dirt and crack layers not at 3 (QualitySettings).
	const scene::QualitySettings defaults;
	const scene::QualitySettings& quality = sources.quality ? *sources.quality : defaults;
	for (size_t m = 0; m < materials.size(); ++m) {
		geom::MeshMaterial material = materials[m];
		for (std::string& path : material.maps) {
			path = meshTexturePath(path, geometry->path,
				[&](const std::string& candidate) { return sources.files.exists(candidate); });
		}
		// Alpha-tested materials use "<name>_alphatest.dds" where it exists:
		// the plain textures keep alpha in 0..0.5 for alpha to coverage
		// (0x10151830). A tree's maps all do (its +0x2a2 branch): its
		// leaves are alpha-tested whatever their alpha mode (treeLeaf).
		// Loaded antialiased, a tree's leaves keep the plain ones (the
		// branch is skipped while 0x1000b500 and +0x50a hold), and so do
		// the overgrowth's distant leaves: 0x1008c7e0 reads only
		// OvergrowthAtlas.tai then. (The rest of a tree, its trunks and
		// their alpha-tested "tile_" maps, keep "_alphatest": the rewrite's
		// trunks have no shader to double a plain alpha.)
		std::string diffusePath = material.maps.empty() ? std::string() : material.maps[0];
		const TreeMaterial overgrowthKind = overgrowthMaterial(diffusePath); // by the mesh's own name
		const bool leaf = treeLeaf(tree, material.maps) || (overgrowth && tree && overgrowthKind == TreeMaterial::Leaf);
		const bool coverage = leaf && m_coverageTextures;
		if ((material.alphaMode == 2 || tree) && !coverage && diffusePath.size() > 4) {
			const std::string alphaTest = diffusePath.substr(0, diffusePath.size() - 4) + "_alphatest.dds";
			if (sources.files.exists(alphaTest)) {
				diffusePath = alphaTest;
			}
		}
		bool plainTexture = coverage && !diffusePath.empty() && sources.files.exists(diffusePath);
		// The overgrowth's distant meshes use textures only its atlases hold.
		const AtlasEntry* atlas = nullptr;
		if (!diffusePath.empty() && !m_overgrowthAtlas.empty() && !sources.files.exists(diffusePath)) {
			std::string key = lower(material.maps[0]);
			std::replace(key.begin(), key.end(), '\\', '/');
			const bool alphaTestFirst = material.alphaMode == 2 && !coverage;
			const auto& first = alphaTestFirst ? m_overgrowthAlphaTestAtlas : m_overgrowthAtlas;
			const auto& second = alphaTestFirst ? m_overgrowthAtlas : m_overgrowthAlphaTestAtlas;
			auto found = first.find(key);
			if (found != first.end()) {
				atlas = &found->second;
				plainTexture = coverage;
			} else if ((found = second.find(key)) != second.end()) {
				atlas = &found->second;
			}
			if (atlas) {
				diffusePath = atlas->atlas;
			}
		}
		SDL_GPUTexture* diffuse = diffusePath.empty() ? nullptr
			: texture(sources.files, diffusePath, atlas ? scene::TextureClass::Overgrowth : textureClass);
		Submesh submesh {material.indexStart, material.indexCount, static_cast<int32_t>(material.vertexStart),
			diffuse ? diffuse : m_gpu.whiteTexture(), material.alphaMode == 2 ? 0.5f : 0.0f,
			skinned && m < gpuMesh->rigs.size() ? static_cast<int>(m) : -1};
		if (atlas) {
			submesh.uvAtlas[0] = atlas->size[0];
			submesh.uvAtlas[1] = atlas->size[1];
			submesh.uvAtlas[2] = atlas->offset[0];
			submesh.uvAtlas[3] = atlas->offset[1];
		}
		// Alpha mode 1 normally blends over the scene. StaticMesh.fx's
		// alpha_one technique explicitly uses SRCALPHA / ONE too: glow3
		// and similar meshes have opaque black around their light texture.
		// Respect that material technique independently of a geometry's
		// hasAdditiveAlpha switch, which selects the BMadditive path.
		const std::string technique = lower(material.technique);
		const std::string effect = lower(material.effect);
		submesh.blended = material.alphaMode == 1;
		submesh.additive = submesh.blended && (geometry->hasAdditiveAlpha
			|| (effect == "staticmesh.fx" && technique == "alpha_one"));
		if (submesh.additive) {
			submesh.uvScroll[0] = geometry->simpleUVTranslation[0];
			submesh.uvScroll[1] = geometry->simpleUVTranslation[1];
		}
		// StaticMesh.fx techniques name their layers: "BaseDetailDirtCrack..."
		// with maps base, detail, dirt, crack in that order (normal maps follow).
		if (!submesh.additive) {
			if (effect == "staticmesh.fx" && !skinned) {
				submesh.specularKind = technique.find("detail") != std::string::npos && material.alphaMode != 2 ? 2 : 1;
			} else if (effect == "bundledmesh.fx") {
				submesh.specularKind = technique.find("colormapgloss") != std::string::npos ? 4 : 3;
			}
		}
		// The static template's load (0x100de6d0): a geometry whose path has
		// "vegitation" is a tree (+0x2a2); its leaves (treeLeaf, 0x100db6f0)
		// are drawn with RaShaderLeaf, which alpha-tests them at 127 whatever
		// their alpha mode (AlphaTestEnable true, AlphaRef 127).
		submesh.leaf = leaf;
		if (submesh.leaf) {
			submesh.alphaReference = 0.5f;
			submesh.coverageTexture = plainTexture && diffuse;
		}
		const auto treeKind = overgrowth && tree ? overgrowthKind
			: treeMaterial(tree, material.alphaMode, effect == "staticmesh.fx" && technique.compare(0, 10, "basedetail") == 0);
		submesh.trunk = treeTrunk(leaf, treeKind);
		if (submesh.trunk) submesh.specularKind = 0;
		// BundledMesh.fx's "AnimatedUV..." techniques (_HASUVANIMATION_).
		submesh.animatedUV = bundled && technique.find("animateduv") != std::string::npos;
		// A "...NoHemiLight" technique drops _USEHEMIMAP_ (the material's +0x3d set at
		// 0x101496a0; permutation bit 0x40 only without it: 0x100c4028 / 0x100c5709).
		submesh.noHemi = bundled && technique.find("nohemilight") != std::string::npos;
		if (lower(material.effect) == "staticmesh.fx" && technique.compare(0, 10, "basedetail") == 0) {
			const bool hasDirt = technique.find("dirt") != std::string::npos;
			const bool hasCrack = technique.find("crack") != std::string::npos;
			// RaShaderSTM.mfx's literals: NDetail and NCrack's normal maps
			// follow the colour layers (the "_deb" / "_crb" maps), and
			// "parallaxdetail" takes the detail normal map's alpha as the
			// height.
			const bool hasDetailNormal = technique.find("ndetail") != std::string::npos;
			const bool hasCrackNormal = technique.find("ncrack") != std::string::npos;
			const bool parallax = technique.find("parallaxdetail") != std::string::npos;
			size_t map = 1;
			// The lighting level drops the dirt and crack maps and the
			// normal maps (0x10151830 skips "_di" / "_cr" / "_deb" / "_crb"
			// without their literals; RaShaderSTM.mfx's DeprecationList:
			// parallax goes first, then crack and ncrack, then ndetail and
			// dirt).
			const auto next = [&](bool load) -> SDL_GPUTexture* {
				if (map >= material.maps.size()) {
					return nullptr;
				}
				const std::string& path = material.maps[map++];
				return load ? texture(sources.files, path, textureClass) : nullptr;
			};
			submesh.detail = next(true);
			submesh.dirt = hasDirt ? next(quality.staticMeshDirtAndCrack()) : nullptr;
			submesh.crack = hasCrack ? next(quality.staticMeshDirtAndCrack()) : nullptr;
			submesh.detailNormal = hasDetailNormal || parallax ? next(quality.staticMeshDirtAndCrack()) : nullptr;
			submesh.crackNormal = hasCrackNormal && submesh.crack ? next(quality.staticMeshDirtAndCrack()) : nullptr;
			// ParallaxScaleBias (0x100d9c00): the template's height scale
			// for the material (GeometryTemplate.setMaterialParallaxHeightScale,
			// 15 by default: 0x100db690) over the detail map's width and
			// height; the bias is -0.5 x it.
			if (parallax && submesh.detailNormal && submesh.detail && quality.meshNormalMaps()) {
				const auto scale = geometry->parallaxHeightScale.find(geom::GeometryTemplate::materialKey(
					static_cast<int>(geometryIndex), static_cast<int>(lodIndex), static_cast<int>(m)));
				const float height = scale != geometry->parallaxHeightScale.end() ? scale->second : 15.0f;
				const auto size = m_textureSizes.find(submesh.detail);
				if (size != m_textureSizes.end() && size->second[0] > 0 && size->second[1] > 0) {
					submesh.parallax[0] = height / static_cast<float>(size->second[0]);
					submesh.parallax[1] = height / static_cast<float>(size->second[1]);
				}
			}
			if (submesh.detailNormal) {
				++m_stats.normalMapped;
			}
		}
		// The level's envmap: every static mesh's material (0x100bdb00 sets
		// each drawable's cube, +0x13c, to the EnvMapManager's), a bundled
		// one's with an "envmap" technique (0x100c5030: the material's +0x37,
		// +0x120) above the low lighting level (RaShaderBM.mfx drops envmap
		// with the hemi map).
		submesh.envMap = !submesh.additive &&
			((effect == "staticmesh.fx" && !skinned) ||
				(bundled && technique.find("envmap") != std::string::npos && quality.meshHemiAndShadows()));
		if (submesh.trunk) submesh.envMap = false;
		// Normal maps (RaShaderBM/SM _HASNORMALMAP_), only at the top
		// lighting level: the loaders skip "_b.dds" (bundled, 0x101499b0) and
		// "_b.dds" / "_b_os.dds" (skinned, 0x100d4160) otherwise. A bundled
		// mesh's second map ending "_b.dds" is its tangent-space normal map
		// (+0x35); a skinned mesh's second map, unless the SpecularLUT, is in
		// tangent space when its technique names "tangent" (+0x100), else in
		// the object's (_OBJSPACENORMALMAP_, the first person's arms).
		if ((bundled || skinned) && material.maps.size() > 1 && quality.meshNormalMaps() && !submesh.additive) {
			const int kind = normalMapKind(bundled, skinned, lower(material.maps[1]), technique);
			if (kind != 0) {
				submesh.normalMap = texture(sources.files, material.maps[1], textureClass);
				if (submesh.normalMap) {
					submesh.normalKind = kind;
					++m_stats.normalMapped;
				}
			}
		}
		if (skinned && !submesh.additive) {
			submesh.specularKind = 5; // RaShaderSM: pow(N.H, SpecularPower) x the gloss
		}
		if (bundled) {
			const auto gloss = geometry->staticGloss.find(geom::GeometryTemplate::materialKey(
				static_cast<int>(geometryIndex), static_cast<int>(lodIndex), static_cast<int>(m)));
			if (gloss != geometry->staticGloss.end()) {
				submesh.staticGloss = gloss->second;
			}
		}
		gpuMesh->submeshes.push_back(submesh);
	}
	++m_stats.meshes;
	slot = std::move(gpuMesh);
	return slot.get();
}

void LevelRenderer::addTemplate(const LevelSources& sources, const world::ObjectTemplate& objectTemplate,
	const Mat4& local, const Mat4& objectWorld, uint32_t object, int depth, const std::string& path,
	const std::string& inheritedSkeleton)
{
	if (depth > 8) {
		return; // guards against templates that contain themselves
	}
	if (objectTemplate.typeName() == "LightSource") {
		const effects::LightSourceLook look = effects::lightSourceOf(objectTemplate);
		if (look.enabled && !look.directional && object != 0) {
			m_objectLights.push_back({object, local, look});
		}
		return;
	}
	if (!objectTemplate.geometry().empty()) {
		if (const GpuMesh* gpuMesh = mesh(sources, objectTemplate.geometry())) {
			const Mat4 world = local * objectWorld;
			Instance instance {gpuMesh, world, object, local, nullptr, {1, 1, 0, 0}, {}, nullptr, nullptr};
			instance.geometry = objectTemplate.geometry();
			if (depth > 0) {
				instance.source = path; // the template names from the root's child down, '/'-joined
			}
			if (gpuMesh->skinned) {
				const auto skeleton3P = objectTemplate.settings().find("skeleton3P");
				const auto skeletonPath = objectTemplate.settings().find("skeleton");
				const std::string& selected = skeleton3P != objectTemplate.settings().end() ? skeleton3P->second
					: skeletonPath != objectTemplate.settings().end() ? skeletonPath->second : inheritedSkeleton;
				if (!selected.empty()) {
					instance.skeleton = skeleton(sources.files, selected);
				} else if (objectTemplate.typeName() == "Kit") {
					// shortcut: the soldiers' shared 3P skeleton, all shipped soldiers name it; read it from the wearer if a mod differs.
					instance.skeleton = skeleton(sources.files, "Objects/Soldiers/Common/Animations/3p_setup.ske");
					instance.wornKit = true;
				}
				if (!sources.previewAnimation.empty()) {
					instance.animation = animation(sources.files, sources.previewAnimation);
				}
			}
			if (gpuMesh->bundled) {
				instance.parts.assign(kMaxMeshParts, Mat4 {});
				collectParts(sources, objectTemplate, Mat4 {}, instance.parts, 0);
				instance.partsTemplate = &objectTemplate;
			}
			lightmapFor(sources, objectTemplate.geometry(), 0, world, instance.lightmap, instance.lightmapTransform);
			if (instance.lightmap) {
				++m_stats.lightmappedInstances;
			}
			m_instances.push_back(instance);
		}
	}
	std::string childSkeleton = inheritedSkeleton;
	if (const auto it = objectTemplate.settings().find("skeleton3P"); it != objectTemplate.settings().end()) {
		childSkeleton = it->second;
	} else if (const auto it = objectTemplate.settings().find("skeleton"); it != objectTemplate.settings().end()) {
		childSkeleton = it->second;
	}
	// Object.disableChildren: a placed object's children are out of the scene.
	if (depth == 0 && object != 0) {
		if (const world::Object* placed = sources.objects.find(object); placed && placed->childrenDisabled()) {
			return;
		}
	}
	// A kit's children are its inventory (knife, rifle, beacon...), made as objects of their own, not parts of its mesh.
	if (objectTemplate.typeName() == "Kit") {
		return;
	}
	for (const world::ChildTemplate& child : objectTemplate.children()) {
		if (const world::ObjectTemplate* childTemplate = sources.templates.find(child.name);
			childTemplate && drawnWithObject(childTemplate->typeName())) {
			addTemplate(sources, *childTemplate, objectTransform(child.position, child.rotation) * local, objectWorld,
				object, depth + 1, path.empty() ? child.name : path + "/" + child.name, childSkeleton);
		}
	}
}

std::shared_ptr<anim::Skeleton> LevelRenderer::skeleton(const io::FileManager& files, const std::string& path)
{
	auto& slot = m_skeletons[lower(path)];
	std::vector<uint8_t> data;
	if (!slot && files.readFile(path, data)) {
		if (auto parsed = anim::parseSkeleton(data)) {
			slot = std::make_shared<anim::Skeleton>(std::move(*parsed));
		}
	}
	return slot;
}

std::shared_ptr<anim::Animation> LevelRenderer::animation(const io::FileManager& files, const std::string& path)
{
	auto& slot = m_animations[lower(path)];
	std::vector<uint8_t> data;
	if (!slot && files.readFile(path, data)) {
		if (auto parsed = anim::parseAnimation(data)) {
			slot = std::make_shared<anim::Animation>(std::move(*parsed));
		}
	}
	return slot;
}

// A vehicle's parts (turrets, wheels, doors) are child templates with a
// "geometryPart" index into the parent's bundled mesh; each part is drawn
// with its template's position relative to the object.
const LevelRenderer::AnimatedUVLook& LevelRenderer::animatedUVLook(const world::ObjectTemplate& objectTemplate) const
{
	const auto known = m_animatedUVLooks.find(&objectTemplate);
	if (known != m_animatedUVLooks.end()) {
		return known->second;
	}
	AnimatedUVLook look;
	AnimatedUVSettings& s = look.settings;
	const auto& settings = objectTemplate.settings();
	const auto text = [&settings](const char* name) -> const std::string* {
		const auto found = settings.find(name);
		return found != settings.end() ? &found->second : nullptr;
	};
	const auto flag = [&text](const char* name, bool& out) {
		if (const std::string* t = text(name)) {
			out = std::atoi(t->c_str()) != 0;
		}
	};
	const auto vec2 = [&text](const char* name, float out[2]) {
		if (const std::string* t = text(name)) {
			parseAnimatedUVVec2(*t, out);
		}
	};
	// The setters 0x0051f30b..0x0051ff58 (template +0x12c..+0x160).
	flag("animatedUVTranslation", s.translation);
	vec2("animatedUVTranslationSpeed", s.speed);
	vec2("animatedUVTranslationMax", s.max);
	vec2("animatedUVTranslationSize", s.size);
	if (const std::string* t = text("animatedUVTranslationIndex")) {
		s.translationIndex = std::atoi(t->c_str());
	}
	flag("animatedUVTranslationReverse", s.translationReverse);
	if (const std::string* t = text("animatedUVRotation")) {
		s.rotation = std::atoi(t->c_str()) != 0;
	}
	vec2("animatedUVRotationScale", s.scale);
	if (const std::string* t = text("animatedUVRotationIndex")) {
		s.rotationIndex = std::atoi(t->c_str());
	}
	if (const std::string* t = text("animatedUVRotationRadius")) {
		s.radius = static_cast<float>(std::atof(t->c_str()));
	}
	flag("animatedUVRotationReverse", s.rotationReverse);
	// Spring.rotateUV (0x00543fab, +0x2b8).
	flag("rotateUV", look.rotateUV);
	return m_animatedUVLooks.emplace(&objectTemplate, look).first->second;
}

void LevelRenderer::collectParts(const LevelSources& sources, const world::ObjectTemplate& objectTemplate,
	const Mat4& relative, std::vector<Mat4>& parts, int depth, uint32_t object,
	std::vector<AnimatedUVPart>* animated) const
{
	if (depth > 8) {
		return;
	}
	for (const world::ChildTemplate& child : objectTemplate.children()) {
		const world::ObjectTemplate* childTemplate = sources.templates.find(child.name);
		if (!childTemplate) {
			continue;
		}
		Mat4 local = objectTransform(child.position, child.rotation) * relative;
		world::Vec3 offset, rotation;
		anim::Matrix bone;
		if (object != 0 && sources.partBone && sources.partBone(object, child.name, bone)) {
			// boneIndex (0x0063d470 -> the part's slot 0xe8): the bone's
			// local matrix is the part's.
			Mat4 boned;
			std::copy(bone.begin(), bone.end(), boned.m.begin());
			local = boned * relative;
		} else if (object != 0 && sources.partPose && sources.partPose(object, child.name, offset, rotation)) {
			// A Spring with rotateUV keeps its mesh unturned: 0x006cace0
			// and 0x006b7261 skip the set rotation (0x00614b50); its
			// texture turns instead (AnimatedUV.h).
			if (animatedUVLook(*childTemplate).rotateUV && childTemplate->typeName() == "Spring") {
				rotation = {};
			}
			local = objectTransform(offset, rotation) * local;
		}
		if (animated) {
			const AnimatedUVLook& look = animatedUVLook(*childTemplate);
			if (look.settings.any()) {
				animated->push_back({&look.settings, local});
			}
		}
		const auto part = childTemplate->settings().find("geometryPart");
		if (part != childTemplate->settings().end()) {
			const int index = std::atoi(part->second.c_str());
			if (index > 0 && index < kMaxMeshParts) {
				parts[static_cast<size_t>(index)] = local;
			}
		}
		collectParts(sources, *childTemplate, local, parts, depth + 1, object, animated);
	}
}

void LevelRenderer::buildTerrain(const LevelSources& sources)
{
	const terrain::Heightmap* heightmap = sources.heightmaps.primary();
	if (!heightmap || !heightmap->loaded()) {
		return;
	}
	// The primary heightmap split into patches (8x8 or 4x4), each with its
	// colormap, lightmap and detail maps ("<base>XXxYY.dds"), sharing one
	// index buffer.
	const terrain::TerrainData* data = sources.terrainData;
	const int patchesPerSide = data ? data->patchesPerSide : kDefaultTerrainPatches;
	const std::string colormapBase = data ? data->colormapBaseName : sources.levelPath + "Colormaps/tx";
	const std::string lightmapBase = data ? data->lightmapBaseName : sources.levelPath + "Lightmaps/tx";
	if (data) {
		m_terrainSunColor = data->sunColor;
		m_terrainGIColor = data->giColor;
		m_terrainColorsFromData = true;
		int sides = 0;
		for (size_t i = 0; i < data->layers.size() && i < kMaxDetailLayers; ++i) {
			const terrain::DetailLayer& layer = data->layers[i];
			m_detailLayers[i] = texture(sources.files, layer.texture + ".dds", scene::TextureClass::TerrainDetail);
			m_detailTiling[i] = layer.tiling;
			// The layer's pass (0x1014e570): side-projected over envmapped
			// (whose cube is the frame's, drawTerrain), NEARTEXTILING's
			// side tiling and y offset. A side-projected layer's x and z
			// planes take "<texture>_side" when there is one, else the
			// texture itself (the record's +0xc, 0x100b2bdc).
			TerrainLayerKind kind = TerrainLayerKind::Plain;
			if (layer.sideProjection) {
				kind = TerrainLayerKind::Mountain;
				const std::string side = layer.texture + "_side.dds";
				if (sides < kMaxDetailSides && sources.files.exists(side)) {
					m_detailSides[sides] = texture(sources.files, side, scene::TextureClass::TerrainDetail);
					if (m_detailSides[sides]) {
						kind = sides == 0 ? TerrainLayerKind::MountainSideA : TerrainLayerKind::MountainSideB;
						++sides;
					}
				}
			} else if (layer.envMap) {
				kind = TerrainLayerKind::EnvMap;
			}
			m_detailLayerInfo[i][0] = layer.sideTiling[0];
			m_detailLayerInfo[i][1] = layer.sideTiling[1];
			m_detailLayerInfo[i][2] = layer.yOffset;
			m_detailLayerInfo[i][3] = static_cast<float>(kind);
		}
	}
	const int samples = heightmap->width();
	const int quads = (samples - 1) / patchesPerSide;
	const int side = quads + 1;
	const auto& scale = heightmap->scale();
	m_patchesPerSide = patchesPerSide;
	m_patchOrigin[0] = heightmap->originX();
	m_patchOrigin[1] = heightmap->originZ();
	m_patchSize = quads * scale[0];
	// terraindata.raw's compiled patches when they fit the heightmap: drawn
	// by LOD cells (buildTerrainLods); else the heightmap at full detail.
	const bool lods = buildTerrainLods(sources, quads);
	m_stats.terrainLods = lods;
	if (!lods) {
		std::vector<uint16_t> indices;
		indices.reserve(static_cast<size_t>(quads) * quads * 6);
		for (int y = 0; y < quads; ++y) {
			for (int x = 0; x < quads; ++x) {
				const auto at = [side](int px, int py) { return static_cast<uint16_t>(py * side + px); };
				indices.insert(indices.end(), {at(x, y), at(x, y + 1), at(x + 1, y), at(x + 1, y), at(x, y + 1), at(x + 1, y + 1)});
			}
		}
		m_terrainIndexCount = static_cast<uint32_t>(indices.size());
		m_terrainIndices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(),
			static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
	}
	for (int py = 0; py < patchesPerSide; ++py) {
		for (int px = 0; px < patchesPerSide; ++px) {
			std::vector<Vertex> vertices(static_cast<size_t>(side) * side);
			const terrain::TerrainPatchMesh* compiled =
				lods ? &data->meshes->patches[static_cast<size_t>(py * patchesPerSide + px)] : nullptr;
			for (int y = 0; y < side; ++y) {
				for (int x = 0; x < side; ++x) {
					const int sx = px * quads + x;
					const int sy = py * quads + y;
					Vertex& v = vertices[static_cast<size_t>(y) * side + x];
					v.position[0] = heightmap->originX() + sx * scale[0];
					v.position[2] = heightmap->originZ() + sy * scale[2];
					if (compiled) {
						// The compiled vertex (0x1014be90): its height and the
						// three geomorph deltas in heightmap units, the normal
						// a D3DCOLOR (B, G, R, A; x = R, y = G, z = B), the
						// LOD step it drops out at on a cell's edge.
						const terrain::TerrainVertex& c = compiled->vertices[static_cast<size_t>(y) * side + x];
						v.position[1] = c.height * scale[1];
						for (int i = 0; i < 3; ++i) {
							v.normal[i] = (c.normal[2 - i] - 127.5f) / 127.5f;
						}
						v.lightmapCoord[0] = c.morph[0] * scale[1];
						v.lightmapCoord[1] = c.morph[1] * scale[1];
						v.part = c.morph[2] * scale[1];
						v.part2 = static_cast<float>(c.edge);
					} else {
						v.position[1] = heightmap->sample(sx, sy);
						const float dx = heightmap->sample(sx + 1, sy) - heightmap->sample(sx - 1, sy);
						const float dz = heightmap->sample(sx, sy + 1) - heightmap->sample(sx, sy - 1);
						const Vec3f n = normalize({-dx, 2 * scale[0], -dz});
						v.normal[0] = n.x;
						v.normal[1] = n.y;
						v.normal[2] = n.z;
						v.part2 = -1; // no LOD step: never morphed
					}
					// Colormap and lightmap texels run along +x and +z.
					v.texCoord[0] = static_cast<float>(x) / quads;
					v.texCoord[1] = static_cast<float>(y) / quads;
				}
			}
			// "<base><x patch>x<z patch>", counted from the -x/-z corner
			// (checked against the building footprints painted into the
			// colormaps); detail maps add "_1", "_2" (0x1014b6a0).
			char name[32];
			std::snprintf(name, sizeof(name), "%02dx%02d", px, py);
			TerrainPatch patch;
			patch.boundsMin = patch.boundsMax = {vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]};
			for (const Vertex& v : vertices) {
				patch.boundsMin = {std::fmin(patch.boundsMin.x, v.position[0]), std::fmin(patch.boundsMin.y, v.position[1]),
					std::fmin(patch.boundsMin.z, v.position[2])};
				patch.boundsMax = {std::fmax(patch.boundsMax.x, v.position[0]), std::fmax(patch.boundsMax.y, v.position[1]),
					std::fmax(patch.boundsMax.z, v.position[2])};
			}
			patch.vertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
				static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
			patch.colormap = texture(sources.files, colormapBase + name + ".dds", scene::TextureClass::TerrainColormap);
			patch.lightmap = texture(sources.files, lightmapBase + name + ".dds", scene::TextureClass::TerrainLightmap);
			if (data) {
				for (int map = 0; map < 2; ++map) {
					const std::string path = data->detailmapBaseName + name + "_" + std::to_string(map + 1) + ".dds";
					if (sources.files.exists(path)) {
						patch.detailWeights[map] = texture(sources.files, path, scene::TextureClass::TerrainDetailMaps);
					}
				}
			}
			m_terrain.push_back(patch);
		}
	}
	m_stats.terrainPatches = m_terrain.size();
	m_terrainExtent = heightmap->worldWidth();
	// TerrainWaterColor and WaterHeight (0x100af7e0: terraindata.raw's water
	// colour, the cluster's water level): the distant cells' low-detail pass
	// (Shared_PS_LowDetail) tints the ground under the sea.
	m_terrainWater = {0, 0, 0, -1e9f};
	if (data && sources.heightmaps.hasSea()) {
		m_terrainWater = {data->waterColor[0], data->waterColor[1], data->waterColor[2],
			sources.heightmaps.seaWaterLevel()};
	}
	// The low-detail texture (0x100b0850): <level>lowdetailtexturehi.dds,
	// else <level>/lowDetailTexture.dds (the editor's name, 0x100ccab0),
	// else common/terrain/textures/Default.dds; tiled by terraindata.raw's
	// far tilings.
	for (const char* name : {"lowdetailtexturehi.dds", "lowDetailTexture.dds"}) {
		if (!m_lowDetailTexture && sources.files.exists(sources.levelPath + name)) {
			m_lowDetailTexture = texture(sources.files, sources.levelPath + name, scene::TextureClass::TerrainLowDetail);
		}
	}
	if (!m_lowDetailTexture && sources.files.exists("common/terrain/textures/Default.dds")) {
		m_lowDetailTexture = texture(sources.files, "common/terrain/textures/Default.dds", scene::TextureClass::TerrainLowDetail);
	}
	if (data) {
		m_farTiling[0] = data->farSideTiling[0];
		m_farTiling[1] = data->farSideTiling[1];
		m_farTiling[2] = data->farTopTilingHigh;
		m_farTiling[3] = data->farYOffset;
		// The surrounding terrain's (STFARTEXTILING, STTEXSCALE: 0x100af7e0):
		// world metres over patch size x the secondary x scale, the tilings
		// across times patches per side over a quarter of that scale.
		const float secondary = data->secondaryWorldScale[0] > 0 ? data->secondaryWorldScale[0] : 1.0f;
		const float across = static_cast<float>(data->patchesPerSide) / (secondary * 0.25f);
		m_surroundingFarTiling[0] = data->farSideTiling[0] * across;
		m_surroundingFarTiling[1] = data->farSideTiling[1];
		m_surroundingFarTiling[2] = data->farTopTilingHigh * across;
		m_surroundingFarTiling[3] = data->farYOffset;
		m_surroundingTexScale = data->patchSize > 0 ? 1.0f / (static_cast<float>(data->patchSize) * secondary) : 0.0f;
	}
}

// terraindata.raw's LOD meshes (0x100b00a0, 0x100aed30): the index ranges of
// the shared tree's nodes and the cells the terrain culler draws, each a node
// of a patch's tree as big as terrainCuller.baseCellSize (64 m by default,
// 0x1009c820; a patch's quarter at 2 m a quad). False when the file has no
// meshes or they don't fit the heightmap (then the heightmap is drawn whole).
bool LevelRenderer::buildTerrainLods(const LevelSources& sources, int quads)
{
	const terrain::TerrainData* data = sources.terrainData;
	const terrain::Heightmap* heightmap = sources.heightmaps.primary();
	if (!data || !data->meshes || !heightmap || data->patchSize != quads || data->patchesPerSide <= 0) {
		return false;
	}
	const terrain::TerrainMeshes& meshes = *data->meshes;
	const size_t side = static_cast<size_t>(quads) + 1;
	if (meshes.nodes.size() != 85 || meshes.indices.empty() ||
		meshes.patches.size() != static_cast<size_t>(data->patchesPerSide * data->patchesPerSide)) {
		return false;
	}
	for (const terrain::TerrainPatchMesh& patch : meshes.patches) {
		if (patch.vertices.size() != side * side) {
			return false;
		}
	}
	for (uint16_t index : meshes.indices) {
		if (index >= side * side) {
			return false;
		}
	}
	// The cell: baseCellSize metres if that is a node of the tree (128, 64
	// or 32 quads: levels 0-2, the ones with stitched variants), else the
	// nearest of them.
	const scene::RendererSettings defaults;
	const scene::RendererSettings& settings = sources.renderer ? *sources.renderer : defaults;
	const float metresPerQuad = heightmap->scale()[0];
	int cellQuads = static_cast<int>(std::lround(settings.baseCellSize / std::fmax(metresPerQuad, 1e-3f)));
	cellQuads = std::clamp(cellQuads, quads / 4, quads);
	int level = 0;
	while ((quads >> level) > cellQuads && level < 2) {
		++level;
	}
	cellQuads = quads >> level;
	if (cellQuads <= 0) {
		return false;
	}
	m_terrainNodeRanges.assign(meshes.nodes.size(), {});
	for (size_t n = 0; n < meshes.nodes.size(); ++n) {
		for (int lod = 0; lod < 3; ++lod) {
			for (int mask = 0; mask < 11; ++mask) {
				const terrain::IndexRange& r = meshes.nodes[n][static_cast<size_t>(lod)][static_cast<size_t>(mask)];
				if (r.start >= 0 && r.count > 0 && static_cast<size_t>(r.start) + r.count <= meshes.indices.size()) {
					m_terrainNodeRanges[n][static_cast<size_t>(lod * 11 + mask)] = {
						static_cast<uint32_t>(r.start), static_cast<uint32_t>(r.count)};
				}
			}
		}
	}
	// The nodes of that level by the corner their full-detail triangles
	// start at (the tree is ordered as a quadtree: 0; 1-4; 5-20; 21-84).
	const int perPatch = quads / cellQuads;
	std::vector<int> nodeAt(static_cast<size_t>(perPatch * perPatch), -1);
	const size_t first = level == 0 ? 0 : level == 1 ? 1 : 5;
	const size_t last = level == 0 ? 1 : level == 1 ? 5 : 21;
	for (size_t n = first; n < last; ++n) {
		const TerrainNodeRange& r = m_terrainNodeRanges[n][0];
		int x0 = quads, z0 = quads;
		for (uint32_t i = r.start; i < r.start + r.count; ++i) {
			x0 = std::min(x0, static_cast<int>(meshes.indices[i] % side));
			z0 = std::min(z0, static_cast<int>(meshes.indices[i] / side));
		}
		if (r.count > 0 && x0 % cellQuads == 0 && z0 % cellQuads == 0) {
			nodeAt[static_cast<size_t>((z0 / cellQuads) * perPatch + x0 / cellQuads)] = static_cast<int>(n);
		}
	}
	if (std::find(nodeAt.begin(), nodeAt.end(), -1) != nodeAt.end()) {
		return false;
	}
	const int patches = data->patchesPerSide;
	m_terrainCellsPerPatch = perPatch;
	m_terrainCellsPerSide = patches * perPatch;
	m_terrainCells.assign(static_cast<size_t>(m_terrainCellsPerSide) * m_terrainCellsPerSide, {});
	m_terrainCellNodes.assign(m_terrainCells.size(), 0);
	const float yScale = heightmap->scale()[1];
	for (int cz = 0; cz < m_terrainCellsPerSide; ++cz) {
		for (int cx = 0; cx < m_terrainCellsPerSide; ++cx) {
			const size_t c = static_cast<size_t>(cz) * m_terrainCellsPerSide + cx;
			TerrainLodCell& cell = m_terrainCells[c];
			const int px = cx / perPatch, pz = cz / perPatch;
			m_terrainCellNodes[c] = nodeAt[static_cast<size_t>((cz % perPatch) * perPatch + cx % perPatch)];
			const terrain::TerrainPatchMesh& patch = meshes.patches[static_cast<size_t>(pz * patches + px)];
			float low = 1e30f, high = -1e30f;
			const int x0 = (cx % perPatch) * cellQuads, z0 = (cz % perPatch) * cellQuads;
			for (int z = z0; z <= z0 + cellQuads; ++z) {
				for (int x = x0; x <= x0 + cellQuads; ++x) {
					const terrain::TerrainVertex& v = patch.vertices[static_cast<size_t>(z) * side + x];
					// Morphed anywhere between its own height and its LODs'.
					for (float h : {v.height, v.height - v.morph[0], v.height - v.morph[1], v.height - v.morph[2]}) {
						low = std::fmin(low, h * yScale);
						high = std::fmax(high, h * yScale);
					}
				}
			}
			const float x = heightmap->originX() + static_cast<float>(cx * cellQuads) * metresPerQuad;
			const float z = heightmap->originZ() + static_cast<float>(cz * cellQuads) * heightmap->scale()[2];
			cell.boundsMin = {x, low, z};
			cell.boundsMax = {x + cellQuads * metresPerQuad, high, z + cellQuads * heightmap->scale()[2]};
		}
	}
	m_terrainLodIndices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_INDEX, meshes.indices.data(),
		static_cast<uint32_t>(meshes.indices.size() * sizeof(uint16_t)));
	return m_terrainLodIndices != nullptr;
}

float LevelRenderer::viewZoom(const Camera& camera) const
{
	// RenderView +0x1c (0x10066b30): tan(first fov / 2) / tan(fov / 2).
	const float base = m_baseFovY > 0 ? m_baseFovY : camera.fovY;
	const float t = std::tan(camera.fovY * 0.5f);
	return t > 1e-6f ? std::tan(base * 0.5f) / t : 1.0f;
}

// The terrain culler's frame (TerrainLod.h): visibility, LODs, stitches.
void LevelRenderer::updateTerrainCells(const Camera& camera, const Frustum& frustum)
{
	const scene::RendererSettings defaults;
	scene::RendererSettings settings = m_sources && m_sources->renderer ? *m_sources->renderer : defaults;
	// VideoSettings' terrain quality: lodSwitchDistance 64 / 80 / 100 (0x10010040), unless a script set it.
	if (m_sources && m_sources->quality && !settings.lodSwitchDistanceSet) {
		settings.lodSwitchDistance = std::max(m_sources->quality->terrainLodSwitchDistance(), settings.baseCellSize);
	}
	m_terrainLodSwitch = terrainLodSwitch(viewZoom(camera), settings);
	const terrain::Heightmap* heightmap = m_sources ? m_sources->heightmaps.primary() : nullptr;
	const float ground = heightmap ? heightmap->heightAt(camera.position.x, camera.position.z) : 0.0f;
	m_terrainHeightAboveGround = std::fabs(camera.position.y - ground);
	for (TerrainLodCell& cell : m_terrainCells) {
		cell.visible = frustum.containsBox(cell.boundsMin, cell.boundsMax);
	}
	pickTerrainLods(m_terrainCells, m_terrainCellsPerSide, camera.position.x, camera.position.z,
		m_terrainHeightAboveGround, m_terrainLodSwitch, settings.lod2Start);
}

// The surrounding terrain: each secondary heightmap of the cluster drawn
// with its colormap and low-detail component map, "<base>_s<i>.dds" (the
// format RendDX9 builds the names with, "%s%s_s%i" at 0x10154110), i
// counting the eight around the primary row by row from the -x/-z corner
// (the order Heightdata.con declares them in).
void LevelRenderer::buildSurroundingTerrain(const LevelSources& sources)
{
	const terrain::TerrainData* data = sources.terrainData;
	if (!data) {
		return;
	}
	// Every other sample: the secondary maps are 257 samples 4 m apart;
	// 129 x 129 vertices fit 16-bit indices and are plenty at that range.
	constexpr int kStride = 2;
	int side = 0;
	for (const auto& heightmap : sources.heightmaps.heightmaps()) {
		const int x = heightmap->clusterX(), y = heightmap->clusterY();
		if ((x == 0 && y == 0) || x < -1 || x > 1 || y < -1 || y > 1) {
			continue;
		}
		if (!heightmap->loaded() || heightmap->width() < 3 || heightmap->width() != heightmap->height()) {
			continue;
		}
		int index = (y + 1) * 3 + (x + 1);
		if (index > 4) {
			--index; // the primary's place is skipped
		}
		const int n = (heightmap->width() - 1) / kStride + 1;
		if (side != 0 && n != side) {
			continue; // one index buffer for all of them
		}
		const auto& scale = heightmap->scale();
		std::vector<Vertex> vertices(static_cast<size_t>(n) * n);
		SurroundingPatch patch;
		for (int row = 0; row < n; ++row) {
			for (int column = 0; column < n; ++column) {
				const int sx = column * kStride, sy = row * kStride;
				Vertex& v = vertices[static_cast<size_t>(row) * n + column];
				v.position[0] = heightmap->originX() + sx * scale[0];
				v.position[1] = heightmap->sample(sx, sy);
				v.position[2] = heightmap->originZ() + sy * scale[2];
				const float dx = heightmap->sample(sx + 1, sy) - heightmap->sample(sx - 1, sy);
				const float dz = heightmap->sample(sx, sy + 1) - heightmap->sample(sx, sy - 1);
				const Vec3f normal = normalize({-dx, 2 * scale[0], -dz});
				v.normal[0] = normal.x;
				v.normal[1] = normal.y;
				v.normal[2] = normal.z;
				v.texCoord[0] = static_cast<float>(column) / (n - 1);
				v.texCoord[1] = static_cast<float>(row) / (n - 1);
				const Vec3f p {v.position[0], v.position[1], v.position[2]};
				if (row == 0 && column == 0) {
					patch.boundsMin = patch.boundsMax = p;
				}
				patch.boundsMin = {std::fmin(patch.boundsMin.x, p.x), std::fmin(patch.boundsMin.y, p.y), std::fmin(patch.boundsMin.z, p.z)};
				patch.boundsMax = {std::fmax(patch.boundsMax.x, p.x), std::fmax(patch.boundsMax.y, p.y), std::fmax(patch.boundsMax.z, p.z)};
			}
		}
		const std::string suffix = "_s" + std::to_string(index) + ".dds";
		if (!sources.files.exists(data->colormapBaseName + suffix)) {
			continue;
		}
		patch.colormap = texture(sources.files, data->colormapBaseName + suffix);
		if (sources.files.exists(data->lowDetailmapBaseName + suffix)) {
			patch.lowComponent = texture(sources.files, data->lowDetailmapBaseName + suffix);
		}
		if (!patch.colormap) {
			continue;
		}
		side = n;
		patch.vertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
			static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
		m_surrounding.push_back(patch);
	}
	if (m_surrounding.empty()) {
		return;
	}
	std::vector<uint16_t> indices;
	for (int row = 0; row + 1 < side; ++row) {
		for (int column = 0; column + 1 < side; ++column) {
			const auto at = [side](int px, int py) { return static_cast<uint16_t>(py * side + px); };
			indices.insert(indices.end(), {at(column, row), at(column, row + 1), at(column + 1, row), at(column + 1, row),
				at(column, row + 1), at(column + 1, row + 1)});
		}
	}
	m_surroundingIndexCount = static_cast<uint32_t>(indices.size());
	m_surroundingIndices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(),
		static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
	m_stats.surroundingPatches = m_surrounding.size();
}

void LevelRenderer::buildSky(const LevelSources& sources)
{
	const world::ObjectTemplate* skyTemplate = sources.templates.find(sources.sky.skyTemplate);
	if (!skyTemplate || skyTemplate->geometry().empty()) {
		return;
	}
	m_skyMesh = mesh(sources, skyTemplate->geometry());
	if (!sources.sky.skyTexture.empty()) {
		m_skyTexture = texture(sources.files, sources.sky.skyTexture + ".dds");
	}
	m_skyRotation = sources.sky.domeRotation;
}

void LevelRenderer::loadLightmapAtlas(const LevelSources& sources)
{
	// Lines: "<texture path>\t\t<atlas path>, <index>, <u>, <v>, <width>, <height>".
	std::string text;
	if (!sources.files.readText(sources.levelPath + "Lightmaps/Objects/LightmapAtlas.tai", text)) {
		return;
	}
	std::istringstream lines(text);
	std::string line;
	while (std::getline(lines, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::istringstream fields(line);
		std::string texturePath, atlasPath;
		fields >> texturePath >> atlasPath;
		if (atlasPath.empty() || atlasPath.back() != ',') {
			continue;
		}
		atlasPath.pop_back();
		AtlasEntry entry {atlasPath, {0, 0}, {0, 0}};
		int index = 0;
		char comma = 0;
		fields >> index >> comma >> entry.offset[0] >> comma >> entry.offset[1] >> comma >> entry.size[0] >> comma >>
			entry.size[1];
		if (!fields) {
			continue;
		}
		const size_t slash = texturePath.find_last_of("/\\");
		std::string key = lower(texturePath.substr(slash == std::string::npos ? 0 : slash + 1));
		if (key.size() > 4 && key.compare(key.size() - 4, 4, ".dds") == 0) {
			key.resize(key.size() - 4);
		}
		m_lightmapAtlas[key] = entry;
	}
}

// The overgrowth: trees placed by OvergrowthCollision.con (Object.
// isOvergrowth) are drawn whole within Overgrowth.viewDistance *
// viewDistanceScale * closeDistPercentage, and with their
// "<geometry>_lod" mesh beyond, whose textures the level's overgrowth
// atlases hold (OvergrowthAtlas.tai: "<texture> <atlas>, <index>, <u>, <v>,
// <width>, <height>", as the lightmaps' atlas).
void LevelRenderer::loadOvergrowth(const LevelSources& sources)
{
	const std::string directory = sources.levelPath + "overgrowth/";
	const auto readAtlas = [&](const std::string& path, std::map<std::string, AtlasEntry>& into) {
		std::string text;
		if (!sources.files.readText(path, text)) {
			return;
		}
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line)) {
			if (line.empty() || line[0] == '#') {
				continue;
			}
			std::istringstream fields(line);
			std::string texturePath, atlasPath;
			fields >> texturePath >> atlasPath;
			if (atlasPath.empty() || atlasPath.back() != ',') {
				continue;
			}
			atlasPath.pop_back();
			std::replace(atlasPath.begin(), atlasPath.end(), '\\', '/');
			AtlasEntry entry {atlasPath, {0, 0}, {0, 0}};
			int index = 0;
			char comma = 0;
			fields >> index >> comma >> entry.offset[0] >> comma >> entry.offset[1] >> comma >> entry.size[0] >> comma >>
				entry.size[1];
			if (!fields) {
				continue;
			}
			std::string key = lower(texturePath);
			std::replace(key.begin(), key.end(), '\\', '/');
			into[key] = entry;
		}
	};
	readAtlas(directory + "OvergrowthAtlas.tai", m_overgrowthAtlas);
	readAtlas(directory + "OvergrowthAtlas_alphatest.tai", m_overgrowthAlphaTestAtlas);
	if (m_overgrowthAtlas.empty()) {
		return;
	}
	// The console's Overgrowth.* rows as the level's Overgrowth.con set
	// them (scene::OvergrowthSettings); without a console (--view of a
	// level whose scripts didn't run) the file's view distances are read.
	float viewDistance = 0, scale = 1, close = 0;
	m_overgrowthCloseDistFade = 0.3f;
	m_overgrowthLodTimer = 0;
	if (sources.overgrowth && sources.overgrowth->configured) {
		viewDistance = sources.overgrowth->viewDistance;
		scale = sources.overgrowth->viewDistanceScale;
		close = sources.overgrowth->closeDistPercentage;
		m_overgrowthCloseDistFade = sources.overgrowth->closeDistFade;
		m_overgrowthLodTimer = sources.overgrowth->lodTimer;
	} else {
		std::string text;
		if (!sources.files.readText(directory + "Overgrowth.con", text)) {
			return;
		}
		std::istringstream lines(text);
		std::string command;
		while (lines >> command) {
			std::string value;
			std::getline(lines, value);
			const std::string name = lower(command);
			if (name == "overgrowth.viewdistance") {
				viewDistance = static_cast<float>(std::atof(value.c_str()));
			} else if (name == "overgrowth.viewdistancescale") {
				scale = static_cast<float>(std::atof(value.c_str()));
			} else if (name == "overgrowth.closedistpercentage") {
				close = static_cast<float>(std::atof(value.c_str()));
			} else if (name == "overgrowth.closedistfade") {
				m_overgrowthCloseDistFade = static_cast<float>(std::atof(value.c_str()));
			} else if (name == "overgrowth.lodtimer") {
				m_overgrowthLodTimer = static_cast<float>(std::atof(value.c_str()));
			}
		}
	}
	m_overgrowthViewDistance = viewDistance > 0 ? viewDistance : 500.0f;
	// The cells' size (0x10120420), then a whole number of them across the
	// terrain's smaller side (0x1008f510: W / floor(W / size)).
	const terrain::Heightmap* primary = sources.heightmaps.primary();
	const float side = primary && primary->loaded() ? std::min(primary->worldWidth(), primary->worldDepth()) : 0.0f;
	m_overgrowthCellSize = OvergrowthCells::cellSize(m_overgrowthViewDistance, side);
	m_overgrowthCloseDistance = viewDistance * scale * close;
	m_overgrowthCloseBase = viewDistance * close;
	m_overgrowthScriptScale = scale;
	m_overgrowthQualityScale = sources.quality ? sources.quality->overgrowthViewDistanceScale() : 1.0f;
}

float LevelRenderer::overgrowthCloseDistance() const
{
	// 0x1008b7a0 / 0x1008b550: C = zoom x viewDistance (+0x24) x
	// closeDistPercentage (+0xc8) x the geometry quality's factor (+0x110,
	// 0x10010040: 0.4 / 0.7 / 1); a tree is near within its mesh radius plus
	// C x Overgrowth.viewDistanceScale (+0x28, 0x1008763d) (0x1008b580; the
	// type's +0x9c factor taken as 1). The zoom is applied by the caller.
	const float quality = m_sources && m_sources->quality ? m_sources->quality->overgrowthViewDistanceScale() : 1.0f;
	return m_overgrowthCloseBase * m_overgrowthScriptScale * quality;
}

// The sea: a grid at sea level over each heightmap of the cluster (the
// primary and the surrounding ones), each vertex carrying the water's depth
// there (sea level minus the terrain's height) for its opacity, and a ring
// of deep water beyond them to the horizon (RendDX9's WaterSurrounding and
// WaterDistant shader variants draw those parts).
//
// The original draws it per frame in terrain patches (DrawWater, 0x100b53c0):
// each patch of the heightmap's grid that passes the view tests is a
// quad at sea level (0x100b1f10), skipped where the patch's height (+0x98) is
// above the water; outside the heightmap a patch takes WaterSurrounding,
// beyond 40 m (+0x674) of the camera or with the camera over 40 m above the
// sea WaterDistant, and near it the full Water technique only where one of
// the light manager's +0x6f8 boxes meets the patch (slot 0xf8, 0x101a9b90;
// else WaterDistant again). Those differ by USE_SHADOWS (the shade
// the rewrite doesn't draw) against USE_HEIGHTALPHA, so the rewrite's grids
// take one technique; their opacity from the depth stands in for the
// lightmap's.
void LevelRenderer::buildWater(const LevelSources& sources)
{
	const terrain::Heightmap* primary = sources.heightmaps.primary();
	if (!primary || !primary->loaded() || !sources.heightmaps.hasSea()) {
		return;
	}
	m_water = &sources.water;
	const float level = sources.heightmaps.seaWaterLevel();
	const auto groundAt = [&sources](float x, float z) {
		for (const auto& heightmap : sources.heightmaps.heightmaps()) {
			if (heightmap->loaded() && heightmap->covers(x, z)) {
				return heightmap->heightAt(x, z);
			}
		}
		return -1e4f;
	};
	const auto addGrid = [&](float x0, float z0, float x1, float z1, int cells) {
		std::vector<Vertex> vertices;
		vertices.reserve(static_cast<size_t>(cells + 1) * (cells + 1));
		bool anyWater = false;
		for (int y = 0; y <= cells; ++y) {
			for (int x = 0; x <= cells; ++x) {
				Vertex v {};
				v.position[0] = x0 + (x1 - x0) * x / cells;
				v.position[1] = level;
				v.position[2] = z0 + (z1 - z0) * y / cells;
				v.normal[1] = 1;
				v.texCoord[0] = std::min(level - groundAt(v.position[0], v.position[2]), 1000.0f);
				anyWater = anyWater || v.texCoord[0] > -2.0f;
				vertices.push_back(v);
			}
		}
		if (!anyWater) {
			return; // all of it under ground
		}
		std::vector<uint16_t> indices;
		for (int y = 0; y < cells; ++y) {
			for (int x = 0; x < cells; ++x) {
				const auto at = [cells](int px, int py) { return static_cast<uint16_t>(py * (cells + 1) + px); };
				indices.insert(indices.end(), {at(x, y), at(x, y + 1), at(x + 1, y), at(x + 1, y), at(x, y + 1), at(x + 1, y + 1)});
			}
		}
		WaterBlock block;
		block.vertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
			static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
		block.indices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(),
			static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
		block.indexCount = static_cast<uint32_t>(indices.size());
		block.boundsMin = {std::min(x0, x1), level - 1, std::min(z0, z1)};
		block.boundsMax = {std::max(x0, x1), level + 1, std::max(z0, z1)};
		m_waterBlocks.push_back(block);
	};
	// The cluster's extent: the heightmaps it has.
	float lo[2] = {primary->originX(), primary->originZ()};
	float hi[2] = {lo[0] + primary->worldWidth(), lo[1] + primary->worldDepth()};
	for (const auto& heightmap : sources.heightmaps.heightmaps()) {
		if (!heightmap->loaded()) {
			continue;
		}
		const bool isPrimary = heightmap.get() == primary;
		const float x0 = heightmap->originX(), z0 = heightmap->originZ();
		addGrid(x0, z0, x0 + heightmap->worldWidth(), z0 + heightmap->worldDepth(), isPrimary ? 128 : 64);
		lo[0] = std::min(lo[0], x0);
		lo[1] = std::min(lo[1], z0);
		hi[0] = std::max(hi[0], x0 + heightmap->worldWidth());
		hi[1] = std::max(hi[1], z0 + heightmap->worldDepth());
	}
	// Beyond: four strips to the horizon (the far plane is 6 km).
	constexpr float kHorizon = 8000.0f;
	addGrid(-kHorizon, -kHorizon, kHorizon, lo[1], 16);
	addGrid(-kHorizon, hi[1], kHorizon, kHorizon, 16);
	addGrid(-kHorizon, lo[1], lo[0], hi[1], 16);
	addGrid(hi[0], lo[1], kHorizon, hi[1], 16);
	// The normal map frames (WaterMapFrame0/1 of the 2D variants) and the
	// level's cube map ("/Water/envmap.dds").
	for (int frame = 1; frame <= 8; ++frame) {
		char path[64];
		std::snprintf(path, sizeof(path), "common/textures/waterbump/Waterbump_%02d.dds", frame);
		if (!sources.files.exists(path)) {
			break;
		}
		m_waterFrames.push_back(texture(sources.files, path));
	}
	m_waterEnvMap = cubeTexture(sources.files, sources.levelPath + "Water/envmap.dds");
	// The 3D variants' volume (the terrain's water map name, "watervolume"
	// at 0x100b4690: Textures/watervolume.dds, 256 x 256 x 8).
	if (sources.files.exists("common/textures/watervolume.dds")) {
		m_waterVolume = texture(sources.files, "common/textures/watervolume.dds");
	}
}

// Roads placed by CompiledRoads.con: objects of a RoadCompiled template
// whose mesh was set with Object.geometry.loadMesh.
void LevelRenderer::buildRoads(const LevelSources& sources)
{
	for (const auto& object : sources.objects.objects()) {
		const world::ObjectTemplate* objectTemplate = object->objectTemplate();
		if (!objectTemplate || object->mesh().empty()) {
			continue;
		}
		const geom::GeometryTemplate* geometry = sources.geometries.find(objectTemplate->geometry());
		if (!geometry || geometry->kind != geom::MeshKind::Road) {
			continue;
		}
		RoadLook& look = m_roadLooks[lower(geometry->name)];
		if (!look.loaded) {
			look.loaded = true;
			std::vector<uint8_t> data;
			if (sources.files.readFile(geometry->path, data)) {
				if (const auto settings = geom::parseRoadSettings(data)) {
					look.texture0 = settings->texture0.empty() ? nullptr : texture(sources.files, settings->texture0 + ".dds");
					look.texture1 = settings->texture1.empty() ? nullptr : texture(sources.files, settings->texture1 + ".dds");
					look.uniforms = {{settings->blendFactor, 0, 0, 0}, {}};
					look.priority = settings->priority;
				}
			}
		}
		std::vector<uint8_t> data;
		std::optional<geom::CompiledRoad> file;
		if (look.texture0 && sources.files.readFile(object->mesh(), data)) {
			file = geom::parseCompiledRoad(data);
		}
		if (!file || file->indices.empty()) {
			++m_stats.missingRoads;
			continue;
		}
		std::vector<Vertex> vertices(file->vertices.size());
		for (size_t i = 0; i < vertices.size(); ++i) {
			const geom::RoadVertex& source = file->vertices[i];
			Vertex& v = vertices[i];
			std::copy(source.position.begin(), source.position.end(), v.position);
			v.normal[1] = 1;
			std::copy(source.texCoord0.begin(), source.texCoord0.end(), v.texCoord);
			std::copy(source.texCoord1.begin(), source.texCoord1.end(), v.lightmapCoord);
			v.weight = source.alpha;
		}
		Road road;
		road.vertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
			static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
		road.indices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_INDEX, file->indices.data(),
			static_cast<uint32_t>(file->indices.size() * sizeof(uint16_t)));
		road.indexCount = static_cast<uint32_t>(file->indices.size());
		road.world = objectTransform(object->position(), object->rotation());
		road.texture0 = look.texture0;
		road.texture1 = look.texture1 ? look.texture1 : look.texture0;
		road.uniforms = look.uniforms;
		road.priority = look.priority;
		road.boundsMin[0] = file->boundsMin[0];
		road.boundsMin[1] = file->boundsMin[2];
		road.boundsMax[0] = file->boundsMax[0];
		road.boundsMax[1] = file->boundsMax[2];
		// The header's sphere is already world space, as the bounds are.
		road.center = {file->position[0], file->position[1], file->position[2]};
		road.radius = file->radius;
		m_roads.push_back(road);
	}
	std::stable_sort(m_roads.begin(), m_roads.end(), [](const Road& a, const Road& b) { return a.priority < b.priority; });
	m_stats.roads = m_roads.size();
}

void LevelRenderer::buildUndergrowth(const LevelSources& sources)
{
	const terrain::Heightmap* heightmap = sources.heightmaps.primary();
	const terrain::TerrainData* data = sources.terrainData;
	std::string configText;
	std::vector<uint8_t> datFile, materialMap;
	if (!heightmap || !heightmap->loaded() || !data || data->patchesPerSide <= 0 ||
		!sources.files.readText(sources.levelPath + "Undergrowth.cfg", configText) ||
		!sources.files.readFile(sources.levelPath + "Undergrowth.dat", datFile) ||
		!sources.files.readFile(sources.levelPath + "Undergrowth.raw", materialMap)) {
		return;
	}
	auto materials = terrain::parseUndergrowthData(datFile);
	if (!materials) {
		return;
	}
	const terrain::UndergrowthConfig config = terrain::parseUndergrowthConfig(configText);
	const auto& scale = heightmap->scale();
	terrain::UndergrowthGenerator::Terrain ground;
	ground.mapWidth = heightmap->width();
	ground.mapHeight = heightmap->height();
	if (materialMap.size() != static_cast<size_t>(ground.mapWidth) * ground.mapHeight) {
		return;
	}
	ground.materialMap = std::move(materialMap);
	ground.sampleSpacing = {scale[0], scale[2]};
	// Terrain patches subdivided PatchSubdivide times (0x101100d0).
	ground.patchesPerSide = data->patchesPerSide * config.patchSubdivide;
	ground.patchSize = data->patchSize * scale[0] / config.patchSubdivide;
	ground.height = [heightmap](float x, float z) { return heightmap->heightAt(x, z); };
	ground.normal = [heightmap, step = scale[0]](float x, float z) {
		const float dx = heightmap->heightAt(x + step, z) - heightmap->heightAt(x - step, z);
		const float dz = heightmap->heightAt(x, z + step) - heightmap->heightAt(x, z - step);
		const Vec3f n = normalize({-dx, 2 * step, -dz});
		return std::array<float, 3> {n.x, n.y, n.z};
	};
	for (const auto& material : *materials) {
		m_stats.undergrowthTypes += material.types.size();
	}
	m_undergrowth = std::make_unique<terrain::UndergrowthGenerator>(std::move(*materials), std::move(ground));
	m_undergrowthViewDistance = config.viewDistance;
	m_undergrowthFadeScale = config.viewDistanceFadeScale;
	m_undergrowthSwayScale = config.swayScale;
	m_undergrowthSway.seed(); // 0x1010ad20, before the cfg is read
	m_undergrowthSwayTicks = 0;
	m_undergrowthAlphaRef = config.alphaRef;
	// The "_alphatest" atlas where there is one, but antialiased (0x101100d0
	// skips that test with 0x1000b500 and +0x50a): the plain one, its
	// alpha for coverage.
	const std::string atlas = sources.levelPath + "UndergrowthAtlas_alphatest0.dds";
	const bool plain = m_coverageTextures || !sources.files.exists(atlas);
	m_undergrowthAtlas = texture(sources.files,
		plain ? sources.levelPath + "UndergrowthAtlas0.dds" : atlas, scene::TextureClass::Undergrowth);
}

void LevelRenderer::updateUndergrowth(const Camera& camera)
{
	if (!m_undergrowth) {
		return;
	}
	const int n = m_undergrowth->patchesPerSide();
	const float size = m_undergrowth->patchSize();
	const float reach = undergrowthViewDistance() + size;
	const auto patchAt = [n, size](float world) {
		return static_cast<int>(std::floor(world / size)) + n / 2;
	};
	const int x0 = std::max(patchAt(camera.position.x - reach), 0);
	const int x1 = std::min(patchAt(camera.position.x + reach), n - 1);
	const int z0 = std::max(patchAt(camera.position.z - reach), 0);
	const int z1 = std::min(patchAt(camera.position.z + reach), n - 1);
	// Drop patches well out of view.
	SDL_GPUDevice* device = m_gpu.device();
	for (auto it = m_undergrowthPatches.begin(); it != m_undergrowthPatches.end();) {
		const int px = it->first % n, pz = it->first / n;
		if (px < x0 - 2 || px > x1 + 2 || pz < z0 - 2 || pz > z1 + 2) {
			if (it->second.vertices) {
				SDL_ReleaseGPUBuffer(device, it->second.vertices);
				SDL_ReleaseGPUBuffer(device, it->second.indices);
			}
			it = m_undergrowthPatches.erase(it);
		} else {
			++it;
		}
	}
	bool uploaded = false;
	for (int pz = z0; pz <= z1; ++pz) {
		for (int px = x0; px <= x1; ++px) {
			const int index = pz * n + px;
			if (m_undergrowthPatches.count(index)) {
				continue;
			}
			UndergrowthPatchBuffers& buffers = m_undergrowthPatches[index];
			const terrain::UndergrowthPatch patch = m_undergrowth->generate(index);

			if (patch.indices.empty()) {
				continue;
			}
			std::vector<Vertex> vertices(patch.vertices.size());
			float lowY = std::numeric_limits<float>::max(), highY = -std::numeric_limits<float>::max();
			for (size_t i = 0; i < vertices.size(); ++i) {
				const terrain::UndergrowthPatchVertex& source = patch.vertices[i];
				lowY = std::min(lowY, source.position[1]);
				highY = std::max(highY, source.position[1]);
				Vertex& v = vertices[i];
				std::copy(source.position.begin(), source.position.end(), v.position);
				v.normal[1] = 1;
				v.texCoord[0] = source.texCoord[0];
				v.texCoord[1] = source.texCoord[1];
				v.lightmapCoord[0] = source.height;
				v.part = source.swayIndex;
				v.part2 = source.sway / 255.0f;
				v.weight = source.terrainColorMix / 255.0f;
			}
			buffers.vertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
				static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
			buffers.indices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_INDEX, patch.indices.data(),
				static_cast<uint32_t>(patch.indices.size() * sizeof(uint32_t)));
			buffers.indexCount = static_cast<uint32_t>(patch.indices.size());
			// Its sphere: about the box the patch's square and its plants'
			// heights make (0x1010a9a0, 0x101a7b10).
			const auto origin = m_undergrowth->patchOrigin(index);
			const float half = size * 0.5f, halfY = (highY - lowY) * 0.5f;
			buffers.sphere[0] = origin[0] + half;
			buffers.sphere[1] = lowY + halfY;
			buffers.sphere[2] = origin[1] + half;
			buffers.sphere[3] = std::sqrt(2 * half * half + halfY * halfY);
			uploaded = true;
		}
	}
	if (uploaded) {
		m_gpu.flushUploads();
	}
}

void LevelRenderer::drawUndergrowth(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass,
	const Mat4& viewProjection, const Camera& camera, const scene::LightSettings& lights)
{
	if (!m_undergrowth || m_undergrowthPatches.empty() || m_patchSize <= 0 || !m_undergrowthAtlas) {
		return;
	}
	// Antialiased (0x1010e270 with 0x1000b500 and +0x50a), between the
	// scene draw's Alpha2Mask on and off (0x1002cef0): alpha to coverage,
	// the alpha reference 0 and the atlas's alpha over the alpha cut
	// (Transparency_x8: 1 / AlphaRef / 8, times 8 in Undergrowth.fx).
	const bool coverage = alphaToCoverage();
	SDL_BindGPUGraphicsPipeline(pass, coverage ? m_gpu.coverageUndergrowthPipeline() : m_gpu.undergrowthPipeline());
	const bool undergrowthShadows = !(m_sources && m_sources->shadows && !m_sources->shadows->undergrowthShadows);
	useShadows(commands, pass, 1, 3, false, undergrowthShadows);
	const auto& sun = m_terrainColorsFromData ? m_terrainSunColor : lights.terrainSunColor;
	const auto& gi = m_terrainColorsFromData ? m_terrainGIColor : lights.terrainGIColor;
	LightingUniforms lit = lighting(m_viewFog, lights, sun, gi, coverage ? 0.0f : m_undergrowthAlphaRef, true);
	LightingUniforms unlit = lighting(m_viewFog, lights, sun, gi, coverage ? 0.0f : m_undergrowthAlphaRef, false);
	if (coverage) {
		lit.material[2] = unlit.material[2] = AlphaToCoverage::undergrowthAlphaScale(m_undergrowthAlphaRef);
	}

	UndergrowthUniforms uniforms {};
	uniforms.viewProjection = viewProjection;
	uniforms.cameraPosition[0] = camera.position.x;
	uniforms.cameraPosition[1] = camera.position.y;
	uniforms.cameraPosition[2] = camera.position.z;
	// FadeAndHeightScaleOffset (0x1010e270, handle +0x94): (D, 1 / (D x
	// undergrowth.viewDistanceFade)), D = undergrowth.viewDistanceScale
	// (+0x578) x ViewDistance (+0x30); plants grow out of the ground over the
	// outer viewDistanceFade (+0x57c, 0.2) of D (Undergrowth.cfg's
	// ViewDistanceFadeScale, +0x34, is read by nothing).
	const scene::RendererSettings fadeDefaults;
	const scene::RendererSettings& ug = m_sources && m_sources->renderer ? *m_sources->renderer : fadeDefaults;
	uniforms.fade[0] = undergrowthViewDistance();
	uniforms.fade[1] = 1.0f / std::max(undergrowthViewDistance() * ug.undergrowthViewDistanceFade, 0.01f);
	// undergrowth.simpleShaderEnable: the terrain quality's 1 / 1 / 0 (0x10010040, slot 0x40).
	uniforms.fade[2] = m_sources && m_sources->quality && m_sources->quality->undergrowthSimpleShader() ? 1.0f : 0.0f;
	// The 16 sway offsets (terrain::UndergrowthSway: 0x1010ad20, 0x1010a900),
	// uploaded as SwayOffset from +0x1b8.
	const uint64_t swayNow = SDL_GetTicksNS();
	if (m_undergrowthSwayTicks) {
		m_undergrowthSway.advance(static_cast<float>(static_cast<double>(swayNow - m_undergrowthSwayTicks) / 1e9));
	}
	m_undergrowthSwayTicks = swayNow;
	for (int i = 0; i < 16; ++i) {
		const auto offset = m_undergrowthSway.offset(i);
		uniforms.swayOffsets[i][0] = offset[0];
		uniforms.swayOffsets[i][1] = offset[1];
	}
	for (const auto& [index, buffers] : m_undergrowthPatches) {
		if (!buffers.vertices) {
			continue;
		}
		// The terrain patch (colormap, lightmap) holding this undergrowth patch.
		const auto origin = m_undergrowth->patchOrigin(index);
		const float centerX = origin[0] + m_undergrowth->patchSize() * 0.5f;
		const float centerZ = origin[1] + m_undergrowth->patchSize() * 0.5f;
		const int tx = static_cast<int>(std::floor((centerX - m_patchOrigin[0]) / m_patchSize));
		const int tz = static_cast<int>(std::floor((centerZ - m_patchOrigin[1]) / m_patchSize));
		if (tx < 0 || tz < 0 || tx >= m_patchesPerSide || tz >= m_patchesPerSide) {
			continue;
		}
		const TerrainPatch& patch = m_terrain[static_cast<size_t>(tz * m_patchesPerSide + tx)];
		uniforms.terrainTexCoord[0] = 1 / m_patchSize;
		uniforms.terrainTexCoord[1] = 1 / m_patchSize;
		uniforms.terrainTexCoord[2] = -(m_patchOrigin[0] / m_patchSize + tx);
		uniforms.terrainTexCoord[3] = -(m_patchOrigin[1] / m_patchSize + tz);
		SDL_PushGPUVertexUniformData(commands, 0, &uniforms, sizeof(uniforms));
		// The patch's point lights (MeshLights.h): the nearest 4 about its sphere.
		LightingUniforms& patchLit = patch.lightmap ? lit : unlit;
		int picked[kUndergrowthPatchLights];
		const int pickedCount = m_listedLights.empty() ? 0
			: pickUndergrowthLights(m_listedLights.data(), m_listedLights.size(), buffers.sphere, buffers.sphere[3], picked);
		applyUndergrowthLights(patchLit, picked, pickedCount);
		SDL_PushGPUFragmentUniformData(commands, 0, &patchLit, sizeof(patchLit));
		SDL_GPUTextureSamplerBinding samplers[3] = {
			{m_undergrowthAtlas, m_gpu.clampSampler()},
			{patch.colormap ? patch.colormap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
			{patch.lightmap ? patch.lightmap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
		};
		SDL_BindGPUFragmentSamplers(pass, 0, samplers, 3);
		SDL_BindGPUVertexSamplers(pass, 0, samplers + 1, 2);
		SDL_GPUBufferBinding vertexBinding {buffers.vertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		SDL_GPUBufferBinding indexBinding {buffers.indices, 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
		SDL_DrawGPUIndexedPrimitives(pass, buffers.indexCount, 1, 0, 0, 0);
	}
}

void LevelRenderer::addObject(const LevelSources& sources, const world::Object& object)
{
	const auto known = std::lower_bound(m_knownObjects.begin(), m_knownObjects.end(), object.id());
	if (known != m_knownObjects.end() && *known == object.id()) {
		return;
	}
	m_knownObjects.insert(known, object.id());
	if (const world::ObjectTemplate* objectTemplate = object.objectTemplate()) {
		const size_t first = m_instances.size();
		addTemplate(sources, *objectTemplate, Mat4 {}, objectTransform(object.position(), object.rotation()), object.id(), 0);
		const terrain::Heightmap* ground = sources.heightmaps.primary();
		for (size_t i = first; i < m_instances.size(); ++i) {
			Instance& instance = m_instances[i];
			instance.overgrowth = object.isOvergrowth() && m_overgrowthCloseDistance > 0;
			if (!instance.overgrowth) {
				continue;
			}
			// The type's factor from its geometry's radius, and the cell the
			// tree is in (OvergrowthLod.h). The original gathers them once at
			// load: the preLoad 0x100909e0 makes a record per cell at the
			// ground's height under its centre, then 0x1008fb30 puts each
			// isOvergrowth object (slot 0x124) with a "<geometry>_lod" mesh in
			// the cell under it, its type found or added by name (0x10121790)
			// and its own draw left to the overgrowth.
			instance.overgrowthTypeFactor = OvergrowthBlend::typeFactor(std::max(instance.mesh->bounds.radius, 0.0f));
			const float size = m_overgrowthCellSize;
			const float x = std::floor(instance.world.at(3, 0) / size) * size;
			const float z = std::floor(instance.world.at(3, 2) / size) * size;
			instance.overgrowthCell[0] = x;
			instance.overgrowthCell[1] = ground && ground->loaded() ? ground->heightAt(x + size * 0.5f, z + size * 0.5f) : 0.0f;
			instance.overgrowthCell[2] = z;
		}
	}
}

void LevelRenderer::syncObjects()
{
	if (!m_sources) {
		return;
	}
	const world::ObjectManager& objects = m_sources->objects;
	// Drop what belongs to removed objects.
	m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(),
						  [&objects](const Instance& instance) {
							  return instance.object != 0 && !objects.find(instance.object);
						  }),
		m_instances.end());
	m_knownObjects.erase(std::remove_if(m_knownObjects.begin(), m_knownObjects.end(),
							 [&objects](uint32_t id) { return !objects.find(id); }),
		m_knownObjects.end());
	m_objectLights.erase(std::remove_if(m_objectLights.begin(), m_objectLights.end(),
							 [&objects](const ObjectLight& light) { return !objects.find(light.object); }),
		m_objectLights.end());
	const size_t before = m_instances.size();
	for (const auto& object : objects.objects()) {
		addObject(*m_sources, *object);
	}
	if (m_instances.size() != before) {
		m_gpu.flushUploads();
	}
	// Move instances with their objects.
	const world::Object* current = nullptr;
	Mat4 objectWorld;
	bool hiddenInView = false;
	for (Instance& instance : m_instances) {
		if (instance.object == 0) {
			continue;
		}
		if (!current || current->id() != instance.object) {
			current = objects.find(instance.object);
			if (!current) {
				continue;
			}
			objectWorld = objectTransform(current->position(), current->rotation());
			hiddenInView = m_sources->hiddenInView && m_sources->hiddenInView(instance.object);
		}
		instance.world = instance.local * objectWorld;
		instance.hidden = current->hidden() || hiddenInView;
		const int look = m_sources->partLook && !instance.source.empty() ? m_sources->partLook(instance.object, instance.source) : 0;
		instance.hidden = instance.hidden || look == 2;
		// The cockpit from inside: another geometry of its mesh.
		if (instance.mesh->geometries > 1) {
			int geometry = -1, lod = 0;
			if (!(m_sources->geometryFor && m_sources->geometryFor(instance.object, instance.source, geometry, lod))) {
				geometry = -1;
				lod = 0;
			}
			if (look == 1 && instance.mesh->geometries > 2) {
				geometry = 2; // a wrecked part: the mesh's wreck geometry
				lod = 0;
			}
			if (geometry != instance.shownGeometry || lod != instance.shownLod) {
				const GpuMesh* shown = mesh(*m_sources, instance.geometry, geometry, geometry < 0 ? 0 : lod);
				if (shown) {
					instance.mesh = shown;
					m_gpu.flushUploads();
				}
				instance.shownGeometry = geometry;
				instance.shownLod = lod;
			}
		}
		instance.fade = current->fade();
		instance.objectOrigin = {current->position().x, current->position().y, current->position().z};
		if (instance.cullRadius < 0) {
			// The object's radius (slot 0x8c) as the mesh's sphere, x the
			// template's cullRadiusScale (+0x58, 1 by default; read once).
			const world::ObjectTemplate* t = current->objectTemplate();
			float scale = 1;
			if (t) {
				const auto it = t->settings().find("cullRadiusScale");
				if (it != t->settings().end()) {
					scale = static_cast<float>(std::atof(it->second.c_str()));
				}
				instance.cullActor = t->typeName() == "Soldier" || t->typeName() == "PlayerControlObject";
			}
			// A soldier: his collision box's; another skinned mesh has no
			// sphere, so its geometry's box radius (RendDX9 0x10160d60).
			const float radius = t && t->typeName() == "Soldier" ? soldierCullRadius()
				: instance.mesh->skinned ? instance.mesh->skinnedRadius : instance.mesh->bounds.radius;
			instance.cullRadius = std::max(radius, 0.0f) * scale;
		}
		if (!instance.parts.empty() && m_sources->partTransforms) {
			std::vector<anim::Matrix> placed(instance.parts.size(), anim::Matrix {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
			if (m_sources->partTransforms(instance.object, placed)) {
				for (size_t i = 0; i < placed.size(); ++i) {
					std::copy(placed[i].begin(), placed[i].end(), instance.parts[i].m.begin());
				}
				continue;
			}
		}
		if (instance.partsTemplate && m_sources->partPose) {
			std::fill(instance.parts.begin(), instance.parts.end(), Mat4 {});
			m_animatedUVParts.clear();
			collectParts(*m_sources, *instance.partsTemplate, Mat4 {}, instance.parts, 0, instance.object,
				&m_animatedUVParts);
			// The parts' animated UVs (0x0063b9b0, each object update): the
			// distance each moved along its own z axis in the world.
			instance.animatedUV = !m_animatedUVParts.empty();
			instance.uvStates.resize(m_animatedUVParts.size());
			for (size_t i = 0; i < m_animatedUVParts.size(); ++i) {
				const Mat4 world = m_animatedUVParts[i].local * objectWorld;
				const float position[3] = {world.at(3, 0), world.at(3, 1), world.at(3, 2)};
				float z[3] = {world.at(2, 0), world.at(2, 1), world.at(2, 2)};
				const float length = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
				if (length > 0) {
					z[0] /= length;
					z[1] /= length;
					z[2] /= length;
				}
				updateAnimatedUV(*m_animatedUVParts[i].settings, instance.uvStates[i], position, z, instance.uvMatrices);
			}
		}
		if (instance.mesh->skinned && m_sources->animationFor) {
			const std::string path = m_sources->animationFor(instance.object);
			if (!path.empty()) {
				instance.animation = animation(m_sources->files, path);
				instance.animationDuration = m_sources->animationDurationFor
					? m_sources->animationDurationFor(instance.object) : 0.0f;
			}
		}
	}
	syncDebris();
	syncPlaced();
}

void LevelRenderer::syncPlaced()
{
	m_placedScratch.clear();
	if (m_sources && m_sources->placedTemplates) {
		m_sources->placedTemplates(m_placedScratch);
	}
	if (m_placedScratch.empty() && !m_hadPlaced) {
		return;
	}
	m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(), [](const Instance& i) { return i.placed; }),
		m_instances.end());
	m_hadPlaced = !m_placedScratch.empty();
	bool uploaded = false;
	for (const LevelSources::PlacedTemplate& placed : m_placedScratch) {
		if (!placed.objectTemplate || placed.objectTemplate->geometry().empty()) {
			continue;
		}
		const std::string& geometry = placed.objectTemplate->geometry();
		const size_t before = m_stats.meshes;
		const GpuMesh* gpuMesh = mesh(*m_sources, geometry);
		uploaded = uploaded || m_stats.meshes != before;
		if (!gpuMesh) {
			continue;
		}
		Instance instance {gpuMesh, Mat4 {}, 0, Mat4 {}, nullptr, {1, 1, 0, 0}, {}, nullptr, nullptr};
		for (int row = 0; row < 3; ++row) {
			const effects::Vec3& r = placed.rotation.rows[static_cast<size_t>(row)];
			instance.world.at(row, 0) = r.x;
			instance.world.at(row, 1) = r.y;
			instance.world.at(row, 2) = r.z;
		}
		instance.world.at(3, 0) = placed.position.x;
		instance.world.at(3, 1) = placed.position.y;
		instance.world.at(3, 2) = placed.position.z;
		instance.local = instance.world;
		instance.geometry = geometry;
		instance.placed = true;
		if (gpuMesh->bundled) {
			auto parts = m_placedParts.find(placed.objectTemplate);
			if (parts == m_placedParts.end()) {
				std::vector<Mat4> rest(kMaxMeshParts, Mat4 {});
				collectParts(*m_sources, *placed.objectTemplate, Mat4 {}, rest, 0);
				parts = m_placedParts.emplace(placed.objectTemplate, std::move(rest)).first;
			}
			instance.parts = parts->second;
		}
		m_instances.push_back(std::move(instance));
	}
	if (uploaded) {
		m_gpu.flushUploads();
	}
}

void LevelRenderer::syncDebris()
{
	m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(), [](const Instance& i) { return i.debris; }),
		m_instances.end());
	if (!m_sources || !m_sources->effects || m_sources->effects->debris().empty()) {
		return;
	}
	// A part not shown collapses onto the piece's origin.
	Mat4 collapsed;
	collapsed.m = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
	bool uploaded = false;
	for (const effects::DebrisPiece& piece : m_sources->effects->debris()) {
		const float alpha = piece.alpha();
		if (alpha <= 0.0f) {
			continue;
		}
		const size_t before = m_stats.meshes;
		const GpuMesh* gpuMesh = mesh(*m_sources, piece.look->geometry);
		uploaded = uploaded || m_stats.meshes != before;
		if (!gpuMesh) {
			continue;
		}
		Instance instance {gpuMesh, Mat4 {}, 0, Mat4 {}, nullptr, {1, 1, 0, 0}, {}, nullptr, nullptr};
		const effects::Vec3 scale = piece.scale();
		const float axes[3] = {scale.x, scale.y, scale.z};
		for (int row = 0; row < 3; ++row) {
			const effects::Vec3& r = piece.rotation.rows[static_cast<size_t>(row)];
			instance.world.at(row, 0) = r.x * axes[row];
			instance.world.at(row, 1) = r.y * axes[row];
			instance.world.at(row, 2) = r.z * axes[row];
		}
		instance.world.at(3, 0) = piece.position.x;
		instance.world.at(3, 1) = piece.position.y;
		instance.world.at(3, 2) = piece.position.z;
		instance.local = instance.world;
		instance.geometry = piece.look->geometry;
		instance.fade = std::min(alpha, 1.0f);
		instance.debris = true;
		if (gpuMesh->bundled) {
			instance.parts.assign(kMaxMeshParts, collapsed);
			if (piece.look->part >= 0 && piece.look->part < kMaxMeshParts) {
				instance.parts[static_cast<size_t>(piece.look->part)] = Mat4 {};
			}
		}
		m_instances.push_back(std::move(instance));
	}
	if (uploaded) {
		m_gpu.flushUploads();
	}
}

// The HUD's quads for this frame: two triangles each, rotated about their
// centres (or their setPictureNodeCenterPoint pivots), grouped by texture
// and blend effect in drawing order.
void LevelRenderer::buildOverlay()
{
	m_overlayGroups.clear();
	const bool fps = m_sources && m_sources->renderer && m_sources->renderer->drawFps == 1;
	if (!m_sources || (!m_sources->overlay && !fps)) {
		return;
	}
	const uint64_t now = SDL_GetTicks();
	const float seconds = m_lastOverlayTicks ? static_cast<float>(now - m_lastOverlayTicks) / 1000.0f : 0.0f;
	m_lastOverlayTicks = now;
	std::vector<Vertex> vertices;
	std::vector<hud::HudQuad> quads;
	if (m_sources->overlay) {
		quads = m_sources->overlay(seconds);
	}
	if (fps) {
		appendFpsText(quads);
	}
	for (const hud::HudQuad& q : quads) {
		SDL_GPUTexture* image = texture(m_sources->files, q.texture, scene::TextureClass::Interface);
		if (!image) {
			continue;
		}
		const float cx = q.pivotSet ? q.pivot[0] : (q.x0 + q.x1) * 0.5f;
		const float cy = q.pivotSet ? q.pivot[1] : (q.y0 + q.y1) * 0.5f;
		const float c = std::cos(q.rotation * 3.14159265f / 180.0f), s = std::sin(q.rotation * 3.14159265f / 180.0f);
		// The quad's corners (x, y, u, v), turned about its centre, then cut
		// to its clip rectangle when it has one (Sutherland-Hodgman).
		std::vector<std::array<float, 4>> polygon;
		for (const auto& corner : {std::array<float, 4> {q.x0, q.y0, q.u0, q.v0}, {q.x1, q.y0, q.u1, q.v0},
				 {q.x1, q.y1, q.u1, q.v1}, {q.x0, q.y1, q.u0, q.v1}}) {
			const float dx = corner[0] - cx, dy = corner[1] - cy;
			polygon.push_back({cx + dx * c - dy * s, cy + dx * s + dy * c, corner[2], corner[3]});
		}
		if (q.clipped) {
			for (int edge = 0; edge < 4 && !polygon.empty(); ++edge) {
				const int axis = edge % 2;
				const float limit = q.clip[edge];
				const bool low = edge < 2;
				const auto inside = [&](const std::array<float, 4>& p) { return low ? p[axis] >= limit : p[axis] <= limit; };
				std::vector<std::array<float, 4>> kept;
				for (size_t i = 0; i < polygon.size(); ++i) {
					const auto& a = polygon[i];
					const auto& b = polygon[(i + 1) % polygon.size()];
					if (inside(a)) {
						kept.push_back(a);
					}
					if (inside(a) != inside(b)) {
						const float t = (limit - a[axis]) / (b[axis] - a[axis]);
						std::array<float, 4> p;
						for (int k = 0; k < 4; ++k) {
							p[k] = a[k] + (b[k] - a[k]) * t;
						}
						kept.push_back(p);
					}
				}
				polygon = std::move(kept);
			}
		}
		if (polygon.size() < 3) {
			continue;
		}
		// A wedge (HudQuad::sweep): the polygon cut to each quarter turn.
		const auto pieces = q.sweep >= 0 ? hud::sweepPieces(polygon, q.sweepAbout ? q.sweepCentre[0] : (q.x0 + q.x1) * 0.5f,
													 q.sweepAbout ? q.sweepCentre[1] : (q.y0 + q.y1) * 0.5f, q.sweep)
										 : std::vector<std::vector<std::array<float, 4>>> {polygon};
		const uint32_t first = static_cast<uint32_t>(vertices.size());
		for (const auto& piece : pieces) {
			for (size_t i = 1; i + 1 < piece.size(); ++i) {
				for (const auto& corner : {piece[0], piece[i], piece[i + 1]}) {
					Vertex v {};
					v.position[0] = corner[0];
					v.position[1] = corner[1];
					v.texCoord[0] = corner[2];
					v.texCoord[1] = corner[3];
					v.normal[0] = q.color[0];
					v.normal[1] = q.color[1];
					v.normal[2] = q.color[2];
					v.weight = q.color[3];
					vertices.push_back(v);
				}
			}
		}
		const uint32_t count = static_cast<uint32_t>(vertices.size()) - first;
		if (!m_overlayGroups.empty() && m_overlayGroups.back().texture == image &&
			m_overlayGroups.back().blend[0] == q.blend[0] && m_overlayGroups.back().blend[1] == q.blend[1]) {
			m_overlayGroups.back().vertexCount += count;
		} else {
			m_overlayGroups.push_back({image, first, count, {q.blend[0], q.blend[1]}});
		}
	}
	if (vertices.empty()) {
		m_overlayGroups.clear();
		return;
	}
	m_gpu.uploadDynamic(m_overlayVertices, SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
		static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
	m_gpu.flushUploads();
}

void LevelRenderer::appendFpsText(std::vector<hud::HudQuad>& quads)
{
	if (!m_fpsText) {
		m_fpsText = std::make_unique<FpsText>();
	}
	FpsText& fps = *m_fpsText;
	const uint64_t now = SDL_GetTicksNS();
	if (fps.lastNs) {
		fps.milliseconds = static_cast<double>(now - fps.lastNs) / 1e6;
	}
	fps.lastNs = now;
	if (!fps.loaded) {
		fps.loaded = true;
		for (const char* path : {"Fonts/800/system_8x11", "Fonts/system_8x11"}) {
			std::string text;
			if (m_sources->files.readText(std::string(path) + ".dif", text)) {
				fps.font = hud::parseHudFont(text);
				fps.texture = std::string(path) + ".dds";
				break;
			}
		}
	}
	if (!fps.font || fps.font->textureWidth <= 0 || fps.font->textureHeight <= 0) {
		return;
	}
	char line[64];
	std::snprintf(line, sizeof(line), "%.1f / %.1f", fps.milliseconds,
		fps.milliseconds > 0 ? 1000.0 / fps.milliseconds : 0.0);
	const hud::HudFont& font = *fps.font;
	float x = 6;
	const float y = 6;
	for (const char* c = line; *c; ++c) {
		const hud::Glyph* glyph = font.glyph(static_cast<unsigned char>(*c));
		if (!glyph) {
			continue;
		}
		hud::HudQuad q {};
		q.texture = fps.texture;
		q.x0 = x + glyph->before;
		q.y0 = y + static_cast<float>(glyph->top);
		q.x1 = q.x0 + static_cast<float>(glyph->x1 - glyph->x0);
		q.y1 = q.y0 + static_cast<float>(glyph->y1 - glyph->y0);
		q.u0 = static_cast<float>(glyph->x0) / static_cast<float>(font.textureWidth);
		q.v0 = static_cast<float>(glyph->y0) / static_cast<float>(font.textureHeight);
		q.u1 = static_cast<float>(glyph->x1) / static_cast<float>(font.textureWidth);
		q.v1 = static_cast<float>(glyph->y1) / static_cast<float>(font.textureHeight);
		q.color[0] = q.color[1] = q.color[2] = q.color[3] = 1;
		quads.push_back(q);
		x += glyph->advance();
	}
}

// An image made in memory, uploaded once while it lives (keyed by the
// image; textures of images gone are released).
SDL_GPUTexture* LevelRenderer::menuTexture(const std::shared_ptr<const ui::MenuImage>& image)
{
	for (auto it = m_menuTextures.begin(); it != m_menuTextures.end();) {
		if (it->second.image.expired()) {
			if (it->second.texture) {
				SDL_ReleaseGPUTexture(m_gpu.device(), it->second.texture);
			}
			it = m_menuTextures.erase(it);
		} else {
			++it;
		}
	}
	const auto known = m_menuTextures.find(image.get());
	if (known != m_menuTextures.end()) {
		return known->second.texture;
	}
	texture::DdsTexture upload;
	upload.format = texture::TextureFormat::RGBA8;
	upload.width = image->width;
	upload.height = image->height;
	upload.faces.push_back({{image->width, image->height, image->rgba}});
	SDL_GPUTexture* created = image->width && image->height ? m_gpu.createTexture(upload) : nullptr;
	m_menuTextures[image.get()] = {image, created};
	return created;
}

// The menu's triangles for this frame, grouped by texture in drawing order.
void LevelRenderer::buildMenu(float seconds)
{
	m_menuGroups.clear();
	if (!m_sources || !m_sources->menu) {
		return;
	}
	const ui::MenuFrame frame = m_sources->menu(seconds);
	m_menuSize[0] = frame.width;
	m_menuSize[1] = frame.height;
	std::vector<Vertex> vertices;
	// A stencil clear covers the stage (no colour is written).
	const auto stageQuad = [&frame] {
		std::vector<ui::MenuVertex> quad(6);
		const float corners[6][2] = {{0, 0}, {frame.width, 0}, {frame.width, frame.height}, {0, 0}, {frame.width, frame.height},
			{0, frame.height}};
		for (int i = 0; i < 6; ++i) {
			quad[static_cast<size_t>(i)].x = corners[i][0];
			quad[static_cast<size_t>(i)].y = corners[i][1];
		}
		return quad;
	};
	for (const ui::MenuBatch& batch : frame.batches) {
		const bool clear = batch.stencil == ui::MenuBatch::Stencil::Clear;
		SDL_GPUTexture* image = clear ? m_gpu.whiteTexture()
			: batch.image             ? menuTexture(batch.image)
									  : texture(m_sources->files, batch.texture, scene::TextureClass::Interface);
		const std::vector<ui::MenuVertex> quad = clear ? stageQuad() : std::vector<ui::MenuVertex>();
		const std::vector<ui::MenuVertex>& batchVertices = clear ? quad : batch.vertices;
		if (!image || batchVertices.empty()) {
			continue;
		}
		const uint32_t first = static_cast<uint32_t>(vertices.size());
		for (const ui::MenuVertex& m : batchVertices) {
			Vertex v {};
			v.position[0] = m.x;
			v.position[1] = m.y;
			v.texCoord[0] = m.u;
			v.texCoord[1] = m.v;
			v.normal[0] = m.mult[0];
			v.normal[1] = m.mult[1];
			v.normal[2] = m.mult[2];
			v.weight = m.mult[3];
			v.lightmapCoord[0] = m.add[0];
			v.lightmapCoord[1] = m.add[1];
			v.part = m.add[2];
			v.part2 = m.add[3];
			vertices.push_back(v);
		}
		const uint32_t count = static_cast<uint32_t>(batchVertices.size());
		const MenuGroup* last = m_menuGroups.empty() ? nullptr : &m_menuGroups.back();
		if (last && last->texture == image && last->repeat == batch.repeat && last->stencil == batch.stencil &&
			last->stencilLevel == batch.stencilLevel) {
			m_menuGroups.back().vertexCount += count;
		} else {
			m_menuGroups.push_back({image, batch.repeat, first, count, batch.stencil, batch.stencilLevel});
		}
	}
	if (vertices.empty()) {
		m_menuGroups.clear();
		return;
	}
	m_gpu.uploadDynamic(m_menuVertices, SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
		static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
	m_gpu.flushUploads();
}

// Two triangles per particle, uploaded for this frame. The managers are
// told the camera, the water, the wind and the settings first (what their
// next update reads: the view distance, the distance LOD).
void LevelRenderer::buildParticles(const Camera& camera)
{
	if (m_sources) {
		// The manager's settings with VideoSettings' effects row over them
		// (0x100991b0; no shipped script sets ParticleSystemManager.*).
		m_particleSettings = m_sources->particles ? *m_sources->particles : scene::ParticleSystemSettings {};
		if (m_sources->quality) {
			m_sources->quality->applyEffects(m_particleSettings);
		}
		const bool water = m_sources->heightmaps.hasSea();
		const float sea = m_sources->heightmaps.seaWaterLevel();
		for (const effects::EffectManager* manager : {m_sources->effects, m_sources->objectEffects}) {
			if (manager) {
				manager->setSettings(&m_particleSettings);
				manager->setWind(m_sources->wind);
				manager->setWater(water, sea);
				// collisionEnable's ground: the primary terrain.
				if (const terrain::Heightmap* heights = m_sources->heightmaps.primary(); heights && heights->loaded()) {
					manager->setGround([heights](float x, float z) { return heights->heightAt(x, z); });
				}
				manager->setCamera({camera.position.x, camera.position.y, camera.position.z}, std::tan(camera.fovY / 2));
			}
		}
	}
	buildParticleBuffer({m_sources ? m_sources->effects : nullptr, m_sources ? m_sources->objectEffects : nullptr},
		m_particleVertices, m_particleGroups, camera, true);
	buildShimmer(camera);
}

// The particles' light (FXCommon.fx calcParticleLighting, Particles.fx
// psParticleMedium / psParticleHigh by the manager's shader quality):
// lerp(1, lerp(effectShadowColor, effectSunColor, saturate(lm + offset)),
// lightFactor), lm the base hemi map's alpha under the particle (High; 1
// at Medium), offset saturate(clamp((y - HemiShadowAltitude) / 10, 0, 1) +
// lightMapIntensityOffset) with HemiShadowAltitude the base hemi map's
// height (its fourth argument, HemiMapManager +0x44; RendDX9 0x10131d70)
// and lightFactor the light manager's defaultEffectLightAffectionFactor
// (the templates' own, +0x1fc, stays unset: 0x10139800). Low (1) is unlit.
std::array<float, 3> LevelRenderer::particleLight(const effects::Vec3& at, float lightMapOffset) const
{
	const int quality = m_particleSettings.particleSystemShaderQuality;
	if (!m_sources || quality <= 1) {
		return {1, 1, 1};
	}
	const scene::LightSettings& lights = m_sources->lights;
	const float altitude = m_sources->hemi ? m_sources->hemi->height : 0;
	const float offset = std::clamp(std::clamp((at.y - altitude) / 10.0f, 0.0f, 1.0f) + lightMapOffset, 0.0f, 1.0f);
	const float lm = quality >= 3 ? hemiShadowAt(at.x, at.z) : 1.0f;
	const float lut = std::clamp(lm + offset, 0.0f, 1.0f);
	const float f = lights.defaultEffectLightAffectionFactor;
	std::array<float, 3> out;
	for (int i = 0; i < 3; ++i) {
		const float diffuse = lights.effectShadowColor[static_cast<size_t>(i)]
			+ (lights.effectSunColor[static_cast<size_t>(i)] - lights.effectShadowColor[static_cast<size_t>(i)]) * lut;
		out[static_cast<size_t>(i)] = 1 + (diffuse - 1) * f;
	}
	return out;
}

effects::ParticleView LevelRenderer::particleView(const Camera& camera) const
{
	effects::ParticleView view;
	const Vec3f forward = camera.forward();
	view.eye = {camera.position.x, camera.position.y, camera.position.z};
	view.forward = {forward.x, forward.y, forward.z};
	view.settings = m_sources ? &m_particleSettings : nullptr;
	view.nearPlane = camera.nearPlane;
	return view;
}

// The heat shimmer's sprites, drawn after the world over a copy of it.
void LevelRenderer::buildShimmer(const Camera& camera)
{
	m_shimmerGroups.clear();
	if (!m_sources) {
		return;
	}
	const effects::ParticleView view = particleView(camera);
	std::vector<Vertex> vertices;
	static const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
	for (const effects::EffectManager* manager : {m_sources->effects, m_sources->objectEffects}) {
		if (!manager) {
			continue;
		}
		manager->forEachShimmerGroup(view, [&](const std::string& path, bool, const std::vector<effects::Sprite>& sprites) {
			SDL_GPUTexture* image = texture(m_sources->files, path + ".dds");
			if (!image) {
				return;
			}
			const uint32_t first = static_cast<uint32_t>(vertices.size());
			for (const effects::Sprite& sprite : sprites) {
				for (const auto& corner : corners) {
					Vertex v {};
					v.position[0] = sprite.position.x;
					v.position[1] = sprite.position.y;
					v.position[2] = sprite.position.z;
					v.weight = sprite.color[3];
					v.texCoord[0] = corner[0];
					v.texCoord[1] = corner[1];
					v.detailCoord[0] = sprite.pivot[0];
					v.detailCoord[1] = sprite.pivot[1];
					// The flipbook frame: its corner and size (particle.vert).
					v.dirtCoord[0] = sprite.frame[0];
					v.dirtCoord[1] = sprite.frame[1];
					v.crackCoord[0] = sprite.frame[2] - sprite.frame[0];
					v.crackCoord[1] = sprite.frame[3] - sprite.frame[1];
					v.part = sprite.size;
					v.part2 = sprite.rotation;
					vertices.push_back(v);
				}
			}
			m_shimmerGroups.push_back({image, false, first, static_cast<uint32_t>(vertices.size()) - first});
		});
	}
	if (!vertices.empty()) {
		m_gpu.uploadDynamic(m_shimmerVertices, SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
			static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
	}
}

void LevelRenderer::buildParticleBuffer(std::initializer_list<const effects::EffectManager*> managers,
	SDL_GPUBuffer*& buffer, std::vector<ParticleGroup>& groups, const Camera& camera, bool weather, bool zoomed)
{
	const Vec3f eye = camera.position;
	groups.clear();
	if (!m_sources) {
		return;
	}
	std::vector<Vertex> vertices;
	effects::ParticleView view = particleView(camera);
	view.zoomed = zoomed;
	for (const effects::EffectManager* manager : managers) {
		if (!manager) {
			continue;
		}
		manager->forEachGroup(view, [&](const std::string& path, bool additive, const std::vector<effects::Sprite>& sprites) {
			SDL_GPUTexture* image = texture(m_sources->files, path + ".dds");
			if (!image) {
				return;
			}
			const uint32_t first = static_cast<uint32_t>(vertices.size());
			static const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
			for (const effects::Sprite& sprite : sprites) {
				const std::array<float, 3> light = particleLight(sprite.position, sprite.lightMapOffset);
				for (const auto& corner : corners) {
					Vertex v {};
					v.position[0] = sprite.position.x;
					v.position[1] = sprite.position.y;
					v.position[2] = sprite.position.z;
					v.normal[0] = sprite.color[0] * light[0];
					v.normal[1] = sprite.color[1] * light[1];
					v.normal[2] = sprite.color[2] * light[2];
					v.weight = sprite.color[3];
					v.texCoord[0] = corner[0];
					v.texCoord[1] = corner[1];
					v.detailCoord[0] = sprite.pivot[0];
					v.detailCoord[1] = sprite.pivot[1];
					// The flipbook frame: its corner and size (particle.vert).
					v.dirtCoord[0] = sprite.frame[0];
					v.dirtCoord[1] = sprite.frame[1];
					v.crackCoord[0] = sprite.frame[2] - sprite.frame[0];
					v.crackCoord[1] = sprite.frame[3] - sprite.frame[1];
					v.part = sprite.size;
					v.part2 = sprite.rotation;
					vertices.push_back(v);
				}
			}
			if (!groups.empty() && groups.back().texture == image && groups.back().additive == additive) {
				groups.back().vertexCount += static_cast<uint32_t>(vertices.size()) - first;
			} else {
				groups.push_back({image, additive, first, static_cast<uint32_t>(vertices.size()) - first});
			}
		});
		// Non-screen-aligned quads and trails: placed already (a negative
		// size tells particle.vert, the coordinates ride in lightmapCoord).
		manager->forEachViewedWorldGroup(view,
			[&](const std::string& path, bool additive, const std::vector<effects::WorldVertex>& triangles) {
				SDL_GPUTexture* image = texture(m_sources->files, path + ".dds");
				if (!image) {
					return;
				}
				const uint32_t first = static_cast<uint32_t>(vertices.size());
				for (const effects::WorldVertex& w : triangles) {
					const std::array<float, 3> light = particleLight(w.position, w.lightMapOffset);
					Vertex v {};
					v.position[0] = w.position.x;
					v.position[1] = w.position.y;
					v.position[2] = w.position.z;
					v.normal[0] = w.color[0] * light[0];
					v.normal[1] = w.color[1] * light[1];
					v.normal[2] = w.color[2] * light[2];
					v.weight = w.color[3];
					v.lightmapCoord[0] = w.u;
					v.lightmapCoord[1] = w.v;
					v.part = -1;
					vertices.push_back(v);
				}
				groups.push_back({image, additive, first, static_cast<uint32_t>(vertices.size()) - first, true});
			});
	}
	if (weather && m_sources && m_sources->rain) {
		const scene::RainSettings& rain = *m_sources->rain;
		const uint64_t now = SDL_GetTicks();
		const float seconds = m_lastWeatherTicks ? std::min(static_cast<float>(now - m_lastWeatherTicks) / 1000.0f, 0.1f) : 0.0f;
		m_lastWeatherTicks = now;
		m_weather.update(rain, {eye.x, eye.y, eye.z}, seconds);
		// Pixels to metres at a distance: the game's 600-line screen.
		const float perPixel = 2 * std::tan(camera.fovY / 2) / 600.0f;
		const Vec3f forward = camera.forward();
		const uint32_t first = static_cast<uint32_t>(vertices.size());
		const auto push = [&](const Vec3f& at, float u, float v, float alpha) {
			Vertex out {};
			out.position[0] = at.x;
			out.position[1] = at.y;
			out.position[2] = at.z;
			out.normal[0] = rain.particleColor[0];
			out.normal[1] = rain.particleColor[1];
			out.normal[2] = rain.particleColor[2];
			out.weight = rain.particleColor[3] * alpha;
			out.lightmapCoord[0] = u;
			out.lightmapCoord[1] = v;
			out.part = -1;
			vertices.push_back(out);
		};
		SDL_GPUTexture* image = nullptr;
		if (rain.lineDisplay) {
			image = m_gpu.whiteTexture(); // psLine: the colour alone
			for (const effects::Weather::Drop& drop : m_weather.drops()) {
				const Vec3f a {drop.position.x, drop.position.y, drop.position.z}, b {drop.tail.x, drop.tail.y, drop.tail.z};
				const Vec3f toEye = eye - a;
				const float depth = dot(toEye * -1.0f, forward);
				if (depth <= 0.1f) {
					continue;
				}
				const Vec3f side = normalize(cross(b - a, toEye)) * (0.5f * perPixel * depth);
				const Vec3f c0 = a - side, c1 = a + side, c2 = b + side, c3 = b - side;
				for (const Vec3f* c : {&c0, &c1, &c2, &c0, &c2, &c3}) {
					push(*c, 0.5f, 0.5f, drop.alpha);
				}
			}
		} else if (!rain.texture.empty()) {
			image = texture(m_sources->files, rain.texture + ".dds");
			const Vec3f right = camera.right();
			const Vec3f up = cross(forward, right) * -1.0f;
			for (const effects::Weather::Drop& drop : m_weather.drops()) {
				const Vec3f at {drop.position.x, drop.position.y, drop.position.z};
				const float depth = dot(at - eye, forward);
				if (depth <= 0.1f) {
					continue;
				}
				const float half = 0.5f * drop.pixels * perPixel * depth;
				const Vec3f r = right * half, u = up * half;
				const Vec3f c0 = at - r - u, c1 = at + r - u, c2 = at + r + u, c3 = at - r + u;
				push(c0, 0, 1, drop.alpha);
				push(c1, 1, 1, drop.alpha);
				push(c2, 1, 0, drop.alpha);
				push(c0, 0, 1, drop.alpha);
				push(c2, 1, 0, drop.alpha);
				push(c3, 0, 0, drop.alpha);
			}
		}
		if (image && vertices.size() > first) {
			// Added (Rain.fx: SrcAlpha, One).
			groups.push_back({image, true, first, static_cast<uint32_t>(vertices.size()) - first});
		} else {
			vertices.resize(first);
		}
	}
	if (vertices.empty()) {
		groups.clear();
		return;
	}
	m_gpu.uploadDynamic(buffer, SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
		static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
	m_gpu.flushUploads();
}

// The lights of this frame (MeshLights.h): the effect managers'
// LightSources and the placed objects' dynamic ones (the levels' light
// bundles; a static one, isDynamic 0, is the lightmaps' only), flickered,
// listed when their spheres meet the view within lightCullDistance. The
// meshes pick their own from the list (drawInstance); the terrain, roads
// and particles take the nearest the camera.
void LevelRenderer::collectPointLights(const Camera& camera)
{
	m_pointLights.clear();
	m_listedLights.clear();
	m_cameraLights.clear();
	m_terrainLights.clear();
	if (!m_sources) {
		return;
	}
	// The light manager's limits: the dynamic shadows' quality sets them
	// (0x10010040), else lightManager.* as the scripts left them.
	if (m_sources->shadows) {
		m_lightLimits = LightLimits::forShadowQuality(m_sources->shadows->quality);
	} else {
		m_lightLimits.cullDistance = m_sources->lights.lightCullDistance;
		m_lightLimits.staticMesh = m_sources->lights.maxLightsPerStaticMesh;
		m_lightLimits.bundledMesh = m_sources->lights.maxLightsPerBundledMesh;
		m_lightLimits.skinnedMesh = m_sources->lights.maxLightsPerSkinnedMesh;
	}
	const float cull = m_lightLimits.cullDistance;
	if (cull <= 0) {
		return;
	}
	const auto consider = [&](const effects::PointLight& light) {
		const Vec3f at {light.position.x, light.position.y, light.position.z};
		const Vec3f d = at - camera.position;
		const float reach = std::max(light.range2, 0.0f);
		if (std::sqrt(dot(d, d)) - reach > cull || !m_frameFrustum.containsSphere(at, reach)) {
			return;
		}
		m_pointLights.push_back(light);
		ListedLight listed;
		listed.position[0] = at.x;
		listed.position[1] = at.y;
		listed.position[2] = at.z;
		listed.radius = reach;
		listed.spot = light.spot;
		listed.affectsLightmapped = light.affectsLightmapped;
		listed.supportsEmitter = light.supportsEmitter;
		m_listedLights.push_back(listed);
	};
	// VideoSettings' dynamic lighting 0 turns the dynamic lights off (the
	// light manager's slot 0xf4, 0x10010040): the effects' lights.
	const bool dynamicLights = !m_sources->quality || m_sources->quality->dynamicLightsEnabled();
	for (const effects::EffectManager* manager :
		{m_sources->effects, m_sources->objectEffects, m_hasFirstPerson ? m_firstPerson.effects : nullptr}) {
		if (!manager || !dynamicLights) {
			continue;
		}
		for (const effects::PointLight& light : manager->lights()) {
			consider(light);
		}
	}
	const world::Object* current = nullptr;
	Mat4 objectWorld;
	for (const ObjectLight& placed : m_objectLights) {
		if (!placed.look.dynamic) {
			continue; // every caller's filter asks for isDynamic (0x100a7f00 bit 8)
		}
		if (!current || current->id() != placed.object) {
			current = m_sources->objects.find(placed.object);
			if (!current) {
				continue;
			}
			objectWorld = objectTransform(current->position(), current->rotation());
		}
		if (current->hidden()) {
			continue;
		}
		const Mat4 world = placed.local * objectWorld;
		effects::PointLight light;
		light.position = {world.at(3, 0), world.at(3, 1), world.at(3, 2)};
		light.color = placed.look.color;
		// 0x0064a920: the ranges flickered, the colour not.
		const float f = flickerFactor(placed.look.flicker, placed.look.flicker > 0 ? m_flickerRand.next() : 0);
		light.range1 = placed.look.range1 * f;
		light.range2 = placed.look.range2 * f;
		light.templateRange2 = placed.look.range2;
		light.spot = placed.look.spot;
		const effects::Vec3& a = placed.look.direction;
		light.direction = {a.x * world.at(0, 0) + a.y * world.at(1, 0) + a.z * world.at(2, 0),
			a.x * world.at(0, 1) + a.y * world.at(1, 1) + a.z * world.at(2, 1),
			a.x * world.at(0, 2) + a.y * world.at(1, 2) + a.z * world.at(2, 2)};
		light.cone = placed.look.cone;
		light.affectsLightmapped = placed.look.affectsLightmapped;
		light.specColor = placed.look.specColor;
		light.supportsEmitter = placed.look.supportsEmitter;
		consider(light);
	}
	// The rest's: the nearest the camera first.
	std::vector<std::pair<float, int>> byCamera;
	for (size_t i = 0; i < m_pointLights.size(); ++i) {
		const effects::PointLight& l = m_pointLights[i];
		const Vec3f d = Vec3f {l.position.x, l.position.y, l.position.z} - camera.position;
		byCamera.push_back({std::sqrt(dot(d, d)) - l.range2, static_cast<int>(i)});
	}
	std::stable_sort(byCamera.begin(), byCamera.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	for (size_t i = 0; i < byCamera.size() && i < static_cast<size_t>(kMaxPointLights); ++i) {
		m_cameraLights.push_back(byCamera[i].second);
	}
	// The terrain takes every listed Point light (0x909), a pass each; the
	// shader's kMaxPointLights nearest the camera here (pickTerrainLights).
	const float eye[3] = {camera.position.x, camera.position.y, camera.position.z};
	int terrainPicked[kMaxPointLights];
	m_terrainLights.assign(terrainPicked,
		terrainPicked + pickTerrainLights(m_listedLights.data(), m_listedLights.size(), eye, kMaxPointLights, terrainPicked));
}

void LevelRenderer::bindMeshLayers(SDL_GPURenderPass* pass, const Submesh* submesh, SDL_GPUTexture* envMap) const
{
	SDL_GPUTexture* flat = m_gpu.flatNormalTexture();
	const SDL_GPUTextureSamplerBinding samplers[3] = {
		{submesh && submesh->detailNormal ? submesh->detailNormal : flat, m_gpu.repeatSampler()},
		{submesh && submesh->crackNormal ? submesh->crackNormal : flat, m_gpu.repeatSampler()},
		{envMap ? envMap : m_gpu.whiteCubeTexture(), m_gpu.clampSampler()},
	};
	SDL_BindGPUFragmentSamplers(pass, kMeshLayerSamplers, samplers, 3);
}

void LevelRenderer::applyMeshLights(LightingUniforms& uniforms, const int* picked, int count, LightedMesh kind) const
{
	uniforms.lightCount[0] = static_cast<float>(count);
	uniforms.lightCount[1] = static_cast<float>(static_cast<int>(kind) + 1);
	for (int i = 0; i < count; ++i) {
		const effects::PointLight& l = m_pointLights[static_cast<size_t>(picked[i])];
		// Static meshes by the light's (flickered) range, bundled and
		// skinned ones by the template's; a spot is a point light here.
		const float range = kind == LightedMesh::Static || l.templateRange2 < 0 ? l.range2 : l.templateRange2;
		const float position[4] = {l.position.x, l.position.y, l.position.z, range};
		const float color[4] = {l.color.x, l.color.y, l.color.z, l.range1};
		// The meshes have no cone; lightSpot's xyz carries the skinned
		// meshes' specular colour (the Light record's specularColor: the
		// template's speccolor, 0x10162070).
		const float spot[4] = {l.specColor.x, l.specColor.y, l.specColor.z, -2.0f};
		const float extra[4] = {0, 1, 0, 0};
		std::copy(position, position + 4, uniforms.lightPosition[i]);
		std::copy(color, color + 4, uniforms.lightColor[i]);
		std::copy(spot, spot + 4, uniforms.lightSpot[i]);
		std::copy(extra, extra + 4, uniforms.lightExtra[i]);
	}
}

void LevelRenderer::applyTerrainLights(LightingUniforms& uniforms) const
{
	applyUndergrowthLights(uniforms, m_terrainLights.data(), static_cast<int>(m_terrainLights.size()));
}

void LevelRenderer::applyUndergrowthLights(LightingUniforms& uniforms, const int* picked, int count) const
{
	uniforms.lightCount[0] = static_cast<float>(count);
	uniforms.lightCount[1] = 0;
	for (int i = 0; i < count; ++i) {
		const effects::PointLight& l = m_pointLights[static_cast<size_t>(picked[i])];
		// POINTLIGHT / PointLightPosAtten: the position and 1 / R^2 with R
		// the light object's range (slot 0x198, flickered).
		const float position[4] = {l.position.x, l.position.y, l.position.z, pointLightInverseRange2(l.range2)};
		const float color[4] = {l.color.x, l.color.y, l.color.z, 0};
		std::copy(position, position + 4, uniforms.lightPosition[i]);
		std::copy(color, color + 4, uniforms.lightColor[i]);
	}
}

void LevelRenderer::applyPointLights(LightingUniforms& uniforms) const
{
	uniforms.lightCount[0] = static_cast<float>(m_cameraLights.size());
	uniforms.lightCount[1] = 0;
	for (size_t i = 0; i < m_cameraLights.size(); ++i) {
		const effects::PointLight& l = m_pointLights[static_cast<size_t>(m_cameraLights[i])];
		const float position[4] = {l.position.x, l.position.y, l.position.z, l.range2};
		const float color[4] = {l.color.x, l.color.y, l.color.z, l.range1};
		const float spot[4] = {l.direction.x, l.direction.y, l.direction.z, l.spot ? l.cone : -2.0f};
		const float extra[4] = {0, l.affectsLightmapped ? 1.0f : 0.0f, 0, 0};
		std::copy(position, position + 4, uniforms.lightPosition[i]);
		std::copy(color, color + 4, uniforms.lightColor[i]);
		std::copy(spot, spot + 4, uniforms.lightSpot[i]);
		std::copy(extra, extra + 4, uniforms.lightExtra[i]);
	}
}

// The decals as squares on their surfaces, lifted off them a little.
// The terrain's triangles about a decal (0x10164d70: the 3 x 3 heightmap
// cells about the hit, each split (4c, 4c + 1, 4c + 2), (4c + 2, 4c + 1,
// 4c + 3) as the table 0x101f78f0 has it), and the decal clipped to them.
// The cells reach far enough for a decal up to a cell across; larger ones
// take the cells under their square.
std::vector<effects::DecalVertex> LevelRenderer::clipDecalToTerrain(const terrain::Heightmap& ground,
	const effects::Decal& decal, float normalOffset)
{
	const float sx = ground.scale()[0], sz = ground.scale()[2];
	if (sx <= 0 || sz <= 0) {
		return {};
	}
	const float half = decal.size * 0.5f;
	const int reachX = std::max(1, static_cast<int>(std::ceil(half * 1.4143f / sx)));
	const int reachZ = std::max(1, static_cast<int>(std::ceil(half * 1.4143f / sz)));
	const int cx = static_cast<int>(std::floor((decal.position.x - ground.originX()) / sx));
	const int cz = static_cast<int>(std::floor((decal.position.z - ground.originZ()) / sz));
	std::vector<std::array<effects::Vec3, 3>> triangles;
	for (int j = cz - reachZ; j <= cz + reachZ; ++j) {
		for (int i = cx - reachX; i <= cx + reachX; ++i) {
			const float x0 = ground.originX() + static_cast<float>(i) * sx, x1 = x0 + sx;
			const float z0 = ground.originZ() + static_cast<float>(j) * sz, z1 = z0 + sz;
			const effects::Vec3 c[4] = {{x0, ground.heightAt(x0, z0), z0}, {x1, ground.heightAt(x1, z0), z0},
				{x0, ground.heightAt(x0, z1), z1}, {x1, ground.heightAt(x1, z1), z1}};
			triangles.push_back({c[0], c[1], c[2]});
			triangles.push_back({c[2], c[1], c[3]});
		}
	}
	return effects::clipDecal(decal.position, decal.normal, decal.tangent, half, decal.frame, triangles, 0, normalOffset);
}

void LevelRenderer::buildDecals()
{
	m_decalGroups.clear();
	if (!m_sources || !m_sources->effects || m_sources->effects->decals().empty()) {
		return;
	}
	std::vector<Vertex> vertices;
	// The objects drawn this frame (the ObjectDrawer marks their decal sets,
	// +0x19: Decal::ownerDrawn).
	std::vector<uint32_t> drawn;
	for (const effects::Decal& decal : m_sources->effects->decals()) {
		if (decal.object != 0) {
			for (const DrawItem& item : m_drawList) {
				if (item.instance->object != 0) {
					drawn.push_back(item.instance->object);
				}
			}
			std::sort(drawn.begin(), drawn.end());
			break;
		}
	}
	const scene::ParticleSystemSettings& ps = m_particleSettings;
	const terrain::Heightmap* ground = m_sources->heightmaps.primary();
	for (const effects::Decal& decal : m_sources->effects->decals()) {
		decal.ownerDrawn = decal.object == 0 || std::binary_search(drawn.begin(), drawn.end(), decal.object);
		SDL_GPUTexture* image = texture(m_sources->files, decal.texture + ".dds");
		if (!image) {
			continue;
		}
		// The normal-mapped technique (Decals.fx pass 2, RendDX9 0x10163810
		// with (1, 1) from 0x100e6a80): the draw (0x10164040) sends a set
		// whose owner object was stamped this frame (owner slot 0xb0 against
		// the renderer's frame count +0x108, slot 0x60 0x1000d0f0, bumped by
		// 0x10006b50) to the objects' list (+0xe08); those sets' decals with
		// a normal map (+0x38, 0x10163570's filter) go through technique 2
		// with their map (+0x34 to "NormalMap", +0x317c, 0x101631f0) and the
		// camera (WorldCamPos +0x3180). The world's set (no owner) draws all
		// its decals with technique 0, the normal map unused.
		SDL_GPUTexture* normalImage = nullptr;
		if (decal.object != 0 && decal.ownerDrawn && !decal.normalMap.empty()) {
			normalImage = texture(m_sources->files, decal.normalMap + ".dds");
		}
		effects::Vec3 n = decal.normal, tangent = decal.tangent, at = decal.position;
		// Below VideoSettings' effects 2 objects get no decal sets of their own
		// (DecalManager +0x322e, 0x100e6b70; set by 0x10010040).
		if (decal.object != 0 && m_sources->quality && !m_sources->quality->decalsEnabled()) {
			continue;
		}
		Mat4 place;
		const auto turn = [&place](const effects::Vec3& d) {
			return effects::Vec3 {d.x * place.at(0, 0) + d.y * place.at(1, 0) + d.z * place.at(2, 0),
				d.x * place.at(0, 1) + d.y * place.at(1, 1) + d.z * place.at(2, 1),
				d.x * place.at(0, 2) + d.y * place.at(1, 2) + d.z * place.at(2, 2)};
		};
		const auto objectPlace = [&](const effects::Vec3& p) {
			return turn(p) + effects::Vec3 {place.at(3, 0), place.at(3, 1), place.at(3, 2)};
		};
		if (decal.object != 0) {
			// On a moving object: from its space to the world's.
			const world::Object* holder = m_sources->objects.find(decal.object);
			if (!holder || holder->hidden()) {
				continue;
			}
			place = objectTransform(holder->position(), holder->rotation());
			if (decal.part >= 0) {
				// On a collisionPart child: the part's own set, drawn at the
				// part's matrix (its frame in the object's, then the object's).
				effects::Mat3 r;
				effects::Vec3 t;
				if (!m_sources->collisionPartFrame || !m_sources->collisionPartFrame(decal.object, decal.part, r, t)) {
					continue;
				}
				Mat4 part;
				for (int row = 0; row < 3; ++row) {
					part.at(row, 0) = r.rows[static_cast<size_t>(row)].x;
					part.at(row, 1) = r.rows[static_cast<size_t>(row)].y;
					part.at(row, 2) = r.rows[static_cast<size_t>(row)].z;
				}
				part.at(3, 0) = t.x;
				part.at(3, 1) = t.y;
				part.at(3, 2) = t.z;
				place = part * place;
			}
			n = turn(n);
			tangent = turn(tangent);
			at = objectPlace(at);
		}
		// Its age's fade (0x10163570: not drawn below 0x3240's 0.05).
		const float alpha = m_particleSettings.decalAlpha(decal.age);
		if (alpha < scene::ParticleSystemSettings::kDecalCullAlpha) {
			continue;
		}
		// Clipped once (effects::clipDecal, 0x10165f60) to the surface it hit,
		// in the decal's own space: a world decal on the terrain to its 3 x 3
		// cells about the hit (0x10164d70, whose triangles it doesn't turn
		// away by angle); one on a static object or a vehicle to that
		// object's collision faces (the projectile LOD's, the mesh the
		// exe's add 0x005a36d0 passes) about the hit, turned away past
		// decalGeomClipAngle as 0x101655c0 does. With nothing left the add
		// frees the decal (0x10166735 -> 0x10164660): not drawn. With
		// decalGeomClipAngle 0, the flat square (0x10164940).
		if (ps.decalGeomClipAngle > 0 && !decal.clipped) {
			std::vector<effects::DecalVertex> pieces;
			if (decal.object == 0 && decal.surface == 0 && ground && ground->loaded() &&
				ground->covers(decal.position.x, decal.position.z)) {
				pieces = clipDecalToTerrain(*ground, decal, ps.decalNormalOffset);
			} else if ((decal.object != 0 || decal.surface != 0) && m_sources->collisionFaces) {
				std::vector<std::array<effects::Vec3, 3>> faces;
				m_sources->collisionFaces(decal.object, decal.object != 0 ? 0 : decal.surface, decal.part, decal.position,
					decal.size * 0.5f * 1.4143f, faces);
				pieces = effects::clipDecal(decal.position, decal.normal, decal.tangent, decal.size * 0.5f, decal.frame,
					faces, ps.decalGeomClipAngle, ps.decalNormalOffset);
			}
			decal.clipped = std::make_shared<const std::vector<effects::DecalVertex>>(std::move(pieces));
		}
		if (decal.clipped) {
			if (decal.clipped->empty()) {
				continue;
			}
			const uint32_t first = static_cast<uint32_t>(vertices.size());
			for (const effects::DecalVertex& p : *decal.clipped) {
				effects::Vec3 position = p.position;
				if (decal.object != 0) {
					position = objectPlace(position);
				}
				Vertex vertex {};
				vertex.position[0] = position.x;
				vertex.position[1] = position.y;
				vertex.position[2] = position.z;
				vertex.normal[0] = n.x;
				vertex.normal[1] = n.y;
				vertex.normal[2] = n.z;
				vertex.texCoord[0] = p.u;
				vertex.texCoord[1] = p.v;
				vertex.lightmapCoord[0] = decal.color.x;
				vertex.lightmapCoord[1] = decal.color.y;
				vertex.part = decal.color.z;
				vertex.weight = alpha;
				vertex.tangent[0] = tangent.x;
				vertex.tangent[1] = tangent.y;
				vertex.tangent[2] = tangent.z;
				vertex.tangent[3] = normalImage ? 1.0f : 0.0f;
				vertices.push_back(vertex);
			}
			const uint32_t count = static_cast<uint32_t>(vertices.size()) - first;
			if (!m_decalGroups.empty() && m_decalGroups.back().texture == image && m_decalGroups.back().normalMap == normalImage) {
				m_decalGroups.back().vertexCount += count;
			} else {
				m_decalGroups.push_back({image, false, first, count, false, normalImage});
			}
			continue;
		}
		const effects::Vec3 u = tangent * (decal.size * 0.5f);
		const effects::Vec3 v = physics::cross(n, tangent) * (decal.size * 0.5f);
		const effects::Vec3 centre = at + n * m_particleSettings.decalNormalOffset;
		const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
		const uint32_t first = static_cast<uint32_t>(vertices.size());
		for (const auto& c : corners) {
			const effects::Vec3 p = centre + u * c[0] + v * c[1];
			Vertex vertex {};
			vertex.position[0] = p.x;
			vertex.position[1] = p.y;
			vertex.position[2] = p.z;
			vertex.normal[0] = n.x;
			vertex.normal[1] = n.y;
			vertex.normal[2] = n.z;
			vertex.texCoord[0] = c[0] < 0 ? decal.frame[0] : decal.frame[2];
			vertex.texCoord[1] = c[1] < 0 ? decal.frame[3] : decal.frame[1];
			vertex.lightmapCoord[0] = decal.color.x;
			vertex.lightmapCoord[1] = decal.color.y;
			vertex.part = decal.color.z;
			vertex.weight = alpha;
			vertex.tangent[0] = tangent.x;
			vertex.tangent[1] = tangent.y;
			vertex.tangent[2] = tangent.z;
			vertex.tangent[3] = normalImage ? 1.0f : 0.0f;
			vertices.push_back(vertex);
		}
		if (!m_decalGroups.empty() && m_decalGroups.back().texture == image && m_decalGroups.back().normalMap == normalImage) {
			m_decalGroups.back().vertexCount += 6;
		} else {
			m_decalGroups.push_back({image, false, first, 6, false, normalImage});
		}
	}
	if (vertices.empty()) {
		m_decalGroups.clear();
		return;
	}
	m_gpu.uploadDynamic(m_decalVertices, SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
		static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
	m_gpu.flushUploads();
}

// Each mesh particle draws its system's mesh, scaled, turned and placed,
// its texture times the particle's colour and alpha: blended and lit as a
// blended mesh material, or added (MeshParticleMesh.fx's Diffuse and
// Additive techniques).
void LevelRenderer::drawMeshParticles(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass,
	const Mat4& viewProjection, const scene::LightSettings& lights,
	std::initializer_list<const effects::EffectManager*> managers)
{
	if (!m_sources) {
		return;
	}
	const float seconds = static_cast<float>(SDL_GetTicks() - m_startTicks) / 1000.0f;
	SDL_GPUGraphicsPipeline* boundPipeline = nullptr;
	for (const effects::EffectManager* manager : managers) {
		if (!manager) {
			continue;
		}
		manager->forEachMeshGroup([&](const std::string& geometry, bool additive,
									  const std::vector<effects::MeshSprite>& particles) {
			const GpuMesh* particleMesh = mesh(*m_sources, geometry);
			if (!particleMesh) {
				return;
			}
			SDL_GPUGraphicsPipeline* pipeline = additive ? m_gpu.additiveMeshPipeline() : m_gpu.fadedMeshPipeline();
			if (pipeline != boundPipeline) {
				SDL_BindGPUGraphicsPipeline(pass, pipeline);
				useShadows(commands, pass, 1, 5, true);
				boundPipeline = pipeline;
			}
			SDL_GPUBufferBinding vertexBinding {particleMesh->vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
			SDL_GPUBufferBinding indexBinding {particleMesh->indices, 0};
			SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
			LightingUniforms base = lighting(m_viewFog, lights, lights.sunColor, lights.skyColor, 0, false);
			applyPointLights(base);
			for (const effects::MeshSprite& particle : particles) {
				Mat4 world;
				for (int row = 0; row < 3; ++row) {
					const effects::Vec3& r = particle.rotation.rows[static_cast<size_t>(row)];
					const float scale = particle.size * (row == 2 ? particle.stretch : 1.0f);
					world.at(row, 0) = r.x * scale;
					world.at(row, 1) = r.y * scale;
					world.at(row, 2) = r.z * scale;
				}
				world.at(3, 0) = particle.position.x;
				world.at(3, 1) = particle.position.y;
				world.at(3, 2) = particle.position.z;
				TransformUniforms transforms {world * viewProjection, world, {1, 1, 0, 0}, {}};
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
				for (const Submesh& submesh : particleMesh->submeshes) {
					LightingUniforms uniforms = base;
					uniforms.material[2] = additive ? kMaterialAdditive : kMaterialBlended;
					uniforms.material[3] = std::max(particle.color[3], 1e-3f);
					uniforms.uvOffset[0] = scrollOffset(submesh.uvScroll[0], seconds);
					uniforms.uvOffset[1] = scrollOffset(submesh.uvScroll[1], seconds);
					std::copy(particle.color.begin(), particle.color.begin() + 3, uniforms.tint);
					SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof(uniforms));
					SDL_GPUTextureSamplerBinding samplers[5] = {
						{submesh.texture, m_gpu.repeatSampler()},
						{m_gpu.whiteTexture(), m_gpu.clampSampler()},
						{m_gpu.whiteTexture(), m_gpu.repeatSampler()},
						{m_gpu.whiteTexture(), m_gpu.repeatSampler()},
						{m_gpu.whiteTexture(), m_gpu.repeatSampler()},
					};
					SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
					bindMeshLayers(pass, nullptr);
					SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
				}
			}
		});
	}
}

void LevelRenderer::drawParticles(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection,
	const Camera& camera, const scene::LightSettings& lights, SDL_GPUBuffer* buffer,
	const std::vector<ParticleGroup>& groups)
{
	if (!buffer || groups.empty()) {
		return;
	}
	const LightingUniforms fog = lighting(m_viewFog, lights, lights.sunColor, lights.skyColor, 0, false);
	ParticleUniforms uniforms {};
	uniforms.viewProjection = viewProjection;
	const Vec3f right = camera.right();
	const Vec3f up = cross(camera.forward(), right) * -1.0f;
	uniforms.cameraRight[0] = right.x;
	uniforms.cameraRight[1] = right.y;
	uniforms.cameraRight[2] = right.z;
	uniforms.cameraUp[0] = up.x;
	uniforms.cameraUp[1] = up.y;
	uniforms.cameraUp[2] = up.z;
	SDL_GPUBufferBinding vertexBinding {buffer, 0};
	bool bound = false, additive = false;
	for (const ParticleGroup& group : groups) {
		if (!bound || group.additive != additive) {
			SDL_BindGPUGraphicsPipeline(pass, m_gpu.particlePipeline(group.additive));
			SDL_PushGPUVertexUniformData(commands, 0, &uniforms, sizeof(uniforms));
			SDL_PushGPUFragmentUniformData(commands, 0, &fog, sizeof(fog));
			SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
			bound = true;
			additive = group.additive;
		}
		SDL_GPUTextureSamplerBinding sampler {group.texture, group.repeat ? m_gpu.repeatSampler() : m_gpu.clampSampler()};
		SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
		SDL_DrawGPUPrimitives(pass, group.vertexCount, 1, group.firstVertex, 0);
	}
}

// Lightmaps are named after the geometry, the LOD and the position
// truncated to whole metres ("<geometry>=<lod, two digits>=<x>=<y>=<z>").
void LevelRenderer::lightmapFor(const LevelSources& sources, const std::string& geometry, int lod, const Mat4& world,
	SDL_GPUTexture*& lightmap, float transform[4])
{
	lightmap = nullptr;
	for (int axis = 0; axis < 3; ++axis) {
		if (!(std::fabs(world.at(3, axis)) < 1e6f)) {
			return; // somewhere no lightmap is
		}
	}
	char lodText[8];
	std::snprintf(lodText, sizeof(lodText), "=%02d=", lod);
	std::ostringstream key;
	key << lower(geometry) << lodText << static_cast<int>(world.at(3, 0)) << '=' << static_cast<int>(world.at(3, 1))
		<< '=' << static_cast<int>(world.at(3, 2));
	const auto entry = m_lightmapAtlas.find(key.str());
	lightmap = nullptr;
	if (entry == m_lightmapAtlas.end()) {
		return;
	}
	lightmap = texture(sources.files, entry->second.atlas);
	if (lightmap) {
		transform[0] = entry->second.size[0];
		transform[1] = entry->second.size[1];
		transform[2] = entry->second.offset[0];
		transform[3] = entry->second.offset[1];
	}
}

// An instance's sphere in the world: its parts' spheres where they are now.
BoundingSphere LevelRenderer::instanceBounds(const Instance& instance, const GpuMesh& mesh) const
{
	if (mesh.skinned || mesh.bounds.radius < 0) {
		return {}; // unknown: soldiers are posed by their skeletons
	}
	if (!mesh.bundled || instance.parts.empty()) {
		return transformSphere(mesh.bounds, instance.world);
	}
	// The parts' spheres, each moved by its part and the object, in one box.
	bool any = false;
	Vec3f lo, hi;
	for (size_t part = 0; part < mesh.partBounds.size() && part < instance.parts.size(); ++part) {
		if (mesh.partBounds[part].radius < 0) {
			continue;
		}
		const BoundingSphere b = transformSphere(mesh.partBounds[part], instance.parts[part] * instance.world);
		const Vec3f r {b.radius, b.radius, b.radius};
		lo = any ? Vec3f {std::fmin(lo.x, b.center.x - b.radius), std::fmin(lo.y, b.center.y - b.radius),
					   std::fmin(lo.z, b.center.z - b.radius)}
				 : b.center - r;
		hi = any ? Vec3f {std::fmax(hi.x, b.center.x + b.radius), std::fmax(hi.y, b.center.y + b.radius),
					   std::fmax(hi.z, b.center.z + b.radius)}
				 : b.center + r;
		any = true;
	}
	if (!any) {
		return transformSphere(mesh.bounds, instance.world);
	}
	BoundingSphere all;
	all.center = (lo + hi) * 0.5f;
	const Vec3f half = (hi - lo) * 0.5f;
	all.radius = std::sqrt(dot(half, half));
	return all;
}

// The instances the world pass draws: those in the view, each with the LOD
// its distance asks for (the exterior's; a cockpit keeps its own).
void LevelRenderer::prepareInstances(const Camera& camera, const Mat4& viewProjection)
{
	m_drawList.clear();
	m_cloakedList.clear();
	m_roadStatics.clear();
	m_culledInstances = 0;
	const Frustum frustum = Frustum::fromViewProjection(viewProjection);
	m_frameFrustum = frustum; // the lights' list (collectPointLights)
	bool uploaded = false;
	const scene::RendererSettings rendererDefaults;
	const scene::RendererSettings& renderer =
		m_sources && m_sources->renderer ? *m_sources->renderer : rendererDefaults;
	const float zoom = viewZoom(camera);
	const float closeOvergrowth = overgrowthCloseDistance() * zoom;
	// lodTimer's seconds, and the camera's height over the ground (the
	// cells' near fade).
	const uint64_t overgrowthNow = SDL_GetTicks();
	const float overgrowthSeconds =
		m_lastOvergrowthTicks ? std::min(static_cast<float>(overgrowthNow - m_lastOvergrowthTicks) / 1000.0f, 0.1f) : 0.0f;
	m_lastOvergrowthTicks = overgrowthNow;
	const terrain::Heightmap* groundMap = m_sources ? m_sources->heightmaps.primary() : nullptr;
	const float cameraAboveGround = groundMap && groundMap->loaded()
		? camera.position.y - groundMap->heightAt(camera.position.x, camera.position.z)
		: 100.0f;
	static const bool cloakAll = [] { // BF2142_POST_EFFECT=camo: every soldier cloaked (screenshots)
		const char* forced = std::getenv("BF2142_POST_EFFECT");
		return forced && std::string(forced) == "camo";
	}();
	// The Titans' parts (TitanParts.h).
	m_cameraInTitan = cameraInTitan(camera);
	if (m_sources && m_sources->cameraInTitanOut) {
		*m_sources->cameraInTitanOut = m_cameraInTitan;
	}
	const TitanView titanView {m_cameraInTitan, renderer.showAllTitanParts};
	const uint32_t titan1 = m_sources && m_sources->titanObject ? m_sources->titanObject(1) : 0;
	const uint32_t titan2 = m_sources && m_sources->titanObject ? m_sources->titanObject(2) : 0;
	const int lightingQuality = m_sources && m_sources->quality ? m_sources->quality->lighting : 3;
	// The object manager's distance fade (DistanceFade.h): the geometry
	// quality's scales, the view distance GSViewDistance x the view
	// distance scale, the frame's seconds.
	const ObjectCullScales cullScales = objectCullScales(m_sources && m_sources->quality ? m_sources->quality->geometry : 3);
	const float viewFar = viewFarDistance(m_sources && m_sources->viewDistance ? m_sources->viewDistance() : 500.0f, m_viewDistanceScale);
	const CullRecord cullRoot = cullRecord(cullScales, zoom, viewFar, false, overgrowthSeconds);
	const CullRecord cullActor = cullRecord(cullScales, zoom, viewFar, true, overgrowthSeconds);
	for (Instance& instance : m_instances) {
		const bool forceCloak = cloakAll && instance.mesh->skinned && !instance.hidden;
		if ((instance.hidden || forceCloak) && instance.object != 0 && m_sources &&
			(instance.mesh->skinned || instance.mesh->bundled) &&
			(forceCloak || (m_sources->cloaked && m_sources->cloaked(instance.object)))) {
			// Active camouflage: hidden from the world pass, drawn into the
			// camouflage mask (the first person's own soldier stays out).
			if (!(m_sources->hiddenInView && m_sources->hiddenInView(instance.object))) {
				m_cloakedList.push_back({&instance, instance.mesh, nullptr, instance.lightmapTransform});
			}
			continue;
		}
		if (instance.hidden || instance.fade <= 0.0f) {
			continue;
		}
		const GpuMesh& base = *instance.mesh;
		// A road template's mesh is the roads' (0x100bdb00: drawn with
		// drawRoads, whatever drawStaticMeshes says; RoadStatics.h).
		if (base.road) {
			if (renderer.drawRoads) {
				const BoundingSphere sphere = instanceBounds(instance, base);
				if (frustum.containsSphere(sphere.center, sphere.radius)) {
					m_roadStatics.push_back(&instance);
				} else {
					++m_culledInstances;
				}
			}
			continue;
		}
		// renderer.drawObjects / drawBundledMeshes / drawStaticMeshes /
		// drawSkinnedMeshes / drawOvergrowth / drawTrees (RendererSettings).
		if (!renderer.drawObjects ||
			!(base.skinned       ? renderer.drawSkinnedMeshes
				: base.bundled   ? renderer.drawBundledMeshes
				: instance.overgrowth ? renderer.drawOvergrowth && renderer.drawStaticMeshes
				: base.tree      ? renderer.drawTrees && renderer.drawStaticMeshes
								 : renderer.drawStaticMeshes)) {
			continue;
		}
		bool interior = false;
		// Projectiles are world-space transient models, like their tracer
		// and trail effects. The Titan's indoor actor cutoff must not hide
		// the missile body while leaving its trail visible through openings.
		// Ordinary frustum/depth tests still apply to these models.
		// The native inside test is a union of room bounding boxes, not the
		// retail camera's Titan membership and portal visibility. Applying the
		// retail static-mesh switch globally makes the entire outside world,
		// unmarked Titan details, or rooms disappear at a box boundary. Keep
		// static geometry available on both sides and let depth/frustum tests
		// resolve visibility. Bundled and skinned meshes retain distance culling.
		if (base.titanInterior || base.titanExterior || titanView.cameraInside) {
			const Vec3f origin {instance.world.at(3, 0), instance.world.at(3, 1), instance.world.at(3, 2)};
			const Vec3f d = origin - camera.position;
			const float distance = std::sqrt(dot(d, d));
			// The authored Titan trees contain bundled signs and monitors marked
			// exterior, including parts visible from their rooms. With our coarse
			// room-box test that flag cannot safely hide children of either Titan.
			// Keep their distance, frustum, depth and part-destruction checks.
			const bool titanChild = instance.object != 0 && !instance.source.empty() &&
				(instance.object == titan1 || instance.object == titan2);
			const TitanDraw how = base.skinned
				? titanSkinnedDraw(titanView, distance, renderer.indoorSkinnedMeshCullDistance)
				: base.bundled ? titanBundledDraw(titanView, base.titanExterior && !titanChild, distance, renderer.indoorBundledMeshCullDistance)
							   : TitanDraw::Normal;
			if (how == TitanDraw::Skip && !instance.placed) {
				continue;
			}
			interior = how == TitanDraw::Interior && base.bundled;
		}
		const BoundingSphere sphere = instanceBounds(instance, *instance.mesh);
		if (!frustum.containsSphere(sphere.center, sphere.radius)) {
			++m_culledInstances;
			continue;
		}
		DrawItem item {&instance, instance.mesh, instance.lightmap, instance.lightmapTransform, interior};
		if (m_distanceFadeOn && instance.object != 0 && !instance.overgrowth && !instance.debris) {
			// 0x006fb320 on the objects in view; one faded out isn't drawn.
			// A child instance uses its own mesh radius, so its distance must
			// also be measured from the child's world position. Using the root
			// puts a Titan's small signs roughly 70 m away even when the camera
			// is beside them and can fade them out with the camera or FOV.
			const Vec3f fadeOrigin = instance.source.empty() ? instance.objectOrigin
				: Vec3f {instance.world.at(3, 0), instance.world.at(3, 1), instance.world.at(3, 2)};
			const Vec3f d = fadeOrigin - camera.position;
			instance.distanceFade = stepDistanceFade(instance.distanceFade, instance.cullRadius, dot(d, d),
				instance.cullActor ? cullActor : cullRoot);
			if (instance.distanceFade <= 0.0f) {
				continue;
			}
			if (instance.distanceFade < 1.0f) {
				item.fade = instance.fade * instance.distanceFade;
			}
		}
		if (instance.overgrowth && m_sources) {
			// OvergrowthLod.h: the near pass's cross-fade within R, the
			// cells' fades for the "_lod" meshes beyond.
			const Vec3f origin {instance.world.at(3, 0), instance.world.at(3, 1), instance.world.at(3, 2)};
			const Vec3f d = (sphere.radius >= 0 ? sphere.center : origin) - camera.position;
			const float distance = std::sqrt(dot(d, d));
			const float nearRadius = closeOvergrowth * instance.overgrowthTypeFactor + std::max(sphere.radius, 0.0f);
			const auto lodMesh = [&]() -> const GpuMesh* {
				Instance::Lod& entry = instance.overgrowthLod;
				if (!entry.resolved) {
					entry.resolved = true;
					if (m_sources->geometries.find(instance.geometry + "_lod")) {
						entry.mesh = mesh(*m_sources, instance.geometry + "_lod", -1, 0, true);
					}
					uploaded = true;
				}
				return entry.mesh;
			};
			if (distance >= nearRadius) {
				// In its cell's batch: kept while the cell's nearest corner is
				// within viewDistance, faded over its last 30% and near the
				// camera (0x10120ef0, 0x10121410).
				const float size = m_overgrowthCellSize;
				const float* cell = instance.overgrowthCell;
				float nearest = std::numeric_limits<float>::max();
				for (int corner = 0; corner < 4; ++corner) {
					const Vec3f at {cell[0] + (corner & 1 ? size : 0.0f), cell[1], cell[2] + (corner & 2 ? size : 0.0f)};
					const Vec3f e = at - camera.position;
					nearest = std::min(nearest, dot(e, e));
				}
				const float kept = OvergrowthCells::distanceFade(std::sqrt(nearest), m_overgrowthViewDistance);
				if (kept < 0) {
					continue;
				}
				const Vec3f centre = Vec3f {cell[0] + size * 0.5f, cell[1], cell[2] + size * 0.5f} - camera.position;
				const float fade = kept * OvergrowthCells::nearFade(std::sqrt(dot(centre, centre)), cameraAboveGround);
				if (const GpuMesh* lod = lodMesh()) {
					item.mesh = lod;
					item.lightmap = nullptr;
					item.fade = instance.fade * fade;
					m_drawList.push_back(item);
				}
				continue;
			}
			float w = OvergrowthBlend::weight(distance, nearRadius, instance.overgrowthTypeFactor, m_overgrowthCloseDistFade);
			w = OvergrowthBlend::eased(w, instance.overgrowthLodState, overgrowthSeconds, m_overgrowthLodTimer);
			// Antialiased both alphas are doubled (0x1008da30 with 0x1000b500
			// and +0x50a): the leaves' coverage, and the trunks opaque sooner.
			const bool coverage = alphaToCoverage();
			if (w < 1) {
				if (const GpuMesh* lod = lodMesh()) {
					DrawItem far = item;
					far.mesh = lod;
					far.lightmap = nullptr;
					far.fade = instance.fade * AlphaToCoverage::nearAlpha(OvergrowthBlend::lodAlpha(w), coverage);
					m_drawList.push_back(far);
				}
			}
			if (w <= 0) {
				continue;
			}
			if (w < 1 || coverage) {
				item.fade = instance.fade * AlphaToCoverage::nearAlpha(OvergrowthBlend::fullAlpha(w), coverage);
			}
		}
		const bool noLods = renderer.staticNoLods && !base.skinned && !base.bundled;
		if (m_sources && !noLods && instance.shownGeometry < 0 && base.lodCount > 1 && !instance.geometry.empty()) {
			const Vec3f origin {instance.world.at(3, 0), instance.world.at(3, 1), instance.world.at(3, 2)};
			const Vec3f toCamera = (sphere.radius >= 0 ? sphere.center : origin) - camera.position;
			const float distance = lodDistance(std::sqrt(dot(toCamera, toCamera)), zoom, sphere.radius);
			// The LODs left after the skipped ones switch as a mesh of that many.
			const int kept = static_cast<int>(base.lodCount) - base.skippedLods;
			const std::vector<float> distances = lodSwitchDistances(base.lodDistances, kept);
			// renderer +0x974: VideoSettings' geometry quality, 0.5 / 0.8 / 1 (0x10010040).
			const float quality = m_sources->quality ? m_sources->quality->geometryLodFactor() : 1.0f;
			const float scale = quality * (base.skinned ? renderer.skinnedMeshLodDistanceScale
						: base.bundled               ? renderer.bundleMeshLodDistanceScale
													 : renderer.staticMeshLodDistanceScale);
			int lod = base.skippedLods + lodForDistance(distance, distances, kept, scale);
			// A Titan hull part draws at its pair's LOD (0x100bdb00).
			if (base.titanExterior && !base.bundled && !base.skinned) {
				lod = std::min(m_titanExteriorLods.resolve(base.titanPart, lod, static_cast<int>(base.lodCount)),
					static_cast<int>(base.lodCount) - 1);
			}
			if (lod > 0) {
				instance.lods.resize(base.lodCount);
				Instance::Lod& entry = instance.lods[static_cast<size_t>(lod)];
				if (!entry.resolved) {
					entry.resolved = true;
					entry.mesh = mesh(*m_sources, instance.geometry, -1, lod);
					if (instance.lightmap) {
						lightmapFor(*m_sources, instance.geometry, lod, instance.world, entry.lightmap, entry.lightmapTransform);
					}
					uploaded = true;
				}
				if (entry.mesh) {
					// A LOD without its own lightmap is lit as an unlightmapped mesh.
					item.mesh = entry.mesh;
					item.lightmap = entry.lightmap;
					item.lightmapTransform = entry.lightmapTransform;
				}
			}
		}
		// The static template's "use lightmaps" (0x100df010, asked by the
		// static mesh's draw 0x100bdb00): at the low lighting quality (the
		// settings' lighting, +0x38, under 2) a static mesh draws without
		// its lightmap unless it is a Titan's part or under objects/water/.
		if (item.lightmap && !item.mesh->bundled && !item.mesh->skinned && !instance.overgrowth &&
			!staticKeepsLightmap(item.mesh->titanInterior || item.mesh->titanExterior, item.mesh->waterObject, lightingQuality)) {
			item.lightmap = nullptr;
		}
		m_drawList.push_back(item);
	}
	m_titanExteriorLods.endFrame();
	if (uploaded) {
		m_gpu.flushUploads();
	}
}

bool LevelRenderer::cameraInTitan(const Camera& camera) const
{
	if (m_sources && m_sources->renderer && m_sources->renderer->insideTitanOverride) {
		return true; // ObjectDrawer.isInsideTitanOverride (+0x91)
	}
	// The exe tells RendDX9 the Titan the camera is in (game slot 0x110:
	// the player's soldier's Titan, else a ray from the camera up through
	// the Titan's hull, 0x006fa8b0 / 0x006f2b00). The rewrite takes the
	// camera inside a Titan when it is within the box of one of its rooms
	// (a static mesh marked isTitanInterior).
	for (const Instance& instance : m_instances) {
		const GpuMesh& mesh = *instance.mesh;
		if (!mesh.titanInterior || mesh.bundled || mesh.skinned || instance.hidden) {
			continue;
		}
		const Vec3f d = camera.position - Vec3f {instance.world.at(3, 0), instance.world.at(3, 1), instance.world.at(3, 2)};
		bool inside = true;
		for (int axis = 0; axis < 3 && inside; ++axis) {
			// The rows are the rotation's axes (no scale on placed objects).
			const float along = d.x * instance.world.at(axis, 0) + d.y * instance.world.at(axis, 1) + d.z * instance.world.at(axis, 2);
			const float lo = axis == 0 ? mesh.boxMin.x : axis == 1 ? mesh.boxMin.y : mesh.boxMin.z;
			const float hi = axis == 0 ? mesh.boxMax.x : axis == 1 ? mesh.boxMax.y : mesh.boxMax.z;
			inside = along >= lo && along <= hi;
		}
		if (inside) {
			return true;
		}
	}
	return false;
}

void LevelRenderer::loadEnvMaps(const LevelSources& sources)
{
	// EnvMaps.h: the .emi's maps and their cubes.
	m_envMaps.clear();
	// The Titan interiors' cube (0x10063900, loaded with the manager).
	m_indoorEnvMap = cubeTexture(sources.files, "common/textures/indoorenvmap.dds");
	std::vector<uint8_t> data;
	if (!sources.files.readFile(sources.levelPath + "Envmaps/EnvironmentMapInfo.emi", data)) {
		return;
	}
	for (const EnvMapEntry& entry : parseEnvMapInfo(std::string(data.begin(), data.end()))) {
		if (SDL_GPUTexture* cube = cubeTexture(sources.files, sources.levelPath + "Envmaps/" + entry.name + ".dds")) {
			m_envMaps.push_back({entry, cube});
		}
	}
}

void LevelRenderer::build(const LevelSources& sources)
{
	m_startTicks = SDL_GetTicks();
	m_sources = &sources; // the quality levels the loaders read
	m_placedParts.clear();
	m_hadPlaced = false;
	// texturemanager.customTextureSuffix as the level's Init.con left it: the
	// original swaps the textures at the end of the load (0x1000deb0 ->
	// 0x10018ca0); the rewrite loads the variants as it goes.
	m_textureSuffix = world::customTextureSuffix();
	// Antialiased at the load: the trees and the undergrowth keep their
	// plain textures for alpha to coverage (0x10151830, 0x1008c7e0,
	// 0x101100d0); the frame's sample count follows the setting.
	m_coverageTextures = sources.quality && sources.quality->antialiasingSamples > 1 &&
		m_gpu.setWorldSamples(static_cast<uint32_t>(sources.quality->antialiasingSamples)) > 1;
	const auto progress = [&sources](float done) {
		if (sources.buildProgress) {
			sources.buildProgress(done);
		}
	};
	loadLightmapAtlas(sources);
	loadOvergrowth(sources);
	buildTerrain(sources);
	progress(0.2f);
	buildSky(sources);
	buildSkyLayers(sources);
	loadHemiMap(sources);
	loadSimpleShadowmap(sources);
	loadTitanHemiMaps(sources);
	loadPostTextures(sources);
	buildWater(sources);
	loadEnvMaps(sources);
	buildRoads(sources);
	progress(0.3f);
	buildUndergrowth(sources);
	progress(0.4f);
	Vec3f sum;
	size_t placed = 0;
	const size_t objectCount = sources.objects.objects().size();
	size_t done = 0;
	for (const auto& object : sources.objects.objects()) {
		if (++done % 256 == 0) {
			progress(0.4f + 0.6f * static_cast<float>(done) / static_cast<float>(objectCount));
		}
		const size_t before = m_instances.size();
		addObject(sources, *object);
		const Vec3f at {object->position().x, object->position().y, object->position().z};
		if (m_instances.size() > before && std::fabs(at.x) < 1e5f && std::fabs(at.y) < 1e5f && std::fabs(at.z) < 1e5f) {
			sum = sum + at;
			++placed;
		}
	}
	m_center = placed ? sum * (1.0f / placed) : Vec3f {};
	m_stats.instances = m_instances.size();
	m_gpu.flushUploads();
	progress(1);
}

Camera LevelRenderer::suggestedCamera() const
{
	Camera camera;
	camera.position = m_center + Vec3f {-120, 80, -120};
	camera.yaw = 0.785f;
	camera.pitch = -0.4f;
	return camera;
}

Camera LevelRenderer::overheadCamera(const OverheadView& view) const
{
	const terrain::Heightmap* heightmap = m_sources ? m_sources->heightmaps.primary() : nullptr;
	float xs[5], zs[5], heights[5] = {0, 0, 0, 0, 0};
	overheadGroundPoints(view, m_patchOrigin, m_terrainExtent, xs, zs);
	for (int i = 0; heightmap && i < 5; ++i) {
		heights[i] = heightmap->heightAt(xs[i], zs[i]);
	}
	Camera camera;
	camera.position = overheadCameraPosition(view, m_patchOrigin, m_terrainExtent, heights, m_terrainWater[3]);
	camera.yaw = 0;
	// Straight down (the exe's rows 1 0 0 / 0 0 1 / 0 -1 0): screen right
	// +x, up +z; a hair off vertical for lookAt's up vector.
	camera.pitch = -1.5697963f;
	camera.fovY = view.fovY;
	camera.nearPlane = 1.0f;
	return camera;
}

void LevelRenderer::loadTitanHemiMaps(const LevelSources& sources)
{
	m_titanHemiMaps.clear();
	if (!sources.hemi) {
		return;
	}
	for (const auto& titan : sources.hemi->titans) {
		if (titan.path.empty() || titan.size <= 0) {
			continue;
		}
		// 0x100ed200: a missing file is an 8 x 8 white map.
		TitanHemiMap map;
		std::vector<uint8_t> data;
		const auto image = sources.files.readFile(titan.path + ".dds", data) ? texture::parseDds(data) : std::nullopt;
		if (image && image->format == texture::TextureFormat::RGBA8 && !image->faces.empty() && !image->faces[0].empty()) {
			map.width = image->width;
			map.height = image->height;
			map.rgba = image->faces[0][0].data;
		} else {
			map.width = map.height = 8;
			map.rgba.assign(8 * 8 * 4, 0xff);
		}
		std::copy(titan.position.begin(), titan.position.end(), map.offset);
		map.size = titan.size;
		map.team = titan.team == 1 ? 1 : 2; // its last argument: 1 the PAC's Titan, 0 the EU's
		m_titanHemiMaps.push_back(std::move(map));
	}
}

void LevelRenderer::placeTitanHemiMaps(const Camera& /*camera*/)
{
	for (TitanHemiMap& map : m_titanHemiMaps) {
		map.placed = false;
		const uint32_t id = m_sources && m_sources->titanObject ? m_sources->titanObject(map.team) : 0;
		const world::Object* titan = id ? m_sources->objects.find(id) : nullptr;
		if (!titan) {
			continue;
		}
		const Mat4 world = objectTransform(titan->position(), titan->rotation());
		map.origin = {world.at(3, 0), world.at(3, 1), world.at(3, 2)};
		for (int i = 0; i < 3; ++i) {
			map.axes[i] = {world.at(i, 0), world.at(i, 1), world.at(i, 2)};
		}
		map.placed = true;
	}
}

void LevelRenderer::hemiObjectFor(const Instance& instance, const GpuMesh& mesh, float out[4]) const
{
	const Vec3f origin {instance.world.at(3, 0), instance.world.at(3, 1), instance.world.at(3, 2)};
	// HeightOverTerrain (BM 0x100c2b70, SM 0x10160e90): the height over
	// what a ray 4 radii down meets, less a radius, in radii (at least
	// 2 m); the rewrite's ray meets only the terrain.
	const terrain::Heightmap* heightmap = m_sources ? m_sources->heightmaps.primary() : nullptr;
	out[3] = heightOverTerrain(heightmap ? origin.y - heightmap->heightAt(origin.x, origin.z) : -1.0f, mesh.bounds.radius);
	// HemiMapConstantColor: 1 with the hemi technique outside the Titans;
	// with the camera in one (the interior path 0x100c3880 / 0x10162070),
	// the Titan map the object's origin is over (the nearest above its
	// floor, 0x100edce0), sampled bilinearly 1 texel a metre (0x100ecb10),
	// else the object's last one. HeightOverTerrain is 0 on that path.
	out[0] = out[1] = out[2] = 1;
	if (!m_cameraInTitan) {
		return;
	}
	out[3] = 0;
	const TitanHemiMap* best = nullptr;
	Vec3f bestLocal;
	for (const TitanHemiMap& map : m_titanHemiMaps) {
		if (!map.placed) {
			continue;
		}
		const Vec3f local = titanMapLocal(map.origin, map.axes, map.offset, origin);
		if (overTitanMap(local, map.size) && (!best || local.y < bestLocal.y)) {
			best = &map;
			bestLocal = local;
		}
	}
	if (best && best->width >= 2 && best->height >= 2) {
		titanHemiSample(best->rgba.data(), best->width, best->height, best->size, bestLocal, instance.titanHemi);
	}
	std::copy(instance.titanHemi, instance.titanHemi + 3, out);
}

void LevelRenderer::draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* color, GpuContext::Target& target,
	const Camera& viewCamera, const scene::LightSettings& lights)
{
	if (m_baseFovY <= 0) {
		m_baseFovY = viewCamera.fovY; // the zoom is against the view's first fov (0x10066b30)
	}
	// The commander's overhead view takes the player's (0x00779680).
	OverheadView overhead;
	const bool overheadOn = m_sources && m_sources->overheadView && m_sources->overheadView(overhead);
	// The draw switches and near planes (renderer.draw*, tweakNearPlane*).
	const scene::RendererSettings switchDefaults;
	const scene::RendererSettings& switches =
		m_sources && m_sources->renderer ? *m_sources->renderer : switchDefaults;
	Camera camera = overheadOn ? overheadCamera(overhead) : viewCamera;
	m_distanceFadeOn = !overheadOn;
	m_viewDistanceScale = frameViewDistanceScale(m_sources && m_sources->quality ? m_sources->quality->viewDistanceScale : 1.0f, overheadOn);
	if (switches.tweakNearPlaneSet && switches.tweakNearPlane > 0) {
		camera.nearPlane = switches.tweakNearPlane;
	}
	// With the post-production the world is drawn into the scene texture,
	// then post.frag into `color` with the HUD over it.
	const bool post = postEnabled() && m_gpu.postPipeline();
	if (post) {
		ensurePostTargets(target.width, target.height);
	}
	// The antialiasing: the world's passes multisampled, resolved into the
	// scene texture (or `color` without the post-production).
	uint32_t samples =
		m_gpu.setWorldSamples(m_sources && m_sources->quality ? static_cast<uint32_t>(std::max(m_sources->quality->antialiasingSamples, 1)) : 1);
	if (samples > 1) {
		ensureMultisampleTargets(target.width, target.height, samples);
		if (!m_multisampleColor || !m_multisampleDepth) {
			samples = m_gpu.setWorldSamples(1);
		}
	}
	const bool multisampled = samples > 1;
	SDL_GPUTexture* const worldColor = post && m_sceneColor ? m_sceneColor : color;
	SDL_GPUColorTargetInfo colorTarget {};
	colorTarget.texture = multisampled ? m_multisampleColor : worldColor;
	if (multisampled) {
		colorTarget.resolve_texture = worldColor;
	}
	// The fog the camera is in: the level's, or the water's below the sea (Fog.h).
	{
		const terrain::HeightmapCluster* cluster = m_sources ? &m_sources->heightmaps : nullptr;
		const scene::WaterSettings* water = m_sources ? &m_sources->water : nullptr;
		const scene::WaterSettings waterDefaults;
		const scene::WaterSettings& w = water ? *water : waterDefaults;
		const float viewScale = m_viewDistanceScale;
		m_viewFog = viewFog(lights.fogColor, lights.fogStartEndAndBase, w.fogColor, w.fogStartEndAndBase,
			cluster && cluster->hasSea(), cluster ? cluster->seaWaterLevel() : 0.0f, camera.position.y, viewScale);
	}
	// The envmap nearest the camera (EnvMaps.h, 0x10064f30).
	m_envMap = nullptr;
	{
		float best = std::numeric_limits<float>::max();
		for (const EnvMap& map : m_envMaps) {
			const Vec3f d = camera.position - Vec3f {map.entry.position[0], map.entry.position[1], map.entry.position[2]};
			if (dot(d, d) < best) {
				best = dot(d, d);
				m_envMap = map.cube;
			}
		}
	}
	colorTarget.clear_color = {m_viewFog.color[0], m_viewFog.color[1], m_viewFog.color[2], 1.0f};
	colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
	colorTarget.store_op = SDL_GPU_STOREOP_STORE;
	SDL_GPUDepthStencilTargetInfo depthTarget {};
	depthTarget.texture = multisampled ? m_multisampleDepth : target.depth;
	depthTarget.clear_depth = 1.0f;
	depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
	depthTarget.store_op = SDL_GPU_STOREOP_DONT_CARE;
	depthTarget.stencil_load_op = SDL_GPU_LOADOP_CLEAR; // the menus' masks start from 0
	depthTarget.clear_stencil = 0;
	depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	updateUndergrowth(camera);
	buildParticles(camera);
	// The mesh particles' meshes, loaded before the pass begins.
	if (m_sources) {
		for (const effects::EffectManager* manager : {m_sources->effects, m_sources->objectEffects}) {
			if (manager) {
				manager->forEachMeshGroup([&](const std::string& geometry, bool, const std::vector<effects::MeshSprite>&) {
					mesh(*m_sources, geometry);
				});
			}
		}
		m_gpu.flushUploads();
	}
	// The first-person models, loaded before the pass begins.
	m_firstPerson = FirstPersonScene {};
	m_hasFirstPerson = !overheadOn && m_sources && m_sources->firstPerson && m_sources->firstPerson(m_firstPerson) &&
		switches.draw1pMeshes && switches.drawObjects;
	if (m_hasFirstPerson && switches.tweakNearPlane1pSet && switches.tweakNearPlane1p > 0) {
		m_firstPerson.nearPlane = switches.tweakNearPlane1p;
	}
	if (m_hasFirstPerson) {
		for (const FirstPersonModel& model : m_firstPerson.models) {
			mesh(*m_sources, model.geometry, model.geometryIndex, model.lod);
		}
		m_gpu.flushUploads();
	}
	// A zoomed first person (the systems' +8, 0x0061a1c0; zoomed here: the
	// view's field of view narrower than its first, 0x10066b30) has its
	// particles drawn with the first-person meshes' view, an unzoomed one
	// with the world's (0x100990b0; see effects::ParticleTemplate::showZoomed).
	m_firstPersonZoomed = viewZoom(camera) > 1.05f;
	buildParticleBuffer({m_hasFirstPerson ? m_firstPerson.effects : nullptr}, m_firstPersonParticleVertices,
		m_firstPersonParticleGroups, camera, false, m_firstPersonZoomed);
	collectPointLights(camera);
	if (m_hasFirstPerson && m_firstPerson.effects) {
		m_firstPerson.effects->forEachMeshGroup([&](const std::string& geometry, bool, const std::vector<effects::MeshSprite>&) {
			mesh(*m_sources, geometry);
		});
		m_gpu.flushUploads();
	}
	buildOverlay();
	// The menu keeps its own clock: it plays with or without a HUD.
	const uint64_t now = SDL_GetTicks();
	const float menuSeconds = m_lastMenuTicks ? static_cast<float>(now - m_lastMenuTicks) / 1000.0f : 0.0f;
	m_lastMenuTicks = now;
	buildMenu(menuSeconds);
	const Mat4 projection = overheadOn
		? overheadProjection(overhead, static_cast<float>(target.width), static_cast<float>(target.height), camera.nearPlane,
			  6000.0f)
		: Mat4::perspective(camera.fovY, static_cast<float>(target.width) / target.height, camera.nearPlane, 6000.0f);
	// The enhanced mode's passes this frame, and TAA's jitter (Enhanced/LevelRendererEnhanced.cpp).
	beginEnhancedFrame(camera, target.width, target.height, overheadOn, post && m_sceneColor, projection);
	const Mat4 viewProjection = m_enh.jittered ? camera.view() * projection * m_enh.jitter : camera.view() * projection;
	prepareInstances(camera, viewProjection);
	// The particle systems seen this frame (RendDX9's draws stamp the frame
	// of each system whose box meets the view frustum, 0x101a4720): one
	// unseen for 60 frames stops emitting (Emitter::lastSeenFrame). The
	// first-person manager's systems ride the view, so they aren't counted.
	if (m_sources) {
		for (const effects::EffectManager* manager : {m_sources->effects, m_sources->objectEffects}) {
			if (manager) {
				manager->markViewed([this](const effects::Vec3& centre, float radius) {
					return m_frameFrustum.containsSphere({centre.x, centre.y, centre.z}, radius);
				});
			}
		}
	}
	buildDecals(); // after the draw list: it marks the sets whose owner is drawn
	placeTitanHemiMaps(camera);
	const float seconds = static_cast<float>(SDL_GetTicks() - m_startTicks) / 1000.0f;
	m_poses.clear();
	// The shadow maps, before the world pass that reads them.
	renderShadows(commands, camera, lights, seconds);
	const float aspect = static_cast<float>(target.width) / static_cast<float>(target.height);
	if (m_enh.csm) {
		renderCascades(commands, camera, lights, seconds, aspect);
	}
	// The terrain's cells this frame (before the enhanced prepass, which draws them too).
	const Frustum frustum = Frustum::fromViewProjection(viewProjection);
	const scene::RendererSettings rendererDefaults;
	const scene::RendererSettings& renderer =
		m_sources && m_sources->renderer ? *m_sources->renderer : rendererDefaults;
	bool terrainUpdated = false;
	const auto updateTerrain = [&] {
		terrainUpdated = true;
		if (!m_terrainCells.empty()) {
			updateTerrainCells(camera, frustum);
		} else {
			m_terrainLodSwitch = terrainLodSwitch(viewZoom(camera), renderer);
			const terrain::Heightmap* heightmap = m_sources ? m_sources->heightmaps.primary() : nullptr;
			m_terrainHeightAboveGround = std::fabs(camera.position.y -
				(heightmap ? heightmap->heightAt(camera.position.x, camera.position.z) : 0.0f));
		}
	};
	if (m_enh.prepass) {
		updateTerrain();
		renderEnhancedPrepass(commands, camera, viewProjection, seconds, aspect);
	}
	m_enh.aoBound = m_enh.ssao; // the world's opaque draws take the AO
	// The heat shimmer reads a copy of the scene: the world pass ends before it.
	const bool shimmer = post && m_sceneColor && !m_shimmerGroups.empty() && m_gpu.shimmerPipeline();
	if (!m_cloakedList.empty() || shimmer) {
		depthTarget.store_op = SDL_GPU_STOREOP_STORE; // the camouflage mask tests against it
		depthTarget.stencil_store_op = SDL_GPU_STOREOP_STORE;
	}
	// Multisampled, each part of the world pass resolves its colour as it
	// ends, and keeps its samples too when another part follows (the
	// shimmer's, the camouflage's or the first person's).
	const bool cloaked = post && !m_cloakedList.empty();
	int worldParts = 1 + (shimmer ? 1 : 0) + (cloaked ? 1 : 0) + (m_hasFirstPerson ? 1 : 0);
	const auto worldStore = [&]() {
		return !multisampled ? SDL_GPU_STOREOP_STORE
			: --worldParts > 0 ? SDL_GPU_STOREOP_RESOLVE_AND_STORE
							   : SDL_GPU_STOREOP_RESOLVE;
	};
	colorTarget.store_op = worldStore();
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colorTarget, 1, &depthTarget);

	// Sky dome around the camera, unlit and unfogged, behind everything.
	// Below the sea the dome is the water's fog colour (SkyDomeUnderWater,
	// 0x100fd2f0): the clear colour already is.
	if (!m_viewFog.underwaterSky && switches.drawSkyDome) {
		drawSky(commands, pass, viewProjection, camera);
	}

	// Terrain.
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.terrainPipeline());
	useShadows(commands, pass, 2, 5 + kMaxDetailLayers, false);
	// The terrain's sun and GI colors come from terraindata.raw; the
	// script values are the editor's (the game doesn't have them).
	const auto& terrainSun = m_terrainColorsFromData ? m_terrainSunColor : lights.terrainSunColor;
	const auto& terrainGI = m_terrainColorsFromData ? m_terrainGIColor : lights.terrainGIColor;
	LightingUniforms terrainLit = lighting(m_viewFog, lights, terrainSun, terrainGI, 0, true);
	LightingUniforms terrainUnlit = lighting(m_viewFog, lights, terrainSun, terrainGI, 0, false);
	applyTerrainLights(terrainLit);
	applyTerrainLights(terrainUnlit);
	TransformUniforms transforms {viewProjection, Mat4 {}, {1, 1, 0, 0}, {}};
	transforms.partsUsed = 0; // terrain.vert has none
	SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
	TerrainDetailUniforms detail {};
	for (int i = 0; i < kMaxDetailLayers; ++i) {
		detail.tiling[i] = m_detailTiling[i];
		std::copy(std::begin(m_detailLayerInfo[i]), std::end(m_detailLayerInfo[i]), detail.layers[i]);
		// An envmap layer without a current cube is a plain one (0x1014e570:
		// EnvMapManager +0x64 null).
		if (!m_envMap && detail.layers[i][3] == static_cast<float>(TerrainLayerKind::EnvMap)) {
			detail.layers[i][3] = static_cast<float>(TerrainLayerKind::Plain);
		}
	}
	detail.eye[0] = camera.position.x;
	detail.eye[1] = camera.position.y;
	detail.eye[2] = camera.position.z;
	// The current cube (TEXLAYER6 for pass 11) and the side textures
	// (TEXLAYER6 for pass 5) after the shadow maps.
	{
		SDL_GPUTextureSamplerBinding extra[1 + kMaxDetailSides] = {
			{m_envMap ? m_envMap : m_gpu.whiteCubeTexture(), m_gpu.repeatSampler()},
		};
		for (int i = 0; i < kMaxDetailSides; ++i) {
			extra[1 + i] = {m_detailSides[i] ? m_detailSides[i] : m_gpu.whiteTexture(), m_gpu.repeatSampler()};
		}
		SDL_BindGPUFragmentSamplers(pass, 7 + kMaxDetailLayers, extra, 1 + kMaxDetailSides);
	}
	std::copy(std::begin(m_farTiling), std::end(m_farTiling), detail.farTiling);
	// The low-detail texture's coordinates (TEXSCALE, 0x100af7e0): a patch
	// across, 255 / patch size of the vertices' 0..255 grid bytes, and
	// -0.00615148 a metre up for the x and z planes.
	detail.farScale[0] = m_patchSize > 0 ? 1.0f / m_patchSize : 0.0f;
	detail.farScale[1] = m_lowDetailTexture ? 1.0f : 0.0f;
	detail.farScale[3] = kLowDetailVerticalScale;
	std::copy(m_terrainWater.begin(), m_terrainWater.end(), detail.water);
	// The geomorph (TerrainShader_Shared.fx geoMorphPosition): the camera's
	// height above the ground in w, the squared morph limits of the cell's
	// LOD (NEARFARMORPHLIMITS, 0x100af7e0: lod<n>SwitchStart / Stop times the
	// LOD 0 distance; the third LOD has none), MORPHDELTASELECTOR picking the
	// LOD's delta and MORPHDELTAADDER the deltas a cell's stitched edge
	// vertices take whole (0x1014d700): at LOD 1 the vertices that drop at
	// LOD 1 move fully to it, at LOD 2 those that drop at LOD 2.
	if (!terrainUpdated) {
		updateTerrain();
	}
	const auto morphFor = [&](int lod, bool morph) {
		const TerrainMorph m = terrainMorph(lod, morph, m_terrainLodSwitch, renderer, camera.position.x,
			camera.position.z, m_terrainHeightAboveGround);
		TerrainMorphUniforms u {};
		static_assert(sizeof(u) == sizeof(m));
		std::memcpy(&u, &m, sizeof(u));
		return u;
	};
	const auto orBlack = [this](SDL_GPUTexture* t) { return t ? t : m_gpu.blackTexture(); };
	const auto bindPatch = [&](const TerrainPatch& patch) {
		SDL_GPUBufferBinding vertexBinding {patch.vertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		SDL_PushGPUFragmentUniformData(commands, 0, patch.lightmap ? &terrainLit : &terrainUnlit, sizeof(terrainLit));
		detail.detailFade[2] = patch.detailWeights[0] != nullptr ? 1.0f : 0.0f;
		SDL_PushGPUFragmentUniformData(commands, 1, &detail, sizeof(detail));
		SDL_GPUTextureSamplerBinding samplers[5 + kMaxDetailLayers] = {
			{patch.colormap ? patch.colormap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
			{patch.lightmap ? patch.lightmap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
			{orBlack(patch.detailWeights[0]), m_gpu.clampSampler()},
			{orBlack(patch.detailWeights[1]), m_gpu.clampSampler()},
		};
		for (int i = 0; i < kMaxDetailLayers; ++i) {
			samplers[4 + i] = {m_detailLayers[i] ? m_detailLayers[i] : m_gpu.whiteTexture(), m_gpu.repeatSampler()};
		}
		samplers[4 + kMaxDetailLayers] = {m_lowDetailTexture ? m_lowDetailTexture : m_gpu.whiteTexture(), m_gpu.repeatSampler()};
		SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5 + kMaxDetailLayers);
	};
	static const bool lodDebug = std::getenv("BF2142_TERRAIN_LODS") != nullptr;
	if (!switches.drawTerrain) {
	} else if (!m_terrainCells.empty()) {
		SDL_GPUBufferBinding indexBinding {m_terrainLodIndices, 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		const int perPatch = m_terrainCellsPerPatch;
		for (size_t p = 0; p < m_terrain.size(); ++p) {
			const int px = static_cast<int>(p) % m_patchesPerSide, pz = static_cast<int>(p) / m_patchesPerSide;
			bool bound = false;
			int pushed = -1;
			for (int j = 0; j < perPatch; ++j) {
				for (int i = 0; i < perPatch; ++i) {
					const size_t c = static_cast<size_t>(pz * perPatch + j) * m_terrainCellsPerSide +
						static_cast<size_t>(px * perPatch + i);
					const TerrainLodCell& cell = m_terrainCells[c];
					if (!cell.visible) {
						continue;
					}
					const auto& ranges = m_terrainNodeRanges[static_cast<size_t>(m_terrainCellNodes[c])];
					TerrainNodeRange range = ranges[static_cast<size_t>(cell.lod * 11 + cell.mask)];
					if (range.count == 0) {
						range = ranges[static_cast<size_t>(cell.lod * 11)]; // a node without stitched variants
					}
					if (range.count == 0) {
						continue;
					}
					if (!bound) {
						bindPatch(m_terrain[p]);
						bound = true;
					}
					if (pushed != cell.lod) {
						const TerrainMorphUniforms morph = morphFor(cell.lod, true);
						SDL_PushGPUVertexUniformData(commands, 1, &morph, sizeof(morph));
						pushed = cell.lod;
						if (lodDebug) { // BF2142_TERRAIN_LODS=1: cells tinted by LOD
							detail.detailFade[0] = static_cast<float>(cell.lod + 1);
							SDL_PushGPUFragmentUniformData(commands, 1, &detail, sizeof(detail));
						}
					}
					SDL_DrawGPUIndexedPrimitives(pass, range.count, 1, range.start, 0, 0);
				}
			}
		}
	} else if (m_terrainIndices) {
		SDL_GPUBufferBinding indexBinding {m_terrainIndices, 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		// The heightmap at full detail: no morph, the detail fading as LOD 0's.
		const TerrainMorphUniforms morph = morphFor(0, false);
		SDL_PushGPUVertexUniformData(commands, 1, &morph, sizeof(morph));
		for (const TerrainPatch& patch : m_terrain) {
			if (!frustum.containsBox(patch.boundsMin, patch.boundsMax)) {
				continue;
			}
			bindPatch(patch);
			SDL_DrawGPUIndexedPrimitives(pass, m_terrainIndexCount, 1, 0, 0, 0);
		}
	}
	// The surrounding terrain.
	if (!m_surrounding.empty() && switches.drawTerrain) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.surroundingPipeline());
		SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
		const TerrainMorphUniforms still = morphFor(2, false);
		SDL_PushGPUVertexUniformData(commands, 1, &still, sizeof(still));
		SDL_PushGPUFragmentUniformData(commands, 0, &terrainUnlit, sizeof(terrainUnlit));
		// STTEXSCALE and STFARTEXTILING (0x100af7e0).
		TerrainDetailUniforms surroundingDetail = detail;
		std::copy(std::begin(m_surroundingFarTiling), std::end(m_surroundingFarTiling), surroundingDetail.farTiling);
		surroundingDetail.farScale[0] = m_surroundingTexScale;
		SDL_PushGPUFragmentUniformData(commands, 1, &surroundingDetail, sizeof(surroundingDetail));
		SDL_GPUBufferBinding indexBinding {m_surroundingIndices, 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		for (const SurroundingPatch& patch : m_surrounding) {
			if (!frustum.containsBox(patch.boundsMin, patch.boundsMax)) {
				continue;
			}
			SDL_GPUBufferBinding vertexBinding {patch.vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
			SDL_GPUTextureSamplerBinding samplers[3] = {
				{patch.colormap, m_gpu.clampSampler()},
				{patch.lowComponent ? patch.lowComponent : m_gpu.blackTexture(), m_gpu.clampSampler()},
				{m_lowDetailTexture ? m_lowDetailTexture : m_gpu.whiteTexture(), m_gpu.repeatSampler()},
			};
			SDL_BindGPUFragmentSamplers(pass, 0, samplers, 3);
			SDL_DrawGPUIndexedPrimitives(pass, m_surroundingIndexCount, 1, 0, 0, 0);
		}
	}

	// Roads, once per terrain patch they overlap, lit by that patch's lightmap.
	const auto patchIndex = [this](float world, int axis) {
		return static_cast<int>(std::floor((world - m_patchOrigin[axis]) / m_patchSize));
	};
	if ((!m_roads.empty() || !m_roadStatics.empty()) && m_patchSize > 0 && switches.drawRoads) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.roadPipeline());
		useShadows(commands, pass, 2, 3, false);
		// The road renderer's view distance (0x100baa70 / 0x100ba1d0): f =
		// terrainCuller.lodSwitchDistance (the culler's +0x18) x
		// Renderer.roadLodDistMod (+0x88, 1.3); a road farther than f plus
		// its radius isn't drawn, and RoadCompiled.fx fades the rest out from
		// f x lod0SwitchStart (the culler's +0x20) to f: FadeOut = (1 / (f -
		// f0), f0 / (f - f0)).
		const float roadFar = static_cast<float>(renderer.lodSwitchDistance) * renderer.roadLodDistMod;
		const float roadNear = roadFar * renderer.lod0SwitchStart;
		const float roadScale = roadFar > roadNear ? 1.0f / (roadFar - roadNear) : 0.0f;
		for (const Road& road : m_roads) {
			const Vec3f toRoad = road.center - camera.position;
			if (std::sqrt(dot(toRoad, toRoad)) > roadFar + road.radius) {
				continue;
			}
			RoadUniforms roadUniforms = road.uniforms;
			roadUniforms.road[1] = roadScale;
			roadUniforms.road[2] = roadScale * roadNear;
			roadUniforms.eye[0] = camera.position.x;
			roadUniforms.eye[1] = camera.position.y;
			roadUniforms.eye[2] = camera.position.z;
			SDL_GPUBufferBinding vertexBinding {road.vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
			SDL_GPUBufferBinding indexBinding {road.indices, 0};
			SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
			SDL_PushGPUFragmentUniformData(commands, 1, &roadUniforms, sizeof(roadUniforms));
			const int x0 = std::max(patchIndex(road.boundsMin[0], 0), 0);
			const int x1 = std::min(patchIndex(road.boundsMax[0], 0), m_patchesPerSide - 1);
			const int z0 = std::max(patchIndex(road.boundsMin[1], 1), 0);
			const int z1 = std::min(patchIndex(road.boundsMax[1], 1), m_patchesPerSide - 1);
			for (int pz = z0; pz <= z1; ++pz) {
				for (int px = x0; px <= x1; ++px) {
					const TerrainPatch& patch = m_terrain[static_cast<size_t>(pz * m_patchesPerSide + px)];
					TransformUniforms roadTransforms {road.world * viewProjection, road.world,
						{1 / m_patchSize, 1 / m_patchSize, -(m_patchOrigin[0] / m_patchSize + px),
							-(m_patchOrigin[1] / m_patchSize + pz)},
						{}};
					roadTransforms.partsUsed = 0;
					SDL_PushGPUVertexUniformData(commands, 0, &roadTransforms, pushUniformSize(roadTransforms));
					SDL_PushGPUFragmentUniformData(commands, 0, patch.lightmap ? &terrainLit : &terrainUnlit,
						sizeof(terrainLit));
					SDL_GPUTextureSamplerBinding samplers[3] = {
						{road.texture0, m_gpu.repeatSampler()},
						{road.texture1, m_gpu.repeatSampler()},
						{patch.lightmap ? patch.lightmap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
					};
					SDL_BindGPUFragmentSamplers(pass, 0, samplers, 3);
					SDL_DrawGPUIndexedPrimitives(pass, road.indexCount, 1, 0, 0, 0);
				}
			}
		}
		// The road static meshes (RoadStatics.h): their own vertices through
		// the road's lighting and fade, per terrain patch as the compiled
		// roads, each material with its RaShaderRoad*.fx technique. The light
		// buffer RaShaderRoad samples by screen position is the terrain's
		// lighting there: the patch's lightmap stands in, as for the roads.
		if (!m_roadStatics.empty()) {
			SDL_BindGPUGraphicsPipeline(pass, m_gpu.roadMeshPipeline());
			useShadows(commands, pass, 2, 3, false);
		}
		for (const Instance* instance : m_roadStatics) {
			const GpuMesh& mesh = *instance->mesh;
			const BoundingSphere sphere = instanceBounds(*instance, mesh);
			const Vec3f toMesh = sphere.center - camera.position;
			if (std::sqrt(dot(toMesh, toMesh)) > roadFar + sphere.radius) {
				continue; // faded out (the fade's alpha is 0 beyond roadFar)
			}
			SDL_GPUBufferBinding vertexBinding {mesh.vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
			SDL_GPUBufferBinding indexBinding {mesh.indices, 0};
			SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
			const float reach = std::max(sphere.radius, 0.0f);
			const int x0 = std::max(patchIndex(sphere.center.x - reach, 0), 0);
			const int x1 = std::min(patchIndex(sphere.center.x + reach, 0), m_patchesPerSide - 1);
			const int z0 = std::max(patchIndex(sphere.center.z - reach, 1), 0);
			const int z1 = std::min(patchIndex(sphere.center.z + reach, 1), m_patchesPerSide - 1);
			for (const Submesh& submesh : mesh.submeshes) {
				RoadUniforms roadUniforms {};
				roadUniforms.road[1] = roadScale;
				roadUniforms.road[2] = roadScale * roadNear;
				roadUniforms.road[3] = static_cast<float>(roadStaticTechnique(submesh.detail != nullptr, mesh.roadNoBlend));
				roadUniforms.eye[0] = camera.position.x;
				roadUniforms.eye[1] = camera.position.y;
				roadUniforms.eye[2] = camera.position.z;
				SDL_PushGPUFragmentUniformData(commands, 1, &roadUniforms, sizeof(roadUniforms));
				for (int pz = z0; pz <= z1; ++pz) {
					for (int px = x0; px <= x1; ++px) {
						const TerrainPatch& patch = m_terrain[static_cast<size_t>(pz * m_patchesPerSide + px)];
						TransformUniforms meshTransforms {instance->world * viewProjection, instance->world,
							{1 / m_patchSize, 1 / m_patchSize, -(m_patchOrigin[0] / m_patchSize + px),
								-(m_patchOrigin[1] / m_patchSize + pz)},
							{}};
						meshTransforms.parts[0] = viewProjection;
						meshTransforms.partsUsed = 1;
						SDL_PushGPUVertexUniformData(commands, 0, &meshTransforms, pushUniformSize(meshTransforms));
						SDL_PushGPUFragmentUniformData(commands, 0, patch.lightmap ? &terrainLit : &terrainUnlit,
							sizeof(terrainLit));
						SDL_GPUTextureSamplerBinding samplers[3] = {
							{submesh.texture, m_gpu.repeatSampler()},
							{submesh.detail ? submesh.detail : m_gpu.whiteTexture(), m_gpu.repeatSampler()},
							{patch.lightmap ? patch.lightmap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
						};
						SDL_BindGPUFragmentSamplers(pass, 0, samplers, 3);
						SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
					}
				}
			}
		}
	}

	if (switches.drawUndergrowth && switches.undergrowthEnabled) {
		drawUndergrowth(commands, pass, viewProjection, camera, lights);
	}

	// Objects: the opaque materials first; then, after the water, the
	// transparent ones (blended materials, and every material of an object
	// fading out) sorted back to front, the additive ones, and the
	// particles: RendDX9's queue order (0x1002cef0: RSMTWater, then
	// RSMTTransparency sorted by distance, RSMTAdditive, the particles).
	// The queue's pipeline, culled or not by the material (meshCulled),
	// rebound only when that changes.
	SDL_GPUGraphicsPipeline* meshBound = nullptr;
	const auto bindMeshPipeline = [&](SDL_GPUGraphicsPipeline* pipeline) {
		if (pipeline != meshBound) {
			SDL_BindGPUGraphicsPipeline(pass, pipeline);
			useShadows(commands, pass, 1, 5, true);
			meshBound = pipeline;
		}
	};
	const auto useMeshPipeline = [&](MeshQueue queue, bool culled) { bindMeshPipeline(m_gpu.meshPipeline(queue, culled)); };
	// Antialiased, the overgrowth's leaves take alpha to coverage
	// (0x1002cef0 sets Alpha2Mask before the overgrowth): opaque, their
	// fade a dissolve of their coverage rather than a blend.
	const bool coverage = alphaToCoverage();
	const auto coverageLeaf = [coverage](const DrawItem& item, const Submesh& submesh) {
		return coverage && submesh.leaf && item.instance->overgrowth && !submesh.blended;
	};
	const auto queueFor = [&](const DrawItem& item, const Submesh& submesh) {
		return coverageLeaf(item, submesh) ? MeshQueue::Opaque : queueOf(submesh, item.drawnFade());
	};
	useMeshPipeline(MeshQueue::Opaque, false);
	// Lightmapped statics use the static sun/sky colors.
	LightingUniforms uniformsFor[2][2] = {
		{lighting(m_viewFog, lights, lights.sunColor, lights.skyColor, 0, false),
			lighting(m_viewFog, lights, lights.sunColor, lights.skyColor, 0.5f, false)},
		{lighting(m_viewFog, lights, lights.staticSunColor, lights.staticSkyColor, 0, true),
			lighting(m_viewFog, lights, lights.staticSunColor, lights.staticSkyColor, 0.5f, true)},
	};
	// The objects' point lights are their own (drawInstance).
	const float windSpeed = m_sources && m_sources->wind ? m_sources->wind->speedAt() : 0.0f;
	// The lighting level's bundled and skinned meshes (QualitySettings).
	const bool hemiAndShadows = !m_sources || !m_sources->quality || m_sources->quality->meshHemiAndShadows();
	const GpuMesh* bound = nullptr;
	bool culling = false;
	const uint32_t inside = m_sources && m_sources->cockpitVehicle ? m_sources->cockpitVehicle() : 0;
	const auto drawInstance = [&](const DrawItem& item, MeshQueue queue) {
		const Instance& instance = *item.instance;
		bool any = false;
		// Its point lights (MeshLights.h): the listed ones about its origin.
		const LightedMesh lightKind = item.mesh->skinned ? LightedMesh::Skinned
			: item.mesh->bundled                         ? LightedMesh::Bundled
														 : LightedMesh::Static;
		int picked[kMaxPointLights];
		int pickedCount = 0;
		if (!m_listedLights.empty()) {
			const float origin[3] = {instance.world.at(3, 0), instance.world.at(3, 1), instance.world.at(3, 2)};
			pickedCount = pickMeshLights(m_listedLights.data(), m_listedLights.size(), origin,
				std::max(item.mesh->bounds.radius, 0.0f), lightKind, item.lightmap != nullptr, m_lightLimits, picked,
				kMaxPointLights);
		}
		for (const Submesh& submesh : item.mesh->submeshes) {
			any = any || queueFor(item, submesh) == queue;
		}
		if (!any) {
			return;
		}
		// The occupant's own exterior hull (the gunner's cockpitSubGeom 1):
		// back faces culled, or the view is inside the hull's far side. Wrecks,
		// crew kits and dropped kits (also geometry 1) stay two-sided.
		if (queue == MeshQueue::Opaque) {
			const bool cull = instance.shownGeometry == 1 && inside != 0 && instance.object == inside;
			if (cull != culling) {
				culling = cull;
				SDL_BindGPUGraphicsPipeline(pass, cull ? m_gpu.meshPipeline(MeshQueue::Opaque, true) : m_gpu.meshPipeline());
			}
		}
		if (item.mesh != bound) {
			bound = item.mesh;
			SDL_GPUBufferBinding vertexBinding {bound->vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
			SDL_GPUBufferBinding indexBinding {bound->indices, 0};
			SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		}
		transforms.worldViewProjection = instance.world * viewProjection;
		transforms.world = instance.world;
		const Vec3f trunkSun = normalize(Vec3f {-lights.sunDirection[0], -lights.sunDirection[1], -lights.sunDirection[2]});
		transforms.trunkSunDirection[0] = trunkSun.x;
		transforms.trunkSunDirection[1] = trunkSun.y;
		transforms.trunkSunDirection[2] = trunkSun.z;
		// A static mesh reads only the first part (pushUniformSize).
		if (instance.parts.empty() && !bound->skinned) {
			transforms.parts[0] = Mat4 {};
			transforms.partsUsed = 1;
		} else {
			for (int i = 0; i < kMaxMeshParts; ++i) {
				transforms.parts[i] = instance.parts.empty() ? Mat4 {} : instance.parts[static_cast<size_t>(i)];
			}
			transforms.partsUsed = kMaxMeshParts;
		}
		std::copy(item.lightmapTransform, item.lightmapTransform + 4, transforms.lightmapTransform);
		// The leaves' sway (not the overgrowth's: RaShaderLeaf's OVERGROWTH has none).
		transforms.sway[0] = 0;
		transforms.sway[1] = bound->bounds.radius;
		// WindSpeed (WindManager slot 0x30 at the object, +0x208, 0x100bdb00:
		// a helicopter's downwash bends the trees under it) + WIND_ADD.
		const scene::WindSettings* wind = m_sources ? m_sources->wind : nullptr;
		transforms.sway[2] = (wind && !wind->localWinds.empty()
				? wind->speedAt({instance.world.at(3, 0), instance.world.at(3, 1), instance.world.at(3, 2)})
				: windSpeed) + 5;
		transforms.sway[3] = seconds;
		// Bundled and static meshes' normal maps are in tangent space; a
		// skinned mesh's material says (its normalKind).
		transforms.normalSpace[0] = bound->skinned ? 0.0f : 1.0f;
		transforms.normalSpace[1] = 0;
		transforms.uvUsed = bound->bundled;
		if (bound->bundled) {
			for (int i = 0; i < kAnimatedUVMatrices; ++i) {
				const UVMatrix& m = instance.uvMatrices[static_cast<size_t>(i)];
				float* rows = transforms.uvMatrices[i * 2];
				rows[0] = m.r0[0];
				rows[1] = m.r0[1];
				rows[2] = m.r1[0];
				rows[3] = m.r1[1];
				transforms.uvMatrices[i * 2 + 1][0] = m.r2[0];
				transforms.uvMatrices[i * 2 + 1][1] = m.r2[1];
			}
		}
		SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
		bool swaying = false;
		const bool lightmapped = item.lightmap != nullptr;
		float hemiObject[4] = {1, 1, 1, 0};
		float simpleShadow = 0;
		if (bound->skinned || bound->bundled) {
			hemiObjectFor(instance, *bound, hemiObject);
			simpleShadow = simpleShadowFor(instance.world, bound->skinned, instance.shadowCaster);
		}
		static const std::vector<anim::Matrix> unposed;
		const std::vector<anim::Matrix>& pose = bound->skinned ? poseFor(instance, seconds) : unposed;
		for (const Submesh& submesh : bound->submeshes) {
			if (queueFor(item, submesh) != queue) {
				continue;
			}
			const bool leafCoverage = coverageLeaf(item, submesh);
			if (leafCoverage) {
				bindMeshPipeline(m_gpu.coverageMeshPipeline());
			} else {
				useMeshPipeline(queue, meshCulled(bound->bundled || bound->skinned, submesh.blended, submesh.leaf));
			}
			const float space = bound->skinned ? static_cast<float>(submesh.normalKind) : transforms.normalSpace[0];
			// Identity matrices without a part that animates them (RendDX9
			// copies identities while +0x28c is unmade).
			const float uvAnimated = submesh.animatedUV ? 1.0f : 0.0f;
			if (submesh.rig >= 0 && !pose.empty()) {
				skinPalette(*bound, submesh, pose, transforms);
				transforms.normalSpace[0] = space;
				transforms.normalSpace[1] = uvAnimated;
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
			} else if (space != transforms.normalSpace[0] || uvAnimated != transforms.normalSpace[1]) {
				transforms.normalSpace[0] = space;
				transforms.normalSpace[1] = uvAnimated;
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
			}
			const bool sway = submesh.leaf && !instance.overgrowth;
			if (sway != swaying) {
				swaying = sway;
				transforms.sway[0] = sway ? 1.0f : 0.0f;
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
			}
			LightingUniforms uniforms = uniformsFor[lightmapped][submesh.alphaReference > 0];
			if (submesh.trunk) {
				uniforms = lighting(m_viewFog, lights, lights.treeSunColor, lights.treeAmbientColor, 0, false);
			}
			applyMeshLights(uniforms, picked, pickedCount, lightKind);
			materialUniforms(uniforms, submesh, item.drawnFade(), seconds);
			if (submesh.trunk) uniforms.material[2] += kMaterialTrunk;
			specularUniforms(uniforms, submesh, camera, lights);
			if (bound->skinned || bound->bundled) {
				meshKindUniforms(uniforms, submesh, hemiAndShadows);
				std::copy(hemiObject, hemiObject + 4, uniforms.hemiObject);
			}
			// The Titan interior path binds the indoor cube (0x100c3880,
			// EnvMapManager +0x6c) for the envmap materials.
			SDL_GPUTexture* const envMap = item.interior && m_indoorEnvMap ? m_indoorEnvMap : m_envMap;
			layerUniforms(uniforms, submesh, envMap != nullptr);
			uniforms.layers[3] = simpleShadow;
			if (leafCoverage) {
				// RaShaderLeafOG with HASALPHA2MASK: the texture's alpha
				// times 4 x the tree's Transparency (the near pass's doubled
				// alphas, prepareInstances), which the vertex colour holds
				// to 1 (COLOR0 is clamped); that is the coverage, with no
				// alpha test.
				uniforms.material[0] = 0;
				uniforms.material[3] = 0;
				uniforms.tint[3] = AlphaToCoverage::leafAlphaScale(item.drawnFade());
			} else if (submesh.coverageTexture) {
				// A plain texture without coverage (a tree that isn't
				// overgrowth, or antialiasing turned off since the load):
				// RaShaderLeaf's 2 x alpha against AlphaRef 127.
				uniforms.tint[3] = AlphaToCoverage::kPlainLeafAlphaScale;
			}
			SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof(uniforms));
			const auto orWhite = [this](SDL_GPUTexture* t) { return t ? t : m_gpu.whiteTexture(); };
			SDL_GPUTextureSamplerBinding samplers[5] = {
				{submesh.texture, m_gpu.repeatSampler()},
				{lightmapped ? item.lightmap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
				{orWhite(submesh.detail), m_gpu.repeatSampler()},
				{orWhite(submesh.normalMap ? submesh.normalMap : submesh.dirt), m_gpu.repeatSampler()},
				{orWhite(submesh.crack), m_gpu.repeatSampler()},
			};
			SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
			bindMeshLayers(pass, &submesh, envMap);
			SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
		}
	};
	for (const DrawItem& item : m_drawList) {
		drawInstance(item, MeshQueue::Opaque);
	}
	if (culling) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.meshPipeline());
	}
	// Decals over the opaque surfaces.
	if (m_decalVertices && !m_decalGroups.empty()) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.decalPipeline());
		meshBound = nullptr;
		const LightingUniforms decalLighting = lighting(m_viewFog, lights, lights.sunColor, lights.skyColor, 0, false);
		SDL_PushGPUFragmentUniformData(commands, 0, &decalLighting, sizeof(decalLighting));
		// Decals.fx's DecalFadeDistanceAndInterval (0x10163810).
		TransformUniforms decalTransforms {viewProjection, Mat4 {},
			{m_particleSettings.decalFadeDistance, m_particleSettings.decalFadeDistance * m_particleSettings.decalStartFadePercentage, 0, 0}, {}};
		decalTransforms.partsUsed = 0;
		// WorldCamPos (+0x3180, set per batch by 0x101631f0): the eye, for the
		// normal-mapped technique's half vector.
		decalTransforms.sway[0] = camera.position.x;
		decalTransforms.sway[1] = camera.position.y;
		decalTransforms.sway[2] = camera.position.z;
		SDL_PushGPUVertexUniformData(commands, 0, &decalTransforms, pushUniformSize(decalTransforms));
		SDL_GPUBufferBinding vertexBinding {m_decalVertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		for (const ParticleGroup& group : m_decalGroups) {
			SDL_GPUTextureSamplerBinding sampler[2] = {{group.texture, m_gpu.clampSampler()},
				{group.normalMap ? group.normalMap : m_gpu.whiteTexture(), m_gpu.clampSampler()}};
			SDL_BindGPUFragmentSamplers(pass, 0, sampler, 2);
			SDL_DrawGPUPrimitives(pass, group.vertexCount, 1, group.firstVertex, 0);
		}
		bound = nullptr;
	}

	// Water, blended over what is below it.
	if (!m_waterBlocks.empty() && m_water) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.waterPipeline());
		const LightingUniforms waterLighting = lighting(m_viewFog, lights, lights.sunColor, lights.skyColor, 0, false);
		SDL_PushGPUFragmentUniformData(commands, 0, &waterLighting, sizeof(waterLighting));
		// WaterCycleTime (0x100b17d0; t the renderer's clock in seconds, a
		// waterAnimSpeed of 0 taken as 0.01): the 3D variants (Water3D,
		// WaterHighEnd3D: lighting quality 2 and up) take frac(t / speed), and
		// the shader scrolls the volume's coordinates by waterScroll times it
		// and moves through its slices by ten times it; the 2D ones (quality
		// 0-1) run once through all the waterbump frames in 0.2 x speed
		// seconds, blending each into the next, unscrolled.
		const float speed = m_water->animSpeed != 0 ? m_water->animSpeed : 0.01f;
		const size_t frameCount = std::max<size_t>(m_waterFrames.size(), 1);
		float cycle = 0;
		size_t frame0 = 0;
		if (m_waterVolume) {
			cycle = seconds / speed - std::floor(seconds / speed);
		} else {
			const float position = seconds / (speed * 0.2f) - std::floor(seconds / (speed * 0.2f));
			const float frames = position * static_cast<float>(frameCount);
			frame0 = static_cast<size_t>(frames) % frameCount;
			cycle = frames - std::floor(frames);
		}
		const size_t frame1 = (frame0 + 1) % frameCount;
		const WaterUniforms water {
			{camera.position.x, camera.position.y, camera.position.z, seconds},
			{m_water->color[0], m_water->color[1], m_water->color[2], m_water->sunIntensity},
			{m_water->specularColor[0], m_water->specularColor[1], m_water->specularColor[2], m_water->specularColor[3]},
			{m_water->alphaAdd, m_water->alphaHeightMul, m_water->specularPower, m_waterEnvMap ? 1.0f : 0.0f},
			{m_water->scroll[0], m_water->scroll[1], cycle, m_waterVolume ? 1.0f : 0.0f},
		};
		SDL_PushGPUFragmentUniformData(commands, 1, &water, sizeof(water));
		TransformUniforms waterTransforms {viewProjection, Mat4 {}, {1, 1, 0, 0}, {}};
		waterTransforms.partsUsed = 0;
		SDL_PushGPUVertexUniformData(commands, 0, &waterTransforms, pushUniformSize(waterTransforms));
		SDL_GPUTexture* flat = m_gpu.whiteTexture();
		SDL_GPUTextureSamplerBinding samplers[4] = {
			{m_waterFrames.empty() ? flat : m_waterFrames[frame0], m_gpu.repeatSampler()},
			{m_waterFrames.empty() ? flat : m_waterFrames[frame1], m_gpu.repeatSampler()},
			{m_waterEnvMap ? m_waterEnvMap : m_gpu.whiteCubeTexture(), m_gpu.clampSampler()},
			{m_waterVolume ? m_waterVolume : m_gpu.whiteVolumeTexture(), m_gpu.repeatSampler()},
		};
		SDL_BindGPUFragmentSamplers(pass, 0, samplers, 4);
		for (const WaterBlock& block : m_waterBlocks) {
			if (!frustum.containsBox(block.boundsMin, block.boundsMax)) {
				continue;
			}
			SDL_GPUBufferBinding vertexBinding {block.vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
			SDL_GPUBufferBinding indexBinding {block.indices, 0};
			SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
			SDL_DrawGPUIndexedPrimitives(pass, block.indexCount, 1, 0, 0, 0);
		}
		bound = nullptr;
	}

	// The sun flare, behind what is in front of it (0x1002cef0: with the
	// sky dome's switch and its own).
	if (switches.drawSkyDome && switches.drawSunFlare) {
		drawSunFlare(commands, pass, viewProjection, camera);
	}
	// The transparent materials, farthest object first.
	m_enh.aoBound = false; // nor the transparent ones, the additive ones or the first person
	meshBound = nullptr; // the water's, the sun flare's
	useMeshPipeline(MeshQueue::Transparent, false);
	std::vector<std::pair<float, const DrawItem*>> transparent;
	for (const DrawItem& item : m_drawList) {
		transparent.push_back({transparencyKey(item.instance->world, camera.position), &item});
	}
	sortBackToFront(transparent);
	for (const auto& [distance, item] : transparent) {
		drawInstance(*item, MeshQueue::Transparent);
	}
	// The additive ones (not sorted: RSMTAdditive keeps its order).
	useMeshPipeline(MeshQueue::Additive, false);
	for (const DrawItem& item : m_drawList) {
		drawInstance(item, MeshQueue::Additive);
	}
	// Particles, after what they are in front of: the mesh particles, then the sprites.
	if (switches.drawParticles) {
		drawMeshParticles(commands, pass, viewProjection, lights,
			{m_sources ? m_sources->effects : nullptr, m_sources ? m_sources->objectEffects : nullptr});
		drawParticles(commands, pass, viewProjection, camera, lights, m_particleVertices, m_particleGroups);
	}
	if (shimmer) {
		SDL_EndGPURenderPass(pass);
		if (!m_shimmerSource || m_shimmerSize[0] != target.width || m_shimmerSize[1] != target.height) {
			if (m_shimmerSource) {
				SDL_ReleaseGPUTexture(m_gpu.device(), m_shimmerSource);
			}
			m_shimmerSource = m_gpu.createColorTexture(target.width, target.height);
			m_shimmerSize[0] = target.width;
			m_shimmerSize[1] = target.height;
		}
		SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
		SDL_GPUTextureLocation from {m_sceneColor, 0, 0, 0, 0, 0};
		SDL_GPUTextureLocation to {m_shimmerSource, 0, 0, 0, 0, 0};
		SDL_CopyGPUTextureToTexture(copy, &from, &to, target.width, target.height, 1, false);
		SDL_EndGPUCopyPass(copy);
		SDL_GPUColorTargetInfo keepColor = colorTarget;
		keepColor.load_op = SDL_GPU_LOADOP_LOAD;
		keepColor.store_op = worldStore();
		SDL_GPUDepthStencilTargetInfo keepDepth = depthTarget;
		keepDepth.load_op = SDL_GPU_LOADOP_LOAD;
		keepDepth.stencil_load_op = SDL_GPU_LOADOP_LOAD;
		pass = SDL_BeginGPURenderPass(commands, &keepColor, 1, &keepDepth);
		drawShimmer(commands, pass, viewProjection, camera, target.width, target.height);
	}
	// The cloaked soldiers' mask, tested against the world's depth.
	if (cloaked) {
		SDL_EndGPURenderPass(pass);
		drawCamouflage(commands, depthTarget.texture, viewProjection, seconds);
		SDL_GPUColorTargetInfo keepColor = colorTarget;
		keepColor.load_op = SDL_GPU_LOADOP_LOAD;
		keepColor.store_op = worldStore();
		SDL_GPUDepthStencilTargetInfo keepDepth = depthTarget;
		keepDepth.load_op = SDL_GPU_LOADOP_LOAD;
		keepDepth.stencil_load_op = SDL_GPU_LOADOP_LOAD;
		pass = SDL_BeginGPURenderPass(commands, &keepColor, 1, &keepDepth);
	}
	// The first-person pass: the world's depth cleared, the weapon and arms
	// drawn with their own projection.
	if (m_hasFirstPerson) {
		SDL_EndGPURenderPass(pass);
		SDL_GPUColorTargetInfo keepColor = colorTarget;
		keepColor.load_op = SDL_GPU_LOADOP_LOAD;
		keepColor.store_op = worldStore();
		SDL_GPUDepthStencilTargetInfo clearDepth = depthTarget;
		clearDepth.store_op = SDL_GPU_STOREOP_DONT_CARE;
		clearDepth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		pass = SDL_BeginGPURenderPass(commands, &keepColor, 1, &clearDepth);
		drawFirstPerson(commands, pass, camera, static_cast<float>(target.width) / target.height, lights);
	}
	// The post-production into the target (renderer.drawPostProduction),
	// the HUD and menu over it.
	if (post && m_sceneColor) {
		SDL_EndGPURenderPass(pass);
		m_postSource = m_sceneColor;
		if (m_enh.prepass) {
			showEnhancedBuffer(commands);
		}
		if (m_enh.taa) {
			resolveTaa(commands); // m_postSource: the resolve
		}
		const PostUniforms postUniforms = preparePost(commands, cloaked);
		SDL_GPUColorTargetInfo finalColor {};
		finalColor.texture = color;
		finalColor.load_op = SDL_GPU_LOADOP_DONT_CARE;
		finalColor.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPUDepthStencilTargetInfo hudDepth = depthTarget; // the menus' masks start from 0
		hudDepth.texture = target.depth;
		hudDepth.store_op = SDL_GPU_STOREOP_DONT_CARE;
		hudDepth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		pass = SDL_BeginGPURenderPass(commands, &finalColor, 1, &hudDepth);
		drawPost(commands, pass, postUniforms);
	} else if (multisampled) {
		// Without it, the HUD and menu over the resolved world in `color`,
		// with the single-sampled depth-stencil.
		SDL_EndGPURenderPass(pass);
		SDL_GPUColorTargetInfo keepColor {};
		keepColor.texture = color;
		keepColor.load_op = SDL_GPU_LOADOP_LOAD;
		keepColor.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPUDepthStencilTargetInfo hudDepth = depthTarget;
		hudDepth.texture = target.depth;
		hudDepth.store_op = SDL_GPU_STOREOP_DONT_CARE;
		hudDepth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		pass = SDL_BeginGPURenderPass(commands, &keepColor, 1, &hudDepth);
	}
	// The HUD over everything.
	if (m_overlayVertices && !m_overlayGroups.empty()) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.uiPipeline());
		const float screen[4] = {800, 600, m_sources ? m_sources->hudXScale : 1.0f, 0};
		SDL_PushGPUVertexUniformData(commands, 0, screen, sizeof(screen));
		SDL_GPUBufferBinding vertexBinding {m_overlayVertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		// The blend effects' factors (addNodeBlendEffect: the render-state
		// node 0x00870c40 hands each quad below it through, the vehicle
		// HUDs' 7 2 adding their glass over the view).
		SDL_GPUGraphicsPipeline* boundPipeline = m_gpu.uiPipeline();
		for (const OverlayGroup& group : m_overlayGroups) {
			SDL_GPUGraphicsPipeline* pipeline = m_gpu.uiBlendPipeline(group.blend[0], group.blend[1]);
			if (!pipeline) {
				pipeline = m_gpu.uiPipeline();
			}
			if (pipeline != boundPipeline) {
				SDL_BindGPUGraphicsPipeline(pass, pipeline);
				boundPipeline = pipeline;
			}
			SDL_GPUTextureSamplerBinding sampler {group.texture, m_gpu.clampSampler()};
			SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
			SDL_DrawGPUPrimitives(pass, group.vertexCount, 1, group.firstVertex, 0);
		}
	}
	// A menu over the HUD, in its stage.
	if (m_menuVertices && !m_menuGroups.empty()) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.uiPipeline());
		const float stage[4] = {m_menuSize[0], m_menuSize[1], 0, 0};
		SDL_PushGPUVertexUniformData(commands, 0, stage, sizeof(stage));
		SDL_GPUBufferBinding vertexBinding {m_menuVertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		using Stencil = ui::MenuBatch::Stencil;
		Stencil bound = Stencil::Off;
		for (const MenuGroup& group : m_menuGroups) {
			// The mask states (GpuContext::UiStencil): the reference is the
			// level, 0 for a clear.
			if (group.stencil != bound) {
				bound = group.stencil;
				SDL_BindGPUGraphicsPipeline(pass, m_gpu.uiPipeline(bound == Stencil::Submit ? GpuContext::UiStencil::Submit
						: bound == Stencil::Test                                            ? GpuContext::UiStencil::Test
						: bound == Stencil::Clear                                           ? GpuContext::UiStencil::Clear
																							: GpuContext::UiStencil::Off));
			}
			SDL_SetGPUStencilReference(pass, static_cast<Uint8>(group.stencil == Stencil::Clear ? 0 : group.stencilLevel));
			// shortcut: fonts always get RegularScaled's linear; RendDX9.dll 0x10181a90 points them (Regular) at window width <= 1024, port with the Fonts/800 path.
			SDL_GPUTextureSamplerBinding sampler {group.texture, m_gpu.menuSampler(group.repeat)};
			SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
			SDL_DrawGPUPrimitives(pass, group.vertexCount, 1, group.firstVertex, 0);
		}
	}
	SDL_EndGPURenderPass(pass);
	endEnhancedFrame(camera, overheadOn);
	m_gpu.setEnhancedLit(false); // the variants stay inside this frame
}

void LevelRenderer::drawFirstPerson(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Camera& camera,
	float aspect, const scene::LightSettings& lights)
{
	const FirstPersonScene& scene = m_firstPerson;
	// TAA's jitter on both of its views (Enhanced/LevelRendererEnhanced.cpp).
	const auto jittered = [this](const Mat4& m) { return m_enh.jittered ? m * m_enh.jitter : m; };
	const Mat4 viewProjection = jittered(camera.view() * Mat4::perspective(scene.fovY, aspect, scene.nearPlane, scene.farPlane));
	// In a seat camera's inside view with allowInsideStaticSun the cockpit
	// takes the camera's static sun and ambient (BF2142.exe 0x006104d0..
	// 0x00610580: insideStaticSunDirection / Color / AmbientColor, flickering
	// to insideStaticSunDamageColor when damaged), the dynamic lights only
	// with allowInsideDynamicLights.
	InsideSun inside;
	const bool insideSun = m_sources && m_sources->insideSun && m_sources->insideSun(inside);
	scene::LightSettings insideLights;
	if (insideSun) {
		insideLights = lights;
		insideLights.sunDirection = inside.direction;
		insideLights.enableSun = true;
	}
	const scene::LightSettings& useLights = insideSun ? insideLights : lights;
	const std::array<float, 3>& sunColor = insideSun ? inside.color : lights.sunColor;
	const std::array<float, 3>& skyColor = insideSun ? inside.ambient : lights.skyColor;
	LightingUniforms lit[2] = {lighting(m_viewFog, useLights, sunColor, skyColor, 0, false),
		lighting(m_viewFog, useLights, sunColor, skyColor, 0.5f, false)};
	if (!insideSun || inside.dynamicLights) {
		applyPointLights(lit[0]);
		applyPointLights(lit[1]);
	}
	const float seconds = static_cast<float>(SDL_GetTicks() - m_startTicks) / 1000.0f;
	const bool hemiAndShadows = !m_sources || !m_sources->quality || m_sources->quality->meshHemiAndShadows();
	// The queue's pipeline, culled or not by the material (meshCulled).
	SDL_GPUGraphicsPipeline* modelBound = nullptr;
	const auto useModelPipeline = [&](MeshQueue queue, bool culled) {
		SDL_GPUGraphicsPipeline* pipeline = m_gpu.meshPipeline(queue, culled);
		if (pipeline != modelBound) {
			SDL_BindGPUGraphicsPipeline(pass, pipeline);
			useShadows(commands, pass, 1, 5, true);
			modelBound = pipeline;
		}
	};
	const auto drawModel = [&](const FirstPersonModel& model, MeshQueue queue) {
		const GpuMesh* gpuMesh = mesh(*m_sources, model.geometry, model.geometryIndex, model.lod);
		if (!gpuMesh) {
			return;
		}
		bool any = false;
		for (const Submesh& submesh : gpuMesh->submeshes) {
			any = any || queueOf(submesh, 1.0f) == queue;
		}
		if (!any) {
			return;
		}
		SDL_GPUBufferBinding vertexBinding {gpuMesh->vertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		SDL_GPUBufferBinding indexBinding {gpuMesh->indices, 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		TransformUniforms transforms {model.world * viewProjection, model.world, {1, 1, 0, 0}, {}};
		for (size_t i = 0; i < model.parts.size() && i < static_cast<size_t>(kMaxMeshParts); ++i) {
			transforms.parts[i] = model.parts[i];
		}
		SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
		for (const Submesh& submesh : gpuMesh->submeshes) {
			if (queueOf(submesh, 1.0f) != queue) {
				continue;
			}
			useModelPipeline(queue, meshCulled(gpuMesh->bundled || gpuMesh->skinned, submesh.blended, submesh.leaf));
			transforms.normalSpace[0] = gpuMesh->skinned ? static_cast<float>(submesh.normalKind) : gpuMesh->bundled ? 1.0f : 0.0f;
			if (gpuMesh->skinned) {
				if (submesh.rig < 0 || model.pose.empty()) {
					continue; // a skinned mesh without its pose isn't drawn
				}
				const auto& rig = gpuMesh->rigs[static_cast<size_t>(submesh.rig)];
				for (size_t b = 0; b < rig.size() && b < static_cast<size_t>(kMaxMeshParts); ++b) {
					const int node = rig[b].node;
					const anim::Matrix skin = node >= 0 && static_cast<size_t>(node) < model.pose.size()
						? anim::multiply(rig[b].meshToBone, model.pose[static_cast<size_t>(node)])
						: rig[b].meshToBone;
					std::copy(skin.begin(), skin.end(), transforms.parts[b].m.begin());
				}
			}
			SDL_PushGPUVertexUniformData(commands, 0, &transforms, pushUniformSize(transforms));
			LightingUniforms uniforms = lit[submesh.alphaReference > 0];
			materialUniforms(uniforms, submesh, 1.0f, seconds);
			specularUniforms(uniforms, submesh, camera, useLights);
			uniforms.material[2] = submesh.additive ? kMaterialAdditive : submesh.blended ? kMaterialBlended : 0.0f;
			if (gpuMesh->skinned || gpuMesh->bundled) {
				meshKindUniforms(uniforms, submesh, hemiAndShadows);
				// The first person's meshes go through the same draws (not in
				// the dynamic map: the bundled ones take it too).
				uniforms.layers[3] = simpleShadowFor(model.world, gpuMesh->skinned, false);
			}
			layerUniforms(uniforms, submesh, m_envMap != nullptr);
			SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof(uniforms));
			SDL_GPUTextureSamplerBinding samplers[5] = {
				{submesh.texture, m_gpu.repeatSampler()},
				{m_gpu.whiteTexture(), m_gpu.clampSampler()},
				{m_gpu.whiteTexture(), m_gpu.repeatSampler()},
				{submesh.normalMap ? submesh.normalMap : m_gpu.whiteTexture(), m_gpu.repeatSampler()},
				{m_gpu.whiteTexture(), m_gpu.repeatSampler()},
			};
			SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
			bindMeshLayers(pass, &submesh);
			SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
		}
	};
	// RSMT1Person: the opaque materials, and the additive ones (the
	// bundled mesh sends those to the first-person queue too, 0x100c3880;
	// drawn here after all the opaque ones).
	useModelPipeline(MeshQueue::Opaque, false);
	for (const FirstPersonModel& model : scene.models) {
		drawModel(model, MeshQueue::Opaque);
	}
	useModelPipeline(MeshQueue::Additive, false);
	for (const FirstPersonModel& model : scene.models) {
		drawModel(model, MeshQueue::Additive);
	}
	// The first-person particles (0x1002ea10 / 0x1002cef0 -> the particle
	// manager's slot 0x30, 0x10099860, after the first-person opaque
	// queues): unzoomed with the world's camera (renderer slot 0x5c), zoomed
	// with the first-person meshes' view (slot 0xdc, +0xfc); then
	// RSMT1PersonTransparency back to front (the sights' glass, the displays).
	const Mat4 particleViewProjection = m_firstPersonZoomed ? viewProjection
		: jittered(camera.view() * Mat4::perspective(camera.fovY, aspect, camera.nearPlane, 6000.0f));
	drawMeshParticles(commands, pass, particleViewProjection, lights, {scene.effects});
	drawParticles(commands, pass, particleViewProjection, camera, lights, m_firstPersonParticleVertices,
		m_firstPersonParticleGroups);
	std::vector<std::pair<float, const FirstPersonModel*>> transparent;
	for (const FirstPersonModel& model : scene.models) {
		transparent.push_back({transparencyKey(model.world, camera.position), &model});
	}
	sortBackToFront(transparent);
	modelBound = nullptr; // the particles'
	useModelPipeline(MeshQueue::Transparent, false);
	for (const auto& [distance, model] : transparent) {
		drawModel(*model, MeshQueue::Transparent);
	}
}

} // namespace dice::hfe::render

namespace dice::hfe::render {

float LevelRenderer::undergrowthViewDistance() const
{
	// undergrowth.viewDistanceScale (+0x578, clamped to 0..1 by 0x101162c0):
	// the quality switch's 0.5 / 0.75 / 1 (0x10010040) unless a script set it.
	const scene::RendererSettings* r = m_sources ? m_sources->renderer : nullptr;
	if (r && r->undergrowthViewDistanceScaleSet) {
		return m_undergrowthViewDistance * r->undergrowthViewDistanceScale;
	}
	return m_undergrowthViewDistance * (m_sources && m_sources->quality ? m_sources->quality->undergrowthViewDistanceScale() : 1.0f);
}

} // namespace dice::hfe::render
