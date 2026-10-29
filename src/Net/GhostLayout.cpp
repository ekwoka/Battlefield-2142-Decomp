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
	std::vector<NetworkableState>& states, unsigned mode, const std::vector<MotionHistory>* history, size_t* cursor)
{
	states.clear();
	size_t local = 0;
	size_t& next = cursor ? *cursor : local;
	for (size_t g = 0; g < layout.groups.size() && g < kMaxGroups; ++g) {
		const NetworkableHints& hints = layout.groups[g].hints;
		const MotionHistory* copy = nullptr;
		if (history && hints.kind == NetworkableKind::SimpleObject) {
			if (next >= history->size()) {
				return false; // the slot keeps fewer (0x0057f7b0 fails the record)
			}
			copy = &(*history)[next++];
		}
		NetworkableState state;
		if (!decodeNetworkable(in, hints, reference, state, mode, copy) || !state.complete) {
			return false;
		}
		states.push_back(std::move(state));
	}
	return !in.bad();
}

void encodeGhostState(BitWriter& out, const GhostPartLayout& layout, const Vec3f* reference,
	const std::vector<NetworkableState>& states, unsigned mode, const std::vector<MotionHistory>* history, size_t* cursor)
{
	size_t local = 0;
	size_t& next = cursor ? *cursor : local;
	for (size_t g = 0; g < layout.groups.size() && g < kMaxGroups; ++g) {
		const NetworkableHints& hints = layout.groups[g].hints;
		const MotionHistory* copy = nullptr;
		if (history && hints.kind == NetworkableKind::SimpleObject && next < history->size()) {
			copy = &(*history)[next++];
		}
		NetworkableState empty;
		empty.kind = hints.kind;
		encodeNetworkable(out, hints, reference, g < states.size() ? states[g] : empty, mode, copy);
	}
}

bool decodeControlState(BitReader& in, const GhostPartLayout& ghost, const GhostPartLayout* second,
	const Vec3f& reference, const PlayerOwnStateHints& own, ControlState& out,
	const std::array<std::vector<MotionHistory>, 8>* slots)
{
	out = {};
	out.recorded = in.read(1) == 1;
	out.slot = static_cast<int>(in.read(3));
	const std::vector<MotionHistory>* history = nullptr;
	if (out.recorded) {
		if (!slots) {
			return false;
		}
		history = &(*slots)[static_cast<size_t>(out.slot) & 7];
	}
	size_t cursor = 0;
	if (!decodeGhostState(in, ghost, reference, out.groups, 3, history, &cursor)) {
		return false;
	}
	if (second && !decodeGhostState(in, *second, reference, out.second, 3, history, &cursor)) {
		return false;
	}
	out.ownBits = in.read(10);
	const size_t start = in.position();
	out.hasOwn = decodePlayerOwnState(in, own, out.own);
	if (in.bad() || in.position() > start + out.ownBits) {
		out.hasOwn = false; // 0x00580cd0: more than the length is an error
		return false;
	}
	in.seek(start + out.ownBits); // fewer: the rest is skipped (0x006873e0)
	out.complete = !in.bad();
	return out.complete;
}

void encodeControlState(BitWriter& out, const GhostPartLayout& ghost, const GhostPartLayout* second,
	const Vec3f* reference, const PlayerOwnStateHints& own, const ControlState& state,
	const std::vector<MotionHistory>* history)
{
	out.write(state.recorded ? 1 : 0, 1);
	out.write(static_cast<uint32_t>(state.slot) & 7, 3);
	const std::vector<MotionHistory>* against = state.recorded ? history : nullptr;
	size_t cursor = 0;
	encodeGhostState(out, ghost, reference, state.groups, 3, against, &cursor);
	if (second) {
		encodeGhostState(out, *second, reference, state.second, 3, against, &cursor);
	}
	// 0x00580830: 10 bits reserved (0x006873e0), the player's vtable 0x1c8,
	// then the length written back.
	BitWriter body;
	if (state.hasOwn) {
		encodePlayerOwnState(body, own, state.own);
	}
	out.write(static_cast<uint32_t>(std::min<size_t>(body.bits(), 0x3ff)), 10);
	out.append(body);
}

std::vector<MotionHistory> motionHistoryOf(const ControlState& state)
{
	std::vector<MotionHistory> copies;
	for (const std::vector<NetworkableState>* groups : {&state.groups, &state.second}) {
		for (const NetworkableState& s : *groups) {
			if (s.kind == NetworkableKind::SimpleObject) {
				copies.push_back({s.position, s.velocity, s.angularVelocity});
			}
		}
	}
	return copies;
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

// ObjectTemplate.physicsType (template +0xf4; 0x006bf540 / 0x006bf5d0
// take None 0, Point 1, RotationalPoint 2, Mesh 3, MechLegs 4 or a digit;
// 0 by default, 0x005f7d90). hasPointPhysics, hasRotationalPointPhysics and
// hasResponsePhysics set it too (0x005f6e20: 1, 2, 3).
int physicsTypeOf(const world::ObjectTemplate& t)
{
	const std::string type = lower(setting(t, "physicsType"));
	if (!type.empty()) {
		static const char* const kNames[] = {"none", "point", "rotationalpoint", "mesh", "mechlegs"};
		for (int i = 0; i < 5; ++i) {
			if (type == kNames[i]) {
				return i;
			}
		}
		return std::atoi(type.c_str());
	}
	if (std::atoi(setting(t, "hasResponsePhysics").c_str()) != 0) {
		return 3;
	}
	if (std::atoi(setting(t, "hasRotationalPointPhysics").c_str()) != 0) {
		return 2;
	}
	if (std::atoi(setting(t, "hasPointPhysics").c_str()) != 0) {
		return 1;
	}
	return 0;
}

bool contains(const std::vector<std::string>& list, const std::string& name)
{
	const std::string l = lower(name);
	return std::any_of(list.begin(), list.end(), [&](const std::string& s) { return lower(s) == l; });
}

} // namespace

float NetworkableInfoTable::basePriority(const std::string& info) const
{
	const auto it = infos.find(lower(info));
	return it != infos.end() ? it->second.basePriority : 1.0f;
}

NetworkableInfoTable& networkableInfos()
{
	static NetworkableInfoTable table;
	return table;
}

namespace {

bool hasComponent(const world::ObjectTemplate& t, const char* name)
{
	const std::string l = lower(name);
	return std::any_of(t.components().begin(), t.components().end(), [&](const std::string& c) { return lower(c) == l; });
}

// 0x0060ec00 (template slot 64, run first by the object init 0x0063d720)
// makes each child of the template's list (+0x248, in addTemplate order),
// runs the child's init (which makes the grandchildren) and only then
// pushes it onto the root's list (object vtable 0x148) when it has armor
// (+0x48): post-order, children in template order, nested seats included,
// the root left out. Each with its armor.fullChildSync.
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
		// Engine ancestor, 0x0064f7b0; engine vtable 0x1a8 < 0) is a newCar2
		// one and the spring's physics (object +0x54, vtable 0x7c) has flag 8:
		// only ResponsePhysics keeps flags (0x006ba350), set from the
		// template's grip (+0xfc), and 8 is EngineGrip.
		node.hints.springExtra = (ancestorEngineType & 0x80) != 0 && (node.hints.responseGrip & 8) != 0;
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
	const bool collisionPhysics = std::atoi(setting(t, "hasCollisionPhysics").c_str()) != 0; // template +0xf1 bit 4
	const bool mobilePhysics = std::atoi(setting(t, "hasMobilePhysics").c_str()) != 0;       // template +0xf1 bit 2
	const int physicsType = physicsTypeOf(t);
	h.kind = networkableKindForTemplateType(t.typeName(), hasParent, mobilePhysics);
	// The physics node (object +0x50, made by template slot 29, 0x005f6ba0):
	// with collision and mobile physics, Point / RotationalPoint / Mesh give
	// nodes that move (0x006c5e70, 0x006c6480, 0x006c70c0: vtable 0x164
	// 0x006a10e0 returns 1); otherwise none, or Mesh's static one
	// (0x006c5500, whose 0x164 returns 0).
	h.mobile = collisionPhysics && mobilePhysics && physicsType >= 1 && physicsType <= 3;
	// The other physics (object +0x54, template slot 31, 0x005f6a60): with
	// collision and mobile physics, Mesh gives ResponsePhysics (0x006ba0d0),
	// whose flags (vtable 0x7c / 0x80) are the template's grip (2 by
	// default: ContactGrip; 8 EngineGrip, 0x80 EngineDummyGrip, 0x0051aed0).
	if (collisionPhysics && mobilePhysics && physicsType == 3) {
		const std::string grip = setting(t, "grip");
		h.responseGrip = grip.empty() ? 2 : std::atoi(grip.c_str());
	}
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
		// 0x00605b00 asks the seat's PCO interface (vtable 0x0096dcb8) slot
		// 0x40 (0x0058cc60) for a map whose size (+0x2c) picks the layout.
		// The root PCO's init (0x005cd4f0 -> 0x0058dde0) fills each seat's
		// with 0x0058bdb0: a walk of the seat's whole subtree, nested seats
		// included, taking the objects answering 0xc367 whose pcoId
		// (slot 0x48) is the seat's: Camera, FreeCamera and OverheadCamera.
		// A seat's pcoId is ObjectTemplate.setPcoId (+0x3a0, -1 by default)
		// and a camera's falls back to its nearest PCO's; the shipped data
		// sets neither, so every camera below the seat counts.
		int cameras = 0;
		std::function<void(const world::ObjectTemplate&, int)> count = [&](const world::ObjectTemplate& at, int depth) {
			for (const world::ChildTemplate& child : at.children()) {
				const world::ObjectTemplate* c = templates.find(child.name);
				if (!c || depth > 16) {
					continue;
				}
				const std::string type = lower(c->typeName());
				if (type == "camera" || type == "freecamera" || type == "overheadcamera") {
					++cameras;
				}
				count(*c, depth + 1);
			}
		};
		count(t, 0);
		h.seatCameras = cameras;
	}
	return h;
}

bool firearmSendsHeat(const world::ObjectTemplate& firearm)
{
	// The template's +0x2c8 is ObjectTemplate.maxResetSpeed's (0x00521fab:
	// the setter 0x005220a3 -> 0x00522125, the only writer), whatever the
	// class: 0x005c1360 tests its y.
	const std::string key = !setting(firearm, "maxResetSpeed").empty() ? "maxResetSpeed" : "setMaxResetSpeed";
	return vec3Setting(firearm, key.c_str()).y > 0;
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
