#include "Natives.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace dice::hfe::ui {

using avm1::CallArgs;
using avm1::Interpreter;
using avm1::ObjectRef;
using avm1::Value;

namespace {

constexpr const char* kListObject = "dice.bf2.List";

// One of the dice objects: its slots live in the registry, so values the
// game sets later show through, and members scripts add are kept as
// ordinary properties (the movies add dice.UIs and friends).
class NativeObject : public avm1::Object {
public:
	NativeObject(NativeRegistry& registry, std::string path, ObjectRef proto)
		: avm1::Object(std::move(proto)), m_registry(registry), m_path(std::move(path))
	{
	}

	bool getMember(Interpreter& vm, const std::string& written, Value& out) override
	{
		if (findOwn(written)) {
			return avm1::Object::getMember(vm, written, out);
		}
		// The menus' SWF version matches names without regard to case
		// (the video page calls getTexturequality).
		const std::string name = m_registry.memberNamed(m_path, written);
		if (m_registry.has(m_path, name)) {
			if (m_registry.slotType(m_path, name) == NativeSlot::Function) {
				out = Value(function(vm, name));
				return true;
			}
			if (m_registry.readSlot(m_path, name, out)) {
				return true;
			}
		}
		return avm1::Object::getMember(vm, name, out);
	}

	void setMember(Interpreter& vm, const std::string& name, const Value& value) override
	{
		const std::string member = m_registry.memberNamed(m_path, name);
		if (!findOwn(name) && m_registry.has(m_path, member) && m_registry.writeSlot(vm, m_path, member, value)) {
			return;
		}
		avm1::Object::setMember(vm, name, value);
	}

	bool hasOwnMember(const std::string& name) const override
	{
		return avm1::Object::hasOwnMember(name) || m_registry.has(m_path, m_registry.memberNamed(m_path, name));
	}

	void ownEnumerableNames(std::vector<std::string>& out) const override
	{
		for (const std::string& member : m_registry.members(m_path)) {
			out.push_back(member);
		}
		avm1::Object::ownEnumerableNames(out);
	}

	void clearReferences() override
	{
		m_functions.clear();
		avm1::Object::clearReferences();
	}

private:
	// One function object per member, so that identity holds (a == a).
	ObjectRef function(Interpreter& vm, const std::string& name)
	{
		ObjectRef& f = m_functions[name];
		if (!f) {
			NativeRegistry* registry = &m_registry;
			const std::string path = m_path;
			f = vm.newFunction([registry, path, name](Interpreter& vm, CallArgs& c) -> Value {
				return registry->call(vm, path, name, c.args);
			});
		}
		return f;
	}

	NativeRegistry& m_registry;
	std::string m_path;
	std::map<std::string, ObjectRef> m_functions;
};

int toInt(Interpreter& vm, const Value& v, int fallback)
{
	if (v.isUndefined()) {
		return fallback;
	}
	const double n = vm.toNumber(v);
	return std::isnan(n) ? fallback : static_cast<int>(n);
}

std::vector<std::string> splitSpaces(const std::string& s)
{
	std::vector<std::string> out;
	std::istringstream in(s);
	std::string word;
	while (in >> word) {
		out.push_back(word);
	}
	return out;
}

} // namespace

std::string describeValue(Interpreter& vm, const Value& value)
{
	switch (value.type()) {
	case Value::Type::Undefined: return "undefined";
	case Value::Type::Null: return "null";
	case Value::Type::Boolean: return value.asBoolean() ? "true" : "false";
	case Value::Type::Number: return avm1::numberToString(value.asNumber());
	case Value::Type::String: return "\"" + value.asString() + "\"";
	case Value::Type::Object: {
		if (const auto* boxed = dynamic_cast<const avm1::PrimitiveObject*>(value.asObject().get())) {
			return describeValue(vm, boxed->primitive); // new String("...")
		}
		const std::string path = vm.host().targetPath(value.asObject());
		if (!path.empty()) {
			return path;
		}
		return value.asObject()->isFunction() ? "[function]" : "[object]";
	}
	}
	return {};
}

// --- NativeList

// The select boxes (selectBoxSmall/Medium/Large/NewAcc) show the list
// their first argument names ("displayModes 6").
const std::vector<std::vector<std::string>>& NativeList::rows() const
{
	if (listId.compare(0, 9, "selectBox") == 0 && !args.empty()) {
		return registry.listRows(args[0]);
	}
	return registry.listRows(listId);
}

std::string NativeList::rowPrefix(Interpreter& vm) const
{
	if (holder && !vm.getMember(Value(holder), "0").isObject() && vm.getMember(Value(holder), "row0").isObject()) {
		return "row";
	}
	return {};
}

int NativeList::visibleRows(Interpreter& vm) const
{
	if (!holder) {
		return 0;
	}
	const std::string prefix = rowPrefix(vm);
	int count = 0;
	while (count < 1000 && vm.getMember(Value(holder), prefix + std::to_string(count)).isObject()) {
		++count;
	}
	return count;
}

void NativeList::populate(Interpreter& vm)
{
	if (!holder) {
		return;
	}
	const auto& data = rows();
	const std::string prefix = rowPrefix(vm);
	const int shown = visibleRows(vm);
	for (int i = 0; i < shown; ++i) {
		const Value row = vm.getMember(Value(holder), prefix + std::to_string(i));
		const int index = firstIndex + i;
		const bool filled = index >= 0 && index < static_cast<int>(data.size());
		for (size_t column = 0; column < 10; ++column) { // cell0..cell9 (0x100f0000's names)
			const Value cell = vm.getMember(row, "cell" + std::to_string(column));
			if (!cell.isObject()) {
				continue;
			}
			const bool has = filled && column < data[static_cast<size_t>(index)].size();
			const Value text(has ? data[static_cast<size_t>(index)][column] : std::string());
			vm.setMember(cell, "text", text);
		}
		vm.setMember(row, "isClickable", Value(filled ? 1 : 0));
		vm.setMember(row, "isSelected", Value(filled && index == selectedIndex));
		vm.setMember(row, "rowIndex", Value(index));
	}
}

bool NativeList::getMember(Interpreter& vm, const std::string& name, Value& out)
{
	if (name == "m_firstIndex") {
		out = Value(firstIndex);
		return true;
	}
	if (name == "m_rowCount") {
		out = Value(static_cast<double>(rows().size()));
		return true;
	}
	if (name == "m_selectedIndex") {
		out = Value(selectedIndex);
		return true;
	}
	return avm1::Object::getMember(vm, name, out);
}

void NativeList::setMember(Interpreter& vm, const std::string& name, const Value& value)
{
	if (name == "m_firstIndex") {
		firstIndex = toInt(vm, value, firstIndex);
	} else if (name == "m_selectedIndex") {
		selectedIndex = toInt(vm, value, selectedIndex);
	} else if (name != "m_rowCount") {
		avm1::Object::setMember(vm, name, value);
	}
}

void NativeList::clearReferences()
{
	holder.reset();
	avm1::Object::clearReferences();
}

// --- NativeRegistry

NativeRegistry::NativeRegistry()
{
	for (size_t i = 0; i < kNativeSlotCount; ++i) {
		slot(kNativeSlots[i].object, kNativeSlots[i].member, kNativeSlots[i].type);
	}
}

NativeRegistry::Slot& NativeRegistry::slot(const std::string& object, const std::string& member, NativeSlot type)
{
	auto& members = m_objects[object];
	auto it = members.find(member);
	if (it == members.end()) {
		it = members.emplace(member, Slot()).first;
		it->second.type = type;
	}
	return it->second;
}

const NativeRegistry::Slot* NativeRegistry::find(const std::string& object, const std::string& member) const
{
	const auto o = m_objects.find(object);
	if (o == m_objects.end()) {
		return nullptr;
	}
	const auto m = o->second.find(member);
	return m == o->second.end() ? nullptr : &m->second;
}

std::string NativeRegistry::memberNamed(const std::string& object, const std::string& name) const
{
	const auto o = m_objects.find(object);
	if (o == m_objects.end() || o->second.count(name)) {
		return name;
	}
	const std::string lower = avm1::PropertyMap::key(name);
	for (const auto& [member, slot] : o->second) {
		if (avm1::PropertyMap::key(member) == lower) {
			return member;
		}
	}
	return name;
}

void NativeRegistry::setFunction(const std::string& object, const std::string& member, Function function)
{
	Slot& s = slot(object, member, NativeSlot::Function);
	s.type = NativeSlot::Function;
	s.function = std::move(function);
	s.defaultBinding = false;
}

void NativeRegistry::setDefaultFunction(const std::string& object, const std::string& member, Function function)
{
	Slot& s = slot(object, member, NativeSlot::Function);
	if (s.function && !s.defaultBinding) {
		return; // the game's binding wins
	}
	s.type = NativeSlot::Function;
	s.function = std::move(function);
	s.defaultBinding = true;
}

void NativeRegistry::setValue(const std::string& object, const std::string& member, const Value& value)
{
	NativeSlot type = NativeSlot::String;
	if (value.isBoolean()) {
		type = NativeSlot::Bool;
	} else if (value.isNumber()) {
		type = NativeSlot::Float;
	} else if (value.isObject()) {
		type = NativeSlot::ObjectRef;
	}
	Slot& s = slot(object, member, type);
	if (s.type == NativeSlot::Function || s.type == NativeSlot::Getter) {
		s.type = type;
	}
	s.value = value;
}

void NativeRegistry::setGetter(const std::string& object, const std::string& member, Getter getter, Setter setter)
{
	Slot& s = slot(object, member, NativeSlot::Getter);
	s.type = NativeSlot::Getter;
	s.getter = std::move(getter);
	s.setter = std::move(setter);
}

void NativeRegistry::setListMethod(const std::string& member, ListMethod method) { m_listMethods[member] = std::move(method); }

void NativeRegistry::setListRows(const std::string& listId, Rows rows) { m_lists[listId] = std::move(rows); }

const NativeRegistry::Rows& NativeRegistry::listRows(const std::string& listId) const
{
	static const Rows empty;
	const auto it = m_lists.find(listId);
	return it == m_lists.end() ? empty : it->second;
}

bool NativeRegistry::has(const std::string& object, const std::string& member) const { return find(object, member) != nullptr; }

NativeSlot NativeRegistry::slotType(const std::string& object, const std::string& member) const
{
	const Slot* s = find(object, member);
	return s ? s->type : NativeSlot::Function;
}

bool NativeRegistry::isBound(const std::string& object, const std::string& member) const
{
	const Slot* s = find(object, member);
	if (!s) {
		return false;
	}
	switch (s->type) {
	case NativeSlot::Function: return static_cast<bool>(s->function);
	case NativeSlot::Getter: return static_cast<bool>(s->getter);
	default: return !s->value.isUndefined();
	}
}

std::vector<std::string> NativeRegistry::objects() const
{
	std::vector<std::string> out;
	for (const auto& [name, members] : m_objects) {
		out.push_back(name);
	}
	return out;
}

std::vector<std::string> NativeRegistry::members(const std::string& object) const
{
	std::vector<std::string> out;
	const auto it = m_objects.find(object);
	if (it != m_objects.end()) {
		for (const auto& [name, s] : it->second) {
			out.push_back(name);
		}
	}
	return out;
}

bool NativeRegistry::readSlot(const std::string& object, const std::string& member, Value& out) const
{
	const Slot* s = find(object, member);
	if (!s) {
		return false;
	}
	switch (s->type) {
	case NativeSlot::Function: return false;
	case NativeSlot::Getter: out = s->getter ? s->getter() : Value(); return true;
	case NativeSlot::Bool: out = s->value.isUndefined() ? Value(false) : Value(avm1::toBoolean(s->value)); return true;
	case NativeSlot::Int:
		out = s->value.isNumber() ? Value(avm1::toInteger(s->value.asNumber())) : Value(0);
		return true;
	case NativeSlot::Float: out = s->value.isNumber() ? s->value : Value(0); return true;
	case NativeSlot::String: out = s->value.isUndefined() ? Value("") : s->value; return true;
	case NativeSlot::Object:
	case NativeSlot::ObjectRef: out = s->value; return true;
	}
	return false;
}

bool NativeRegistry::writeSlot(Interpreter& vm, const std::string& object, const std::string& member, const Value& value)
{
	auto o = m_objects.find(object);
	if (o == m_objects.end()) {
		return false;
	}
	auto m = o->second.find(member);
	if (m == o->second.end()) {
		return false;
	}
	Slot& s = m->second;
	switch (s.type) {
	case NativeSlot::Function: return false; // a script's own function shadows the native
	case NativeSlot::Getter:
		if (s.setter) {
			s.setter(value);
		}
		return true;
	case NativeSlot::Bool: s.value = Value(avm1::toBoolean(value)); return true;
	case NativeSlot::Int: s.value = Value(avm1::toInteger(vm.toNumber(value))); return true;
	case NativeSlot::Float: s.value = Value(vm.toNumber(value)); return true;
	case NativeSlot::String: s.value = Value(vm.toString(value)); return true;
	case NativeSlot::Object:
	case NativeSlot::ObjectRef: s.value = value; return true;
	}
	return false;
}

void NativeRegistry::log(Interpreter& vm, const std::string& object, const std::string& member, const std::vector<Value>& args,
	bool bound)
{
	if (!bound) {
		const std::string key = object + "." + member;
		if (m_unboundSeen.insert(key).second) {
			m_unbound.push_back(key);
		}
	}
	if (m_calls.size() >= maxLoggedCalls && !m_observer) {
		return;
	}
	NativeCall call;
	call.object = object;
	call.member = member;
	call.bound = bound;
	call.time = vm.clock();
	for (const Value& arg : args) {
		call.args.push_back(describeValue(vm, arg));
	}
	if (m_observer) {
		m_observer(call);
	}
	if (m_calls.size() < maxLoggedCalls) {
		m_calls.push_back(std::move(call));
	}
}

Value NativeRegistry::call(Interpreter& vm, const std::string& object, const std::string& member, const std::vector<Value>& args)
{
	const Slot* s = find(object, member);
	const bool bound = s && s->function;
	log(vm, object, member, args, bound);
	if (!bound) {
		return Value();
	}
	const Function function = s->function; // the call may rebind it
	return function(vm, args);
}

Value NativeRegistry::callList(Interpreter& vm, NativeList& list, const std::string& member, const std::vector<Value>& args)
{
	const auto it = m_listMethods.find(member);
	log(vm, std::string(kListObject) + ":" + list.listId, member, args, true);
	if (it != m_listMethods.end()) {
		const ListMethod method = it->second;
		return method(vm, list, args);
	}
	return defaultListMethod(vm, list, member, args);
}

// The lists' built-in behaviour: rows come from setListRows; scrolling
// moves the first shown row within [0, rows - shown rows].
Value NativeRegistry::defaultListMethod(Interpreter& vm, NativeList& list, const std::string& member,
	const std::vector<Value>& args)
{
	const auto arg = [&](size_t i) { return i < args.size() ? args[i] : Value(); };
	const int count = static_cast<int>(list.rows().size());
	const int shown = std::max(1, list.visibleRows(vm));
	const int lastFirst = std::max(0, count - shown);
	const auto scrollTo = [&](int first) {
		list.firstIndex = std::clamp(first, 0, lastFirst);
		list.populate(vm);
		return Value(list.firstIndex);
	};
	if (member == "fillList" || member == "refreshList") {
		list.firstIndex = std::clamp(list.firstIndex, 0, lastFirst);
		list.populate(vm);
		return Value(true);
	}
	if (member == "sortList") {
		const size_t column = static_cast<size_t>(std::max(0, toInt(vm, arg(0), 0)));
		const bool ascending = arg(1).isUndefined() || avm1::toBoolean(arg(1));
		Rows& rows = m_lists[list.listId];
		std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
			const std::string& x = column < a.size() ? a[column] : std::string();
			const std::string& y = column < b.size() ? b[column] : std::string();
			return ascending ? x < y : y < x;
		});
		list.populate(vm);
		return Value();
	}
	if (member == "scrollUp") {
		return scrollTo(list.firstIndex - toInt(vm, arg(0), 1));
	}
	if (member == "scrollDown") {
		return scrollTo(list.firstIndex + toInt(vm, arg(0), 1));
	}
	if (member == "pageUp") {
		return scrollTo(list.firstIndex - shown);
	}
	if (member == "pageDown") {
		return scrollTo(list.firstIndex + shown);
	}
	if (member == "setFirstIndex") {
		return scrollTo(toInt(vm, arg(0), 0));
	}
	if (member == "scrollPercent") {
		const double percent = std::clamp(vm.toNumber(arg(0)), 0.0, 100.0);
		return scrollTo(static_cast<int>(std::lround(percent / 100.0 * lastFirst)));
	}
	if (member == "scrollToSelection") {
		if (list.selectedIndex >= 0 && (list.selectedIndex < list.firstIndex || list.selectedIndex >= list.firstIndex + shown)) {
			scrollTo(list.selectedIndex);
		}
		return Value();
	}
	if (member == "selectRow") {
		// The row clip's number among those shown.
		const int row = toInt(vm, arg(0), -1);
		list.selectedIndex = row < 0 ? -1 : list.firstIndex + row;
		list.populate(vm);
		return Value();
	}
	if (member == "setArgs") {
		list.args = splitSpaces(vm.toString(arg(0)));
		return Value();
	}
	return Value(); // abortFill
}

void NativeRegistry::install(Interpreter& vm)
{
	const ObjectRef objectProto = vm.prototypes().object;
	std::map<std::string, ObjectRef> made;
	// "dice", "dice.bf2", ... made on demand; each child set on its parent.
	std::function<ObjectRef(const std::string&)> object = [&](const std::string& path) -> ObjectRef {
		auto it = made.find(path);
		if (it != made.end()) {
			return it->second;
		}
		ObjectRef o = vm.make<NativeObject>(*this, path, objectProto);
		made[path] = o;
		const size_t dot = path.rfind('.');
		if (dot == std::string::npos) {
			vm.global()->define(path, Value(o), avm1::kDontEnum);
		} else {
			object(path.substr(0, dot))->define(path.substr(dot + 1), Value(o));
		}
		return o;
	};
	for (const auto& [path, members] : m_objects) {
		if (path != kListObject) {
			object(path);
		}
	}

	// dice.bf2.List: a class (0x1003a2b0) whose prototype holds the list
	// methods (0x100399f0); instances are NativeLists.
	auto prototype = vm.newObject();
	auto list = vm.newClass(
		[this](Interpreter& vm, CallArgs& c) -> Value {
			auto* self = dynamic_cast<NativeList*>(c.thisObject.get());
			if (!c.constructing || !self) {
				return Value();
			}
			self->holder = c[0].object();
			self->listId = c.size() > 1 ? vm.toString(c[1]) : std::string();
			self->args = c.size() > 2 ? splitSpaces(vm.toString(c[2])) : std::vector<std::string>();
			log(vm, std::string(kListObject) + ":" + self->listId, "__constructor__", c.args, true);
			return Value();
		},
		[this, prototype](Interpreter& vm) -> ObjectRef { return vm.make<NativeList>(*this, prototype); }, prototype);
	for (const std::string& member : members(kListObject)) {
		if (slotType(kListObject, member) != NativeSlot::Function) {
			continue;
		}
		vm.defineMethod(prototype, member, [this, member](Interpreter& vm, CallArgs& c) -> Value {
			auto* self = dynamic_cast<NativeList*>(c.thisObject.get());
			return self ? callList(vm, *self, member, c.args) : Value();
		});
	}
	object("dice.bf2")->define("List", Value(list));
}

} // namespace dice::hfe::ui
