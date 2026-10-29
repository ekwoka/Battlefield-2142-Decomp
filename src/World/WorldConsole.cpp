#include "WorldConsole.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Object.h"
#include "ObjectTemplateManager.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace dice::hfe::world {

namespace {

Vec3 toVec3(const ConsoleVec3& v) { return {v.x, v.y, v.z}; }
ConsoleVec3 toConsole(const Vec3& v) { return {v.x, v.y, v.z}; }

// "[a/b/c/d][e/f/g/h][i/j/k/l][m/n/o/p]": a 4x4 matrix by rows, row vectors
// (the translation is the last row).
bool parseMatrix(const std::string& text, float m[16])
{
	return std::sscanf(text.c_str(), " [%f/%f/%f/%f][%f/%f/%f/%f][%f/%f/%f/%f][%f/%f/%f/%f]", &m[0], &m[1], &m[2],
			   &m[3], &m[4], &m[5], &m[6], &m[7], &m[8], &m[9], &m[10], &m[11], &m[12], &m[13], &m[14], &m[15]) == 16;
}

// Yaw/pitch/roll in degrees of a rotation built as roll (z), then pitch
// (x), then yaw (y), the engine's order; ignores scale.
Vec3 rotationFromMatrix(const float m[16])
{
	constexpr float toDegrees = 180.0f / 3.14159265358979f;
	const float sinPitch = std::fmax(-1.0f, std::fmin(1.0f, -m[9]));
	return {std::atan2(m[8], m[10]) * toDegrees, std::asin(sinPitch) * toDegrees, std::atan2(m[1], m[5]) * toDegrees};
}

std::string formatMatrix(const Vec3& position, const Vec3& rotation)
{
	constexpr float toRadians = 3.14159265358979f / 180.0f;
	const float cy = std::cos(rotation.x * toRadians), sy = std::sin(rotation.x * toRadians);
	const float cp = std::cos(rotation.y * toRadians), sp = std::sin(rotation.y * toRadians);
	const float cr = std::cos(rotation.z * toRadians), sr = std::sin(rotation.z * toRadians);
	// Rows of rotationZ * rotationX * rotationY.
	const float rows[3][3] = {
		{cr * cy + sr * sp * sy, sr * cp, -cr * sy + sr * sp * cy},
		{-sr * cy + cr * sp * sy, cr * cp, sr * sy + cr * sp * cy},
		{cp * sy, -sp, cp * cy},
	};
	char text[256];
	std::snprintf(text, sizeof(text), "[%g/%g/%g/0][%g/%g/%g/0][%g/%g/%g/0][%g/%g/%g/1]", rows[0][0], rows[0][1],
		rows[0][2], rows[1][0], rows[1][1], rows[1][2], rows[2][0], rows[2][1], rows[2][2], position.x, position.y,
		position.z);
	return text;
}

// Scripts name templates by their first word ("ptrproxy" arguments).
std::string firstWord(const std::string& text)
{
	const size_t start = text.find_first_not_of(" \t");
	if (start == std::string::npos) {
		return {};
	}
	const size_t end = text.find_first_of(" \t", start);
	return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

} // namespace

void registerWorldConsoleMethods(Console& console, ObjectTemplateManager& templates, ObjectManager& objects)
{
	const uint32_t access = ConsoleAccess::Anyone;
	const auto activeTemplate = [&templates] { return templates.active() != nullptr; };
	const auto activeObject = [&objects] { return objects.active() != nullptr; };

	// --- ObjectTemplate ---------------------------------------------------
	bindMethod(console, "ObjectTemplate.create", access, [&templates](std::string type, std::string name) {
		ObjectTemplate* created = templates.create(type, firstWord(name));
		return created ? created->name() : std::string();
	});

	ConsoleMethod active;
	active.name = "ObjectTemplate.active";
	active.kind = ConsoleMethodKind::ReadWriteProperty;
	active.access = access;
	active.maxArgs = 1;
	active.invoke = [&templates](const std::vector<std::string>& args) -> std::string {
		if (args.empty()) {
			return templates.active() ? templates.active()->name() : std::string();
		}
		templates.setActive(templates.find(firstWord(args[0])));
		return {};
	};
	console.registerMethod(active);

	// deviation.setFireDev max perShot decay: a method of three floats
	// (0x004e7af4 -> 0x004e7c73 -> template 0x005f5360: +0x10, +0x8, +0xc),
	// which the generic template properties (one value each) can't keep:
	// kept on the template as the three numbers.
	ConsoleMethod fireDev;
	fireDev.name = "ObjectTemplate.deviation.setFireDev";
	fireDev.kind = ConsoleMethodKind::Method;
	fireDev.access = access;
	fireDev.minArgs = 3;
	fireDev.maxArgs = 3;
	fireDev.isActive = activeTemplate;
	fireDev.invoke = [&templates](const std::vector<std::string>& args) -> std::string {
		templates.active()->settings()["deviation.setFireDev"] = args[0] + " " + args[1] + " " + args[2];
		return {};
	};
	console.registerMethod(fireDev);

	// The soldier deviation's other builds (SoldierDeviationComp, template
	// 0x005de3c0 / 0x005de3e0 / 0x005de400): setTurnDev and setSpeedDev of
	// four numbers (0x004e846a, 0x004e8673), setMiscDev of three
	// (0x004e8828), kept as the numbers in order.
	for (const auto& [property, count] : {std::pair<const char*, int> {"deviation.setTurnDev", 4},
			 {"deviation.setSpeedDev", 4}, {"deviation.setMiscDev", 3}}) {
		ConsoleMethod build;
		build.name = std::string("ObjectTemplate.") + property;
		build.kind = ConsoleMethodKind::Method;
		build.access = access;
		build.minArgs = count;
		build.maxArgs = count;
		build.isActive = activeTemplate;
		build.invoke = [&templates, key = std::string(property)](const std::vector<std::string>& args) -> std::string {
			std::string value;
			for (const std::string& arg : args) {
				value += (value.empty() ? "" : " ") + arg;
			}
			templates.active()->settings()[key] = value;
			return {};
		};
		console.registerMethod(std::move(build));
	}

	// The unlocks' Ability lists (template component 0xc4b3):
	// ability.setMaterialDamageMod <material> <modifier> (0x0054eadc ->
	// slot 0x94) and ability.addExtraAmmoItem <item> <mags> <rounds> <flag>
	// (0x0054ebed), kept in order as "<name>.list" = "args;...".
	for (const auto& [property, count] : {std::pair<const char*, int> {"ability.setMaterialDamageMod", 2},
			 {"ability.addExtraAmmoItem", 4}}) {
		ConsoleMethod list;
		list.name = std::string("ObjectTemplate.") + property;
		list.kind = ConsoleMethodKind::Method;
		list.access = access;
		list.minArgs = count;
		list.maxArgs = count;
		list.isActive = activeTemplate;
		list.invoke = [&templates, key = std::string(property) + ".list"](const std::vector<std::string>& args) -> std::string {
			std::string& kept = templates.active()->settings()[key];
			for (size_t i = 0; i < args.size(); ++i) {
				kept += (i ? " " : "") + firstWord(args[i]);
			}
			kept += ";";
			return {};
		};
		console.registerMethod(std::move(list));
	}

	// replaceItem <item> (ItemContainer, 0x004e0758 -> 0x005bdd80: each
	// call appends to the container's list at +0x2b4): the kit items the
	// unlock's own item takes the place of. Kept in order as
	// "replaceItem.list" = "item;...".
	{
		ConsoleMethod replace;
		replace.name = "ObjectTemplate.replaceItem";
		replace.kind = ConsoleMethodKind::Method;
		replace.access = access;
		replace.minArgs = 1;
		replace.maxArgs = 1;
		replace.isActive = activeTemplate;
		replace.invoke = [&templates](const std::vector<std::string>& args) -> std::string {
			templates.active()->settings()["replaceItem.list"] += firstWord(args[0]) + ";";
			return {};
		};
		console.registerMethod(std::move(replace));
	}

	// supplyGroupNeeded <team> <group> [optional] (0x00503dec / 0x00503f73:
	// two or three arguments, optional false by default; the ControlPoint
	// template's vt+0xc0 adds the pair to the team's list). Kept in order
	// as "supplyGroupNeeded.list" = "team group optional;...".
	ConsoleMethod supplyGroup;
	supplyGroup.name = "ObjectTemplate.supplyGroupNeeded";
	supplyGroup.kind = ConsoleMethodKind::Method;
	supplyGroup.access = access;
	supplyGroup.minArgs = 2;
	supplyGroup.maxArgs = 3;
	supplyGroup.isActive = activeTemplate;
	supplyGroup.invoke = [&templates](const std::vector<std::string>& args) -> std::string {
		const bool optional = args.size() > 2 && parseConsoleValue<bool>(args[2]);
		templates.active()->settings()["supplyGroupNeeded.list"] += std::to_string(parseConsoleValue<int>(args[0]))
			+ " " + std::to_string(parseConsoleValue<int>(args[1])) + (optional ? " 1;" : " 0;");
		return {};
	};
	console.registerMethod(supplyGroup);

	// activeSafe <type> <name>: the type is only checked in debug builds.
	bindMethod(console, "ObjectTemplate.activeSafe", access, [&templates](std::string, std::string name) {
		templates.setActive(templates.find(firstWord(name)));
	});

	ConsoleMethod createComponent;
	createComponent.name = "ObjectTemplate.createComponent";
	createComponent.access = access;
	createComponent.minArgs = 1;
	createComponent.maxArgs = 1;
	createComponent.isActive = activeTemplate;
	createComponent.invoke = [&templates](const std::vector<std::string>& args) {
		templates.addComponent(*templates.active(), firstWord(args[0]));
		return std::string();
	};
	console.registerMethod(createComponent);

	// Soldiers' hit capsules on skeleton bones (bone, material, offset,
	// radius, length), kept in order as skeletonCollisionBone.0, .1, ...
	ConsoleMethod collisionBone;
	collisionBone.name = "ObjectTemplate.setSkeletonCollisionBone";
	collisionBone.access = access;
	collisionBone.minArgs = 5;
	collisionBone.maxArgs = 5;
	collisionBone.isActive = activeTemplate;
	collisionBone.invoke = [&templates](const std::vector<std::string>& args) {
		auto& settings = templates.active()->settings();
		int index = 0;
		while (settings.count("skeletonCollisionBone." + std::to_string(index))) {
			++index;
		}
		settings["skeletonCollisionBone." + std::to_string(index)] =
			args[0] + " " + args[1] + " " + args[2] + " " + args[3] + " " + args[4];
		return std::string();
	};
	console.registerMethod(collisionBone);

	// The soldier's eye for each pose (0 stand, 1 crouch, 2 prone) from its
	// origin, x/y/z (the property at 0x004d0000's descriptor table), kept as
	// "poseCameraPosition.<pose>".
	ConsoleMethod poseCamera;
	poseCamera.name = "ObjectTemplate.poseCameraPosition";
	poseCamera.access = access;
	poseCamera.minArgs = 2;
	poseCamera.maxArgs = 2;
	poseCamera.isActive = activeTemplate;
	poseCamera.invoke = [&templates](const std::vector<std::string>& args) {
		templates.active()->settings()["poseCameraPosition." + std::to_string(parseConsoleValue<int>(args[0]))] =
			firstWord(args[1]);
		return std::string();
	};
	console.registerMethod(poseCamera);

	// Vehicle seats: a PlayerControlObject with seat information is a seat
	// (its name, the soldier's position and rotation in the seat), and
	// where a soldier leaving it goes. Kept as "seatInformation" ("name
	// x/y/z yaw/pitch/roll") and "soldierExitLocation" ("x/y/z yaw/pitch/roll").
	for (const char* name : {"seatInformation", "setSoldierExitLocation"}) {
		ConsoleMethod seat;
		seat.name = std::string("ObjectTemplate.") + name;
		seat.access = access;
		seat.minArgs = std::string(name) == "seatInformation" ? 3 : 2;
		seat.maxArgs = seat.minArgs;
		seat.isActive = activeTemplate;
		const std::string key = std::string(name) == "seatInformation" ? "seatInformation" : "soldierExitLocation";
		seat.invoke = [&templates, key](const std::vector<std::string>& args) {
			std::string value;
			for (const std::string& arg : args) {
				value += (value.empty() ? "" : " ") + arg;
			}
			templates.active()->settings()[key] = value;
			return std::string();
		};
		console.registerMethod(seat);
	}

	// Object spawners: the template each team's spawner creates.
	ConsoleMethod setObjectTemplate;
	setObjectTemplate.name = "ObjectTemplate.setObjectTemplate";
	setObjectTemplate.access = access;
	setObjectTemplate.minArgs = 2;
	setObjectTemplate.maxArgs = 2;
	setObjectTemplate.isActive = activeTemplate;
	setObjectTemplate.invoke = [&templates](const std::vector<std::string>& args) {
		const int team = parseConsoleValue<int>(args[0]);
		templates.active()->settings()["objectTemplate.team" + std::to_string(team)] = firstWord(args[1]);
		return std::string();
	};
	console.registerMethod(setObjectTemplate);

	// Soldier skeletons (0x004de37d): write-only methods; the renderer reads
	// them from the template's settings.
	for (const char* property : {"skeleton1P", "skeleton3P"}) {
		bindMethod(console, std::string("ObjectTemplate.") + property, access, [&templates, property](std::string path) {
			if (templates.active()) {
				templates.active()->settings()[property] = firstWord(path);
			}
		});
	}

	// mapMaterial <collision mesh material index> <name> <material id>
	// (0x0051c71f): kept as "mapMaterial.<index>" = id.
	bindMethod(console, "ObjectTemplate.mapMaterial", access, [&templates](int index, std::string, int id) {
		if (templates.active()) {
			templates.active()->settings()["mapMaterial." + std::to_string(index)] = std::to_string(id);
		}
	});

	// armor.addArmorEffect <percent> <effect> <position> <rotation>: an
	// effect the armour starts as its percent falls to the level (see
	// GameWorld's Titans, 0x00654730); kept in order as
	// "armor.addArmorEffect.list" = "level effect position rotation;...".
	bindMethod(console, "ObjectTemplate.armor.addArmorEffect", access,
		[&templates](int level, std::string effect, std::string position, std::string rotation) {
			if (templates.active()) {
				std::string& list = templates.active()->settings()["armor.addArmorEffect.list"];
				list += std::to_string(level) + " " + effect + " " + position + " " + rotation + ";";
			}
		});
	// armor.addArmorEffectSpectacular, the same: the level's effects after a
	// spectacular hit (GameWorld's armour effects), kept apart as
	// "armor.addArmorEffectSpectacular.list".
	bindMethod(console, "ObjectTemplate.armor.addArmorEffectSpectacular", access,
		[&templates](int level, std::string effect, std::string position, std::string rotation) {
			if (templates.active()) {
				std::string& list = templates.active()->settings()["armor.addArmorEffectSpectacular.list"];
				list += std::to_string(level) + " " + effect + " " + position + " " + rotation + ";";
			}
		});
	// armor.addWreckArmorEffect, the same arguments (template slot 0x16c,
	// 0x00630c00: names +0x7c, positions +0x8c, rotations +0x9c, levels
	// +0xac). The armour's instance resolves them into its own level map
	// (0x0062fb50, +0x1c) and points +0x210 at it (0x00655630), but nothing
	// reads +0x210 in BF2142.exe and no shipped template calls it: kept as
	// "armor.addWreckArmorEffect.list", never started.
	bindMethod(console, "ObjectTemplate.armor.addWreckArmorEffect", access,
		[&templates](int level, std::string effect, std::string position, std::string rotation) {
			if (templates.active()) {
				std::string& list = templates.active()->settings()["armor.addWreckArmorEffect.list"];
				list += std::to_string(level) + " " + effect + " " + position + " " + rotation + ";";
			}
		});

	// setGearRatios <5 ratios> (0x005406d1): an engine's ratio per gear, kept
	// as "gearRatios" = "a/b/c/d/e".
	bindMethod(console, "ObjectTemplate.setGearRatios", access,
		[&templates](float a, float b, float c, float d, float e) {
			if (templates.active()) {
				char text[128];
				std::snprintf(text, sizeof(text), "%g/%g/%g/%g/%g", a, b, c, d, e);
				templates.active()->settings()["gearRatios"] = text;
			}
		});

	bindProperty<std::string>(console, "ObjectTemplate.geometry", access,
		[&templates] { return templates.active() ? templates.active()->geometry() : std::string(); },
		[&templates](const std::string& name) {
			if (templates.active()) {
				templates.active()->setGeometry(firstWord(name));
			}
		});

	// Children: addTemplate appends, setPosition/setRotation change the
	// child at `index`, by default the one added last (0x00a739a8).
	// Sounds ("S_...") a template already created for itself are selected
	// rather than added twice (0x0060fae0).
	static int lastChild = -1;
	bindMethod(console, "ObjectTemplate.addTemplate", access, [&templates](std::string name) {
		ObjectTemplate* parent = templates.active();
		if (!parent) {
			return;
		}
		name = firstWord(name);
		auto& children = parent->children();
		if (name.compare(0, 2, "S_") == 0) {
			for (size_t i = 0; i < children.size(); ++i) {
				if (!CaseInsensitiveLess()(children[i].name, name) && !CaseInsensitiveLess()(name, children[i].name)) {
					lastChild = static_cast<int>(i);
					return;
				}
			}
		}
		children.push_back({name, {}, {}});
		lastChild = static_cast<int>(children.size()) - 1;
	});
	const auto childAt = [&templates](int index) -> ChildTemplate* {
		ObjectTemplate* parent = templates.active();
		if (!parent || index < 0 || index >= static_cast<int>(parent->children().size())) {
			return nullptr;
		}
		return &parent->children()[static_cast<size_t>(index)];
	};
	ConsoleMethod setPosition;
	setPosition.name = "ObjectTemplate.setPosition";
	setPosition.access = access;
	setPosition.minArgs = 1;
	setPosition.maxArgs = 2;
	setPosition.isActive = activeTemplate;
	setPosition.invoke = [childAt](const std::vector<std::string>& args) {
		const int index = args.size() > 1 ? parseConsoleValue<int>(args[1]) : lastChild;
		if (ChildTemplate* child = childAt(index)) {
			child->position = toVec3(parseConsoleValue<ConsoleVec3>(args[0]));
		}
		return std::string();
	};
	console.registerMethod(setPosition);
	ConsoleMethod setRotation = setPosition;
	setRotation.name = "ObjectTemplate.setRotation";
	setRotation.invoke = [childAt](const std::vector<std::string>& args) {
		const int index = args.size() > 1 ? parseConsoleValue<int>(args[1]) : lastChild;
		if (ChildTemplate* child = childAt(index)) {
			child->rotation = toVec3(parseConsoleValue<ConsoleVec3>(args[0]));
		}
		return std::string();
	};
	console.registerMethod(setRotation);

	// --- Object -----------------------------------------------------------
	// create [template [position [rotation]]]; no template means the active one.
	ConsoleMethod create;
	create.name = "Object.create";
	create.access = access;
	create.maxArgs = 3;
	create.invoke = [&templates, &objects](const std::vector<std::string>& args) -> std::string {
		ObjectTemplate* objectTemplate = args.empty() ? templates.active() : templates.find(firstWord(args[0]));
		const Vec3 position = args.size() > 1 ? toVec3(parseConsoleValue<ConsoleVec3>(args[1])) : Vec3 {};
		const Vec3 rotation = args.size() > 2 ? toVec3(parseConsoleValue<ConsoleVec3>(args[2])) : Vec3 {};
		Object* object = objects.create(objectTemplate, position, rotation);
		return object ? std::to_string(object->id()) : std::string();
	};
	console.registerMethod(create);

	ConsoleMethod position;
	position.name = "Object.absolutePosition";
	position.kind = ConsoleMethodKind::ReadWriteProperty;
	position.access = access;
	position.maxArgs = 1;
	position.isActive = activeObject;
	position.invoke = [&objects](const std::vector<std::string>& args) -> std::string {
		if (args.empty()) {
			return formatConsoleValue(toConsole(objects.active()->position()));
		}
		objects.active()->setPosition(toVec3(parseConsoleValue<ConsoleVec3>(args[0])));
		return {};
	};
	console.registerMethod(position);

	ConsoleMethod rotation = position;
	rotation.name = "Object.rotation";
	rotation.invoke = [&objects](const std::vector<std::string>& args) -> std::string {
		if (args.empty()) {
			return formatConsoleValue(toConsole(objects.active()->rotation()));
		}
		objects.active()->setRotation(toVec3(parseConsoleValue<ConsoleVec3>(args[0])));
		return {};
	};
	console.registerMethod(rotation);

	ConsoleMethod name = position;
	name.name = "Object.name";
	name.invoke = [&objects](const std::vector<std::string>& args) -> std::string {
		if (args.empty()) {
			return objects.active()->name();
		}
		objects.active()->setName(args[0]);
		return {};
	};
	console.registerMethod(name);

	ConsoleMethod overgrowth = position;
	overgrowth.name = "Object.isOvergrowth";
	overgrowth.invoke = [&objects](const std::vector<std::string>& args) -> std::string {
		if (args.empty()) {
			return objects.active()->isOvergrowth() ? "1" : "0";
		}
		objects.active()->setOvergrowth(parseConsoleValue<bool>(args[0]));
		return {};
	};
	console.registerMethod(overgrowth);

	// Overgrowth collision objects are placed by matrix.
	ConsoleMethod transformation = position;
	transformation.name = "Object.absoluteTransformation";
	transformation.invoke = [&objects](const std::vector<std::string>& args) -> std::string {
		Object* object = objects.active();
		if (args.empty()) {
			return formatMatrix(object->position(), object->rotation());
		}
		float m[16];
		if (parseMatrix(args[0], m)) {
			object->setPosition({m[12], m[13], m[14]});
			object->setRotation(rotationFromMatrix(m));
		}
		return {};
	};
	console.registerMethod(transformation);

	// 0x004ddfb0.
	ConsoleMethod controlPointId = position;
	controlPointId.name = "Object.setControlPointId";
	controlPointId.kind = ConsoleMethodKind::Method;
	controlPointId.minArgs = 1;
	controlPointId.invoke = [&objects](const std::vector<std::string>& args) -> std::string {
		objects.active()->setControlPointId(parseConsoleValue<int>(args[0]));
		return {};
	};
	console.registerMethod(controlPointId);

	ConsoleMethod loadMesh = position;
	loadMesh.name = "Object.geometry.loadMesh";
	loadMesh.kind = ConsoleMethodKind::Method;
	loadMesh.minArgs = 1;
	loadMesh.invoke = [&objects](const std::vector<std::string>& args) -> std::string {
		objects.active()->setMesh(args[0]);
		return {};
	};
	console.registerMethod(loadMesh);
}

void registerUntypedTemplateProperties(Console& console, ObjectTemplateManager& templates,
	const std::vector<std::string>& names)
{
	for (const std::string& property : names) {
		if (console.hasMethod("ObjectTemplate." + property)) {
			continue; // typed already
		}
		ConsoleMethod method;
		method.name = "ObjectTemplate." + property;
		method.kind = ConsoleMethodKind::ReadWriteProperty;
		method.access = ConsoleAccess::Anyone;
		method.maxArgs = 1;
		method.isActive = [&templates] { return templates.active() != nullptr; };
		method.invoke = [&templates, property](const std::vector<std::string>& args) -> std::string {
			auto& settings = templates.active()->settings();
			if (args.empty()) {
				const auto it = settings.find(property);
				return it == settings.end() ? std::string() : it->second;
			}
			settings[property] = args[0];
			// List adders (fire.addFireRate, ...) also keep every value in
			// order, as "<property>.list".
			const size_t dot = property.rfind('.');
			if (property.compare(dot == std::string::npos ? 0 : dot + 1, 3, "add") == 0) {
				std::string& list = settings[property + ".list"];
				list += (list.empty() ? "" : " ") + args[0];
			}
			return {};
		};
		console.registerMethod(std::move(method));
	}
}

} // namespace dice::hfe::world
