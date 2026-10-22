// Flash Player 7 classes that need a stage, input, sound or the network.
// Until the display-list layer provides them, they exist so that scripts
// referencing them (class X extends MovieClip, Key.isDown, new Sound())
// run without errors: methods do nothing and report "nothing happened"
// (false, 0, null). The host replaces or extends these objects; MovieClip,
// TextField and Button only provide empty prototypes for it to fill.
//
// Real behaviour here: Key's key-code constants, TextFormat's fields,
// LoadVars.decode/toString and the listener lists (AsBroadcaster).
#include "Builtins.h"

#include <algorithm>
#include <initializer_list>

namespace dice::hfe::ui::avm1::builtins {

namespace {

// Methods that ignore their arguments and return `result`.
void stubMethods(Interpreter& vm, const ObjectRef& object, std::initializer_list<const char*> names, const Value& result = Value())
{
	for (const char* name : names) {
		vm.defineMethod(object, name, [result](Interpreter&, CallArgs&) -> Value { return result; });
	}
}

ObjectRef prototypeOf(Interpreter& vm, const std::shared_ptr<NativeFunction>& ctor)
{
	return vm.getMember(Value(ctor), "prototype").object();
}

// An object with addListener/removeListener (Key, Mouse, Stage, Selection).
ObjectRef broadcaster(Interpreter& vm, const std::string& name)
{
	const ObjectRef object = vm.newObject();
	initializeBroadcaster(vm, object);
	defineGlobal(vm, name, Value(object));
	return object;
}

void installKey(Interpreter& vm)
{
	const ObjectRef key = broadcaster(vm, "Key");
	const struct {
		const char* name;
		int code;
	} codes[] = {{"BACKSPACE", 8}, {"TAB", 9}, {"ENTER", 13}, {"SHIFT", 16}, {"CONTROL", 17}, {"ALT", 18}, {"CAPSLOCK", 20},
		{"ESCAPE", 27}, {"SPACE", 32}, {"PGUP", 33}, {"PGDN", 34}, {"END", 35}, {"HOME", 36}, {"LEFT", 37}, {"UP", 38},
		{"RIGHT", 39}, {"DOWN", 40}, {"INSERT", 45}, {"DELETEKEY", 46}};
	for (const auto& c : codes) {
		key->define(c.name, Value(c.code), kDontEnum | kDontDelete | kReadOnly);
	}
	stubMethods(vm, key, {"isDown", "isToggled"}, Value(false));
	stubMethods(vm, key, {"getCode", "getAscii"}, Value(0));
}

void installTextFormat(Interpreter& vm)
{
	// new TextFormat(font, size, color, bold, italic, underline, url,
	// target, align, leftMargin, rightMargin, indent, leading)
	auto textFormat = defineClass(vm, "TextFormat", [](Interpreter& vm, CallArgs& c) -> Value {
		static const char* const fields[] = {"font", "size", "color", "bold", "italic", "underline", "url", "target", "align",
			"leftMargin", "rightMargin", "indent", "leading", "blockIndent", "bullet", "tabStops"};
		if (c.constructing && c.thisObject) {
			size_t i = 0;
			for (const char* field : fields) {
				c.thisObject->setMember(vm, field, i < c.size() ? c[i] : Value::null());
				++i;
			}
		}
		return Value();
	});
	vm.defineMethod(prototypeOf(vm, textFormat), "getTextExtent", [](Interpreter& vm, CallArgs&) -> Value {
		const ObjectRef extent = vm.newObject();
		for (const char* field : {"width", "height", "ascent", "descent", "textFieldWidth", "textFieldHeight"}) {
			extent->define(field, Value(0));
		}
		return Value(extent);
	});
}

void installLoadVars(Interpreter& vm)
{
	auto loadVars = defineClass(vm, "LoadVars");
	const ObjectRef proto = prototypeOf(vm, loadVars);
	stubMethods(vm, proto, {"load", "send", "sendAndLoad"}, Value(false));
	stubMethods(vm, proto, {"addRequestHeader"});
	stubMethods(vm, proto, {"getBytesLoaded", "getBytesTotal"}, Value(0));
	// decode("a=1&b=2") sets this.a and this.b.
	vm.defineMethod(proto, "decode", [](Interpreter& vm, CallArgs& c) -> Value {
		const std::string query = vm.toString(c[0]);
		const Value unescape = vm.getMember(Value(vm.global()), "unescape");
		size_t at = 0;
		while (at < query.size() && c.thisObject) {
			const size_t amp = std::min(query.find('&', at), query.size());
			const std::string pair = query.substr(at, amp - at);
			const size_t eq = pair.find('=');
			if (eq != std::string::npos) {
				const Value name = vm.call(unescape, nullptr, {Value(pair.substr(0, eq))});
				const Value value = vm.call(unescape, nullptr, {Value(pair.substr(eq + 1))});
				c.thisObject->setMember(vm, vm.toString(name), value);
			}
			at = amp + 1;
		}
		return Value();
	});
	vm.defineMethod(proto, "toString", [](Interpreter& vm, CallArgs& c) -> Value {
		if (!c.thisObject) {
			return Value("");
		}
		const Value escape = vm.getMember(Value(vm.global()), "escape");
		std::vector<std::string> names;
		c.thisObject->ownEnumerableNames(names);
		std::string out;
		for (const std::string& name : names) {
			const Value value = vm.getMember(Value(c.thisObject), name);
			if (value.isFunction()) {
				continue;
			}
			out += (out.empty() ? "" : "&") + vm.toString(vm.call(escape, nullptr, {Value(name)})) + "=" +
				vm.toString(vm.call(escape, nullptr, {value}));
		}
		return Value(out);
	});
}

void installXml(Interpreter& vm)
{
	// XML/XMLNode: no parser; documents stay empty.
	for (const char* name : {"XMLNode", "XML"}) {
		auto ctor = defineClass(vm, name, [](Interpreter& vm, CallArgs& c) -> Value {
			if (c.constructing && c.thisObject) {
				c.thisObject->setMember(vm, "childNodes", Value(vm.newArray()));
				c.thisObject->setMember(vm, "firstChild", Value::null());
				c.thisObject->setMember(vm, "attributes", Value(vm.newObject()));
			}
			return Value();
		});
		const ObjectRef proto = prototypeOf(vm, ctor);
		stubMethods(vm, proto, {"appendChild", "insertBefore", "removeNode", "parseXML", "addRequestHeader"});
		stubMethods(vm, proto, {"load", "send", "sendAndLoad", "hasChildNodes"}, Value(false));
		stubMethods(vm, proto, {"cloneNode", "createElement", "createTextNode"}, Value::null());
		stubMethods(vm, proto, {"getBytesLoaded", "getBytesTotal"}, Value(0));
		stubMethods(vm, proto, {"toString"}, Value(""));
		proto->define("ignoreWhite", Value(false));
		proto->define("loaded", Value(false));
		proto->define("status", Value(0));
	}
}

void installMedia(Interpreter& vm)
{
	auto sound = defineClass(vm, "Sound");
	stubMethods(vm, prototypeOf(vm, sound), {"attachSound", "start", "stop", "setVolume", "setPan", "setTransform", "loadSound"});
	stubMethods(vm, prototypeOf(vm, sound), {"getVolume"}, Value(100));
	stubMethods(vm, prototypeOf(vm, sound), {"getPan", "getBytesLoaded", "getBytesTotal"}, Value(0));
	stubMethods(vm, prototypeOf(vm, sound), {"getTransform"}, Value::null());

	auto color = defineClass(vm, "Color");
	stubMethods(vm, prototypeOf(vm, color), {"setRGB", "setTransform"});
	stubMethods(vm, prototypeOf(vm, color), {"getRGB"}, Value(0));
	vm.defineMethod(prototypeOf(vm, color), "getTransform", [](Interpreter& vm, CallArgs&) -> Value {
		const ObjectRef t = vm.newObject();
		for (const char* f : {"ra", "ga", "ba", "aa"}) {
			t->define(f, Value(100));
		}
		for (const char* f : {"rb", "gb", "bb", "ab"}) {
			t->define(f, Value(0));
		}
		return Value(t);
	});

	for (const char* name : {"Camera", "Microphone", "Video", "NetConnection", "NetStream"}) {
		auto ctor = defineClass(vm, name);
		stubMethods(vm, prototypeOf(vm, ctor), {"connect", "play", "close", "attachVideo", "clear", "pause", "seek", "setBufferTime"});
	}
}

void installStage(Interpreter& vm)
{
	const ObjectRef stage = broadcaster(vm, "Stage");
	stage->define("width", Value(800));
	stage->define("height", Value(600));
	stage->define("scaleMode", Value("showAll"));
	stage->define("align", Value(""));
	stage->define("showMenu", Value(false));

	const ObjectRef mouse = broadcaster(vm, "Mouse");
	stubMethods(vm, mouse, {"show", "hide"}, Value(0));

	const ObjectRef selection = broadcaster(vm, "Selection");
	stubMethods(vm, selection, {"getFocus"}, Value::null());
	stubMethods(vm, selection, {"setFocus"}, Value(false));
	stubMethods(vm, selection, {"setSelection"});
	stubMethods(vm, selection, {"getBeginIndex", "getEndIndex", "getCaretIndex"}, Value(-1));

	const ObjectRef accessibility = vm.newObject();
	stubMethods(vm, accessibility, {"isActive"}, Value(false));
	stubMethods(vm, accessibility, {"updateProperties"});
	defineGlobal(vm, "Accessibility", Value(accessibility));
}

void installMisc(Interpreter& vm)
{
	auto sharedObject = defineClass(vm, "SharedObject");
	vm.defineMethod(sharedObject, "getLocal", [](Interpreter& vm, CallArgs&) -> Value {
		const ObjectRef so = vm.newObject();
		so->define("data", Value(vm.newObject()));
		stubMethods(vm, so, {"flush"}, Value(true));
		stubMethods(vm, so, {"clear", "close"});
		stubMethods(vm, so, {"getSize"}, Value(0));
		return Value(so);
	});

	auto localConnection = defineClass(vm, "LocalConnection");
	stubMethods(vm, prototypeOf(vm, localConnection), {"connect", "send"}, Value(false));
	stubMethods(vm, prototypeOf(vm, localConnection), {"close"});
	stubMethods(vm, prototypeOf(vm, localConnection), {"domain"}, Value("localhost"));

	auto loader = defineClass(vm, "MovieClipLoader", [](Interpreter& vm, CallArgs& c) -> Value {
		if (c.constructing && c.thisObject) {
			initializeBroadcaster(vm, c.thisObject);
		}
		return Value();
	});
	stubMethods(vm, prototypeOf(vm, loader), {"loadClip", "unloadClip"}, Value(false));
	stubMethods(vm, prototypeOf(vm, loader), {"getProgress"}, Value::null());

	auto printJob = defineClass(vm, "PrintJob");
	stubMethods(vm, prototypeOf(vm, printJob), {"start", "addPage"}, Value(false));
	stubMethods(vm, prototypeOf(vm, printJob), {"send"});

	for (const char* name : {"ContextMenu", "ContextMenuItem"}) {
		auto ctor = defineClass(vm, name, [](Interpreter& vm, CallArgs& c) -> Value {
			if (c.constructing && c.thisObject) {
				c.thisObject->setMember(vm, "customItems", Value(vm.newArray()));
				if (c.size() > 0) {
					c.thisObject->setMember(vm, "caption", c[0]);
				}
			}
			return Value();
		});
		stubMethods(vm, prototypeOf(vm, ctor), {"hideBuiltInItems"});
		stubMethods(vm, prototypeOf(vm, ctor), {"copy"}, Value::null());
	}
}

} // namespace

void installStubs(Interpreter& vm)
{
	// Display objects: classes for scripts to extend; the host provides the
	// methods and makes the instances.
	for (const char* name : {"MovieClip", "TextField", "Button", "TextSnapshot"}) {
		defineClass(vm, name);
	}
	installKey(vm);
	installTextFormat(vm);
	installLoadVars(vm);
	installXml(vm);
	installMedia(vm);
	installStage(vm);
	installMisc(vm);
}

} // namespace dice::hfe::ui::avm1::builtins
