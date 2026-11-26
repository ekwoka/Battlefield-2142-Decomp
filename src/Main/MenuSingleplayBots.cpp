#include "MenuSingleplayBots.h"

#include "MenuBindings.h"

#include "UI/Avm1/Interpreter.h"
#include "UI/Player/Player.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace dice::hfe::menu {

namespace {

using ui::avm1::Interpreter;
using ui::avm1::Value;

constexpr const char* kPage = "singleplay.mc_singleplayer";
// The added instances' names (the page's own are mcBotSkill, mcBotSkillText...).
constexpr const char* kHeader = "mcBotCountHeader";
constexpr const char* kSlider = "mcBotCount";
constexpr const char* kNumber = "mcBotCountNumber";
constexpr const char* kCallback = "adjustBotCount";
// The page's startServer, kept on the page while the wrapper stands in.
constexpr const char* kRetailStart = "__retailStartServer";
// Above the page's authored depths (1..26).
constexpr int kHeaderDepth = 1000, kSliderDepth = 1001, kNumberDepth = 1002;
// Page units, after the authored rows: HEADER_botskill at (281.9, 535),
// mcBotSkill at (281.9, 554) scaled (1, 0.996), the rounds' number
// mcRoundsPerMapText at (464.9, 502) scaled 0.187 wide, 183 right of its
// slider. The new column starts at START PLAYLIST's left edge (595, 574,
// just below the slider), on the bot skill's lines.
constexpr double kColumnX = 595, kHeaderY = 535, kSliderY = 554;
constexpr double kNumberX = kColumnX + (464.9 - 281.9);

double percentOf(int players)
{
	return static_cast<double>(players - MenuBindings::kSingleplayPlayersMin)
		/ (MenuBindings::kSingleplayPlayersMax - MenuBindings::kSingleplayPlayersMin);
}

int playersOf(double percent)
{
	if (!std::isfinite(percent)) {
		percent = 0;
	}
	const int span = MenuBindings::kSingleplayPlayersMax - MenuBindings::kSingleplayPlayersMin;
	const int players = MenuBindings::kSingleplayPlayersMin + static_cast<int>(std::lround(percent * span));
	return std::max(MenuBindings::kSingleplayPlayersMin, std::min(MenuBindings::kSingleplayPlayersMax, players));
}

// The authored init values of a TextContainer (as HEADER_botskill's and
// mcRoundsPerMapText's onClipEvent(construct)).
Value textInit(Interpreter& vm, const std::string& label, const char* localizationId, const char* style)
{
	const Value init(vm.newObject());
	vm.setMember(init, "__inAuthorMode", Value(true));
	vm.setMember(init, "label", Value(label));
	vm.setMember(init, "labelColor", Value(16777215));
	vm.setMember(init, "localizationId", Value(localizationId));
	vm.setMember(init, "styleId", Value(style));
	vm.setMember(init, "upperCase", Value(false));
	return init;
}

Value attach(Interpreter& vm, const Value& page, const char* linkage, const char* name, int depth, const Value& init, double x,
	double y, double xscale, double yscale)
{
	const Value clip = vm.callMember(page, "attachMovie", {Value(linkage), Value(name), Value(depth), init});
	if (clip.isObject()) {
		vm.setMember(clip, "_x", Value(x));
		vm.setMember(clip, "_y", Value(y));
		vm.setMember(clip, "_xscale", Value(xscale));
		vm.setMember(clip, "_yscale", Value(yscale));
	}
	return clip;
}

void showNumber(Interpreter& vm, const Value& page, int players)
{
	const Value number = vm.getMember(page, kNumber);
	if (number.isObject()) {
		vm.callMember(number, "setValue", {Value(std::to_string(players))});
	}
}

void add(Interpreter& vm, const Value& page, MenuBindings& bindings)
{
	MenuBindings* const owner = &bindings;
	attach(vm, page, "TextContainer", kHeader, kHeaderDepth, textInit(vm, "Max players", "MULTIPLAY_3_maxplayers", "TradeGothic_13_left"),
		kColumnX, kHeaderY, 119.3, 100);
	// The callback first: the slider calls it by name on its _parent.
	vm.setMember(page, kCallback, Value(vm.newFunction([owner](Interpreter& vm, ui::avm1::CallArgs& call) {
		const Value self(call.thisObject);
		const Value slider = vm.getMember(self, kSlider);
		if (!slider.isObject()) {
			return Value();
		}
		const int players = playersOf(vm.toNumber(vm.callMember(slider, "getPercent", {})));
		owner->setSingleplayPlayers(players);
		showNumber(vm, self, players);
		return Value();
	})));
	const Value init(vm.newObject());
	vm.setMember(init, "__inAuthorMode", Value(true));
	vm.setMember(init, "callBackFunctionSpecial", Value(kCallback));
	vm.setMember(init, "isEnabled", Value(true));
	vm.setMember(init, "localizationId", Value(""));
	const Value slider = attach(vm, page, "Slider", kSlider, kSliderDepth, init, kColumnX, kSliderY, 100, 99.6);
	attach(vm, page, "TextContainer", kNumber, kNumberDepth, textInit(vm, "16", "", "Helvetica_11_left"), kNumberX, kSliderY,
		18.7, 100);
	const int players = bindings.singleplayPlayers();
	if (slider.isObject()) {
		vm.callMember(slider, "setPercent", {Value(percentOf(players))});
	}
	showNumber(vm, page, players);

	// startServer (the playlist button's this._parent.startServer() and
	// instantAction's startServer()) with the target armed.
	const Value retail = vm.getMember(page, "startServer");
	if (retail.isObject()) {
		vm.setMember(page, kRetailStart, retail);
		vm.setMember(page, "startServer", Value(vm.newFunction([owner](Interpreter& vm, ui::avm1::CallArgs& call) {
			const Value start = vm.getMember(Value(call.thisObject), kRetailStart);
			owner->setSingleplayStarting(true);
			Value result;
			try {
				result = vm.call(start, call.thisObject, call.args);
			} catch (...) {
				owner->setSingleplayStarting(false);
				throw;
			}
			owner->setSingleplayStarting(false);
			return result;
		})));
	}
}

void removeControl(Interpreter& vm, const Value& page)
{
	for (const char* name : {kHeader, kSlider, kNumber}) {
		const Value clip = vm.getMember(page, name);
		if (clip.isObject()) {
			vm.callMember(clip, "removeMovieClip", {});
		}
	}
	vm.setMember(page, kCallback, Value());
	const Value retail = vm.getMember(page, kRetailStart);
	if (retail.isObject()) {
		vm.setMember(page, "startServer", retail);
		vm.setMember(page, kRetailStart, Value());
	}
}

} // namespace

void updateSingleplayBotCount(ui::Player& player, MenuBindings& bindings)
{
	Interpreter* vm = player.interpreter();
	if (!vm) {
		return;
	}
	const Value page = player.evaluate(kPage);
	if (!page.isObject()) {
		return;
	}
	const bool on = bindings.singleplayBotCount && bindings.singleplayBotCount();
	const bool shown = vm->getMember(page, kSlider).isObject();
	if (on && !shown) {
		add(*vm, page, bindings);
	} else if (!on && shown) {
		removeControl(*vm, page);
	}
}

} // namespace dice::hfe::menu
