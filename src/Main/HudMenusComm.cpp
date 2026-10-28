#include "HudMenusComm.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Hud/Hud.h"

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

} // namespace dice::hfe::menu
