#include "WorldConsole.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Object.h"
#include "ObjectTemplateManager.h"

#include <cmath>
#include <cstdio>

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
