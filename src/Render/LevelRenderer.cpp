#include "LevelRenderer.h"
#include "MeshQueues.h"

#include <array>

#include "Geometry/GeometryTemplateManager.h"
#include "Geometry/MeshFile.h"
#include "Geometry/RoadFile.h"
#include "IO/File/FileManager.h"
#include "Scene/EffectSettings.h"
#include "Scene/LightSettings.h"
#include "Scene/PostProductionSettings.h"
#include "Terrain/Heightmap.h"
#include "Terrain/TerrainData.h"
#include "Terrain/Undergrowth.h"
#include "Texture/TgaFile.h"
#include "Effects/Effects.h"
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

// Fog end until the view-distance scaling is known (see LightSettings.h).
constexpr float kFogEnd = 1400.0f;

LightingUniforms lighting(const scene::LightSettings& lights, const std::array<float, 3>& sunColor,
	const std::array<float, 3>& skyColor, float alphaReference, bool lightmapped)
{
	const Vec3f sun = normalize(Vec3f {-lights.sunDirection[0], -lights.sunDirection[1], -lights.sunDirection[2]});
	const auto& fog = lights.fogColor;
	const float on = lights.enableSun ? 1.0f : 0.0f; // lightManager.enableSun
	return LightingUniforms {
		{sun.x, sun.y, sun.z, 0},
		{sunColor[0] * on, sunColor[1] * on, sunColor[2] * on, 1},
		{skyColor[0], skyColor[1], skyColor[2], 1},
		{fog[0] / 255.0f, fog[1] / 255.0f, fog[2] / 255.0f, kFogEnd},
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
	uniforms.specular[3] = 0.15f;
}

// A bundled or skinned mesh's kind bits (mesh.frag): the hemi map (not at
// the low lighting level, which also drops their dynamic shadows: RaShaderBM
// and RaShaderSM's DeprecationList rows 1 and 2) and its normal map.
template <class S>
void meshKindUniforms(LightingUniforms& uniforms, const S& submesh, bool hemiAndShadows)
{
	uniforms.material[2] += hemiAndShadows ? kMaterialHemi : kMaterialNoShadows;
	if (submesh.normalMap) {
		uniforms.material[2] += kMaterialNormalMap;
	}
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
	if (m_overlayVertices) {
		SDL_ReleaseGPUBuffer(device, m_overlayVertices);
	}
	if (m_menuVertices) {
		SDL_ReleaseGPUBuffer(device, m_menuVertices);
	}
	for (const auto& [key, entry] : m_menuTextures) {
		if (entry.texture) {
			SDL_ReleaseGPUTexture(device, entry.texture);
		}
	}
	if (m_particleVertices) {
		SDL_ReleaseGPUBuffer(device, m_particleVertices);
	}
	if (m_shimmerVertices) {
		SDL_ReleaseGPUBuffer(device, m_shimmerVertices);
	}
	if (m_firstPersonParticleVertices) {
		SDL_ReleaseGPUBuffer(device, m_firstPersonParticleVertices);
	}
	if (m_decalVertices) {
		SDL_ReleaseGPUBuffer(device, m_decalVertices);
	}
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
}

SDL_GPUTexture* LevelRenderer::texture(const io::FileManager& files, const std::string& path,
	scene::TextureClass textureClass)
{
	// VideoSettings' texture quality: the class's mip skips (0x10017710).
	const int skip = m_sources && m_sources->quality ? m_sources->quality->skipMipLevels(textureClass) : 0;
	const std::string key = skip > 0 ? lower(path) + "#skip" + std::to_string(skip) : lower(path);
	const auto it = m_textures.find(key);
	if (it != m_textures.end()) {
		return it->second;
	}
	SDL_GPUTexture* gpuTexture = nullptr;
	std::vector<uint8_t> data;
	if (files.readFile(path, data)) {
		const bool targa = key.size() > 4 && key.compare(key.size() - 4, 4, ".tga") == 0;
		if (auto image = targa ? texture::parseTga(data) : texture::parseDds(data)) {
			texture::skipTopLevels(*image, skip);
			gpuTexture = m_gpu.createTexture(*image);
		}
	}
	++(gpuTexture ? m_stats.textures : m_stats.missingTextures);
	m_textures[key] = gpuTexture;
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
	if (files.readFile(path, data)) {
		if (const auto image = texture::parseDds(data)) {
			gpuTexture = m_gpu.createTexture(*image, true);
		}
	}
	m_cubeTextures[key] = gpuTexture;
	return gpuTexture;
}

const LevelRenderer::GpuMesh* LevelRenderer::mesh(const LevelSources& sources, const std::string& geometryName,
	int wantedGeometry, int wantedLod)
{
	const std::string key = wantedGeometry < 0 && wantedLod == 0
		? lower(geometryName)
		: lower(geometryName) + "#" + std::to_string(wantedGeometry) + "/" + std::to_string(wantedLod);
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
	// Bundled and skinned meshes' tangents (RaShaderBM/SM createWorld2TanMat
	// / getTangentBasis: the binormal cross(tangent, normal) times
	// 1 - 2 x the third blend index byte). They have no detail, dirt or
	// crack layers, so they carry them in those coordinates' place
	// (Vertex::dirtCoord xy, crackCoord x; the sign in crackCoord y).
	const geom::VertexElement* tangent = bundled || skinned ? file->element(geom::VertexUsage::Tangent) : nullptr;
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
		float t[3] = {1, 0, 0};
		if (tangent && file->readFloats(i, *tangent, t, 3)) {
			v.dirtCoord[0] = t[0];
			v.dirtCoord[1] = t[1];
			v.crackCoord[0] = t[2];
			v.crackCoord[1] = partIndex ? 1.0f - 2.0f * file->vertices[vertexOffset + partIndex->offset + 2] : 1.0f;
		}
		v.part = partIndex ? file->vertices[vertexOffset + partIndex->offset] : 0.0f;
		v.part2 = skinned && partIndex ? file->vertices[vertexOffset + partIndex->offset + 1] : v.part;
		v.weight = 1;
		if (blendWeight) {
			file->readFloats(i, *blendWeight, &v.weight, 1);
		}
	}
	auto gpuMesh = std::make_unique<GpuMesh>();
	gpuMesh->bundled = bundled;
	gpuMesh->skinned = skinned;
	gpuMesh->radiusShadowBounds = geometry->useRadiusForShadowDepth || geometry->useRadiusForShadowSpatial;
	gpuMesh->geometries = file->geometries.size();
	gpuMesh->lodCount = file->geometries[geometryIndex].lods.size();
	if (geometryIndex < geometry->lodDistances.size()) {
		gpuMesh->lodDistances = geometry->lodDistances[geometryIndex];
	}
	// The third-person and wreck geometries' skipped LODs (bundled
	// 0x100a31d0: of three geometries 1 takes maxSkip3pLods and 2
	// maxSkipWreckLods, of two 1 the former; skinned 0x100d4b..: the third
	// person's, geometry 0 of one and 1 of two), at most renderer +0x910 (the
	// geometry quality's) and one less than the LODs. Static meshes keep
	// theirs here (0x100dd480 also asks the template's +0xc8, not known).
	if ((bundled || skinned) && wantedLod == 0) {
		const size_t count = file->geometries.size();
		unsigned wanted = 0;
		if (bundled && count == 3) {
			wanted = geometryIndex == 1 ? geometry->maxSkip3pLods : geometryIndex == 2 ? geometry->maxSkipWreckLods : 0;
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
	const scene::TextureClass textureClass = skinned ? scene::TextureClass::SkinnedMesh
		: bundled                                    ? scene::TextureClass::BundledMesh
		: tree                                       ? scene::TextureClass::Overgrowth
													 : scene::TextureClass::StaticMesh;
	// The lighting quality's level (renderer +0x6b4): normal maps only at 0,
	// static meshes' dirt and crack layers not at 3 (QualitySettings).
	const scene::QualitySettings defaults;
	const scene::QualitySettings& quality = sources.quality ? *sources.quality : defaults;
	for (size_t m = 0; m < materials.size(); ++m) {
		const geom::MeshMaterial& material = materials[m];
		// Alpha-tested materials use "<name>_alphatest.dds" where it exists:
		// the plain textures keep alpha in 0..0.5 for alpha to coverage
		// (0x10151830).
		std::string diffusePath = material.maps.empty() ? std::string() : material.maps[0];
		if (material.alphaMode == 2 && diffusePath.size() > 4) {
			const std::string alphaTest = diffusePath.substr(0, diffusePath.size() - 4) + "_alphatest.dds";
			if (sources.files.exists(alphaTest)) {
				diffusePath = alphaTest;
			}
		}
		// The overgrowth's distant meshes use textures only its atlases hold.
		const AtlasEntry* atlas = nullptr;
		if (!diffusePath.empty() && !m_overgrowthAtlas.empty() && !sources.files.exists(diffusePath)) {
			std::string key = lower(material.maps[0]);
			std::replace(key.begin(), key.end(), '\\', '/');
			const auto& first = material.alphaMode == 2 ? m_overgrowthAlphaTestAtlas : m_overgrowthAtlas;
			const auto& second = material.alphaMode == 2 ? m_overgrowthAtlas : m_overgrowthAlphaTestAtlas;
			auto found = first.find(key);
			if (found != first.end()) {
				atlas = &found->second;
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
		// 0x100c3880: alpha mode 1 is blended (AlphaBlendEnable, no depth
		// writes, no culling, SrcBlend SRCALPHA / DestBlend INVSRCALPHA), or
		// with the template's hasAdditiveAlpha added (DestBlend ONE, the
		// BMadditive shader, no alpha test).
		submesh.blended = material.alphaMode == 1;
		submesh.additive = submesh.blended && geometry->hasAdditiveAlpha;
		if (submesh.additive) {
			submesh.uvScroll[0] = geometry->simpleUVTranslation[0];
			submesh.uvScroll[1] = geometry->simpleUVTranslation[1];
		}
		// StaticMesh.fx techniques name their layers: "BaseDetailDirtCrack..."
		// with maps base, detail, dirt, crack in that order (normal maps follow).
		const std::string technique = lower(material.technique);
		const std::string effect = lower(material.effect);
		if (!submesh.additive) {
			if (effect == "staticmesh.fx" && !skinned) {
				submesh.specularKind = technique.find("detail") != std::string::npos && material.alphaMode != 2 ? 2 : 1;
			} else if (effect == "bundledmesh.fx") {
				submesh.specularKind = technique.find("colormapgloss") != std::string::npos ? 4 : 3;
			}
		}
		// 0x100de8xx: a geometry whose path has "vegitation" is a tree
		// (+0x2a2); its alpha-tested materials are the leaves (RaShaderLeaf).
		submesh.leaf = tree && material.alphaMode == 2;
		if (lower(material.effect) == "staticmesh.fx" && technique.compare(0, 10, "basedetail") == 0) {
			const bool hasDirt = technique.find("dirt") != std::string::npos;
			const bool hasCrack = technique.find("crack") != std::string::npos;
			size_t map = 1;
			// The lighting level drops the dirt and crack maps (0x10151830
			// skips "_di" / "_cr" without their literals).
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
		}
		// Normal maps (RaShaderBM/SM _HASNORMALMAP_), only at the top
		// lighting level: the loaders skip "_b.dds" (bundled, 0x101499b0) and
		// "_b.dds" / "_b_os.dds" (skinned, 0x100d4160) otherwise. A bundled
		// mesh's second map ending "_b.dds" is its tangent-space normal map
		// (+0x35); a skinned mesh's second map, unless the SpecularLUT, is in
		// tangent space when its technique names "tangent" (+0x100), else in
		// the object's (_OBJSPACENORMALMAP_, the first person's arms).
		if ((bundled || skinned) && material.maps.size() > 1 && quality.meshNormalMaps() && !submesh.additive) {
			const std::string second = lower(material.maps[1]);
			const bool bump = second.size() >= 6 && second.compare(second.size() - 6, 6, "_b.dds") == 0;
			const bool skinnedMap = skinned && second.find("specularlut") == std::string::npos;
			if (bump || skinnedMap) {
				submesh.normalMap = texture(sources.files, material.maps[1], textureClass);
				if (submesh.normalMap) {
					submesh.normalKind = bundled || technique.find("tangent") != std::string::npos ? 1 : 2;
					++m_stats.normalMapped;
				}
			}
		}
		if (skinned && !submesh.additive) {
			submesh.specularKind = 5; // RaShaderSM: pow(N.H, SpecularPower) x the gloss
		}
		gpuMesh->submeshes.push_back(submesh);
	}
	++m_stats.meshes;
	slot = std::move(gpuMesh);
	return slot.get();
}

void LevelRenderer::addTemplate(const LevelSources& sources, const world::ObjectTemplate& objectTemplate,
	const Mat4& local, const Mat4& objectWorld, uint32_t object, int depth)
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
			if (gpuMesh->skinned) {
				const auto setting = objectTemplate.settings().find("skeleton3P");
				if (setting != objectTemplate.settings().end()) {
					instance.skeleton = skeleton(sources.files, setting->second);
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
	for (const world::ChildTemplate& child : objectTemplate.children()) {
		if (const world::ObjectTemplate* childTemplate = sources.templates.find(child.name);
			childTemplate && drawnWithObject(childTemplate->typeName())) {
			addTemplate(sources, *childTemplate, objectTransform(child.position, child.rotation) * local, objectWorld,
				object, depth + 1);
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
void LevelRenderer::collectParts(const LevelSources& sources, const world::ObjectTemplate& objectTemplate,
	const Mat4& relative, std::vector<Mat4>& parts, int depth, uint32_t object) const
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
		if (object != 0 && sources.partPose && sources.partPose(object, child.name, offset, rotation)) {
			local = objectTransform(offset, rotation) * local;
		}
		const auto part = childTemplate->settings().find("geometryPart");
		if (part != childTemplate->settings().end()) {
			const int index = std::atoi(part->second.c_str());
			if (index > 0 && index < kMaxMeshParts) {
				parts[static_cast<size_t>(index)] = local;
			}
		}
		collectParts(sources, *childTemplate, local, parts, depth + 1, object);
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
		for (size_t i = 0; i < data->layers.size() && i < kMaxDetailLayers; ++i) {
			m_detailLayers[i] = texture(sources.files, data->layers[i].texture + ".dds", scene::TextureClass::TerrainDetail);
			m_detailTiling[i] = data->layers[i].tiling;
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
	std::string text;
	if (m_overgrowthAtlas.empty() || !sources.files.readText(directory + "Overgrowth.con", text)) {
		return;
	}
	float viewDistance = 0, scale = 1, close = 0;
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
		}
	}
	m_overgrowthCloseDistance = viewDistance * scale * close;
	m_overgrowthCloseBase = viewDistance * close;
	m_overgrowthScriptScale = scale;
	m_overgrowthQualityScale = sources.quality ? sources.quality->overgrowthViewDistanceScale() : 1.0f;
}

float LevelRenderer::overgrowthCloseDistance() const
{
	if (m_sources && m_sources->quality) {
		const float quality = m_sources->quality->overgrowthViewDistanceScale();
		if (quality != m_overgrowthQualityScale) {
			return m_overgrowthCloseBase * quality;
		}
	}
	return m_overgrowthCloseBase * m_overgrowthScriptScale;
}

// The sea: a grid at sea level over each heightmap of the cluster (the
// primary and the surrounding ones), each vertex carrying the water's depth
// there (sea level minus the terrain's height) for its opacity, and a ring
// of deep water beyond them to the horizon (RendDX9's WaterSurrounding and
// WaterDistant shader variants draw those parts).
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
					look.uniforms = {{settings->blendFactor, settings->fadeStart, settings->fadeEnd, 0}};
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
	m_undergrowthAlphaRef = config.alphaRef;
	const std::string atlas = sources.levelPath + "UndergrowthAtlas_alphatest0.dds";
	m_undergrowthAtlas = texture(sources.files,
		sources.files.exists(atlas) ? atlas : sources.levelPath + "UndergrowthAtlas0.dds", scene::TextureClass::Undergrowth);
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
			for (size_t i = 0; i < vertices.size(); ++i) {
				const terrain::UndergrowthPatchVertex& source = patch.vertices[i];
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
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.undergrowthPipeline());
	const bool undergrowthShadows = !(m_sources && m_sources->shadows && !m_sources->shadows->undergrowthShadows);
	useShadows(commands, pass, 1, 3, false, undergrowthShadows);
	const auto& sun = m_terrainColorsFromData ? m_terrainSunColor : lights.terrainSunColor;
	const auto& gi = m_terrainColorsFromData ? m_terrainGIColor : lights.terrainGIColor;
	LightingUniforms lit = lighting(lights, sun, gi, m_undergrowthAlphaRef, true);
	LightingUniforms unlit = lighting(lights, sun, gi, m_undergrowthAlphaRef, false);

	UndergrowthUniforms uniforms {};
	uniforms.viewProjection = viewProjection;
	uniforms.cameraPosition[0] = camera.position.x;
	uniforms.cameraPosition[1] = camera.position.y;
	uniforms.cameraPosition[2] = camera.position.z;
	// Plants reach full height at (1 - fade scale) of the view distance.
	uniforms.fade[0] = undergrowthViewDistance();
	uniforms.fade[1] = 1.0f / std::max(undergrowthViewDistance() * m_undergrowthFadeScale, 0.01f);
	// undergrowth.simpleShaderEnable: the terrain quality's 1 / 1 / 0 (0x10010040, slot 0x40).
	uniforms.fade[2] = m_sources && m_sources->quality && m_sources->quality->undergrowthSimpleShader() ? 1.0f : 0.0f;
	// The engine animates 16 sway offsets; until that is decompiled, each
	// swings on its own phase, as far as SwayScale.
	const float seconds = static_cast<float>(SDL_GetTicks() - m_startTicks) / 1000.0f;
	for (int i = 0; i < 16; ++i) {
		const float phase = seconds * (1.3f + 0.11f * i) + i * 0.7f;
		uniforms.swayOffsets[i][0] = std::sin(phase) * m_undergrowthSwayScale;
		uniforms.swayOffsets[i][1] = std::cos(phase * 0.8f) * m_undergrowthSwayScale;
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
		SDL_PushGPUFragmentUniformData(commands, 0, patch.lightmap ? &lit : &unlit, sizeof(lit));
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
		for (size_t i = first; i < m_instances.size(); ++i) {
			m_instances[i].overgrowth = object.isOvergrowth() && m_overgrowthCloseDistance > 0;
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
		// The cockpit from inside: another geometry of its mesh.
		if (instance.mesh->geometries > 1) {
			int geometry = -1, lod = 0;
			if (!(m_sources->geometryFor && m_sources->geometryFor(instance.object, geometry, lod))) {
				geometry = -1;
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
			collectParts(*m_sources, *instance.partsTemplate, Mat4 {}, instance.parts, 0, instance.object);
		}
		if (instance.mesh->skinned && m_sources->animationFor) {
			const std::string path = m_sources->animationFor(instance.object);
			if (!path.empty()) {
				instance.animation = animation(m_sources->files, path);
			}
		}
	}
}

// The HUD's quads for this frame: two triangles each, rotated about their
// centres (or their setPictureNodeCenterPoint pivots), grouped by texture
// and blend effect in drawing order.
void LevelRenderer::buildOverlay()
{
	m_overlayGroups.clear();
	if (m_overlayVertices) {
		SDL_ReleaseGPUBuffer(m_gpu.device(), m_overlayVertices);
		m_overlayVertices = nullptr;
	}
	if (!m_sources || !m_sources->overlay) {
		return;
	}
	const uint64_t now = SDL_GetTicks();
	const float seconds = m_lastOverlayTicks ? static_cast<float>(now - m_lastOverlayTicks) / 1000.0f : 0.0f;
	m_lastOverlayTicks = now;
	std::vector<Vertex> vertices;
	for (const hud::HudQuad& q : m_sources->overlay(seconds)) {
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
		const uint32_t first = static_cast<uint32_t>(vertices.size());
		for (size_t i = 1; i + 1 < polygon.size(); ++i) {
			for (const auto& corner : {polygon[0], polygon[i], polygon[i + 1]}) {
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
	m_overlayVertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
		static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
	m_gpu.flushUploads();
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
	if (m_menuVertices) {
		SDL_ReleaseGPUBuffer(m_gpu.device(), m_menuVertices);
		m_menuVertices = nullptr;
	}
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
	m_menuVertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
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
	return view;
}

// The heat shimmer's sprites, drawn after the world over a copy of it.
void LevelRenderer::buildShimmer(const Camera& camera)
{
	m_shimmerGroups.clear();
	if (m_shimmerVertices) {
		SDL_ReleaseGPUBuffer(m_gpu.device(), m_shimmerVertices);
		m_shimmerVertices = nullptr;
	}
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
					v.part = sprite.size;
					v.part2 = sprite.rotation;
					vertices.push_back(v);
				}
			}
			m_shimmerGroups.push_back({image, false, first, static_cast<uint32_t>(vertices.size()) - first});
		});
	}
	if (!vertices.empty()) {
		m_shimmerVertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
			static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
	}
}

void LevelRenderer::buildParticleBuffer(std::initializer_list<const effects::EffectManager*> managers,
	SDL_GPUBuffer*& buffer, std::vector<ParticleGroup>& groups, const Camera& camera, bool weather)
{
	const Vec3f eye = camera.position;
	groups.clear();
	if (buffer) {
		SDL_ReleaseGPUBuffer(m_gpu.device(), buffer); // released once the GPU is done with it
		buffer = nullptr;
	}
	if (!m_sources) {
		return;
	}
	std::vector<Vertex> vertices;
	const effects::ParticleView view = particleView(camera);
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
	buffer = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
		static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
	m_gpu.flushUploads();
}

// The lights of this frame: the effect managers' LightSources and the
// placed objects' (the levels' light bundles), the ones nearest the camera
// first (the shaders take kMaxPointLights; RendDX9 picks per mesh,
// lightManager.maxLightsPer*Mesh, within lightCullDistance).
void LevelRenderer::collectPointLights(const Camera& camera)
{
	m_pointLights.clear();
	if (!m_sources) {
		return;
	}
	std::vector<std::pair<float, effects::PointLight>> found;
	const auto consider = [&](const effects::PointLight& light) {
		const Vec3f d = Vec3f {light.position.x, light.position.y, light.position.z} - camera.position;
		found.push_back({std::sqrt(dot(d, d)) - light.range2, light});
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
		light.range1 = placed.look.range1;
		light.range2 = placed.look.range2;
		light.spot = placed.look.spot;
		const effects::Vec3& a = placed.look.direction;
		light.direction = {a.x * world.at(0, 0) + a.y * world.at(1, 0) + a.z * world.at(2, 0),
			a.x * world.at(0, 1) + a.y * world.at(1, 1) + a.z * world.at(2, 1),
			a.x * world.at(0, 2) + a.y * world.at(1, 2) + a.z * world.at(2, 2)};
		light.cosInner = placed.look.cosInner;
		light.cosOuter = placed.look.cosOuter;
		// A static light (isDynamic 0) is in the lightmaps already (the
		// lightmap generator's lights): only unlightmapped surfaces take it.
		light.affectsLightmapped = placed.look.affectsLightmapped && placed.look.dynamic;
		consider(light);
	}
	std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	for (size_t i = 0; i < found.size() && i < static_cast<size_t>(kMaxPointLights); ++i) {
		m_pointLights.push_back(found[i].second);
	}
}

void LevelRenderer::applyPointLights(LightingUniforms& uniforms) const
{
	uniforms.lightCount[0] = static_cast<float>(m_pointLights.size());
	for (size_t i = 0; i < m_pointLights.size(); ++i) {
		const effects::PointLight& l = m_pointLights[i];
		const float position[4] = {l.position.x, l.position.y, l.position.z, l.range2};
		const float color[4] = {l.color.x, l.color.y, l.color.z, l.range1};
		const float spot[4] = {l.direction.x, l.direction.y, l.direction.z, l.spot ? l.cosOuter : -2.0f};
		const float extra[4] = {l.cosInner, l.affectsLightmapped ? 1.0f : 0.0f, 0, 0};
		std::copy(position, position + 4, uniforms.lightPosition[i]);
		std::copy(color, color + 4, uniforms.lightColor[i]);
		std::copy(spot, spot + 4, uniforms.lightSpot[i]);
		std::copy(extra, extra + 4, uniforms.lightExtra[i]);
	}
}

// The decals as squares on their surfaces, lifted off them a little.
void LevelRenderer::buildDecals()
{
	m_decalGroups.clear();
	if (m_decalVertices) {
		SDL_ReleaseGPUBuffer(m_gpu.device(), m_decalVertices);
		m_decalVertices = nullptr;
	}
	if (!m_sources || !m_sources->effects || m_sources->effects->decals().empty()) {
		return;
	}
	std::vector<Vertex> vertices;
	for (const effects::Decal& decal : m_sources->effects->decals()) {
		SDL_GPUTexture* image = texture(m_sources->files, decal.texture + ".dds");
		if (!image) {
			continue;
		}
		effects::Vec3 n = decal.normal, tangent = decal.tangent, at = decal.position;
		// Below VideoSettings' effects 2 objects get no decal sets of their own
		// (DecalManager +0x322e, 0x100e6b70; set by 0x10010040).
		if (decal.object != 0 && m_sources->quality && !m_sources->quality->decalsEnabled()) {
			continue;
		}
		if (decal.object != 0) {
			// On a moving object: from its space to the world's.
			const world::Object* holder = m_sources->objects.find(decal.object);
			if (!holder || holder->hidden()) {
				continue;
			}
			const Mat4 place = objectTransform(holder->position(), holder->rotation());
			const auto turn = [&place](const effects::Vec3& d) {
				return effects::Vec3 {d.x * place.at(0, 0) + d.y * place.at(1, 0) + d.z * place.at(2, 0),
					d.x * place.at(0, 1) + d.y * place.at(1, 1) + d.z * place.at(2, 1),
					d.x * place.at(0, 2) + d.y * place.at(1, 2) + d.z * place.at(2, 2)};
			};
			n = turn(n);
			tangent = turn(tangent);
			at = turn(at) + effects::Vec3 {place.at(3, 0), place.at(3, 1), place.at(3, 2)};
		}
		const effects::Vec3 u = tangent * (decal.size * 0.5f);
		const effects::Vec3 v = physics::cross(n, tangent) * (decal.size * 0.5f);
		const effects::Vec3 centre = at + n * 0.02f;
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
			vertex.weight = 1;
			vertices.push_back(vertex);
		}
		if (!m_decalGroups.empty() && m_decalGroups.back().texture == image) {
			m_decalGroups.back().vertexCount += 6;
		} else {
			m_decalGroups.push_back({image, false, first, 6});
		}
	}
	if (vertices.empty()) {
		m_decalGroups.clear();
		return;
	}
	m_decalVertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
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
			LightingUniforms base = lighting(lights, lights.sunColor, lights.skyColor, 0, false);
			applyPointLights(base);
			for (const effects::MeshSprite& particle : particles) {
				Mat4 world;
				for (int row = 0; row < 3; ++row) {
					const effects::Vec3& r = particle.rotation.rows[static_cast<size_t>(row)];
					world.at(row, 0) = r.x * particle.size;
					world.at(row, 1) = r.y * particle.size;
					world.at(row, 2) = r.z * particle.size;
				}
				world.at(3, 0) = particle.position.x;
				world.at(3, 1) = particle.position.y;
				world.at(3, 2) = particle.position.z;
				TransformUniforms transforms {world * viewProjection, world, {1, 1, 0, 0}, {}};
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
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
	const LightingUniforms fog = lighting(lights, lights.sunColor, lights.skyColor, 0, false);
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
	m_culledInstances = 0;
	const Frustum frustum = Frustum::fromViewProjection(viewProjection);
	bool uploaded = false;
	const scene::RendererSettings rendererDefaults;
	const scene::RendererSettings& renderer =
		m_sources && m_sources->renderer ? *m_sources->renderer : rendererDefaults;
	const float zoom = viewZoom(camera);
	const float closeOvergrowth = overgrowthCloseDistance();
	static const bool cloakAll = [] { // BF2142_POST_EFFECT=camo: every soldier cloaked (screenshots)
		const char* forced = std::getenv("BF2142_POST_EFFECT");
		return forced && std::string(forced) == "camo";
	}();
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
		const BoundingSphere sphere = instanceBounds(instance, *instance.mesh);
		if (!frustum.containsSphere(sphere.center, sphere.radius)) {
			++m_culledInstances;
			continue;
		}
		DrawItem item {&instance, instance.mesh, instance.lightmap, instance.lightmapTransform};
		const GpuMesh& base = *instance.mesh;
		if (instance.overgrowth && m_sources) {
			const Vec3f origin {instance.world.at(3, 0), instance.world.at(3, 1), instance.world.at(3, 2)};
			const Vec3f d = (sphere.radius >= 0 ? sphere.center : origin) - camera.position;
			if (dot(d, d) > closeOvergrowth * closeOvergrowth) {
				Instance::Lod& entry = instance.overgrowthLod;
				if (!entry.resolved) {
					entry.resolved = true;
					if (m_sources->geometries.find(instance.geometry + "_lod")) {
						entry.mesh = mesh(*m_sources, instance.geometry + "_lod");
					}
					uploaded = true;
				}
				if (entry.mesh) {
					item.mesh = entry.mesh;
					item.lightmap = nullptr;
					m_drawList.push_back(item);
					continue;
				}
			}
		}
		if (m_sources && instance.shownGeometry < 0 && base.lodCount > 1 && !instance.geometry.empty()) {
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
			const int lod = base.skippedLods + lodForDistance(distance, distances, kept, scale);
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
		m_drawList.push_back(item);
	}
	if (uploaded) {
		m_gpu.flushUploads();
	}
}

void LevelRenderer::build(const LevelSources& sources)
{
	m_startTicks = SDL_GetTicks();
	m_sources = &sources; // the quality levels the loaders read
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
	loadTitanHemiMaps(sources);
	loadPostTextures(sources);
	buildWater(sources);
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
	float x = view.x, z = view.z;
	if (m_terrainExtent > 160) {
		x = std::clamp(x, m_patchOrigin[0] + 80, m_patchOrigin[0] + m_terrainExtent - 80);
		z = std::clamp(z, m_patchOrigin[1] + 80, m_patchOrigin[1] + m_terrainExtent - 80);
	}
	const terrain::Heightmap* heightmap = m_sources ? m_sources->heightmaps.primary() : nullptr;
	float ground = 0;
	if (heightmap) {
		ground = (heightmap->heightAt(x, z) + heightmap->heightAt(x, z + 2) + heightmap->heightAt(x, z - 2) +
					 heightmap->heightAt(x + 2, z) + heightmap->heightAt(x - 2, z)) *
			0.2f;
	}
	ground = std::max(ground, m_terrainWater[3]);
	Camera camera;
	camera.position = {x, ground + view.height, z};
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

namespace {

// A point in a Titan hemi map's space: inverse(TitanWorld) then the
// map's offset taken off (0x100eda70).
Vec3f titanMapLocal(const Vec3f& origin, const Vec3f axes[3], const float offset[3], const Vec3f& point)
{
	const Vec3f d = point - origin;
	return {dot(d, axes[0]) - offset[0], dot(d, axes[1]) - offset[1], dot(d, axes[2]) - offset[2]};
}

} // namespace

void LevelRenderer::placeTitanHemiMaps(const Camera& camera)
{
	m_cameraInTitan = false;
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
		// The exe asks the game which Titan the camera's soldier is in, else
		// tests the camera against the Titans (0x006fa8b0); the rewrite takes
		// the camera over a Titan map's square.
		const Vec3f local = titanMapLocal(map.origin, map.axes, map.offset, camera.position);
		if (local.y > 0 && std::fabs(local.x) < map.size / 2 && std::fabs(local.z) < map.size / 2) {
			m_cameraInTitan = true;
		}
	}
}

void LevelRenderer::hemiObjectFor(const Instance& instance, const GpuMesh& mesh, float out[4]) const
{
	const Vec3f origin {instance.world.at(3, 0), instance.world.at(3, 1), instance.world.at(3, 2)};
	// HeightOverTerrain (BM 0x100c2b70, SM 0x10160e90): the height over
	// what a ray 4 radii down meets, less a radius, in radii (at least
	// 2 m); the rewrite's ray meets only the terrain.
	const float radius = mesh.bounds.radius > 0 ? mesh.bounds.radius : 1.0f;
	const terrain::Heightmap* heightmap = m_sources ? m_sources->heightmaps.primary() : nullptr;
	float height = 4 * radius;
	if (heightmap) {
		const float below = origin.y - heightmap->heightAt(origin.x, origin.z);
		if (below >= 0 && below < 4 * radius) {
			height = below;
		}
	}
	out[3] = std::max(height - radius, 0.0f) / std::max(radius, 2.0f);
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
		if (local.y > 0 && std::fabs(local.x) < map.size / 2 && std::fabs(local.z) < map.size / 2 &&
			(!best || local.y < bestLocal.y)) {
			best = &map;
			bestLocal = local;
		}
	}
	if (best && best->width >= 2 && best->height >= 2) {
		const float scaleU = static_cast<float>(best->width) / best->size;
		const float scaleV = static_cast<float>(best->height) / best->size;
		const float u = std::clamp((best->size / 2 - bestLocal.x) * scaleU, 0.0f, static_cast<float>(best->width - 2));
		const float v = std::clamp((best->size / 2 + bestLocal.z) * scaleV, 0.0f, static_cast<float>(best->height - 2));
		const uint32_t x = static_cast<uint32_t>(u), y = static_cast<uint32_t>(v);
		const float fx = u - static_cast<float>(x), fy = v - static_cast<float>(y);
		for (int c = 0; c < 3; ++c) {
			const auto at = [&](uint32_t tx, uint32_t ty) {
				return static_cast<float>(best->rgba[(static_cast<size_t>(ty) * best->width + tx) * 4 + static_cast<size_t>(c)]) / 255.0f;
			};
			const float top = at(x, y) * (1 - fx) + at(x + 1, y) * fx;
			const float bottom = at(x, y + 1) * (1 - fx) + at(x + 1, y + 1) * fx;
			instance.titanHemi[c] = top * (1 - fy) + bottom * fy;
		}
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
	const Camera camera = overheadOn ? overheadCamera(overhead) : viewCamera;
	// With the post-production the world is drawn into the scene texture,
	// then post.frag into `color` with the HUD over it.
	const bool post = postEnabled() && m_gpu.postPipeline();
	if (post) {
		ensurePostTargets(target.width, target.height);
	}
	SDL_GPUColorTargetInfo colorTarget {};
	colorTarget.texture = post && m_sceneColor ? m_sceneColor : color;
	colorTarget.clear_color = {lights.fogColor[0] / 255.0f, lights.fogColor[1] / 255.0f, lights.fogColor[2] / 255.0f, 1.0f};
	colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
	colorTarget.store_op = SDL_GPU_STOREOP_STORE;
	SDL_GPUDepthStencilTargetInfo depthTarget {};
	depthTarget.texture = target.depth;
	depthTarget.clear_depth = 1.0f;
	depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
	depthTarget.store_op = SDL_GPU_STOREOP_DONT_CARE;
	depthTarget.stencil_load_op = SDL_GPU_LOADOP_CLEAR; // the menus' masks start from 0
	depthTarget.clear_stencil = 0;
	depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	updateUndergrowth(camera);
	buildParticles(camera);
	buildDecals();
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
	m_hasFirstPerson = !overheadOn && m_sources && m_sources->firstPerson && m_sources->firstPerson(m_firstPerson);
	if (m_hasFirstPerson) {
		for (const FirstPersonModel& model : m_firstPerson.models) {
			mesh(*m_sources, model.geometry, model.geometryIndex, model.lod);
		}
		m_gpu.flushUploads();
	}
	buildParticleBuffer({m_hasFirstPerson ? m_firstPerson.effects : nullptr}, m_firstPersonParticleVertices,
		m_firstPersonParticleGroups, camera);
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
	const Mat4 viewProjection = camera.view() *
		(overheadOn ? overheadProjection(overhead, static_cast<float>(target.width), static_cast<float>(target.height),
						  camera.nearPlane, 6000.0f)
					: Mat4::perspective(camera.fovY, static_cast<float>(target.width) / target.height, camera.nearPlane,
						  6000.0f));
	prepareInstances(camera, viewProjection);
	placeTitanHemiMaps(camera);
	const float seconds = static_cast<float>(SDL_GetTicks() - m_startTicks) / 1000.0f;
	m_poses.clear();
	// The shadow maps, before the world pass that reads them.
	renderShadows(commands, camera, lights, seconds);
	// The heat shimmer reads a copy of the scene: the world pass ends before it.
	const bool shimmer = post && m_sceneColor && !m_shimmerGroups.empty() && m_gpu.shimmerPipeline();
	if (!m_cloakedList.empty() || shimmer) {
		depthTarget.store_op = SDL_GPU_STOREOP_STORE; // the camouflage mask tests against it
		depthTarget.stencil_store_op = SDL_GPU_STOREOP_STORE;
	}
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colorTarget, 1, &depthTarget);

	// Sky dome around the camera, unlit and unfogged, behind everything.
	drawSky(commands, pass, viewProjection, camera);

	// Terrain.
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.terrainPipeline());
	useShadows(commands, pass, 2, 5 + kMaxDetailLayers, false);
	// The terrain's sun and GI colors come from terraindata.raw; the
	// script values are the editor's (the game doesn't have them).
	const auto& terrainSun = m_terrainColorsFromData ? m_terrainSunColor : lights.terrainSunColor;
	const auto& terrainGI = m_terrainColorsFromData ? m_terrainGIColor : lights.terrainGIColor;
	LightingUniforms terrainLit = lighting(lights, terrainSun, terrainGI, 0, true);
	LightingUniforms terrainUnlit = lighting(lights, terrainSun, terrainGI, 0, false);
	applyPointLights(terrainLit);
	applyPointLights(terrainUnlit);
	TransformUniforms transforms {viewProjection, Mat4 {}, {1, 1, 0, 0}, {}};
	SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
	const Frustum frustum = Frustum::fromViewProjection(viewProjection);
	TerrainDetailUniforms detail {};
	for (int i = 0; i < kMaxDetailLayers; ++i) {
		detail.tiling[i] = m_detailTiling[i];
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
	const scene::RendererSettings rendererDefaults;
	const scene::RendererSettings& renderer =
		m_sources && m_sources->renderer ? *m_sources->renderer : rendererDefaults;
	if (!m_terrainCells.empty()) {
		updateTerrainCells(camera, frustum);
	} else {
		m_terrainLodSwitch = terrainLodSwitch(viewZoom(camera), renderer);
		const terrain::Heightmap* heightmap = m_sources ? m_sources->heightmaps.primary() : nullptr;
		m_terrainHeightAboveGround = std::fabs(camera.position.y -
			(heightmap ? heightmap->heightAt(camera.position.x, camera.position.z) : 0.0f));
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
	if (!m_terrainCells.empty()) {
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
	if (!m_surrounding.empty()) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.surroundingPipeline());
		SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
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
	if (!m_roads.empty() && m_patchSize > 0) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.roadPipeline());
		useShadows(commands, pass, 2, 3, false);
		for (const Road& road : m_roads) {
			SDL_GPUBufferBinding vertexBinding {road.vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
			SDL_GPUBufferBinding indexBinding {road.indices, 0};
			SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
			SDL_PushGPUFragmentUniformData(commands, 1, &road.uniforms, sizeof(road.uniforms));
			const auto patchIndex = [this](float world, int axis) {
				return static_cast<int>(std::floor((world - m_patchOrigin[axis]) / m_patchSize));
			};
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
					SDL_PushGPUVertexUniformData(commands, 0, &roadTransforms, sizeof(roadTransforms));
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
	}

	drawUndergrowth(commands, pass, viewProjection, camera, lights);

	// Objects: the opaque materials first; then, after the water, the
	// transparent ones (blended materials, and every material of an object
	// fading out) sorted back to front, the additive ones, and the
	// particles: RendDX9's queue order (0x1002cef0: RSMTWater, then
	// RSMTTransparency sorted by distance, RSMTAdditive, the particles).
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.meshPipeline());
	useShadows(commands, pass, 1, 5, true);
	// Lightmapped statics use the static sun/sky colors.
	LightingUniforms uniformsFor[2][2] = {
		{lighting(lights, lights.sunColor, lights.skyColor, 0, false),
			lighting(lights, lights.sunColor, lights.skyColor, 0.5f, false)},
		{lighting(lights, lights.staticSunColor, lights.staticSkyColor, 0, true),
			lighting(lights, lights.staticSunColor, lights.staticSkyColor, 0.5f, true)},
	};
	for (auto& row : uniformsFor) {
		for (LightingUniforms& uniforms : row) {
			applyPointLights(uniforms);
		}
	}
	const float windSpeed = m_sources && m_sources->wind ? m_sources->wind->speedAt() : 0.0f;
	// The lighting level's bundled and skinned meshes (QualitySettings).
	const bool hemiAndShadows = !m_sources || !m_sources->quality || m_sources->quality->meshHemiAndShadows();
	const GpuMesh* bound = nullptr;
	const auto drawInstance = [&](const DrawItem& item, MeshQueue queue) {
		const Instance& instance = *item.instance;
		bool any = false;
		for (const Submesh& submesh : item.mesh->submeshes) {
			any = any || queueOf(submesh, instance.fade) == queue;
		}
		if (!any) {
			return;
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
		for (int i = 0; i < kMaxMeshParts; ++i) {
			transforms.parts[i] = instance.parts.empty() ? Mat4 {} : instance.parts[static_cast<size_t>(i)];
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
		// Bundled meshes' normal maps are in tangent space; a skinned mesh's
		// material says (its normalKind).
		transforms.normalSpace[0] = bound->bundled ? 1.0f : 0.0f;
		SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
		bool swaying = false;
		const bool lightmapped = item.lightmap != nullptr;
		float hemiObject[4] = {1, 1, 1, 0};
		if (bound->skinned || bound->bundled) {
			hemiObjectFor(instance, *bound, hemiObject);
		}
		static const std::vector<anim::Matrix> unposed;
		const std::vector<anim::Matrix>& pose = bound->skinned ? poseFor(instance, seconds) : unposed;
		for (const Submesh& submesh : bound->submeshes) {
			if (queueOf(submesh, instance.fade) != queue) {
				continue;
			}
			const float space = bound->skinned ? static_cast<float>(submesh.normalKind) : transforms.normalSpace[0];
			if (submesh.rig >= 0 && !pose.empty()) {
				skinPalette(*bound, submesh, pose, transforms);
				transforms.normalSpace[0] = space;
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
			} else if (space != transforms.normalSpace[0]) {
				transforms.normalSpace[0] = space;
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
			}
			const bool sway = submesh.leaf && !instance.overgrowth;
			if (sway != swaying) {
				swaying = sway;
				transforms.sway[0] = sway ? 1.0f : 0.0f;
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
			}
			LightingUniforms uniforms = uniformsFor[lightmapped][submesh.alphaReference > 0];
			materialUniforms(uniforms, submesh, instance.fade, seconds);
			specularUniforms(uniforms, submesh, camera, lights);
			if (bound->skinned || bound->bundled) {
				meshKindUniforms(uniforms, submesh, hemiAndShadows);
				std::copy(hemiObject, hemiObject + 4, uniforms.hemiObject);
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
			SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
		}
	};
	for (const DrawItem& item : m_drawList) {
		drawInstance(item, MeshQueue::Opaque);
	}
	// Decals over the opaque surfaces.
	if (m_decalVertices && !m_decalGroups.empty()) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.decalPipeline());
		const LightingUniforms decalLighting = lighting(lights, lights.sunColor, lights.skyColor, 0, false);
		SDL_PushGPUFragmentUniformData(commands, 0, &decalLighting, sizeof(decalLighting));
		TransformUniforms decalTransforms {viewProjection, Mat4 {}, {1, 1, 0, 0}, {}};
		SDL_PushGPUVertexUniformData(commands, 0, &decalTransforms, sizeof(decalTransforms));
		SDL_GPUBufferBinding vertexBinding {m_decalVertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		for (const ParticleGroup& group : m_decalGroups) {
			SDL_GPUTextureSamplerBinding sampler {group.texture, m_gpu.clampSampler()};
			SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
			SDL_DrawGPUPrimitives(pass, group.vertexCount, 1, group.firstVertex, 0);
		}
		bound = nullptr;
	}

	// Water, blended over what is below it.
	if (!m_waterBlocks.empty() && m_water) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.waterPipeline());
		const LightingUniforms waterLighting = lighting(lights, lights.sunColor, lights.skyColor, 0, false);
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
			{m_water->color[0], m_water->color[1], m_water->color[2], 1},
			{m_water->specularColor[0], m_water->specularColor[1], m_water->specularColor[2], m_water->specularColor[3]},
			{m_water->alphaAdd, m_water->alphaHeightMul, m_water->specularPower, m_waterEnvMap ? 1.0f : 0.0f},
			{m_water->scroll[0], m_water->scroll[1], cycle, m_waterVolume ? 1.0f : 0.0f},
		};
		SDL_PushGPUFragmentUniformData(commands, 1, &water, sizeof(water));
		TransformUniforms waterTransforms {viewProjection, Mat4 {}, {1, 1, 0, 0}, {}};
		SDL_PushGPUVertexUniformData(commands, 0, &waterTransforms, sizeof(waterTransforms));
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

	// The sun flare, behind what is in front of it.
	drawSunFlare(commands, pass, viewProjection, camera);
	// The transparent materials, farthest object first.
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.fadedMeshPipeline());
	useShadows(commands, pass, 1, 5, true);
	std::vector<std::pair<float, const DrawItem*>> transparent;
	for (const DrawItem& item : m_drawList) {
		transparent.push_back({transparencyKey(item.instance->world, camera.position), &item});
	}
	sortBackToFront(transparent);
	for (const auto& [distance, item] : transparent) {
		drawInstance(*item, MeshQueue::Transparent);
	}
	// The additive ones (not sorted: RSMTAdditive keeps its order).
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.additiveMeshPipeline());
	useShadows(commands, pass, 1, 5, true);
	for (const DrawItem& item : m_drawList) {
		drawInstance(item, MeshQueue::Additive);
	}
	// Particles, after what they are in front of: the mesh particles, then the sprites.
	drawMeshParticles(commands, pass, viewProjection, lights,
		{m_sources ? m_sources->effects : nullptr, m_sources ? m_sources->objectEffects : nullptr});
	drawParticles(commands, pass, viewProjection, camera, lights, m_particleVertices, m_particleGroups);
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
		SDL_GPUDepthStencilTargetInfo keepDepth = depthTarget;
		keepDepth.load_op = SDL_GPU_LOADOP_LOAD;
		keepDepth.stencil_load_op = SDL_GPU_LOADOP_LOAD;
		pass = SDL_BeginGPURenderPass(commands, &keepColor, 1, &keepDepth);
		drawShimmer(commands, pass, viewProjection, camera, target.width, target.height);
	}
	// The cloaked soldiers' mask, tested against the world's depth.
	const bool cloaked = post && !m_cloakedList.empty();
	if (cloaked) {
		SDL_EndGPURenderPass(pass);
		drawCamouflage(commands, target.depth, viewProjection, seconds);
		SDL_GPUColorTargetInfo keepColor = colorTarget;
		keepColor.load_op = SDL_GPU_LOADOP_LOAD;
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
		const PostUniforms postUniforms = preparePost(commands, cloaked);
		SDL_GPUColorTargetInfo finalColor {};
		finalColor.texture = color;
		finalColor.load_op = SDL_GPU_LOADOP_DONT_CARE;
		finalColor.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPUDepthStencilTargetInfo hudDepth = depthTarget; // the menus' masks start from 0
		hudDepth.store_op = SDL_GPU_STOREOP_DONT_CARE;
		hudDepth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		pass = SDL_BeginGPURenderPass(commands, &finalColor, 1, &hudDepth);
		drawPost(commands, pass, postUniforms);
	}
	// The HUD over everything.
	if (m_overlayVertices && !m_overlayGroups.empty()) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.uiPipeline());
		const float screen[4] = {800, 600, 0, 0};
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
			SDL_GPUTextureSamplerBinding sampler {group.texture, group.repeat ? m_gpu.repeatSampler() : m_gpu.clampSampler()};
			SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
			SDL_DrawGPUPrimitives(pass, group.vertexCount, 1, group.firstVertex, 0);
		}
	}
	SDL_EndGPURenderPass(pass);
}

void LevelRenderer::drawFirstPerson(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Camera& camera,
	float aspect, const scene::LightSettings& lights)
{
	const FirstPersonScene& scene = m_firstPerson;
	const Mat4 viewProjection = camera.view() * Mat4::perspective(scene.fovY, aspect, scene.nearPlane, scene.farPlane);
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
	LightingUniforms lit[2] = {lighting(useLights, sunColor, skyColor, 0, false),
		lighting(useLights, sunColor, skyColor, 0.5f, false)};
	if (!insideSun || inside.dynamicLights) {
		applyPointLights(lit[0]);
		applyPointLights(lit[1]);
	}
	const float seconds = static_cast<float>(SDL_GetTicks() - m_startTicks) / 1000.0f;
	const bool hemiAndShadows = !m_sources || !m_sources->quality || m_sources->quality->meshHemiAndShadows();
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
		SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
		for (const Submesh& submesh : gpuMesh->submeshes) {
			if (queueOf(submesh, 1.0f) != queue) {
				continue;
			}
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
			SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
			LightingUniforms uniforms = lit[submesh.alphaReference > 0];
			materialUniforms(uniforms, submesh, 1.0f, seconds);
			specularUniforms(uniforms, submesh, camera, useLights);
			uniforms.material[2] = submesh.additive ? kMaterialAdditive : submesh.blended ? kMaterialBlended : 0.0f;
			if (gpuMesh->skinned || gpuMesh->bundled) {
				meshKindUniforms(uniforms, submesh, hemiAndShadows);
			}
			SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof(uniforms));
			SDL_GPUTextureSamplerBinding samplers[5] = {
				{submesh.texture, m_gpu.repeatSampler()},
				{m_gpu.whiteTexture(), m_gpu.clampSampler()},
				{m_gpu.whiteTexture(), m_gpu.repeatSampler()},
				{submesh.normalMap ? submesh.normalMap : m_gpu.whiteTexture(), m_gpu.repeatSampler()},
				{m_gpu.whiteTexture(), m_gpu.repeatSampler()},
			};
			SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
			SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
		}
	};
	// RSMT1Person: the opaque materials, and the additive ones (the
	// bundled mesh sends those to the first-person queue too, 0x100c3880;
	// drawn here after all the opaque ones).
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.meshPipeline());
	useShadows(commands, pass, 1, 5, true);
	for (const FirstPersonModel& model : scene.models) {
		drawModel(model, MeshQueue::Opaque);
	}
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.additiveMeshPipeline());
	useShadows(commands, pass, 1, 5, true);
	for (const FirstPersonModel& model : scene.models) {
		drawModel(model, MeshQueue::Additive);
	}
	// The first-person particles, then RSMT1PersonTransparency back to front
	// (the sights' glass, the displays).
	drawMeshParticles(commands, pass, viewProjection, lights, {scene.effects});
	drawParticles(commands, pass, viewProjection, camera, lights, m_firstPersonParticleVertices,
		m_firstPersonParticleGroups);
	std::vector<std::pair<float, const FirstPersonModel*>> transparent;
	for (const FirstPersonModel& model : scene.models) {
		transparent.push_back({transparencyKey(model.world, camera.position), &model});
	}
	sortBackToFront(transparent);
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.fadedMeshPipeline());
	useShadows(commands, pass, 1, 5, true);
	for (const auto& [distance, model] : transparent) {
		drawModel(*model, MeshQueue::Transparent);
	}
}

} // namespace dice::hfe::render

namespace dice::hfe::render {

float LevelRenderer::undergrowthViewDistance() const
{
	return m_undergrowthViewDistance * (m_sources && m_sources->quality ? m_sources->quality->undergrowthViewDistanceScale() : 1.0f);
}

} // namespace dice::hfe::render
