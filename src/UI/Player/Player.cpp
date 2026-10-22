#include "Player.h"

#include "DisplayList.h"
#include "Draw.h"
#include "Stage.h"

#include "UI/Dfm.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace dice::hfe::ui {

using avm1::Interpreter;
using avm1::ObjectRef;
using avm1::Value;
using player::ButtonObject;
using player::Clip;
using player::DisplayObject;
using player::Kind;
using player::kDepthOffset;
using player::TextField;

namespace {

constexpr float kTwips = 20.0f;
constexpr uint16_t kTagDoInitAction = 59;
constexpr uint16_t kTagRemoveObject = 5;
constexpr uint16_t kTagRemoveObject2 = 28;

constexpr uint32_t kMouseClipEvents = kClipEventPress | kClipEventRelease | kClipEventReleaseOutside | kClipEventRollOver |
	kClipEventRollOut | kClipEventDragOver | kClipEventDragOut;

const char* const kMouseMethods[] = {"onPress", "onRelease", "onReleaseOutside", "onRollOver", "onRollOut", "onDragOver", "onDragOut"};

bool isRemove(const SwfTag& tag) { return tag.code == kTagRemoveObject || tag.code == kTagRemoveObject2; }

// Flash key codes to the codes of button key conditions and keyPress clip
// events (SWF 7 spec, DefineButton2 CondKeyPress).
int buttonKeyCode(int code, uint32_t charCode)
{
	switch (code) {
	case 37: return 1;  // left
	case 39: return 2;  // right
	case 36: return 3;  // home
	case 35: return 4;  // end
	case 45: return 5;  // insert
	case 46: return 6;  // delete
	case 8: return 8;   // backspace
	case 13: return 13; // enter
	case 38: return 14; // up
	case 40: return 15; // down
	case 33: return 16; // page up
	case 34: return 17; // page down
	case 9: return 18;  // tab
	case 27: return 19; // escape
	default: return charCode >= 32 && charCode <= 126 ? static_cast<int>(charCode) : 0;
	}
}

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

void appendUtf8(std::string& out, uint32_t code)
{
	if (code < 0x80) {
		out += static_cast<char>(code);
	} else if (code < 0x800) {
		out += static_cast<char>(0xc0 | code >> 6);
		out += static_cast<char>(0x80 | (code & 0x3f));
	} else if (code < 0x10000) {
		out += static_cast<char>(0xe0 | code >> 12);
		out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
		out += static_cast<char>(0x80 | (code & 0x3f));
	} else {
		out += static_cast<char>(0xf0 | code >> 18);
		out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
		out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
		out += static_cast<char>(0x80 | (code & 0x3f));
	}
}

// Steps over one UTF-8 character.
size_t nextChar(const std::string& s, size_t at)
{
	if (at >= s.size()) {
		return s.size();
	}
	++at;
	while (at < s.size() && (static_cast<unsigned char>(s[at]) & 0xc0) == 0x80) {
		++at;
	}
	return at;
}

size_t previousChar(const std::string& s, size_t at)
{
	if (at == 0) {
		return 0;
	}
	--at;
	while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xc0) == 0x80) {
		--at;
	}
	return at;
}

// A primitive's text without running script (bound text fields).
std::string primitiveText(const Value& v)
{
	switch (v.type()) {
	case Value::Type::Undefined: return {};
	case Value::Type::Null: return "null";
	case Value::Type::Boolean: return v.asBoolean() ? "true" : "false";
	case Value::Type::Number: return avm1::numberToString(v.asNumber());
	case Value::Type::String: return v.asString();
	case Value::Type::Object: return v.asObject()->isFunction() ? "[type Function]" : "[object Object]";
	}
	return {};
}

} // namespace

Player::Player() = default;

Player::~Player() { reset(); }

void Player::reset()
{
	m_actions.clear();
	m_inits.clear();
	m_hover.clear();
	m_pressed.clear();
	m_focus.reset();
	m_drag = Drag();
	m_classes.clear();
	m_root.reset();
	m_vm.reset(); // breaks the cycles between clips, prototypes and closures
	m_drawer.reset();
	m_timelines.clear();
	m_rootTimeline.reset();
	m_exportNames.clear();
	m_dictionary = swf::CharacterDictionary();
	m_swf.clear();
	m_movie = SwfMovie();
	m_time = 0;
	m_tick = 0;
	m_instanceCount = 0;
	m_errors.clear();
	m_traces.clear();
}

bool Player::loadDfm(const std::vector<uint8_t>& dfm, std::string* error)
{
	auto swf = decodeDfm(dfm, error);
	if (!swf) {
		return false;
	}
	return load(std::move(*swf), error);
}

bool Player::load(std::vector<uint8_t> bytes, std::string* error)
{
	reset();
	if (bytes.size() > 3 && bytes[0] == 'C') {
		auto plain = uncompressSwf(bytes, error);
		if (!plain) {
			return false;
		}
		bytes = std::move(*plain);
	}
	m_swf = std::move(bytes);
	auto movie = parseSwf(m_swf, error);
	if (!movie) {
		m_swf.clear();
		return false;
	}
	m_movie = std::move(*movie);
	if (!m_dictionary.load(m_swf, m_movie, error)) {
		m_swf.clear();
		return false;
	}
	m_frameRate = m_movie.header.frameRate ? m_movie.header.framesPerSecond() : 30.0;
	m_stageWidth = m_movie.header.widthTwips() / kTwips;
	m_stageHeight = m_movie.header.heightTwips() / kTwips;
	for (const auto& [name, id] : m_dictionary.exports()) {
		m_exportNames.emplace(id, name);
	}
	m_drawer = std::make_unique<Drawer>(m_dictionary);
	// The engine's fonts, from the game's root ("/Fonts/...").
	m_drawer->setFontReader([this](const std::string& path, std::string& text) {
		std::vector<uint8_t> bytes;
		if (!m_reader || !m_reader("/" + path, bytes)) {
			return false;
		}
		text.assign(bytes.begin(), bytes.end());
		return true;
	});
	m_vm = std::make_unique<Interpreter>(static_cast<avm1::Host*>(this));
	m_rootTimeline = std::make_unique<Timeline>(Timeline::fromTags(m_movie.tags, m_swf));
	installBuiltins();
	m_natives.install(*m_vm);
	installDefaults();

	m_root = m_vm->make<Clip>(*this, 0, m_rootTimeline.get());
	m_root->setProto(m_vm->getMember(m_vm->getMember(Value(m_vm->global()), "MovieClip"), "prototype").object());

	// #initclip blocks: every class definition, in tag order, before frame 1.
	for (const SwfTag& tag : m_movie.tags) {
		if (tag.code == kTagDoInitAction && tag.length > 2) {
			m_vm->execute(m_swf.data() + tag.offset + 2, tag.length - 2, {m_root, nullptr});
		}
	}
	enterFrame(*m_root, 0, true);
	runActions();
	syncTextFields();
	return true;
}

const Timeline& Player::timelineOf(uint16_t sprite)
{
	auto& timeline = m_timelines[sprite];
	if (!timeline) {
		const swf::Sprite* s = m_dictionary.sprite(sprite);
		timeline = std::make_unique<Timeline>(s ? Timeline::fromTags(s->tags, m_swf) : Timeline::fromTags({}, m_swf));
	}
	return *timeline;
}

const Timeline& Player::emptyTimeline()
{
	if (!m_emptyTimeline) {
		m_emptyTimeline = std::make_unique<Timeline>(Timeline::fromTags({}, m_swf));
	}
	return *m_emptyTimeline;
}

ObjectRef Player::root() const { return m_root; }

// --- Ticks

void Player::advance(double seconds)
{
	if (!m_root) {
		return;
	}
	m_time += std::max(0.0, seconds);
	const double period = 1.0 / m_frameRate;
	// At most a second's worth per call; a longer stall is dropped.
	const int most = std::max(1, static_cast<int>(std::lround(m_frameRate)));
	int ticks = 0;
	while (m_time + 1e-9 >= period && ticks < most) {
		m_time -= period;
		tick();
		++ticks;
	}
	if (m_time + 1e-9 >= period) {
		m_time = 0; // too far behind: drop the rest
	}
	m_time = std::max(0.0, m_time);
}

void Player::tick()
{
	if (!m_root) {
		return;
	}
	++m_tick;
	advanceClip(*m_root);
	runActions();
	enterFrameEvents();
	m_vm->advance(1000.0 / m_frameRate);
	runActions();
	syncTextFields();
}

void Player::advanceClip(Clip& clip)
{
	if (clip.bornTick != m_tick && !clip.image) {
		if (clip.frame < 0) {
			enterFrame(clip, 0, true);
		} else if (clip.playing && clip.frameCount() > 1) {
			const int next = clip.frame + 1;
			if (next >= clip.frameCount()) {
				gotoFrame(clip, 0, true);
			} else {
				enterFrame(clip, next, true);
			}
		}
	}
	std::vector<std::shared_ptr<DisplayObject>> children;
	children.reserve(clip.children.size());
	for (const auto& [depth, child] : clip.children) {
		children.push_back(child);
	}
	for (const auto& child : children) {
		if (child->parent != &clip) {
			continue; // removed by a sibling's frame
		}
		if (child->kind == Kind::Clip) {
			advanceClip(static_cast<Clip&>(*child));
		} else if (child->kind == Kind::Button) {
			for (const auto& part : static_cast<ButtonObject&>(*child).parts) {
				if (part && part->kind == Kind::Clip) {
					advanceClip(static_cast<Clip&>(*part));
				}
			}
		}
	}
}

void Player::enterFrame(Clip& clip, int frame, bool runActions)
{
	if (frame < 0 || frame >= clip.frameCount()) {
		return;
	}
	clip.frame = frame;
	const Frame& f = clip.timeline->frames[static_cast<size_t>(frame)];
	auto self = std::static_pointer_cast<Clip>(clip.shared_from_this());
	if (runActions) {
		for (const ActionBlock& block : f.actions) {
			queueActions(self, block);
		}
	}
	for (const SwfTag& tag : f.controls) {
		if (isRemove(tag)) {
			if (auto child = clip.childAt(removedDepth(tag, m_swf))) {
				remove(*child);
			}
		} else if (tag.code == 4 || tag.code == 26) {
			place(clip, parsePlaceObject(tag, m_swf));
		}
	}
}

void Player::gotoFrame(Clip& clip, int frame, bool play)
{
	clip.playing = play;
	if (clip.frameCount() == 0 || clip.image) {
		return;
	}
	frame = std::clamp(frame, 0, clip.frameCount() - 1);
	if (frame == clip.frame) {
		return;
	}
	if (frame > clip.frame) {
		for (int i = clip.frame + 1; i < frame; ++i) {
			enterFrame(clip, i, false);
		}
		enterFrame(clip, frame, true);
		return;
	}
	// Back: what the timeline holds at `frame`, replayed from the start.
	std::map<int, PlaceRecord> state;
	for (int i = 0; i <= frame; ++i) {
		for (const SwfTag& tag : clip.timeline->frames[static_cast<size_t>(i)].controls) {
			if (isRemove(tag)) {
				state.erase(removedDepth(tag, m_swf));
				continue;
			}
			const PlaceRecord r = parsePlaceObject(tag, m_swf);
			auto it = state.find(r.depth);
			if (r.hasCharacter && (it == state.end() || !r.move || it->second.character != r.character)) {
				PlaceRecord fresh = r;
				if (it != state.end() && r.move) {
					if (!fresh.hasMatrix) {
						fresh.hasMatrix = it->second.hasMatrix;
						fresh.matrix = it->second.matrix;
					}
					if (!fresh.hasColour) {
						fresh.hasColour = it->second.hasColour;
						fresh.colour = it->second.colour;
					}
					if (!fresh.hasName) {
						fresh.hasName = it->second.hasName;
						fresh.name = it->second.name;
					}
				}
				fresh.move = false;
				state[r.depth] = fresh;
			} else if (it != state.end()) {
				PlaceRecord& s = it->second;
				if (r.hasMatrix) {
					s.hasMatrix = true;
					s.matrix = r.matrix;
				}
				if (r.hasColour) {
					s.hasColour = true;
					s.colour = r.colour;
				}
				if (r.hasRatio) {
					s.hasRatio = true;
					s.ratio = r.ratio;
				}
				if (r.hasName) {
					s.hasName = true;
					s.name = r.name;
				}
				if (r.hasClipDepth) {
					s.hasClipDepth = true;
					s.clipDepth = r.clipDepth;
				}
			}
		}
	}
	std::vector<std::shared_ptr<DisplayObject>> current;
	for (const auto& [depth, child] : clip.children) {
		if (depth < kDepthOffset && child->placedByTimeline) {
			current.push_back(child);
		}
	}
	for (const auto& child : current) {
		const auto it = state.find(child->depth);
		if (it == state.end() || it->second.character != child->character) {
			remove(*child);
		} else {
			applyPlace(*child, it->second, false);
		}
	}
	for (const auto& [depth, record] : state) {
		if (!clip.childAt(depth)) {
			place(clip, record);
		}
	}
	clip.frame = frame;
	auto self = std::static_pointer_cast<Clip>(clip.shared_from_this());
	for (const ActionBlock& block : clip.timeline->frames[static_cast<size_t>(frame)].actions) {
		queueActions(self, block);
	}
}

// --- Placing and removing

std::shared_ptr<DisplayObject> Player::create(uint16_t character)
{
	Interpreter& vm = *m_vm;
	const auto proto = [&vm](const char* cls) {
		return vm.getMember(vm.getMember(Value(vm.global()), cls), "prototype").object();
	};
	std::shared_ptr<DisplayObject> object;
	if (m_dictionary.sprite(character)) {
		object = vm.make<Clip>(*this, character, &timelineOf(character));
		object->setProto(proto("MovieClip"));
	} else if (m_dictionary.shape(character)) {
		object = vm.make<DisplayObject>(*this, Kind::Shape, character);
	} else if (m_dictionary.morphShape(character)) {
		object = vm.make<DisplayObject>(*this, Kind::MorphShape, character);
	} else if (const swf::EditText* field = m_dictionary.editText(character)) {
		object = vm.make<TextField>(*this, character, *field);
		object->setProto(proto("TextField"));
	} else if (const swf::Button* button = m_dictionary.button(character)) {
		auto b = vm.make<ButtonObject>(*this, character, *button);
		b->setProto(proto("Button"));
		for (const swf::ButtonRecord& record : button->records) {
			std::shared_ptr<DisplayObject> part = create(record.characterId);
			if (part) {
				part->matrix = record.matrix;
				part->colour = record.colorTransform;
				part->depth = record.depth;
				part->bornTick = m_tick;
				if (part->kind == Kind::Clip) {
					enterFrame(static_cast<Clip&>(*part), 0, true);
				}
			}
			b->parts.push_back(part);
		}
		object = b;
	}
	return object;
}

void Player::adopt(Clip& parent, const std::shared_ptr<DisplayObject>& object, int depth)
{
	if (auto existing = parent.childAt(depth); existing && existing != object) {
		remove(*existing);
	}
	if (object->parent) {
		object->parent->children.erase(object->depth);
	}
	object->parent = &parent;
	object->depth = depth;
	object->removed = false;
	object->bornTick = m_tick;
	parent.children[depth] = object;
	if (object->name.empty() && object->kind != Kind::Shape && object->kind != Kind::MorphShape) {
		object->name = "instance" + std::to_string(++m_instanceCount);
	}
}

void Player::applyPlace(DisplayObject& object, const PlaceRecord& r, bool fresh)
{
	if (r.hasMatrix && (fresh || !object.scriptTransformed)) {
		object.matrix = r.matrix;
		object.decomposed = false;
	}
	if (r.hasColour && (fresh || !object.scriptTransformed)) {
		object.colour = r.colour;
	}
	if (r.hasRatio) {
		object.ratio = r.ratio;
	}
	if (r.hasName) {
		object.name = r.name;
	}
	if (r.hasClipDepth) {
		object.clipDepth = r.clipDepth;
	}
	if (r.hasEvents) {
		object.events = r.events;
	}
}

void Player::place(Clip& parent, const PlaceRecord& r)
{
	auto existing = parent.childAt(r.depth);
	if (!r.hasCharacter) {
		if (existing) {
			applyPlace(*existing, r, false);
		}
		return;
	}
	if (existing && r.move && existing->character == r.character) {
		applyPlace(*existing, r, false);
		return;
	}
	std::shared_ptr<DisplayObject> object = create(r.character);
	if (!object) {
		return;
	}
	if (existing && r.move) {
		object->matrix = existing->matrix; // a new character in the old one's place
		object->colour = existing->colour;
		object->name = existing->name;
	}
	object->placedByTimeline = true;
	applyPlace(*object, r, true);
	adopt(parent, object, r.depth);
	start(object);
}

void Player::start(const std::shared_ptr<DisplayObject>& object)
{
	if (object->kind == Kind::Clip) {
		auto& clip = static_cast<Clip&>(*object);
		if (clip.frame < 0) {
			enterFrame(clip, 0, true);
		}
		m_inits.push_back({object, nullptr});
		Action load;
		load.type = Action::Type::Event;
		load.target = object;
		load.bits = kClipEventLoad;
		load.method = "onLoad";
		m_actions.push_back(load);
	} else if (object->kind == Kind::Text) {
		auto& field = static_cast<TextField&>(*object);
		if (!field.variable.empty()) {
			// A bound field shows its variable, or gives it its text.
			syncTextField(field);
			if (field.lastVariableText.empty() && !field.value.empty()) {
				writeVariable(*m_vm, field);
			}
		}
		field.applyAutoSize();
	}
}

void Player::runPendingInits()
{
	while (!m_inits.empty()) {
		std::vector<PendingInit> inits = std::move(m_inits);
		m_inits.clear();
		for (const PendingInit& init : inits) {
			runInit(init.object, init.initObject);
		}
	}
}

// Construct events, then the registered class's constructor (with
// __proto__ already set, so that component parameters reach their
// setters), then Initialize events. 0x40000 construct events are how the
// menus' components get their parameters.
void Player::runInit(const std::shared_ptr<DisplayObject>& object, const ObjectRef& initObject)
{
	if (object->removed) {
		return;
	}
	Interpreter& vm = *m_vm;
	const ObjectRef constructor = registeredClass(object->character);
	if (constructor) {
		if (ObjectRef proto = vm.getMember(Value(constructor), "prototype").object()) {
			object->setProto(proto);
		}
	}
	if (initObject) {
		std::vector<std::string> names;
		initObject->ownEnumerableNames(names);
		for (const std::string& name : names) {
			object->setMember(vm, name, vm.getMember(Value(initObject), name));
		}
	}
	dispatch(*object, kClipEventConstruct, {});
	if (constructor) {
		vm.initializeInstance(object, Value(constructor), {});
	}
	dispatch(*object, kClipEventInitialize, {});
}

std::shared_ptr<DisplayObject> Player::instantiate(Clip& parent, uint16_t character, int depth, const std::string& name)
{
	std::shared_ptr<DisplayObject> object = create(character);
	if (!object) {
		return nullptr;
	}
	object->name = name;
	adopt(parent, object, depth);
	start(object);
	return object;
}

std::shared_ptr<Clip> Player::createEmptyClip(Clip& parent, const std::string& name, int depth)
{
	auto clip = m_vm->make<Clip>(*this, 0, &emptyTimeline());
	clip->setProto(m_vm->getMember(m_vm->getMember(Value(m_vm->global()), "MovieClip"), "prototype").object());
	clip->name = name;
	adopt(parent, clip, depth);
	start(clip);
	runPendingInits();
	return clip;
}

std::shared_ptr<TextField> Player::createTextField(Clip& parent, const std::string& name, int depth, const swf::Rect& bounds)
{
	swf::EditText definition;
	definition.bounds = bounds;
	definition.hasFont = true;
	definition.fontHeight = 12 * 20;
	definition.readOnly = true;
	definition.hasTextColor = true;
	auto field = m_vm->make<TextField>(*this, 0, definition);
	field->setProto(m_vm->getMember(m_vm->getMember(Value(m_vm->global()), "TextField"), "prototype").object());
	field->name = name;
	adopt(parent, field, depth);
	start(field);
	return field;
}

std::shared_ptr<DisplayObject> Player::attachMovie(Clip& parent, const std::string& linkage, const std::string& name, int depth,
	const ObjectRef& initObject)
{
	const uint16_t id = m_dictionary.exportedId(linkage);
	if (id == 0) {
		return nullptr;
	}
	std::shared_ptr<DisplayObject> object = create(id);
	if (!object) {
		return nullptr;
	}
	object->name = name;
	adopt(parent, object, depth);
	start(object);
	if (object->kind == Kind::Clip && initObject) {
		m_inits.back().initObject = initObject;
	}
	runPendingInits();
	return object;
}

std::shared_ptr<DisplayObject> Player::duplicate(DisplayObject& source, const std::string& name, int depth, const ObjectRef& initObject)
{
	Clip* parent = source.parent;
	if (!parent) {
		return nullptr;
	}
	std::shared_ptr<DisplayObject> object;
	if (source.character == 0 && source.kind == Kind::Clip) {
		object = m_vm->make<Clip>(*this, 0, &emptyTimeline());
		object->setProto(source.proto());
	} else {
		object = create(source.character);
	}
	if (!object) {
		return nullptr;
	}
	object->name = name;
	object->matrix = source.matrix;
	object->colour = source.colour;
	object->events = source.events;
	object->visible = source.visible;
	adopt(*parent, object, depth);
	start(object);
	if (object->kind == Kind::Clip && initObject) {
		m_inits.back().initObject = initObject;
	}
	runPendingInits();
	return object;
}

void Player::queueUnload(DisplayObject& object)
{
	if (object.kind != Kind::Clip) {
		return;
	}
	auto& clip = static_cast<Clip&>(object);
	for (const auto& [depth, child] : clip.children) {
		queueUnload(*child);
	}
	Action unload;
	unload.type = Action::Type::Event;
	unload.target = std::static_pointer_cast<DisplayObject>(object.shared_from_this());
	unload.bits = kClipEventUnload;
	unload.method = "onUnload";
	m_actions.push_back(unload);
}

void Player::remove(DisplayObject& object)
{
	Clip* parent = object.parent;
	if (!parent) {
		return;
	}
	auto keep = object.shared_from_this();
	queueUnload(object);
	parent->children.erase(object.depth);
	object.parent = nullptr;
	object.removed = true;
	if (TextField* f = focus(); f && (f == &object || object.isAncestorOf(f))) {
		m_focus.reset();
	}
}

void Player::swapDepths(DisplayObject& object, int depth)
{
	Clip* parent = object.parent;
	if (!parent || depth == object.depth) {
		return;
	}
	auto self = std::static_pointer_cast<DisplayObject>(object.shared_from_this());
	auto other = parent->childAt(depth);
	parent->children.erase(object.depth);
	if (other) {
		other->depth = object.depth;
		parent->children[other->depth] = other;
		other->placedByTimeline = false;
	}
	object.depth = depth;
	object.placedByTimeline = false;
	parent->children[depth] = self;
}

ObjectRef Player::registeredClass(uint16_t character) const
{
	if (character == 0) {
		return nullptr;
	}
	const auto range = m_exportNames.equal_range(character);
	for (auto it = range.first; it != range.second; ++it) {
		const auto found = m_classes.find(it->second);
		if (found != m_classes.end()) {
			return found->second;
		}
	}
	return nullptr;
}

// --- Actions

void Player::queueActions(const std::shared_ptr<DisplayObject>& target, const ActionBlock& block)
{
	Action action;
	action.target = target;
	action.offset = block.offset;
	action.length = block.length;
	m_actions.push_back(action);
}

void Player::runBlock(const std::shared_ptr<DisplayObject>& target, size_t offset, size_t length)
{
	if (offset + length > m_swf.size() || length == 0) {
		return;
	}
	m_vm->execute(m_swf.data() + offset, length, {target, nullptr});
}

void Player::dispatch(DisplayObject& object, uint32_t bits, const std::string& method, const std::vector<Value>& args)
{
	auto self = std::static_pointer_cast<DisplayObject>(object.shared_from_this());
	if (bits) {
		const std::vector<ClipEvent> events = object.events; // a handler may replace them
		for (const ClipEvent& event : events) {
			if ((event.events & bits) && !(event.events & kClipEventKeyPress && bits == kClipEventKeyPress)) {
				runBlock(self, event.actions.offset, event.actions.length);
			}
		}
	}
	if (!method.empty()) {
		m_vm->callMethod(self, method, args);
	}
}

void Player::runActions()
{
	if (m_runningActions > 0 || !m_vm) {
		return;
	}
	++m_runningActions;
	for (;;) {
		runPendingInits();
		if (m_actions.empty()) {
			break;
		}
		Action action = std::move(m_actions.front());
		m_actions.pop_front();
		if (action.type == Action::Type::Block) {
			if (!action.target->removed) {
				runBlock(action.target, action.offset, action.length);
			}
		} else if (!action.target->removed || action.bits == kClipEventUnload) {
			dispatch(*action.target, action.bits, action.method);
		}
	}
	--m_runningActions;
}

void Player::allObjects(DisplayObject& from, std::vector<std::shared_ptr<DisplayObject>>& out)
{
	out.push_back(std::static_pointer_cast<DisplayObject>(from.shared_from_this()));
	if (from.kind == Kind::Clip) {
		for (const auto& [depth, child] : static_cast<Clip&>(from).children) {
			allObjects(*child, out);
		}
	}
}

void Player::enterFrameEvents() { clipEventsToAll(kClipEventEnterFrame, "onEnterFrame"); }

void Player::clipEventsToAll(uint32_t bits, const char* method, int keyCode)
{
	if (!m_root) {
		return;
	}
	std::vector<std::shared_ptr<DisplayObject>> all;
	allObjects(*m_root, all);
	for (const auto& object : all) {
		if (object->kind != Kind::Clip) {
			continue;
		}
		if (keyCode >= 0) {
			// keyPress clip events for one key.
			for (const ClipEvent& event : object->events) {
				if ((event.events & kClipEventKeyPress) && event.keyCode == keyCode) {
					queueActions(object, event.actions);
				}
			}
			continue;
		}
		Action action;
		action.type = Action::Type::Event;
		action.target = object;
		action.bits = bits;
		action.method = method ? method : "";
		m_actions.push_back(action);
	}
	runActions();
}

// --- Text fields bound to variables

namespace {

bool variableSlot(Interpreter& vm, TextField& field, ObjectRef& holder, std::string& name)
{
	if (!field.parent || field.variable.empty()) {
		return false;
	}
	const std::string& var = field.variable;
	const size_t split = var.find_last_of(":.");
	const ObjectRef from = field.parent->shared_from_this();
	if (split == std::string::npos) {
		holder = from;
		name = var;
	} else {
		holder = vm.resolveTarget(from, var.substr(0, split));
		name = var.substr(split + 1);
	}
	return holder != nullptr && !name.empty();
}

} // namespace

void Player::writeVariable(Interpreter& vm, TextField& field)
{
	ObjectRef holder;
	std::string name;
	if (!variableSlot(vm, field, holder, name)) {
		return;
	}
	const std::string text = field.plainText();
	field.lastVariableText = text;
	holder->setMember(vm, name, Value(text));
}

void Player::syncTextField(TextField& field)
{
	ObjectRef holder;
	std::string name;
	if (!variableSlot(*m_vm, field, holder, name)) {
		return;
	}
	Value value;
	if (!holder->getMember(*m_vm, name, value) || value.isUndefined()) {
		return;
	}
	const std::string text = primitiveText(value);
	if (text != field.lastVariableText) {
		field.lastVariableText = text;
		field.value = text;
		field.caret = std::min(field.caret, field.value.size());
		field.applyAutoSize();
	}
}

void Player::syncTextFields()
{
	if (!m_root) {
		return;
	}
	std::vector<std::shared_ptr<DisplayObject>> all;
	allObjects(*m_root, all);
	for (const auto& object : all) {
		if (object->kind == Kind::Text && !static_cast<TextField&>(*object).variable.empty()) {
			syncTextField(static_cast<TextField&>(*object));
		}
	}
}

// --- Images

bool Player::loadImage(Clip& clip, const std::string& url)
{
	if (url.empty()) {
		return false;
	}
	std::string path = url;
	if (path.rfind("./", 0) == 0) {
		path = path.substr(2);
	}
	std::vector<uint8_t> data;
	if (!m_reader || !m_reader(path, data) || data.empty()) {
		trace("loadMovie: cannot read " + url);
		return false;
	}
	swf::Bitmap bitmap;
	std::string error;
	if (!swf::decodeJpeg(data.data(), data.size(), bitmap, &error)) {
		trace("loadMovie: cannot decode " + url + ": " + error);
		return false;
	}
	swf::unpremultiply(bitmap);
	auto image = std::make_shared<MenuImage>();
	image->width = bitmap.width;
	image->height = bitmap.height;
	image->rgba = std::move(bitmap.rgba);
	// The clip's content gives way to the image (its transform and
	// properties stay).
	std::vector<std::shared_ptr<DisplayObject>> children;
	for (const auto& [depth, child] : clip.children) {
		children.push_back(child);
	}
	for (const auto& child : children) {
		remove(*child);
	}
	clip.timeline = &emptyTimeline();
	clip.frame = 0;
	clip.drawing = swf::Shape();
	clip.image = image;
	clip.url = url;
	clip.bytes = data.size();
	return true;
}

// --- Dragging and focus

void Player::startDrag(DisplayObject& object, bool lockCenter, const double* bounds)
{
	m_drag.object = std::static_pointer_cast<DisplayObject>(object.shared_from_this());
	m_drag.lockCenter = lockCenter;
	m_drag.constrained = bounds != nullptr;
	if (bounds) {
		std::copy(bounds, bounds + 4, m_drag.bounds);
	}
	m_drag.offsetX = m_drag.offsetY = 0;
	if (!lockCenter && object.parent) {
		const swf::Point local = inverse(object.parent->worldMatrix()).apply({m_mouseX * kTwips, m_mouseY * kTwips});
		m_drag.offsetX = object.matrix.tx - local.x;
		m_drag.offsetY = object.matrix.ty - local.y;
	}
}

void Player::stopDrag() { m_drag.object.reset(); }

TextField* Player::focus() const { return m_focus.lock().get(); }

void Player::setFocus(const std::shared_ptr<TextField>& field)
{
	auto old = m_focus.lock();
	if (old == field) {
		return;
	}
	m_focus = field;
	if (field) {
		field->caret = field->value.size();
	}
	const Value oldValue = old ? Value(old) : Value::null();
	const Value newValue = field ? Value(field) : Value::null();
	if (old) {
		m_vm->callMethod(old, "onKillFocus", {newValue});
	}
	if (field) {
		m_vm->callMethod(field, "onSetFocus", {oldValue});
	}
	broadcast("Selection", "onSetFocus", {oldValue, newValue});
}

// --- Mouse

bool Player::isInteractive(DisplayObject& object)
{
	Interpreter& vm = *m_vm;
	auto self = object.shared_from_this();
	if (object.kind == Kind::Text) {
		return static_cast<TextField&>(object).input;
	}
	if (object.kind != Kind::Button && object.kind != Kind::Clip) {
		return false;
	}
	const Value enabled = vm.getMember(Value(self), "enabled");
	if (!enabled.isUndefined() && !avm1::toBoolean(enabled)) {
		return false;
	}
	if (object.kind == Kind::Button) {
		return true;
	}
	for (const ClipEvent& event : object.events) {
		if (event.events & kMouseClipEvents) {
			return true;
		}
	}
	for (const char* method : kMouseMethods) {
		if (vm.getMember(Value(self), method).isFunction()) {
			return true;
		}
	}
	return false;
}

// The interactive objects under the point: the deepest first, then each
// interactive ancestor. The menus need all of them: their components take
// on(release) on the outer instance while the button art inside fades on
// its own onRollOver.
bool Player::pick(Clip& clip, swf::Point stage, Chain& chain)
{
	for (auto it = clip.children.rbegin(); it != clip.children.rend(); ++it) {
		const std::shared_ptr<DisplayObject> child = it->second;
		if (!child->visible || child->isMask || child->clipDepth != 0 || child->kind == Kind::Shape ||
			child->kind == Kind::MorphShape) {
			continue;
		}
		const bool interactive = isInteractive(*child);
		if (child->kind == Kind::Clip) {
			const Value hitArea = interactive ? m_vm->getMember(Value(child), "hitArea") : Value();
			if (auto* area = dynamic_cast<DisplayObject*>(hitArea.object().get())) {
				if (area->hitTest(stage, true, true)) {
					chain.push_back(child);
					return true;
				}
				continue;
			}
			if (pick(static_cast<Clip&>(*child), stage, chain)) {
				if (interactive) {
					chain.push_back(child);
				}
				return true;
			}
			if (interactive && child->hitTest(stage, true, false)) {
				chain.push_back(child);
				return true;
			}
		} else if (interactive && child->hitTest(stage, true, false)) {
			chain.push_back(child);
			return true;
		}
	}
	return false;
}

Player::Chain Player::pickAt(float x, float y)
{
	Chain chain;
	if (m_root && m_root->visible) {
		pick(*m_root, {x * kTwips, y * kTwips}, chain);
	}
	return chain;
}

namespace {

bool inChain(const std::vector<std::shared_ptr<DisplayObject>>& chain, const std::shared_ptr<DisplayObject>& object)
{
	return std::find(chain.begin(), chain.end(), object) != chain.end();
}

} // namespace

void Player::updateHover(const Chain& now)
{
	if (!m_pressed.empty()) {
		// Dragging: the pressed objects learn when the mouse leaves or comes back.
		for (const auto& object : m_pressed) {
			const bool over = inChain(now, object), was = inChain(m_hover, object);
			if (over && !was) {
				mouseEvent(*object, kClipEventDragOver, "onDragOver", swf::kCondOutDownToOverDown, swf::kButtonDown);
			} else if (!over && was) {
				mouseEvent(*object, kClipEventDragOut, "onDragOut", swf::kCondOverDownToOutDown, swf::kButtonOver);
			}
		}
		m_hover.clear();
		for (const auto& object : now) {
			if (inChain(m_pressed, object)) {
				m_hover.push_back(object);
			}
		}
		return;
	}
	const Chain old = m_hover;
	m_hover = now;
	for (const auto& object : old) {
		if (!inChain(now, object)) {
			mouseEvent(*object, kClipEventRollOut, "onRollOut", swf::kCondOverUpToIdle, swf::kButtonUp);
		}
	}
	for (const auto& object : now) {
		if (!inChain(old, object) && object->kind != Kind::Text) {
			mouseEvent(*object, kClipEventRollOver, "onRollOver", swf::kCondIdleToOverUp, swf::kButtonOver);
		}
	}
}

void Player::mouseEvent(DisplayObject& object, uint32_t clipBits, const char* method, uint16_t buttonConditions, uint8_t buttonState)
{
	if (object.removed) {
		return;
	}
	if (object.kind == Kind::Button) {
		auto& button = static_cast<ButtonObject&>(object);
		button.state = buttonState;
		if (object.parent) {
			auto target = std::static_pointer_cast<DisplayObject>(object.parent->shared_from_this());
			for (const swf::ButtonCondAction& action : button.button.actions) {
				if ((action.conditions & buttonConditions) && action.keyCode == 0) {
					runBlock(target, action.actionOffset, action.actionLength);
				}
			}
		}
		dispatch(object, 0, method);
	} else if (object.kind == Kind::Clip) {
		// Button-like clips follow their _up/_over/_down frames.
		auto& clip = static_cast<Clip&>(object);
		const char* label = buttonState == swf::kButtonDown ? "_down" : buttonState == swf::kButtonOver ? "_over" : "_up";
		const auto found = clip.timeline->labels.find(label);
		if (found != clip.timeline->labels.end()) {
			gotoFrame(clip, found->second, false);
		}
		dispatch(object, clipBits, method);
	}
}

void Player::mouseMove(float x, float y)
{
	m_mouseX = x;
	m_mouseY = y;
	if (!m_root) {
		return;
	}
	if (auto dragged = m_drag.object.lock(); dragged && dragged->parent) {
		swf::Point local = inverse(dragged->parent->worldMatrix()).apply({x * kTwips, y * kTwips});
		local.x += m_drag.offsetX;
		local.y += m_drag.offsetY;
		if (m_drag.constrained) {
			local.x = std::clamp(local.x, static_cast<float>(m_drag.bounds[0] * kTwips), static_cast<float>(m_drag.bounds[2] * kTwips));
			local.y = std::clamp(local.y, static_cast<float>(m_drag.bounds[1] * kTwips), static_cast<float>(m_drag.bounds[3] * kTwips));
		}
		dragged->matrix.tx = local.x;
		dragged->matrix.ty = local.y;
		dragged->scriptTransformed = true;
	}
	broadcast("Mouse", "onMouseMove");
	clipEventsToAll(kClipEventMouseMove, "onMouseMove");

	updateHover(pickAt(x, y));
	runActions();
	syncTextFields();
}

void Player::mouseDown()
{
	m_mouseIsDown = true;
	if (!m_root) {
		return;
	}
	broadcast("Mouse", "onMouseDown");
	clipEventsToAll(kClipEventMouseDown, "onMouseDown");
	const Chain now = pickAt(m_mouseX, m_mouseY);
	updateHover(now);
	if (!now.empty() && now.front()->kind == Kind::Text) {
		setFocus(std::static_pointer_cast<TextField>(now.front()));
	} else {
		if (focus()) {
			setFocus(nullptr);
		}
		m_pressed = now;
		for (const auto& object : now) {
			mouseEvent(*object, kClipEventPress, "onPress", swf::kCondOverUpToOverDown, swf::kButtonDown);
		}
	}
	runActions();
	syncTextFields();
}

void Player::mouseUp()
{
	m_mouseIsDown = false;
	if (!m_root) {
		return;
	}
	broadcast("Mouse", "onMouseUp");
	clipEventsToAll(kClipEventMouseUp, "onMouseUp");
	if (!m_pressed.empty()) {
		const Chain pressed = std::move(m_pressed);
		m_pressed.clear();
		const Chain now = pickAt(m_mouseX, m_mouseY);
		for (const auto& object : pressed) {
			if (inChain(now, object)) {
				mouseEvent(*object, kClipEventRelease, "onRelease", swf::kCondOverDownToOverUp, swf::kButtonOver);
			} else {
				mouseEvent(*object, kClipEventReleaseOutside, "onReleaseOutside", swf::kCondOutDownToIdle, swf::kButtonUp);
			}
		}
		// What was pressed and left behind is no longer hovered.
		Chain hover;
		for (const auto& object : m_hover) {
			if (inChain(now, object)) {
				hover.push_back(object);
			}
		}
		m_hover = hover;
		updateHover(now);
	}
	runActions();
	syncTextFields();
}

void Player::mouseWheel(int delta)
{
	if (!m_root) {
		return;
	}
	broadcast("Mouse", "onMouseWheel", {Value(delta)});
	runActions();
}

bool Player::handCursor()
{
	for (const auto& object : m_hover) {
		if (object->removed || object->kind == Kind::Text) {
			continue;
		}
		const Value hand = m_vm->getMember(Value(object), "useHandCursor");
		if (hand.isUndefined() || avm1::toBoolean(hand)) {
			return true;
		}
	}
	return false;
}

void Player::broadcast(const std::string& object, const std::string& message, const std::vector<Value>& args)
{
	const ObjectRef target = m_vm->getMember(Value(m_vm->global()), object).object();
	if (!target) {
		return;
	}
	std::vector<Value> all {Value(message)};
	all.insert(all.end(), args.begin(), args.end());
	m_vm->callMethod(target, "broadcastMessage", all);
}

// --- Keys

void Player::buttonKeys(DisplayObject& from, int buttonKey)
{
	if (from.kind == Kind::Button && from.parent) {
		auto target = std::static_pointer_cast<DisplayObject>(from.parent->shared_from_this());
		for (const swf::ButtonCondAction& action : static_cast<ButtonObject&>(from).button.actions) {
			if (action.keyCode == buttonKey) {
				runBlock(target, action.actionOffset, action.actionLength);
			}
		}
	} else if (from.kind == Kind::Clip) {
		std::vector<std::shared_ptr<DisplayObject>> children;
		for (const auto& [depth, child] : static_cast<Clip&>(from).children) {
			children.push_back(child);
		}
		for (const auto& child : children) {
			buttonKeys(*child, buttonKey);
		}
	}
}

void Player::typeIntoFocus(int code, uint32_t charCode)
{
	auto field = m_focus.lock();
	if (!field || !field->input) {
		return;
	}
	std::string text = field->plainText();
	size_t caret = std::min(field->caret, text.size());
	bool changed = false;
	if (code == 8) {
		if (caret > 0) {
			const size_t from = previousChar(text, caret);
			text.erase(from, caret - from);
			caret = from;
			changed = true;
		}
	} else if (code == 46) {
		if (caret < text.size()) {
			text.erase(caret, nextChar(text, caret) - caret);
			changed = true;
		}
	} else if (code == 37) {
		caret = previousChar(text, caret);
	} else if (code == 39) {
		caret = nextChar(text, caret);
	} else if (code == 36) {
		caret = 0;
	} else if (code == 35) {
		caret = text.size();
	} else if (charCode == 13 || charCode == 10) {
		if (field->field.multiline) {
			text.insert(caret, "\n");
			++caret;
			changed = true;
		}
	} else if (charCode >= 32 && charCode != 127) {
		if (field->maxChars <= 0 || static_cast<int>(avm1::utf16Length(text)) < field->maxChars) {
			std::string typed;
			appendUtf8(typed, charCode);
			text.insert(caret, typed);
			caret += typed.size();
			changed = true;
		}
	}
	if (changed) {
		field->field.html = false;
		field->caret = caret;
		field->setText(*m_vm, text, false);
		field->caret = caret;
		m_vm->callMethod(field, "onChanged", {Value(field)});
	} else {
		field->caret = caret;
	}
}

void Player::keyDown(int code, uint32_t charCode)
{
	m_keysDown.insert(code);
	m_lastKey = code;
	m_lastChar = charCode;
	if (!m_root) {
		return;
	}
	broadcast("Key", "onKeyDown");
	clipEventsToAll(kClipEventKeyDown, nullptr);
	const int key = buttonKeyCode(code, charCode);
	if (key) {
		clipEventsToAll(kClipEventKeyPress, nullptr, key);
		buttonKeys(*m_root, key);
	}
	typeIntoFocus(code, charCode);
	runActions();
	syncTextFields();
}

void Player::keyUp(int code)
{
	m_keysDown.erase(code);
	m_lastKey = code;
	if (!m_root) {
		return;
	}
	broadcast("Key", "onKeyUp");
	clipEventsToAll(kClipEventKeyUp, nullptr);
	runActions();
	syncTextFields();
}

// --- Frames

MenuFrame Player::frame()
{
	MenuFrame frame;
	frame.width = m_stageWidth;
	frame.height = m_stageHeight;
	if (m_root) {
		m_drawer->stencil().reset();
		m_root->draw(*m_drawer, {}, {}, frame.batches);
		m_drawer->stencil().flush(frame.batches);
	}
	return frame;
}

// --- Inspection

Value Player::evaluate(const std::string& path)
{
	if (!m_root) {
		return Value();
	}
	Value v(m_root);
	size_t at = 0;
	try {
		while (at <= path.size()) {
			const size_t dot = std::min(path.find('.', at), path.size());
			const std::string part = path.substr(at, dot - at);
			if (!part.empty() && part != "_root") {
				v = m_vm->getMember(v, part);
			}
			at = dot + 1;
		}
	} catch (...) {
		return Value();
	}
	return v;
}

std::string Player::dumpDisplayList(int maxDepth) const
{
	std::ostringstream out;
	std::function<void(const DisplayObject&, int)> dump = [&](const DisplayObject& o, int level) {
		static const char* const kinds[] = {"shape", "morph", "text", "button", "clip"};
		out << std::string(static_cast<size_t>(level) * 2, ' ') << o.depth << " " << kinds[static_cast<int>(o.kind)] << " #"
			<< o.character << " " << o.name;
		if (o.kind == Kind::Clip) {
			const auto& c = static_cast<const Clip&>(o);
			out << " frame " << c.frame + 1 << "/" << c.frameCount() << (c.playing ? " playing" : "");
			if (c.image) {
				out << " image " << c.url;
			}
			if (!c.drawing.paths.empty()) {
				out << " drawing " << c.drawing.fillStyles.size() << " fills";
			}
			if (c.mask) {
				out << " masked";
			}
			if (c.isMask) {
				out << " mask";
			}
		}
		if (o.kind == Kind::Text) {
			std::string text = static_cast<const TextField&>(o).plainText();
			if (text.size() > 40) {
				text = text.substr(0, 40) + "...";
			}
			const auto& tf = static_cast<const TextField&>(o);
			out << " \"" << text << "\" font " << tf.field.fontId << " size " << tf.field.fontHeight / kTwips;
		}
		out << (o.visible ? "" : " hidden") << " at " << o.matrix.tx / kTwips << "," << o.matrix.ty / kTwips;
		if (o.matrix.a != 1 || o.matrix.d != 1 || o.matrix.b != 0 || o.matrix.c != 0) {
			out << " matrix " << o.matrix.a << "," << o.matrix.b << "," << o.matrix.c << "," << o.matrix.d;
		}
		if (o.colour.mult[3] != 1) {
			out << " alpha " << o.colour.mult[3];
		}
		out << "\n";
		if (o.kind == Kind::Clip && level < maxDepth) {
			for (const auto& [depth, child] : static_cast<const Clip&>(o).children) {
				dump(*child, level + 1);
			}
		}
	};
	if (m_root) {
		dump(*m_root, 0);
	}
	return out.str();
}

std::vector<Player::Clickable> Player::clickables()
{
	std::vector<Clickable> out;
	if (!m_root) {
		return out;
	}
	std::function<void(Clip&)> walk = [&](Clip& clip) {
		for (auto it = clip.children.rbegin(); it != clip.children.rend(); ++it) {
			DisplayObject& child = *it->second;
			if (!child.visible || child.isMask || child.clipDepth != 0) {
				continue;
			}
			if (child.kind != Kind::Shape && child.kind != Kind::MorphShape && isInteractive(child)) {
				swf::Rect r;
				if (child.boundsIn(child.worldMatrix(), r) && r.xMax > r.xMin && r.yMax > r.yMin) {
					out.push_back({child.targetPath(), r.xMin / kTwips, r.yMin / kTwips, (r.xMax - r.xMin) / kTwips,
						(r.yMax - r.yMin) / kTwips});
				}
			} else if (child.kind == Kind::Clip) {
				walk(static_cast<Clip&>(child));
			}
		}
	};
	walk(*m_root);
	return out;
}

// --- avm1::Host

void Player::trace(const std::string& message)
{
	if (m_traces.size() < 10000) {
		m_traces.push_back(message);
	}
	if (m_traceHandler) {
		m_traceHandler(message);
	}
}

void Player::scriptError(const std::string& message)
{
	if (m_errors.size() < 10000) {
		m_errors.push_back(message);
	}
	if (m_traceHandler) {
		m_traceHandler("script error: " + message);
	}
}

ObjectRef Player::root(const ObjectRef& from)
{
	if (auto* object = dynamic_cast<DisplayObject*>(from.get())) {
		if (Clip* top = object->root()) {
			return top->shared_from_this();
		}
	}
	return m_root;
}

ObjectRef Player::level(int number) { return number == 0 ? m_root : nullptr; }

std::string Player::targetPath(const ObjectRef& clip)
{
	auto* object = dynamic_cast<DisplayObject*>(clip.get());
	return object ? object->targetPath() : std::string();
}

Value Player::getProperty(const ObjectRef& clip, int index)
{
	if (auto* object = dynamic_cast<DisplayObject*>(clip.get())) {
		return object->property(index);
	}
	return m_root ? m_root->property(index) : Value();
}

void Player::setProperty(const ObjectRef& clip, int index, const Value& value)
{
	if (auto* object = dynamic_cast<DisplayObject*>(clip.get())) {
		object->setProperty(*m_vm, index, value);
	}
}

void Player::gotoFrame(const ObjectRef& clip, int frame, bool play)
{
	if (auto* c = dynamic_cast<Clip*>(clip.get())) {
		gotoFrame(*c, frame, play);
	}
}

void Player::gotoLabel(const ObjectRef& clip, const std::string& label, bool play)
{
	auto* c = dynamic_cast<Clip*>(clip.get());
	if (!c) {
		return;
	}
	auto it = c->timeline->labels.find(label);
	if (it == c->timeline->labels.end()) {
		const std::string wanted = lower(label);
		for (it = c->timeline->labels.begin(); it != c->timeline->labels.end(); ++it) {
			if (lower(it->first) == wanted) {
				break;
			}
		}
	}
	if (it != c->timeline->labels.end()) {
		gotoFrame(*c, it->second, play);
	} else {
		c->playing = play; // Flash: an unknown label only changes play/stop
	}
}

void Player::play(const ObjectRef& clip)
{
	if (auto* c = dynamic_cast<Clip*>(clip.get())) {
		c->playing = true;
	}
}

void Player::stop(const ObjectRef& clip)
{
	if (auto* c = dynamic_cast<Clip*>(clip.get())) {
		c->playing = false;
	}
}

void Player::nextFrame(const ObjectRef& clip)
{
	if (auto* c = dynamic_cast<Clip*>(clip.get())) {
		gotoFrame(*c, c->frame + 1, false);
	}
}

void Player::prevFrame(const ObjectRef& clip)
{
	if (auto* c = dynamic_cast<Clip*>(clip.get())) {
		gotoFrame(*c, std::max(0, c->frame - 1), false);
	}
}

void Player::callFrame(const ObjectRef& clip, const std::string& frame)
{
	auto* c = dynamic_cast<Clip*>(clip.get());
	if (!c) {
		return;
	}
	int index = -1;
	const double n = avm1::stringToNumber(frame);
	if (!std::isnan(n)) {
		index = static_cast<int>(n) - 1;
	} else if (auto it = c->timeline->labels.find(frame); it != c->timeline->labels.end()) {
		index = it->second;
	}
	if (index < 0 || index >= c->frameCount()) {
		return;
	}
	auto self = std::static_pointer_cast<DisplayObject>(c->shared_from_this());
	for (const ActionBlock& block : c->timeline->frames[static_cast<size_t>(index)].actions) {
		runBlock(self, block.offset, block.length);
	}
}

void Player::getUrl(const ObjectRef& clip, const std::string& url, const std::string& target, int method, bool loadTarget,
	bool loadVariables)
{
	(void)method;
	if (loadVariables) {
		trace("loadVariables is not supported: " + url);
		return;
	}
	if (loadTarget || target.rfind("_level", 0) == 0) {
		ObjectRef into = target.rfind("_level", 0) == 0 ? level(static_cast<int>(avm1::stringToNumber(target.substr(6))))
														: m_vm->resolveTarget(clip ? clip : m_root, target);
		auto* c = dynamic_cast<Clip*>(into.get());
		if (!c) {
			return;
		}
		if (url.empty()) {
			c->image.reset(); // unloadMovie
			c->url.clear();
		} else if (c != m_root.get()) {
			loadImage(*c, url);
		}
		return;
	}
	if (m_urlHandler) {
		m_urlHandler(url, target);
	}
}

void Player::startDrag(const ObjectRef& clip, bool lockCenter, const double* bounds)
{
	if (auto* object = dynamic_cast<DisplayObject*>(clip.get())) {
		startDrag(*object, lockCenter, bounds);
	}
}

void Player::cloneSprite(const ObjectRef& source, const std::string& name, int depth)
{
	if (auto* object = dynamic_cast<DisplayObject*>(source.get())) {
		duplicate(*object, name, depth + kDepthOffset, nullptr);
	}
}

void Player::removeSprite(const ObjectRef& clip)
{
	if (auto* object = dynamic_cast<DisplayObject*>(clip.get()); object && object->depth >= kDepthOffset) {
		remove(*object);
	}
}

bool Player::registerClass(const std::string& linkage, const ObjectRef& constructor)
{
	if (constructor) {
		m_classes[linkage] = constructor;
	} else {
		m_classes.erase(linkage);
	}
	return true;
}

// --- Built-in native behaviour

void Player::installDefaults()
{
	NativeRegistry& n = m_natives;
	const auto arg = [](const std::vector<Value>& args, size_t i) { return i < args.size() ? args[i] : Value(); };
	// Locale (0x1003ae60): strings through the game's localizer.
	n.setDefaultFunction("dice.bf2.Locale", "getString", [this, arg](Interpreter& vm, const std::vector<Value>& args) {
		return Value(m_natives.localize(vm.toString(arg(args, 0))));
	});
	n.setDefaultFunction("dice.bf2.Locale", "getStringUppercase", [this, arg](Interpreter& vm, const std::vector<Value>& args) {
		std::string s = m_natives.localize(vm.toString(arg(args, 0)));
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		return Value(s);
	});
	n.setDefaultFunction("dice.bf2.Locale", "getLanguage", [](Interpreter&, const std::vector<Value>&) { return Value("English"); });
	n.setDefaultFunction("dice.bf2.Locale", "convertToWide", [arg](Interpreter& vm, const std::vector<Value>& args) {
		return Value(vm.toString(arg(args, 0)));
	});
	// 0x1003a9d0: the width of a text field's text (its first line's text
	// object, measured at the text's length).
	n.setDefaultFunction("dice.bf2.Locale", "getTextboxWidth", [arg](Interpreter&, const std::vector<Value>& args) {
		auto* field = dynamic_cast<TextField*>(arg(args, 0).object().get());
		return Value(field ? static_cast<double>(field->firstLineWidth()) : 0.0);
	});
	// Nothing to report: 0 is "started normally" (the menu shows a message
	// for the other reasons, 1 kicked, 2 banned, ...).
	n.setDefaultFunction("dice.bf2.Logic", "getActivateReason", [](Interpreter&, const std::vector<Value>&) { return Value(0); });
	n.setDefaultFunction("dice.MessageHandler", "getNumMessages", [](Interpreter&, const std::vector<Value>&) { return Value(0); });
	// 0x1003b8e0: Logic.active() is 1 in the game (0 would be the authoring tool).
	n.setDefaultFunction("dice.bf2.Logic", "active", [](Interpreter&, const std::vector<Value>&) { return Value(1); });
	n.setDefaultFunction("dice.bf2.Logic", "getStorageString", [this, arg](Interpreter& vm, const std::vector<Value>& args) {
		const auto it = m_storage.find(vm.toString(arg(args, 0)));
		return Value(it == m_storage.end() ? std::string() : it->second);
	});
	n.setDefaultFunction("dice.bf2.Logic", "setStorageString", [this, arg](Interpreter& vm, const std::vector<Value>& args) {
		m_storage[vm.toString(arg(args, 0))] = vm.toString(arg(args, 1));
		return Value();
	});
	n.setDefaultFunction("dice.bf2.Logic", "eraseStorageString", [this, arg](Interpreter& vm, const std::vector<Value>& args) {
		m_storage.erase(vm.toString(arg(args, 0)));
		return Value();
	});
	n.setDefaultFunction("dice.bf2.Mouse", "insideMovieClip", [this, arg](Interpreter&, const std::vector<Value>& args) {
		auto* object = dynamic_cast<DisplayObject*>(arg(args, 0).object().get());
		return Value(object && object->hitTest({m_mouseX * kTwips, m_mouseY * kTwips}, false, false));
	});
}

} // namespace dice::hfe::ui
