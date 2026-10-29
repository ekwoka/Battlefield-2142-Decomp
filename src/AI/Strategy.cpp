#include "Strategy.h"

#include "Behaviour.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::ai {

Strategy::Strategy()
{
	for (auto& row : modifiers) {
		row.fill(1.0f);
	}
}

void StrategySettings::addDefaults()
{
	using C = StrategyCondition;
	const auto condition = [this](const char* name, C::Conditional conditional, C::Compare compare, int side, int type,
							   float constant, C::Strength strength) {
		conditions[name] = {conditional, compare, side, type, constant, strength};
	};
	condition("basicEnemyCPCond", C::Conditional::Crisp, C::Compare::EqualGreater, 1, 14, 1, C::Strength::Advisory);
	condition("basicNeutralCPCond", C::Conditional::Crisp, C::Compare::EqualGreater, 0, 20, 1, C::Strength::Advisory);
	condition("noEnemyCond", C::Conditional::Crisp, C::Compare::Equal, 1, 14, 0, C::Strength::Required);
	condition("noNeutralCond", C::Conditional::Crisp, C::Compare::Equal, 0, 20, 0, C::Strength::Required);
	prerequisites["basicEnemyPrereq"] = {{"basicEnemyCPCond", 20.0f}, {"basicNeutralCPCond", 20.0f}};
	prerequisites["guardPrereq"] = {{"noEnemyCond", 2.0f}, {"noNeutralCond", 2.0f}};
	Strategy guard;
	guard.name = "basicGuard";
	guard.aggression = 0;
	guard.attacks = 0;
	guard.defences = 4;
	guard.modifiers[static_cast<int>(AreaStatus::Owned)][kTypeControlPoint] = 10;
	guard.prerequisite = "guardPrereq";
	strategies[guard.name] = guard;
	Strategy attack;
	attack.name = "basicAttackEnemy";
	attack.aggression = 1;
	attack.attacks = 3;
	attack.defences = 0;
	attack.modifiers[static_cast<int>(AreaStatus::Hostile)][kTypeControlPoint] = 10;
	attack.modifiers[static_cast<int>(AreaStatus::Neutral)][kTypeControlPoint] = 12;
	attack.prerequisite = "basicEnemyPrereq";
	strategies[attack.name] = attack;
	for (int side = 1; side <= 2; ++side) {
		sideStrategies[side] = {"basicAttackEnemy", "basicGuard"};
	}
}

StrategicArea* StrategySettings::findArea(const std::string& name)
{
	for (StrategicArea& a : areas) {
		if (a.name == name) {
			return &a;
		}
	}
	return nullptr;
}

float SideStatus::value(int type) const
{
	switch (type) {
	case 13:
		return tickets;
	case 14:
		return controlPoints;
	case 15:
		return time;
	case 16:
		return sinceStrategy;
	case 17:
		return attacks;
	case 18:
		return defences;
	case 19:
		return friendlyAreas;
	case 20:
		return neutralAreas;
	case 21:
		return hostileAreas;
	default:
		return 0;
	}
}

float conditionValue(const StrategyCondition& condition, const SideStatus& friendly, const SideStatus& enemy)
{
	// A constant condition (0x100a9620): the friendly side's value for
	// Friendly, the enemy's for anything else (Both included), against the
	// constant k; 0x1005cf90 then compares v - k (Equal*, Difference*) or
	// v / k (Quotient*) with 0 (the condition's +8, which
	// createConstantCondition leaves 0).
	using C = StrategyCondition;
	const float v = (condition.side == 0 ? friendly : enemy).value(condition.type);
	const float k = condition.constant;
	const int compare = static_cast<int>(condition.compare);
	if (condition.conditional == C::Conditional::Crisp) {
		// 0x1005cce0.
		switch (compare) {
		case 0:
		case 6:
			return v - k == 0 ? 1.0f : -1.0f;
		case 1:
		case 7:
			return 0 <= v - k ? 1.0f : -1.0f;
		case 2:
		case 8:
			return v - k <= 0 ? 1.0f : -1.0f;
		case 3:
			return k == 0 ? -1.0f : v / k == 0 ? 1.0f : -1.0f;
		case 4:
			return k == 0 || 0 <= v / k ? 1.0f : -1.0f;
		case 5:
			return k == 0 ? -1.0f : v / k <= 0 ? 1.0f : -1.0f;
		default:
			return 0;
		}
	}
	// 0x1005ce20, squared keeping the sign for FuzzySqr.
	float f = 0;
	switch (compare) {
	case 0:
	case 6:
		f = -std::fabs(v - k);
		break;
	case 1:
	case 7:
		f = v - k;
		break;
	case 2:
	case 8:
		f = -(v - k);
		break;
	case 3:
		f = k != 0 ? -std::fabs(v / k) : -1000.0f;
		break;
	case 4:
		f = k != 0 ? v / k : 1000.0f;
		break;
	case 5:
		f = k != 0 ? -(v / k) : -1000.0f;
		break;
	default:
		return 0;
	}
	return condition.conditional == C::Conditional::FuzzySqr ? (f < 0 ? -f * f : f * f) : f;
}

float prerequisiteValue(const StrategySettings& settings, const std::string& name, const SideStatus& friendly,
	const SideStatus& enemy)
{
	using S = StrategyCondition::Strength;
	const auto it = settings.prerequisites.find(name);
	if (it == settings.prerequisites.end()) {
		return 0;
	}
	float sum = 0;
	for (const auto& [conditionName, weight] : it->second) {
		const auto c = settings.conditions.find(conditionName);
		if (c == settings.conditions.end()) {
			continue;
		}
		float term = conditionValue(c->second, friendly, enemy) * weight;
		switch (c->second.strength) {
		case S::Required:
		case S::RequiredPositive:
			if (term < 0) {
				return 0;
			}
			break;
		case S::RequiredNegative:
			if (term > 0) {
				return 0;
			}
			break;
		case S::AdvisoryPositive:
			term = std::max(term, 0.0f);
			break;
		case S::AdvisoryNegative:
			term = std::min(term, 0.0f);
			break;
		case S::Advisory:
			break;
		}
		sum += term;
	}
	return sum;
}

bool strategyWeightsChanged(const std::vector<float>& previous, const std::vector<float>& current)
{
	if (previous.size() != current.size()) {
		return true;
	}
	for (size_t i = 0; i < current.size(); ++i) {
		float ratio = 1;
		if (current[i] == 0) {
			ratio = previous[i] == 0 ? 1.0f : 0.0f;
		} else {
			ratio = previous[i] / current[i];
		}
		if (ratio < 0.83f || ratio >= 1.2f) {
			return true;
		}
	}
	return false;
}

int requiredStrength(float enemyStrategicStrength, bool owned, float friendlyRatio)
{
	// Owned: round((2 - S(+0xb0)) x max(sum, 1)), +0xb0 only set by the
	// area's constructor (taken as 0); else round((2 - S(the friendly
	// share of the strengths, +0xb8)) x max(sum, 1) x 1.25).
	const float sum = std::max(std::round(enemyStrategicStrength), 1.0f);
	const float required = owned ? 2.0f * sum : (2.0f - scurve(friendlyRatio)) * sum * 1.25f;
	return static_cast<int>(std::lround(required));
}

float modifiedTemperature(const Strategy& strategy, float temperature, AreaStatus status, bool controlPoint,
	bool hostileNeighbour)
{
	const auto& mods = strategy.modifiers[static_cast<int>(status)];
	float t = temperature;
	if (controlPoint) {
		t *= mods[kTypeControlPoint];
	}
	switch (status) {
	case AreaStatus::Owned:
		t *= hostileNeighbour ? mods[kTypeFront] : mods[kTypeSafe];
		break;
	case AreaStatus::Hostile:
		t *= mods[kTypeEnemy];
		break;
	case AreaStatus::Neutral:
		t *= mods[kTypeNeutral];
		break;
	}
	return t;
}

float defenceValue(const AreaState& area)
{
	if (area.status != AreaStatus::Owned) {
		return 0;
	}
	const float v = (area.enemyStrength + area.temperature) * 2.0f;
	return area.hostileNeighbour ? v * 1.5f : area.neutralNeighbour ? v * 1.25f : v;
}

float attackValue(const AreaState& area, float enemyAverageStrategicStrength)
{
	if (area.status == AreaStatus::Owned || !area.ownedNeighbour) {
		return 0;
	}
	float v = area.temperature; // (x + 1) temp, x = 0
	v *= area.hostileNeighbour ? 1.0f : area.neutralNeighbour ? 1.5f : 1.25f;
	const float total = area.friendlyStrength + area.enemyStrength;
	const float frac = total > 0 ? area.friendlyStrength / total : 0.0f;
	const float penalty = (frac + 1) * 1.5f * enemyAverageStrategicStrength;
	const float d = area.squadDistance;
	return d > 1 && d < 1e5f ? 100 * v / d - penalty : -penalty;
}

std::vector<int> selectAreas(std::vector<std::pair<float, int>> candidates, int n, float keep)
{
	std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	std::vector<int> chosen;
	if (n <= 0 || candidates.empty()) {
		return chosen;
	}
	const size_t top = std::min(candidates.size(), static_cast<size_t>(n));
	const float limit = candidates[top - 1].first * keep;
	for (size_t i = 0; i < candidates.size() && i < static_cast<size_t>(2 * n); ++i) {
		if (i < top || candidates[i].first >= limit) {
			chosen.push_back(candidates[i].second);
		}
	}
	return chosen;
}

} // namespace dice::hfe::ai
