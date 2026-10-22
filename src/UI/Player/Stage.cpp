#include "Stage.h"

#include "Player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace dice::hfe::ui::player {

using avm1::Interpreter;
using avm1::ObjectRef;
using avm1::Value;

namespace {

constexpr float kTwips = 20.0f;
constexpr double kPi = 3.14159265358979323846;

// "_x" -> kPropX, ...
int propertyIndex(const std::string& name)
{
	static const std::unordered_map<std::string, int> indices = [] {
		std::unordered_map<std::string, int> map;
		for (int i = 0; i < avm1::kClipPropertyCount; ++i) {
			if (const char* n = avm1::clipPropertyName(i)) {
				map[n] = i;
			}
		}
		return map;
	}();
	const auto it = indices.find(name);
	return it == indices.end() ? -1 : it->second;
}

double number(Interpreter& vm, const Value& v) { return vm.toNumber(v); }

uint32_t rgbOf(swf::Rgba c) { return static_cast<uint32_t>(c.r) << 16 | static_cast<uint32_t>(c.g) << 8 | c.b; }

swf::Rgba rgbaOf(uint32_t rgb, uint8_t alpha = 255)
{
	return {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb), alpha};
}

// HTML markup dropped, entities decoded, <br> and </p> as newlines.
std::string stripHtml(const std::string& html)
{
	std::string out;
	for (size_t i = 0; i < html.size(); ++i) {
		if (html[i] == '<') {
			const size_t close = html.find('>', i);
			std::string tag = html.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
			std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (tag.rfind("br", 0) == 0 || tag.rfind("/p", 0) == 0) {
				out += '\n';
			}
			i = close == std::string::npos ? html.size() : close;
		} else if (html[i] == '&') {
			static const std::pair<const char*, char> entities[] = {
				{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&nbsp;", ' '}};
			bool matched = false;
			for (const auto& [entity, c] : entities) {
				const std::string e = entity;
				if (html.compare(i, e.size(), e) == 0) {
					out += c;
					i += e.size() - 1;
					matched = true;
					break;
				}
			}
			if (!matched) {
				out += '&';
			}
		} else {
			out += html[i];
		}
	}
	if (!out.empty() && out.back() == '\n') {
		out.pop_back(); // the last paragraph's end
	}
	return out;
}

bool contains(const swf::Rect& r, swf::Point p) { return p.x >= r.xMin && p.x <= r.xMax && p.y >= r.yMin && p.y <= r.yMax; }

} // namespace

void unite(swf::Rect& into, bool& any, const swf::Rect& r)
{
	if (!any) {
		into = r;
		any = true;
		return;
	}
	into.xMin = std::min(into.xMin, r.xMin);
	into.yMin = std::min(into.yMin, r.yMin);
	into.xMax = std::max(into.xMax, r.xMax);
	into.yMax = std::max(into.yMax, r.yMax);
}

swf::Rect transformRect(const swf::Matrix& m, const swf::Rect& r)
{
	const swf::Point corners[4] = {m.apply({static_cast<float>(r.xMin), static_cast<float>(r.yMin)}),
		m.apply({static_cast<float>(r.xMax), static_cast<float>(r.yMin)}), m.apply({static_cast<float>(r.xMax), static_cast<float>(r.yMax)}),
		m.apply({static_cast<float>(r.xMin), static_cast<float>(r.yMax)})};
	float x0 = corners[0].x, x1 = x0, y0 = corners[0].y, y1 = y0;
	for (const swf::Point& p : corners) {
		x0 = std::min(x0, p.x);
		x1 = std::max(x1, p.x);
		y0 = std::min(y0, p.y);
		y1 = std::max(y1, p.y);
	}
	return {static_cast<int32_t>(std::floor(x0)), static_cast<int32_t>(std::ceil(x1)), static_cast<int32_t>(std::floor(y0)),
		static_cast<int32_t>(std::ceil(y1))};
}

// --- DisplayObject

bool DisplayObject::getMember(Interpreter& vm, const std::string& name, Value& out)
{
	if (findOwn(name)) {
		return avm1::Object::getMember(vm, name, out);
	}
	if (specialMember(vm, name, out)) {
		return true;
	}
	return avm1::Object::getMember(vm, name, out);
}

void DisplayObject::setMember(Interpreter& vm, const std::string& name, const Value& value)
{
	if (!name.empty() && name[0] == '_') {
		const int index = propertyIndex(name);
		if (index >= 0) {
			setProperty(vm, index, value);
			return;
		}
	}
	if (setSpecialMember(vm, name, value)) {
		return;
	}
	avm1::Object::setMember(vm, name, value);
}

bool DisplayObject::specialMember(Interpreter& vm, const std::string& name, Value& out)
{
	if (name.empty() || name[0] != '_') {
		return false;
	}
	const int index = propertyIndex(name);
	if (index >= 0) {
		out = property(index);
		return true;
	}
	if (name == "_parent") {
		if (!parent) {
			return false;
		}
		out = Value(parent->shared_from_this());
		return true;
	}
	if (name == "_root" || name == "_level0") {
		Clip* top = root();
		out = top ? Value(top->shared_from_this()) : Value();
		return top != nullptr;
	}
	if (name == "_global") {
		out = Value(vm.global());
		return true;
	}
	return false;
}

bool DisplayObject::setSpecialMember(Interpreter& vm, const std::string& name, const Value& value)
{
	(void)vm, (void)value;
	return name == "_parent" || name == "_root" || name == "_level0";
}

// Display properties count as own members, so that `with (clip) { _x = 1; }`
// assigns the clip's and not the timeline's.
bool DisplayObject::hasOwnMember(const std::string& name) const
{
	return avm1::Object::hasOwnMember(name) || (!name.empty() && name[0] == '_' && propertyIndex(name) >= 0);
}

bool TextField::hasOwnMember(const std::string& name) const
{
	static const char* const names[] = {"text", "htmlText", "html", "textColor", "variable", "autoSize", "maxChars", "multiline",
		"wordWrap", "selectable", "type", "length", "textWidth", "textHeight", "password", "border", "background",
		"backgroundColor", "borderColor", "embedFonts", "scroll", "maxscroll", "bottomScroll", "hscroll", "maxhscroll",
		"restrict", "condenseWhite"};
	for (const char* n : names) {
		if (name == n) {
			return true;
		}
	}
	return DisplayObject::hasOwnMember(name);
}

bool DisplayObject::defaultValue(Interpreter& vm, bool preferString, Value& out)
{
	(void)vm, (void)preferString;
	out = Value(targetPath());
	return true;
}

void DisplayObject::decompose()
{
	if (decomposed) {
		return;
	}
	xscale = std::sqrt(matrix.a * matrix.a + matrix.b * matrix.b) * 100.0;
	yscale = std::sqrt(matrix.c * matrix.c + matrix.d * matrix.d) * 100.0;
	if (matrix.a * matrix.d - matrix.b * matrix.c < 0) {
		yscale = -yscale;
	}
	rotation = std::atan2(matrix.b, matrix.a) * 180.0 / kPi;
	decomposed = true;
}

void DisplayObject::setScale(double xs, double ys, double rot)
{
	xscale = xs;
	yscale = ys;
	rotation = rot;
	decomposed = true;
	const double r = rot * kPi / 180.0;
	const double cosR = std::cos(r), sinR = std::sin(r);
	matrix.a = static_cast<float>(xs / 100.0 * cosR);
	matrix.b = static_cast<float>(xs / 100.0 * sinR);
	matrix.c = static_cast<float>(-ys / 100.0 * sinR);
	matrix.d = static_cast<float>(ys / 100.0 * cosR);
	scriptTransformed = true;
}

Value DisplayObject::property(int index)
{
	auto* clip = kind == Kind::Clip ? static_cast<Clip*>(this) : nullptr;
	switch (index) {
	case avm1::kPropX: return Value(matrix.tx / kTwips);
	case avm1::kPropY: return Value(matrix.ty / kTwips);
	case avm1::kPropXScale: decompose(); return Value(xscale);
	case avm1::kPropYScale: decompose(); return Value(yscale);
	case avm1::kPropRotation: decompose(); return Value(rotation);
	case avm1::kPropCurrentFrame: return clip ? Value(std::max(clip->frame, 0) + 1) : Value();
	case avm1::kPropTotalFrames:
	case avm1::kPropFramesLoaded: return clip ? Value(clip->frameCount()) : Value();
	case avm1::kPropAlpha: return Value(std::round(colour.mult[3] * 100.0 * 256.0) / 256.0);
	case avm1::kPropVisible: return Value(visible);
	case avm1::kPropWidth:
	case avm1::kPropHeight: {
		swf::Rect r;
		if (!boundsIn(matrix, r)) {
			return Value(0);
		}
		return Value((index == avm1::kPropWidth ? r.xMax - r.xMin : r.yMax - r.yMin) / static_cast<double>(kTwips));
	}
	case avm1::kPropTarget: return Value(slashPath());
	case avm1::kPropName: return Value(name);
	case avm1::kPropDropTarget: return Value("");
	case avm1::kPropUrl: return Value(clip && !clip->url.empty() ? clip->url : std::string("mainMenu.swf"));
	case avm1::kPropHighQuality: return Value(1);
	case avm1::kPropFocusRect: return Value(true);
	case avm1::kPropSoundBufTime: return Value(5);
	case avm1::kPropQuality: return Value("HIGH");
	case avm1::kPropXMouse:
	case avm1::kPropYMouse: {
		const swf::Point local = inverse(worldMatrix()).apply({player.mouseX() * kTwips, player.mouseY() * kTwips});
		return Value((index == avm1::kPropXMouse ? local.x : local.y) / kTwips);
	}
	default: return Value();
	}
}

void DisplayObject::setProperty(Interpreter& vm, int index, const Value& value)
{
	const auto finite = [&](double& n) {
		n = number(vm, value);
		return std::isfinite(n);
	};
	double n = 0;
	switch (index) {
	case avm1::kPropX:
		if (finite(n)) {
			matrix.tx = static_cast<float>(n * kTwips);
			scriptTransformed = true;
		}
		break;
	case avm1::kPropY:
		if (finite(n)) {
			matrix.ty = static_cast<float>(n * kTwips);
			scriptTransformed = true;
		}
		break;
	case avm1::kPropXScale:
		if (finite(n)) {
			decompose();
			setScale(n, yscale, rotation);
		}
		break;
	case avm1::kPropYScale:
		if (finite(n)) {
			decompose();
			setScale(xscale, n, rotation);
		}
		break;
	case avm1::kPropRotation:
		if (finite(n)) {
			decompose();
			n = std::fmod(n, 360.0);
			if (n > 180) {
				n -= 360;
			} else if (n < -180) {
				n += 360;
			}
			setScale(xscale, yscale, n);
		}
		break;
	case avm1::kPropAlpha:
		if (finite(n)) {
			colour.mult[3] = static_cast<float>(n / 100.0);
			scriptTransformed = true;
		}
		break;
	case avm1::kPropVisible:
		if (value.isBoolean()) {
			visible = value.asBoolean();
		} else {
			n = number(vm, value);
			visible = !std::isnan(n) && n != 0;
		}
		break;
	case avm1::kPropWidth:
	case avm1::kPropHeight: {
		swf::Rect r;
		if (!finite(n) || !localBounds(r)) {
			break;
		}
		if (kind == Kind::Text) {
			// Text fields resize their box instead of scaling (by the
			// current scale, so the result is n pixels wide on the parent).
			auto& bounds = static_cast<TextField*>(this)->field.bounds;
			decompose();
			if (index == avm1::kPropWidth && xscale != 0) {
				bounds.xMax = bounds.xMin + static_cast<int32_t>(n * kTwips * 100.0 / std::fabs(xscale));
			} else if (index == avm1::kPropHeight && yscale != 0) {
				bounds.yMax = bounds.yMin + static_cast<int32_t>(n * kTwips * 100.0 / std::fabs(yscale));
			}
			break;
		}
		decompose();
		const double extent = index == avm1::kPropWidth ? r.xMax - r.xMin : r.yMax - r.yMin;
		if (extent <= 0) {
			break;
		}
		const double scale = n * kTwips / extent * 100.0;
		if (index == avm1::kPropWidth) {
			setScale(scale, yscale, rotation);
		} else {
			setScale(xscale, scale, rotation);
		}
		break;
	}
	case avm1::kPropName: name = vm.toString(value); break;
	default: break; // read-only or global
	}
}

swf::Matrix DisplayObject::worldMatrix() const
{
	swf::Matrix m = matrix;
	for (const DisplayObject* p = parent; p; p = p->parent) {
		m = multiply(p->matrix, m);
	}
	return m;
}

bool DisplayObject::localBounds(swf::Rect& out)
{
	const swf::CharacterDictionary& dictionary = player.dictionary();
	if (kind == Kind::Shape) {
		if (const swf::Shape* shape = dictionary.shape(character)) {
			out = shape->bounds;
			return true;
		}
	} else if (kind == Kind::MorphShape) {
		if (const swf::MorphShape* morph = dictionary.morphShape(character)) {
			const auto lerp = [this](int32_t a, int32_t b) { return static_cast<int32_t>(a + (b - a) * ratio); };
			out = {lerp(morph->startBounds.xMin, morph->endBounds.xMin), lerp(morph->startBounds.xMax, morph->endBounds.xMax),
				lerp(morph->startBounds.yMin, morph->endBounds.yMin), lerp(morph->startBounds.yMax, morph->endBounds.yMax)};
			return true;
		}
	}
	return false;
}

bool DisplayObject::boundsIn(const swf::Matrix& space, swf::Rect& out)
{
	swf::Rect local;
	if (!localBounds(local)) {
		return false;
	}
	out = transformRect(space, local);
	return true;
}

bool DisplayObject::hitTest(swf::Point stage, bool shape, bool includeHidden)
{
	if (!visible && !includeHidden) {
		return false;
	}
	return hitLocal(inverse(worldMatrix()).apply(stage), shape, includeHidden);
}

bool DisplayObject::hitLocal(swf::Point local, bool shape, bool includeHidden)
{
	(void)includeHidden;
	if (kind == Kind::Shape && shape) {
		return player.drawer().hitShape(character, local);
	}
	swf::Rect r;
	return localBounds(r) && contains(r, local);
}

void DisplayObject::draw(Drawer& drawer, const swf::Matrix& world, const swf::ColorTransform& tint, std::vector<MenuBatch>& out)
{
	if (!visible || isMask || clipDepth != 0) {
		return;
	}
	const swf::Matrix at = multiply(world, matrix);
	const swf::ColorTransform c = combine(tint, colour);
	if (kind == Kind::Shape) {
		drawer.shape(character, at, c, out);
	} else if (kind == Kind::MorphShape) {
		drawer.morphShape(character, ratio, at, c, out);
	}
}

Clip* DisplayObject::root()
{
	DisplayObject* top = this;
	while (top->parent) {
		top = top->parent;
	}
	return top->kind == Kind::Clip ? static_cast<Clip*>(top) : nullptr;
}

std::string DisplayObject::targetPath() const
{
	if (!parent) {
		return removed ? std::string() : std::string("_level0");
	}
	return parent->targetPath() + "." + name;
}

std::string DisplayObject::slashPath() const
{
	if (!parent) {
		return "/";
	}
	const std::string above = parent->slashPath();
	return (above == "/" ? above : above + "/") + name;
}

bool DisplayObject::isAncestorOf(const DisplayObject* other) const
{
	for (; other; other = other->parent) {
		if (other == this) {
			return true;
		}
	}
	return false;
}

// --- TextField

TextField::TextField(Player& player, uint16_t character, const swf::EditText& definition)
	: DisplayObject(player, Kind::Text, character), field(definition)
{
	value = field.initialText;
	variable = field.variableName;
	input = !field.readOnly;
	selectable = !field.noSelect;
	background = field.border;
	if (!field.hasTextColor) {
		field.textColor = {0, 0, 0, 255};
	}
	if (field.hasMaxLength) {
		maxChars = field.maxLength;
	}
	if (field.autoSize) {
		autoSize = "left";
	}
	caret = value.size();
}

std::string TextField::plainText() const { return field.html ? stripHtml(value) : value; }

void TextField::setText(Interpreter& vm, const std::string& text, bool html)
{
	(void)html;
	value = text;
	caret = std::min(caret, value.size());
	if (!variable.empty()) {
		player.writeVariable(vm, *this);
	}
	applyAutoSize();
}

void TextField::applyAutoSize()
{
	field.autoSize = autoSize != "none";
	if (!field.autoSize) {
		return;
	}
	swf::EditText measure = field;
	if (!field.wordWrap) {
		measure.bounds.xMax = measure.bounds.xMin + 1000000;
	}
	const Drawer::TextLayout layout = player.drawer().layoutText(measure, plainText().empty() ? std::string(" ") : value);
	const int32_t height = static_cast<int32_t>(std::ceil(layout.textHeight)) + 80;
	field.bounds.yMax = field.bounds.yMin + height;
	if (field.wordWrap) {
		return;
	}
	const int32_t width = static_cast<int32_t>(std::ceil(layout.textWidth)) + 80 + field.leftMargin + field.rightMargin;
	if (autoSize == "right") {
		field.bounds.xMin = field.bounds.xMax - width;
	} else if (autoSize == "center") {
		const int32_t middle = (field.bounds.xMin + field.bounds.xMax) / 2;
		field.bounds.xMin = middle - width / 2;
		field.bounds.xMax = field.bounds.xMin + width;
	} else {
		field.bounds.xMax = field.bounds.xMin + width;
	}
}

float TextField::textWidth() const { return player.drawer().layoutText(field, value).textWidth / kTwips; }

float TextField::textHeight() const { return player.drawer().layoutText(field, value).textHeight / kTwips; }

bool TextField::specialMember(Interpreter& vm, const std::string& member, Value& out)
{
	if (member == "text") {
		out = Value(plainText());
	} else if (member == "htmlText") {
		out = Value(value);
	} else if (member == "html") {
		out = Value(field.html);
	} else if (member == "textColor") {
		out = Value(static_cast<double>(rgbOf(field.textColor)));
	} else if (member == "variable") {
		out = variable.empty() ? Value::null() : Value(variable);
	} else if (member == "autoSize") {
		out = Value(autoSize);
	} else if (member == "maxChars") {
		out = maxChars > 0 ? Value(maxChars) : Value::null();
	} else if (member == "multiline") {
		out = Value(field.multiline);
	} else if (member == "wordWrap") {
		out = Value(field.wordWrap);
	} else if (member == "selectable") {
		out = Value(selectable);
	} else if (member == "type") {
		out = Value(input ? "input" : "dynamic");
	} else if (member == "length") {
		out = Value(static_cast<double>(avm1::utf16Length(plainText())));
	} else if (member == "textWidth") {
		out = Value(std::round(textWidth()));
	} else if (member == "textHeight") {
		out = Value(std::round(textHeight()));
	} else if (member == "password") {
		out = Value(field.password);
	} else if (member == "border") {
		out = Value(field.border);
	} else if (member == "background") {
		out = Value(background);
	} else if (member == "backgroundColor") {
		out = Value(static_cast<double>(rgbOf(backgroundColour)));
	} else if (member == "borderColor") {
		out = Value(static_cast<double>(rgbOf(borderColour)));
	} else if (member == "embedFonts") {
		out = Value(field.useOutlines);
	} else if (member == "scroll") {
		out = Value(scroll);
	} else if (member == "maxscroll" || member == "bottomScroll") {
		const Drawer::TextLayout layout = player.drawer().layoutText(field, value);
		const int lines = static_cast<int>(layout.lines.size());
		const int shown = layout.lineHeight > 0
			? std::max(1, static_cast<int>((field.bounds.yMax - field.bounds.yMin - 80) / layout.lineHeight))
			: 1;
		out = Value(member == "maxscroll" ? std::max(1, lines - shown + 1) : std::min(lines, scroll + shown - 1));
	} else if (member == "hscroll" || member == "maxhscroll") {
		out = Value(0);
	} else if (member == "restrict") {
		out = Value::null();
	} else if (member == "condenseWhite") {
		out = Value(false);
	} else {
		return DisplayObject::specialMember(vm, member, out);
	}
	return true;
}

bool TextField::setSpecialMember(Interpreter& vm, const std::string& member, const Value& v)
{
	if (member == "text") {
		setText(vm, v.isUndefined() ? std::string() : vm.toString(v), false);
	} else if (member == "htmlText") {
		setText(vm, vm.toString(v), true);
	} else if (member == "html") {
		field.html = avm1::toBoolean(v);
	} else if (member == "textColor") {
		field.textColor = rgbaOf(static_cast<uint32_t>(avm1::toUint32(vm.toNumber(v))), field.textColor.a);
	} else if (member == "variable") {
		variable = v.isNullish() ? std::string() : vm.toString(v);
		lastVariableText.clear();
	} else if (member == "autoSize") {
		if (v.isBoolean()) {
			autoSize = v.asBoolean() ? "left" : "none";
		} else if (v.isString()) {
			const std::string s = v.asString();
			autoSize = s == "left" || s == "right" || s == "center" ? s : "none";
		} else {
			autoSize = "none";
		}
		applyAutoSize();
	} else if (member == "maxChars") {
		maxChars = v.isNullish() ? 0 : static_cast<int>(vm.toNumber(v));
	} else if (member == "multiline") {
		field.multiline = avm1::toBoolean(v);
	} else if (member == "wordWrap") {
		field.wordWrap = avm1::toBoolean(v);
		applyAutoSize();
	} else if (member == "selectable") {
		selectable = avm1::toBoolean(v);
	} else if (member == "type") {
		input = vm.toString(v) == "input";
	} else if (member == "password") {
		field.password = avm1::toBoolean(v);
	} else if (member == "border") {
		field.border = avm1::toBoolean(v);
	} else if (member == "background") {
		background = avm1::toBoolean(v);
	} else if (member == "backgroundColor") {
		backgroundColour = rgbaOf(avm1::toUint32(vm.toNumber(v)));
	} else if (member == "borderColor") {
		borderColour = rgbaOf(avm1::toUint32(vm.toNumber(v)));
	} else if (member == "embedFonts") {
		field.useOutlines = avm1::toBoolean(v);
	} else if (member == "scroll") {
		scroll = std::max(1, static_cast<int>(vm.toNumber(v)));
	} else if (member == "textWidth" || member == "textHeight" || member == "length" || member == "maxscroll" ||
		member == "bottomScroll" || member == "maxhscroll") {
		// read-only
	} else {
		return DisplayObject::setSpecialMember(vm, member, v);
	}
	return true;
}

bool TextField::localBounds(swf::Rect& out)
{
	out = field.bounds;
	return true;
}

bool TextField::hitLocal(swf::Point local, bool shape, bool includeHidden)
{
	(void)shape, (void)includeHidden;
	return contains(field.bounds, local);
}

void TextField::draw(Drawer& drawer, const swf::Matrix& world, const swf::ColorTransform& tint, std::vector<MenuBatch>& out)
{
	if (!visible || isMask || clipDepth != 0) {
		return;
	}
	const swf::Matrix at = multiply(world, matrix);
	const swf::ColorTransform c = combine(tint, colour);
	if (background) {
		drawer.rectangle(field.bounds, backgroundColour, at, c, out);
	}
	if (field.border) {
		const swf::Rect& b = field.bounds;
		drawer.rectangle({b.xMin, b.xMax, b.yMin, b.yMin + 20}, borderColour, at, c, out);
		drawer.rectangle({b.xMin, b.xMax, b.yMax - 20, b.yMax}, borderColour, at, c, out);
		drawer.rectangle({b.xMin, b.xMin + 20, b.yMin, b.yMax}, borderColour, at, c, out);
		drawer.rectangle({b.xMax - 20, b.xMax, b.yMin, b.yMax}, borderColour, at, c, out);
	}
	float scrollY = 0;
	if (scroll > 1) {
		scrollY = drawer.layoutText(field, value).lineHeight * static_cast<float>(scroll - 1);
	}
	drawer.editText(field, value, at, c, out, scrollY);
	// A caret blinking at the end of the focused input field's text.
	if (input && player.focus() == this && (player.currentTick() / 15) % 2 == 0) {
		const Drawer::TextLayout layout = drawer.layoutText(field, value.empty() ? std::string(" ") : value);
		float x = layout.left, y = field.bounds.yMin + 40.0f;
		if (!layout.lines.empty() && !value.empty()) {
			const Drawer::TextLine& line = layout.lines.back();
			x += line.width;
			if (field.align == swf::EditText::Align::Right) {
				x += layout.width - line.width;
			} else if (field.align == swf::EditText::Align::Center) {
				x += (layout.width - line.width) / 2;
			}
			y += layout.lineHeight * static_cast<float>(layout.lines.size() - 1);
		}
		const float height = layout.lineHeight > 0 ? layout.lineHeight : field.fontHeight;
		drawer.rectangle({static_cast<int32_t>(x), static_cast<int32_t>(x + 20), static_cast<int32_t>(y),
							 static_cast<int32_t>(y + height)},
			field.textColor, at, c, out);
	}
}

// --- ButtonObject

ButtonObject::ButtonObject(Player& player, uint16_t character, const swf::Button& button)
	: DisplayObject(player, Kind::Button, character), button(button)
{
}

bool ButtonObject::localBounds(swf::Rect& out)
{
	bool any = false;
	for (size_t i = 0; i < parts.size() && i < button.records.size(); ++i) {
		swf::Rect r;
		if (parts[i] && (button.records[i].states & (state | swf::kButtonHitTest)) && parts[i]->boundsIn(parts[i]->matrix, r)) {
			unite(out, any, r);
		}
	}
	return any;
}

bool ButtonObject::hitLocal(swf::Point local, bool shape, bool includeHidden)
{
	for (size_t i = 0; i < parts.size() && i < button.records.size(); ++i) {
		if (parts[i] && (button.records[i].states & swf::kButtonHitTest) &&
			parts[i]->hitLocal(inverse(parts[i]->matrix).apply(local), shape, includeHidden)) {
			return true;
		}
	}
	return false;
}

void ButtonObject::draw(Drawer& drawer, const swf::Matrix& world, const swf::ColorTransform& tint, std::vector<MenuBatch>& out)
{
	if (!visible || isMask || clipDepth != 0) {
		return;
	}
	const swf::Matrix at = multiply(world, matrix);
	const swf::ColorTransform c = combine(tint, colour);
	for (size_t i = 0; i < parts.size() && i < button.records.size(); ++i) {
		if (parts[i] && (button.records[i].states & state)) {
			parts[i]->draw(drawer, at, c, out);
		}
	}
}

void ButtonObject::clearReferences()
{
	parts.clear();
	DisplayObject::clearReferences();
}

// --- Clip

Clip::~Clip()
{
	for (auto& [d, child] : children) {
		if (child && child->parent == this) {
			child->parent = nullptr;
		}
	}
}

DisplayObject* Clip::childNamed(const std::string& childName) const
{
	for (const auto& [d, child] : children) {
		if (avm1::PropertyMap::key(child->name) == avm1::PropertyMap::key(childName) && child->kind != Kind::Shape && child->kind != Kind::MorphShape) {
			return child.get();
		}
	}
	return nullptr;
}

std::shared_ptr<DisplayObject> Clip::childAt(int at) const
{
	const auto it = children.find(at);
	return it == children.end() ? nullptr : it->second;
}

int Clip::highestDepth() const { return children.empty() ? -1 : children.rbegin()->first; }

bool Clip::getMember(Interpreter& vm, const std::string& member, Value& out)
{
	if (findOwn(member)) {
		return avm1::Object::getMember(vm, member, out);
	}
	if (DisplayObject* child = childNamed(member)) {
		out = Value(child->shared_from_this());
		return true;
	}
	return DisplayObject::getMember(vm, member, out);
}

bool Clip::hasOwnMember(const std::string& member) const { return DisplayObject::hasOwnMember(member) || childNamed(member); }

bool Clip::localBounds(swf::Rect& out)
{
	bool any = false;
	if (image) {
		unite(out, any, {0, static_cast<int32_t>(image->width * kTwips), 0, static_cast<int32_t>(image->height * kTwips)});
	}
	if (!drawing.paths.empty()) {
		unite(out, any, drawing.bounds);
	}
	for (const auto& [d, child] : children) {
		swf::Rect r;
		if (child->clipDepth == 0 && !child->isMask && child->boundsIn(child->matrix, r)) {
			unite(out, any, r);
		}
	}
	return any;
}

bool Clip::hitLocal(swf::Point local, bool shape, bool includeHidden)
{
	if (image && contains({0, static_cast<int32_t>(image->width * kTwips), 0, static_cast<int32_t>(image->height * kTwips)}, local)) {
		return true;
	}
	if (!drawing.paths.empty() && contains(drawing.bounds, local)) {
		if (!shape || hitMesh(drawingMesh, local)) {
			return true;
		}
	}
	if (!shape) {
		swf::Rect r;
		return localBounds(r) && contains(r, local);
	}
	for (auto it = children.rbegin(); it != children.rend(); ++it) {
		DisplayObject& child = *it->second;
		if ((!child.visible && !includeHidden) || child.clipDepth != 0 || child.isMask) {
			continue;
		}
		if (child.hitLocal(inverse(child.matrix).apply(local), shape, includeHidden)) {
			return true;
		}
	}
	return false;
}

void Clip::draw(Drawer& drawer, const swf::Matrix& world, const swf::ColorTransform& tint, std::vector<MenuBatch>& out)
{
	if (!visible || isMask || clipDepth != 0) {
		return;
	}
	const swf::Matrix at = multiply(world, matrix);
	const swf::ColorTransform c = combine(tint, colour);
	if (c.mult[3] <= 0 && c.add[3] <= 0) {
		return; // fully transparent
	}
	if (image) {
		drawer.image(image, at, c, out);
	}
	if (!drawing.paths.empty()) {
		if (drawingDirty) {
			drawingMesh = swf::tessellateShape(drawing);
			drawingDirty = false;
		}
		drawer.dynamicShape(drawing, drawingMesh, reinterpret_cast<uintptr_t>(this) & 0xffffffffffu, at, c, out);
	}
	// Mask layers (clip depths) and setMask() cut what they cover to their
	// bounding box on the stage.
	struct Layer {
		int lastDepth;
		swf::Rect box; // stage twips
	};
	std::vector<Layer> layers;
	const size_t begin = out.size();
	for (const auto& [d, child] : children) {
		layers.erase(std::remove_if(layers.begin(), layers.end(), [d = d](const Layer& l) { return d > l.lastDepth; }), layers.end());
		if (child->clipDepth != 0) {
			swf::Rect box;
			if (child->boundsIn(multiply(at, child->matrix), box)) {
				layers.push_back({child->clipDepth, box});
			} else {
				layers.push_back({child->clipDepth, {0, 0, 0, 0}});
			}
			continue;
		}
		const size_t first = out.size();
		child->draw(drawer, at, c, out);
		for (const Layer& layer : layers) {
			clipBatches(out, first, layer.box.xMin / kTwips, layer.box.yMin / kTwips, layer.box.xMax / kTwips, layer.box.yMax / kTwips);
		}
	}
	if (auto* m = dynamic_cast<DisplayObject*>(mask.get())) {
		swf::Rect box;
		if (!m->boundsIn(m->worldMatrix(), box)) {
			box = {0, 0, 0, 0};
		}
		clipBatches(out, begin, box.xMin / kTwips, box.yMin / kTwips, box.xMax / kTwips, box.yMax / kTwips);
	}
}

void Clip::clearReferences()
{
	for (auto& [d, child] : children) {
		if (child && child->parent == this) {
			child->parent = nullptr;
		}
	}
	children.clear();
	mask.reset();
	DisplayObject::clearReferences();
}

} // namespace dice::hfe::ui::player
