// AIDLL.dll's behaviour selection, rewritten (Bot::think 0x10099280 and
// the arbiter vtbl_1011b868).
//
// Each equipment type (aiSettings.setVehicle: Infantery, Tank, Car, ...)
// has a set of behaviours (aiSettings.setVehicleBehaviour <vehicle>
// <behaviour> <BB class> <BBP pattern> <priority> <urgency curve>
// <inhibitors>). Every think, for each behaviour b:
//
//   weight(b) = personality[b]                          (the bot's modifiers, "StandardWeights")
//             * curve_cur(t) if b is the running one    (FUN_100ea430; t = time since the last think)
//             * inhibitors_cur[b]                       (the running behaviour's modifier set, e.g. AvoidInhibit)
//
// (the last two only while a behaviour runs). A behaviour whose weight is
// above 0 computes its urgency (BB vtable slot 9: urgency(bot, full,
// weight)); the patterns decide themselves whether they scale by the
// weight (MoveTo, Idle, MedicAssist and Change do; Fire, Revive and Flee
// return fixed urgencies). The arbiter latches every behaviour's urgency
// and runs the largest (ties: the first in setVehicleBehaviour order;
// none above 0: the default behaviour) whenever the bot needs a new plan
// or some behaviour's urgency left [0.87, 1.15] times its latched value
// or went from 0 to non-zero or back (0x100f7030 with 0x100ccdd0).
#pragma once

#include <map>
#include <string>
#include <vector>

namespace dice::hfe::ai {

// aiSettings.createUCConstant/createUCLinear/createUCXInverse: the weight of
// the running behaviour by the seconds since the last think.
struct UrgencyCurve {
	enum class Kind { Constant, Linear, XInverse };
	Kind kind = Kind::Constant;
	float a = 1, b = 0, c = 0, d = 0;
	// Constant (0x10087ec0): a. Linear (0x10087ef0): max(0, a t + b).
	// XInverse (0x10087f50): max(0, a / (c t + b) + d).
	float value(float t) const;
};

// The fuzzy curve AIDLL uses for scores in 0..1 (FUN_100cbce0, table built
// by 0x100cb6f0): a table of 101 values looked up at trunc(x * 100), linear
// (x * 2.924) up to 0.18, flattening towards 1. Clamped outside 0..1.
float fuzzy(float x);
// The S-shaped curve (0x100cc930, table built by 0x100cc350): 101 values,
// interpolated, 0.5 at 0.5; clamped to 0..1 (0 for not a number).
float scurve(float x);

struct BehaviourSetup {
	std::string name;     // the behaviour (MoveTo, Fire, ...)
	std::string bb;       // its BB class (BBMoveTo, BBFireInfantery, ...)
	std::string pattern;  // its BBP class (BBPGotoWaypointInfantery, ...)
	int priority = 0;     // the think queue it is evaluated in (not modelled: all are evaluated every think)
	std::string curve;    // UCUnion, UCFire, ...
	std::string inhibitors; // the modifier set applied to the others while it runs
};

struct VehicleSetup {
	std::string modifiers; // aisettings.setVehicleMods (not used by bots: ai.addBot gives the personality)
	std::vector<BehaviourSetup> behaviours;
	std::string defaultBehaviour = "Idle";
	std::map<std::string, float> materialCost;
	std::string sensingPattern = "Null";
};

// The selection state of one bot.
struct Arbitration {
	int running = -1;               // index into the set, -1 before the first plan
	bool replan = true;             // Bot +0x10: the next think picks anew
	std::vector<float> latched;     // urgencies as of the last pick
	std::vector<float> urgency;     // as computed this think
};

// Weights for this think (above); `personality` and the inhibitor sets
// look behaviours up by name (missing: 1).
std::vector<float> behaviourWeights(const std::vector<BehaviourSetup>& set, const Arbitration& state, float sinceLastThink,
	const std::map<std::string, float>& personality, const std::map<std::string, std::map<std::string, float>>& modifierSets,
	const std::map<std::string, UrgencyCurve>& curves);

// Whether the arbiter picks anew: the flag, or an urgency that changed
// (0x100ccdd0 with 0.87 and 1.15).
bool urgencyChanged(float latched, float now, float low = 0.87f, float high = 1.15f);

// Picks (latching the urgencies) when needed; returns the running index.
// `defaultIndex` runs when no urgency is above 0.
int arbitrate(Arbitration& state, int defaultIndex);

// MoveTo's goal (the order a bot follows, 0x1008aaa0): urgency at a
// distance (squared, d2) from the last waypoint of radius r (at least
// botRadius + 1). Arrived (d2 < r^2): 0 and done. Otherwise
// clamp(d2' / (1.5 r^2), 0.1, 1) times a factor: 1 for a plain point; for
// a strategic area, 2 when the bot's team does not hold it, 0 once inside
// it (it is then reached), else 1 with d2' = d2 - areaRadius^2.
struct GoalArea {
	bool exists = false;
	bool teamHolds = true;  // the area's per-team "taken" slot is 0 (provisional reading of +0x70)
	bool inside = false;    // the bot inside the area (0x1005ddd0)
	float radius = 0;       // the area's radius for the bot's team (+0x98)
};
float gotoUrgency(float d2, float radius, float botRadius, const GoalArea& area, bool& arrived);

} // namespace dice::hfe::ai
