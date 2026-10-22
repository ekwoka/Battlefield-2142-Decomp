#include "GhostLayout.h"

#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <set>

namespace dice::hfe::net {

namespace {

constexpr size_t kMaxGroups = 12; // 0x0067c690 warns "has to many statemasks" above this

struct Flat {
	const GhostTemplateNode* node;
	int parent; // index in the flat list
};

// 0x0067a5e0: the subtree of `node` without other PlayerControlObjects'
// subtrees, in pre-order.
void walkGhost(const GhostTemplateNode& node, int parent, std::vector<Flat>& out)
{
	const int self = static_cast<int>(out.size());
	out.push_back({&node, parent});
	for (const GhostTemplateNode& child : node.children) {
		if (!child.controlObject) {
			walkGhost(child, self, out);
		}
	}
}

// 0x0067a270: every PlayerControlObject of the tree, in pre-order.
void findControlObjects(const GhostTemplateNode& node, bool isRoot, std::vector<std::pair<const GhostTemplateNode*, bool>>& out)
{
	if (node.controlObject) {
		out.push_back({&node, isRoot});
	}
	for (const GhostTemplateNode& child : node.children) {
		findControlObjects(child, false, out);
	}
}

} // namespace

std::vector<GhostPartLayout> buildGhostLayout(const GhostTemplateNode& root)
{
	std::vector<std::pair<const GhostTemplateNode*, bool>> roots;
	findControlObjects(root, true, roots);
	if (roots.empty()) {
		roots.push_back({&root, true});
	}
	std::vector<GhostPartLayout> parts;
	for (const auto& [ghostRoot, isTreeRoot] : roots) {
		GhostPartLayout part;
		part.templateId = ghostRoot->templateId;
		part.name = ghostRoot->name;
		std::vector<Flat> flat;
		walkGhost(*ghostRoot, -1, flat);
		for (size_t i = 0; i < flat.size(); ++i) {
			const GhostTemplateNode& n = *flat[i].node;
			GhostNodeLayout node;
			node.templateId = n.templateId;
			node.name = n.name;
			node.parent = flat[i].parent;
			node.kind = n.hints.kind;
			const bool hasParent = !(isTreeRoot && i == 0);
			const bool treeRoot = isTreeRoot && i == 0;
			const bool networked = n.hints.kind != NetworkableKind::Unknown;
			const bool skipped = (n.projectile && hasParent) || (n.forceNetworkableId && !treeRoot && hasParent);
			if (networked && !skipped) {
				int group = -1;
				if (!n.unique) {
					for (size_t g = 0; g < part.groups.size(); ++g) {
						const GhostTemplateNode& first = *flat[static_cast<size_t>(part.groups[g].members.front())].node;
						if (first.templateId == n.templateId || (!n.groupKey.empty() && first.groupKey == n.groupKey)) {
							group = static_cast<int>(g);
							break;
						}
					}
				}
				if (group < 0) {
					GhostGroupLayout g;
					g.hints = n.hints;
					part.groups.push_back(g);
					group = static_cast<int>(part.groups.size() - 1);
				}
				part.groups[static_cast<size_t>(group)].members.push_back(static_cast<int>(i));
				node.group = group;
			}
			part.nodes.push_back(node);
		}
		parts.push_back(std::move(part));
	}
	return parts;
}

bool decodeGhostState(BitReader& in, const GhostPartLayout& layout, const Vec3f& reference,
	std::vector<NetworkableState>& states)
{
	states.clear();
	for (size_t g = 0; g < layout.groups.size() && g < kMaxGroups; ++g) {
		NetworkableState state;
		if (!decodeNetworkable(in, layout.groups[g].hints, reference, state) || !state.complete) {
			return false;
		}
		states.push_back(std::move(state));
	}
	return !in.bad();
}

void encodeGhostState(BitWriter& out, const GhostPartLayout& layout, const Vec3f* reference,
	const std::vector<NetworkableState>& states)
{
	for (size_t g = 0; g < layout.groups.size() && g < kMaxGroups; ++g) {
		NetworkableState empty;
		empty.kind = layout.groups[g].hints.kind;
		encodeNetworkable(out, layout.groups[g].hints, reference, g < states.size() ? states[g] : empty);
	}
}

// ---------------------------------------------------------------------------
// From the client's templates

namespace {

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

std::string setting(const world::ObjectTemplate& t, const char* key)
{
	const auto it = t.settings().find(key);
	return it == t.settings().end() ? std::string() : it->second;
}

Vec3f vec3Setting(const world::ObjectTemplate& t, const char* key)
{
	Vec3f v;
	const std::string text = setting(t, key);
	if (text.empty()) {
		return v;
	}
	float* c[3] = {&v.x, &v.y, &v.z};
	size_t at = 0;
	for (int i = 0; i < 3 && at <= text.size(); ++i) {
		*c[i] = static_cast<float>(std::atof(text.c_str() + at));
		const size_t slash = text.find('/', at);
		if (slash == std::string::npos) {
			break;
		}
		at = slash + 1;
	}
	return v;
}

// The c_ET constants (symbols/BF2142.exe.constants.csv).
int engineTypeOf(const std::string& text)
{
	static const std::pair<const char*, int> kTypes[] = {{"c_etcar", 2}, {"c_ethelicopter", 33}, {"c_etnewcar", 66},
		{"c_etnewcar2", 194}, {"c_etnewtank", 70}, {"c_etplane", 1}, {"c_etrocket", 17}, {"c_etship", 9},
		{"c_ettank", 6}, {"c_ettorpedo", 25}};
	const std::string l = lower(text);
	for (const auto& [name, value] : kTypes) {
		if (l == name) {
			return value;
		}
	}
	return std::atoi(text.c_str());
}

bool contains(const std::vector<std::string>& list, const std::string& name)
{
	const std::string l = lower(name);
	return std::any_of(list.begin(), list.end(), [&](const std::string& s) { return lower(s) == l; });
}

bool hasComponent(const world::ObjectTemplate& t, const char* name)
{
	const std::string l = lower(name);
	return std::any_of(t.components().begin(), t.components().end(), [&](const std::string& c) { return lower(c) == l; });
}

// 0x0060ec00 pushes every armored object below the root onto the root's
// list (object vtable 0x148), each after its own children (provisional:
// post-order), with its armor.fullChildSync.
void collectArmored(const GhostTemplateNode& node, std::vector<bool>& out)
{
	for (const GhostTemplateNode& child : node.children) {
		collectArmored(child, out);
		if (child.hints.hasArmor) {
			out.push_back(child.fullChildSync);
		}
	}
}

GhostTemplateNode build(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t,
	const NetworkableInfoTable& infos, bool hasParent, bool inVehicle, int ancestorEngineType,
	std::set<const world::ObjectTemplate*>& path)
{
	GhostTemplateNode node;
	node.templateId = t.id();
	node.name = t.name();
	node.type = t.typeName();
	const std::string type = lower(t.typeName());
	node.controlObject = type == "playercontrolobject" || type == "parachute";
	node.projectile = type == "genericprojectile";
	const std::string info = setting(t, "networkableInfo");
	node.unique = contains(infos.unique, info);
	node.forceNetworkableId = contains(infos.forceNetworkableId, info);
	node.hints = networkableHintsFrom(templates, t, hasParent);
	inVehicle = inVehicle || node.controlObject;
	node.hints.inVehicle = inVehicle;
	node.hints.armoredChildrenKnown = true; // filled for the root below; others' lists are empty
	node.fullChildSync = std::atoi(setting(t, "armor.fullChildSync").c_str()) != 0;
	if (node.hints.kind == NetworkableKind::Spring) {
		// 0x00604f10: an extra field when the spring's engine (the nearest
		// Engine ancestor, 0x0064f7b0) is a newCar2 one (provisional: the
		// physics node's flag 8 is assumed set for those).
		node.hints.springExtra = (ancestorEngineType & 0x80) != 0;
	}
	// 0x00678070: RotationalBundles (and Cameras) with the same rotation
	// settings share a group even when their templates differ.
	if (type == "rotationalbundle" || type == "camera") {
		node.groupKey = "rotationalbundle";
		for (const char* key : {"automaticReset", "inputToYaw", "inputToPitch", "inputToRoll", "maxRotation",
				 "minRotation", "maxSpeed", "acceleration"}) {
			node.groupKey += "|" + lower(setting(t, key));
		}
	}
	if (type == "engine") {
		ancestorEngineType = engineTypeOf(setting(t, "engineType"));
	}
	path.insert(&t);
	for (const world::ChildTemplate& child : t.children()) {
		const world::ObjectTemplate* c = templates.find(child.name);
		if (!c || path.count(c)) {
			continue;
		}
		node.children.push_back(build(templates, *c, infos, true, inVehicle, ancestorEngineType, path));
	}
	path.erase(&t);
	return node;
}

} // namespace

NetworkableHints networkableHintsFrom(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t,
	bool hasParent)
{
	NetworkableHints h;
	// Every class's slot-94 function checks the template's networkableInfo
	// (0x006fa2a0); without one the object has no networkable.
	if (setting(t, "networkableInfo").empty() && lower(t.typeName()) != "soldier") {
		return h;
	}
	h.kind = networkableKindForTemplateType(t.typeName(), hasParent);
	h.mobile = std::atoi(setting(t, "hasMobilePhysics").c_str()) != 0;
	h.hasArmor = hasComponent(t, "Armor") || !setting(t, "armor.maxHitPoints").empty();
	h.hasTeamChange = hasComponent(t, "TeamChange");
	h.parachute = lower(t.typeName()) == "parachute";
	if (const std::string slots = setting(t, "inventorySize"); !slots.empty()) {
		h.soldierItemSlots = std::atoi(slots.c_str()); // Soldier mask 0x2000 (template +0x298)
	}
	h.maxHitPoints = std::atoi(setting(t, "armor.maxHitPoints").c_str());
	h.wreckHitPoints = std::atoi(setting(t, "armor.wreckHitPoints").c_str());
	h.maxSpeed = vec3Setting(t, "maxSpeed");
	h.minRotation = vec3Setting(t, "minRotation");
	h.maxRotation = vec3Setting(t, "maxRotation");
	h.engineType = engineTypeOf(setting(t, "engineType"));
	h.engineGears = std::atoi(setting(t, "numberOfGears").c_str());
	h.maxAngleOfAttack = static_cast<float>(std::atof(setting(t, "maxAngleOfAttack").c_str()));
	h.minRpm = static_cast<float>(std::atof(setting(t, "newCar2.minRpm").c_str()));
	h.maxRpm = static_cast<float>(std::atof(setting(t, "newCar2.maxRpm").c_str()));
	if (const std::string w = setting(t, "numWindows"); !w.empty()) {
		h.windows = std::atoi(w.c_str());
	}
	h.overheadRange = static_cast<float>(std::atof(setting(t, "speed").c_str()));
	if (h.kind == NetworkableKind::PlayerControlObject) {
		// The seat's weapons (PCO interface +0x2c, provisional): the
		// GenericFireArms below it, other seats' subtrees left out.
		int weapons = 0;
		std::function<void(const world::ObjectTemplate&, int)> count = [&](const world::ObjectTemplate& at, int depth) {
			for (const world::ChildTemplate& child : at.children()) {
				const world::ObjectTemplate* c = templates.find(child.name);
				if (!c || depth > 16) {
					continue;
				}
				const std::string type = lower(c->typeName());
				if (type == "genericfirearm") {
					++weapons;
				} else if (type != "playercontrolobject" && type != "parachute") {
					count(*c, depth + 1);
				}
			}
		};
		count(t, 0);
		h.seatWeapons = weapons;
	}
	return h;
}

GhostTemplateNode ghostTemplateFrom(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t,
	const NetworkableInfoTable& infos)
{
	std::set<const world::ObjectTemplate*> path;
	GhostTemplateNode root = build(templates, t, infos, false, false, 0, path);
	collectArmored(root, root.hints.armoredChildren);
	return root;
}

} // namespace dice::hfe::net
