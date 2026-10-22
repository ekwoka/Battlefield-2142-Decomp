// Creates and looks up object templates ("dice.hfe.world.ObjectTemplateManager.Standard",
// vtable 0x0099b080 in 1.51).
//
// Each template type registers its class ID under a name; scripts create
// templates by type name ("ObjectTemplate.create SimpleObject rock_01") and
// then set properties on the "active" template, which is the one created
// or activated last.
#pragma once

#include "ObjectTemplate.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::world {

// Creates the template object for a type. Types without one get a plain
// ObjectTemplate.
using TemplateFactory = std::function<std::unique_ptr<ObjectTemplate>(uint32_t classId, const std::string& typeName,
	const std::string& name, uint32_t id)>;

class ObjectTemplateManager {
public:
	ObjectTemplateManager();

	// Registers the types the executable defines (and ones from other
	// modules, e.g. the renderer's particle and sound types). Registering a
	// type again replaces its factory.
	void registerType(uint32_t classId, const std::string& typeName, TemplateFactory factory = {});
	// 0 if unknown. Names are matched exactly, ignoring case.
	uint32_t classIdOf(const std::string& typeName) const;
	const std::string& typeNameOf(uint32_t classId) const;

	// "ObjectTemplate.create type name". An existing template of that name
	// is activated and returned unchanged, whatever its type. An unknown
	// type creates nothing and clears the active template.
	ObjectTemplate* create(const std::string& typeName, const std::string& name);
	ObjectTemplate* create(uint32_t classId, const std::string& name);

	ObjectTemplate* find(const std::string& name) const;
	ObjectTemplate* findById(uint32_t id) const;
	void destroy(ObjectTemplate* objectTemplate);

	ObjectTemplate* active() const { return m_active; }
	void setActive(ObjectTemplate* objectTemplate) { m_active = objectTemplate; }

	// Templates created while loading a level are flagged so they can be
	// unloaded with it.
	void setLoading(bool loading) { m_loading = loading; }

	// Called for each new template before it becomes active, in place of
	// the original's per-class initialisation (vtable slot 6), e.g. to
	// create default child templates.
	void addCreationListener(std::function<void(ObjectTemplate&)> listener)
	{
		m_creationListeners.push_back(std::move(listener));
	}

	// "ObjectTemplate.createComponent <name>" on the active template; the
	// listeners run as the component's creation (e.g. sound components create
	// their sounds).
	void addComponent(ObjectTemplate& owner, const std::string& component);
	void addComponentListener(std::function<void(ObjectTemplate&, const std::string&)> listener)
	{
		m_componentListeners.push_back(std::move(listener));
	}

	// The script being run, recorded as each template's creation file.
	void setCurrentFileProvider(std::function<std::string()> provider) { m_currentFile = std::move(provider); }

	size_t count() const { return m_templates.size(); }
	const std::vector<std::unique_ptr<ObjectTemplate>>& templates() const { return m_templates; }

private:
	std::map<std::string, uint32_t, CaseInsensitiveLess> m_typeIds;
	std::map<uint32_t, std::string> m_typeNames;
	std::map<uint32_t, TemplateFactory> m_factories;
	std::vector<std::unique_ptr<ObjectTemplate>> m_templates; // creation order
	std::map<std::string, ObjectTemplate*, CaseInsensitiveLess> m_byName;
	std::map<uint32_t, ObjectTemplate*> m_byId;
	ObjectTemplate* m_active = nullptr;
	uint32_t m_nextId = 0;           // network-shared templates
	uint32_t m_nextClientOnlyId = 0; // client-only templates
	bool m_loading = false;
	std::function<std::string()> m_currentFile;
	std::vector<std::function<void(ObjectTemplate&)>> m_creationListeners;
	std::vector<std::function<void(ObjectTemplate&, const std::string&)>> m_componentListeners;
};

// The template types BF2142.exe registers, with their class IDs
// (symbols/BF2142.exe.templates.csv).
struct TemplateTypeInfo {
	uint32_t classId;
	const char* name;
};
const std::vector<TemplateTypeInfo>& engineTemplateTypes();

} // namespace dice::hfe::world
