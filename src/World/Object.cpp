#include "Object.h"

#include <algorithm>

namespace dice::hfe::world {

bool Object::childDisabled(const std::string& path) const
{
	const CaseInsensitiveLess less;
	for (size_t start = 0; !m_disabledChildren.empty() && start <= path.size();) {
		const size_t slash = std::min(path.find('/', start), path.size());
		const std::string name = path.substr(start, slash - start);
		for (const std::string& disabled : m_disabledChildren) {
			if (!less(name, disabled) && !less(disabled, name)) {
				return true;
			}
		}
		start = slash + 1;
	}
	return false;
}

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
			++m_removals;
			return;
		}
	}
}

void ObjectManager::clear()
{
	m_removals += m_objects.size();
	m_objects.clear();
	m_byId.clear();
	m_active = nullptr;
}

} // namespace dice::hfe::world
