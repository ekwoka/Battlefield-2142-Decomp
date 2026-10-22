// The ActionScript 1/2 interpreter: conversions, hand-assembled bytecode
// for the operators, functions, classes and exceptions, the built-in
// classes, and (with $BF2142_GAME_DIR) the class definitions of the
// shipped main menu.
#include "IO/File/ZipArchive.h"
#include "TestMain.h"
#include "UI/Avm1/Interpreter.h"
#include "UI/Dfm.h"
#include "UI/Swf.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>

using namespace dice::hfe;
using namespace dice::hfe::ui::avm1;

namespace {

// Action codes used below.
enum : uint8_t {
	kNot = 0x12,
	kPop = 0x17,
	kToInteger = 0x18,
	kGetVariable = 0x1c,
	kSetVariable = 0x1d,
	kTrace = 0x26,
	kThrow = 0x2a,
	kDefineLocal = 0x3c,
	kCallFunction = 0x3d,
	kReturn = 0x3e,
	kModulo = 0x3f,
	kNewObject = 0x40,
	kInitArray = 0x42,
	kInitObject = 0x43,
	kTypeOf = 0x44,
	kEnumerate2 = 0x55,
	kAdd2 = 0x47,
	kLess2 = 0x48,
	kEquals2 = 0x49,
	kPushDuplicate = 0x4c,
	kGetMember = 0x4e,
	kSetMember = 0x4f,
	kIncrement = 0x50,
	kCallMethod = 0x52,
	kInstanceOf = 0x54,
	kBitURShift = 0x65,
	kStrictEquals = 0x66,
	kExtends = 0x69,
	kConstantPool = 0x88,
	kDefineFunction2 = 0x8e,
	kTry = 0x8f,
	kWith = 0x94,
	kPush = 0x96,
	kJump = 0x99,
	kDefineFunction = 0x9b,
	kIf = 0x9d,
};

struct Null {};
struct Undefined {};
struct Reg {
	uint8_t n;
};
struct Const {
	uint8_t n;
};

// One Push operand.
struct Item {
	uint8_t type;
	std::string s;
	double d = 0;
	int32_t i = 0;
	Item(const char* v) : type(0), s(v) {}
	Item(double v) : type(6), d(v) {}
	Item(int v) : type(7), i(v) {}
	Item(bool v) : type(5), i(v) {}
	Item(Null) : type(2) {}
	Item(Undefined) : type(3) {}
	Item(Reg r) : type(4), i(r.n) {}
	Item(Const c) : type(8), i(c.n) {}
};

// A tiny assembler for test programs.
class Asm {
public:
	std::vector<uint8_t> bytes;

	Asm& op(uint8_t code)
	{
		bytes.push_back(code);
		return *this;
	}
	Asm& action(uint8_t code, const std::vector<uint8_t>& payload)
	{
		bytes.push_back(code);
		put16(bytes, payload.size());
		bytes.insert(bytes.end(), payload.begin(), payload.end());
		return *this;
	}
	Asm& push(std::initializer_list<Item> items)
	{
		std::vector<uint8_t> p;
		for (const Item& item : items) {
			p.push_back(item.type);
			switch (item.type) {
			case 0: putString(p, item.s); break;
			case 4: case 5: case 8: p.push_back(static_cast<uint8_t>(item.i)); break;
			case 7: put32(p, static_cast<uint32_t>(item.i)); break;
			case 6: {
				uint64_t bits;
				std::memcpy(&bits, &item.d, 8);
				put32(p, static_cast<uint32_t>(bits >> 32));
				put32(p, static_cast<uint32_t>(bits));
				break;
			}
			default: break;
			}
		}
		return action(kPush, p);
	}
	Asm& get(const char* name) { return push({name}).op(kGetVariable); }
	Asm& trace() { return op(kTrace); }
	Asm& traceVar(const char* name) { return get(name).trace(); }
	Asm& constants(std::initializer_list<const char*> pool)
	{
		std::vector<uint8_t> p;
		put16(p, pool.size());
		for (const char* s : pool) {
			putString(p, s);
		}
		return action(kConstantPool, p);
	}
	// A forward branch; land() points it at the current end.
	size_t branch(uint8_t code)
	{
		action(code, {0, 0});
		return bytes.size() - 2;
	}
	void land(size_t at)
	{
		const int16_t offset = static_cast<int16_t>(bytes.size() - (at + 2));
		bytes[at] = static_cast<uint8_t>(offset);
		bytes[at + 1] = static_cast<uint8_t>(offset >> 8);
	}
	Asm& jumpBack(uint8_t code, size_t to)
	{
		const int16_t offset = static_cast<int16_t>(static_cast<int64_t>(to) - static_cast<int64_t>(bytes.size() + 5));
		return action(code, {static_cast<uint8_t>(offset), static_cast<uint8_t>(offset >> 8)});
	}
	// DefineFunction2 with (register, name) parameters.
	Asm& function2(const char* name, std::initializer_list<std::pair<uint8_t, const char*>> params, uint8_t registers, uint16_t flags,
		const Asm& body)
	{
		std::vector<uint8_t> p;
		putString(p, name);
		put16(p, params.size());
		p.push_back(registers);
		put16(p, flags);
		for (const auto& [reg, param] : params) {
			p.push_back(reg);
			putString(p, param);
		}
		put16(p, body.bytes.size());
		action(kDefineFunction2, p);
		bytes.insert(bytes.end(), body.bytes.begin(), body.bytes.end());
		return *this;
	}
	Asm& function(const char* name, std::initializer_list<const char*> params, const Asm& body)
	{
		std::vector<uint8_t> p;
		putString(p, name);
		put16(p, params.size());
		for (const char* param : params) {
			putString(p, param);
		}
		put16(p, body.bytes.size());
		action(kDefineFunction, p);
		bytes.insert(bytes.end(), body.bytes.begin(), body.bytes.end());
		return *this;
	}
	// try { tryBody } catch (catchVar) { catchBody } finally { finallyBody }
	Asm& tryBlock(const Asm& tryBody, const char* catchVar, const Asm* catchBody, const Asm* finallyBody)
	{
		std::vector<uint8_t> p;
		p.push_back(static_cast<uint8_t>((catchBody ? 1 : 0) | (finallyBody ? 2 : 0)));
		put16(p, tryBody.bytes.size());
		put16(p, catchBody ? catchBody->bytes.size() : 0);
		put16(p, finallyBody ? finallyBody->bytes.size() : 0);
		putString(p, catchVar);
		action(kTry, p);
		bytes.insert(bytes.end(), tryBody.bytes.begin(), tryBody.bytes.end());
		if (catchBody) {
			bytes.insert(bytes.end(), catchBody->bytes.begin(), catchBody->bytes.end());
		}
		if (finallyBody) {
			bytes.insert(bytes.end(), finallyBody->bytes.begin(), finallyBody->bytes.end());
		}
		return *this;
	}

private:
	static void put16(std::vector<uint8_t>& out, size_t v)
	{
		out.push_back(static_cast<uint8_t>(v));
		out.push_back(static_cast<uint8_t>(v >> 8));
	}
	static void put32(std::vector<uint8_t>& out, uint32_t v)
	{
		put16(out, v & 0xffff);
		put16(out, v >> 16);
	}
	static void putString(std::vector<uint8_t>& out, const std::string& s)
	{
		out.insert(out.end(), s.begin(), s.end());
		out.push_back(0);
	}
};

class TraceHost : public Host {
public:
	std::string output; // trace lines joined by '|'
	std::string errors;
	void trace(const std::string& message) override { output += (output.empty() ? "" : "|") + message; }
	void scriptError(const std::string& message) override { errors += message + "\n"; }
};

// Runs `program` on a fresh interpreter and returns what it traced.
std::string run(const Asm& program, bool* ok = nullptr)
{
	TraceHost host;
	Interpreter vm(&host);
	const bool result = vm.execute(program.bytes.data(), program.bytes.size(), {});
	if (ok) {
		*ok = result;
	}
	if (!result && !ok) {
		std::fprintf(stderr, "script failed: %s\n", vm.lastError().c_str());
	}
	return host.output;
}

} // namespace

TEST(Avm1_PrimitiveConversions)
{
	CHECK(stringToNumber("0x1F") == 31);
	CHECK(stringToNumber("012") == 10); // octal, SWF 6+
	CHECK(stringToNumber("-012") == -10);
	CHECK(stringToNumber("  12") == 12);
	CHECK(std::isnan(stringToNumber("12 ")));
	CHECK(std::isnan(stringToNumber("")));
	CHECK(std::isnan(stringToNumber("abc")));
	CHECK(stringToNumber("1e3") == 1000);
	CHECK(stringToNumber(".5") == 0.5);
	CHECK(stringToNumber("019") == 19); // not octal digits: decimal

	CHECK(numberToString(1e15) == "1e+15");
	CHECK(numberToString(123456789012345.0) == "123456789012345");
	CHECK(numberToString(0.0001) == "0.0001");
	CHECK(numberToString(0.00001) == "1e-5");
	CHECK(numberToString(1.0 / 3) == "0.333333333333333");
	CHECK(numberToString(-0.0) == "0");
	CHECK(numberToString(std::nan("")) == "NaN");
	CHECK(numberToString(-INFINITY) == "-Infinity");
	CHECK(numberToString(255, 16) == "ff");
	CHECK(numberToString(-5, 2) == "-101");

	CHECK(toBoolean(Value("0"))); // SWF 7: non-empty strings are true
	CHECK(!toBoolean(Value("")));
	CHECK(!toBoolean(Value(std::nan(""))));
	CHECK(toInt32(4294967297.0) == 1);
	CHECK(toInt32(-1.9) == -1);
	CHECK(toUint32(-1) == 4294967295u);
	CHECK(arrayIndex("12") == 12 && arrayIndex("012") == -1 && arrayIndex("-1") == -1);
	CHECK(utf16Length("h\xc3\xa9llo") == 5);
	CHECK(utf16ToUtf8(utf8ToUtf16("\xf0\x9f\x98\x80")) == "\xf0\x9f\x98\x80");
}

TEST(Avm1_InterpreterConversions)
{
	Interpreter vm;
	CHECK(std::isnan(vm.toNumber(Value())));       // undefined -> NaN in SWF 7
	CHECK(std::isnan(vm.toNumber(Value::null()))); // so is null
	CHECK(vm.toNumber(Value(true)) == 1);
	CHECK(vm.toString(Value()) == "undefined");
	CHECK(vm.toString(Value(vm.newObject())) == "[object Object]");
	CHECK(vm.toString(Value(vm.newArray({Value(1), Value("a"), Value()}))) == "1,a,undefined");
	CHECK(vm.toString(Value(vm.newFunction({}))) == "[type Function]");
	CHECK(std::string(vm.typeOf(Value(vm.newFunction({})))) == "function");
	CHECK(std::string(vm.typeOf(Value::null())) == "null");

	CHECK(vm.equals(Value(), Value::null()));
	CHECK(!vm.equals(Value(), Value(0)));
	CHECK(vm.equals(Value("1"), Value(1)));
	CHECK(vm.equals(Value(true), Value("1")));
	CHECK(!vm.equals(Value(std::nan("")), Value(std::nan(""))));
	CHECK(!strictEquals(Value("1"), Value(1)));

	CHECK(vm.add(Value(1), Value("2")).asString() == "12");
	CHECK(vm.add(Value(1), Value(true)).asNumber() == 2);
	CHECK(std::isnan(vm.add(Value(1), Value()).asNumber()));
	CHECK(vm.add(Value("a"), Value()).asString() == "aundefined");
	CHECK(vm.less(Value("10"), Value("9")).asBoolean());  // both strings: compared as text
	CHECK(!vm.less(Value(10), Value("9")).asBoolean()); // otherwise as numbers
	CHECK(vm.less(Value(1), Value("x")).isUndefined());  // NaN

	// valueOf drives arithmetic, toString drives string conversion.
	const ObjectRef o = vm.newObject();
	vm.defineMethod(o, "valueOf", [](Interpreter&, CallArgs&) -> Value { return Value(41); });
	vm.defineMethod(o, "toString", [](Interpreter&, CallArgs&) -> Value { return Value("obj"); });
	CHECK(vm.add(Value(o), Value(1)).asNumber() == 42);
	CHECK(vm.toString(Value(o)) == "obj");
}

TEST(Avm1_Arithmetic)
{
	Asm a;
	a.push({1, "2"}).op(kAdd2).trace();                    // "12"
	a.push({1, 2}).op(kAdd2).trace();                      // 3
	a.push({Undefined {}, 1}).op(kAdd2).trace();           // NaN
	a.push({"a", Undefined {}}).op(kAdd2).trace();         // aundefined
	a.push({Null {}, Undefined {}}).op(kEquals2).trace();  // true
	a.push({"1", 1}).op(kStrictEquals).trace();            // false
	a.push({1, "x"}).op(kLess2).trace();                   // undefined
	a.push({-7.5, 2}).op(kModulo).trace();                 // -1.5
	a.push({-1, 28}).op(kBitURShift).trace();              // 15
	a.push({"5"}).op(kIncrement).trace();                  // 6
	a.push({-3.7}).op(kToInteger).trace();                 // -3
	a.push({Null {}}).op(kTypeOf).trace();                 // null
	a.push({0.1, 0.2}).op(kAdd2).trace();                  // 0.3
	a.push({Undefined {}}).op(kNot).trace();               // true
	CHECK(run(a) == "12|3|NaN|aundefined|true|false|undefined|-1.5|15|6|-3|null|0.3|true");
}

TEST(Avm1_VariablesConstantsAndLoops)
{
	// var i = 0, s = ""; while (i < 5) { s += i; i++; } trace(s);
	Asm a;
	a.constants({"i", "s"});
	a.push({Const {0}, 0}).op(kSetVariable);
	a.push({Const {1}, ""}).op(kSetVariable);
	const size_t top = a.bytes.size();
	a.push({Const {0}}).op(kGetVariable).push({5}).op(kLess2).op(kNot);
	const size_t exit = a.branch(kIf);
	a.push({Const {1}, Const {1}}).op(kGetVariable).push({Const {0}}).op(kGetVariable).op(kAdd2).op(kSetVariable);
	a.push({Const {0}, Const {0}}).op(kGetVariable).op(kIncrement).op(kSetVariable);
	a.jumpBack(kJump, top);
	a.land(exit);
	a.push({Const {1}}).op(kGetVariable).trace();
	// Paths: o = {b: {c: 7}}; trace(o.b.c) via "o.b.c" and "/:o"-style names.
	a.push({"o", "b", "c", 7, 1}).op(kInitObject).push({1}).op(kInitObject).op(kSetVariable);
	a.traceVar("o.b.c");
	a.push({"o.b.c", 8}).op(kSetVariable).traceVar("o.b.c");
	a.traceVar(":s");
	CHECK(run(a) == "01234|7|8|01234");
}

TEST(Avm1_Function2RegistersAndClosures)
{
	// function add(a, b) { return a + b + this.k; }  with this in r1, a in r2, b in r3
	Asm add;
	add.push({Reg {2}, Reg {3}}).op(kAdd2).push({Reg {1}, "k"}).op(kGetMember).op(kAdd2).op(kReturn);
	// function makeCounter() { var n = 0; return function () { n = n + 1; return n; }; }
	Asm inner;
	inner.push({"n", "n"}).op(kGetVariable).op(kIncrement).op(kSetVariable).get("n").op(kReturn);
	Asm make;
	make.push({"n", 0}).op(kDefineLocal);
	make.function2("", {}, 1, 0, inner).op(kReturn);

	Asm a;
	a.function2("add", {{2, "a"}, {3, "b"}}, 4, 0x0001 /* preload this */, add);
	// obj = {k: 10}; obj.f = add; trace(obj.f(1, 2));
	a.push({"obj"}).push({"k", 10, 1}).op(kInitObject).op(kSetVariable);
	a.get("obj").push({"f"}).get("add").op(kSetMember);
	a.push({2, 1, 2}).get("obj").push({"f"}).op(kCallMethod).trace();
	a.function2("makeCounter", {}, 1, 0, make);
	a.push({"c1", 0, "makeCounter"}).op(kCallFunction).op(kSetVariable);
	a.push({"c2", 0, "makeCounter"}).op(kCallFunction).op(kSetVariable);
	a.push({0, "c1"}).op(kCallFunction).op(kPop);
	a.push({0, "c1"}).op(kCallFunction).trace();      // 2
	a.push({0, "c2"}).op(kCallFunction).trace();      // 1: a closure of its own
	a.push({0, "missing"}).op(kCallFunction).trace(); // undefined
	a.get("n").op(kTypeOf).trace();                   // locals stay local
	CHECK(run(a) == "13|2|1|undefined|undefined");
}

TEST(Avm1_ArgumentsAndDefineFunction)
{
	// function f(x) { return arguments.length + ":" + x + ":" + arguments[1]; }
	Asm body;
	body.get("arguments").push({"length"}).op(kGetMember).push({":"}).op(kAdd2).get("x").op(kAdd2);
	body.push({":"}).op(kAdd2).get("arguments").push({1}).op(kGetMember).op(kAdd2).op(kReturn);
	Asm a;
	a.function("f", {"x"}, body);
	a.push({"b", "a", 2, "f"}).op(kCallFunction).trace();
	CHECK(run(a) == "2:a:b");
}

TEST(Avm1_ClassesExtendsAndSuper)
{
	// _global.Base = function (x) { this.x = x; };
	// Base.prototype.describe = function () { return "base" + this.x; };
	// _global.Mid = function (x) { super(x * 2); };       Mid extends Base
	// Mid.prototype.describe = function () { return "mid(" + super.describe() + ")"; };
	// _global.Leaf = function (x) { super(x + 1); };      Leaf extends Mid
	// Leaf.prototype.describe = function () { return "leaf(" + super.describe() + ")"; };
	Asm base;
	base.push({Reg {1}, "x", Reg {2}}).op(kSetMember);
	Asm baseDescribe;
	baseDescribe.push({"base", Reg {1}, "x"}).op(kGetMember).op(kAdd2).op(kReturn);
	const uint16_t thisAndSuper = 0x0001 | 0x0010; // this in r1, super in r2
	Asm mid;
	mid.push({Reg {3}, 2}).op(0x0c).push({1, Reg {2}, Undefined {}}).op(kCallMethod).op(kPop);
	Asm midDescribe;
	midDescribe.push({"mid(", 0, Reg {2}, "describe"}).op(kCallMethod).op(kAdd2).push({")"}).op(kAdd2).op(kReturn);
	Asm leaf;
	leaf.push({Reg {3}, 1}).op(kAdd2).push({1, Reg {2}, Undefined {}}).op(kCallMethod).op(kPop);
	Asm leafDescribe;
	leafDescribe.push({"leaf(", 0, Reg {2}, "describe"}).op(kCallMethod).op(kAdd2).push({")"}).op(kAdd2).op(kReturn);

	Asm a;
	a.push({"_global"}).op(kGetVariable).push({"Base"}).function2("", {{2, "x"}}, 3, 0x0001, base).op(kSetMember);
	a.get("Base").push({"prototype"}).op(kGetMember).push({"describe"}).function2("", {}, 2, 0x0001, baseDescribe).op(kSetMember);
	a.push({"_global"}).op(kGetVariable).push({"Mid"}).function2("", {{3, "x"}}, 4, thisAndSuper, mid).op(kSetMember);
	a.get("Mid").get("Base").op(kExtends);
	a.get("Mid").push({"prototype"}).op(kGetMember).push({"describe"}).function2("", {}, 3, thisAndSuper, midDescribe).op(kSetMember);
	a.push({"_global"}).op(kGetVariable).push({"Leaf"}).function2("", {{3, "x"}}, 4, thisAndSuper, leaf).op(kSetMember);
	a.get("Leaf").get("Mid").op(kExtends);
	a.get("Leaf").push({"prototype"}).op(kGetMember).push({"describe"}).function2("", {}, 3, thisAndSuper, leafDescribe).op(kSetMember);

	a.push({"o", 4, 1, "Leaf"}).op(kNewObject).op(kSetVariable);
	a.push({0, "o"}).op(kGetVariable).push({"describe"}).op(kCallMethod).trace(); // leaf(mid(base10))
	a.get("o").get("Base").op(kInstanceOf).trace();                                // true
	a.get("o").push({"x"}).op(kGetMember).trace();                                 // 10
	a.push({"m", 3, 1, "Mid"}).op(kNewObject).op(kSetVariable);
	a.push({0, "m"}).op(kGetVariable).push({"describe"}).op(kCallMethod).trace(); // mid(base6)
	a.get("m").get("Leaf").op(kInstanceOf).trace();                                // false
	CHECK(run(a) == "leaf(mid(base10))|true|10|mid(base6)|false");
}

TEST(Avm1_TryCatchFinally)
{
	// try { trace("t"); throw "boom"; } catch (e) { trace("c:" + e); } finally { trace("f"); }
	Asm t1, c1, f1;
	t1.push({"t"}).trace().push({"boom"}).op(kThrow).push({"not reached"}).trace();
	c1.push({"c:"}).get("e").op(kAdd2).trace();
	f1.push({"f"}).trace();
	Asm a;
	a.tryBlock(t1, "e", &c1, &f1);
	// function g() { try { return "r"; } finally { trace("g-finally"); } }
	Asm gt, gf;
	gt.push({"r"}).op(kReturn);
	gf.push({"g-finally"}).trace();
	Asm g;
	g.tryBlock(gt, "", nullptr, &gf).push({"after"}).op(kReturn);
	a.function("g", {}, g);
	a.push({0, "g"}).op(kCallFunction).trace();
	// An inner try/finally without catch lets the throw reach the outer catch.
	Asm innerTry, innerFinally, outerCatch;
	innerTry.push({"x"}).op(kThrow);
	innerFinally.push({"inner-finally"}).trace();
	Asm outerTry;
	outerTry.tryBlock(innerTry, "", nullptr, &innerFinally);
	outerCatch.push({"outer:"}).get("e2").op(kAdd2).trace();
	a.tryBlock(outerTry, "e2", &outerCatch, nullptr);
	a.push({"end"}).trace();
	CHECK(run(a) == "t|c:boom|f|g-finally|r|inner-finally|outer:x|end");

	// An uncaught throw ends the block and fails the entry point.
	Asm uncaught;
	uncaught.push({"before"}).trace().push({"oops"}).op(kThrow).push({"after"}).trace();
	bool ok = true;
	CHECK(run(uncaught, &ok) == "before");
	CHECK(!ok);
}

TEST(Avm1_GuardsAgainstBadCode)
{
	// while (true) {}
	Asm loop;
	loop.jumpBack(kJump, 0);
	TraceHost host;
	Interpreter vm(&host);
	vm.setInstructionBudget(100000);
	CHECK(!vm.execute(loop.bytes.data(), loop.bytes.size(), {}));
	CHECK(vm.lastError().find("budget") != std::string::npos);
	CHECK(!host.errors.empty());

	// Truncated push, jump outside, garbage: no crash, just failure.
	const std::vector<uint8_t> truncated {kPush, 0x05, 0x00, 0x00, 'a'};
	CHECK(!vm.execute(truncated.data(), truncated.size(), {}));
	const std::vector<uint8_t> wild {kJump, 0x02, 0x00, 0x00, 0x40};
	CHECK(!vm.execute(wild.data(), wild.size(), {}));
	uint32_t seed = 1;
	for (int round = 0; round < 2000; ++round) {
		std::vector<uint8_t> noise(1 + round % 64);
		for (uint8_t& b : noise) {
			seed = seed * 1103515245 + 12345;
			b = static_cast<uint8_t>(seed >> 16);
		}
		vm.execute(noise.data(), noise.size(), {});
	}
	// Infinite recursion hits the call depth limit.
	Asm self;
	self.push({0, "self"}).op(kCallFunction).op(kReturn);
	Asm recurse;
	recurse.function("self", {}, self).push({0, "self"}).op(kCallFunction);
	CHECK(!vm.execute(recurse.bytes.data(), recurse.bytes.size(), {}));
	CHECK(vm.lastError().find("depth") != std::string::npos);
	// Still usable afterwards.
	Asm fine;
	fine.push({"ok"}).trace();
	CHECK(vm.execute(fine.bytes.data(), fine.bytes.size(), {}));
}

TEST(Avm1_ObjectsEnumerationAndWith)
{
	Asm a;
	// o = {a: 1, b: 2}; o.c = 3; ASSetPropFlags(o, "b", 1); for (k in o) trace(k);
	a.push({"o", "a", 1, "b", 2, 2}).op(kInitObject).op(kSetVariable);
	a.get("o").push({"c", 3}).op(kSetMember);
	a.push({1, "b"}).get("o").push({3, "ASSetPropFlags"}).op(kCallFunction).op(kPop);
	a.get("o").op(kEnumerate2);
	// The compiler's for..in loop: names until the null marker.
	const size_t top = a.bytes.size();
	a.action(0x87 /* StoreRegister */, {0}).push({Null {}}).op(kEquals2);
	const size_t done = a.branch(kIf);
	a.push({Reg {0}}).trace();
	a.jumpBack(kJump, top);
	a.land(done);
	// with (o) { trace(a + c); }
	Asm body;
	body.get("a").get("c").op(kAdd2).trace();
	a.get("o").action(kWith, {static_cast<uint8_t>(body.bytes.size()), 0});
	a.bytes.insert(a.bytes.end(), body.bytes.begin(), body.bytes.end());
	// typeof undeclared inside with falls back to the timeline
	a.get("c").op(kTypeOf).trace();
	CHECK(run(a) == "c|a|4|undefined");
}

TEST(Avm1_AddPropertyAndWatch)
{
	Interpreter vm;
	const ObjectRef proto = vm.newObject();
	int stored = 0;
	const auto getter = vm.newFunction([&stored](Interpreter&, CallArgs&) -> Value { return Value(stored * 10); });
	const auto setter = vm.newFunction([&stored](Interpreter& vm, CallArgs& c) -> Value {
		stored = static_cast<int>(vm.toNumber(c[0]));
		return Value();
	});
	CHECK(proto->addAccessor("v", getter, setter));
	const ObjectRef instance = vm.newObject();
	instance->setProto(proto);
	// The setter on the prototype takes assignments to the instance.
	vm.setMember(Value(instance), "v", Value(4));
	CHECK(stored == 4);
	CHECK(vm.getMember(Value(instance), "v").asNumber() == 40);
	CHECK(!instance->hasOwnMember("v"));

	const auto watcher = vm.newFunction([](Interpreter& vm, CallArgs& c) -> Value {
		return Value(vm.toNumber(c[2]) * 2); // newValue doubled
	});
	CHECK(vm.callMember(Value(instance), "watch", {Value("w"), Value(watcher)}).asBoolean());
	vm.setMember(Value(instance), "w", Value(5));
	CHECK(vm.getMember(Value(instance), "w").asNumber() == 10);
	CHECK(vm.callMember(Value(instance), "hasOwnProperty", {Value("w")}).asBoolean());
	CHECK(vm.callMember(Value(proto), "isPrototypeOf", {Value(instance)}).asBoolean());
}

TEST(Avm1_ArraySorting)
{
	Interpreter vm;
	const auto nums = [&vm](std::initializer_list<double> values) {
		std::vector<Value> v;
		for (double d : values) {
			v.push_back(Value(d));
		}
		return vm.newArray(v);
	};
	const auto text = [&vm](const Value& v) { return vm.toString(v); };
	const Value array = vm.getMember(Value(vm.global()), "Array");
	const double numeric = vm.toNumber(vm.getMember(array, "NUMERIC"));
	const double descending = vm.toNumber(vm.getMember(array, "DESCENDING"));

	auto a = nums({10, 9, 1, 100});
	vm.callMember(Value(a), "sort", {});
	CHECK(text(Value(a)) == "1,10,100,9"); // default: as strings
	vm.callMember(Value(a), "sort", {Value(numeric)});
	CHECK(text(Value(a)) == "1,9,10,100");
	vm.callMember(Value(a), "sort", {Value(numeric + descending)});
	CHECK(text(Value(a)) == "100,10,9,1");
	const auto byValue = vm.newFunction([](Interpreter& vm, CallArgs& c) -> Value { return Value(vm.toNumber(c[0]) - vm.toNumber(c[1])); });
	vm.callMember(Value(a), "sort", {Value(byValue)});
	CHECK(text(Value(a)) == "1,9,10,100");
	auto b = nums({3, 1, 2});
	CHECK(text(vm.callMember(Value(b), "sort", {Value(numeric + 8)})) == "1,2,0"); // RETURNINDEXEDARRAY
	CHECK(text(Value(b)) == "3,1,2");
	CHECK(vm.callMember(Value(nums({1, 2, 1})), "sort", {Value(4)}).asNumber() == 0); // UNIQUESORT

	const auto row = [&vm](const char* name, double age) {
		const ObjectRef o = vm.newObject();
		o->define("name", Value(name));
		o->define("age", Value(age));
		return Value(o);
	};
	auto people = vm.newArray({row("bob", 30), row("Al", 5), row("cy", 30)});
	vm.callMember(Value(people), "sortOn", {Value("name")});
	const auto names = [&] {
		std::string out;
		for (const Value& p : people->elements) {
			out += vm.toString(vm.getMember(p, "name")) + " ";
		}
		return out;
	};
	CHECK(names() == "Al bob cy ");
	// Per-field options: age as numbers, both descending.
	vm.callMember(Value(people), "sortOn",
		{Value(vm.newArray({Value("age"), Value("name")})), Value(vm.newArray({Value(numeric + descending), Value(descending)}))});
	CHECK(names() == "cy bob Al ");

	// A comparator that lies does not break anything.
	const auto liar = vm.newFunction([](Interpreter& vm, CallArgs&) -> Value { return Value(vm.random() - 0.5); });
	auto many = nums({5, 3, 8, 1, 9, 2, 7, 4, 6, 0, 11, 15, 13, 12, 14, 10});
	vm.callMember(Value(many), "sort", {Value(liar)});
	CHECK(many->elements.size() == 16);

	auto c = nums({1, 2, 3, 4, 5});
	CHECK(text(vm.callMember(Value(c), "splice", {Value(1), Value(2), Value("x")})) == "2,3");
	CHECK(text(Value(c)) == "1,x,4,5");
	CHECK(text(vm.callMember(Value(c), "slice", {Value(-2)})) == "4,5");
	CHECK(vm.callMember(Value(c), "push", {Value(6)}).asNumber() == 5);
	CHECK(text(vm.callMember(Value(c), "concat", {Value(nums({7})), Value(8)})) == "1,x,4,5,6,7,8");
	CHECK(text(vm.callMember(Value(c), "join", {Value("-")})) == "1-x-4-5-6");
	vm.callMember(Value(c), "reverse", {});
	CHECK(text(Value(c)) == "6,5,4,x,1");
	vm.setMember(Value(c), "length", Value(2));
	CHECK(text(Value(c)) == "6,5");
}

TEST(Avm1_StringMethods)
{
	Interpreter vm;
	const auto call = [&vm](const char* s, const char* method, std::vector<Value> args) {
		return vm.callMember(Value(s), method, std::move(args));
	};
	const auto text = [&vm](const Value& v) { return vm.toString(v); };
	CHECK(vm.getMember(Value("h\xc3\xa9llo"), "length").asNumber() == 5);
	CHECK(text(call("Hello", "charAt", {Value(1)})) == "e");
	CHECK(call("Hello", "charCodeAt", {Value(0)}).asNumber() == 72);
	CHECK(std::isnan(call("Hello", "charCodeAt", {Value(9)}).asNumber()));
	CHECK(call("Hello", "indexOf", {Value("l")}).asNumber() == 2);
	CHECK(call("Hello", "lastIndexOf", {Value("l")}).asNumber() == 3);
	CHECK(call("Hello", "indexOf", {Value("z")}).asNumber() == -1);
	CHECK(text(call("Hello", "substr", {Value(-3), Value(2)})) == "ll");
	CHECK(text(call("Hello", "substring", {Value(3), Value(1)})) == "el");
	CHECK(text(call("Hello", "slice", {Value(1), Value(-1)})) == "ell");
	CHECK(text(call("Hello", "toUpperCase", {})) == "HELLO");
	CHECK(text(call("\xc3\x89T\xc3\x89", "toLowerCase", {})) == "\xc3\xa9t\xc3\xa9");
	CHECK(text(call("a,b,,c", "split", {Value(",")})) == "a,b,,c");
	CHECK(vm.getMember(call("a,b,,c", "split", {Value(",")}), "length").asNumber() == 4);
	CHECK(vm.getMember(call("abc", "split", {Value("")}), "length").asNumber() == 3);
	CHECK(text(call("a", "concat", {Value(1), Value(true)})) == "a1true");
	const Value string = vm.getMember(Value(vm.global()), "String");
	CHECK(text(vm.callMember(string, "fromCharCode", {Value(72), Value(105)})) == "Hi");
	const Value wrapped = vm.constructWith(string, {Value("abc")});
	CHECK(std::string(vm.typeOf(wrapped)) == "object");
	CHECK(vm.getMember(wrapped, "length").asNumber() == 3);
	CHECK(text(vm.callMember(Value(255), "toString", {Value(16)})) == "ff");

	const Value global(vm.global());
	CHECK(vm.callMember(global, "parseInt", {Value("0x1A")}).asNumber() == 26);
	CHECK(vm.callMember(global, "parseInt", {Value("12px")}).asNumber() == 12);
	CHECK(vm.callMember(global, "parseInt", {Value("ff"), Value(16)}).asNumber() == 255);
	CHECK(vm.callMember(global, "parseFloat", {Value("3.5e2x")}).asNumber() == 350);
	CHECK(text(vm.callMember(global, "escape", {Value("a b&\xc3\xa9")})) == "a%20b%26%C3%A9");
	CHECK(text(vm.callMember(global, "unescape", {Value("a%20b%26%C3%A9")})) == "a b&\xc3\xa9");
	const Value math = vm.getMember(global, "Math");
	CHECK(vm.callMember(math, "round", {Value(-2.5)}).asNumber() == -2);
	CHECK(vm.callMember(math, "max", {Value(1), Value("7"), Value(3)}).asNumber() == 7);

	const Value date = vm.constructWith(vm.getMember(global, "Date"), {Value(2006), Value(9), Value(17), Value(12), Value(30)});
	CHECK(vm.callMember(date, "getFullYear", {}).asNumber() == 2006);
	CHECK(vm.callMember(date, "getMonth", {}).asNumber() == 9);
	CHECK(vm.callMember(date, "getDate", {}).asNumber() == 17);
	CHECK(vm.callMember(date, "getDay", {}).asNumber() == 2); // a Tuesday
	CHECK(vm.callMember(date, "getMinutes", {}).asNumber() == 30);
	CHECK(vm.callMember(vm.getMember(global, "Date"), "UTC", {Value(1970), Value(0), Value(2)}).asNumber() == 86400000);
}

TEST(Avm1_IntervalsAdvanceWithTheClock)
{
	TraceHost host;
	Interpreter vm(&host);
	// count = 0; id = setInterval(function () { trace(++count); if (count == 3) clearInterval(id); }, 100);
	Asm tick;
	tick.push({"count", "count"}).op(kGetVariable).op(kIncrement).op(kSetVariable).get("count").trace();
	tick.get("count").push({3}).op(kEquals2).op(kNot);
	const size_t skip = tick.branch(kIf);
	tick.get("id").push({1, "clearInterval"}).op(kCallFunction).op(kPop);
	tick.land(skip);
	Asm a;
	a.push({"count", 0}).op(kSetVariable);
	a.push({"id", 100}).function("", {}, tick).push({2, "setInterval"}).op(kCallFunction).op(kSetVariable);
	CHECK(vm.execute(a.bytes.data(), a.bytes.size(), {}));
	vm.advance(50);
	CHECK(host.output.empty());
	vm.advance(50);
	CHECK(host.output == "1");
	vm.advance(100);
	vm.advance(100);
	vm.advance(100);
	vm.advance(1000);
	CHECK(host.output == "1|2|3");
}

// A stand-in for the display list: clips with a name, a parent, children
// and an _x, and a host that records what the timeline was told.
class FakeClip : public Object {
public:
	FakeClip(std::string name, ObjectRef parent) : name(std::move(name)), parent(std::move(parent)) {}
	std::string name;
	ObjectRef parent;
	std::vector<std::shared_ptr<FakeClip>> children;
	double x = 0;

	const char* typeName() const override { return "movieclip"; }
	bool getMember(Interpreter& vm, const std::string& member, Value& out) override
	{
		if (member == "_x") {
			out = Value(x);
			return true;
		}
		if (member == "_parent") {
			out = parent ? Value(parent) : Value();
			return parent != nullptr;
		}
		for (const auto& child : children) {
			if (child->name == member) {
				out = Value(child);
				return true;
			}
		}
		return Object::getMember(vm, member, out);
	}
	void setMember(Interpreter& vm, const std::string& member, const Value& value) override
	{
		if (member == "_x") {
			x = vm.toNumber(value);
			return;
		}
		Object::setMember(vm, member, value);
	}
};

class StageHost : public TraceHost {
public:
	std::shared_ptr<FakeClip> stage;
	std::string log;
	std::string linkage;
	ObjectRef registered;

	ObjectRef root(const ObjectRef&) override { return stage; }
	std::string targetPath(const ObjectRef& clip) override
	{
		const auto* c = dynamic_cast<FakeClip*>(clip.get());
		if (!c) {
			return {};
		}
		return c->parent ? targetPath(c->parent) + "." + c->name : "_level0";
	}
	Value getProperty(const ObjectRef& clip, int index) override
	{
		const auto* c = dynamic_cast<FakeClip*>(clip.get());
		return c && index == kPropX ? Value(c->x) : Value();
	}
	void setProperty(const ObjectRef& clip, int index, const Value& value) override
	{
		if (auto* c = dynamic_cast<FakeClip*>(clip.get()); c && index == kPropX && value.isNumber()) {
			c->x = value.asNumber();
		}
	}
	void gotoFrame(const ObjectRef& clip, int frame, bool play) override
	{
		log += targetPath(clip) + " goto " + std::to_string(frame) + (play ? " play;" : ";");
	}
	void gotoLabel(const ObjectRef& clip, const std::string& label, bool play) override
	{
		log += targetPath(clip) + " label " + label + (play ? " play;" : ";");
	}
	void stop(const ObjectRef& clip) override { log += targetPath(clip) + " stop;"; }
	bool registerClass(const std::string& name, const ObjectRef& constructor) override
	{
		linkage = name;
		registered = constructor;
		return true;
	}
};

TEST(Avm1_HostMovieClips)
{
	StageHost host;
	Interpreter vm(&host);
	host.stage = vm.make<FakeClip>("_level0", nullptr);
	auto menu = vm.make<FakeClip>("menu", host.stage);
	auto button = vm.make<FakeClip>("button", menu);
	host.stage->children.push_back(menu);
	menu->children.push_back(button);
	button->x = 12;

	Asm a;
	a.get("_root.menu.button._x").trace();                 // 12 through getMember
	a.get("/menu/button:_x").trace();                      // the SWF 4 path form
	a.push({"/menu/button", 0, 40}).op(0x23);              // SetProperty(_x)
	a.push({"button", 0}).op(0x22).trace();                // GetProperty relative to this timeline
	a.get("button").op(0x45).trace();                      // TargetPath
	a.get("button").op(kTypeOf).trace();                   // movieclip
	a.push({"button:intro"}).action(0x9f, {0x01});         // gotoAndPlay("button:intro")
	a.push({3}).action(0x9f, {0x00});                      // gotoAndStop(3) on the timeline
	a.action(0x8b, {'b', 'u', 't', 't', 'o', 'n', 0}).op(0x07).action(0x8b, {0}).op(0x07); // tellTarget
	CHECK(vm.execute(a.bytes.data(), a.bytes.size(), {menu, nullptr}));
	CHECK(host.output == "12|12|40|_level0.menu.button|movieclip");
	CHECK(host.log == "_level0.menu.button label intro play;_level0.menu goto 2;_level0.menu.button stop;_level0.menu stop;");

	// Object.registerClass("Widget", Widget), then the host attaches an
	// instance, runs the constructor on it and calls its events.
	Asm widget;
	widget.push({Reg {1}, "ready", true}).op(kSetMember);
	Asm onEnterFrame;
	onEnterFrame.push({"frame:"}).push({Reg {1}, "_x"}).op(kGetMember).op(kAdd2).trace();
	Asm b;
	b.push({"_global"}).op(kGetVariable).push({"Widget"}).function2("", {}, 2, 0x0001, widget).op(kSetMember);
	b.get("Widget").push({"prototype"}).op(kGetMember).push({"onEnterFrame"}).function2("", {}, 2, 0x0001, onEnterFrame).op(kSetMember);
	b.get("Widget").push({"Widget", 2, "Object"}).op(kGetVariable).push({"registerClass"}).op(kCallMethod).op(kPop);
	CHECK(vm.execute(b.bytes.data(), b.bytes.size(), {host.stage, nullptr}));
	CHECK(host.linkage == "Widget" && host.registered != nullptr);
	CHECK(vm.initializeInstance(button, Value(host.registered), {}));
	CHECK(toBoolean(vm.getMember(Value(button), "ready")));
	CHECK(vm.callMethod(button, "onEnterFrame", {}));
	CHECK(vm.callMethod(button, "onPress", {})); // no handler: nothing happens
	CHECK(host.output == "12|12|40|_level0.menu.button|movieclip|frame:40");
}

// Every DoInitAction of the shipped main menu: they define the
// __Packages.* ActionScript 2 classes.
TEST(Avm1_MainMenuClassesLoad)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	const auto zip = io::ZipArchive::open(std::string(gameDir) + "/mods/bf2142/Menu_server.zip");
	CHECK(zip != nullptr);
	std::vector<uint8_t> dfm;
	if (!zip || !zip->read("External/FlashMenu/mainMenu.dfm", dfm)) {
		CHECK(false);
		return;
	}
	const auto swf = ui::decodeDfm(dfm);
	const auto movie = swf ? ui::parseSwf(*swf) : std::nullopt;
	CHECK(movie.has_value());
	if (!movie) {
		return;
	}
	TraceHost host;
	Interpreter vm(&host);
	const ObjectRef root = vm.newObject();
	int blocks = 0, clean = 0;
	for (const ui::SwfTag& tag : movie->tags) {
		if (tag.code != ui::kSwfDoInitAction || tag.length < 2) {
			continue;
		}
		++blocks;
		const uint8_t* body = swf->data() + tag.offset + 2; // after the sprite id
		if (vm.execute(body, tag.length - 2, {root, nullptr})) {
			++clean;
		} else {
			std::fprintf(stderr, "DoInitAction %d: %s\n", blocks, vm.lastError().c_str());
		}
	}
	std::fprintf(stderr, "mainMenu.dfm: %d of %d DoInitAction blocks ran cleanly\n", clean, blocks);
	CHECK(blocks > 0);
	CHECK(clean == blocks);
	const Value uis = vm.getMember(vm.getMember(Value(vm.global()), "dice"), "UIs");
	CHECK(uis.isObject());
	CHECK(uis.isObject() && uis.asObject()->properties().size() > 0);
	if (uis.isObject()) {
		std::fprintf(stderr, "_global.dice.UIs has %zu members\n", uis.asObject()->properties().size());
	}
}

TEST(Avm1_WithCallsFunctionsOnItsObject)
{
	// with (o) { trace(f()); } where f is o's method: `this` is o (the
	// menus' `with (mask) { beginFill(...) }`).
	TraceHost host;
	Interpreter vm(&host);
	const ObjectRef o = vm.newObject();
	o->define("n", Value(5), 0);
	o->define("f", Value(vm.newFunction([](Interpreter& vm, CallArgs& c) -> Value {
		return c.thisObject ? vm.getMember(Value(c.thisObject), "n") : Value("no this");
	})), 0);
	vm.global()->define("o", Value(o), 0);
	Asm body;
	body.push({0, "f"}).op(kCallFunction).trace();
	Asm a;
	a.get("o").action(kWith, {static_cast<uint8_t>(body.bytes.size()), 0});
	a.bytes.insert(a.bytes.end(), body.bytes.begin(), body.bytes.end());
	CHECK(vm.execute(a.bytes.data(), a.bytes.size(), {}));
	CHECK(host.output == "5");
}

TEST(Avm1_NamesIgnoreCase)
{
	// DICE's gameswf matched names without regard to case: the menus write
	// _root.UpdateManager1 for updateManager1.
	TraceHost host;
	Interpreter vm(&host);
	const ObjectRef o = vm.newObject();
	o->define("updateManager1", Value(7), 0);
	vm.global()->define("o", Value(o), 0);
	Asm a;
	a.get("O").push({"UpdateManager1"}).op(kGetMember).trace();
	CHECK(vm.execute(a.bytes.data(), a.bytes.size(), {}));
	CHECK(host.output == "7");
	// The spelling first given stays (for..in shows it).
	o->define("UPDATEMANAGER1", Value(8), 0);
	CHECK(o->properties().size() == 1 && o->properties().entries().front().first == "updateManager1");
}
