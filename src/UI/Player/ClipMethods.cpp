// The player's side of Flash 7's display classes: MovieClip, TextField and
// Button prototypes, and Key, Mouse, Selection, Stage, Color and
// MovieClipLoader, replacing the interpreter's stand-ins
// (Avm1/BuiltinsStubs.cpp) with ones that act on the display list.
#include "Player.h"
#include "Stage.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::ui {

using avm1::CallArgs;
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

ObjectRef member(Interpreter& vm, const ObjectRef& object, const std::string& name)
{
	return vm.getMember(Value(object), name).object();
}

ObjectRef prototypeOf(Interpreter& vm, const std::string& cls) { return member(vm, member(vm, vm.global(), cls), "prototype"); }

DisplayObject* asObject(CallArgs& c) { return dynamic_cast<DisplayObject*>(c.thisObject.get()); }
Clip* asClip(CallArgs& c) { return dynamic_cast<Clip*>(c.thisObject.get()); }
TextField* asField(CallArgs& c) { return dynamic_cast<TextField*>(c.thisObject.get()); }

double num(Interpreter& vm, const Value& v, double fallback = 0)
{
	if (v.isUndefined()) {
		return fallback;
	}
	const double n = vm.toNumber(v);
	return std::isnan(n) ? fallback : n;
}

// A clip from an object or a target path (relative to `from`).
DisplayObject* targetOf(Interpreter& vm, const Value& v, const ObjectRef& from)
{
	if (v.isObject()) {
		return dynamic_cast<DisplayObject*>(v.asObject().get());
	}
	if (v.isNullish()) {
		return nullptr;
	}
	if (v.isNumber()) {
		return dynamic_cast<DisplayObject*>(vm.host().level(static_cast<int>(v.asNumber())).get());
	}
	return dynamic_cast<DisplayObject*>(vm.resolveTarget(from, vm.toString(v)).get());
}

swf::Rgba rgba(uint32_t rgb, double alphaPercent)
{
	return {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb),
		static_cast<uint8_t>(std::clamp(alphaPercent, 0.0, 100.0) * 2.55 + 0.5)};
}

// Frame arguments: 1-based numbers or labels ("label" or "path:label").
void gotoArg(Player& player, Interpreter& vm, Clip& clip, const Value& frame, bool play)
{
	if (frame.isNumber()) {
		player.gotoFrame(clip, static_cast<int>(frame.asNumber()) - 1, play);
		return;
	}
	const std::string s = vm.toString(frame);
	const double n = avm1::stringToNumber(s);
	if (!std::isnan(n)) {
		player.gotoFrame(clip, static_cast<int>(n) - 1, play);
		return;
	}
	auto it = clip.timeline->labels.find(s);
	if (it != clip.timeline->labels.end()) {
		player.gotoFrame(clip, it->second, play);
	} else {
		clip.playing = play;
	}
}

// --- The drawing API

void newPath(Clip& clip)
{
	swf::ShapePath path;
	path.fill1 = clip.drawingFill;
	path.line = clip.drawingLine;
	path.start = clip.pen;
	clip.drawing.paths.push_back(path);
}

void growBounds(Clip& clip, swf::Point p)
{
	swf::Rect& b = clip.drawing.bounds;
	const bool first = clip.drawing.paths.size() == 1 && clip.drawing.paths[0].edges.size() <= 1 && b.xMin == 0 && b.xMax == 0 &&
		b.yMin == 0 && b.yMax == 0;
	const swf::Rect point {static_cast<int32_t>(std::floor(p.x)), static_cast<int32_t>(std::ceil(p.x)),
		static_cast<int32_t>(std::floor(p.y)), static_cast<int32_t>(std::ceil(p.y))};
	bool any = !first;
	player::unite(b, any, point);
}

void addEdge(Clip& clip, swf::Point control, swf::Point to, bool curved)
{
	if (clip.drawing.paths.empty() || clip.drawing.paths.back().fill1 != clip.drawingFill ||
		clip.drawing.paths.back().line != clip.drawingLine) {
		newPath(clip);
	}
	swf::ShapePath& path = clip.drawing.paths.back();
	if (path.edges.empty()) {
		growBounds(clip, path.start);
	}
	const float half = clip.drawingLine && clip.drawingLine <= clip.drawing.lineStyles.size()
		? clip.drawing.lineStyles[clip.drawingLine - 1].width / 2.0f
		: 0.0f;
	path.edges.push_back({curved ? control : swf::Point {(clip.pen.x + to.x) / 2, (clip.pen.y + to.y) / 2}, to, curved});
	growBounds(clip, {to.x - half, to.y - half});
	growBounds(clip, {to.x + half, to.y + half});
	clip.pen = to;
	clip.drawingDirty = true;
}

void closeFill(Clip& clip)
{
	if (!clip.drawingFill || clip.drawing.paths.empty()) {
		return;
	}
	const swf::ShapePath& path = clip.drawing.paths.back();
	if (!path.edges.empty() && (path.start.x != clip.pen.x || path.start.y != clip.pen.y)) {
		addEdge(clip, {}, path.start, false);
	}
}

} // namespace

void Player::installBuiltins()
{
	Interpreter& vm = *m_vm;
	Player& self = *this;

	// ===== MovieClip
	const ObjectRef mc = prototypeOf(vm, "MovieClip");
	mc->define("enabled", Value(true), avm1::kDontEnum);
	mc->define("useHandCursor", Value(true), avm1::kDontEnum);
	const auto method = [&vm](const ObjectRef& proto, const char* name, avm1::NativeCallback callback) {
		vm.defineMethod(proto, name, std::move(callback));
	};

	method(mc, "play", [](Interpreter&, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			clip->playing = true;
		}
		return Value();
	});
	method(mc, "stop", [](Interpreter&, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			clip->playing = false;
		}
		return Value();
	});
	method(mc, "gotoAndPlay", [&self](Interpreter& vm, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			gotoArg(self, vm, *clip, c[0], true);
		}
		return Value();
	});
	method(mc, "gotoAndStop", [&self](Interpreter& vm, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			gotoArg(self, vm, *clip, c[0], false);
		}
		return Value();
	});
	method(mc, "nextFrame", [&self](Interpreter&, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			self.gotoFrame(*clip, clip->frame + 1, false);
		}
		return Value();
	});
	method(mc, "prevFrame", [&self](Interpreter&, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			self.gotoFrame(*clip, std::max(0, clip->frame - 1), false);
		}
		return Value();
	});
	method(mc, "attachMovie", [&self](Interpreter& vm, CallArgs& c) -> Value {
		Clip* clip = asClip(c);
		if (!clip || c.size() < 3) {
			return Value();
		}
		auto object = self.attachMovie(*clip, vm.toString(c[0]), vm.toString(c[1]), static_cast<int>(num(vm, c[2])) + kDepthOffset,
			c[3].object());
		return object ? Value(object) : Value();
	});
	method(mc, "createEmptyMovieClip", [&self](Interpreter& vm, CallArgs& c) -> Value {
		Clip* clip = asClip(c);
		if (!clip || c.size() < 2) {
			return Value();
		}
		return Value(self.createEmptyClip(*clip, vm.toString(c[0]), static_cast<int>(num(vm, c[1])) + kDepthOffset));
	});
	method(mc, "createTextField", [&self](Interpreter& vm, CallArgs& c) -> Value {
		Clip* clip = asClip(c);
		if (!clip || c.size() < 6) {
			return Value();
		}
		const auto t = [&](size_t i) { return static_cast<int32_t>(num(vm, c[i]) * kTwips); };
		const swf::Rect bounds {t(2), t(2) + t(4), t(3), t(3) + t(5)};
		swf::Rect local {0, bounds.xMax - bounds.xMin, 0, bounds.yMax - bounds.yMin};
		auto field = self.createTextField(*clip, vm.toString(c[0]), static_cast<int>(num(vm, c[1])) + kDepthOffset, local);
		field->matrix.tx = static_cast<float>(bounds.xMin);
		field->matrix.ty = static_cast<float>(bounds.yMin);
		return Value(field);
	});
	method(mc, "duplicateMovieClip", [&self](Interpreter& vm, CallArgs& c) -> Value {
		DisplayObject* object = asObject(c);
		if (!object || c.size() < 2) {
			return Value();
		}
		auto copy = self.duplicate(*object, vm.toString(c[0]), static_cast<int>(num(vm, c[1])) + kDepthOffset, c[2].object());
		return copy ? Value(copy) : Value();
	});
	method(mc, "removeMovieClip", [&self](Interpreter&, CallArgs& c) -> Value {
		if (DisplayObject* object = asObject(c); object && object->depth >= kDepthOffset) {
			self.remove(*object);
		}
		return Value();
	});
	method(mc, "swapDepths", [&self](Interpreter& vm, CallArgs& c) -> Value {
		DisplayObject* object = asObject(c);
		if (!object || !object->parent) {
			return Value();
		}
		if (auto* other = dynamic_cast<DisplayObject*>(c[0].object().get())) {
			if (other->parent == object->parent) {
				self.swapDepths(*object, other->depth);
			}
		} else if (!c[0].isUndefined()) {
			self.swapDepths(*object, static_cast<int>(num(vm, c[0])) + kDepthOffset);
		}
		return Value();
	});
	method(mc, "getDepth", [](Interpreter&, CallArgs& c) -> Value {
		DisplayObject* object = asObject(c);
		return object ? Value(object->depth - kDepthOffset) : Value();
	});
	method(mc, "getNextHighestDepth", [](Interpreter&, CallArgs& c) -> Value {
		Clip* clip = asClip(c);
		if (!clip) {
			return Value();
		}
		return Value(std::max(0, clip->highestDepth() - kDepthOffset + 1));
	});
	method(mc, "getInstanceAtDepth", [](Interpreter& vm, CallArgs& c) -> Value {
		Clip* clip = asClip(c);
		if (!clip) {
			return Value();
		}
		auto child = clip->childAt(static_cast<int>(num(vm, c[0])) + kDepthOffset);
		return child && child->kind != Kind::Shape && child->kind != Kind::MorphShape ? Value(child) : Value();
	});
	const auto getBounds = [](Interpreter& vm, CallArgs& c) -> Value {
		DisplayObject* object = asObject(c);
		if (!object) {
			return Value();
		}
		swf::Matrix space = object->worldMatrix();
		if (auto* target = dynamic_cast<DisplayObject*>(c[0].object().get())) {
			space = multiply(inverse(target->worldMatrix()), space);
		} else if (c.size() == 0) {
			space = {};
		}
		swf::Rect r;
		const ObjectRef out = vm.newObject();
		if (!object->boundsIn(space, r)) {
			r = {134217727, 134217727, 134217727, 134217727}; // Flash's "empty"
			out->define("xMin", Value(6710886.35));
			out->define("xMax", Value(6710886.35));
			out->define("yMin", Value(6710886.35));
			out->define("yMax", Value(6710886.35));
			return Value(out);
		}
		out->define("xMin", Value(r.xMin / kTwips));
		out->define("xMax", Value(r.xMax / kTwips));
		out->define("yMin", Value(r.yMin / kTwips));
		out->define("yMax", Value(r.yMax / kTwips));
		return Value(out);
	};
	method(mc, "getBounds", getBounds);
	method(mc, "getRect", getBounds);
	method(mc, "hitTest", [](Interpreter& vm, CallArgs& c) -> Value {
		DisplayObject* object = asObject(c);
		if (!object) {
			return Value(false);
		}
		if (c.size() >= 2) {
			const swf::Point p {static_cast<float>(num(vm, c[0]) * kTwips), static_cast<float>(num(vm, c[1]) * kTwips)};
			return Value(object->hitTest(p, c.size() > 2 && avm1::toBoolean(c[2]), true));
		}
		DisplayObject* other = targetOf(vm, c[0], c.thisObject);
		if (!other) {
			return Value(false);
		}
		swf::Rect a, b;
		if (!object->boundsIn(object->worldMatrix(), a) || !other->boundsIn(other->worldMatrix(), b)) {
			return Value(false);
		}
		return Value(a.xMin <= b.xMax && b.xMin <= a.xMax && a.yMin <= b.yMax && b.yMin <= a.yMax);
	});
	const auto convertPoint = [](bool toGlobal) {
		return [toGlobal](Interpreter& vm, CallArgs& c) -> Value {
			DisplayObject* object = asObject(c);
			const ObjectRef point = c[0].object();
			if (!object || !point) {
				return Value();
			}
			const swf::Matrix world = object->worldMatrix();
			const swf::Matrix m = toGlobal ? world : inverse(world);
			const swf::Point p = m.apply({static_cast<float>(num(vm, vm.getMember(Value(point), "x")) * kTwips),
				static_cast<float>(num(vm, vm.getMember(Value(point), "y")) * kTwips)});
			point->setMember(vm, "x", Value(p.x / kTwips));
			point->setMember(vm, "y", Value(p.y / kTwips));
			return Value();
		};
	};
	method(mc, "localToGlobal", convertPoint(true));
	method(mc, "globalToLocal", convertPoint(false));
	// The clip is cut to the mask's bounding box (Clip::draw); the mask is not drawn.
	method(mc, "setMask", [](Interpreter&, CallArgs& c) -> Value {
		Clip* clip = asClip(c);
		if (!clip) {
			return Value();
		}
		if (auto* old = dynamic_cast<DisplayObject*>(clip->mask.get())) {
			old->isMask = false;
		}
		clip->mask = c[0].object();
		if (auto* mask = dynamic_cast<DisplayObject*>(clip->mask.get())) {
			mask->isMask = true;
		}
		return Value();
	});
	method(mc, "loadMovie", [&self](Interpreter& vm, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			self.loadImage(*clip, vm.toString(c[0]));
		}
		return Value();
	});
	method(mc, "unloadMovie", [&self](Interpreter&, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			std::vector<std::shared_ptr<DisplayObject>> children;
			for (const auto& [depth, child] : clip->children) {
				children.push_back(child);
			}
			for (const auto& child : children) {
				self.remove(*child);
			}
			clip->image.reset();
			clip->video.reset();
			clip->url.clear();
			clip->bytes = 0;
		}
		return Value();
	});
	method(mc, "loadVariables", [](Interpreter&, CallArgs&) -> Value { return Value(); });
	method(mc, "getURL", [this](Interpreter& vm, CallArgs& c) -> Value {
		if (m_urlHandler) {
			m_urlHandler(vm.toString(c[0]), c.size() > 1 ? vm.toString(c[1]) : std::string());
		}
		return Value();
	});
	const auto bytes = [&self](Interpreter&, CallArgs& c) -> Value {
		Clip* clip = asClip(c);
		if (!clip) {
			return Value();
		}
		return Value(static_cast<double>(clip->image ? clip->bytes : self.swf().size()));
	};
	method(mc, "getBytesLoaded", bytes);
	method(mc, "getBytesTotal", bytes);
	method(mc, "getSWFVersion", [](Interpreter&, CallArgs&) -> Value { return Value(7); });
	method(mc, "getTextSnapshot", [](Interpreter&, CallArgs&) -> Value { return Value::null(); });
	method(mc, "startDrag", [&self](Interpreter& vm, CallArgs& c) -> Value {
		if (DisplayObject* object = asObject(c)) {
			double bounds[4];
			const bool constrained = c.size() >= 5;
			for (int i = 0; i < 4 && constrained; ++i) {
				bounds[i] = num(vm, c[static_cast<size_t>(i) + 1]);
			}
			self.startDrag(*object, avm1::toBoolean(c[0]), constrained ? bounds : nullptr);
		}
		return Value();
	});
	method(mc, "stopDrag", [&self](Interpreter&, CallArgs&) -> Value {
		self.stopDrag();
		return Value();
	});
	for (const char* name : {"attachAudio", "attachVideo", "setFocus"}) {
		method(mc, name, [](Interpreter&, CallArgs&) -> Value { return Value(); });
	}

	// Drawing API: paths into the clip's own shape, drawn under its children.
	method(mc, "clear", [](Interpreter&, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			clip->drawing = swf::Shape();
			clip->drawingMesh = swf::ShapeMesh();
			clip->drawingFill = clip->drawingLine = 0;
			clip->pen = {};
			clip->drawingDirty = false;
		}
		return Value();
	});
	method(mc, "lineStyle", [](Interpreter& vm, CallArgs& c) -> Value {
		Clip* clip = asClip(c);
		if (!clip) {
			return Value();
		}
		if (c[0].isUndefined()) {
			clip->drawingLine = 0;
		} else {
			swf::LineStyle style;
			style.width = static_cast<uint16_t>(std::clamp(num(vm, c[0]), 0.0, 255.0) * kTwips);
			style.color = rgba(avm1::toUint32(num(vm, c[1])), num(vm, c[2], 100));
			clip->drawing.lineStyles.push_back(style);
			clip->drawingLine = static_cast<uint32_t>(clip->drawing.lineStyles.size());
		}
		newPath(*clip);
		return Value();
	});
	const auto beginFill = [](Interpreter& vm, CallArgs& c, uint32_t rgb, double alpha) {
		Clip* clip = asClip(c);
		if (!clip) {
			return;
		}
		closeFill(*clip);
		swf::FillStyle style;
		style.color = rgba(rgb, alpha);
		clip->drawing.fillStyles.push_back(style);
		clip->drawingFill = static_cast<uint32_t>(clip->drawing.fillStyles.size());
		newPath(*clip);
		(void)vm;
	};
	method(mc, "beginFill", [beginFill](Interpreter& vm, CallArgs& c) -> Value {
		if (c[0].isUndefined()) {
			if (Clip* clip = asClip(c)) {
				closeFill(*clip);
				clip->drawingFill = 0;
			}
			return Value();
		}
		beginFill(vm, c, avm1::toUint32(num(vm, c[0])), num(vm, c[1], 100));
		return Value();
	});
	// Gradients draw as their first colour.
	method(mc, "beginGradientFill", [beginFill](Interpreter& vm, CallArgs& c) -> Value {
		const Value colours = vm.getMember(c[1], "0");
		const Value alphas = vm.getMember(c[2], "0");
		beginFill(vm, c, avm1::toUint32(num(vm, colours)), num(vm, alphas, 100));
		return Value();
	});
	method(mc, "endFill", [](Interpreter&, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			closeFill(*clip);
			clip->drawingFill = 0;
			newPath(*clip);
		}
		return Value();
	});
	method(mc, "moveTo", [](Interpreter& vm, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			clip->pen = {static_cast<float>(num(vm, c[0]) * kTwips), static_cast<float>(num(vm, c[1]) * kTwips)};
			newPath(*clip);
		}
		return Value();
	});
	method(mc, "lineTo", [](Interpreter& vm, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			addEdge(*clip, {}, {static_cast<float>(num(vm, c[0]) * kTwips), static_cast<float>(num(vm, c[1]) * kTwips)}, false);
		}
		return Value();
	});
	method(mc, "curveTo", [](Interpreter& vm, CallArgs& c) -> Value {
		if (Clip* clip = asClip(c)) {
			addEdge(*clip, {static_cast<float>(num(vm, c[0]) * kTwips), static_cast<float>(num(vm, c[1]) * kTwips)},
				{static_cast<float>(num(vm, c[2]) * kTwips), static_cast<float>(num(vm, c[3]) * kTwips)}, true);
		}
		return Value();
	});

	// ===== Button: the depth methods and the default flags
	const ObjectRef button = prototypeOf(vm, "Button");
	button->define("enabled", Value(true), avm1::kDontEnum);
	button->define("useHandCursor", Value(true), avm1::kDontEnum);
	for (const char* name : {"getDepth", "swapDepths", "getBounds", "hitTest", "localToGlobal", "globalToLocal"}) {
		button->define(name, vm.getMember(Value(mc), name), avm1::kDontEnum);
	}

	// ===== TextField
	const ObjectRef tf = prototypeOf(vm, "TextField");
	for (const char* name : {"getDepth", "swapDepths", "getBounds", "hitTest", "localToGlobal", "globalToLocal"}) {
		tf->define(name, vm.getMember(Value(mc), name), avm1::kDontEnum);
	}
	method(tf, "removeTextField", [&self](Interpreter&, CallArgs& c) -> Value {
		if (TextField* field = asField(c); field && field->depth >= kDepthOffset) {
			self.remove(*field);
		}
		return Value();
	});
	// TextFormat in: color, size, align, font, leftMargin, rightMargin, indent, leading.
	const auto applyFormat = [](Interpreter& vm, TextField& field, const ObjectRef& format) {
		if (!format) {
			return;
		}
		const auto get = [&](const char* name) { return vm.getMember(Value(format), name); };
		const Value color = get("color");
		if (!color.isNullish()) {
			const uint32_t rgb = avm1::toUint32(vm.toNumber(color));
			field.field.textColor = {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb),
				field.field.textColor.a};
		}
		const Value size = get("size");
		if (!size.isNullish()) {
			field.field.fontHeight = static_cast<uint16_t>(std::clamp(vm.toNumber(size), 1.0, 1000.0) * kTwips);
		}
		const Value align = get("align");
		if (align.isString()) {
			const std::string a = align.asString();
			field.field.align = a == "right" ? swf::EditText::Align::Right
				: a == "center"                ? swf::EditText::Align::Center
				: a == "justify"               ? swf::EditText::Align::Justify
											   : swf::EditText::Align::Left;
		}
		const Value font = get("font");
		if (font.isString()) {
			for (const auto& [id, character] : field.player.dictionary().characters()) {
				if (const auto* f = std::get_if<swf::Font>(&character); f && f->name == font.asString()) {
					field.field.fontId = id;
					break;
				}
			}
		}
		const auto twips = [&](const char* name, auto& out) {
			const Value v = get(name);
			if (!v.isNullish()) {
				out = static_cast<std::remove_reference_t<decltype(out)>>(vm.toNumber(v) * kTwips);
			}
		};
		twips("leftMargin", field.field.leftMargin);
		twips("rightMargin", field.field.rightMargin);
		twips("indent", field.field.indent);
		twips("leading", field.field.leading);
		field.applyAutoSize();
	};
	const auto setFormat = [applyFormat](Interpreter& vm, CallArgs& c) -> Value {
		if (TextField* field = asField(c)) {
			// setTextFormat([begin, [end,]] format): the whole field here.
			ObjectRef format;
			for (const Value& arg : c.args) {
				if (arg.isObject()) {
					format = arg.asObject();
				}
			}
			applyFormat(vm, *field, format);
		}
		return Value();
	};
	method(tf, "setTextFormat", setFormat);
	method(tf, "setNewTextFormat", setFormat);
	const auto getFormat = [](Interpreter& vm, CallArgs& c) -> Value {
		TextField* field = asField(c);
		const Value cls = vm.getMember(Value(vm.global()), "TextFormat");
		const Value format = vm.constructWith(cls, {});
		if (!field || !format.isObject()) {
			return format;
		}
		const ObjectRef f = format.asObject();
		const swf::Rgba col = field->field.textColor;
		f->setMember(vm, "color", Value(static_cast<double>(col.r << 16 | col.g << 8 | col.b)));
		f->setMember(vm, "size", Value(field->field.fontHeight / kTwips));
		static const char* const aligns[] = {"left", "right", "center", "justify"};
		f->setMember(vm, "align", Value(aligns[static_cast<int>(field->field.align) & 3]));
		if (const swf::Font* font = field->player.dictionary().font(field->field.fontId)) {
			f->setMember(vm, "font", Value(font->name));
			f->setMember(vm, "bold", Value(font->bold));
			f->setMember(vm, "italic", Value(font->italic));
		}
		f->setMember(vm, "leftMargin", Value(field->field.leftMargin / kTwips));
		f->setMember(vm, "rightMargin", Value(field->field.rightMargin / kTwips));
		f->setMember(vm, "indent", Value(field->field.indent / kTwips));
		f->setMember(vm, "leading", Value(field->field.leading / kTwips));
		return format;
	};
	method(tf, "getTextFormat", getFormat);
	method(tf, "getNewTextFormat", getFormat);
	method(tf, "replaceSel", [](Interpreter& vm, CallArgs& c) -> Value {
		if (TextField* field = asField(c)) {
			std::string text = field->plainText();
			const size_t at = std::min(field->caret, text.size());
			const std::string add = vm.toString(c[0]);
			text.insert(at, add);
			field->setText(vm, text, false);
			field->caret = at + add.size();
		}
		return Value();
	});
	method(tf, "replaceText", [](Interpreter& vm, CallArgs& c) -> Value {
		if (TextField* field = asField(c)) {
			const std::u16string text = avm1::utf8ToUtf16(field->plainText());
			const size_t begin = std::min(text.size(), static_cast<size_t>(std::max(0.0, num(vm, c[0]))));
			const size_t end = std::clamp(static_cast<size_t>(std::max(0.0, num(vm, c[1]))), begin, text.size());
			const std::u16string next = text.substr(0, begin) + avm1::utf8ToUtf16(vm.toString(c[2])) + text.substr(end);
			field->setText(vm, avm1::utf16ToUtf8(next), false);
		}
		return Value();
	});
	vm.defineMethod(member(vm, vm.global(), "TextField"), "getFontList", [](Interpreter& vm, CallArgs&) -> Value {
		return Value(vm.newArray());
	});

	// ===== Key
	const ObjectRef key = member(vm, vm.global(), "Key");
	if (key) {
		method(key, "isDown", [&self](Interpreter& vm, CallArgs& c) -> Value {
			return Value(self.keyIsDown(static_cast<int>(num(vm, c[0]))));
		});
		method(key, "getCode", [&self](Interpreter&, CallArgs&) -> Value { return Value(self.lastKeyCode()); });
		method(key, "getAscii", [&self](Interpreter&, CallArgs&) -> Value { return Value(static_cast<double>(self.lastKeyChar())); });
	}

	// ===== Mouse
	if (const ObjectRef mouse = member(vm, vm.global(), "Mouse")) {
		method(mouse, "show", [&self](Interpreter&, CallArgs&) -> Value {
			self.setCursorVisible(true);
			return Value(0);
		});
		method(mouse, "hide", [&self](Interpreter&, CallArgs&) -> Value {
			self.setCursorVisible(false);
			return Value(1);
		});
	}

	// ===== Stage
	if (const ObjectRef stage = member(vm, vm.global(), "Stage")) {
		stage->define("width", Value(static_cast<double>(m_stageWidth)));
		stage->define("height", Value(static_cast<double>(m_stageHeight)));
		stage->define("scaleMode", Value("noScale"));
	}

	// ===== Selection
	if (const ObjectRef selection = member(vm, vm.global(), "Selection")) {
		method(selection, "getFocus", [&self](Interpreter&, CallArgs&) -> Value {
			TextField* field = self.focus();
			return field ? Value(field->targetPath()) : Value::null();
		});
		method(selection, "setFocus", [&self](Interpreter& vm, CallArgs& c) -> Value {
			DisplayObject* target = targetOf(vm, c[0], self.root());
			auto* field = dynamic_cast<TextField*>(target);
			self.setFocus(field ? std::static_pointer_cast<TextField>(field->shared_from_this()) : nullptr);
			return Value(field != nullptr);
		});
		const auto caret = [&self](Interpreter&, CallArgs&) -> Value {
			TextField* field = self.focus();
			if (!field) {
				return Value(-1);
			}
			return Value(static_cast<double>(avm1::utf16Length(field->plainText().substr(0, field->caret))));
		};
		method(selection, "getBeginIndex", caret);
		method(selection, "getEndIndex", caret);
		method(selection, "getCaretIndex", caret);
		method(selection, "setSelection", [&self](Interpreter& vm, CallArgs& c) -> Value {
			if (TextField* field = self.focus()) {
				const std::u16string text = avm1::utf8ToUtf16(field->plainText());
				const size_t end = std::min(text.size(), static_cast<size_t>(std::max(0.0, num(vm, c[1]))));
				field->caret = avm1::utf16ToUtf8(text.substr(0, end)).size();
			}
			return Value();
		});
	}

	// ===== Color(target): a clip's colour transform
	const ObjectRef colorProto = vm.newObject();
	auto color = vm.newClass(
		[](Interpreter&, CallArgs& c) -> Value {
			if (c.constructing && c.thisObject) {
				c.thisObject->define("__target", c[0], avm1::kDontEnum);
			}
			return Value();
		},
		{}, colorProto);
	const auto colorTarget = [&self](Interpreter& vm, CallArgs& c) -> DisplayObject* {
		if (!c.thisObject) {
			return nullptr;
		}
		const avm1::Property* p = c.thisObject->findOwn("__target");
		return p ? targetOf(vm, p->value, self.root()) : nullptr;
	};
	method(colorProto, "setRGB", [colorTarget](Interpreter& vm, CallArgs& c) -> Value {
		if (DisplayObject* target = colorTarget(vm, c)) {
			const uint32_t rgb = avm1::toUint32(num(vm, c[0]));
			for (int i = 0; i < 3; ++i) {
				target->colour.mult[i] = 0;
			}
			target->colour.add[0] = static_cast<int16_t>(rgb >> 16 & 0xff);
			target->colour.add[1] = static_cast<int16_t>(rgb >> 8 & 0xff);
			target->colour.add[2] = static_cast<int16_t>(rgb & 0xff);
			target->scriptTransformed = true;
		}
		return Value();
	});
	method(colorProto, "getRGB", [colorTarget](Interpreter& vm, CallArgs& c) -> Value {
		DisplayObject* target = colorTarget(vm, c);
		if (!target) {
			return Value(0);
		}
		return Value(static_cast<double>((target->colour.add[0] & 0xff) << 16 | (target->colour.add[1] & 0xff) << 8 |
			(target->colour.add[2] & 0xff)));
	});
	method(colorProto, "setTransform", [colorTarget](Interpreter& vm, CallArgs& c) -> Value {
		DisplayObject* target = colorTarget(vm, c);
		const ObjectRef t = c[0].object();
		if (!target || !t) {
			return Value();
		}
		static const char* const multipliers[] = {"ra", "ga", "ba", "aa"};
		static const char* const offsets[] = {"rb", "gb", "bb", "ab"};
		for (int i = 0; i < 4; ++i) {
			const Value m = vm.getMember(Value(t), multipliers[i]);
			if (!m.isUndefined()) {
				target->colour.mult[i] = static_cast<float>(num(vm, m) / 100.0);
			}
			const Value a = vm.getMember(Value(t), offsets[i]);
			if (!a.isUndefined()) {
				target->colour.add[i] = static_cast<int16_t>(std::clamp(num(vm, a), -255.0, 255.0));
			}
		}
		target->scriptTransformed = true;
		return Value();
	});
	method(colorProto, "getTransform", [colorTarget](Interpreter& vm, CallArgs& c) -> Value {
		DisplayObject* target = colorTarget(vm, c);
		const ObjectRef t = vm.newObject();
		static const char* const multipliers[] = {"ra", "ga", "ba", "aa"};
		static const char* const offsets[] = {"rb", "gb", "bb", "ab"};
		for (int i = 0; i < 4; ++i) {
			t->define(multipliers[i], Value(target ? target->colour.mult[i] * 100.0 : 100.0));
			t->define(offsets[i], Value(target ? static_cast<double>(target->colour.add[i]) : 0.0));
		}
		return Value(t);
	});
	vm.global()->define("Color", Value(color), avm1::kDontEnum);

	// ===== MovieClipLoader: synchronous image loads with the usual events
	if (const ObjectRef loader = prototypeOf(vm, "MovieClipLoader")) {
		method(loader, "loadClip", [&self](Interpreter& vm, CallArgs& c) -> Value {
			auto* clip = dynamic_cast<Clip*>(targetOf(vm, c[1], self.root()));
			const ObjectRef listeners = c.thisObject;
			const auto tell = [&](const char* event, std::vector<Value> args) {
				if (listeners) {
					args.insert(args.begin(), Value(event));
					vm.callMember(Value(listeners), "broadcastMessage", args);
				}
			};
			if (!clip) {
				return Value(false);
			}
			const Value target(clip->shared_from_this());
			tell("onLoadStart", {target});
			if (!self.loadImage(*clip, vm.toString(c[0]))) {
				tell("onLoadError", {target, Value("URLNotFound")});
				return Value(true);
			}
			const Value size(static_cast<double>(clip->bytes));
			tell("onLoadProgress", {target, size, size});
			tell("onLoadComplete", {target});
			tell("onLoadInit", {target});
			return Value(true);
		});
		method(loader, "unloadClip", [&self](Interpreter& vm, CallArgs& c) -> Value {
			if (auto* clip = dynamic_cast<Clip*>(targetOf(vm, c[0], self.root()))) {
				clip->image.reset();
				clip->video.reset();
			clip->video.reset();
				clip->url.clear();
				clip->bytes = 0;
				return Value(true);
			}
			return Value(false);
		});
		method(loader, "getProgress", [&self](Interpreter& vm, CallArgs& c) -> Value {
			const ObjectRef out = vm.newObject();
			auto* clip = dynamic_cast<Clip*>(targetOf(vm, c[0], self.root()));
			const double size = clip ? static_cast<double>(clip->image ? clip->bytes : self.swf().size()) : 0.0;
			out->define("bytesLoaded", Value(size));
			out->define("bytesTotal", Value(size));
			return Value(out);
		});
	}
}

} // namespace dice::hfe::ui
