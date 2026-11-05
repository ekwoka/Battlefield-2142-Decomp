#include "AI/Random.h"

#include <cstdint>

namespace dice::hfe::ai {

namespace {

// One Schrage step x' = a x mod (2^31 - 1) as 0x10030810 writes it (the
// signed shifts and the final correction kept).
uint32_t schrage(uint32_t x, uint32_t a)
{
	const uint32_t low = (x & 0xffff) * a;
	uint32_t high = static_cast<uint32_t>(static_cast<int32_t>(x) >> 16) * a + static_cast<uint32_t>(static_cast<int32_t>(low) >> 16);
	high = (high & 0x7fff) * 0x10000 + static_cast<uint32_t>(static_cast<int32_t>(high) >> 15) - 0x7fffffffu + (low & 0xffff);
	if (static_cast<int32_t>(high) < 0) {
		high += 0x7fffffffu;
	}
	return high;
}

} // namespace

uint32_t parkMillerStep(uint32_t x)
{
	return schrage(schrage(x, 0x5e30), 0x661f); // 24112 x 26143 = 630360016
}

void Random::seed(uint32_t seed)
{
	// 0x100309a0: the seed word's low 31 bits start the Park-Miller run, and
	// each of its 624 next values is a state word; the index is set past the
	// end so the first draw refills.
	m_seed = seed;
	uint32_t x = seed & 0x7fffffff;
	for (uint32_t& word : m_state) {
		x = parkMillerStep(x);
		word = x;
	}
	m_index = 624;
}

void Random::refill()
{
	// 0x100308f0: the twister's recurrence (M = 397, the matrix 0x9908b0df
	// from the table at 0x10113ae4 = {0, 0x9908b0df}).
	constexpr uint32_t kMatrix[2] = {0, 0x9908b0dfu};
	const auto mix = [&](size_t i, size_t j, size_t k) {
		const uint32_t y = (m_state[i] & 0x80000000u) | (m_state[j] & 0x7fffffffu);
		m_state[i] = m_state[k] ^ (y >> 1) ^ kMatrix[y & 1];
	};
	for (size_t i = 0; i < 227; ++i) {
		mix(i, i + 1, i + 397);
	}
	for (size_t i = 227; i < 623; ++i) {
		mix(i, i + 1, i - 227);
	}
	mix(623, 0, 396);
	m_index = 0;
}

uint32_t Random::next()
{
	// 0x10046770 (also inlined in several AI functions). The tempering
	// masks are DICE's, not MT19937's (0x9d2c5680 / 0xefc60000): the word is
	// masked with 0x56b64a before the shift by 7 and with 0xffffb716 before
	// the shift by 15.
	if (m_index > 623) {
		refill();
	}
	uint32_t y = m_state[m_index++];
	y ^= y >> 11;
	y ^= (y & 0x0056b64au) << 7;
	y ^= (y & 0xffffb716u) << 15;
	return y ^ (y >> 18);
}

float wordToFloat(uint32_t u)
{
	float f = static_cast<float>(static_cast<int32_t>(u));
	if (static_cast<int32_t>(u) < 0) {
		f += 4294967296.0f;
	}
	return f;
}

float Random::uniform()
{
	const float u = static_cast<float>(static_cast<double>(next()) * (1.0 / 4294967296.0)); // 2.3283064e-10
	return u < 1.0f ? u : 0x1.fffffep-1f;
}

} // namespace dice::hfe::ai
