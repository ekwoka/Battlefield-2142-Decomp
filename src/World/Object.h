// Objects placed in the world: instances of object templates.
//
// Level scripts place static objects with
//     Object.create <template>
//     Object.absolutePosition x/y/z
//     Object.rotation yaw/pitch/roll
// which create the object and then modify the "active" one.
#pragma once

#include "ObjectTemplate.h"
#include "WorldMath.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dice::hfe::world {

class Object {
public:
	Object(uint32_t id, ObjectTemplate* objectTemplate) : m_id(id), m_template(objectTemplate) {}

	uint32_t id() const { return m_id; }
	ObjectTemplate* objectTemplate() const { return m_template; }

	const Vec3& position() const { return m_position; }
	void setPosition(const Vec3& position) { m_position = position; }
	const Vec3& rotation() const { return m_rotation; }
	void setRotation(const Vec3& rotation) { m_rotation = rotation; }

	const std::string& name() const { return m_name; }
	void setName(std::string name) { m_name = std::move(name); }

	// A mesh of the object's own instead of its template's geometry
	// ("Object.geometry.loadMesh"; compiled roads).
	const std::string& mesh() const { return m_mesh; }
	void setMesh(std::string path) { m_mesh = std::move(path); }

	// 0 neutral, 1 and 2 the two armies.
	int team() const { return m_team; }
	void setTeam(int team) { m_team = team; }
	// The control point an object belongs to ("Object.setControlPointId";
	// spawners spawn for its owner), -1 for none.
	int controlPointId() const { return m_controlPointId; }
	void setControlPointId(int id) { m_controlPointId = id; }
	// Destroyed objects stay until removed (wrecks).
	bool destroyed() const { return m_destroyed; }
	void setDestroyed(bool destroyed) { m_destroyed = destroyed; }
	// Not drawn (a soldier inside a vehicle).
	bool hidden() const { return m_hidden; }
	void setHidden(bool hidden) { m_hidden = hidden; }

private:
	uint32_t m_id;
	ObjectTemplate* m_template;
	Vec3 m_position;
	Vec3 m_rotation; // yaw/pitch/roll in degrees
	std::string m_name;
	std::string m_mesh;
	int m_team = 0;
	int m_controlPointId = -1;
	bool m_destroyed = false;
	bool m_hidden = false;
};

class ObjectManager {
public:
	Object* create(ObjectTemplate* objectTemplate, const Vec3& position = {}, const Vec3& rotation = {});
	Object* active() const { return m_active; }
	void setActive(Object* object) { m_active = object; }

	const std::vector<std::unique_ptr<Object>>& objects() const { return m_objects; }
	// ObjectManager slot 0x30: the object with an id, or null.
	Object* find(uint32_t id) const;
	void remove(uint32_t id);
	void clear();

private:
	std::vector<std::unique_ptr<Object>> m_objects;
	std::unordered_map<uint32_t, Object*> m_byId; // find's index
	Object* m_active = nullptr;
	uint32_t m_nextId = 1;
};

} // namespace dice::hfe::world
