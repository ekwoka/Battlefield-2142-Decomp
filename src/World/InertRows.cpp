#include "InertRows.h"

#include "Console/Console.h"

#include <utility>

namespace dice::hfe::world {

namespace {

std::string g_customTextureSuffix;

struct InertRow {
	const char* name;
	ConsoleMethodKind kind;
	int minArgs;
	int maxArgs;
};

constexpr ConsoleMethodKind Method = ConsoleMethodKind::Method;
constexpr ConsoleMethodKind Property = ConsoleMethodKind::ReadWriteProperty;

// Why each row is inert here. The argument counts are the rows'
// (symbols/BF2142.exe.console.csv, RendDX9_ori.dll.console.csv).
constexpr InertRow kInertRows[] = {
	// The object's render object's slot 0xe0 (0x006ed811 -> 0x006f6f90):
	// stored by static meshes only (+0x2b0) and read back only for the
	// console (engine.md, "Object.setLightSourceMask").
	{"Object.setLightSourceMask", Method, 1, 1},
	// The heightmap's slot 0x24 (executor 0x006b3a5f -> the cluster's active
	// heightmap, vtable 0x009949f8 slot 9 0x006d3a50): its modified flag
	// +0xf0, which an edited height (0x006d4040) also sets; the editor's
	// "save me" flag, nothing in play reads it.
	{"heightmap.setModified", Method, 1, 1},
	// The award definitions of awards.con (0x00420a85). src/Stats reads
	// awards.con itself (Stats::parseAwards); the awards a soldier holds
	// were the stats server's (engine.md, "Awards").
	{"awards.setData", Method, 2, 9},
	// The ragdoll template's script rows (BF2142.exe 0x008ae503..0x008afbee,
	// RagDoll.cpp's reader). The rewrite reads ragDollInit.con itself
	// (Animation/RagDoll.h, loadRagDoll), so these lines are already taken.
	{"RagDoll.reset", Method, 0, 0},
	{"RagDoll.save", Method, 0, 0},
	{"RagDoll.active", Method, 1, 1},
	{"RagDoll.drawConstraints", Method, 1, 1},
	{"RagDoll.gravity", Method, 1, 1},
	{"RagDoll.skeleton", Method, 1, 1},
	{"RagDoll.addParticle", Method, 2, 2},
	{"RagDoll.addBone", Method, 2, 2},
	{"RagDoll.lockBone", Method, 2, 2},
	{"RagDoll.toSkeleton", Method, 3, 3},
	{"RagDoll.linkMode", Method, 2, 2},
	{"RagDoll.addConstraint", Method, 2, 2},
	{"RagDoll.addCapsuleCollision", Method, 2, 2},
	{"RagDoll.addAngularConstraint", Method, 4, 4},
	{"RagDoll.addDihedralAngleConstraint", Method, 5, 5},
	{"RagDoll.removeConstraint", Method, 2, 2},
	{"RagDoll.addForce", Method, 2, 2},
	{"RagDoll.setParticleCollisionCriteria", Method, 2, 2},
	{"RagDoll.addJointConstraint", Method, 2, 2},
	{"RagDoll.addDistanceLessThanConstraint", Method, 3, 3},
	// The console's slot 0x7c (executor 0x0066ce8d). Every shipped call (170
	// in the archives) is inside an `if v_arg1 == BF2Editor` block of a
	// level's StaticObjects.con: the editor's.
	{"Console.allowMultipleFileLoad", Property, 0, 1},
	// The exe's AI weapon record +0x70 (executor 0x00887e88, getter
	// 0x00892720, slot 46) and +0x10 (0x008874ab, getter 0x00892500, slot
	// 50): AIDLL's weapon choice (0x10074ef0) reads neither (engine.md).
	{"weaponTemplate.shouldRearm", Property, 0, 1},
	{"weaponTemplate.fireRate", Property, 0, 1},
	// 0x0042771b -> 0x004bc1e0: makes an object of the template 10 m in
	// front of the local player's camera. Called only by AI/vehicle.con, a
	// developer's spawning script nothing runs.
	{"GameLogic.createObject", Method, 1, 1},
	// The geometry templates' build options (RendDX9; uncited.md, render
	// pass 5): compressVertexData (slot 0x90, executor 0x100748d7) and
	// maxTextureRepeat (0xa0, 0x10074c2e) are the vertex packer's, written
	// back by the templates' .con writers (0x100a1af0, 0x100da580) and used
	// when the editor compiles a mesh; lightmapOverride, doNotGenerateLightmaps
	// and setLightmapSizeAll are the lightmapper's; isAd (0xd8, 0x1007375c)
	// is IGA's. boundingRadiusModifier (0xe0, 0x100738c1): the base template
	// (vtable 0x101d3f58) and the bundled and skinned ones have the empty
	// 0x100bc0b0 there; the static mesh template (0x101d6588) stores it at
	// +0x2b0 (0x100e54d0), read only by its slot 57 (0x100e54e0) for the
	// console. Only bp1_flagpole sets it.
	{"GeometryTemplate.compressVertexData", Property, 0, 1},
	{"GeometryTemplate.maxTextureRepeat", Property, 0, 1},
	{"GeometryTemplate.lightmapOverride", Property, 0, 1},
	{"GeometryTemplate.doNotGenerateLightmaps", Property, 0, 1},
	{"GeometryTemplate.setLightmapSizeAll", Method, 3, 3},
	{"GeometryTemplate.isAd", Property, 0, 1},
	{"GeometryTemplate.boundingRadiusModifier", Property, 0, 1},
	// The terrain renderer's slot 0x14 (executor 0x10076a8b): builds its
	// render data after terrain.load. The rewrite's level renderer builds the
	// terrain when the level is loaded (Render/LevelRenderer).
	{"terrain.init", Method, 0, 0},
	// The rain manager's cell display (slot 0xa8, executor 0x1007fe21): a
	// debug drawing of the emitter's cells; every shipped weather.con sets 0.
	{"rainManager.cellDisplay", Property, 0, 1},
	// The undergrowth's slot 0x10 (executor 0x10085fa8) with the level's
	// folder. The rewrite reads the level's Undergrowth.cfg / .dat / .raw
	// itself (LevelRenderer::buildUndergrowth).
	{"undergrowth.load", Method, 1, 1},
};

} // namespace

const std::string& customTextureSuffix() { return g_customTextureSuffix; }
void setCustomTextureSuffix(std::string suffix) { g_customTextureSuffix = std::move(suffix); }

void registerInertConsoleRows(Console& console)
{
	for (const InertRow& row : kInertRows) {
		ConsoleMethod method;
		method.name = row.name;
		method.kind = row.kind;
		method.access = ConsoleAccess::Anyone;
		method.minArgs = row.minArgs;
		method.maxArgs = row.maxArgs;
		method.invoke = [](const std::vector<std::string>&) { return std::string(); };
		method.stub = true; // a real binding registered later replaces it
		console.registerMethod(std::move(method));
	}

	ConsoleMethod suffix;
	suffix.name = "texturemanager.customTextureSuffix";
	suffix.kind = ConsoleMethodKind::ReadWriteProperty;
	suffix.access = ConsoleAccess::Anyone;
	suffix.maxArgs = 1;
	suffix.invoke = [](const std::vector<std::string>& args) -> std::string {
		if (args.empty()) {
			return g_customTextureSuffix;
		}
		g_customTextureSuffix = args[0];
		return {};
	};
	console.registerMethod(std::move(suffix));
}

} // namespace dice::hfe::world
