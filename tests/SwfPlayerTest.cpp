// The Flash player: a hand-built movie (a sprite, a button, a text field
// bound to a variable, frame scripts calling gotoAndStop, attachMovie and
// a registered class), the native registry, and (with $BF2142_GAME_DIR)
// ten seconds of the shipped main menu with its images.
#include "IO/File/ZipArchive.h"
#include "TestMain.h"
#include "UI/Dfm.h"
#include "UI/Player/Draw.h"
#include "UI/Player/Player.h"
#include "UI/Player/Stencil.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>

using namespace dice::hfe;
using namespace dice::hfe::ui;

namespace {

class Bits {
public:
	std::vector<uint8_t> bytes;

	void ub(int n, uint32_t v)
	{
		for (int i = n - 1; i >= 0; --i) {
			if (m_bit == 0) {
				bytes.push_back(0);
			}
			if ((v >> i) & 1) {
				bytes.back() |= static_cast<uint8_t>(0x80 >> m_bit);
			}
			m_bit = (m_bit + 1) & 7;
		}
	}
	void sb(int n, int32_t v) { ub(n, static_cast<uint32_t>(v) & ((n == 32 ? 0 : 1u << n) - 1)); }
	void align() { m_bit = 0; }
	void u8(uint8_t v)
	{
		align();
		bytes.push_back(v);
	}
	void u16(uint16_t v)
	{
		u8(static_cast<uint8_t>(v));
		u8(static_cast<uint8_t>(v >> 8));
	}
	void u32(uint32_t v)
	{
		u16(static_cast<uint16_t>(v));
		u16(static_cast<uint16_t>(v >> 16));
	}
	void str(const std::string& s)
	{
		align();
		bytes.insert(bytes.end(), s.begin(), s.end());
		bytes.push_back(0);
	}
	void raw(const std::vector<uint8_t>& b)
	{
		align();
		bytes.insert(bytes.end(), b.begin(), b.end());
	}
	void rect(int32_t x0, int32_t x1, int32_t y0, int32_t y1)
	{
		ub(5, 16);
		sb(16, x0);
		sb(16, x1);
		sb(16, y0);
		sb(16, y1);
		align();
	}
	void translate(int32_t x, int32_t y)
	{
		ub(1, 0); // no scale
		ub(1, 0); // no rotation
		ub(5, 20);
		sb(20, x);
		sb(20, y);
		align();
	}

private:
	int m_bit = 0;
};

// Tags with long headers (as long as any).
void tag(std::vector<uint8_t>& out, uint16_t code, const std::vector<uint8_t>& body)
{
	Bits b;
	b.u16(static_cast<uint16_t>(code << 6 | 0x3f));
	b.u32(static_cast<uint32_t>(body.size()));
	b.raw(body);
	out.insert(out.end(), b.bytes.begin(), b.bytes.end());
}

// A tiny AVM1 assembler.
class Asm {
public:
	std::vector<uint8_t> bytes;

	Asm& op(uint8_t code)
	{
		bytes.push_back(code);
		return *this;
	}
	Asm& push(std::initializer_list<const char*> strings)
	{
		std::vector<uint8_t> p;
		for (const char* s : strings) {
			p.push_back(0);
			p.insert(p.end(), s, s + std::strlen(s));
			p.push_back(0);
		}
		return action(0x96, p);
	}
	Asm& pushNumber(int32_t n)
	{
		std::vector<uint8_t> p {7};
		for (int i = 0; i < 4; ++i) {
			p.push_back(static_cast<uint8_t>(n >> (8 * i)));
		}
		return action(0x96, p);
	}
	Asm& pushTrue() { return action(0x96, {5, 1}); }
	Asm& action(uint8_t code, const std::vector<uint8_t>& payload)
	{
		bytes.push_back(code);
		bytes.push_back(static_cast<uint8_t>(payload.size()));
		bytes.push_back(static_cast<uint8_t>(payload.size() >> 8));
		bytes.insert(bytes.end(), payload.begin(), payload.end());
		return *this;
	}
	Asm& get(const char* name) { return push({name}).op(0x1c); }
	// name = <true>
	Asm& setTrue(const char* name)
	{
		push({name});
		pushTrue();
		return op(0x1d);
	}
	// function name(...) { body } as DefineFunction (no parameters).
	Asm& function(const char* name, const Asm& body)
	{
		std::vector<uint8_t> p(name, name + std::strlen(name));
		p.push_back(0);
		p.push_back(0); // no parameters
		p.push_back(0);
		p.push_back(static_cast<uint8_t>(body.bytes.size()));
		p.push_back(static_cast<uint8_t>(body.bytes.size() >> 8));
		action(0x9b, p);
		bytes.insert(bytes.end(), body.bytes.begin(), body.bytes.end());
		return *this;
	}
	Asm& end() { return op(0); }
};

// A 100 x 100 pixel square shape (DefineShape) filled with `rgb`.
std::vector<uint8_t> squareShape(uint16_t id, uint32_t rgb)
{
	Bits b;
	b.u16(id);
	b.rect(0, 2000, 0, 2000);
	b.u8(1); // one fill style
	b.u8(0x00);
	b.u8(static_cast<uint8_t>(rgb >> 16));
	b.u8(static_cast<uint8_t>(rgb >> 8));
	b.u8(static_cast<uint8_t>(rgb));
	b.u8(0); // no line styles
	b.ub(4, 1); // fill bits
	b.ub(4, 0); // line bits
	// Style change: move to 0,0 with fill style 1 on the right.
	b.ub(1, 0);
	b.ub(5, 0x05); // new styles 0, line 0, fill1 1, fill0 0, move 1
	b.ub(5, 1);
	b.sb(1, 0);
	b.sb(1, 0);
	b.ub(1, 1);
	const auto line = [&b](bool vertical, int32_t delta) {
		b.ub(1, 1); // edge
		b.ub(1, 1); // straight
		b.ub(4, 11); // 13 bits
		b.ub(1, 0); // not general
		b.ub(1, vertical ? 1 : 0);
		b.sb(13, delta);
	};
	line(false, 2000);
	line(true, 2000);
	line(false, -2000);
	line(true, -2000);
	b.ub(6, 0); // end
	b.align();
	return b.bytes;
}

std::vector<uint8_t> placeObject(uint16_t depth, uint16_t character, const char* name, int32_t x, int32_t y,
	const std::vector<uint8_t>& clipActions = {})
{
	Bits b;
	uint8_t flags = 0x02 | 0x04; // character, matrix
	if (name) {
		flags |= 0x20;
	}
	if (!clipActions.empty()) {
		flags |= 0x80;
	}
	b.u8(flags);
	b.u16(depth);
	b.u16(character);
	b.translate(x, y);
	if (name) {
		b.str(name);
	}
	if (!clipActions.empty()) {
		b.raw(clipActions);
	}
	return b.bytes;
}

// The test movie, two root frames:
//   characters: 1 red square, 2 sprite "Box" (frame 1: square, stop;
//   frame 2: nothing), 3 a button of the square whose release sets
//   _root.clicked, 4 a text field bound to `score`, 5 sprite "Widget"
//   registered to class Widget (constructor sets this.ready).
//   frame 1: box at depth 1, btn at depth 2 (200,0), field at depth 3;
//   script: box.gotoAndStop(2); attachMovie("Widget", "w", 5);
//   score = 42; onEnterFrame counts ticks.
std::vector<uint8_t> testMovie()
{
	std::vector<uint8_t> tags;
	tag(tags, 2, squareShape(1, 0xff0000));

	// Sprite 2 "Box": square on frame 1 with stop(); an empty frame 2.
	{
		std::vector<uint8_t> inner;
		tag(inner, 26, placeObject(1, 1, nullptr, 0, 0));
		tag(inner, 12, Asm().op(0x07).end().bytes); // stop
		tag(inner, 1, {});
		tag(inner, 28, {1, 0}); // RemoveObject2 depth 1
		tag(inner, 1, {});
		tag(inner, 0, {});
		Bits b;
		b.u16(2);
		b.u16(2);
		b.raw(inner);
		tag(tags, 39, b.bytes);
	}
	// Button 3: the square in every state; on release, _root.clicked = true.
	{
		Bits records;
		records.u8(0x0f);
		records.u16(1);
		records.u16(1);
		records.translate(0, 0);
		records.ub(1, 0); // cxform: no add
		records.ub(1, 0); // no mult
		records.ub(4, 1);
		records.align();
		records.u8(0); // end of records
		Asm release;
		release.setTrue("_root.clicked").end();
		Bits b;
		b.u16(3);
		b.u8(0);
		b.u16(static_cast<uint16_t>(2 + records.bytes.size()));
		b.raw(records.bytes);
		b.u16(0); // last condition
		b.u8(0x08); // OverDownToOverUp
		b.u8(0x00);
		b.raw(release.bytes);
		tag(tags, 34, b.bytes);
	}
	// Edit text 4: read-only, bound to `score`, initially "0".
	{
		Bits b;
		b.u16(4);
		b.rect(0, 2000, 0, 400);
		b.u8(0x80 | 0x08);
		b.u8(0);
		b.str("score");
		b.str("0");
		tag(tags, 37, b.bytes);
	}
	// Sprite 5 "Widget": one frame with the square.
	{
		std::vector<uint8_t> inner;
		tag(inner, 26, placeObject(1, 1, nullptr, 0, 0));
		tag(inner, 1, {});
		tag(inner, 0, {});
		Bits b;
		b.u16(5);
		b.u16(1);
		b.raw(inner);
		tag(tags, 39, b.bytes);
	}
	{
		Bits b;
		b.u16(2);
		b.u16(2);
		b.str("Box");
		b.u16(5);
		b.str("Widget");
		tag(tags, 56, b.bytes);
	}
	// #initclip for Widget: _global.Widget = function () { this.ready = true; };
	// Object.registerClass("Widget", _global.Widget);
	{
		Asm ctor;
		ctor.push({"this"}).op(0x1c).push({"ready"}).pushTrue().op(0x4f).end();
		Asm init;
		init.push({"_global"}).op(0x1c).push({"Widget"}).function("", ctor).op(0x4f);
		init.push({"_global"}).op(0x1c).push({"Widget"}).op(0x4e);
		init.push({"Widget"}).pushNumber(2).push({"Object"}).op(0x1c).push({"registerClass"}).op(0x52).op(0x17).end();
		Bits b;
		b.u16(5);
		b.raw(init.bytes);
		tag(tags, 59, b.bytes);
	}
	// Frame 1.
	tag(tags, 26, placeObject(1, 2, "box", 0, 0));
	tag(tags, 26, placeObject(2, 3, "btn", 4000, 0));
	tag(tags, 26, placeObject(3, 4, "field", 0, 4000));
	{
		Asm a;
		a.pushNumber(2).pushNumber(1).push({"box"}).op(0x1c).push({"gotoAndStop"}).op(0x52).op(0x17);
		a.pushNumber(5).push({"w", "Widget"}).pushNumber(3).push({"this"}).op(0x1c).push({"attachMovie"}).op(0x52).op(0x17);
		a.push({"score"}).pushNumber(42).op(0x1d);
		a.push({"ticks"}).pushNumber(0).op(0x1d);
		// onEnterFrame = function () { ticks = ticks + 1; };
		Asm count;
		count.push({"_root.ticks"}).get("_root.ticks").pushNumber(1).op(0x47).op(0x1d).end();
		a.push({"this"}).op(0x1c).push({"onEnterFrame"}).function("", count).op(0x4f);
		a.op(0x07).end(); // stop
		tag(tags, 12, a.bytes);
	}
	tag(tags, 1, {});
	tag(tags, 1, {});
	tag(tags, 0, {});

	Bits header;
	header.raw({'F', 'W', 'S', 7, 0, 0, 0, 0});
	header.rect(0, 1024 * 20, 0, 768 * 20);
	header.u16(30 << 8);
	header.u16(2);
	std::vector<uint8_t> movie = header.bytes;
	movie.insert(movie.end(), tags.begin(), tags.end());
	const uint32_t size = static_cast<uint32_t>(movie.size());
	for (int i = 0; i < 4; ++i) {
		movie[4 + static_cast<size_t>(i)] = static_cast<uint8_t>(size >> (8 * i));
	}
	return movie;
}

std::string text(Player& player, const std::string& path)
{
	const avm1::Value v = player.evaluate(path);
	return v.isString() ? v.asString() : v.isNumber() ? avm1::numberToString(v.asNumber()) : std::string();
}

} // namespace

TEST(SwfPlayer_SyntheticMovie)
{
	Player player;
	std::string error;
	CHECK(player.load(testMovie(), &error));
	if (!error.empty()) {
		std::fprintf(stderr, "load: %s\n", error.c_str());
	}
	CHECK(player.scriptErrors().empty());
	for (const std::string& e : player.scriptErrors()) {
		std::fprintf(stderr, "script error: %s\n", e.c_str());
	}
	// gotoAndStop(2) removed the box's square.
	CHECK(text(player, "box._currentframe") == "2");
	CHECK(player.evaluate("box").isObject());
	// attachMovie made a Widget, constructed by its registered class.
	CHECK(player.evaluate("w").isObject());
	CHECK(avm1::toBoolean(player.evaluate("w.ready")));
	CHECK(text(player, "w._name") == "w");
	// Frame 1's own script stopped the root.
	player.advance(0.5);
	CHECK(text(player, "_currentframe") == "1");
	// onEnterFrame ran once per tick (15 ticks in half a second).
	if (text(player, "ticks") != "15") { std::fprintf(stderr, "ticks = %s\n", text(player, "ticks").c_str()); CHECK(false); }
	// The field shows its variable.
	CHECK(text(player, "field.text") == "42");
	// Drawn: the button's square and the widget's (the box's is gone).
	const MenuFrame frame = player.frame();
	CHECK(frame.width == 1024 && frame.height == 768);
	CHECK(frame.batches.size() == 2);
	// Clicking the button (at 200..300, 0..100) runs its release action.
	CHECK(!player.evaluate("clicked").isBoolean());
	player.mouseMove(250, 50);
	CHECK(player.handCursor());
	player.mouseDown();
	player.mouseUp();
	CHECK(avm1::toBoolean(player.evaluate("clicked")));
	// Outside it nothing is clickable.
	player.mouseMove(600, 600);
	CHECK(!player.handCursor());
	// Script display properties.
	player.interpreter()->setMember(player.evaluate("w"), "_x", avm1::Value(300));
	CHECK(text(player, "w._x") == "300");
	CHECK(text(player, "w._width") == "100");
	CHECK(player.dumpDisplayList().find("clip #5 w") != std::string::npos);
}

// Masks cut triangles to a rectangle, interpolating what they carry.
TEST(SwfPlayer_MaskClipping)
{
	const auto vertex = [](float x, float y) {
		MenuVertex v {x, y, x / 100, y / 100, {1, 1, 1, 1}, {0, 0, 0, 0}};
		return v;
	};
	std::vector<MenuBatch> batches(2);
	batches[0].vertices = {vertex(0, 0), vertex(100, 0), vertex(0, 100)};
	batches[1].vertices = {vertex(200, 200), vertex(300, 200), vertex(200, 300)};
	clipBatches(batches, 0, 0, 0, 50, 50);
	CHECK(batches.size() == 1); // the second lies outside
	double area = 0;
	bool uvFollow = true;
	const auto& v = batches[0].vertices;
	for (size_t i = 0; i + 2 < v.size(); i += 3) {
		area += std::abs((v[i + 1].x - v[i].x) * (v[i + 2].y - v[i].y) - (v[i + 2].x - v[i].x) * (v[i + 1].y - v[i].y)) / 2;
	}
	for (const MenuVertex& p : v) {
		uvFollow &= std::abs(p.u - p.x / 100) < 1e-4f && p.x <= 50 && p.y <= 50;
	}
	CHECK(std::abs(area - 2500) < 0.5);
	CHECK(uvFollow);
}

namespace {

MenuBatch quad(float x0, float y0, float x1, float y1)
{
	const auto v = [](float x, float y) {
		MenuVertex m {x, y, x / 100, y / 100, {1, 1, 1, 1}, {0, 0, 0, 0}};
		return m;
	};
	MenuBatch b;
	b.vertices = {v(x0, y0), v(x1, y0), v(x1, y1), v(x0, y0), v(x1, y1), v(x0, y1)};
	return b;
}

double area(const std::vector<MenuBatch>& batches)
{
	double a = 0;
	for (const MenuBatch& batch : batches) {
		const auto& v = batch.vertices;
		for (size_t i = 0; i + 2 < v.size(); i += 3) {
			a += std::abs((v[i + 1].x - v[i].x) * (v[i + 2].y - v[i].y) - (v[i + 2].x - v[i].x) * (v[i + 1].y - v[i].y)) / 2;
		}
	}
	return a;
}

} // namespace

// Masks as the game's stencil (begin/end submit, disable): what a mask
// draws is not shown, what follows shows only where the stencil equals the
// level, by the mask's shape and not its box.
TEST(SwfPlayer_StencilMasks)
{
	Stencil stencil;
	stencil.setCpu(true); // the exact geometry path (the renderer has the GPU one)
	std::vector<MenuBatch> out;
	stencil.reset();
	// A triangular mask (drawn twice: overlap does not count twice at level 1).
	stencil.beginSubmit(out);
	MenuBatch triangle;
	triangle.vertices = {{0, 0, 0, 0, {1, 1, 1, 1}, {}}, {100, 0, 0, 0, {1, 1, 1, 1}, {}}, {0, 100, 0, 0, {1, 1, 1, 1}, {}}};
	out.push_back(triangle);
	out.push_back(triangle);
	stencil.endSubmit(out);
	CHECK(out.empty()); // colour writes off
	out.push_back(quad(0, 0, 100, 100));
	stencil.disable(out);
	CHECK(std::abs(area(out) - 5000) < 0.5);
	bool uvFollow = true;
	for (const MenuVertex& v : out[0].vertices) {
		uvFollow &= std::abs(v.u - v.x / 100) < 1e-4f && v.x + v.y <= 100.01f;
	}
	CHECK(uvFollow);
	// After disable nothing is cut.
	out.clear();
	stencil.flush(out);
	out.push_back(quad(0, 0, 100, 100));
	stencil.flush(out);
	CHECK(std::abs(area(out) - 10000) < 0.5);
	// Nested: the inner level shows where both masks drew; after the inner
	// disable the test is off (as RendDX9.dll leaves it) and the level drops.
	out.clear();
	stencil.reset();
	stencil.beginSubmit(out);
	out.push_back(quad(0, 0, 100, 100));
	stencil.endSubmit(out);
	stencil.beginSubmit(out);
	out.push_back(quad(50, 50, 150, 150));
	stencil.endSubmit(out);
	CHECK(stencil.level() == 2);
	out.push_back(quad(0, 0, 200, 200));
	stencil.disable(out);
	CHECK(std::abs(area(out) - 2500) < 0.5);
	out.push_back(quad(0, 0, 10, 10));
	stencil.disable(out);
	CHECK(stencil.level() == 0 && std::abs(area(out) - 2600) < 0.5);
	// A new first level clears the stencil.
	out.clear();
	stencil.beginSubmit(out);
	out.push_back(quad(300, 300, 310, 310));
	stencil.endSubmit(out);
	out.push_back(quad(0, 0, 200, 200));
	stencil.disable(out);
	CHECK(out.empty());
}

TEST(SwfPlayer_Natives)
{
	Player player;
	NativeRegistry& natives = player.natives();
	CHECK(natives.has("dice.bf2.Profile", "loginOnlineAccount"));
	CHECK(natives.slotType("dice.bf2.List", "m_rowCount") == NativeSlot::Getter);
	CHECK(natives.objects().size() == 23);
	natives.setFunction("dice.bf2.Client", "getPlayerName", [](avm1::Interpreter&, const std::vector<avm1::Value>& args) {
		return avm1::Value("Tester" + std::to_string(args.size()));
	});
	natives.setLocalizer([](const std::string& key) { return key == "HELLO" ? std::string("Hello") : key; });
	natives.setListRows("serverList", {{"Alpha", "32"}, {"Bravo", "16"}, {"Charlie", "64"}});
	CHECK(player.load(testMovie()));
	avm1::Interpreter& vm = *player.interpreter();
	const auto run = [&](const std::string& path, const std::string& method, std::vector<avm1::Value> args) {
		return vm.callMember(player.evaluate(path), method, std::move(args));
	};
	const avm1::Value global(vm.global());
	const avm1::Value bf2 = vm.getMember(vm.getMember(global, "dice"), "bf2");
	const auto call = [&](const char* object, const char* member, std::vector<avm1::Value> args) {
		return vm.callMember(vm.getMember(bf2, object), member, std::move(args));
	};
	(void)run;
	CHECK(vm.toString(call("Client", "getPlayerName", {avm1::Value(1)})) == "Tester1");
	CHECK(vm.toString(call("Locale", "getString", {avm1::Value("HELLO")})) == "Hello");
	CHECK(vm.toString(call("Locale", "getLanguage", {})) == "English");
	CHECK(call("Profile", "isOnline", {}).isUndefined());
	call("Profile", "isOnline", {});
	CHECK(natives.unboundCalls().size() == 1 && natives.unboundCalls()[0] == "dice.bf2.Profile.isOnline");
	CHECK(natives.calls().size() >= 4);

	// A list filling a holder's rows: rows "0" and "1" with cells.
	avm1::Value holder = vm.callMember(player.root(), "createEmptyMovieClip", {avm1::Value("holder"), avm1::Value(10)});
	for (int row = 0; row < 2; ++row) {
		avm1::Value clip = vm.callMember(holder, "createEmptyMovieClip", {avm1::Value(std::to_string(row)), avm1::Value(row)});
		vm.callMember(clip, "createTextField", {avm1::Value("cell0"), avm1::Value(0), avm1::Value(0), avm1::Value(0), avm1::Value(100),
												avm1::Value(20)});
	}
	const avm1::Value listClass = vm.getMember(bf2, "List");
	const avm1::Value list = vm.constructWith(listClass, {holder, avm1::Value("serverList"), avm1::Value("a b")});
	CHECK(list.isObject());
	CHECK(avm1::toBoolean(vm.callMember(list, "fillList", {avm1::Value(true)})));
	CHECK(vm.toNumber(vm.getMember(list, "m_rowCount")) == 3);
	CHECK(text(player, "holder.1.cell0.text") == "Bravo");
	vm.callMember(list, "scrollDown", {});
	CHECK(text(player, "holder.0.cell0.text") == "Bravo");
	CHECK(vm.toNumber(vm.getMember(list, "m_firstIndex")) == 1);
	vm.callMember(list, "sortList", {avm1::Value(1), avm1::Value(false)});
	CHECK(text(player, "holder.0.cell0.text") == "Alpha"); // 64, 32, 16 from the second row
	CHECK(player.scriptErrors().empty());
}

// Ten seconds of the shipped main menu: no script errors, the login page
// drawn, images loaded from Menu_client.zip, and the natives it calls.
TEST(SwfPlayer_MainMenu)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	const std::string mod = std::string(gameDir) + "/mods/bf2142/";
	const auto server = io::ZipArchive::open(mod + "Menu_server.zip");
	const auto client = io::ZipArchive::open(mod + "Menu_client.zip");
	CHECK(server && client);
	std::vector<uint8_t> dfm;
	if (!server || !client || !server->read("External/FlashMenu/mainMenu.dfm", dfm)) {
		CHECK(false);
		return;
	}
	Player player;
	size_t images = 0;
	player.setFileReader([&](const std::string& path, std::vector<uint8_t>& out) {
		const bool ok = client->read("External/FlashMenu/" + path, out);
		images += ok ? 1 : 0;
		return ok;
	});
	std::string error;
	CHECK(player.loadDfm(dfm, &error));
	for (int i = 0; i < 300; ++i) {
		player.tick();
	}
	for (const std::string& e : player.scriptErrors()) {
		std::fprintf(stderr, "mainMenu script error: %s\n", e.c_str());
	}
	CHECK(player.scriptErrors().empty());
	// The root stops at "login" (frame 14) and the login page shows.
	CHECK(text(player, "_currentframe") == "14");
	CHECK(avm1::toBoolean(player.evaluate("login._visible")));
	CHECK(text(player, "login.HEADER_accountname.m_mcLabel.labelTextField.text") == "HEADER_ACCOUNTNAME");
	const MenuFrame frame = player.frame();
	CHECK(frame.batches.size() > 20);
	CHECK(images > 10);
	// Stable: another second changes nothing in the display list.
	const std::string before = player.dumpDisplayList();
	for (int i = 0; i < 30; ++i) {
		player.tick();
	}
	CHECK(player.dumpDisplayList() == before);
	// Typing into the account field and pressing LOGIN asks the profile natives.
	const auto clickables = player.clickables();
	CHECK(clickables.size() >= 5);
	for (const auto& c : clickables) {
		if (c.target == "_level0.login.mcAccountName.mcInput.inputField") {
			player.mouseMove(c.x + 5, c.y + 5);
			player.mouseDown();
			player.mouseUp();
			for (char ch : std::string("Tester")) {
				player.keyDown(std::toupper(static_cast<unsigned char>(ch)), static_cast<uint8_t>(ch));
				player.keyUp(std::toupper(static_cast<unsigned char>(ch)));
			}
		}
	}
	CHECK(text(player, "login.mcAccountName.mcInput.inputField.text") == "Tester");
	player.natives().clearCalls();
	for (const auto& c : clickables) {
		if (c.target == "_level0.login.btnLogin") {
			player.mouseMove(c.x + c.width / 2, c.y + c.height / 2);
			player.mouseDown();
			player.mouseUp();
		}
	}
	bool askedProfile = false;
	for (const NativeCall& call : player.natives().calls()) {
		askedProfile |= call.object == "dice.bf2.Profile";
	}
	CHECK(askedProfile);
	CHECK(player.scriptErrors().empty());
	std::fprintf(stderr, "mainMenu: %zu batches, %zu images, unbound natives called:", frame.batches.size(), images);
	for (const std::string& name : player.natives().unboundCalls()) {
		std::fprintf(stderr, " %s", name.c_str());
	}
	std::fprintf(stderr, "\n");
}
