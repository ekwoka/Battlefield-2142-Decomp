#include "LevelRenderer.h"

#include <array>

#include "Geometry/GeometryTemplateManager.h"
#include "Geometry/MeshFile.h"
#include "Geometry/RoadFile.h"
#include "IO/File/FileManager.h"
#include "Scene/LightSettings.h"
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
#include <optional>
#include <sstream>

namespace dice::hfe::render {

namespace {

constexpr int kDefaultTerrainPatches = 8; // colormaps per side without terrain data
constexpr float kDetailFadeStart = 60.0f;
constexpr float kDetailFadeEnd = 140.0f;

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
	return LightingUniforms {
		{sun.x, sun.y, sun.z, 0},
		{sunColor[0], sunColor[1], sunColor[2], 1},
		{skyColor[0], skyColor[1], skyColor[2], 1},
		{fog[0] / 255.0f, fog[1] / 255.0f, fog[2] / 255.0f, kFogEnd},
		{alphaReference, lightmapped ? 1.0f : 0.0f, 0, 0},
	};
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
	return Mat4::lookAt(position, position + forward(), {0, 1, 0});
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
	if (m_waterVertices) {
		SDL_ReleaseGPUBuffer(device, m_waterVertices);
		SDL_ReleaseGPUBuffer(device, m_waterIndices);
	}
	if (m_terrainIndices) {
		SDL_ReleaseGPUBuffer(device, m_terrainIndices);
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
	if (m_firstPersonParticleVertices) {
		SDL_ReleaseGPUBuffer(device, m_firstPersonParticleVertices);
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
}

SDL_GPUTexture* LevelRenderer::texture(const io::FileManager& files, const std::string& path)
{
	const std::string key = lower(path);
	const auto it = m_textures.find(key);
	if (it != m_textures.end()) {
		return it->second;
	}
	SDL_GPUTexture* gpuTexture = nullptr;
	std::vector<uint8_t> data;
	if (files.readFile(path, data)) {
		const bool targa = key.size() > 4 && key.compare(key.size() - 4, 4, ".tga") == 0;
		if (const auto image = targa ? texture::parseTga(data) : texture::parseDds(data)) {
			gpuTexture = m_gpu.createTexture(*image);
		}
	}
	++(gpuTexture ? m_stats.textures : m_stats.missingTextures);
	m_textures[key] = gpuTexture;
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
		SDL_GPUTexture* diffuse = diffusePath.empty() ? nullptr : texture(sources.files, diffusePath);
		Submesh submesh {material.indexStart, material.indexCount, static_cast<int32_t>(material.vertexStart),
			diffuse ? diffuse : m_gpu.whiteTexture(), material.alphaMode == 2 ? 0.5f : 0.0f,
			skinned && m < gpuMesh->rigs.size() ? static_cast<int>(m) : -1};
		submesh.blended = material.alphaMode == 1;
		// StaticMesh.fx techniques name their layers: "BaseDetailDirtCrack..."
		// with maps base, detail, dirt, crack in that order (normal maps follow).
		const std::string technique = lower(material.technique);
		if (lower(material.effect) == "staticmesh.fx" && technique.compare(0, 10, "basedetail") == 0) {
			const bool hasDirt = technique.find("dirt") != std::string::npos;
			const bool hasCrack = technique.find("crack") != std::string::npos;
			size_t map = 1;
			const auto next = [&]() -> SDL_GPUTexture* {
				return map < material.maps.size() ? texture(sources.files, material.maps[map++]) : nullptr;
			};
			submesh.detail = next();
			submesh.dirt = hasDirt ? next() : nullptr;
			submesh.crack = hasCrack ? next() : nullptr;
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
	if (!objectTemplate.geometry().empty()) {
		if (const GpuMesh* gpuMesh = mesh(sources, objectTemplate.geometry())) {
			const Mat4 world = local * objectWorld;
			Instance instance {gpuMesh, world, object, local, nullptr, {1, 1, 0, 0}, {}, nullptr, nullptr};
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
			// Lightmaps are named after the geometry, LOD and the position
			// truncated to whole metres.
			std::ostringstream key;
			key << lower(objectTemplate.geometry()) << "=00=" << static_cast<int>(world.at(3, 0)) << '='
				<< static_cast<int>(world.at(3, 1)) << '=' << static_cast<int>(world.at(3, 2));
			const auto entry = m_lightmapAtlas.find(key.str());
			if (entry != m_lightmapAtlas.end()) {
				instance.lightmap = texture(sources.files, entry->second.atlas);
				if (instance.lightmap) {
					instance.lightmapTransform[0] = entry->second.size[0];
					instance.lightmapTransform[1] = entry->second.size[1];
					instance.lightmapTransform[2] = entry->second.offset[0];
					instance.lightmapTransform[3] = entry->second.offset[1];
					++m_stats.lightmappedInstances;
				}
			}
			m_instances.push_back(instance);
		}
	}
	for (const world::ChildTemplate& child : objectTemplate.children()) {
		if (const world::ObjectTemplate* childTemplate = sources.templates.find(child.name)) {
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
			m_detailLayers[i] = texture(sources.files, data->layers[i].texture + ".dds");
			m_detailTiling[i] = data->layers[i].tiling;
		}
	}
	const int samples = heightmap->width();
	const int quads = (samples - 1) / patchesPerSide;
	const int side = quads + 1;
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

	const auto& scale = heightmap->scale();
	m_patchesPerSide = patchesPerSide;
	m_patchOrigin[0] = heightmap->originX();
	m_patchOrigin[1] = heightmap->originZ();
	m_patchSize = quads * scale[0];
	for (int py = 0; py < patchesPerSide; ++py) {
		for (int px = 0; px < patchesPerSide; ++px) {
			std::vector<Vertex> vertices(static_cast<size_t>(side) * side);
			for (int y = 0; y < side; ++y) {
				for (int x = 0; x < side; ++x) {
					const int sx = px * quads + x;
					const int sy = py * quads + y;
					Vertex& v = vertices[static_cast<size_t>(y) * side + x];
					v.position[0] = heightmap->originX() + sx * scale[0];
					v.position[1] = heightmap->sample(sx, sy);
					v.position[2] = heightmap->originZ() + sy * scale[2];
					const float dx = heightmap->sample(sx + 1, sy) - heightmap->sample(sx - 1, sy);
					const float dz = heightmap->sample(sx, sy + 1) - heightmap->sample(sx, sy - 1);
					const Vec3f n = normalize({-dx, 2 * scale[0], -dz});
					v.normal[0] = n.x;
					v.normal[1] = n.y;
					v.normal[2] = n.z;
					// Colormap and lightmap texels run along +x and +z.
					v.texCoord[0] = static_cast<float>(x) / quads;
					v.texCoord[1] = static_cast<float>(y) / quads;
					v.lightmapCoord[0] = v.texCoord[0];
					v.lightmapCoord[1] = v.texCoord[1];
				}
			}
			// "<base><x patch>x<z patch>", counted from the -x/-z corner
			// (checked against the building footprints painted into the
			// colormaps); detail maps add "_1", "_2" (0x1014b6a0).
			char name[32];
			std::snprintf(name, sizeof(name), "%02dx%02d", px, py);
			TerrainPatch patch;
			patch.vertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
				static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
			patch.colormap = texture(sources.files, colormapBase + name + ".dds");
			patch.lightmap = texture(sources.files, lightmapBase + name + ".dds");
			if (data) {
				for (int map = 0; map < 2; ++map) {
					const std::string path = data->detailmapBaseName + name + "_" + std::to_string(map + 1) + ".dds";
					if (sources.files.exists(path)) {
						patch.detailWeights[map] = texture(sources.files, path);
					}
				}
			}
			m_terrain.push_back(patch);
		}
	}
	m_stats.terrainPatches = m_terrain.size();
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

// A grid over the primary heightmap at sea level; each vertex carries the
// water depth (sea level minus terrain height) for the opacity.
void LevelRenderer::buildWater(const LevelSources& sources)
{
	const terrain::Heightmap* heightmap = sources.heightmaps.primary();
	if (!heightmap || !heightmap->loaded() || !sources.heightmaps.hasSea()) {
		return;
	}
	m_water = &sources.water;
	constexpr int kCells = 128;
	const float level = sources.heightmaps.seaWaterLevel();
	const float width = heightmap->worldWidth();
	const float depthSpan = heightmap->worldDepth();
	std::vector<Vertex> vertices;
	vertices.reserve((kCells + 1) * (kCells + 1));
	for (int y = 0; y <= kCells; ++y) {
		for (int x = 0; x <= kCells; ++x) {
			Vertex v {};
			v.position[0] = heightmap->originX() + width * x / kCells;
			v.position[1] = level;
			v.position[2] = heightmap->originZ() + depthSpan * y / kCells;
			v.normal[1] = 1;
			v.texCoord[0] = level - heightmap->heightAt(v.position[0], v.position[2]);
			vertices.push_back(v);
		}
	}
	std::vector<uint16_t> indices;
	for (int y = 0; y < kCells; ++y) {
		for (int x = 0; x < kCells; ++x) {
			const auto at = [](int px, int py) { return static_cast<uint16_t>(py * (kCells + 1) + px); };
			indices.insert(indices.end(), {at(x, y), at(x, y + 1), at(x + 1, y), at(x + 1, y), at(x, y + 1), at(x + 1, y + 1)});
		}
	}
	m_waterVertices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
		static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
	m_waterIndices = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(),
		static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
	m_waterIndexCount = static_cast<uint32_t>(indices.size());
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
	m_undergrowthAtlas = texture(sources.files, sources.files.exists(atlas) ? atlas
																		 : sources.levelPath + "UndergrowthAtlas0.dds");
}

void LevelRenderer::updateUndergrowth(const Camera& camera)
{
	if (!m_undergrowth) {
		return;
	}
	const int n = m_undergrowth->patchesPerSide();
	const float size = m_undergrowth->patchSize();
	const float reach = m_undergrowthViewDistance + size;
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
	uniforms.fade[0] = m_undergrowthViewDistance;
	uniforms.fade[1] = 1.0f / std::max(m_undergrowthViewDistance * m_undergrowthFadeScale, 0.01f);
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
		addTemplate(sources, *objectTemplate, Mat4 {}, objectTransform(object.position(), object.rotation()), object.id(), 0);
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
// centres, grouped by texture in drawing order.
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
		SDL_GPUTexture* image = texture(m_sources->files, q.texture);
		if (!image) {
			continue;
		}
		const float cx = (q.x0 + q.x1) * 0.5f, cy = (q.y0 + q.y1) * 0.5f;
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
		if (!m_overlayGroups.empty() && m_overlayGroups.back().texture == image) {
			m_overlayGroups.back().vertexCount += count;
		} else {
			m_overlayGroups.push_back({image, first, count});
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
									  : texture(m_sources->files, batch.texture);
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

// Two triangles per particle, uploaded for this frame.
void LevelRenderer::buildParticles(const Camera& camera)
{
	(void)camera;
	buildParticleBuffer(m_sources ? m_sources->effects : nullptr, m_particleVertices, m_particleGroups);
}

void LevelRenderer::buildParticleBuffer(const effects::EffectManager* manager, SDL_GPUBuffer*& buffer,
	std::vector<ParticleGroup>& groups)
{
	groups.clear();
	if (buffer) {
		SDL_ReleaseGPUBuffer(m_gpu.device(), buffer); // released once the GPU is done with it
		buffer = nullptr;
	}
	if (!m_sources || !manager) {
		return;
	}
	std::vector<Vertex> vertices;
	manager->forEachGroup([&](const std::string& path, bool additive, const std::vector<effects::Sprite>& sprites) {
		SDL_GPUTexture* image = texture(m_sources->files, path + ".dds");
		if (!image) {
			return;
		}
		const uint32_t first = static_cast<uint32_t>(vertices.size());
		static const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
		for (const effects::Sprite& sprite : sprites) {
			for (const auto& corner : corners) {
				Vertex v {};
				v.position[0] = sprite.position.x;
				v.position[1] = sprite.position.y;
				v.position[2] = sprite.position.z;
				v.normal[0] = sprite.color[0];
				v.normal[1] = sprite.color[1];
				v.normal[2] = sprite.color[2];
				v.weight = sprite.color[3];
				v.texCoord[0] = corner[0];
				v.texCoord[1] = corner[1];
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
	if (vertices.empty()) {
		groups.clear();
		return;
	}
	buffer = m_gpu.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(),
		static_cast<uint32_t>(vertices.size() * sizeof(Vertex)));
	m_gpu.flushUploads();
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
		SDL_GPUTextureSamplerBinding sampler {group.texture, m_gpu.clampSampler()};
		SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
		SDL_DrawGPUPrimitives(pass, group.vertexCount, 1, group.firstVertex, 0);
	}
}

void LevelRenderer::build(const LevelSources& sources)
{
	m_startTicks = SDL_GetTicks();
	loadLightmapAtlas(sources);
	buildTerrain(sources);
	buildSky(sources);
	buildWater(sources);
	buildRoads(sources);
	buildUndergrowth(sources);
	m_sources = &sources;
	Vec3f sum;
	size_t placed = 0;
	for (const auto& object : sources.objects.objects()) {
		const size_t before = m_instances.size();
		addObject(sources, *object);
		if (m_instances.size() > before) {
			sum = sum + Vec3f {object->position().x, object->position().y, object->position().z};
			++placed;
		}
	}
	m_center = placed ? sum * (1.0f / placed) : Vec3f {};
	m_stats.instances = m_instances.size();
	m_gpu.flushUploads();
}

Camera LevelRenderer::suggestedCamera() const
{
	Camera camera;
	camera.position = m_center + Vec3f {-120, 80, -120};
	camera.yaw = 0.785f;
	camera.pitch = -0.4f;
	return camera;
}

void LevelRenderer::draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* color, GpuContext::Target& target,
	const Camera& camera, const scene::LightSettings& lights)
{
	SDL_GPUColorTargetInfo colorTarget {};
	colorTarget.texture = color;
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
	// The first-person models, loaded before the pass begins.
	m_firstPerson = FirstPersonScene {};
	m_hasFirstPerson = m_sources && m_sources->firstPerson && m_sources->firstPerson(m_firstPerson);
	if (m_hasFirstPerson) {
		for (const FirstPersonModel& model : m_firstPerson.models) {
			mesh(*m_sources, model.geometry, model.geometryIndex, model.lod);
		}
		m_gpu.flushUploads();
	}
	buildParticleBuffer(m_hasFirstPerson ? m_firstPerson.effects : nullptr, m_firstPersonParticleVertices,
		m_firstPersonParticleGroups);
	buildOverlay();
	// The menu keeps its own clock: it plays with or without a HUD.
	const uint64_t now = SDL_GetTicks();
	const float menuSeconds = m_lastMenuTicks ? static_cast<float>(now - m_lastMenuTicks) / 1000.0f : 0.0f;
	m_lastMenuTicks = now;
	buildMenu(menuSeconds);
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colorTarget, 1, &depthTarget);

	const Mat4 viewProjection = camera.view() *
		Mat4::perspective(camera.fovY, static_cast<float>(target.width) / target.height, camera.nearPlane, 6000.0f);

	// Sky dome around the camera, unlit and unfogged, behind everything.
	if (m_skyMesh) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.skyPipeline());
		LightingUniforms sky {};
		sky.skyColor[0] = sky.skyColor[1] = sky.skyColor[2] = 1;
		sky.fogColor[3] = 1e9f;
		SDL_PushGPUFragmentUniformData(commands, 0, &sky, sizeof(sky));
		const Mat4 world = Mat4::rotationY(m_skyRotation * 3.14159265f / 180.0f) * Mat4::translation(camera.position);
		TransformUniforms skyTransforms {world * viewProjection, world, {1, 1, 0, 0}, {}};
		SDL_PushGPUVertexUniformData(commands, 0, &skyTransforms, sizeof(skyTransforms));
		SDL_GPUBufferBinding vertexBinding {m_skyMesh->vertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		SDL_GPUBufferBinding indexBinding {m_skyMesh->indices, 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		for (const Submesh& submesh : m_skyMesh->submeshes) {
			SDL_GPUTextureSamplerBinding samplers[5] = {
				{m_skyTexture ? m_skyTexture : submesh.texture, m_gpu.clampSampler()},
				{m_gpu.whiteTexture(), m_gpu.clampSampler()},
				{m_gpu.whiteTexture(), m_gpu.clampSampler()},
				{m_gpu.whiteTexture(), m_gpu.clampSampler()},
				{m_gpu.whiteTexture(), m_gpu.clampSampler()},
			};
			SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
			SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
		}
	}

	// Terrain.
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.terrainPipeline());
	// The terrain's sun and GI colors come from terraindata.raw; the
	// script values are the editor's (the game doesn't have them).
	const auto& terrainSun = m_terrainColorsFromData ? m_terrainSunColor : lights.terrainSunColor;
	const auto& terrainGI = m_terrainColorsFromData ? m_terrainGIColor : lights.terrainGIColor;
	const LightingUniforms terrainLit = lighting(lights, terrainSun, terrainGI, 0, true);
	const LightingUniforms terrainUnlit = lighting(lights, terrainSun, terrainGI, 0, false);
	TransformUniforms transforms {viewProjection, Mat4 {}, {1, 1, 0, 0}, {}};
	SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
	if (m_terrainIndices) {
		SDL_GPUBufferBinding indexBinding {m_terrainIndices, 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
	}
	for (const TerrainPatch& patch : m_terrain) {
		SDL_GPUBufferBinding vertexBinding {patch.vertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		SDL_PushGPUFragmentUniformData(commands, 0, patch.lightmap ? &terrainLit : &terrainUnlit, sizeof(terrainLit));
		TerrainDetailUniforms detail {};
		for (int i = 0; i < kMaxDetailLayers; ++i) {
			detail.tiling[i] = m_detailTiling[i];
		}
		const bool hasDetail = patch.detailWeights[0] != nullptr;
		detail.detailFade[0] = kDetailFadeStart;
		detail.detailFade[1] = kDetailFadeEnd;
		detail.detailFade[2] = hasDetail ? 1.0f : 0.0f;
		SDL_PushGPUFragmentUniformData(commands, 1, &detail, sizeof(detail));
		const auto orBlack = [this](SDL_GPUTexture* t) { return t ? t : m_gpu.blackTexture(); };
		SDL_GPUTextureSamplerBinding samplers[4 + kMaxDetailLayers] = {
			{patch.colormap ? patch.colormap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
			{patch.lightmap ? patch.lightmap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
			{orBlack(patch.detailWeights[0]), m_gpu.clampSampler()},
			{orBlack(patch.detailWeights[1]), m_gpu.clampSampler()},
		};
		for (int i = 0; i < kMaxDetailLayers; ++i) {
			samplers[4 + i] = {m_detailLayers[i] ? m_detailLayers[i] : m_gpu.whiteTexture(), m_gpu.repeatSampler()};
		}
		SDL_BindGPUFragmentSamplers(pass, 0, samplers, 4 + kMaxDetailLayers);
		SDL_DrawGPUIndexedPrimitives(pass, m_terrainIndexCount, 1, 0, 0, 0);
	}

	// Roads, once per terrain patch they overlap, lit by that patch's lightmap.
	if (!m_roads.empty() && m_patchSize > 0) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.roadPipeline());
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

	// Objects.
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.meshPipeline());
	// Lightmapped statics use the static sun/sky colors.
	const LightingUniforms uniformsFor[2][2] = {
		{lighting(lights, lights.sunColor, lights.skyColor, 0, false),
			lighting(lights, lights.sunColor, lights.skyColor, 0.5f, false)},
		{lighting(lights, lights.staticSunColor, lights.staticSkyColor, 0, true),
			lighting(lights, lights.staticSunColor, lights.staticSkyColor, 0.5f, true)},
	};
	const GpuMesh* bound = nullptr;
	// Objects fading out (a corpse's countdown, 0x00652380) go in the
	// blended bucket, after the opaque ones (RendDX9's skinned mesh draws
	// with alpha below 1 as transparent, 0x10162070).
	std::vector<const Instance*> faded;
	const auto drawInstance = [&](const Instance& instance) {
		if (instance.mesh != bound) {
			bound = instance.mesh;
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
		std::copy(std::begin(instance.lightmapTransform), std::end(instance.lightmapTransform),
			transforms.lightmapTransform);
		SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
		const bool lightmapped = instance.lightmap != nullptr;
		std::vector<anim::Matrix> pose;
		if (bound->skinned && instance.skeleton
			&& !(m_sources && m_sources->skeletonPose && instance.object != 0
				&& m_sources->skeletonPose(instance.object, *instance.skeleton, pose))) {
			const float seconds = static_cast<float>(SDL_GetTicks() - m_startTicks) / 1000.0f;
			pose = anim::evaluatePose(*instance.skeleton, instance.animation.get(), seconds * anim::kFramesPerSecond);
		}
		for (const Submesh& submesh : bound->submeshes) {
			if (submesh.rig >= 0 && !pose.empty()) {
				// Bone palette of this material's rig: mesh -> bone -> world.
				const auto& rig = bound->rigs[static_cast<size_t>(submesh.rig)];
				for (size_t b = 0; b < rig.size() && b < static_cast<size_t>(kMaxMeshParts); ++b) {
					const int node = rig[b].node;
					const anim::Matrix skin = node >= 0 && static_cast<size_t>(node) < pose.size()
						? anim::multiply(rig[b].meshToBone, pose[static_cast<size_t>(node)])
						: rig[b].meshToBone;
					std::copy(skin.begin(), skin.end(), transforms.parts[b].m.begin());
				}
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
			}
			LightingUniforms uniforms = uniformsFor[lightmapped][submesh.alphaReference > 0];
			uniforms.material[2] = (submesh.detail ? 1.0f : 0.0f) + (submesh.dirt ? 2.0f : 0.0f) + (submesh.crack ? 4.0f : 0.0f);
			uniforms.material[3] = instance.fade < 1.0f ? std::max(instance.fade, 1e-3f) : 0.0f;
			SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof(uniforms));
			const auto orWhite = [this](SDL_GPUTexture* t) { return t ? t : m_gpu.whiteTexture(); };
			SDL_GPUTextureSamplerBinding samplers[5] = {
				{submesh.texture, m_gpu.repeatSampler()},
				{lightmapped ? instance.lightmap : m_gpu.whiteTexture(), m_gpu.clampSampler()},
				{orWhite(submesh.detail), m_gpu.repeatSampler()},
				{orWhite(submesh.dirt), m_gpu.repeatSampler()},
				{orWhite(submesh.crack), m_gpu.repeatSampler()},
			};
			SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
			SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
		}
	};
	for (const Instance& instance : m_instances) {
		if (instance.hidden || instance.fade <= 0.0f) {
			continue;
		}
		if (instance.fade < 1.0f) {
			faded.push_back(&instance);
			continue;
		}
		drawInstance(instance);
	}
	if (!faded.empty()) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.fadedMeshPipeline());
		for (const Instance* instance : faded) {
			drawInstance(*instance);
		}
	}
	// Particles, after what they are in front of.
	drawParticles(commands, pass, viewProjection, camera, lights, m_particleVertices, m_particleGroups);

	// Water last, blended over what is below it.
	if (m_waterVertices && m_water) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.waterPipeline());
		const LightingUniforms waterLighting = lighting(lights, lights.sunColor, lights.skyColor, 0, false);
		SDL_PushGPUFragmentUniformData(commands, 0, &waterLighting, sizeof(waterLighting));
		const float seconds = static_cast<float>(SDL_GetTicks() - m_startTicks) / 1000.0f;
		const WaterUniforms water {
			{camera.position.x, camera.position.y, camera.position.z, seconds},
			{m_water->color[0], m_water->color[1], m_water->color[2], 1},
			{m_water->specularColor[0], m_water->specularColor[1], m_water->specularColor[2], m_water->specularColor[3]},
			{m_water->alphaAdd, m_water->alphaHeightMul, m_water->specularPower, 0},
		};
		SDL_PushGPUFragmentUniformData(commands, 1, &water, sizeof(water));
		TransformUniforms waterTransforms {viewProjection, Mat4 {}, {1, 1, 0, 0}, {}};
		SDL_PushGPUVertexUniformData(commands, 0, &waterTransforms, sizeof(waterTransforms));
		SDL_GPUBufferBinding vertexBinding {m_waterVertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		SDL_GPUBufferBinding indexBinding {m_waterIndices, 0};
		SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		SDL_DrawGPUIndexedPrimitives(pass, m_waterIndexCount, 1, 0, 0, 0);
	}
	// The first-person pass: the world's depth cleared, the weapon and arms
	// drawn with their own projection.
	if (m_hasFirstPerson) {
		SDL_EndGPURenderPass(pass);
		SDL_GPUColorTargetInfo keepColor = colorTarget;
		keepColor.load_op = SDL_GPU_LOADOP_LOAD;
		pass = SDL_BeginGPURenderPass(commands, &keepColor, 1, &depthTarget);
		drawFirstPerson(commands, pass, camera, static_cast<float>(target.width) / target.height, lights);
	}
	// The HUD over everything.
	if (m_overlayVertices && !m_overlayGroups.empty()) {
		SDL_BindGPUGraphicsPipeline(pass, m_gpu.uiPipeline());
		const float screen[4] = {800, 600, 0, 0};
		SDL_PushGPUVertexUniformData(commands, 0, screen, sizeof(screen));
		SDL_GPUBufferBinding vertexBinding {m_overlayVertices, 0};
		SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
		for (const OverlayGroup& group : m_overlayGroups) {
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
	SDL_BindGPUGraphicsPipeline(pass, m_gpu.meshPipeline());
	const LightingUniforms lit[2] = {lighting(lights, lights.sunColor, lights.skyColor, 0, false),
		lighting(lights, lights.sunColor, lights.skyColor, 0.5f, false)};
	for (const FirstPersonModel& model : scene.models) {
		const GpuMesh* gpuMesh = mesh(*m_sources, model.geometry, model.geometryIndex, model.lod);
		if (!gpuMesh) {
			continue;
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
			if (submesh.blended) {
				continue; // additive glows (sights, displays): no blended mesh pipeline yet
			}
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
				SDL_PushGPUVertexUniformData(commands, 0, &transforms, sizeof(transforms));
			}
			LightingUniforms uniforms = lit[submesh.alphaReference > 0];
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
	drawParticles(commands, pass, viewProjection, camera, lights, m_firstPersonParticleVertices,
		m_firstPersonParticleGroups);
}

} // namespace dice::hfe::render
