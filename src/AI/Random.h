// AIDLL.dll's random numbers (the "AIRandom" object, DAT_101394cc): a
// Mersenne twister of 624 words with DICE's own tempering masks, seeded
// through a Park-Miller generator. Every draw of the AI (bots, squads, the
// SAI, the AI commander) comes from the one object.
//
// The object (0x9c8 bytes, made by the AI's start 0x10001740 and set as
// the singleton by its constructor 0x100308a0; destructor 0x100308d0):
//   +0      the seed (the console's aiRandom.seed reads and writes it,
//           0x10023670 / 0x100237d0, without seeding again)
//   +4      the 624 state words
//   +0x9c4  the index of the next word
#pragma once

#include <array>
#include <cstdint>

namespace dice::hfe::ai {

class Random {
public:
	// 0x100309a0, run once at the AI's start: a seed of 0 is taken from the
	// clock (IBFTimer slot 0x10 x 1e6; here the caller supplies one).
	void seed(uint32_t seed);
	static Random seeded(uint32_t s)
	{
		Random r;
		r.seed(s);
		return r;
	}
	uint32_t seedWord() const { return m_seed; }
	// aiRandom.seed's setter: the word only (the state stays).
	void setSeedWord(uint32_t seed) { m_seed = seed; }

	// 0x10046770: the next 32-bit word.
	uint32_t next();
	// 0x10046a50: next() x 2^-32 in [0, 1). The original returns it on the
	// x87 stack, where it stays below 1; rounded to a float the top 2^-25
	// of the range would come out as 1, so it is held just under.
	float uniform();

private:
	void refill(); // 0x100308f0
	uint32_t m_seed = 0;
	std::array<uint32_t, 624> m_state {};
	uint32_t m_index = 0; // 0x100308a0 leaves it 0 (and the state empty) until seeded
};

// A word as the callers that scale it themselves turn it into a float
// (0x10050720, 0x1009e020...): (float)(int)u, plus 2^32 when negative,
// all in single precision; multiply by 2.3283064e-10f for [0, 1].
float wordToFloat(uint32_t u);

// 0x10030810: one step of the Park-Miller generator x' = 630360016 x mod
// (2^31 - 1), done as two Schrage steps by 24112 and 26143.
uint32_t parkMillerStep(uint32_t x);

} // namespace dice::hfe::ai
