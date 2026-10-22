#include "Object.h"

namespace dice::hfe::world {

Object* ObjectManager::create(ObjectTemplate* objectTemplate, const Vec3& position, const Vec3& rotation)
{
	if (!objectTemplate) {
		m_active = nullptr;
		return nullptr;
	}
	auto object = std::make_unique<Object>(m_nextId++, objectTemplate);
	object->setPosition(position);
	object->setRotation(rotation);
	m_active = object.get();
	m_byId[m_active->id()] = m_active;
	m_objects.push_back(std::move(object));
	return m_active;
}

Object* ObjectManager::find(uint32_t id) const
{
	const auto it = m_byId.find(id);
	return it == m_byId.end() ? nullptr : it->second;
}

void ObjectManager::remove(uint32_t id)
{
	for (auto it = m_objects.begin(); it != m_objects.end(); ++it) {
		if ((*it)->id() == id) {
			if (m_active == it->get()) {
				m_active = nullptr;
			}
			m_byId.erase(id);
			m_objects.erase(it);
			return;
		}
	}
}

void ObjectManager::clear()
{
	m_objects.clear();
	m_byId.clear();
	m_active = nullptr;
}

} // namespace dice::hfe::world
