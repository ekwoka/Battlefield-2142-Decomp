// AIDLL.dll's strategic AI (the "SAI", one per side; vtbl 0x10118480,
// update 0x100a2dd0; constructor 0x100a39c0, destructor 0x100a3620 /
// 0x100a3f60), rewritten.
//
// The scripts describe it (AI/AIDefaultStrategies.ai, the level's
// GameModes/<mode>/<size>/ai/StrategicAreas.ai):
//   aiStrategy.createConstantCondition <name> <Crisp|Fuzzy|FuzzySqr> <compare> <Friendly|Enemy|Both> <type> <constant>
//   aiStrategy.setConditionStrength <Required|...|Advisory...>
//   aiStrategy.createPrerequisite <name>, aiStrategy.addCondition <condition> <weight>
//   aiStrategy.createStrategy <name>, .Aggression, .NumberOfAttacks, .NumberOfDefences,
//       .setStrategicObjectsModifier <type> <value> [Owned|Hostile|Neutral], .setPrerequisite <name>
//   ai.addSAIStrategy <side> <strategy>
//   aiStrategicArea.createFromControlPoint <name> <cp id> <temperature>, .addNeighbour,
//       .setOrderPosition <map> <x/y/z>, .addWayPoint <x/y/z> <map> <area> <area>, ...
//
// Each side picks a strategy (0x100a0130): every strategy's weight is its
// prerequisite (the sum of its conditions' values times their weights,
// 0x100a9300); while no weight moves by more than about 20 % the current
// one stays, otherwise a roulette wheel over the weights picks (the first
// added when all are 0). The strategy's modifiers scale each area's
// temperature by the area's status; areas get an attack value (not owned,
// next to an owned one) or a defence value (owned), and the best
// NumberOfAttacks / NumberOfDefences of them get squads, the nearest free
// squad first, until each has the strength it requires (0x100a21a0,
// 0x100a2630, 0x10089730). Squads get the area's order position as an
// attack or defend order.
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe::ai {

// Strategic object types (0x1005a1c0) and statuses (Owned 0, Hostile 1, Neutral 2).
enum class AreaStatus { Owned = 0, Hostile = 1, Neutral = 2 };
constexpr int kTypeControlPoint = 16;
constexpr int kTypeFront = 22;
constexpr int kTypeSafe = 23;
constexpr int kTypeEnemy = 24;
constexpr int kTypeNeutral = 26;
constexpr int kObjectTypes = 27;

struct StrategyCondition {
	enum class Conditional { Crisp, Fuzzy, FuzzySqr };
	enum class Compare { Equal, EqualGreater, EqualSmaller, Quotient, QuotientGreater, QuotientSmaller, Difference,
		DifferenceGreater, DifferenceSmaller };
	enum class Strength { Required, RequiredPositive, RequiredNegative, Advisory, AdvisoryPositive, AdvisoryNegative };
	Conditional conditional = Conditional::Crisp;
	Compare compare = Compare::Equal;
	int side = 0; // Friendly 0, Enemy 1, Both 2
	int type = 14; // Ticket 13, ControlPoint 14, Time 15, StartTime 16, Attacks 17, Defences 18,
	               // NumberOfFriendlyAreas 19, NumberOfNeutralAreas 20, NumberOfHostileAreas 21
	float constant = 0;
	// setConditionStrength follows every createConstantCondition in the
	// shipped AIDefaultStrategies.ai, so the default never counts; where
	// the original keeps it was not found.
	Strength strength = Strength::Required;
};

struct Strategy {
	std::string name;
	float aggression = 0.5f;
	int attacks = 0;
	int defences = 0;
	std::string prerequisite;
	float prerequisiteWeight = 1;
	float timeLimit = 1e9f;
	// mod[status][type], 1 by default (0x1005c2c0).
	std::array<std::array<float, kObjectTypes>, 3> modifiers;
	Strategy();
};

// The original's area object (vtable 0x10116280: constructed by
// 0x10062e10 for a box area and 0x1005ffe0 from a control point,
// destroyed by 0x10062f90 / 0x10062820; its strategic-object base, vtable
// 0x10116340: constructor 0x10063fc0, destructor 0x10063f90 / 0x10063c30).
struct StrategicArea {
	std::string name;
	int controlPoint = -1;     // createFromControlPoint
	float temperature = 50;    // its basic temperature (the third argument)
	int layer = 0;
	std::vector<std::string> neighbours;
	bool hasOrderPosition[2] = {false, false}; // Infantry, Vehicle
	float orderPosition[2][3] = {};
	int side = 0;
	float vehicleSearchRadius = 0;
	bool controlPointFlag = false; // addObjectTypeFlag ControlPoint
	// aiStrategicArea.create: an area of its own, a box over x/z at a
	// height, without a control point. The rewrite's SAI keeps its status
	// (boxAreaStatus) for the neighbours' flags; it orders no unit there.
	bool box = false;
	float boxMin[2] = {}, boxMax[2] = {};
	float height = 0;
	// setTakeable <side> <bool> (+399 + side, 0x1001ab30): whether a side's
	// units alone in an area without a control point make it theirs
	// (0x1005f210's status rule; a control point's area takes it from its
	// point, 0x100629e0).
	std::array<bool, 3> takeable {true, true, true};
	// Such an area's status for each side (+0x70 + 4 side), kept from one
	// update to the next; Neutral at the start (the area's constructor
	// wasn't traced).
	std::array<AreaStatus, 3> boxStatus {AreaStatus::Neutral, AreaStatus::Neutral, AreaStatus::Neutral};
};

// 0x1005f210's status rule for an area without a control point (+0x164
// -1), from the units of each side in it as the side knows them: with any
// there, the side's own alone (one or more, no enemy) make it Owned when
// it is takeable for the side; else the enemy's alone make it Hostile
// when it is takeable for the enemy (the other side's byte, 0x191 - (side
// != 1)); otherwise, and with nobody there, the status stays.
AreaStatus boxAreaStatus(AreaStatus previous, int friendly, int enemy, bool takeableMine, bool takeableTheirs);

struct StrategicWaypoint {
	float position[3];
	int map = 0; // Infantry 0, Vehicle 1
	std::string from, to;
};

struct StrategySettings {
	std::map<std::string, StrategyCondition> conditions;
	std::map<std::string, std::vector<std::pair<std::string, float>>> prerequisites;
	std::map<std::string, Strategy> strategies;
	std::map<int, std::vector<std::string>> sideStrategies; // ai.addSAIStrategy (sides 1, 2)
	std::vector<StrategicArea> areas;
	std::vector<StrategicWaypoint> waypoints;
	// aiStrategy.forceStrategy <name> <side> (0x1003a720, the strategy
	// manager's +0x84 + 4 side): a side's forced strategy, which the SAI
	// takes at every update in place of its choice (0x100a0130 reads it
	// through 0x100398d0); empty for none (an unknown name clears it).
	std::array<std::string, 3> forced;
	// Being edited by the scripts.
	std::string condition, prerequisite, strategy, area;

	// The data of AI/AIDefaultStrategies.ai and AIDefaultStrategiesAdd.ai,
	// for when the scripts were not run.
	void addDefaults();
	StrategicArea* findArea(const std::string& name);
};

// The values a side's conditions read (the status map 0x100a1450).
struct SideStatus {
	float tickets = 0;
	float controlPoints = 0; // held
	float time = 0;
	float sinceStrategy = 0;
	float attacks = 0, defences = 0;
	float friendlyAreas = 0, neutralAreas = 0, hostileAreas = 0;
	float value(int type) const;
};

// A condition's value (0x1005cce0 crisp, 0x1005ce20 fuzzy): Crisp gives
// +-1, Fuzzy the difference, FuzzySqr its signed square.
float conditionValue(const StrategyCondition& condition, const SideStatus& friendly, const SideStatus& enemy);
// A prerequisite's value: the weighted sum; a Required(Positive) term below
// 0 or a RequiredNegative one above 0 makes it 0, Advisory(Positive/
// Negative) terms are clamped to their sign.
float prerequisiteValue(const StrategySettings& settings, const std::string& name, const SideStatus& friendly,
	const SideStatus& enemy);

// SAI step 11 (0x100a09c0): the shares of the bots outside the areas'
// assignments to send to attack and to defend. No area held: all attack;
// no neutral nor hostile: all defend; else the running strategy's
// Aggression (aiStrategy.Aggression, the record's +4, 0.5 by default) and
// its complement.
void aggressionShares(float aggression, float friendlyAreas, float neutralAreas, float hostileAreas, float& attack,
	float& defence);
// Whether the next bot attacks, with A and D the attack and defence
// strengths assigned so far (rounded, the step 10 tally 0x100a0420): with
// none, an attack share of 0.5 or more; else a share above 0 and (no
// defence share, or A / (A + D) not over it).
bool assignsAttack(float attack, float defence, float attacking, float defending);

// Whether the strategy weights moved enough to pick again (0x100a0130):
// some prev/cur outside [0.83, 1.2) (1 when both are 0, 0 when only cur is).
bool strategyWeightsChanged(const std::vector<float>& previous, const std::vector<float>& current);
// Whether the running strategy's time is up when the weights did not move
// (0x100a0130, 0x100a02a8): after (2 - 2 x 0.5^picks) x its timeLimit
// seconds (picks: the times this side's record of it was re-picked for
// its time, +0xc), so at once the first time, then after timeLimit,
// 1.5 timeLimit, ... towards 2 timeLimit. A re-pick spins the roulette
// again and counts one more.
bool strategyTimeUp(float sinceStart, int picks, float timeLimit);

// A unit's certainty for a side: the side's AI object of it (vtbl
// 0x10118838; made when the side first learns of the unit, 0x1003b900 from
// BF2142.exe 0x008a2f00, with that time). Slot 9 (0x100a9b60): 1 while a
// sensor of the side senses it (+0xa8 above 0, slots 5 and 6
// 0x100aa040 / 0x100aa0f0); otherwise slot 10 (0x100a9b90):
// 1 - S((now - last seen) / degeneration), the template's degeneration
// (+0x50). A degeneration of 0 keeps it at 1 (S of a number that isn't
// finite is 0, 0x100cc930). The side's own units and neutral ones have the
// static object (0x008a3980's other kind): always 1.
float knowledgeCertainty(bool sensed, float sinceSeen, float degeneration);

// What the SAI knows of an area this update.
struct AreaState {
	AreaStatus status = AreaStatus::Neutral; // for the side
	bool contested = false; // owned, the flag below 0.9
	float temperature = 0;  // basic + units' temperatures, then the strategy's modifiers
	float friendlyStrength = 0, enemyStrength = 0;
	int required = 0;        // strength it needs
	float squadDistance = 0; // to the nearest friendly squad (attack value), 0: none
	bool hostileNeighbour = false, neutralNeighbour = false, ownedNeighbour = false;
};

// Required strength (FUN_1005f210): the enemies' strategic strengths' sum, at least 1, x 2 when owned, else
// x (2 - S(friendly / (friendly + enemy) battle strength)) x 1.25.
int requiredStrength(float enemyStrategicStrength, bool owned, float friendlyRatio = 0);
// The strategy's modifiers on an area's temperature (0x1005def0).
float modifiedTemperature(const Strategy& strategy, float temperature, AreaStatus status, bool controlPoint,
	bool hostileNeighbour);
// Defence value of an owned area (slot 10): (enemyStrength + temp) * 2,
// x1.5 with a hostile neighbour, else x1.25 with a neutral one.
float defenceValue(const AreaState& area);
// Attack value of an area not owned next to an owned one (slot 11):
// v = temp, x1.5 with a neutral neighbour (no hostile one), x1.25 with
// neither; 100 v / d - penalty for a squad 1..1e5 m away, -penalty
// otherwise; penalty = 1.5 (friendly/(friendly+enemy) + 1) * the enemy's
// average strategic strength (1 for soldiers).
float attackValue(const AreaState& area, float enemyAverageStrategicStrength = 1);
// The areas kept for attack/defence from candidates (value, index) sorted
// best first: the top n, then ranks n+1..2n with at least `keep` times the
// value at rank n (0.8 attack, 0.9 defence).
std::vector<int> selectAreas(std::vector<std::pair<float, int>> candidates, int n, float keep);

} // namespace dice::hfe::ai
