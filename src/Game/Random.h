// The engine's random numbers (0x0042b230, also inlined in RendDX9 as
// 0x10025420): a Lehmer generator, two steps modulo 2^31 - 1 with the
// multipliers 24112 and 26143, each computed in 16-bit halves.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace dice::hfe::game {

inline uint32_t lehmerNext(uint32_t& state)
{
	const auto step = [](uint32_t x, uint32_t a) {
		const uint32_t low = (x & 0xffff) * a;
		const uint32_t high = static_cast<uint32_t>(static_cast<int32_t>(x) >> 16) * a +
			static_cast<uint32_t>(static_cast<int32_t>(low) >> 16);
		uint32_t result = (high & 0x7fff) * 0x10000 + static_cast<uint32_t>(static_cast<int32_t>(high) >> 15) -
			0x7fffffff + (low & 0xffff);
		if (static_cast<int32_t>(result) < 0) {
			result += 0x7fffffff;
		}
		return result;
	};
	state = step(step(state, 0x5e30), 0x661f);
	return state;
}

// A generator with its own state; the game's shared one (0x00a05b30)
// starts at 0x759dc150.
class Random {
public:
	explicit Random(uint32_t seed = 0x759dc150) : m_state(seed) {}

	uint32_t next() { return lehmerNext(m_state); }
	// (0, 1], from the top 24 bits (as 0x100254b0 and the inlined copies do).
	float unit()
	{
		const uint32_t r = next();
		return static_cast<float>((static_cast<int32_t>(r | 0x80) >> 7) + 1) * 5.9604645e-08f;
	}
	uint32_t state() const { return m_state; }

private:
	uint32_t m_state;
};

// "CRD_UNIFORM/0.52/1.05/0" (RandomDistribution in scripts): a
// distribution and its parameters; a plain number is constant.
struct RandomDistribution {
	enum class Type { None, Uniform, Exponential, Normal } type = Type::None;
	float a = 0, b = 0, c = 0;

	static RandomDistribution parse(const std::string& text)
	{
		RandomDistribution d;
		char type[32] = {};
		if (std::sscanf(text.c_str(), "%31[^/]/%f/%f/%f", type, &d.a, &d.b, &d.c) < 2) {
			d.a = static_cast<float>(std::atof(text.c_str()));
			return d;
		}
		if (std::strcmp(type, "CRD_UNIFORM") == 0) {
			d.type = Type::Uniform;
		} else if (std::strcmp(type, "CRD_EXPONENTIAL") == 0) {
			d.type = Type::Exponential;
		} else if (std::strcmp(type, "CRD_NORMAL") == 0) {
			d.type = Type::Normal;
		}
		return d;
	}

	float sample(Random& random) const
	{
		switch (type) {
		case Type::Uniform:
			return a + (b - a) * random.unit();
		case Type::Exponential:
			return a - std::log(random.unit()) * b;
		case Type::Normal: {
			// Box-Muller: mean a, deviation b.
			const float u = random.unit(), v = random.unit();
			return a + b * std::sqrt(-2.0f * std::log(u)) * std::cos(6.2831853f * v);
		}
		case Type::None:
		default:
			return a;
		}
	}
};

} // namespace dice::hfe::game
