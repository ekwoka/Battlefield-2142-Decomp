// Draws a loaded level: the primary terrain with its colormaps, and the
// static objects placed by the level scripts with their meshes.
//
// A stand-in for RendDX9's renderers (terrain and the surrounding terrain,
// static, bundled and skinned meshes with their LODs, roads, undergrowth,
// water, particles, decals), the dynamic shadows, the hemi maps, the sky's
// clouds and sun flare and the post-production: the original's shaders
// rewritten for the GPU layer, with frustum culling.
#pragma once

#include "Animation/Pose.h"
#include "Culling.h"
#include "EnvMaps.h"
#include "Fog.h"
#include "TerrainLod.h"
#include "TitanParts.h"
#include "Effects/Effects.h"
#include "Scene/EffectSettings.h"
#include "GpuContext.h"
#include "OverheadView.h"
#include "PostProduction.h"
#include "Hud/Hud.h"
#include "UI/MenuDraw.h"
#include "World/WorldMath.h"

#include <array>
#include <functional>
#include <initializer_list>
#include <map>
#include <unordered_map>
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
struct HemiMapSettings;
struct LightSettings;
struct ParticleSystemSettings;
struct QualitySettings;
struct WindSettings;
struct PostProductionSettings;
struct ShadowSettings;
struct RendererSettings;
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
	float roll = 0; // radians about the view axis (a seat camera's shakes)

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
// A seat camera's static interior sun (BF2142.exe 0x006104d0..0x00610580,
// PlayerCamera::insideSun): in the vehicle's inside view the cockpit is lit
// by this sun and ambient instead of the level's, and the dynamic lights
// only with allowInsideDynamicLights.
struct InsideSun {
	bool dynamicLights = true;
	std::array<float, 3> direction {-0.25f, -0.5f, 0.75f}; // the way the light travels
	std::array<float, 3> color {1, 1, 1};
	std::array<float, 3> ambient {0.7f, 0.7f, 0.7f};
};

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
	// More of them, drawn with those (the bundles attached to objects:
	// engines, exhausts, trails; effects::ObjectEffects).
	const effects::EffectManager* objectEffects = nullptr;
	// The first-person view, asked each frame: false draws none.
	std::function<bool(FirstPersonScene& scene)> firstPerson;
	// Objects the world pass leaves out this frame (in first person the
	// player's own soldier, its weapon and kit); asked by syncObjects.
	std::function<bool(uint32_t object)> hiddenInView;
	// The geometry and LOD an object's meshes with more than one geometry
	// draw instead of the exterior (a vehicle's cockpit seen from inside,
	// BF2142.exe 0x00612880); false for the default. Asked by syncObjects.
	std::function<bool(uint32_t object, int& geometry, int& lod)> geometryFor;
	// Rain and snow (rainManager.*), if any.
	const scene::RainSettings* rain = nullptr;
	// renderer.global*LodDistanceScale and terrainCuller.* (defaults if null).
	const scene::RendererSettings* renderer = nullptr;
	// Told how far build() got (0..1, the last call 1), for a loading screen.
	std::function<void(float done)> buildProgress;
	// renderer.bloom*/default*/emp*, shadowManager.*, HemiMapManager.*
	// (defaults if null).
	const scene::PostProductionSettings* post = nullptr;
	const scene::ShadowSettings* shadows = nullptr;
	const scene::HemiMapSettings* hemi = nullptr;
	// ParticleSystemManager.*, windManager.*, VideoSettings.* (defaults if null).
	const scene::ParticleSystemSettings* particles = nullptr;
	const scene::WindSettings* wind = nullptr;
	const scene::QualitySettings* quality = nullptr;
	// The interior sun of the seat camera now; false when there is none.
	std::function<bool(InsideSun& sun)> insideSun;
	// The game state the post-production follows (EMP, tinnitus, the Titan
	// flash, the camera effect), asked each frame; none if null.
	std::function<void(PostEffects& effects)> postEffects;
	// The team's Titan (1 PAC, 2 EU; 0 without one), for the Titan
	// interiors' hemi maps (HemiMapManager.loadTitanHemiMap).
	std::function<uint32_t(int team)> titanObject;
	// Hidden objects drawn as active camouflage (a cloaked soldier's
	// refraction); asked each frame for the hidden ones.
	std::function<bool(uint32_t object)> cloaked;
	// The commander's overhead view, asked each frame: true while it is
	// on (the world pass is then drawn from it, without the first person).
	std::function<bool(OverheadView& view)> overheadView;
	// Told each frame whether the camera is inside a Titan (the object
	// drawer's slot 0x40, which RendDX9's NametagManager reads too,
	// 0x100f3490: the 3D icons' showInside / showOutside); may be null.
	bool* cameraInTitanOut = nullptr;
};

struct LevelStats {
	size_t meshes = 0;
	size_t missingMeshes = 0;
	std::vector<std::string> missingMeshNames; // geometry: reason
	size_t textures = 0;
	size_t missingTextures = 0;
	size_t instances = 0;
	size_t terrainPatches = 0;
	bool terrainLods = false; // the primary terrain drawn from terraindata.raw's LOD meshes
	size_t lightmappedInstances = 0;
	size_t roads = 0;
	size_t undergrowthTypes = 0;
	size_t missingRoads = 0;
	size_t surroundingPatches = 0;
	bool hemiMap = false;     // a base hemi map loaded
	size_t cloudLayers = 0;   // the sky's cloud layers drawn
	bool sunFlare = false;    // a sun flare drawn
	size_t normalMapped = 0;  // bundled and skinned materials drawn with their normal maps
};

class LevelRenderer {
public:
	explicit LevelRenderer(GpuContext& gpu);
	~LevelRenderer();

	void build(const LevelSources& sources);
	const LevelStats& stats() const { return m_stats; }

	// A camera position above the middle of the placed objects.
	Camera suggestedCamera() const;

	// The overhead view's camera (OverheadView).
	Camera overheadCamera(const OverheadView& view) const;

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
		// Alpha mode 1: drawn after the opaque ones, sorted back to front
		// (RendDX9's transparency queues), or added when the geometry has
		// additive alpha (RSMTAdditive; 0x100c3880).
		bool blended = false;
		bool additive = false;
		float uvScroll[2] = {0, 0}; // additive: simpleUVTranslation, per second
		// Where the texture lies in an atlas (the overgrowth's distant trees'
		// textures, OvergrowthAtlas.tai): scale, then offset.
		float uvAtlas[4] = {1, 1, 0, 0};
		// mesh.frag's specular kind (LightingUniforms::eye.w): 1 / 2 a static
		// mesh's (StaticMesh.fx: the detail map's alpha as the gloss when it
		// has one and isn't alpha-tested, RaShaderSTM getCompositeDiffuse),
		// 3 / 4 a bundled mesh's (BundledMesh.fx: the colour map's alpha with
		// a "ColormapGloss" technique), 5 a skinned mesh's (RaShaderSM:
		// pow(N.H, SpecularPower 20) x the normal map's alpha, else
		// StaticGloss); 0 none (additive).
		int specularKind = 0;
		// StaticGloss (BundledMesh.fx): GeometryTemplate.setSpecularStaticGloss
		// for this geometry, LOD and material, else 0.15 (the material
		// block's, 0x10040070 +0xe0).
		float staticGloss = 0.15f;
		// A bundled or skinned mesh's normal map (RaShaderBM/SM
		// _HASNORMALMAP_) and its space: 1 the tangent's, 2 the object's
		// (RaShaderSM _OBJSPACENORMALMAP_); 0 none.
		SDL_GPUTexture* normalMap = nullptr;
		int normalKind = 0;
		// A static mesh's RaShaderSTM per-pixel maps: the detail normal map
		// ("_deb", NDetail; its alpha the parallax height), the crack normal
		// map ("_crb", NCrack), and ParallaxScaleBias's scale (0: none).
		SDL_GPUTexture* detailNormal = nullptr;
		SDL_GPUTexture* crackNormal = nullptr;
		float parallax[2] = {0, 0};
		// Lit with the level's envmap (EnvMapManager.h).
		bool envMap = false;
		// A tree's leaves (a static mesh under "vegitation", RendDX9
		// 0x100de8xx; its alpha-tested materials): RaShaderLeaf's sway.
		bool leaf = false;
	};
	struct RigBone {
		int node;
		anim::Matrix meshToBone;
	};
	struct GpuMesh {
		bool bundled = false;
		bool skinned = false;
		bool radiusShadowBounds = false; // GeometryTemplate.useRadiusForShadowDepth / Spatial
		// GeometryTemplate.isTitanInterior / isTitanExterior /
		// titanExteriorPartIndex (TitanParts.h) and the LOD's box (mesh
		// space; the interiors' rooms tell whether the camera is inside).
		bool titanInterior = false;
		bool titanExterior = false;
		int titanPart = 0;
		Vec3f boxMin, boxMax;
		size_t geometries = 1; // in its file
		size_t lodCount = 1;   // of the geometry drawn
		std::vector<float> lodDistances; // how far each LOD is drawn (empty: the defaults)
		// The exterior's first LODs dropped (GeometryTemplate.maxSkip3pLods
		// capped by the geometry quality's renderer +0x910): its LOD 0 drawn
		// from LOD skippedLods on.
		int skippedLods = 0;
		// The vertices' spheres by part (bundled meshes; one, part 0, for
		// static ones), in the part's space; unknown for skinned meshes.
		std::vector<BoundingSphere> partBounds;
		BoundingSphere bounds; // all of them, unmoved
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
		std::string geometry;  // its geometry template
		int shownGeometry = -1; // the geometry drawn (-1: the exterior)
		int shownLod = 0;
		// The exterior's LODs by distance, loaded when first drawn: the mesh
		// and that LOD's lightmap (the atlas keys name the LOD).
		struct Lod {
			bool resolved = false;
			const GpuMesh* mesh = nullptr;
			SDL_GPUTexture* lightmap = nullptr;
			float lightmapTransform[4] {1, 1, 0, 0};
		};
		std::vector<Lod> lods;
		// HemiMapConstantColor from a Titan interior's map (the exe's per
		// object cache, BM +0x2d0 / SM +0x570: kept while the object is
		// outside every map; 0.5 0.5 0.5 at first).
		mutable float titanHemi[3] = {0.5f, 0.5f, 0.5f};
		bool overgrowth = false; // Object.isOvergrowth
		Lod overgrowthLod;       // its "<geometry>_lod" mesh
	};
	// What the world pass draws of an instance this frame.
	struct DrawItem {
		const Instance* instance;
		const GpuMesh* mesh;
		SDL_GPUTexture* lightmap;
		const float* lightmapTransform;
	};
	std::vector<DrawItem> m_drawList;
	size_t m_culledInstances = 0;
	// Culls the instances against the camera and picks their LODs.
	void prepareInstances(const Camera& camera, const Mat4& viewProjection);
	BoundingSphere instanceBounds(const Instance& instance, const GpuMesh& mesh) const;
	void lightmapFor(const LevelSources& sources, const std::string& geometry, int lod, const Mat4& world,
		SDL_GPUTexture*& lightmap, float transform[4]);
	struct TerrainPatch {
		Vec3f boundsMin, boundsMax;
		SDL_GPUBuffer* vertices = nullptr; // full detail; terraindata.raw's compiled vertices with m_terrainCells
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
	// A texture by path, its top mip levels skipped by its class at the
	// texture quality (RendDX9's texture classes, scene::TextureClass).
	SDL_GPUTexture* texture(const io::FileManager& files, const std::string& path,
		scene::TextureClass textureClass = scene::TextureClass::Default);
	void addTemplate(const LevelSources& sources, const world::ObjectTemplate& objectTemplate, const Mat4& local,
		const Mat4& objectWorld, uint32_t object, int depth);
	void addObject(const LevelSources& sources, const world::Object& object);
	void collectParts(const LevelSources& sources, const world::ObjectTemplate& objectTemplate, const Mat4& relative,
		std::vector<Mat4>& parts, int depth, uint32_t object = 0) const;
	void buildTerrain(const LevelSources& sources);
	// The primary terrain's LODs (terraindata.raw's compiled patches, drawn
	// as RendDX9's terrain culler draws them: cells of terrainCuller.
	// baseCellSize metres, each at LOD 0..2 by its distance, its edges
	// stitched to finer neighbours, geomorphed in the vertex shader).
	struct TerrainNodeRange {
		uint32_t start = 0;
		uint32_t count = 0;
	};
	bool buildTerrainLods(const LevelSources& sources, int quads);
	// This frame's cells: visibility, LODs, stitches (0x1009f210, 0x1009cfa0).
	void updateTerrainCells(const Camera& camera, const Frustum& frustum);
	std::vector<TerrainLodCell> m_terrainCells; // row-major from the -x/-z corner (TerrainLod.h)
	std::vector<int> m_terrainCellNodes;        // each cell's node of the shared LOD index tree
	std::vector<std::array<TerrainNodeRange, 33>> m_terrainNodeRanges; // [node][lod * 11 + mask]
	int m_terrainCellsPerSide = 0;
	int m_terrainCellsPerPatch = 0; // per side
	float m_terrainLodSwitch = 0;   // this frame's LOD 0 distance (the culler's +0)
	float m_terrainHeightAboveGround = 0; // the camera's (terrain +0x294)
	float m_baseFovY = 0; // the view's first field of view: the zoom is against it (0x10066b30)
	float viewZoom(const Camera& camera) const;
	// The secondary heightmaps around the level (the surrounding terrain).
	void buildSurroundingTerrain(const LevelSources& sources);
	SDL_GPUTexture* cubeTexture(const io::FileManager& files, const std::string& path);
	void buildSky(const LevelSources& sources);
	void buildWater(const LevelSources& sources);
	void loadEnvMaps(const LevelSources& sources);
	void buildRoads(const LevelSources& sources);
	void buildUndergrowth(const LevelSources& sources);
	// Generates the undergrowth patches in view and drops distant ones.
	void updateUndergrowth(const Camera& camera);
	void drawUndergrowth(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection,
		const Camera& camera, const scene::LightSettings& lights);
	std::shared_ptr<anim::Skeleton> skeleton(const io::FileManager& files, const std::string& path);
	std::shared_ptr<anim::Animation> animation(const io::FileManager& files, const std::string& path);
	void loadLightmapAtlas(const LevelSources& sources);
	// The overgrowth's settings (Overgrowth.con) and texture atlases
	// (OvergrowthAtlas.tai, OvergrowthAtlas_alphatest.tai).
	void loadOvergrowth(const LevelSources& sources);
	std::map<std::string, AtlasEntry> m_overgrowthAtlas;          // by texture path (lower case)
	std::map<std::string, AtlasEntry> m_overgrowthAlphaTestAtlas; // the alpha-tested ones
	float m_overgrowthCloseDistance = 0; // the distant mesh from here (0: no overgrowth)
	// Overgrowth.viewDistance x closeDistPercentage, and viewDistanceScale
	// (+0x110) as Overgrowth.con set it and as the geometry quality's
	// switch would (0x10010040: 0.4 / 0.7 / 1) when the quality was loaded.
	float m_overgrowthCloseBase = 0;
	float m_overgrowthScriptScale = 1;
	float m_overgrowthQualityScale = 1;
	// This frame's close distance: the script's scale, or the quality's
	// once the video settings change it after the level's script ran (the
	// switch writes +0x110 when they are applied).
	float overgrowthCloseDistance() const;

	// The skinned instances' bone poses this frame (shared by the shadow
	// and world passes), and a submesh's palette into `transforms`.
	std::map<const Instance*, std::vector<anim::Matrix>> m_poses;
	const std::vector<anim::Matrix>& poseFor(const Instance& instance, float seconds);
	static void skinPalette(const GpuMesh& mesh, const Submesh& submesh, const std::vector<anim::Matrix>& pose,
		TransformUniforms& transforms);

	// The dynamic shadows (LevelRendererPost.cpp): an orthographic map
	// along the sun over a region ahead of the camera, its centre snapped
	// so the projection holds still between moves; the dynamic objects
	// redrawn into one map each frame, the static meshes into another only
	// when the region moves.
	struct ShadowRegion {
		bool enabled = false;
		Vec3f center;
		float radius = 0;      // half the region's side, metres
		float depth = 0;       // the light's depth range
		Mat4 viewProjection;   // world -> the light's clip space
		uint32_t size = 0;     // texels across
	};
	ShadowRegion m_shadow;
	SDL_GPUTexture* m_shadowDynamic = nullptr;
	SDL_GPUTexture* m_shadowStatic = nullptr;
	uint32_t m_shadowMapSize = 0;
	bool m_shadowStaticValid = false;
	bool m_shadowDrawn = false; // the maps hold a frame's casters (shadowUpdatesEnabled 0 keeps them)
	Vec3f m_shadowStaticCenter;
	size_t m_shadowStaticInstances = 0; // m_instances' count when the static map was drawn
	ShadowUniforms m_shadowUniforms;
	void renderShadows(SDL_GPUCommandBuffer* commands, const Camera& camera, const scene::LightSettings& lights,
		float seconds);
	void drawShadowCasters(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* map, bool statics, float seconds);
	// After binding a lit pipeline: its Shadows block at uniform `slot` and
	// the maps (and the hemi map) from sampler `firstSampler`.
	void useShadows(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, uint32_t slot, uint32_t firstSampler,
		bool hemi, bool undergrowth = false);

	// The hemi map (HemiMapManager.setBaseHemiMap).
	SDL_GPUTexture* m_hemiMap = nullptr;
	// Its alpha on the CPU (the particles' calcParticleLighting) and the
	// share of the sun there (1 without a map).
	std::vector<uint8_t> m_hemiAlpha;
	uint32_t m_hemiAlphaSize = 0;
	float hemiShadowAt(float x, float z) const;
	void loadHemiMap(const LevelSources& sources);
	// The Titan interiors' hemi maps (HemiMapManager.loadTitanHemiMap),
	// sampled on the CPU as RendDX9 does (0x100ecb10), and this frame's
	// placement of each in its Titan (0x100eda70).
	struct TitanHemiMap {
		std::vector<uint8_t> rgba; // the top level, RGBA8
		uint32_t width = 0, height = 0;
		float offset[3] {};
		float size = 0;
		int team = 0; // the Titan's team (1 PAC, 2 EU)
		bool placed = false;
		Vec3f origin;    // the Titan's position
		Vec3f axes[3];   // its rotation's rows
	};
	std::vector<TitanHemiMap> m_titanHemiMaps;
	// The camera inside a Titan this frame (the object drawer's slot 0x40,
	// TitanParts.h), and the hull parts' LOD pairs.
	bool m_cameraInTitan = false;
	TitanExteriorLods m_titanExteriorLods;
	bool cameraInTitan(const Camera& camera) const;
	void loadTitanHemiMaps(const LevelSources& sources);
	void placeTitanHemiMaps(const Camera& camera);
	// HemiMapConstantColor and HeightOverTerrain of a bundled or skinned
	// instance this frame (LightingUniforms::hemiObject).
	void hemiObjectFor(const Instance& instance, const GpuMesh& mesh, float out[4]) const;

	// The fog the camera is in this frame (Fog.h; draw()).
	ViewFog m_viewFog;

	// The sky's cloud layers and sun flare.
	SDL_GPUTexture* m_cloudTextures[2] = {};
	int m_cloudLayers = 0;
	float m_cloudOffsets[4] {};
	uint64_t m_lastSkyTicks = 0;
	SDL_GPUTexture* m_flareTexture = nullptr;
	const GpuMesh* m_flareMesh = nullptr;
	SDL_GPUBuffer* m_flareVertices = nullptr;
	void buildSkyLayers(const LevelSources& sources);
	void drawSky(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection,
		const Camera& camera);
	void drawSunFlare(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection,
		const Camera& camera);

	// The post-production (LevelRendererPost.cpp): the world drawn into
	// m_sceneColor, the bloom chain at a quarter of its size, the
	// tinnitus's last frames, the cloaked meshes' mask; then post.frag into
	// the target, the HUD and menu over it.
	SDL_GPUTexture* m_sceneColor = nullptr;
	uint32_t m_sceneSize[2] {};
	SDL_GPUTexture* m_bloomTargets[2] = {};
	uint32_t m_bloomSize[2] {};
	SDL_GPUTexture* m_history[4] = {};
	int m_historyNext = 0;
	bool m_historyValid = false; // the frames above hold a running tinnitus's
	SDL_GPUTexture* m_camoMask = nullptr;
	SDL_GPUTexture* m_postTextures[4] = {}; // noise, random, frame, scanline
	PostProductionState m_postState;
	uint64_t m_lastPostTicks = 0;
	std::vector<DrawItem> m_cloakedList;
	bool postEnabled() const;
	void ensurePostTargets(uint32_t width, uint32_t height);
	void loadPostTextures(const LevelSources& sources);
	// Draws the cloaked meshes into the mask (the world's depth loaded).
	void drawCamouflage(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* depth, const Mat4& viewProjection, float seconds);
	// The bloom chain and the tinnitus's history from m_sceneColor, then
	// the post pass into `pass` (already begun on the target).
	PostUniforms preparePost(SDL_GPUCommandBuffer* commands, bool cloaked);
	void drawPost(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const PostUniforms& uniforms);
	void fullscreenPass(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, SDL_GPUTexture* source,
		const BloomUniforms& uniforms);
	void releasePostTargets();

	GpuContext& m_gpu;
	std::map<std::string, std::unique_ptr<GpuMesh>> m_meshes; // by geometry name (lower case)
	std::map<std::string, SDL_GPUTexture*> m_textures;         // by path (lower case)
	std::map<std::string, SDL_GPUTexture*> m_cubeTextures;     // by path (lower case)
	std::map<SDL_GPUTexture*, std::array<uint32_t, 2>> m_textureSizes; // width, height (the parallax's scale)
	// texture()'s front cache by the path as asked and the class (the mip
	// skip it was loaded with, the texture).
	std::array<std::unordered_map<std::string, std::pair<int, SDL_GPUTexture*>>, 16> m_texturesByPath;
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
		bool repeat = false; // trails tile their texture along themselves
	};
	SDL_GPUBuffer* m_particleVertices = nullptr;
	// The heat shimmer's sprites (ParticleHeatShimmer) and the copy of the
	// scene they displace.
	SDL_GPUBuffer* m_shimmerVertices = nullptr;
	std::vector<ParticleGroup> m_shimmerGroups;
	SDL_GPUTexture* m_shimmerSource = nullptr;
	uint32_t m_shimmerSize[2] = {0, 0};
	void buildShimmer(const Camera& camera);
	void drawShimmer(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection,
		const Camera& camera, uint32_t width, uint32_t height);
	std::array<float, 3> particleLight(const effects::Vec3& at, float lightMapOffset) const;
	// The particle manager's settings in use (VideoSettings' effects row applied).
	scene::ParticleSystemSettings m_particleSettings;
	// The undergrowth's view distance: Undergrowth.cfg's times VideoSettings'
	// terrain quality (undergrowth.viewDistanceScale 0.5 / 0.75 / 1, 0x10010040).
	float undergrowthViewDistance() const;
	effects::ParticleView particleView(const Camera& camera) const;
	struct OverlayGroup {
		SDL_GPUTexture* texture;
		uint32_t firstVertex;
		uint32_t vertexCount;
		int blend[2] = {0, 0}; // HudQuad::blend (D3DBLEND source, destination)
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
	// This frame's point lights (the effects' LightSources nearest the
	// camera), copied into the lit shaders' uniforms.
	std::vector<effects::PointLight> m_pointLights;
	void collectPointLights(const Camera& camera);
	// The LightSources in the placed objects' template trees (the levels'
	// light bundles: ambient_small, houselight_big ...; lights carried by
	// vehicles), placed with their objects each frame.
	struct ObjectLight {
		uint32_t object = 0;
		Mat4 local; // in the object's space
		effects::LightSourceLook look;
	};
	std::vector<ObjectLight> m_objectLights;
	void applyPointLights(LightingUniforms& uniforms) const;
	// mesh.frag's samplers 8-10: a static material's detail and crack
	// normal maps (flat without) and this frame's envmap.
	void bindMeshLayers(SDL_GPURenderPass* pass, const Submesh* submesh) const;
	// This frame's decals: one buffer, drawn in groups by texture.
	SDL_GPUBuffer* m_decalVertices = nullptr;
	std::vector<ParticleGroup> m_decalGroups;
	void buildDecals();
	// Mesh particles (MeshParticleSystems) of the effect managers.
	void drawMeshParticles(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Mat4& viewProjection,
		const scene::LightSettings& lights, std::initializer_list<const effects::EffectManager*> managers);
	// Effect managers' particles into a buffer (released and rebuilt).
	void buildParticleBuffer(std::initializer_list<const effects::EffectManager*> managers, SDL_GPUBuffer*& buffer,
		std::vector<ParticleGroup>& groups, const Camera& camera, bool weather = false, bool zoomed = false);
	// Rain and snow (LevelSources::rain), stepped by buildParticles.
	effects::Weather m_weather;
	uint64_t m_lastWeatherTicks = 0;
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
	SDL_GPUBuffer* m_terrainLodIndices = nullptr; // terraindata.raw's shared LOD index buffer
	// The sea: grids over the heightmaps (16-bit indices each) and a ring
	// beyond them to the horizon.
	struct WaterBlock {
		SDL_GPUBuffer* vertices = nullptr;
		SDL_GPUBuffer* indices = nullptr;
		uint32_t indexCount = 0;
		Vec3f boundsMin, boundsMax;
	};
	std::vector<WaterBlock> m_waterBlocks;
	std::vector<SDL_GPUTexture*> m_waterFrames; // the normal map frames (Textures/waterbump)
	SDL_GPUTexture* m_waterEnvMap = nullptr;     // <level>/Water/envmap.dds, a cube map
	// The level's envmaps (EnvMaps.h) and the one nearest the camera this
	// frame (null: none, the materials drawn without).
	struct EnvMap {
		EnvMapEntry entry;
		SDL_GPUTexture* cube = nullptr;
	};
	std::vector<EnvMap> m_envMaps;
	SDL_GPUTexture* m_envMap = nullptr;
	SDL_GPUTexture* m_waterVolume = nullptr;     // Textures/watervolume.dds (the 3D variants)
	struct SurroundingPatch {
		SDL_GPUBuffer* vertices = nullptr;
		SDL_GPUTexture* colormap = nullptr;
		SDL_GPUTexture* lowComponent = nullptr;
		Vec3f boundsMin, boundsMax;
	};
	std::vector<SurroundingPatch> m_surrounding;
	SDL_GPUBuffer* m_surroundingIndices = nullptr;
	uint32_t m_surroundingIndexCount = 0;
	SDL_GPUTexture* m_lowDetailTexture = nullptr; // <level>/lowDetailTexture.dds
	float m_farTiling[4] {};                      // side x, side y, top, y offset
	float m_surroundingFarTiling[4] {};           // STFARTEXTILING (0x100af7e0)
	float m_surroundingTexScale = 0;              // STTEXSCALE.x: world metres to its low-detail coordinates
	float m_terrainExtent = 0;                    // the primary heightmap's size in metres
	std::array<float, 4> m_terrainWater {};       // TerrainWaterColor (rgb), WaterHeight (w; below -1e8: none)
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
