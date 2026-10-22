// The game's side of the Flash menus: the objects SwiffPlayer.dll puts under
// _global.dice before a movie runs (0x10009050): dice.General,
// dice.MessageHandler and dice.bf2.{Locale, Logic, Singleplay, Multiplay,
// Profile, Client, Sound, Cursor, Mod, EndOfRound, ControlSettings,
// VideoSettings, AudioSettings, ServerSettings, ServerInfo,
// GeneralSettings, PlayerStats, Shader, BuddySystem, Mouse}, plus the
// dice.bf2.List class whose instances back the menus' lists.
//
// Each object is a set of typed slots (SwiffPlayer's helpers
// 0x10005380-0x100054d0 register one type each, 0x10005500/0x10005690
// set and get them): 0 bool, 1 string, 2 int, 3 float, 4/6 object,
// 5 native function, 7 getter callback. Here every slot of the shipped
// player exists from the start (NativeTable.cpp), so scripts never meet
// undefined objects; the game binds behaviour to them by name. A function
// nobody bound returns undefined and is noted once in unboundCalls().
//
// Lists: `new dice.bf2.List(holder, listId, args)` (constructor 0x10039220)
// makes a list that fills its holder clip's rows: row clips named "0",
// "1", ... (0x100386b0; the server lists' holders call them "row0", ...)
// with text fields "cell0", "cell1", ..., and isClickable/isSelected/
// rowIndex set on each row. The game
// supplies the rows as string columns (setListRows) or replaces any list
// method (setListMethod).
#pragma once

#include "UI/Avm1/Interpreter.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dice::hfe::ui {

enum class NativeSlot : uint8_t {
	Bool = 0,
	String = 1,
	Int = 2,
	Float = 3,
	Object = 4,
	Function = 5,
	ObjectRef = 6,
	Getter = 7,
};

struct NativeSlotSpec {
	const char* object; // "dice.bf2.Locale"
	const char* member;
	NativeSlot type;
};
extern const NativeSlotSpec kNativeSlots[];
extern const size_t kNativeSlotCount;
extern const char* const kNativeListIds[];
extern const size_t kNativeListIdCount;

// One call from a movie into a native function (or list method).
struct NativeCall {
	std::string object; // "dice.bf2.Locale", or "dice.bf2.List:serverList"
	std::string member;
	std::vector<std::string> args; // primitives printed, objects as "[object]"/target paths
	bool bound = false;
	double time = 0; // movie milliseconds
};

class NativeRegistry;

// A dice.bf2.List instance.
class NativeList : public avm1::Object {
public:
	NativeList(NativeRegistry& registry, avm1::ObjectRef proto) : registry(registry) { setProto(std::move(proto)); }

	NativeRegistry& registry;
	std::string listId;
	std::vector<std::string> args; // the constructor's third argument split at spaces
	avm1::ObjectRef holder;        // the clip with rows "0", "1", ...
	int firstIndex = 0;
	int selectedIndex = -1;

	// The rows the game set for this list id (empty if none).
	const std::vector<std::vector<std::string>>& rows() const;
	// How many row clips the holder has ("0", "1", ... or "row0", "row1",
	// ... until one is missing).
	int visibleRows(avm1::Interpreter& vm) const;
	std::string rowPrefix(avm1::Interpreter& vm) const;
	// Writes rows [firstIndex, firstIndex + visibleRows) into the holder's
	// row clips: cellN.text, isClickable and isSelected.
	void populate(avm1::Interpreter& vm);

	bool getMember(avm1::Interpreter& vm, const std::string& name, avm1::Value& out) override;
	void setMember(avm1::Interpreter& vm, const std::string& name, const avm1::Value& value) override;
	void clearReferences() override;
};

class NativeRegistry {
public:
	using Function = std::function<avm1::Value(avm1::Interpreter&, const std::vector<avm1::Value>&)>;
	using Getter = std::function<avm1::Value()>;
	using Setter = std::function<void(const avm1::Value&)>;
	using ListMethod = std::function<avm1::Value(avm1::Interpreter&, NativeList&, const std::vector<avm1::Value>&)>;
	using Localizer = std::function<std::string(const std::string& key)>;
	using Rows = std::vector<std::vector<std::string>>;

	NativeRegistry();

	// --- Binding (any time, before or after a movie loads)

	// object is a dotted path ("dice.bf2.Locale"). Members not in the table
	// are added.
	void setFunction(const std::string& object, const std::string& member, Function function);
	// Bool/String/Int/Float slots (converted to the slot's type when read).
	void setValue(const std::string& object, const std::string& member, const avm1::Value& value);
	// Getter slots; without a setter, assignments are ignored.
	void setGetter(const std::string& object, const std::string& member, Getter getter, Setter setter = {});
	// Replaces a dice.bf2.List method (fillList, sortList, ...) for every list.
	void setListMethod(const std::string& member, ListMethod method);
	// The rows of a list id (columns of text).
	void setListRows(const std::string& listId, Rows rows);
	const Rows& listRows(const std::string& listId) const;
	// Locale.getString's text for a key (default: the key itself).
	void setLocalizer(Localizer localizer) { m_localizer = std::move(localizer); }
	std::string localize(const std::string& key) const { return m_localizer ? m_localizer(key) : key; }

	// Built-in behaviour (Player installs these): bound like setFunction
	// unless the game bound the member already, and replaceable by it.
	void setDefaultFunction(const std::string& object, const std::string& member, Function function);

	// --- What exists and what the movie did

	bool has(const std::string& object, const std::string& member) const;
	// The member's registered spelling for a name in any case (the name
	// itself when none matches).
	std::string memberNamed(const std::string& object, const std::string& name) const;
	NativeSlot slotType(const std::string& object, const std::string& member) const;
	bool isBound(const std::string& object, const std::string& member) const;
	std::vector<std::string> objects() const;
	std::vector<std::string> members(const std::string& object) const;

	// Every call, oldest first (up to maxLoggedCalls; later ones are dropped).
	const std::vector<NativeCall>& calls() const { return m_calls; }
	void clearCalls() { m_calls.clear(); }
	size_t maxLoggedCalls = 20000;
	// "object.member" of each unbound function the first time it is called.
	const std::vector<std::string>& unboundCalls() const { return m_unbound; }
	// Told of every call as it happens.
	void setCallObserver(std::function<void(const NativeCall&)> observer) { m_observer = std::move(observer); }

	// --- For the player

	// Creates _global.dice (and dice.bf2.List) in `vm`.
	void install(avm1::Interpreter& vm);
	// Runs a native function slot (logging it).
	avm1::Value call(avm1::Interpreter& vm, const std::string& object, const std::string& member,
		const std::vector<avm1::Value>& args);
	avm1::Value callList(avm1::Interpreter& vm, NativeList& list, const std::string& member, const std::vector<avm1::Value>& args);
	// A slot's value (Bool/String/Int/Float/Getter) as scripts see it.
	bool readSlot(const std::string& object, const std::string& member, avm1::Value& out) const;
	bool writeSlot(avm1::Interpreter& vm, const std::string& object, const std::string& member, const avm1::Value& value);

private:
	struct Slot {
		NativeSlot type = NativeSlot::Function;
		Function function;
		bool defaultBinding = false;
		avm1::Value value;
		Getter getter;
		Setter setter;
	};

	Slot& slot(const std::string& object, const std::string& member, NativeSlot type);
	const Slot* find(const std::string& object, const std::string& member) const;
	void log(avm1::Interpreter& vm, const std::string& object, const std::string& member, const std::vector<avm1::Value>& args,
		bool bound);
	avm1::Value defaultListMethod(avm1::Interpreter& vm, NativeList& list, const std::string& member,
		const std::vector<avm1::Value>& args);

	std::map<std::string, std::map<std::string, Slot>> m_objects;
	std::map<std::string, ListMethod> m_listMethods;
	std::map<std::string, Rows> m_lists;
	Localizer m_localizer;
	std::vector<NativeCall> m_calls;
	std::vector<std::string> m_unbound;
	std::set<std::string> m_unboundSeen;
	std::function<void(const NativeCall&)> m_observer;
};

// A Value printed without running script (for logs).
std::string describeValue(avm1::Interpreter& vm, const avm1::Value& value);

} // namespace dice::hfe::ui
