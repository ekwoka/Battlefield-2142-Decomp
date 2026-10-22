#include "Object.h"

#include "Interpreter.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::ui::avm1 {

namespace {

// Prototype chains longer than this are treated as cycles.
constexpr int kMaxChain = 256;

ObjectRef refOf(Object* o) { return o ? o->shared_from_this() : nullptr; }

} // namespace

// --- PropertyMap

// Names are matched without regard to case, as DICE's gameswf did (the
// menus write `_root.UpdateManager1` for `updateManager1`); each keeps the
// spelling it was first given.
std::string PropertyMap::key(const std::string& name)
{
	std::string lower = name;
	for (char& c : lower) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return lower;
}

Property* PropertyMap::find(const std::string& name)
{
	const auto it = m_index.find(key(name));
	return it == m_index.end() ? nullptr : &m_entries[it->second].second;
}

const Property* PropertyMap::find(const std::string& name) const
{
	const auto it = m_index.find(key(name));
	return it == m_index.end() ? nullptr : &m_entries[it->second].second;
}

Property& PropertyMap::insert(const std::string& name)
{
	const auto it = m_index.find(key(name));
	if (it != m_index.end()) {
		return m_entries[it->second].second;
	}
	m_index.emplace(key(name), m_entries.size());
	m_entries.emplace_back(name, Property {});
	return m_entries.back().second;
}

bool PropertyMap::erase(const std::string& name)
{
	const auto it = m_index.find(key(name));
	if (it == m_index.end()) {
		return false;
	}
	const size_t at = it->second;
	m_index.erase(it);
	m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(at));
	for (size_t i = at; i < m_entries.size(); ++i) {
		m_index[key(m_entries[i].first)] = i;
	}
	return true;
}

void PropertyMap::clear()
{
	m_entries.clear();
	m_index.clear();
}

// --- Object

bool Object::getFromChain(Interpreter& vm, Object* start, const std::string& name, const ObjectRef& self, Value& out)
{
	Object* o = start;
	for (int depth = 0; o && depth < kMaxChain; ++depth, o = o->m_proto.get()) {
		const Property* p = o->m_properties.find(name);
		if (!p) {
			continue;
		}
		if (p->accessor) {
			out = p->getter ? vm.call(p->getter, self, {}, refOf(o)) : Value();
		} else {
			out = p->value;
		}
		return true;
	}
	return false;
}

bool Object::getMember(Interpreter& vm, const std::string& name, Value& out)
{
	if (name == "__proto__") {
		out = m_proto ? Value(m_proto) : Value();
		return m_proto != nullptr;
	}
	const ObjectRef self = shared_from_this();
	if (getFromChain(vm, this, name, self, out)) {
		return true;
	}
	// SWF 6+: obj.__resolve(name) answers for missing members.
	Value resolver;
	if (name != "__resolve" && getFromChain(vm, this, "__resolve", self, resolver) && resolver.isFunction()) {
		out = vm.call(resolver, self, {Value(name)});
		return true;
	}
	return false;
}

void Object::setMember(Interpreter& vm, const std::string& name, const Value& value)
{
	if (name == "__proto__") {
		m_proto = value.object();
		return;
	}
	// An accessor anywhere on the chain takes the assignment (AS2 classes
	// keep their get/set pairs on the prototype); a plain property found
	// first shadows it.
	Object* o = this;
	for (int depth = 0; o && depth < kMaxChain; ++depth, o = o->m_proto.get()) {
		const Property* p = o->m_properties.find(name);
		if (!p) {
			continue;
		}
		if (p->accessor) {
			if (p->setter) {
				vm.call(p->setter, shared_from_this(), {value}, refOf(o));
			}
			return;
		}
		if (o == this && (p->flags & kReadOnly)) {
			return;
		}
		break;
	}
	storeWatched(vm, name, value);
}

void Object::storeWatched(Interpreter& vm, const std::string& name, const Value& value)
{
	Value stored = value;
	const auto watch = m_watches.find(name);
	if (watch != m_watches.end()) {
		const Watch w = watch->second; // the callback may unwatch
		const Property* old = m_properties.find(name);
		stored = vm.call(w.callback, shared_from_this(), {Value(name), old ? old->value : Value(), value, w.userData});
	}
	Property& p = m_properties.insert(name);
	if (!p.accessor && !(p.flags & kReadOnly)) {
		p.value = stored;
	}
}

bool Object::deleteMember(Interpreter&, const std::string& name)
{
	const Property* p = m_properties.find(name);
	if (!p || (p->flags & kDontDelete)) {
		return false;
	}
	m_watches.erase(name);
	return m_properties.erase(name);
}

bool Object::hasOwnMember(const std::string& name) const { return m_properties.find(name) != nullptr; }

void Object::ownEnumerableNames(std::vector<std::string>& out) const
{
	for (const auto& [name, p] : m_properties.entries()) {
		if (!(p.flags & kDontEnum)) {
			out.push_back(name);
		}
	}
}

bool Object::defaultValue(Interpreter&, bool, Value&) { return false; }

void Object::clearReferences()
{
	m_properties.clear();
	m_proto.reset();
	m_interfaces.clear();
	m_watches.clear();
}

void Object::define(const std::string& name, const Value& value, uint8_t flags)
{
	Property& p = m_properties.insert(name);
	p.value = value;
	p.getter.reset();
	p.setter.reset();
	p.accessor = false;
	p.flags = flags;
}

bool Object::addAccessor(const std::string& name, const ObjectRef& getter, const ObjectRef& setter)
{
	if (name.empty() || !getter || !getter->isFunction() || (setter && !setter->isFunction())) {
		return false;
	}
	Property& p = m_properties.insert(name);
	p.value = Value();
	p.getter = getter;
	p.setter = setter;
	p.accessor = true;
	return true;
}

Object* Object::findOwner(const std::string& name)
{
	Object* o = this;
	for (int depth = 0; o && depth < kMaxChain; ++depth, o = o->m_proto.get()) {
		if (o->m_properties.find(name)) {
			return o;
		}
	}
	return nullptr;
}

// --- Functions

const Value& CallArgs::operator[](size_t i) const
{
	static const Value undefined;
	return i < args.size() ? args[i] : undefined;
}

ObjectRef Function::createInstance(Interpreter&) { return nullptr; }

NativeFunction::NativeFunction(ObjectRef proto, NativeCallback callback, InstanceFactory factory)
	: Function(std::move(proto)), m_callback(std::move(callback)), m_factory(std::move(factory))
{
}

Value NativeFunction::call(Interpreter& vm, CallArgs& args) { return m_callback ? m_callback(vm, args) : Value(); }

ObjectRef NativeFunction::createInstance(Interpreter& vm) { return m_factory ? m_factory(vm) : nullptr; }

void NativeFunction::clearReferences()
{
	Function::clearReferences();
	m_callback = nullptr;
	m_factory = nullptr;
}

// --- ArrayObject

bool ArrayObject::getMember(Interpreter& vm, const std::string& name, Value& out)
{
	if (name == "length") {
		out = Value(elements.size());
		return true;
	}
	const int64_t index = arrayIndex(name);
	if (index >= 0 && static_cast<size_t>(index) < elements.size()) {
		out = elements[static_cast<size_t>(index)];
		return true;
	}
	return Object::getMember(vm, name, out);
}

void ArrayObject::setMember(Interpreter& vm, const std::string& name, const Value& value)
{
	if (name == "length") {
		setLength(vm.toNumber(value));
		return;
	}
	const int64_t index = arrayIndex(name);
	if (index >= 0 && static_cast<size_t>(index) < kMaxLength) {
		const size_t i = static_cast<size_t>(index);
		if (i >= elements.size()) {
			elements.resize(i + 1);
		}
		elements[i] = value;
		return;
	}
	Object::setMember(vm, name, value);
}

bool ArrayObject::deleteMember(Interpreter& vm, const std::string& name)
{
	const int64_t index = arrayIndex(name);
	if (index >= 0 && static_cast<size_t>(index) < elements.size()) {
		elements[static_cast<size_t>(index)] = Value();
		return true;
	}
	return Object::deleteMember(vm, name);
}

bool ArrayObject::hasOwnMember(const std::string& name) const
{
	const int64_t index = arrayIndex(name);
	if (index >= 0 && static_cast<size_t>(index) < elements.size()) {
		return true;
	}
	return Object::hasOwnMember(name);
}

void ArrayObject::ownEnumerableNames(std::vector<std::string>& out) const
{
	// Holes (undefined) are left out, as the player never created them.
	for (size_t i = 0; i < elements.size(); ++i) {
		if (!elements[i].isUndefined()) {
			out.push_back(std::to_string(i));
		}
	}
	Object::ownEnumerableNames(out);
}

void ArrayObject::clearReferences()
{
	elements.clear();
	Object::clearReferences();
}

void ArrayObject::setLength(double length)
{
	if (std::isnan(length) || length < 0) {
		return;
	}
	elements.resize(static_cast<size_t>(std::min(length, static_cast<double>(kMaxLength))));
}

// --- PrimitiveObject

bool PrimitiveObject::getMember(Interpreter& vm, const std::string& name, Value& out)
{
	if (primitive.isString() && name == "length") {
		out = Value(utf16Length(primitive.asString()));
		return true;
	}
	return Object::getMember(vm, name, out);
}

// --- SuperObject

bool SuperObject::getMember(Interpreter& vm, const std::string& name, Value& out)
{
	if (!home) {
		return false;
	}
	return getFromChain(vm, home->proto().get(), name, self, out);
}

void SuperObject::setMember(Interpreter& vm, const std::string& name, const Value& value)
{
	if (self) {
		self->setMember(vm, name, value);
	}
}

ObjectRef SuperObject::constructorFunction(Interpreter& vm)
{
	Value ctor;
	if (home && getFromChain(vm, home.get(), "__constructor__", self, ctor)) {
		return ctor.isFunction() ? ctor.asObject() : nullptr;
	}
	return nullptr;
}

void SuperObject::clearReferences()
{
	self.reset();
	home.reset();
	Object::clearReferences();
}

} // namespace dice::hfe::ui::avm1
