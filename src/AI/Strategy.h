// AIDLL.dll's strategic AI (the "SAI", one per side; vtbl 0x10118480,
// update 0x100a2dd0), rewritten.
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
};

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

// Whether the strategy weights moved enough to pick again (0x100a0130):
// some prev/cur outside [0.83, 1.2) (1 when both are 0, 0 when only cur is).
bool strategyWeightsChanged(const std::vector<float>& previous, const std::vector<float>& current);

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

// Required strength (FUN_1005f210): round(2 * max(enemies' strategic strength, 1)), x1.25 unless owned.
int requiredStrength(float enemyStrategicStrength, bool owned);
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
