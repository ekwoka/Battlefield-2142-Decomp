// Draws a loaded level: the primary terrain with its colormaps, and the
// static objects placed by the level scripts with their meshes.
//
// This is a first, simplified stand-in for RendDX9's renderers (terrain,
// static mesh, bundled mesh): diffuse textures with sun/sky lighting and
// fog, LOD 0 only, no lightmaps, shadows or effects yet.
#pragma once

#include "Animation/Pose.h"
#include "GpuContext.h"
#include "Hud/Hud.h"
#include "UI/MenuDraw.h"
#include "World/WorldMath.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe {
namespace io {
class FileManager;
}
namespace world {
class Object;
class ObjectManager;
class ObjectTemplate;
class ObjectTemplateManager;
} // namespace world
namespace geom {
class GeometryTemplateManager;
}
namespace terrain {
class HeightmapCluster;
struct TerrainData;
class UndergrowthGenerator;
} // namespace terrain
namespace effects {
class EffectManager;
}
namespace scene {
struct LightSettings;
struct SkySettings;
struct WaterSettings;
} // namespace scene
} // namespace dice::hfe

namespace dice::hfe::render {

struct Camera {
	Vec3f position;
	float yaw = 0;   // radians, 0 looks along +z
	float pitch = 0; // radians, positive looks up
	float fovY = 1.0f;
	float nearPlane = 0.5f;

	Vec3f forward() const;
	Vec3f right() const;
	Mat4 view() const;
};

// A model of the first-person view (the held weapon, the soldier's arms):
// one geometry and LOD of a bundled or skinned mesh, placed in the world
// (BF2142.exe draws the first-person geometry, 0, of the soldier's and the
// weapon's meshes; the third person is geometry 1).
struct FirstPersonModel {
	std::string geometry; // the geometry template
	int geometryIndex = 0;
	int lod = 0;
	Mat4 world;
	// Skinned: the skeleton's node matrices in model space (as
	// LevelSources::skeletonPose gives them).
	std::vector<anim::Matrix> pose;
	// Bundled: the parts' transforms in model space (part 0 first).
	std::vector<Mat4> parts;
};

// The first-person pass, drawn over the world after a depth clear with its
// own projection (RendDX9's first-person pass), so the weapon never cuts
// into walls.
struct FirstPersonScene {
	// RendDX9's first-person view (+0xfc of the renderer): its own 0.82 rad
	// whatever the world's field of view or zoom, near
	// renderer.tweakNearPlane1p (0.16), far 15 (0x1002aba0).
	float fovY = 0.82f;
	float nearPlane = 0.16f;
	float farPlane = 15.0f;
	std::vector<FirstPersonModel> models;
	// Effects of the first-person models (the muzzle flash), drawn in the
	// same pass; may be null.
	const effects::EffectManager* effects = nullptr;
};

struct LevelSources {
	const io::FileManager& files;
	const world::ObjectTemplateManager& templates;
	const world::ObjectManager& objects;
	const geom::GeometryTemplateManager& geometries;
	const terrain::HeightmapCluster& heightmaps;
	const terrain::TerrainData* terrainData; // terraindata.raw header, if loaded
	const scene::LightSettings& lights;
	const scene::SkySettings& sky;
	const scene::WaterSettings& water;
	std::string levelPath; // "Levels/<name>/"
	// Animation played by skinned objects for previewing (a .baf path); empty for the setup pose.
	std::string previewAnimation;
	// The animation an object plays now (a .baf path), asked each frame;
	// empty keeps the preview animation.
	std::function<std::string(uint32_t object)> animationFor;
	// The pose of an object's skeleton from the game's animation systems
	// (world matrices per node); false to play the preview animation.
	std::function<bool(uint32_t object, const anim::Skeleton& skeleton, std::vector<anim::Matrix>& world)> skeletonPose;
	// All mesh-part transforms of an object (relative to it) when the game
	// places them itself (held weapons); false otherwise.
	std::function<bool(uint32_t object, std::vector<anim::Matrix>& parts)> partTransforms;
	// Parts the game moves (wheels, steering): an offset and yaw/pitch/roll
	// before the part's placement in its parent; asked each frame.
	std::function<bool(uint32_t object, const std::string& part, world::Vec3& offset, world::Vec3& rotation)> partPose;
	// The HUD drawn over the frame (quads in its 800 x 600 screen), asked
	// each frame with the seconds since the last one.
	std::function<std::vector<hud::HudQuad>(float seconds)> overlay;
	// A Flash menu over everything (the main menu, the end of round).
	std::function<ui::MenuFrame(float seconds)> menu;
	// Effects to draw (particles), if any.
	const effects::EffectManager* effects = nullptr;
	// The first-person view, asked each frame: false draws none.
	std::function<bool(FirstPersonScene& scene)> firstPerson;
	// Objects the world pass leaves out this frame (in first person the
	// player's own soldier, its weapon and kit); asked by syncObjects.
	std::function<bool(uint32_t object)> hiddenInView;
};

struct LevelStats {
	size_t meshes = 0;
	size_t missingMeshes = 0;
	std::vector<std::string> missingMeshNames; // geometry: reason
	size_t textures = 0;
	size_t missingTextures = 0;
	size_t instances = 0;
	size_t terrainPatches = 0;
	size_t lightmappedInstances = 0;
	size_t roads = 0;
	size_t undergrowthTypes = 0;
	size_t missingRoads = 0;
};

class LevelRenderer {
public:
	explicit LevelRenderer(GpuContext& gpu);
	~LevelRenderer();

	void build(const LevelSources& sources);
	const LevelStats& stats() const { return m_stats; }

	// A camera position above the middle of the placed objects.
	Camera suggestedCamera() const;

	// Follows the world's objects: moves instances with their objects, adds
	// new objects and drops removed ones (call once a frame before draw).
	void syncObjects();

	// Records a render pass into `commands` that clears and draws into
	// `color` (a swapchain or offscreen texture) using `target`'s depth.
	void draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* color, GpuContext::Target& target,
		const Camera& camera, const scene::LightSettings& lights);

private:
	struct Submesh {
		uint32_t firstIndex;
		uint32_t indexCount;
		int32_t vertexOffset;
		SDL_GPUTexture* texture;
		float alphaReference;
		int rig = -1; // skinned meshes: the bone set this material uses
		SDL_GPUTexture* detail = nullptr; // static mesh layers, null if unused
		SDL_GPUTexture* dirt = nullptr;
		SDL_GPUTexture* crack = nullptr;
		bool blended = false; // alpha mode 1 (blended or additive)
	};
	struct RigBone {
		int node;
		anim::Matrix meshToBone;
	};
	struct GpuMesh {
		bool bundled = false;
		bool skinned = false;
		std::vector<std::vector<RigBone>> rigs;
		SDL_GPUBuffer* vertices = nullptr;
		SDL_GPUBuffer* indices = nullptr;
		std::vector<Submesh> submeshes;
	};
	struct Instance {
		const GpuMesh* mesh;
		Mat4 world;
		uint32_t object = 0; // the world object it belongs to
		Mat4 local;          // relative to that object
		SDL_GPUTexture* lightmap; // null without one
		float lightmapTransform[4];
		std::vector<Mat4> parts; // bundled meshes: part transforms relative to the object
		std::shared_ptr<anim::Skeleton> skeleton; // skinned meshes
		std::shared_ptr<anim::Animation> animation;
		bool hidden = false; // its object is hidden
		float fade = 1;      // its object's fade (below 1: drawn blended, after the opaque ones)
		const world::ObjectTemplate* partsTemplate = nullptr; // whose children place the parts
	};
	struct TerrainPatch {
		SDL_GPUBuffer* vertices = nullptr;
		SDL_GPUTexture* colormap = nullptr;
		SDL_GPUTexture* lightmap = nullptr;
		SDL_GPUTexture* detailWeights[2] = {}; // layers 0-2 and 3-5
	};
	// A compiled road, drawn over the terrain patches its bounds overlap.
	struct Road {
		SDL_GPUBuffer* vertices = nullptr;
		SDL_GPUBuffer* indices = nullptr;
		uint32_t indexCount = 0;
		Mat4 world;
		SDL_GPUTexture* texture0 = nullptr;
		SDL_GPUTexture* texture1 = nullptr;
		RoadUniforms uniforms {};
		uint32_t priority = 0;
		float boundsMin[2] {}; // world x, z
		float boundsMax[2] {};
	};
	struct RoadLook {
		bool loaded = false;
		SDL_GPUTexture* texture0 = nullptr;
		SDL_GPUTexture* texture1 = nullptr;
		RoadUniforms uniforms {};
		uint32_t priority = 0;
	};
	static constexpr int kMaxDetailLayers = 6;
	// Where a texture lives in a texture atlas (.tai files).
	struct AtlasEntry {
		std::string atlas;
		float offset[2];
		float size[2];
	};

	// A geometry of a mesh file at a LOD; geometryIndex -1 is the one the
	// world shows (the exterior: 1 of bundled and skinned meshes with more
	// than one).
	const GpuMesh* mesh(const LevelSources& sources, const std::string& geometryName, int geometryIndex = -1,
		int lod = 0);
	SDL_GPUTexture* texture(const io::FileManager& files, const std::string& path);
	void addTemplate(const LevelSources& sources, const world::ObjectTemplate& objectTemplate, const Mat4& local,
		const Mat4& objectWorld, uint32_t object, int depth);
	void addObject(const LevelSources& sources, const world::Object& object);
	void collectParts(const LevelSources& sources, const world::ObjectTemplate& objectTemplate, const Mat4& relative,
		std::vector<Mat4>& parts, int depth, uint32_t object = 0) const;
	void buildTerrain(const LevelSources& sources);
	void buildSky(const LevelSources& sources);
	void buildWater(const LevelSources& sources);
	void buildRoads(const LevelSources& sources);
	void buildUndergrowth(const LevelSources& sources);
	// Generates the undergrowth patches in view and drops distant ones.
	void updateUndergrowth(const Camera& camera);
	void drawUndergrowth(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection,
		const Camera& camera, const scene::LightSettings& lights);
	std::shared_ptr<anim::Skeleton> skeleton(const io::FileManager& files, const std::string& path);
	std::shared_ptr<anim::Animation> animation(const io::FileManager& files, const std::string& path);
	void loadLightmapAtlas(const LevelSources& sources);

	GpuContext& m_gpu;
	std::map<std::string, std::unique_ptr<GpuMesh>> m_meshes; // by geometry name (lower case)
	std::map<std::string, SDL_GPUTexture*> m_textures;         // by path (lower case)
	std::map<std::string, std::shared_ptr<anim::Skeleton>> m_skeletons;
	std::map<std::string, std::shared_ptr<anim::Animation>> m_animations;
	std::vector<Instance> m_instances;
	const LevelSources* m_sources = nullptr;
	std::vector<uint32_t> m_knownObjects; // sorted ids of objects already added
	std::map<std::string, AtlasEntry> m_lightmapAtlas; // "<geometry>=<lod>=<x>=<y>=<z>" -> atlas
	std::vector<TerrainPatch> m_terrain; // row-major from the -x/-z corner
	int m_patchesPerSide = 0;
	float m_patchOrigin[2] {}; // world x, z of the terrain's corner
	float m_patchSize = 0;     // metres
	std::vector<Road> m_roads; // in drawing order (priority)
	std::map<std::string, RoadLook> m_roadLooks; // by geometry template (lower case)
	struct UndergrowthPatchBuffers {
		SDL_GPUBuffer* vertices = nullptr; // null when the patch is empty
		SDL_GPUBuffer* indices = nullptr;
		uint32_t indexCount = 0;
	};
	std::unique_ptr<terrain::UndergrowthGenerator> m_undergrowth;
	float m_undergrowthViewDistance = 0;
	float m_undergrowthFadeScale = 0;
	float m_undergrowthSwayScale = 0;
	float m_undergrowthAlphaRef = 0;
	SDL_GPUTexture* m_undergrowthAtlas = nullptr;
	std::map<int, UndergrowthPatchBuffers> m_undergrowthPatches; // by patch index
	// This frame's particles: one buffer, drawn in groups.
	struct ParticleGroup {
		SDL_GPUTexture* texture;
		bool additive;
		uint32_t firstVertex;
		uint32_t vertexCount;
	};
	SDL_GPUBuffer* m_particleVertices = nullptr;
	struct OverlayGroup {
		SDL_GPUTexture* texture;
		uint32_t firstVertex;
		uint32_t vertexCount;
	};
	SDL_GPUBuffer* m_overlayVertices = nullptr;
	// The menu layer: its triangles by texture, and the textures of images
	// made in memory (released when their image goes).
	struct MenuGroup {
		SDL_GPUTexture* texture;
		bool repeat;
		uint32_t firstVertex;
		uint32_t vertexCount;
		ui::MenuBatch::Stencil stencil = ui::MenuBatch::Stencil::Off; // its mask state
		int stencilLevel = 0;
	};
	SDL_GPUBuffer* m_menuVertices = nullptr;
	std::vector<MenuGroup> m_menuGroups;
	float m_menuSize[2] = {1024, 768};
	struct MenuTexture {
		std::weak_ptr<const ui::MenuImage> image;
		SDL_GPUTexture* texture;
	};
	std::map<const ui::MenuImage*, MenuTexture> m_menuTextures;
	SDL_GPUTexture* menuTexture(const std::shared_ptr<const ui::MenuImage>& image);
	void buildMenu(float seconds);
	std::vector<OverlayGroup> m_overlayGroups;
	uint64_t m_lastOverlayTicks = 0;
	uint64_t m_lastMenuTicks = 0;
	void buildOverlay();
	std::vector<ParticleGroup> m_particleGroups;
	void buildParticles(const Camera& camera);
	// One effect manager's particles into a buffer (released and rebuilt).
	void buildParticleBuffer(const effects::EffectManager* manager, SDL_GPUBuffer*& buffer,
		std::vector<ParticleGroup>& groups);
	void drawParticles(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection,
		const Camera& camera, const scene::LightSettings& lights, SDL_GPUBuffer* buffer,
		const std::vector<ParticleGroup>& groups);
	// The first-person pass (FirstPersonScene).
	void drawFirstPerson(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Camera& camera,
		float aspect, const scene::LightSettings& lights);
	SDL_GPUBuffer* m_firstPersonParticleVertices = nullptr;
	std::vector<ParticleGroup> m_firstPersonParticleGroups;
	FirstPersonScene m_firstPerson;
	bool m_hasFirstPerson = false;
	const GpuMesh* m_skyMesh = nullptr;
	SDL_GPUTexture* m_skyTexture = nullptr;
	float m_skyRotation = 0;
	SDL_GPUBuffer* m_terrainIndices = nullptr;
	SDL_GPUBuffer* m_waterVertices = nullptr;
	SDL_GPUBuffer* m_waterIndices = nullptr;
	uint32_t m_waterIndexCount = 0;
	const scene::WaterSettings* m_water = nullptr;
	Uint64 m_startTicks = 0;
	SDL_GPUTexture* m_detailLayers[kMaxDetailLayers] = {};
	float m_detailTiling[kMaxDetailLayers] = {};
	std::array<float, 3> m_terrainSunColor {};
	std::array<float, 3> m_terrainGIColor {};
	bool m_terrainColorsFromData = false;
	uint32_t m_terrainIndexCount = 0;
	Vec3f m_center;
	LevelStats m_stats;
};

} // namespace dice::hfe::render
