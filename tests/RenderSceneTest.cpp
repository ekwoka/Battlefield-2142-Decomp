#include "Render/HudBlend.h"
// The renderer's GPU-free logic: the Titans' part culling (TitanParts.h),
// the view's fog (Fog.h), the shadows', hemi maps', sky's and normal maps'
// arithmetic (SceneMath.h), the overhead view's camera (OverheadView.h),
// the quality levels' mip skips and the undergrowth's simple shader, the
// geometry templates' Titan and material settings, and the sprites'
// flipbooks and the effect bundles' age, speed and rotation settings.
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Animation/AnimationConsole.h"
#include "Animation/AnimationSystem.h"
#include "Animation/Pose.h"
#include "Effects/Effects.h"
#include "Geometry/GeometryTemplateManager.h"
#include "Render/AnimatedUV.h"
#include "Render/DistanceFade.h"
#include "Render/MeshLights.h"
#include "Render/OvergrowthLod.h"
#include "Render/EnvMaps.h"
#include "Render/Fog.h"
#include "Render/OverheadView.h"
#include "Render/RoadStatics.h"
#include "Render/SceneMath.h"
#include "Render/SimpleShadowmap.h"
#include "Render/StaticLightmaps.h"
#include "Render/TextureSuffix.h"
#include "Render/TitanParts.h"
#include "Render/TreeMaterials.h"
#include "Geometry/MeshFile.h"
#include "IO/File/ZipArchive.h"
#include "IO/File/FileManager.h"
#include "Scene/EffectSettings.h"
#include "Scene/LightSettings.h"
#include "Scene/OvergrowthSettings.h"
#include "TestMain.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

bool near(float a, float b, float epsilon = 1e-4f)
{
	return std::fabs(a - b) < epsilon;
}

} // namespace

// The overgrowth's far palms (Suez Canal): the shipped "_lod" mesh marks its
// fronds opaque (alpha mode 0), yet RendDX9 draws every material without
// "tile_" in its texture name with LeafOG, which alpha-tests; drawn as an
// opaque trunk they filled each frond card with the texels' hidden colour.
TEST(RenderScene_OvergrowthLodPalmFrondsAreAlphaTestedLeaves)
{
	using render::TreeMaterial;
	CHECK(render::overgrowthMaterial("objects/vegitation/africa/textures/tile_africa_de_lod.dds") == TreeMaterial::TrunkBase);
	CHECK(render::overgrowthMaterial("objects/vegitation/africa/textures/leaf_af_palmtree_b_de_lod.dds") == TreeMaterial::Leaf);
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	auto zip = io::ZipArchive::open(std::string(gameDir) + "/mods/bf2142/Objects/Vegitation_client.zip");
	CHECK(zip != nullptr);
	if (!zip) return;
	std::vector<uint8_t> data;
	CHECK(zip->read("africa/af_palmtree_b/meshes/af_palmtree_b01_lod.staticmesh", data));
	auto mesh = geom::parseMesh(data, geom::MeshKind::Static);
	CHECK(mesh.has_value());
	if (!mesh) return;
	int fronds = 0, trunks = 0;
	for (const auto& material : mesh->geometries[0].lods[0].materials) {
		CHECK(material.alphaMode == 0);
		const auto kind = render::overgrowthMaterial(material.maps[0]);
		fronds += kind == TreeMaterial::Leaf;
		trunks += kind == TreeMaterial::TrunkBase;
		// The static-mesh rule alone would have made the fronds a trunk.
		CHECK(render::treeMaterial(true, material.alphaMode, false) == TreeMaterial::TrunkBase);
	}
	CHECK(fronds == 1);
	CHECK(trunks == 1);
	// The cut-out texture the tree loader swaps in (0x10151b64).
	std::vector<uint8_t> cutout;
	CHECK(zip->read("africa/textures/leaf_af_palmtree_b_de_lod_alphatest.dds", cutout));
}

// A soldier's distance fade: its skinned mesh had no radius, so the cull
// fell back to the 80 m minimum and enemies past ~88 m showed only when
// zoomed. The mesh's box radius (0x101a7b10) x the template's
// cullRadiusScale 2.5 keeps him much further.
TEST(RenderScene_SoldierDistanceFadeUsesTheSkinnedMeshRadius)
{
	const auto cutoff = [](float radius, float zoom) {
		const render::CullRecord record = render::cullRecord(render::objectCullScales(3), zoom, 760, true, 0);
		float d = 1;
		while (d < 2000 && render::stepDistanceFade(1, radius, d * d, record) > 0) d += 1;
		return d;
	};
	// Without a radius: the minimum's 80 m, x sqrt(1.2) for the fade band.
	CHECK(cutoff(0, 1) < 90);
	CHECK(cutoff(0, 4) > 170); // zoomed in, the same soldier comes back
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	auto zip = io::ZipArchive::open(std::string(gameDir) + "/mods/bf2142/Objects/Soldiers_client.zip");
	CHECK(zip != nullptr);
	if (!zip) return;
	std::vector<uint8_t> data;
	CHECK(zip->read("us/meshes/us_light_soldier.skinnedmesh", data));
	auto mesh = geom::parseMesh(data, geom::MeshKind::Skinned);
	CHECK(mesh.has_value() && mesh->geometries.size() == 2);
	if (!mesh || mesh->geometries.size() < 2) return;
	const auto& box = mesh->geometries[1].lods[0].bounds; // the third-person body
	const float radius = render::boxHalfDiagonal(box.min, box.max);
	CHECK(radius > 1.0f && radius < 1.3f);
	const float far = cutoff(radius * 2.5f, 1); // us_light_soldier's cullRadiusScale
	CHECK(far > 150 && far < 200);
}

TEST(RenderScene_TreeMaterialDispatch)
{
	using render::TreeMaterial;
	CHECK(render::treeMaterial(true, 0, false) == TreeMaterial::TrunkBase);
	CHECK(render::treeMaterial(true, 0, true) == TreeMaterial::TrunkDetail);
	CHECK(render::treeMaterial(true, 2, true) == TreeMaterial::Leaf);
	CHECK(render::treeMaterial(false, 0, true) == TreeMaterial::Other);
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	auto zip = io::ZipArchive::open(std::string(gameDir) + "/mods/bf2142/Objects/Vegitation_client.zip");
	CHECK(zip != nullptr);
	if (!zip) return;
	std::vector<uint8_t> data;
	CHECK(zip->read("europe/eu_elmtree/meshes/eu_elmtree01.staticmesh", data));
	auto mesh = geom::parseMesh(data, geom::MeshKind::Static);
	CHECK(mesh.has_value());
	if (!mesh) return;
	bool trunk = false, leaf = false;
	for (const auto& material : mesh->geometries[0].lods[0].materials) {
		const bool detail = material.technique == "BaseDetail";
		const auto kind = render::treeMaterial(true, material.alphaMode, detail);
		trunk |= kind == TreeMaterial::TrunkDetail && material.maps.size() >= 2;
		leaf |= kind == TreeMaterial::Leaf;
	}
	CHECK(trunk);
	CHECK(leaf);
}

TEST(RenderScene_ShippedFlagsLoadAndAnimate)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	Console console(&files);
	bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
		[&files](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
	std::string out;
	console.run("ServerArchives.con", {}, out);
	console.run("ClientArchives.con", {}, out);
	anim::AnimationSystems systems([&files](const std::string& path, std::vector<uint8_t>& data) {
		return files.readFile(path, data);
	});
	anim::registerAnimationConsoleMethods(console, systems);
	const std::string systemPath = "Objects/Common/Flags/AnimationSystem.inc";
	const anim::AnimationSystemTemplate* system = systems.get(systemPath, [&] { console.run(systemPath, {}, out); });
	CHECK(system->findClip("Objects/Common/Flags/Animation/flag_idle.baf") >= 0);
	CHECK(system->findBundle("flag_idle") >= 0);
	CHECK(system->findTrigger("root") >= 0);
	const int clipIndex = system->findClip("Objects/Common/Flags/Animation/flag_idle.baf");
	if (clipIndex < 0) return;
	const anim::Clip& clip = system->clips()[static_cast<size_t>(clipIndex)];
	CHECK(clip.looping);
	CHECK(near(clip.length, 18.0f)); // authored AnimationManager.length, seconds
	const int bundleIndex = system->findBundle("flag_idle");
	const int rootIndex = system->findTrigger("root");
	CHECK(bundleIndex >= 0 && system->bundles()[static_cast<size_t>(bundleIndex)].clips.size() == 1);
	CHECK(bundleIndex >= 0 && system->bundles()[static_cast<size_t>(bundleIndex)].clips[0].clip == static_cast<size_t>(clipIndex));
	CHECK(rootIndex >= 0 && bundleIndex >= 0 && std::find(system->triggers()[static_cast<size_t>(rootIndex)].bundles.begin(),
		system->triggers()[static_cast<size_t>(rootIndex)].bundles.end(), static_cast<size_t>(bundleIndex))
		!= system->triggers()[static_cast<size_t>(rootIndex)].bundles.end());
	const anim::Skeleton* skeleton = systems.skeleton("Objects/Common/Flags/flag_setup.ske");
	const anim::Animation* animation = systems.animation(clip.path);
	CHECK(skeleton && skeleton->nodes.size() > 0);
	CHECK(animation && animation->frameCount > 1);
	if (!skeleton || !animation) return;
	const auto positions = [&](float seconds) {
		const auto pose = anim::evaluatePose(*skeleton, animation,
			seconds * static_cast<float>(animation->frameCount) / clip.length);
		std::vector<std::array<float, 3>> result;
		for (const char* meshPath : {"Objects/Common/Flags/flag_EU/meshes/flag_eu.skinnedmesh",
				"Objects/Common/Flags/flag_Pac/meshes/flag_Pac.skinnedmesh",
				"Objects/Common/Flags/flag_neutral/meshes/flag_neutral.skinnedmesh"}) {
			std::vector<uint8_t> bytes;
			CHECK(files.readFile(meshPath, bytes));
			auto mesh = geom::parseMesh(bytes, geom::MeshKind::Skinned);
			CHECK(mesh && mesh->vertexCount > 0 && !mesh->geometries[0].lods[0].rigs.empty());
			if (!mesh) continue;
			const auto* pos = mesh->element(geom::VertexUsage::Position);
			const auto* indices = mesh->element(geom::VertexUsage::BlendIndices);
			const auto* weightElement = mesh->element(geom::VertexUsage::BlendWeight);
			CHECK(pos && indices && weightElement);
			if (!pos || !indices || !weightElement) continue;
			const auto& rig = mesh->geometries[0].lods[0].rigs[0];
			bool sampled = false;
			for (uint32_t vertex = 0; vertex < mesh->vertexCount && !sampled; ++vertex) {
				const size_t base = static_cast<size_t>(vertex) * mesh->vertexStride;
				const uint8_t boneA = mesh->vertices[base + indices->offset];
				const uint8_t boneB = mesh->vertices[base + indices->offset + 1];
				if (boneA >= rig.size() || boneB >= rig.size()) continue;
				float p[3] {}, weight = 1;
				mesh->readFloats(vertex, *pos, p, 3);
				mesh->readFloats(vertex, *weightElement, &weight, 1);
				const auto transform = [&](uint8_t bone) {
					const int node = rig[bone].id;
					if (node < 0 || static_cast<size_t>(node) >= pose.size()) return std::array<float, 3> {p[0], p[1], p[2]};
					const anim::Matrix skin = anim::multiply(rig[bone].matrix, pose[static_cast<size_t>(node)]);
					return std::array<float, 3> {p[0] * skin[0] + p[1] * skin[4] + p[2] * skin[8] + skin[12],
						p[0] * skin[1] + p[1] * skin[5] + p[2] * skin[9] + skin[13],
						p[0] * skin[2] + p[1] * skin[6] + p[2] * skin[10] + skin[14]};
				};
				const auto a = transform(boneA), b = transform(boneB);
				result.push_back({a[0] * weight + b[0] * (1 - weight), a[1] * weight + b[1] * (1 - weight),
					a[2] * weight + b[2] * (1 - weight)});
				sampled = true;
			}
			CHECK(sampled);
		}
		return result;
	};
	const auto start = positions(0.0f);
	const auto phase = positions(4.5f);
	const auto loop = positions(18.0f);
	CHECK(start.size() == 3 && phase.size() == start.size() && loop.size() == start.size());
	bool changed = false, wrapped = true;
	for (size_t i = 0; i < start.size(); ++i) {
		for (int axis = 0; axis < 3; ++axis) {
			changed |= std::fabs(start[i][axis] - phase[i][axis]) > 1e-4f;
			wrapped &= near(start[i][axis], loop[i][axis], 1e-4f);
		}
	}
	CHECK(changed);
	CHECK(wrapped);
	// Optional retained matched frames. This software SVG uses the same parsed
	// retail mesh, inverse-bind matrices and evaluated bone poses as the GPU
	// palette path, at four timestamps (rather than a single still).
	if (const char* evidenceDir = std::getenv("BF2142_FLAG_ANIMATION_EVIDENCE_DIR")) {
		std::filesystem::create_directories(evidenceDir);
		std::ofstream svg(std::filesystem::path(evidenceDir) / "shipped-flag-phases.svg");
		const std::array<const char*, 3> paths = {"Objects/Common/Flags/flag_neutral/meshes/flag_neutral.skinnedmesh",
			"Objects/Common/Flags/flag_Pac/meshes/flag_Pac.skinnedmesh",
			"Objects/Common/Flags/flag_EU/meshes/flag_eu.skinnedmesh"};
		const std::array<const char*, 3> colors = {"#8b929d", "#c83f43", "#366db0"};
		const std::array<float, 4> times = {0, 4.5f, 9, 13.5f};
		svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"960\" height=\"520\" viewBox=\"0 0 960 520\">"
			<< "<rect width=\"100%\" height=\"100%\" fill=\"#171c25\"/>";
		for (size_t column = 0; column < paths.size(); ++column) {
			std::vector<uint8_t> bytes;
			if (!files.readFile(paths[column], bytes)) continue;
			auto mesh = geom::parseMesh(bytes, geom::MeshKind::Skinned);
			if (!mesh || mesh->geometries.empty() || mesh->geometries[0].lods.empty()
				|| mesh->geometries[0].lods[0].rigs.empty()) continue;
			const auto* position = mesh->element(geom::VertexUsage::Position);
			const auto* boneIndices = mesh->element(geom::VertexUsage::BlendIndices);
			const auto* boneWeight = mesh->element(geom::VertexUsage::BlendWeight);
			if (!position || !boneIndices || !boneWeight) continue;
			const auto& rig = mesh->geometries[0].lods[0].rigs[0];
			for (size_t row = 0; row < times.size(); ++row) {
				const auto pose = anim::evaluatePose(*skeleton, animation,
					times[row] * static_cast<float>(animation->frameCount) / clip.length);
				std::vector<std::array<float, 3>> vertices(mesh->vertexCount);
				for (uint32_t v = 0; v < mesh->vertexCount; ++v) {
					float p[3] {}, weight = 1;
					mesh->readFloats(v, *position, p, 3);
					mesh->readFloats(v, *boneWeight, &weight, 1);
					const size_t offset = static_cast<size_t>(v) * mesh->vertexStride + boneIndices->offset;
					const uint8_t pair[2] = {mesh->vertices[offset], mesh->vertices[offset + 1]};
					for (int influence = 0; influence < 2; ++influence) {
						const uint8_t index = pair[influence];
						const float amount = influence == 0 ? weight : 1 - weight;
						if (amount <= 0 || index >= rig.size() || rig[index].id < 0
							|| static_cast<size_t>(rig[index].id) >= pose.size()) continue;
						const anim::Matrix skin = anim::multiply(rig[index].matrix, pose[static_cast<size_t>(rig[index].id)]);
						for (int axis = 0; axis < 3; ++axis) {
							const int m = axis;
							vertices[v][axis] += amount * (p[0] * skin[m] + p[1] * skin[4 + m]
								+ p[2] * skin[8 + m] + skin[12 + m]);
						}
					}
				}
				float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
				for (const auto& v : vertices) { minX = std::min(minX, v[0]); maxX = std::max(maxX, v[0]); minY = std::min(minY, v[1]); maxY = std::max(maxY, v[1]); }
				const float scale = std::min(210.0f / std::max(maxX - minX, 1e-4f), 76.0f / std::max(maxY - minY, 1e-4f));
				const float originX = 160 + static_cast<float>(column) * 320;
				const float originY = 35 + static_cast<float>(row) * 120;
				svg << "<text x=\"" << originX << "\" y=\"" << originY + 12 << "\" fill=\"#e5e9f0\" font-size=\"12\">"
					<< (column == 0 ? "neutral" : column == 1 ? "PAC" : "EU") << " · " << times[row] << " s</text>";
				for (const geom::MeshMaterial& material : mesh->geometries[0].lods[0].materials) {
					const uint32_t end = std::min<uint32_t>(material.indexStart + material.indexCount, mesh->indices.size());
					for (uint32_t at = material.indexStart; at + 2 < end; at += 3) {
						const uint16_t a = mesh->indices[at], b = mesh->indices[at + 1], c = mesh->indices[at + 2];
						if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) continue;
						svg << "<polygon fill=\"" << colors[column] << "\" fill-opacity=\"0.75\" stroke=\"#eef2f7\" stroke-opacity=\"0.16\" stroke-width=\"0.3\" points=\"";
						for (uint16_t index : {a, b, c}) svg << originX + 10 + (vertices[index][0] - minX) * scale << ','
							<< originY + 28 + (maxY - vertices[index][1]) * scale << ' ';
						svg << "\"/>";
					}
				}
			}
		}
		svg << "</svg>";
	}
}

TEST(RenderScene_TitanPartsCulledAsRendDX9)
{
	using render::TitanDraw;
	const render::TitanView outside {false, false}, inside {true, false}, all {true, true};
	// Static meshes (0x100bdb00): the rooms only from inside, the rest only from outside.
	CHECK(render::titanStaticDraw(outside, true) == TitanDraw::Skip);
	CHECK(render::titanStaticDraw(outside, false) == TitanDraw::Normal);
	CHECK(render::titanStaticDraw(inside, true) == TitanDraw::Interior);
	CHECK(render::titanStaticDraw(inside, false) == TitanDraw::Skip);
	CHECK(render::titanStaticDraw(all, false) == TitanDraw::Normal); // showAllTitanParts
	// Bundled (0x100c5030): from inside no exterior parts and nothing past 53 m.
	CHECK(render::titanBundledDraw(outside, true, 500, 53) == TitanDraw::Normal);
	CHECK(render::titanBundledDraw(inside, true, 5, 53) == TitanDraw::Skip);
	CHECK(render::titanBundledDraw(inside, false, 60, 53) == TitanDraw::Skip);
	CHECK(render::titanBundledDraw(inside, false, 50, 53) == TitanDraw::Interior);
	// Skinned (0x10162070): nothing past indoorSkinnedMeshCullDistance.
	CHECK(render::titanSkinnedDraw(inside, 54, 53) == TitanDraw::Skip);
	CHECK(render::titanSkinnedDraw(inside, 53, 53) == TitanDraw::Interior);
	CHECK(render::titanSkinnedDraw(outside, 1000, 53) == TitanDraw::Normal);

	// The hull parts' LOD pairs (0x100baf40 / 0x100baf90 / 0x100bb470).
	render::TitanExteriorLods lods;
	CHECK(lods.resolve(1, 2, 3) == 0); // the pairs start at 0
	CHECK(lods.resolve(2, 1, 3) == 0);
	CHECK(lods.resolve(3, 3, 3) == 0);
	CHECK(lods.resolve(4, 2, 3) == 0);
	lods.endFrame();
	CHECK(lods.resolve(1, 2, 3) == 1); // min(2, 1)
	CHECK(lods.resolve(4, 2, 3) == 2); // min(3, 2)
	CHECK(lods.resolve(0, 0, 3) == 1); // part 0 reads the first pair, records nothing
	CHECK(lods.resolve(3, 3, 1) == 1); // capped at the count
}

TEST(RenderScene_GeometryTemplateTitanAndMaterialSettings)
{
	Console console;
	geom::GeometryTemplateManager geometries;
	geom::registerGeometryConsoleMethods(console, geometries);
	std::string out;
	CHECK(console.execute("GeometryTemplate.create StaticMesh eu_titan_exterior_main_front", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.isTitanInterior 1", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.isTitanExterior 1", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.titanExteriorPartIndex 1", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.setMaterialParallaxHeightScale 0 0 3 25", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.setSpecularStaticGloss 0 0 0 0.5", out) == ConsoleResult::Ok);
	const geom::GeometryTemplate* hull = geometries.find("eu_titan_exterior_main_front");
	// One excludes the other (0x10148670 / 0x10148690).
	CHECK(hull && hull->titanExterior && !hull->titanInterior && hull->titanExteriorPart == 1);
	CHECK(hull && hull->parallaxHeightScale.at(geom::GeometryTemplate::materialKey(0, 0, 3)) == 25);
	CHECK(hull && hull->staticGloss.empty()); // bundled templates only (0x10073eb9)
	CHECK(console.execute("GeometryTemplate.create BundledMesh unl_shotgun_02", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.setSpecularStaticGloss 1 0 0 0", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.setMaterialReflectionScale 1 0 2 0.25", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.nrOfAnimatedUVMatrix 6", out) == ConsoleResult::Ok);
	const geom::GeometryTemplate* gun = geometries.find("unl_shotgun_02");
	CHECK(gun && gun->staticGloss.at(geom::GeometryTemplate::materialKey(1, 0, 0)) == 0);
	CHECK(gun && gun->reflectionScale.at(geom::GeometryTemplate::materialKey(1, 0, 2)) == 0.25f);
	CHECK(gun && gun->animatedUVMatrices == 6);
	CHECK(geom::GeometryTemplate::materialKey(1, 2, 3) == 0x10203u);
}

TEST(RenderScene_ViewFogUnderTheSea)
{
	// Cerbere Landing's Sky.con, the sea at 34 m.
	const std::array<float, 3> fogColor {110, 123, 132}, waterColor {62, 77, 94};
	const std::array<float, 4> fog {0, 140, 1, 0.5f}, water {0, 10, 2, 0};
	const render::ViewFog above = render::viewFog(fogColor, fog, waterColor, water, true, 34, 40, 1);
	CHECK(!above.underwater && !above.underwaterSky && near(above.color[0], 110 / 255.0f));
	CHECK(above.startEndAndBase[1] == 140);
	// At the surface the water's fog, but the sky dome still the sky.
	const render::ViewFog surface = render::viewFog(fogColor, fog, waterColor, water, true, 34, 34, 1);
	CHECK(surface.underwater && !surface.underwaterSky);
	const render::ViewFog below = render::viewFog(fogColor, fog, waterColor, water, true, 34, 30, 0.25f);
	CHECK(below.underwater && below.underwaterSky && near(below.color[2], 94 / 255.0f));
	// The end and the fourth value by the view distance, at least 0.5 (0x10028020).
	CHECK(near(below.startEndAndBase[1], 5) && below.startEndAndBase[2] == 2);
	// Without a sea (-1000) never under water.
	CHECK(!render::viewFog(fogColor, fog, waterColor, water, false, 34, -10, 1).underwater);
	// RaCommon.fx calcFog: clear at the start, gone at the end.
	CHECK(near(render::fogVisibility(fog, 0), 1));
	CHECK(near(render::fogVisibility(fog, 70), 0.375f)); // max(0.5, 0.5) - 0.125
	CHECK(near(render::fogVisibility(fog, 140), 0));
	// FogRange as 0x100355c0 makes it, in metres (Suez Canal: -10 / 355 / 0.2 / 0).
	const auto range = render::fogRange({-10, 355, 0.2f, 0});
	CHECK(near(range[0], 1 / 365.0f) && near(range[1], -0.2f / 365) && near(range[2], 10 / 365.0f) &&
		near(range[3], 355 / 365.0f));

	// The console names (0x1004f4a0 / 0x1004f740, terrain.water*).
	Console console;
	scene::WaterSettings settings;
	scene::registerWaterConsoleMethods(console, settings);
	std::string out;
	CHECK(console.execute("renderer.waterFogColor 62.00/77.00/94.00", out) == ConsoleResult::Ok);
	CHECK(console.execute("renderer.waterFogStartEndAndBase 0.00/10.00/2.00/0.00", out) == ConsoleResult::Ok);
	CHECK(console.execute("terrain.waterSunIntensity 0.8", out) == ConsoleResult::Ok);
	CHECK(console.execute("terrain.waterSpecularPower 12", out) == ConsoleResult::Ok);
	CHECK(settings.fogColor[1] == 77 && settings.fogStartEndAndBase[1] == 10 && near(settings.sunIntensity, 0.8f));
	CHECK(settings.specularPower == 12);
}

TEST(RenderScene_ShadowRegionAndHemiMaps)
{
	CHECK(render::shadowRegionRadius(3, 40) == 80 && render::shadowRegionRadius(2, 40) == 60);
	CHECK(render::shadowRegionRadius(1, 4) == 10);
	CHECK(render::shadowMapTexels(3, 1024) == 2048 && render::shadowMapTexels(1, 100) == 256);
	CHECK(render::shadowMapTexels(3, 4096) == 4096);
	// Half the region ahead, snapped to half the radius: still while the
	// camera moves within a step.
	const render::Vec3f a = render::shadowRegionCenter({1, 3, 2}, {0, -0.5f, 1}, 40);
	const render::Vec3f b = render::shadowRegionCenter({4, 3, 5}, {0, 0, 1}, 40);
	CHECK(a.x == 0 && a.y == 0 && a.z == 20);
	CHECK(b.x == 0 && b.z == 20);

	// A BC3 alpha block: 8 levels between the ends, then 6 with 0 and 255.
	const uint8_t eight[8] = {255, 0, 0b10001000, 0, 0, 0, 0, 0}; // texels' indices 0, 1, 2, 0
	uint8_t alpha[16];
	render::decodeBc3Alpha(eight, alpha);
	CHECK(alpha[0] == 255 && alpha[1] == 0 && alpha[2] == 218 && alpha[3] == 255); // 2: (6 x 255 + 0) / 7
	const uint8_t six[8] = {0, 100, 0b00111110, 0, 0, 0, 0, 0}; // texel 0: 6 -> 0, texel 1: 7 -> 255
	render::decodeBc3Alpha(six, alpha);
	CHECK(alpha[0] == 0 && alpha[1] == 255 && alpha[2] == 0);

	// The base map's texel (vsParticle's HemiMapInfo, v flipped).
	int px = 0, py = 0;
	render::hemiMapTexel(0, 0, 0, 0, 2048, 512, px, py);
	CHECK(px == 256 && py == 256);
	render::hemiMapTexel(-1024, -1024, 0, 0, 2048, 512, px, py);
	CHECK(px == 0 && py == 511);
	render::hemiMapTexel(5000, 5000, 0, 0, 2048, 512, px, py);
	CHECK(px == 511 && py == 0);

	// A Titan map: over its square and above its floor, bilinear.
	const render::Vec3f axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	const float offset[3] = {2, -10, -33};
	const render::Vec3f local = render::titanMapLocal({100, 150, 0}, axes, offset, {102, 145, -33});
	CHECK(local.x == 0 && local.y == 5 && local.z == 0);
	CHECK(render::overTitanMap(local, 128) && !render::overTitanMap({0, -1, 0}, 128) && !render::overTitanMap({70, 1, 0}, 128));
	std::vector<uint8_t> rgba(4 * 4 * 4, 0);
	rgba[(1 * 4 + 1) * 4] = 255; // texel (1, 1) red
	float colour[3];
	render::titanHemiSample(rgba.data(), 4, 4, 4, {1, 1, -1}, colour); // u = 1, v = 1
	CHECK(near(colour[0], 1) && near(colour[1], 0));
	render::titanHemiSample(rgba.data(), 4, 4, 4, {0.5f, 1, -1}, colour); // u = 1.5
	CHECK(near(colour[0], 0.5f));

	// HeightOverTerrain: in radii over the ground less one, at least 2 m radii.
	CHECK(near(render::heightOverTerrain(3, 1), 1));     // (3 - 1) / 2
	CHECK(near(render::heightOverTerrain(6, 1), 1.5f));  // past 4 radii: (4 - 1) / 2
	CHECK(near(render::heightOverTerrain(-1, 3), 3));    // nothing hit: 4 radii
	CHECK(near(render::heightOverTerrain(1, 3), 0));
	// The clouds wrap within a tile.
	CHECK(near(render::cloudOffset(0.9f, 0.5f, 0.4f), 0.1f));
}

TEST(RenderScene_NormalMapsAndMipSkips)
{
	CHECK(render::normalMapKind(true, false, "vehicles/a/textures/hull_b.dds", "base") == 1);
	CHECK(render::normalMapKind(true, false, "vehicles/a/textures/hull_c.dds", "base") == 0);
	CHECK(render::normalMapKind(false, true, "soldiers/a_b_os.dds", "humanskin") == 2);
	CHECK(render::normalMapKind(false, true, "soldiers/a_b.dds", "humanskintangent") == 1);
	CHECK(render::normalMapKind(false, true, "shaders/specularlut_pow36.dds", "") == 0);
	CHECK(render::normalMapKind(false, false, "x_b.dds", "") == 0);

	// GSSkipMipLevels by the texture quality, and its classes (0x10010040).
	scene::QualitySettings q;
	q.texture = 1;
	CHECK(q.skipMipLevels(scene::TextureClass::StaticMesh) == 2);
	CHECK(q.skipMipLevels(scene::TextureClass::Undergrowth) == 1);
	CHECK(q.skipMipLevels(scene::TextureClass::Interface) == 0);
	q.texture = 3;
	CHECK(q.skipMipLevels(scene::TextureClass::Overgrowth) == 0);
	// The lighting level's normal maps (renderer +0x6b4).
	q.lighting = 2;
	CHECK(!q.meshNormalMaps() && q.meshHemiAndShadows());

	// undergrowth.simpleShaderEnable holds until the settings are applied again.
	Console console;
	scene::QualitySettings video;
	scene::registerQualityConsoleMethods(console, video);
	std::string out;
	CHECK(video.undergrowthSimpleShader() == false); // the high terrain quality
	CHECK(console.execute("undergrowth.simpleShaderEnable 1", out) == ConsoleResult::Ok);
	CHECK(video.undergrowthSimpleShader());
	CHECK(console.execute("VideoSettings.setTerrainQuality 3", out) == ConsoleResult::Ok);
	CHECK(!video.undergrowthSimpleShader());
	CHECK(console.execute("VideoSettings.setTerrainQuality 1", out) == ConsoleResult::Ok);
	CHECK(video.undergrowthSimpleShader());
}

TEST(RenderScene_OverheadCameraPlace)
{
	render::OverheadView view;
	view.x = 10;
	view.z = 2000;
	view.height = 260;
	const float origin[2] = {-1024, -1024};
	float xs[5], zs[5];
	render::overheadGroundPoints(view, origin, 2048, xs, zs);
	CHECK(xs[0] == 10 && zs[0] == 944 && zs[1] == 946 && xs[3] == 12); // 80 m inside the edge
	const float heights[5] = {10, 20, 30, 40, 50};
	const render::Vec3f at = render::overheadCameraPosition(view, origin, 2048, heights, -1e9f);
	CHECK(at.x == 10 && at.z == 944 && near(at.y, 290));
	const render::Vec3f sea = render::overheadCameraPosition(view, origin, 2048, heights, 100);
	CHECK(near(sea.y, 360)); // the sea's height when higher
	const render::Vec3f small = render::overheadCameraPosition(view, origin, 100, heights, -1e9f);
	CHECK(small.z == 2000); // a small terrain: no clamp
}

namespace {

void addSprite(world::ObjectTemplateManager& templates, const char* name)
{
	world::ObjectTemplate* sprite = templates.create("SpriteParticleSystem", name);
	sprite->settings()["emitFrequency"] = "10";
	sprite->settings()["emitTime"] = "0.2";
	sprite->settings()["timeToLive"] = "2";
	sprite->settings()["gravity"] = "0";
	sprite->settings()["textureName"] = "flipbook";
}

} // namespace

TEST(RenderScene_SpriteFlipbooks)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "SpriteParticleSystem");
	addSprite(templates, "anim");
	world::ObjectTemplate* anim = templates.find("anim");
	anim->settings()["animationEnable"] = "1";
	anim->settings()["animationFrameCount"] = "4";
	anim->settings()["animationFrameCountX"] = "2";
	anim->settings()["setAnimationFrameWidthRelative"] = "0.5";
	anim->settings()["setAnimationFrameHeightRelative"] = "0.5";
	anim->settings()["animationSpeed"] = "1.73913";
	anim->settings()["animationPlayOnce"] = "1";
	anim->settings()["showInZoomedMode"] = "0";
	const effects::ParticleTemplate t = effects::ParticleTemplate::fromTemplate(*anim);
	// animationPlayOnce: frames / timeToLive (0x10138790).
	CHECK(t.animated && near(t.animationSpeed, 2) && !t.showZoomed && t.selfShadowIntensity == 1);
	// The frame: the clock's whole part modulo the frames, 2 across (0x10131f80).
	auto frame = effects::spriteFrame(t, 0.5f);
	CHECK(frame[0] == 0 && frame[1] == 0 && frame[2] == 0.5f && frame[3] == 0.5f);
	frame = effects::spriteFrame(t, 3.2f);
	CHECK(frame[0] == 0.5f && frame[1] == 0.5f);
	frame = effects::spriteFrame(t, 5.0f); // wraps
	CHECK(frame[0] == 0.5f && frame[1] == 0);

	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_anim");
	bundle->children().push_back({"anim", {}, {}});
	effects::EffectManager manager(templates);
	CHECK(manager.spawn("e_anim", {}));
	for (int i = 0; i < 30; ++i) { // 1 s: the first particles two frames on
		manager.update(1 / 30.0f);
	}
	bool advanced = false;
	manager.forEachGroup([&](const std::string&, bool, const std::vector<effects::Sprite>& sprites) {
		for (const effects::Sprite& s : sprites) {
			advanced = advanced || s.frame[0] > 0;
		}
	});
	CHECK(advanced);
	// A zoomed first person keeps it (showInZoomedMode 0, showInFirstPerson
	// 1: 0x0061a1c0's flags & 5 still hold; zoomed only picks the view).
	effects::ParticleView zoomed;
	zoomed.zoomed = true;
	size_t shown = 0;
	manager.forEachGroup(zoomed, [&](const std::string&, bool, const std::vector<effects::Sprite>& s) { shown += s.size(); });
	CHECK(shown > 0);
	// In first person a system shows with either flag; neither: not started.
	world::ObjectTemplate* neither = templates.find("anim");
	neither->settings()["showInFirstPerson"] = "0";
	effects::EffectManager firstPerson(templates);
	CHECK(firstPerson.spawn("e_anim", {}, {}, effects::EffectManager::View::FirstPerson));
	CHECK(firstPerson.emitters() == 0);
	neither->settings()["showInZoomedMode"] = "1"; // showInZoomedMode alone: started
	effects::EffectManager zoomedOnly(templates);
	CHECK(zoomedOnly.spawn("e_anim", {}, {}, effects::EffectManager::View::FirstPerson));
	CHECK(zoomedOnly.emitters() == 1);
}

TEST(RenderScene_BundleAgeSpeedAndRotation)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "SpriteParticleSystem");
	addSprite(templates, "plain");
	addSprite(templates, "upright");
	templates.find("upright")->settings()["noRotationFromParent"] = "1";
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_bundle");
	bundle->settings()["randomAgeFactor"] = "CRD_NONE/0.5/0/0";
	bundle->settings()["speedFromPhysics"] = "1";
	bundle->children().push_back({"plain", {0, 0, 2}, {}});
	bundle->children().push_back({"upright", {0, 0, 2}, {}});
	effects::EffectManager manager(templates);
	// Turned 90 degrees about y: the plain system's offset turns with it.
	const effects::Mat3 turn = effects::Mat3::fromYawPitchRoll({90, 0, 0});
	CHECK(manager.spawn("e_bundle", {10, 0, 0}, turn));
	const auto& emitters = manager.emitterList();
	CHECK(emitters.size() == 2);
	if (emitters.size() == 2) {
		const effects::Emitter& plain = *emitters[0];
		const effects::Emitter& upright = *emitters[1];
		// randomAgeFactor scales both systems' age (0x00619f90).
		CHECK(plain.timeScale == 0.5f && upright.timeScale == 0.5f);
		CHECK(plain.physicsVelocity && upright.physicsVelocity);
		// noRotationFromParent: the offset unturned, its own rotation.
		CHECK(near(upright.position().x, 10) && near(upright.position().z, 2));
		CHECK(!(near(plain.position().x, 10) && near(plain.position().z, 2)));
	}
	// speedFromPhysics: the direction times the square root of the speed (0x00618ae0).
	CHECK(manager.spawnAttached("e_bundle", 7, {}, {}, effects::EffectManager::View::Any, 3));
	manager.setPhysicsVelocity(7, 3, {0, 0, 16});
	bool set = false;
	for (const auto& e : manager.emitterList()) {
		if (e->anchor == 7) {
			set = near(e->inheritedVelocity.z, 4) && near(e->inheritedVelocity.x, 0);
		}
	}
	CHECK(set);
}

TEST(RenderScene_EnvMapsAndNearFade)
{
	// Belgrade's EnvironmentMapInfo.emi (0x10064770) and the nearest map (0x10064f30).
	const auto maps = render::parseEnvMapInfo(
		"# EnvironmentMapInformation file\n# Version 0\n#\nEnvMap0 -88,54,66,1 \r\nEnvMap1 100,20,-5,0\n");
	CHECK(maps.size() == 2 && maps[0].name == "EnvMap0" && maps[0].flag && near(maps[0].position[2], 66));
	CHECK(render::nearestEnvMap(maps, {90, 0, 0}) == 1 && render::nearestEnvMap(maps, {-80, 50, 60}) == 0);
	CHECK(render::nearestEnvMap({}, {0, 0, 0}) == -1);
	// nearCullingEnable (0x101318e0): beyond 6 m past the near plane or
	// 0.15 m and smaller, none; a big particle at the near plane, gone.
	CHECK(effects::ParticleView::nearFade(10, 0.1f, 1) < 0);
	CHECK(effects::ParticleView::nearFade(1, 0.1f, 0.1f) < 0);
	CHECK(near(effects::ParticleView::nearFade(0.1f, 0.1f, 1), 0));
	// 5 m past it: t = 0.5, f = 1 - 0.05 x 4 = 0.8: 0.5 + 0.5 x 0.8.
	CHECK(near(effects::ParticleView::nearFade(5.1f, 0.1f, 0.2f), 0.9f));
}

TEST(RenderScene_AnimatedUVFollowsThePartsTravel)
{
	// EU_TANK_tracks_left: v scrolls by the distance / 8, wrapped into [0, 0.285].
	render::AnimatedUVSettings tracks;
	tracks.translation = true;
	tracks.max[0] = 0;
	tracks.max[1] = 0.285f;
	tracks.size[0] = 0;
	tracks.size[1] = 8;
	tracks.translationIndex = 5;
	std::array<render::UVMatrix, render::kAnimatedUVMatrices> matrices {};
	render::AnimatedUVState state;
	const float z[3] = {0, 0, 1};
	float at[3] = {10, 0, 20};
	render::updateAnimatedUV(tracks, state, at, z, matrices); // the first update moves nothing
	CHECK(state.uv[1] == 0 && matrices[5].r2[1] == 0);
	at[2] = 19; // a metre backwards along z: +1 / 8
	render::updateAnimatedUV(tracks, state, at, z, matrices);
	CHECK(near(state.uv[1], 0.125f) && near(matrices[5].r2[1], 0.125f) && matrices[5].r2[0] == 0);
	at[2] = 17; // two more: 0.375 wraps to 0.09
	render::updateAnimatedUV(tracks, state, at, z, matrices);
	CHECK(near(state.uv[1], 0.375f - 0.285f));
	at[2] = 25; // forwards: -1, back into [0, 0.285]
	render::updateAnimatedUV(tracks, state, at, z, matrices);
	CHECK(state.uv[1] >= 0 && state.uv[1] <= 0.285f && near(state.uv[1], std::fmod(0.09f - 1.0f + 4 * 0.285f, 0.285f), 1e-3f));
	// Reverse subtracts; sideways travel does nothing.
	render::AnimatedUVSettings reverse = tracks;
	reverse.translationReverse = true;
	render::AnimatedUVState r;
	float p[3] = {0, 0, 0};
	render::updateAnimatedUV(reverse, r, p, z, matrices);
	p[2] = -0.8f;
	render::updateAnimatedUV(reverse, r, p, z, matrices);
	CHECK(near(r.uv[1], 0.285f - 0.1f));
	p[0] = 5;
	render::updateAnimatedUV(reverse, r, p, z, matrices);
	CHECK(near(r.uv[1], 0.185f));

	// EU_TANK_Rwheel_06's turn: angle = distance / radius 0.3, the matrix
	// rows (cos sx, -sin sy), (sin sx, cos sy), row 2 zero.
	render::AnimatedUVSettings wheel;
	wheel.rotation = true;
	wheel.radius = 0.3f;
	wheel.scale[0] = 0.5f;
	wheel.scale[1] = 1;
	wheel.rotationIndex = 3;
	render::AnimatedUVState w;
	float c[3] = {0, 0, 0};
	matrices[3].r2[0] = 7;
	render::updateAnimatedUV(wheel, w, c, z, matrices);
	c[2] = -0.3f; // one radian
	render::updateAnimatedUV(wheel, w, c, z, matrices);
	CHECK(near(w.angle, 1.0f, 1e-4f));
	CHECK(near(matrices[3].r0[0], std::cos(1.0f) * 0.5f) && near(matrices[3].r0[1], -std::sin(1.0f)));
	CHECK(near(matrices[3].r1[0], std::sin(1.0f) * 0.5f) && near(matrices[3].r1[1], std::cos(1.0f)));
	CHECK(matrices[3].r2[0] == 0 && matrices[3].r2[1] == 0);
	c[2] = 0.3f; // two radians back: wrapped into [0, 2 pi]
	render::updateAnimatedUV(wheel, w, c, z, matrices);
	CHECK(near(w.angle, 6.2831855f - 1.0f, 1e-4f));

	float v[2] = {0, 0};
	render::parseAnimatedUVVec2("0.2/0.285", v);
	CHECK(near(v[0], 0.2f) && near(v[1], 0.285f));
}

// RendDX9's decal clipping (0x10165f60 / 0x101655c0 / 0x10165020): a decal
// over a fold of the ground is cut to the faces it lies on and laid onto
// each; faces turned too far from the decal are left out by the clip
// angle; a decal off every face leaves nothing.
TEST(RenderScene_DecalClippedToTheSurface)
{
	using effects::Vec3;
	// Two faces meeting along x = 0: flat for x < 0, sloping up 45 degrees for x > 0.
	const std::vector<std::array<Vec3, 3>> fold = {
		{Vec3 {-4, 0, -4}, Vec3 {0, 0, -4}, Vec3 {-4, 0, 4}},
		{Vec3 {-4, 0, 4}, Vec3 {0, 0, -4}, Vec3 {0, 0, 4}},
		{Vec3 {0, 0, -4}, Vec3 {4, 4, -4}, Vec3 {0, 0, 4}},
		{Vec3 {0, 0, 4}, Vec3 {4, 4, -4}, Vec3 {4, 4, 4}},
	};
	const std::array<float, 4> frame {0, 0, 1, 1};
	const auto pieces = effects::clipDecal({0, 0, 0}, {0, 1, 0}, {1, 0, 0}, 1, frame, fold, 0, 0.01f);
	CHECK(!pieces.empty() && pieces.size() % 3 == 0 && pieces.size() <= 128);
	bool onSurface = true, inFrame = true, rises = false;
	for (const auto& p : pieces) {
		const float ground = p.position.x > 0 ? p.position.x : 0.0f;
		onSurface = onSurface && std::fabs(p.position.y - ground - 0.01f) < 1e-3f;
		inFrame = inFrame && p.u >= -1e-4f && p.u <= 1.0001f && p.v >= -1e-4f && p.v <= 1.0001f;
		rises = rises || p.position.y > 0.5f;
	}
	CHECK(onSurface && inFrame && rises);
	// decalGeomClipAngle 30: the 45-degree faces are left out.
	const auto flatOnly = effects::clipDecal({0, 0, 0}, {0, 1, 0}, {1, 0, 0}, 1, frame, fold, 30, 0.01f);
	bool flat = !flatOnly.empty();
	for (const auto& p : flatOnly) {
		flat = flat && p.position.x <= 1e-3f;
	}
	CHECK(flat);
	// Nowhere near the faces.
	CHECK(effects::clipDecal({50, 0, 50}, {0, 1, 0}, {1, 0, 0}, 1, frame, fold, 0, 0.01f).empty());
}

// The doubled ageing (0x10164f10): a decal set whose owner wasn't drawn the
// last frame ages twice as fast; world decals don't.
TEST(RenderScene_DecalSetsAgeTwiceUnseen)
{
	world::ObjectTemplateManager templates;
	templates.registerType(5, "Decal");
	world::ObjectTemplate* hole = templates.create("Decal", "hole");
	hole->settings()["decalTextureName"] = "hole";
	effects::EffectManager manager(templates);
	CHECK(manager.spawnDecal("hole", {0, 0, 0}, {0, 1, 0}));
	CHECK(manager.spawnDecal("hole", {11, 1, 0}, {1, 0, 0}, 42, {10, 0, 0}, {0, 0, 0}));
	CHECK(manager.spawnDecal("hole", {11, 1, 0}, {1, 0, 0}, 43, {10, 0, 0}, {0, 0, 0}));
	manager.decals()[0].ownerDrawn = false; // ignored for the world's set
	manager.decals()[2].ownerDrawn = false;
	manager.update(1);
	CHECK(std::fabs(manager.decals()[0].age - 1) < 1e-5f);
	CHECK(std::fabs(manager.decals()[1].age - 1) < 1e-5f);
	CHECK(std::fabs(manager.decals()[2].age - 2) < 1e-5f);
}

// rainManager.freeze (+0x65, 0x1011ab20 / 0x1011d250): the box stops
// following the camera.
TEST(RenderScene_RainFreezeHoldsTheBox)
{
	scene::RainSettings rain;
	rain.enabled = true;
	rain.particleCount = 200;
	rain.emitRadius = {10, 10, 10};
	rain.offset = {0, 0, 0};
	effects::Weather weather;
	weather.update(rain, {0, 0, 0}, 0);
	rain.freeze = true;
	weather.update(rain, {500, 0, 0}, 0.1f);
	bool held = !weather.drops().empty();
	for (const auto& drop : weather.drops()) {
		held = held && std::fabs(drop.position.x) <= 11.0f;
	}
	CHECK(held);
	rain.freeze = false;
	weather.update(rain, {500, 0, 0}, 0.1f);
	bool moved = !weather.drops().empty();
	for (const auto& drop : weather.drops()) {
		moved = moved && std::fabs(drop.position.x - 500) <= 11.0f;
	}
	CHECK(moved);
}

// The renderer's console switches (renderer.draw*, 0x1004c7a0..0x1004e0b0;
// drawFps 0x10053ae0; the road renderer's Renderer.road*; undergrowth.*).
TEST(RenderScene_RendererConsoleSwitches)
{
	Console console;
	scene::RendererSettings settings;
	scene::registerRendererConsoleMethods(console, settings);
	std::string out;
	CHECK(settings.drawTerrain && settings.drawSkyDome && settings.drawFps == 0);
	console.execute("renderer.drawTerrain 0", out);
	console.execute("renderer.drawFps 7", out);
	console.execute("Renderer.roadLodDistMod 2", out);
	console.execute("StaticMeshRenderer.enableRendering 0", out);
	console.execute("undergrowth.viewDistanceFade 3", out);
	console.execute("renderer.tweakNearPlane1p 0.1", out);
	CHECK(!settings.drawTerrain && settings.drawFps == 1 && settings.roadLodDistMod == 2);
	CHECK(!settings.drawStaticMeshes && settings.undergrowthViewDistanceFade == 1);
	CHECK(settings.tweakNearPlane1pSet && std::fabs(settings.tweakNearPlane1p - 0.1f) < 1e-6f);
	CHECK(!settings.tweakNearPlaneSet);
}

// The overgrowth's cells, cross-fade and lodTimer (OvergrowthLod.h,
// 0x1008da30 / 0x10120ef0 / 0x10121410) and its console rows.
TEST(RenderScene_OvergrowthCrossFadeAndCells)
{
	using render::OvergrowthBlend;
	using render::OvergrowthCells;
	// 500 / 4 = 125, fitted to a 1024 m terrain: 8 cells of 128.
	CHECK(near(OvergrowthCells::cellSize(500, 1024), 128));
	CHECK(near(OvergrowthCells::cellSize(140, 0), 35));
	// Kept up to 70% at 1, to 0 at the view distance, gone beyond.
	CHECK(near(OvergrowthCells::distanceFade(300, 500), 1));
	CHECK(near(OvergrowthCells::distanceFade(425, 500), 0.5f));
	CHECK(OvergrowthCells::distanceFade(500, 500) < 0);
	// The cell about a camera on the ground fades to 0.4 at its centre.
	CHECK(near(OvergrowthCells::nearFade(0, 0), 0.4f));
	CHECK(near(OvergrowthCells::nearFade(6, 0), 0.5f));
	CHECK(near(OvergrowthCells::nearFade(6, 10), 1));
	// The type factor: (radius / 5)^2 within 0.3..1.
	CHECK(near(OvergrowthBlend::typeFactor(1), 0.3f));
	CHECK(near(OvergrowthBlend::typeFactor(4), 0.64f));
	CHECK(near(OvergrowthBlend::typeFactor(10), 1));
	// w: 1 within 70% of R (factor 1, closeDistFade 0.3), 0 at R.
	CHECK(near(OvergrowthBlend::weight(50, 100, 1, 0.3f), 1));
	CHECK(near(OvergrowthBlend::weight(85, 100, 1, 0.3f), 0.5f));
	CHECK(near(OvergrowthBlend::weight(100, 100, 1, 0.3f), 0));
	// Both meshes at full alpha in the middle, one of them fading at the ends.
	CHECK(near(OvergrowthBlend::fullAlpha(0.5f), 1) && near(OvergrowthBlend::lodAlpha(0.5f), 1));
	CHECK(near(OvergrowthBlend::fullAlpha(0.25f), 0.5f) && near(OvergrowthBlend::lodAlpha(0.25f), 1));
	CHECK(near(OvergrowthBlend::lodAlpha(1), 0) && near(OvergrowthBlend::fullAlpha(0), 0));
	// lodTimer 0: w as it is; 2 s: eased at 0.5 a second.
	float t = 0;
	CHECK(near(OvergrowthBlend::eased(0.3f, t, 0.1f, 0), 0.3f));
	t = 0;
	CHECK(near(OvergrowthBlend::eased(0.4f, t, 0.1f, 2), 0.8f)); // at least 1 - 2w = 0.2
	CHECK(near(OvergrowthBlend::eased(0.4f, t, 0.2f, 2), 0.7f)); // 0.2 + 0.1
	CHECK(near(OvergrowthBlend::eased(0.9f, t, 0.2f, 2), 0.8f)); // at most 2 - 2w = 0.2
	CHECK(near(OvergrowthBlend::eased(1, t, 0.2f, 2), 1) && t == 0);

	Console console;
	std::string out;
	scene::OvergrowthSettings og;
	scene::registerOvergrowthConsoleMethods(console, og);
	console.execute("Overgrowth.viewDistance 350", out);
	console.execute("Overgrowth.closeDistPercentage 0.15", out);
	console.execute("Overgrowth.addMaterial wood 7", out);
	console.execute("Overgrowth.setActiveMaterial wood", out);
	console.execute("Overgrowth.addType spruce", out);
	console.execute("Overgrowth.setActiveType spruce", out);
	console.execute("OvergrowthType.geometry eu_spruce_b01", out);
	console.execute("OvergrowthType.density 50", out);
	console.execute("Overgrowth.preLoad", out);
	CHECK(og.configured && og.preLoaded && near(og.viewDistance, 350) && near(og.closeDistPercentage, 0.15f));
	CHECK(og.materials.size() == 1 && og.materials[0].index == 7 && og.activeMaterial == 0);
	CHECK(!og.addMaterial("other", 7)); // the index is taken (0x10092840)
	const scene::OvergrowthSettings::Type* spruce = og.typeForGeometry("EU_Spruce_b01");
	CHECK(spruce && near(spruce->density, 50) && near(spruce->randomScale[0], 1));
	console.execute("Overgrowth.getActiveMaterialName", out);
	CHECK(out == "wood");
}

// The meshes' point lights (MeshLights.h, 0x100a9f80 / 0x100a7f00) and
// the flicker (0x0064a920).
TEST(RenderScene_MeshLightsPickedPerMesh)
{
	using render::LightedMesh;
	render::LightLimits limits = render::LightLimits::forShadowQuality(2);
	CHECK(limits.staticMesh == 2 && near(limits.cullDistance, 100));
	CHECK(render::LightLimits::forShadowQuality(0).maxFor(LightedMesh::Bundled) == 0);
	render::ListedLight lights[4];
	const float at[4][3] = {{10, 0, 0}, {2, 0, 0}, {5, 0, 0}, {50, 0, 0}};
	for (int i = 0; i < 4; ++i) {
		std::copy(at[i], at[i] + 3, lights[i].position);
		lights[i].radius = 8;
	}
	lights[2].spot = true;
	lights[1].affectsLightmapped = false;
	const float origin[3] = {0, 0, 0};
	int picked[8];
	// Within light radius + mesh radius, nearest first, at most 2.
	int n = render::pickMeshLights(lights, 4, origin, 3, LightedMesh::Static, false, limits, picked, 8);
	CHECK(n == 2 && picked[0] == 1 && picked[1] == 2);
	// Bundled meshes take no spots; lightmapped statics only affectLightmappedObjects.
	n = render::pickMeshLights(lights, 4, origin, 3, LightedMesh::Bundled, false, limits, picked, 8);
	CHECK(n == 2 && picked[0] == 1 && picked[1] == 0);
	n = render::pickMeshLights(lights, 4, origin, 3, LightedMesh::Static, true, limits, picked, 8);
	CHECK(n == 2 && picked[0] == 2 && picked[1] == 0);
	// Bundled and skinned meshes only supportsEmitter lights (0x8b09's bit
	// 0x8000); static meshes any.
	lights[1].supportsEmitter = false;
	n = render::pickMeshLights(lights, 4, origin, 3, LightedMesh::Skinned, false, limits, picked, 8);
	CHECK(n == 1 && picked[0] == 0);
	n = render::pickMeshLights(lights, 4, origin, 3, LightedMesh::Static, false, limits, picked, 8);
	CHECK(n == 2 && picked[0] == 1 && picked[1] == 2);
	// An undergrowth patch (0x1010e270): Point lights only (0xb09), the
	// nearest 4 about its sphere, whatever the emitter flag.
	n = render::pickUndergrowthLights(lights, 4, origin, 3, picked);
	CHECK(n == 2 && picked[0] == 1 && picked[1] == 0);
	n = render::pickUndergrowthLights(lights, 4, origin, 50, picked);
	CHECK(n == 3 && picked[2] == 3);
	CHECK(render::terrainTakesLight(lights[0]) && !render::terrainTakesLight(lights[2]));
	// The flicker's factor lies in 1 - flicker .. 1; MSVC's rand from 1: 41.
	render::MsvcRand rand;
	CHECK(rand.next() == 41);
	CHECK(near(render::flickerFactor(0.5f, 32767), 1) && near(render::flickerFactor(0.5f, 0), 0.5f));
	CHECK(near(render::flickerFactor(0, 0), 1));
}

// The terrain's point-light passes (Hi_Terrain's PerPixelPointlight,
// 0x1014e080; LevelRenderer::applyTerrainLights): every listed Point light
// (filter 0x909, no spots), the shader's 8 nearest the camera by distance
// less radius, each adding colour x N.L x saturate(1 - d^2 / R^2) x 0.5.
TEST(RenderScene_TerrainPointLightPasses)
{
	render::ListedLight lights[11];
	for (int i = 0; i < 11; ++i) {
		lights[i].position[0] = 10.0f * static_cast<float>(i + 1); // 10 .. 110 m along x
		lights[i].radius = 5;
	}
	lights[0].spot = true;    // a spot: not the terrain's
	lights[10].radius = 200;  // far, but reaching over the camera: first
	const float camera[3] = {0, 0, 0};
	int picked[8];
	const int n = render::pickTerrainLights(lights, 11, camera, 8, picked);
	CHECK(n == 8);
	CHECK(picked[0] == 10 && picked[1] == 1 && picked[2] == 2 && picked[7] == 7);
	for (int i = 0; i < n; ++i) {
		CHECK(!lights[picked[i]].spot);
	}
	CHECK(render::pickTerrainLights(lights, 1, camera, 8, picked) == 0); // the spot alone
	CHECK(near(render::pointLightInverseRange2(10), 0.01f) && near(render::pointLightInverseRange2(0), 10000));
	// One light's term over a flat patch (normal up), R = 10 m.
	const float up[3] = {0, 1, 0};
	const float inv = render::pointLightInverseRange2(10);
	const float above[3] = {0, 5, 0};    // straight up, 5 of 10 m: 1 - 0.25
	CHECK(near(render::terrainPointLight(above, inv, up), 0.375f));
	const float aside[3] = {5, 5, 0};    // 45 degrees, d^2 50: 0.7071 x 0.5 x 0.5
	CHECK(near(render::terrainPointLight(aside, inv, up), 0.70710678f * 0.5f * 0.5f));
	const float below[3] = {0, -5, 0};   // behind the surface: none
	CHECK(render::terrainPointLight(below, inv, up) == 0);
	const float beyond[3] = {0, 11, 0};  // past R: none
	CHECK(render::terrainPointLight(beyond, inv, up) == 0);
}

// The object manager's distance fade (0x006ff110 / 0x006fb280 / 0x006fb320):
// at high geometry quality (8 / 10 / 80) an object of R = 1 is whole within
// 0.8 x 6400 m^2 of its edge, eases out to 6400 and is gone past 1.2 x 6400;
// a vehicle's area takes +0x144's 10.
TEST(RenderScene_ObjectDistanceFade)
{
	using namespace dice::hfe::render;
	const ObjectCullScales high = objectCullScales(3);
	CHECK(high.area == 8 && high.actorArea == 10 && high.minimum == 80);
	CHECK(objectCullScales(1).minimum == 40 && objectCullScales(2).actorArea == 8);
	CHECK(viewFarDistance(330.4f, 1) == 330 && viewFarDistance(5000, 0.5f) == 1500 && viewFarDistance(5, 1) == 20);
	const CullRecord prop = cullRecord(high, 1, 1000, false, 0.1f);
	CHECK(prop.minArea == 6400 && prop.areaScale == 640);
	// R = 1: pi x 640 < 6400, so A = 6400.
	CHECK(stepDistanceFade(0, 1, 1 + 5000, prop) == 1);                  // d^2 5000 <= 5120
	CHECK(stepDistanceFade(1, 1, 1 + 6000, prop) == 1);                  // whole stays whole in the near band
	const float in = stepDistanceFade(0, 1, 1 + 6000, prop);             // 1 - 880 / 1200 = 0.2667 vs 0.4
	CHECK(std::fabs(in - 0.4f) < 1e-5f);
	const float out = stepDistanceFade(1, 1, 1 + 7000, prop);            // 1 - 600 / 1400 = 0.571 vs 0.6
	CHECK(std::fabs(out - (1 - 600.0f / 1400.0f)) < 1e-5f);
	CHECK(stepDistanceFade(0, 1, 1 + 7000, prop) == 0);                  // gone stays gone in the far band
	CHECK(stepDistanceFade(1, 1, 1 + 7700, prop) == 0);                  // past 1.2 A
	// The view distance cuts first: (R + p1)^2.
	const CullRecord shortView = cullRecord(high, 1, 50, false, 0.1f);
	CHECK(stepDistanceFade(1, 1, 1 + 2700, shortView) == 0);
	// A big object (R = 10): pi x 100 x 640 = 201062 m^2, whole to ~401 m.
	CHECK(stepDistanceFade(0, 10, 100 + 400 * 400, prop) == 1);
	// A vehicle of R = 5 at 260 m: pi x 25 x 1000 = 78540; 0.8 of it is 62832.
	const CullRecord vehicle = cullRecord(high, 1, 1000, true, 0.1f);
	CHECK(stepDistanceFade(0, 5, 25 + 240 * 240, vehicle) == 1);
	CHECK(stepDistanceFade(0, 5, 25 + 240 * 240, prop) < 1); // as a prop: 50265, past 0.8 A
	// Zoomed 4x: p0 and p2 grow with it.
	const CullRecord zoomed = cullRecord(high, 4, 1000, false, 0.1f);
	CHECK(stepDistanceFade(0, 1, 1 + 15000, zoomed) == 1);
}

// The light manager's SimpleShadowmap (RendDX9 0x100aa290, 0x100a8cb0 /
// 0x100a9160) and the meshes' sun factor (0x100c3880 / 0x10162070).
TEST(RenderScene_SimpleShadowmap)
{
	using namespace dice::hfe::render;
	// 0x100a8f40: 2-bit steps of the byte.
	CHECK(SimpleShadowmap::quantize(0) == 0 && SimpleShadowmap::quantize(42) == 0 && SimpleShadowmap::quantize(43) == 1);
	CHECK(SimpleShadowmap::quantize(128) == 2 && SimpleShadowmap::quantize(212) == 2 && SimpleShadowmap::quantize(213) == 3);
	CHECK(SimpleShadowmap::quantize(255) == 3);
	// An 8 x 8 map (16 m across, 2 m a texel, about the origin): all lit
	// but file column 5, row 1 (x 2..4, z 4..6: the first row is +z), and
	// that texel indoors.
	const int n = 8;
	std::vector<uint8_t> occlusion(n * n, 255), io(n * n, 255);
	occlusion[1 * n + 5] = 0;
	io[1 * n + 5] = 0;
	SimpleShadowmap map;
	CHECK(map.occlusionAt(0, 0) == 1); // nothing loaded
	map.load(n, occlusion, io);
	CHECK(map.loaded() && map.size() == n);
	CHECK(std::fabs(map.occlusionAt(3, 5)) < 1e-6f);   // the texel's centre
	CHECK(std::fabs(map.outdoorsAt(3, 5)) < 1e-6f);
	CHECK(std::fabs(map.occlusionAt(4, 5) - 0.5f) < 1e-5f); // half way to the next texel
	CHECK(std::fabs(map.occlusionAt(3, 3) - 1) < 1e-6f);    // the row below it (z 2..4)
	CHECK(std::fabs(map.occlusionAt(-3, 5) - 1) < 1e-6f);
	// Past the map: clamped to +-N, the border is lit and outdoors.
	CHECK(map.occlusionAt(100, -100) == 1 && map.outdoorsAt(-100, 100) == 1);
	// 1/3 steps.
	occlusion[1 * n + 5] = 90; // q 1
	map.load(n, occlusion, {});
	CHECK(std::fabs(map.occlusionAt(3, 5) - 1.0f / 3.0f) < 1e-5f);
	CHECK(map.outdoorsAt(3, 5) == 1); // no IO file: outdoors
	// A short file loads nothing.
	map.load(n, std::vector<uint8_t>(10, 0), {});
	CHECK(!map.loaded() && map.occlusionAt(3, 5) == 1);

	// The factor: outdoors with nothing above, the map faded out by the
	// height over the ground across 3..6 m; indoors with a roof, the map.
	CHECK(std::fabs(simpleShadowSunScale(0.25f, 1, false, 1) - 0.25f) < 1e-6f);
	CHECK(std::fabs(simpleShadowSunScale(0.25f, 1, false, 4.5f) - 0.625f) < 1e-6f);
	CHECK(simpleShadowSunScale(0.25f, 1, false, 6) == 1);
	CHECK(simpleShadowSunScale(0.25f, 0, false, 1) == 1); // indoors, open above
	CHECK(simpleShadowSunScale(0.25f, 0, true, 1) == 0.25f);
	CHECK(simpleShadowSunScale(0.25f, 1, true, 1) == 1); // outdoors under something
	// renderer.useSunScaling (skinned meshes): taken as indoors.
	CHECK(simpleShadowSunScale(0.25f, 1, true, 1, true) == 0.25f);
	CHECK(std::fabs(simpleShadowSunScale(0.25f, 0, false, 1, true) - 0.25f) < 1e-6f);
	// The downward ray: 6 m when it meets nothing.
	CHECK(simpleShadowHeight(2) == 2 && simpleShadowHeight(7) == 6 && simpleShadowHeight(-1) == 6);
}


TEST(RenderScene_StaticLightmapsByLightingQuality)
{
	using namespace dice::hfe::render;
	// 0x100df010: low lighting (1) drops the statics' lightmaps but Titan
	// parts' and objects/water/ meshes'; medium (2) and high (3) keep all.
	CHECK(!staticKeepsLightmap(false, false, 1));
	CHECK(staticKeepsLightmap(true, false, 1));
	CHECK(staticKeepsLightmap(false, true, 1));
	CHECK(staticKeepsLightmap(false, false, 2));
	CHECK(staticKeepsLightmap(false, false, 3));
}

TEST(RenderScene_CustomTextureSuffix)
{
	using namespace dice::hfe::render;
	// 0x10018ca0: "<name up to its last '.'>_<suffix>.dds" (0x1019f4c0).
	CHECK(customSuffixedTexturePath("objects/vehicles/eu_fav/textures/eu_fav_c.dds", "Snow") ==
		"objects/vehicles/eu_fav/textures/eu_fav_c_Snow.dds");
	CHECK(customSuffixedTexturePath("textures/billboard_color01.tga", "wake") == "textures/billboard_color01_wake.dds");
	CHECK(customSuffixedTexturePath("noextension", "HEAT") == "noextension_HEAT.dds");
	// None without a suffix, nor for IGA's "rad_" textures.
	CHECK(customSuffixedTexturePath("objects/x.dds", "").empty());
	CHECK(customSuffixedTexturePath("rad_2041100.dds", "HEAT").empty());
	// 0x10017340: the name as given, then its .dds (the heavy-armour kit
	// pictures ship only as Soldier_xx1.dds), then the bare stem.
	CHECK(texturePathCandidates("Ingame/Customization/soldiers/Soldier_021.tga")
		== std::vector<std::string>({"Ingame/Customization/soldiers/Soldier_021.tga",
			"Ingame/Customization/soldiers/Soldier_021.dds", "Ingame/Customization/soldiers/Soldier_021"}));
	CHECK(texturePathCandidates("Common/Textures/x") == std::vector<std::string>({"Common/Textures/x.dds", "Common/Textures/x"}));
	CHECK(texturePathCandidates("mods/a.b/x") == std::vector<std::string>({"mods/a.b/x.dds", "mods/a.b/x"}));
}

TEST(RenderScene_RoadStaticMeshTechniques)
{
	using namespace dice::hfe::render;
	// 0x100de6d0's flags (a case-blind find) and 0x100bdb00's keys.
	CHECK(roadStaticPath("Objects/roads/statics/t_intersection/Meshes/t_intersection.staticMesh"));
	CHECK(!roadStaticNoBlend("Objects/roads/statics/t_intersection/Meshes/t_intersection.staticMesh"));
	CHECK(roadStaticNoBlend("Objects/Roads/staticsnoblend/concreteblock_helipad/Meshes/concreteblock_helipad.staticMesh"));
	CHECK(!roadStaticPath("Objects/StaticObjects/roads_sign.staticMesh"));
	CHECK(!roadStaticNoBlend("Objects/StaticObjects/noBlend/x.staticMesh"));
	CHECK(roadStaticTechnique(false, false) == RoadTechnique::Road);
	CHECK(roadStaticTechnique(false, true) == RoadTechnique::Road); // no detail map: Road even under noBlend
	CHECK(roadStaticTechnique(true, false) == RoadTechnique::RoadDetail);
	CHECK(roadStaticTechnique(true, true) == RoadTechnique::RoadDetailNoBlend);
}

// A transparent-white HUD texel must contribute no colour in the walkers'
// 7/2 additive mode. Mistaking Meme 7 for D3DBLEND_DESTALPHA instead uses
// the opaque scene's alpha and exposes the entire white texture rectangle.
TEST(Render_HudUsesMemeBlendFactors)
{
	CHECK(render::hudBlendToD3D(7) == 5); // D3DBLEND_SRCALPHA
	CHECK(render::hudBlendToD3D(8) == 6); // default 7/8 is ordinary alpha blending
	CHECK(render::hudBlendToD3D(2) == 2); // additive destination remains ONE
	CHECK(render::hudBlendToD3D(5) == 9);
	CHECK(render::hudBlendToD3D(9) == 7);
}

TEST(RenderScene_MeshLocalTextures)
{
	io::FileManager files;
	const std::string root = "Objects/Weapons/Handheld/EU_Rorsch_Mk_V/";
	const std::string mesh = root + "Meshes/eu_rorsch_mk_v.bundledmesh";
	const auto exists = [&](const std::string& p) { return files.exists(p); };
	files.addMemoryFile(root + "Textures/rifle_01_c.dds", {1});
	files.addMemoryFile(root + "Textures/rifle_01_b.dds", {2});
	CHECK(render::meshTexturePath("rifle_01_c.dds", mesh, exists) == root + "Textures/rifle_01_c.dds");
	CHECK(render::meshTexturePath("rifle_01_b.dds", mesh, exists) == root + "Textures/rifle_01_b.dds");
	CHECK(render::meshTexturePath("Common/Textures/x.dds", mesh, exists) == "Common/Textures/x.dds");
	CHECK(render::meshTexturePath("missing.dds", mesh, exists) == "missing.dds");
	files.addMemoryFile("rifle_01_c.dds", {3});
	CHECK(render::meshTexturePath("rifle_01_c.dds", mesh, exists) == "rifle_01_c.dds");

	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	io::FileManager shipped;
	shipped.setRootPath(gameDir);
	CHECK(shipped.mountArchive("mods/Project_Remaster_v17_Final/Objects/Weapons_client.zip", "Objects/Weapons"));
	std::vector<uint8_t> data;
	CHECK(shipped.readFile(mesh, data));
	const auto parsed = geom::parseMesh(data, geom::MeshKind::Bundled);
	CHECK(parsed.has_value());
	int localMaps = 0;
	for (const auto& geometry : parsed->geometries) for (const auto& lod : geometry.lods)
		for (const auto& material : lod.materials) for (const auto& path : material.maps) {
			if (path.find_first_of("/\\") != std::string::npos) continue;
			const auto resolved = render::meshTexturePath(path, mesh,
				[&](const std::string& p) { return shipped.exists(p); });
			CHECK(resolved != path);
			CHECK(shipped.readFile(resolved, data));
			++localMaps;
		}
	CHECK(localMaps > 20);
}
