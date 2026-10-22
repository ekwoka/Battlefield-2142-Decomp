#include "Behaviour.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::ai {

float UrgencyCurve::value(float t) const
{
	switch (kind) {
	case Kind::Constant:
		return a;
	case Kind::Linear:
		return std::max(0.0f, a * t + b);
	case Kind::XInverse:
		return std::max(0.0f, a / (c * t + b) + d);
	}
	return 1;
}

float fuzzy(float x)
{
	// The table AIDLL fills at 0x100cb6f0 (DAT_10139530, 101 floats).
	static const float table[101] = {
		0.0f, 0.02924211f, 0.05848421f, 0.08772631f, 0.1169684f, 0.1462105f, 0.1754526f, 0.2046947f,
		0.2339368f, 0.2631789f, 0.292421f, 0.3216631f, 0.3509052f, 0.3801474f, 0.4093895f, 0.4386316f,
		0.4678737f, 0.4971158f, 0.5263579f, 0.5512463f, 0.5789474f, 0.6040105f, 0.6267948f, 0.6475968f,
		0.6666663f, 0.6842105f, 0.7004053f, 0.7154f, 0.7293232f, 0.7422863f, 0.7543863f, 0.7657042f,
		0.7763158f, 0.7862842f, 0.7956653f, 0.8045115f, 0.8128653f, 0.8207685f, 0.8282548f, 0.8353579f,
		0.8421053f, 0.8485242f, 0.8546369f, 0.8604653f, 0.8660284f, 0.8713453f, 0.8764305f, 0.881299f,
		0.8859653f, 0.89044f, 0.8947369f, 0.8988653f, 0.9028337f, 0.9066536f, 0.9103316f, 0.9138758f,
		0.9172937f, 0.9205906f, 0.9237748f, 0.9268506f, 0.9298242f, 0.9327011f, 0.9354842f, 0.938179f,
		0.9407895f, 0.94332f, 0.9457737f, 0.9481537f, 0.9504642f, 0.9527074f, 0.9548874f, 0.9570053f,
		0.9590642f, 0.9610674f, 0.9630158f, 0.9649127f, 0.9667589f, 0.9685579f, 0.9703106f, 0.972019f,
		0.9736843f, 0.9753084f, 0.9768937f, 0.97844f, 0.9799495f, 0.9814243f, 0.9828642f, 0.9842705f,
		0.9856463f, 0.9869895f, 0.9883043f, 0.9895895f, 0.9908463f, 0.9920769f, 0.9932811f, 0.99446f,
		0.9956138f, 0.9967442f, 0.9978516f, 0.9989368f, 1.0f};
	if (x > 1) {
		return 1;
	}
	if (x < 0) {
		return 0;
	}
	// Truncated (the FPU's rounding set to chop, 0x100cbd2c); the
	// neighbour's weight is the constant 0 (0x101132e4).
	int i = static_cast<int>(x * 100);
	if (i == 100) {
		i = 99;
	}
	return table[i];
}

float scurve(float x)
{
	// The table built at 0x100cc350 (101 floats).
	static const float table[101] = {0.0f, 0.00224939f, 0.00338782f, 0.00463053f, 0.00598675f, 0.00746651f, 0.00908064f,
		0.0108408f, 0.0127597f, 0.0148509f, 0.0171289f, 0.0196096f, 0.0223098f, 0.0252474f, 0.0284416f, 0.0319131f,
		0.0356835f, 0.0397757f, 0.0442142f, 0.0490243f, 0.0542328f, 0.0598676f, 0.0659573f, 0.0725317f, 0.0796212f,
		0.0872565f, 0.0954685f, 0.104288f, 0.113745f, 0.123869f, 0.134688f, 0.146227f, 0.158509f, 0.171554f, 0.185376f,
		0.199988f, 0.215393f, 0.231592f, 0.248576f, 0.26633f, 0.284831f, 0.304048f, 0.323941f, 0.344461f, 0.365551f,
		0.387148f, 0.409179f, 0.431565f, 0.454224f, 0.477065f, 0.5f, 0.522934f, 0.545776f, 0.568435f, 0.590821f,
		0.612852f, 0.634449f, 0.655539f, 0.676059f, 0.695952f, 0.715168f, 0.73367f, 0.751424f, 0.768408f, 0.784607f,
		0.800012f, 0.814624f, 0.828446f, 0.841491f, 0.853773f, 0.865312f, 0.876131f, 0.886255f, 0.895712f, 0.904532f,
		0.912744f, 0.920379f, 0.927468f, 0.934043f, 0.940132f, 0.945767f, 0.950976f, 0.955786f, 0.960224f, 0.964317f,
		0.968087f, 0.971558f, 0.974753f, 0.97769f, 0.98039f, 0.982871f, 0.985149f, 0.98724f, 0.989159f, 0.990919f,
		0.992534f, 0.994013f, 0.995369f, 0.996612f, 0.997751f, 1.0f};
	if (!(x >= 0)) {
		return 0; // below 0, or not a number
	}
	if (x >= 1) {
		return 1;
	}
	const int i = static_cast<int>(x * 100);
	const float f = x * 100 - static_cast<float>(i);
	return (1 - f) * table[i] + f * table[i + 1];
}

std::vector<float> behaviourWeights(const std::vector<BehaviourSetup>& set, const Arbitration& state, float sinceLastThink,
	const std::map<std::string, float>& personality, const std::map<std::string, std::map<std::string, float>>& modifierSets,
	const std::map<std::string, UrgencyCurve>& curves)
{
	const auto lookup = [](const std::map<std::string, float>& values, const std::string& name) {
		const auto it = values.find(name);
		return it != values.end() ? it->second : 1.0f;
	};
	std::vector<float> weights(set.size(), 1.0f);
	const BehaviourSetup* running = state.running >= 0 && static_cast<size_t>(state.running) < set.size() && !state.replan
		? &set[static_cast<size_t>(state.running)]
		: nullptr;
	const std::map<std::string, float>* inhibitors = nullptr;
	if (running) {
		const auto it = modifierSets.find(running->inhibitors);
		inhibitors = it != modifierSets.end() ? &it->second : nullptr;
	}
	for (size_t i = 0; i < set.size(); ++i) {
		float w = lookup(personality, set[i].name);
		if (running) {
			if (static_cast<int>(i) == state.running) {
				const auto curve = curves.find(running->curve);
				w *= curve != curves.end() ? curve->second.value(sinceLastThink) : 1.0f;
			}
			w *= inhibitors ? lookup(*inhibitors, set[i].name) : 1.0f;
		}
		weights[i] = w;
	}
	return weights;
}

bool urgencyChanged(float latched, float now, float low, float high)
{
	if (latched == 0) {
		return now > 0;
	}
	if (now == 0) {
		return true;
	}
	const float ratio = now / latched;
	return ratio < low || ratio > high;
}

int arbitrate(Arbitration& state, int defaultIndex)
{
	state.latched.resize(state.urgency.size(), 0.0f);
	bool pick = state.replan || state.running < 0;
	for (size_t i = 0; i < state.urgency.size() && !pick; ++i) {
		pick = urgencyChanged(state.latched[i], state.urgency[i]);
	}
	if (pick) {
		float best = 0;
		int chosen = -1;
		for (size_t i = 0; i < state.urgency.size(); ++i) {
			state.latched[i] = state.urgency[i];
			if (state.urgency[i] > best) {
				best = state.urgency[i];
				chosen = static_cast<int>(i);
			}
		}
		state.running = chosen >= 0 ? chosen : defaultIndex;
		state.replan = false;
	}
	return state.running;
}

float gotoUrgency(float d2, float radius, float botRadius, const GoalArea& area, bool& arrived)
{
	const float r = std::max(std::ceil(radius), botRadius + 1);
	arrived = d2 < r * r;
	if (arrived) {
		return 0;
	}
	float factor = 1;
	float d2Left = d2;
	if (area.exists) {
		if (!area.teamHolds) {
			factor = 2;
		} else if (area.inside) {
			factor = 0;
			d2Left = 0;
		} else {
			d2Left = d2 - area.radius * area.radius;
		}
	}
	const float scale = r * r * 1.5f;
	return std::clamp(d2Left / scale, 0.1f, 1.0f) * factor;
}

} // namespace dice::hfe::ai
