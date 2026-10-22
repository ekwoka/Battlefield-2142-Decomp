#include "ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>

namespace dice::hfe::world {

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const
{
	return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
	});
}

ObjectTemplate::ObjectTemplate(uint32_t classId, std::string typeName, std::string name, uint32_t id)
	: m_classId(classId), m_typeName(std::move(typeName)), m_name(std::move(name)), m_id(id)
{
}

const std::vector<TemplateTypeInfo>& engineTemplateTypes()
{
	// Registered by the world modules' init functions (e.g. 0x0051a8c4,
	// 0x004da65b) through ObjectTemplateManager slot 0x14.
	static const std::vector<TemplateTypeInfo> types = {
		{0x9471, "Bundle"},
		{0x9472, "Camera"},
		{0x9474, "EffectBundle"},
		{0x9475, "Emitter"},
		{0x9476, "Engine"},
		{0x9477, "EntryPoint"},
		{0x9478, "FloatingBundle"},
		{0x947b, "Particle"},
		{0x947d, "RotationalBundle"},
		{0x947e, "SimpleObject"},
		{0x9480, "SpriteParticle"},
		{0x9481, "Spring"},
		{0x9482, "MechLegs"},
		{0x9483, "Wing"},
		{0x9484, "WheelEffectBundle"},
		{0x9485, "AirDraftEffectBundle"},
		{0x9486, "GroundEffectBundle"},
		{0x9487, "Decal"},
		{0x9488, "EnvMap"},
		{0x9489, "ForceObject"},
		{0x948a, "SpawnPoint"},
		{0x948b, "FreeCamera"},
		{0x948c, "OverheadCamera"},
		{0x948d, "LookAtObject"},
		{0x9492, "Item"},
		{0x9493, "Soldier"},
		{0x9496, "Kit"},
		{0x9499, "ItemContainer"},
		{0x949a, "Flag"},
		{0x949c, "AnimatedBundle"},
		{0x949d, "AntennaObject"},
		{0x94a1, "ObjectSpawner"},
		{0x94a2, "FlagBase"},
		{0x94a3, "KitPart"},
		{0x94a4, "SupplyDepot"},
		{0x94a5, "ControlPoint"},
		{0x94ae, "LandingGear"},
		{0x94af, "Obstacle"},
		{0x94b3, "AreaObject"},
		{0x94b6, "DestroyTargetObjectiveTemplate"},
		{0x94b8, "ANDCompositeObjectiveTemplate"},
		{0x94bb, "TimerObjectiveTemplate"},
		{0x94be, "TargetObject"},
		{0x94c7, "LightSource"},
		{0x94cb, "Rotor"},
		{0x94cf, "SpriteParticleSystem"},
		{0x94d0, "MeshParticleSystem"},
		{0x94d1, "TrailSystem"},
		{0x94d2, "ParticleSystemEmitter"},
		{0x94d3, "GenericFireArm"},
		{0x9537, "NonScreenAlignedParticleSystem"},
		{0x9539, "GenericProjectile"},
		{0x9c40, "Ladder"}, // registered as decimal 40000
		{0x9c41, "DynamicBundle"},
		{0x9c42, "RemoteControlledObject"},
		{0x9c43, "TurnableRemoteControlledObject"},
		{0x9c44, "AmbientEffectArea"},
		{0x9c45, "Trigger"},
		{0x9c46, "LadderContainer"},
		{0x9c47, "SupplyObject"},
		{0x9c52, "UAVVehicle"},
		{0xc4c2, "PlayerControlObject"},
		{0xc4d0, "DestroyableObject"},
		{0xc4dc, "DestroyableWindowsBundle"},
		{0xc4dd, "DestroyableWindow"},
		{0xc5a8, "Parachute"},
		// Registered by the sound module (0x007f2xxx); Ghidra shows the ID
		// as a pointer into .rdata.
		{0x985fd1, "Sound"},
	};
	return types;
}

ObjectTemplateManager::ObjectTemplateManager()
{
	for (const TemplateTypeInfo& type : engineTemplateTypes()) {
		registerType(type.classId, type.name);
	}
}

void ObjectTemplateManager::registerType(uint32_t classId, const std::string& typeName, TemplateFactory factory)
{
	m_typeIds[typeName] = classId;
	m_typeNames[classId] = typeName;
	if (factory) {
		m_factories[classId] = std::move(factory);
	}
}

uint32_t ObjectTemplateManager::classIdOf(const std::string& typeName) const
{
	const auto it = m_typeIds.find(typeName);
	return it == m_typeIds.end() ? 0 : it->second;
}

const std::string& ObjectTemplateManager::typeNameOf(uint32_t classId) const
{
	static const std::string unknown = "UnknownTemplateClass";
	const auto it = m_typeNames.find(classId);
	return it == m_typeNames.end() ? unknown : it->second;
}

ObjectTemplate* ObjectTemplateManager::create(const std::string& typeName, const std::string& name)
{
	return create(classIdOf(typeName), name);
}

ObjectTemplate* ObjectTemplateManager::create(uint32_t classId, const std::string& name)
{
	if (ObjectTemplate* existing = find(name)) {
		m_active = existing;
		return existing;
	}
	if (classId == 0 || m_typeNames.count(classId) == 0) {
		m_active = nullptr; // the class manager can't create it
		return nullptr;
	}
	uint32_t flags = m_loading ? TemplateFlags::Loading : 0;
	const uint32_t id = (flags & TemplateFlags::ClientOnly) ? m_nextClientOnlyId++ : m_nextId++;
	const auto factory = m_factories.find(classId);
	std::unique_ptr<ObjectTemplate> objectTemplate = factory != m_factories.end()
		? factory->second(classId, typeNameOf(classId), name, id)
		: std::make_unique<ObjectTemplate>(classId, typeNameOf(classId), name, id);
	objectTemplate->setFlags(flags);
	if (m_currentFile) {
		objectTemplate->setCreationFile(m_currentFile());
	}
	ObjectTemplate* created = objectTemplate.get();
	m_byName[name] = created;
	m_byId[id] = created;
	m_templates.push_back(std::move(objectTemplate));
	for (const auto& listener : m_creationListeners) {
		listener(*created); // may create further templates
	}
	m_active = created;
	return created;
}

void ObjectTemplateManager::addComponent(ObjectTemplate& owner, const std::string& component)
{
	owner.components().push_back(component);
	ObjectTemplate* active = m_active;
	for (const auto& listener : m_componentListeners) {
		listener(owner, component);
	}
	m_active = active; // components that create templates restore the active one
}

ObjectTemplate* ObjectTemplateManager::find(const std::string& name) const
{
	if (name.empty()) {
		return nullptr;
	}
	const auto it = m_byName.find(name);
	return it == m_byName.end() ? nullptr : it->second;
}

ObjectTemplate* ObjectTemplateManager::findById(uint32_t id) const
{
	const auto it = m_byId.find(id);
	return it == m_byId.end() ? nullptr : it->second;
}

void ObjectTemplateManager::destroy(ObjectTemplate* objectTemplate)
{
	if (!objectTemplate) {
		return;
	}
	if (m_active == objectTemplate) {
		m_active = nullptr;
	}
	m_byName.erase(objectTemplate->name());
	m_byId.erase(objectTemplate->id());
	m_templates.erase(std::remove_if(m_templates.begin(), m_templates.end(),
						  [&](const auto& t) { return t.get() == objectTemplate; }),
		m_templates.end());
}

} // namespace dice::hfe::world
