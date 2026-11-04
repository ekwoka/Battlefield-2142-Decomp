#include "HudMenusComm.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameWorld.h"
#include "Hud/Hud.h"
#include "HudWorld.h"
#include "World/Object.h"
#include "World/ObjectTemplate.h"

#include <cmath>
#include <utility>

namespace dice::hfe::menu {

CommRose::CommRose(hud::HudManager& hud, std::string name, int entries)
	: m_hud(hud), m_name(std::move(name)), m_entries(entries)
{
}

// 0x007b8310: an empty key or text sets nothing; the text is kept
// localised, the command as given (only when not empty).
void CommRose::setStringMap(const std::string& key, const std::string& text, const std::string& command)
{
	if (key.empty() || text.empty()) {
		return;
	}
	m_texts[key] = m_localize ? m_localize(text) : text;
	if (!command.empty()) {
		m_commands[key] = command;
	}
}

void CommRose::setVariableMap(const std::string& key, const std::string& value)
{
	if (!key.empty()) {
		m_variables[key] = value;
	}
}

std::string CommRose::key(const std::string& path, int entry) const
{
	return "Interface_" + path + ":" + std::to_string(entry);
}

std::string CommRose::text(int entry) const
{
	const auto it = m_texts.find(key(m_path, entry));
	return it != m_texts.end() ? it->second : std::string();
}

std::string CommRose::command(int entry) const
{
	const auto it = m_commands.find(key(m_path, entry));
	return it != m_commands.end() ? it->second : std::string();
}

void CommRose::showLevel(const std::string& path)
{
	for (int i = 1; i <= m_entries; ++i) {
		const std::string k = key(path, i);
		const auto t = m_texts.find(k);
		const auto v = m_variables.find(k);
		m_hud.setString(m_name + std::to_string(i) + "String", t != m_texts.end() ? t->second : std::string());
		m_hud.setString(m_name + std::to_string(i) + "Variable", v != m_variables.end() ? v->second : std::string());
	}
}

// 0x007b7130. Its last line clears "Radio%iMoseOver" (a slip in the name,
// after the counter went up: Radio2MoseOver..Radio<n+1>MoseOver), so the
// HUD's Radio<n>MouseOver variables keep what the last mouseOver set; the
// rewrite does the same.
void CommRose::reset()
{
	m_path = "1";
	m_open = false;
	showLevel(m_path);
	for (int i = 1; i <= m_entries; ++i) {
		m_hud.set("Radio" + std::to_string(i + 1) + "MoseOver", 0);
	}
}

// 0x007b73d0.
std::string CommRose::selectOrder(const std::string& entry)
{
	const std::string here = m_path + ":" + entry;
	bool deeper = false;
	for (int i = 1; i <= m_entries; ++i) {
		deeper = deeper || m_texts.count(key(here, i)) != 0;
	}
	if (deeper) {
		m_path = here;
		showLevel(m_path);
		return {};
	}
	const auto it = m_commands.find("Interface_" + here);
	m_open = false;
	return it != m_commands.end() ? it->second : std::string();
}

CommRoses::CommRoses(hud::HudManager& hud) : m_hud(hud)
{
	// 0x0075e1f0's names and counts.
	for (const auto& [name, entries] : std::vector<std::pair<const char*, int>> {{"Radio", 11}, {"RadioVehicle", 11},
			 {"SquadLeader", 11}, {"Squad", 11}, {"Lonewolf", 0}, {"Commander", 11}, {"DemoCamera", 3},
			 {"DemoRecorder", 9}, {"Camera", 3}}) {
		m_roses.emplace_back(hud, name, entries);
	}
}

void CommRoses::setLocalizer(const std::function<std::string(const std::string&)>& localize)
{
	for (CommRose& rose : m_roses) {
		rose.setLocalizer(localize);
	}
}

CommRose* CommRoses::find(const std::string& name)
{
	for (CommRose& rose : m_roses) {
		if (rose.name() == name) {
			return &rose;
		}
	}
	return nullptr;
}

void CommRoses::bind(Console& console, std::function<void(const std::string&)> run)
{
	m_run = std::move(run);
	const auto access = ConsoleAccess::Anyone;
	for (CommRose& rose : m_roses) {
		CommRose* r = &rose;
		const std::string object = rose.name() + "Interface";
		bindMethod(console, object + ".setStringMap", access, [r](std::string key, std::string text, std::string command) {
			r->setStringMap(key, text, command);
		});
		bindMethod(console, object + ".setVariableMap", access, [r](std::string key, std::string text, std::string value) {
			(void)text;
			r->setVariableMap(key, value);
		});
		bindMethod(console, object + ".selectOrder", access, [this, r](std::string entry) {
			const std::string line = r->selectOrder(entry);
			if (!r->open() && r->name() == m_shown) {
				close();
			}
			if (!line.empty() && m_run) {
				m_run(line);
			}
			return true;
		});
	}
	// Every rose's buttons call the radio interface's (0x007b70b0).
	bindMethod(console, "RadioInterface.mouseOver", access, [this](int entry) {
		const CommRose* r = find("Radio");
		for (int i = 1; r && i <= r->entries(); ++i) {
			m_hud.set("Radio" + std::to_string(i) + "MouseOver", i == entry ? 1.0f : 0.0f);
		}
		return true;
	});
	bindMethod(console, "Radio.setSpottedMenuActive", access, [this] { openSpotted(); });
}

void CommRoses::show(const std::string& name)
{
	m_shown = name;
	for (const CommRose& rose : m_roses) {
		const std::string variable = rose.name() == "DemoRecorder" ? "DemoRecInterfaceShow" : rose.name() + "InterfaceShow";
		m_hud.set(variable, rose.name() == name ? 1.0f : 0.0f);
	}
	m_hud.set("SpottedInterfaceShow", name == "Spotted" ? 1.0f : 0.0f);
}

void CommRoses::openRadio(bool inVehicle)
{
	CommRose* rose = find(inVehicle ? "RadioVehicle" : "Radio");
	rose->reset();
	rose->setOpen(true);
	show(rose->name());
}

void CommRoses::openTactical(bool commander, bool squadLeader, bool inSquad)
{
	CommRose* rose = find(commander ? "Commander" : squadLeader ? "SquadLeader" : inSquad ? "Squad" : "Lonewolf");
	rose->reset();
	rose->setOpen(true);
	show(rose->name());
}

void CommRoses::openSpotted()
{
	show("Spotted");
}

void CommRoses::close()
{
	for (CommRose& rose : m_roses) {
		rose.setOpen(false);
	}
	show("");
}

std::string spottedMessage(const world::ObjectTemplate* t)
{
	if (!t) {
		return {};
	}
	const auto it = t->settings().find("radio.spottedMessage");
	if (it == t->settings().end()) {
		return {};
	}
	std::string name = it->second;
	if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
		name = name.substr(1, name.size() - 2);
	}
	return name;
}

SpotChoice spotChoice(const host::HudWorld& world, game::GameWorld& round, int enemy)
{
	SpotChoice out;
	game::GameWorld::PlayerInfo e;
	if (enemy < 0 || !world.playerInfo(enemy, e)) {
		return out;
	}
	out.player = enemy;
	out.object = e.soldier;
	const auto templateOf = [&round](uint32_t id) -> const world::ObjectTemplate* {
		const world::Object* o = id ? round.objects().find(id) : nullptr;
		return o ? o->objectTemplate() : nullptr;
	};
	const world::ObjectTemplate* kit = templateOf(e.kit);
	const auto setting = [](const world::ObjectTemplate* t, const char* key, bool otherwise) {
		if (!t) {
			return otherwise;
		}
		const auto it = t->settings().find(key);
		return it == t->settings().end() ? otherwise : std::atof(it->second.c_str()) != 0;
	};
	if (const world::ObjectTemplate* vehicle = templateOf(e.vehicle)) {
		// 0x0075bfe0: the root only when canBeSpotted; 0x0075be30: its
		// rider's soldier with usePlayerIcon.
		if (setting(vehicle, "vehicleHud.canBeSpotted", true) && !setting(vehicle, "vehicleHud.usePlayerIcon", false)) {
			out.object = e.vehicle;
			// 0x0042e9d0: the vehicle's own Radio component.
			out.voice = spottedMessage(vehicle);
			if (out.voice.empty()) {
				out.voice = "spotted";
			}
			return out;
		}
	}
	// A soldier: its kit's Radio component (else the soldier's own).
	out.voice = spottedMessage(kit);
	if (out.voice.empty()) {
		out.voice = spottedMessage(templateOf(e.soldier));
	}
	if (out.voice.empty()) {
		out.voice = "spotted";
	}
	return out;
}

int crosshairEnemy(const host::HudWorld& world, game::GameWorld& round, int viewer, const float eye[3], const float dir[3], float reach)
{
	game::GameWorld::PlayerInfo me;
	if (!world.playerInfo(viewer, me) || !me.alive) {
		return -1;
	}
	const physics::Vec3 from {eye[0], eye[1], eye[2]};
	const physics::Vec3 along {dir[0], dir[1], dir[2]};
	int found = -1;
	float best = reach;
	for (const game::GameWorld::PlayerInfo& p : world.playerList()) {
		if (!p.alive || p.id == me.id || p.team == me.team) {
			continue;
		}
		// The body's centre (the feet + 1 m, as PlayerInfo has them).
		physics::Vec3 centre = p.position + physics::Vec3 {0, 1.0f, 0};
		float radius = 0.6f;
		if (p.vehicle) {
			if (const world::Object* v = round.objects().find(p.vehicle)) {
				centre = {v->position().x, v->position().y, v->position().z};
				radius = 2.5f;
			}
		}
		const physics::Vec3 d = centre - from;
		const float t = physics::dot(d, along);
		if (t <= 0 || t >= best || physics::lengthSquared(d) - t * t > radius * radius) {
			continue;
		}
		if (!round.lineOfSight(from, centre, p.vehicle ? p.vehicle : me.vehicle)) {
			continue;
		}
		best = t;
		found = p.id;
	}
	return found;
}

} // namespace dice::hfe::menu
