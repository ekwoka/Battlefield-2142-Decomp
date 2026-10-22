// AVM1 objects: named properties with flags, a __proto__ chain, and the
// special kinds the player builds in (functions, arrays, primitive
// wrappers, dates, the "super" object).
//
// Objects are shared_ptr-owned. Prototype/constructor cycles keep each
// other alive; the Interpreter breaks them when it shuts down (see
// Interpreter::make).
//
// Host objects (movie clips, text fields) subclass Object and override the
// virtual member hooks to expose _x, child clips and so on.
#pragma once

#include "Value.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dice::hfe::ui::avm1 {

class Interpreter;

// The bits ASSetPropFlags sets and clears.
enum PropertyFlag : uint8_t {
	kDontEnum = 1,
	kDontDelete = 2,
	kReadOnly = 4,
};

struct Property {
	Value value;
	// Object.addProperty accessors. `value` is unused when `accessor` is set;
	// a missing setter makes the property read-only.
	ObjectRef getter;
	ObjectRef setter;
	bool accessor = false;
	uint8_t flags = 0;
};

// Properties in insertion order (for..in reports the newest first).
class PropertyMap {
public:
	Property* find(const std::string& name);
	const Property* find(const std::string& name) const;
	// The existing property or a new one at the end.
	Property& insert(const std::string& name);
	bool erase(const std::string& name);
	void clear();
	size_t size() const { return m_entries.size(); }
	const std::vector<std::pair<std::string, Property>>& entries() const { return m_entries; }

	static std::string key(const std::string& name); // lower case

private:
	std::vector<std::pair<std::string, Property>> m_entries;
	std::unordered_map<std::string, size_t> m_index; // by key()
};

class Object : public std::enable_shared_from_this<Object> {
public:
	Object() = default;
	explicit Object(ObjectRef proto) : m_proto(std::move(proto)) {}
	virtual ~Object() = default;
	Object(const Object&) = delete;
	Object& operator=(const Object&) = delete;

	// --- What scripts see (GetMember, SetMember, ...). Host objects override.

	// Looks `name` up on this object and its prototype chain, running
	// addProperty getters with this object as `this`, then __resolve.
	virtual bool getMember(Interpreter& vm, const std::string& name, Value& out);
	// Assigns `name`: readOnly properties ignore it, a setter found on the
	// chain is called instead, a watch() callback can replace the value.
	virtual void setMember(Interpreter& vm, const std::string& name, const Value& value);
	// Removes an own property. False when missing or dontDelete.
	virtual bool deleteMember(Interpreter& vm, const std::string& name);
	virtual bool hasOwnMember(const std::string& name) const;
	// Own enumerable names, oldest first.
	virtual void ownEnumerableNames(std::vector<std::string>& out) const;
	// typeof: "object", "function" or (for hosts) "movieclip".
	virtual const char* typeName() const { return "object"; }
	virtual bool isFunction() const { return false; }
	// A host's own ToPrimitive (movie clips convert to their target path).
	// Return false to fall back to valueOf/toString.
	virtual bool defaultValue(Interpreter& vm, bool preferString, Value& out);
	// Drops every reference this object holds (interpreter shutdown).
	virtual void clearReferences();

	// --- Plain storage: no hooks, getters or flags checks.

	Property* findOwn(const std::string& name) { return m_properties.find(name); }
	const Property* findOwn(const std::string& name) const { return m_properties.find(name); }
	// Creates or overwrites a data property.
	void define(const std::string& name, const Value& value, uint8_t flags = 0);
	// Object.addProperty: false when `getter` is not a function.
	bool addAccessor(const std::string& name, const ObjectRef& getter, const ObjectRef& setter);
	PropertyMap& properties() { return m_properties; }
	const PropertyMap& properties() const { return m_properties; }

	const ObjectRef& proto() const { return m_proto; }
	void setProto(ObjectRef proto) { m_proto = std::move(proto); }

	// The object on this chain that holds `name` as a stored property
	// (CallMethod uses it to find what "super" means), or null.
	Object* findOwner(const std::string& name);

	// ImplementsOp: interfaces declared on this (prototype) object.
	std::vector<ObjectRef>& interfaces() { return m_interfaces; }

	// Object.watch callbacks.
	struct Watch {
		ObjectRef callback;
		Value userData;
	};
	void watch(const std::string& name, Watch watch) { m_watches[name] = std::move(watch); }
	bool unwatch(const std::string& name) { return m_watches.erase(name) > 0; }

	// Looks `name` up along the chain starting at `start`; getters run with
	// `self` as `this`. Shared by objects and the super object.
	static bool getFromChain(Interpreter& vm, Object* start, const std::string& name, const ObjectRef& self, Value& out);

protected:
	// The data assignment after accessors and readOnly were dealt with.
	void storeWatched(Interpreter& vm, const std::string& name, const Value& value);

private:
	PropertyMap m_properties;
	ObjectRef m_proto;
	std::vector<ObjectRef> m_interfaces;
	std::unordered_map<std::string, Watch> m_watches;
};

// --- Functions

// One call's arguments. `superHome` is the prototype object of the class
// whose method runs: "super.x" looks in its __proto__ and "super()" calls
// its __constructor__.
struct CallArgs {
	ObjectRef thisObject;
	std::vector<Value> args;
	ObjectRef superHome;
	bool constructing = false;

	const Value& operator[](size_t i) const;
	size_t size() const { return args.size(); }
};

class Function : public Object {
public:
	using Object::Object;
	bool isFunction() const override { return true; }
	const char* typeName() const override { return "function"; }
	// Runs the function. May throw ScriptThrow/ScriptAbort (Interpreter.h).
	virtual Value call(Interpreter& vm, CallArgs& args) = 0;
	// The object `new` passes as `this`: built-in classes make their own
	// kind (Array makes an ArrayObject). Null means a plain object.
	virtual ObjectRef createInstance(Interpreter& vm);
};

using NativeCallback = std::function<Value(Interpreter&, CallArgs&)>;
using InstanceFactory = std::function<ObjectRef(Interpreter&)>;

// A function implemented in C++ (built-ins, host methods).
class NativeFunction : public Function {
public:
	NativeFunction(ObjectRef proto, NativeCallback callback, InstanceFactory factory = {});
	Value call(Interpreter& vm, CallArgs& args) override;
	ObjectRef createInstance(Interpreter& vm) override;
	void clearReferences() override;

private:
	NativeCallback m_callback;
	InstanceFactory m_factory;
};

// --- Built-in kinds

// Arrays keep their elements in a vector; "length" and index names map
// onto it. Other names are ordinary properties.
class ArrayObject : public Object {
public:
	// Larger indices are stored as plain properties.
	static constexpr size_t kMaxLength = size_t(1) << 24;

	using Object::Object;
	std::vector<Value> elements;

	bool getMember(Interpreter& vm, const std::string& name, Value& out) override;
	void setMember(Interpreter& vm, const std::string& name, const Value& value) override;
	bool deleteMember(Interpreter& vm, const std::string& name) override;
	bool hasOwnMember(const std::string& name) const override;
	void ownEnumerableNames(std::vector<std::string>& out) const override;
	void clearReferences() override;

	// Sets "length": truncates or pads with undefined (capped at kMaxLength).
	void setLength(double length);
};

// new String/Number/Boolean, and the temporary object a primitive becomes
// for a method call ("abc".charAt(1)).
class PrimitiveObject : public Object {
public:
	PrimitiveObject(ObjectRef proto, Value primitive) : Object(std::move(proto)), primitive(std::move(primitive)) {}
	Value primitive;

	bool getMember(Interpreter& vm, const std::string& name, Value& out) override;
};

// Date instances: milliseconds since 1970 UTC (NaN when invalid).
class DateObject : public Object {
public:
	using Object::Object;
	double time = 0;
};

// The value of "super" inside a method: member reads start at the parent
// prototype, and calls keep the original `this`.
class SuperObject : public Object {
public:
	SuperObject(ObjectRef self, ObjectRef home) : self(std::move(self)), home(std::move(home)) {}
	ObjectRef self; // `this` of the method using super
	ObjectRef home; // the prototype whose __proto__ super reads

	bool getMember(Interpreter& vm, const std::string& name, Value& out) override;
	void setMember(Interpreter& vm, const std::string& name, const Value& value) override;
	// super(...) calls home.__constructor__.
	ObjectRef constructorFunction(Interpreter& vm);
	void clearReferences() override;
};

} // namespace dice::hfe::ui::avm1
