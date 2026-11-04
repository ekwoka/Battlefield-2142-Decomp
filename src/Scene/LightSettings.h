// Level lighting and fog as set by Sky.con ("lightManager.*", "renderer.fog*").
//
// The renderer reads these each frame. Colors are linear 0..1 except the
// fog color, which scripts give as 0..255 (RendDX9 scales it by 1/255).
// Fog start/end/base: RendDX9 scales the end and the fourth value by the
// view distance (0x10028020) and makes RaCommon.fx's FogRange of them
// (0x100355c0), in metres (render/Fog.h, Shaders/fog.glsl).
#pragma once

#include <array>
#include <string>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::scene {

struct LightSettings {
	std::array<float, 3> sunDirection {-0.25f, -0.93f, -0.26f}; // direction the light travels
	std::array<float, 3> sunColor {1, 1, 1};
	std::array<float, 3> skyColor {0.3f, 0.3f, 0.3f};
	std::array<float, 3> ambientColor {0.1f, 0.1f, 0.1f};
	// Lightmapped static meshes use these instead of sun/sky color.
	std::array<float, 3> staticSunColor {1, 1, 1};
	std::array<float, 3> staticSkyColor {0.5f, 0.5f, 0.5f};
	// Terrain ("terrain.sunColor", "terrain.GIColor"). Those methods only
	// exist in the editor; the game reads the colors from terraindata.raw.
	std::array<float, 3> terrainSunColor {0.7f, 0.7f, 0.5f};
	std::array<float, 3> terrainGIColor {1, 1, 1};
	std::array<float, 3> fogColor {160, 170, 180}; // 0..255
	std::array<float, 4> fogStartEndAndBase {10, 100, 1, 0};
	// The rest of lightManager.* (RendDX9 0x1007c509..0x1007e64c). The
	// renderer reads enableSun (no sun term without it) and hemiLerpBias
	// (HemiMapConstants.w); the tree, effect and specular colours are kept
	// for the shaders that take them (RaShaderLeaf/Trunk's TreeSkyColor...,
	// Particles.fx's EffectSunColor/EffectShadowColor, the specular terms).
	std::array<float, 3> treeAmbientColor {0, 0, 0};
	std::array<float, 3> treeSunColor {1, 1, 1};
	std::array<float, 3> treeSkyColor {0, 0, 0};
	std::array<float, 3> staticSpecularColor {0, 0, 0};
	std::array<float, 3> singlePointColor {1, 1, 1};
	std::array<float, 3> effectSunColor {1, 1, 1};
	std::array<float, 3> effectShadowColor {0.5f, 0.5f, 0.5f};
	std::array<float, 3> sunSpecColor {1, 1, 1};
	float defaultEffectLightAffectionFactor = 1;
	std::string groundHemi;
	float hemiLerpBias = 1;
	float lightCullDistance = 100;
	int maxLightsPerBundledMesh = 4;
	int maxLightsPerStaticMesh = 4;
	int maxLightsPerSkinnedMesh = 4;
	int maxLightsPerTerrainPatch = 4;
	float skinnedMeshShaderLodDistance = 50;
	std::array<float, 2> lightMapAmbientAlphaCutoffRange {0, 1};
	bool enableSun = true;
	bool enableStencilCullerReset = false;
};

// The sky dome ("Skydome.*" in Sky.con): a template whose mesh is drawn
// around the camera with the sky texture.
struct SkySettings {
	std::string skyTemplate;
	std::string skyTexture; // without ".dds"
	float domeRotation = 0; // degrees about y
	std::array<float, 3> domePosition {0, 0, 0};
	float domeSize = 1;
	// The cloud layers (SkyDome.fx SkyDomeNV3x / DualClouds): textures on
	// the dome's second coordinate set, scrolled (scrollDirection per
	// second), faded out from fadeCloudsDistances.x over .y metres of the
	// dome's radius; two layers blend by cloudLerpFactors (0x100fd2f0).
	bool hasCloudLayer = false;
	bool hasCloudLayer2 = false;
	std::string cloudTemplate;
	std::string cloudTexture;  // without ".dds"
	std::string cloudTexture2;
	std::array<float, 2> scrollDirection {0, 0};
	std::array<float, 2> scrollDirection2 {0, 0};
	std::array<float, 2> fadeCloudsDistances {1000, 1100};
	std::array<float, 2> cloudLerpFactors {0.5f, 0.5f};
	// The sun flare (SkyDomeSunFlare, 0x100fd900): flareTemplate's mesh with
	// flareTexture along -flareDirection, added; its strength is the share of
	// it that isn't occluded (plus flareFadeAdd, times flareFadeMul).
	std::string flareTemplate;
	std::string flareTexture;  // without ".dds"
	std::array<float, 3> flareDirection {0, -1, 0};
	float flareFadeAdd = 0;
	float flareFadeMul = 1;
};

// Sea water ("renderer.water*" in Water.con).
struct WaterSettings {
	std::array<float, 3> color {0.1f, 0.12f, 0.12f};
	std::array<float, 4> specularColor {1, 1, 1, 1}; // w: intensity
	float specularPower = 50;
	float alphaAdd = 0.3f;
	float alphaHeightMul = 0;
	std::array<float, 2> scroll {0, 0};
	float animSpeed = 0;
	// terrain.waterSunIntensity (0x100766e2 -> 0x100767e9): the water
	// material's PointColor (+0x4c of its block, 0x10040070) becomes
	// (1, intensity, 0.8, 1); RaShaderWaterBase.fx without a lightmap takes
	// it as the lightmap, its g the sun's share (shadFac). No call leaves the
	// block's 0.
	float sunIntensity = 0;
	// renderer.waterFogColor (0..255, kept / 255 at +0x54, 0x10049640) and
	// renderer.waterFogStartEndAndBase (+0x60, 0x10049680) of the fog object
	// (0x1004a590's defaults): the fog with the camera at or below the sea
	// (0x100497a0 / 0x10049850; Fog.h).
	std::array<float, 3> fogColor {51.5f, 51.5f, 128.0f};
	std::array<float, 4> fogStartEndAndBase {10, 250, 1, 0.5f};
};

// Rain and snow (RendDX9's rainManager.*, 0x1007e761..0x1008009a; set by
// the levels' Sky.con, drawn by Shaders/Rain.fx): particles in a box about
// the camera, points (snow) or lines (rain).
struct RainSettings {
	bool enabled = false;
	int particleCount = 0;
	std::array<float, 4> pos {};             // where the editor left it
	std::array<float, 3> offset {0, 10, 0};  // the box's centre from the camera
	std::array<float, 3> emitRadius {30, 30, 30};
	std::array<float, 3> particleScale {0, 0.1f, 0}; // the point size's distance terms
	std::array<float, 4> particleColor {1, 1, 1, 1};
	float particleSize = 8, particleMaxSize = 12; // pixels
	std::array<float, 3> particleVelocity {0, -1, 0};
	float particleSpeed = 1;
	float fadeOutRange = 0.875f; // of the box, where the fade starts
	std::array<float, 3> maxDeviation {};
	std::array<float, 2> angleSpeed {};
	// The deviation groups' walk (RendDX9 0x1011a860, the rain manager's
	// +0x6384 / +0x6388 / +0x638c, constructor 0x1011d370): one chance in
	// `randomness` a frame that a group's speeds change to (a random
	// 0..randomDeviationAngleSpeed - 1) x maxRandomChange.
	int randomness = 1000;
	float maxRandomChange = 0.3f;
	int randomDeviationAngleSpeed = 1;
	bool lineDisplay = false;
	float lineLength = 3;
	// rainManager.freeze (the manager's +0x65, 0; 0x1007fc4b -> slot 0xa0):
	// the box stays where it was (effects::Weather).
	bool freeze = false;
	std::string texture; // without ".dds"
};

// RendDX9 settings the level scripts set (Init.con, Terrain.con).
struct RendererSettings {
	// renderer.global{Static,Bundle,Skinned}MeshLodDistanceScale (renderer
	// +0x9d8 / +0x9dc / +0x9e0, 1 at 0x1002aba0): times every switch distance.
	float staticMeshLodDistanceScale = 1;
	float bundleMeshLodDistanceScale = 1;
	float skinnedMeshLodDistanceScale = 1;
	// terrainCuller.* (the culler's constructor 0x1009d5a0 and 0x1009c820;
	// setters 0x1009c9b0..0x1009ca60, 0x1009cf40): the LOD 0 distance
	// (taken between baseCellSize and 1000), the morph ranges and the LOD 2
	// start as multiples of it, the cell size in metres (16..512, a power of
	// two, at least 32 and at most a patch; checked by the renderer).
	int lodSwitchDistance = 100;
	// Set by a script (two shipped levels): it then holds over the terrain
	// quality's 64 / 80 / 100, which RendDX9's quality switch sets before
	// the level loads (0x10029e20 -> 0x10010040).
	bool lodSwitchDistanceSet = false;
	float lod0SwitchStart = 0.4f;
	float lod0SwitchStop = 0.9f;
	float lod1SwitchStart = 2.0f;
	float lod1SwitchStop = 2.7f;
	float lod2Start = 3.0f;
	int baseCellSize = 64;
	// renderer.indoorSkinnedMeshCullDistance / indoorBundledMeshCullDistance
	// (+0x598 / +0x59c, 53 at 0x1002aba0): with the camera inside a Titan
	// the skinned (0x10162070) and bundled (0x100c5030) meshes farther than
	// these aren't drawn.
	float indoorSkinnedMeshCullDistance = 53;
	float indoorBundledMeshCullDistance = 53;
	// ObjectDrawer.isInsideTitanOverride / showAllTitanParts (0x1007597d /
	// 0x10075a7e: the drawer's +0x91 / +0x92): the camera counts as inside a
	// Titan (slot 0x40, 0x100b9db0: +0x90 or +0x91), and the static meshes
	// skip the interior / exterior culling (slot 0x48; TitanParts.h).
	bool insideTitanOverride = false;
	bool showAllTitanParts = false;
	// The renderer's draw switches (renderer.draw*, the console objects
	// 0x1004c7a0..0x1004e0b0; the renderer's bytes, all 1 at 0x1002aba0),
	// read by the frame 0x1002cef0 and the views 0x100264c0 / 0x10027630:
	// drawObjects +0x68c (every mesh), drawBundledMeshes +0x690,
	// drawStaticMeshes +0x691, drawSkinnedMeshes +0x692, draw1pMeshes +0x594
	// (the first-person pass), drawParticles +0x68e (the particles and mesh
	// particles), drawSkyDome +0x6a2 (the sun flare needs it too),
	// drawSunFlare +0x6a3, drawRoads +0x693, drawTerrain +0x694,
	// drawUndergrowth +0x695, drawOvergrowth +0x696, drawNametags +0x697,
	// drawTrees +0x698 (the vegetation: 0x100e0000's quality copy sets it
	// with the undergrowth and overgrowth; here the static meshes with
	// leaves).
	bool drawObjects = true;
	bool drawBundledMeshes = true;
	bool drawStaticMeshes = true;
	bool drawSkinnedMeshes = true;
	bool draw1pMeshes = true;
	bool drawParticles = true;
	bool drawSkyDome = true;
	bool drawSunFlare = true;
	bool drawRoads = true;
	bool drawTerrain = true;
	bool drawUndergrowth = true;
	bool drawOvergrowth = true;
	bool drawNametags = true;
	bool drawTrees = true;
	// renderer.drawFps (+0x69c, 0; 0x10053ae0): above 0 it becomes 1, and the
	// frame (0x10028bb0 -> 0x10027570) prints "%.1f / %.1f", the frame's
	// milliseconds and frames a second, at (6, 6) of the debug text.
	int drawFps = 0;
	// renderer.tweakNearPlane / tweakNearPlane1p (+0x584 0.041 / +0x588
	// 0.16; 0x10051b40 / 0x10051ce0): the world view's and the first-person
	// view's near planes (0x10028bb0 hands them to the views' slot 0x34; the
	// first person's far plane stays +0x58c, 15). The world's is the device
	// setting's until a script sets it: the rewrite keeps its camera's near
	// plane unless one does (`*Set`).
	float tweakNearPlane = 0.041f;
	float tweakNearPlane1p = 0.16f;
	bool tweakNearPlaneSet = false;
	bool tweakNearPlane1pSet = false;
	// renderer.minCullDistance (0x10051990: the renderer's slots 0x84 / 0x88,
	// both the value): objects whose screen size falls under it are culled
	// by the original's object drawer; kept, the rewrite culls by frustum.
	float minCullDistance = 0;
	// Kept for the console only: renderer.hud (+0x6a7, 0, read by its getter
	// alone), renderer.vsync / presentAsync (the device's 0x1000adb0 /
	// 0x1000adf0; the swapchain's present mode is the platform layer's),
	// renderer.useSunScaling (device +0x41), renderer.alphaToMask (+0x57c),
	// renderer.enableHWOcclusion (the occlusion query object's 0x10004900),
	// renderer.drawGraphs (+0x6a0, the profiler's graphs),
	// renderer.renderAheadLimit (slots 0xb0 / 0xb4), Renderer.fieldOfView1p
	// (DAT_1020d424, read by nothing but its getter).
	bool hud = false;
	bool vsync = false;
	bool presentAsync = false;
	bool useSunScaling = false;
	bool alphaToMask = false;
	bool enableHWOcclusion = false;
	bool drawGraphs = false;
	int renderAheadLimit = 0;
	float fieldOfView1p = 0;
	// The road renderer's (RoadCompiledRenderer, constructor 0x100ba7e0;
	// Renderer.road* 0x100727ee..0x10072b02): roadLodDistMod (+0x88, 1.3)
	// scales the roads' view distance (LevelRenderer's roads);
	// roadDepthBias / roadSlopeScaleDepthBias (+0x8c / +0x90, -1e-5 each)
	// are D3DRS_DEPTHBIAS / SLOPESCALEDEPTHBIAS (0x100baa70), which the
	// road pipeline's own bias stands for; disableFadeOnRoads (+0x84, 0) is
	// read by nothing but its accessor.
	float roadLodDistMod = 1.3f;
	float roadDepthBias = -1e-5f;
	float roadSlopeScaleDepthBias = -1e-5f;
	bool disableFadeOnRoads = false;
	// StaticMeshRenderer.enableRendering (0x10079e04) calls the static mesh
	// renderer's slot 0x18, as renderer.drawStaticMeshes does (0x1004cfa0):
	// the same switch. StaticMeshRenderer.noLods (+0x90 of the static mesh
	// renderer, 0x10079f17): every static mesh at its first LOD.
	bool staticNoLods = false;
	// undergrowth.enabled (0x100860c9 -> the undergrowth's slot 0x30): no
	// undergrowth drawn while clear.
	bool undergrowthEnabled = true;
	// undergrowth.viewDistanceScale / viewDistanceFade (the system's +0x578 /
	// +0x57c, 1 / 0.2, both clamped to 0..1; 0x100867bd / 0x100868c4): the
	// patches' view distance times ViewDistance, and the outer share of it
	// over which plants grow out of the ground (0x1010e270). The scale is the
	// quality switch's until a script sets it.
	float undergrowthViewDistanceScale = 1;
	bool undergrowthViewDistanceScaleSet = false;
	float undergrowthViewDistanceFade = 0.2f;
	// undergrowth.zPassPatchCount (+0x574, 0): the nearest N patches drawn
	// depth-only first (ZOnly); the rewrite's single pass needs none.
	int undergrowthZPassPatchCount = 0;
};

void registerRendererConsoleMethods(Console& console, RendererSettings& settings);
void registerLightConsoleMethods(Console& console, LightSettings& settings);
void registerRainConsoleMethods(Console& console, RainSettings& settings);
void registerWaterConsoleMethods(Console& console, WaterSettings& settings);
void registerSkyConsoleMethods(Console& console, SkySettings& settings);

} // namespace dice::hfe::scene
