// The bytecode loop. An action is one opcode byte; opcodes 0x80 and up
// carry a u16 length and that many bytes of payload. Code regions (a
// function body, a try or with block) run as nested calls of run() over
// their byte range: a jump out of the range hands control back to the
// enclosing region, which is how break/continue/return unwind through
// try/finally and with.
#include "Interpreter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_set>

namespace dice::hfe::ui::avm1 {

// One activation of a block or function.
struct Frame {
	std::shared_ptr<const ActionBytes> code;
	std::shared_ptr<const ConstantPool> constants;
	std::vector<Value> stack;
	std::vector<Value> registers;
	// Searched last to first, then _global. A block's chain starts with its
	// timeline; a function's is its closure plus its activation object.
	// With pushes onto it.
	std::vector<ObjectRef> scope;
	std::vector<ObjectRef> withObjects; // the With blocks' objects on `scope`
	ObjectRef activation; // locals (null outside functions)
	ObjectRef target;     // SetTarget's timeline
	ObjectRef originalTarget;
	ObjectRef thisObject;
	ObjectRef superHome;
	Value returnValue;
	size_t pc = 0; // where execution continues after a region is left
	bool inFunction = false;
};

namespace {

// Stack and string sizes beyond which a script is considered broken.
constexpr size_t kMaxStack = size_t(1) << 20;
constexpr size_t kMaxRegisters = 256;

// Bounds-checked reads from an action's payload.
class ActionReader {
public:
	ActionReader(const uint8_t* data, size_t begin, size_t end) : m_data(data), m_at(begin), m_end(end) {}

	bool ok() const { return m_ok; }
	bool atEnd() const { return m_at >= m_end; }

	uint8_t u8() { return need(1) ? m_data[m_at++] : 0; }
	uint16_t u16()
	{
		if (!need(2)) {
			return 0;
		}
		const uint16_t v = static_cast<uint16_t>(m_data[m_at] | m_data[m_at + 1] << 8);
		m_at += 2;
		return v;
	}
	int16_t s16() { return static_cast<int16_t>(u16()); }
	uint32_t u32()
	{
		const uint32_t lo = u16();
		return lo | static_cast<uint32_t>(u16()) << 16;
	}
	float f32()
	{
		const uint32_t bits = u32();
		float f;
		std::memcpy(&f, &bits, sizeof f);
		return f;
	}
	// Push's doubles store the high 32-bit word first, each little-endian.
	double f64()
	{
		const uint64_t hi = u32();
		const uint64_t lo = u32();
		const uint64_t bits = hi << 32 | lo;
		double d;
		std::memcpy(&d, &bits, sizeof d);
		return d;
	}
	std::string string()
	{
		const size_t start = m_at;
		while (m_at < m_end && m_data[m_at] != 0) {
			++m_at;
		}
		if (m_at >= m_end) {
			m_ok = false;
			return {};
		}
		return std::string(reinterpret_cast<const char*>(m_data + start), m_at++ - start);
	}

private:
	bool need(size_t n)
	{
		if (m_at + n > m_end) {
			m_ok = false;
			return false;
		}
		return true;
	}

	const uint8_t* m_data;
	size_t m_at;
	size_t m_end;
	bool m_ok = true;
};

Value& reg(Frame& f, size_t n)
{
	if (n >= f.registers.size()) {
		f.registers.resize(std::min(n + 1, kMaxRegisters));
	}
	return f.registers[std::min(n, kMaxRegisters - 1)];
}

std::string codeUnitToUtf8(uint32_t code)
{
	if (code == 0) {
		return {};
	}
	if (code >= 0x10000 && code <= 0x10ffff) {
		code -= 0x10000;
		return utf16ToUtf8({static_cast<char16_t>(0xd800 + (code >> 10)), static_cast<char16_t>(0xdc00 + (code & 0x3ff))});
	}
	return utf16ToUtf8(std::u16string(1, static_cast<char16_t>(code & 0xffff)));
}

// StringExtract: `count` UTF-16 units from 1-based `index`.
std::string extract(const std::string& s, double index, double count)
{
	const std::u16string text = utf8ToUtf16(s);
	const double size = static_cast<double>(text.size());
	double from = toInteger(index) - 1;
	if (from < 0) {
		from = 0;
	}
	if (from >= size) {
		return {};
	}
	double n = toInteger(count);
	if (n < 0 || from + n > size) {
		n = size - from;
	}
	return utf16ToUtf8(text.substr(static_cast<size_t>(from), static_cast<size_t>(n)));
}

// Skips `count` whole actions from `at` (WaitForFrame).
size_t skipActions(const ActionBytes& code, size_t at, int count)
{
	for (int i = 0; i < count && at < code.size(); ++i) {
		if (code[at] & 0x80) {
			if (at + 3 > code.size()) {
				return code.size();
			}
			at += 3 + static_cast<size_t>(code[at + 1] | code[at + 2] << 8);
		} else {
			++at;
		}
	}
	return std::min(at, code.size());
}

} // namespace

// --- Entry

bool Interpreter::execute(const uint8_t* actions, size_t length, const ExecutionContext& context)
{
	auto code = std::make_shared<const ActionBytes>(actions, actions + length);
	return guarded([&] {
		Frame f;
		f.code = code;
		f.constants = std::make_shared<const ConstantPool>();
		f.target = f.originalTarget = context.target ? context.target : m_global;
		f.thisObject = context.thisObject ? context.thisObject : f.target;
		f.scope.push_back(f.target);
		f.registers.resize(4);
		run(f, 0, code->size());
	});
}

Value Interpreter::runFunction(ScriptFunction& fn, CallArgs& args)
{
	if (!fn.code) {
		return Value();
	}
	Frame f;
	f.code = fn.code;
	f.constants = fn.constants;
	f.inFunction = true;
	f.target = f.originalTarget = fn.target ? fn.target : m_global;
	f.thisObject = args.thisObject;
	f.superHome = args.superHome;
	f.activation = make<Object>();
	f.scope = fn.scope;
	// Called as a movie clip's method (row.onRelease = _parent.rowRelease),
	// the clip is the target, as in gameswf: its variables come before the
	// defining timeline's, and _parent/_root are the clip's.
	if (args.thisObject && std::strcmp(args.thisObject->typeName(), "movieclip") == 0) {
		f.target = f.originalTarget = args.thisObject;
		f.scope.push_back(args.thisObject);
	}
	f.scope.push_back(f.activation);
	f.registers.resize(fn.isFunction2 ? std::max<size_t>(fn.registerCount, 1) : 4);

	const uint16_t flags = fn.isFunction2 ? fn.flags : 0;
	ObjectRef arguments;
	if (!(flags & ScriptFunction::kSuppressArguments) || (flags & ScriptFunction::kPreloadArguments)) {
		auto array = newArray(args.args);
		array->define("callee", Value(fn.shared_from_this()), kDontEnum);
		array->define("caller", Value::null(), kDontEnum);
		arguments = array;
	}
	if (fn.isFunction2) {
		// Preloaded values take registers 1, 2, ... in this order.
		size_t r = 1;
		if (flags & ScriptFunction::kPreloadThis) {
			reg(f, r++) = f.thisObject ? Value(f.thisObject) : Value();
		}
		if (flags & ScriptFunction::kPreloadArguments) {
			reg(f, r++) = Value(arguments);
		}
		if (flags & ScriptFunction::kPreloadSuper) {
			reg(f, r++) = superObject(f);
		}
		if (flags & ScriptFunction::kPreloadRoot) {
			reg(f, r++) = Value(rootOf(f.target));
		}
		if (flags & ScriptFunction::kPreloadParent) {
			reg(f, r++) = getMember(Value(f.target), "_parent");
		}
		if (flags & ScriptFunction::kPreloadGlobal) {
			reg(f, r++) = Value(m_global);
		}
	}
	if (arguments && !(flags & ScriptFunction::kPreloadArguments)) {
		f.activation->define("arguments", Value(arguments));
	}
	for (size_t i = 0; i < fn.parameters.size(); ++i) {
		const ScriptFunction::Parameter& p = fn.parameters[i];
		if (fn.isFunction2 && p.reg != 0) {
			reg(f, p.reg) = args[i];
		} else {
			f.activation->define(p.name, args[i]);
		}
	}
	const Flow flow = run(f, fn.begin, fn.end);
	return flow == Flow::Return ? f.returnValue : Value();
}

// --- Variables

bool Interpreter::specialName(Frame& f, const std::string& name, Value& out)
{
	if (name.empty() || name[0] == '_') {
		if (name == "_global") {
			out = Value(m_global);
			return true;
		}
		if (name == "_root") {
			out = Value(rootOf(f.target));
			return true;
		}
		if (name.size() > 6 && name.compare(0, 6, "_level") == 0) {
			const double n = stringToNumber(name.substr(6));
			if (!std::isnan(n)) {
				out = Value(m_host->level(toInt32(n)));
				return true;
			}
		}
		return false;
	}
	if (name == "this") {
		out = f.thisObject ? Value(f.thisObject) : Value();
		return true;
	}
	if (name == "super") {
		out = superObject(f);
		return true;
	}
	return false;
}

bool Interpreter::lookupScope(Frame& f, const std::string& name, Value& out, ObjectRef* holder)
{
	for (size_t i = f.scope.size(); i-- > 0;) {
		const ObjectRef o = f.scope[i];
		if (o && o->getMember(*this, name, out)) {
			if (holder) {
				*holder = o;
			}
			return true;
		}
	}
	if (m_global->getMember(*this, name, out)) {
		if (holder) {
			*holder = m_global;
		}
		return true;
	}
	return false;
}

Value Interpreter::getVariable(Frame& f, const std::string& name)
{
	if (name.empty()) {
		return Value();
	}
	// SWF 4 target paths: "/a/b:x", "../:x", ":x"; "/a/b" alone is the clip.
	if (name.find_first_of(":/") != std::string::npos) {
		const size_t colon = name.rfind(':');
		if (colon == std::string::npos) {
			return Value(resolveTarget(f.target, name));
		}
		const ObjectRef clip = colon == 0 ? f.target : resolveTarget(f.target, name.substr(0, colon));
		return clip ? getMember(Value(clip), name.substr(colon + 1)) : Value();
	}
	// Dot paths: "a.b.c", the first part found like any variable.
	if (name.find('.') != std::string::npos) {
		size_t at = name.find('.');
		Value v = getVariable(f, name.substr(0, at));
		while (at < name.size() && !v.isNullish()) {
			const size_t stop = std::min(name.find('.', at + 1), name.size());
			v = getMember(v, name.substr(at + 1, stop - at - 1));
			at = stop;
		}
		return at >= name.size() ? v : Value();
	}
	Value out;
	if (specialName(f, name, out) || lookupScope(f, name, out)) {
		return out;
	}
	return Value();
}

void Interpreter::setVariable(Frame& f, const std::string& name, const Value& value)
{
	if (name.empty()) {
		return;
	}
	const size_t split = name.find_first_of(":/") != std::string::npos ? name.find_last_of(":/") : name.rfind('.');
	if (split != std::string::npos) {
		const std::string path = name.substr(0, split);
		ObjectRef holder;
		if (name[split] == '.') {
			holder = getVariable(f, path).object();
		} else {
			holder = path.empty() ? f.target : resolveTarget(f.target, path);
		}
		if (holder) {
			holder->setMember(*this, name.substr(split + 1), value);
		}
		return;
	}
	// An existing variable is assigned where it lives; a new one goes to
	// the timeline (never to _global or a function's locals).
	for (size_t i = f.scope.size(); i-- > 0;) {
		const ObjectRef& o = f.scope[i];
		if (o && (o->hasOwnMember(name) || o->findOwner(name))) {
			o->setMember(*this, name, value);
			return;
		}
	}
	f.target->setMember(*this, name, value);
}

Value Interpreter::superObject(Frame& f)
{
	ObjectRef home = f.superHome;
	if (!home && f.thisObject) {
		home = f.thisObject->proto();
	}
	return Value(make<SuperObject>(f.thisObject, home));
}

Value Interpreter::callFunctionByName(Frame& f, const std::string& name, std::vector<Value> args)
{
	// "a.b.f()" written as a path calls f with `this` = a.b.
	const size_t dot = name.rfind('.');
	if (dot != std::string::npos && name.find_first_of(":/") == std::string::npos) {
		return callMember(getVariable(f, name.substr(0, dot)), name.substr(dot + 1), std::move(args));
	}
	// A function found on a With block's object is called on that object
	// (with (clip) { beginFill(...); } draws into the clip).
	if (name.find_first_of(":/") == std::string::npos) {
		Value fn;
		ObjectRef holder;
		if (!specialName(f, name, fn) && lookupScope(f, name, fn, &holder)
			&& std::find(f.withObjects.begin(), f.withObjects.end(), holder) != f.withObjects.end()) {
			return call(fn, holder, std::move(args));
		}
	}
	const Value fn = getVariable(f, name);
	return call(fn, f.thisObject, std::move(args));
}

// --- Movie clip targets

ObjectRef Interpreter::targetOf(Frame& f, const Value& target)
{
	if (target.isObject()) {
		return target.asObject();
	}
	const std::string path = toString(target);
	return path.empty() ? f.target : resolveTarget(f.target, path);
}

void Interpreter::setTarget(Frame& f, const Value& target)
{
	ObjectRef clip;
	if (target.isObject()) {
		clip = target.asObject();
	} else {
		const std::string path = toString(target);
		clip = path.empty() ? f.originalTarget : resolveTarget(f.originalTarget, path);
	}
	f.target = clip ? clip : f.originalTarget;
	if (!f.inFunction && !f.scope.empty()) {
		f.scope[0] = f.target;
	}
}

void Interpreter::gotoFrameExpression(Frame& f, const Value& frameValue, bool play, int bias)
{
	ObjectRef clip = f.target;
	std::string frame;
	if (frameValue.isNumber()) {
		const double n = toInteger(frameValue.asNumber());
		m_host->gotoFrame(clip, toInt32(n) - 1 + bias, play);
		return;
	}
	frame = toString(frameValue);
	const size_t colon = frame.rfind(':');
	if (colon != std::string::npos) {
		clip = resolveTarget(f.target, frame.substr(0, colon));
		frame = frame.substr(colon + 1);
	}
	if (!clip) {
		return;
	}
	const double n = stringToNumber(frame);
	if (!std::isnan(n)) {
		m_host->gotoFrame(clip, toInt32(n) - 1 + bias, play);
	} else {
		m_host->gotoLabel(clip, frame, play);
	}
}

// --- for..in

void Interpreter::enumerate(Frame& f, const Value& object)
{
	f.stack.push_back(Value::null()); // end marker
	const ObjectRef start = object.object();
	if (!start) {
		return;
	}
	// Names pop newest first, own before inherited, each once; a name seen
	// closer to the object (even dontEnum) hides the same name further up.
	std::vector<std::vector<std::string>> levels;
	std::unordered_set<std::string> seen;
	Object* o = start.get();
	for (int depth = 0; o && depth < 256; ++depth, o = o->proto().get()) {
		std::vector<std::string> names;
		o->ownEnumerableNames(names);
		std::vector<std::string> fresh;
		for (std::string& name : names) {
			if (seen.insert(name).second) {
				fresh.push_back(std::move(name));
			}
		}
		for (const auto& entry : o->properties().entries()) {
			seen.insert(entry.first);
		}
		levels.push_back(std::move(fresh));
	}
	for (size_t i = levels.size(); i-- > 0;) {
		for (std::string& name : levels[i]) {
			if (f.stack.size() >= kMaxStack) {
				abort("stack overflow in for..in");
			}
			f.stack.push_back(Value(std::move(name)));
		}
	}
}

// --- Functions, try, with

std::shared_ptr<ScriptFunction> Interpreter::defineFunction(Frame& f, size_t at, size_t length, bool function2, size_t& next)
{
	ActionReader r(f.code->data(), at, at + length);
	auto fn = make<ScriptFunction>(m_protos.function);
	fn->isFunction2 = function2;
	fn->name = r.string();
	const uint16_t count = r.u16();
	if (function2) {
		fn->registerCount = r.u8();
		fn->flags = r.u16();
	}
	for (uint16_t i = 0; i < count && r.ok(); ++i) {
		ScriptFunction::Parameter p;
		if (function2) {
			p.reg = r.u8();
		}
		p.name = r.string();
		fn->parameters.push_back(std::move(p));
	}
	const size_t codeSize = r.u16();
	if (!r.ok()) {
		abort("malformed DefineFunction");
	}
	fn->code = f.code;
	fn->begin = at + length;
	fn->end = fn->begin + codeSize;
	if (fn->end > f.code->size()) {
		abort("DefineFunction body runs past the end of the block");
	}
	fn->constants = f.constants;
	fn->scope = f.scope;
	fn->target = f.target;
	const ObjectRef proto = newObject();
	proto->define("constructor", Value(fn), kDontEnum);
	fn->define("prototype", Value(proto), kDontEnum);
	next = fn->end;
	return fn;
}

Interpreter::Flow Interpreter::runWith(Frame& f, const ObjectRef& object, size_t begin, size_t end)
{
	if (!object) {
		return run(f, begin, end);
	}
	struct Pop {
		Frame& f;
		size_t depth, withDepth;
		~Pop()
		{
			f.scope.resize(depth);
			f.withObjects.resize(withDepth);
		}
	} pop {f, f.scope.size(), f.withObjects.size()};
	f.scope.push_back(object);
	f.withObjects.push_back(object);
	return run(f, begin, end);
}

Interpreter::Flow Interpreter::runTry(Frame& f, size_t at, size_t length, size_t& next)
{
	ActionReader r(f.code->data(), at, at + length);
	const uint8_t flags = r.u8();
	const size_t trySize = r.u16();
	const size_t catchSize = r.u16();
	const size_t finallySize = r.u16();
	const bool catchInRegister = flags & 4;
	const bool hasFinally = flags & 2;
	const bool hasCatch = flags & 1;
	uint8_t catchRegister = 0;
	std::string catchName;
	if (catchInRegister) {
		catchRegister = r.u8();
	} else {
		catchName = r.string();
	}
	const size_t tryBegin = at + length;
	const size_t catchBegin = tryBegin + trySize;
	const size_t finallyBegin = catchBegin + catchSize;
	const size_t after = finallyBegin + finallySize;
	if (!r.ok() || after > f.code->size()) {
		abort("malformed Try");
	}

	const size_t stackDepth = f.stack.size();
	Flow flow = Flow::Leave;
	size_t continuation = after;
	// Leaving the try or catch body into the handler area (falling off the
	// end, or a jump to the finally block) continues after the statement.
	const auto settle = [&](Flow result) {
		flow = result;
		if (result == Flow::Leave) {
			continuation = f.pc >= catchBegin && f.pc <= after ? after : f.pc;
		}
	};
	std::optional<Value> pending;
	try {
		settle(run(f, tryBegin, catchBegin));
	} catch (const ScriptThrow& thrown) {
		if (f.stack.size() > stackDepth) {
			f.stack.resize(stackDepth);
		}
		if (hasCatch) {
			if (catchInRegister) {
				reg(f, catchRegister) = thrown.value;
			} else if (f.activation) {
				f.activation->define(catchName, thrown.value);
			} else {
				setVariable(f, catchName, thrown.value);
			}
			try {
				settle(run(f, catchBegin, finallyBegin));
			} catch (const ScriptThrow& again) {
				if (f.stack.size() > stackDepth) {
					f.stack.resize(stackDepth);
				}
				pending = again.value;
			}
		} else {
			pending = thrown.value;
		}
	}
	if (hasFinally && finallySize > 0) {
		const Value returning = f.returnValue;
		if (run(f, finallyBegin, after) == Flow::Return) {
			return Flow::Return; // return in finally wins over everything
		}
		if (f.pc != after) {
			next = f.pc; // so does a jump out (break)
			return Flow::Leave;
		}
		f.returnValue = returning;
	}
	if (pending) {
		throw ScriptThrow {*pending};
	}
	next = continuation;
	return flow;
}

// --- The loop

Interpreter::Flow Interpreter::run(Frame& f, size_t begin, size_t end)
{
	const ActionBytes& bytes = *f.code;
	const uint8_t* code = bytes.data();
	const size_t size = bytes.size();
	end = std::min(end, size);

	const auto pop = [&f]() -> Value {
		if (f.stack.empty()) {
			return Value(); // underflow reads undefined, as in the player
		}
		Value v = std::move(f.stack.back());
		f.stack.pop_back();
		return v;
	};
	const auto push = [&](Value v) {
		if (f.stack.size() >= kMaxStack) {
			abort("stack overflow");
		}
		f.stack.push_back(std::move(v));
	};
	const auto popNumber = [&] { return toNumber(pop()); };
	const auto popString = [&] { return toString(pop()); };
	// An argument count then the arguments, first argument on top.
	const auto popArgs = [&] {
		const double n = popNumber();
		std::vector<Value> args;
		const size_t count = n > 0 ? static_cast<size_t>(std::min(n, static_cast<double>(f.stack.size()))) : 0;
		args.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			args.push_back(pop());
		}
		return args;
	};

	size_t pc = begin;
	while (pc >= begin && pc < end) {
		if (m_remaining == 0) {
			abort("instruction budget exhausted");
		}
		--m_remaining;

		const uint8_t op = code[pc];
		if (op == 0x00) { // End
			f.pc = size;
			return Flow::Return;
		}
		size_t payload = pc + 1;
		size_t length = 0;
		if (op & 0x80) {
			if (pc + 3 > size) {
				abort("truncated action");
			}
			length = static_cast<size_t>(code[pc + 1] | code[pc + 2] << 8);
			payload = pc + 3;
			if (payload + length > size) {
				abort("action runs past the end of the block");
			}
		}
		size_t next = payload + length;
		ActionReader r(code, payload, next);

		switch (op) {
		// --- SWF 3 timeline control
		case 0x04: m_host->nextFrame(f.target); break;
		case 0x05: m_host->prevFrame(f.target); break;
		case 0x06: m_host->play(f.target); break;
		case 0x07: m_host->stop(f.target); break;
		case 0x08: m_host->toggleQuality(); break;
		case 0x09: m_host->stopSounds(); break;

		// --- SWF 4 arithmetic, numeric comparison, logic
		case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f: {
			const double b = popNumber();
			const double a = popNumber();
			switch (op) {
			case 0x0a: push(Value(a + b)); break;
			case 0x0b: push(Value(a - b)); break;
			case 0x0c: push(Value(a * b)); break;
			case 0x0d: push(Value(a / b)); break;
			case 0x0e: push(Value(a == b)); break;
			default: push(Value(a < b)); break;
			}
			break;
		}
		case 0x10: case 0x11: { // And, Or
			const bool b = toBoolean(pop());
			const bool a = toBoolean(pop());
			push(Value(op == 0x10 ? a && b : a || b));
			break;
		}
		case 0x12: push(Value(!toBoolean(pop()))); break; // Not

		// --- SWF 4 strings
		case 0x13: case 0x29: case 0x68: { // StringEquals, StringLess, StringGreater
			const std::string b = popString();
			const std::string a = popString();
			push(Value(op == 0x13 ? a == b : op == 0x29 ? a < b : a > b));
			break;
		}
		case 0x14: case 0x31: push(Value(utf16Length(popString()))); break; // StringLength, MBStringLength
		case 0x15: case 0x35: { // StringExtract, MBStringExtract
			const double count = popNumber();
			const double index = popNumber();
			push(Value(extract(popString(), index, count)));
			break;
		}
		case 0x21: { // StringAdd
			const std::string b = popString();
			std::string a = popString();
			if (a.size() + b.size() > (size_t(1) << 26)) {
				abort("string too long");
			}
			push(Value(a + b));
			break;
		}
		case 0x32: case 0x36: { // CharToAscii, MBCharToAscii
			const std::u16string s = utf8ToUtf16(popString());
			uint32_t c = s.empty() ? 0 : s[0];
			if (op == 0x36 && s.size() > 1 && c >= 0xd800 && c < 0xdc00 && s[1] >= 0xdc00 && s[1] < 0xe000) {
				c = 0x10000 + ((c - 0xd800) << 10) + (s[1] - 0xdc00);
			}
			push(Value(c));
			break;
		}
		case 0x33: case 0x37: push(Value(codeUnitToUtf8(toUint32(popNumber())))); break; // AsciiToChar, MBAsciiToChar

		// --- Stack and conversions
		case 0x17: pop(); break;
		case 0x18: push(Value(toInt32(popNumber()))); break; // ToInteger (int())
		case 0x4a: push(Value(popNumber())); break;
		case 0x4b: push(Value(popString())); break;
		case 0x4c: push(f.stack.empty() ? Value() : f.stack.back()); break;
		case 0x4d: { // StackSwap
			Value a = pop();
			Value b = pop();
			push(std::move(a));
			push(std::move(b));
			break;
		}
		case 0x44: { // TypeOf
			const Value v = pop();
			push(Value(typeOf(v)));
			break;
		}
		case 0x87: { // StoreRegister
			const uint8_t n = r.u8();
			reg(f, n) = f.stack.empty() ? Value() : f.stack.back();
			break;
		}
		case 0x88: { // ConstantPool
			auto pool = std::make_shared<ConstantPool>();
			const uint16_t count = r.u16();
			pool->reserve(count);
			for (uint16_t i = 0; i < count && r.ok(); ++i) {
				pool->push_back(r.string());
			}
			f.constants = pool;
			break;
		}
		case 0x96: // Push
			while (!r.atEnd() && r.ok()) {
				const uint8_t type = r.u8();
				switch (type) {
				case 0: push(Value(r.string())); break;
				case 1: push(Value(static_cast<double>(r.f32()))); break;
				case 2: push(Value::null()); break;
				case 3: push(Value()); break;
				case 4: push(reg(f, r.u8())); break;
				case 5: push(Value(r.u8() != 0)); break;
				case 6: push(Value(r.f64())); break;
				case 7: push(Value(static_cast<int32_t>(r.u32()))); break;
				case 8:
				case 9: {
					const size_t i = type == 8 ? r.u8() : r.u16();
					push(i < f.constants->size() ? Value((*f.constants)[i]) : Value());
					break;
				}
				default: abort("unknown Push type " + std::to_string(type));
				}
			}
			break;

		// --- SWF 5 operators
		case 0x47: { // Add2
			const Value b = pop();
			const Value a = pop();
			Value sum = add(a, b);
			if (sum.isString() && sum.asString().size() > (size_t(1) << 26)) {
				abort("string too long");
			}
			push(std::move(sum));
			break;
		}
		case 0x48: case 0x67: { // Less2, Greater
			const Value b = pop();
			const Value a = pop();
			push(op == 0x48 ? less(a, b) : less(b, a));
			break;
		}
		case 0x49: case 0x66: { // Equals2, StrictEquals
			const Value b = pop();
			const Value a = pop();
			push(Value(op == 0x49 ? equals(a, b) : strictEquals(a, b)));
			break;
		}
		case 0x3f: { // Modulo
			const double b = popNumber();
			const double a = popNumber();
			push(Value(std::fmod(a, b)));
			break;
		}
		case 0x50: push(Value(popNumber() + 1)); break; // Increment
		case 0x51: push(Value(popNumber() - 1)); break; // Decrement
		case 0x60: case 0x61: case 0x62: { // BitAnd, BitOr, BitXor
			const int32_t b = toInt32(popNumber());
			const int32_t a = toInt32(popNumber());
			push(Value(op == 0x60 ? a & b : op == 0x61 ? a | b : a ^ b));
			break;
		}
		case 0x63: case 0x64: case 0x65: { // BitLShift, BitRShift, BitURShift
			const uint32_t count = toUint32(popNumber()) & 31;
			const double value = popNumber();
			if (op == 0x63) {
				push(Value(static_cast<int32_t>(toUint32(value) << count)));
			} else if (op == 0x64) {
				const int32_t v = toInt32(value);
				push(Value(v < 0 ? ~(~v >> count) : v >> count));
			} else {
				push(Value(toUint32(value) >> count));
			}
			break;
		}

		// --- Variables and members
		case 0x1c: { // GetVariable
			const std::string name = popString();
			push(getVariable(f, name));
			break;
		}
		case 0x1d: { // SetVariable
			const Value value = pop();
			setVariable(f, popString(), value);
			break;
		}
		case 0x3c: { // DefineLocal
			const Value value = pop();
			const std::string name = popString();
			if (f.activation) {
				f.activation->define(name, value);
			} else {
				f.target->setMember(*this, name, value);
			}
			break;
		}
		case 0x41: { // DefineLocal2
			const std::string name = popString();
			const ObjectRef& holder = f.activation ? f.activation : f.target;
			if (!holder->hasOwnMember(name)) {
				if (f.activation) {
					holder->define(name, Value());
				} else {
					holder->setMember(*this, name, Value());
				}
			}
			break;
		}
		case 0x3a: { // Delete
			const std::string name = popString();
			const Value object = pop();
			push(Value(object.isObject() && object.asObject()->deleteMember(*this, name)));
			break;
		}
		case 0x3b: { // Delete2
			const std::string name = popString();
			ObjectRef holder = m_global;
			for (size_t i = f.scope.size(); i-- > 0;) {
				if (f.scope[i] && f.scope[i]->hasOwnMember(name)) {
					holder = f.scope[i];
					break;
				}
			}
			push(Value(holder->deleteMember(*this, name)));
			break;
		}
		case 0x4e: { // GetMember
			const std::string name = popString();
			const Value object = pop();
			push(getMember(object, name));
			break;
		}
		case 0x4f: { // SetMember
			const Value value = pop();
			const std::string name = popString();
			setMember(pop(), name, value);
			break;
		}
		case 0x42: { // InitArray
			const double n = popNumber();
			const size_t count = n > 0 ? static_cast<size_t>(std::min(n, static_cast<double>(f.stack.size()))) : 0;
			std::vector<Value> elements;
			elements.reserve(count);
			for (size_t i = 0; i < count; ++i) {
				elements.push_back(pop());
			}
			push(Value(newArray(std::move(elements))));
			break;
		}
		case 0x43: { // InitObject: pairs, the last one on top
			const double n = popNumber();
			const size_t count = n > 0 ? static_cast<size_t>(std::min(n, static_cast<double>(f.stack.size() / 2))) : 0;
			std::vector<std::pair<std::string, Value>> pairs(count);
			for (size_t i = count; i-- > 0;) {
				pairs[i].second = pop();
				pairs[i].first = popString();
			}
			const ObjectRef object = newObject();
			for (auto& [name, value] : pairs) {
				object->define(name, value);
			}
			push(Value(object));
			break;
		}
		case 0x46: { // Enumerate
			const std::string name = popString();
			enumerate(f, getVariable(f, name));
			break;
		}
		case 0x55: enumerate(f, pop()); break; // Enumerate2

		// --- Calls and objects
		case 0x3d: { // CallFunction
			const std::string name = popString();
			std::vector<Value> args = popArgs();
			push(callFunctionByName(f, name, std::move(args)));
			break;
		}
		case 0x52: { // CallMethod
			const Value name = pop();
			const Value object = pop();
			std::vector<Value> args = popArgs();
			push(callMember(object, name.isNullish() ? std::string() : toString(name), std::move(args)));
			break;
		}
		case 0x40: { // NewObject
			const std::string name = popString();
			std::vector<Value> args = popArgs();
			push(constructWith(getVariable(f, name), std::move(args)));
			break;
		}
		case 0x53: { // NewMethod
			const Value name = pop();
			const Value object = pop();
			std::vector<Value> args = popArgs();
			const std::string method = name.isNullish() ? std::string() : toString(name);
			push(constructWith(method.empty() ? object : getMember(object, method), std::move(args)));
			break;
		}
		case 0x3e: // Return
			f.returnValue = pop();
			f.pc = next;
			return Flow::Return;
		case 0x9b: case 0x8e: { // DefineFunction, DefineFunction2
			const auto fn = defineFunction(f, payload, length, op == 0x8e, next);
			if (fn->name.empty()) {
				push(Value(fn));
			} else if (f.activation) {
				f.activation->define(fn->name, Value(fn));
			} else {
				f.target->setMember(*this, fn->name, Value(fn));
			}
			break;
		}
		case 0x54: { // InstanceOf
			const Value ctor = pop();
			const Value object = pop();
			push(Value(instanceOf(object, ctor)));
			break;
		}
		case 0x2b: { // CastOp
			const Value object = pop();
			const Value ctor = pop();
			push(instanceOf(object, ctor) ? object : Value::null());
			break;
		}
		case 0x2c: { // ImplementsOp
			const Value ctor = pop();
			std::vector<Value> interfaces = popArgs();
			const ObjectRef proto = getMember(ctor, "prototype").object();
			if (proto) {
				proto->interfaces().clear();
				for (const Value& i : interfaces) {
					if (i.isFunction()) {
						proto->interfaces().push_back(i.asObject());
					}
				}
			}
			break;
		}
		case 0x69: { // Extends
			const Value superclass = pop();
			const Value subclass = pop();
			if (subclass.isObject() && superclass.isObject()) {
				const ObjectRef proto = make<Object>(getMember(superclass, "prototype").object());
				proto->define("__constructor__", superclass, kDontEnum);
				proto->define("constructor", superclass, kDontEnum);
				subclass.asObject()->define("prototype", Value(proto), kDontEnum);
			}
			break;
		}
		case 0x2a: // Throw
			throw ScriptThrow {pop()};
		case 0x8f: { // Try
			if (runTry(f, payload, length, next) == Flow::Return) {
				return Flow::Return;
			}
			break;
		}
		case 0x94: { // With
			const size_t bodySize = r.u16();
			const Value object = pop();
			const size_t bodyEnd = std::min(next + bodySize, size);
			if (runWith(f, object.isPrimitive() ? toObject(object) : object.asObject(), next, bodyEnd) == Flow::Return) {
				return Flow::Return;
			}
			next = f.pc;
			break;
		}

		// --- Control flow
		case 0x99: case 0x9d: { // Jump, If
			const int16_t offset = r.s16();
			if (op == 0x9d && !toBoolean(pop())) {
				break;
			}
			const int64_t to = static_cast<int64_t>(next) + offset;
			if (to < 0 || to > static_cast<int64_t>(size)) {
				abort("jump outside the block");
			}
			next = static_cast<size_t>(to);
			break;
		}

		// --- Movie clips (delegated to the host)
		case 0x81: m_host->gotoFrame(f.target, r.u16(), false); break; // GotoFrame
		case 0x8c: m_host->gotoLabel(f.target, r.string(), false); break; // GoToLabel
		case 0x9f: { // GotoFrame2
			const uint8_t flags = r.u8();
			const int bias = (flags & 2) ? r.u16() : 0;
			gotoFrameExpression(f, pop(), flags & 1, bias);
			break;
		}
		case 0x8b: setTarget(f, Value(r.string())); break; // SetTarget
		case 0x20: setTarget(f, pop()); break;            // SetTarget2
		case 0x8a: { // WaitForFrame
			const int frame = r.u16();
			const int skip = r.u8();
			if (r.ok() && !m_host->frameLoaded(f.target, frame)) {
				next = skipActions(bytes, next, skip);
			}
			break;
		}
		case 0x8d: { // WaitForFrame2
			const int skip = r.u8();
			const Value frame = pop();
			if (frame.isNumber() && !m_host->frameLoaded(f.target, toInt32(frame.asNumber()) - 1)) {
				next = skipActions(bytes, next, skip);
			}
			break;
		}
		case 0x9e: { // Call
			std::string frame = popString();
			ObjectRef clip = f.target;
			const size_t colon = frame.rfind(':');
			if (colon != std::string::npos) {
				clip = resolveTarget(f.target, frame.substr(0, colon));
				frame = frame.substr(colon + 1);
			}
			if (clip) {
				m_host->callFrame(clip, frame);
			}
			break;
		}
		case 0x22: { // GetProperty
			const int index = toInt32(popNumber());
			const ObjectRef clip = targetOf(f, pop());
			push(clip || index >= kPropHighQuality ? m_host->getProperty(clip, index) : Value());
			break;
		}
		case 0x23: { // SetProperty
			const Value value = pop();
			const int index = toInt32(popNumber());
			const ObjectRef clip = targetOf(f, pop());
			if (clip || index >= kPropHighQuality) {
				m_host->setProperty(clip, index, value);
			}
			break;
		}
		case 0x45: { // TargetPath
			const Value v = pop();
			const std::string path = v.isObject() ? m_host->targetPath(v.asObject()) : std::string();
			push(path.empty() ? Value() : Value(path));
			break;
		}
		case 0x24: { // CloneSprite
			const int depth = toInt32(popNumber());
			const std::string name = popString();
			const ObjectRef source = targetOf(f, pop());
			if (source) {
				m_host->cloneSprite(source, name, depth);
			}
			break;
		}
		case 0x25: { // RemoveSprite
			if (const ObjectRef clip = targetOf(f, pop())) {
				m_host->removeSprite(clip);
			}
			break;
		}
		case 0x27: { // StartDrag
			const ObjectRef clip = targetOf(f, pop());
			const bool lockCenter = toBoolean(pop());
			const bool constrain = toBoolean(pop());
			double bounds[4] = {};
			if (constrain) {
				bounds[3] = popNumber(); // y2
				bounds[2] = popNumber(); // x2
				bounds[1] = popNumber(); // y1
				bounds[0] = popNumber(); // x1
			}
			if (clip) {
				m_host->startDrag(clip, lockCenter, constrain ? bounds : nullptr);
			}
			break;
		}
		case 0x28: m_host->stopDrag(); break; // EndDrag
		case 0x83: { // GetURL
			const std::string url = r.string();
			const std::string target = r.string();
			if (r.ok()) {
				m_host->getUrl(f.target, url, target, 0, false, false);
			}
			break;
		}
		case 0x9a: { // GetURL2
			const uint8_t flags = r.u8();
			const std::string target = popString();
			const std::string url = popString();
			m_host->getUrl(f.target, url, target, flags >> 6, flags & 2, flags & 1);
			break;
		}

		// --- Utilities
		case 0x26: m_host->trace(popString()); break; // Trace
		case 0x30: { // RandomNumber
			const int32_t max = toInt32(popNumber());
			push(Value(max > 0 ? std::floor(random() * max) : 0.0));
			break;
		}
		case 0x34: { // GetTime
			const double t = m_host->timer();
			push(Value(std::floor(t >= 0 ? t : m_clock)));
			break;
		}

		default: break; // unknown actions are skipped, as the player does
		}
		if (!r.ok()) {
			abort("malformed action 0x" + numberToString(op, 16));
		}
		pc = next;
	}
	f.pc = pc;
	return Flow::Leave;
}

} // namespace dice::hfe::ui::avm1
