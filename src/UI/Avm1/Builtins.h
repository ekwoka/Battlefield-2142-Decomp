// The built-in classes and functions of Flash Player 7, installed into
// _global by the Interpreter's constructor. Internal to the Avm1 library.
#pragma once

#include "Interpreter.h"

namespace dice::hfe::ui::avm1::builtins {

// Object, Function, Error, AsBroadcaster and the global functions
// (parseInt, escape, setInterval, ASSetPropFlags, ...). Builtins.cpp.
void installCore(Interpreter& vm);
// Array. BuiltinsArray.cpp.
void installArray(Interpreter& vm);
// String, Number, Boolean, Math. BuiltinsString.cpp.
void installString(Interpreter& vm);
// Date. BuiltinsDate.cpp.
void installDate(Interpreter& vm);
// The player's other classes (MovieClip, Key, Stage, XML, Sound, ...) as
// harmless stand-ins. BuiltinsStubs.cpp.
void installStubs(Interpreter& vm);

// AsBroadcaster.initialize(object): _listeners, addListener,
// removeListener and broadcastMessage.
void initializeBroadcaster(Interpreter& vm, const ObjectRef& object);

// Defines `name` on _global (dontEnum like the player's own globals).
void defineGlobal(Interpreter& vm, const std::string& name, const Value& value);

// A class whose constructor only keeps `this` (for stubs and plain
// classes the host fills in later), installed as _global[name].
std::shared_ptr<NativeFunction> defineClass(Interpreter& vm, const std::string& name, NativeCallback constructor = {});

} // namespace dice::hfe::ui::avm1::builtins
