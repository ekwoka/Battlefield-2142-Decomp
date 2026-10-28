// Level lighting and fog as set by Sky.con ("lightManager.*", "renderer.fog*").
//
// The renderer reads these each frame. Colors are linear 0..1 except the
// fog color, which scripts give as 0..255 (RendDX9 scales it by 1/255).
// Fog start/end: RendDX9 (0x100355c0, called from 0x10020000) uses
// fogStartEndAndBase.x as the start and scales .y by a view-distance factor
// for the end; that factor isn't identified yet, so the end is fixed here.
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
};

// The sky dome ("Skydome.*" in Sky.con): a template whose mesh is drawn
// around the camera with the sky texture.
struct SkySettings {
	std::string skyTemplate;
	std::string skyTexture; // without ".dds"
	float domeRotation = 0; // degrees about y
	std::array<float, 3> domePosition {0, 0, 0};
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
	bool lineDisplay = false;
	float lineLength = 3;
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
	float lod0SwitchStart = 0.4f;
	float lod0SwitchStop = 0.9f;
	float lod1SwitchStart = 2.0f;
	float lod1SwitchStop = 2.7f;
	float lod2Start = 3.0f;
	int baseCellSize = 64;
};

void registerRendererConsoleMethods(Console& console, RendererSettings& settings);
void registerLightConsoleMethods(Console& console, LightSettings& settings);
void registerRainConsoleMethods(Console& console, RainSettings& settings);
void registerWaterConsoleMethods(Console& console, WaterSettings& settings);
void registerSkyConsoleMethods(Console& console, SkySettings& settings);

} // namespace dice::hfe::scene
