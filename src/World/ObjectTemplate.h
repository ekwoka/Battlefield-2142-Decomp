// Object templates: the definitions scripts build with "ObjectTemplate.*".
//
// Every placeable thing (a soldier, a vehicle, a rock, a particle emitter)
// is an object template of one of the registered types ("SimpleObject",
// "PlayerControlObject", ...). Templates form trees: "addTemplate" adds a
// child at a position and rotation relative to the parent. Objects in the
// world ("Object.create") are instances of a template.
//
// In the original every type is its own C++ class with typed fields, and a
// property such as "ObjectTemplate.mass" casts the active template to its
// class (see symbols/BF2142.exe.template_properties.csv for the map). The
// rewrite types properties as the subsystems that use them are rewritten;
// until then a property's last value is kept as text in `settings`.
#pragma once

#include "WorldMath.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::world {

class ObjectTemplate;

// A child template inside a parent (e.g. a wheel of a vehicle).
struct ChildTemplate {
	std::string name; // resolved lazily: the child may be defined later
	Vec3 position;
	Vec3 rotation; // yaw/pitch/roll in degrees
};

struct CaseInsensitiveLess {
	bool operator()(const std::string& a, const std::string& b) const;
};

// Template flags (IObjectTemplate::flags, vtable slot 0x44).
namespace TemplateFlags {
constexpr uint32_t Loading = 0x4000;    // created while loading a level
constexpr uint32_t ClientOnly = 0x2000; // numbered separately from network-shared templates
} // namespace TemplateFlags

class ObjectTemplate {
public:
	ObjectTemplate(uint32_t classId, std::string typeName, std::string name, uint32_t id);
	virtual ~ObjectTemplate() = default;

	uint32_t classId() const { return m_classId; }
	const std::string& typeName() const { return m_typeName; }
	const std::string& name() const { return m_name; }
	uint32_t id() const { return m_id; }
	uint32_t flags() const { return m_flags; }
	void setFlags(uint32_t flags) { m_flags = flags; }

	// "geometry": the mesh (a GeometryTemplate name) drawn for this template.
	const std::string& geometry() const { return m_geometry; }
	void setGeometry(std::string name) { m_geometry = std::move(name); }

	std::vector<ChildTemplate>& children() { return m_children; }
	const std::vector<ChildTemplate>& children() const { return m_children; }

	// Components added with "ObjectTemplate.createComponent" (e.g.
	// "DefaultSoundComp", "Armor"), in order. Their properties ("armor.*")
	// are kept with the untyped settings for now.
	std::vector<std::string>& components() { return m_components; }
	const std::vector<std::string>& components() const { return m_components; }

	// Untyped properties: name (without "ObjectTemplate.") -> last value.
	std::map<std::string, std::string, CaseInsensitiveLess>& settings() { return m_settings; }
	const std::map<std::string, std::string, CaseInsensitiveLess>& settings() const { return m_settings; }

	// Script file that created the template (for "ObjectTemplate.fileName"-style queries).
	const std::string& creationFile() const { return m_creationFile; }
	void setCreationFile(std::string file) { m_creationFile = std::move(file); }

private:
	uint32_t m_classId;
	std::string m_typeName;
	std::string m_name;
	uint32_t m_id;
	uint32_t m_flags = 0;
	std::string m_geometry;
	std::vector<ChildTemplate> m_children;
	std::vector<std::string> m_components;
	std::map<std::string, std::string, CaseInsensitiveLess> m_settings;
	std::string m_creationFile;
};

} // namespace dice::hfe::world
